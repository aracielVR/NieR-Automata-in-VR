// main.cpp - DLL entry, one-time initialisation, crash reporting.
#include "common.h"
#include <dbghelp.h>

HMODULE g_hSelf;
wchar_t g_modDir[MAX_PATH];
wchar_t g_iniPath[MAX_PATH];

static INIT_ONCE g_initOnce = INIT_ONCE_STATIC_INIT;
volatile bool g_initDone;

// ---------------------------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------------------------
void DescribeAddress(void* addr, char* out, size_t cap) {
    HMODULE mod = nullptr;
    wchar_t name[MAX_PATH] = L"?";
    if (addr &&
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)addr, &mod) &&
        mod) {
        GetModuleFileNameW(mod, name, MAX_PATH);
        const wchar_t* base = name;
        for (const wchar_t* p = name; *p; p++)
            if (*p == L'\\' || *p == L'/') base = p + 1;
        pure::format(out, cap, "%S+0x%llx", base, (unsigned long long)((char*)addr - (char*)mod));
    } else {
        pure::format(out, cap, "%p (no module)", addr);
    }
}

bool LogBudget(volatile LONG* counter, LONG max) {
    LONG n = InterlockedIncrement(counter);
    if (n == max) Log("(the next message repeats; further repeats are suppressed)");
    return n <= max;
}

// ---------------------------------------------------------------------------------------------
// Crash reporting.
// The report is produced on a dedicated thread with its own stack, so it also works for stack
// overflows (where the crashing thread has no stack left to run anything).
// ---------------------------------------------------------------------------------------------
typedef BOOL(WINAPI* PFN_MiniDumpWriteDump)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                            PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

static LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter;
static HANDLE g_crashRequest, g_crashDone;
static EXCEPTION_POINTERS* volatile g_crashEp;
static volatile DWORD g_crashThread;
static volatile LONG g_crashReported;
static PFN_MiniDumpWriteDump g_writeDump;
static DWORD g_reporterTid;

static void WriteCrashReport(EXCEPTION_POINTERS* ep, DWORD threadId) {
    EXCEPTION_RECORD* er = ep->ExceptionRecord;
    char where[512];
    DescribeAddress(er->ExceptionAddress, where, sizeof(where));
    Log("!!! CRASH: exception 0x%08x at %s, thread %u%s", (unsigned)er->ExceptionCode, where, (unsigned)threadId,
        threadId == g_xrThreadId ? " (VR thread)" : "");
    if (er->ExceptionCode == EXCEPTION_STACK_OVERFLOW) Log("!!! (stack overflow: usually endless recursion)");
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
        Log("!!! access violation: %s address %p",
            er->ExceptionInformation[0] == 0 ? "reading" : (er->ExceptionInformation[0] == 1 ? "writing" : "executing"),
            (void*)er->ExceptionInformation[1]);
    if (ep->ContextRecord) {
        CONTEXT* c = ep->ContextRecord;
        Log("!!! rip=%p rsp=%p rax=%p rcx=%p rdx=%p r8=%p r9=%p", (void*)c->Rip, (void*)c->Rsp, (void*)c->Rax,
            (void*)c->Rcx, (void*)c->Rdx, (void*)c->R8, (void*)c->R9);
        // Code addresses found on the stack: a rough call path without needing symbols.
        void** sp = (void**)c->Rsp;
        int shown = 0;
        for (int i = 0; i < 512 && shown < 16; i++) {
            void* v = nullptr;
            if (!ReadProcessMemory(GetCurrentProcess(), sp + i, &v, sizeof(v), nullptr)) break;
            HMODULE m = nullptr;
            if (v && (uintptr_t)v > 0x10000 &&
                GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCWSTR)v, &m) &&
                m) {
                DescribeAddress(v, where, sizeof(where));
                Log("!!!   stack[%d] %s", i, where);
                shown++;
            }
        }
    }
    wchar_t dumpPath[MAX_PATH];
    lstrcpynW(dumpPath, g_modDir, MAX_PATH - 32);
    lstrcatW(dumpPath, L"NieRVR_crash.dmp");
    HANDLE f = CreateFileW(dumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        Log("!!! cannot create NieRVR_crash.dmp (error %u)", (unsigned)GetLastError());
        return;
    }
    if (g_writeDump) {
        MINIDUMP_EXCEPTION_INFORMATION mei;
        mei.ThreadId = threadId;
        mei.ExceptionPointers = ep;
        mei.ClientPointers = FALSE;
        BOOL ok = g_writeDump(GetCurrentProcess(), GetCurrentProcessId(), f,
                              (MINIDUMP_TYPE)(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithUnloadedModules |
                                              MiniDumpWithThreadInfo),
                              &mei, nullptr, nullptr);
        Log("!!! minidump %s: NieRVR_crash.dmp", ok ? "written" : "FAILED");
    } else {
        Log("!!! dbghelp.dll unavailable, no minidump");
    }
    CloseHandle(f);
}

static DWORD WINAPI CrashReporterThread(void*) {
    for (;;) {
        WaitForSingleObject(g_crashRequest, INFINITE);
        if (g_crashEp) WriteCrashReport(g_crashEp, g_crashThread);
        SetEvent(g_crashDone);
    }
}

// Uses almost no stack: hands the work to the reporter thread and waits.
static void ReportCrash(EXCEPTION_POINTERS* ep) {
    if (!ep || !ep->ExceptionRecord || InterlockedExchange(&g_crashReported, 1) != 0) return;
    g_crashEp = ep;
    g_crashThread = GetCurrentThreadId();
    if (g_crashRequest && g_crashThread != g_reporterTid) {  // the reporter can't wait on itself
        SetEvent(g_crashRequest);
        WaitForSingleObject(g_crashDone, 20000);
    }
}

static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    ReportCrash(ep);
    return g_prevFilter ? g_prevFilter(ep) : EXCEPTION_CONTINUE_SEARCH;
}

// Stack overflows are never meant to be survived; catch them first-chance in case the game's
// own handler swallows them before our unhandled-exception filter sees them.
static LONG WINAPI StackOverflowWatcher(EXCEPTION_POINTERS* ep) {
    if (ep && ep->ExceptionRecord && ep->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW) ReportCrash(ep);
    return EXCEPTION_CONTINUE_SEARCH;
}

void InstallCrashHandler() {
    if (!g_crashRequest) {
        HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");  // preloaded: no LoadLibrary while crashing
        g_writeDump = dbghelp ? (PFN_MiniDumpWriteDump)(void*)GetProcAddress(dbghelp, "MiniDumpWriteDump") : nullptr;
        g_crashRequest = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        g_crashDone = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HANDLE t = CreateThread(nullptr, 256 * 1024, CrashReporterThread, nullptr, 0, &g_reporterTid);
        if (t) CloseHandle(t);
        else g_crashRequest = nullptr;
        AddVectoredExceptionHandler(1, StackOverflowWatcher);
    }
    LPTOP_LEVEL_EXCEPTION_FILTER prev = SetUnhandledExceptionFilter(CrashFilter);
    if (prev != CrashFilter) g_prevFilter = prev;  // re-installing must not make us call ourselves
}

// ---------------------------------------------------------------------------------------------
// Environment info for the log
// ---------------------------------------------------------------------------------------------
typedef LONG(WINAPI* PFN_RtlGetVersion)(OSVERSIONINFOW*);

static void LogEnvironment() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    Log("NieRVR %s (built %s %s)", NIERVR_VERSION, __DATE__, __TIME__);
    Log("game exe: %S", exe);
    Log("mod dir:  %S", g_modDir);
    OSVERSIONINFOW vi = {};
    vi.dwOSVersionInfoSize = sizeof(vi);
    PFN_RtlGetVersion rtlGetVersion = (PFN_RtlGetVersion)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    if (rtlGetVersion && rtlGetVersion(&vi) == 0)
        Log("windows %u.%u build %u", (unsigned)vi.dwMajorVersion, (unsigned)vi.dwMinorVersion, (unsigned)vi.dwBuildNumber);

    // List the DLLs sitting next to the game: the usual source of mod conflicts.
    wchar_t pattern[MAX_PATH];
    lstrcpynW(pattern, g_modDir, MAX_PATH - 8);
    lstrcatW(pattern, L"*.dll");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            Log("game-dir dll: %S (%u KB)", fd.cFileName, (unsigned)(fd.nFileSizeLow / 1024));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    // Overlays / injected modules already present
    const wchar_t* known[] = {L"GameOverlayRenderer64.dll", L"RTSSHooks64.dll", L"SpecialK64.dll", L"ReShade64.dll",
                              L"dxgi.dll", L"nvspcap64.dll", L"DiscordHook64.dll", L"obs-vulkan64.dll"};
    for (const wchar_t* k : known) {
        HMODULE m = GetModuleHandleW(k);
        if (m) {
            wchar_t p[MAX_PATH];
            GetModuleFileNameW(m, p, MAX_PATH);
            Log("loaded module: %S", p);
        }
    }
    Log("config: Enabled=%d Distance=%.2f Width=%.2f HeightOffset=%.2f ChainLoad='%S' LoaderPath='%S' Verbose=%d "
        "Probe=%d CaptureKey=0x%x",
        g_cfg.enabled, (double)g_cfg.distance, (double)g_cfg.width, (double)g_cfg.heightOffset, g_cfg.chainLoad,
        g_cfg.loaderPath, g_cfg.verbose, g_cfg.probe, (unsigned)g_cfg.keyCapture);
    Log("config: Stereo=%d WorldScale=%.3f CameraDolly=%.2f LevelCamera=%d Anamorphic=%d VerticalFill=%.2f "
        "FovScale=%.2f SplitUI=%d ExtraResolutions=%d AmbientOcclusion=%d Shadows=%d Vignette=%d LowHealthEffect=%d DistortionEffects=%d FixLighting=%d CullingFov=%.0f LodFix=%d FlatBelowDraws=%d SteepView=%.0f-%.0f TopDownFlat=%d (%.0f/%.0f)",
        g_cfg.stereo, (double)g_cfg.worldScale, (double)g_cfg.cameraDolly, g_cfg.levelCamera, g_cfg.anamorphic,
        (double)g_cfg.verticalFill, (double)g_cfg.fovScale, g_cfg.uiSplit, g_cfg.extraResolutions,
        g_cfg.ambientOcclusion, g_cfg.shadows, g_cfg.vignette, g_cfg.lowHealthFx, g_cfg.distortionFx, g_cfg.viewFix, (double)g_cfg.cullFov, g_cfg.lodFix, g_cfg.flatBelowDraws,
        (double)g_cfg.steepFrom, (double)g_cfg.steepFull, g_cfg.topDownFlat,
        (double)g_cfg.topDownFrom, (double)g_cfg.topDownUntil);
    if (g_cfg.iniAdded) Log("config: added %d new settings (with their defaults) to the existing NieRVR.ini", g_cfg.iniAdded);
    if (g_cfg.cullFovAsked > 0)
        Log("config: CullingFov=%.0f is more than the 170 maximum (a field of view of 180 or more doesn't exist); using 170 (written back to NieRVR.ini)",
            (double)g_cfg.cullFovAsked);
    {
        char list[120];
        size_t at = 0;
        list[0] = 0;
        for (int i = 0; i < g_cfg.fovRealCount && at + 16 < sizeof(list); i++)
            at += pure::format(list + at, sizeof(list) - at, "%s0x%x", i ? "," : "", g_cfg.fovRealCallers[i]);
        Log("config: HeadCullingMargin=%.0f EyeSync=%d BothEyes=%d", (double)g_cfg.headCullMargin, g_cfg.eyeSync,
            g_cfg.bothEyes);
        Log("config: HeadCulling=%d WideProjection=%d FovRealCallers=%s FovEasingCaller=0x%x", g_cfg.headCulling,
            g_cfg.wideProjection, at ? list : "(none)", g_cfg.fovEasingCaller);
    }
}

static BOOL CALLBACK InitOnceCallback(PINIT_ONCE, PVOID, PVOID*) {
    GetModuleFileNameW(g_hSelf, g_modDir, MAX_PATH);
    wchar_t* slash = g_modDir;
    for (wchar_t* p = g_modDir; *p; p++)
        if (*p == L'\\' || *p == L'/') slash = p;
    slash[1] = 0;

    lstrcpynW(g_iniPath, g_modDir, MAX_PATH - 16);
    lstrcatW(g_iniPath, L"NieRVR.ini");
    ConfigLoad();

    wchar_t logPath[MAX_PATH];
    lstrcpynW(logPath, g_modDir, MAX_PATH - 16);
    lstrcatW(logPath, L"NieRVR.log");
    LogOpen(logPath);
    LogEnvironment();

    InitializeCriticalSection(&g_share.cs);
    InstallCrashHandler();
    if (!ProxyLoadReal()) Log("FATAL: could not load the real d3d11.dll - the game will not start");
    DisplayInstallHooks();
    CullingInstallHook();
    g_initDone = true;
    return TRUE;
}

void EnsureInit() { InitOnceExecuteOnce(&g_initOnce, InitOnceCallback, nullptr, nullptr); }

extern "C" BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hSelf = hinst;
        DisableThreadLibraryCalls(hinst);
        // Display hooks must see the game's very first resolution queries, which happen before it
        // touches D3D11. They only patch code here; everything else waits for the first call into
        // the DLL (outside the loader lock).
        DisplayEarlyHooks();
    }
    return TRUE;
}
