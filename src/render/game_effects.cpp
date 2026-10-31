// game_effects.cpp - switches off NieR effects that don't suit VR: vignette, shadows, ambient occlusion, low-health filter,
// sun glare, lens flare.
#include "render/game_effects.h"
#include "core/com.h"
#include "core/config.h"
#include "core/log.h"
#include "render/camera.h"
#include "render/texture_tags.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <d3dcompiler.h>

namespace {

constexpr UINT kVignetteBufferSize = 48;
ID3D11Resource* s_vignetteBuffer;
void* s_vignetteMapped;

// Light scattering (the haze and glow around the sun): its "CB" (pixel shader cb8; the shader declares 160 bytes,
// the game's buffer has 288, written once per frame and bound nowhere else) holds the sun's
// direction in VIEW space, LightDirView at float 32, which the game works out from its own camera - not patched with
// the eye's view like the camera buffers, so the glow sat where the game camera would see the sun (JJ 2026-10-28,
// NOTES A12: identical in captures whose eye views differed by 40 deg). Turned into this eye's view here.
constexpr UINT kScatterBufferSize = 160, kScatterSlot = 8, kScatterLightDir = 32;  // size: at least
ID3D11Resource* s_scatterBuffer;
UINT s_scatterWidth;  // the buffer's own size
void* s_scatterMapped;
uint32_t s_scatterPatched;

// Sun trace (logged every 2 s while the sun is drawn; JJ 2026-10-28: the glow flickers on head nods): whether the
// SUN card blinks on/off between frames (the game's own visibility decision), how far the glow's direction steps per
// frame, and how far the one-frame-old eye view used at upload is from the eye's real one (seen at the scattering draw).
constexpr uint64_t kSunShader = 0x097e9b9b9a83389e;  // the sun card (brightness 21, NOTES A12)
struct SunTrace {
	uint32_t frames, sunFrames, blinks, sunDraws, steps, bigSteps, staleChecks;
	bool sunLastFrame;
	float maxStep, sumStep, maxStale, sumStale;
	float used[2][3], gameDir[3];
	bool haveUsed[2];
	DWORD since;
} s_sun;

float AngleDeg(const float* a, const float* b) {
	float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
	float la = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]), lb = sqrtf(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
	float c = la > 0 && lb > 0 ? d / (la * lb) : 1;
	c = c > 1 ? 1 : (c < -1 ? -1 : c);
	return acosf(c) * 57.29578f;
}

void SunTraceFrame() {
	SunTrace& s = s_sun;
	if (!s.since) s.since = GetTickCount();
	bool sun = s.sunDraws > 0;
	if (s.frames && sun != s.sunLastFrame) s.blinks++;
	s.sunLastFrame = sun;
	s.sunFrames += sun;
	s.sunDraws = 0;
	s.frames++;
	if (GetTickCount() - s.since < 2000) return;
	if (s.sunFrames || s.steps)
		Log("sun trace (2 s, %u frames): sun drawn in %u, blinked on/off %u times; glow direction step per frame avg %.2f "
			"max %.2f deg (%u over 0.5); one-frame-old eye view off by avg %.2f max %.2f deg",
			s.frames, s.sunFrames, s.blinks, (double)(s.steps ? s.sumStep / s.steps : 0), (double)s.maxStep, s.bigSteps,
			(double)(s.staleChecks ? s.sumStale / s.staleChecks : 0), (double)s.maxStale);
	bool keep[2] = {s.haveUsed[0], s.haveUsed[1]};
	float used[2][3];
	memcpy(used, s.used, sizeof(used));
	s = {};
	s.since = GetTickCount();
	memcpy(s.used, used, sizeof(used));
	s.haveUsed[0] = keep[0];
	s.haveUsed[1] = keep[1];
}

void SunTraceGlow(const float* gameDir, const float* used) {
	int eye = StereoFrameEye();
	memcpy(s_sun.gameDir, gameDir, sizeof(s_sun.gameDir));
	if (eye < 0 || eye > 1) return;
	if (s_sun.haveUsed[eye]) {
		float step = AngleDeg(s_sun.used[eye], used);
		s_sun.steps++;
		s_sun.sumStep += step;
		s_sun.maxStep = step > s_sun.maxStep ? step : s_sun.maxStep;
		s_sun.bigSteps += step > 0.5f;
	}
	memcpy(s_sun.used[eye], used, sizeof(s_sun.used[eye]));
	s_sun.haveUsed[eye] = true;
}

// At the scattering draw this eye's camera is known: what the direction should have been, against what was used.
void SunTraceStale() {
	float T[16], Tinv[16];
	int eye = StereoFrameEye();
	if (eye < 0 || eye > 1 || !s_sun.haveUsed[eye] || !StereoGetViewFix(T, Tinv)) return;
	const float* L = s_sun.gameDir;
	float e[3];
	for (int r = 0; r < 3; r++) e[r] = T[r * 4] * L[0] + T[r * 4 + 1] * L[1] + T[r * 4 + 2] * L[2];
	float off = AngleDeg(e, s_sun.used[eye]);
	s_sun.staleChecks++;
	s_sun.sumStale += off;
	s_sun.maxStale = off > s_sun.maxStale ? off : s_sun.maxStale;
}

void PatchScatter(float* floats) {
	// uploaded at the start of the frame, before this eye's camera: the eye's view from its last frame then
	float T[16];
	if (!g_cfg.scatterPerEye) return;
	if (!StereoGetEyeViewFix(T)) {  // mono frames: already the game's own view
		static uint32_t skipped;
		if (StereoActive() && skipped++ < 3) Log("sun glow: an upload in stereo came before any eye view was known: left as it is");
		return;
	}
	const float* L = floats + kScatterLightDir;
	float e[3];
	for (int r = 0; r < 3; r++) e[r] = T[r * 4] * L[0] + T[r * 4 + 1] * L[1] + T[r * 4 + 2] * L[2];
	if (!s_scatterPatched++)
		Log("sun glow: light scattering's sun direction turned from the game camera's view (%.3f %.3f %.3f) into the "
			"eye's (%.3f %.3f %.3f) (ScatterPerEye=1)",
			(double)L[0], (double)L[1], (double)L[2], (double)e[0], (double)e[1], (double)e[2]);
	SunTraceGlow(L, e);
	for (int r = 0; r < 3; r++) floats[kScatterLightDir + r] = e[r];
}

void FindScatterBuffer(ID3D11DeviceContext* c) {
	ComPtr<ID3D11Buffer> buffer;
	c->PSGetConstantBuffers(kScatterSlot, 1, &buffer);
	if (!buffer) return;
	D3D11_BUFFER_DESC desc;
	buffer->GetDesc(&desc);
	// a buffer of another size here would be another layout: left alone (the game's is 288 bytes)
	if (desc.ByteWidth < kScatterBufferSize || desc.ByteWidth > kPatchScratchFloats * sizeof(float)) {
		static bool logged;
		if (!logged) Log("sun glow: light scattering's parameters are in a buffer of %u bytes: not patched", desc.ByteWidth);
		logged = true;
		return;
	}
	s_scatterBuffer = buffer.Get();
	s_scatterWidth = desc.ByteWidth;
	Log("sun glow: light scattering parameters found (buffer of %u bytes)", desc.ByteWidth);
}

void PatchVignette(float* floats) {
	if (!g_cfg.vignette) floats[8] = 0.0001f;
}

void FindVignetteBuffer(ID3D11DeviceContext* c) {
	ComPtr<ID3D11Buffer> buffer;
	c->PSGetConstantBuffers(1, 1, &buffer);
	if (!buffer) return;
	D3D11_BUFFER_DESC desc;
	buffer->GetDesc(&desc);
	if (desc.ByteWidth == kVignetteBufferSize) {
		s_vignetteBuffer = buffer.Get();
		Log("vignette: parameters found (buffer of %u bytes); %s", desc.ByteWidth,
			g_cfg.vignette ? "kept (Vignette=1)" : "removed from the next frame on");
	}
}

// --- shadows -----------------------------------------------------------------------------------
// Shadows off: the game's shadow-mask draw runs with a pixel shader that just writes 1 ("lit")
// instead of sampling the shadow map, keeping the game's own blend state, write mask, stencil and
// render targets.
ID3D11PixelShader* s_litShader;
bool s_litShaderTried;
// Sun glare off: the glare pass keeps running, but its pixels come out black. The game's own draw,
// blend state and targets are left exactly as they were - skipping the draw outright crashed the game
// in its own C runtime on the title screens (JJ, 2026-10-27).
ID3D11PixelShader* s_blackShader;
bool s_blackShaderTried;
// Lens flare off: the flare's draws keep running with a pixel shader that writes nothing at all (discard), which
// is right whatever the effect's blend state is - black would paint black squares under a normal alpha blend.
ID3D11PixelShader* s_noPixelsShader;
bool s_noPixelsShaderTried;
ID3D11PixelShader* s_savedShader;
ID3D11ClassInstance* s_savedInstances[8];
UINT s_savedInstanceCount;
bool s_restoreShader;

// A replacement pixel shader with no inputs, so it links to whatever vertex shader is bound.
ID3D11PixelShader* ConstantShader(ID3D11DeviceContext* c, const char* source, const char* what, ID3D11PixelShader** slot, bool* tried) {
	if (*tried) return *slot;
	*tried = true;
	HMODULE compilerDll = LoadLibraryW(L"d3dcompiler_47.dll");
	pD3DCompile compile = compilerDll ? (pD3DCompile)(void*)GetProcAddress(compilerDll, "D3DCompile") : nullptr;
	ID3DBlob *blob = nullptr, *errors = nullptr;
	HRESULT hr = compile ? compile(source, strlen(source), what, nullptr, nullptr, "main", "ps_5_0", 0, 0, &blob, &errors) : E_FAIL;
	ComPtr<ID3D11Device> device;
	c->GetDevice(&device);
	if (SUCCEEDED(hr) && device) hr = device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, slot);
	if (FAILED(hr)) {
		*slot = nullptr;
		Log("%s: no replacement shader (0x%08x); that pass stays as it is", what, (unsigned)hr);
	}
	if (blob) blob->Release();
	if (errors) errors->Release();
	return *slot;
}

ID3D11PixelShader* LitShader(ID3D11DeviceContext* c) {
	return ConstantShader(c, "float4 main() : SV_Target { return 1; }", "shadows off", &s_litShader, &s_litShaderTried);
}

ID3D11PixelShader* BlackShader(ID3D11DeviceContext* c) {
	return ConstantShader(c, "float4 main() : SV_Target { return 0; }", "sun glare off", &s_blackShader, &s_blackShaderTried);
}

ID3D11PixelShader* NoPixelsShader(ID3D11DeviceContext* c) {
	return ConstantShader(c, "float4 main() : SV_Target { discard; return 0; }", "lens flare off", &s_noPixelsShader,
						  &s_noPixelsShaderTried);
}

// Swaps the pixel shader for this draw (through the hooked call, so synchronized mode records it too);
// GameEffectsAfterDraw puts the game's back.
void SwapPixelShader(ID3D11DeviceContext* c, ID3D11PixelShader* replacement) {
	s_savedInstanceCount = 8;
	c->PSGetShader(&s_savedShader, s_savedInstances, &s_savedInstanceCount);
	c->PSSetShader(replacement, nullptr, 0);
	s_restoreShader = true;
}

// --- low-health filter -------------------------------------------------------------------------
// NieR's effect system draws the low-health grey, blur and ripples like particles: quads whose
// texture is a copy of the 3D scene made in the middle of the effect pass, drawn back over the whole
// view. Normal frames only let effects sample one full-size scene copy, the refraction copy bound at
// t10 when the first effect draws. In stereo, effect draws that sample any other full-size copy AND
// use one of the known low-health shaders are skipped. Other shaders that read a scene copy (Pod
// 042's shots, flight-unit effects) are kept and only logged.
ID3D11Resource* s_refractionCopy;  // this frame's normal screen copy (t10 at the first effect draw)
bool s_sawEffect;
uint32_t s_filterSkips;
uint32_t s_glareSkips;  // sun glare draws drawn black
uint32_t s_flareDraws;  // lens flare draws that drew nothing
constexpr uint64_t kLensFlareShader = 0x721e4f4a7c4185e0;  // the flare's pixel shader (captures, NOTES A11/A12)
DWORD s_filterReportTick;
bool s_filterLogged;

constexpr uint32_t kLowHealthShaders[] = {0x86575858, 0x833497f9, 0xcebc6f6d, 0x8b838343, 0x32c5d826};
uint32_t s_keptLogged[16];
int32_t s_keptCount;

bool LowHealthShader(uint64_t hash) {
	for (uint32_t known : kLowHealthShaders)
		if ((uint32_t)hash == known) return true;
	return false;
}

// Which effect shaders were skipped, for the minute report (bytecode hash, count).
struct Skipped {
	uint64_t hash;
	uint32_t count;
	UINT slot;
};
Skipped s_skipped[8];

void NoteSkip(uint64_t hash, UINT slot) {
	for (auto& entry : s_skipped)
		if (entry.hash == hash && entry.count) {
			entry.count++;
			return;
		}
	for (auto& entry : s_skipped)
		if (!entry.count) {
			entry = {hash, 1, slot};
			return;
		}
}

ID3D11Resource* BoundResource(ID3D11DeviceContext* c, UINT slot) {
	ComPtr<ID3D11ShaderResourceView> view;
	c->PSGetShaderResources(slot, 1, &view);
	if (!view) return nullptr;
	ComPtr<ID3D11Resource> res;
	view->GetResource(&res);
	return res.Get();
}

bool Tex2D(ID3D11Resource* res, D3D11_TEXTURE2D_DESC* desc) {
	if (!res) return false;
	D3D11_RESOURCE_DIMENSION dim;
	res->GetType(&dim);
	if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return false;
	((ID3D11Texture2D*)res)->GetDesc(desc);
	return true;
}
}  // namespace

void GameEffectsBeginFrame() {
	s_refractionCopy = nullptr;
	s_sawEffect = false;
	SunTraceFrame();
}

bool GameEffectsBeforeDraw(ID3D11DeviceContext* c, PixelShaderClass ps, uint64_t psHash) {
	if (ps == PS_TONE_MAPPING && !s_vignetteBuffer) FindVignetteBuffer(c);
	if (ps == PS_LIGHT_SCATTERING && !s_scatterBuffer) FindScatterBuffer(c);
	if (ps == PS_LIGHT_SCATTERING && s_scatterBuffer) SunTraceStale();
	if (ps == PS_EFFECT && psHash == kSunShader) s_sun.sunDraws++;
	// The game's glare pass (g_GlareColor, g_GlareParam, one texture) is the bloom around the sun - not the
	// lens flare's rings and streaks, which come from elsewhere (JJ, 2026-10-27).
	if (ps == PS_GLARE && !g_cfg.sunGlare && BlackShader(c)) {
		if (!s_glareSkips++) Log("sun glare: the game's glare pass draws black, so nothing is added (SunGlare=0)");
		SwapPixelShader(c, s_blackShader);
		return false;
	}
	// The lens flare's rings and streaks are effect-system quads drawn with a shader other effects share (the title
	// graphics, the circle around 2B, some weapons): its draws are the ones with a flare texture at t0 - HelixMod's
	// 3D fix found them the same way (NOTES A11). Only with the flare's own pixel shader: the SUN itself (PS
	// 097e9b9b9a83389e, brightness 21) uses one of the same textures, and removing it took the sun away (A12).
	if (ps == PS_EFFECT && psHash == kLensFlareShader && !g_cfg.lensFlare && TextureTagsIsLensFlare(BoundResource(c, 0)) &&
		NoPixelsShader(c)) {
		if (!s_flareDraws++) Log("lens flare: its draws now draw nothing (LensFlare=0)");
		SwapPixelShader(c, s_noPixelsShader);
		return false;
	}
	if (ps == PS_SHADOW_MASK && !g_cfg.shadows && LitShader(c)) {
		SwapPixelShader(c, s_litShader);
		return false;
	}
	// ambient occlusion off: instead of the SAO pass, its output is cleared to 1 (no occlusion)
	if (ps != PS_AMBIENT_OCCLUSION || g_cfg.ambientOcclusion) return false;
	ComPtr<ID3D11RenderTargetView> rtv;
	c->OMGetRenderTargets(1, &rtv, nullptr);
	if (rtv) {
		const FLOAT one[4] = {1, 1, 1, 1};
		c->ClearRenderTargetView(rtv.Get(), one);
	}
	return true;
}

bool GameEffectsDrawNothing(ID3D11DeviceContext* c) {
	if (s_restoreShader || !NoPixelsShader(c)) return false;
	SwapPixelShader(c, s_noPixelsShader);
	return true;
}

void GameEffectsAfterDraw(ID3D11DeviceContext* c) {
	if (!s_restoreShader) return;
	s_restoreShader = false;
	c->PSSetShader(s_savedShader, s_savedInstanceCount ? s_savedInstances : nullptr, s_savedInstanceCount);
	if (s_savedShader) s_savedShader->Release();
	for (UINT i = 0; i < s_savedInstanceCount && i < 8; i++)
		if (s_savedInstances[i]) s_savedInstances[i]->Release();
	s_savedShader = nullptr;
	s_savedInstanceCount = 0;
}

bool GameEffectsSkipEffectDraw(ID3D11DeviceContext* c, uint32_t sampled, uint64_t shaderHash) {
	if (!s_sawEffect) {
		s_sawEffect = true;
		s_refractionCopy = BoundResource(c, 10);
	}
	if (g_cfg.lowHealthFx || !StereoActive() || !sampled) return false;
	ComPtr<ID3D11RenderTargetView> rtv;
	c->OMGetRenderTargets(1, &rtv, nullptr);
	if (!rtv) return false;
	ComPtr<ID3D11Resource> target;
	rtv->GetResource(&target);
	D3D11_TEXTURE2D_DESC targetDesc;
	if (!Tex2D(target.Get(), &targetDesc)) return false;
	bool skip = false;
	UINT hit = 0;
	for (UINT slot = 0; slot < 32 && !skip; slot++) {
		if (!(sampled >> slot & 1)) continue;
		ID3D11Resource* res = BoundResource(c, slot);
		D3D11_TEXTURE2D_DESC desc;
		if (!res || res == target.Get() || res == s_refractionCopy || !Tex2D(res, &desc)) continue;
		skip = desc.Width == targetDesc.Width && desc.Height == targetDesc.Height && desc.Format == targetDesc.Format;
		hit = slot;
	}
	if (!skip) return false;
	if (!LowHealthShader(shaderHash)) {
		// reads a scene copy too, but isn't the low-health effect (e.g. Pod shots): kept, logged once
		bool seen = false;
		for (int i = 0; i < s_keptCount; i++) seen |= s_keptLogged[i] == (uint32_t)shaderHash;
		if (!seen && s_keptCount < 16) {
			s_keptLogged[s_keptCount++] = (uint32_t)shaderHash;
			Log("screen filter: effect %08x reads a scene copy at t%u but isn't a known low-health shader: kept",
				(unsigned)(uint32_t)shaderHash, hit);
		}
		return false;
	}
	s_filterSkips++;
	NoteSkip(shaderHash, hit);
	if (!s_filterLogged) {
		s_filterLogged = true;
		s_filterReportTick = GetTickCount();
		Log("screen filter effect (redraws a copy of the scene: low-health grey, blur, ripples) "
			"removed while in stereo (LowHealthEffect=0)");
	} else if (GetTickCount() - s_filterReportTick > 60000) {
		char list[300];
		size_t used = 0;
		list[0] = 0;
		for (auto& entry : s_skipped)
			if (entry.count && used + 40 < sizeof(list))
				used += snprintf(list + used, sizeof(list) - used, " %08x/t%u x%u", (unsigned)(entry.hash & 0xffffffff),
								 entry.slot, entry.count);
		Log("screen filter: %u draws removed in the last minute (shader/slot/count:%s)", s_filterSkips, list);
		for (auto& entry : s_skipped) entry = {};
		s_filterSkips = 0;
		s_filterReportTick = GetTickCount();
	}
	return true;
}

void GameEffectsOnMap(ID3D11Resource* res, void* data, UINT size) {
	if (res == s_vignetteBuffer && size == kVignetteBufferSize) s_vignetteMapped = data;
	if (res == s_scatterBuffer && size == s_scatterWidth) s_scatterMapped = data;
}

void GameEffectsBeforeUnmap(ID3D11Resource* res) {
	if (res == s_vignetteBuffer && s_vignetteMapped) {
		PatchVignette((float*)s_vignetteMapped);
		s_vignetteMapped = nullptr;
	}
	if (res == s_scatterBuffer && s_scatterMapped) {
		// the mapped memory may be write-combined: patch a local copy, write the three floats back
		float local[kScatterBufferSize / 4];
		memcpy(local, s_scatterMapped, kScatterBufferSize);
		PatchScatter(local);
		memcpy((float*)s_scatterMapped + kScatterLightDir, local + kScatterLightDir, 3 * sizeof(float));
		s_scatterMapped = nullptr;
	}
}

// `scratch` holds kPatchScratchFloats floats (d3d_hooks.cpp, replay.cpp)
const void* GameEffectsPatchUpdate(ID3D11Resource* res, const void* data, UINT size, float* scratch) {
	UINT needed = res == s_vignetteBuffer ? kVignetteBufferSize : (res == s_scatterBuffer ? s_scatterWidth : 0);
	if (!needed || size != needed || size > kPatchScratchFloats * sizeof(float)) return data;
	if (data != scratch) memcpy(scratch, data, needed);
	if (res == s_vignetteBuffer) PatchVignette(scratch);
	else PatchScatter(scratch);
	return scratch;
}
