// viewfix.cpp - keeps NieR's CPU-computed *view-space* data consistent with the eye camera.
//
// Two compute passes read data the game prepared on the CPU in its own camera's view space:
//  * tiled light culling (CS with "g_LightAry"): point-light positions, view space, used to build
//    the per-tile light lists (32x30-pixel tiles) that the forward shaders light with;
//  * reflection-probe selection (CS with "g_CubeInput"): view space -> probe box matrices.
// With the stereo camera (eye offset, levelling, CameraDolly) those no longer match the depth
// buffer, so lights were culled from the wrong tiles: blocky, tile-shaped patches around 2B and on
// the floor, worse the further the dolly moved the camera from the game's own. Right before each of
// these dispatches we run a tiny compute shader that rewrites a copy of the data into the eye's view
// space, and let the game's pass read the copy instead.
//
// Global illumination volumes (0.4.2/0.4.3, idea from FAR by DrDaxxy / Kaldaien): the probe-selection
// pass runs per pixel and loops over every slot of g_CubeInput (loop count = the buffer view's size,
// GetDimensions; 128 slots, 30-52 in use in the 0.4.2 log). It writes the chosen *slot numbers* per
// pixel and later shaders sample g_CubeTexArray at those slots, so slots must never move (0.4.2
// sorted them: wrong lighting on grass, trees, interiors). Now: each frame the game's buffer is copied
// to the CPU (read 3 frames later, never waiting); pure::gi_pick decides which slots stay on (all
// used ones, or the GIVolumes nearest to the camera); the copy pass marks the others empty, and the
// game's pass gets a view that ends right after the last slot left on. With all volumes kept this
// only cuts the empty tail of the loop (no visual change).
#include "common.h"
#include <d3dcompiler.h>

namespace {
const char kCSLights[] = R"(
cbuffer P : register(b0) { row_major float4x4 T; row_major float4x4 Tinv; uint first; uint count; uint2 pad; };
struct L { float4 a, b, c; };
StructuredBuffer<L> inL : register(t0);
RWStructuredBuffer<L> outL : register(u0);
[numthreads(64, 1, 1)] void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= count) return;
    L l = inL[id.x];
    l.b.xyz = mul(T, float4(l.b.xyz, 1)).xyz;   // view-space position (radius in a.w unchanged)
    outL[first + id.x] = l;
}
)";
const char kCSProbes[] = R"(
cbuffer P : register(b0) {
    row_major float4x4 T; row_major float4x4 Tinv; uint first; uint count; uint2 pad;
    uint4 on0; uint4 on1;   // GI volumes: bit per slot that stays on (others are marked empty)
};
struct C { float4 a, b, c, d, e, f; };
StructuredBuffer<C> inC : register(t0);
RWStructuredBuffer<C> outC : register(u0);
[numthreads(64, 1, 1)] void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= count) return;
    C p = inC[id.x];
    uint w = id.x >> 5;
    uint bits = w < 4 ? on0[w] : on1[w - 4];
    if (((bits >> (id.x & 31)) & 1) == 0) p.a.w = 0;  // the game's pass skips slots whose a.w is 0
    p.b = mul(p.b, Tinv);  // rows of the view -> box matrix, now taking eye view-space positions
    p.c = mul(p.c, Tinv);
    p.d = mul(p.d, Tinv);
    outC[first + id.x] = p;
}
)";


struct Fix {
    ID3D11ComputeShader* cs;
    ID3D11ShaderResourceView* lenSrv[257];  // probes: views of buf ending after slot n-1 (made on demand)
    ID3D11Buffer* buf;
    ID3D11ShaderResourceView* srv;
    ID3D11UnorderedAccessView* uav;
    D3D11_BUFFER_DESC desc;
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
    int slot;            // the game's SRV slot for this data
    UINT stride;
    const char* name;
    bool logged, failed;
};
Fix g_fix[2] = {{nullptr, {}, nullptr, nullptr, nullptr, {}, {}, 5, 48, "point lights", false, false},
                {nullptr, {}, nullptr, nullptr, nullptr, {}, {}, 1, 96, "reflection probes", false, false}};

// read-back of the game's GI volume buffer: a ring of staging copies, one per frame, each read
// (without waiting) a few frames later
const int kGiRing = 4;
struct GiCopy {
    ID3D11Buffer* buf;
    bool pending;
    UINT first, count;
};
GiCopy g_giRing[kGiRing];
UINT g_giStagingSize;
int g_giNext;
pure::GiPick g_giPick;     // the latest decision
bool g_giHave;             // g_giPick is valid
DWORD g_giLogTick, g_giInfoTick;
int g_giUsedMin = 9999, g_giUsedMax = -1, g_giHighMax = -1, g_giLenMin = 9999, g_giLenMax = -1, g_giSlots;
float g_giEntries[256 * 24];

ID3D11Buffer* g_params;
bool g_init, g_initFailed;
ID3D11ShaderResourceView* g_restoreSrv;
int g_restoreSlot = -1;

template <typename T>
void Rel(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

bool Init(ID3D11Device* dev) {
    if (g_init || g_initFailed) return g_init;
    HMODULE comp = LoadLibraryW(L"d3dcompiler_47.dll");
    pD3DCompile compile = comp ? (pD3DCompile)(void*)GetProcAddress(comp, "D3DCompile") : nullptr;
    const char* entry[2] = {"Lights", "Probes"};
    const char* src[2] = {kCSLights, kCSProbes};
    for (int i = 0; i < 2 && compile; i++) {
        ID3DBlob *b = nullptr, *err = nullptr;
        HRESULT hr = compile(src[i], lstrlenA(src[i]), entry[i], nullptr, nullptr, "main", "cs_5_0",
                             D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &b, &err);
        if (SUCCEEDED(hr)) hr = dev->CreateComputeShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_fix[i].cs);
        if (FAILED(hr))
            Log("ERROR: view fix shader %s: 0x%08x %s", entry[i], (unsigned)hr,
                err ? (const char*)err->GetBufferPointer() : "");
        if (b) b->Release();
        if (err) err->Release();
    }
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = 176;  // = sizeof the parameter struct below
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(dev->CreateBuffer(&bd, nullptr, &g_params))) g_params = nullptr;
    g_init = g_params && g_fix[0].cs && g_fix[1].cs;
    g_initFailed = !g_init;
    if (g_initFailed) Log("view fix: unavailable (shader compiler or resources missing); lighting may show tiles");
    return g_init;
}

// Our copy of the game's buffer behind `src`: same size/stride, plus UAV, and an SRV like the game's.
bool EnsureCopy(ID3D11Device* dev, Fix& f, ID3D11ShaderResourceView* src, UINT* first, UINT* count) {
    D3D11_SHADER_RESOURCE_VIEW_DESC sd;
    src->GetDesc(&sd);
    if (sd.ViewDimension != D3D11_SRV_DIMENSION_BUFFER && sd.ViewDimension != D3D11_SRV_DIMENSION_BUFFEREX) return false;
    ID3D11Resource* r = nullptr;
    src->GetResource(&r);
    if (!r) return false;
    D3D11_RESOURCE_DIMENSION dim;
    r->GetType(&dim);
    D3D11_BUFFER_DESC d = {};
    if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) ((ID3D11Buffer*)r)->GetDesc(&d);
    r->Release();
    if (dim != D3D11_RESOURCE_DIMENSION_BUFFER || d.StructureByteStride != f.stride ||
        !(d.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED))
        return false;
    *first = sd.Buffer.FirstElement;
    *count = sd.Buffer.NumElements;
    if (f.buf && f.desc.ByteWidth == d.ByteWidth && memcmp(&f.srvDesc, &sd, sizeof(sd)) == 0) return true;
    for (auto& v : f.lenSrv) Rel(v);
    Rel(f.uav);
    Rel(f.srv);
    Rel(f.buf);
    D3D11_BUFFER_DESC nd = {};
    nd.ByteWidth = d.ByteWidth;
    nd.Usage = D3D11_USAGE_DEFAULT;
    nd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    nd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    nd.StructureByteStride = f.stride;
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
    ud.Format = DXGI_FORMAT_UNKNOWN;
    ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = d.ByteWidth / f.stride;
    bool ok = SUCCEEDED(dev->CreateBuffer(&nd, nullptr, &f.buf)) &&
              SUCCEEDED(dev->CreateShaderResourceView(f.buf, &sd, &f.srv)) &&
              SUCCEEDED(dev->CreateUnorderedAccessView(f.buf, &ud, &f.uav));
    if (!ok) {
        Rel(f.uav);
        Rel(f.srv);
        Rel(f.buf);
        return false;
    }
    f.desc = d;
    f.srvDesc = sd;
    return true;
}

// Each frame (not in the both-eyes replay): copy the game's GI volume buffer into the ring, and use
// the oldest copy the GPU has finished to decide which slots stay on.
void GiReadback(ID3D11DeviceContext* c, ID3D11Device* dev, ID3D11ShaderResourceView* src, UINT first, UINT count) {
    // newest finished copy
    for (int k = 0; k < kGiRing; k++) {
        GiCopy& g = g_giRing[(g_giNext + k) % kGiRing];  // oldest first
        if (!g.pending) continue;
        D3D11_MAPPED_SUBRESOURCE m;
        if (c->Map(g.buf, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) != S_OK) break;
        UINT n = g.count > 256 ? 256 : g.count;
        if ((g.first + n) * 96 <= g_giStagingSize) {
            memcpy(g_giEntries, (const uint8_t*)m.pData + g.first * 96, n * 96);
            int keep = g_cfg.giVolumes >= 128 ? 256 : g_cfg.giVolumes;
            g_giPick = pure::gi_pick(g_giEntries, (int)n, keep, 8);
            g_giHave = true;
            g_giSlots = (int)g.count;
            if (g_giPick.used < g_giUsedMin) g_giUsedMin = g_giPick.used;
            if (g_giPick.used > g_giUsedMax) g_giUsedMax = g_giPick.used;
            if (g_giPick.highest > g_giHighMax) g_giHighMax = g_giPick.highest;
            if (g_giPick.length < g_giLenMin) g_giLenMin = g_giPick.length;
            if (g_giPick.length > g_giLenMax) g_giLenMax = g_giPick.length;
        }
        c->Unmap(g.buf, 0);
        g.pending = false;
    }
    // this frame's copy
    ID3D11Resource* r = nullptr;
    src->GetResource(&r);
    if (!r) return;
    D3D11_BUFFER_DESC d;
    ((ID3D11Buffer*)r)->GetDesc(&d);
    if (g_giStagingSize != d.ByteWidth) {
        for (auto& g : g_giRing) {
            Rel(g.buf);
            g.pending = false;
        }
        g_giStagingSize = 0;
        D3D11_BUFFER_DESC sd = {};
        sd.ByteWidth = d.ByteWidth;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.MiscFlags = d.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        sd.StructureByteStride = d.StructureByteStride;
        bool ok = true;
        for (auto& g : g_giRing) ok = ok && SUCCEEDED(dev->CreateBuffer(&sd, nullptr, &g.buf));
        if (!ok) {  // try a plain buffer
            for (auto& g : g_giRing) Rel(g.buf);
            sd.MiscFlags = 0;
            sd.StructureByteStride = 0;
            ok = true;
            for (auto& g : g_giRing) ok = ok && SUCCEEDED(dev->CreateBuffer(&sd, nullptr, &g.buf));
        }
        if (ok) g_giStagingSize = d.ByteWidth;
        else
            for (auto& g : g_giRing) Rel(g.buf);
    }
    GiCopy& g = g_giRing[g_giNext];
    if (g_giStagingSize && !g.pending) {
        c->CopyResource(g.buf, r);
        g.pending = true;
        g.first = first;
        g.count = count;
        g_giNext = (g_giNext + 1) % kGiRing;
    }
    r->Release();
}

void GiReport() {
    if (!g_giHave) return;
    DWORD t = GetTickCount();
    if (t - g_giInfoTick > 500) {
        g_giInfoTick = t;
        char info[160];
        if (g_giPick.dropping)
            pure::format(info, sizeof(info), "GI volumes: %d in use, %d nearest kept; the pass checks slots 0-%d of %d",
                         g_giPick.used, g_giPick.kept, g_giPick.length - 1, g_giSlots);
        else
            pure::format(info, sizeof(info), "GI volumes: %d in use (last slot %d), all kept; the pass checks slots 0-%d of %d",
                         g_giPick.used, g_giPick.highest, g_giPick.length - 1, g_giSlots);
        MenuSetInfo(12, info);
    }
    if (t - g_giLogTick < 30000 || g_giUsedMax < 0) return;
    g_giLogTick = t;
    Log("GI volumes (30 s): %d-%d of %d slots in use, last used slot up to %d; the game's pass checked %d-%d slots "
        "(GIVolumes=%d: %s)",
        g_giUsedMin, g_giUsedMax, g_giSlots, g_giHighMax, g_giLenMin, g_giLenMax, g_cfg.giVolumes,
        g_cfg.giVolumes >= 128 ? "all kept, only the empty tail skipped" : "the nearest kept, the rest switched off");
    g_giUsedMin = 9999;
    g_giUsedMax = g_giHighMax = g_giLenMax = -1;
    g_giLenMin = 9999;
}
}  // namespace

// kind: 1 = light culling, 2 = probe selection (GI volumes). Called right before the game's dispatch.
void ViewFixBeforeDispatch(ID3D11DeviceContext* c, int kind) {
    g_restoreSlot = -1;
    if (kind < 1 || kind > 2) return;
    float T[16], Tinv[16];
    bool view = g_cfg.viewFix && StereoGetViewFix(T, Tinv);  // false in mono frames: the data already matches
    bool gi = kind == 2;  // GI volumes: always read back; slots switched off / loop cut once a pick exists
    if (!view && !gi) return;
    if (!view) {
        for (int i = 0; i < 16; i++) T[i] = Tinv[i] = (i % 5 == 0) ? 1.f : 0.f;
    }
    ID3D11Device* dev = nullptr;
    c->GetDevice(&dev);
    if (!dev) return;
    Fix& f = g_fix[kind - 1];
    ID3D11ShaderResourceView* src = nullptr;
    UINT first = 0, count = 0;
    bool ok = Init(dev);
    if (ok) {
        c->CSGetShaderResources(f.slot, 1, &src);
        ok = src && EnsureCopy(dev, f, src, &first, &count) && count > 0;
    }
    if (!ok) {
        dev->Release();
        if (src) src->Release();
        if (!f.failed && g_init) {
            f.failed = true;
            Log("view fix: %s buffer not in the expected format; left unchanged", f.name);
        }
        return;
    }
    if (gi && !g_replaying) GiReadback(c, dev, src, first, count);  // the replay reuses this frame's pick
    if (gi) GiReport();
    // GI volumes: the view the game's pass gets ends after the last slot left on
    ID3D11ShaderResourceView* use = f.srv;
    uint32_t on[8];
    for (auto& w : on) w = 0xffffffffu;
    bool trim = gi && g_giHave && count <= 256 && g_giSlots == (int)count;
    if (trim) {
        UINT len = (UINT)g_giPick.length;
        if (len > count) len = count;
        if (!f.lenSrv[len]) {
            D3D11_SHADER_RESOURCE_VIEW_DESC sd = f.srvDesc;
            sd.Buffer.NumElements = len;  // same place in the BUFFER and BUFFEREX views
            if (FAILED(dev->CreateShaderResourceView(f.buf, &sd, &f.lenSrv[len]))) f.lenSrv[len] = nullptr;
        }
        if (f.lenSrv[len]) {
            use = f.lenSrv[len];
            memcpy(on, g_giPick.mask, sizeof(on));
        } else {
            trim = false;
        }
    }
    dev->Release();
    if (!view && !trim) {  // flat frame, nothing to change yet
        src->Release();
        return;
    }
    struct {
        float T[16], Tinv[16];
        UINT first, count, pad[2];
        uint32_t on[8];
    } p;
    static_assert(sizeof(p) == 176, "matches g_params ByteWidth and the HLSL cbuffer");
    memcpy(p.T, T, 64);
    memcpy(p.Tinv, Tinv, 64);
    p.first = first;
    p.count = count;
    p.pad[0] = p.pad[1] = 0;
    memcpy(p.on, on, sizeof(on));
    c->UpdateSubresource(g_params, 0, nullptr, &p, 0, 0);

    // save the game's compute state we touch
    ID3D11ComputeShader* gcs = nullptr;
    ID3D11ClassInstance* inst[8] = {};
    UINT ninst = 8;
    c->CSGetShader(&gcs, inst, &ninst);
    ID3D11ShaderResourceView* gsrv0 = nullptr;
    c->CSGetShaderResources(0, 1, &gsrv0);
    ID3D11UnorderedAccessView* guav0 = nullptr;
    c->CSGetUnorderedAccessViews(0, 1, &guav0);
    ID3D11Buffer* gcb0 = nullptr;
    c->CSGetConstantBuffers(0, 1, &gcb0);

    ID3D11ShaderResourceView* none = nullptr;
    c->CSSetShaderResources(0, 1, &none);  // the game's t0 may alias nothing of ours, but be safe
    c->CSSetShader(f.cs, nullptr, 0);
    c->CSSetConstantBuffers(0, 1, &g_params);
    c->CSSetShaderResources(0, 1, &src);
    UINT keepCounts = (UINT)-1;
    c->CSSetUnorderedAccessViews(0, 1, &f.uav, &keepCounts);
    ProbeRawDispatch(c, (count + 63) / 64, 1, 1);
    ID3D11UnorderedAccessView* noUav = nullptr;
    c->CSSetUnorderedAccessViews(0, 1, &noUav, &keepCounts);

    // restore, and let the game's pass read our copy
    c->CSSetShader(gcs, inst, ninst);
    c->CSSetConstantBuffers(0, 1, &gcb0);
    c->CSSetShaderResources(0, 1, &gsrv0);
    c->CSSetUnorderedAccessViews(0, 1, &guav0, &keepCounts);
    c->CSSetShaderResources(f.slot, 1, &use);
    g_restoreSrv = src;  // keeps our reference until after the dispatch
    g_restoreSlot = f.slot;
    if (gcs) gcs->Release();
    for (UINT i = 0; i < ninst && i < 8; i++)
        if (inst[i]) inst[i]->Release();
    if (gsrv0) gsrv0->Release();
    if (guav0) guav0->Release();
    if (gcb0) gcb0->Release();
    if (!f.logged && view) {
        f.logged = true;
        Log("view fix: %s moved into the eye's view space (%u entries) for the game's compute pass", f.name, count);
    }
    static bool trimLogged;
    if (trim && !trimLogged) {
        trimLogged = true;
        Log("GI volumes: the game's GI pass now checks slots 0-%d of %u (%d in use, %d kept; GIVolumes=%d)",
            g_giPick.length - 1, count, g_giPick.used, g_giPick.kept, g_cfg.giVolumes);
    }
}

void ViewFixAfterDispatch(ID3D11DeviceContext* c) {
    if (g_restoreSlot < 0) return;
    c->CSSetShaderResources((UINT)g_restoreSlot, 1, &g_restoreSrv);
    if (g_restoreSrv) g_restoreSrv->Release();
    g_restoreSrv = nullptr;
    g_restoreSlot = -1;
}
