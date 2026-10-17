// uisplit.cpp - separates NieR's 2D UI (HUD, menus) from the 3D scene, on the game side.
//
// NieR draws the UI last, with UIParam/cbUIParam vertex shaders, into an intermediate render
// target; one final full-screen pass then writes that target to the backbuffer (see
// DEVELOPMENT.md). So, per frame:
//   1. at the first UI draw, copy the intermediate target ("pre-UI": scene only);
//   2. after the game's final pass into the backbuffer, run that same draw again with the pre-UI
//      copy as input, into our own texture: the finished scene without UI.
// The VR thread then shows the scene in stereo and keys the UI out of (full - scene) onto a flat
// layer. If the game ever draws differently, no scene image is produced and nothing changes.
//
// The scene image also carries the UI's real coverage in its alpha channel: every UI draw is run a
// second time into it with an alpha-only blend (over: a + (1 - a) * dst; additive: unchanged), and
// the final pass replay writes colour only. The VR side then rebuilds the HUD exactly
// (ui = full - (1 - a) * scene) instead of guessing it from where the two images differ.
//
// Some "UI" draws are really screen effects: NieR's film grain is a white, 20%-opaque texture
// drawn over the whole 1600x900 UI canvas. Those are drawn into the pre-UI copy as well, so they
// stay part of the 3D image instead of turning the HUD layer into a faint full-screen ghost.
//
// Others redraw the screen itself: for the glitch/distortion effects (when hit, at low health) and
// the blurred background of some menus, NieR first copies the UI target (scene + HUD so far) into
// another full-size texture, then draws pieces of that copy back over the UI target, shifted and
// tinted. On the HUD layer that is a flat copy of the whole screen flashing in front of the 3D view.
// In stereo those draws are skipped (DistortionEffects=0) or kept in the 3D image (=1).
#include "common.h"

namespace {
ID3D11Texture2D* g_bb;            // the swapchain backbuffer (raw pointer: identity only, never called)
D3D11_TEXTURE2D_DESC g_bbDesc;     // its description, taken at Present while it was certainly alive
bool g_vsIsUI;                     // current vertex shader is a UI shader
bool g_uiSeen, g_sceneReady;
ID3D11Resource* g_uiTarget;        // the intermediate the UI is drawn into
ID3D11Texture2D* g_preUI;
D3D11_TEXTURE2D_DESC g_preDesc;
ID3D11ShaderResourceView* g_preSRV;
ID3D11RenderTargetView* g_preRTV;
D3D11_RENDER_TARGET_VIEW_DESC g_preRTVDesc;
bool g_replayEffect;   // the current UI draw is a screen effect: also draw it into the pre-UI copy
bool g_coverDraw;      // the current UI draw goes into the UI target: add its alpha to the coverage
bool g_loggedEffect;
ID3D11Resource* g_screenCopy;      // this frame's copy of the UI target, made for distortion effects
unsigned g_copyEffects;            // distortion draws removed/kept since the last report
DWORD g_copyReportTick;
bool g_loggedCopy;
// BothEyes: the game's final pass of this frame (state captured by replay.cpp), to draw the right
// eye's scene image after its playback.
bool g_finalOk;
int g_finalSlot;
DrawArgs g_finalArgs;

// Parameters of UI draws: the first 96 bytes of the UIParam / cbUIParam cbuffers (both start with
// g_WorldMatrix, g_Color0, g_Color1, g_DispRate), remembered per buffer.
struct UiCb {
    ID3D11Resource* buf;
    float f[24];
};
UiCb g_uiCb[32];
int g_uiCbNext;
struct UiPending {
    ID3D11Resource* res;
    const float* data;
};
UiPending g_uiPending[8];
ID3D11Resource* g_vsCb1;  // buffer bound at VS slot b1 (UI parameters)
D3D11_SHADER_RESOURCE_VIEW_DESC g_preSRVDesc;
ID3D11Texture2D* g_scene;
ID3D11RenderTargetView* g_sceneRTV;
D3D11_TEXTURE2D_DESC g_sceneDesc;
D3D11_RENDER_TARGET_VIEW_DESC g_sceneRTVDesc;
unsigned g_framesWithScene, g_framesWithUI;
bool g_loggedOk, g_loggedNoFinal;
volatile LONG g_budget;

template <typename T>
void Rel(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

bool SameDesc(const D3D11_TEXTURE2D_DESC& a, const D3D11_TEXTURE2D_DESC& b) {
    return a.Width == b.Width && a.Height == b.Height && a.Format == b.Format && a.ArraySize == b.ArraySize &&
           a.MipLevels == b.MipLevels && a.SampleDesc.Count == b.SampleDesc.Count;
}

ID3D11Device* DeviceOf(ID3D11DeviceContext* c) {
    ID3D11Device* d = nullptr;
    c->GetDevice(&d);
    if (d) d->Release();  // the context keeps it alive
    return d;
}
void StoreUiCb(ID3D11Resource* res, const float* data) {
    for (auto& e : g_uiCb)
        if (e.buf == res) {
            memcpy(e.f, data, sizeof(e.f));
            return;
        }
    UiCb& e = g_uiCb[g_uiCbNext];
    g_uiCbNext = (g_uiCbNext + 1) % 32;
    e.buf = res;
    memcpy(e.f, data, sizeof(e.f));
}

const float* FindUiCb(ID3D11Resource* res) {
    if (!res) return nullptr;
    for (auto& e : g_uiCb)
        if (e.buf == res) return e.f;
    return nullptr;
}

// Blend states derived from the game's: mode 0 = UI coverage (alpha channel only), mode 1 = the
// final pass replay (colour only, so the coverage survives).
struct DerivedBlend {
    ID3D11BlendState* game;
    bool isNull;
    int mode;
    ID3D11BlendState* ours;
};
DerivedBlend g_blends[48];
int g_blendCount;
volatile LONG g_blendLogBudget;

ID3D11BlendState* Derived(ID3D11Device* dev, ID3D11BlendState* game, int mode) {
    for (int i = 0; i < g_blendCount; i++)
        if (g_blends[i].game == game && g_blends[i].isNull == !game && g_blends[i].mode == mode) return g_blends[i].ours;
    D3D11_BLEND_DESC d = {};
    if (game) {
        game->GetDesc(&d);
    } else {
        for (auto& r : d.RenderTarget) {
            r.SrcBlend = r.SrcBlendAlpha = D3D11_BLEND_ONE;
            r.DestBlend = r.DestBlendAlpha = D3D11_BLEND_ZERO;
            r.BlendOp = r.BlendOpAlpha = D3D11_BLEND_OP_ADD;
            r.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        }
    }
    D3D11_RENDER_TARGET_BLEND_DESC& r = d.RenderTarget[0];
    if (mode == 0) {
        bool additive = r.BlendEnable && r.DestBlend == D3D11_BLEND_ONE;
        if (!r.BlendEnable) {
            r.SrcBlend = D3D11_BLEND_ONE;
            r.DestBlend = D3D11_BLEND_ZERO;
            r.BlendOp = D3D11_BLEND_OP_ADD;
        }
        r.BlendEnable = TRUE;
        r.SrcBlendAlpha = additive ? D3D11_BLEND_ZERO : D3D11_BLEND_ONE;
        r.DestBlendAlpha = additive ? D3D11_BLEND_ONE : D3D11_BLEND_INV_SRC_ALPHA;
        r.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        r.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALPHA;
        if (LogBudget(&g_blendLogBudget, 6))
            Log("ui split: UI blend %s: colour %d/%d op %d, alpha %d/%d -> coverage %s", game ? "state" : "(default)",
                (int)(game ? d.RenderTarget[0].SrcBlend : 0), (int)(game ? d.RenderTarget[0].DestBlend : 0),
                (int)r.BlendOp, (int)r.SrcBlendAlpha, (int)r.DestBlendAlpha, additive ? "unchanged (additive)" : "over");
    } else {
        r.RenderTargetWriteMask &= D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN |
                                   D3D11_COLOR_WRITE_ENABLE_BLUE;
    }
    d.IndependentBlendEnable = FALSE;
    d.AlphaToCoverageEnable = FALSE;
    ID3D11BlendState* ours = nullptr;
    if (FAILED(dev->CreateBlendState(&d, &ours))) ours = nullptr;
    if (g_blendCount < 48) g_blends[g_blendCount++] = {game, !game, mode, ours};
    return ours;
}

// Draw `args` again with RT0 replaced (and optionally the blend state), restoring both after.
void ReplayInto(ID3D11DeviceContext* c, ID3D11RenderTargetView* target, const DrawArgs& args, int blendMode) {
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    c->OMGetRenderTargets(1, &rtv, &dsv);
    ID3D11BlendState* bs = nullptr;
    FLOAT factor[4];
    UINT mask = 0xffffffff;
    ID3D11BlendState* ours = nullptr;
    if (blendMode >= 0) {
        c->OMGetBlendState(&bs, factor, &mask);
        ours = Derived(DeviceOf(c), bs, blendMode);
    }
    if (blendMode < 0 || ours) {
        if (ours) c->OMSetBlendState(ours, factor, mask);
        ProbeSetRenderTargets(c, 1, &target, dsv);
        ProbeReplayDraw(c, args);
        ProbeSetRenderTargets(c, 1, &rtv, dsv);
        if (ours) c->OMSetBlendState(bs, factor, mask);
    }
    if (bs) bs->Release();
    if (rtv) rtv->Release();
    if (dsv) dsv->Release();
}

// Full-screen white overlay at the centre of the 1600x900 UI canvas, unscaled: a screen effect.
bool IsScreenEffect(const float* f) {
    auto within = [](float a, float b, float e) { return a > b - e && a < b + e; };
    return within(f[0], 1, 0.01f) && within(f[5], 1, 0.01f) && within(f[3], 800, 0.5f) && within(f[7], 450, 0.5f) &&
           f[16] > 0.95f && f[17] > 0.95f && f[18] > 0.95f;
}
}  // namespace

// Our scene texture: backbuffer-sized, with an RTV like the game's final-pass view of the
// backbuffer (the default view until that pass has been seen once).
static bool EnsureScene(ID3D11Device* dev, const D3D11_RENDER_TARGET_VIEW_DESC* rd) {
    D3D11_TEXTURE2D_DESC bd;
    bd = g_bbDesc;
    bool descOk = rd ? memcmp(rd, &g_sceneRTVDesc, sizeof(*rd)) == 0 : true;
    if (g_scene && SameDesc(bd, g_sceneDesc) && descOk) return true;
    Rel(g_sceneRTV);
    Rel(g_scene);
    D3D11_TEXTURE2D_DESC nd = bd;
    nd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    nd.MiscFlags = 0;
    nd.CPUAccessFlags = 0;
    nd.Usage = D3D11_USAGE_DEFAULT;
    nd.MipLevels = 1;
    nd.ArraySize = 1;
    bool ok = SUCCEEDED(dev->CreateTexture2D(&nd, nullptr, &g_scene)) &&
              SUCCEEDED(dev->CreateRenderTargetView(g_scene, rd, &g_sceneRTV));
    g_sceneDesc = bd;
    if (rd) g_sceneRTVDesc = *rd;
    else memset(&g_sceneRTVDesc, 0, sizeof(g_sceneRTVDesc));
    if (!ok) {
        Rel(g_sceneRTV);
        Rel(g_scene);
    }
    return ok;
}

// UIParam (112 B) and cbUIParam (144 B) as declared in the shaders; NieR allocates them rounded up
// (128 / 160 B were seen), so accept the whole range.
static bool IsUiParamSize(UINT size) { return size >= 112 && size <= 176; }

void UiOnMap(ID3D11Resource* res, void* data, UINT size) {
    if (!IsUiParamSize(size)) return;
    for (auto& p : g_uiPending)
        if (!p.res) {
            p = {res, (const float*)data};
            return;
        }
}

void UiBeforeUnmap(ID3D11Resource* res) {
    for (auto& p : g_uiPending)
        if (p.res == res) {
            float local[24];
            memcpy(local, p.data, sizeof(local));  // mapped memory may be write-combined: read once
            StoreUiCb(res, local);
            p.res = nullptr;
            return;
        }
}

void UiOnUpdate(ID3D11Resource* res, const void* data, UINT size) {
    if (IsUiParamSize(size)) StoreUiCb(res, (const float*)data);
}

void UiOnSetVSConstantBuffers(UINT start, UINT n, ID3D11Buffer* const* bufs) {
    if (start <= 1 && start + n > 1) g_vsCb1 = bufs[1 - start];
}

void UiSetBackbuffer(ID3D11Texture2D* bb) {
    if (bb) bb->GetDesc(&g_bbDesc);
    g_bb = bb;
}

// ResizeBuffers releases the old backbuffer: forget it until the next Present (0.3.4 crashed
// calling into the freed one when the game switched to borderless).
void UiOnResizeBuffers() { g_bb = nullptr; }

void UiBeginFrame() {
    if (g_uiSeen) g_framesWithUI++;
    if (g_uiSeen && !g_sceneReady && !g_loggedNoFinal && g_framesWithUI > 120) {
        g_loggedNoFinal = true;
        Log("ui split: UI draws found but the final pass into the backbuffer wasn't recognised; HUD stays in the "
            "3D image");
    }
    g_uiSeen = false;
    g_sceneReady = false;
    g_uiTarget = nullptr;
    g_replayEffect = false;
    g_coverDraw = false;
    g_screenCopy = nullptr;
    g_finalOk = false;
    for (auto& p : g_uiPending) p.res = nullptr;
}

void UiOnSetVS(bool isUI) { g_vsIsUI = isUI; }

// Current render target 0's resource (raw pointer: valid while it stays bound).
ID3D11Resource* CurrentRT(ID3D11DeviceContext* c) {
    ID3D11RenderTargetView* rtv = nullptr;
    c->OMGetRenderTargets(1, &rtv, nullptr);
    if (!rtv) return nullptr;
    ID3D11Resource* r = nullptr;
    rtv->GetResource(&r);
    rtv->Release();
    if (r) r->Release();
    return r;
}

// Does the current pixel shader read `res` (slots t0-t7)?
static bool PsReads(ID3D11DeviceContext* c, ID3D11Resource* res) {
    if (!res) return false;
    ID3D11ShaderResourceView* srvs[8] = {};
    c->PSGetShaderResources(0, 8, srvs);
    bool found = false;
    for (auto& v : srvs) {
        if (!v) continue;
        ID3D11Resource* r = nullptr;
        v->GetResource(&r);
        if (r == res) found = true;
        if (r) r->Release();
        v->Release();
    }
    return found;
}

static bool FullSize(ID3D11Resource* r) {
    D3D11_RESOURCE_DIMENSION dim;
    r->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return false;
    D3D11_TEXTURE2D_DESC d;
    ((ID3D11Texture2D*)r)->GetDesc(&d);
    return d.Width == g_bbDesc.Width && d.Height == g_bbDesc.Height;
}

// Distortion draw found (it reads this frame's screen copy): true = skip it.
static bool OnCopyEffect() {
    bool skip = !g_cfg.distortionFx && StereoActive();
    g_copyEffects++;
    if (!g_loggedCopy) {
        g_loggedCopy = true;
        g_copyReportTick = GetTickCount();
        Log("ui split: screen distortion effect (redraws a copy of the screen, e.g. when hit): %s",
            g_cfg.distortionFx ? "kept in the 3D image (DistortionEffects=1)" : "removed while in stereo (DistortionEffects=0)");
    } else if (GetTickCount() - g_copyReportTick > 60000) {
        Log("ui split: %u screen distortion draws %s in the last minute", g_copyEffects,
            g_cfg.distortionFx ? "kept" : "seen (removed while in stereo)");
        g_copyReportTick = GetTickCount();
        g_copyEffects = 0;
    }
    return skip;
}

// Before a draw: at the first UI draw of the frame, keep a copy of the scene-only target.
// Returns true when the draw must be skipped.
bool UiBeforeDraw(ID3D11DeviceContext* c) {
    if (!g_bb) return false;
    if (!g_vsIsUI) {
        // During the UI: a draw into another full-size texture that reads the UI target is NieR
        // copying the screen for a distortion effect; draws into the UI target that read that
        // copy are the effect itself.
        if (!g_uiSeen || !g_uiTarget) return false;
        ID3D11Resource* rt0 = CurrentRT(c);
        if (!rt0 || rt0 == (ID3D11Resource*)g_bb) return false;
        if (rt0 == g_uiTarget) {
            if (!g_screenCopy || !PsReads(c, g_screenCopy)) return false;
            if (OnCopyEffect()) return true;
            g_replayEffect = g_preRTV != nullptr;  // kept: part of the 3D image, not the HUD
            return false;
        }
        if (FullSize(rt0) && PsReads(c, g_uiTarget)) g_screenCopy = rt0;
        return false;
    }
    if (g_uiSeen) {
        g_replayEffect = false;
        g_coverDraw = false;
        if (g_screenCopy && CurrentRT(c) == g_uiTarget && PsReads(c, g_screenCopy)) {
            if (OnCopyEffect()) return true;
            g_replayEffect = g_preRTV != nullptr;  // kept: part of the 3D image, not the HUD
            return false;
        }
        const float* p = FindUiCb(g_vsCb1);
        bool effect = p && IsScreenEffect(p);
        g_replayEffect = effect && g_preRTV;
        g_coverDraw = !effect && g_sceneRTV && CurrentRT(c) == g_uiTarget;
        return false;
    }
    ID3D11Resource* rt0 = CurrentRT(c);
    if (!rt0 || rt0 == (ID3D11Resource*)g_bb) return false;
    D3D11_RESOURCE_DIMENSION dim;
    rt0->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return false;
    D3D11_TEXTURE2D_DESC d;
    ((ID3D11Texture2D*)rt0)->GetDesc(&d);
    D3D11_TEXTURE2D_DESC bd;
    bd = g_bbDesc;
    // the UI goes into a full-screen intermediate; ignore UI drawn into smaller offscreen textures
    if (d.SampleDesc.Count != 1 || d.Width != bd.Width || d.Height != bd.Height) return false;
    ReplayStop(true);  // BothEyes: the scene is finished, the HUD is drawn once (not recorded)
    GpuProfMark(c, "UI start");
    ReprojectAtUiStart(c);
    g_uiSeen = true;
    if (!g_preUI || !SameDesc(d, g_preDesc)) {
        Rel(g_preSRV);
        Rel(g_preUI);
        D3D11_TEXTURE2D_DESC nd = d;
        Rel(g_preRTV);
        nd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        nd.MiscFlags = 0;
        nd.CPUAccessFlags = 0;
        nd.Usage = D3D11_USAGE_DEFAULT;
        HRESULT hr = DeviceOf(c)->CreateTexture2D(&nd, nullptr, &g_preUI);
        if (FAILED(hr)) {
            if (LogBudget(&g_budget, 3)) Log("ui split: pre-UI copy texture failed 0x%08x", (unsigned)hr);
            g_preUI = nullptr;
            return false;
        }
        g_preDesc = d;
        Log("ui split: UI is drawn into a %ux%u format %d target", d.Width, d.Height, (int)d.Format);
    }
    c->CopyResource(g_preUI, rt0);
    g_uiTarget = rt0;
    // A render target view on the copy, matching the game's view of its UI target, so screen
    // effects can be drawn into it too.
    ID3D11RenderTargetView* rtv = nullptr;
    c->OMGetRenderTargets(1, &rtv, nullptr);
    if (rtv) {
        D3D11_RENDER_TARGET_VIEW_DESC rd;
        rtv->GetDesc(&rd);
        if (!g_preRTV || memcmp(&rd, &g_preRTVDesc, sizeof(rd)) != 0) {
            Rel(g_preRTV);
            if (FAILED(DeviceOf(c)->CreateRenderTargetView(g_preUI, &rd, &g_preRTV))) g_preRTV = nullptr;
            g_preRTVDesc = rd;
        }
        rtv->Release();
    }
    // Coverage starts empty each frame.
    if (g_bb && EnsureScene(DeviceOf(c), g_sceneRTVDesc.ViewDimension ? &g_sceneRTVDesc : nullptr)) {
        const FLOAT zero[4] = {0, 0, 0, 0};
        c->ClearRenderTargetView(g_sceneRTV, zero);
    }
    const float* p = FindUiCb(g_vsCb1);
    bool effect = p && IsScreenEffect(p);
    g_replayEffect = effect && g_preRTV;
    g_coverDraw = !effect && g_sceneRTV;
    return false;
}

// After a draw: if this was the final pass (into the backbuffer, reading the UI target), run it
// again on the pre-UI copy into our scene texture.
void UiAfterDraw(ID3D11DeviceContext* c, const DrawArgs& args) {
    if (g_coverDraw) {
        // same draw, alpha only, into the coverage (our scene texture's alpha channel)
        g_coverDraw = false;
        ReplayInto(c, g_sceneRTV, args, 0);
        return;
    }
    if (g_replayEffect) {
        // same draw, same state, into the scene-only copy
        g_replayEffect = false;
        ReplayInto(c, g_preRTV, args, -1);
        if (!g_loggedEffect) {
            g_loggedEffect = true;
            Log("ui split: full-screen overlay (film grain or screen effect) kept in the 3D image");
        }
        return;
    }
    if (!g_uiSeen || g_sceneReady || !g_uiTarget || !g_preUI) return;
    if (CurrentRT(c) != (ID3D11Resource*)g_bb) return;
    ID3D11ShaderResourceView* srvs[8] = {};
    c->PSGetShaderResources(0, 8, srvs);
    int slot = -1;
    for (int i = 0; i < 8; i++) {
        if (!srvs[i] || slot >= 0) continue;
        ID3D11Resource* r = nullptr;
        srvs[i]->GetResource(&r);
        if (r == g_uiTarget) slot = i;
        if (r) r->Release();
    }
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    if (slot >= 0) c->OMGetRenderTargets(1, &rtv, &dsv);
    bool ok = slot >= 0 && rtv;
    ID3D11Device* dev = DeviceOf(c);
    if (ok) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd;
        srvs[slot]->GetDesc(&sd);
        if (!g_preSRV || memcmp(&sd, &g_preSRVDesc, sizeof(sd)) != 0) {
            Rel(g_preSRV);
            ok = SUCCEEDED(dev->CreateShaderResourceView(g_preUI, &sd, &g_preSRV));
            g_preSRVDesc = sd;
        }
    }
    if (ok) {
        D3D11_RENDER_TARGET_VIEW_DESC rd;
        rtv->GetDesc(&rd);
        bool had = g_scene && memcmp(&rd, &g_sceneRTVDesc, sizeof(rd)) == 0;
        ok = EnsureScene(dev, &rd);
        if (ok && !had) {  // new texture: this frame's coverage is lost, start from "no UI"
            const FLOAT zero[4] = {0, 0, 0, 0};
            c->ClearRenderTargetView(g_sceneRTV, zero);
        }
    }
    if (ok) {
        // Same pipeline state, different input and output: the scene without the UI. Colour only,
        // so the UI coverage gathered in the alpha channel stays.
        if (StereoBothEyesFrame()) {  // the same pass will make the right eye's scene image
            ReplayCaptureFinalPassState(c);
            g_finalOk = true;
            g_finalSlot = slot;
            g_finalArgs = args;
        }
        c->PSSetShaderResources(slot, 1, &g_preSRV);
        ReplayInto(c, g_sceneRTV, args, 1);
        c->PSSetShaderResources(slot, 1, &srvs[slot]);
        g_sceneReady = true;
        g_framesWithScene++;
        if (!g_loggedOk) {
            g_loggedOk = true;
            Log("ui split: working (final pass reads the UI target at slot t%d); HUD and menus go to a flat layer",
                slot);
        }
    }
    if (rtv) rtv->Release();
    if (dsv) dsv->Release();
    for (auto& s : srvs)
        if (s) s->Release();
}

ID3D11Texture2D* UiSceneTexture() { return g_sceneReady ? g_scene : nullptr; }

// BothEyes, during the right eye's playback (which ended where the UI starts): the UI target now
// holds the right eye's finished scene. Copy it and run the game's final pass on the copy into our
// scene texture, exactly as for the left eye but without any UI (alpha = no coverage).
bool UiSecondEyeScene(ID3D11DeviceContext* c) {
    if (!g_finalOk || !g_sceneReady || !g_uiTarget || !g_preUI || !g_preSRV || !g_sceneRTV) return false;
    c->CopyResource(g_preUI, g_uiTarget);
    const FLOAT zero[4] = {0, 0, 0, 0};
    c->ClearRenderTargetView(g_sceneRTV, zero);
    ReplayApplyFinalPassState(c);
    c->PSSetShaderResources((UINT)g_finalSlot, 1, &g_preSRV);
    ReplayInto(c, g_sceneRTV, g_finalArgs, 1);
    return true;
}
