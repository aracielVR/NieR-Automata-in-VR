// probe.cpp - camera probe.
//
// Press [Debug] CaptureKey (F11) in game: the next complete frame of the game's D3D11 activity is
// recorded (constant-buffer contents, shader binds, draws, render targets, shader bytecode) to
// NieRVR_capture_N.bin, and likely projection matrices are summarised in the log. The capture is
// analysed offline (tools/analyze_capture.py) to find where NieR keeps its camera matrices.
//
// Hooks cost almost nothing while not capturing: one pointer compare and a flag check per call.
#include "common.h"
#include <d3dcompiler.h>

namespace {

enum RecType : uint16_t {
    R_FRAME_BEGIN = 1,  // u32 frame, u32 0, u32 0
    R_SET_CB = 2,       // u8 stage, u8 pad[3], u32 start, u32 count, u64 bufs[count]
    R_CB_DATA = 3,      // u64 buf, u32 byteWidth, u8 via(0 map,1 update,2 readback), u8 pad[3], data
    R_SET_SHADER = 4,   // u8 stage, pad[7], u64 shader
    R_DRAW = 5,         // u8 kind(0 Draw,1 Indexed,2 Inst,3 IndexedInst), pad[3], u32 a, u32 b, u32 c
    R_SET_RT = 6,       // u32 numRTV, u32 w, u32 h, u32 fmt, u64 dsv, u64 rtv0
    R_VIEWPORT = 7,     // u32 n, then n * 6 floats
    R_SHADER_BLOB = 8,  // u8 stage, pad[7], u64 shader, u64 hash, bytecode
    R_DISPATCH = 9,     // u32 x, y, z
    R_EXEC_CMDLIST = 10,
    R_FRAME_END = 12,
    R_SET_SRV = 13,     // u8 stage, pad[3], u32 start, u32 count, count * {u64 res, u32 w, u32 h, u32 fmt, u32 dim}
    R_SET_UAV = 14,     // same layout as R_SET_SRV (stage = CS)
};
enum Stage : uint8_t { ST_VS = 0, ST_PS = 1, ST_GS = 2, ST_CS = 3 };

// vtable slots (verified against the d3d11.h layout at build time, see tools/vtable_check.c)
enum {
    CTX_VSSetConstantBuffers = 7, CTX_PSSetShader = 9, CTX_VSSetShader = 11, CTX_DrawIndexed = 12, CTX_Draw = 13,
    CTX_Map = 14, CTX_Unmap = 15, CTX_PSSetConstantBuffers = 16, CTX_DrawIndexedInstanced = 20, CTX_DrawInstanced = 21,
    CTX_GSSetConstantBuffers = 22, CTX_OMSetRenderTargets = 33, CTX_Dispatch = 41, CTX_RSSetViewports = 44,
    CTX_UpdateSubresource = 48, CTX_ExecuteCommandList = 58, CTX_CSSetConstantBuffers = 71,
    CTX_PSSetShaderResources = 8, CTX_CSSetShaderResources = 67, CTX_CSSetUnorderedAccessViews = 68,
    CTX_CSSetShader = 69,
    DEV_CreateVertexShader = 12, DEV_CreatePixelShader = 15, DEV_CreateComputeShader = 18,
    DEV_CreateDeferredContext = 27,
};

ID3D11Device* g_dev;
ID3D11DeviceContext* g_ctx;
volatile bool g_capturing;
bool g_armed;
SRWLOCK g_recLock = SRWLOCK_INIT;
uint8_t* g_buf;
size_t g_len, g_cap;
bool g_overflow;
unsigned g_captureIndex;
bool g_keyWasDown;
volatile LONG g_deferredContexts;

// --- shader table: shader object -> bytecode copy -------------------------------------------
struct ShaderEntry {
    void* ptr;
    uint64_t hash;
    uint8_t* blob;
    uint32_t len;
    uint8_t stage;
    bool referenced;
    bool ui;   // vertex shader of NieR's 2D UI (uses the UIParam / cbUIParam cbuffers)
    bool sao;  // pixel shader of NieR's ambient occlusion (cbuffer variable "SaoGlobalParams")
    bool shadowMask;  // pixel shader of the screen-space shadow pass (reads "ShadowView_Buffer", not "ScatterParam")
    bool scatter;     // light scattering (fog / light shafts) pixel shader: "ScatterParam" (also reads the shadow map)
    bool vignette;    // tone-mapping pixel shader with "g_VignettoParam"
    uint8_t csKind;   // compute: 1 = tiled light culling ("g_LightAry"), 2 = probe selection ("g_CubeInput")
    bool effect;      // pixel shader of NieR's effect system (particles, screen filters): "EffectParameterIndex"
    uint32_t sampled; // texture registers the shader samples (bit mask, pure::dxbc_sampled_textures)
};
const uint32_t kShaderSlots = 1 << 16;
ShaderEntry* g_shaders;
SRWLOCK g_shaderLock = SRWLOCK_INIT;
uint32_t g_shaderCount;

uint32_t SlotOf(void* p) {
    uint64_t h = (uint64_t)(uintptr_t)p * 0x9E3779B97F4A7C15ull;
    return (uint32_t)(h >> 48) & (kShaderSlots - 1);
}

uint64_t Fnv1a(const uint8_t* d, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) h = (h ^ d[i]) * 1099511628211ull;
    return h;
}

void RememberShader(void* shader, const void* bytecode, SIZE_T len, uint8_t stage) {
    if (!g_shaders || !shader || !bytecode || len == 0 || len > (1u << 24)) return;
    uint8_t* copy = (uint8_t*)HeapAlloc(GetProcessHeap(), 0, len);
    if (!copy) return;
    memcpy(copy, bytecode, len);
    AcquireSRWLockExclusive(&g_shaderLock);
    uint32_t i = SlotOf(shader);
    for (uint32_t probe = 0; probe < kShaderSlots; probe++, i = (i + 1) & (kShaderSlots - 1)) {
        ShaderEntry& e = g_shaders[i];
        if (e.ptr == shader || e.ptr == nullptr) {
            if (e.blob) HeapFree(GetProcessHeap(), 0, e.blob);  // pointer reused by a new shader
            if (!e.ptr) g_shaderCount++;
            e.ptr = shader;
            e.hash = Fnv1a(copy, len);
            e.blob = copy;
            e.len = (uint32_t)len;
            e.stage = stage;
            e.referenced = false;
            e.ui = false;
            e.sao = false;
            e.shadowMask = false;
            e.scatter = false;
            e.vignette = false;
            e.csKind = 0;
            e.effect = false;
            e.sampled = stage == ST_PS ? pure::dxbc_sampled_textures(copy, len) : 0;
            if (stage == ST_CS)
                for (uint32_t k = 0; k + 11 <= len && !e.csKind; k++) {
                    if (memcmp(copy + k, "g_LightAry", 10) == 0) e.csKind = 1;
                    else if (memcmp(copy + k, "g_CubeInput", 11) == 0) e.csKind = 2;
                }
            // The reflection data names the cbuffers; UI vertex shaders use "UIParam"/"cbUIParam".
            if (stage == ST_VS)
                for (uint32_t k = 0; k + 7 <= len && !e.ui; k++)
                    e.ui = memcmp(copy + k, "UIParam", 7) == 0;
            if (stage == ST_PS)
                for (uint32_t k = 0; k + 15 <= len && !e.sao; k++)
                    e.sao = memcmp(copy + k, "SaoGlobalParams", 15) == 0;
            if (stage == ST_PS)
                for (uint32_t k = 0; k + 17 <= len && !e.shadowMask; k++)
                    e.shadowMask = memcmp(copy + k, "ShadowView_Buffer", 17) == 0;
            // 0.4.4: the light-scattering shader reads ShadowView_Buffer too; "Shadows off" cleared its
            // target to white = dense grey fog (user report). It is not the shadow mask.
            if (stage == ST_PS)
                for (uint32_t k = 0; k + 12 <= len && !e.scatter; k++)
                    e.scatter = memcmp(copy + k, "ScatterParam", 12) == 0;
            if (e.scatter) e.shadowMask = false;
            if (stage == ST_PS)
                for (uint32_t k = 0; k + 15 <= len && !e.vignette; k++)
                    e.vignette = memcmp(copy + k, "g_VignettoParam", 15) == 0;
            if (stage == ST_PS)
                for (uint32_t k = 0; k + 20 <= len && !e.effect; k++)
                    e.effect = memcmp(copy + k, "EffectParameterIndex", 20) == 0;
            ReleaseSRWLockExclusive(&g_shaderLock);
            return;
        }
    }
    ReleaseSRWLockExclusive(&g_shaderLock);
    HeapFree(GetProcessHeap(), 0, copy);  // table full
}

ShaderEntry* FindShader(void* shader) {
    if (!g_shaders || !shader) return nullptr;
    uint32_t i = SlotOf(shader);
    for (uint32_t probe = 0; probe < kShaderSlots; probe++, i = (i + 1) & (kShaderSlots - 1)) {
        if (g_shaders[i].ptr == shader) return &g_shaders[i];
        if (!g_shaders[i].ptr) return nullptr;
    }
    return nullptr;
}

// --- recording -------------------------------------------------------------------------------
struct RecHeader {
    uint16_t type;
    uint16_t pad;
    uint32_t size;
};

void Emit(uint16_t type, const void* a, uint32_t alen, const void* b = nullptr, uint32_t blen = 0) {
    AcquireSRWLockExclusive(&g_recLock);
    size_t need = sizeof(RecHeader) + alen + blen;
    if (!g_buf || g_len + need > g_cap) {
        g_overflow = true;
    } else {
        RecHeader h = {type, 0, alen + blen};
        memcpy(g_buf + g_len, &h, sizeof(h));
        memcpy(g_buf + g_len + sizeof(h), a, alen);
        if (blen) memcpy(g_buf + g_len + sizeof(h) + alen, b, blen);
        g_len += need;
    }
    ReleaseSRWLockExclusive(&g_recLock);
}

// Constant buffers seen this frame: kept alive (AddRef) until the frame is written.
struct SeenBuf {
    ID3D11Buffer* buf;
    bool hasData;
};
const int kMaxSeen = 8192;
SeenBuf g_seen[kMaxSeen];
int g_seenCount;

SeenBuf* SeenFind(ID3D11Buffer* b) {
    for (int i = 0; i < g_seenCount; i++)
        if (g_seen[i].buf == b) return &g_seen[i];
    return nullptr;
}
void SeenAdd(ID3D11Buffer* b, bool data) {
    if (!b) return;
    AcquireSRWLockExclusive(&g_recLock);
    SeenBuf* s = SeenFind(b);
    if (!s && g_seenCount < kMaxSeen) {
        s = &g_seen[g_seenCount++];
        s->buf = b;
        s->hasData = false;
        b->AddRef();
    }
    if (s && data) s->hasData = true;
    ReleaseSRWLockExclusive(&g_recLock);
}

// Buffers currently mapped (Map .. Unmap) during the capture.
struct Mapped {
    ID3D11Resource* res;
    void* data;
    UINT size;
};
Mapped g_mapped[128];

bool IsConstantBuffer(ID3D11Resource* r, UINT* size) {
    D3D11_RESOURCE_DIMENSION dim;
    r->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) return false;
    D3D11_BUFFER_DESC d;
    ((ID3D11Buffer*)r)->GetDesc(&d);
    *size = d.ByteWidth;
    return (d.BindFlags & D3D11_BIND_CONSTANT_BUFFER) != 0;
}

void EmitCbData(ID3D11Buffer* buf, UINT byteWidth, uint8_t via, const void* data, UINT len) {
    struct {
        uint64_t buf;
        uint32_t byteWidth;
        uint8_t via, pad[3];
    } h = {(uint64_t)(uintptr_t)buf, byteWidth, via, {0, 0, 0}};
    static_assert(sizeof(h) == 16, "R_CB_DATA layout");
    Emit(R_CB_DATA, &h, sizeof(h), data, len);
    SeenAdd(buf, true);
}

bool IsUIShader(void* shader) {
    AcquireSRWLockShared(&g_shaderLock);
    ShaderEntry* e = FindShader(shader);
    bool ui = e && e->ui;
    ReleaseSRWLockShared(&g_shaderLock);
    return ui;
}

// Pixel shader classes we act on: 1 = SAO, 2 = shadow mask, 3 = vignette (tone mapping),
// 4 = effect system. `sampled` gets the texture registers it samples.
int PsClass(void* shader, uint32_t* sampled = nullptr, uint64_t* hash = nullptr) {
    AcquireSRWLockShared(&g_shaderLock);
    ShaderEntry* e = FindShader(shader);
    int k = !e ? 0 : e->sao ? 1 : e->shadowMask ? 2 : e->vignette ? 3 : e->effect ? 4 : e->scatter ? 5 : 0;
    if (sampled) *sampled = e ? e->sampled : 0;
    if (hash) *hash = e ? e->hash : 0;
    ReleaseSRWLockShared(&g_shaderLock);
    return k;
}

void MarkShader(void* shader) {
    AcquireSRWLockShared(&g_shaderLock);
    ShaderEntry* e = FindShader(shader);
    if (e) e->referenced = true;
    ReleaseSRWLockShared(&g_shaderLock);
}

// --- hooks -----------------------------------------------------------------------------------
typedef void(STDMETHODCALLTYPE* PFN_SetCB)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*);
typedef void(STDMETHODCALLTYPE* PFN_SetShader)(ID3D11DeviceContext*, void*, ID3D11ClassInstance* const*, UINT);
typedef void(STDMETHODCALLTYPE* PFN_DrawIndexed)(ID3D11DeviceContext*, UINT, UINT, INT);
typedef void(STDMETHODCALLTYPE* PFN_Draw)(ID3D11DeviceContext*, UINT, UINT);
typedef void(STDMETHODCALLTYPE* PFN_DrawIndexedInstanced)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
typedef void(STDMETHODCALLTYPE* PFN_DrawInstanced)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_Map)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT,
                                            D3D11_MAPPED_SUBRESOURCE*);
typedef void(STDMETHODCALLTYPE* PFN_Unmap)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
typedef void(STDMETHODCALLTYPE* PFN_OMSetRT)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*,
                                             ID3D11DepthStencilView*);
typedef void(STDMETHODCALLTYPE* PFN_UpdateSubresource)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*,
                                                       const void*, UINT, UINT);
typedef void(STDMETHODCALLTYPE* PFN_RSSetViewports)(ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*);
typedef void(STDMETHODCALLTYPE* PFN_ExecuteCommandList)(ID3D11DeviceContext*, ID3D11CommandList*, BOOL);
typedef void(STDMETHODCALLTYPE* PFN_Dispatch)(ID3D11DeviceContext*, UINT, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateShader)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, void**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateDeferredContext)(ID3D11Device*, UINT, ID3D11DeviceContext**);

PFN_SetCB o_SetCB[4];
PFN_SetShader o_VSSetShader, o_PSSetShader;
PFN_DrawIndexed o_DrawIndexed;
PFN_Draw o_Draw;
PFN_DrawIndexedInstanced o_DrawIndexedInstanced;
PFN_DrawInstanced o_DrawInstanced;
PFN_Map o_Map;
PFN_Unmap o_Unmap;
PFN_OMSetRT o_OMSetRT;
PFN_UpdateSubresource o_UpdateSubresource;
PFN_RSSetViewports o_RSSetViewports;
PFN_ExecuteCommandList o_ExecuteCommandList;
PFN_Dispatch o_Dispatch;
PFN_CreateShader o_CreateVS, o_CreatePS, o_CreateCS;
PFN_CreateDeferredContext o_CreateDeferredContext;

inline bool Recording(ID3D11DeviceContext* c) { return g_capturing && c == g_ctx; }

template <int S>
void STDMETHODCALLTYPE H_SetCB(ID3D11DeviceContext* c, UINT start, UINT n, ID3D11Buffer* const* bufs) {
    if (g_replaying) return o_SetCB[S](c, start, n, bufs);
    if (S == ST_VS && c == g_ctx && bufs) UiOnSetVSConstantBuffers(start, n, bufs);
    if (RecOn(c)) RecSetCB(S == ST_VS ? RS_VS : S == ST_PS ? RS_PS : S == ST_GS ? RS_GS : RS_CS, start, n, bufs);
    if (Recording(c) && bufs && n <= 14) {
        struct {
            uint8_t stage, pad[3];
            uint32_t start, count;
        } h = {(uint8_t)S, {0, 0, 0}, start, n};
        static_assert(sizeof(h) == 12, "R_SET_CB layout");
        uint64_t ptrs[14];
        for (UINT i = 0; i < n; i++) {
            ptrs[i] = (uint64_t)(uintptr_t)bufs[i];
            SeenAdd(bufs[i], false);
        }
        Emit(R_SET_CB, &h, sizeof(h), ptrs, n * 8);
    }
    o_SetCB[S](c, start, n, bufs);
}

void EmitSetShader(uint8_t stage, void* sh) {
    struct {
        uint8_t stage, pad[7];
        uint64_t sh;
    } h = {stage, {0}, (uint64_t)(uintptr_t)sh};
    static_assert(sizeof(h) == 16, "R_SET_SHADER layout");
    Emit(R_SET_SHADER, &h, sizeof(h));
    MarkShader(sh);
}
void STDMETHODCALLTYPE H_VSSetShader(ID3D11DeviceContext* c, void* sh, ID3D11ClassInstance* const* ci, UINT n) {
    if (g_replaying) return o_VSSetShader(c, sh, ci, n);
    if (RecOn(c)) RecSetShader(RS_VS, sh, ci, n);
    if (c == g_ctx) UiOnSetVS(IsUIShader(sh));
    if (Recording(c)) EmitSetShader(ST_VS, sh);
    o_VSSetShader(c, sh, ci, n);
}
int g_psClass;          // PsClass() of the game's current pixel shader
uint32_t g_psSampled;   // and the texture registers it samples
uint64_t g_psHash;      // and its bytecode hash (stable between runs, for the log)
// Vignette: the 48-byte PSParamBuffer1 bound at b1 when the tone-mapping shader draws;
// g_VignettoParam.x (float 8) is the radius where darkening ends, as in
// v = pow(saturate((1 - |uv - 0.5|) / x), y). A tiny x keeps v = 1 everywhere.
ID3D11Resource* g_vigBuf;
void* g_vigMapped;
void PatchVignette(float* f) {
    if (!g_cfg.vignette) f[8] = 0.0001f;
}
void STDMETHODCALLTYPE H_PSSetShader(ID3D11DeviceContext* c, void* sh, ID3D11ClassInstance* const* ci, UINT n) {
    if (g_replaying) return o_PSSetShader(c, sh, ci, n);
    if (RecOn(c)) RecSetShader(RS_PS, sh, ci, n);
    if (c == g_ctx) {
        int was = g_psClass;
        g_psClass = PsClass(sh, &g_psSampled, &g_psHash);
        // GPU timing (0.4.4): the expensive full-screen passes get their own marks
        static const char* const kNames[6] = {nullptr, "AO (SAO)", "shadow mask", nullptr, nullptr, "light scattering"};
        if (g_psClass != was) {
            const char* n = (g_psClass >= 0 && g_psClass < 6) ? kNames[g_psClass] : nullptr;
            const char* w = (was >= 0 && was < 6) ? kNames[was] : nullptr;
            if (n) GpuProfMark(c, n);
            else if (w) GpuProfMark(c, "(after a full-screen pass)");
        }
    }
    if (Recording(c)) EmitSetShader(ST_PS, sh);
    o_PSSetShader(c, sh, ci, n);
}

void EmitDraw(uint8_t kind, UINT a, UINT b, UINT cc) {
    struct {
        uint8_t kind, pad[3];
        uint32_t a, b, c;
    } h = {kind, {0, 0, 0}, a, b, cc};
    static_assert(sizeof(h) == 16, "R_DRAW layout");
    Emit(R_DRAW, &h, sizeof(h));
}
// Shadows off (0.4.5): the game's shadow-mask draw runs with a pixel shader that just writes 1
// ("lit") instead of sampling the shadow map, keeping the game's own blend state, write mask, stencil
// and render targets. (0.3.x-0.4.4 skipped the draw and cleared the whole first render target to 1,
// which also wiped what the other channels hold: dense grey fog, user report.)
ID3D11PixelShader* g_litPS;
bool g_litTried;
ID3D11PixelShader* g_savedPS;
ID3D11ClassInstance* g_savedInst[8];
UINT g_savedInstN;
bool g_restorePS;
ID3D11PixelShader* LitShader() {
    if (g_litTried) return g_litPS;
    g_litTried = true;
    HMODULE comp = LoadLibraryW(L"d3dcompiler_47.dll");
    pD3DCompile compile = comp ? (pD3DCompile)(void*)GetProcAddress(comp, "D3DCompile") : nullptr;
    static const char src[] = "float4 main() : SV_Target { return 1; }";  // no inputs: links to any VS
    ID3DBlob *b = nullptr, *err = nullptr;
    HRESULT hr = compile ? compile(src, sizeof(src) - 1, "lit", nullptr, nullptr, "main", "ps_5_0", 0, 0, &b, &err) : E_FAIL;
    if (SUCCEEDED(hr) && g_dev) hr = g_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_litPS);
    if (FAILED(hr)) {
        g_litPS = nullptr;
        Log("shadows off: no replacement shader (0x%08x); the shadow pass stays on", (unsigned)hr);
    }
    if (b) b->Release();
    if (err) err->Release();
    return g_litPS;
}

// Ambient occlusion off: instead of the SAO pass, its output is cleared to 1 (no occlusion).
// Returns true when the draw must be skipped.
bool SkipDraw(ID3D11DeviceContext* c) {
    StereoCountDraw();
    if (g_psClass == 3 && !g_vigBuf) {  // learn which buffer holds the vignette parameters
        ID3D11Buffer* b = nullptr;
        c->PSGetConstantBuffers(1, 1, &b);
        if (b) {
            D3D11_BUFFER_DESC d;
            b->GetDesc(&d);
            if (d.ByteWidth == 48) {
                g_vigBuf = b;
                Log("vignette: parameters found (buffer of %u bytes); %s", d.ByteWidth,
                    g_cfg.vignette ? "kept (Vignette=1)" : "removed from the next frame on");
            }
            b->Release();
        }
    }
    if (g_psClass == 2 && !g_cfg.shadows && LitShader()) {
        // swap the pixel shader for this draw (through the hooked call, so BothEyes records it too)
        g_savedInstN = 8;
        c->PSGetShader(&g_savedPS, g_savedInst, &g_savedInstN);
        c->PSSetShader(g_litPS, nullptr, 0);
        g_restorePS = true;
        return false;
    }
    bool clear = g_psClass == 1 && !g_cfg.ambientOcclusion;
    if (!clear) return false;
    ID3D11RenderTargetView* rtv = nullptr;
    c->OMGetRenderTargets(1, &rtv, nullptr);
    if (rtv) {
        const FLOAT one[4] = {1, 1, 1, 1};
        c->ClearRenderTargetView(rtv, one);
        rtv->Release();
    }
    return true;
}

// Shared by the four draw hooks: true = the game's draw must not run.
bool PreDraw(ID3D11DeviceContext* c) {
    CapturePollMidFrame();
    if (SkipDraw(c)) return true;
    if (UiBeforeDraw(c)) return true;
    if (g_psClass == 4 && ScreenFxSkipEffectDraw(c, g_psSampled, g_psHash)) return true;
    GpuProfDraw();
    return false;
}
void PostDraw(ID3D11DeviceContext* c, const DrawArgs& a) {
    if (g_restorePS) {  // shadows off: the game's shadow shader back for whatever it draws next
        g_restorePS = false;
        c->PSSetShader(g_savedPS, g_savedInstN ? g_savedInst : nullptr, g_savedInstN);
        if (g_savedPS) g_savedPS->Release();
        for (UINT i = 0; i < g_savedInstN && i < 8; i++)
            if (g_savedInst[i]) g_savedInst[i]->Release();
        g_savedPS = nullptr;
        g_savedInstN = 0;
    }
    UiAfterDraw(c, a);
}

void STDMETHODCALLTYPE H_Draw(ID3D11DeviceContext* c, UINT n, UINT s) {
    if (g_replaying) return o_Draw(c, n, s);
    bool g = c == g_ctx;
    if (g && PreDraw(c)) return;
    if (RecOn(c)) RecDraw(0, n, s, 0, 0, 0);
    if (Recording(c)) EmitDraw(0, n, s, 0);
    o_Draw(c, n, s);
    if (g) PostDraw(c, {0, n, s, 0, 0, 0});
}
void STDMETHODCALLTYPE H_DrawIndexed(ID3D11DeviceContext* c, UINT n, UINT s, INT b) {
    if (g_replaying) return o_DrawIndexed(c, n, s, b);
    bool g = c == g_ctx;
    if (g && PreDraw(c)) return;
    if (RecOn(c)) RecDraw(1, n, s, b, 0, 0);
    if (Recording(c)) EmitDraw(1, n, s, (UINT)b);
    o_DrawIndexed(c, n, s, b);
    if (g) PostDraw(c, {1, n, s, b, 0, 0});
}
void STDMETHODCALLTYPE H_DrawInstanced(ID3D11DeviceContext* c, UINT v, UINT i, UINT sv, UINT si) {
    if (g_replaying) return o_DrawInstanced(c, v, i, sv, si);
    bool g = c == g_ctx;
    if (g && PreDraw(c)) return;
    if (RecOn(c)) RecDraw(2, v, i, (INT)sv, si, 0);
    if (Recording(c)) EmitDraw(2, v, i, sv);
    o_DrawInstanced(c, v, i, sv, si);
    if (g) PostDraw(c, {2, v, i, (INT)sv, si, 0});
}
void STDMETHODCALLTYPE H_DrawIndexedInstanced(ID3D11DeviceContext* c, UINT n, UINT i, UINT s, INT b, UINT si) {
    if (g_replaying) return o_DrawIndexedInstanced(c, n, i, s, b, si);
    bool g = c == g_ctx;
    if (g && PreDraw(c)) return;
    if (RecOn(c)) RecDraw(3, n, i, (INT)s, (UINT)b, si);
    if (Recording(c)) EmitDraw(3, n, i, s);
    o_DrawIndexedInstanced(c, n, i, s, b, si);
    if (g) PostDraw(c, {3, n, i, (INT)s, (UINT)b, si});
}

HRESULT STDMETHODCALLTYPE H_Map(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, D3D11_MAP type, UINT flags,
                                D3D11_MAPPED_SUBRESOURCE* m) {
    if (!g_replaying && r && RecOn(c)) RecBeforeMap(r, type);
    HRESULT hr = o_Map(c, r, sub, type, flags, m);
    if (g_replaying) return hr;
    if (SUCCEEDED(hr) && RecOn(c)) RecMap(r, sub, type, flags, m);
    UINT size;
    if (c != g_ctx || FAILED(hr) || !m || !r || type == D3D11_MAP_READ) return hr;
    bool needStereo = StereoWatching();
    if (!needStereo && !g_capturing && !g_cfg.uiSplit && !g_vigBuf) return hr;
    if (!IsConstantBuffer(r, &size)) return hr;
    if (needStereo) StereoOnMap(r, m->pData, size);
    if (r == g_vigBuf && size == 48) g_vigMapped = m->pData;
    if (g_cfg.uiSplit) UiOnMap(r, m->pData, size);
    if (Recording(c)) {
        AcquireSRWLockExclusive(&g_recLock);
        for (auto& e : g_mapped)
            if (!e.res) {
                e = {r, m->pData, size};
                break;
            }
        ReleaseSRWLockExclusive(&g_recLock);
    }
    return hr;
}

void STDMETHODCALLTYPE H_Unmap(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub) {
    if (g_replaying) return o_Unmap(c, r, sub);
    if (RecOn(c) && r) RecUnmap(r, sub);  // the game's own data, before the camera is patched in
    if (c == g_ctx && r) {
        StereoBeforeUnmap(r);  // patch the camera for this frame's eye
        UiBeforeUnmap(r);      // remember UI draw parameters
        if (r == g_vigBuf && g_vigMapped) {
            PatchVignette((float*)g_vigMapped);
            g_vigMapped = nullptr;
        }
    }
    if (Recording(c) && r) {
        Mapped found = {};
        AcquireSRWLockExclusive(&g_recLock);
        for (auto& e : g_mapped)
            if (e.res == r) {
                found = e;
                e.res = nullptr;
                break;
            }
        ReleaseSRWLockExclusive(&g_recLock);
        if (found.res && found.data) EmitCbData((ID3D11Buffer*)r, found.size, 0, found.data, found.size);
    }
    o_Unmap(c, r, sub);
}

void STDMETHODCALLTYPE H_UpdateSubresource(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, const D3D11_BOX* box,
                                           const void* data, UINT rp, UINT dp) {
    if (g_replaying) return o_UpdateSubresource(c, r, sub, box, data, rp, dp);
    if (RecOn(c)) RecUpdate(r, sub, box, data, rp, dp);
    UINT size;
    float scratch[64];
    if (c == g_ctx && r && data && !box && (StereoWatching() || g_capturing || g_cfg.uiSplit || g_vigBuf) &&
        IsConstantBuffer(r, &size)) {
        // Constant buffers can only be updated whole, so `data` holds byteWidth bytes.
        if (StereoWatching()) data = StereoPatchUpdate(size, data, scratch);
        if (r == g_vigBuf && size == 48) {
            if (data != scratch) memcpy(scratch, data, 48);
            PatchVignette(scratch);
            data = scratch;
        }
        if (g_cfg.uiSplit) UiOnUpdate(r, data, size);
        if (Recording(c)) EmitCbData((ID3D11Buffer*)r, size, 1, data, size);
    }
    o_UpdateSubresource(c, r, sub, box, data, rp, dp);
}

void STDMETHODCALLTYPE H_OMSetRT(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtvs,
                                 ID3D11DepthStencilView* dsv) {
    if (g_replaying) return o_OMSetRT(c, n, rtvs, dsv);
    if (RecOn(c)) RecOMSetRT(n, rtvs, dsv);
    if (c == g_ctx) {
        GpuProfTargets(c, n, rtvs, dsv, "");
        if (dsv) ReprojectNoteDsv(dsv);
    }
    if (Recording(c)) {
        struct {
            uint32_t n, w, h, fmt;
            uint64_t dsv, rtv0;
        } rec = {n, 0, 0, 0, (uint64_t)(uintptr_t)dsv, 0};
        static_assert(sizeof(rec) == 32, "R_SET_RT layout");
        ID3D11View* view = (n && rtvs && rtvs[0]) ? (ID3D11View*)rtvs[0] : (ID3D11View*)dsv;
        if (n && rtvs) rec.rtv0 = (uint64_t)(uintptr_t)rtvs[0];
        if (view) {
            ID3D11Resource* res = nullptr;
            view->GetResource(&res);
            D3D11_RESOURCE_DIMENSION dim;
            if (res) {
                res->GetType(&dim);
                if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
                    D3D11_TEXTURE2D_DESC td;
                    ((ID3D11Texture2D*)res)->GetDesc(&td);
                    rec.w = td.Width;
                    rec.h = td.Height;
                    rec.fmt = td.Format;
                }
                // record resources (not views), so render targets can be matched with later SRV reads
                if (n && rtvs && rtvs[0]) rec.rtv0 = (uint64_t)(uintptr_t)res;
                res->Release();
            }
        }
        if (dsv) {
            ID3D11Resource* dres = nullptr;
            dsv->GetResource(&dres);
            if (dres) {
                rec.dsv = (uint64_t)(uintptr_t)dres;
                dres->Release();
            }
        }
        Emit(R_SET_RT, &rec, sizeof(rec));
    }
    o_OMSetRT(c, n, rtvs, dsv);
}

void STDMETHODCALLTYPE H_RSSetViewports(ID3D11DeviceContext* c, UINT n, const D3D11_VIEWPORT* vp) {
    if (g_replaying) return o_RSSetViewports(c, n, vp);
    if (RecOn(c)) RecViewports(n, vp);
    if (Recording(c) && vp && n <= 16) {
        uint32_t nn = n;
        Emit(R_VIEWPORT, &nn, 4, vp, n * sizeof(D3D11_VIEWPORT));
    }
    o_RSSetViewports(c, n, vp);
}

void STDMETHODCALLTYPE H_ExecuteCommandList(ID3D11DeviceContext* c, ID3D11CommandList* cl, BOOL restore) {
    if (RecOn(c)) RecUnsupported("ExecuteCommandList (deferred rendering)");
    if (Recording(c)) {
        uint32_t z = 0;
        Emit(R_EXEC_CMDLIST, &z, 4);
    }
    o_ExecuteCommandList(c, cl, restore);
}

// Shader-resource / UAV bindings (capture only): which textures and buffers each pass reads and
// writes, so data flow between passes (e.g. compute passes) can be followed offline.
struct ViewRec {
    uint64_t res;
    uint32_t w, h, fmt, dim;
};
static_assert(sizeof(ViewRec) == 24, "ViewRec layout");
template <typename V>
void DescribeView(V* v, ViewRec* r) {
    *r = {};
    if (!v) return;
    ID3D11Resource* res = nullptr;
    v->GetResource(&res);
    if (!res) return;
    r->res = (uint64_t)(uintptr_t)res;
    D3D11_RESOURCE_DIMENSION dim;
    res->GetType(&dim);
    r->dim = (uint32_t)dim;
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        D3D11_TEXTURE2D_DESC d;
        ((ID3D11Texture2D*)res)->GetDesc(&d);
        r->w = d.Width;
        r->h = d.Height;
        r->fmt = d.Format;
    } else if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
        D3D11_BUFFER_DESC d;
        ((ID3D11Buffer*)res)->GetDesc(&d);
        r->w = d.ByteWidth;
        r->h = d.StructureByteStride;
    }
    res->Release();
}
// NieR binds pixel shader resources many slots at a time (more than 16: 0.3.12 dropped those calls
// and its captures showed almost no texture bindings), so long calls go out in chunks of 16, without
// the trailing empty slots.
template <typename V>
void EmitViews(uint16_t type, uint8_t stage, UINT start, UINT n, V* const* views) {
    if (!views) return;
    if (n > 128) n = 128;
    while (n > 0 && !views[n - 1]) n--;
    for (UINT base = 0; base < n; base += 16) {
        UINT k = n - base < 16 ? n - base : 16;
        struct {
            uint8_t stage, pad[3];
            uint32_t start, count;
        } h = {stage, {0, 0, 0}, start + base, k};
        static_assert(sizeof(h) == 12, "R_SET_SRV layout");
        ViewRec recs[16];
        for (UINT i = 0; i < k; i++) DescribeView(views[base + i], &recs[i]);
        Emit(type, &h, sizeof(h), recs, k * sizeof(ViewRec));
    }
}
typedef void(STDMETHODCALLTYPE* PFN_SetSRV)(ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);
typedef void(STDMETHODCALLTYPE* PFN_SetUAV)(ID3D11DeviceContext*, UINT, UINT, ID3D11UnorderedAccessView* const*,
                                            const UINT*);
PFN_SetSRV o_PSSetSRV, o_CSSetSRV;
PFN_SetUAV o_CSSetUAV;
PFN_SetShader o_CSSetShader;
void STDMETHODCALLTYPE H_PSSetSRV(ID3D11DeviceContext* c, UINT s, UINT n, ID3D11ShaderResourceView* const* v) {
    if (g_replaying) return o_PSSetSRV(c, s, n, v);
    if (RecOn(c)) RecSetSRV(RS_PS, s, n, v);
    if (Recording(c)) EmitViews(R_SET_SRV, ST_PS, s, n, v);
    o_PSSetSRV(c, s, n, v);
}
void STDMETHODCALLTYPE H_CSSetSRV(ID3D11DeviceContext* c, UINT s, UINT n, ID3D11ShaderResourceView* const* v) {
    if (g_replaying) return o_CSSetSRV(c, s, n, v);
    if (RecOn(c)) RecSetSRV(RS_CS, s, n, v);
    if (Recording(c)) EmitViews(R_SET_SRV, ST_CS, s, n, v);
    o_CSSetSRV(c, s, n, v);
}
void STDMETHODCALLTYPE H_CSSetUAV(ID3D11DeviceContext* c, UINT s, UINT n, ID3D11UnorderedAccessView* const* v,
                                  const UINT* counts) {
    if (g_replaying) return o_CSSetUAV(c, s, n, v, counts);
    if (RecOn(c)) RecCSSetUAV(s, n, v, counts);
    if (Recording(c)) EmitViews(R_SET_UAV, ST_CS, s, n, v);
    o_CSSetUAV(c, s, n, v, counts);
}
int g_csKind;
int CsKind(void* shader) {
    AcquireSRWLockShared(&g_shaderLock);
    ShaderEntry* e = FindShader(shader);
    int k = e ? e->csKind : 0;
    ReleaseSRWLockShared(&g_shaderLock);
    return k;
}
void STDMETHODCALLTYPE H_CSSetShader(ID3D11DeviceContext* c, void* sh, ID3D11ClassInstance* const* ci, UINT n) {
    if (g_replaying) return o_CSSetShader(c, sh, ci, n);
    if (RecOn(c)) RecSetShader(RS_CS, sh, ci, n);
    if (c == g_ctx) g_csKind = CsKind(sh);
    if (Recording(c)) EmitSetShader(ST_CS, sh);
    o_CSSetShader(c, sh, ci, n);
}

void STDMETHODCALLTYPE H_Dispatch(ID3D11DeviceContext* c, UINT x, UINT y, UINT z) {
    if (g_replaying) return o_Dispatch(c, x, y, z);
    if (RecOn(c)) RecDispatch(x, y, z, c == g_ctx ? g_csKind : 0);
    if (c == g_ctx) GpuProfCompute(c, "", g_csKind);
    if (Recording(c)) {
        uint32_t v[3] = {x, y, z};
        Emit(R_DISPATCH, v, 12);
    }
    bool fix = c == g_ctx && g_csKind;
    if (fix) {  // the fix's own calls aren't the game's: not recorded (playback runs the fix again)
        InterlockedIncrement(&g_recSuspend);
        ViewFixBeforeDispatch(c, g_csKind);
        InterlockedDecrement(&g_recSuspend);
    }
    o_Dispatch(c, x, y, z);
    if (fix) {
        InterlockedIncrement(&g_recSuspend);
        ViewFixAfterDispatch(c);
        InterlockedDecrement(&g_recSuspend);
    }
}

HRESULT STDMETHODCALLTYPE H_CreateVS(ID3D11Device* d, const void* bc, SIZE_T len, ID3D11ClassLinkage* l, void** out) {
    HRESULT hr = o_CreateVS(d, bc, len, l, out);
    if (d == g_dev && SUCCEEDED(hr) && out) RememberShader(*out, bc, len, ST_VS);
    return hr;
}
HRESULT STDMETHODCALLTYPE H_CreatePS(ID3D11Device* d, const void* bc, SIZE_T len, ID3D11ClassLinkage* l, void** out) {
    HRESULT hr = o_CreatePS(d, bc, len, l, out);
    if (d == g_dev && SUCCEEDED(hr) && out) RememberShader(*out, bc, len, ST_PS);
    return hr;
}
HRESULT STDMETHODCALLTYPE H_CreateCS(ID3D11Device* d, const void* bc, SIZE_T len, ID3D11ClassLinkage* l, void** out) {
    HRESULT hr = o_CreateCS(d, bc, len, l, out);
    if (d == g_dev && SUCCEEDED(hr) && out) RememberShader(*out, bc, len, ST_CS);
    return hr;
}
HRESULT STDMETHODCALLTYPE H_CreateDeferredContext(ID3D11Device* d, UINT flags, ID3D11DeviceContext** out) {
    HRESULT hr = o_CreateDeferredContext(d, flags, out);
    if (d == g_dev && SUCCEEDED(hr)) {
        LONG n = InterlockedIncrement(&g_deferredContexts);
        Log("probe: game created deferred context #%d (multithreaded rendering)", (int)n);
    }
    return hr;
}

bool Hook(void** vtbl, int index, void* detour, void* orig, const char* name) {
    return HookFunction(vtbl[index], detour, (void**)orig, name);
}

// --- frame end: readback, shader blobs, file, summary ---------------------------------------
void ReadbackUnseenBuffers(int* readbacks) {
    for (int i = 0; i < g_seenCount; i++) {
        SeenBuf& s = g_seen[i];
        if (s.hasData) continue;
        D3D11_BUFFER_DESC d;
        s.buf->GetDesc(&d);
        if (d.ByteWidth > 65536) continue;
        d.Usage = D3D11_USAGE_STAGING;
        d.BindFlags = 0;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        d.MiscFlags = 0;
        d.StructureByteStride = 0;
        ID3D11Buffer* staging = nullptr;
        if (FAILED(g_dev->CreateBuffer(&d, nullptr, &staging))) continue;
        g_ctx->CopyResource(staging, s.buf);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(g_ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
            struct {
                uint64_t buf;
                uint32_t byteWidth;
                uint8_t via, pad[3];
            } h = {(uint64_t)(uintptr_t)s.buf, d.ByteWidth, 2, {0, 0, 0}};
            static_assert(sizeof(h) == 16, "R_CB_DATA layout");
            Emit(R_CB_DATA, &h, sizeof(h), m.pData, d.ByteWidth);
            g_ctx->Unmap(staging, 0);
            (*readbacks)++;
        }
        staging->Release();
    }
}

void EmitShaderBlobs(int* count) {
    AcquireSRWLockExclusive(&g_shaderLock);
    for (uint32_t i = 0; i < kShaderSlots; i++) {
        ShaderEntry& e = g_shaders[i];
        if (!e.ptr || !e.referenced) continue;
        e.referenced = false;
        struct {
            uint8_t stage, pad[7];
            uint64_t sh, hash;
        } h = {e.stage, {0}, (uint64_t)(uintptr_t)e.ptr, e.hash};
        static_assert(sizeof(h) == 24, "R_SHADER_BLOB layout");
        Emit(R_SHADER_BLOB, &h, sizeof(h), e.blob, e.len);
        (*count)++;
    }
    ReleaseSRWLockExclusive(&g_shaderLock);
}

// Logs the projection matrices found in this frame's constant buffers.
void Summarise() {
    struct Cand {
        uint64_t buf;
        uint32_t offset;
        pure::ProjInfo p;
        unsigned draws;
    } cands[24];
    int nc = 0;
    unsigned draws = 0, cbData = 0, rts = 0, dispatches = 0;
    for (size_t off = 0; off + sizeof(RecHeader) <= g_len;) {
        RecHeader h;
        memcpy(&h, g_buf + off, sizeof(h));
        const uint8_t* p = g_buf + off + sizeof(h);
        if (h.type == R_DRAW) draws++;
        if (h.type == R_SET_RT) rts++;
        if (h.type == R_DISPATCH) dispatches++;
        if (h.type == R_CB_DATA && h.size >= 16) {
            cbData++;
            uint64_t buf;
            memcpy(&buf, p, 8);
            const uint8_t* data = p + 16;
            uint32_t len = h.size - 16;
            for (uint32_t o = 0; o + 64 <= len && nc < 24; o += 16) {
                float m[16];
                memcpy(m, data + o, 64);
                pure::ProjInfo pi;
                if (!pure::classify_projection(m, &pi)) continue;
                bool dup = false;
                for (int i = 0; i < nc; i++)
                    if (cands[i].buf == buf && cands[i].offset == o) dup = true;
                if (!dup) cands[nc++] = {buf, o, pi, 0};
            }
        }
        off += sizeof(h) + h.size;
    }
    // how many draws had each candidate buffer bound (any stage)
    uint64_t bound[4][14] = {};
    for (size_t off = 0; off + sizeof(RecHeader) <= g_len;) {
        RecHeader h;
        memcpy(&h, g_buf + off, sizeof(h));
        const uint8_t* p = g_buf + off + sizeof(h);
        if (h.type == R_SET_CB) {
            uint8_t st = p[0];
            uint32_t start, count;
            memcpy(&start, p + 4, 4);
            memcpy(&count, p + 8, 4);
            for (uint32_t i = 0; i < count && st < 4 && start + i < 14; i++) memcpy(&bound[st][start + i], p + 12 + i * 8, 8);
        } else if (h.type == R_DRAW) {
            for (int i = 0; i < nc; i++) {
                bool used = false;
                for (int s = 0; s < 4 && !used; s++)
                    for (int k = 0; k < 14 && !used; k++) used = bound[s][k] == cands[i].buf;
                if (used) cands[i].draws++;
            }
        }
        off += sizeof(h) + h.size;
    }
    Log("probe: frame had %u draws, %u dispatches, %u render-target changes, %u constant-buffer uploads%s", draws,
        dispatches, rts, cbData, g_overflow ? " (CAPTURE TRUNCATED: buffer full)" : "");
    if (!nc) Log("probe: no perspective projection matrix found in constant buffers this frame");
    for (int i = 0; i < nc; i++) {
        const pure::ProjInfo& p = cands[i].p;
        Log("probe: projection in cbuffer %p +%u: %s-major, vfov %.2f deg, aspect %.4f, off-centre (%.4f, %.4f), "
            "z = %.6f + %.6f/z, w sign %.0f, used by %u draws",
            (void*)(uintptr_t)cands[i].buf, cands[i].offset, p.rowMajor ? "row" : "column", (double)p.vfovDeg,
            (double)p.aspect, (double)p.cx, (double)p.cy, (double)p.A, (double)p.B, (double)p.wSign, cands[i].draws);
    }
}

void WriteCapture() {
    wchar_t path[MAX_PATH];
    lstrcpynW(path, g_modDir, MAX_PATH - 40);
    char name[48];
    pure::format(name, sizeof(name), "NieRVR_capture_%u.bin", g_captureIndex);
    int n = lstrlenW(path);
    for (int i = 0; name[i] && n < MAX_PATH - 1; i++) path[n++] = (wchar_t)name[i];
    path[n] = 0;
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        Log("ERROR: probe: cannot write %s (error %u)", name, (unsigned)GetLastError());
        return;
    }
    const char magic[8] = {'N', 'V', 'R', 'C', 'A', 'P', '0', '1'};
    DWORD w;
    WriteFile(f, magic, 8, &w, nullptr);
    for (size_t off = 0; off < g_len;) {
        DWORD chunk = (DWORD)(g_len - off > (16u << 20) ? (16u << 20) : g_len - off);
        if (!WriteFile(f, g_buf + off, chunk, &w, nullptr)) break;
        off += chunk;
    }
    CloseHandle(f);
    Log("probe: wrote %s (%u KB)", name, (unsigned)((g_len + 8) >> 10));
}

void FinishCapture() {
    g_capturing = false;
    int readbacks = 0, blobs = 0;
    ReadbackUnseenBuffers(&readbacks);
    EmitShaderBlobs(&blobs);
    uint32_t z[3] = {g_captureIndex, 0, 0};
    Emit(R_FRAME_END, z, 12);
    Summarise();
    Log("probe: %d constant buffers read back, %d shaders included, %u shaders known", readbacks, blobs, g_shaderCount);
    WriteCapture();
    AcquireSRWLockExclusive(&g_recLock);  // a late hook call on another thread must see g_buf == null
    uint8_t* buf = g_buf;
    g_buf = nullptr;
    g_len = g_cap = 0;
    for (int i = 0; i < g_seenCount; i++) g_seen[i].buf->Release();
    g_seenCount = 0;
    for (auto& e : g_mapped) e.res = nullptr;
    ReleaseSRWLockExclusive(&g_recLock);
    VirtualFree(buf, 0, MEM_RELEASE);
}

}  // namespace

// Frame queue (0.4.5): by default DXGI lets the game's CPU run up to 3 frames ahead of the GPU. The
// head pose is read when the CPU starts a frame, so each queued frame adds a frame time of delay
// before it is drawn (0.4.4 log: images on screen ~100 ms after their pose). [Render]
// MaxFrameLatency (1 by default; 0 = leave the game's) is applied to the game's device and checked
// every 2 s in case the game sets its own.
void FrameLatencyTick() {
    static DWORD last;
    static int applied = -1;
    static UINT gameOwn;  // the game's own queue length, seen before we changed it (0 = not yet)
    DWORD t = GetTickCount();
    if (!g_dev || (t - last < 2000 && applied == g_cfg.maxFrameLatency)) return;
    last = t;
    IDXGIDevice1* d = nullptr;
    if (FAILED(g_dev->QueryInterface(__uuidof(IDXGIDevice1), (void**)&d)) || !d) return;
    UINT cur = 0;
    d->GetMaximumFrameLatency(&cur);
    if (!gameOwn) gameOwn = cur ? cur : 3;
    if (g_cfg.maxFrameLatency == 0 && applied > 0 && cur != gameOwn) {  // back to the game's own
        d->SetMaximumFrameLatency(gameOwn);
        cur = gameOwn;
    }
    if (g_cfg.maxFrameLatency > 0 && cur != (UINT)g_cfg.maxFrameLatency) {
        HRESULT hr = d->SetMaximumFrameLatency((UINT)g_cfg.maxFrameLatency);
        static int logs;
        if (logs++ < 5)
            Log("frame queue: the game's device queued up to %u frames; now %d (0x%08x)%s", cur, g_cfg.maxFrameLatency,
                (unsigned)hr, logs == 5 ? " (not logged again)" : "");
    } else if (applied != g_cfg.maxFrameLatency) {
        Log("frame queue: up to %u frames (MaxFrameLatency=%d%s)", cur, g_cfg.maxFrameLatency,
            g_cfg.maxFrameLatency ? "" : ": the game's own");
    }
    applied = g_cfg.maxFrameLatency;
    d->Release();
}

void ProbeInstall(ID3D11Device* dev) {
    static volatile LONG done = 0;
    if (InterlockedCompareExchange(&done, 1, 0) != 0) return;
    if (!g_cfg.probe) Log("camera probe capture key disabled ([Debug] Probe=0)");
    g_dev = dev;
    dev->GetImmediateContext(&g_ctx);  // keep the reference: lives as long as the game's device
    g_shaders = (ShaderEntry*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(ShaderEntry) * kShaderSlots);

    void** dv = *(void***)dev;
    void** cv = *(void***)g_ctx;
    int ok = 0;
    ok += Hook(dv, DEV_CreateVertexShader, (void*)&H_CreateVS, &o_CreateVS, "CreateVertexShader");
    ok += Hook(dv, DEV_CreatePixelShader, (void*)&H_CreatePS, &o_CreatePS, "CreatePixelShader");
    ok += Hook(dv, DEV_CreateComputeShader, (void*)&H_CreateCS, &o_CreateCS, "CreateComputeShader");
    ok += Hook(dv, DEV_CreateDeferredContext, (void*)&H_CreateDeferredContext, &o_CreateDeferredContext,
               "CreateDeferredContext");
    ok += Hook(cv, CTX_VSSetConstantBuffers, (void*)&H_SetCB<ST_VS>, &o_SetCB[ST_VS], "VSSetConstantBuffers");
    ok += Hook(cv, CTX_PSSetConstantBuffers, (void*)&H_SetCB<ST_PS>, &o_SetCB[ST_PS], "PSSetConstantBuffers");
    ok += Hook(cv, CTX_GSSetConstantBuffers, (void*)&H_SetCB<ST_GS>, &o_SetCB[ST_GS], "GSSetConstantBuffers");
    ok += Hook(cv, CTX_CSSetConstantBuffers, (void*)&H_SetCB<ST_CS>, &o_SetCB[ST_CS], "CSSetConstantBuffers");
    ok += Hook(cv, CTX_VSSetShader, (void*)&H_VSSetShader, &o_VSSetShader, "VSSetShader");
    ok += Hook(cv, CTX_PSSetShader, (void*)&H_PSSetShader, &o_PSSetShader, "PSSetShader");
    ok += Hook(cv, CTX_Draw, (void*)&H_Draw, &o_Draw, "Draw");
    ok += Hook(cv, CTX_DrawIndexed, (void*)&H_DrawIndexed, &o_DrawIndexed, "DrawIndexed");
    ok += Hook(cv, CTX_DrawInstanced, (void*)&H_DrawInstanced, &o_DrawInstanced, "DrawInstanced");
    ok += Hook(cv, CTX_DrawIndexedInstanced, (void*)&H_DrawIndexedInstanced, &o_DrawIndexedInstanced,
               "DrawIndexedInstanced");
    ok += Hook(cv, CTX_Map, (void*)&H_Map, &o_Map, "Map");
    ok += Hook(cv, CTX_Unmap, (void*)&H_Unmap, &o_Unmap, "Unmap");
    ok += Hook(cv, CTX_UpdateSubresource, (void*)&H_UpdateSubresource, &o_UpdateSubresource, "UpdateSubresource");
    ok += Hook(cv, CTX_OMSetRenderTargets, (void*)&H_OMSetRT, &o_OMSetRT, "OMSetRenderTargets");
    ok += Hook(cv, CTX_RSSetViewports, (void*)&H_RSSetViewports, &o_RSSetViewports, "RSSetViewports");
    ok += Hook(cv, CTX_ExecuteCommandList, (void*)&H_ExecuteCommandList, &o_ExecuteCommandList, "ExecuteCommandList");
    ok += Hook(cv, CTX_Dispatch, (void*)&H_Dispatch, &o_Dispatch, "Dispatch");
    ok += Hook(cv, CTX_PSSetShaderResources, (void*)&H_PSSetSRV, &o_PSSetSRV, "PSSetShaderResources");
    ok += Hook(cv, CTX_CSSetShaderResources, (void*)&H_CSSetSRV, &o_CSSetSRV, "CSSetShaderResources");
    ok += Hook(cv, CTX_CSSetUnorderedAccessViews, (void*)&H_CSSetUAV, &o_CSSetUAV, "CSSetUnorderedAccessViews");
    ok += Hook(cv, CTX_CSSetShader, (void*)&H_CSSetShader, &o_CSSetShader, "CSSetShader");
    ReplayInstall(g_ctx);
    Log("D3D11 hooks ready: %d/25 installed (camera patching + probe; capture key 0x%x)", ok,
        (unsigned)g_cfg.keyCapture);
}

void ProbeRawDispatch(ID3D11DeviceContext* c, UINT x, UINT y, UINT z) { o_Dispatch(c, x, y, z); }

// Playback of a recorded constant-buffer upload for the second eye: the same camera patching as
// the live hooks (scene camera, CamParam, vignette).
const void* ProbePatchForReplay(ID3D11Resource* r, const void* data, UINT size, float* scratch) {
    data = StereoPatchUpdate(size, data, scratch);
    if (r == g_vigBuf && size == 48) {
        if (data != scratch) memcpy(scratch, data, 48);
        PatchVignette(scratch);
        data = scratch;
    }
    return data;
}

// Calls the original (unhooked) functions: used by the UI split to replay the final pass.
void ProbeReplayDraw(ID3D11DeviceContext* c, const DrawArgs& d) {
    switch (d.kind) {
        case 0: o_Draw(c, d.a, d.b); break;
        case 1: o_DrawIndexed(c, d.a, d.b, d.c); break;
        case 2: o_DrawInstanced(c, d.a, d.b, (UINT)d.c, d.d); break;
        case 3: o_DrawIndexedInstanced(c, d.a, d.b, (UINT)d.c, (INT)d.d, d.e); break;
    }
}
void ProbeSetRenderTargets(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtvs,
                           ID3D11DepthStencilView* dsv) {
    o_OMSetRT(c, n, rtvs, dsv);
}

void ProbeSetTarget(ID3D11Device* presentingDevice) {
    if (!g_dev || presentingDevice == g_dev) return;
    Log("probe: the presenting device differs from the first created device - following the presenting one");
    if (g_ctx) g_ctx->Release();
    g_dev = presentingDevice;
    g_dev->GetImmediateContext(&g_ctx);
}

void ProbeBeforePresent() {
    if (!g_ctx) return;
    if (g_capturing) FinishCapture();
    // hotkey (edge-triggered, only while the game window has focus)
    int vk = g_cfg.probe ? g_cfg.keyCapture : 0;
    if (vk > 0 && vk < 256) {
        bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
        bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;  // Ctrl+key is another hotkey
        if (down && !g_keyWasDown && !ctrl) {
            HWND fg = GetForegroundWindow();
            DWORD pid = 0;
            if (fg) GetWindowThreadProcessId(fg, &pid);
            if (pid == GetCurrentProcessId()) g_armed = true;
        }
        g_keyWasDown = down;
    }
}

void ProbeAfterPresent() {
    if (!g_armed || g_capturing) return;
    g_armed = false;
    g_cap = 256u << 20;  // reserved+committed lazily by the OS as it is touched
    g_buf = (uint8_t*)VirtualAlloc(nullptr, g_cap, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!g_buf) {
        Log("ERROR: probe: could not allocate capture memory");
        g_cap = 0;
        return;
    }
    g_len = 0;
    g_overflow = false;
    g_captureIndex++;
    uint32_t h[3] = {g_captureIndex, 0, 0};
    Emit(R_FRAME_BEGIN, h, 12);
    Log("probe: capturing frame (capture %u)", g_captureIndex);
    g_capturing = true;
}
static_assert(sizeof(D3D11_VIEWPORT) == 24, "R_VIEWPORT layout");
