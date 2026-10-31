// foveated.cpp - foveated shading ([Render] FoveatedShading): the edges of each eye's 3D image shaded at a lower rate, with NVIDIA's variable rate shading.
#include "render/foveated.h"
#include "core/com.h"
#include "core/config.h"
#include "core/log.h"
#include "render/camera.h"
#include "render/foveation.h"
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <vector>
#include <windows.h>

namespace {
// NVAPI (nvapi.h, github.com/NVIDIA/nvapi, MIT): the driver's nvapi64.dll, found by function id
constexpr uint32_t kNvInitialize = 0x0150e828, kNvGetGraphicsCaps = 0x52b1499a, kNvCreateRateView = 0x99ca2dff,
				   kNvSetRateView = 0x1b0c2f83, kNvSetViewportRates = 0x34f7938f;
constexpr uint32_t kMaxRates = 16;
enum NvRate : int32_t { NV_RATE_X1 = 5,
						NV_RATE_2X2 = 8,
						NV_RATE_4X4 = 11 };  // NV_PIXEL_SHADING_RATE
struct NvCaps {                                                               // NV_D3D1x_GRAPHICS_CAPS_V2
	uint32_t bits;                                                            // bit 1: variable pixel rate shading
	uint16_t sm[4];
	uint32_t reserved[13];
};
struct NvViewportRates {  // NV_D3D11_VIEWPORT_SHADING_RATE_DESC_V1
	bool enable;
	int32_t table[kMaxRates];
};
struct NvViewportsRates {  // NV_D3D11_VIEWPORTS_SHADING_RATE_DESC_V1
	uint32_t version, count;
	NvViewportRates* viewports;
};
struct NvRateViewDesc {  // NV_D3D11_SHADING_RATE_RESOURCE_VIEW_DESC_V1
	uint32_t version;
	DXGI_FORMAT format;
	int32_t dimension;  // 4: Texture2D
	uint32_t mipSlice, pad[2];
};
constexpr uint32_t NvVersion(uint32_t size, uint32_t version) { return size | version << 16; }

using QueryFn = void*(__cdecl*)(uint32_t id);
using InitializeFn = int32_t(__cdecl*)();
using GetCapsFn = int32_t(__cdecl*)(IUnknown* device, uint32_t version, NvCaps* caps);
using CreateRateViewFn = int32_t(__cdecl*)(ID3D11Device* device, ID3D11Resource* resource, NvRateViewDesc* desc, IUnknown** view);
using SetRateViewFn = int32_t(__cdecl*)(IUnknown* context, IUnknown* view);
using SetViewportRatesFn = int32_t(__cdecl*)(IUnknown* context, NvViewportsRates* desc);

CreateRateViewFn s_createRateView;
SetRateViewFn s_setRateView;
SetViewportRatesFn s_setViewportRates;
bool s_checked, s_supported;
bool s_on;          // VRS is on in the context now
bool s_sceneOver;   // this frame's scene is done (the UI, our own passes)
// One map per lens: with AsymmetricEyes the two eyes look outwards, so the full-detail middle sits
// on a different side in each of them. Slot 0 = lens pointing left, 1 = straight ahead or right.
int32_t s_strength;  // the maps'
UINT s_mapW[2], s_mapH[2];
float s_mapHalfW[2], s_mapHalfH[2], s_mapCentreX[2], s_mapCentreY[2];
ID3D11Texture2D* s_map[2];
IUnknown* s_mapView[2];
int32_t s_slot;          // the one this frame's eye uses
IUnknown* s_boundView;   // the map handed to the driver last: never free this one while it is bound
bool s_loggedOn;
UINT s_gameW, s_gameH;

// Once: NVAPI and the GPU's support
bool Supported(ID3D11Device* device) {
	if (s_checked) return s_supported;
	s_checked = true;
	HMODULE nvapi = LoadLibraryW(L"nvapi64.dll");
	QueryFn query = nvapi ? (QueryFn)(void*)GetProcAddress(nvapi, "nvapi_QueryInterface") : nullptr;
	if (!query) {
		Log("foveated shading: off (no NVIDIA driver: variable rate shading needs an NVIDIA RTX or GTX 16 card)");
		return false;
	}
	auto initialize = (InitializeFn)query(kNvInitialize);
	auto getCaps = (GetCapsFn)query(kNvGetGraphicsCaps);
	s_createRateView = (CreateRateViewFn)query(kNvCreateRateView);
	s_setRateView = (SetRateViewFn)query(kNvSetRateView);
	s_setViewportRates = (SetViewportRatesFn)query(kNvSetViewportRates);
	NvCaps caps = {};
	bool ok = initialize && getCaps && s_createRateView && s_setRateView && s_setViewportRates && initialize() == 0 &&
			  getCaps(device, NvVersion(sizeof(NvCaps), 2), &caps) == 0;
	s_supported = ok && (caps.bits & 2);
	Log("foveated shading: %s", s_supported ? "available (NVIDIA variable rate shading)"
											: "off (this GPU or driver has no variable rate shading: NVIDIA RTX or GTX 16 cards)");
	return s_supported;
}

// The map must be re-bound whenever the eye changes, not only when the rates go on and off: each eye
// has its own map, and leaving the other eye's bound would hand the driver a map we are free to
// destroy a moment later (2026-10-27: that freed map was read inside the D3D11 runtime).
void SetRates(ID3D11DeviceContext* c, bool on) {
	IUnknown* want = on ? s_mapView[s_slot] : nullptr;
	if (on == s_on && want == s_boundView) return;
	NvViewportRates viewport = {true, {}};
	for (int32_t& rate : viewport.table) rate = NV_RATE_X1;
	viewport.table[pure::FOVEATION_HALF] = NV_RATE_2X2;
	viewport.table[pure::FOVEATION_QUARTER] = NV_RATE_4X4;
	NvViewportsRates desc = {NvVersion(sizeof(NvViewportsRates), 1), on ? 1u : 0u, &viewport};
	if (on) s_setRateView(c, want);
	if (on != s_on) s_setViewportRates(c, &desc);
	s_boundView = want;
	s_on = on;
}

// Only ever called with the rates switched off for this slot (see EnsureMap).
void ReleaseMap(int32_t slot) {
	SafeRelease(s_mapView[slot]);
	SafeRelease(s_map[slot]);
}

// The rate map for a width x height target and this frame's eye field of view (rebuilt when they change)
bool EnsureMap(ID3D11DeviceContext* c, ID3D11Device* device, UINT width, UINT height) {
	float halfW, halfH;
	float centre[2] = {0, 0};
	if (!StereoEyeTangents(&halfW, &halfH, centre)) return false;
	s_slot = centre[0] < 0 ? 0 : 1;
	bool same = s_map[s_slot] && s_mapW[s_slot] == width && s_mapH[s_slot] == height && s_strength == g_cfg.foveated &&
				fabsf(halfW - s_mapHalfW[s_slot]) < 0.01f * s_mapHalfW[s_slot] && fabsf(halfH - s_mapHalfH[s_slot]) < 0.01f * s_mapHalfH[s_slot] &&
				fabsf(centre[0] - s_mapCentreX[s_slot]) < 0.002f && fabsf(centre[1] - s_mapCentreY[s_slot]) < 0.002f;
	if (same) return true;
	// the driver still holds the old map if it is the bound one: take it back before freeing it
	if (s_mapView[s_slot] && s_mapView[s_slot] == s_boundView) SetRates(c, false);
	ReleaseMap(s_slot);
	int32_t tilesX = pure::foveation_tiles((int32_t)width), tilesY = pure::foveation_tiles((int32_t)height);
	std::vector<uint8_t> rates((size_t)(tilesX * tilesY));
	pure::foveation_map((int32_t)width, (int32_t)height, halfW, halfH, centre[0], centre[1], pure::foveation_rings(g_cfg.foveated), rates.data());
	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = (UINT)tilesX;
	desc.Height = (UINT)tilesY;
	desc.MipLevels = desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R8_UINT;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_IMMUTABLE;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	D3D11_SUBRESOURCE_DATA data = {rates.data(), (UINT)tilesX, 0};
	NvRateViewDesc viewDesc = {NvVersion(sizeof(NvRateViewDesc), 1), DXGI_FORMAT_R8_UINT, 4, 0, {}};
	if (FAILED(device->CreateTexture2D(&desc, &data, &s_map[s_slot])) || s_createRateView(device, s_map[s_slot], &viewDesc, &s_mapView[s_slot]) != 0) {
		ReleaseMap(s_slot);
		Log("foveated shading: off (the rate map could not be made)");
		s_supported = false;
		return false;
	}
	s_mapW[s_slot] = width;
	s_mapH[s_slot] = height;
	s_mapHalfW[s_slot] = halfW;
	s_mapHalfH[s_slot] = halfH;
	s_mapCentreX[s_slot] = centre[0];
	s_mapCentreY[s_slot] = centre[1];
	s_strength = g_cfg.foveated;
	if (!s_loggedOn) {
		s_loggedOn = true;
		pure::FoveationRings rings = pure::foveation_rings(g_cfg.foveated);
		Log("foveated shading: on for the scene (%ux%u, %dx%d tiles): full detail within %.0f deg of the centre, then half%s",
			width, height, tilesX, tilesY, (double)rings.fullDeg, rings.halfDeg < 89 ? " and a quarter beyond" : "");
	}
	return true;
}

bool TargetSize(ID3D11View* view, UINT* width, UINT* height) {
	ComPtr<ID3D11Resource> res;
	view->GetResource(&res);
	D3D11_RESOURCE_DIMENSION dim;
	if (!res || (res->GetType(&dim), dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D)) return false;
	D3D11_TEXTURE2D_DESC desc;
	((ID3D11Texture2D*)res.Get())->GetDesc(&desc);
	*width = desc.Width;
	*height = desc.Height;
	return desc.SampleDesc.Count == 1;
}
}  // namespace

void FoveatedFrameStart() { s_sceneOver = false; }
void FoveatedSetGameSize(UINT width, UINT height) {
	s_gameW = width;
	s_gameH = height;
}
void FoveatedSceneAgain() { s_sceneOver = false; }

void FoveatedSceneEnd(ID3D11DeviceContext* c) {
	s_sceneOver = true;
	if (s_on) SetRates(c, false);
}

void FoveatedOnTargets(ID3D11DeviceContext* c, UINT count, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) {
	bool want = g_cfg.foveated && !s_sceneOver && StereoActive();
	ID3D11View* view = count && rtvs && rtvs[0] ? (ID3D11View*)rtvs[0] : (ID3D11View*)dsv;
	UINT width = 0, height = 0;
	want = want && view && TargetSize(view, &width, &height) && width == s_gameW && height == s_gameH;
	if (want) {
		ComPtr<ID3D11Device> device;
		c->GetDevice(&device);
		want = device && Supported(device.Get()) && EnsureMap(c, device.Get(), width, height);
	}
	if (want || s_on) SetRates(c, want);
}
