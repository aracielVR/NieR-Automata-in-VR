// hot_module.cpp - [Debug] loads NieRVR_debug.dll (src/hot/) and reloads it whenever it's rebuilt.
#include "debug/hot_module.h"
#include "core/config.h"
#include "core/log.h"
#include "core/main.h"
#include "debug/frame_capture.h"
#include "hot/hot_api.h"
#include "xr/xr_thread.h"
#include <windows.h>

namespace {
constexpr wchar_t kFileName[] = L"NieRVR_debug.dll";
constexpr wchar_t kLoadedName[] = L"NieRVR_debug.loaded.dll";  // the copy that's loaded: the build can replace the original
constexpr DWORD kCheckMs = 1000;

HMODULE s_module;
HotPollFn s_poll;
FILETIME s_loadedTime, s_seenTime;  // the loaded build's write time, and the one seen at the last check
bool s_checked;
DWORD s_checkedAt;

void HostLog(const char* text) { Log("%s", text); }

const HotHost s_host = {kHotApiVersion, sizeof(Config), HostLog, &g_cfg, g_modDir, GameIsForeground, CaptureRequest};

void ModPath(wchar_t* out, const wchar_t* name) {
	lstrcpynW(out, g_modDir, MAX_PATH - 32);
	lstrcatW(out, name);
}

bool WrittenAt(const wchar_t* path, FILETIME* out) {
	WIN32_FILE_ATTRIBUTE_DATA data;
	if (!GetFileAttributesExW(path, GetFileExInfoStandard, &data)) return false;
	*out = data.ftLastWriteTime;
	return true;
}

void Unload() {
	if (s_module) FreeLibrary(s_module);
	s_module = nullptr;
	s_poll = nullptr;
}

void Load(const wchar_t* path, const FILETIME& written) {
	Unload();
	wchar_t loaded[MAX_PATH];
	ModPath(loaded, kLoadedName);
	if (!CopyFileW(path, loaded, FALSE)) return;  // still being written: next check
	s_loadedTime = written;
	s_module = LoadLibraryW(loaded);
	if (!s_module) {
		Log("ERROR: hot module: cannot load NieRVR_debug.dll (error %u)", (unsigned)GetLastError());
		return;
	}
	auto init = (HotInitFn)(void*)GetProcAddress(s_module, "HotInit");
	s_poll = (HotPollFn)(void*)GetProcAddress(s_module, "HotPoll");
	if (!init || !s_poll || !init(&s_host)) {
		Log("ERROR: hot module: NieRVR_debug.dll was built from different headers than d3d11.dll: rebuild and restart");
		Unload();
		return;
	}
	Log("hot module: NieRVR_debug.dll loaded");
}

// Loads a new build once its write time has held still for a check (the linker is done)
void CheckFile() {
	wchar_t path[MAX_PATH];
	ModPath(path, kFileName);
	FILETIME written;
	if (!WrittenAt(path, &written)) return;
	bool settled = !CompareFileTime(&written, &s_seenTime);
	s_seenTime = written;
	if (settled && CompareFileTime(&written, &s_loadedTime)) Load(path, written);
}
}  // namespace

void HotModulePoll() {
	if (!g_cfg.probe) return;
	DWORD now = GetTickCount();
	if (!s_checked || now - s_checkedAt >= kCheckMs) {
		s_checked = true;
		s_checkedAt = now;
		CheckFile();
	}
	if (s_poll) s_poll();
}
