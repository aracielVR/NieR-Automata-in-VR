// log.cpp - thread-safe, crash-tolerant log file (NieRVR.log).
#include "common.h"

static HANDLE g_logFile = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_logCs;
static LARGE_INTEGER g_qpcFreq, g_qpcStart;

void LogOpen(const wchar_t* path) {
    InitializeCriticalSection(&g_logCs);
    QueryPerformanceFrequency(&g_qpcFreq);
    QueryPerformanceCounter(&g_qpcStart);
    // Keep the previous session's log as NieRVR.prev.log for comparison
    wchar_t prev[MAX_PATH];
    lstrcpynW(prev, path, MAX_PATH - 16);
    int n = lstrlenW(prev);
    if (n > 4) {
        prev[n - 4] = 0;  // strip ".log"
        lstrcatW(prev, L".prev.log");
        MoveFileExW(path, prev, MOVEFILE_REPLACE_EXISTING);
    }
    // Writes go straight to the OS cache, so the log survives a game crash.
    g_logFile = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
}

static void LogWrite(const char* fmt, va_list ap) {
    if (g_logFile == INVALID_HANDLE_VALUE) return;
    char line[2048];
    SYSTEMTIME st;
    GetLocalTime(&st);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double secs = (double)(now.QuadPart - g_qpcStart.QuadPart) / (double)g_qpcFreq.QuadPart;
    int n = pure::format(line, sizeof(line), "%02d:%02d:%02d.%03d [%9.3f] [t%5u] ", st.wHour, st.wMinute, st.wSecond,
                         st.wMilliseconds, secs, (unsigned)GetCurrentThreadId());
    if (n > (int)sizeof(line) - 3) n = (int)sizeof(line) - 3;
    int m = pure::vformat(line + n, sizeof(line) - n - 2, fmt, ap);
    int total = n + m;
    if (total > (int)sizeof(line) - 3) total = (int)sizeof(line) - 3;
    line[total++] = '\r';
    line[total++] = '\n';
    EnterCriticalSection(&g_logCs);
    DWORD written;
    WriteFile(g_logFile, line, (DWORD)total, &written, nullptr);
    LeaveCriticalSection(&g_logCs);
}

void Log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    LogWrite(fmt, ap);
    va_end(ap);
}

void LogV(const char* fmt, ...) {
    if (!g_cfg.verbose) return;
    va_list ap;
    va_start(ap, fmt);
    LogWrite(fmt, ap);
    va_end(ap);
}
