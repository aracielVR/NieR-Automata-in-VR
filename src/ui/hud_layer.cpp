// hud_layer.cpp - separates NieR's 2D UI (HUD, menus) from the 3D scene, on the game side.
#include "ui/hud_layer.h"
#include "core/com.h"
#include "core/config.h"
#include "core/log.h"
#include "debug/gpu_timing.h"
#include "render/camera.h"
#include "render/foveated.h"
#include "render/game_effects.h"
#include "render/replay.h"
#include "render/reproject.h"
#include "render/upscale.h"
#include <atomic>
#include <string.h>
#include "render/d3d_hooks.h"

namespace {
ID3D11Texture2D* s_backbuffer;          // the swapchain backbuffer (raw pointer: identity only, never called)
D3D11_TEXTURE2D_DESC s_backbufferDesc;  // its description, taken at Present while it was certainly alive
bool s_uiShaderBound;                   // current vertex shader is a UI shader
bool s_uiStarted, s_sceneReady;
ID3D11Resource* s_uiTarget;  // the intermediate the UI is drawn into
ID3D11Texture2D* s_preUiCopy;
D3D11_TEXTURE2D_DESC s_preUiDesc;
ID3D11ShaderResourceView* s_preUiSrv;
ID3D11RenderTargetView* s_preUiRtv;
D3D11_RENDER_TARGET_VIEW_DESC s_preUiRtvDesc;
bool s_drawIntoPreUi;     // the current UI draw is a screen effect: also draw it into the pre-UI copy
bool s_drawIntoCoverage;  // the current UI draw goes into the UI target: add its alpha to the coverage
bool s_loggedEffect;
ID3D11Resource* s_screenCopy;  // this frame's copy of the UI target, made for distortion effects
uint32_t s_distortionDraws;    // distortion draws removed/kept since the last report
DWORD s_distortionReportTick;
bool s_loggedDistortion;
// Synchronized: the game's final pass of this frame (state captured by replay.cpp), to draw the right
// eye's scene image after its playback.
bool s_finalPassSaved;
int32_t s_finalPassSlot;
DrawArgs s_finalPassArgs;

// Parameters of UI draws: the first 96 bytes of the UIParam / cbUIParam cbuffers (both start with
// g_WorldMatrix, g_Color0, g_Color1, g_DispRate), remembered per buffer. NieR fills ~470 of these buffers a
// frame, some (the cutscene bands) right at its start and drawn at its end: a 32-entry ring had long forgotten
// them by then (capture 2026-10-31, NOTES A17). So a table by buffer that holds a whole frame and more.
struct UiParams {
	ID3D11Resource* buffer;
	float floats[24];
};
constexpr int32_t kUiParamSlots = 4096;  // a power of two
UiParams s_uiParams[kUiParamSlots];
int32_t s_uiParamsUsed;
struct PendingUiMap {
	ID3D11Resource* res;
	const float* data;
};
PendingUiMap s_pendingUiMaps[8];
ID3D11Resource* s_vsSlot1Buffer;  // buffer bound at VS slot b1 (UI parameters)
D3D11_SHADER_RESOURCE_VIEW_DESC s_preUiSrvDesc;
ID3D11Texture2D* s_scene;
ID3D11RenderTargetView* s_sceneRtv;
D3D11_TEXTURE2D_DESC s_sceneDesc;
D3D11_RENDER_TARGET_VIEW_DESC s_sceneRtvDesc;
uint32_t s_framesWithUI;
bool s_loggedWorking, s_loggedNoFinal;
std::atomic<int32_t> s_preUiFailLogs;

bool SameDesc(const D3D11_TEXTURE2D_DESC& a, const D3D11_TEXTURE2D_DESC& b) {
	return a.Width == b.Width && a.Height == b.Height && a.Format == b.Format && a.ArraySize == b.ArraySize &&
		   a.MipLevels == b.MipLevels && a.SampleDesc.Count == b.SampleDesc.Count;
}

ID3D11Device* DeviceOf(ID3D11DeviceContext* c) {
	ComPtr<ID3D11Device> device;
	c->GetDevice(&device);
	return device.Get();
}
uint32_t UiParamSlot(ID3D11Resource* res) { return (uint32_t)(((uintptr_t)res >> 4) * 0x9E3779B97F4A7C15ull >> 52) & (kUiParamSlots - 1); }

void StoreUiParams(ID3D11Resource* res, const float* data) {
	for (uint32_t i = UiParamSlot(res), n = 0; n < kUiParamSlots; i = (i + 1) & (kUiParamSlots - 1), n++) {
		UiParams& e = s_uiParams[i];
		if (e.buffer == res) {
			memcpy(e.floats, data, sizeof(e.floats));
			return;
		}
		if (!e.buffer) {
			if (s_uiParamsUsed >= kUiParamSlots * 3 / 4) {  // nearly full (buffers come and go): start again
				memset(s_uiParams, 0, sizeof(s_uiParams));
				s_uiParamsUsed = 0;
				StoreUiParams(res, data);
				return;
			}
			e.buffer = res;
			memcpy(e.floats, data, sizeof(e.floats));
			s_uiParamsUsed++;
			return;
		}
	}
}

const float* FindUiParams(ID3D11Resource* res) {
	if (!res) return nullptr;
	for (uint32_t i = UiParamSlot(res), n = 0; n < kUiParamSlots; i = (i + 1) & (kUiParamSlots - 1), n++) {
		if (s_uiParams[i].buffer == res) return s_uiParams[i].floats;
		if (!s_uiParams[i].buffer) return nullptr;
	}
	return nullptr;
}

// Blend states derived from the game's: mode 0 = UI coverage (alpha channel only), mode 1 = the
// final pass replay (colour only, so the coverage survives).
struct DerivedBlend {
	ID3D11BlendState* game;
	bool isNull;
	int32_t mode;
	ID3D11BlendState* ours;
};
DerivedBlend s_blends[48];
int32_t s_blendCount;
std::atomic<int32_t> s_blendLogs;

ID3D11BlendState* DerivedBlendState(ID3D11Device* device, ID3D11BlendState* game, int mode) {
	for (int i = 0; i < s_blendCount; i++)
		if (s_blends[i].game == game && s_blends[i].isNull == !game && s_blends[i].mode == mode) return s_blends[i].ours;
	D3D11_BLEND_DESC desc = {};
	if (game) {
		game->GetDesc(&desc);
	} else {
		for (auto& rt : desc.RenderTarget) {
			rt.SrcBlend = rt.SrcBlendAlpha = D3D11_BLEND_ONE;
			rt.DestBlend = rt.DestBlendAlpha = D3D11_BLEND_ZERO;
			rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
			rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		}
	}
	D3D11_RENDER_TARGET_BLEND_DESC& rt = desc.RenderTarget[0];
	if (mode == 0) {
		bool additive = rt.BlendEnable && rt.DestBlend == D3D11_BLEND_ONE;
		if (!rt.BlendEnable) {
			rt.SrcBlend = D3D11_BLEND_ONE;
			rt.DestBlend = D3D11_BLEND_ZERO;
			rt.BlendOp = D3D11_BLEND_OP_ADD;
		}
		rt.BlendEnable = TRUE;
		rt.SrcBlendAlpha = additive ? D3D11_BLEND_ZERO : D3D11_BLEND_ONE;
		rt.DestBlendAlpha = additive ? D3D11_BLEND_ONE : D3D11_BLEND_INV_SRC_ALPHA;
		rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
		rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALPHA;
		if (LogBudget(s_blendLogs, 6))
			Log("ui split: UI blend %s: colour %d/%d op %d, alpha %d/%d -> coverage %s", game ? "state" : "(default)",
				(int)(game ? desc.RenderTarget[0].SrcBlend : 0), (int)(game ? desc.RenderTarget[0].DestBlend : 0),
				(int)rt.BlendOp, (int)rt.SrcBlendAlpha, (int)rt.DestBlendAlpha, additive ? "unchanged (additive)" : "over");
	} else {
		rt.RenderTargetWriteMask &= D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN |
									D3D11_COLOR_WRITE_ENABLE_BLUE;
	}
	desc.IndependentBlendEnable = FALSE;
	desc.AlphaToCoverageEnable = FALSE;
	ID3D11BlendState* ours = nullptr;
	if (FAILED(device->CreateBlendState(&desc, &ours))) ours = nullptr;
	if (s_blendCount < 48) s_blends[s_blendCount++] = {game, !game, mode, ours};
	return ours;
}

// Draw `args` again with RT0 replaced (and optionally the blend state), restoring both after.
void ReplayInto(ID3D11DeviceContext* c, ID3D11RenderTargetView* target, const DrawArgs& args, int blendMode) {
	ComPtr<ID3D11RenderTargetView> rtv;
	ComPtr<ID3D11DepthStencilView> dsv;
	c->OMGetRenderTargets(1, &rtv, &dsv);
	ComPtr<ID3D11BlendState> gameBlend;
	FLOAT factor[4];
	UINT sampleMask = 0xffffffff;
	ID3D11BlendState* derived = nullptr;
	if (blendMode >= 0) {
		c->OMGetBlendState(&gameBlend, factor, &sampleMask);
		derived = DerivedBlendState(DeviceOf(c), gameBlend.Get(), blendMode);
	}
	if (blendMode < 0 || derived) {
		if (derived) c->OMSetBlendState(derived, factor, sampleMask);
		OriginalSetRenderTargets(c, 1, &target, dsv.Get());
		OriginalDraw(c, args);
		OriginalSetRenderTargets(c, 1, rtv.GetAddressOf(), dsv.Get());
		if (derived) c->OMSetBlendState(gameBlend.Get(), factor, sampleMask);
	}
}

// In stereo below 5:4: sets a viewport taller than the backbuffer, so the final pass's 5:4 box covers
// all of it, and returns true with the game's viewport in `previous` (to put back afterwards).
bool StretchFinalPass(ID3D11DeviceContext* c, D3D11_VIEWPORT* previous) {
	float width = (float)s_backbufferDesc.Width, height = (float)s_backbufferDesc.Height;
	if (!StereoActive() || height <= 0 || width / height >= kGameMinAspect - 0.01f) return false;
	UINT count = 1;
	c->RSGetViewports(&count, previous);
	if (count == 0) return false;
	float boxHeight = width / kGameMinAspect;  // the rows the game's box covers
	D3D11_VIEWPORT stretched = *previous;
	stretched.TopLeftX = 0;
	stretched.Width = width;
	stretched.Height = height * height / boxHeight;
	stretched.TopLeftY = (height - stretched.Height) / 2;
	c->RSSetViewports(1, &stretched);
	return true;
}

// Full-screen white overlay at the centre of the 1600x900 UI canvas, unscaled: a screen effect.
bool IsScreenEffect(const float* params) {
	auto within = [](float a, float b, float e) { return a > b - e && a < b + e; };
	return within(params[0], 1, 0.01f) && within(params[5], 1, 0.01f) && within(params[3], 800, 0.5f) && within(params[7], 450, 0.5f) &&
		   params[16] > 0.95f && params[17] > 0.95f && params[18] > 0.95f;
}

// A cutscene's letterbox band: an unscaled rectangle centred across the UI canvas, dark brown (0.035 0.024 0.008)
// at any opacity (0.41 when shown), near the top or bottom: at the canvas edges (y 0 / 1280) in 3D cutscenes, at
// the film's own edges (y 190 / 1090) over pre-rendered films (captures 2026-10-31, NOTES A17).
bool IsCutsceneBand(const float* params) {
	auto within = [](float a, float b, float e) { return a > b - e && a < b + e; };
	return within(params[0], 1, 0.01f) && within(params[5], 1, 0.01f) && within(params[3], 800, 0.5f) &&
		   (params[7] < 300 || params[7] > 850) && within(params[16], 0.0353f, 0.006f) &&
		   within(params[17], 0.0235f, 0.006f) && within(params[18], 0.0078f, 0.006f);
}
// Cutscene band draws in the last 10 s: seen, hidden, and why the rest weren't (logged by UiBeginFrame).
uint32_t s_bandSeen, s_bandHidden, s_bandOff, s_bandMenu, s_bandSwapFailed;
DWORD s_bandReportTick;

// HideBlackBars (CropBlackBars=1): the cutscene's bands draw nothing - except while the flat menu screen is up.
bool HideCutsceneBand(ID3D11DeviceContext* c, const float* params) {
	if (!params || !IsCutsceneBand(params)) return false;
	++s_bandSeen;
	if (!g_cfg.cropBlackBars) return ++s_bandOff, false;
	if (g_stereo.menuMode) return ++s_bandMenu, false;  // (a plain read: a frame late at worst)
	if (!GameEffectsDrawNothing(c)) return ++s_bandSwapFailed, false;
	++s_bandHidden;
	return true;
}

void ReportBands() {
	DWORD now = GetTickCount();
	if (now - s_bandReportTick < 10000) return;
	s_bandReportTick = now;
	if (s_bandSeen)
		Log("cutscene bands (10 s): %u band draws, %u drawn as nothing; kept: %u switched off (CropBlackBars=0), %u flat menu, "
			"%u shader swap failed (3D picture %s)",
			s_bandSeen, s_bandHidden, s_bandOff, s_bandMenu, s_bandSwapFailed, StereoActive() ? "on" : "off");
	s_bandSeen = s_bandHidden = s_bandOff = s_bandMenu = s_bandSwapFailed = 0;
}

// Our scene texture: backbuffer-sized, with an RTV like the game's final-pass view of the
// backbuffer (the default view until that pass has been seen once).
bool EnsureScene(ID3D11Device* device, const D3D11_RENDER_TARGET_VIEW_DESC* rtvDesc) {
	D3D11_TEXTURE2D_DESC desc = s_backbufferDesc;
	bool sameView = rtvDesc ? memcmp(rtvDesc, &s_sceneRtvDesc, sizeof(*rtvDesc)) == 0 : true;
	if (s_scene && SameDesc(desc, s_sceneDesc) && sameView) return true;
	SafeRelease(s_sceneRtv);
	SafeRelease(s_scene);
	D3D11_TEXTURE2D_DESC newDesc = desc;
	newDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	newDesc.MiscFlags = 0;
	newDesc.CPUAccessFlags = 0;
	newDesc.Usage = D3D11_USAGE_DEFAULT;
	newDesc.MipLevels = 1;
	newDesc.ArraySize = 1;
	bool ok = SUCCEEDED(device->CreateTexture2D(&newDesc, nullptr, &s_scene)) &&
			  SUCCEEDED(device->CreateRenderTargetView(s_scene, rtvDesc, &s_sceneRtv));
	s_sceneDesc = desc;
	if (rtvDesc) s_sceneRtvDesc = *rtvDesc;
	else memset(&s_sceneRtvDesc, 0, sizeof(s_sceneRtvDesc));
	if (!ok) {
		SafeRelease(s_sceneRtv);
		SafeRelease(s_scene);
	}
	return ok;
}

// UIParam (112 B) and cbUIParam (144 B) as declared in the shaders; NieR allocates them rounded up
// (128 / 160 B were seen), so accept the whole range.
bool IsUiParamSize(UINT size) { return size >= 112 && size <= 176; }
}  // namespace

void UiOnMap(ID3D11Resource* res, void* data, UINT size) {
	if (!IsUiParamSize(size)) return;
	for (auto& p : s_pendingUiMaps)
		if (!p.res) {
			p = {res, (const float*)data};
			return;
		}
}

void UiBeforeUnmap(ID3D11Resource* res) {
	for (auto& p : s_pendingUiMaps)
		if (p.res == res) {
			float local[24];
			memcpy(local, p.data, sizeof(local));  // mapped memory may be write-combined: read once
			StoreUiParams(res, local);
			p.res = nullptr;
			return;
		}
}

void UiOnUpdate(ID3D11Resource* res, const void* data, UINT size) {
	if (IsUiParamSize(size)) StoreUiParams(res, (const float*)data);
}

void UiOnSetVSConstantBuffers(UINT start, UINT count, ID3D11Buffer* const* buffers) {
	if (start <= 1 && start + count > 1) s_vsSlot1Buffer = buffers[1 - start];
}

void UiSetBackbuffer(ID3D11Texture2D* backbuffer) {
	if (backbuffer) backbuffer->GetDesc(&s_backbufferDesc);
	s_backbuffer = backbuffer;
}

// ResizeBuffers releases the old backbuffer: forget it until the next Present (calling into the freed
// one crashed when the game switched to borderless).
void UiOnResizeBuffers() { s_backbuffer = nullptr; }

void UiBeginFrame() {
	ReportBands();
	if (s_uiStarted) s_framesWithUI++;
	if (s_uiStarted && !s_sceneReady && !s_loggedNoFinal && s_framesWithUI > 120) {
		s_loggedNoFinal = true;
		Log("ui split: UI draws found but the final pass into the backbuffer wasn't recognised; HUD stays in the "
			"3D image");
	}
	s_uiStarted = false;
	s_sceneReady = false;
	s_uiTarget = nullptr;
	s_drawIntoPreUi = false;
	s_drawIntoCoverage = false;
	s_screenCopy = nullptr;
	s_finalPassSaved = false;
	for (auto& p : s_pendingUiMaps) p.res = nullptr;
}

void UiOnSetVS(bool isUI) { s_uiShaderBound = isUI; }

namespace {
// Current render target 0's resource (raw pointer: valid while it stays bound).
ID3D11Resource* CurrentTarget(ID3D11DeviceContext* c) {
	ComPtr<ID3D11RenderTargetView> rtv;
	c->OMGetRenderTargets(1, &rtv, nullptr);
	if (!rtv) return nullptr;
	ComPtr<ID3D11Resource> res;
	rtv->GetResource(&res);
	return res.Get();
}

// Does the current pixel shader read `res` (slots t0-t7)?
bool PixelShaderReads(ID3D11DeviceContext* c, ID3D11Resource* res) {
	if (!res) return false;
	ID3D11ShaderResourceView* srvs[8] = {};
	c->PSGetShaderResources(0, 8, srvs);
	bool found = false;
	for (auto& view : srvs) {
		if (!view) continue;
		ComPtr<ID3D11Resource> viewRes;
		view->GetResource(&viewRes);
		if (viewRes.Get() == res) found = true;
		view->Release();
	}
	return found;
}

bool IsFullSize(ID3D11Resource* res) {
	D3D11_RESOURCE_DIMENSION dim;
	res->GetType(&dim);
	if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return false;
	D3D11_TEXTURE2D_DESC desc;
	((ID3D11Texture2D*)res)->GetDesc(&desc);
	return desc.Width == s_backbufferDesc.Width && desc.Height == s_backbufferDesc.Height;
}

// Distortion draw found (it reads this frame's screen copy): true = skip it.
bool OnDistortionDraw() {
	bool skip = !g_cfg.distortionFx && StereoActive();
	s_distortionDraws++;
	if (!s_loggedDistortion) {
		s_loggedDistortion = true;
		s_distortionReportTick = GetTickCount();
		Log("ui split: screen distortion effect (redraws a copy of the screen, e.g. when hit): %s",
			g_cfg.distortionFx ? "kept in the 3D image (DistortionEffects=1)" : "removed while in stereo (DistortionEffects=0)");
	} else if (GetTickCount() - s_distortionReportTick > 60000) {
		Log("ui split: %u screen distortion draws %s in the last minute", s_distortionDraws,
			g_cfg.distortionFx ? "kept" : "seen (removed while in stereo)");
		s_distortionReportTick = GetTickCount();
		s_distortionDraws = 0;
	}
	return skip;
}
}  // namespace

// Before a draw: at the first UI draw of the frame, keep a copy of the scene-only target.
// Returns true when the draw must be skipped.
bool UiBeforeDraw(ID3D11DeviceContext* c) {
	if (!s_backbuffer) return false;
	if (!s_uiShaderBound) {
		// During the UI: a draw into another full-size texture that reads the UI target is NieR
		// copying the screen for a distortion effect; draws into the UI target that read that
		// copy are the effect itself.
		if (!s_uiStarted || !s_uiTarget) return false;
		ID3D11Resource* target = CurrentTarget(c);
		if (!target || target == (ID3D11Resource*)s_backbuffer) return false;
		if (target == s_uiTarget) {
			if (!s_screenCopy || !PixelShaderReads(c, s_screenCopy)) return false;
			if (OnDistortionDraw()) return true;
			s_drawIntoPreUi = s_preUiRtv != nullptr;  // kept: part of the 3D image, not the HUD
			return false;
		}
		if (IsFullSize(target) && PixelShaderReads(c, s_uiTarget)) s_screenCopy = target;
		return false;
	}
	if (s_uiStarted) {
		s_drawIntoPreUi = false;
		s_drawIntoCoverage = false;
		if (s_screenCopy && CurrentTarget(c) == s_uiTarget && PixelShaderReads(c, s_screenCopy)) {
			if (OnDistortionDraw()) return true;
			s_drawIntoPreUi = s_preUiRtv != nullptr;  // kept: part of the 3D image, not the HUD
			return false;
		}
		const float* params = FindUiParams(s_vsSlot1Buffer);
		bool effect = params && IsScreenEffect(params);
		bool band = HideCutsceneBand(c, params);
		s_drawIntoPreUi = effect && s_preUiRtv;
		s_drawIntoCoverage = !effect && !band && s_sceneRtv && CurrentTarget(c) == s_uiTarget;
		return false;
	}
	ID3D11Resource* target = CurrentTarget(c);
	if (!target || target == (ID3D11Resource*)s_backbuffer) return false;
	D3D11_RESOURCE_DIMENSION dim;
	target->GetType(&dim);
	if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return false;
	D3D11_TEXTURE2D_DESC desc;
	((ID3D11Texture2D*)target)->GetDesc(&desc);
	const D3D11_TEXTURE2D_DESC& backbufferDesc = s_backbufferDesc;
	// the UI goes into a full-screen intermediate; ignore UI drawn into smaller offscreen textures
	if (desc.SampleDesc.Count != 1 || desc.Width != backbufferDesc.Width || desc.Height != backbufferDesc.Height) return false;
	ReplayStop(true);  // Synchronized: the scene is finished, the HUD is drawn once (not recorded)
	GpuTimingMark(c, "UI start");
	FoveatedSceneEnd(c);
	ReprojectAtUiStart(c);
	UpscaleAtUiStart(c);
	s_uiStarted = true;
	if (!s_preUiCopy || !SameDesc(desc, s_preUiDesc)) {
		SafeRelease(s_preUiSrv);
		SafeRelease(s_preUiCopy);
		D3D11_TEXTURE2D_DESC newDesc = desc;
		SafeRelease(s_preUiRtv);
		newDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		newDesc.MiscFlags = 0;
		newDesc.CPUAccessFlags = 0;
		newDesc.Usage = D3D11_USAGE_DEFAULT;
		HRESULT hr = DeviceOf(c)->CreateTexture2D(&newDesc, nullptr, &s_preUiCopy);
		if (FAILED(hr)) {
			if (LogBudget(s_preUiFailLogs, 3)) Log("ui split: pre-UI copy texture failed 0x%08x", (unsigned)hr);
			s_preUiCopy = nullptr;
			return false;
		}
		s_preUiDesc = desc;
		Log("ui split: UI is drawn into a %ux%u format %d target", desc.Width, desc.Height, (int)desc.Format);
	}
	c->CopyResource(s_preUiCopy, target);
	s_uiTarget = target;
	// A render target view on the copy, matching the game's view of its UI target, so screen
	// effects can be drawn into it too.
	ComPtr<ID3D11RenderTargetView> rtv;
	c->OMGetRenderTargets(1, &rtv, nullptr);
	if (rtv) {
		D3D11_RENDER_TARGET_VIEW_DESC rtvDesc;
		rtv->GetDesc(&rtvDesc);
		if (!s_preUiRtv || memcmp(&rtvDesc, &s_preUiRtvDesc, sizeof(rtvDesc)) != 0) {
			SafeRelease(s_preUiRtv);
			if (FAILED(DeviceOf(c)->CreateRenderTargetView(s_preUiCopy, &rtvDesc, &s_preUiRtv))) s_preUiRtv = nullptr;
			s_preUiRtvDesc = rtvDesc;
		}
	}
	// Coverage starts empty each frame.
	if (s_backbuffer && EnsureScene(DeviceOf(c), s_sceneRtvDesc.ViewDimension ? &s_sceneRtvDesc : nullptr)) {
		const FLOAT zero[4] = {0, 0, 0, 0};
		c->ClearRenderTargetView(s_sceneRtv, zero);
	}
	const float* params = FindUiParams(s_vsSlot1Buffer);
	bool effect = params && IsScreenEffect(params);
	bool band = HideCutsceneBand(c, params);
	s_drawIntoPreUi = effect && s_preUiRtv;
	s_drawIntoCoverage = !effect && !band && s_sceneRtv;
	return false;
}

// After a draw: if this was the final pass (into the backbuffer, reading the UI target), run it
// again on the pre-UI copy into our scene texture.
void UiAfterDraw(ID3D11DeviceContext* c, const DrawArgs& args) {
	if (s_drawIntoCoverage) {
		// same draw, alpha only, into the coverage (our scene texture's alpha channel)
		s_drawIntoCoverage = false;
		ReplayInto(c, s_sceneRtv, args, 0);
		return;
	}
	if (s_drawIntoPreUi) {
		// same draw, same state, into the scene-only copy
		s_drawIntoPreUi = false;
		ReplayInto(c, s_preUiRtv, args, -1);
		if (!s_loggedEffect) {
			s_loggedEffect = true;
			Log("ui split: full-screen overlay (film grain or screen effect) kept in the 3D image");
		}
		return;
	}
	if (!s_uiStarted || s_sceneReady || !s_uiTarget || !s_preUiCopy) return;
	if (CurrentTarget(c) != (ID3D11Resource*)s_backbuffer) return;
	ID3D11ShaderResourceView* srvs[8] = {};
	c->PSGetShaderResources(0, 8, srvs);
	int slot = -1;
	for (int i = 0; i < 8; i++) {
		if (!srvs[i] || slot >= 0) continue;
		ComPtr<ID3D11Resource> res;
		srvs[i]->GetResource(&res);
		if (res.Get() == s_uiTarget) slot = i;
	}
	ComPtr<ID3D11RenderTargetView> rtv;
	ComPtr<ID3D11DepthStencilView> dsv;
	if (slot >= 0) c->OMGetRenderTargets(1, &rtv, &dsv);
	bool ok = slot >= 0 && rtv;
	ID3D11Device* device = DeviceOf(c);
	if (ok) {
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
		srvs[slot]->GetDesc(&srvDesc);
		if (!s_preUiSrv || memcmp(&srvDesc, &s_preUiSrvDesc, sizeof(srvDesc)) != 0) {
			SafeRelease(s_preUiSrv);
			ok = SUCCEEDED(device->CreateShaderResourceView(s_preUiCopy, &srvDesc, &s_preUiSrv));
			s_preUiSrvDesc = srvDesc;
		}
	}
	if (ok) {
		D3D11_RENDER_TARGET_VIEW_DESC rtvDesc;
		rtv->GetDesc(&rtvDesc);
		bool sameView = s_scene && memcmp(&rtvDesc, &s_sceneRtvDesc, sizeof(rtvDesc)) == 0;
		ok = EnsureScene(device, &rtvDesc);
		if (ok && !sameView) {  // new texture: this frame's coverage is lost, start from "no UI"
			const FLOAT zero[4] = {0, 0, 0, 0};
			c->ClearRenderTargetView(s_sceneRtv, zero);
		}
	}
	if (ok) {
		// Same pipeline state, different input and output: the scene without the UI. Colour only,
		// so the UI coverage gathered in the alpha channel stays.
		if (StereoSynchronizedFrame()) {  // the same pass will make the right eye's scene image
			ReplayCaptureFinalPassState(c);
			s_finalPassSaved = true;
			s_finalPassSlot = slot;
			s_finalPassArgs = args;
		}
		D3D11_VIEWPORT gameViewport;
		bool stretched = StretchFinalPass(c, &gameViewport);
		if (stretched) {  // the game's own pass again, over the whole backbuffer
			const FLOAT black[4] = {0, 0, 0, 1};
			c->ClearRenderTargetView(rtv.Get(), black);
			OriginalDraw(c, args);
		}
		c->PSSetShaderResources(slot, 1, &s_preUiSrv);
		ReplayInto(c, s_sceneRtv, args, 1);
		c->PSSetShaderResources(slot, 1, &srvs[slot]);
		if (stretched) c->RSSetViewports(1, &gameViewport);
		s_sceneReady = true;
		if (!s_loggedWorking) {
			s_loggedWorking = true;
			Log("ui split: working (final pass reads the UI target at slot t%d); HUD and menus go to a flat layer",
				slot);
		}
	}
	for (auto& srv : srvs)
		if (srv) srv->Release();
}

ID3D11Texture2D* UiSceneTexture() { return s_sceneReady ? s_scene : nullptr; }

// Synchronized, during the right eye's playback (which ended where the UI starts): the UI target now
// holds the right eye's finished scene. Copy it and run the game's final pass on the copy into our
// scene texture, exactly as for the left eye but without any UI (alpha = no coverage).
bool UiSecondEyeScene(ID3D11DeviceContext* c) {
	if (!s_finalPassSaved || !s_sceneReady || !s_uiTarget || !s_preUiCopy || !s_preUiSrv || !s_sceneRtv) return false;
	c->CopyResource(s_preUiCopy, s_uiTarget);
	const FLOAT zero[4] = {0, 0, 0, 0};
	c->ClearRenderTargetView(s_sceneRtv, zero);
	ReplayApplyFinalPassState(c);
	c->PSSetShaderResources((UINT)s_finalPassSlot, 1, &s_preUiSrv);
	D3D11_VIEWPORT gameViewport;
	bool stretched = StretchFinalPass(c, &gameViewport);
	ReplayInto(c, s_sceneRtv, s_finalPassArgs, 1);
	if (stretched) c->RSSetViewports(1, &gameViewport);
	return true;
}
