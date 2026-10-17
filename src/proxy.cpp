// proxy.cpp - stands in for d3d11.dll: loads the real one and forwards every export to it.
#include "common.h"

static const char* const kThunkNames[] = {
#include "gen/export_names.inc"
};
static const int kThunkCount = (int)(sizeof(kThunkNames) / sizeof(kThunkNames[0]));

extern "C" {
// Read by the assembly thunks (gen/thunks.S).
void* g_realProcs[sizeof(kThunkNames) / sizeof(kThunkNames[0])];
void NVR_ResolveAll();
}

PFN_D3D11_CREATE_DEVICE g_realCreateDevice;
PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN g_realCreateDeviceAndSwapChain;
static HMODULE g_realDll;

// Used for any export the real DLL doesn't have (e.g. older Windows builds).
static HRESULT WINAPI MissingExport() { return E_NOTIMPL; }

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
        g_realDll = LoadLibraryW(path);
        if (g_realDll)
            Log("chain-loaded %S", path);
        else
            Log("ERROR: ChainLoad '%S' failed to load (error %u), falling back to the system d3d11.dll", path,
                (unsigned)GetLastError());
    }
    if (!g_realDll) {
        UINT n = GetSystemDirectoryW(path, MAX_PATH - 16);
        lstrcpyW(path + n, L"\\d3d11.dll");
        g_realDll = LoadLibraryW(path);
        if (!g_realDll) {
            Log("ERROR: could not load %S (error %u)", path, (unsigned)GetLastError());
            return false;
        }
        Log("loaded system %S", path);
    }
    if (g_realDll == g_hSelf) {
        Log("ERROR: real d3d11 resolved to this mod itself - check ChainLoad");
        return false;
    }
    g_realCreateDevice = (PFN_D3D11_CREATE_DEVICE)(void*)GetProcAddress(g_realDll, "D3D11CreateDevice");
    g_realCreateDeviceAndSwapChain =
        (PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN)(void*)GetProcAddress(g_realDll, "D3D11CreateDeviceAndSwapChain");
    int missing = 0;
    for (int i = 0; i < kThunkCount; i++) {
        void* p = (void*)GetProcAddress(g_realDll, kThunkNames[i]);
        if (!p) {
            missing++;
            LogV("real d3d11 lacks export %s", kThunkNames[i]);
            p = (void*)&MissingExport;
        }
        g_realProcs[i] = p;
    }
    Log("proxy ready: %d forwarded exports (%d not present in this Windows build)", kThunkCount, missing);
    return g_realCreateDevice && g_realCreateDeviceAndSwapChain;
}

extern "C" void NVR_ResolveAll() { EnsureInit(); }

// ---------------------------------------------------------------------------------------------
// The two exports the game uses to create its renderer.
// ---------------------------------------------------------------------------------------------
extern "C" HRESULT WINAPI NVR_D3D11CreateDevice(IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType, HMODULE software,
                                                UINT flags, const D3D_FEATURE_LEVEL* levels, UINT numLevels,
                                                UINT sdkVersion, ID3D11Device** device, D3D_FEATURE_LEVEL* outLevel,
                                                ID3D11DeviceContext** context) {
    EnsureInit();
    if (!g_realCreateDevice) return E_FAIL;
    HRESULT hr = g_realCreateDevice(adapter, driverType, software, flags, levels, numLevels, sdkVersion, device,
                                    outLevel, context);
    Log("game called D3D11CreateDevice(driverType=%d, flags=0x%x) -> 0x%08x", (int)driverType, flags, (unsigned)hr);
    if (SUCCEEDED(hr) && device && *device && g_cfg.enabled) {
        HookWithDummySwapChain();
        ProbeInstall(*device);
    }
    return hr;
}

extern "C" HRESULT WINAPI NVR_D3D11CreateDeviceAndSwapChain(IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType,
                                                            HMODULE software, UINT flags,
                                                            const D3D_FEATURE_LEVEL* levels, UINT numLevels,
                                                            UINT sdkVersion, const DXGI_SWAP_CHAIN_DESC* scDesc,
                                                            IDXGISwapChain** swapChain, ID3D11Device** device,
                                                            D3D_FEATURE_LEVEL* outLevel,
                                                            ID3D11DeviceContext** context) {
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
        if (device && *device) ProbeInstall(*device);
    }
    return hr;
}
