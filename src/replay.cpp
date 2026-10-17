// replay.cpp - both eyes every frame ("BothEyes", 0.3.26, experimental).
//
// Alternate-eye rendering gives each eye a new image 30 times a second, a game frame apart, which
// makes fast camera motion uncomfortable. Here the game renders the left eye as usual while every
// call it makes on its immediate context is recorded (with the data it uploads); at Present the
// recording is played back once more with the camera patched for the right eye. The game's own
// code runs once; the GPU draws the scene twice.
//
// Recording window: from just after the previous Present to the first UI draw (the HUD is drawn
// once, for the left eye, and shown on the flat HUD layer). Frames without UI are recorded up to
// Present (then the replay also runs the game's final pass into the backbuffer).
//
// What playback needs besides the calls themselves:
//  * the pipeline state at the start of the window (StateBlock), restored before playback, and the
//    state at Present, restored after it, so the game never sees a difference;
//  * the data of every Map/UpdateSubresource, copied at record time before the mod patches the
//    camera into it: playback uploads it again and patches it for the right eye (the same code
//    finds and rewrites the camera buffers, so lights/reflection fixes follow the right eye too);
//  * the view-space fix compute passes (viewfix.cpp), run again before the same game dispatches;
//  * every object referenced, kept alive (AddRef) until playback is over.
// Anything the recorder doesn't understand (deferred command lists, a D3D11.1 call it can't
// replay, a buffer too large) cancels the second eye for that frame: it then shows the previous
// right image, as in alternate-eye mode, and the log says why.
#include "common.h"
#include <smmintrin.h>
#include <d3d11_1.h>

volatile bool g_replaying;      // playback in progress: every hook passes straight through
volatile LONG g_recOn;          // the recorder is taking calls
ID3D11DeviceContext* g_recCtx;  // the game's immediate context
volatile LONG g_recSuspend;     // mod-internal calls in progress (not recorded)

namespace {
enum Op : uint16_t {
    OP_SET_CB = 1, OP_SET_SRV, OP_SET_SAMP, OP_SET_SHADER, OP_CS_UAV, OP_IA_LAYOUT, OP_IA_VB, OP_IA_IB, OP_IA_TOPO,
    OP_DRAW, OP_DRAW_AUTO, OP_DRAW_INDIRECT, OP_DISPATCH, OP_DISPATCH_INDIRECT, OP_OM_RT, OP_OM_RT_UAV, OP_OM_BLEND,
    OP_OM_DS, OP_RS_STATE, OP_RS_VP, OP_RS_SCISSOR, OP_SO, OP_COPY_REGION, OP_COPY, OP_UPDATE, OP_COPY_COUNT,
    OP_CLEAR_RTV, OP_CLEAR_UAV_U, OP_CLEAR_UAV_F, OP_CLEAR_DSV, OP_GENMIPS, OP_MINLOD, OP_RESOLVE, OP_MAP,
    OP_PREDICATION, OP_CLEAR_STATE, OP_DISCARD_RES, OP_DISCARD_VIEW, OP_GEN, OP_COUNT
};
const char* const kOpNames[OP_COUNT] = {"?", "SetConstantBuffers", "SetShaderResources", "SetSamplers", "SetShader",
    "CSSetUnorderedAccessViews", "IASetInputLayout", "IASetVertexBuffers", "IASetIndexBuffer", "IASetPrimitiveTopology",
    "Draw*", "DrawAuto", "Draw*Indirect", "Dispatch", "DispatchIndirect", "OMSetRenderTargets",
    "OMSetRenderTargetsAndUAVs", "OMSetBlendState", "OMSetDepthStencilState", "RSSetState", "RSSetViewports",
    "RSSetScissorRects", "SOSetTargets", "CopySubresourceRegion", "CopyResource", "UpdateSubresource",
    "CopyStructureCount", "ClearRenderTargetView", "ClearUAVUint", "ClearUAVFloat", "ClearDepthStencilView",
    "GenerateMips", "SetResourceMinLOD", "ResolveSubresource", "Map", "SetPredication", "ClearState",
    "DiscardResource", "DiscardView", "dynamic buffer refill"};
enum Stage { S_VS, S_HS, S_DS, S_GS, S_PS, S_CS, S_COUNT };

struct CmdHeader {
    uint16_t op;
    uint16_t a;     // small argument (stage, kind, ...)
    uint32_t size;  // payload bytes after this header
};

const size_t kArenaCap = 192u << 20;  // per frame: commands + uploaded data
const size_t kMaxRefs = 1 << 18;
const UINT kMaxMapCopy = 16u << 20;   // a single Map/Update larger than this cancels the frame

uint8_t* g_arena;
size_t g_len;
IUnknown** g_refs;
size_t g_refCount;
SRWLOCK g_lock = SRWLOCK_INIT;
bool g_ready;            // hooks installed, memory allocated
bool g_frameValid;       // the recording can be played back
const char* g_cancelWhy; // why not (logged)
bool g_stoppedAtUi;      // recording ended at the first UI draw
unsigned g_counts[OP_COUNT];
unsigned g_frameCmds;
double g_copyMs;         // time spent copying upload data while recording (this frame)

// ---- statistics, logged every 30 s -----------------------------------------------------------
unsigned g_statFrames, g_statReplayed, g_statCancelled, g_statNoCamera;
double g_statReplayMs, g_statReplayMaxMs, g_statCopyMs, g_statCmds, g_statBytes, g_statSkipped;
DWORD g_statTick;
char g_lastCancel[160];
bool g_loggedCounts;

LARGE_INTEGER g_qpf;
double NowMs() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    if (!g_qpf.QuadPart) QueryPerformanceFrequency(&g_qpf);
    return (double)t.QuadPart * 1000.0 / (double)g_qpf.QuadPart;
}

void Cancel(const char* why) {
    if (g_frameValid) {
        g_frameValid = false;
        g_cancelWhy = why;
    }
    InterlockedExchange(&g_recOn, 0);
}

// ---- arena -----------------------------------------------------------------------------------
uint8_t* Begin(Op op, uint16_t a, size_t payload) {
    size_t need = sizeof(CmdHeader) + ((payload + 7) & ~(size_t)7);
    if (g_len + need > kArenaCap) {
        Cancel("recording memory full");
        return nullptr;
    }
    CmdHeader* h = (CmdHeader*)(g_arena + g_len);
    h->op = op;
    h->a = a;
    h->size = (uint32_t)((payload + 7) & ~(size_t)7);
    uint8_t* p = g_arena + g_len + sizeof(CmdHeader);
    g_len += need;
    if (op < OP_COUNT) g_counts[op]++;
    g_frameCmds++;
    return p;
}
void Ref(IUnknown* u) {
    if (!u) return;
    if (g_refCount >= kMaxRefs) {
        Cancel("too many objects referenced");
        return;
    }
    u->AddRef();
    g_refs[g_refCount++] = u;
}
template <typename T>
void RefAll(T* const* p, UINT n) {
    if (p)
        for (UINT i = 0; i < n; i++) Ref((IUnknown*)p[i]);
}
void ReleaseRefs() {
    for (size_t i = 0; i < g_refCount; i++) g_refs[i]->Release();
    g_refCount = 0;
}

// Takes the lock and checks the recorder is still on (a call may race with ReplayStop).
struct Rec {
    bool on;
    Rec() {
        AcquireSRWLockExclusive(&g_lock);
        on = g_recOn && g_frameValid;
    }
    ~Rec() { ReleaseSRWLockExclusive(&g_lock); }
};

// ---- formats: bytes of a row of `w` texels and number of rows for `h` texel rows ---------------
bool FormatInfo(DXGI_FORMAT f, UINT* bytesPerBlock, UINT* blockDim) {
    *blockDim = 1;
    switch (f) {
        case DXGI_FORMAT_R32G32B32A32_TYPELESS: case DXGI_FORMAT_R32G32B32A32_FLOAT: case DXGI_FORMAT_R32G32B32A32_UINT:
        case DXGI_FORMAT_R32G32B32A32_SINT:
            *bytesPerBlock = 16; return true;
        case DXGI_FORMAT_R32G32B32_TYPELESS: case DXGI_FORMAT_R32G32B32_FLOAT: case DXGI_FORMAT_R32G32B32_UINT:
        case DXGI_FORMAT_R32G32B32_SINT:
            *bytesPerBlock = 12; return true;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM:
        case DXGI_FORMAT_R16G16B16A16_UINT: case DXGI_FORMAT_R16G16B16A16_SNORM: case DXGI_FORMAT_R16G16B16A16_SINT:
        case DXGI_FORMAT_R32G32_TYPELESS: case DXGI_FORMAT_R32G32_FLOAT: case DXGI_FORMAT_R32G32_UINT:
        case DXGI_FORMAT_R32G32_SINT: case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
            *bytesPerBlock = 8; return true;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_UINT:
        case DXGI_FORMAT_R11G11B10_FLOAT: case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_UINT: case DXGI_FORMAT_R8G8B8A8_SNORM:
        case DXGI_FORMAT_R8G8B8A8_SINT: case DXGI_FORMAT_R16G16_TYPELESS: case DXGI_FORMAT_R16G16_FLOAT:
        case DXGI_FORMAT_R16G16_UNORM: case DXGI_FORMAT_R16G16_UINT: case DXGI_FORMAT_R16G16_SNORM:
        case DXGI_FORMAT_R16G16_SINT: case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R32_UINT: case DXGI_FORMAT_R32_SINT:
        case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT: case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8X8_TYPELESS: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: case DXGI_FORMAT_R9G9B9E5_SHAREDEXP:
            *bytesPerBlock = 4; return true;
        case DXGI_FORMAT_R8G8_TYPELESS: case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R8G8_UINT:
        case DXGI_FORMAT_R8G8_SNORM: case DXGI_FORMAT_R8G8_SINT: case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_D16_UNORM: case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_R16_UINT:
        case DXGI_FORMAT_R16_SNORM: case DXGI_FORMAT_R16_SINT: case DXGI_FORMAT_B5G6R5_UNORM:
        case DXGI_FORMAT_B5G5R5A1_UNORM: case DXGI_FORMAT_B4G4R4A4_UNORM:
            *bytesPerBlock = 2; return true;
        case DXGI_FORMAT_R8_TYPELESS: case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_UINT: case DXGI_FORMAT_R8_SNORM:
        case DXGI_FORMAT_R8_SINT: case DXGI_FORMAT_A8_UNORM:
            *bytesPerBlock = 1; return true;
        case DXGI_FORMAT_BC1_TYPELESS: case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC4_TYPELESS: case DXGI_FORMAT_BC4_UNORM: case DXGI_FORMAT_BC4_SNORM:
            *bytesPerBlock = 8; *blockDim = 4; return true;
        case DXGI_FORMAT_BC2_TYPELESS: case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_TYPELESS: case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC5_TYPELESS: case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC5_SNORM:
        case DXGI_FORMAT_BC6H_TYPELESS: case DXGI_FORMAT_BC6H_UF16: case DXGI_FORMAT_BC6H_SF16:
        case DXGI_FORMAT_BC7_TYPELESS: case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_UNORM_SRGB:
            *bytesPerBlock = 16; *blockDim = 4; return true;
        default: return false;
    }
}

// Extent of a subresource (or a box in it): w x h x d texels, its format, and whether it's a buffer.
struct Extent {
    bool buffer;
    UINT w, h, d;
    DXGI_FORMAT fmt;
    UINT byteWidth, bind, usage;
};
bool GetExtent(ID3D11Resource* r, UINT sub, Extent* e) {
    *e = {};
    D3D11_RESOURCE_DIMENSION dim;
    r->GetType(&dim);
    switch (dim) {
        case D3D11_RESOURCE_DIMENSION_BUFFER: {
            D3D11_BUFFER_DESC d;
            ((ID3D11Buffer*)r)->GetDesc(&d);
            e->buffer = true;
            e->w = e->byteWidth = d.ByteWidth;
            e->h = e->d = 1;
            e->bind = d.BindFlags;
            e->usage = d.Usage;
            return true;
        }
        case D3D11_RESOURCE_DIMENSION_TEXTURE1D: {
            D3D11_TEXTURE1D_DESC d;
            ((ID3D11Texture1D*)r)->GetDesc(&d);
            UINT mip = sub % (d.MipLevels ? d.MipLevels : 1);
            e->w = d.Width >> mip ? d.Width >> mip : 1;
            e->h = e->d = 1;
            e->fmt = d.Format;
            e->usage = d.Usage;
            return true;
        }
        case D3D11_RESOURCE_DIMENSION_TEXTURE2D: {
            D3D11_TEXTURE2D_DESC d;
            ((ID3D11Texture2D*)r)->GetDesc(&d);
            UINT mip = sub % (d.MipLevels ? d.MipLevels : 1);
            e->w = d.Width >> mip ? d.Width >> mip : 1;
            e->h = d.Height >> mip ? d.Height >> mip : 1;
            e->d = 1;
            e->fmt = d.Format;
            e->usage = d.Usage;
            return true;
        }
        case D3D11_RESOURCE_DIMENSION_TEXTURE3D: {
            D3D11_TEXTURE3D_DESC d;
            ((ID3D11Texture3D*)r)->GetDesc(&d);
            UINT mip = sub % (d.MipLevels ? d.MipLevels : 1);
            e->w = d.Width >> mip ? d.Width >> mip : 1;
            e->h = d.Height >> mip ? d.Height >> mip : 1;
            e->d = d.Depth >> mip ? d.Depth >> mip : 1;
            e->fmt = d.Format;
            e->usage = d.Usage;
            return true;
        }
        default: return false;
    }
}
// Bytes of source data an upload of w x h x d texels reads, with the given pitches.
bool UploadBytes(const Extent& e, UINT w, UINT h, UINT d, UINT rowPitch, UINT depthPitch, size_t* out) {
    if (e.buffer) {
        *out = w;
        return true;
    }
    UINT bpb, bd;
    if (!FormatInfo(e.fmt, &bpb, &bd)) return false;
    UINT bw = (w + bd - 1) / bd, bh = (h + bd - 1) / bd;
    size_t row = (size_t)bw * bpb;
    size_t slice = bh ? (size_t)rowPitch * (bh - 1) + row : 0;
    *out = d ? (size_t)depthPitch * (d - 1) + slice : 0;
    return true;
}

// Fast copy out of mapped (possibly write-combined) memory.
__attribute__((target("sse4.1"))) void CopyFromMapped(void* dst, const void* src, size_t n) {
    const uint8_t* s = (const uint8_t*)src;
    uint8_t* d = (uint8_t*)dst;
    if ((((uintptr_t)s) & 15) == 0) {
        size_t blocks = n / 64;
        for (size_t i = 0; i < blocks; i++) {
            __m128i a = _mm_stream_load_si128((__m128i*)(s + 0));
            __m128i b = _mm_stream_load_si128((__m128i*)(s + 16));
            __m128i c = _mm_stream_load_si128((__m128i*)(s + 32));
            __m128i dd = _mm_stream_load_si128((__m128i*)(s + 48));
            _mm_storeu_si128((__m128i*)(d + 0), a);
            _mm_storeu_si128((__m128i*)(d + 16), b);
            _mm_storeu_si128((__m128i*)(d + 32), c);
            _mm_storeu_si128((__m128i*)(d + 48), dd);
            s += 64;
            d += 64;
        }
        n -= blocks * 64;
    }
    memcpy(d, s, n);
}

// ---- open Maps (between Map and Unmap) --------------------------------------------------------
struct OpenMap {
    ID3D11Resource* res;
    UINT sub;
    D3D11_MAP type;
    UINT flags;
    D3D11_MAPPED_SUBRESOURCE m;
};
OpenMap g_open[64];
}  // namespace

// ---------------------------------------------------------------------------------------------
// Recorder entry points (called from the hooks before the original call, lock taken inside)
// ---------------------------------------------------------------------------------------------
struct RecSlots {  // payload of Set{CB,SRV,Samp} and similar: start, count, then pointers
    uint32_t start, n;
};

void RecSetSlots(int op, int stage, UINT start, UINT n, void* const* objs) {
    Rec r;
    if (!r.on) return;
    if (n > 128) n = 128;
    uint8_t* p = Begin((Op)op, (uint16_t)stage, sizeof(RecSlots) + n * 8);
    if (!p) return;
    RecSlots s = {start, n};
    memcpy(p, &s, sizeof(s));
    for (UINT i = 0; i < n; i++) {
        void* o = objs ? objs[i] : nullptr;
        memcpy(p + sizeof(s) + i * 8, &o, 8);
        Ref((IUnknown*)o);
    }
}
void RecSetCB(int stage, UINT start, UINT n, ID3D11Buffer* const* b) { RecSetSlots(OP_SET_CB, stage, start, n, (void* const*)b); }
void RecSetSRV(int stage, UINT start, UINT n, ID3D11ShaderResourceView* const* v) {
    RecSetSlots(OP_SET_SRV, stage, start, n, (void* const*)v);
}
void RecSetSamplers(int stage, UINT start, UINT n, ID3D11SamplerState* const* s) {
    RecSetSlots(OP_SET_SAMP, stage, start, n, (void* const*)s);
}
void RecSetShader(int stage, void* sh, ID3D11ClassInstance* const* ci, UINT n) {
    Rec r;
    if (!r.on) return;
    if (n > 16) n = 16;
    uint8_t* p = Begin(OP_SET_SHADER, (uint16_t)stage, 16 + n * 8);
    if (!p) return;
    uint64_t hdr[2] = {(uint64_t)(uintptr_t)sh, n};
    memcpy(p, hdr, 16);
    Ref((IUnknown*)sh);
    for (UINT i = 0; i < n; i++) {
        void* o = ci ? ci[i] : nullptr;
        memcpy(p + 16 + i * 8, &o, 8);
        Ref((IUnknown*)o);
    }
}
void RecCSSetUAV(UINT start, UINT n, ID3D11UnorderedAccessView* const* v, const UINT* counts) {
    Rec r;
    if (!r.on) return;
    if (n > 64) n = 64;
    uint8_t* p = Begin(OP_CS_UAV, counts ? 1 : 0, sizeof(RecSlots) + n * 12);
    if (!p) return;
    RecSlots s = {start, n};
    memcpy(p, &s, sizeof(s));
    for (UINT i = 0; i < n; i++) {
        void* o = v ? v[i] : nullptr;
        memcpy(p + sizeof(s) + i * 8, &o, 8);
        Ref((IUnknown*)o);
        UINT c = counts ? counts[i] : (UINT)-1;
        memcpy(p + sizeof(s) + n * 8 + i * 4, &c, 4);
    }
}
void RecDraw(int kind, UINT a, UINT b, INT c, UINT d, UINT e) {
    Rec r;
    if (!r.on) return;
    uint8_t* p = Begin(OP_DRAW, (uint16_t)kind, 20);
    if (!p) return;
    uint32_t v[5] = {a, b, (uint32_t)c, d, e};
    memcpy(p, v, 20);
}
void RecDispatch(UINT x, UINT y, UINT z, int viewFixKind) {
    Rec r;
    if (!r.on) return;
    uint8_t* p = Begin(OP_DISPATCH, (uint16_t)viewFixKind, 12);
    if (!p) return;
    uint32_t v[3] = {x, y, z};
    memcpy(p, v, 12);
}
void RecOMSetRT(UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) {
    Rec r;
    if (!r.on) return;
    if (n > 8) n = 8;
    uint8_t* p = Begin(OP_OM_RT, (uint16_t)n, 8 + n * 8);
    if (!p) return;
    memcpy(p, &dsv, 8);
    Ref(dsv);
    for (UINT i = 0; i < n; i++) {
        void* o = rtvs ? rtvs[i] : nullptr;
        memcpy(p + 8 + i * 8, &o, 8);
        Ref((IUnknown*)o);
    }
}
void RecViewports(UINT n, const D3D11_VIEWPORT* vp) {
    Rec r;
    if (!r.on) return;
    if (n > 16) n = 16;
    uint8_t* p = Begin(OP_RS_VP, (uint16_t)n, n * sizeof(D3D11_VIEWPORT));
    if (p && vp) memcpy(p, vp, n * sizeof(D3D11_VIEWPORT));
}
void RecUnsupported(const char* what) {
    Rec r;
    if (!r.on) return;
    Cancel(what);
}

// ---- dynamic vertex/index/structured buffers ------------------------------------------------
// Copying their contents on the CPU at every Map could be huge (a particle buffer written in many
// small NO_OVERWRITE pieces). Instead each "generation" of such a buffer (from a DISCARD, or its
// first Map in the frame, to the next DISCARD or the end of the recording) is copied once on the
// GPU into a snapshot buffer when it ends: NO_OVERWRITE never changes data the GPU still uses, so
// the final contents serve every draw of that generation. Playback binds the snapshot instead.
namespace {
struct Gen {
    ID3D11Buffer* buf;
    ID3D11Buffer* snap;
    bool open;
};
Gen g_gens[1024];
int g_genCount;
struct Pooled {
    UINT byteWidth, bind, misc, stride;
    ID3D11Buffer* b;
    bool used;
};
Pooled g_pool[1024];
int g_poolCount;
size_t g_poolBytes;
const size_t kPoolCap = 768u << 20;

bool IsDynBuffer(ID3D11Resource* r, D3D11_BUFFER_DESC* d) {
    D3D11_RESOURCE_DIMENSION dim;
    r->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) return false;
    ((ID3D11Buffer*)r)->GetDesc(d);
    return d->Usage == D3D11_USAGE_DYNAMIC && !(d->BindFlags & D3D11_BIND_CONSTANT_BUFFER);
}
ID3D11Buffer* PoolGet(const D3D11_BUFFER_DESC& d) {
    for (int i = 0; i < g_poolCount; i++) {
        Pooled& p = g_pool[i];
        if (!p.used && p.byteWidth == d.ByteWidth && p.bind == d.BindFlags && p.misc == d.MiscFlags &&
            p.stride == d.StructureByteStride) {
            p.used = true;
            return p.b;
        }
    }
    if (g_poolCount >= 1024 || g_poolBytes + d.ByteWidth > kPoolCap) return nullptr;
    D3D11_BUFFER_DESC nd = d;
    nd.Usage = D3D11_USAGE_DEFAULT;
    nd.CPUAccessFlags = 0;
    ID3D11Device* dev = nullptr;
    g_recCtx->GetDevice(&dev);
    ID3D11Buffer* b = nullptr;
    HRESULT hr = dev ? dev->CreateBuffer(&nd, nullptr, &b) : E_FAIL;
    if (dev) dev->Release();
    if (FAILED(hr) || !b) return nullptr;
    g_pool[g_poolCount++] = {d.ByteWidth, d.BindFlags, d.MiscFlags, d.StructureByteStride, b, true};
    g_poolBytes += d.ByteWidth;
    return b;
}
// GPU copy of the generation's final contents (recorder off or suspended while issuing it).
void CloseGen(Gen& g) {
    if (!g.open) return;
    g.open = false;
    InterlockedIncrement(&g_recSuspend);
    g_recCtx->CopyResource(g.snap, g.buf);
    InterlockedDecrement(&g_recSuspend);
}
Gen* OpenGenOf(ID3D11Resource* r) {
    for (int i = g_genCount - 1; i >= 0; i--)
        if (g_gens[i].buf == (ID3D11Buffer*)r) return g_gens[i].open ? &g_gens[i] : nullptr;
    return nullptr;
}
}  // namespace

// Before the game's Map: a DISCARD ends the buffer's current generation.
void RecBeforeMap(ID3D11Resource* res, D3D11_MAP type) {
    if (type != D3D11_MAP_WRITE_DISCARD) return;
    Rec r;
    if (!r.on) return;
    D3D11_BUFFER_DESC d;
    if (!IsDynBuffer(res, &d)) return;
    if (Gen* g = OpenGenOf(res)) CloseGen(*g);
}

// Map: remember where the game writes; Unmap copies what it wrote (before any patching).
void RecMap(ID3D11Resource* res, UINT sub, D3D11_MAP type, UINT flags, const D3D11_MAPPED_SUBRESOURCE* m) {
    if (type == D3D11_MAP_READ || !m || !m->pData) return;
    Rec r;
    if (!r.on) return;
    D3D11_BUFFER_DESC d;
    if (IsDynBuffer(res, &d)) {
        if (OpenGenOf(res)) return;  // NO_OVERWRITE into the current generation
        if (g_genCount >= 1024) {
            Cancel("too many dynamic buffer refills");
            return;
        }
        ID3D11Buffer* snap = PoolGet(d);
        if (!snap) {
            Cancel("out of snapshot buffers for dynamic buffers");
            return;
        }
        int idx = g_genCount++;
        g_gens[idx] = {(ID3D11Buffer*)res, snap, true};
        uint8_t* p = Begin(OP_GEN, 0, 16);
        if (!p) return;
        uint64_t v[2] = {(uint64_t)(uintptr_t)res, (uint64_t)idx};
        memcpy(p, v, 16);
        Ref(res);
        return;
    }
    for (auto& o : g_open)
        if (!o.res) {
            o = {res, sub, type, flags, *m};
            return;
        }
    Cancel("too many buffers mapped at once");
}
void RecUnmap(ID3D11Resource* res, UINT sub) {
    Rec r;
    if (!r.on) return;
    OpenMap om = {};
    for (auto& o : g_open)
        if (o.res == res && o.sub == sub) {
            om = o;
            o.res = nullptr;
            break;
        }
    if (!om.res) return;  // a READ map, or mapped before the recording started
    Extent e;
    size_t bytes = 0;
    if (!GetExtent(res, sub, &e) || !UploadBytes(e, e.w, e.h, e.d, om.m.RowPitch, om.m.DepthPitch, &bytes)) {
        Cancel("a mapped texture has a format the recorder doesn't know");
        return;
    }
    if (bytes > kMaxMapCopy) {
        Cancel("a mapped resource is too large to record");
        return;
    }
    struct Hdr {
        void* res;
        uint32_t sub, type, flags, rowPitch, depthPitch, bytes, isCB, pad;
    } h = {res, sub, (uint32_t)om.type, om.flags, om.m.RowPitch, om.m.DepthPitch, (uint32_t)bytes,
           (uint32_t)(e.buffer && (e.bind & D3D11_BIND_CONSTANT_BUFFER) ? 1 : 0), 0};
    uint8_t* p = Begin(OP_MAP, 0, sizeof(h) + bytes);
    if (!p) return;
    memcpy(p, &h, sizeof(h));
    double t0 = NowMs();
    CopyFromMapped(p + sizeof(h), om.m.pData, bytes);
    g_copyMs += NowMs() - t0;
    Ref(res);
}
void RecUpdate(ID3D11Resource* res, UINT sub, const D3D11_BOX* box, const void* data, UINT rp, UINT dp) {
    Rec r;
    if (!r.on || !res || !data) return;
    Extent e;
    if (!GetExtent(res, sub, &e)) {
        Cancel("UpdateSubresource on an unknown resource type");
        return;
    }
    UINT w = box ? box->right - box->left : e.w, h = box ? box->bottom - box->top : e.h,
         d = box ? box->back - box->front : e.d;
    if (box && (box->right <= box->left || box->bottom <= box->top || box->back <= box->front)) return;  // no-op
    size_t bytes = 0;
    if (!UploadBytes(e, w, h, d, rp, dp, &bytes)) {
        Cancel("UpdateSubresource on a texture format the recorder doesn't know");
        return;
    }
    if (bytes > kMaxMapCopy) {
        Cancel("an UpdateSubresource too large to record");
        return;
    }
    struct Hdr {
        void* res;
        uint32_t sub, hasBox;
        D3D11_BOX box;
        uint32_t rp, dp, bytes, isCB;
    } hd = {res, sub, box ? 1u : 0u, box ? *box : D3D11_BOX{}, rp, dp, (uint32_t)bytes,
           (uint32_t)(e.buffer && (e.bind & D3D11_BIND_CONSTANT_BUFFER) && !box ? 1 : 0)};
    uint8_t* p = Begin(OP_UPDATE, 0, sizeof(hd) + bytes);
    if (!p) return;
    memcpy(p, &hd, sizeof(hd));
    memcpy(p + sizeof(hd), data, bytes);
    Ref(res);
}

// ---------------------------------------------------------------------------------------------
// Hooks for the context methods probe.cpp doesn't hook already
// ---------------------------------------------------------------------------------------------
namespace {
#define CTX_ARGS ID3D11DeviceContext* c
inline bool On(ID3D11DeviceContext* c) { return RecOn(c); }

typedef void(STDMETHODCALLTYPE* PFN_SetSlots)(CTX_ARGS, UINT, UINT, void* const*);
typedef void(STDMETHODCALLTYPE* PFN_SetShaderX)(CTX_ARGS, void*, ID3D11ClassInstance* const*, UINT);
// slot setters: op, stage, vtable index
struct SlotHook {
    int op, stage, index;
    const char* name;
    PFN_SetSlots orig;
};
SlotHook g_slotHooks[] = {
    {OP_SET_SAMP, S_PS, 10, "PSSetSamplers", nullptr},       {OP_SET_SRV, S_VS, 25, "VSSetShaderResources", nullptr},
    {OP_SET_SAMP, S_VS, 26, "VSSetSamplers", nullptr},       {OP_SET_SRV, S_GS, 31, "GSSetShaderResources", nullptr},
    {OP_SET_SAMP, S_GS, 32, "GSSetSamplers", nullptr},       {OP_SET_SRV, S_HS, 59, "HSSetShaderResources", nullptr},
    {OP_SET_SAMP, S_HS, 61, "HSSetSamplers", nullptr},       {OP_SET_CB, S_HS, 62, "HSSetConstantBuffers", nullptr},
    {OP_SET_SRV, S_DS, 63, "DSSetShaderResources", nullptr}, {OP_SET_SAMP, S_DS, 65, "DSSetSamplers", nullptr},
    {OP_SET_CB, S_DS, 66, "DSSetConstantBuffers", nullptr},  {OP_SET_SAMP, S_CS, 70, "CSSetSamplers", nullptr},
};
static_assert(sizeof(g_slotHooks) / sizeof(g_slotHooks[0]) == 12, "H_Slots<0..11> below");
template <int I>
void STDMETHODCALLTYPE H_Slots(CTX_ARGS, UINT s, UINT n, void* const* v) {
    if (On(c)) RecSetSlots(g_slotHooks[I].op, g_slotHooks[I].stage, s, n, v);
    g_slotHooks[I].orig(c, s, n, v);
}
struct ShaderHook {
    int stage, index;
    const char* name;
    PFN_SetShaderX orig;
};
ShaderHook g_shaderHooks[] = {{S_GS, 23, "GSSetShader", nullptr}, {S_HS, 60, "HSSetShader", nullptr},
                              {S_DS, 64, "DSSetShader", nullptr}};
template <int I>
void STDMETHODCALLTYPE H_Shader(CTX_ARGS, void* sh, ID3D11ClassInstance* const* ci, UINT n) {
    if (On(c)) RecSetShader(g_shaderHooks[I].stage, sh, ci, n);
    g_shaderHooks[I].orig(c, sh, ci, n);
}

// The rest, one by one.
typedef void(STDMETHODCALLTYPE* PFN_IASetInputLayout)(CTX_ARGS, ID3D11InputLayout*);
typedef void(STDMETHODCALLTYPE* PFN_IASetVertexBuffers)(CTX_ARGS, UINT, UINT, ID3D11Buffer* const*, const UINT*, const UINT*);
typedef void(STDMETHODCALLTYPE* PFN_IASetIndexBuffer)(CTX_ARGS, ID3D11Buffer*, DXGI_FORMAT, UINT);
typedef void(STDMETHODCALLTYPE* PFN_IASetPrimitiveTopology)(CTX_ARGS, D3D11_PRIMITIVE_TOPOLOGY);
typedef void(STDMETHODCALLTYPE* PFN_SetPredication)(CTX_ARGS, ID3D11Predicate*, BOOL);
typedef void(STDMETHODCALLTYPE* PFN_OMSetRTUAV)(CTX_ARGS, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*,
                                                UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*);
typedef void(STDMETHODCALLTYPE* PFN_OMSetBlendState)(CTX_ARGS, ID3D11BlendState*, const FLOAT[4], UINT);
typedef void(STDMETHODCALLTYPE* PFN_OMSetDepthStencilState)(CTX_ARGS, ID3D11DepthStencilState*, UINT);
typedef void(STDMETHODCALLTYPE* PFN_SOSetTargets)(CTX_ARGS, UINT, ID3D11Buffer* const*, const UINT*);
typedef void(STDMETHODCALLTYPE* PFN_DrawAuto)(CTX_ARGS);
typedef void(STDMETHODCALLTYPE* PFN_DrawIndirect)(CTX_ARGS, ID3D11Buffer*, UINT);
typedef void(STDMETHODCALLTYPE* PFN_RSSetState)(CTX_ARGS, ID3D11RasterizerState*);
typedef void(STDMETHODCALLTYPE* PFN_RSSetScissorRects)(CTX_ARGS, UINT, const D3D11_RECT*);
typedef void(STDMETHODCALLTYPE* PFN_CopySubresourceRegion)(CTX_ARGS, ID3D11Resource*, UINT, UINT, UINT, UINT,
                                                           ID3D11Resource*, UINT, const D3D11_BOX*);
typedef void(STDMETHODCALLTYPE* PFN_CopyResource)(CTX_ARGS, ID3D11Resource*, ID3D11Resource*);
typedef void(STDMETHODCALLTYPE* PFN_CopyStructureCount)(CTX_ARGS, ID3D11Buffer*, UINT, ID3D11UnorderedAccessView*);
typedef void(STDMETHODCALLTYPE* PFN_ClearRTV)(CTX_ARGS, ID3D11RenderTargetView*, const FLOAT[4]);
typedef void(STDMETHODCALLTYPE* PFN_ClearUAVUint)(CTX_ARGS, ID3D11UnorderedAccessView*, const UINT[4]);
typedef void(STDMETHODCALLTYPE* PFN_ClearUAVFloat)(CTX_ARGS, ID3D11UnorderedAccessView*, const FLOAT[4]);
typedef void(STDMETHODCALLTYPE* PFN_ClearDSV)(CTX_ARGS, ID3D11DepthStencilView*, UINT, FLOAT, UINT8);
typedef void(STDMETHODCALLTYPE* PFN_GenerateMips)(CTX_ARGS, ID3D11ShaderResourceView*);
typedef void(STDMETHODCALLTYPE* PFN_SetResourceMinLOD)(CTX_ARGS, ID3D11Resource*, FLOAT);
typedef void(STDMETHODCALLTYPE* PFN_Resolve)(CTX_ARGS, ID3D11Resource*, UINT, ID3D11Resource*, UINT, DXGI_FORMAT);
typedef void(STDMETHODCALLTYPE* PFN_ClearState)(CTX_ARGS);
typedef void(STDMETHODCALLTYPE* PFN_DiscardResource)(CTX_ARGS, ID3D11Resource*);
typedef void(STDMETHODCALLTYPE* PFN_DiscardView)(CTX_ARGS, ID3D11View*);
typedef void(STDMETHODCALLTYPE* PFN_Any)();

PFN_IASetInputLayout o_IASetInputLayout;
PFN_IASetVertexBuffers o_IASetVertexBuffers;
PFN_IASetIndexBuffer o_IASetIndexBuffer;
PFN_IASetPrimitiveTopology o_IASetPrimitiveTopology;
PFN_SetPredication o_SetPredication;
PFN_OMSetRTUAV o_OMSetRTUAV;
PFN_OMSetBlendState o_OMSetBlendState;
PFN_OMSetDepthStencilState o_OMSetDepthStencilState;
PFN_SOSetTargets o_SOSetTargets;
PFN_DrawAuto o_DrawAuto;
PFN_DrawIndirect o_DrawIndexedInstancedIndirect, o_DrawInstancedIndirect, o_DispatchIndirect;
PFN_RSSetState o_RSSetState;
PFN_RSSetScissorRects o_RSSetScissorRects;
PFN_CopySubresourceRegion o_CopySubresourceRegion;
PFN_CopyResource o_CopyResource;
PFN_CopyStructureCount o_CopyStructureCount;
PFN_ClearRTV o_ClearRTV;
PFN_ClearUAVUint o_ClearUAVUint;
PFN_ClearUAVFloat o_ClearUAVFloat;
PFN_ClearDSV o_ClearDSV;
PFN_GenerateMips o_GenerateMips;
PFN_SetResourceMinLOD o_SetResourceMinLOD;
PFN_Resolve o_Resolve;
PFN_ClearState o_ClearState;
PFN_DiscardResource o_DiscardResource;
PFN_DiscardView o_DiscardView;
PFN_Any o_Ctx1[8];

void STDMETHODCALLTYPE H_IASetInputLayout(CTX_ARGS, ID3D11InputLayout* l) {
    if (On(c)) {
        Rec r;
        uint8_t* p = r.on ? Begin(OP_IA_LAYOUT, 0, 8) : nullptr;
        if (p) {
            memcpy(p, &l, 8);
            Ref(l);
        }
    }
    o_IASetInputLayout(c, l);
}
void STDMETHODCALLTYPE H_IASetVertexBuffers(CTX_ARGS, UINT s, UINT n, ID3D11Buffer* const* b, const UINT* st, const UINT* of) {
    if (On(c)) {
        Rec r;
        UINT k = n > 32 ? 32 : n;
        uint8_t* p = r.on ? Begin(OP_IA_VB, 0, 8 + k * 16) : nullptr;
        if (p) {
            uint32_t h[2] = {s, k};
            memcpy(p, h, 8);
            for (UINT i = 0; i < k; i++) {
                void* o = b ? b[i] : nullptr;
                UINT sv = st ? st[i] : 0, ov = of ? of[i] : 0;
                memcpy(p + 8 + i * 8, &o, 8);
                memcpy(p + 8 + k * 8 + i * 4, &sv, 4);
                memcpy(p + 8 + k * 12 + i * 4, &ov, 4);
                Ref((IUnknown*)o);
            }
        }
    }
    o_IASetVertexBuffers(c, s, n, b, st, of);
}
void STDMETHODCALLTYPE H_IASetIndexBuffer(CTX_ARGS, ID3D11Buffer* b, DXGI_FORMAT f, UINT o) {
    if (On(c)) {
        Rec r;
        uint8_t* p = r.on ? Begin(OP_IA_IB, 0, 16) : nullptr;
        if (p) {
            uint32_t v[2] = {(uint32_t)f, o};
            memcpy(p, &b, 8);
            memcpy(p + 8, v, 8);
            Ref(b);
        }
    }
    o_IASetIndexBuffer(c, b, f, o);
}
void STDMETHODCALLTYPE H_IASetPrimitiveTopology(CTX_ARGS, D3D11_PRIMITIVE_TOPOLOGY t) {
    if (On(c)) {
        Rec r;
        if (r.on) Begin(OP_IA_TOPO, (uint16_t)t, 0);
    }
    o_IASetPrimitiveTopology(c, t);
}
void STDMETHODCALLTYPE H_SetPredication(CTX_ARGS, ID3D11Predicate* pr, BOOL v) {
    if (On(c)) {
        Rec r;
        uint8_t* p = r.on ? Begin(OP_PREDICATION, (uint16_t)(v ? 1 : 0), 8) : nullptr;
        if (p) {
            memcpy(p, &pr, 8);
            Ref(pr);
        }
    }
    o_SetPredication(c, pr, v);
}
void STDMETHODCALLTYPE H_OMSetRTUAV(CTX_ARGS, UINT nrt, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv,
                                    UINT us, UINT nu, ID3D11UnorderedAccessView* const* uavs, const UINT* counts) {
    if (On(c)) {
        Rec r;
        UINT kr = nrt == D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL ? 0 : (nrt > 8 ? 8 : nrt);
        UINT ku = nu == D3D11_KEEP_UNORDERED_ACCESS_VIEWS ? 0 : (nu > 8 ? 8 : nu);
        uint8_t* p = r.on ? Begin(OP_OM_RT_UAV, 0, 24 + kr * 8 + ku * 12) : nullptr;
        if (p) {
            uint32_t h[4] = {nrt, us, nu, counts ? 1u : 0u};
            memcpy(p, h, 16);
            memcpy(p + 16, &dsv, 8);
            if (nrt != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) Ref(dsv);
            for (UINT i = 0; i < kr; i++) {
                void* o = rtvs ? rtvs[i] : nullptr;
                memcpy(p + 24 + i * 8, &o, 8);
                Ref((IUnknown*)o);
            }
            for (UINT i = 0; i < ku; i++) {
                void* o = uavs ? uavs[i] : nullptr;
                UINT cnt = counts ? counts[i] : (UINT)-1;
                memcpy(p + 24 + kr * 8 + i * 8, &o, 8);
                memcpy(p + 24 + kr * 8 + ku * 8 + i * 4, &cnt, 4);
                Ref((IUnknown*)o);
            }
        }
    }
    if (c == g_recCtx && !g_replaying && nrt != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL)
        GpuProfTargets(c, nrt, rtvs, dsv, "");
    o_OMSetRTUAV(c, nrt, rtvs, dsv, us, nu, uavs, counts);
}
void STDMETHODCALLTYPE H_OMSetBlendState(CTX_ARGS, ID3D11BlendState* bs, const FLOAT f[4], UINT mask) {
    if (On(c)) {
        Rec r;
        uint8_t* p = r.on ? Begin(OP_OM_BLEND, (uint16_t)(f ? 1 : 0), 8 + 16 + 4) : nullptr;
        if (p) {
            memcpy(p, &bs, 8);
            if (f) memcpy(p + 8, f, 16);
            memcpy(p + 24, &mask, 4);
            Ref(bs);
        }
    }
    o_OMSetBlendState(c, bs, f, mask);
}
void STDMETHODCALLTYPE H_OMSetDepthStencilState(CTX_ARGS, ID3D11DepthStencilState* ds, UINT ref) {
    if (On(c)) {
        Rec r;
        uint8_t* p = r.on ? Begin(OP_OM_DS, 0, 12) : nullptr;
        if (p) {
            memcpy(p, &ds, 8);
            memcpy(p + 8, &ref, 4);
            Ref(ds);
        }
    }
    o_OMSetDepthStencilState(c, ds, ref);
}
void STDMETHODCALLTYPE H_SOSetTargets(CTX_ARGS, UINT n, ID3D11Buffer* const* b, const UINT* off) {
    if (On(c)) {
        Rec r;
        UINT k = n > 4 ? 4 : n;
        uint8_t* p = r.on ? Begin(OP_SO, (uint16_t)k, k * 12) : nullptr;
        if (p)
            for (UINT i = 0; i < k; i++) {
                void* o = b ? b[i] : nullptr;
                UINT ov = off ? off[i] : 0;
                memcpy(p + i * 8, &o, 8);
                memcpy(p + k * 8 + i * 4, &ov, 4);
                Ref((IUnknown*)o);
            }
    }
    o_SOSetTargets(c, n, b, off);
}
void STDMETHODCALLTYPE H_DrawAuto(CTX_ARGS) {
    if (On(c)) {
        Rec r;
        if (r.on) Begin(OP_DRAW_AUTO, 0, 0);
    }
    o_DrawAuto(c);
}
void RecIndirect(Op op, uint16_t kind, ID3D11Buffer* b, UINT off) {
    Rec r;
    uint8_t* p = r.on ? Begin(op, kind, 12) : nullptr;
    if (p) {
        memcpy(p, &b, 8);
        memcpy(p + 8, &off, 4);
        Ref(b);
    }
}
void STDMETHODCALLTYPE H_DrawIndexedInstancedIndirect(CTX_ARGS, ID3D11Buffer* b, UINT off) {
    if (On(c)) RecIndirect(OP_DRAW_INDIRECT, 1, b, off);
    o_DrawIndexedInstancedIndirect(c, b, off);
}
void STDMETHODCALLTYPE H_DrawInstancedIndirect(CTX_ARGS, ID3D11Buffer* b, UINT off) {
    if (On(c)) RecIndirect(OP_DRAW_INDIRECT, 0, b, off);
    o_DrawInstancedIndirect(c, b, off);
}
void STDMETHODCALLTYPE H_DispatchIndirect(CTX_ARGS, ID3D11Buffer* b, UINT off) {
    if (On(c)) RecIndirect(OP_DISPATCH_INDIRECT, 0, b, off);
    o_DispatchIndirect(c, b, off);
}
void STDMETHODCALLTYPE H_RSSetState(CTX_ARGS, ID3D11RasterizerState* s) {
    if (On(c)) {
        Rec r;
        uint8_t* p = r.on ? Begin(OP_RS_STATE, 0, 8) : nullptr;
        if (p) {
            memcpy(p, &s, 8);
            Ref(s);
        }
    }
    o_RSSetState(c, s);
}
void STDMETHODCALLTYPE H_RSSetScissorRects(CTX_ARGS, UINT n, const D3D11_RECT* rc) {
    if (On(c)) {
        Rec r;
        UINT k = n > 16 ? 16 : n;
        uint8_t* p = r.on ? Begin(OP_RS_SCISSOR, (uint16_t)k, k * sizeof(D3D11_RECT)) : nullptr;
        if (p && rc) memcpy(p, rc, k * sizeof(D3D11_RECT));
    }
    o_RSSetScissorRects(c, n, rc);
}
void STDMETHODCALLTYPE H_CopySubresourceRegion(CTX_ARGS, ID3D11Resource* dst, UINT ds, UINT x, UINT y, UINT z,
                                               ID3D11Resource* src, UINT ss, const D3D11_BOX* box) {
    if (On(c)) {
        Rec r;
        uint8_t* p = r.on ? Begin(OP_COPY_REGION, (uint16_t)(box ? 1 : 0), 16 + 20 + sizeof(D3D11_BOX)) : nullptr;
        if (p) {
            memcpy(p, &dst, 8);
            memcpy(p + 8, &src, 8);
            uint32_t v[5] = {ds, x, y, z, ss};
            memcpy(p + 16, v, 20);
            if (box) memcpy(p + 36, box, sizeof(*box));
            Ref(dst);
            Ref(src);
        }
    }
    o_CopySubresourceRegion(c, dst, ds, x, y, z, src, ss, box);
}
void STDMETHODCALLTYPE H_CopyResource(CTX_ARGS, ID3D11Resource* dst, ID3D11Resource* src) {
    if (On(c)) {
        Rec r;
        uint8_t* p = r.on ? Begin(OP_COPY, 0, 16) : nullptr;
        if (p) {
            memcpy(p, &dst, 8);
            memcpy(p + 8, &src, 8);
            Ref(dst);
            Ref(src);
        }
    }
    o_CopyResource(c, dst, src);
}
void STDMETHODCALLTYPE H_CopyStructureCount(CTX_ARGS, ID3D11Buffer* b, UINT off, ID3D11UnorderedAccessView* v) {
    if (On(c)) {
        Rec r;
        uint8_t* p = r.on ? Begin(OP_COPY_COUNT, 0, 20) : nullptr;
        if (p) {
            memcpy(p, &b, 8);
            memcpy(p + 8, &v, 8);
            memcpy(p + 16, &off, 4);
            Ref(b);
            Ref(v);
        }
    }
    o_CopyStructureCount(c, b, off, v);
}
void RecClear(Op op, IUnknown* view, const void* v16) {
    Rec r;
    uint8_t* p = r.on ? Begin(op, 0, 24) : nullptr;
    if (p) {
        memcpy(p, &view, 8);
        memcpy(p + 8, v16, 16);
        Ref(view);
    }
}
void STDMETHODCALLTYPE H_ClearRTV(CTX_ARGS, ID3D11RenderTargetView* v, const FLOAT col[4]) {
    if (On(c)) RecClear(OP_CLEAR_RTV, v, col);
    o_ClearRTV(c, v, col);
}
void STDMETHODCALLTYPE H_ClearUAVUint(CTX_ARGS, ID3D11UnorderedAccessView* v, const UINT val[4]) {
    if (On(c)) RecClear(OP_CLEAR_UAV_U, v, val);
    o_ClearUAVUint(c, v, val);
}
void STDMETHODCALLTYPE H_ClearUAVFloat(CTX_ARGS, ID3D11UnorderedAccessView* v, const FLOAT val[4]) {
    if (On(c)) RecClear(OP_CLEAR_UAV_F, v, val);
    o_ClearUAVFloat(c, v, val);
}
void STDMETHODCALLTYPE H_ClearDSV(CTX_ARGS, ID3D11DepthStencilView* v, UINT flags, FLOAT d, UINT8 s) {
    if (On(c)) {
        Rec r;
        uint8_t* p = r.on ? Begin(OP_CLEAR_DSV, 0, 20) : nullptr;
        if (p) {
            memcpy(p, &v, 8);
            uint32_t sv = s;
            memcpy(p + 8, &flags, 4);
            memcpy(p + 12, &d, 4);
            memcpy(p + 16, &sv, 4);
            Ref(v);
        }
    }
    o_ClearDSV(c, v, flags, d, s);
}
void STDMETHODCALLTYPE H_GenerateMips(CTX_ARGS, ID3D11ShaderResourceView* v) {
    if (On(c)) {
        Rec r;
        uint8_t* p = r.on ? Begin(OP_GENMIPS, 0, 8) : nullptr;
        if (p) {
            memcpy(p, &v, 8);
            Ref(v);
        }
    }
    o_GenerateMips(c, v);
}
void STDMETHODCALLTYPE H_SetResourceMinLOD(CTX_ARGS, ID3D11Resource* res, FLOAT lod) {
    if (On(c)) {
        Rec r;
        uint8_t* p = r.on ? Begin(OP_MINLOD, 0, 12) : nullptr;
        if (p) {
            memcpy(p, &res, 8);
            memcpy(p + 8, &lod, 4);
            Ref(res);
        }
    }
    o_SetResourceMinLOD(c, res, lod);
}
void STDMETHODCALLTYPE H_Resolve(CTX_ARGS, ID3D11Resource* dst, UINT ds, ID3D11Resource* src, UINT ss, DXGI_FORMAT f) {
    if (On(c)) {
        Rec r;
        uint8_t* p = r.on ? Begin(OP_RESOLVE, 0, 28) : nullptr;
        if (p) {
            memcpy(p, &dst, 8);
            memcpy(p + 8, &src, 8);
            uint32_t v[3] = {ds, ss, (uint32_t)f};
            memcpy(p + 16, v, 12);
            Ref(dst);
            Ref(src);
        }
    }
    o_Resolve(c, dst, ds, src, ss, f);
}
void STDMETHODCALLTYPE H_ClearState(CTX_ARGS) {
    if (On(c)) {
        Rec r;
        if (r.on) Begin(OP_CLEAR_STATE, 0, 0);
    }
    o_ClearState(c);
}
void STDMETHODCALLTYPE H_DiscardResource(CTX_ARGS, ID3D11Resource* res) {
    if (On(c)) {
        Rec r;
        uint8_t* p = r.on ? Begin(OP_DISCARD_RES, 0, 8) : nullptr;
        if (p) {
            memcpy(p, &res, 8);
            Ref(res);
        }
    }
    o_DiscardResource(c, res);
}
void STDMETHODCALLTYPE H_DiscardView(CTX_ARGS, ID3D11View* v) {
    if (On(c)) {
        Rec r;
        uint8_t* p = r.on ? Begin(OP_DISCARD_VIEW, 0, 8) : nullptr;
        if (p) {
            memcpy(p, &v, 8);
            Ref(v);
        }
    }
    o_DiscardView(c, v);
}
// D3D11.1 calls the recorder can't replay: seeing one in the window cancels the second eye.
// (Pass-through trampolines: all of these take only integer/pointer arguments, so forwarding ten
// pointer-sized arguments hands each original exactly what it was called with.)
template <int I>
void STDMETHODCALLTYPE H_Ctx1(CTX_ARGS, void* a, void* b, void* d, void* e, void* f, void* g, void* h, void* i,
                              void* j, void* k) {
    static const char* const names[8] = {"CopySubresourceRegion1", "UpdateSubresource1", "VSSetConstantBuffers1",
                                         "PSSetConstantBuffers1", "CSSetConstantBuffers1", "ClearView",
                                         "SwapDeviceContextState", "GSSetConstantBuffers1"};
    if (On(c)) RecUnsupported(names[I]);
    // (CopySubresourceRegion1 has 9 arguments after the context; the extra ones are ignored)
    ((void(STDMETHODCALLTYPE*)(CTX_ARGS, void*, void*, void*, void*, void*, void*, void*, void*, void*, void*))o_Ctx1[I])(
        c, a, b, d, e, f, g, h, i, j, k);
}
}  // namespace

// ---------------------------------------------------------------------------------------------
// Pipeline state snapshot
// ---------------------------------------------------------------------------------------------
namespace {
const UINT kSrvSlots = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;   // 128
const UINT kCbSlots = D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT;  // 14
const UINT kSampSlots = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT;          // 16
const UINT kUavSlots = D3D11_PS_CS_UAV_REGISTER_COUNT;                  // 8 (feature level 11.0)
struct StageState {
    ID3D11DeviceChild* sh;
    ID3D11ClassInstance* inst[16];
    UINT ninst;
    ID3D11Buffer* cb[kCbSlots];
    ID3D11ShaderResourceView* srv[kSrvSlots];
    ID3D11SamplerState* samp[kSampSlots];
};
struct StateBlock {
    bool valid;
    ID3D11InputLayout* il;
    ID3D11Buffer* vb[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
    UINT stride[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT], off[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
    ID3D11Buffer* ib;
    DXGI_FORMAT ibFmt;
    UINT ibOff;
    D3D11_PRIMITIVE_TOPOLOGY topo;
    StageState st[S_COUNT];
    ID3D11UnorderedAccessView* csUav[kUavSlots];
    ID3D11RasterizerState* rs;
    D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    UINT nvp;
    D3D11_RECT sc[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    UINT nsc;
    ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
    ID3D11DepthStencilView* dsv;
    ID3D11UnorderedAccessView* omUav[kUavSlots];
    ID3D11BlendState* bs;
    FLOAT bf[4];
    UINT mask;
    ID3D11DepthStencilState* dss;
    UINT sref;
    ID3D11Buffer* so[D3D11_SO_BUFFER_SLOT_COUNT];
    ID3D11Predicate* pred;
    BOOL predVal;
};
StateBlock g_startState, g_endState, g_finalState;

template <typename T>
void RelArr(T** a, UINT n) {
    for (UINT i = 0; i < n; i++)
        if (a[i]) {
            a[i]->Release();
            a[i] = nullptr;
        }
}
template <typename T>
void Rel1(T*& p) {
    if (p) p->Release();
    p = nullptr;
}
void ReleaseState(StateBlock& s) {
    if (!s.valid) return;
    Rel1(s.il);
    RelArr(s.vb, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT);
    Rel1(s.ib);
    for (auto& st : s.st) {
        Rel1(st.sh);
        RelArr(st.inst, 16);
        RelArr(st.cb, kCbSlots);
        RelArr(st.srv, kSrvSlots);
        RelArr(st.samp, kSampSlots);
    }
    RelArr(s.csUav, kUavSlots);
    Rel1(s.rs);
    RelArr(s.rtv, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT);
    Rel1(s.dsv);
    RelArr(s.omUav, kUavSlots);
    Rel1(s.bs);
    Rel1(s.dss);
    RelArr(s.so, D3D11_SO_BUFFER_SLOT_COUNT);
    Rel1(s.pred);
    s.valid = false;
}

void CaptureState(ID3D11DeviceContext* c, StateBlock& s) {
    ReleaseState(s);
    memset(&s, 0, sizeof(s));
    c->IAGetInputLayout(&s.il);
    c->IAGetVertexBuffers(0, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT, s.vb, s.stride, s.off);
    c->IAGetIndexBuffer(&s.ib, &s.ibFmt, &s.ibOff);
    c->IAGetPrimitiveTopology(&s.topo);
    StageState* t = s.st;
    t[S_VS].ninst = t[S_HS].ninst = t[S_DS].ninst = t[S_GS].ninst = t[S_PS].ninst = t[S_CS].ninst = 16;
    c->VSGetShader((ID3D11VertexShader**)&t[S_VS].sh, t[S_VS].inst, &t[S_VS].ninst);
    c->HSGetShader((ID3D11HullShader**)&t[S_HS].sh, t[S_HS].inst, &t[S_HS].ninst);
    c->DSGetShader((ID3D11DomainShader**)&t[S_DS].sh, t[S_DS].inst, &t[S_DS].ninst);
    c->GSGetShader((ID3D11GeometryShader**)&t[S_GS].sh, t[S_GS].inst, &t[S_GS].ninst);
    c->PSGetShader((ID3D11PixelShader**)&t[S_PS].sh, t[S_PS].inst, &t[S_PS].ninst);
    c->CSGetShader((ID3D11ComputeShader**)&t[S_CS].sh, t[S_CS].inst, &t[S_CS].ninst);
    c->VSGetConstantBuffers(0, kCbSlots, t[S_VS].cb);
    c->HSGetConstantBuffers(0, kCbSlots, t[S_HS].cb);
    c->DSGetConstantBuffers(0, kCbSlots, t[S_DS].cb);
    c->GSGetConstantBuffers(0, kCbSlots, t[S_GS].cb);
    c->PSGetConstantBuffers(0, kCbSlots, t[S_PS].cb);
    c->CSGetConstantBuffers(0, kCbSlots, t[S_CS].cb);
    c->VSGetShaderResources(0, kSrvSlots, t[S_VS].srv);
    c->HSGetShaderResources(0, kSrvSlots, t[S_HS].srv);
    c->DSGetShaderResources(0, kSrvSlots, t[S_DS].srv);
    c->GSGetShaderResources(0, kSrvSlots, t[S_GS].srv);
    c->PSGetShaderResources(0, kSrvSlots, t[S_PS].srv);
    c->CSGetShaderResources(0, kSrvSlots, t[S_CS].srv);
    c->VSGetSamplers(0, kSampSlots, t[S_VS].samp);
    c->HSGetSamplers(0, kSampSlots, t[S_HS].samp);
    c->DSGetSamplers(0, kSampSlots, t[S_DS].samp);
    c->GSGetSamplers(0, kSampSlots, t[S_GS].samp);
    c->PSGetSamplers(0, kSampSlots, t[S_PS].samp);
    c->CSGetSamplers(0, kSampSlots, t[S_CS].samp);
    c->CSGetUnorderedAccessViews(0, kUavSlots, s.csUav);
    c->RSGetState(&s.rs);
    s.nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    c->RSGetViewports(&s.nvp, s.vp);
    s.nsc = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    c->RSGetScissorRects(&s.nsc, s.sc);
    c->OMGetRenderTargetsAndUnorderedAccessViews(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, s.rtv, &s.dsv, 0, kUavSlots,
                                                 s.omUav);
    c->OMGetBlendState(&s.bs, s.bf, &s.mask);
    c->OMGetDepthStencilState(&s.dss, &s.sref);
    c->SOGetTargets(D3D11_SO_BUFFER_SLOT_COUNT, s.so);
    c->GetPredication(&s.pred, &s.predVal);
    s.valid = true;
}

void ApplyState(ID3D11DeviceContext* c, const StateBlock& s) {
    if (!s.valid) return;
    c->IASetInputLayout(s.il);
    c->IASetVertexBuffers(0, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT, s.vb, s.stride, s.off);
    c->IASetIndexBuffer(s.ib, s.ibFmt, s.ibOff);
    c->IASetPrimitiveTopology(s.topo);
    const StageState* t = s.st;
    c->VSSetShader((ID3D11VertexShader*)t[S_VS].sh, t[S_VS].inst, t[S_VS].ninst);
    c->HSSetShader((ID3D11HullShader*)t[S_HS].sh, t[S_HS].inst, t[S_HS].ninst);
    c->DSSetShader((ID3D11DomainShader*)t[S_DS].sh, t[S_DS].inst, t[S_DS].ninst);
    c->GSSetShader((ID3D11GeometryShader*)t[S_GS].sh, t[S_GS].inst, t[S_GS].ninst);
    c->PSSetShader((ID3D11PixelShader*)t[S_PS].sh, t[S_PS].inst, t[S_PS].ninst);
    c->CSSetShader((ID3D11ComputeShader*)t[S_CS].sh, t[S_CS].inst, t[S_CS].ninst);
    c->VSSetConstantBuffers(0, kCbSlots, t[S_VS].cb);
    c->HSSetConstantBuffers(0, kCbSlots, t[S_HS].cb);
    c->DSSetConstantBuffers(0, kCbSlots, t[S_DS].cb);
    c->GSSetConstantBuffers(0, kCbSlots, t[S_GS].cb);
    c->PSSetConstantBuffers(0, kCbSlots, t[S_PS].cb);
    c->CSSetConstantBuffers(0, kCbSlots, t[S_CS].cb);
    // Outputs first (a resource bound as output is unbound from inputs, and the reverse), then
    // the inputs, so each ends up exactly as captured.
    {
        UINT firstUav = kUavSlots;
        for (UINT i = 0; i < kUavSlots; i++)
            if (s.omUav[i]) {
                firstUav = i;
                break;
            }
        UINT keep[kUavSlots];
        for (auto& k : keep) k = (UINT)-1;
        if (firstUav < kUavSlots)
            c->OMSetRenderTargetsAndUnorderedAccessViews(firstUav, s.rtv, s.dsv, firstUav, kUavSlots - firstUav,
                                                         s.omUav + firstUav, keep);
        else
            c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, s.rtv, s.dsv);
        c->CSSetUnorderedAccessViews(0, kUavSlots, s.csUav, keep);
    }
    c->SOSetTargets(D3D11_SO_BUFFER_SLOT_COUNT, s.so, nullptr);
    c->VSSetShaderResources(0, kSrvSlots, t[S_VS].srv);
    c->HSSetShaderResources(0, kSrvSlots, t[S_HS].srv);
    c->DSSetShaderResources(0, kSrvSlots, t[S_DS].srv);
    c->GSSetShaderResources(0, kSrvSlots, t[S_GS].srv);
    c->PSSetShaderResources(0, kSrvSlots, t[S_PS].srv);
    c->CSSetShaderResources(0, kSrvSlots, t[S_CS].srv);
    c->VSSetSamplers(0, kSampSlots, t[S_VS].samp);
    c->HSSetSamplers(0, kSampSlots, t[S_HS].samp);
    c->DSSetSamplers(0, kSampSlots, t[S_DS].samp);
    c->GSSetSamplers(0, kSampSlots, t[S_GS].samp);
    c->PSSetSamplers(0, kSampSlots, t[S_PS].samp);
    c->CSSetSamplers(0, kSampSlots, t[S_CS].samp);
    c->RSSetState(s.rs);
    c->RSSetViewports(s.nvp, s.vp);
    c->RSSetScissorRects(s.nsc, s.sc);
    c->OMSetBlendState(s.bs, s.bf, s.mask);
    c->OMSetDepthStencilState(s.dss, s.sref);
    c->SetPredication(s.pred, s.predVal);
}

// ---------------------------------------------------------------------------------------------
// Playback
// ---------------------------------------------------------------------------------------------
template <typename T>
T Get(const uint8_t* p, size_t off) {
    T v;
    memcpy(&v, p + off, sizeof(T));
    return v;
}

// Playback bindings of dynamic buffers: the game's own buffer is replaced by the snapshot of the
// generation the recording was in at that point. The game's bindings are tracked so a new
// generation can rebind the slots that use the buffer.
struct Subst {
    ID3D11Buffer* buf;
    ID3D11Buffer* snap;
};
Subst g_subst[256];
int g_substCount;
struct ViewCache {
    ID3D11ShaderResourceView* game;
    ID3D11Buffer* snap;
    ID3D11ShaderResourceView* ours;
};
ViewCache g_views[256];
int g_viewCount;
struct Track {
    ID3D11Buffer* vb[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
    UINT stride[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT], off[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
    ID3D11Buffer* ib;
    DXGI_FORMAT ibFmt;
    UINT ibOff;
    ID3D11ShaderResourceView* srv[S_COUNT][kSrvSlots];
};
Track g_track;

ID3D11Buffer* SubBuf(ID3D11Buffer* b) {
    if (!b) return b;
    for (int i = 0; i < g_substCount; i++)
        if (g_subst[i].buf == b) return g_subst[i].snap;
    return b;
}
ID3D11ShaderResourceView* SubView(ID3D11ShaderResourceView* v) {
    if (!v || !g_substCount) return v;
    ID3D11Resource* r = nullptr;
    v->GetResource(&r);
    if (r) r->Release();  // the view keeps it alive
    ID3D11Buffer* snap = SubBuf((ID3D11Buffer*)r);
    if (snap == (ID3D11Buffer*)r) return v;
    for (int i = 0; i < g_viewCount; i++)
        if (g_views[i].game == v && g_views[i].snap == snap) return g_views[i].ours;
    if (g_viewCount >= 256) return v;
    D3D11_SHADER_RESOURCE_VIEW_DESC d;
    v->GetDesc(&d);
    ID3D11Device* dev = nullptr;
    g_recCtx->GetDevice(&dev);
    ID3D11ShaderResourceView* ours = nullptr;
    if (!dev || FAILED(dev->CreateShaderResourceView(snap, &d, &ours))) ours = nullptr;
    if (dev) dev->Release();
    if (!ours) return v;
    g_views[g_viewCount++] = {v, snap, ours};
    return ours;
}
void ReleaseViewCache() {
    for (int i = 0; i < g_viewCount; i++) g_views[i].ours->Release();
    g_viewCount = 0;
}
void SetSrvs(ID3D11DeviceContext* c, int stage, UINT st, UINT n, ID3D11ShaderResourceView* const* b) {
    switch (stage) {
        case S_VS: c->VSSetShaderResources(st, n, b); break;
        case S_HS: c->HSSetShaderResources(st, n, b); break;
        case S_DS: c->DSSetShaderResources(st, n, b); break;
        case S_GS: c->GSSetShaderResources(st, n, b); break;
        case S_PS: c->PSSetShaderResources(st, n, b); break;
        case S_CS: c->CSSetShaderResources(st, n, b); break;
    }
}
// A new generation of `buf`: rebind every slot that uses it.
void Rebind(ID3D11DeviceContext* c, ID3D11Buffer* buf) {
    for (UINT i = 0; i < D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT; i++)
        if (g_track.vb[i] == buf) {
            ID3D11Buffer* s = SubBuf(buf);
            c->IASetVertexBuffers(i, 1, &s, &g_track.stride[i], &g_track.off[i]);
        }
    if (g_track.ib == buf) c->IASetIndexBuffer(SubBuf(buf), g_track.ibFmt, g_track.ibOff);
    for (int s = 0; s < S_COUNT; s++)
        for (UINT i = 0; i < kSrvSlots; i++) {
            ID3D11ShaderResourceView* v = g_track.srv[s][i];
            if (!v) continue;
            ID3D11Resource* r = nullptr;
            v->GetResource(&r);
            if (r) r->Release();
            if ((ID3D11Buffer*)r != buf) continue;
            ID3D11ShaderResourceView* t = SubView(v);
            SetSrvs(c, s, i, 1, &t);
        }
}

// Reused shadow maps (ReuseShadows=1): depth-only passes into a square depth texture (the shadow
// map is drawn from the light, not from the eye, so the left eye's one is right for the right eye
// too). The right eye's replay skips their draws and clears; the texture still holds the left's.
struct DsvClass {
    ID3D11DepthStencilView* v;
    bool shadow;
};
DsvClass g_dsvCache[32];
int g_dsvCount;
unsigned g_skippedDraws;
bool g_loggedShadow;
bool IsShadowDsv(ID3D11DepthStencilView* v) {
    if (!v) return false;
    for (int i = 0; i < g_dsvCount; i++)
        if (g_dsvCache[i].v == v) return g_dsvCache[i].shadow;
    bool s = false;
    ID3D11Resource* r = nullptr;
    v->GetResource(&r);
    if (r) {
        D3D11_RESOURCE_DIMENSION dim;
        r->GetType(&dim);
        if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
            D3D11_TEXTURE2D_DESC d;
            ((ID3D11Texture2D*)r)->GetDesc(&d);
            s = d.Width == d.Height && d.Width >= 512;
            if (s && !g_loggedShadow) {
                g_loggedShadow = true;
                Log("both eyes: the right eye reuses the shadow map (%ux%u, format %d) drawn for the left eye", d.Width,
                    d.Height, (int)d.Format);
            }
        }
        r->Release();
    }
    if (g_dsvCount < 32) g_dsvCache[g_dsvCount++] = {v, s};
    return s;
}
bool AllNull(ID3D11RenderTargetView* const* r, UINT n) {
    for (UINT i = 0; i < n; i++)
        if (r && r[i]) return false;
    return true;
}

void Play(ID3D11DeviceContext* c) {
    float scratch[64];
    bool skipping = false;  // inside a reused shadow pass
    g_dsvCount = 0;
    g_skippedDraws = 0;
    g_substCount = 0;
    memset(&g_track, 0, sizeof(g_track));
    memcpy(g_track.vb, g_startState.vb, sizeof(g_track.vb));
    memcpy(g_track.stride, g_startState.stride, sizeof(g_track.stride));
    memcpy(g_track.off, g_startState.off, sizeof(g_track.off));
    g_track.ib = g_startState.ib;
    g_track.ibFmt = g_startState.ibFmt;
    g_track.ibOff = g_startState.ibOff;
    for (int s = 0; s < S_COUNT; s++) memcpy(g_track.srv[s], g_startState.st[s].srv, sizeof(g_track.srv[s]));
    for (size_t off = 0; off < g_len;) {
        CmdHeader h;
        memcpy(&h, g_arena + off, sizeof(h));
        const uint8_t* p = g_arena + off + sizeof(h);
        off += sizeof(h) + h.size;
        switch (h.op) {
            case OP_SET_CB:
            case OP_SET_SRV:
            case OP_SET_SAMP: {
                RecSlots s = Get<RecSlots>(p, 0);
                void* const* v = (void* const*)(p + sizeof(RecSlots));
                UINT st = s.start, n = s.n;
                if (h.op == OP_SET_CB) {
                    ID3D11Buffer* const* b = (ID3D11Buffer* const*)v;
                    switch (h.a) {
                        case S_VS: c->VSSetConstantBuffers(st, n, b); break;
                        case S_HS: c->HSSetConstantBuffers(st, n, b); break;
                        case S_DS: c->DSSetConstantBuffers(st, n, b); break;
                        case S_GS: c->GSSetConstantBuffers(st, n, b); break;
                        case S_PS: c->PSSetConstantBuffers(st, n, b); break;
                        case S_CS: c->CSSetConstantBuffers(st, n, b); break;
                    }
                } else if (h.op == OP_SET_SRV) {
                    ID3D11ShaderResourceView* const* b = (ID3D11ShaderResourceView* const*)v;
                    ID3D11ShaderResourceView* t[kSrvSlots];
                    for (UINT i = 0; i < n && st + i < kSrvSlots && h.a < S_COUNT; i++) {
                        g_track.srv[h.a][st + i] = b[i];
                        t[i] = SubView(b[i]);
                    }
                    if (n <= kSrvSlots) SetSrvs(c, h.a, st, n, t);
                } else {
                    ID3D11SamplerState* const* b = (ID3D11SamplerState* const*)v;
                    switch (h.a) {
                        case S_VS: c->VSSetSamplers(st, n, b); break;
                        case S_HS: c->HSSetSamplers(st, n, b); break;
                        case S_DS: c->DSSetSamplers(st, n, b); break;
                        case S_GS: c->GSSetSamplers(st, n, b); break;
                        case S_PS: c->PSSetSamplers(st, n, b); break;
                        case S_CS: c->CSSetSamplers(st, n, b); break;
                    }
                }
                break;
            }
            case OP_SET_SHADER: {
                void* sh = Get<void*>(p, 0);
                UINT n = (UINT)Get<uint64_t>(p, 8);
                ID3D11ClassInstance* const* ci = n ? (ID3D11ClassInstance* const*)(p + 16) : nullptr;
                switch (h.a) {
                    case S_VS: c->VSSetShader((ID3D11VertexShader*)sh, ci, n); break;
                    case S_HS: c->HSSetShader((ID3D11HullShader*)sh, ci, n); break;
                    case S_DS: c->DSSetShader((ID3D11DomainShader*)sh, ci, n); break;
                    case S_GS: c->GSSetShader((ID3D11GeometryShader*)sh, ci, n); break;
                    case S_PS: c->PSSetShader((ID3D11PixelShader*)sh, ci, n); break;
                    case S_CS: c->CSSetShader((ID3D11ComputeShader*)sh, ci, n); break;
                }
                break;
            }
            case OP_CS_UAV: {
                RecSlots s = Get<RecSlots>(p, 0);
                const UINT* counts = (const UINT*)(p + sizeof(RecSlots) + s.n * 8);
                c->CSSetUnorderedAccessViews(s.start, s.n, (ID3D11UnorderedAccessView* const*)(p + sizeof(RecSlots)),
                                             h.a ? counts : nullptr);
                break;
            }
            case OP_IA_LAYOUT: c->IASetInputLayout(Get<ID3D11InputLayout*>(p, 0)); break;
            case OP_IA_VB: {
                UINT s = Get<uint32_t>(p, 0), k = Get<uint32_t>(p, 4);
                ID3D11Buffer* const* b = (ID3D11Buffer* const*)(p + 8);
                const UINT* strides = (const UINT*)(p + 8 + k * 8);
                const UINT* offs = (const UINT*)(p + 8 + k * 12);
                ID3D11Buffer* t[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
                for (UINT i = 0; i < k && s + i < D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT; i++) {
                    g_track.vb[s + i] = b[i];
                    g_track.stride[s + i] = strides[i];
                    g_track.off[s + i] = offs[i];
                    t[i] = SubBuf(b[i]);
                }
                c->IASetVertexBuffers(s, k, t, strides, offs);
                break;
            }
            case OP_IA_IB:
                g_track.ib = Get<ID3D11Buffer*>(p, 0);
                g_track.ibFmt = (DXGI_FORMAT)Get<uint32_t>(p, 8);
                g_track.ibOff = Get<uint32_t>(p, 12);
                c->IASetIndexBuffer(SubBuf(g_track.ib), g_track.ibFmt, g_track.ibOff);
                break;
            case OP_GEN: {
                ID3D11Buffer* b = (ID3D11Buffer*)(uintptr_t)Get<uint64_t>(p, 0);
                int idx = (int)Get<uint64_t>(p, 8);
                if (idx < 0 || idx >= g_genCount) break;
                int k = 0;
                while (k < g_substCount && g_subst[k].buf != b) k++;
                if (k == g_substCount) {
                    if (g_substCount >= 256) break;
                    g_substCount++;
                }
                g_subst[k] = {b, g_gens[idx].snap};
                Rebind(c, b);
                break;
            }
            case OP_IA_TOPO: c->IASetPrimitiveTopology((D3D11_PRIMITIVE_TOPOLOGY)h.a); break;
            case OP_DRAW: {
                if (skipping) {
                    g_skippedDraws++;
                    break;
                }
                GpuProfDraw();
                const uint32_t* v = (const uint32_t*)p;
                switch (h.a) {
                    case 0: c->Draw(v[0], v[1]); break;
                    case 1: c->DrawIndexed(v[0], v[1], (INT)v[2]); break;
                    case 2: c->DrawInstanced(v[0], v[1], v[2], v[3]); break;
                    case 3: c->DrawIndexedInstanced(v[0], v[1], v[2], (INT)v[3], v[4]); break;
                }
                break;
            }
            case OP_DRAW_AUTO: c->DrawAuto(); break;
            case OP_DRAW_INDIRECT:
                if (h.a)
                    c->DrawIndexedInstancedIndirect(SubBuf(Get<ID3D11Buffer*>(p, 0)), Get<uint32_t>(p, 8));
                else
                    c->DrawInstancedIndirect(SubBuf(Get<ID3D11Buffer*>(p, 0)), Get<uint32_t>(p, 8));
                break;
            case OP_DISPATCH: {
                GpuProfCompute(c, "R ", h.a);
                const uint32_t* v = (const uint32_t*)p;
                if (h.a) ViewFixBeforeDispatch(c, h.a);
                c->Dispatch(v[0], v[1], v[2]);
                if (h.a) ViewFixAfterDispatch(c);
                break;
            }
            case OP_DISPATCH_INDIRECT: c->DispatchIndirect(SubBuf(Get<ID3D11Buffer*>(p, 0)), Get<uint32_t>(p, 8)); break;
            case OP_OM_RT: {
                ID3D11RenderTargetView* const* rtvs = (ID3D11RenderTargetView* const*)(p + 8);
                ID3D11DepthStencilView* dsv = Get<ID3D11DepthStencilView*>(p, 0);
                skipping = g_cfg.reuseShadows && AllNull(rtvs, h.a) && IsShadowDsv(dsv);
                c->OMSetRenderTargets(h.a, rtvs, dsv);
                GpuProfTargets(c, h.a, rtvs, dsv, skipping ? "R reused " : "R ");
                break;
            }
            case OP_OM_RT_UAV: {
                UINT nrt = Get<uint32_t>(p, 0), us = Get<uint32_t>(p, 4), nu = Get<uint32_t>(p, 8),
                     hasCounts = Get<uint32_t>(p, 12);
                UINT kr = nrt == D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL ? 0 : (nrt > 8 ? 8 : nrt);
                UINT ku = nu == D3D11_KEEP_UNORDERED_ACCESS_VIEWS ? 0 : (nu > 8 ? 8 : nu);
                if (nrt != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) {
                    skipping = g_cfg.reuseShadows && AllNull((ID3D11RenderTargetView* const*)(p + 24), kr) && nu == 0 &&
                               IsShadowDsv(Get<ID3D11DepthStencilView*>(p, 16));
                    GpuProfTargets(c, kr, (ID3D11RenderTargetView* const*)(p + 24), Get<ID3D11DepthStencilView*>(p, 16),
                                   skipping ? "R reused " : "R ");
                }
                c->OMSetRenderTargetsAndUnorderedAccessViews(
                    nrt, kr ? (ID3D11RenderTargetView* const*)(p + 24) : nullptr, Get<ID3D11DepthStencilView*>(p, 16), us, nu,
                    ku ? (ID3D11UnorderedAccessView* const*)(p + 24 + kr * 8) : nullptr,
                    hasCounts && ku ? (const UINT*)(p + 24 + kr * 8 + ku * 8) : nullptr);
                break;
            }
            case OP_OM_BLEND:
                c->OMSetBlendState(Get<ID3D11BlendState*>(p, 0), h.a ? (const FLOAT*)(p + 8) : nullptr, Get<uint32_t>(p, 24));
                break;
            case OP_OM_DS: c->OMSetDepthStencilState(Get<ID3D11DepthStencilState*>(p, 0), Get<uint32_t>(p, 8)); break;
            case OP_RS_STATE: c->RSSetState(Get<ID3D11RasterizerState*>(p, 0)); break;
            case OP_RS_VP: c->RSSetViewports(h.a, (const D3D11_VIEWPORT*)p); break;
            case OP_RS_SCISSOR: c->RSSetScissorRects(h.a, (const D3D11_RECT*)p); break;
            case OP_SO: c->SOSetTargets(h.a, (ID3D11Buffer* const*)p, (const UINT*)(p + h.a * 8)); break;
            case OP_COPY_REGION: {
                const uint32_t* v = (const uint32_t*)(p + 16);
                c->CopySubresourceRegion(Get<ID3D11Resource*>(p, 0), v[0], v[1], v[2], v[3],
                                         (ID3D11Resource*)SubBuf((ID3D11Buffer*)Get<ID3D11Resource*>(p, 8)), v[4],
                                         h.a ? (const D3D11_BOX*)(p + 36) : nullptr);
                break;
            }
            case OP_COPY:
                c->CopyResource(Get<ID3D11Resource*>(p, 0), (ID3D11Resource*)SubBuf((ID3D11Buffer*)Get<ID3D11Resource*>(p, 8)));
                break;
            case OP_UPDATE: {
                struct Hdr {
                    void* res;
                    uint32_t sub, hasBox;
                    D3D11_BOX box;
                    uint32_t rp, dp, bytes, isCB;
                } u;
                memcpy(&u, p, sizeof(u));
                const void* data = p + sizeof(u);
                if (u.isCB) data = ProbePatchForReplay((ID3D11Resource*)u.res, data, u.bytes, scratch);
                c->UpdateSubresource((ID3D11Resource*)u.res, u.sub, u.hasBox ? &u.box : nullptr, data, u.rp, u.dp);
                break;
            }
            case OP_COPY_COUNT:
                c->CopyStructureCount(Get<ID3D11Buffer*>(p, 0), Get<uint32_t>(p, 16), Get<ID3D11UnorderedAccessView*>(p, 8));
                break;
            case OP_CLEAR_RTV: c->ClearRenderTargetView(Get<ID3D11RenderTargetView*>(p, 0), (const FLOAT*)(p + 8)); break;
            case OP_CLEAR_UAV_U: c->ClearUnorderedAccessViewUint(Get<ID3D11UnorderedAccessView*>(p, 0), (const UINT*)(p + 8)); break;
            case OP_CLEAR_UAV_F: c->ClearUnorderedAccessViewFloat(Get<ID3D11UnorderedAccessView*>(p, 0), (const FLOAT*)(p + 8)); break;
            case OP_CLEAR_DSV:
                if (g_cfg.reuseShadows && IsShadowDsv(Get<ID3D11DepthStencilView*>(p, 0))) break;
                c->ClearDepthStencilView(Get<ID3D11DepthStencilView*>(p, 0), Get<uint32_t>(p, 8), Get<float>(p, 12),
                                         (UINT8)Get<uint32_t>(p, 16));
                break;
            case OP_GENMIPS: c->GenerateMips(Get<ID3D11ShaderResourceView*>(p, 0)); break;
            case OP_MINLOD: c->SetResourceMinLOD(Get<ID3D11Resource*>(p, 0), Get<float>(p, 8)); break;
            case OP_RESOLVE:
                c->ResolveSubresource(Get<ID3D11Resource*>(p, 0), Get<uint32_t>(p, 16), Get<ID3D11Resource*>(p, 8),
                                      Get<uint32_t>(p, 20), (DXGI_FORMAT)Get<uint32_t>(p, 24));
                break;
            case OP_MAP: {
                struct Hdr {
                    void* res;
                    uint32_t sub, type, flags, rowPitch, depthPitch, bytes, isCB, pad;
                } m;
                memcpy(&m, p, sizeof(m));
                const void* data = p + sizeof(m);
                if (m.isCB) data = ProbePatchForReplay((ID3D11Resource*)m.res, data, m.bytes, scratch);
                // A buffer written with NO_OVERWRITE is mapped with DISCARD here and gets its whole
                // recorded contents (which include everything written before): draws already
                // submitted keep reading the old copy, so nothing they use is overwritten.
                D3D11_MAP type = (D3D11_MAP)m.type;
                if (type == D3D11_MAP_WRITE_NO_OVERWRITE) type = D3D11_MAP_WRITE_DISCARD;
                D3D11_MAPPED_SUBRESOURCE ms;
                if (SUCCEEDED(c->Map((ID3D11Resource*)m.res, m.sub, type, 0, &ms))) {
                    if (ms.RowPitch == m.rowPitch && ms.DepthPitch == m.depthPitch) {
                        memcpy(ms.pData, data, m.bytes);
                    } else {  // a different row pitch this time: copy row by row
                        Extent e;
                        if (GetExtent((ID3D11Resource*)m.res, m.sub, &e)) {
                            UINT bpb = 1, bd = 1;
                            FormatInfo(e.fmt, &bpb, &bd);
                            UINT rows = (e.h + bd - 1) / bd, row = ((e.w + bd - 1) / bd) * bpb;
                            for (UINT z = 0; z < e.d; z++)
                                for (UINT y = 0; y < rows; y++)
                                    memcpy((uint8_t*)ms.pData + (size_t)z * ms.DepthPitch + (size_t)y * ms.RowPitch,
                                           (const uint8_t*)data + (size_t)z * m.depthPitch + (size_t)y * m.rowPitch, row);
                        }
                    }
                    c->Unmap((ID3D11Resource*)m.res, m.sub);
                }
                break;
            }
            case OP_PREDICATION: c->SetPredication(Get<ID3D11Predicate*>(p, 0), h.a ? TRUE : FALSE); break;
            case OP_CLEAR_STATE:
                c->ClearState();
                memset(&g_track, 0, sizeof(g_track));
                break;
            case OP_DISCARD_RES:
            case OP_DISCARD_VIEW: {
                ID3D11DeviceContext1* c1 = nullptr;
                if (SUCCEEDED(c->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&c1))) {
                    if (h.op == OP_DISCARD_RES)
                        c1->DiscardResource(Get<ID3D11Resource*>(p, 0));
                    else
                        c1->DiscardView(Get<ID3D11View*>(p, 0));
                    c1->Release();
                }
                break;
            }
            default: break;
        }
    }
}
}  // namespace

// ---------------------------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------------------------
void ReplayInstall(ID3D11DeviceContext* ctx) {
    static bool done;
    if (done) return;
    done = true;
    g_recCtx = ctx;
    g_arena = (uint8_t*)VirtualAlloc(nullptr, kArenaCap, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    g_refs = (IUnknown**)VirtualAlloc(nullptr, kMaxRefs * sizeof(IUnknown*), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!g_arena || !g_refs) {
        Log("both eyes: could not reserve recording memory; BothEyes unavailable");
        return;
    }
    void** v = *(void***)ctx;
    int ok = 0, want = 0;
#define HK(idx, fn, orig, name) \
    want++;                     \
    ok += HookFunction(v[idx], (void*)&fn, (void**)&orig, name)
    HK(10, H_Slots<0>, g_slotHooks[0].orig, g_slotHooks[0].name);
    HK(25, H_Slots<1>, g_slotHooks[1].orig, g_slotHooks[1].name);
    HK(26, H_Slots<2>, g_slotHooks[2].orig, g_slotHooks[2].name);
    HK(31, H_Slots<3>, g_slotHooks[3].orig, g_slotHooks[3].name);
    HK(32, H_Slots<4>, g_slotHooks[4].orig, g_slotHooks[4].name);
    HK(59, H_Slots<5>, g_slotHooks[5].orig, g_slotHooks[5].name);
    HK(61, H_Slots<6>, g_slotHooks[6].orig, g_slotHooks[6].name);
    HK(62, H_Slots<7>, g_slotHooks[7].orig, g_slotHooks[7].name);
    HK(63, H_Slots<8>, g_slotHooks[8].orig, g_slotHooks[8].name);
    HK(65, H_Slots<9>, g_slotHooks[9].orig, g_slotHooks[9].name);
    HK(66, H_Slots<10>, g_slotHooks[10].orig, g_slotHooks[10].name);
    HK(70, H_Slots<11>, g_slotHooks[11].orig, g_slotHooks[11].name);
    HK(23, H_Shader<0>, g_shaderHooks[0].orig, g_shaderHooks[0].name);
    HK(60, H_Shader<1>, g_shaderHooks[1].orig, g_shaderHooks[1].name);
    HK(64, H_Shader<2>, g_shaderHooks[2].orig, g_shaderHooks[2].name);
    HK(17, H_IASetInputLayout, o_IASetInputLayout, "IASetInputLayout");
    HK(18, H_IASetVertexBuffers, o_IASetVertexBuffers, "IASetVertexBuffers");
    HK(19, H_IASetIndexBuffer, o_IASetIndexBuffer, "IASetIndexBuffer");
    HK(24, H_IASetPrimitiveTopology, o_IASetPrimitiveTopology, "IASetPrimitiveTopology");
    HK(30, H_SetPredication, o_SetPredication, "SetPredication");
    HK(34, H_OMSetRTUAV, o_OMSetRTUAV, "OMSetRenderTargetsAndUnorderedAccessViews");
    HK(35, H_OMSetBlendState, o_OMSetBlendState, "OMSetBlendState");
    HK(36, H_OMSetDepthStencilState, o_OMSetDepthStencilState, "OMSetDepthStencilState");
    HK(37, H_SOSetTargets, o_SOSetTargets, "SOSetTargets");
    HK(38, H_DrawAuto, o_DrawAuto, "DrawAuto");
    HK(39, H_DrawIndexedInstancedIndirect, o_DrawIndexedInstancedIndirect, "DrawIndexedInstancedIndirect");
    HK(40, H_DrawInstancedIndirect, o_DrawInstancedIndirect, "DrawInstancedIndirect");
    HK(42, H_DispatchIndirect, o_DispatchIndirect, "DispatchIndirect");
    HK(43, H_RSSetState, o_RSSetState, "RSSetState");
    HK(45, H_RSSetScissorRects, o_RSSetScissorRects, "RSSetScissorRects");
    HK(46, H_CopySubresourceRegion, o_CopySubresourceRegion, "CopySubresourceRegion");
    HK(47, H_CopyResource, o_CopyResource, "CopyResource");
    HK(49, H_CopyStructureCount, o_CopyStructureCount, "CopyStructureCount");
    HK(50, H_ClearRTV, o_ClearRTV, "ClearRenderTargetView");
    HK(51, H_ClearUAVUint, o_ClearUAVUint, "ClearUnorderedAccessViewUint");
    HK(52, H_ClearUAVFloat, o_ClearUAVFloat, "ClearUnorderedAccessViewFloat");
    HK(53, H_ClearDSV, o_ClearDSV, "ClearDepthStencilView");
    HK(54, H_GenerateMips, o_GenerateMips, "GenerateMips");
    HK(55, H_SetResourceMinLOD, o_SetResourceMinLOD, "SetResourceMinLOD");
    HK(57, H_Resolve, o_Resolve, "ResolveSubresource");
    HK(110, H_ClearState, o_ClearState, "ClearState");
    ID3D11DeviceContext1* c1 = nullptr;
    if (SUCCEEDED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&c1)) && c1) {
        void** v1 = *(void***)c1;
#define HK1(idx, fn, orig, name) \
    want++;                      \
    ok += HookFunction(v1[idx], (void*)&fn, (void**)&orig, name)
        HK1(117, H_DiscardResource, o_DiscardResource, "DiscardResource");
        HK1(118, H_DiscardView, o_DiscardView, "DiscardView");
        HK1(115, H_Ctx1<0>, o_Ctx1[0], "CopySubresourceRegion1");
        HK1(116, H_Ctx1<1>, o_Ctx1[1], "UpdateSubresource1");
        HK1(119, H_Ctx1<2>, o_Ctx1[2], "VSSetConstantBuffers1");
        HK1(123, H_Ctx1<3>, o_Ctx1[3], "PSSetConstantBuffers1");
        HK1(124, H_Ctx1<4>, o_Ctx1[4], "CSSetConstantBuffers1");
        HK1(132, H_Ctx1<5>, o_Ctx1[5], "ClearView");
        HK1(131, H_Ctx1<6>, o_Ctx1[6], "SwapDeviceContextState");
        HK1(122, H_Ctx1<7>, o_Ctx1[7], "GSSetConstantBuffers1");
#undef HK1
        c1->Release();
    }
#undef HK
    g_ready = ok == want;
    Log("both eyes: %d/%d extra context hooks in place%s (BothEyes=%d, F5 switches it)", ok, want,
        g_ready ? "" : "; BothEyes unavailable", g_cfg.bothEyes);
}

bool ReplayAvailable() { return g_ready; }

// Our own drawing at Present (depth reprojection): the game's state is saved and put back.
static StateBlock g_tempState;
void StateSaveTemp(ID3D11DeviceContext* c) { CaptureState(c, g_tempState); }
void StateRestoreTemp(ID3D11DeviceContext* c) {
    ApplyState(c, g_tempState);
    ReleaseState(g_tempState);
}

// Right after Present (StereoBeginFrame has decided this frame renders both eyes).
// Drops every reference the recorder holds on the game's objects. The swapchain can only be
// resized when nothing holds its back buffer (0.3.27: the game froze switching to 4.8K, its
// ResizeBuffers failed with DXGI_ERROR_INVALID_CALL because the saved pipeline states still had
// the back buffer's render target view).
void ReplayRelease() {
    if (!g_ready) return;
    AcquireSRWLockExclusive(&g_lock);
    InterlockedExchange(&g_recOn, 0);
    g_frameValid = false;
    for (auto& o : g_open) o.res = nullptr;
    ReleaseRefs();
    g_len = 0;
    ReleaseViewCache();
    ReleaseState(g_startState);
    ReleaseState(g_endState);
    ReleaseState(g_finalState);
    ReleaseSRWLockExclusive(&g_lock);
}

void ReplayFrameStart(ID3D11DeviceContext* c, bool bothEyes) {
    if (!g_ready) return;
    if (!bothEyes) {
        ReplayRelease();
        return;
    }
    ReleaseState(g_finalState);
    AcquireSRWLockExclusive(&g_lock);
    InterlockedExchange(&g_recOn, 0);
    for (auto& o : g_open) o.res = nullptr;
    ReleaseRefs();
    g_len = 0;
    g_frameCmds = 0;
    g_copyMs = 0;
    memset(g_counts, 0, sizeof(g_counts));
    g_frameValid = bothEyes;
    g_cancelWhy = nullptr;
    g_stoppedAtUi = false;
    g_genCount = 0;
    for (int i = 0; i < g_poolCount; i++) g_pool[i].used = false;
    ReleaseViewCache();
    ReleaseSRWLockExclusive(&g_lock);
    if (!bothEyes) return;
    CaptureState(c, g_startState);
    InterlockedExchange(&g_recOn, 1);
}

// At the first UI draw (fromUi) or at Present.
void ReplayStop(bool fromUi) {
    if (!g_recOn) return;
    AcquireSRWLockExclusive(&g_lock);
    if (g_recOn) {
        InterlockedExchange(&g_recOn, 0);
        g_stoppedAtUi = fromUi;
        for (int i = 0; i < g_genCount; i++) CloseGen(g_gens[i]);
        for (auto& o : g_open)
            if (o.res) {  // still mapped: the game writes it later, can't replay this frame
                g_frameValid = false;
                g_cancelWhy = "a resource stayed mapped across the end of the recording";
            }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

// The final pass (UI target -> backbuffer) as the game drew it, for the second eye's scene image.
void ReplayCaptureFinalPassState(ID3D11DeviceContext* c) { CaptureState(c, g_finalState); }
void ReplayApplyFinalPassState(ID3D11DeviceContext* c) { ApplyState(c, g_finalState); }

// At Present, after the left eye's frame was captured: draw the frame again for the right eye.
// Returns true when the right eye's image is ready (in the UI split's scene texture, or in the
// backbuffer when the frame had no UI).
bool ReplaySecondEye(ID3D11DeviceContext* c, bool* inBackbuffer) {
    *inBackbuffer = false;
    if (!g_ready) return false;
    ReplayStop(false);
    bool ok = g_frameValid && g_len > 0;
    g_statFrames++;
    if (!ok) {
        if (g_cancelWhy) {
            g_statCancelled++;
            pure::format(g_lastCancel, sizeof(g_lastCancel), "%s", g_cancelWhy);
        }
        return false;
    }
    double t0 = NowMs();
    g_replaying = true;
    CaptureState(c, g_endState);
    StereoBeginSecondEye();
    GpuProfMark(c, "replay start");
    ApplyState(c, g_startState);
    Play(c);
    GpuProfMark(c, "replay end");
    bool patched = StereoSecondEyePatched(), ready = patched;
    if (ready && g_stoppedAtUi) {
        ready = UiSecondEyeScene(c);
        if (!ready) pure::format(g_lastCancel, sizeof(g_lastCancel), "%s", "the final pass wasn't seen this frame");
    } else {
        *inBackbuffer = ready;
    }
    ApplyState(c, g_endState);
    ReleaseState(g_endState);
    ReleaseState(g_startState);
    ReleaseState(g_finalState);
    g_replaying = false;
    double ms = NowMs() - t0;
    if (ready) g_statReplayed++;
    else if (!patched) g_statNoCamera++;
    else g_statCancelled++;
    g_statReplayMs += ms;
    if (ms > g_statReplayMaxMs) g_statReplayMaxMs = ms;
    g_statCopyMs += g_copyMs;
    g_statCmds += g_frameCmds;
    g_statBytes += (double)g_len;
    g_statSkipped += g_skippedDraws;
    if (!g_loggedCounts) {
        g_loggedCounts = true;
        char line[900];
        size_t at = 0;
        for (int i = 1; i < OP_COUNT && at + 48 < sizeof(line); i++)
            if (g_counts[i]) at += pure::format(line + at, sizeof(line) - at, " %s %u,", kOpNames[i], g_counts[i]);
        Log("both eyes: first frame drawn twice: %u calls recorded (%u KB), %s;%s", g_frameCmds, (unsigned)(g_len >> 10),
            g_stoppedAtUi ? "stopped at the first UI draw" : "frame without UI, recorded to Present", line);
    }
    return ready;
}

// From the culling report (every 30 s, game thread).
void ReplayReport() {
    DWORD now = GetTickCount();
    if (now - g_statTick < 30000) return;
    g_statTick = now;
    if (!g_statFrames) return;
    unsigned n = g_statReplayed + g_statNoCamera;
    Log("both eyes: %u frames, %u drawn twice (%u without the camera found), %u cancelled%s%s; second eye %.1f ms "
        "of CPU time on average (max %.1f), recording copies %.2f ms, %.0f calls and %.0f KB per frame, %.0f shadow "
        "draws reused per frame",
        g_statFrames, g_statReplayed, g_statNoCamera, g_statCancelled, g_statCancelled ? ", last because: " : "",
        g_statCancelled ? g_lastCancel : "", n ? g_statReplayMs / n : 0.0, g_statReplayMaxMs,
        n ? g_statCopyMs / n : 0.0, n ? g_statCmds / n : 0.0, n ? g_statBytes / n / 1024 : 0.0, n ? g_statSkipped / n : 0.0);
    {
        char info[160];
        pure::format(info, sizeof(info), "Both eyes: %u of %u frames drawn twice, right eye %.1f ms CPU", g_statReplayed,
                     g_statFrames, n ? g_statReplayMs / n : 0.0);
        MenuSetInfo(3, info);
    }
    g_statFrames = g_statReplayed = g_statCancelled = g_statNoCamera = 0;
    g_statReplayMs = g_statReplayMaxMs = g_statCopyMs = g_statCmds = g_statBytes = g_statSkipped = 0;
}
