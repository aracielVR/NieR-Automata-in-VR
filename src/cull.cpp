// cull.cpp - widens the field of view NieR culls (and lays out) its scene with.
//
// The game decides what to draw from its own camera: about 50 degrees tall, 16:9. In the headset
// you see ~95 x 109 degrees and can look around, so things outside the game's view were missing
// (NPC hair, screen contents, lights at the edges). While stereo is rendering, the game camera's
// field-of-view setter is hooked to hand the game a wider value (CullingFov), so its own camera
// update builds a wider culling frustum. The renderer's projection is replaced per eye anyway
// (stereo.cpp), so the VR image keeps its shape.
//
// Also here: a background memory scan for the camera object and a hardware-breakpoint "watch"
// (Ctrl+F5) that logs which game code reads/writes the camera fov: the diagnostic that found the
// setter (0.3.10).
#include "common.h"

namespace {
struct Cand {
    uintptr_t fov;        // address of the fov float; the projection starts 564 bytes before
    float origFov, origP00, origP11;
    bool wide;            // our values are in place
};
SRWLOCK g_lock = SRWLOCK_INIT;
Cand g_cands[8];
int g_count;
volatile LONG g_scanning;
DWORD g_lastScan;
int g_invalidRun;
bool g_loggedEffect;

bool ReadMem(uintptr_t a, void* out, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), (void*)a, out, n, &got) && got == n;
}
float Abs(float v) { return v < 0 ? -v : v; }

// p: 16 floats of projection, f: the fov 564 bytes after its start
bool Signature(const float* p, float f) {
    if (!(f > 0.3f && f < 2.9f) || !(p[0] > 0.05f) || !(p[5] > 0.05f)) return false;
    float want = 1.0f / pure::ftan(f * 0.5f);
    if (Abs(p[5] - want) > 2e-3f * want) return false;
    float aspect = p[5] / p[0];
    if (!(aspect > 1.0f && aspect < 3.5f)) return false;
    if (Abs(p[1]) > 1e-6f || Abs(p[2]) > 1e-6f || Abs(p[3]) > 1e-6f || Abs(p[4]) > 1e-6f) return false;
    if (Abs(p[15]) > 1e-6f) return false;
    return Abs(p[14] + 1) < 1e-5f || Abs(p[11] + 1) < 1e-5f;
}

DWORD WINAPI ScanThread(void*) {
    static uint8_t chunk[(1 << 20) + 1024];
    Cand found[8];
    int n = 0;
    uint64_t scanned = 0;
    DWORD t0 = GetTickCount();
    MEMORY_BASIC_INFORMATION mbi;
    for (uint8_t* addr = nullptr; VirtualQuery(addr, &mbi, sizeof(mbi)) == sizeof(mbi) && n < 8;
         addr = (uint8_t*)mbi.BaseAddress + mbi.RegionSize) {
        if ((uintptr_t)addr >= 0x7FF000000000ull) break;
        bool rw = mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE &&
                  !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) && (mbi.Protect & PAGE_READWRITE);
        if (!rw || mbi.RegionSize > (1ull << 31)) continue;
        uint8_t* base = (uint8_t*)mbi.BaseAddress;
        if (base <= chunk && chunk < base + mbi.RegionSize) continue;
        if (GetTickCount() - t0 > 30000) break;
        // read in 1 MB steps, overlapping by 576 bytes so the 564-byte pattern never straddles
        for (size_t off = 0; off < mbi.RegionSize && n < 8; off += (1 << 20) - 576) {
            size_t len = mbi.RegionSize - off < sizeof(chunk) ? mbi.RegionSize - off : sizeof(chunk);
            SIZE_T got = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), base + off, chunk, len, &got) || got < 600) break;
            scanned += got;
            for (size_t i = 564; i + 4 <= got && n < 8; i += 4) {
                float f;
                memcpy(&f, chunk + i, 4);
                if (!(f > 0.3f && f < 2.9f)) continue;
                float p[16];
                memcpy(p, chunk + i - 564, 64);
                if (!Signature(p, f)) continue;
                uintptr_t at = (uintptr_t)(base + off + i);
                bool dup = false;
                for (int k = 0; k < n; k++) dup |= found[k].fov == at;
                if (dup) continue;
                found[n++] = {at, f, p[0], p[5], false};
            }
        }
    }
    AcquireSRWLockExclusive(&g_lock);
    memcpy(g_cands, found, sizeof(found));
    g_count = n;
    g_invalidRun = 0;
    ReleaseSRWLockExclusive(&g_lock);
    Log("culling: scan found %d camera%s (%u MB in %u ms)%s", n, n == 1 ? "" : "s", (unsigned)(scanned >> 20),
        (unsigned)(GetTickCount() - t0), n ? "" : "; the game's culling stays as it is");
    for (int k = 0; k < n; k++)
        Log("culling: camera %d: field of view %.1f deg, aspect %.3f", k,
            (double)(found[k].origFov * 57.29578f), (double)(found[k].origP11 / found[k].origP00));
    InterlockedExchange(&g_scanning, 0);
    return 0;
}

void StartScan() {
    if (InterlockedCompareExchange(&g_scanning, 1, 0) != 0) return;
    g_lastScan = GetTickCount();
    HANDLE h = CreateThread(nullptr, 1 << 20, ScanThread, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
    else InterlockedExchange(&g_scanning, 0);
}
}  // namespace

// Once per frame on the game thread (StereoBeginFrame). wide: stereo is rendering this frame.
bool CullingWatchActive();
void CullingWatch();

// ---------------------------------------------------------------------------------------------
// The fix (0.3.11): the 0.3.10 watch showed the camera controller calls a tiny setter every frame,
//   NieRAutomata.exe+0x27ace0:  mov rax,[rcx]; movss [rax+0x274],xmm1; mov dword [rax+0x284],1; ret
// (fov at +0x274, "dirty" flag at +0x284), and the camera update (exe+0x27aaa2) rebuilds the
// projection and the culling frustum from that fov when the flag is set. So our writes at Present
// were overwritten before the next frame was culled. Hooking the setter widens the value the game
// itself uses, right where it is set. Only the main 16:9 cameras are widened (render-to-texture
// cameras are square), and only while stereo is rendering.
// ---------------------------------------------------------------------------------------------
typedef void (*PFN_SetFov)(void** cam, float fov);
PFN_SetFov o_SetFov;
volatile LONG g_wideOn;
volatile LONG g_widened, g_passed;
bool g_hookOk;

volatile float g_gameFov;  // the last field of view the game asked for (main camera, radians)
// fovstubs.S: the main camera object (0 = off) and the widened fov (radians) for the sites below.
extern "C" {
extern void* volatile nvr_fov_obj;
extern void* volatile nvr_fov_site_obj[4];  // per read site (Shift+F5 test switches sites off)
extern volatile LONG nvr_head_cull;          // fovstubs.S calls nvr_head_cull_builder when set
extern volatile float nvr_fov_wide;
extern volatile float nvr_fov_wide_b;  // the frustum builder's (site B): headset-sized with head culling
extern void* nvr_fov_resume[4];
extern void* nvr_fov_orig[4];
extern volatile LONG64 nvr_fov_hits[4];
void nvr_fov_stub0();
void nvr_fov_stub1();
void nvr_fov_stub2();
void nvr_fov_stub3();
void nvr_post_builder();
extern void* nvr_post_orig;
}
int g_sitesOk;     // fov read sites redirected (0 = old way: widen the value SetFov stores)
bool g_siteA;      // the projection rebuild site is among them (required for the new way)

// ---------------------------------------------------------------------------------------------
// Head-direction culling (0.3.22). A symmetric frustum can't reach past 90 deg to a side, and at
// CullingFov=170 it reaches ~87: turn your head further than ~35-40 deg from where the game camera
// points and the edge of your view leaves it (0.3.21 user report). The frustum builder after site B
// gets LookAt(eye, target, up) as three pointers (0.3.20/0.3.21 logs: r14 = eye, rbp = target,
// rsi = up, values with leftover w lanes, i.e. a temporary on the stack). fovstubs.S calls this
// right before that call: the target and up are turned by the eye's rotation relative to the game
// camera (StereoHeadRelative), so the culling view looks where you look. Only done when the three
// vectors are on this thread's stack: then they're a copy made for this call and the camera itself
// isn't changed. If they're not, head culling switches itself off and says so in the log.
// ---------------------------------------------------------------------------------------------
volatile LONG g_hcDone, g_hcSkipped;
bool g_headCullRefused, g_hcLogged;
// 0.3.22 log: the look-at is a fixed block in the exe's data (not on the stack). The builder call is
// the camera update's last act (code after site B: two movss to the stack, call, epilogue), so
// 0.3.23 hooks the instruction after the call and puts the original target/up back there.
bool g_hcRestoreOk;      // the after-call hook is in place
float* volatile g_hcBlock;  // block changed and not yet restored (0 = none)
DWORD g_hcTid;
float g_hcSaved[6];
volatile LONG g_hcRestored;

static bool OnThisStack(const void* p, size_t n) {
    NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
    const uint8_t* a = (const uint8_t*)p;
    return a >= (const uint8_t*)tib->StackLimit && a + n <= (const uint8_t*)tib->StackBase;
}

extern "C" void nvr_head_cull_builder(float* eye, float* target, float* up) {
    if (g_headCullRefused) return;
    bool stack = OnThisStack(eye, 48);
    if (target != eye + 4 || up != eye + 8 || (!stack && !g_hcRestoreOk)) {
        g_headCullRefused = true;
        nvr_head_cull = 0;
        Log("culling: head direction: the frustum builder's look-at (eye %p, target %p, up %p) isn't laid out as "
            "expected or can't be put back after the call; head culling stays off (send me this log)",
            (void*)eye, (void*)target, (void*)up);
        return;
    }
    static bool whereLogged;
    if (!whereLogged) {
        whereLogged = true;
        char where[300];
        DescribeAddress(eye, where, sizeof(where));
        Log("culling: head direction: look-at block at %s (%s)", where,
            stack ? "on the stack" : "game data: put back right after the call");
    }
    pure::M4 rel;
    float t[3], u[3];
    if (!StereoHeadRelative(&rel) || !pure::head_lookat(eye, target, up, rel, t, u)) {
        InterlockedIncrement(&g_hcSkipped);
        return;
    }
    if (!g_hcLogged) {
        g_hcLogged = true;
        Log("culling: head direction: first turn; eye %.2f %.2f %.2f, target %.2f %.2f %.2f -> %.2f %.2f %.2f, "
            "up %.3f %.3f %.3f -> %.3f %.3f %.3f",
            (double)eye[0], (double)eye[1], (double)eye[2], (double)target[0], (double)target[1], (double)target[2],
            (double)t[0], (double)t[1], (double)t[2], (double)up[0], (double)up[1], (double)up[2], (double)u[0],
            (double)u[1], (double)u[2]);
    }
    if (!stack) {
        memcpy(g_hcSaved, target, 12);
        memcpy(g_hcSaved + 3, up, 12);
        g_hcTid = GetCurrentThreadId();
        g_hcBlock = eye;
    }
    memcpy(target, t, 12);
    memcpy(up, u, 12);
    InterlockedIncrement(&g_hcDone);
}

// fovstubs.S (nvr_post_builder): right after the frustum builder call returns.
extern "C" void nvr_head_cull_restore() {
    float* b = g_hcBlock;
    if (!b || g_hcTid != GetCurrentThreadId()) return;
    memcpy(b + 4, g_hcSaved, 12);
    memcpy(b + 8, g_hcSaved + 3, 12);
    g_hcBlock = nullptr;
    InterlockedIncrement(&g_hcRestored);
}

// The getter: `mov rax,[rcx]; movss xmm0,[rax+0x274]; ret`, ~11 calls a frame from 5-6 places.
// 0.3.17 left it alone and culling went back to the game's own fov, so culling frusta are built
// from its result. But the camera controller also reads it to ease towards its target fov, which is
// how widening polluted the game's own fov (0.3.16, 0.3.18). 0.3.18 guessed the controller by
// distance to the SetFov call: wrong (0.3.18 log: SetFov from exe+0x73e272, getter callers at
// +0x7807ed/+0x7808df 4x a frame, +0x75164f/+0x7397de/+0x729b49 1x a frame, none nearby).
// 0.3.19: the controller is the caller that reads the fov last before SetFov, on SetFov's thread.
// For the first 60 SetFov calls with stereo on every caller gets the real value while the mod
// counts who that is; then that caller keeps the real value and all others get the wide one.
typedef float (*PFN_GetFov)(void** cam);
PFN_GetFov o_GetFov;
bool g_getterOk;
struct GetterCaller {
    void* ra;
    volatile LONG count;
    volatile LONG votes;  // times it was the last reader before SetFov
    bool rejected;        // picked before, but the game's own fov still drifted wide: not the easing
    bool real;            // FovRealCallers: always the game's own fov
    unsigned off;         // exe offset of the return address
};
// Shift+F5 test steps (0.3.23): which reader is the culling? Each step takes the wide fov away from
// one candidate; the step where things vanish much sooner names it.
//   0 normal  1 getter callers +0x7807ed/+0x7808df real  2 getter caller +0x75164f real
//   3 frustum builder (site B) real  4 render camera copies (sites C/D) real
//   5 projection rebuild (site A) wide (0.3.22 default; Pod aims up)  6 nothing widened
volatile LONG g_fovTest;
const int kTestSteps = 7;
bool GetterReal(const struct GetterCaller* e);
bool SiteOn(int i) {
    LONG t = g_fovTest;
    if (t == 6) return false;
    if (i == 0) return t == 5 || g_cfg.wideProjection;
    if (i == 1) return t != 3;
    return t != 4;
}
GetterCaller g_callers[16];
volatile LONG g_callerCount;
SRWLOCK g_callerLock = SRWLOCK_INIT;
volatile DWORD g_setFovThread;
GetterCaller* volatile g_lastOnSetFovThread;
GetterCaller* volatile g_controller;  // decided after 60 votes
volatile LONG g_votes;
bool g_controllerLogged;
void* g_knownEasingRa;   // from the ini (FovEasingCaller): the easing caller found in an earlier run
bool g_easingSaved;

GetterCaller* NoteCaller(void* ra) {
    LONG n = g_callerCount;
    for (LONG i = 0; i < n && i < 16; i++)
        if (g_callers[i].ra == ra) return &g_callers[i];
    AcquireSRWLockExclusive(&g_callerLock);
    GetterCaller* e = nullptr;
    n = g_callerCount;
    for (LONG i = 0; i < n && i < 16; i++)
        if (g_callers[i].ra == ra) e = &g_callers[i];
    if (!e && n < 16) {
        e = &g_callers[n];
        e->ra = ra;
        e->count = 0;
        e->votes = 0;
        unsigned off = (unsigned)((uint8_t*)ra - (uint8_t*)GetModuleHandleW(nullptr));
        for (int i = 0; i < g_cfg.fovRealCount; i++)
            if (g_cfg.fovRealCallers[i] == off) e->real = true;
        e->off = off;
        InterlockedIncrement(&g_callerCount);
        if (ra == g_knownEasingRa && !g_controller) {
            g_controller = e;  // known from an earlier run: no search needed
            g_easingSaved = true;
        }
    }
    ReleaseSRWLockExclusive(&g_callerLock);
    return e;
}

float H_GetFov(void** cam) {
    float v = o_GetFov(cam);
    void* obj = nvr_fov_obj;
    if (obj && cam && *cam == obj) {
        GetterCaller* e = NoteCaller(__builtin_return_address(0));
        if (e) {
            InterlockedIncrement(&e->count);
            if (GetCurrentThreadId() == g_setFovThread) g_lastOnSetFovThread = e;
            GetterCaller* ctl = g_controller;
            if (ctl && e != ctl && !GetterReal(e)) return nvr_fov_wide;
        }
    }
    return v;
}

// From H_SetFov while widening: count which getter caller read the fov just before.
void VoteController() {
    GetterCaller* last = g_lastOnSetFovThread;
    g_lastOnSetFovThread = nullptr;
    if (g_controller || !last) return;
    InterlockedIncrement(&last->votes);
    // 0.3.24: with the caller known from the ini, wait for it (it only starts reading once the
    // gameplay camera runs; 0.3.21-0.3.23 voted first, picked wrong callers, the game's own fov
    // drifted wide meanwhile and Pod 042 aimed at the sky for the first ~15 s)
    if (InterlockedIncrement(&g_votes) < (g_knownEasingRa ? 3600 : 60)) return;
    GetterCaller* best = nullptr;
    LONG n = g_callerCount;
    for (LONG i = 0; i < n && i < 16; i++)
        if (!g_callers[i].rejected && (!best || g_callers[i].votes > best->votes)) best = &g_callers[i];
    g_controller = best;
    g_controllerLogged = false;
}

// If the game's own fov still climbs towards the wide one with a controller picked, the pick was
// wrong: drop it and count again without it.
int g_driftRun;
void CheckDrift(float requested, float want) {
    GetterCaller* ctl = g_controller;
    if (!ctl) {
        g_driftRun = 0;
        return;
    }
    g_driftRun = requested > want - 0.26f ? g_driftRun + 1 : 0;  // within 15 deg of the wide fov
    if (g_driftRun < 30) return;
    g_driftRun = 0;
    ctl->rejected = true;
    LONG n = g_callerCount;
    for (LONG i = 0; i < n && i < 16; i++) g_callers[i].votes = 0;
    g_votes = 0;
    g_controller = nullptr;
    char where[300];
    DescribeAddress(ctl->ra, where, sizeof(where));
    Log("culling: the game's own fov kept drifting wide with %s as the camera's easing; looking again", where);
    g_easingSaved = false;
}

// 0.3.24: with head culling the frustum is centred on where you look, so it only has to hold the
// headset's view plus a margin (~135 deg tall instead of 170: far fewer objects drawn).
volatile LONG g_hcFovLogged;
float HeadCullFov(float want) {
    if (!nvr_head_cull || g_cfg.headCullMargin <= 0) return want;
    float f = StereoHeadCullFov(g_cfg.headCullMargin);
    if (!(f > 0.5f) || f >= want) return want;
    if (!InterlockedExchange(&g_hcFovLogged, 1))
        Log("culling: head direction: culling view %.0f deg tall (headset view + %.0f deg margin) instead of %.0f",
            (double)(f * 57.29578f), (double)g_cfg.headCullMargin, (double)(want * 57.29578f));
    return f;
}

void H_SetFov(void** cam, float fov) {
    g_setFovThread = GetCurrentThreadId();
    if (cam && *cam) {
        const float* obj = (const float*)*cam;
        float p00 = obj[16], p11 = obj[21];  // the camera's projection lives at +0x40 (P00, P11)
        float aspect = p00 > 1e-4f ? p11 / p00 : 0;
        bool main = aspect > 1.2f && aspect < 3.5f;
        if (main) g_gameFov = fov;
        float want = g_cfg.cullFov * 0.0174533f;
        if (g_wideOn && main && want > fov) {
            InterlockedIncrement(&g_widened);
            if (g_siteA) {
                // 0.3.17: the object keeps the game's own fov; only its projection/frustum reads
                // see the wide one
                if (g_getterOk && nvr_fov_obj == *cam) {
                    VoteController();
                    CheckDrift(fov, want);
                }
                nvr_fov_wide = want;
                nvr_fov_wide_b = HeadCullFov(want);
                nvr_fov_obj = *cam;
                for (int i = 0; i < 4; i++) nvr_fov_site_obj[i] = SiteOn(i) ? *cam : nullptr;
            } else {
                fov = want;
            }
        } else if (g_wideOn) {
            InterlockedIncrement(&g_passed);
        }
    }
    o_SetFov(cam, fov);
}

static const uint8_t kSetFovBytes[] = {0x48, 0x8b, 0x01, 0xf3, 0x0f, 0x11, 0x88, 0x74, 0x02, 0x00, 0x00,
                                       0xc7, 0x80, 0x84, 0x02, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xc3};

// Where the camera's fov field (+0x274) is read (0.3.10 hardware-breakpoint watch):
//   exe+0x27aaaf  camera update, when dirty: movss xmm1,[rbx+0x274] -> * k -> projection
//   exe+0x27abf7  camera update: movss xmm1,[rax+0x274] -> argument of a frustum builder
//   exe+0x22392d, exe+0x223fd6  render threads: movss xmm0,[rax+0x274] -> * k -> [rbx+0xd4]
//   exe+0x19415a  worker, ~70x per frame: model detail (screen size of each object)
//   exe+0x27ad44  getter (camera controller: eases the fov from the current value)
// 0.3.11-0.3.16 widened the stored value, so all of them saw 120 deg: the controller then eased
// from 120 (the game "asked" for 113-119, 0.3.16 log) and model detail dropped. Now only the first
// four are redirected (fovstubs.S); each is found by its bytes plus neighbours and must match once.
struct FovSite {
    const char* what;
    uint8_t bytes[28];
    int len, at;  // pattern length, offset of the movss in it
};
static const FovSite kSites[4] = {
    {"projection rebuild",
     {0x83, 0xbb, 0x84, 0x02, 0x00, 0x00, 0x00, 0x0f, 0x84, 0xb4, 0x00, 0x00, 0x00, 0xf3, 0x0f, 0x10, 0x8b, 0x74, 0x02,
      0x00, 0x00, 0xf3, 0x0f, 0x59, 0x0d},
     25, 13},
    {"frustum builder",
     {0xf3, 0x0f, 0x10, 0x80, 0x70, 0x02, 0x00, 0x00, 0xf3, 0x0f, 0x11, 0x4c, 0x24, 0x30, 0xf3, 0x0f, 0x10, 0x88, 0x74,
      0x02, 0x00, 0x00, 0xf3, 0x0f, 0x11, 0x44, 0x24, 0x28},
     28, 14},
    {"render camera 1",
     {0xf3, 0x0f, 0x10, 0x80, 0x74, 0x02, 0x00, 0x00, 0xf3, 0x0f, 0x59, 0x05, 0x2b, 0x6f, 0xb9, 0x00, 0xf3, 0x0f, 0x11,
      0x83, 0xd4, 0x00, 0x00, 0x00},
     24, 0},
    {"render camera 2",
     {0xf3, 0x0f, 0x10, 0x80, 0x74, 0x02, 0x00, 0x00, 0xf3, 0x0f, 0x59, 0x05, 0x82, 0x68, 0xb9, 0x00, 0xf3, 0x0f, 0x11,
      0x86, 0xd4, 0x00, 0x00, 0x00},
     24, 0},
};

// ---------------------------------------------------------------------------------------------
// Diagnostics for head-direction culling (0.3.20): what the frustum builder after site B gets
// (rdx, r8, r9: pointers? vectors?) and who asks the camera for its matrix (getter returning
// obj+0xc0, right after the fov getter). Logged a few times; the game is not changed.
// ---------------------------------------------------------------------------------------------
extern "C" void* volatile nvr_diag_regs[3];  // fovstubs.S: r14, rbp, rsi at the frustum builder call
typedef void* (*PFN_GetMat)(void** cam);
PFN_GetMat o_GetMat;
volatile LONG g_builderLogs;
struct MatCaller {
    void* ra;
    volatile LONG count;
};
MatCaller g_matCallers[24];
volatile LONG g_matCallerCount;
SRWLOCK g_matLock = SRWLOCK_INIT;

bool Readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    return (const uint8_t*)p + n <= (const uint8_t*)mbi.BaseAddress + mbi.RegionSize;
}

void LogFloats(const char* what, const void* p) {
    uintptr_t v = (uintptr_t)p;
    if (v < 0x10000 || !Readable(p, 64)) {
        Log("culling:   %s = 0x%llx (not readable memory)", what, (unsigned long long)v);
        return;
    }
    const float* f = (const float*)p;
    char line[400];
    size_t at = 0;
    for (int i = 0; i < 16 && at + 20 < sizeof(line); i++)
        at += pure::format(line + at, sizeof(line) - at, " %.3f", (double)f[i]);
    Log("culling:   %s =%s", what, line);
}

// From CullingUpdate (game thread), a few times: the frustum builder's pointer arguments.
void LogBuilderArgs() {
    if (!nvr_fov_obj || !LogBudget(&g_builderLogs, 6)) return;
    float pos[3], fwd[3], up[3];
    if (StereoGameCamera(pos, fwd, up))
        Log("culling: frustum builder arguments; game camera at %.2f %.2f %.2f, forward %.3f %.3f %.3f, up %.3f %.3f %.3f",
            (double)pos[0], (double)pos[1], (double)pos[2], (double)fwd[0], (double)fwd[1], (double)fwd[2],
            (double)up[0], (double)up[1], (double)up[2]);
    LogFloats("rdx (r14)", nvr_diag_regs[0]);
    LogFloats("r8 (rbp)", nvr_diag_regs[1]);
    LogFloats("r9 (rsi)", nvr_diag_regs[2]);
    const uint8_t* obj = (const uint8_t*)nvr_fov_obj;
    for (int off = 0x80; off < 0x280; off += 0x40) {
        char what[48];
        pure::format(what, sizeof(what), "camera object +0x%x", off);
        LogFloats(what, obj + off);
    }
}

void* H_GetMat(void** cam) {
    void* obj = nvr_fov_obj;
    if (obj && cam && *cam == obj) {
        void* ra = __builtin_return_address(0);
        LONG n = g_matCallerCount;
        bool found = false;
        for (LONG i = 0; i < n && i < 24; i++)
            if (g_matCallers[i].ra == ra) {
                InterlockedIncrement(&g_matCallers[i].count);
                found = true;
                break;
            }
        if (!found) {
            AcquireSRWLockExclusive(&g_matLock);
            n = g_matCallerCount;
            if (n < 24) {
                g_matCallers[n].ra = ra;
                g_matCallers[n].count = 1;
                InterlockedIncrement(&g_matCallerCount);
            }
            ReleaseSRWLockExclusive(&g_matLock);
        }
    }
    return o_GetMat(cam);
}

static uint8_t* FindOnce(HMODULE exe, const uint8_t* pat, int len, int* matches) {
    auto* dos = (IMAGE_DOS_HEADER*)exe;
    auto* nt = (IMAGE_NT_HEADERS64*)((char*)exe + dos->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    uint8_t* found = nullptr;
    *matches = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uint8_t* b = (uint8_t*)exe + sec[i].VirtualAddress;
        size_t n = sec[i].Misc.VirtualSize;
        for (size_t k = 0; k + (size_t)len <= n; k++)
            if (b[k] == pat[0] && memcmp(b + k, pat, (size_t)len) == 0) {
                found = b + k;
                (*matches)++;
            }
    }
    return *matches == 1 ? found : nullptr;
}

static void FovSitesInstall(HMODULE exe) {
    if (!g_cfg.lodFix) {
        Log("culling: LodFix=0, the camera's stored field of view is widened (model detail drops)");
        return;
    }
    void (*stubs[4])() = {nvr_fov_stub0, nvr_fov_stub1, nvr_fov_stub2, nvr_fov_stub3};
    uint8_t* at[4] = {};
    for (int i = 0; i < 4; i++) {
        int m = 0;
        uint8_t* p = FindOnce(exe, kSites[i].bytes, kSites[i].len, &m);
        if (!p) {
            Log("culling: fov read '%s' %s", kSites[i].what, m ? "found more than once" : "not found");
            continue;
        }
        at[i] = p + kSites[i].at;
    }
    if (!at[0]) {
        Log("culling: without the projection site the stored field of view is widened as before (model detail drops)");
        return;
    }
    if (at[1]) {
        // for later work on head culling: the code after the frustum builder read (its call)
        char hex[2 * 96 + 1];
        static const char kHex[] = "0123456789abcdef";
        for (int k = 0; k < 96; k++) {
            hex[2 * k] = kHex[at[1][k] >> 4];
            hex[2 * k + 1] = kHex[at[1][k] & 15];
        }
        hex[2 * 96] = 0;
        Log("culling: code at the frustum builder read: %s", hex);
    }
    uint8_t* post = nullptr;
    if (at[1]) {
        // movss [rsp+0x28],xmm0; movss [rsp+0x20],xmm1; call builder; mov rbp,[rsp+0x90] <- hooked
        static const uint8_t kArgs[] = {0xf3, 0x0f, 0x11, 0x44, 0x24, 0x28, 0xf3, 0x0f, 0x11, 0x4c, 0x24, 0x20, 0xe8};
        static const uint8_t kAfter[] = {0x48, 0x8b, 0xac, 0x24, 0x90, 0x00, 0x00, 0x00};
        if (memcmp(at[1] + 8, kArgs, sizeof(kArgs)) == 0 && memcmp(at[1] + 25, kAfter, sizeof(kAfter)) == 0)
            post = at[1] + 25;
        else
            Log("culling: head direction: the code after the frustum builder read isn't as expected; head culling off");
    }
    for (int i = 0; i < 4; i++) {
        if (!at[i]) continue;
        nvr_fov_resume[i] = at[i] + 8;
        bool ok = HookFunction(at[i], (void*)stubs[i], &nvr_fov_orig[i], kSites[i].what);
        if (i == 0 && !ok) return;
        if (ok) g_sitesOk++;
        char where[300];
        DescribeAddress(at[i], where, sizeof(where));
        Log("culling: fov read '%s' at %s %s", kSites[i].what, where, ok ? "widened" : "could not be changed");
    }
    if (post) {
        g_hcRestoreOk = HookFunction(post, (void*)nvr_post_builder, &nvr_post_orig, "after frustum builder");
        char where[300];
        DescribeAddress(post, where, sizeof(where));
        Log("culling: head direction: after-call hook at %s %s", where, g_hcRestoreOk ? "in place" : "failed (head culling off)");
    }
    g_siteA = true;
    Log("culling: the camera keeps the game's own field of view; model detail follows it (LodFix=1)");
    static const uint8_t kGetter[] = {0xf3, 0x0f, 0x10, 0x80, 0x6c, 0x02, 0x00, 0x00, 0xc3, 0xcc, 0xcc, 0xcc, 0xcc,
                                      0x48, 0x8b, 0x01, 0xf3, 0x0f, 0x10, 0x80, 0x74, 0x02, 0x00, 0x00, 0xc3};
    int m = 0;
    uint8_t* g = FindOnce(exe, kGetter, (int)sizeof(kGetter), &m);
    if (!g) {
        Log("culling: fov getter %s; culling may stay narrow", m ? "found more than once" : "not found");
        return;
    }
    // diagnostics: the matrix getter right after it (searched before the fov getter is hooked: the
    // pattern starts inside it, 0.3.20 found nothing)
    static const uint8_t kMatGetter[] = {0xf3, 0x0f, 0x10, 0x80, 0x74, 0x02, 0x00, 0x00, 0xc3, 0xcc, 0xcc, 0xcc, 0xcc,
                                         0x48, 0x8b, 0x01, 0x48, 0x05, 0xc0, 0x00, 0x00, 0x00, 0xc3};
    uint8_t* mg = FindOnce(exe, kMatGetter, (int)sizeof(kMatGetter), &m);
    if (g_cfg.fovEasingCaller) g_knownEasingRa = (uint8_t*)exe + g_cfg.fovEasingCaller;
    g_getterOk = HookFunction(g + 13, (void*)&H_GetFov, (void**)&o_GetFov, "fov getter");
    if (mg) HookFunction(mg + 13, (void*)&H_GetMat, (void**)&o_GetMat, "camera matrix getter (diagnostic)");
    Log("culling: diagnostics on: frustum builder arguments recorded, camera matrix getter %s", mg ? "watched" : "not found");
    char where[300];
    DescribeAddress(g + 13, where, sizeof(where));
    Log("culling: fov getter at %s %s", where, g_getterOk ? "hooked: culling gets the wide view, the camera's easing the real one" : "could not be hooked");
}

// Finds the setter by its exact bytes in the exe's code (so another build of the game either
// matches exactly or is left alone).
void CullingInstallHook() {
    if (g_cfg.cullFov <= 1) {
        Log("culling: CullingFov=0, game culling left as it is");
        return;
    }
    HMODULE exe = GetModuleHandleW(nullptr);
    auto* dos = (IMAGE_DOS_HEADER*)exe;
    auto* nt = (IMAGE_NT_HEADERS64*)((char*)exe + dos->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    uint8_t* found = nullptr;
    int matches = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uint8_t* b = (uint8_t*)exe + sec[i].VirtualAddress;
        size_t n = sec[i].Misc.VirtualSize;
        for (size_t k = 0; k + sizeof(kSetFovBytes) <= n; k++)
            if (b[k] == 0x48 && memcmp(b + k, kSetFovBytes, sizeof(kSetFovBytes)) == 0) {
                found = b + k;
                matches++;
            }
    }
    if (matches != 1) {
        Log("culling: camera fov setter %s; culling left as it is (send me the log)",
            matches ? "found more than once" : "not found (different game version?)");
        return;
    }
    g_hookOk = HookFunction(found, (void*)&H_SetFov, (void**)&o_SetFov, "camera SetFov");
    char where[300];
    DescribeAddress(found, where, sizeof(where));
    Log("culling: %s camera fov setter at %s; during stereo the game culls with %.0f deg", g_hookOk ? "hooked" : "could not hook",
        where, (double)g_cfg.cullFov);
    if (g_hookOk) FovSitesInstall(exe);
}

// Game frame times (0.3.24: "jitter/stutter" with head culling), reported with the 30 s summary.
LARGE_INTEGER g_qpf, g_lastFrame;
double g_ftSum, g_ftMax;
LONG g_ftCount, g_ftSlow, g_ftHc;

void CullingUpdate(bool wide) {
    {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (!g_qpf.QuadPart) QueryPerformanceFrequency(&g_qpf);
        if (g_lastFrame.QuadPart && wide) {
            double ms = (double)(now.QuadPart - g_lastFrame.QuadPart) * 1000.0 / (double)g_qpf.QuadPart;
            if (ms < 1000) {
                g_ftSum += ms;
                g_ftCount++;
                if (ms > g_ftMax) g_ftMax = ms;
                if (ms > 25) g_ftSlow++;
                if (nvr_head_cull) g_ftHc++;
            }
        }
        g_lastFrame = now;
    }
    InterlockedExchange(&g_wideOn, wide && g_hookOk && g_cfg.cullFov > 1 ? 1 : 0);
    if (!g_wideOn) {
        nvr_fov_obj = nullptr;
        for (int i = 0; i < 4; i++) nvr_fov_site_obj[i] = nullptr;
    }
    nvr_head_cull = g_wideOn && g_cfg.headCulling && !g_headCullRefused && !g_fovTest ? 1 : 0;
    static DWORD last;
    if (wide && g_hookOk && GetTickCount() - last > 30000) {  // a short report now and then
        last = GetTickCount();
        LONG w = InterlockedExchange(&g_widened, 0), p = InterlockedExchange(&g_passed, 0);
        if (w || p) Log("culling: %ld camera updates widened, %ld left alone (other cameras) in the last 30 s", (long)w, (long)p);
        if (g_ftCount) {
            Log("culling: game frames in stereo: %ld, avg %.1f ms, slowest %.1f ms, %ld slower than 25 ms (head culling on in %ld%%)",
                (long)g_ftCount, g_ftSum / g_ftCount, g_ftMax, (long)g_ftSlow, (long)(g_ftHc * 100 / g_ftCount));
            {
                char info[160];
                pure::format(info, sizeof(info), "Game frame time %.1f ms average, slowest %.1f ms, %ld slower than 25 ms",
                             g_ftSum / g_ftCount, g_ftMax, (long)g_ftSlow);
                MenuSetInfo(2, info);
            }
            g_ftSum = g_ftMax = 0;
            g_ftCount = g_ftSlow = g_ftHc = 0;
        }
        if (g_siteA) {
            LONG64 h[4];
            for (int i = 0; i < 4; i++) h[i] = InterlockedExchange64(&nvr_fov_hits[i], 0);
            Log("culling: widened reads: projection %lld, frustum %lld, render cameras %lld / %lld; game's own fov %.0f deg",
                (long long)h[0], (long long)h[1], (long long)h[2], (long long)h[3], (double)(g_gameFov * 57.29578f));
            LONG n = g_callerCount;
            GetterCaller* ctl = g_controller;
            for (LONG i = 0; i < n && i < 16; i++) {
                LONG c = InterlockedExchange(&g_callers[i].count, 0);
                if (!c) continue;
                char where[300];
                DescribeAddress(g_callers[i].ra, where, sizeof(where));
                Log("culling:   fov getter caller %s: %ld calls, %ld votes, %s", where, (long)c, (long)g_callers[i].votes,
                    !ctl ? "real value (still finding the camera's easing)"
                         : (&g_callers[i] == ctl ? "real value (camera easing)"
                                                 : (g_callers[i].real ? "real value (FovRealCallers)"
                                                                      : (GetterReal(&g_callers[i]) ? "real value (Shift+F5 test)" : "wide value"))));
            }
            LogBuilderArgs();
            {
                LONG d = InterlockedExchange(&g_hcDone, 0), k = InterlockedExchange(&g_hcSkipped, 0);
                LONG r = InterlockedExchange(&g_hcRestored, 0);
                if (g_cfg.headCulling && !g_headCullRefused)
                    Log("culling: head direction: culling view turned %ld times (%ld put back after), %ld left as the "
                        "game's (no recent eye pose)%s",
                        (long)d, (long)r, (long)k, g_fovTest ? " - off during the Shift+F5 test" : "");
            }
            LONG mn = g_matCallerCount;
            for (LONG i = 0; i < mn && i < 24; i++) {
                LONG c = InterlockedExchange(&g_matCallers[i].count, 0);
                if (!c) continue;
                char where[300];
                DescribeAddress(g_matCallers[i].ra, where, sizeof(where));
                Log("culling:   camera matrix getter caller %s: %ld calls", where, (long)c);
            }
            if (ctl && !g_easingSaved && g_gameFov < nvr_fov_wide - 0.5f) {
                g_easingSaved = true;
                unsigned off = (unsigned)((uint8_t*)ctl->ra - (uint8_t*)GetModuleHandleW(nullptr));
                ConfigSaveEasingCaller(off);
                Log("culling: camera easing caller saved to NieRVR.ini (FovEasingCaller=0x%x): no search next time", off);
            }
            if (ctl && !g_controllerLogged) {
                g_controllerLogged = true;
                char where[300];
                DescribeAddress(ctl->ra, where, sizeof(where));
                Log("culling: the camera's easing reads the fov at %s (%ld of %ld votes)", where, (long)ctl->votes, (long)g_votes);
            }
        }
    }
}

// From PatchScene: the vertical fov the game rendered this frame with (before our replacement).
float CullingGameFov() { return g_gameFov * 57.29578f; }

void CullingObserveGameFov(float deg) {
    if (g_loggedEffect || !g_hookOk) return;
    float want = g_cfg.cullFov;
    if (deg > want - 2 && deg < want + 2) {
        g_loggedEffect = true;
        Log("culling: confirmed, the game's own camera now uses %.0f deg (culling follows it)", (double)deg);
    }
}

// ---------------------------------------------------------------------------------------------
// Diagnostics: who reads and writes the camera's field of view?
// Writing the fov (and P00/P11) reaches the projection the game renders with (0.3.9 log), but not
// its culling. To find the code that culls, hardware data breakpoints (debug registers DR0-DR3 on
// every thread of the process) watch the fov and P11 of the found cameras for two seconds. Each hit
// is logged with the instruction address (NieRAutomata.exe+offset), whether the value changed
// (write) or not (read), and the code bytes around it, plus a dump of the camera object, so the
// culling code can be identified offline.
// ---------------------------------------------------------------------------------------------
#include <tlhelp32.h>
namespace {
struct Hit {
    volatile LONG64 rip;
    volatile LONG count, writes;
    volatile LONG slotMask;
    DWORD tid;
};
Hit g_hits[96];
uintptr_t g_watchAddr[4];
int g_watchCount;
volatile LONG g_watchArmed, g_watchBusy;
PVOID g_veh;

LONG WINAPI WatchHandler(EXCEPTION_POINTERS* ep) {
    if (!g_watchArmed || ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    DWORD64 dr6 = c->Dr6;
    if (!(dr6 & 0xF)) return EXCEPTION_CONTINUE_SEARCH;
    // slots come in pairs on the same address: even = write-only, odd = read or write. (Reading the
    // value here would trigger the breakpoint again, so writes are told apart by the slot.)
    bool wrote = (dr6 & 0x5) != 0;
    LONG64 rip = (LONG64)c->Rip;
    for (auto& h : g_hits) {
        LONG64 cur = h.rip;
        if (cur == 0) cur = InterlockedCompareExchange64(&h.rip, rip, 0), cur = cur ? cur : rip;
        if (cur != rip) continue;
        InterlockedIncrement(&h.count);
        if (wrote) InterlockedIncrement(&h.writes);
        InterlockedOr(&h.slotMask, (LONG)(dr6 & 0xF));
        h.tid = GetCurrentThreadId();
        break;
    }
    c->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

void SetDebugRegs(bool arm) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te;
    te.dwSize = sizeof(te);
    DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    int n = 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (!t) continue;
        if (SuspendThread(t) != (DWORD)-1) {
            CONTEXT ctx = {};
            ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(t, &ctx)) {
                DWORD64 dr7 = 0;
                if (arm) {
                    DWORD64* regs[4] = {&ctx.Dr0, &ctx.Dr1, &ctx.Dr2, &ctx.Dr3};
                    for (int s = 0; s < g_watchCount; s++) {
                        *regs[s] = g_watchAddr[s];
                        dr7 |= 1ull << (2 * s);           // local enable
                        dr7 |= (s & 1 ? 3ull : 1ull) << (16 + 4 * s);  // odd: read or write, even: write
                        dr7 |= 3ull << (18 + 4 * s);      // 4 bytes
                    }
                }
                ctx.Dr7 = dr7;
                ctx.Dr6 = 0;
                if (SetThreadContext(t, &ctx)) n++;
            }
            ResumeThread(t);
        }
        CloseHandle(t);
    }
    CloseHandle(snap);
    Log("culling watch: debug registers %s on %d threads", arm ? "set" : "cleared", n);
}

void LogBytes(const char* what, uintptr_t at, int len) {
    uint8_t b[96];
    if (len > 96) len = 96;
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), (void*)at, b, len, &got) || (int)got != len) return;
    char line[400];
    int o = 0;
    for (int i = 0; i < len; i++) o += pure::format(line + o, sizeof(line) - o, "%02x", b[i]);
    Log("culling watch:   %s %s", what, line);
}

DWORD WINAPI WatchThread(void*) {
    memset((void*)g_hits, 0, sizeof(g_hits));
    if (!g_veh) g_veh = AddVectoredExceptionHandler(1, WatchHandler);
    InterlockedExchange(&g_watchArmed, 1);
    SetDebugRegs(true);
    Sleep(2000);
    SetDebugRegs(false);
    Sleep(50);
    InterlockedExchange(&g_watchArmed, 0);
    int n = 0;
    for (auto& h : g_hits)
        if (h.rip) n++;
    Log("culling watch: %d code locations touched the watched values in 2 s:", n);
    for (auto& h : g_hits) {
        if (!h.rip) continue;
        char where[300];
        DescribeAddress((void*)(uintptr_t)h.rip, where, sizeof(where));
        Log("culling watch: %s  hits %ld (%ld writes)  slots 0x%lx  thread %u", where, (long)h.count,
            (long)h.writes, (long)h.slotMask, (unsigned)h.tid);
        LogBytes("code[-48..+16]:", (uintptr_t)h.rip - 48, 64);
    }
    // the camera objects themselves, as floats relative to the fov field
    AcquireSRWLockShared(&g_lock);
    int count = g_count;
    Cand cands[8];
    memcpy(cands, g_cands, sizeof(cands));
    ReleaseSRWLockShared(&g_lock);
    for (int k = 0; k < count && k < 2; k++) {
        float f[256];
        if (!ReadMem(cands[k].fov - 640, f, sizeof(f))) continue;
        Log("culling watch: camera %d object around the fov (offsets in bytes from the fov field):", k);
        for (int i = 0; i < 256; i += 8) {
            char line[300];
            int o = pure::format(line, sizeof(line), "%d:", i * 4 - 640);
            for (int j = 0; j < 8; j++) o += pure::format(line + o, sizeof(line) - o, " %.5f", (double)f[i + j]);
            Log("culling watch: %s", line);
        }
    }
    InterlockedExchange(&g_watchBusy, 0);
    return 0;
}
}  // namespace

// Starts the 2-second watch on the found cameras (from the VR thread's hotkey, or automatically).
void CullingWatch() {
    if (InterlockedCompareExchange(&g_watchBusy, 1, 0) != 0) return;
    // 0.3.21: in stereo, watch who reads the main camera's matrices instead (head-direction culling
    // needs the code that builds the culling views from them): +0xe0 = the camera's forward row of
    // its world matrix (+0xc0), +0x80 = the first row of the 4x4 block before it.
    if (void* obj = nvr_fov_obj) {
        g_watchCount = 0;
        g_watchAddr[g_watchCount++] = (uintptr_t)obj + 0xe0;
        g_watchAddr[g_watchCount++] = (uintptr_t)obj + 0xe0;
        g_watchAddr[g_watchCount++] = (uintptr_t)obj + 0x80;
        g_watchAddr[g_watchCount++] = (uintptr_t)obj + 0x80;
        Log("culling watch: watching the main camera's matrices (slots 0x1/0x2 = +0xe0 forward row, 0x4/0x8 = +0x80) for 2 s");
        HANDLE h = CreateThread(nullptr, 256 * 1024, WatchThread, nullptr, 0, nullptr);
        if (h) CloseHandle(h);
        else InterlockedExchange(&g_watchBusy, 0);
        return;
    }
    AcquireSRWLockShared(&g_lock);
    g_watchCount = 0;
    for (int k = 0; k < g_count && g_watchCount < 4; k++) {  // camera k: slots 2k (write), 2k+1 (read/write)
        g_watchAddr[g_watchCount++] = g_cands[k].fov;
        g_watchAddr[g_watchCount++] = g_cands[k].fov;
    }
    ReleaseSRWLockShared(&g_lock);
    if (!g_watchCount) {
        Log("culling watch: searching for the camera first; press Ctrl+F5 again in ~10 s");
        if (!g_scanning) StartScan();
        InterlockedExchange(&g_watchBusy, 0);
        return;
    }
    Log("culling watch: watching %d values for 2 s", g_watchCount);
    HANDLE h = CreateThread(nullptr, 256 * 1024, WatchThread, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
    else InterlockedExchange(&g_watchBusy, 0);
}

bool CullingWatchActive() { return g_watchBusy != 0; }

bool GetterReal(const GetterCaller* e) {
    LONG t = g_fovTest;
    if (e->real || t == 6) return true;
    if (t == 1) return e->off == 0x7807ed || e->off == 0x7808df;
    if (t == 2) return e->off == 0x75164f;
    return false;
}

// Ctrl+Shift+F5: head-direction culling on/off (saved to NieRVR.ini).
void CullingToggleHeadCulling() {
    g_cfg.headCulling = !g_cfg.headCulling;
    g_hcFovLogged = 0;
    if (g_headCullRefused)
        Log("culling: head direction culling can't be used with this game build (see the earlier log line)");
    else
        Log("culling: head direction culling %s", g_cfg.headCulling ? "ON (the culling view follows your head)"
                                                                     : "OFF (the culling view follows the game camera)");
    ConfigSaveStereo();
}

// Shift+F5: which places get the wide field of view (a test to find what changes Pod 042's aim
// or the culling; not saved, back to 0 on the next start).
void CullingCycleTest() {
    LONG t = (g_fovTest + 1) % kTestSteps;
    InterlockedExchange(&g_fovTest, t);
    static const char* const kWhat[kTestSteps] = {
        "0: normal",
        "1: fov getter callers +0x7807ed/+0x7808df get the real fov",
        "2: fov getter caller +0x75164f gets the real fov",
        "3: frustum builder gets the real fov",
        "4: render camera copies get the real fov",
        "5: the camera's projection gets the wide fov (as 0.3.22)",
        "6: nothing widened (the game's own culling)"};
    Log("culling: Shift+F5 test step %s%s", kWhat[t], t ? " (head culling paused)" : "");
}
