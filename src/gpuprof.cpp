// gpuprof.cpp - GPU timing of the game's frame, pass by pass (0.3.29, diagnostic).
//
// Every 5 s one frame is measured with D3D11 timestamp queries: a mark at every render-target
// change of the game (and of the BothEyes replay), at the first UI draw, at Present and around the
// replay. The results are read back a few frames later (never waiting) and logged: the slowest
// passes with their size and draw count, and totals for the left eye's scene, the UI, and the
// right eye's replay. The same summary goes to the Info page of the settings menu.
//
// The first time a full-screen depth buffer is seen its format is logged too: the depth-based
// correction for alternate-eye mode will need to read it.
#include "common.h"

namespace {
const int kMax = 320;
struct Mark {
    char label[56];
    unsigned draws;
    LONGLONG cpu;  // QueryPerformanceCounter when the mark was submitted (0.4.2)
};
struct Frame {
    ID3D11Query* disjoint;
    ID3D11Query* ts[kMax];
    Mark marks[kMax];
    int n;
    bool pending;
};
Frame g_f;
bool g_active, g_init, g_failed;
DWORD g_last;
ID3D11DeviceContext* g_ctx;
char g_lastLabel[56];
unsigned g_depthKeys[4];

const char* FmtName(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R24G8_TYPELESS: return "R24G8_TYPELESS";
        case DXGI_FORMAT_D24_UNORM_S8_UINT: return "D24_UNORM_S8_UINT";
        case DXGI_FORMAT_R32_TYPELESS: return "R32_TYPELESS";
        case DXGI_FORMAT_D32_FLOAT: return "D32_FLOAT";
        case DXGI_FORMAT_R32G8X24_TYPELESS: return "R32G8X24_TYPELESS";
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return "D32_FLOAT_S8X24_UINT";
        case DXGI_FORMAT_R16_TYPELESS: return "R16_TYPELESS";
        case DXGI_FORMAT_D16_UNORM: return "D16_UNORM";
        default: return "other";
    }
}

bool Init(ID3D11DeviceContext* c) {
    if (g_init) return !g_failed;
    g_init = true;
    ID3D11Device* dev = nullptr;
    c->GetDevice(&dev);
    if (!dev) return !(g_failed = true);
    D3D11_QUERY_DESC d = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    HRESULT hr = dev->CreateQuery(&d, &g_f.disjoint);
    d.Query = D3D11_QUERY_TIMESTAMP;
    for (int i = 0; i < kMax && SUCCEEDED(hr); i++) hr = dev->CreateQuery(&d, &g_f.ts[i]);
    dev->Release();
    if (FAILED(hr)) {
        g_failed = true;
        Log("gpu timing: queries unavailable (0x%08x)", (unsigned)hr);
        return false;
    }
    g_ctx = c;
    return true;
}

void Describe(ID3D11View* v, char* out, size_t n, bool* fullDepth, ID3D11DepthStencilView* dsv) {
    out[0] = 0;
    if (!v) return;
    ID3D11Resource* r = nullptr;
    v->GetResource(&r);
    if (!r) return;
    D3D11_RESOURCE_DIMENSION dim;
    r->GetType(&dim);
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        D3D11_TEXTURE2D_DESC d;
        ((ID3D11Texture2D*)r)->GetDesc(&d);
        pure::format(out, n, "%ux%u f%d", d.Width, d.Height, (int)d.Format);
        // depth buffer details, once per size (for the depth-based eye correction)
        if (dsv && (ID3D11View*)dsv == v && d.Width >= 640) {
            unsigned key = d.Width * 7919u + d.Height;
            int slot = -1;
            for (int i = 0; i < 4 && slot < 0; i++)
                if (g_depthKeys[i] == key) slot = -2;
            for (int i = 0; i < 4 && slot == -1; i++)
                if (!g_depthKeys[i]) slot = i;
            if (slot >= 0) {
                g_depthKeys[slot] = key;
                D3D11_DEPTH_STENCIL_VIEW_DESC vd;
                dsv->GetDesc(&vd);
                Log("gpu timing: depth buffer %ux%u, format %d (%s), view format %d, %u sample(s), bind 0x%x%s",
                    d.Width, d.Height, (int)d.Format, FmtName(d.Format), (int)vd.Format, d.SampleDesc.Count,
                    d.BindFlags, (d.BindFlags & D3D11_BIND_SHADER_RESOURCE) ? " (shader-readable)" : " (not shader-readable)");
            }
            if (fullDepth) *fullDepth = true;
        }
    }
    r->Release();
}
}  // namespace

// At the start of the game's frame (right after Present).
void GpuProfFrameStart(ID3D11DeviceContext* c) {
    if (g_active || g_f.pending || GetTickCount() - g_last < 5000) return;
    if (!Init(c)) return;
    g_last = GetTickCount();
    g_active = true;
    g_f.n = 0;
    g_lastLabel[0] = 0;
    c->Begin(g_f.disjoint);
    GpuProfMark(c, "frame start");
}

void GpuProfMark(ID3D11DeviceContext* c, const char* label) {
    if (!g_active || c != g_ctx || g_f.n >= kMax) return;
    Mark& m = g_f.marks[g_f.n];
    pure::format(m.label, sizeof(m.label), "%s", label);
    m.draws = 0;
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    m.cpu = q.QuadPart;
    c->End(g_f.ts[g_f.n]);
    g_f.n++;
    pure::format(g_lastLabel, sizeof(g_lastLabel), "%s", label);
}

// A render-target change of the game (or of the replay, prefix "R ").
void GpuProfTargets(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv,
                    const char* prefix) {
    if (!g_active || c != g_ctx) return;
    char a[40] = "", b[40] = "", label[56];
    ID3D11RenderTargetView* rt0 = (n && rtvs) ? rtvs[0] : nullptr;
    Describe(rt0, a, sizeof(a), nullptr, nullptr);
    Describe(dsv, b, sizeof(b), nullptr, dsv);
    if (rt0)
        pure::format(label, sizeof(label), "%s%s%s%s", prefix, a, n > 1 ? " +MRT" : "", dsv ? " +depth" : "");
    else if (dsv)
        pure::format(label, sizeof(label), "%sdepth only %s", prefix, b);
    else
        pure::format(label, sizeof(label), "%sno target", prefix);
    GpuProfMark(c, label);
}
// kind: 1 light lists, 2 GI volumes (probe selection), else another compute pass. Each kind gets its
// own mark so its time is measured on its own (0.4.2; before, every dispatch was one "compute").
void GpuProfCompute(ID3D11DeviceContext* c, const char* prefix, int kind) {
    if (!g_active || c != g_ctx) return;
    char label[56];
    pure::format(label, sizeof(label), "%scompute: %s", prefix,
                 kind == 1 ? "light lists" : kind == 2 ? "GI volumes" : "other");
    if (strcmp(label, g_lastLabel) != 0) GpuProfMark(c, label);
}
void GpuProfDraw() {
    if (g_active && g_f.n > 0) g_f.marks[g_f.n - 1].draws++;
}

// At Present, after everything of this frame (including the replay) was submitted.
void GpuProfFrameEnd(ID3D11DeviceContext* c) {
    if (!g_active || c != g_ctx) return;
    GpuProfMark(c, "end");
    c->End(g_f.disjoint);
    g_active = false;
    g_f.pending = true;
}

// Each frame: once the GPU has the results, log them.
void GpuProfPoll(ID3D11DeviceContext* c) {
    if (!g_f.pending || c != g_ctx) return;
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
    if (c->GetData(g_f.disjoint, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) return;
    uint64_t t[kMax];
    for (int i = 0; i < g_f.n; i++)
        if (c->GetData(g_f.ts[i], &t[i], sizeof(uint64_t), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) return;
    g_f.pending = false;
    if (dj.Disjoint || !dj.Frequency || g_f.n < 2) return;
    double toMs = 1000.0 / (double)dj.Frequency;
    LARGE_INTEGER qf;
    QueryPerformanceFrequency(&qf);
    double cpuToMs = 1000.0 / (double)qf.QuadPart;
    double giMs = 0;
    int giN = 0;
    // segments: mark i lasts until mark i+1
    struct Seg {
        int i;
        double ms;
    } segs[kMax];
    int ns = 0;
    double total = (double)(t[g_f.n - 1] - t[0]) * toMs, left = 0, ui = 0, right = 0, shadowR = 0;
    int phase = 0;  // 0 left scene, 1 UI and final, 2 replay, 3 after
    for (int i = 0; i + 1 < g_f.n; i++) {
        const char* l = g_f.marks[i].label;
        if (!strcmp(l, "UI start") || !strcmp(l, "present")) phase = 1;
        if (!strcmp(l, "replay start")) phase = 2;
        if (!strcmp(l, "replay end")) phase = 3;
        double ms = (double)(t[i + 1] - t[i]) * toMs;
        if (ms < 0) ms = 0;
        if (phase == 0) left += ms;
        if (phase == 1) ui += ms;
        if (phase == 2) right += ms;
        if (phase == 2 && strstr(l, "depth only")) shadowR += ms;
        if (strstr(l, "compute: GI volumes")) giMs += ms, giN++;
        segs[ns++] = {i, ms};
    }
    // slowest 10
    for (int a = 0; a < ns; a++)
        for (int b = a + 1; b < ns; b++)
            if (segs[b].ms > segs[a].ms) {
                Seg s = segs[a];
                segs[a] = segs[b];
                segs[b] = s;
            }
    double cpuTotal = (double)(g_f.marks[g_f.n - 1].cpu - g_f.marks[0].cpu) * cpuToMs;
    Log("gpu timing: frame %.2f ms on the GPU (%.2f ms of CPU time between the first and last mark): left eye scene "
        "%.2f, UI + final %.2f, right eye replay %.2f (depth-only passes in it %.2f); GI volumes pass %.2f ms (%d x, "
        "GIVolumes=%d); %d passes",
        total, cpuTotal, left, ui, right, shadowR, giMs, giN, g_cfg.giVolumes, g_f.n - 1);
    char info[160];
    pure::format(info, sizeof(info), "GPU per frame %.1f ms: left scene %.1f, UI %.1f, right eye %.1f; GI pass %.2f ms",
                 total, left, ui, right, giMs);
    MenuSetInfo(4, info);
    for (int k = 0; k < ns && k < 10; k++) {
        const Mark& m = g_f.marks[segs[k].i];
        // CPU time to the next mark: when it is as long as the GPU time, the GPU was waiting for the game
        double cpu = (double)(g_f.marks[segs[k].i + 1].cpu - m.cpu) * cpuToMs;
        Log("gpu timing:   %6.2f ms  %-44s %4u draws  (CPU %.2f ms)", segs[k].ms, m.label, m.draws, cpu);
        if (k < 5) {
            pure::format(info, sizeof(info), "  %.2f ms  %s (%u draws)", segs[k].ms, m.label, m.draws);
            MenuSetInfo(5 + k, info);
        }
    }
}
