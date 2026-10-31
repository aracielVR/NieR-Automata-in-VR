// hot_api.h - what d3d11.dll gives the hot-reloadable NieRVR_debug.dll (debug/hot_module.cpp loads it).
#pragma once
#include <stdint.h>
#include "core/config.h"

constexpr uint32_t kHotApiVersion = 2;  // raise when HotHost changes

struct HotHost {
	uint32_t version, configSize;  // kHotApiVersion, sizeof(Config): both DLLs built from the same headers
	void (*log)(const char* text);
	const Config* cfg;
	const wchar_t* modDir;
	bool (*gameIsForeground)();
	void (*requestCapture)(const char* why);  // CaptureRequest
};

using HotInitFn = bool (*)(const HotHost* host);  // false: built from different headers
using HotPollFn = void (*)();                     // each game frame (Present)
