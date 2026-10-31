// xr_main.h - the VR thread (xr_main.cpp).
#pragma once
#include <atomic>
#include <windows.h>

void XrStartThread();
extern std::atomic<DWORD> g_xrThreadId;
