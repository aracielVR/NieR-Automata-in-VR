// xr_loader.cpp - locating the OpenXR runtime.
//
// Two ways in:
//  1. The official Khronos openxr_loader.dll, if present next to the mod (or set via LoaderPath).
//     This also enables OpenXR API layers such as OpenXR Toolkit.
//  2. Otherwise we do what that loader would do for the runtime part: read the active runtime's
//     manifest from the registry and negotiate with the runtime DLL directly. This needs no extra
//     files and works with SteamVR, Meta/Oculus, Virtual Desktop (VDXR), Pimax, Pico, WMR, etc.
#include "xr.h"

static bool IsAbsolute(const wchar_t* p) { return p[0] && (p[1] == L':' || (p[0] == L'\\' && p[1] == L'\\')); }

static PFN_xrGetInstanceProcAddr FromLoaderDll(const wchar_t* path) {
    HMODULE m = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m) {
        Log("OpenXR: could not load loader '%S' (error %u)", path, (unsigned)GetLastError());
        return nullptr;
    }
    PFN_xrGetInstanceProcAddr gipa = (PFN_xrGetInstanceProcAddr)(void*)GetProcAddress(m, "xrGetInstanceProcAddr");
    if (gipa) Log("OpenXR: using loader %S (API layers enabled)", path);
    else Log("OpenXR: '%S' has no xrGetInstanceProcAddr export", path);
    return gipa;
}

static bool ReadManifestPath(wchar_t* out, DWORD cap) {
    DWORD n = GetEnvironmentVariableW(L"XR_RUNTIME_JSON", out, cap);
    if (n > 0 && n < cap) {
        Log("OpenXR: XR_RUNTIME_JSON overrides the active runtime: %S", out);
        return true;
    }
    DWORD bytes = cap * sizeof(wchar_t);
    LSTATUS st = RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", L"ActiveRuntime",
                              RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, out, &bytes);
    if (st != ERROR_SUCCESS) {
        Log("ERROR: OpenXR: no active OpenXR runtime is set (HKLM\\SOFTWARE\\Khronos\\OpenXR\\1\\ActiveRuntime, "
            "status %d). Set one in your VR software (SteamVR, Meta Link, Virtual Desktop, Pimax Play...)",
            (int)st);
        return false;
    }
    return true;
}

static PFN_xrGetInstanceProcAddr FromActiveRuntime() {
    wchar_t manifest[MAX_PATH * 2];
    if (!ReadManifestPath(manifest, MAX_PATH * 2)) return nullptr;
    Log("OpenXR: active runtime manifest: %S", manifest);

    HANDLE f = CreateFileW(manifest, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        Log("ERROR: OpenXR: cannot open runtime manifest (error %u)", (unsigned)GetLastError());
        return nullptr;
    }
    static char json[65536];
    DWORD len = 0;
    ReadFile(f, json, sizeof(json) - 1, &len, nullptr);
    CloseHandle(f);
    json[len] = 0;

    char name[256] = "?";
    pure::json_find_string(json, len, "name", name, sizeof(name));
    char lib[MAX_PATH * 2];
    if (!pure::json_find_string(json, len, "library_path", lib, sizeof(lib))) {
        Log("ERROR: OpenXR: runtime manifest has no library_path");
        return nullptr;
    }
    wchar_t libW[MAX_PATH * 2];
    if (!MultiByteToWideChar(CP_UTF8, 0, lib, -1, libW, MAX_PATH * 2)) return nullptr;
    for (wchar_t* p = libW; *p; p++)
        if (*p == L'/') *p = L'\\';

    wchar_t full[MAX_PATH * 2];
    if (IsAbsolute(libW)) {
        lstrcpynW(full, libW, MAX_PATH * 2);
    } else {
        // relative to the manifest's folder
        lstrcpynW(full, manifest, MAX_PATH * 2);
        wchar_t* slash = nullptr;
        for (wchar_t* p = full; *p; p++)
            if (*p == L'\\' || *p == L'/') slash = p;
        if (slash) slash[1] = 0;
        else full[0] = 0;
        lstrcpynW(full + lstrlenW(full), libW, MAX_PATH * 2 - lstrlenW(full));
    }
    Log("OpenXR: runtime '%s' -> %S", name, full);

    HMODULE m = LoadLibraryExW(full, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m) {
        Log("ERROR: OpenXR: runtime DLL failed to load (error %u)", (unsigned)GetLastError());
        return nullptr;
    }

    char negName[128] = "xrNegotiateLoaderRuntimeInterface";
    pure::json_find_string(json, len, "xrNegotiateLoaderRuntimeInterface", negName, sizeof(negName));
    PFN_xrNegotiateLoaderRuntimeInterface negotiate =
        (PFN_xrNegotiateLoaderRuntimeInterface)(void*)GetProcAddress(m, negName);
    if (negotiate) {
        XrNegotiateLoaderInfo li = {};
        li.structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO;
        li.structVersion = XR_LOADER_INFO_STRUCT_VERSION;
        li.structSize = sizeof(li);
        li.minInterfaceVersion = 1;
        li.maxInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
        li.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
        li.maxApiVersion = XR_MAKE_VERSION(1, 0x3ff, 0xfff);
        XrNegotiateRuntimeRequest rr = {};
        rr.structType = XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST;
        rr.structVersion = XR_RUNTIME_INFO_STRUCT_VERSION;
        rr.structSize = sizeof(rr);
        XrResult r = negotiate(&li, &rr);
        if (XR_SUCCEEDED(r) && rr.getInstanceProcAddr) {
            Log("OpenXR: negotiated with runtime (interface %u, API %u.%u)", rr.runtimeInterfaceVersion,
                (unsigned)XR_VERSION_MAJOR(rr.runtimeApiVersion), (unsigned)XR_VERSION_MINOR(rr.runtimeApiVersion));
            return rr.getInstanceProcAddr;
        }
        Log("OpenXR: runtime negotiation failed (%d), trying plain export", (int)r);
    }
    PFN_xrGetInstanceProcAddr gipa = (PFN_xrGetInstanceProcAddr)(void*)GetProcAddress(m, "xrGetInstanceProcAddr");
    if (!gipa) Log("ERROR: OpenXR: runtime exports neither %s nor xrGetInstanceProcAddr", negName);
    return gipa;
}

PFN_xrGetInstanceProcAddr XrLoadEntry() {
    static PFN_xrGetInstanceProcAddr cached;
    if (cached) return cached;
    wchar_t path[MAX_PATH];
    if (g_cfg.loaderPath[0]) {
        if (IsAbsolute(g_cfg.loaderPath)) {
            lstrcpynW(path, g_cfg.loaderPath, MAX_PATH);
        } else {
            lstrcpynW(path, g_modDir, MAX_PATH);
            lstrcpynW(path + lstrlenW(path), g_cfg.loaderPath, MAX_PATH - lstrlenW(path));
        }
        cached = FromLoaderDll(path);
    } else {
        lstrcpynW(path, g_modDir, MAX_PATH - 24);
        lstrcatW(path, L"openxr_loader.dll");
        if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) cached = FromLoaderDll(path);
    }
    if (!cached) cached = FromActiveRuntime();
    return cached;
}
