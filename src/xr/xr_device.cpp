// xr_device.cpp - the VR thread's own D3D11 device (on the game's GPU) and its blit shaders, the
// game's frames copied onto it, and menu detection.
#include "xr/xr_thread.h"
#include "core/com.h"
#include "core/locks.h"
#include "core/config.h"
#include "core/log.h"
#include "debug/frame_capture.h"
#include "render/camera.h"
#include <atomic>
#include <stdlib.h>
#include <string.h>
#include <d3dcompiler.h>

namespace {

constexpr char kShader[] = R"(
cbuffer Params : register(b0) { float4 params; };   // params.x: 0 = copy, 1 = sRGB->linear, 2 = linear->sRGB
Texture2D src : register(t0);
Texture2D src2 : register(t1);
SamplerState samp : register(s0);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut VS(uint id : SV_VertexID) {
	VSOut o;
	float2 uv = float2((id << 1) & 2, id & 2);
	o.uv = uv;
	o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
	return o;
}
float3 ToLinear(float3 c) { return lerp(pow((c + 0.055) / 1.055, 2.4), c / 12.92, step(c, 0.04045)); }
float3 ToSrgb(float3 c) { c = saturate(c); return lerp(1.055 * pow(c, 1.0 / 2.4) - 0.055, c * 12.92, step(c, 0.0031308)); }
float3 Convert(float3 c) {
	if (params.x > 1.5) return ToSrgb(c);
	if (params.x > 0.5) return ToLinear(c);
	return c;
}
float4 PS(VSOut i) : SV_Target {
	return float4(Convert(src.Sample(samp, i.uv).rgb), 1);
}
// Late warp: the eye image re-aimed to the headset's pose for this frame, covering exactly
// the eye's own field of view. Output pixel -> direction in the shown eye (outTan: tangents of left,
// right, up, down) -> rotated into the eye the image was rendered from (m0..m2 = R_render^T R_shown)
// -> the frustum it was rendered with (srcTan: tangent half-width, half-height, and where that lens
// points when AsymmetricEyes is on). Rotation is exact; the small
// eye translation between the two poses is ignored, as a compositor's rotational reprojection does.
cbuffer Warp : register(b1) { float4 outTan; float4 srcTan; float4 m0; float4 m1; float4 m2; };
float4 PSWarp(VSOut i) : SV_Target {
	float3 d = float3(lerp(outTan.x, outTan.y, i.uv.x), lerp(outTan.z, outTan.w, i.uv.y), -1);
	float3 r = float3(dot(m0.xyz, d), dot(m1.xyz, d), dot(m2.xyz, d));
	if (r.z > -1e-4) return float4(0, 0, 0, 1);
	float2 t = r.xy / -r.z;
	float2 uv = float2((t.x - srcTan.z) / srcTan.x * 0.5 + 0.5, 0.5 - (t.y - srcTan.w) / srcTan.y * 0.5);
	if (any(uv < 0) || any(uv > 1)) return float4(0, 0, 0, 1);
	return float4(Convert(src.SampleLevel(samp, uv, 0).rgb), 1);
}
// The flat screen (menus, loading screens, the title) as a window hanging in the room, drawn into
// both eyes of a projection layer instead of handed over as a quad layer. A quad layer is composited
// by the VR software from one picture for both eyes, which smears and shears it while the head turns;
// drawn here, each eye sees the window through its own lens, from where that eye really is, so it
// stays still and square-on. Output pixel -> direction in the shown eye -> the screen's own space
// (m0..m2: rows of R_screen^T R_eye, and in .w where that eye is in the screen's space) -> the point
// where the ray crosses the screen's plane (half: its half width and height in metres).
float4 PSScreen(VSOut i) : SV_Target {
	float3 d = float3(lerp(outTan.x, outTan.y, i.uv.x), lerp(outTan.z, outTan.w, i.uv.y), -1);
	float3 dir = float3(dot(m0.xyz, d), dot(m1.xyz, d), dot(m2.xyz, d));
	float3 eye = float3(m0.w, m1.w, m2.w);
	if (dir.z > -1e-6 || eye.z < 1e-4) return float4(0, 0, 0, 1);  // behind the screen, or facing away
	float2 hit = eye.xy + (-eye.z / dir.z) * dir.xy;
	float2 uv = float2(hit.x / srcTan.x * 0.5 + 0.5, 0.5 - hit.y / srcTan.y * 0.5);
	if (any(uv < 0) || any(uv > 1)) return float4(0, 0, 0, 1);
	if (srcTan.w > srcTan.z) uv.y = lerp(srcTan.z, srcTan.w, uv.y);  // only the picture's rows (black bands cut off)
	return float4(Convert(src.SampleLevel(samp, uv, 0).rgb), 1);
}
// HUD/menus. src = full frame, src2 = scene without UI, whose alpha channel holds the UI's real
// coverage (accumulated on the game side from the UI draws' own alpha). Since
// full = ui + (1 - a) * scene, the HUD layer is ui = full - (1 - a) * scene, premultiplied:
// translucent panels darken the 3D scene instead of carrying a flat copy of it.
// params.y/z: where nothing was drawn (a = 0), differences below y are noise, above z kept
// (additive glows).
float4 PSKey(VSOut i) : SV_Target {
	float3 f = src.Sample(samp, i.uv).rgb;
	float4 s = src2.Sample(samp, i.uv);
	float a = saturate(s.a);
	float3 d = abs(f - s.rgb);
	float3 pm = max(f - (1 - a) * s.rgb, 0);
	pm *= max(step(0.004, a), smoothstep(params.y, params.z, max(d.r, max(d.g, d.b))));
	float3 c = a > 0.01 ? Convert(saturate(pm / a)) * a : Convert(pm);
	return float4(c, a);
}
)";

bool CompileShaders() {
	HMODULE compilerDll = LoadLibraryW(L"d3dcompiler_47.dll");
	pD3DCompile compile = compilerDll ? (pD3DCompile)(void*)GetProcAddress(compilerDll, "D3DCompile") : nullptr;
	if (!compile) {
		Log("ERROR: d3dcompiler_47.dll not available (error %u)", (unsigned)GetLastError());
		return false;
	}
	ID3DBlob *vsBlob = nullptr, *psBlob = nullptr, *errors = nullptr;
	HRESULT hr = compile(kShader, sizeof(kShader) - 1, "niervr_blit", nullptr, nullptr, "VS", "vs_4_0",
						 D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsBlob, &errors);
	if (FAILED(hr)) {
		Log("ERROR: VS compile 0x%08x: %s", (unsigned)hr, errors ? (const char*)errors->GetBufferPointer() : "");
		SafeRelease(errors);
		return false;
	}
	hr = compile(kShader, sizeof(kShader) - 1, "niervr_blit", nullptr, nullptr, "PS", "ps_4_0",
				 D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psBlob, &errors);
	if (FAILED(hr)) {
		Log("ERROR: PS compile 0x%08x: %s", (unsigned)hr, errors ? (const char*)errors->GetBufferPointer() : "");
		SafeRelease(errors);
		SafeRelease(vsBlob);
		return false;
	}
	hr = g_vr.device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &g_vr.blitVs);
	if (SUCCEEDED(hr)) hr = g_vr.device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &g_vr.blitPs);
	SafeRelease(vsBlob);
	SafeRelease(psBlob);
	if (SUCCEEDED(hr)) {
		hr = compile(kShader, sizeof(kShader) - 1, "niervr_key", nullptr, nullptr, "PSKey", "ps_4_0",
					 D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psBlob, &errors);
		if (FAILED(hr)) Log("ERROR: PSKey compile 0x%08x: %s", (unsigned)hr, errors ? (const char*)errors->GetBufferPointer() : "");
		SafeRelease(errors);
		if (SUCCEEDED(hr)) hr = g_vr.device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &g_vr.hudKeyPs);
		SafeRelease(psBlob);
	}
	if (SUCCEEDED(hr)) {  // optional: without it the eyes are submitted as rendered (LateWarp off)
		HRESULT warpResult = compile(kShader, sizeof(kShader) - 1, "niervr_warp", nullptr, nullptr, "PSWarp", "ps_4_0",
									 D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psBlob, &errors);
		if (FAILED(warpResult)) Log("late warp: shader compile 0x%08x: %s", (unsigned)warpResult, errors ? (const char*)errors->GetBufferPointer() : "");
		SafeRelease(errors);
		if (SUCCEEDED(warpResult)) g_vr.device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &g_vr.lateWarpPs);
		SafeRelease(psBlob);
		HRESULT screenResult = compile(kShader, sizeof(kShader) - 1, "niervr_screen", nullptr, nullptr, "PSScreen", "ps_4_0",
									   D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psBlob, &errors);
		if (FAILED(screenResult)) Log("flat screen: shader compile 0x%08x: %s", (unsigned)screenResult, errors ? (const char*)errors->GetBufferPointer() : "");
		SafeRelease(errors);
		if (SUCCEEDED(screenResult)) g_vr.device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &g_vr.screenPs);
		SafeRelease(psBlob);
		D3D11_BUFFER_DESC warpDesc = {};
		warpDesc.ByteWidth = 80;
		warpDesc.Usage = D3D11_USAGE_DEFAULT;
		warpDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		if ((g_vr.lateWarpPs || g_vr.screenPs) && FAILED(g_vr.device->CreateBuffer(&warpDesc, nullptr, &g_vr.lateWarpConstants))) {
			SafeRelease(g_vr.lateWarpPs);
			SafeRelease(g_vr.screenPs);
		}
	}
	if (FAILED(hr)) {
		Log("ERROR: CreateShader 0x%08x", (unsigned)hr);
		return false;
	}
	D3D11_SAMPLER_DESC samplerDesc = {};
	samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
	hr = g_vr.device->CreateSamplerState(&samplerDesc, &g_vr.linearSampler);
	D3D11_BUFFER_DESC constantsDesc = {};
	constantsDesc.ByteWidth = 16;
	constantsDesc.Usage = D3D11_USAGE_DEFAULT;
	constantsDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	if (SUCCEEDED(hr)) hr = g_vr.device->CreateBuffer(&constantsDesc, nullptr, &g_vr.blitConstants);
	if (FAILED(hr)) Log("ERROR: sampler/cbuffer 0x%08x", (unsigned)hr);
	return SUCCEEDED(hr);
}
}  // namespace

// Created on the game's GPU as soon as the VR thread starts, before OpenXR is involved, because it
// also owns the texture the game copies its frames into.
bool InitDevice() {
	// Use the system d3d11.dll directly so a chain-loaded mod never wraps our private device.
	wchar_t path[MAX_PATH];
	UINT length = GetSystemDirectoryW(path, MAX_PATH - 16);
	lstrcpyW(path + length, L"\\d3d11.dll");
	HMODULE d3d11 = LoadLibraryW(path);
	PFN_D3D11_CREATE_DEVICE createDevice =
		d3d11 ? (PFN_D3D11_CREATE_DEVICE)(void*)GetProcAddress(d3d11, "D3D11CreateDevice") : nullptr;
	HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
	typedef HRESULT(WINAPI * PFN_CreateDXGIFactory1)(REFIID, void**);
	PFN_CreateDXGIFactory1 createFactory =
		dxgi ? (PFN_CreateDXGIFactory1)(void*)GetProcAddress(dxgi, "CreateDXGIFactory1") : nullptr;
	if (!createDevice || !createFactory) {
		Log("ERROR: cannot load D3D11/DXGI entry points");
		return false;
	}
	ComPtr<IDXGIFactory1> factory;
	if (FAILED(createFactory(__uuidof(IDXGIFactory1), &factory))) {
		Log("ERROR: CreateDXGIFactory1 failed");
		return false;
	}
	ComPtr<IDXGIAdapter1> adapter;
	for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
		DXGI_ADAPTER_DESC1 desc;
		bool match = SUCCEEDED(adapter->GetDesc1(&desc)) &&
					 (!g_share.gameLuidValid || (desc.AdapterLuid.LowPart == g_share.gameLuid.LowPart &&
												 desc.AdapterLuid.HighPart == g_share.gameLuid.HighPart));
		if (match) {
			Log("VR device on GPU: %s", Utf8(desc.Description).s);
			g_vr.deviceLuid = desc.AdapterLuid;
			break;
		}
		adapter.Reset();
	}
	if (!adapter) {
		Log("ERROR: could not find the game's GPU for the VR device");
		return false;
	}
	const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
										D3D_FEATURE_LEVEL_10_0};
	D3D_FEATURE_LEVEL featureLevel;
	HRESULT hr = createDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 4,
							  D3D11_SDK_VERSION, &g_vr.device, &featureLevel, &g_vr.context);
	if (hr == E_INVALIDARG)  // Windows without 11.1 support
		hr = createDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels + 1, 3,
						  D3D11_SDK_VERSION, &g_vr.device, &featureLevel, &g_vr.context);
	if (FAILED(hr)) {
		Log("ERROR: VR D3D11CreateDevice failed 0x%08x", (unsigned)hr);
		return false;
	}
	g_vr.deviceFeatureLevel = featureLevel;
	Log("VR device created (feature level 0x%x)", (unsigned)featureLevel);
	// The VR thread's few small GPU jobs per headset frame must not wait behind the game's frame on
	// the same GPU, or the runtime sees late frames and throttles the app (a Pico headset dropped to
	// 27-32 fps in synchronized mode while the GPU needed only 15-19 ms per game frame).
	{
		ComPtr<IDXGIDevice> dxgiDevice;
		if (SUCCEEDED(g_vr.device->QueryInterface(__uuidof(IDXGIDevice), &dxgiDevice)) && dxgiDevice) {
			HRESULT priorityResult = E_FAIL;
			int priority = 7;
			for (; priority > 0 && FAILED(priorityResult = dxgiDevice->SetGPUThreadPriority(priority)); priority -= 3) {}
			Log("VR device GPU priority %d%s (0x%08x)", SUCCEEDED(priorityResult) ? priority : 0,
				SUCCEEDED(priorityResult) ? " (above the game)" : ": not allowed, normal", (unsigned)priorityResult);
		}
	}
	return CompileShaders();
}

namespace {
// Game frame -> our device
void ReleaseContent() {
	for (auto& texture : g_vr.ring) SafeRelease(texture);
	for (auto& texture : g_vr.ringScene) SafeRelease(texture);
	for (auto& texture : g_vr.ringKey) SafeRelease(texture);
	for (auto& view : g_vr.views) {
		SafeRelease(view.srv);
		SafeRelease(view.tex);
		SafeRelease(view.sceneSrv);
		SafeRelease(view.keySrv);
		SafeRelease(view.keyTex);
		SafeRelease(view.sceneTex);
		view.hasFrame = false;
		view.seq = 0;
	}
}

void TypelessFor(DXGI_FORMAT format, DXGI_FORMAT* texFormat, DXGI_FORMAT* viewFormat, bool* linear) {
	*linear = false;
	switch (format) {
		case DXGI_FORMAT_R8G8B8A8_UNORM:
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
			*texFormat = DXGI_FORMAT_R8G8B8A8_TYPELESS;
			*viewFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
			return;
		case DXGI_FORMAT_B8G8R8A8_UNORM:
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
			*texFormat = DXGI_FORMAT_B8G8R8A8_TYPELESS;
			*viewFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
			return;
		case DXGI_FORMAT_B8G8R8X8_UNORM:
		case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
			*texFormat = DXGI_FORMAT_B8G8R8X8_TYPELESS;
			*viewFormat = DXGI_FORMAT_B8G8R8X8_UNORM;
			return;
		case DXGI_FORMAT_R10G10B10A2_UNORM:
			*texFormat = DXGI_FORMAT_R10G10B10A2_TYPELESS;
			*viewFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
			return;
		case DXGI_FORMAT_R16G16B16A16_FLOAT:
			*texFormat = DXGI_FORMAT_R16G16B16A16_TYPELESS;
			*viewFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
			*linear = true;  // scRGB
			return;
		default:
			*texFormat = *viewFormat = format;
			return;
	}
}
}  // namespace

// Answers the game thread's request for capture textures: a ring of kRingSize created here, on our
// device, and shared with the game's device by handle. Called continuously from the VR loop.
void ServiceShare() {
	EnterCriticalSection(&g_share.lock);
	int32_t requestGen = g_share.requestGen;
	UINT gameW = g_share.requestW, gameH = g_share.requestH;
	UINT sceneW = g_share.requestSceneW ? g_share.requestSceneW : gameW, sceneH = g_share.requestSceneH ? g_share.requestSceneH : gameH;
	bool wantKey = g_share.requestKey;
	DXGI_FORMAT format = g_share.requestFormat;
	LeaveCriticalSection(&g_share.lock);
	if (requestGen == g_vr.servedRequestGen || gameW == 0 || gameH == 0) return;
	g_vr.servedRequestGen = requestGen;  // one attempt per request

	ReleaseContent();
	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = gameW;
	desc.Height = gameH;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = format;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
	// full frames, scene images, and (upscaling) the scene images before upscaling: the HUD is cut out
	// as full - scene, which only works with two images that match pixel for pixel where there's no HUD
	// (an upscaled one left faint outlines of everything on the HUD layer)
	const bool upscaling = wantKey || sceneW != gameW || sceneH != gameH;  // upscaling (DLAA keeps the size)
	HANDLE handles[3 * kRingSize] = {};
	for (int i = 0; i < (upscaling ? 3 : 2) * kRingSize; i++) {
		ID3D11Texture2D** texture = i < kRingSize ? &g_vr.ring[i] : i < 2 * kRingSize ? &g_vr.ringScene[i - kRingSize]
																					  : &g_vr.ringKey[i - 2 * kRingSize];
		bool isScene = i >= kRingSize && i < 2 * kRingSize;
		desc.Width = isScene ? sceneW : gameW;
		desc.Height = isScene ? sceneH : gameH;
		HRESULT hr = g_vr.device->CreateTexture2D(&desc, nullptr, texture);
		ComPtr<IDXGIResource> dxgiResource;
		if (SUCCEEDED(hr)) hr = (*texture)->QueryInterface(__uuidof(IDXGIResource), &dxgiResource);
		if (SUCCEEDED(hr)) {
			hr = dxgiResource->GetSharedHandle(&handles[i]);
		}
		if (FAILED(hr) || !handles[i]) {
			Log("ERROR: capture texture %ux%u format %d could not be created/shared (0x%08x)", desc.Width, desc.Height,
				(int)format, (unsigned)hr);
			ReleaseContent();
			return;
		}
	}

	// Local copies we sample from (left, right, mono), so the ring can be reused.
	DXGI_FORMAT texFormat, viewFormat;
	TypelessFor(format, &texFormat, &viewFormat, &g_vr.gameImageLinear);
	D3D11_TEXTURE2D_DESC copyDesc = desc;
	copyDesc.Width = gameW;
	copyDesc.Height = gameH;
	copyDesc.Format = texFormat;
	copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	copyDesc.MiscFlags = 0;
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Format = viewFormat;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MipLevels = 1;
	D3D11_TEXTURE2D_DESC sceneCopyDesc = copyDesc;
	sceneCopyDesc.Width = sceneW;
	sceneCopyDesc.Height = sceneH;
	for (auto& view : g_vr.views) {
		HRESULT hr = g_vr.device->CreateTexture2D(&copyDesc, nullptr, &view.tex);
		if (SUCCEEDED(hr)) hr = g_vr.device->CreateShaderResourceView(view.tex, &srvDesc, &view.srv);
		if (SUCCEEDED(hr)) hr = g_vr.device->CreateTexture2D(&sceneCopyDesc, nullptr, &view.sceneTex);
		if (SUCCEEDED(hr)) hr = g_vr.device->CreateShaderResourceView(view.sceneTex, &srvDesc, &view.sceneSrv);
		if (SUCCEEDED(hr) && upscaling) hr = g_vr.device->CreateTexture2D(&copyDesc, nullptr, &view.keyTex);
		if (SUCCEEDED(hr) && upscaling) hr = g_vr.device->CreateShaderResourceView(view.keyTex, &srvDesc, &view.keySrv);
		if (FAILED(hr)) {
			Log("ERROR: VR-side copy texture %ux%u fmt %d failed 0x%08x", gameW, gameH, (int)texFormat, (unsigned)hr);
			ReleaseContent();
			return;
		}
	}
	g_vr.gameW = gameW;
	g_vr.gameH = gameH;
	g_vr.sceneW = sceneW;
	g_vr.sceneH = sceneH;

	{
		SectionLock guard(&g_share.lock);
		memcpy(g_share.ring, handles, sizeof(g_share.ring));
		memcpy(g_share.ringScene, handles + kRingSize, sizeof(g_share.ringScene));
		memcpy(g_share.ringKey, handles + 2 * kRingSize, sizeof(g_share.ringKey));  // null when not upscaling
		g_share.forRequestGen = requestGen;
		g_share.ringGen = ++g_vr.publishedGen;
		for (auto& published : g_share.latest) published.seq = 0;
	}
	Log("capture ring published: %d x %ux%u format %d%s, scene images %ux%u", kRingSize, gameW, gameH, (int)format,
		g_vr.gameImageLinear ? " (linear/HDR)" : "", sceneW, sceneH);
}

namespace {
void SetMenuMode(bool on, float coverage);
}  // namespace

// The scene image the HUD is cut out against: the one before upscaling when there is one.
ID3D11ShaderResourceView* KeySrv(const GameView& view) {
	return view.keySrv ? view.keySrv : view.sceneSrv;
}

// Copies newly finished game frames (left / right / mono) into our local textures.
void UpdateContent() {
	if (!g_vr.ring[0]) return;
	FrameShare::Published latest[3];
	EnterCriticalSection(&g_share.lock);
	bool current = g_share.ringGen == g_vr.publishedGen;
	memcpy(latest, g_share.latest, sizeof(latest));
	LeaveCriticalSection(&g_share.lock);
	if (!current) return;
	for (int kind = 0; kind < 3; kind++) {
		if (!latest[kind].seq || latest[kind].seq == g_vr.views[kind].seq || latest[kind].slot < 0 || latest[kind].slot >= kRingSize) continue;
		g_vr.context->CopyResource(g_vr.views[kind].tex, g_vr.ring[latest[kind].slot]);
		if (latest[kind].tag.hasScene) g_vr.context->CopyResource(g_vr.views[kind].sceneTex, g_vr.ringScene[latest[kind].slot]);
		if (latest[kind].tag.hasScene && !latest[kind].tag.noUI && g_vr.views[kind].keyTex && g_vr.ringKey[latest[kind].slot])
			g_vr.context->CopyResource(g_vr.views[kind].keyTex, g_vr.ringKey[latest[kind].slot]);
		// Eye frames without a scene-only image (the UI split didn't work on this screen, e.g.
		// during some damage effects): stay in stereo with the HUD inside the 3D image. Switching to
		// the flat screen here made the view flicker to flat when hit.
		if (kind < 2) {
			if (latest[kind].tag.hasScene) {
				g_vr.framesWithoutScene = 0;
			} else if (++g_vr.framesWithoutScene == 30) {
				static std::atomic<int32_t> budget;
				if (LogBudget(budget, 3)) Log("ui split: no HUD-free image for a moment; HUD shown inside the 3D view");
			}
		}
		g_vr.statSceneFrames += latest[kind].tag.hasScene ? 1 : 0;
		g_vr.views[kind].seq = latest[kind].seq;
		g_vr.views[kind].tag = latest[kind].tag;
		g_vr.views[kind].hasFrame = true;
		g_vr.statGameFrames[kind]++;
		OverlayCountGameImage(kind);
	}
}

void Blit(ID3D11RenderTargetView* rtv, uint32_t width, uint32_t height, ID3D11ShaderResourceView* srv,
		  ID3D11ShaderResourceView* scene, bool dstSrgb, const WarpParams* warp, bool asScreen) {
	int mode = (!g_vr.gameImageLinear && dstSrgb) ? 1 : ((g_vr.gameImageLinear && !dstSrgb) ? 2 : 0);
	// key thresholds on the difference between full and scene-only frames (0..1 colour units)
	float params[4] = {(float)mode, 0.004f, 0.03f, 0};
	static float lastParams[4] = {-1, -1, -1, -1};
	if (memcmp(params, lastParams, sizeof(params)) != 0) {
		g_vr.context->UpdateSubresource(g_vr.blitConstants, 0, nullptr, params, 0, 0);
		memcpy(lastParams, params, sizeof(params));
	}
	D3D11_VIEWPORT viewport = {0, 0, (float)width, (float)height, 0, 1};
	g_vr.context->OMSetRenderTargets(1, &rtv, nullptr);
	g_vr.context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
	g_vr.context->OMSetDepthStencilState(nullptr, 0);
	g_vr.context->RSSetState(nullptr);
	g_vr.context->RSSetViewports(1, &viewport);
	g_vr.context->IASetInputLayout(nullptr);
	g_vr.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	g_vr.context->VSSetShader(g_vr.blitVs, nullptr, 0);
	if (warp) {
		static_assert(sizeof(WarpParams) == 80, "matches the Warp cbuffer");
		static_assert(sizeof(ScreenParams) == sizeof(WarpParams), "the screen shader shares it");
		g_vr.context->UpdateSubresource(g_vr.lateWarpConstants, 0, nullptr, warp, 0, 0);
		g_vr.context->PSSetConstantBuffers(1, 1, &g_vr.lateWarpConstants);
	}
	g_vr.context->PSSetShader(asScreen ? g_vr.screenPs : warp ? g_vr.lateWarpPs
															  : (scene ? g_vr.hudKeyPs : g_vr.blitPs),
							  nullptr, 0);
	ID3D11ShaderResourceView* srvs[2] = {srv, scene};
	g_vr.context->PSSetShaderResources(0, 2, srvs);
	g_vr.context->PSSetSamplers(0, 1, &g_vr.linearSampler);
	g_vr.context->PSSetConstantBuffers(0, 1, &g_vr.blitConstants);
	g_vr.context->Draw(3, 0);
	ID3D11ShaderResourceView* nulls[2] = {nullptr, nullptr};
	g_vr.context->PSSetShaderResources(0, 2, nulls);
	g_vr.context->OMSetRenderTargets(0, nullptr, nullptr);
}

namespace {

constexpr int32_t kCovW = 64, kCovH = 36;
// Only UI covering (nearly) the whole screen is a menu shown flat; popups and text boxes stay on the HUD layer over the 3D scene
constexpr float kMenuCoverage = 0.9f, kGameplayCoverage = 0.6f;

bool EnsureCoverageTargets() {
	if (g_vr.coverageTex) return true;
	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = kCovW;
	desc.Height = kCovH;
	desc.MipLevels = desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_RENDER_TARGET;
	HRESULT hr = g_vr.device->CreateTexture2D(&desc, nullptr, &g_vr.coverageTex);
	if (SUCCEEDED(hr)) hr = g_vr.device->CreateRenderTargetView(g_vr.coverageTex, nullptr, &g_vr.coverageRtv);
	desc.Usage = D3D11_USAGE_STAGING;
	desc.BindFlags = 0;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	for (int i = 0; i < 3 && SUCCEEDED(hr); i++) hr = g_vr.device->CreateTexture2D(&desc, nullptr, &g_vr.coverageStaging[i]);
	if (FAILED(hr)) {
		Log("ERROR: UI coverage targets failed 0x%08x", (unsigned)hr);
		SafeRelease(g_vr.coverageRtv);
		SafeRelease(g_vr.coverageTex);
		for (auto& texture : g_vr.coverageStaging) SafeRelease(texture);
		return false;
	}
	return true;
}

void SetMenuMode(bool on, float coverage) {
	if (on == g_vr.menuMode) return;
	g_vr.menuMode = on;
	{
		ExclusiveLock guard(&g_stereo.lock);
		g_stereo.menuMode = on;
	}
	if (coverage >= 0)
		Log("%s (UI covers %.0f%% of the screen)", on ? "menu detected: flat screen" : "menu closed: back to stereo",
			(double)(coverage * 100));
}
}  // namespace

void MeasureUICoverage() {
	if (!g_vr.hudKeyPs || !EnsureCoverageTargets()) return;
	for (int i = 0; i < 3; i++) {
		if (!g_vr.coveragePending[i]) continue;
		D3D11_MAPPED_SUBRESOURCE mapped;
		if (g_vr.context->Map(g_vr.coverageStaging[i], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped) != S_OK) continue;
		int covered = 0;
		for (int y = 0; y < kCovH; y++) {
			const uint8_t* row = (const uint8_t*)mapped.pData + y * mapped.RowPitch;
			for (int x = 0; x < kCovW; x++) covered += row[x * 4 + 3] > 127;
		}
		g_vr.context->Unmap(g_vr.coverageStaging[i], 0);
		g_vr.coveragePending[i] = false;
		float coverage = (float)covered / (kCovW * kCovH);
		g_vr.statCoverageSum += coverage;
		g_vr.statCoverageCount++;
		if (coverage > g_vr.statCoverageMax) g_vr.statCoverageMax = coverage;
		// Coverage is the UI's real opacity (> 50% per sample point).
		if (coverage > kMenuCoverage)
			g_vr.menuVotes = g_vr.menuVotes < 0 ? 1 : g_vr.menuVotes + 1;
		else if (coverage < kGameplayCoverage)
			g_vr.menuVotes = g_vr.menuVotes > 0 ? -1 : g_vr.menuVotes - 1;
		// Into the flat menu only after ~0.75 s of mostly-UI frames (full-screen damage flashes
		// are shorter), out again quickly.
		if (!g_vr.menuMode && g_vr.menuVotes >= 45) SetMenuMode(true, coverage);
		if (g_vr.menuMode && g_vr.menuVotes <= -4) SetMenuMode(false, coverage);
	}
	// measure the newest frame that has a scene-only image
	int newest = -1;
	for (int i = 0; i < 3; i++)
		if (g_vr.views[i].hasFrame && g_vr.views[i].tag.hasScene && !g_vr.views[i].tag.noUI && (newest < 0 || g_vr.views[i].seq > g_vr.views[newest].seq))
			newest = i;  // (images drawn without UI have none to measure)
	if (newest < 0 || g_vr.views[newest].seq == g_vr.coverageMeasuredSeq || g_vr.coveragePending[g_vr.coverageNext]) return;
	Blit(g_vr.coverageRtv, kCovW, kCovH, g_vr.views[newest].srv, KeySrv(g_vr.views[newest]), false);
	g_vr.context->CopyResource(g_vr.coverageStaging[g_vr.coverageNext], g_vr.coverageTex);
	g_vr.coveragePending[g_vr.coverageNext] = true;
	g_vr.coverageNext = (g_vr.coverageNext + 1) % 3;
	g_vr.coverageMeasuredSeq = g_vr.views[newest].seq;
}

namespace {
// Black bands above and below a picture. The game draws at the headset's (taller) shape and fits
// films (16:9) and its menus (5:4) into the width, leaving black rows above and below. Each picture
// is shrunk to kBarW x kBarH and read back a frame or two later; the rows with anything in them are
// the picture. Symmetric bands only (a loading screen's text isn't a picture edge), and a new result
// only counts once it held for kBarVotes readings (kNoBarVotes to drop the bands), so fades don't make the screen jump.
// (readings come at the headset's rate: ~90 a second)
constexpr int32_t kBarW = 32, kBarH = 256, kBarVotes = 20, kNoBarVotes = 90, kBarSlack = 3;
struct BarMeter {
	const char* what;
	ID3D11Texture2D* tex;
	ID3D11RenderTargetView* rtv;
	ID3D11Texture2D* staging[3];
	bool pending[3], failed;
	int32_t next;
	int32_t candTop, candBottom, votes;  // the latest reading and how many times in a row it came
	int32_t top, bottom;                 // the rows in use (0, kBarH - 1 = no bands)
};
BarMeter s_screenBars = {"flat screen", nullptr, nullptr, {}, {}, false, 0, 0, kBarH - 1, 0, 0, kBarH - 1};
BarMeter s_eyeBars = {"3D picture", nullptr, nullptr, {}, {}, false, 0, 0, kBarH - 1, 0, 0, kBarH - 1};

bool EnsureBarTargets(BarMeter& m) {
	if (m.tex) return true;
	if (m.failed) return false;
	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = kBarW;
	desc.Height = kBarH;
	desc.MipLevels = desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_RENDER_TARGET;
	HRESULT hr = g_vr.device->CreateTexture2D(&desc, nullptr, &m.tex);
	if (SUCCEEDED(hr)) hr = g_vr.device->CreateRenderTargetView(m.tex, nullptr, &m.rtv);
	desc.Usage = D3D11_USAGE_STAGING;
	desc.BindFlags = 0;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	for (int i = 0; i < 3 && SUCCEEDED(hr); i++) hr = g_vr.device->CreateTexture2D(&desc, nullptr, &m.staging[i]);
	if (FAILED(hr)) {
		Log("ERROR: black band targets (%s) failed 0x%08x", m.what, (unsigned)hr);
		SafeRelease(m.rtv);
		SafeRelease(m.tex);
		for (auto& texture : m.staging) SafeRelease(texture);
		m.failed = true;
		return false;
	}
	return true;
}

// Reads back what is ready, measures `srv` again. True when the rows in use changed.
bool MeasureBars(BarMeter& m, ID3D11ShaderResourceView* srv) {
	if (!srv || !EnsureBarTargets(m)) return false;
	bool changed = false;
	for (int i = 0; i < 3; i++) {
		if (!m.pending[i]) continue;
		D3D11_MAPPED_SUBRESOURCE mapped;
		if (g_vr.context->Map(m.staging[i], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped) != S_OK) continue;
		int32_t top = -1, bottom = -1;
		for (int y = 0; y < kBarH; y++) {
			const uint8_t* row = (const uint8_t*)mapped.pData + y * mapped.RowPitch;
			bool lit = false;
			for (int x = 0; x < kBarW && !lit; x++) lit = row[x * 4] > 3 || row[x * 4 + 1] > 3 || row[x * 4 + 2] > 3;  // not alpha
			if (lit) {
				if (top < 0) top = y;
				bottom = y;
			}
		}
		g_vr.context->Unmap(m.staging[i], 0);
		m.pending[i] = false;
		if (top < 0) continue;  // all black (a fade): keep what is in use
		bool bands = top >= 2 && kBarH - 1 - bottom >= 2 && abs(top - (kBarH - 1 - bottom)) <= 2 && bottom - top + 1 >= kBarH * 3 / 10;
		if (!bands) top = 0, bottom = kBarH - 1;
		// a reading within a few rows of the bands in use is the same picture (a dark edge row comes and goes)
		bool inUseBands = m.top > 0 || m.bottom < kBarH - 1;
		if (bands && inUseBands && abs(top - m.top) <= kBarSlack && abs(bottom - m.bottom) <= kBarSlack) top = m.top, bottom = m.bottom;
		if (top == m.candTop && bottom == m.candBottom) m.votes++;
		else m.candTop = top, m.candBottom = bottom, m.votes = 1;
		// dropping the bands needs a full second of pictures reaching the edges (a bright flash or a dark
		// frame in a film doesn't make the screen jump to full height)
		int32_t need = bands ? kBarVotes : kNoBarVotes;
		if (m.votes >= need && (top != m.top || bottom != m.bottom)) {
			m.top = top;
			m.bottom = bottom;
			changed = true;
		}
	}
	if (m.pending[m.next]) return changed;
	Blit(m.rtv, kBarW, kBarH, srv, nullptr, false);
	g_vr.context->CopyResource(m.staging[m.next], m.tex);
	m.pending[m.next] = true;
	m.next = (m.next + 1) % 3;
	return changed;
}

// The part of the picture (0..1 from the top) between the bands; each row sample sits in the middle
// of its band of the picture, so the edges are taken halfway into the first and last lit samples.
void BarRows(const BarMeter& m, float* v0, float* v1) {
	bool bands = m.top > 0 || m.bottom < kBarH - 1;
	*v0 = bands ? (m.top + 0.5f) / kBarH : 0;
	*v1 = bands ? (m.bottom + 0.5f) / kBarH : 1;
}

void LogBars(const BarMeter& m) {
	float v0, v1;
	BarRows(m, &v0, &v1);
	if (v0 > 0) Log("black bands: %s has bands above and below; its picture is rows %.1f%%-%.1f%% (%.2f:1 at the game's shape)", m.what,
					(double)(v0 * 100), (double)(v1 * 100), (double)((float)g_vr.gameW / ((float)g_vr.gameH * (v1 - v0))));
	else Log("black bands: %s has none now", m.what);
}
}  // namespace

void ScreenPictureRows(ID3D11ShaderResourceView* srv, float* v0, float* v1) {
	if (!g_cfg.cropBlackBars) {
		*v0 = 0;
		*v1 = 1;
		return;
	}
	if (MeasureBars(s_screenBars, srv)) LogBars(s_screenBars);
	BarRows(s_screenBars, v0, v1);
}

void WatchEyeBars(ID3D11ShaderResourceView* srv) {
	if (!MeasureBars(s_eyeBars, srv)) return;
	LogBars(s_eyeBars);
	if (s_eyeBars.top > 0) CaptureRequest("black bands appeared in the 3D picture");
}
