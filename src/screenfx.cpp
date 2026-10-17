// screenfx.cpp - NieR's low-health grey and distortion, removed in stereo.
//
// They are drawn by NieR's effect system, like particles: quads whose texture is a copy of the 3D
// scene made in the middle of the effect pass, desaturated (the grey), blurred or rippled (the
// distortion), drawn back over the whole view. Normal frames only ever let effects sample one
// full-size screen copy: the one made after the opaque pass for refraction, bound at t10 when the
// first effect draws. ScreenFxSkipEffectDraw() skips, in stereo (LowHealthEffect=0), effect draws
// that sample any other full-size copy of the scene (same size and format as their target) AND are
// one of the shaders the low-health effect was seen to use. 0.3.14/0.3.15 skipped every such draw:
// that also removed Pod 042's shots and ~100 other effect draws per frame in flight-unit sections,
// which read a scene copy too. Other matching shaders are only logged now.
//
// (0.3.13/0.3.14 also replaced colour-grading tables that kept little colour, guessing that was the
// grey. It wasn't, and it broke areas graded that way on purpose, e.g. the Bunker: removed in 0.3.15.)
#include "common.h"

namespace {
ID3D11Resource* g_refractionCopy;  // this frame's normal screen copy (t10 at the first effect draw)
bool g_sawEffect;
unsigned g_filterSkips;
DWORD g_filterReportTick;
bool g_filterLogged;
// The low-health effect's pixel shaders (FNV-1a of the bytecode, low 32 bits; 0.3.14 capture):
// grey, ripples, blur, and two more drawn with them.
const uint32_t kLowHealthShaders[] = {0x86575858, 0x833497f9, 0xcebc6f6d, 0x8b838343, 0x32c5d826};
uint32_t g_keptLogged[16];
int g_keptCount;

bool LowHealthShader(uint64_t hash) {
    for (uint32_t h : kLowHealthShaders)
        if ((uint32_t)hash == h) return true;
    return false;
}

// Which effect shaders were skipped, for the minute report (bytecode hash, count).
struct Skipped {
    uint64_t hash;
    unsigned count;
    UINT slot;
};
Skipped g_skipped[8];

void NoteSkip(uint64_t hash, UINT slot) {
    for (auto& e : g_skipped)
        if (e.hash == hash && e.count) {
            e.count++;
            return;
        }
    for (auto& e : g_skipped)
        if (!e.count) {
            e = {hash, 1, slot};
            return;
        }
}

ID3D11Resource* BoundResource(ID3D11DeviceContext* c, UINT slot) {
    ID3D11ShaderResourceView* v = nullptr;
    c->PSGetShaderResources(slot, 1, &v);
    if (!v) return nullptr;
    ID3D11Resource* r = nullptr;
    v->GetResource(&r);
    v->Release();
    if (r) r->Release();  // still bound: stays alive for this draw
    return r;
}

bool Tex2D(ID3D11Resource* r, D3D11_TEXTURE2D_DESC* d) {
    if (!r) return false;
    D3D11_RESOURCE_DIMENSION dim;
    r->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return false;
    ((ID3D11Texture2D*)r)->GetDesc(d);
    return true;
}
}  // namespace

void ScreenFxBeginFrame() {
    g_refractionCopy = nullptr;
    g_sawEffect = false;
}

bool ScreenFxSkipEffectDraw(ID3D11DeviceContext* c, uint32_t sampled, uint64_t shaderHash) {
    if (!g_sawEffect) {
        g_sawEffect = true;
        g_refractionCopy = BoundResource(c, 10);
    }
    if (g_cfg.lowHealthFx || !StereoActive() || !sampled) return false;
    ID3D11RenderTargetView* rtv = nullptr;
    c->OMGetRenderTargets(1, &rtv, nullptr);
    if (!rtv) return false;
    ID3D11Resource* rt = nullptr;
    rtv->GetResource(&rt);
    rtv->Release();
    if (rt) rt->Release();
    D3D11_TEXTURE2D_DESC td;
    if (!Tex2D(rt, &td)) return false;
    bool skip = false;
    UINT hit = 0;
    for (UINT slot = 0; slot < 32 && !skip; slot++) {
        if (!(sampled >> slot & 1)) continue;
        ID3D11Resource* r = BoundResource(c, slot);
        D3D11_TEXTURE2D_DESC d;
        if (!r || r == rt || r == g_refractionCopy || !Tex2D(r, &d)) continue;
        skip = d.Width == td.Width && d.Height == td.Height && d.Format == td.Format;
        hit = slot;
    }
    if (!skip) return false;
    if (!LowHealthShader(shaderHash)) {
        // reads a scene copy too, but isn't the low-health effect (e.g. Pod shots): kept, logged once
        bool seen = false;
        for (int i = 0; i < g_keptCount; i++) seen |= g_keptLogged[i] == (uint32_t)shaderHash;
        if (!seen && g_keptCount < 16) {
            g_keptLogged[g_keptCount++] = (uint32_t)shaderHash;
            Log("screen filter: effect %08x reads a scene copy at t%u but isn't a known low-health shader: kept",
                (unsigned)(uint32_t)shaderHash, hit);
        }
        return false;
    }
    g_filterSkips++;
    NoteSkip(shaderHash, hit);
    if (!g_filterLogged) {
        g_filterLogged = true;
        g_filterReportTick = GetTickCount();
        Log("screen filter effect (redraws a copy of the scene: low-health grey, blur, ripples) "
            "removed while in stereo (LowHealthEffect=0)");
    } else if (GetTickCount() - g_filterReportTick > 60000) {
        char list[300];
        size_t at = 0;
        list[0] = 0;
        for (auto& e : g_skipped)
            if (e.count && at + 40 < sizeof(list))
                at += pure::format(list + at, sizeof(list) - at, " %08x/t%u x%u", (unsigned)(e.hash & 0xffffffff),
                                   e.slot, e.count);
        Log("screen filter: %u draws removed in the last minute (shader/slot/count:%s)", g_filterSkips, list);
        for (auto& e : g_skipped) e = {};
        g_filterSkips = 0;
        g_filterReportTick = GetTickCount();
    }
    return true;
}
