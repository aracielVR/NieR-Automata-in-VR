// d3d11_proxy.cpp - stands in for d3d11.dll: loads the real one and forwards every export to it.
#include "core/d3d11_proxy.h"
#include <iterator>
#include "core/config.h"
#include "core/log.h"
#include "core/main.h"
#include "render/d3d_hooks.h"
#include "render/frame_handoff.h"

namespace {
const char* const kThunkNames[] = {
#include "gen/export_names.inc"
};
constexpr int32_t kThunkCount = (int32_t)std::size(kThunkNames);
}  // namespace

extern "C" {
// Read by the assembly thunks (gen/thunks.S).
void* g_realProcs[std::size(kThunkNames)];
void NVR_ResolveAll();
}

namespace {
PFN_D3D11_CREATE_DEVICE s_realCreateDevice;
}  // namespace
PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN g_realCreateDeviceAndSwapChain;
namespace {
HMODULE s_realDll;

// Used for any export the real DLL doesn't have (e.g. older Windows builds).
HRESULT WINAPI MissingExport() { return E_NOTIMPL; }
}  // namespace

bool ProxyLoadReal() {
	wchar_t path[MAX_PATH];
	if (g_cfg.chainLoad[0]) {
		// Relative names are taken from the mod folder
		if (g_cfg.chainLoad[1] == L':' || g_cfg.chainLoad[0] == L'\\') {
			lstrcpynW(path, g_cfg.chainLoad, MAX_PATH);
		} else {
			lstrcpynW(path, g_modDir, MAX_PATH);
			lstrcpynW(path + lstrlenW(path), g_cfg.chainLoad, MAX_PATH - lstrlenW(path));
		}
		s_realDll = LoadLibraryW(path);
		if (s_realDll)
			Log("chain-loaded %s", Utf8(path).s);
		else
			Log("ERROR: ChainLoad '%s' failed to load (error %u), falling back to the system d3d11.dll", Utf8(path).s,
				(unsigned)GetLastError());
	}
	if (!s_realDll) {
		UINT length = GetSystemDirectoryW(path, MAX_PATH - 16);
		lstrcpyW(path + length, L"\\d3d11.dll");
		s_realDll = LoadLibraryW(path);
		if (!s_realDll) {
			Log("ERROR: could not load %s (error %u)", Utf8(path).s, (unsigned)GetLastError());
			return false;
		}
		Log("loaded system %s", Utf8(path).s);
	}
	if (s_realDll == g_hSelf) {
		Log("ERROR: real d3d11 resolved to this mod itself - check ChainLoad");
		return false;
	}
	s_realCreateDevice = (PFN_D3D11_CREATE_DEVICE)(void*)GetProcAddress(s_realDll, "D3D11CreateDevice");
	g_realCreateDeviceAndSwapChain =
		(PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN)(void*)GetProcAddress(s_realDll, "D3D11CreateDeviceAndSwapChain");
	int missing = 0;
	for (int i = 0; i < kThunkCount; i++) {
		void* proc = (void*)GetProcAddress(s_realDll, kThunkNames[i]);
		if (!proc) {
			missing++;
			LogV("real d3d11 lacks export %s", kThunkNames[i]);
			proc = (void*)&MissingExport;
		}
		g_realProcs[i] = proc;
	}
	Log("proxy ready: %d forwarded exports (%d not present in this Windows build)", kThunkCount, missing);
	return s_realCreateDevice && g_realCreateDeviceAndSwapChain;
}

extern "C" void NVR_ResolveAll() { EnsureInit(); }

// The two exports the game uses to create its renderer.
extern "C" HRESULT WINAPI NVR_D3D11CreateDevice(IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType, HMODULE software, UINT flags, const D3D_FEATURE_LEVEL* levels, UINT numLevels, UINT sdkVersion, ID3D11Device** device, D3D_FEATURE_LEVEL* outLevel, ID3D11DeviceContext** context) {
	EnsureInit();
	if (!s_realCreateDevice) return E_FAIL;
	HRESULT hr = s_realCreateDevice(adapter, driverType, software, flags, levels, numLevels, sdkVersion, device,
									outLevel, context);
	Log("game called D3D11CreateDevice(driverType=%d, flags=0x%x) -> 0x%08x", (int)driverType, flags, (unsigned)hr);
	if (SUCCEEDED(hr) && device && *device && g_cfg.enabled) {
		HookWithDummySwapChain();
		D3dHooksInstall(*device);
	}
	return hr;
}

extern "C" HRESULT WINAPI NVR_D3D11CreateDeviceAndSwapChain(IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType, HMODULE software, UINT flags, const D3D_FEATURE_LEVEL* levels, UINT numLevels, UINT sdkVersion, const DXGI_SWAP_CHAIN_DESC* scDesc, IDXGISwapChain** swapChain, ID3D11Device** device, D3D_FEATURE_LEVEL* outLevel, ID3D11DeviceContext** context) {
	EnsureInit();
	if (!g_realCreateDeviceAndSwapChain) return E_FAIL;
	HRESULT hr = g_realCreateDeviceAndSwapChain(adapter, driverType, software, flags, levels, numLevels, sdkVersion,
												scDesc, swapChain, device, outLevel, context);
	if (scDesc)
		Log("game called D3D11CreateDeviceAndSwapChain(%ux%u fmt=%d buffers=%u msaa=%u windowed=%d effect=%d) -> 0x%08x",
			scDesc->BufferDesc.Width, scDesc->BufferDesc.Height, (int)scDesc->BufferDesc.Format, scDesc->BufferCount,
			scDesc->SampleDesc.Count, (int)scDesc->Windowed, (int)scDesc->SwapEffect, (unsigned)hr);
	if (SUCCEEDED(hr) && g_cfg.enabled) {
		if (swapChain && *swapChain)
			HookFromSwapChain(*swapChain);
		else if (device && *device)
			HookWithDummySwapChain();
		if (device && *device) D3dHooksInstall(*device);
	}
	return hr;
}
