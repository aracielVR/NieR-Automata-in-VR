// runtime_loader.cpp - locating the OpenXR runtime.
#include "xr/runtime_loader.h"
#include "core/config.h"
#include "core/log.h"
#include "core/main.h"
#include "core/pure.h"

namespace {
bool IsAbsolute(const wchar_t* path) { return path[0] && (path[1] == L':' || (path[0] == L'\\' && path[1] == L'\\')); }

PFN_xrGetInstanceProcAddr FromLoaderDll(const wchar_t* path) {
	HMODULE module = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (!module) {
		Log("OpenXR: could not load loader '%s' (error %u)", Utf8(path).s, (unsigned)GetLastError());
		return nullptr;
	}
	PFN_xrGetInstanceProcAddr getProcAddr = (PFN_xrGetInstanceProcAddr)(void*)GetProcAddress(module, "xrGetInstanceProcAddr");
	if (getProcAddr) Log("OpenXR: using loader %s (API layers enabled)", Utf8(path).s);
	else Log("OpenXR: '%s' has no xrGetInstanceProcAddr export", Utf8(path).s);
	return getProcAddr;
}

bool ReadManifestPath(wchar_t* out, DWORD cap) {
	DWORD length = GetEnvironmentVariableW(L"XR_RUNTIME_JSON", out, cap);
	if (length > 0 && length < cap) {
		Log("OpenXR: XR_RUNTIME_JSON overrides the active runtime: %s", Utf8(out).s);
		return true;
	}
	DWORD bytes = cap * sizeof(wchar_t);
	LSTATUS status = RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", L"ActiveRuntime",
								  RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, out, &bytes);
	if (status != ERROR_SUCCESS) {
		Log("ERROR: OpenXR: no active OpenXR runtime is set (HKLM\\SOFTWARE\\Khronos\\OpenXR\\1\\ActiveRuntime, "
			"status %d). Set one in your VR software (SteamVR, Meta Link, Virtual Desktop, Pimax Play...)",
			(int)status);
		return false;
	}
	return true;
}

PFN_xrGetInstanceProcAddr FromActiveRuntime() {
	wchar_t manifest[MAX_PATH * 2];
	if (!ReadManifestPath(manifest, MAX_PATH * 2)) return nullptr;
	Log("OpenXR: active runtime manifest: %s", Utf8(manifest).s);

	HANDLE file = CreateFileW(manifest, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		Log("ERROR: OpenXR: cannot open runtime manifest (error %u)", (unsigned)GetLastError());
		return nullptr;
	}
	static char json[65536];
	DWORD len = 0;
	ReadFile(file, json, sizeof(json) - 1, &len, nullptr);
	CloseHandle(file);
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
	for (wchar_t* c = libW; *c; c++)
		if (*c == L'/') *c = L'\\';

	wchar_t full[MAX_PATH * 2];
	if (IsAbsolute(libW)) {
		lstrcpynW(full, libW, MAX_PATH * 2);
	} else {
		// relative to the manifest's folder
		lstrcpynW(full, manifest, MAX_PATH * 2);
		wchar_t* slash = nullptr;
		for (wchar_t* c = full; *c; c++)
			if (*c == L'\\' || *c == L'/') slash = c;
		if (slash) slash[1] = 0;
		else full[0] = 0;
		lstrcpynW(full + lstrlenW(full), libW, MAX_PATH * 2 - lstrlenW(full));
	}
	Log("OpenXR: runtime '%s' -> %s", name, Utf8(full).s);

	HMODULE module = LoadLibraryExW(full, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (!module) {
		Log("ERROR: OpenXR: runtime DLL failed to load (error %u)", (unsigned)GetLastError());
		return nullptr;
	}

	char negName[128] = "xrNegotiateLoaderRuntimeInterface";
	pure::json_find_string(json, len, "xrNegotiateLoaderRuntimeInterface", negName, sizeof(negName));
	PFN_xrNegotiateLoaderRuntimeInterface negotiate =
		(PFN_xrNegotiateLoaderRuntimeInterface)(void*)GetProcAddress(module, negName);
	if (negotiate) {
		XrNegotiateLoaderInfo loaderInfo = {};
		loaderInfo.structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO;
		loaderInfo.structVersion = XR_LOADER_INFO_STRUCT_VERSION;
		loaderInfo.structSize = sizeof(loaderInfo);
		loaderInfo.minInterfaceVersion = 1;
		loaderInfo.maxInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
		loaderInfo.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
		loaderInfo.maxApiVersion = XR_MAKE_VERSION(1, 0x3ff, 0xfff);
		XrNegotiateRuntimeRequest runtimeRequest = {};
		runtimeRequest.structType = XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST;
		runtimeRequest.structVersion = XR_RUNTIME_INFO_STRUCT_VERSION;
		runtimeRequest.structSize = sizeof(runtimeRequest);
		XrResult result = negotiate(&loaderInfo, &runtimeRequest);
		if (XR_SUCCEEDED(result) && runtimeRequest.getInstanceProcAddr) {
			Log("OpenXR: negotiated with runtime (interface %u, API %u.%u)", runtimeRequest.runtimeInterfaceVersion,
				(unsigned)XR_VERSION_MAJOR(runtimeRequest.runtimeApiVersion), (unsigned)XR_VERSION_MINOR(runtimeRequest.runtimeApiVersion));
			return runtimeRequest.getInstanceProcAddr;
		}
		Log("OpenXR: runtime negotiation failed (%d), trying plain export", (int)result);
	}
	PFN_xrGetInstanceProcAddr getProcAddr = (PFN_xrGetInstanceProcAddr)(void*)GetProcAddress(module, "xrGetInstanceProcAddr");
	if (!getProcAddr) Log("ERROR: OpenXR: runtime exports neither %s nor xrGetInstanceProcAddr", negName);
	return getProcAddr;
}
}  // namespace

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
