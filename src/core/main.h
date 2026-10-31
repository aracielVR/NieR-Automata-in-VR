// main.h - DLL entry, one-time initialisation, crash reporting (main.cpp).
#pragma once
#include <atomic>
#include <windows.h>

extern HMODULE g_hSelf;
extern wchar_t g_modDir[MAX_PATH];  // directory of this DLL, with trailing backslash
extern wchar_t g_iniPath[MAX_PATH];
extern std::atomic<bool> g_initDone;  // EnsureInit has finished

void EnsureInit();  // idempotent, thread safe; loads config, log, real d3d11
void InstallCrashHandler();
void DescribeAddress(void* addr, char* out, size_t cap);  // "module.dll+0x1234"
