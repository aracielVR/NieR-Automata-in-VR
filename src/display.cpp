// display.cpp - render resolution.
//
// With [Render] ExtraResolutions=1 the game's resolution list gets extra entries above the
// monitor's native size (125%, 150%, 175%, 200%, 250%, 300%). The game then renders at that size; in windowed
// or borderless mode Windows scales the image down for the monitor, while the headset gets the full
// resolution. Exclusive fullscreen at those sizes is refused (the monitor can't show them), and
// the game window is kept within the monitor. Works on any GPU, no driver settings involved.
//
// NieR doesn't ask DXGI for its mode list: it reads the desktop size with
// EnumDisplaySettingsW(ENUM_CURRENT_SETTINGS) (0.3.2 log) and apparently only offers resolutions up to
// it. So with the option on, calls coming from the game's own code are told the desktop is 300% of
// the real one (our own code and other DLLs see the real size). ExtraResolutions=2 additionally
// rewrites the smallest entries of the game's built-in resolution table (found by scanning the exe
// image; the table and what was changed are logged) in case the list is fixed.
//
// The hooks always log how the game enumerates and applies resolutions (first few calls), so we
// can see which path NieR takes even with the option off.
#include "common.h"

namespace {
typedef HRESULT(WINAPI* PFN_CreateFactory)(REFIID, void**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_GetDisplayModeList)(IDXGIOutput*, DXGI_FORMAT, UINT, UINT*, DXGI_MODE_DESC*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_FindClosest)(IDXGIOutput*, const DXGI_MODE_DESC*, DXGI_MODE_DESC*, IUnknown*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateSwapChain)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*,
                                                        IDXGISwapChain**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_SetFullscreenState)(IDXGISwapChain*, BOOL, IDXGIOutput*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_ResizeTarget)(IDXGISwapChain*, const DXGI_MODE_DESC*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_ResizeBuffers)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
typedef BOOL(WINAPI* PFN_EnumDisplaySettingsW)(LPCWSTR, DWORD, DEVMODEW*);
typedef BOOL(WINAPI* PFN_EnumDisplaySettingsA)(LPCSTR, DWORD, DEVMODEA*);
typedef BOOL(WINAPI* PFN_SetWindowPos)(HWND, HWND, int, int, int, int, UINT);
typedef BOOL(WINAPI* PFN_MoveWindow)(HWND, int, int, int, int, BOOL);

PFN_GetDisplayModeList o_GetDisplayModeList;
typedef HRESULT(STDMETHODCALLTYPE* PFN_GetDisplayModeList1)(IDXGIOutput1*, DXGI_FORMAT, UINT, UINT*, DXGI_MODE_DESC1*);
PFN_GetDisplayModeList1 o_GetDisplayModeList1;
typedef HRESULT(WINAPI* PFN_CreateFactory2)(UINT, REFIID, void**);
PFN_CreateFactory o_CreateDXGIFactory, o_CreateDXGIFactory1;
PFN_CreateFactory2 o_CreateDXGIFactory2;
typedef BOOL(WINAPI* PFN_EnumDisplaySettingsExW)(LPCWSTR, DWORD, DEVMODEW*, DWORD);
typedef BOOL(WINAPI* PFN_EnumDisplaySettingsExA)(LPCSTR, DWORD, DEVMODEA*, DWORD);
PFN_EnumDisplaySettingsExW o_EnumDisplaySettingsExW;
PFN_EnumDisplaySettingsExA o_EnumDisplaySettingsExA;
volatile LONG g_bFactory;
bool g_earlyDxgi;          // dxgi.dll was already loaded when the mod started (factory hooks in place early)
int g_earlyHooks;          // user32/dxgi hooks installed at load time
PFN_FindClosest o_FindClosest;
PFN_CreateSwapChain o_CreateSwapChain;
PFN_SetFullscreenState o_SetFullscreenState;
PFN_ResizeTarget o_ResizeTarget;
PFN_ResizeBuffers o_ResizeBuffers;
PFN_EnumDisplaySettingsW o_EnumDisplaySettingsW;
PFN_EnumDisplaySettingsA o_EnumDisplaySettingsA;
PFN_SetWindowPos o_SetWindowPos;
PFN_MoveWindow o_MoveWindow;

HWND g_gameHwnd;
uintptr_t g_exeBase, g_exeEnd;
UINT g_spoofW, g_spoofH;  // desktop size reported to the game (0 = not spoofing)
volatile LONG g_bSysMet, g_bMonInfo, g_bEnumCur, g_bEnumList;
volatile LONG g_bModes, g_bClosest, g_bSwap, g_bFull, g_bTarget, g_bBuffers, g_bWin;
// 0.4.5: up to 300% (a 1080p monitor reaches 5760x3240 for high-resolution headsets)
const float kFactors[] = {1.25f, 1.5f, 1.75f, 2.0f, 2.5f, 3.0f};
const int kExtra = 6;

UINT Round8(float v) { return ((UINT)(v + 4)) & ~7u; }
// how many of the factors fit Direct3D 11's 16384-pixel texture limit for this base size
int ExtraCount(UINT w, UINT h) {
    int n = 0;
    while (n < kExtra && Round8(w * kFactors[n]) <= 16384 && Round8(h * kFactors[n]) <= 16384) n++;
    return n;
}

bool FromGame(void* ret) { return (uintptr_t)ret >= g_exeBase && (uintptr_t)ret < g_exeEnd; }

void Caller(void* ret, char* out, size_t cap) { DescribeAddress(ret, out, cap); }

void MonitorSize(HWND hwnd, int* w, int* h, int* x, int* y) {
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (mon && GetMonitorInfoW(mon, &mi)) {
        *w = mi.rcMonitor.right - mi.rcMonitor.left;
        *h = mi.rcMonitor.bottom - mi.rcMonitor.top;
        *x = mi.rcMonitor.left;
        *y = mi.rcMonitor.top;
    } else {
        *w = GetSystemMetrics(SM_CXSCREEN);
        *h = GetSystemMetrics(SM_CYSCREEN);
        *x = *y = 0;
    }
}

bool LargerThanMonitor(HWND hwnd, UINT w, UINT h) {
    int mw, mh, mx, my;
    MonitorSize(hwnd, &mw, &mh, &mx, &my);
    return (int)w > mw || (int)h > mh;
}

// --- DXGI mode list --------------------------------------------------------------------------
// The output's current desktop size (the monitor's own resolution, whatever DSR modes it also lists).
void OutputDesktopSize(IDXGIOutput* o, UINT* w, UINT* h) {
    DXGI_OUTPUT_DESC d;
    if (o && SUCCEEDED(o->GetDesc(&d))) {
        LONG dw = d.DesktopCoordinates.right - d.DesktopCoordinates.left, dh = d.DesktopCoordinates.bottom - d.DesktopCoordinates.top;
        if (d.Rotation == DXGI_MODE_ROTATION_ROTATE90 || d.Rotation == DXGI_MODE_ROTATION_ROTATE270) {
            LONG t = dw;
            dw = dh;
            dh = t;
        }
        if (dw > 0 && dh > 0) {
            *w = (UINT)dw;
            *h = (UINT)dh;
        }
    }
}
// Extra sizes are 125-300% of the monitor's *desktop* resolution (baseW x baseH; 0 = unknown, then the
// largest listed mode). 0.5.3 used the largest listed mode: with NVIDIA DSR/DLDSR on, the list itself
// already goes above the monitor (5760x3240 on a 4K monitor), so 4800x2700 disappeared (user report).
// Sizes the list already has are not added twice.
template <typename DESC, typename ORIG>
HRESULT ModeListWithExtras(const char* api, void* ret, ORIG orig, DXGI_FORMAT fmt, UINT flags, UINT* num, DESC* desc,
                           UINT baseW, UINT baseH) {
    bool game = FromGame(ret);
    if (!g_cfg.extraResolutions || !num || !game) {
        HRESULT hr = orig(num, desc);
        if (LogBudget(&g_bModes, 8)) {
            char who[300];
            Caller(ret, who, sizeof(who));
            Log("display: %s from %s (format %d, flags 0x%x, %s) -> %u modes, 0x%08x", api, who, (int)fmt, flags,
                desc ? "list" : "count", num ? *num : 0, (unsigned)hr);
        }
        return hr;
    }
    UINT real = 0;
    HRESULT hr = orig(&real, (DESC*)nullptr);
    if (FAILED(hr) || real == 0) return orig(num, desc);
    DESC* tmp = (DESC*)HeapAlloc(GetProcessHeap(), 0, (real + kExtra) * sizeof(DESC));
    if (!tmp) return orig(num, desc);
    hr = orig(&real, tmp);
    if (FAILED(hr)) {
        HeapFree(GetProcessHeap(), 0, tmp);
        return orig(num, desc);
    }
    // the base: the desktop-size mode at its highest refresh rate (else the largest mode)
    auto rate = [](const DESC& d) {
        return d.RefreshRate.Denominator ? (double)d.RefreshRate.Numerator / d.RefreshRate.Denominator : 0.0;
    };
    UINT best = 0, largest = 0;
    bool haveBase = false;
    for (UINT i = 0; i < real; i++) {
        uint64_t a = (uint64_t)tmp[i].Width * tmp[i].Height, b = (uint64_t)tmp[largest].Width * tmp[largest].Height;
        if (a > b || (a == b && rate(tmp[i]) > rate(tmp[largest]))) largest = i;
        if (baseW && tmp[i].Width == baseW && tmp[i].Height == baseH && (!haveBase || rate(tmp[i]) > rate(tmp[best]))) {
            best = i;
            haveBase = true;
        }
    }
    if (!haveBase) best = largest;
    UINT bw = tmp[best].Width, bh = tmp[best].Height;
    uint32_t* keys = (uint32_t*)HeapAlloc(GetProcessHeap(), 0, real * sizeof(uint32_t));
    uint32_t xw[6], xh[6];
    int added = 0;
    if (keys) {
        for (UINT i = 0; i < real; i++) keys[i] = (tmp[i].Width & 0xffff) << 16 | (tmp[i].Height & 0xffff);
        added = pure::supersample_sizes(bw, bh, keys, (int)real, xw, xh);
        HeapFree(GetProcessHeap(), 0, keys);
    }
    for (int k = 0; k < added; k++) {
        tmp[real + k] = tmp[best];
        tmp[real + k].Width = xw[k];
        tmp[real + k].Height = xh[k];
    }
    UINT total = real + added;
    if (LogBudget(&g_bModes, 8)) {
        char who[300];
        Caller(ret, who, sizeof(who));
        Log("display: %s from %s (format %d, %s): %u native modes up to %ux%u; supersampling base %ux%u (%s) + %d "
            "modes up to %ux%u",
            api, who, (int)fmt, desc ? "list" : "count", real, tmp[largest].Width, tmp[largest].Height, bw, bh,
            haveBase ? "desktop size" : "largest mode", added, added ? tmp[total - 1].Width : 0, added ? tmp[total - 1].Height : 0);
    }
    if (!desc) {
        *num = total;
        HeapFree(GetProcessHeap(), 0, tmp);
        return S_OK;
    }
    UINT cap = *num, n = cap < total ? cap : total;
    memcpy(desc, tmp, n * sizeof(DESC));
    *num = n;
    HeapFree(GetProcessHeap(), 0, tmp);
    return cap < total ? DXGI_ERROR_MORE_DATA : S_OK;
}

HRESULT STDMETHODCALLTYPE H_GetDisplayModeList(IDXGIOutput* o, DXGI_FORMAT fmt, UINT flags, UINT* num,
                                               DXGI_MODE_DESC* desc) {
    UINT bw = 0, bh = 0;
    OutputDesktopSize(o, &bw, &bh);
    return ModeListWithExtras("GetDisplayModeList", __builtin_return_address(0),
                              [&](UINT* n, DXGI_MODE_DESC* d) { return o_GetDisplayModeList(o, fmt, flags, n, d); },
                              fmt, flags, num, desc, bw, bh);
}
HRESULT STDMETHODCALLTYPE H_GetDisplayModeList1(IDXGIOutput1* o, DXGI_FORMAT fmt, UINT flags, UINT* num,
                                                DXGI_MODE_DESC1* desc) {
    UINT bw = 0, bh = 0;
    OutputDesktopSize(o, &bw, &bh);
    return ModeListWithExtras("GetDisplayModeList1", __builtin_return_address(0),
                              [&](UINT* n, DXGI_MODE_DESC1* d) { return o_GetDisplayModeList1(o, fmt, flags, n, d); },
                              fmt, flags, num, desc, bw, bh);
}

HRESULT STDMETHODCALLTYPE H_FindClosest(IDXGIOutput* o, const DXGI_MODE_DESC* want, DXGI_MODE_DESC* out,
                                        IUnknown* dev) {
    HRESULT hr = o_FindClosest(o, want, out, dev);
    if (g_cfg.extraResolutions && want && out && SUCCEEDED(hr) && want->Width && want->Height &&
        LargerThanMonitor(nullptr, want->Width, want->Height)) {
        // keep the requested supersampling size instead of snapping to a native mode
        DXGI_MODE_DESC m = *out;
        m.Width = want->Width;
        m.Height = want->Height;
        *out = m;
    }
    if (LogBudget(&g_bClosest, 6))
        Log("display: FindClosestMatchingMode %ux%u -> %ux%u", want ? want->Width : 0, want ? want->Height : 0,
            out ? out->Width : 0, out ? out->Height : 0);
    return hr;
}

// --- swapchain -------------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE H_CreateSwapChain(IDXGIFactory* f, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* desc,
                                            IDXGISwapChain** out) {
    if (desc && desc->BufferDesc.Width >= 640) {
        g_gameHwnd = desc->OutputWindow;
        if (g_cfg.extraResolutions && !desc->Windowed &&
            LargerThanMonitor(desc->OutputWindow, desc->BufferDesc.Width, desc->BufferDesc.Height)) {
            Log("display: %ux%u is above the monitor's resolution - creating the swapchain windowed instead of "
                "exclusive fullscreen",
                desc->BufferDesc.Width, desc->BufferDesc.Height);
            desc->Windowed = TRUE;
        }
    }
    HRESULT hr = o_CreateSwapChain(f, dev, desc, out);
    if (desc && LogBudget(&g_bSwap, 8))
        Log("display: game created swapchain %ux%u format %d, %s, effect %d, %u buffers, flags 0x%x -> 0x%08x",
            desc->BufferDesc.Width, desc->BufferDesc.Height, (int)desc->BufferDesc.Format,
            desc->Windowed ? "windowed" : "fullscreen", (int)desc->SwapEffect, desc->BufferCount, desc->Flags,
            (unsigned)hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE H_SetFullscreenState(IDXGISwapChain* sc, BOOL full, IDXGIOutput* target) {
    if (full && g_cfg.extraResolutions) {
        DXGI_SWAP_CHAIN_DESC d;
        if (SUCCEEDED(sc->GetDesc(&d)) && LargerThanMonitor(d.OutputWindow, d.BufferDesc.Width, d.BufferDesc.Height)) {
            Log("display: refusing exclusive fullscreen at %ux%u (above the monitor's resolution); staying windowed",
                d.BufferDesc.Width, d.BufferDesc.Height);
            o_SetFullscreenState(sc, FALSE, nullptr);
            return S_OK;
        }
    }
    HRESULT hr = o_SetFullscreenState(sc, full, target);
    if (LogBudget(&g_bFull, 8)) Log("display: SetFullscreenState(%d) -> 0x%08x", (int)full, (unsigned)hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE H_ResizeTarget(IDXGISwapChain* sc, const DXGI_MODE_DESC* m) {
    DXGI_MODE_DESC clamped;
    if (m && g_cfg.extraResolutions) {
        DXGI_SWAP_CHAIN_DESC d;
        HWND w = SUCCEEDED(sc->GetDesc(&d)) ? d.OutputWindow : nullptr;
        if (LargerThanMonitor(w, m->Width, m->Height)) {
            int mw, mh, mx, my;
            MonitorSize(w, &mw, &mh, &mx, &my);
            clamped = *m;
            clamped.Width = (UINT)mw;
            clamped.Height = (UINT)mh;
            m = &clamped;
        }
    }
    HRESULT hr = o_ResizeTarget(sc, m);
    if (LogBudget(&g_bTarget, 8))
        Log("display: ResizeTarget %ux%u -> 0x%08x", m ? m->Width : 0, m ? m->Height : 0, (unsigned)hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE H_ResizeBuffers(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT flags) {
    UiOnResizeBuffers();
    ReplayRelease();
    ReprojectRelease();
    HRESULT hr = o_ResizeBuffers(sc, n, w, h, f, flags);
    if (LogBudget(&g_bBuffers, 8))
        Log("display: ResizeBuffers %ux%u format %d -> 0x%08x", w, h, (int)f, (unsigned)hr);
    return hr;
}

// --- user32 mode list -------------------------------------------------------------------------
template <typename DEVMODE_T>
struct EnumState {
    DEVMODE_T best;      // the base for the extra sizes: the current (desktop) mode, else the largest
    bool haveBest;
    bool haveCurrent;    // best is the device's current mode
    DWORD firstMissing;
    DWORD listed;
    uint32_t sizes[512]; // listed sizes (w << 16 | h), so extra sizes aren't listed twice
    int nsizes;
    UINT extraW[8], extraH[8];  // the extra sizes not already listed (made when the real list ends)
    int nextra;
};
EnumState<DEVMODEW> g_enumW;
EnumState<DEVMODEA> g_enumA;

template <typename DEVMODE_T, typename F, typename C>
BOOL EnumWithExtras(EnumState<DEVMODE_T>& st, const char* api, DWORD i, DEVMODE_T* dm, void* ret, F orig, C current) {
    bool game = FromGame(ret);
    if ((LONG)i < 0) {  // ENUM_CURRENT_SETTINGS / ENUM_REGISTRY_SETTINGS
        BOOL ok = orig();
        bool spoof = ok && dm && game && g_spoofW && (dm->dmFields & DM_PELSWIDTH);
        UINT rw = ok && dm ? dm->dmPelsWidth : 0, rh = ok && dm ? dm->dmPelsHeight : 0;
        if (spoof) {
            dm->dmPelsWidth = g_spoofW;
            dm->dmPelsHeight = g_spoofH;
        }
        if (LogBudget(&g_bEnumCur, 6)) {
            char who[300];
            Caller(ret, who, sizeof(who));
            Log("display: %s(%s) from %s -> %ux%u @%u Hz%s", api, i == ENUM_CURRENT_SETTINGS ? "current" : "registry",
                who, rw, rh, ok && dm ? (unsigned)dm->dmDisplayFrequency : 0,
                spoof ? " (reported to the game as the supersampling maximum)" : "");
        }
        if (spoof && i == ENUM_CURRENT_SETTINGS) {
            static bool once;
            if (!once) {
                once = true;
                Log("display: telling the game the desktop is %ux%u so it offers resolutions above %ux%u", g_spoofW,
                    g_spoofH, rw, rh);
            }
        }
        return ok;
    }
    if (i == 0) {
        st.haveBest = st.haveCurrent = false;
        st.firstMissing = 0;
        st.nsizes = 0;
        st.nextra = 0;
        DEVMODE_T cur = {};
        cur.dmSize = sizeof(cur);
        if (current(&cur) && cur.dmPelsWidth && cur.dmPelsHeight) {
            st.best = cur;
            st.haveBest = st.haveCurrent = true;
        }
    }
    BOOL ok = orig();
    if (ok && dm) {
        if (st.nsizes < 512) st.sizes[st.nsizes++] = (dm->dmPelsWidth & 0xffff) << 16 | (dm->dmPelsHeight & 0xffff);
        if (!st.haveCurrent &&
            (!st.haveBest ||
             (uint64_t)dm->dmPelsWidth * dm->dmPelsHeight > (uint64_t)st.best.dmPelsWidth * st.best.dmPelsHeight ||
             (dm->dmPelsWidth == st.best.dmPelsWidth && dm->dmPelsHeight == st.best.dmPelsHeight &&
              dm->dmDisplayFrequency > st.best.dmDisplayFrequency))) {
            st.best = *dm;
            st.haveBest = true;
        }
        return ok;
    }
    // end of the real list: the extra sizes it doesn't have yet follow it
    bool extras = g_cfg.extraResolutions && st.haveBest && dm && game;
    int ne = st.haveBest ? ExtraCount(st.best.dmPelsWidth, st.best.dmPelsHeight) : 0;
    if (!st.firstMissing) {
        st.firstMissing = i;
        uint32_t xw[6], xh[6];
        st.nextra = st.haveBest ? pure::supersample_sizes(st.best.dmPelsWidth, st.best.dmPelsHeight, st.sizes, st.nsizes, xw, xh) : 0;
        for (int k = 0; k < st.nextra; k++) {
            st.extraW[k] = xw[k];
            st.extraH[k] = xh[k];
        }
    }
    DWORD k = i - st.firstMissing;
    if (extras && k < (DWORD)st.nextra) {
        WORD size = dm->dmSize;
        *dm = st.best;
        dm->dmSize = size;
        dm->dmPelsWidth = st.extraW[k];
        dm->dmPelsHeight = st.extraH[k];
        return TRUE;
    }
    if (LogBudget(&g_bEnumList, 4)) {
        char who[300];
        Caller(ret, who, sizeof(who));
        Log("display: %s list from %s: %u modes; supersampling base %ux%u (%s), %d new sizes up to %d%%", api, who,
            (unsigned)st.firstMissing, st.haveBest ? (unsigned)st.best.dmPelsWidth : 0,
            st.haveBest ? (unsigned)st.best.dmPelsHeight : 0, st.haveCurrent ? "desktop size" : "largest mode",
            extras ? st.nextra : 0, extras && ne ? (int)(kFactors[ne - 1] * 100 + 0.5f) : 0);
    }
    return FALSE;
}

// Hooks installed at load time see calls from everywhere, possibly before the mod has read its
// settings. Calls from the game's own code initialise the mod first (they never run under the
// loader lock); anything else passes straight through until then.
bool Ready(void* ret) {
    if (g_initDone) return true;
    if (!FromGame(ret)) return false;
    EnsureInit();
    return g_initDone;
}

BOOL WINAPI H_EnumDisplaySettingsW(LPCWSTR dev, DWORD i, DEVMODEW* dm) {
    void* ret = __builtin_return_address(0);
    if (!Ready(ret)) return o_EnumDisplaySettingsW(dev, i, dm);
    return EnumWithExtras(g_enumW, "EnumDisplaySettingsW", i, dm, ret,
                          [&] { return o_EnumDisplaySettingsW(dev, i, dm); },
                          [&](DEVMODEW* c) { return o_EnumDisplaySettingsW(dev, ENUM_CURRENT_SETTINGS, c); });
}
BOOL WINAPI H_EnumDisplaySettingsA(LPCSTR dev, DWORD i, DEVMODEA* dm) {
    void* ret = __builtin_return_address(0);
    if (!Ready(ret)) return o_EnumDisplaySettingsA(dev, i, dm);
    return EnumWithExtras(g_enumA, "EnumDisplaySettingsA", i, dm, ret,
                          [&] { return o_EnumDisplaySettingsA(dev, i, dm); },
                          [&](DEVMODEA* c) { return o_EnumDisplaySettingsA(dev, ENUM_CURRENT_SETTINGS, c); });
}
BOOL WINAPI H_EnumDisplaySettingsExW(LPCWSTR dev, DWORD i, DEVMODEW* dm, DWORD fl) {
    void* ret = __builtin_return_address(0);
    if (!Ready(ret)) return o_EnumDisplaySettingsExW(dev, i, dm, fl);
    return EnumWithExtras(g_enumW, "EnumDisplaySettingsExW", i, dm, ret,
                          [&] { return o_EnumDisplaySettingsExW(dev, i, dm, fl); },
                          [&](DEVMODEW* c) { return o_EnumDisplaySettingsExW(dev, ENUM_CURRENT_SETTINGS, c, 0); });
}
BOOL WINAPI H_EnumDisplaySettingsExA(LPCSTR dev, DWORD i, DEVMODEA* dm, DWORD fl) {
    void* ret = __builtin_return_address(0);
    if (!Ready(ret)) return o_EnumDisplaySettingsExA(dev, i, dm, fl);
    return EnumWithExtras(g_enumA, "EnumDisplaySettingsExA", i, dm, ret,
                          [&] { return o_EnumDisplaySettingsExA(dev, i, dm, fl); },
                          [&](DEVMODEA* c) { return o_EnumDisplaySettingsExA(dev, ENUM_CURRENT_SETTINGS, c, 0); });
}

// DXGI factories made by the game: initialise the mod first, so the mode-list hooks exist before
// the game asks for its resolution list (NieR does that before creating its D3D11 device).
void NoteFactory(const char* api, void* ret) {
    if (!FromGame(ret)) return;
    EnsureInit();
    if (LogBudget(&g_bFactory, 4)) {
        char who[300];
        Caller(ret, who, sizeof(who));
        Log("display: game created a DXGI factory (%s) from %s", api, who);
    }
}
HRESULT WINAPI H_CreateDXGIFactory(REFIID r, void** pp) {
    NoteFactory("CreateDXGIFactory", __builtin_return_address(0));
    return o_CreateDXGIFactory(r, pp);
}
HRESULT WINAPI H_CreateDXGIFactory1(REFIID r, void** pp) {
    NoteFactory("CreateDXGIFactory1", __builtin_return_address(0));
    return o_CreateDXGIFactory1(r, pp);
}
HRESULT WINAPI H_CreateDXGIFactory2(UINT fl, REFIID r, void** pp) {
    NoteFactory("CreateDXGIFactory2", __builtin_return_address(0));
    return o_CreateDXGIFactory2(fl, r, pp);
}

// Logged only (to see whether the game also sizes things from these).
typedef int(WINAPI* PFN_GetSystemMetrics)(int);
typedef BOOL(WINAPI* PFN_GetMonitorInfoW)(HMONITOR, LPMONITORINFO);
PFN_GetSystemMetrics o_GetSystemMetrics;
PFN_GetMonitorInfoW o_GetMonitorInfoW;
int WINAPI H_GetSystemMetrics(int idx) {
    void* ret = __builtin_return_address(0);
    if (!Ready(ret)) return o_GetSystemMetrics(idx);
    int v = o_GetSystemMetrics(idx);
    if ((idx == SM_CXSCREEN || idx == SM_CYSCREEN || idx == SM_CXVIRTUALSCREEN || idx == SM_CYVIRTUALSCREEN ||
         idx == SM_CXFULLSCREEN || idx == SM_CYFULLSCREEN) &&
        FromGame(ret) && LogBudget(&g_bSysMet, 8)) {
        char who[300];
        Caller(ret, who, sizeof(who));
        Log("display: GetSystemMetrics(%d) from %s -> %d", idx, who, v);
    }
    return v;
}
BOOL WINAPI H_GetMonitorInfoW(HMONITOR m, LPMONITORINFO mi) {
    void* ret = __builtin_return_address(0);
    if (!Ready(ret)) return o_GetMonitorInfoW(m, mi);
    BOOL ok = o_GetMonitorInfoW(m, mi);
    if (ok && mi && FromGame(ret) && LogBudget(&g_bMonInfo, 6)) {
        char who[300];
        Caller(ret, who, sizeof(who));
        Log("display: GetMonitorInfoW from %s -> %dx%d", who, (int)(mi->rcMonitor.right - mi->rcMonitor.left),
            (int)(mi->rcMonitor.bottom - mi->rcMonitor.top));
    }
    return ok;
}

// --- the game's built-in resolution table -----------------------------------------------------
// Looks for runs of (width, height) int32 pairs with a fixed stride in the exe image, e.g.
// {1280,720},{1600,900},{1920,1080},... Logged always; with ExtraResolutions=2 the smallest entries
// of the single convincing table are replaced by the supersampling sizes.
bool PlausibleRes(int32_t w, int32_t h) {
    return w >= 640 && w <= 16384 && h >= 360 && h <= 8640 && w > h && w * 10 <= h * 36 && w * 10 >= h * 12 &&
           (w % 8) == 0 && (h % 2) == 0;
}

bool Readable(uintptr_t a, size_t len) {
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t end = a + len;
    while (a < end) {
        if (!VirtualQuery((void*)a, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
            (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
            !(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                             PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
            return false;
        a = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }
    return true;
}

struct ResTable {
    uintptr_t addr;
    int stride, n;
    bool has720, has1080, has2160;
};

int ScanResolutionTables(ResTable* out, int cap) {
    if (!g_exeBase) return 0;
    auto* dos = (IMAGE_DOS_HEADER*)g_exeBase;
    auto* nt = (IMAGE_NT_HEADERS64*)(g_exeBase + dos->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    int found = 0;
    static const int strides[] = {8, 12, 16, 20, 24, 32};
    for (int si = 0; si < nt->FileHeader.NumberOfSections; si++) {
        if (sec[si].Characteristics & IMAGE_SCN_MEM_EXECUTE) continue;  // code
        if (!(sec[si].Characteristics & IMAGE_SCN_MEM_READ)) continue;
        uintptr_t b = g_exeBase + sec[si].VirtualAddress, e = b + sec[si].Misc.VirtualSize;
        if (!Readable(b, e - b)) continue;
        for (uintptr_t p = b; p + 8 <= e; p += 4) {
            const int32_t* q = (const int32_t*)p;
            if (!PlausibleRes(q[0], q[1])) continue;
            for (int s : strides) {
                int n = 0;
                bool h720 = false, h1080 = false, h2160 = false;
                int32_t prevW = 0, prevH = 0;
                bool distinct = true;
                while (p + (uintptr_t)n * s + 8 <= e) {
                    const int32_t* r = (const int32_t*)(p + (uintptr_t)n * s);
                    if (!PlausibleRes(r[0], r[1])) break;
                    if (r[0] == prevW && r[1] == prevH) distinct = false;
                    prevW = r[0];
                    prevH = r[1];
                    h720 |= r[0] == 1280 && r[1] == 720;
                    h1080 |= r[0] == 1920 && r[1] == 1080;
                    h2160 |= r[0] == 3840 && r[1] == 2160;
                    n++;
                }
                if (n >= 4 && distinct && (h720 || h1080) && found < cap) {
                    out[found++] = {p, s, n, h720, h1080, h2160};
                    p += (uintptr_t)(n - 1) * s;  // skip past this table
                    break;
                }
            }
        }
    }
    return found;
}

void LogAndPatchTables(bool patch) {
    ResTable t[12];
    int n = ScanResolutionTables(t, 12);
    if (!n) {
        Log("display: no built-in resolution table found in the game exe");
        return;
    }
    int good = -1, goodCount = 0;
    for (int i = 0; i < n; i++) {
        char buf[900];
        size_t len = 0;
        buf[0] = 0;
        for (int k = 0; k < t[i].n && len + 24 < sizeof(buf); k++) {
            const int32_t* r = (const int32_t*)(t[i].addr + (uintptr_t)k * t[i].stride);
            int w = pure::format(buf + len, sizeof(buf) - len, "%s%dx%d", k ? " " : "", r[0], r[1]);
            len += w > 0 ? (size_t)w : 0;
        }
        Log("display: resolution table candidate at NieRAutomata.exe+0x%llx (stride %d, %d entries): %s",
            (unsigned long long)(t[i].addr - g_exeBase), t[i].stride, t[i].n, buf);
        if (t[i].has1080 && t[i].n >= 5) {
            good = i;
            goodCount++;
        }
    }
    if (!patch) return;
    if (goodCount != 1) {
        Log("display: ExtraResolutions=2: %d convincing tables, not patching anything (send me this log)", goodCount);
        return;
    }
    // replace the smallest entries, keeping 1600x900 / 1920x1080 / the monitor size / anything bigger
    int mw, mh, mx, my;
    MonitorSize(nullptr, &mw, &mh, &mx, &my);
    ResTable& T = t[good];
    DWORD old;
    if (!VirtualProtect((void*)T.addr, (SIZE_T)T.n * T.stride, PAGE_READWRITE, &old)) {
        Log("display: could not unprotect the resolution table (%u)", (unsigned)GetLastError());
        return;
    }
    int replaced = 0;
    for (int k = 0; k < ExtraCount((UINT)mw, (UINT)mh); k++) {
        int smallest = -1;
        int64_t sa = 0;
        for (int j = 0; j < T.n; j++) {
            int32_t* r = (int32_t*)(T.addr + (uintptr_t)j * T.stride);
            if ((r[0] == 1600 && r[1] == 900) || (r[0] == 1920 && r[1] == 1080) || (r[0] >= mw && r[1] >= mh))
                continue;
            int64_t a = (int64_t)r[0] * r[1];
            if (smallest < 0 || a < sa) {
                smallest = j;
                sa = a;
            }
        }
        if (smallest < 0) break;
        int32_t* r = (int32_t*)(T.addr + (uintptr_t)smallest * T.stride);
        int32_t nw = (int32_t)Round8(mw * kFactors[k]), nh = (int32_t)Round8(mh * kFactors[k]);
        Log("display: resolution table entry %d: %dx%d -> %dx%d", smallest, r[0], r[1], nw, nh);
        r[0] = nw;
        r[1] = nh;
        replaced++;
    }
    VirtualProtect((void*)T.addr, (SIZE_T)T.n * T.stride, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)T.addr, (SIZE_T)T.n * T.stride);
    Log("display: ExtraResolutions=2: %d entries of the game's resolution table replaced", replaced);
}

// --- keep the game window on the monitor when rendering above its resolution ----------------
bool IsGameTopLevel(HWND hwnd) {
    if (!hwnd) return false;
    if (hwnd == g_gameHwnd) return true;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return pid == GetCurrentProcessId() && !(GetWindowLongW(hwnd, GWL_STYLE) & WS_CHILD);
}

bool ClampWindow(HWND hwnd, int* x, int* y, int* cx, int* cy) {
    if (!g_cfg.extraResolutions || !IsGameTopLevel(hwnd)) return false;
    int mw, mh, mx, my;
    MonitorSize(hwnd, &mw, &mh, &mx, &my);
    // a window a little larger than the monitor (windowed mode at the monitor's size, with its
    // borders) is normal; only supersampling sizes (125%+) are pulled back
    if (*cx * 10 <= mw * 11 && *cy * 10 <= mh * 11) return false;
    if (LogBudget(&g_bWin, 4))
        Log("display: keeping the game window at the monitor size (%dx%d instead of %dx%d)", mw, mh, *cx, *cy);
    *x = mx;
    *y = my;
    *cx = mw;
    *cy = mh;
    return true;
}

BOOL WINAPI H_SetWindowPos(HWND hwnd, HWND after, int x, int y, int cx, int cy, UINT flags) {
    if (!(flags & SWP_NOSIZE) && ClampWindow(hwnd, &x, &y, &cx, &cy)) flags &= ~SWP_NOMOVE;
    return o_SetWindowPos(hwnd, after, x, y, cx, cy, flags);
}
BOOL WINAPI H_MoveWindow(HWND hwnd, int x, int y, int cx, int cy, BOOL repaint) {
    ClampWindow(hwnd, &x, &y, &cx, &cy);
    return o_MoveWindow(hwnd, x, y, cx, cy, repaint);
}

void HookExport(const wchar_t* dll, const char* name, void* detour, void* orig) {
    HMODULE m = GetModuleHandleW(dll);
    if (!m) m = LoadLibraryW(dll);
    void* target = m ? (void*)GetProcAddress(m, name) : nullptr;
    if (target) HookFunction(target, detour, (void**)orig, name);
}
}  // namespace

// At load time (DllMain): hooks that must exist before the game's first display queries. Only
// patches code (no calls into other DLLs), so it is safe under the loader lock.
void DisplayEarlyHooks() {
    HMODULE exe = GetModuleHandleW(nullptr);
    auto* dos = (IMAGE_DOS_HEADER*)exe;
    auto* nt = (IMAGE_NT_HEADERS64*)((char*)exe + dos->e_lfanew);
    g_exeBase = (uintptr_t)exe;
    g_exeEnd = g_exeBase + nt->OptionalHeader.SizeOfImage;
    struct H {
        const wchar_t* dll;
        const char* name;
        void* detour;
        void** orig;
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
    for (auto& h : hooks) {
        HMODULE m = GetModuleHandleW(h.dll);  // never loads anything here
        void* target = m ? (void*)GetProcAddress(m, h.name) : nullptr;
        if (!target) continue;
        if (m == GetModuleHandleW(L"dxgi.dll")) g_earlyDxgi = true;
        if (HookFunction(target, h.detour, h.orig, h.name)) g_earlyHooks++;
    }
}

// Installed once from EnsureInit (first call into the DLL, outside the loader lock).
void DisplayInstallHooks() {
    static volatile LONG done = 0;
    if (InterlockedCompareExchange(&done, 1, 0) != 0) return;
    if (!g_exeBase) DisplayEarlyHooks();
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    if (!o_CreateDXGIFactory && dxgi) {  // dxgi wasn't loaded at start: hook the factory functions now
        HookFunction((void*)GetProcAddress(dxgi, "CreateDXGIFactory"), (void*)&H_CreateDXGIFactory,
                     (void**)&o_CreateDXGIFactory, "CreateDXGIFactory");
        HookFunction((void*)GetProcAddress(dxgi, "CreateDXGIFactory1"), (void*)&H_CreateDXGIFactory1,
                     (void**)&o_CreateDXGIFactory1, "CreateDXGIFactory1");
    }
    PFN_CreateFactory create = o_CreateDXGIFactory ? o_CreateDXGIFactory
                               : dxgi ? (PFN_CreateFactory)(void*)GetProcAddress(dxgi, "CreateDXGIFactory")
                                      : nullptr;
    IDXGIFactory* f = nullptr;
    if (create && SUCCEEDED(create(__uuidof(IDXGIFactory), (void**)&f)) && f) {
        void** fv = *(void***)f;
        HookFunction(fv[10], (void*)&H_CreateSwapChain, (void**)&o_CreateSwapChain, "IDXGIFactory::CreateSwapChain");
        IDXGIAdapter* a = nullptr;
        IDXGIOutput* o = nullptr;
        if (SUCCEEDED(f->EnumAdapters(0, &a)) && a && SUCCEEDED(a->EnumOutputs(0, &o)) && o) {
            void** ov = *(void***)o;
            HookFunction(ov[8], (void*)&H_GetDisplayModeList, (void**)&o_GetDisplayModeList,
                         "IDXGIOutput::GetDisplayModeList");
            HookFunction(ov[9], (void*)&H_FindClosest, (void**)&o_FindClosest, "IDXGIOutput::FindClosestMatchingMode");
            IDXGIOutput1* o1 = nullptr;
            if (SUCCEEDED(o->QueryInterface(__uuidof(IDXGIOutput1), (void**)&o1)) && o1) {
                void** ov1 = *(void***)o1;
                HookFunction(ov1[19], (void*)&H_GetDisplayModeList1, (void**)&o_GetDisplayModeList1,
                             "IDXGIOutput1::GetDisplayModeList1");
                o1->Release();
            }
        } else {
            Log("display: no monitor output found on the first GPU; mode list hooks skipped");
        }
        if (o) o->Release();
        if (a) a->Release();
        f->Release();
    }
    HookExport(L"user32.dll", "SetWindowPos", (void*)&H_SetWindowPos, &o_SetWindowPos);
    HookExport(L"user32.dll", "MoveWindow", (void*)&H_MoveWindow, &o_MoveWindow);
    int mw, mh, mx, my;
    MonitorSize(nullptr, &mw, &mh, &mx, &my);
    if (g_cfg.extraResolutions) {
        int ne = ExtraCount((UINT)mw, (UINT)mh);
        if (ne > 0) {
            g_spoofW = Round8(mw * kFactors[ne - 1]);
            g_spoofH = Round8(mh * kFactors[ne - 1]);
        }
    }
    Log("display: monitor %dx%d; extra resolutions %s; %d display hooks in place from load time%s", mw, mh,
        g_cfg.extraResolutions >= 2 ? "ON + resolution table patch (125-300%, use windowed/borderless)"
        : g_cfg.extraResolutions    ? "ON (125-300%, use windowed/borderless)"
                                    : "off",
        g_earlyHooks, g_earlyDxgi ? "" : " (dxgi.dll was not loaded yet at that point)");
    LogAndPatchTables(g_cfg.extraResolutions >= 2);
}

// First Present: remember the game window; log the backbuffer against the monitor.
void DisplayOnFirstPresent(IDXGISwapChain* sc) {
    DXGI_SWAP_CHAIN_DESC d;
    if (FAILED(sc->GetDesc(&d))) return;
    g_gameHwnd = d.OutputWindow;
    Log("display: game swapchain %ux%u, %s, swap effect %d, %u buffers", d.BufferDesc.Width, d.BufferDesc.Height,
        d.Windowed ? "windowed/borderless" : "exclusive fullscreen", (int)d.SwapEffect, d.BufferCount);
}

// Swapchain methods, hooked through the dummy swapchain made for the Present hook.
void DisplayHookSwapChain(IDXGISwapChain* sc) {
    static volatile LONG done = 0;
    if (InterlockedCompareExchange(&done, 1, 0) != 0) return;
    void** v = *(void***)sc;
    HookFunction(v[10], (void*)&H_SetFullscreenState, (void**)&o_SetFullscreenState, "IDXGISwapChain::SetFullscreenState");
    HookFunction(v[13], (void*)&H_ResizeBuffers, (void**)&o_ResizeBuffers, "IDXGISwapChain::ResizeBuffers");
    HookFunction(v[14], (void*)&H_ResizeTarget, (void**)&o_ResizeTarget, "IDXGISwapChain::ResizeTarget");
}
