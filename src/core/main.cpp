// main.cpp - DLL entry, one-time initialisation, crash reporting.
#include "core/main.h"
#include "render/game_resolution.h"
#include "core/config.h"
#include "core/d3d11_proxy.h"
#include "core/log.h"
#include "render/culling.h"
#include "game/frame_rate.h"
#include "render/extra_resolutions.h"
#include "render/frame_share.h"
#include "xr/xr_main.h"
#include <atomic>
#include <stdio.h>
#include <dbghelp.h>

HMODULE g_hSelf;
wchar_t g_modDir[MAX_PATH];
wchar_t g_iniPath[MAX_PATH];

namespace {
INIT_ONCE s_initOnce = INIT_ONCE_STATIC_INIT;
}  // namespace
std::atomic<bool> g_initDone;

// Small helpers
void DescribeAddress(void* addr, char* out, size_t cap) {
	HMODULE module = nullptr;
	wchar_t name[MAX_PATH] = L"?";
	if (addr &&
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
						   (LPCWSTR)addr, &module) &&
		module) {
		GetModuleFileNameW(module, name, MAX_PATH);
		const wchar_t* base = name;
		for (const wchar_t* c = name; *c; c++)
			if (*c == L'\\' || *c == L'/') base = c + 1;
		snprintf(out, cap, "%s+0x%llx", Utf8(base).s, (uint64_t)((char*)addr - (char*)module));
	} else {
		snprintf(out, cap, "%p (no module)", addr);
	}
}

// Crash reporting.
// The report is produced on a dedicated thread with its own stack, so it also works for stack
// overflows (where the crashing thread has no stack left to run anything).
typedef BOOL(WINAPI* PFN_MiniDumpWriteDump)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
											PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

namespace {
LPTOP_LEVEL_EXCEPTION_FILTER s_prevFilter;
HANDLE s_crashRequest, s_crashDone;
std::atomic<EXCEPTION_POINTERS*> s_crashPointers;
std::atomic<DWORD> s_crashThread;
std::atomic<bool> s_crashReported;
PFN_MiniDumpWriteDump s_writeDump;
DWORD s_reporterThreadId;

void WriteCrashReport(EXCEPTION_POINTERS* pointers, DWORD threadId) {
	EXCEPTION_RECORD* record = pointers->ExceptionRecord;
	char where[512];
	DescribeAddress(record->ExceptionAddress, where, sizeof(where));
	Log("!!! CRASH: exception 0x%08x at %s, thread %u%s", (unsigned)record->ExceptionCode, where, (unsigned)threadId,
		threadId == g_xrThreadId ? " (VR thread)" : "");
	if (record->ExceptionCode == EXCEPTION_STACK_OVERFLOW) Log("!!! (stack overflow: usually endless recursion)");
	if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2)
		Log("!!! access violation: %s address %p",
			record->ExceptionInformation[0] == 0 ? "reading" : (record->ExceptionInformation[0] == 1 ? "writing" : "executing"),
			(void*)record->ExceptionInformation[1]);
	if (pointers->ContextRecord) {
		CONTEXT* context = pointers->ContextRecord;
		Log("!!! rip=%p rsp=%p rax=%p rcx=%p rdx=%p r8=%p r9=%p", (void*)context->Rip, (void*)context->Rsp, (void*)context->Rax,
			(void*)context->Rcx, (void*)context->Rdx, (void*)context->R8, (void*)context->R9);
		// Code addresses found on the stack: a rough call path without needing symbols.
		void** stack = (void**)context->Rsp;
		int shown = 0;
		for (int i = 0; i < 512 && shown < 16; i++) {
			void* value = nullptr;
			if (!ReadProcessMemory(GetCurrentProcess(), stack + i, &value, sizeof(value), nullptr)) break;
			HMODULE module = nullptr;
			if (value && (uintptr_t)value > 0x10000 &&
				GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
								   (LPCWSTR)value, &module) &&
				module) {
				DescribeAddress(value, where, sizeof(where));
				Log("!!!   stack[%d] %s", i, where);
				shown++;
			}
		}
	}
	wchar_t dumpPath[MAX_PATH];
	lstrcpynW(dumpPath, g_modDir, MAX_PATH - 32);
	lstrcatW(dumpPath, L"NieRVR_crash.dmp");
	HANDLE file = CreateFileW(dumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		Log("!!! cannot create NieRVR_crash.dmp (error %u)", (unsigned)GetLastError());
		return;
	}
	if (s_writeDump) {
		MINIDUMP_EXCEPTION_INFORMATION exceptionInfo;
		exceptionInfo.ThreadId = threadId;
		exceptionInfo.ExceptionPointers = pointers;
		exceptionInfo.ClientPointers = FALSE;
		BOOL ok = s_writeDump(GetCurrentProcess(), GetCurrentProcessId(), file,
							  (MINIDUMP_TYPE)(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithUnloadedModules |
											  MiniDumpWithThreadInfo),
							  &exceptionInfo, nullptr, nullptr);
		Log("!!! minidump %s: NieRVR_crash.dmp", ok ? "written" : "FAILED");
	} else {
		Log("!!! dbghelp.dll unavailable, no minidump");
	}
	CloseHandle(file);
}

DWORD WINAPI CrashReporterThread(void*) {
	while (true) {
		WaitForSingleObject(s_crashRequest, INFINITE);
		if (s_crashPointers) WriteCrashReport(s_crashPointers, s_crashThread);
		SetEvent(s_crashDone);
	}
}

// Uses almost no stack: hands the work to the reporter thread and waits.
void ReportCrash(EXCEPTION_POINTERS* pointers) {
	if (!pointers || !pointers->ExceptionRecord || s_crashReported.exchange(true) != 0) return;
	s_crashPointers = pointers;
	s_crashThread = GetCurrentThreadId();
	if (s_crashRequest && s_crashThread != s_reporterThreadId) {  // the reporter can't wait on itself
		SetEvent(s_crashRequest);
		WaitForSingleObject(s_crashDone, 20000);
	}
}

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* pointers) {
	ReportCrash(pointers);
	return s_prevFilter ? s_prevFilter(pointers) : EXCEPTION_CONTINUE_SEARCH;
}

// Stack overflows are never meant to be survived; catch them first-chance in case the game's
// own handler swallows them before our unhandled-exception filter sees them. Access violations are
// only noted in the log, first-chance: the game's own handler may deal with them (and then kill the
// process without our filter ever running), so the log is the only place the address survives.
int32_t s_avLogs = 12;
LONG WINAPI StackOverflowWatcher(EXCEPTION_POINTERS* pointers) {
	if (!pointers || !pointers->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
	const EXCEPTION_RECORD* record = pointers->ExceptionRecord;
	if (record->ExceptionCode == EXCEPTION_STACK_OVERFLOW) ReportCrash(pointers);
	else if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && s_avLogs > 0) {
		s_avLogs--;
		char where[512];
		DescribeAddress(record->ExceptionAddress, where, sizeof(where));
		Log("access violation (first chance) at %s: %s address %p, thread %u%s", where,
			record->NumberParameters >= 2 && record->ExceptionInformation[0] == 1 ? "writing" : "reading",
			record->NumberParameters >= 2 ? (void*)record->ExceptionInformation[1] : nullptr, (unsigned)GetCurrentThreadId(),
			s_avLogs ? "" : " (no more of these are logged)");
		if (pointers->ContextRecord) {  // code addresses on the stack: a rough call path, no symbols needed
			void** stack = (void**)pointers->ContextRecord->Rsp;
			int shown = 0;
			for (int i = 0; i < 256 && shown < 10; i++) {
				void* value = nullptr;
				if (!ReadProcessMemory(GetCurrentProcess(), stack + i, &value, sizeof(value), nullptr)) break;
				HMODULE module = nullptr;
				if (value && (uintptr_t)value > 0x10000 &&
					GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)value,
									   &module) &&
					module) {
					DescribeAddress(value, where, sizeof(where));
					Log("  stack[%d] %s", i, where);
					shown++;
				}
			}
		}
	}
	return EXCEPTION_CONTINUE_SEARCH;
}
}  // namespace

void InstallCrashHandler() {
	if (!s_crashRequest) {
		HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");  // preloaded: no LoadLibrary while crashing
		s_writeDump = dbghelp ? (PFN_MiniDumpWriteDump)(void*)GetProcAddress(dbghelp, "MiniDumpWriteDump") : nullptr;
		s_crashRequest = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		s_crashDone = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		HANDLE thread = CreateThread(nullptr, 256 * 1024, CrashReporterThread, nullptr, 0, &s_reporterThreadId);
		if (thread) CloseHandle(thread);
		else s_crashRequest = nullptr;
		AddVectoredExceptionHandler(1, StackOverflowWatcher);
	}
	LPTOP_LEVEL_EXCEPTION_FILTER prev = SetUnhandledExceptionFilter(CrashFilter);
	if (prev != CrashFilter) s_prevFilter = prev;  // re-installing must not make us call ourselves
}

// Environment info for the log
typedef LONG(WINAPI* PFN_RtlGetVersion)(OSVERSIONINFOW*);

namespace {
void LogEnvironment() {
	wchar_t exe[MAX_PATH];
	GetModuleFileNameW(nullptr, exe, MAX_PATH);
	Log("NieRVR %s (built %s %s)", NIERVR_VERSION, __DATE__, __TIME__);
	Log("game exe: %s", Utf8(exe).s);
	Log("mod dir:  %s", Utf8(g_modDir).s);
	OSVERSIONINFOW version = {};
	version.dwOSVersionInfoSize = sizeof(version);
	PFN_RtlGetVersion rtlGetVersion = (PFN_RtlGetVersion)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
	if (rtlGetVersion && rtlGetVersion(&version) == 0)
		Log("windows %u.%u build %u", (unsigned)version.dwMajorVersion, (unsigned)version.dwMinorVersion, (unsigned)version.dwBuildNumber);

	// List the DLLs sitting next to the game: the usual source of mod conflicts.
	wchar_t pattern[MAX_PATH];
	lstrcpynW(pattern, g_modDir, MAX_PATH - 8);
	lstrcatW(pattern, L"*.dll");
	WIN32_FIND_DATAW found;
	HANDLE search = FindFirstFileW(pattern, &found);
	if (search != INVALID_HANDLE_VALUE) {
		do {
			Log("game-dir dll: %s (%u KB)", Utf8(found.cFileName).s, (unsigned)(found.nFileSizeLow / 1024));
		} while (FindNextFileW(search, &found));
		FindClose(search);
	}
	// Overlays / injected modules already present
	const wchar_t* known[] = {L"GameOverlayRenderer64.dll", L"RTSSHooks64.dll", L"SpecialK64.dll", L"ReShade64.dll",
							  L"dxgi.dll", L"nvspcap64.dll", L"DiscordHook64.dll", L"obs-vulkan64.dll"};
	for (const wchar_t* name : known) {
		HMODULE module = GetModuleHandleW(name);
		if (module) {
			wchar_t path[MAX_PATH];
			GetModuleFileNameW(module, path, MAX_PATH);
			Log("loaded module: %s", Utf8(path).s);
		}
	}
	Log("config: Enabled=%d Distance=%.2f Width=%.2f HeightOffset=%.2f ChainLoad='%s' LoaderPath='%s' Verbose=%d "
		"Probe=%d CaptureKey=0x%x",
		g_cfg.enabled, (double)g_cfg.distance, (double)g_cfg.width, (double)g_cfg.heightOffset, Utf8(g_cfg.chainLoad).s,
		Utf8(g_cfg.loaderPath).s, g_cfg.verbose, g_cfg.probe, (unsigned)g_cfg.keyCapture);
	Log("config: Stereo=%d WorldScale=%.3f CameraDolly=%.2f LevelCamera=%d Anamorphic=%d VerticalFill=%.2f "
		"FovScale=%.2f ExtraResolutions=%d AmbientOcclusion=%d Shadows=%d Vignette=%d LowHealthEffect=%d DistortionEffects=%d CullingFov=%.0f FlatBelowDraws=%d SteepView=%.0f-%.0f TopDownFlat=%d (%.0f/%.0f)",
		g_cfg.stereo, (double)g_cfg.worldScale, (double)g_cfg.cameraDolly, g_cfg.levelCamera, g_cfg.anamorphic,
		(double)g_cfg.verticalFill, (double)g_cfg.fovScale, g_cfg.extraResolutions,
		g_cfg.ambientOcclusion, g_cfg.shadows, g_cfg.vignette, g_cfg.lowHealthFx, g_cfg.distortionFx, (double)g_cfg.cullFov, g_cfg.flatBelowDraws,
		(double)g_cfg.steepFrom, (double)g_cfg.steepFull, g_cfg.topDownFlat,
		(double)g_cfg.topDownFrom, (double)g_cfg.topDownUntil);
	if (g_cfg.iniAdded) Log("config: added %d new settings (with their defaults) to the existing NieRVR.ini", g_cfg.iniAdded);
	if (g_cfg.cullFovAsked > 0)
		Log("config: CullingFov=%.0f is more than the 170 maximum (a field of view of 180 or more doesn't exist); using 170 (written back to NieRVR.ini)",
			(double)g_cfg.cullFovAsked);
	{
		char list[120];
		size_t used = 0;
		list[0] = 0;
		for (int i = 0; i < g_cfg.fovRealCount && used + 16 < sizeof(list); i++)
			used += snprintf(list + used, sizeof(list) - used, "%s0x%x", i ? "," : "", g_cfg.fovRealCallers[i]);
		Log("config: HeadCullingMargin=%.0f Synchronized=%d", (double)g_cfg.headCullMargin,
			g_cfg.synchronized);
		Log("config: HeadCulling=%d FovRealCallers=%s FovEasingCaller=0x%x", g_cfg.headCulling, used ? list : "(none)",
			g_cfg.fovEasingCaller);
	}
}

BOOL CALLBACK InitOnceCallback(PINIT_ONCE, PVOID, PVOID*) {
	GetModuleFileNameW(g_hSelf, g_modDir, MAX_PATH);
	wchar_t* slash = g_modDir;
	for (wchar_t* c = g_modDir; *c; c++)
		if (*c == L'\\' || *c == L'/') slash = c;
	slash[1] = 0;

	lstrcpynW(g_iniPath, g_modDir, MAX_PATH - 16);
	lstrcatW(g_iniPath, L"NieRVR.ini");
	ConfigLoad();

	wchar_t logPath[MAX_PATH];
	lstrcpynW(logPath, g_modDir, MAX_PATH - 16);
	lstrcatW(logPath, L"NieRVR.log");
	LogOpen(logPath);
	LogEnvironment();
	GameResolutionInit();  // before the game reads its own settings, when it can

	InitializeCriticalSection(&g_share.lock);
	InstallCrashHandler();
	if (!ProxyLoadReal()) Log("FATAL: could not load the real d3d11.dll - the game will not start");
	ExtraResolutionsInstallHooks();
	CullingInstallHook();
	FrameRateInstall();
	g_initDone = true;
	return TRUE;
}
}  // namespace

void EnsureInit() { InitOnceExecuteOnce(&s_initOnce, InitOnceCallback, nullptr, nullptr); }

extern "C" BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
	if (reason == DLL_PROCESS_ATTACH) {
		g_hSelf = hinst;
		DisableThreadLibraryCalls(hinst);
		// Display hooks must see the game's very first resolution queries, which happen before it
		// touches D3D11. They only patch code here; everything else waits for the first call into
		// the DLL (outside the loader lock).
		ExtraResolutionsEarlyHooks();
	}
	return TRUE;
}
