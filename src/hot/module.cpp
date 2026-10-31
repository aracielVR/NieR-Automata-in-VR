// module.cpp - NieRVR_debug.dll's exports, and Log / g_cfg / g_modDir backed by d3d11.dll.
#include "hot/module.h"
#include "core/config.h"
#include "core/log.h"
#include "core/main.h"
#include "hot/code_dump.h"
#include "hot/game_state.h"
#include "hot/hot_api.h"
#include "hot/state_diff.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

Config g_cfg;  // copied from d3d11.dll's each frame
wchar_t g_modDir[MAX_PATH];

namespace {
const HotHost* s_host;
}

void Log(const char* fmt, ...) {
	char text[1024];
	va_list args;
	va_start(args, fmt);
	vsnprintf(text, sizeof(text), fmt, args);
	va_end(args);
	s_host->log(text);
}

bool GameIsForeground() { return s_host->gameIsForeground(); }

void RequestCapture(const char* why) { s_host->requestCapture(why); }

extern "C" __declspec(dllexport) bool HotInit(const HotHost* host) {
	if (host->version != kHotApiVersion || host->configSize != sizeof(Config)) return false;
	s_host = host;
	lstrcpynW(g_modDir, host->modDir, MAX_PATH);
	return true;
}

extern "C" __declspec(dllexport) void HotPoll() {
	memcpy(&g_cfg, s_host->cfg, sizeof(g_cfg));
	CodeDumpPoll();
	GameStatePoll();
	StateDiffPoll();
}
