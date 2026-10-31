// upscale.cpp - temporal upscaling of the 3D image for the headset with DLSS ([Upscaler] Mode, Quality).
#include "render/upscale.h"
#include "core/com.h"
#include "core/locks.h"
#include "core/config.h"
#include "core/log.h"
#include "core/pure.h"
#include "render/camera.h"
#include "render/frame_share.h"
#include "render/replay.h"
#include "render/reproject.h"
#include "ui/menu.h"
#include <stdio.h>
#include <string.h>
#include "render/dlss.h"
#include <d3dcompiler.h>

namespace {
// the shader's cbuffer P, the struct that fills it and the buffer it is copied into: one size
constexpr UINT kPrepConstantsSize = 144;
constexpr char kHlsl[] = R"(
cbuffer P : register(b0) {
	row_major float4x4 T;  // this eye's view now -> the same eye's view in its previous frame
	float4 cur;            // projection now: x scale, y scale, P22, P23 (z_ndc = (P22 z + P23) / -z)
	float4 prev;           // its previous projection
	float4 jit;            // this frame's jitter in NDC (x, y), render width, height
	float4 opt;            // x > 0: the previous frame is known (else no motion)
	float4 lens;           // AsymmetricEyes: lens centre as clip-space terms, now (x, y) and then (z, w)
};
Texture2D<float> dep : register(t0);
Texture2D col : register(t1);
RWTexture2D<float4> outCol : register(u0);
RWTexture2D<float> outDep : register(u1);
RWTexture2D<float2> outMv : register(u2);
[numthreads(8, 8, 1)]
void prep(uint3 id : SV_DispatchThreadID) {
	if (id.x >= (uint)jit.z || id.y >= (uint)jit.w) return;
	float d = dep.Load(int3(id.xy, 0));
	outCol[id.xy] = col.Load(int3(id.xy, 0));
	outDep[id.xy] = d;
	float2 mv = 0;
	if (opt.x > 0) {
		float2 uv = (id.xy + 0.5) / jit.zw;
		float2 ndc = float2(uv.x * 2 - 1, 1 - uv.y * 2) - jit.xy;  // where the point is without jitter
		float den = -d - cur.z;
		float z = abs(den) > 1e-9 ? cur.w / den : -1e7;
		if (!(z < -1e-4) || z < -1e7) z = -1e7;  // sky: so far away only turning moves it
		float3 p = float3((ndc.x + lens.x) * -z / cur.x, (ndc.y + lens.y) * -z / cur.y, z);
		float3 q = mul(T, float4(p, 1)).xyz;
		if (q.z < -1e-4) {
			float2 pn = float2(q.x * prev.x + lens.z * q.z, q.y * prev.y + lens.w * q.z) / -q.z;
			mv = float2(pn.x - ndc.x, ndc.y - pn.y) * 0.5 * jit.zw;  // pixels to where it was (y down)
		}
	}
	outMv[id.xy] = mv;
}

Texture2D src : register(t0);    // DLSS's output, or the scene to stretch
Texture2D alpha : register(t1);  // the scene: HUD coverage in alpha
SamplerState smp : register(s0);
cbuffer C : register(b0) { float4 size; };  // output width, height
float4 vs(uint id : SV_VertexID) : SV_Position {
	float2 t = float2((id << 1) & 2, id & 2);
	return float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 ps(float4 pos : SV_Position) : SV_Target {
	float2 uv = pos.xy / size.xy;
	return float4(src.SampleLevel(smp, uv, 0).rgb, alpha.SampleLevel(smp, uv, 0).a);
}
)";

bool s_shadersReady, s_shadersFailed;
ID3D11ComputeShader* s_prepShader;
ID3D11VertexShader* s_composeVs;
ID3D11PixelShader* s_composePs;
ID3D11Buffer* s_prepConstants;
ID3D11Buffer* s_composeConstants;
ID3D11SamplerState* s_linearSampler;

// this frame (from UpscaleBeginPresent)
bool s_upscaling;           // scene images are enlarged
UINT s_renderW, s_renderH;  // render size
UINT s_outputW, s_outputH;  // output size
int32_t s_dlssMode;         // DLSS mode for that enlargement (pure::upscale_mode)
UINT s_targetW, s_targetH;  // the output the headset wants (native x Output), 0 = not known yet
uint32_t s_presentCount;    // Presents so far (the depth copies are tagged with it)

// per eye
struct EyeState {
	ID3D11Texture2D* depthCopy;  // readable twin of the game's depth buffer
	ID3D11ShaderResourceView* depthCopySrv;
	uint32_t depthPresentCount;  // s_presentCount of the frame the depth belongs to
	bool depthValid;
	bool resetHistory;  // the next DLSS frame starts a new history
	uint32_t jitterIndex;
	uint32_t upscaled, stretched;  // eye images since the last report
} s_eyes[2];
D3D11_TEXTURE2D_DESC s_depthCopyDesc;

// render-size inputs for DLSS (shared by the eyes: they are used one after the other)
ID3D11Texture2D *s_dlssColor, *s_dlssDepth, *s_dlssMotion;
ID3D11UnorderedAccessView *s_dlssColorUav, *s_dlssDepthUav, *s_dlssMotionUav;
// output size
ID3D11Texture2D* s_dlssOutput;
ID3D11ShaderResourceView* s_dlssOutputSrv;
ID3D11Texture2D* s_headsetImage;  // what the headset gets: the scene's format
ID3D11RenderTargetView* s_headsetImageRtv;
// the sizes and format the textures above were made for
struct TextureSizes {
	UINT renderW, renderH, outputW, outputH;
	DXGI_FORMAT format;
} s_texturesMadeFor;
// SRV of the game-side scene texture (hud_layer.cpp keeps it while the size stays)
ID3D11Texture2D* s_sceneSrvTexture;
ID3D11ShaderResourceView* s_sceneSrv;

SRWLOCK s_statusLock = SRWLOCK_INIT;
char s_status[128];
char s_advice[160];
DWORD s_lastReportTick;

const char* const kModeNames[5] = {"DLAA", "Quality", "Balanced", "Performance", "Ultra performance"};

DXGI_FORMAT TypedFormat(DXGI_FORMAT f) {
	switch (f) {
		case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
		case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
		case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
		case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
		case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
		default: return f;
	}
}
bool Is8Bit(DXGI_FORMAT f) {
	f = TypedFormat(f);
	return f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8X8_UNORM ||
		   f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
}
// readable twin of a depth format, and the view that reads its depth
bool DepthFormats(DXGI_FORMAT f, DXGI_FORMAT* tex, DXGI_FORMAT* srv) {
	switch (f) {
		case DXGI_FORMAT_D24_UNORM_S8_UINT:
		case DXGI_FORMAT_R24G8_TYPELESS:
			*tex = DXGI_FORMAT_R24G8_TYPELESS;
			*srv = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
			return true;
		case DXGI_FORMAT_D32_FLOAT:
		case DXGI_FORMAT_R32_TYPELESS:
			*tex = DXGI_FORMAT_R32_TYPELESS;
			*srv = DXGI_FORMAT_R32_FLOAT;
			return true;
		case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
		case DXGI_FORMAT_R32G8X24_TYPELESS:
			*tex = DXGI_FORMAT_R32G8X24_TYPELESS;
			*srv = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
			return true;
		case DXGI_FORMAT_D16_UNORM:
		case DXGI_FORMAT_R16_TYPELESS:
			*tex = DXGI_FORMAT_R16_TYPELESS;
			*srv = DXGI_FORMAT_R16_UNORM;
			return true;
		default: return false;
	}
}

void SetStatus(const char* text, const char* advice) {
	ExclusiveLock guard(&s_statusLock);
	snprintf(s_status, sizeof(s_status), "%s", text);
	snprintf(s_advice, sizeof(s_advice), "%s", advice);
}

bool InitShaders(ID3D11Device* dev) {
	if (s_shadersReady || s_shadersFailed) return s_shadersReady;
	ID3DBlob *cs = nullptr, *vs = nullptr, *ps = nullptr, *err = nullptr;
	const UINT fl = D3DCOMPILE_OPTIMIZATION_LEVEL3;
	HRESULT hr = D3DCompile(kHlsl, sizeof(kHlsl) - 1, "Upscale", nullptr, nullptr, "prep", "cs_5_0", fl, 0, &cs, &err);
	if (SUCCEEDED(hr)) hr = D3DCompile(kHlsl, sizeof(kHlsl) - 1, "Upscale", nullptr, nullptr, "vs", "vs_5_0", fl, 0, &vs, &err);
	if (SUCCEEDED(hr)) hr = D3DCompile(kHlsl, sizeof(kHlsl) - 1, "Upscale", nullptr, nullptr, "ps", "ps_5_0", fl, 0, &ps, &err);
	if (SUCCEEDED(hr)) hr = dev->CreateComputeShader(cs->GetBufferPointer(), cs->GetBufferSize(), nullptr, &s_prepShader);
	if (SUCCEEDED(hr)) hr = dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &s_composeVs);
	if (SUCCEEDED(hr)) hr = dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &s_composePs);
	if (FAILED(hr)) Log("ERROR: upscaler shaders: 0x%08x %s", (unsigned)hr, err ? (const char*)err->GetBufferPointer() : "");
	SafeRelease(cs), SafeRelease(vs), SafeRelease(ps), SafeRelease(err);
	D3D11_BUFFER_DESC bd = {};
	bd.ByteWidth = kPrepConstantsSize;
	bd.Usage = D3D11_USAGE_DEFAULT;
	bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	if (SUCCEEDED(hr)) hr = dev->CreateBuffer(&bd, nullptr, &s_prepConstants);
	bd.ByteWidth = 16;
	if (SUCCEEDED(hr)) hr = dev->CreateBuffer(&bd, nullptr, &s_composeConstants);
	D3D11_SAMPLER_DESC sd = {};
	sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.MaxLOD = D3D11_FLOAT32_MAX;
	if (SUCCEEDED(hr)) hr = dev->CreateSamplerState(&sd, &s_linearSampler);
	s_shadersReady = SUCCEEDED(hr);
	s_shadersFailed = !s_shadersReady;
	return s_shadersReady;
}

void ReleaseTargets() {
	SafeRelease(s_dlssColorUav), SafeRelease(s_dlssDepthUav), SafeRelease(s_dlssMotionUav);
	SafeRelease(s_dlssColor), SafeRelease(s_dlssDepth), SafeRelease(s_dlssMotion);
	SafeRelease(s_dlssOutputSrv), SafeRelease(s_dlssOutput);
	SafeRelease(s_headsetImageRtv), SafeRelease(s_headsetImage);
	SafeRelease(s_sceneSrv), SafeRelease(s_sceneSrvTexture);
	s_texturesMadeFor = {};
}

ID3D11Texture2D* MakeTexture(ID3D11Device* dev, UINT w, UINT h, DXGI_FORMAT f, UINT bind) {
	D3D11_TEXTURE2D_DESC d = {};
	d.Width = w, d.Height = h, d.MipLevels = d.ArraySize = 1, d.Format = f, d.SampleDesc.Count = 1;
	d.Usage = D3D11_USAGE_DEFAULT;
	d.BindFlags = bind;
	ID3D11Texture2D* t = nullptr;
	return SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &t)) ? t : nullptr;
}

// Textures for this render size, output size and scene format.
bool EnsureTargets(ID3D11Device* dev, DXGI_FORMAT sceneFmt) {
	const TextureSizes& made = s_texturesMadeFor;
	if (s_headsetImage && made.renderW == s_renderW && made.renderH == s_renderH && made.outputW == s_outputW && made.outputH == s_outputH &&
		made.format == sceneFmt)
		return true;
	ReleaseTargets();
	DlssReleaseFeatures();  // sizes changed: new histories
	for (auto& e : s_eyes) e.resetHistory = true;
	// DLSS works in 8 bits per channel for 8-bit images, else half floats
	DXGI_FORMAT workFormat = Is8Bit(sceneFmt) ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT;
	const UINT computeBind = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	s_dlssColor = MakeTexture(dev, s_renderW, s_renderH, workFormat, computeBind);
	s_dlssDepth = MakeTexture(dev, s_renderW, s_renderH, DXGI_FORMAT_R32_FLOAT, computeBind);
	s_dlssMotion = MakeTexture(dev, s_renderW, s_renderH, DXGI_FORMAT_R16G16_FLOAT, computeBind);
	s_dlssOutput = MakeTexture(dev, s_outputW, s_outputH, workFormat, computeBind);
	s_headsetImage = MakeTexture(dev, s_outputW, s_outputH, sceneFmt, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
	bool ok = s_dlssColor && s_dlssDepth && s_dlssMotion && s_dlssOutput && s_headsetImage;
	ok = ok && SUCCEEDED(dev->CreateUnorderedAccessView(s_dlssColor, nullptr, &s_dlssColorUav)) &&
		 SUCCEEDED(dev->CreateUnorderedAccessView(s_dlssDepth, nullptr, &s_dlssDepthUav)) &&
		 SUCCEEDED(dev->CreateUnorderedAccessView(s_dlssMotion, nullptr, &s_dlssMotionUav)) &&
		 SUCCEEDED(dev->CreateShaderResourceView(s_dlssOutput, nullptr, &s_dlssOutputSrv));
	D3D11_RENDER_TARGET_VIEW_DESC rv = {};
	rv.Format = TypedFormat(sceneFmt);
	rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
	ok = ok && SUCCEEDED(dev->CreateRenderTargetView(s_headsetImage, &rv, &s_headsetImageRtv));
	if (!ok) {
		Log("ERROR: upscaler: could not create its textures (%ux%u -> %ux%u, format %d)", s_renderW, s_renderH, s_outputW, s_outputH,
			(int)sceneFmt);
		ReleaseTargets();
		return false;
	}
	s_texturesMadeFor = {s_renderW, s_renderH, s_outputW, s_outputH, sceneFmt};
	return true;
}

ID3D11ShaderResourceView* SceneSrv(ID3D11Device* dev, ID3D11Texture2D* scene) {
	if (scene == s_sceneSrvTexture && s_sceneSrv) return s_sceneSrv;
	SafeRelease(s_sceneSrv), SafeRelease(s_sceneSrvTexture);
	D3D11_TEXTURE2D_DESC d;
	scene->GetDesc(&d);
	D3D11_SHADER_RESOURCE_VIEW_DESC v = {};
	v.Format = TypedFormat(d.Format);
	v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	v.Texture2D.MipLevels = 1;
	if (FAILED(dev->CreateShaderResourceView(scene, &v, &s_sceneSrv))) return nullptr;
	s_sceneSrvTexture = scene;
	scene->AddRef();  // the view's texture: compared by address, so keep it alive
	return s_sceneSrv;
}

// The depth twin for this depth buffer's size and format.
bool EnsureDepth(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC& src) {
	if (s_eyes[0].depthCopy && s_depthCopyDesc.Width == src.Width && s_depthCopyDesc.Height == src.Height &&
		s_depthCopyDesc.Format == src.Format)
		return true;
	DXGI_FORMAT tf, sf;
	if (!DepthFormats(src.Format, &tf, &sf) || src.SampleDesc.Count != 1) return false;
	for (auto& e : s_eyes) {
		SafeRelease(e.depthCopySrv), SafeRelease(e.depthCopy);
		e.depthValid = false;
		D3D11_SHADER_RESOURCE_VIEW_DESC v = {};
		v.Format = sf;
		v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		v.Texture2D.MipLevels = 1;
		e.depthCopy = MakeTexture(dev, src.Width, src.Height, tf, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE);
		if (!e.depthCopy || FAILED(dev->CreateShaderResourceView(e.depthCopy, &v, &e.depthCopySrv))) {
			Log("upscaler: could not create a readable depth copy (format %d)", (int)src.Format);
			SafeRelease(e.depthCopy);
			return false;
		}
	}
	s_depthCopyDesc = src;
	return true;
}

void CopyDepth(ID3D11DeviceContext* c, int eye, uint32_t present) {
	ID3D11Resource* d = ReprojectSceneDepth();
	if (!s_upscaling || eye < 0 || eye > 1 || !d) return;
	ComPtr<ID3D11Device> dev;
	c->GetDevice(&dev);
	if (!dev) return;
	D3D11_TEXTURE2D_DESC dd;
	((ID3D11Texture2D*)d)->GetDesc(&dd);
	if (dd.Width == s_renderW && dd.Height == s_renderH && EnsureDepth(dev.Get(), dd)) {
		c->CopyResource(s_eyes[eye].depthCopy, d);
		s_eyes[eye].depthValid = true;
		s_eyes[eye].depthPresentCount = present;
	}
}

// Upscales `scene` (render size) for `eye` into s_dlssOutput. False if this frame can't be.
bool RunDlss(ID3D11DeviceContext* c, ID3D11Device* dev, int eye, ID3D11ShaderResourceView* sceneSrv) {
	EyeState& e = s_eyes[eye];
	float T[16], cur[4], prev[4], centres[4], jit[2];
	bool hasPrev = false;
	if (!e.depthValid || e.depthPresentCount != s_presentCount) return false;
	if (!StereoUpscaleCams(eye, T, cur, prev, centres, jit, &hasPrev)) return false;
	// depth convention: farther = larger value normally, smaller with reversed depth
	float dNear = -cur[2] - cur[3] / -1.0f, dFar = -cur[2] - cur[3] / -1000.0f;
	bool inverted = dFar < dNear;
	if (!DlssPrepare(c, eye, s_renderW, s_renderH, s_outputW, s_outputH, s_dlssMode, g_cfg.dlssModel, inverted)) return false;
	struct {
		float T[16];
		float cur[4], prev[4], jit[4], opt[4], lens[4];
	} constants;
	static_assert(sizeof(constants) == kPrepConstantsSize, "cbuffer P");
	memcpy(constants.T, T, 64);
	memcpy(constants.cur, cur, 16);
	memcpy(constants.prev, prev, 16);
	constants.jit[0] = 2 * jit[0] / (float)s_renderW;
	constants.jit[1] = -2 * jit[1] / (float)s_renderH;
	constants.jit[2] = (float)s_renderW;
	constants.jit[3] = (float)s_renderH;
	constants.opt[0] = hasPrev ? 1.f : 0.f;
	constants.opt[1] = constants.opt[2] = constants.opt[3] = 0;
	memcpy(constants.lens, centres, 16);  // this frame's lens centre (x, y) and the previous frame's (z, w)
	c->UpdateSubresource(s_prepConstants, 0, nullptr, &constants, 0, 0);
	c->CSSetShader(s_prepShader, nullptr, 0);
	c->CSSetConstantBuffers(0, 1, &s_prepConstants);
	ID3D11ShaderResourceView* srvs[2] = {e.depthCopySrv, sceneSrv};
	c->CSSetShaderResources(0, 2, srvs);
	ID3D11UnorderedAccessView* uavs[3] = {s_dlssColorUav, s_dlssDepthUav, s_dlssMotionUav};
	c->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);
	c->Dispatch((s_renderW + 7) / 8, (s_renderH + 7) / 8, 1);
	ID3D11ShaderResourceView* nullSrv[2] = {};
	ID3D11UnorderedAccessView* nullUav[3] = {};
	c->CSSetShaderResources(0, 2, nullSrv);
	c->CSSetUnorderedAccessViews(0, 3, nullUav, nullptr);
	c->CSSetShader(nullptr, nullptr, 0);
	DlssInputs in = {};
	in.color = s_dlssColor;
	in.depth = s_dlssDepth;
	in.motion = s_dlssMotion;
	in.output = s_dlssOutput;
	in.jitterX = jit[0];
	in.jitterY = jit[1];
	in.reset = e.resetHistory || !hasPrev;
	(void)dev;
	if (!DlssEvaluate(c, eye, in)) return false;
	e.resetHistory = false;
	e.jitterIndex++;
	return true;
}
}  // namespace

bool UpscaleWantsDepth() { return g_cfg.upscaler != 0; }

bool UpscaleBeginPresent(ID3D11DeviceContext* c, UINT w, UINT h, UINT* outW, UINT* outH) {
	s_presentCount++;
	*outW = w;
	*outH = h;
	bool was = s_upscaling;
	s_upscaling = false;
	// The output follows the headset (its recommended detail x Output), as in other games the
	// display resolution does; the game's resolution is the input and decides the mode. Small changes
	// of the target (a few %) keep the current size: every change rebuilds DLSS and the hand-off.
	EnterCriticalSection(&g_share.lock);
	UINT nativeW = g_share.nativeW, nativeH = g_share.nativeH;
	LeaveCriticalSection(&g_share.lock);
	if (!nativeW || !nativeH) {
		nativeW = (UINT)g_cfg.headsetWidth;
		nativeH = (UINT)g_cfg.headsetHeight;
	}
	UINT wantW = (UINT)(nativeW * g_cfg.upscaleOutput / 100), wantH = (UINT)(nativeH * g_cfg.upscaleOutput / 100);
	if (!s_targetW || wantW > s_targetW * 1.03f || wantW * 1.03f < s_targetW || wantH > s_targetH * 1.03f || wantH * 1.03f < s_targetH)
		s_targetW = wantW, s_targetH = wantH;
	if (g_cfg.upscaler) {
		ComPtr<ID3D11Device> dev;
		c->GetDevice(&dev);
		if (dev) {
			UINT ow, oh;
			if (DlssInit(dev.Get()) && InitShaders(dev.Get()) && pure::upscale_output(w, h, s_targetW, s_targetH, &ow, &oh)) {
				s_upscaling = true;
				s_renderW = w, s_renderH = h, s_outputW = ow, s_outputH = oh;
				s_dlssMode = pure::upscale_mode((float)ow / (float)w);
				*outW = ow;
				*outH = oh;
			}
		}
	}
	if (was && !s_upscaling) {
		ReleaseTargets();
		DlssReleaseFeatures();
		for (auto& e : s_eyes) e.depthValid = false;
	}
	char status[128], advice[160];
	advice[0] = 0;
	if (!g_cfg.upscaler)
		status[0] = 0;
	else if (s_upscaling)
		snprintf(status, sizeof(status), "Upscaler: %s (%.2fx) - the game draws %ux%u, your headset gets %ux%u. %s", kModeNames[s_dlssMode],
				 (double)s_outputW / s_renderW, s_renderW, s_renderH, s_outputW, s_outputH, DlssStatus());
	else
		snprintf(status, sizeof(status), "%s", DlssStatus());
	if (s_upscaling && s_targetW && s_targetH && !g_cfg.panelResolution) {
		// the game resolutions that give each mode for this output (same shape as the game's image)
		uint32_t qualityW = w, qualityH = h, balancedW, balancedH, performanceW, performanceH;
		pure::upscale_output(w, h, s_targetW, s_targetH, &qualityW, &qualityH);  // the target in the game's shape
		if (qualityW == w) {                                                     // the game is at or above the target: show the target itself
			float r = (float)s_targetW / w > (float)s_targetH / h ? (float)s_targetW / w : (float)s_targetH / h;
			qualityW = (uint32_t)(w * r) & ~1u, qualityH = (uint32_t)(h * r) & ~1u;
		}
		uint32_t ow = qualityW, oh = qualityH;
		pure::upscale_input_for(ow, oh, 1, &qualityW, &qualityH);
		pure::upscale_input_for(ow, oh, 2, &balancedW, &balancedH);
		pure::upscale_input_for(ow, oh, 3, &performanceW, &performanceH);
		snprintf(advice, sizeof(advice),
				 "In the game's own resolution list: %ux%u = sharpest, %ux%u = quality, %ux%u = balanced, "
				 "%ux%u = fastest",
				 ow, oh, qualityW, qualityH, balancedW, balancedH, performanceW, performanceH);
	}
	SetStatus(status, advice);
	static char lastAdvice[160];
	if (strcmp(lastAdvice, advice)) {
		snprintf(lastAdvice, sizeof(lastAdvice), "%s", advice);
		MenuSetInfo(INFO_UPSCALE_ADVICE, advice);
	}
	if (GetTickCount() - s_lastReportTick > 30000) {
		s_lastReportTick = GetTickCount();
		if (s_upscaling && (s_eyes[0].upscaled + s_eyes[0].stretched + s_eyes[1].upscaled + s_eyes[1].stretched)) {
			Log("upscaler: %s; last 30 s: left %u upscaled / %u stretched, right %u / %u", status, s_eyes[0].upscaled,
				s_eyes[0].stretched, s_eyes[1].upscaled, s_eyes[1].stretched);
			char info[160];
			snprintf(info, sizeof(info), "Upscaler: %s; %u of %u eye images upscaled (30 s)", status,
					 s_eyes[0].upscaled + s_eyes[1].upscaled,
					 s_eyes[0].upscaled + s_eyes[1].upscaled + s_eyes[0].stretched + s_eyes[1].stretched);
			MenuSetInfo(INFO_UPSCALER, info);
		} else if (g_cfg.upscaler) {
			char info[160];
			snprintf(info, sizeof(info), "Upscaler: %s", status);
			MenuSetInfo(INFO_UPSCALER, info);
		}
		for (auto& e : s_eyes) e.upscaled = e.stretched = 0;
	}
	return s_upscaling;
}

ID3D11Texture2D* UpscaleScene(ID3D11DeviceContext* c, int eye, ID3D11Texture2D* scene) {
	if (!s_upscaling || !scene) return scene;
	ComPtr<ID3D11Device> dev;
	c->GetDevice(&dev);
	if (!dev) return nullptr;
	D3D11_TEXTURE2D_DESC sceneDesc;
	scene->GetDesc(&sceneDesc);
	if (sceneDesc.Width != s_renderW || sceneDesc.Height != s_renderH || !EnsureTargets(dev.Get(), sceneDesc.Format)) return nullptr;
	ID3D11ShaderResourceView* sceneSrv = SceneSrv(dev.Get(), scene);
	if (!sceneSrv) return nullptr;
	g_replaying = true;  // our own calls (and DLSS's): the hooks pass them straight through
	StateSaveTemp(c);
	c->ClearState();
	bool upscaled = eye >= 0 && eye < 2 && RunDlss(c, dev.Get(), eye, sceneSrv);
	if (eye >= 0 && eye < 2) {
		if (upscaled) s_eyes[eye].upscaled++;
		else {
			s_eyes[eye].stretched++;
			s_eyes[eye].resetHistory = true;
		}
	}
	// the image for the headset: DLSS's colour (or the stretched scene) with the scene's HUD coverage
	c->ClearState();
	float size[4] = {(float)s_outputW, (float)s_outputH, 0, 0};
	c->UpdateSubresource(s_composeConstants, 0, nullptr, size, 0, 0);
	D3D11_VIEWPORT vp = {0, 0, (float)s_outputW, (float)s_outputH, 0, 1};
	c->RSSetViewports(1, &vp);
	c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	c->VSSetShader(s_composeVs, nullptr, 0);
	c->PSSetShader(s_composePs, nullptr, 0);
	c->PSSetConstantBuffers(0, 1, &s_composeConstants);
	ID3D11ShaderResourceView* srvs[2] = {upscaled ? s_dlssOutputSrv : sceneSrv, sceneSrv};
	c->PSSetShaderResources(0, 2, srvs);
	c->PSSetSamplers(0, 1, &s_linearSampler);
	c->OMSetRenderTargets(1, &s_headsetImageRtv, nullptr);
	c->Draw(3, 0);
	StateRestoreTemp(c);
	g_replaying = false;
	return s_headsetImage;
}

void UpscaleAtUiStart(ID3D11DeviceContext* c) { CopyDepth(c, StereoFrameEye(), s_presentCount + 1); }  // for the coming Present
void UpscaleCaptureDepth(ID3D11DeviceContext* c, int eye) { CopyDepth(c, eye, s_presentCount); }

bool UpscaleJitter(int eye, float ndc[2], float pixels[2]) {
	if (!s_upscaling || eye < 0 || eye > 1 || !s_renderW || !s_renderH) return false;
	uint32_t n = pure::jitter_phases((float)s_outputW / (float)s_renderW);
	uint32_t i = s_eyes[eye].jitterIndex % n + 1;
	pixels[0] = pure::halton(i, 2) - 0.5f;
	pixels[1] = pure::halton(i, 3) - 0.5f;
	ndc[0] = 2 * pixels[0] / (float)s_renderW;
	ndc[1] = -2 * pixels[1] / (float)s_renderH;
	return true;
}

void UpscaleRelease() {
	ReleaseTargets();
	DlssReleaseFeatures();
	for (auto& e : s_eyes) {
		SafeRelease(e.depthCopySrv), SafeRelease(e.depthCopy);
		e.depthValid = false;
		e.resetHistory = true;
	}
	s_depthCopyDesc = {};
}

// Write the upscaler status to the char*
void UpscaleStatus(char* out, size_t cap) {
	SharedLock guard(&s_statusLock);
	snprintf(out, cap, "%s", s_status);
}
