// extra_resolutions.cpp - adds headset-shaped resolutions to the game's list.
#include "render/extra_resolutions.h"
#include "render/game_resolution.h"
#include "core/com.h"
#include "core/config.h"
#include "core/hook.h"
#include "core/log.h"
#include "core/main.h"
#include "core/pure.h"
#include <atomic>
#include <dxgi1_2.h>
#include <stdio.h>
#include <string.h>

namespace {
typedef HRESULT(WINAPI* PFN_CreateDXGIFactory)(REFIID, void**);
typedef HRESULT(WINAPI* PFN_CreateDXGIFactory2)(UINT, REFIID, void**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_GetDisplayModeList)(IDXGIOutput*, DXGI_FORMAT, UINT, UINT*, DXGI_MODE_DESC*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_GetDisplayModeList1)(IDXGIOutput1*, DXGI_FORMAT, UINT, UINT*, DXGI_MODE_DESC1*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_FindClosestMatchingMode)(IDXGIOutput*, const DXGI_MODE_DESC*, DXGI_MODE_DESC*, IUnknown*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateSwapChain)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*,
														IDXGISwapChain**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_SetFullscreenState)(IDXGISwapChain*, BOOL, IDXGIOutput*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_ResizeTarget)(IDXGISwapChain*, const DXGI_MODE_DESC*);
typedef BOOL(WINAPI* PFN_EnumDisplaySettingsW)(LPCWSTR, DWORD, DEVMODEW*);
typedef BOOL(WINAPI* PFN_EnumDisplaySettingsA)(LPCSTR, DWORD, DEVMODEA*);
typedef BOOL(WINAPI* PFN_EnumDisplaySettingsExW)(LPCWSTR, DWORD, DEVMODEW*, DWORD);
typedef BOOL(WINAPI* PFN_EnumDisplaySettingsExA)(LPCSTR, DWORD, DEVMODEA*, DWORD);
typedef BOOL(WINAPI* PFN_SetWindowPos)(HWND, HWND, int, int, int, int, UINT);
typedef BOOL(WINAPI* PFN_MoveWindow)(HWND, int, int, int, int, BOOL);

PFN_CreateDXGIFactory o_CreateDXGIFactory, o_CreateDXGIFactory1;
PFN_CreateDXGIFactory2 o_CreateDXGIFactory2;
PFN_GetDisplayModeList o_GetDisplayModeList;
PFN_GetDisplayModeList1 o_GetDisplayModeList1;
PFN_FindClosestMatchingMode o_FindClosestMatchingMode;
PFN_CreateSwapChain o_CreateSwapChain;
PFN_SetFullscreenState o_SetFullscreenState;
PFN_ResizeTarget o_ResizeTarget;
PFN_EnumDisplaySettingsW o_EnumDisplaySettingsW;
PFN_EnumDisplaySettingsA o_EnumDisplaySettingsA;
PFN_EnumDisplaySettingsExW o_EnumDisplaySettingsExW;
PFN_EnumDisplaySettingsExA o_EnumDisplaySettingsExA;
PFN_SetWindowPos o_SetWindowPos;
PFN_MoveWindow o_MoveWindow;

HWND s_gameWindow;
uintptr_t s_exeBase, s_exeEnd;
UINT s_spoofedDesktopW, s_spoofedDesktopH;  // desktop size reported to the game (0 = not spoofing)
bool s_dxgiLoadedEarly;                     // dxgi.dll was already loaded when the mod started (factory hooks in place early)
int32_t s_earlyHookCount;                   // user32/dxgi hooks installed at load time
// LogBudget counters: each message is logged only the first few times
std::atomic<int32_t> s_factoryLogs, s_systemMetricsLogs, s_monitorInfoLogs, s_currentModeLogs, s_userModeListLogs;
std::atomic<int32_t> s_dxgiModeListLogs, s_findClosestLogs, s_swapChainLogs, s_fullscreenLogs, s_resizeTargetLogs, s_windowClampLogs;

// The extra sizes: the headset's own shape, one per upscaler mode (pure::headset_sizes), measured
// against the picture the headset is actually sent (its full detail x [Upscaler] Output).
constexpr int32_t kMaxExtraSizes = pure::kHeadsetScaleCount + 2;  // the headset's sizes, the game's own and the panel's

// The headset sizes offered this session (the ini can change while the game runs; the game picked
// from these).
struct HeadsetSizes {
	uint32_t width[kMaxExtraSizes], height[kMaxExtraSizes];
	int32_t count;
} s_headsetSizes;

bool CalledFromGame(void* returnAddress) { return (uintptr_t)returnAddress >= s_exeBase && (uintptr_t)returnAddress < s_exeEnd; }

// The monitor the window is on (the primary one for nullptr), in desktop coordinates.
struct MonitorArea {
	int32_t x, y, width, height;
};
MonitorArea GetMonitorArea(HWND hwnd) {
	HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
	MONITORINFO info = {};
	info.cbSize = sizeof(info);
	if (monitor && GetMonitorInfoW(monitor, &info))
		return {info.rcMonitor.left, info.rcMonitor.top, info.rcMonitor.right - info.rcMonitor.left,
				info.rcMonitor.bottom - info.rcMonitor.top};
	return {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
}

bool LargerThanMonitor(HWND hwnd, UINT width, UINT height) {
	MonitorArea monitor = GetMonitorArea(hwnd);
	return (int)width > monitor.width || (int)height > monitor.height;
}

bool IsHeadsetSize(UINT width, UINT height) {
	for (int i = 0; i < s_headsetSizes.count; i++)
		if (s_headsetSizes.width[i] == width && s_headsetSizes.height[i] == height) return true;
	return false;
}

// A size only the mod offers (the headset's shape, or anything above the monitor): the monitor can't
// show it as a display mode, so it stays windowed and isn't snapped to a real mode.
bool IsExtraSize(HWND hwnd, UINT width, UINT height) { return LargerThanMonitor(hwnd, width, height) || IsHeadsetSize(width, height); }

// The headset sizes the list doesn't have yet, plus the size the game itself is set to.
// The headset's full detail changes when the lens does, so the sizes worked out from it move: without
// the game's own size in here, a game whose saved resolution came from an earlier list cannot find it
// any more. NieR reads the missing entry as -1 and dies on the title screens (2026-10-27).
int HeadsetSizesNotListed(const uint32_t* listed, int listedCount, uint32_t* extraW, uint32_t* extraH) {
	int count = pure::headset_sizes((uint32_t)g_cfg.headsetWidth, (uint32_t)g_cfg.headsetHeight, listed, listedCount, extraW, extraH);
	auto add = [&](uint32_t w, uint32_t h) {
		if (w < 640 || h < 480 || count >= kMaxExtraSizes) return;
		uint32_t key = (w & 0xffff) << 16 | (h & 0xffff);
		for (int i = 0; i < listedCount; i++)
			if (listed[i] == key) return;
		for (int i = 0; i < count; i++)
			if (extraW[i] == w && extraH[i] == h) return;
		extraW[count] = w;
		extraH[count] = h;
		count++;
	};
	add((uint32_t)g_cfg.gameWidth, (uint32_t)g_cfg.gameHeight);  // what the game has saved
	uint32_t panelW, panelH;
	if (GameResolutionPanelSize(&panelW, &panelH)) add(panelW, panelH);  // what the panel has just written for it
	return count;
}

// --- DXGI mode list --------------------------------------------------------------------------
// The output's current desktop size (the monitor's own resolution, whatever DSR modes it also lists).
void OutputDesktopSize(IDXGIOutput* output, UINT* width, UINT* height) {
	DXGI_OUTPUT_DESC desc;
	if (output && SUCCEEDED(output->GetDesc(&desc))) {
		int32_t desktopW = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left, desktopH = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
		if (desc.Rotation == DXGI_MODE_ROTATION_ROTATE90 || desc.Rotation == DXGI_MODE_ROTATION_ROTATE270) {
			int32_t swap = desktopW;
			desktopW = desktopH;
			desktopH = swap;
		}
		if (desktopW > 0 && desktopH > 0) {
			*width = (UINT)desktopW;
			*height = (UINT)desktopH;
		}
	}
}
// The headset sizes follow the real list, copied from its desktop-size mode (desktopW x desktopH; 0 =
// unknown, then the largest mode) for the format and refresh rate. Sizes the list already has are not
// added twice.
template <typename DESC, typename ORIG>
HRESULT ModeListWithExtras(const char* api, void* returnAddress, ORIG getModes, DXGI_FORMAT format, UINT flags, UINT* count, DESC* modes,
						   UINT desktopW, UINT desktopH) {
	bool fromGame = CalledFromGame(returnAddress);
	if (!g_cfg.extraResolutions || !count || !fromGame) {
		HRESULT hr = getModes(count, modes);
		if (LogBudget(s_dxgiModeListLogs, 8)) {
			char caller[300];
			DescribeAddress(returnAddress, caller, sizeof(caller));
			Log("display: %s from %s (format %d, flags 0x%x, %s) -> %u modes, 0x%08x", api, caller, (int)format, flags,
				modes ? "list" : "count", count ? *count : 0, (unsigned)hr);
		}
		return hr;
	}
	UINT nativeCount = 0;
	HRESULT hr = getModes(&nativeCount, (DESC*)nullptr);
	if (FAILED(hr) || nativeCount == 0) return getModes(count, modes);
	DESC* all = (DESC*)HeapAlloc(GetProcessHeap(), 0, (nativeCount + kMaxExtraSizes) * sizeof(DESC));
	if (!all) return getModes(count, modes);
	hr = getModes(&nativeCount, all);
	if (FAILED(hr)) {
		HeapFree(GetProcessHeap(), 0, all);
		return getModes(count, modes);
	}
	// the base: the desktop-size mode at its highest refresh rate (else the largest mode)
	auto refreshHz = [](const DESC& d) {
		return d.RefreshRate.Denominator ? (double)d.RefreshRate.Numerator / d.RefreshRate.Denominator : 0.0;
	};
	UINT base = 0, largest = 0;
	bool haveBase = false;
	for (UINT i = 0; i < nativeCount; i++) {
		uint64_t areaI = (uint64_t)all[i].Width * all[i].Height, areaLargest = (uint64_t)all[largest].Width * all[largest].Height;
		if (areaI > areaLargest || (areaI == areaLargest && refreshHz(all[i]) > refreshHz(all[largest]))) largest = i;
		if (desktopW && all[i].Width == desktopW && all[i].Height == desktopH && (!haveBase || refreshHz(all[i]) > refreshHz(all[base]))) {
			base = i;
			haveBase = true;
		}
	}
	if (!haveBase) base = largest;
	UINT baseW = all[base].Width, baseH = all[base].Height;  // logged
	uint32_t* listedSizes = (uint32_t*)HeapAlloc(GetProcessHeap(), 0, nativeCount * sizeof(uint32_t));
	uint32_t extraW[kMaxExtraSizes], extraH[kMaxExtraSizes];
	int extraCount = 0;
	if (listedSizes) {
		for (UINT i = 0; i < nativeCount; i++) listedSizes[i] = (all[i].Width & 0xffff) << 16 | (all[i].Height & 0xffff);
		extraCount = HeadsetSizesNotListed(listedSizes, (int)nativeCount, extraW, extraH);
		HeapFree(GetProcessHeap(), 0, listedSizes);
	}
	for (int k = 0; k < extraCount; k++) {
		all[nativeCount + k] = all[base];
		all[nativeCount + k].Width = extraW[k];
		all[nativeCount + k].Height = extraH[k];
	}
	UINT total = nativeCount + extraCount;
	if (LogBudget(s_dxgiModeListLogs, 8)) {
		char caller[300];
		DescribeAddress(returnAddress, caller, sizeof(caller));
		Log("display: %s from %s (format %d, %s): %u native modes up to %ux%u (base %ux%u, %s) + %d headset-shaped",
			api, caller, (int)format, modes ? "list" : "count", nativeCount, all[largest].Width, all[largest].Height, baseW, baseH,
			haveBase ? "desktop size" : "largest mode", extraCount);
	}
	if (!modes) {
		*count = total;
		HeapFree(GetProcessHeap(), 0, all);
		return S_OK;
	}
	UINT room = *count, copied = room < total ? room : total;
	memcpy(modes, all, copied * sizeof(DESC));
	*count = copied;
	HeapFree(GetProcessHeap(), 0, all);
	return room < total ? DXGI_ERROR_MORE_DATA : S_OK;
}

HRESULT STDMETHODCALLTYPE H_GetDisplayModeList(IDXGIOutput* output, DXGI_FORMAT format, UINT flags, UINT* count,
											   DXGI_MODE_DESC* modes) {
	UINT desktopW = 0, desktopH = 0;
	OutputDesktopSize(output, &desktopW, &desktopH);
	return ModeListWithExtras("GetDisplayModeList", __builtin_return_address(0), [&](UINT* listCount, DXGI_MODE_DESC* list) { return o_GetDisplayModeList(output, format, flags, listCount, list); }, format, flags, count, modes, desktopW, desktopH);
}
HRESULT STDMETHODCALLTYPE H_GetDisplayModeList1(IDXGIOutput1* output, DXGI_FORMAT format, UINT flags, UINT* count,
												DXGI_MODE_DESC1* modes) {
	UINT desktopW = 0, desktopH = 0;
	OutputDesktopSize(output, &desktopW, &desktopH);
	return ModeListWithExtras("GetDisplayModeList1", __builtin_return_address(0), [&](UINT* listCount, DXGI_MODE_DESC1* list) { return o_GetDisplayModeList1(output, format, flags, listCount, list); }, format, flags, count, modes, desktopW, desktopH);
}

HRESULT STDMETHODCALLTYPE H_FindClosestMatchingMode(IDXGIOutput* output, const DXGI_MODE_DESC* wanted, DXGI_MODE_DESC* closest,
													IUnknown* device) {
	HRESULT hr = o_FindClosestMatchingMode(output, wanted, closest, device);
	if (g_cfg.extraResolutions && wanted && closest && SUCCEEDED(hr) && wanted->Width && wanted->Height &&
		IsExtraSize(nullptr, wanted->Width, wanted->Height)) {
		// keep the requested size instead of snapping to a native mode
		DXGI_MODE_DESC mode = *closest;
		mode.Width = wanted->Width;
		mode.Height = wanted->Height;
		*closest = mode;
	}
	if (LogBudget(s_findClosestLogs, 6))
		Log("display: FindClosestMatchingMode %ux%u -> %ux%u", wanted ? wanted->Width : 0, wanted ? wanted->Height : 0,
			closest ? closest->Width : 0, closest ? closest->Height : 0);
	return hr;
}

// --- swapchain -------------------------------------------------------------------------------
}  // namespace

// Remembered whenever the game picks a size, so the next run still offers it.
void RememberGameSize(UINT width, UINT height) {
	if (width < 640 || height < 480 || ((int32_t)width == g_cfg.gameWidth && (int32_t)height == g_cfg.gameHeight)) return;
	g_cfg.gameWidth = (int32_t)width;
	g_cfg.gameHeight = (int32_t)height;
	ConfigSave(&g_cfg.gameWidth);
	ConfigSave(&g_cfg.gameHeight);
	Log("display: the game is rendering %ux%u; its own list will keep offering that size", width, height);
}

namespace {

HRESULT STDMETHODCALLTYPE H_CreateSwapChain(IDXGIFactory* factory, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc,
											IDXGISwapChain** swapChain) {
	if (desc && desc->BufferDesc.Width >= 640) {
		s_gameWindow = desc->OutputWindow;
		RememberGameSize(desc->BufferDesc.Width, desc->BufferDesc.Height);
		if (g_cfg.extraResolutions && !desc->Windowed &&
			IsExtraSize(desc->OutputWindow, desc->BufferDesc.Width, desc->BufferDesc.Height)) {
			Log("display: %ux%u is not one of the monitor's modes - creating the swapchain windowed instead of "
				"exclusive fullscreen",
				desc->BufferDesc.Width, desc->BufferDesc.Height);
			desc->Windowed = TRUE;
		}
	}
	HRESULT hr = o_CreateSwapChain(factory, device, desc, swapChain);
	if (desc && LogBudget(s_swapChainLogs, 8))
		Log("display: game created swapchain %ux%u format %d, %s, effect %d, %u buffers, flags 0x%x -> 0x%08x",
			desc->BufferDesc.Width, desc->BufferDesc.Height, (int)desc->BufferDesc.Format,
			desc->Windowed ? "windowed" : "fullscreen", (int)desc->SwapEffect, desc->BufferCount, desc->Flags,
			(unsigned)hr);
	return hr;
}

HRESULT STDMETHODCALLTYPE H_SetFullscreenState(IDXGISwapChain* swapChain, BOOL fullscreen, IDXGIOutput* target) {
	if (fullscreen && g_cfg.extraResolutions) {
		DXGI_SWAP_CHAIN_DESC desc;
		if (SUCCEEDED(swapChain->GetDesc(&desc)) && IsExtraSize(desc.OutputWindow, desc.BufferDesc.Width, desc.BufferDesc.Height)) {
			Log("display: refusing exclusive fullscreen at %ux%u (not one of the monitor's modes); staying windowed",
				desc.BufferDesc.Width, desc.BufferDesc.Height);
			o_SetFullscreenState(swapChain, FALSE, nullptr);
			return S_OK;
		}
	}
	HRESULT hr = o_SetFullscreenState(swapChain, fullscreen, target);
	if (LogBudget(s_fullscreenLogs, 8)) Log("display: SetFullscreenState(%d) -> 0x%08x", (int)fullscreen, (unsigned)hr);
	return hr;
}

HRESULT STDMETHODCALLTYPE H_ResizeTarget(IDXGISwapChain* swapChain, const DXGI_MODE_DESC* mode) {
	DXGI_MODE_DESC clamped;
	if (mode && g_cfg.extraResolutions) {
		DXGI_SWAP_CHAIN_DESC desc;
		HWND window = SUCCEEDED(swapChain->GetDesc(&desc)) ? desc.OutputWindow : nullptr;
		if (IsExtraSize(window, mode->Width, mode->Height)) {
			MonitorArea monitor = GetMonitorArea(window);
			clamped = *mode;
			clamped.Width = (UINT)monitor.width;
			clamped.Height = (UINT)monitor.height;
			mode = &clamped;
		}
	}
	HRESULT hr = o_ResizeTarget(swapChain, mode);
	if (LogBudget(s_resizeTargetLogs, 8))
		Log("display: ResizeTarget %ux%u -> 0x%08x", mode ? mode->Width : 0, mode ? mode->Height : 0, (unsigned)hr);
	return hr;
}

// --- user32 mode list -------------------------------------------------------------------------
template <typename DEVMODE_T>
struct EnumState {
	DEVMODE_T base;  // the extra sizes are copies of it: the current (desktop) mode, else the largest
	bool haveBase;
	bool baseIsCurrent;         // base is the device's current mode
	DWORD firstExtraIndex;      // index of the first call past the real list
	uint32_t listedSizes[512];  // listed sizes (w << 16 | h), so extra sizes aren't listed twice
	int32_t listedCount;
	UINT extraW[kMaxExtraSizes], extraH[kMaxExtraSizes];  // the extra sizes not already listed (made when the real list ends)
	int32_t extraCount;
};
EnumState<DEVMODEW> s_enumW;
EnumState<DEVMODEA> s_enumA;

template <typename DEVMODE_T, typename F, typename C>
BOOL EnumWithExtras(EnumState<DEVMODE_T>& state, const char* api, DWORD index, DEVMODE_T* mode, void* returnAddress, F enumOriginal, C getCurrent) {
	bool fromGame = CalledFromGame(returnAddress);
	if ((int32_t)index < 0) {  // ENUM_CURRENT_SETTINGS / ENUM_REGISTRY_SETTINGS
		BOOL ok = enumOriginal();
		bool spoof = ok && mode && fromGame && s_spoofedDesktopW && (mode->dmFields & DM_PELSWIDTH);
		UINT realW = ok && mode ? mode->dmPelsWidth : 0, realH = ok && mode ? mode->dmPelsHeight : 0;
		if (spoof) {
			mode->dmPelsWidth = s_spoofedDesktopW;
			mode->dmPelsHeight = s_spoofedDesktopH;
		}
		if (LogBudget(s_currentModeLogs, 6)) {
			char caller[300];
			DescribeAddress(returnAddress, caller, sizeof(caller));
			Log("display: %s(%s) from %s -> %ux%u @%u Hz%s", api, index == ENUM_CURRENT_SETTINGS ? "current" : "registry",
				caller, realW, realH, ok && mode ? (unsigned)mode->dmDisplayFrequency : 0,
				spoof ? " (reported to the game as large enough for the headset sizes)" : "");
		}
		if (spoof && index == ENUM_CURRENT_SETTINGS) {
			static bool loggedSpoof;
			if (!loggedSpoof) {
				loggedSpoof = true;
				Log("display: telling the game the desktop is %ux%u so it offers the headset sizes above %ux%u", s_spoofedDesktopW,
					s_spoofedDesktopH, realW, realH);
			}
		}
		return ok;
	}
	if (index == 0) {
		state.haveBase = state.baseIsCurrent = false;
		state.firstExtraIndex = 0;
		state.listedCount = 0;
		state.extraCount = 0;
		DEVMODE_T current = {};
		current.dmSize = sizeof(current);
		if (getCurrent(&current) && current.dmPelsWidth && current.dmPelsHeight) {
			state.base = current;
			state.haveBase = state.baseIsCurrent = true;
		}
	}
	BOOL ok = enumOriginal();
	if (ok && mode) {
		if (state.listedCount < 512) state.listedSizes[state.listedCount++] = (mode->dmPelsWidth & 0xffff) << 16 | (mode->dmPelsHeight & 0xffff);
		if (!state.baseIsCurrent &&
			(!state.haveBase ||
			 (uint64_t)mode->dmPelsWidth * mode->dmPelsHeight > (uint64_t)state.base.dmPelsWidth * state.base.dmPelsHeight ||
			 (mode->dmPelsWidth == state.base.dmPelsWidth && mode->dmPelsHeight == state.base.dmPelsHeight &&
			  mode->dmDisplayFrequency > state.base.dmDisplayFrequency))) {
			state.base = *mode;
			state.haveBase = true;
		}
		return ok;
	}
	// end of the real list: the extra sizes it doesn't have yet follow it
	bool addExtras = g_cfg.extraResolutions && state.haveBase && mode && fromGame;
	if (!state.firstExtraIndex) {
		state.firstExtraIndex = index;
		uint32_t extraW[kMaxExtraSizes], extraH[kMaxExtraSizes];
		state.extraCount = state.haveBase ? HeadsetSizesNotListed(state.listedSizes, state.listedCount, extraW, extraH) : 0;
		for (int extraIndex = 0; extraIndex < state.extraCount; extraIndex++) {
			state.extraW[extraIndex] = extraW[extraIndex];
			state.extraH[extraIndex] = extraH[extraIndex];
		}
	}
	DWORD extraIndex = index - state.firstExtraIndex;
	if (addExtras && extraIndex < (DWORD)state.extraCount) {
		WORD keepSize = mode->dmSize;
		*mode = state.base;
		mode->dmSize = keepSize;
		mode->dmPelsWidth = state.extraW[extraIndex];
		mode->dmPelsHeight = state.extraH[extraIndex];
		return TRUE;
	}
	if (LogBudget(s_userModeListLogs, 4)) {
		char caller[300];
		DescribeAddress(returnAddress, caller, sizeof(caller));
		Log("display: %s list from %s: %u modes (base %ux%u, %s), %d headset-shaped", api, caller, (unsigned)state.firstExtraIndex,
			state.haveBase ? (unsigned)state.base.dmPelsWidth : 0, state.haveBase ? (unsigned)state.base.dmPelsHeight : 0,
			state.baseIsCurrent ? "desktop size" : "largest mode", addExtras ? state.extraCount : 0);
	}
	return FALSE;
}

// Hooks installed at load time see calls from everywhere, possibly before the mod has read its
// settings. Calls from the game's own code initialise the mod first (they never run under the
// loader lock); anything else passes straight through until then.
bool ModReady(void* returnAddress) {
	if (g_initDone) return true;
	if (!CalledFromGame(returnAddress)) return false;
	EnsureInit();
	return g_initDone;
}

BOOL WINAPI H_EnumDisplaySettingsW(LPCWSTR device, DWORD index, DEVMODEW* mode) {
	void* returnAddress = __builtin_return_address(0);
	if (!ModReady(returnAddress)) return o_EnumDisplaySettingsW(device, index, mode);
	return EnumWithExtras(s_enumW, "EnumDisplaySettingsW", index, mode, returnAddress, [&] { return o_EnumDisplaySettingsW(device, index, mode); }, [&](DEVMODEW* current) { return o_EnumDisplaySettingsW(device, ENUM_CURRENT_SETTINGS, current); });
}
BOOL WINAPI H_EnumDisplaySettingsA(LPCSTR device, DWORD index, DEVMODEA* mode) {
	void* returnAddress = __builtin_return_address(0);
	if (!ModReady(returnAddress)) return o_EnumDisplaySettingsA(device, index, mode);
	return EnumWithExtras(s_enumA, "EnumDisplaySettingsA", index, mode, returnAddress, [&] { return o_EnumDisplaySettingsA(device, index, mode); }, [&](DEVMODEA* current) { return o_EnumDisplaySettingsA(device, ENUM_CURRENT_SETTINGS, current); });
}
BOOL WINAPI H_EnumDisplaySettingsExW(LPCWSTR device, DWORD index, DEVMODEW* mode, DWORD flags) {
	void* returnAddress = __builtin_return_address(0);
	if (!ModReady(returnAddress)) return o_EnumDisplaySettingsExW(device, index, mode, flags);
	return EnumWithExtras(s_enumW, "EnumDisplaySettingsExW", index, mode, returnAddress, [&] { return o_EnumDisplaySettingsExW(device, index, mode, flags); }, [&](DEVMODEW* current) { return o_EnumDisplaySettingsExW(device, ENUM_CURRENT_SETTINGS, current, 0); });
}
BOOL WINAPI H_EnumDisplaySettingsExA(LPCSTR device, DWORD index, DEVMODEA* mode, DWORD flags) {
	void* returnAddress = __builtin_return_address(0);
	if (!ModReady(returnAddress)) return o_EnumDisplaySettingsExA(device, index, mode, flags);
	return EnumWithExtras(s_enumA, "EnumDisplaySettingsExA", index, mode, returnAddress, [&] { return o_EnumDisplaySettingsExA(device, index, mode, flags); }, [&](DEVMODEA* current) { return o_EnumDisplaySettingsExA(device, ENUM_CURRENT_SETTINGS, current, 0); });
}

// DXGI factories made by the game: initialise the mod first, so the mode-list hooks exist before
// the game asks for its resolution list (NieR does that before creating its D3D11 device).
void OnFactoryCreated(const char* api, void* returnAddress) {
	if (!CalledFromGame(returnAddress)) return;
	EnsureInit();
	if (LogBudget(s_factoryLogs, 4)) {
		char caller[300];
		DescribeAddress(returnAddress, caller, sizeof(caller));
		Log("display: game created a DXGI factory (%s) from %s", api, caller);
	}
}
HRESULT WINAPI H_CreateDXGIFactory(REFIID riid, void** factory) {
	OnFactoryCreated("CreateDXGIFactory", __builtin_return_address(0));
	return o_CreateDXGIFactory(riid, factory);
}
HRESULT WINAPI H_CreateDXGIFactory1(REFIID riid, void** factory) {
	OnFactoryCreated("CreateDXGIFactory1", __builtin_return_address(0));
	return o_CreateDXGIFactory1(riid, factory);
}
HRESULT WINAPI H_CreateDXGIFactory2(UINT flags, REFIID riid, void** factory) {
	OnFactoryCreated("CreateDXGIFactory2", __builtin_return_address(0));
	return o_CreateDXGIFactory2(flags, riid, factory);
}

// Logged only (to see whether the game also sizes things from these).
typedef int(WINAPI* PFN_GetSystemMetrics)(int);
typedef BOOL(WINAPI* PFN_GetMonitorInfoW)(HMONITOR, LPMONITORINFO);
PFN_GetSystemMetrics o_GetSystemMetrics;
PFN_GetMonitorInfoW o_GetMonitorInfoW;
int WINAPI H_GetSystemMetrics(int index) {
	void* returnAddress = __builtin_return_address(0);
	if (!ModReady(returnAddress)) return o_GetSystemMetrics(index);
	int value = o_GetSystemMetrics(index);
	if ((index == SM_CXSCREEN || index == SM_CYSCREEN || index == SM_CXVIRTUALSCREEN || index == SM_CYVIRTUALSCREEN ||
		 index == SM_CXFULLSCREEN || index == SM_CYFULLSCREEN) &&
		CalledFromGame(returnAddress) && LogBudget(s_systemMetricsLogs, 8)) {
		char caller[300];
		DescribeAddress(returnAddress, caller, sizeof(caller));
		Log("display: GetSystemMetrics(%d) from %s -> %d", index, caller, value);
	}
	return value;
}
BOOL WINAPI H_GetMonitorInfoW(HMONITOR monitor, LPMONITORINFO info) {
	void* returnAddress = __builtin_return_address(0);
	if (!ModReady(returnAddress)) return o_GetMonitorInfoW(monitor, info);
	BOOL ok = o_GetMonitorInfoW(monitor, info);
	if (ok && info && CalledFromGame(returnAddress) && LogBudget(s_monitorInfoLogs, 6)) {
		char caller[300];
		DescribeAddress(returnAddress, caller, sizeof(caller));
		Log("display: GetMonitorInfoW from %s -> %dx%d", caller, (int)(info->rcMonitor.right - info->rcMonitor.left),
			(int)(info->rcMonitor.bottom - info->rcMonitor.top));
	}
	return ok;
}

// --- the game's built-in resolution table -----------------------------------------------------
// Looks for runs of (width, height) int32 pairs with a fixed stride in the exe image, e.g.
// {1280,720},{1600,900},{1920,1080},... Logged always; with ExtraResolutions=2 the smallest entries
// of the single convincing table are replaced by the headset sizes.
bool PlausibleResolution(int32_t width, int32_t height) {
	return width >= 640 && width <= 16384 && height >= 360 && height <= 8640 && width > height && width * 10 <= height * 36 && width * 10 >= height * 12 &&
		   (width % 8) == 0 && (height % 2) == 0;
}

bool Readable(uintptr_t addr, size_t len) {
	MEMORY_BASIC_INFORMATION mbi;
	uintptr_t end = addr + len;
	while (addr < end) {
		if (!VirtualQuery((void*)addr, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
			(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
			!(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
							 PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
			return false;
		addr = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
	}
	return true;
}

struct ResolutionTable {
	uintptr_t addr;
	int32_t stride, count;
	bool has720, has1080, has2160;
};

int ScanResolutionTables(ResolutionTable* out, int cap) {
	if (!s_exeBase) return 0;
	auto* dos = (IMAGE_DOS_HEADER*)s_exeBase;
	auto* nt = (IMAGE_NT_HEADERS64*)(s_exeBase + dos->e_lfanew);
	auto* sections = IMAGE_FIRST_SECTION(nt);
	int found = 0;
	static const int strides[] = {8, 12, 16, 20, 24, 32};
	for (int sectionIndex = 0; sectionIndex < nt->FileHeader.NumberOfSections; sectionIndex++) {
		if (sections[sectionIndex].Characteristics & IMAGE_SCN_MEM_EXECUTE) continue;  // code
		if (!(sections[sectionIndex].Characteristics & IMAGE_SCN_MEM_READ)) continue;
		uintptr_t start = s_exeBase + sections[sectionIndex].VirtualAddress, end = start + sections[sectionIndex].Misc.VirtualSize;
		if (!Readable(start, end - start)) continue;
		for (uintptr_t at = start; at + 8 <= end; at += 4) {
			const int32_t* pair = (const int32_t*)at;
			if (!PlausibleResolution(pair[0], pair[1])) continue;
			for (int stride : strides) {
				int count = 0;
				bool has720 = false, has1080 = false, has2160 = false;
				int32_t prevW = 0, prevH = 0;
				bool distinct = true;
				while (at + (uintptr_t)count * stride + 8 <= end) {
					const int32_t* entry = (const int32_t*)(at + (uintptr_t)count * stride);
					if (!PlausibleResolution(entry[0], entry[1])) break;
					if (entry[0] == prevW && entry[1] == prevH) distinct = false;
					prevW = entry[0];
					prevH = entry[1];
					has720 |= entry[0] == 1280 && entry[1] == 720;
					has1080 |= entry[0] == 1920 && entry[1] == 1080;
					has2160 |= entry[0] == 3840 && entry[1] == 2160;
					count++;
				}
				if (count >= 4 && distinct && (has720 || has1080) && found < cap) {
					out[found++] = {at, stride, count, has720, has1080, has2160};
					at += (uintptr_t)(count - 1) * stride;  // skip past this table
					break;
				}
			}
		}
	}
	return found;
}

void LogAndPatchTables(bool patch) {
	ResolutionTable tables[12];
	int count = ScanResolutionTables(tables, 12);
	if (!count) {
		Log("display: no built-in resolution table found in the game exe");
		return;
	}
	int chosen = -1, convincingCount = 0;
	for (int i = 0; i < count; i++) {
		char sizes[900];
		size_t used = 0;
		sizes[0] = 0;
		for (int k = 0; k < tables[i].count && used + 24 < sizeof(sizes); k++) {
			const int32_t* entry = (const int32_t*)(tables[i].addr + (uintptr_t)k * tables[i].stride);
			int written = snprintf(sizes + used, sizeof(sizes) - used, "%s%dx%d", k ? " " : "", entry[0], entry[1]);
			used += written > 0 ? (size_t)written : 0;
		}
		Log("display: resolution table candidate at NieRAutomata.exe+0x%llx (stride %d, %d entries): %s",
			(uint64_t)(tables[i].addr - s_exeBase), tables[i].stride, tables[i].count, sizes);
		if (tables[i].has1080 && tables[i].count >= 5) {
			chosen = i;
			convincingCount++;
		}
	}
	if (!patch) return;
	if (convincingCount != 1) {
		Log("display: ExtraResolutions=2: %d convincing tables, not patching anything (send me this log)", convincingCount);
		return;
	}
	// replace the smallest entries, keeping 1600x900 / 1920x1080 / the monitor size / anything bigger
	MonitorArea monitor = GetMonitorArea(nullptr);
	int mw = monitor.width, mh = monitor.height;
	if (!s_headsetSizes.count) return;
	ResolutionTable& table = tables[chosen];
	DWORD oldProtect;
	if (!VirtualProtect((void*)table.addr, (SIZE_T)table.count * table.stride, PAGE_READWRITE, &oldProtect)) {
		Log("display: could not unprotect the resolution table (%u)", (unsigned)GetLastError());
		return;
	}
	int replaced = 0;
	for (int k = 0; k < s_headsetSizes.count; k++) {
		int smallest = -1;
		int64_t smallestArea = 0;
		for (int j = 0; j < table.count; j++) {
			int32_t* entry = (int32_t*)(table.addr + (uintptr_t)j * table.stride);
			if ((entry[0] == 1600 && entry[1] == 900) || (entry[0] == 1920 && entry[1] == 1080) || (entry[0] >= mw && entry[1] >= mh))
				continue;
			int64_t area = (int64_t)entry[0] * entry[1];
			if (smallest < 0 || area < smallestArea) {
				smallest = j;
				smallestArea = area;
			}
		}
		if (smallest < 0) break;
		int32_t* entry = (int32_t*)(table.addr + (uintptr_t)smallest * table.stride);
		int32_t newW = (int32_t)s_headsetSizes.width[k], newH = (int32_t)s_headsetSizes.height[k];
		Log("display: resolution table entry %d: %dx%d -> %dx%d", smallest, entry[0], entry[1], newW, newH);
		entry[0] = newW;
		entry[1] = newH;
		replaced++;
	}
	VirtualProtect((void*)table.addr, (SIZE_T)table.count * table.stride, oldProtect, &oldProtect);
	FlushInstructionCache(GetCurrentProcess(), (void*)table.addr, (SIZE_T)table.count * table.stride);
	Log("display: ExtraResolutions=2: %d entries of the game's resolution table replaced", replaced);
}

// --- keep the game window on the monitor when rendering above its resolution ----------------
bool IsGameTopLevel(HWND hwnd) {
	if (!hwnd) return false;
	if (hwnd == s_gameWindow) return true;
	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	return pid == GetCurrentProcessId() && !(GetWindowLongW(hwnd, GWL_STYLE) & WS_CHILD);
}

bool ClampWindowToMonitor(HWND hwnd, int* x, int* y, int* cx, int* cy) {
	if (!g_cfg.extraResolutions || !IsGameTopLevel(hwnd)) return false;
	MonitorArea monitor = GetMonitorArea(hwnd);
	// a window a little larger than the monitor (windowed mode at the monitor's size, with its
	// borders) is normal; only supersampling sizes (125%+) are pulled back
	if (*cx * 10 <= monitor.width * 11 && *cy * 10 <= monitor.height * 11) return false;
	if (LogBudget(s_windowClampLogs, 4))
		Log("display: keeping the game window at the monitor size (%dx%d instead of %dx%d)", monitor.width, monitor.height, *cx,
			*cy);
	*x = monitor.x;
	*y = monitor.y;
	*cx = monitor.width;
	*cy = monitor.height;
	return true;
}

BOOL WINAPI H_SetWindowPos(HWND hwnd, HWND after, int x, int y, int cx, int cy, UINT flags) {
	if (!(flags & SWP_NOSIZE) && ClampWindowToMonitor(hwnd, &x, &y, &cx, &cy)) flags &= ~SWP_NOMOVE;
	return o_SetWindowPos(hwnd, after, x, y, cx, cy, flags);
}
BOOL WINAPI H_MoveWindow(HWND hwnd, int x, int y, int cx, int cy, BOOL repaint) {
	ClampWindowToMonitor(hwnd, &x, &y, &cx, &cy);
	return o_MoveWindow(hwnd, x, y, cx, cy, repaint);
}

void HookExport(const wchar_t* dll, const char* name, void* detour, void* original) {
	HMODULE module = GetModuleHandleW(dll);
	if (!module) module = LoadLibraryW(dll);
	void* target = module ? (void*)GetProcAddress(module, name) : nullptr;
	if (target) HookFunction(target, detour, (void**)original, name);
}
}  // namespace

// At load time (DllMain): hooks that must exist before the game's first display queries. Only
// patches code (no calls into other DLLs), so it is safe under the loader lock.
void ExtraResolutionsEarlyHooks() {
	HMODULE exe = GetModuleHandleW(nullptr);
	auto* dos = (IMAGE_DOS_HEADER*)exe;
	auto* nt = (IMAGE_NT_HEADERS64*)((char*)exe + dos->e_lfanew);
	s_exeBase = (uintptr_t)exe;
	s_exeEnd = s_exeBase + nt->OptionalHeader.SizeOfImage;
	struct ExportHook {
		const wchar_t* dll;
		const char* name;
		void* detour;
		void** original;
	} hooks[] = {
		{L"user32.dll", "EnumDisplaySettingsW", (void*)&H_EnumDisplaySettingsW, (void**)&o_EnumDisplaySettingsW},
		{L"user32.dll", "EnumDisplaySettingsA", (void*)&H_EnumDisplaySettingsA, (void**)&o_EnumDisplaySettingsA},
		{L"user32.dll", "EnumDisplaySettingsExW", (void*)&H_EnumDisplaySettingsExW, (void**)&o_EnumDisplaySettingsExW},
		{L"user32.dll", "EnumDisplaySettingsExA", (void*)&H_EnumDisplaySettingsExA, (void**)&o_EnumDisplaySettingsExA},
		{L"user32.dll", "GetSystemMetrics", (void*)&H_GetSystemMetrics, (void**)&o_GetSystemMetrics},
		{L"user32.dll", "GetMonitorInfoW", (void*)&H_GetMonitorInfoW, (void**)&o_GetMonitorInfoW},
		{L"dxgi.dll", "CreateDXGIFactory", (void*)&H_CreateDXGIFactory, (void**)&o_CreateDXGIFactory},
		{L"dxgi.dll", "CreateDXGIFactory1", (void*)&H_CreateDXGIFactory1, (void**)&o_CreateDXGIFactory1},
		{L"dxgi.dll", "CreateDXGIFactory2", (void*)&H_CreateDXGIFactory2, (void**)&o_CreateDXGIFactory2},
	};
	for (auto& hook : hooks) {
		HMODULE module = GetModuleHandleW(hook.dll);  // never loads anything here
		void* target = module ? (void*)GetProcAddress(module, hook.name) : nullptr;
		if (!target) continue;
		if (module == GetModuleHandleW(L"dxgi.dll")) s_dxgiLoadedEarly = true;
		if (HookFunction(target, hook.detour, hook.original, hook.name)) s_earlyHookCount++;
	}
}

// Installed once from EnsureInit (first call into the DLL, outside the loader lock).
void ExtraResolutionsInstallHooks() {
	static std::atomic<bool> installed = false;
	if (installed.exchange(true)) return;
	if (!s_exeBase) ExtraResolutionsEarlyHooks();
	HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
	if (!o_CreateDXGIFactory && dxgi) {  // dxgi wasn't loaded at start: hook the factory functions now
		HookFunction((void*)GetProcAddress(dxgi, "CreateDXGIFactory"), (void*)&H_CreateDXGIFactory,
					 (void**)&o_CreateDXGIFactory, "CreateDXGIFactory");
		HookFunction((void*)GetProcAddress(dxgi, "CreateDXGIFactory1"), (void*)&H_CreateDXGIFactory1,
					 (void**)&o_CreateDXGIFactory1, "CreateDXGIFactory1");
	}
	PFN_CreateDXGIFactory createFactory = o_CreateDXGIFactory ? o_CreateDXGIFactory
										  : dxgi              ? (PFN_CreateDXGIFactory)(void*)GetProcAddress(dxgi, "CreateDXGIFactory")
															  : nullptr;
	ComPtr<IDXGIFactory> factory;
	if (createFactory && SUCCEEDED(createFactory(__uuidof(IDXGIFactory), &factory)) && factory) {
		void** factoryVtable = *(void***)factory.Get();
		HookFunction(factoryVtable[10], (void*)&H_CreateSwapChain, (void**)&o_CreateSwapChain, "IDXGIFactory::CreateSwapChain");
		ComPtr<IDXGIAdapter> adapter;
		ComPtr<IDXGIOutput> output;
		if (SUCCEEDED(factory->EnumAdapters(0, &adapter)) && adapter && SUCCEEDED(adapter->EnumOutputs(0, &output)) && output) {
			void** outputVtable = *(void***)output.Get();
			HookFunction(outputVtable[8], (void*)&H_GetDisplayModeList, (void**)&o_GetDisplayModeList,
						 "IDXGIOutput::GetDisplayModeList");
			HookFunction(outputVtable[9], (void*)&H_FindClosestMatchingMode, (void**)&o_FindClosestMatchingMode, "IDXGIOutput::FindClosestMatchingMode");
			ComPtr<IDXGIOutput1> output1;
			if (SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput1), &output1)) && output1) {
				void** output1Vtable = *(void***)output1.Get();
				HookFunction(output1Vtable[19], (void*)&H_GetDisplayModeList1, (void**)&o_GetDisplayModeList1,
							 "IDXGIOutput1::GetDisplayModeList1");
			}
		} else {
			Log("display: no monitor output found on the first GPU; mode list hooks skipped");
		}
	}
	HookExport(L"user32.dll", "SetWindowPos", (void*)&H_SetWindowPos, &o_SetWindowPos);
	HookExport(L"user32.dll", "MoveWindow", (void*)&H_MoveWindow, &o_MoveWindow);
	MonitorArea monitor = GetMonitorArea(nullptr);
	int mw = monitor.width, mh = monitor.height;
	if (g_cfg.extraResolutions) {
		s_headsetSizes.count = pure::headset_sizes((uint32_t)g_cfg.headsetWidth, (uint32_t)g_cfg.headsetHeight, nullptr, 0,
												   s_headsetSizes.width, s_headsetSizes.height);
		// the game only offers sizes up to the desktop's: report one large enough for the biggest
		for (int i = 0; i < s_headsetSizes.count; i++) {
			UINT width = s_headsetSizes.width[i] > (UINT)mw ? s_headsetSizes.width[i] : (UINT)mw;
			UINT height = s_headsetSizes.height[i] > (UINT)mh ? s_headsetSizes.height[i] : (UINT)mh;
			if (width > s_spoofedDesktopW) s_spoofedDesktopW = width;
			if (height > s_spoofedDesktopH) s_spoofedDesktopH = height;
		}
		if (s_spoofedDesktopW == (UINT)mw && s_spoofedDesktopH == (UINT)mh) s_spoofedDesktopW = s_spoofedDesktopH = 0;  // no need
		char sizes[200] = "";
		size_t used = 0;
		for (int i = 0; i < s_headsetSizes.count && used + 16 < sizeof(sizes); i++)
			used += (size_t)snprintf(sizes + used, sizeof(sizes) - used, "%s%ux%u", i ? " " : "", s_headsetSizes.width[i], s_headsetSizes.height[i]);
		Log("display: headset-shaped resolutions (50-100%% of %dx%d, HeadsetWidth/HeadsetHeight): %s", g_cfg.headsetWidth,
			g_cfg.headsetHeight, sizes);
	}
	Log("display: monitor %dx%d; extra resolutions %s; %d display hooks in place from load time%s", mw, mh,
		g_cfg.extraResolutions >= 2 ? "ON + resolution table patch (headset-shaped, use windowed/borderless)"
		: g_cfg.extraResolutions    ? "ON (headset-shaped, use windowed/borderless)"
									: "off",
		s_earlyHookCount, s_dxgiLoadedEarly ? "" : " (dxgi.dll was not loaded yet at that point)");
	LogAndPatchTables(g_cfg.extraResolutions >= 2);
}

// First Present: remember the game window; log the backbuffer against the monitor.
void ExtraResolutionsOnFirstPresent(IDXGISwapChain* swapChain) {
	DXGI_SWAP_CHAIN_DESC desc;
	if (FAILED(swapChain->GetDesc(&desc))) return;
	s_gameWindow = desc.OutputWindow;
	Log("display: game swapchain %ux%u, %s, swap effect %d, %u buffers", desc.BufferDesc.Width, desc.BufferDesc.Height,
		desc.Windowed ? "windowed/borderless" : "exclusive fullscreen", (int)desc.SwapEffect, desc.BufferCount);
}

// Swapchain methods that change the display mode, hooked through the dummy swapchain made for the Present hook.
void ExtraResolutionsHookSwapChain(IDXGISwapChain* swapChain) {
	static std::atomic<bool> hooked = false;
	if (hooked.exchange(true)) return;
	void** vtable = *(void***)swapChain;
	HookFunction(vtable[10], (void*)&H_SetFullscreenState, (void**)&o_SetFullscreenState, "IDXGISwapChain::SetFullscreenState");
	HookFunction(vtable[14], (void*)&H_ResizeTarget, (void**)&o_ResizeTarget, "IDXGISwapChain::ResizeTarget");
}
