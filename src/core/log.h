// log.h - the log file (NieRVR.log, log.cpp).
#pragma once
#include <windows.h>
#include <atomic>
#include <stdint.h>

void LogOpen(const wchar_t* path);
void Log(const char* fmt, ...) __attribute__((format(gnu_printf, 1, 2)));  // the compiler checks the arguments
void LogV(const char* fmt, ...) __attribute__((format(gnu_printf, 1, 2)));  // only when [Debug] Verbose=1
// Returns true while fewer than `max` messages were logged through this counter.
bool LogBudget(std::atomic<int32_t>& counter, int32_t max);
double NowMs();

// A wide string (path, GPU name) as UTF-8, for "%s": Log("dir: %s", Utf8(path).s). snprintf's own
// %ls would convert with the C locale and fail on non-ASCII names.
struct Utf8 {
	char s[1024];
	explicit Utf8(const wchar_t* w);
};
