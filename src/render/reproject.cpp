// reproject.cpp - "reproject" one eyes frame at the other eyes perspective
#include "render/reproject.h"
#include "core/com.h"
#include "core/config.h"
#include "core/log.h"
#include "render/camera.h"
#include "render/frame_share.h"
#include "render/replay.h"
#include "render/upscale.h"
#include "ui/menu.h"
#include <stdio.h>
#include <string.h>
#include <d3dcompiler.h>

namespace {
// cbuffer P in the shader below, the constants struct that fills it and the buffer it is copied
// into are all this size: a shader whose cbuffer is larger than the buffer bound to it reads past the
// end (a garbage pointer deep inside the D3D11 runtime, 2026-10-27).
constexpr UINT kWarpConstantsSize = 176;
constexpr char kHlsl[] = R"(
cbuffer P : register(b0) {
	row_major float4x4 T;   // source eye view -> destination eye view
	float4 proj;            // source projection: x scale, y scale, P22, P23 (z_ndc = (P22 z + P23) / -z)
	float4 size;            // width, height, grid step, grid width in cells
	float4 dscale;          // depth pixels per colour pixel (x, y): 1, or less for an upscaled colour image
	float4 lens;            // source lens centre as clip-space terms (AsymmetricEyes=2; 0 when centred)
	float4 projDst;         // destination projection (the same as proj when an eye is moved to its own new view)
	float4 lensDst;         // destination lens centre
	float4 edge;            // x: depth ratio and y: stretch (grid steps) above which a triangle spans a gap;
	                        // z > 0: such triangles are dropped (AFW combined: the previous-frame picture fills them)
};
Texture2D<float> dep : register(t0);
Texture2D col : register(t1);
SamplerState smp : register(s0);
struct V {
	float4 pos : SV_Position;
	float2 uv : TEXCOORD0;
	nointerpolation float gap : TEXCOORD1;
};
// one grid corner: the source image's pixel there, moved into the destination view
void Corner(uint2 c, out float4 clip, out float2 uv, out float dist) {
	float2 px = min(float2(c) * size.z, size.xy - 1);
	float d = dep.Load(int3(px * dscale.xy, 0));
	uv = (px + 0.5) / size.xy;
	float2 ndc = float2(uv.x * 2 - 1, 1 - uv.y * 2);
	float z = proj.w / (-d - proj.z);  // view-space z (negative in front)
	float3 p = float3((ndc.x + lens.x) * -z / proj.x, (ndc.y + lens.y) * -z / proj.y, z);
	float3 q = mul(T, float4(p, 1)).xyz;
	clip = float4(q.x * projDst.x + lensDst.x * q.z, q.y * projDst.y + lensDst.y * q.z, projDst.z * q.z + projDst.w, -q.z);
	dist = -z;
}
V vs(uint id : SV_VertexID) {
	uint cell = id / 6, k = id % 6, j = k % 3;
	uint gw = (uint)size.w;
	uint2 c = uint2(cell % gw, cell / gw);
	// each vertex works out its whole triangle, so all three agree on whether it spans a gap
	uint2 c0 = k < 3 ? uint2(0, 0) : uint2(1, 0);
	uint2 c1 = k < 3 ? uint2(1, 0) : uint2(1, 1);
	uint2 c2 = uint2(0, 1);
	float4 p0, p1, p2;
	float2 u0, u1, u2;
	float d0, d1, d2;
	Corner(c + c0, p0, u0, d0);
	Corner(c + c1, p1, u1, d1);
	Corner(c + c2, p2, u2, d2);
	bool gap = false;
	if (edge.x > 0 && p0.w > 0 && p1.w > 0 && p2.w > 0) {
		// a depth edge in the source that the move pulled open: something the destination eye sees and
		// the source eye did not (0.4.1's test, its thresholds)
		float dmin = min(d0, min(d1, d2)), dmax = max(d0, max(d1, d2));
		float2 h = 0.5 * size.xy;
		float2 s0 = p0.xy / p0.w * h, s1 = p1.xy / p1.w * h, s2 = p2.xy / p2.w * h;
		float e = max(length(s0 - s1), max(length(s1 - s2), length(s2 - s0)));
		gap = dmax > dmin * edge.x && e > size.z * edge.y;
	}
	V o;
	o.pos = j == 0 ? p0 : (j == 1 ? p1 : p2);
	o.uv = j == 0 ? u0 : (j == 1 ? u1 : u2);
	o.gap = gap ? 1 : 0;
	return o;
}
float4 ps(V i) : SV_Target {
	if (i.gap > 0.5 && edge.z > 0) discard;
	return col.SampleLevel(smp, i.uv, 0);
}
)";

ID3D11VertexShader* s_warpVs;
ID3D11PixelShader* s_warpPs;
ID3D11Buffer* s_warpConstants;
ID3D11SamplerState* s_linearSampler;
ID3D11DepthStencilState* s_nearestWinsDepth;
ID3D11RasterizerState* s_noCullRasterizer;
ID3D11BlendState* s_opaqueBlend;
bool s_gpuObjectsReady, s_gpuObjectsFailed;

struct EyeState {
	ID3D11Texture2D* sceneCopy;
	ID3D11ShaderResourceView* sceneCopySrv;
	ID3D11Texture2D* depthCopy;
	ID3D11ShaderResourceView* depthCopySrv;
	uint32_t depthPresentCount;  // s_presentCount when the depth was copied
	bool sceneValid, depthValid;
};
EyeState s_eyes[2];
ID3D11Texture2D* s_warpedImage;
ID3D11RenderTargetView* s_warpedImageRtv;
ID3D11Texture2D* s_warpedDepth;
ID3D11DepthStencilView* s_warpedDepthDsv;
D3D11_TEXTURE2D_DESC s_sceneCopyDesc, s_depthCopyDesc;
ID3D11Resource* s_sceneDepth;  // raw: the game's full-size depth buffer, as last bound
UINT s_backbufferW, s_backbufferH;
uint32_t s_presentCount;
uint32_t s_warpedCount, s_skippedCount;
// why frames weren't corrected (logged with the 30 s report)
enum SkipReason { SKIP_NO_IMAGE,
				  SKIP_NO_DEPTH,
				  SKIP_OLD_DEPTH,
				  SKIP_SIZE_MISMATCH,
				  SKIP_NO_CAMERA,
				  SKIP_REASON_COUNT };
uint32_t s_skipCounts[SKIP_REASON_COUNT];
const char* const kSkipReasonText[SKIP_REASON_COUNT] = {"no image of the other eye", "no depth of the other eye",
														"the other eye's depth isn't from the previous frame", "image and depth sizes differ",
														"the camera wasn't found in one of the two frames"};
DWORD s_lastReportTick;
bool s_loggedWorking;

void ReleaseTargets() {
	for (auto& e : s_eyes) {
		SafeRelease(e.sceneCopySrv);
		SafeRelease(e.sceneCopy);
		SafeRelease(e.depthCopySrv);
		SafeRelease(e.depthCopy);
		e.sceneValid = e.depthValid = false;
	}
	SafeRelease(s_warpedImageRtv);
	SafeRelease(s_warpedImage);
	SafeRelease(s_warpedDepthDsv);
	SafeRelease(s_warpedDepth);
	s_sceneCopyDesc = {};
	s_depthCopyDesc = {};
}

bool CreateGpuObjects(ID3D11Device* dev) {
	if (s_gpuObjectsReady || s_gpuObjectsFailed) return s_gpuObjectsReady;
	HMODULE comp = LoadLibraryW(L"d3dcompiler_47.dll");
	pD3DCompile compile = comp ? (pD3DCompile)(void*)GetProcAddress(comp, "D3DCompile") : nullptr;
	ID3DBlob *vsBlob = nullptr, *psBlob = nullptr, *err = nullptr;
	HRESULT hr = compile ? compile(kHlsl, sizeof(kHlsl) - 1, "Reproject", nullptr, nullptr, "vs", "vs_5_0",
								   D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsBlob, &err)
						 : E_FAIL;
	if (SUCCEEDED(hr)) hr = compile(kHlsl, sizeof(kHlsl) - 1, "Reproject", nullptr, nullptr, "ps", "ps_5_0",
									D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psBlob, &err);
	if (SUCCEEDED(hr)) hr = dev->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &s_warpVs);
	if (SUCCEEDED(hr)) hr = dev->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &s_warpPs);
	if (FAILED(hr))
		Log("ERROR: depth reprojection shader: 0x%08x %s", (unsigned)hr, err ? (const char*)err->GetBufferPointer() : "");
	if (vsBlob) vsBlob->Release();
	if (psBlob) psBlob->Release();
	if (err) err->Release();
	D3D11_BUFFER_DESC bd = {};
	bd.ByteWidth = kWarpConstantsSize;
	bd.Usage = D3D11_USAGE_DEFAULT;
	bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	if (SUCCEEDED(hr)) hr = dev->CreateBuffer(&bd, nullptr, &s_warpConstants);
	D3D11_SAMPLER_DESC sd = {};
	sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.MaxLOD = D3D11_FLOAT32_MAX;
	if (SUCCEEDED(hr)) hr = dev->CreateSamplerState(&sd, &s_linearSampler);
	D3D11_DEPTH_STENCIL_DESC dd = {};
	dd.DepthEnable = TRUE;
	dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
	dd.DepthFunc = D3D11_COMPARISON_LESS;
	if (SUCCEEDED(hr)) hr = dev->CreateDepthStencilState(&dd, &s_nearestWinsDepth);
	D3D11_RASTERIZER_DESC rd = {};
	rd.FillMode = D3D11_FILL_SOLID;
	rd.CullMode = D3D11_CULL_NONE;
	rd.DepthClipEnable = TRUE;
	if (SUCCEEDED(hr)) hr = dev->CreateRasterizerState(&rd, &s_noCullRasterizer);
	D3D11_BLEND_DESC bl = {};
	bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	if (SUCCEEDED(hr)) hr = dev->CreateBlendState(&bl, &s_opaqueBlend);
	s_gpuObjectsReady = SUCCEEDED(hr);
	s_gpuObjectsFailed = !s_gpuObjectsReady;
	if (s_gpuObjectsFailed) Log("depth reprojection: unavailable (0x%08x); alternating mode works as before", (unsigned)hr);
	return s_gpuObjectsReady;
}

// Readable twin of the game's depth format, and the view format to sample its depth.
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
DXGI_FORMAT TypedFormat(DXGI_FORMAT f) {
	switch (f) {
		case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
		case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
		case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
		case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
		default: return f;
	}
}

bool EnsureDepth(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC& src) {
	if (s_eyes[0].depthCopy && s_depthCopyDesc.Width == src.Width && s_depthCopyDesc.Height == src.Height &&
		s_depthCopyDesc.Format == src.Format)
		return true;
	DXGI_FORMAT texFormat, srvFormat;
	if (!DepthFormats(src.Format, &texFormat, &srvFormat) || src.SampleDesc.Count != 1) return false;
	for (auto& e : s_eyes) {
		SafeRelease(e.depthCopySrv);
		SafeRelease(e.depthCopy);
		e.depthValid = false;
		D3D11_TEXTURE2D_DESC d = {};
		d.Width = src.Width;
		d.Height = src.Height;
		d.MipLevels = d.ArraySize = 1;
		d.Format = texFormat;
		d.SampleDesc.Count = 1;
		d.Usage = D3D11_USAGE_DEFAULT;
		d.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
		D3D11_SHADER_RESOURCE_VIEW_DESC v = {};
		v.Format = srvFormat;
		v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		v.Texture2D.MipLevels = 1;
		if (FAILED(dev->CreateTexture2D(&d, nullptr, &e.depthCopy)) || FAILED(dev->CreateShaderResourceView(e.depthCopy, &v, &e.depthCopySrv))) {
			Log("depth reprojection: could not create a readable depth copy (format %d)", (int)src.Format);
			return false;
		}
	}
	s_depthCopyDesc = src;
	return true;
}

// The depth covers the same picture as the colour image (same size, or the colour enlarged).
bool SameShape(const D3D11_TEXTURE2D_DESC& depth, const D3D11_TEXTURE2D_DESC& color) {
	if (depth.Width > color.Width || depth.Height > color.Height || !depth.Height || !color.Height) return false;
	float depthAspect = (float)depth.Width / depth.Height, colorAspect = (float)color.Width / color.Height;
	return depthAspect > colorAspect * 0.99f && depthAspect < colorAspect * 1.01f;
}

bool EnsureColor(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC& src) {
	if (s_warpedImage && s_sceneCopyDesc.Width == src.Width && s_sceneCopyDesc.Height == src.Height && s_sceneCopyDesc.Format == src.Format)
		return true;
	for (auto& e : s_eyes) {
		SafeRelease(e.sceneCopySrv);
		SafeRelease(e.sceneCopy);
		e.sceneValid = false;
	}
	SafeRelease(s_warpedImageRtv);
	SafeRelease(s_warpedImage);
	SafeRelease(s_warpedDepthDsv);
	SafeRelease(s_warpedDepth);
	D3D11_TEXTURE2D_DESC d = {};
	d.Width = src.Width;
	d.Height = src.Height;
	d.MipLevels = d.ArraySize = 1;
	d.Format = src.Format;
	d.SampleDesc.Count = 1;
	d.Usage = D3D11_USAGE_DEFAULT;
	d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	D3D11_SHADER_RESOURCE_VIEW_DESC v = {};
	v.Format = TypedFormat(src.Format);
	v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	v.Texture2D.MipLevels = 1;
	bool ok = true;
	for (auto& e : s_eyes)
		ok = ok && SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &e.sceneCopy)) &&
			 SUCCEEDED(dev->CreateShaderResourceView(e.sceneCopy, &v, &e.sceneCopySrv));
	d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	D3D11_RENDER_TARGET_VIEW_DESC rv = {};
	rv.Format = TypedFormat(src.Format);
	rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
	ok = ok && SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &s_warpedImage)) && SUCCEEDED(dev->CreateRenderTargetView(s_warpedImage, &rv, &s_warpedImageRtv));
	D3D11_TEXTURE2D_DESC dd = d;
	dd.Format = DXGI_FORMAT_D32_FLOAT;
	dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
	ok = ok && SUCCEEDED(dev->CreateTexture2D(&dd, nullptr, &s_warpedDepth)) &&
		 SUCCEEDED(dev->CreateDepthStencilView(s_warpedDepth, nullptr, &s_warpedDepthDsv));
	if (!ok) {
		Log("depth reprojection: could not create its %ux%u textures", src.Width, src.Height);
		ReleaseTargets();
		return false;
	}
	s_sceneCopyDesc = src;
	return true;
}
}  // namespace

// From the OMSetRenderTargets hook (game thread): remember the game's full-size depth buffer (also
// for the upscaler, upscale.cpp).
void ReprojectNoteDsv(ID3D11DepthStencilView* dsv) {
	if (!((g_cfg.depthReproject && !StereoSynchronized()) || UpscaleWantsDepth()) || !dsv || !s_backbufferW) return;
	ComPtr<ID3D11Resource> r;
	dsv->GetResource(&r);
	if (!r) return;
	D3D11_RESOURCE_DIMENSION dim;
	r->GetType(&dim);
	if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
		D3D11_TEXTURE2D_DESC d;
		((ID3D11Texture2D*)r.Get())->GetDesc(&d);
		if (d.Width == s_backbufferW && d.Height == s_backbufferH && d.SampleDesc.Count == 1) s_sceneDepth = r.Get();
	}
}

// At the first UI draw: the scene is finished; copy its depth for next frame's reprojection.
void ReprojectAtUiStart(ID3D11DeviceContext* c) {
	int eye = StereoFrameEye();
	if (!g_cfg.depthReproject || StereoSynchronized() || eye < 0 || !s_sceneDepth) return;
	ComPtr<ID3D11Device> dev;
	c->GetDevice(&dev);
	if (!dev) return;
	D3D11_TEXTURE2D_DESC d;
	((ID3D11Texture2D*)s_sceneDepth)->GetDesc(&d);
	if (EnsureDepth(dev.Get(), d)) {
		c->CopyResource(s_eyes[eye].depthCopy, s_sceneDepth);
		s_eyes[eye].depthValid = true;
		s_eyes[eye].depthPresentCount = s_presentCount;
	}
}

void ReprojectRelease() {
	s_sceneDepth = nullptr;
	ReleaseTargets();
}

ID3D11Resource* ReprojectSceneDepth() { return s_sceneDepth; }
void ReprojectSetBackbufferSize(UINT w, UINT h) {
	s_backbufferW = w;
	s_backbufferH = h;
}

// At Present (alternating), after this frame's eye was pushed to the headset: keep this eye's
// scene for next frame and return the other eye's previous image moved to this frame's camera.
// `scene` may be larger than the depth (upscaled, upscale.cpp): the depth is then read scaled.
ID3D11Texture2D* ReprojectOtherEye(ID3D11DeviceContext* c, ID3D11Texture2D* scene, ID3D11Texture2D* bb, FrameTag* tag) {
	// s_presentCount = Presents finished so far: the frame being presented copied its depth with
	// this value, so the other eye's depth from the frame before has s_presentCount - 1. It is only
	// counted up on return, so every check below sees the same value.
	struct CountPresentOnReturn {
		~CountPresentOnReturn() { s_presentCount++; }
	} countPresent;
	{
		D3D11_TEXTURE2D_DESC b;
		bb->GetDesc(&b);
		s_backbufferW = b.Width;
		s_backbufferH = b.Height;
	}
	int eye = StereoFrameEye();
	if (!g_cfg.depthReproject || StereoSynchronized() || eye < 0) {
		s_eyes[0].sceneValid = s_eyes[1].sceneValid = false;
		return nullptr;
	}
	ComPtr<ID3D11Device> dev;
	c->GetDevice(&dev);
	if (!dev) return nullptr;
	if (!CreateGpuObjects(dev.Get())) return nullptr;
	ID3D11Texture2D* src = scene ? scene : bb;
	D3D11_TEXTURE2D_DESC sceneDesc;
	src->GetDesc(&sceneDesc);
	if (!EnsureColor(dev.Get(), sceneDesc)) return nullptr;
	int other = 1 - eye;
	// alternating: the other eye's previous picture (AFW's mode 2); AFW: combined (3) or the drawn eye only (1)
	int mode = !StereoAfw() ? 2 : (g_cfg.afwMode == 1 ? 1 : 3);
	ID3D11Texture2D* result = nullptr;
	EyeState& otherEye = s_eyes[other];
	EyeState& thisEye = s_eyes[eye];
	// AFW's this-frame warp samples this frame's picture: keep it first (it is also next frame's previous picture)
	c->CopyResource(thisEye.sceneCopy, src);
	thisEye.sceneValid = true;

	// previous-frame warp (modes 2 and 3): the other eye's last picture moved to this frame's camera
	float prevT[16], prevProj[4], prevLens[2];
	FrameTag prevTag;
	int skip = mode == 1                                                            ? -1
			   : !otherEye.sceneValid                                               ? SKIP_NO_IMAGE
			   : !otherEye.depthValid                                               ? SKIP_NO_DEPTH
			   : otherEye.depthPresentCount + 1 != s_presentCount                   ? SKIP_OLD_DEPTH
			   : !SameShape(s_depthCopyDesc, sceneDesc)                             ? SKIP_SIZE_MISMATCH
			   : !StereoReprojectParams(other, prevT, prevProj, prevLens, &prevTag) ? SKIP_NO_CAMERA
																					: -1;
	bool prevReady = mode != 1 && skip < 0;
	if (skip >= 0) s_skipCounts[skip]++;
	// this-frame warp (modes 1 and 3): THIS frame's eye moved into the other eye's view by its own depth
	float thisT[16], thisProj[4], thisLens[2], dstProj[4], dstLens[2];
	FrameTag thisTag;
	bool thisReady = mode != 2 && thisEye.depthValid && thisEye.depthPresentCount == s_presentCount && SameShape(s_depthCopyDesc, sceneDesc) &&
					 StereoAfwParams(eye, thisT, thisProj, thisLens, dstProj, dstLens, &thisTag);

	if (prevReady || thisReady) {
		struct {
			float T[16];
			float proj[4];
			float size[4];
			float dscale[4];
			float lens[4];
			float projDst[4];
			float lensDst[4];
			float edge[4];
		} constants = {};
		static_assert(sizeof(constants) == kWarpConstantsSize, "cbuffer P");
		const float gridStep = 4;
		UINT gridW = (sceneDesc.Width + 3) / 4, gridH = (sceneDesc.Height + 3) / 4;
		constants.size[0] = (float)sceneDesc.Width;
		constants.size[1] = (float)sceneDesc.Height;
		constants.size[2] = gridStep;
		constants.size[3] = (float)gridW;
		constants.dscale[0] = (float)s_depthCopyDesc.Width / (float)sceneDesc.Width;
		constants.dscale[1] = (float)s_depthCopyDesc.Height / (float)sceneDesc.Height;
		g_replaying = true;  // our own calls: the hooks pass them straight through
		StateSaveTemp(c);
		const FLOAT black[4] = {0, 0, 0, 1};
		c->ClearRenderTargetView(s_warpedImageRtv, black);
		c->ClearDepthStencilView(s_warpedDepthDsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
		c->ClearState();
		c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		c->VSSetShader(s_warpVs, nullptr, 0);
		c->PSSetShader(s_warpPs, nullptr, 0);
		c->VSSetConstantBuffers(0, 1, &s_warpConstants);
		c->PSSetConstantBuffers(0, 1, &s_warpConstants);
		c->PSSetSamplers(0, 1, &s_linearSampler);
		D3D11_VIEWPORT vp = {0, 0, (float)sceneDesc.Width, (float)sceneDesc.Height, 0, 1};
		c->RSSetViewports(1, &vp);
		c->RSSetState(s_noCullRasterizer);
		c->OMSetDepthStencilState(s_nearestWinsDepth, 0);
		c->OMSetBlendState(s_opaqueBlend, nullptr, 0xffffffff);
		c->OMSetRenderTargets(1, &s_warpedImageRtv, s_warpedDepthDsv);
		auto draw = [&](ID3D11ShaderResourceView* depth, ID3D11ShaderResourceView* colour, const float* T, const float* proj,
						const float* lens, const float* projDst, const float* lensDst, bool dropGaps) {
			memcpy(constants.T, T, 64);
			memcpy(constants.proj, proj, 16);
			memcpy(constants.projDst, projDst, 16);
			constants.lens[0] = lens[0], constants.lens[1] = lens[1];
			constants.lensDst[0] = lensDst[0], constants.lensDst[1] = lensDst[1];
			constants.edge[0] = dropGaps ? 1.03f : 0.f;  // 0.4.1's thresholds
			constants.edge[1] = 2.5f;
			constants.edge[2] = dropGaps ? 1.f : 0.f;
			c->UpdateSubresource(s_warpConstants, 0, nullptr, &constants, 0, 0);
			ID3D11ShaderResourceView* srvs[2] = {depth, colour};
			c->VSSetShaderResources(0, 2, srvs);
			c->PSSetShaderResources(0, 2, srvs);
			c->Draw(gridW * gridH * 6, 0);
		};
		if (prevReady) draw(otherEye.depthCopySrv, otherEye.sceneCopySrv, prevT, prevProj, prevLens, prevProj, prevLens, false);
		if (thisReady) {
			// combined: this frame's eye wins wherever it has real surface; its gaps keep the previous-frame picture
			if (prevReady) c->ClearDepthStencilView(s_warpedDepthDsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
			draw(thisEye.depthCopySrv, thisEye.sceneCopySrv, thisT, thisProj, thisLens, dstProj, dstLens, prevReady);
		}
		StateRestoreTemp(c);
		g_replaying = false;
		result = s_warpedImage;
		*tag = thisReady ? thisTag : prevTag;
		s_warpedCount++;
		if (!s_loggedWorking) {
			s_loggedWorking = true;
			Log("depth reprojection: working (%ux%u, depth format %d, grid every %.0f pixels), %s", sceneDesc.Width, sceneDesc.Height,
				(int)s_depthCopyDesc.Format, (double)gridStep,
				mode == 1   ? "AFW this-frame: the drawn eye is moved into the other eye's view"
				: mode == 3 ? "AFW combined: the drawn eye moved into the other eye, gaps from its previous picture"
							: "the older eye follows the camera");
		}
	} else {
		s_skippedCount++;
	}
	if (GetTickCount() - s_lastReportTick > 30000) {
		s_lastReportTick = GetTickCount();
		if (s_warpedCount || s_skippedCount) {
			char reasonList[300];
			size_t at = 0;
			reasonList[0] = 0;
			for (int i = 0; i < SKIP_REASON_COUNT; i++)
				if (s_skipCounts[i]) at += snprintf(reasonList + at, sizeof(reasonList) - at, "%s%u x %s", at ? ", " : "", s_skipCounts[i], kSkipReasonText[i]);
			Log("depth reprojection: %u images moved to the new camera, %u frames without (30 s)%s%s", s_warpedCount, s_skippedCount,
				at ? ": " : "", reasonList);
		}
		memset(s_skipCounts, 0, sizeof(s_skipCounts));
		char info[160];
		snprintf(info, sizeof(info), "Depth reprojection: %u of %u frames in the last 30 s", s_warpedCount, s_warpedCount + s_skippedCount);
		MenuSetInfo(INFO_REPROJECT, info);
		s_warpedCount = s_skippedCount = 0;
	}
	return result;
}
