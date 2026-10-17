// reproject.cpp - depth-based correction of the older eye in alternate-eye mode (0.3.30).
//
// Alternate eyes: each game frame renders one eye, so the other eye's newest image is a game frame
// older and shows the world from where the camera was then. Here, at each Present, that older image
// is moved to this frame's camera using its depth buffer: every pixel is put back into 3D with its
// depth and projected again from where that eye would be now. Near and far things move by the
// right amounts (the rotation-only "eye sync" of 0.3.25 couldn't do that: NieR's camera orbits 2B).
// The moved image goes to the headset together with the new eye's image, so both eyes follow the
// camera 60 times a second. Things that move by themselves (enemies, 2B's animation) still update
// 30 times a second per eye.
//
// How: the game's depth buffer isn't readable by shaders (D24S8, depth-stencil only, 0.3.29 log), so
// at the first UI draw (the scene is finished) it is copied into a readable twin of the same format
// family. The scene image (the UI split's HUD-free picture) is copied at Present. Next frame a grid
// (one vertex every 4 pixels) is drawn over the old image: each vertex reads the depth there, is put
// into the old eye's view space, moved into the eye's view as it would be now, and projected; the
// pixel shader samples the old colour. A depth test keeps the nearest surface where things overlap.
// Where something uncovers the background the grid would stretch the near object's edge over the
// gap (0.4.0 user: "smearing, rubbery, around 2B"); since 0.4.1 such triangles (a depth jump inside
// a triangle the motion pulled open) are put behind everything and filled with the colour of the
// farthest pixel nearby, i.e. the background (ReprojectionEdges=1).
#include "common.h"
#include <d3dcompiler.h>

namespace {
const char kHlsl[] = R"(
cbuffer P : register(b0) {
    row_major float4x4 T;   // old eye view -> eye view now
    float4 proj;            // x scale, y scale, P22, P23 (z_ndc = (P22 z + P23) / -z)
    float4 size;            // width, height, grid step, grid width in cells
    float4 edge;            // depth ratio and stretch (in grid steps) above which a triangle is a gap; x = 0: off
};
Texture2D<float> dep : register(t0);
Texture2D col : register(t1);
SamplerState smp : register(s0);
struct V {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    nointerpolation float gap : TEXCOORD1;
};
// one grid corner: the old image's pixel there, moved into the eye's view as it is now
void Corner(uint2 c, out float4 clip, out float2 uv, out float dist) {
    float2 px = min(float2(c) * size.z, size.xy - 1);
    float d = dep.Load(int3(px, 0));
    uv = (px + 0.5) / size.xy;
    float2 ndc = float2(uv.x * 2 - 1, 1 - uv.y * 2);
    float z = proj.w / (-d - proj.z);                   // view-space z (negative in front)
    float3 p = float3(ndc.x * -z / proj.x, ndc.y * -z / proj.y, z);
    float3 q = mul(T, float4(p, 1)).xyz;
    clip = float4(q.x * proj.x, q.y * proj.y, proj.z * q.z + proj.w, -q.z);
    clip.z *= 0.999;  // real surfaces stay in front of the gap fill, the sky (depth 1) included
    dist = -z;
}
V vs(uint id : SV_VertexID) {
    uint cell = id / 6, k = id % 6, j = k % 3;
    uint gw = (uint)size.w;
    uint2 c = uint2(cell % gw, cell / gw);
    // each vertex works out its whole triangle, so all three agree whether it spans a gap
    uint2 c0 = k < 3 ? uint2(0, 0) : uint2(1, 0);
    uint2 c1 = k < 3 ? uint2(1, 0) : uint2(1, 1);
    uint2 c2 = uint2(0, 1);
    float4 p0, p1, p2;
    float2 u0, u1, u2;
    float d0, d1, d2;
    Corner(c + c0, p0, u0, d0);
    Corner(c + c1, p1, u1, d1);
    Corner(c + c2, p2, u2, d2);
    float dmin = min(d0, min(d1, d2)), dmax = max(d0, max(d1, d2));
    bool gap = false;
    if (edge.x > 0 && p0.w > 0 && p1.w > 0 && p2.w > 0) {
        float2 h = 0.5 * size.xy;
        float2 s0 = p0.xy / p0.w * h, s1 = p1.xy / p1.w * h, s2 = p2.xy / p2.w * h;
        float e = max(length(s0 - s1), max(length(s1 - s2), length(s2 - s0)));
        // a depth edge in the old image that the camera motion pulled open
        gap = dmax > dmin * edge.x && e > size.z * edge.y;
    }
    V o;
    o.pos = j == 0 ? p0 : (j == 1 ? p1 : p2);
    o.uv = j == 0 ? u0 : (j == 1 ? u1 : u2);
    o.gap = gap ? 1 : 0;
    if (gap) o.pos.z = o.pos.w * 0.9999;  // behind everything real: only fills what nothing else covers
    return o;
}
float4 ps(V i) : SV_Target {
    if (i.gap < 0.5) return col.SampleLevel(smp, i.uv, 0);
    // uncovered background: take the colour of the farthest pixel around (the background, not 2B's
    // edge stretched over the gap)
    float2 px = i.uv * size.xy - 0.5;
    float best = -1;
    float2 bp = px;
    [unroll] for (int y = -1; y <= 1; y++)
        [unroll] for (int x = -1; x <= 1; x++) {
            float2 q = clamp(px + float2(x, y) * size.z, 0, size.xy - 1);
            float d = dep.Load(int3(q, 0));
            if (d > best) { best = d; bp = q; }
        }
    return col.Load(int3(bp, 0));
}
)";

ID3D11VertexShader* g_vs;
ID3D11PixelShader* g_ps;
ID3D11Buffer* g_cb;
ID3D11SamplerState* g_smp;
ID3D11DepthStencilState* g_dss;
ID3D11RasterizerState* g_rs;
ID3D11BlendState* g_bs;
bool g_init, g_failed;

struct EyeData {
    ID3D11Texture2D* color;
    ID3D11ShaderResourceView* colorSrv;
    ID3D11Texture2D* depth;
    ID3D11ShaderResourceView* depthSrv;
    unsigned depthFrame;  // StereoFrame counter when the depth was copied
    bool colorOk, depthOk;
};
EyeData g_eye[2];
ID3D11Texture2D* g_out;
ID3D11RenderTargetView* g_outRtv;
ID3D11Texture2D* g_outDepth;
ID3D11DepthStencilView* g_outDsv;
D3D11_TEXTURE2D_DESC g_colorDesc, g_depthDesc;
ID3D11Resource* g_sceneDepth;  // raw: the game's full-size depth buffer, as last bound
UINT g_bbW, g_bbH;
unsigned g_frameCounter;
unsigned g_warps, g_skips;
// why frames weren't corrected (logged with the 30 s report)
enum { WHY_COLOR, WHY_DEPTH, WHY_OLD, WHY_SIZE, WHY_CAMERA, WHY_COUNT };
unsigned g_why[WHY_COUNT];
const char* const kWhy[WHY_COUNT] = {"no image of the other eye", "no depth of the other eye",
                                     "the other eye's depth isn't from the previous frame", "image and depth sizes differ",
                                     "the camera wasn't found in one of the two frames"};
DWORD g_reportTick;
bool g_logged;

template <typename T>
void Rel(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

void ReleaseTargets() {
    for (auto& e : g_eye) {
        Rel(e.colorSrv);
        Rel(e.color);
        Rel(e.depthSrv);
        Rel(e.depth);
        e.colorOk = e.depthOk = false;
    }
    Rel(g_outRtv);
    Rel(g_out);
    Rel(g_outDsv);
    Rel(g_outDepth);
    g_colorDesc = {};
    g_depthDesc = {};
}

bool Init(ID3D11Device* dev) {
    if (g_init || g_failed) return g_init;
    HMODULE comp = LoadLibraryW(L"d3dcompiler_47.dll");
    pD3DCompile compile = comp ? (pD3DCompile)(void*)GetProcAddress(comp, "D3DCompile") : nullptr;
    ID3DBlob *vb = nullptr, *pb = nullptr, *err = nullptr;
    HRESULT hr = compile ? compile(kHlsl, sizeof(kHlsl) - 1, "Reproject", nullptr, nullptr, "vs", "vs_5_0",
                                   D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vb, &err)
                         : E_FAIL;
    if (SUCCEEDED(hr)) hr = compile(kHlsl, sizeof(kHlsl) - 1, "Reproject", nullptr, nullptr, "ps", "ps_5_0",
                                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &pb, &err);
    if (SUCCEEDED(hr)) hr = dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &g_vs);
    if (SUCCEEDED(hr)) hr = dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &g_ps);
    if (FAILED(hr))
        Log("ERROR: depth reprojection shader: 0x%08x %s", (unsigned)hr, err ? (const char*)err->GetBufferPointer() : "");
    if (vb) vb->Release();
    if (pb) pb->Release();
    if (err) err->Release();
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = 112;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (SUCCEEDED(hr)) hr = dev->CreateBuffer(&bd, nullptr, &g_cb);
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (SUCCEEDED(hr)) hr = dev->CreateSamplerState(&sd, &g_smp);
    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc = D3D11_COMPARISON_LESS;
    if (SUCCEEDED(hr)) hr = dev->CreateDepthStencilState(&dd, &g_dss);
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (SUCCEEDED(hr)) hr = dev->CreateRasterizerState(&rd, &g_rs);
    D3D11_BLEND_DESC bl = {};
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (SUCCEEDED(hr)) hr = dev->CreateBlendState(&bl, &g_bs);
    g_init = SUCCEEDED(hr);
    g_failed = !g_init;
    if (g_failed) Log("depth reprojection: unavailable (0x%08x); alternate eyes work as before", (unsigned)hr);
    return g_init;
}

// Readable twin of the game's depth format, and the view format to sample its depth.
bool DepthFormats(DXGI_FORMAT f, DXGI_FORMAT* tex, DXGI_FORMAT* srv) {
    switch (f) {
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_R24G8_TYPELESS: *tex = DXGI_FORMAT_R24G8_TYPELESS; *srv = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; return true;
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_R32_TYPELESS: *tex = DXGI_FORMAT_R32_TYPELESS; *srv = DXGI_FORMAT_R32_FLOAT; return true;
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        case DXGI_FORMAT_R32G8X24_TYPELESS: *tex = DXGI_FORMAT_R32G8X24_TYPELESS; *srv = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; return true;
        case DXGI_FORMAT_D16_UNORM:
        case DXGI_FORMAT_R16_TYPELESS: *tex = DXGI_FORMAT_R16_TYPELESS; *srv = DXGI_FORMAT_R16_UNORM; return true;
        default: return false;
    }
}
DXGI_FORMAT ColorView(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        default: return f;
    }
}

bool EnsureDepth(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC& src) {
    if (g_eye[0].depth && g_depthDesc.Width == src.Width && g_depthDesc.Height == src.Height &&
        g_depthDesc.Format == src.Format)
        return true;
    DXGI_FORMAT tf, sf;
    if (!DepthFormats(src.Format, &tf, &sf) || src.SampleDesc.Count != 1) return false;
    for (auto& e : g_eye) {
        Rel(e.depthSrv);
        Rel(e.depth);
        e.depthOk = false;
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = src.Width;
        d.Height = src.Height;
        d.MipLevels = d.ArraySize = 1;
        d.Format = tf;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        D3D11_SHADER_RESOURCE_VIEW_DESC v = {};
        v.Format = sf;
        v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        v.Texture2D.MipLevels = 1;
        if (FAILED(dev->CreateTexture2D(&d, nullptr, &e.depth)) || FAILED(dev->CreateShaderResourceView(e.depth, &v, &e.depthSrv))) {
            Log("depth reprojection: could not create a readable depth copy (format %d)", (int)src.Format);
            return false;
        }
    }
    g_depthDesc = src;
    return true;
}

bool EnsureColor(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC& src) {
    if (g_out && g_colorDesc.Width == src.Width && g_colorDesc.Height == src.Height && g_colorDesc.Format == src.Format)
        return true;
    for (auto& e : g_eye) {
        Rel(e.colorSrv);
        Rel(e.color);
        e.colorOk = false;
    }
    Rel(g_outRtv);
    Rel(g_out);
    Rel(g_outDsv);
    Rel(g_outDepth);
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = src.Width;
    d.Height = src.Height;
    d.MipLevels = d.ArraySize = 1;
    d.Format = src.Format;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SHADER_RESOURCE_VIEW_DESC v = {};
    v.Format = ColorView(src.Format);
    v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    v.Texture2D.MipLevels = 1;
    bool ok = true;
    for (auto& e : g_eye)
        ok = ok && SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &e.color)) &&
             SUCCEEDED(dev->CreateShaderResourceView(e.color, &v, &e.colorSrv));
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    D3D11_RENDER_TARGET_VIEW_DESC rv = {};
    rv.Format = ColorView(src.Format);
    rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    ok = ok && SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &g_out)) && SUCCEEDED(dev->CreateRenderTargetView(g_out, &rv, &g_outRtv));
    D3D11_TEXTURE2D_DESC dd = d;
    dd.Format = DXGI_FORMAT_D32_FLOAT;
    dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ok = ok && SUCCEEDED(dev->CreateTexture2D(&dd, nullptr, &g_outDepth)) &&
         SUCCEEDED(dev->CreateDepthStencilView(g_outDepth, nullptr, &g_outDsv));
    if (!ok) {
        Log("depth reprojection: could not create its %ux%u textures", src.Width, src.Height);
        ReleaseTargets();
        return false;
    }
    g_colorDesc = src;
    return true;
}
}  // namespace

// From the OMSetRenderTargets hook (game thread): remember the game's full-size depth buffer.
void ReprojectNoteDsv(ID3D11DepthStencilView* dsv) {
    if (!g_cfg.depthReproject || g_cfg.bothEyes || !dsv || !g_bbW) return;
    ID3D11Resource* r = nullptr;
    dsv->GetResource(&r);
    if (!r) return;
    D3D11_RESOURCE_DIMENSION dim;
    r->GetType(&dim);
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        D3D11_TEXTURE2D_DESC d;
        ((ID3D11Texture2D*)r)->GetDesc(&d);
        if (d.Width == g_bbW && d.Height == g_bbH && d.SampleDesc.Count == 1) g_sceneDepth = r;
    }
    r->Release();  // raw pointer: only compared and copied from while the game still uses it
}

// At the first UI draw: the scene is finished; copy its depth for next frame's reprojection.
void ReprojectAtUiStart(ID3D11DeviceContext* c) {
    int eye = StereoFrameEye();
    if (!g_cfg.depthReproject || g_cfg.bothEyes || eye < 0 || !g_sceneDepth) return;
    ID3D11Device* dev = nullptr;
    c->GetDevice(&dev);
    if (!dev) return;
    D3D11_TEXTURE2D_DESC d;
    ((ID3D11Texture2D*)g_sceneDepth)->GetDesc(&d);
    if (EnsureDepth(dev, d)) {
        c->CopyResource(g_eye[eye].depth, g_sceneDepth);
        g_eye[eye].depthOk = true;
        g_eye[eye].depthFrame = g_frameCounter;
    }
    dev->Release();
}

void ReprojectRelease() {
    g_sceneDepth = nullptr;
    ReleaseTargets();
}

// At Present (alternate eyes), after this frame's eye was pushed to the headset: keep this eye's
// scene for next frame and return the other eye's previous image moved to this frame's camera.
ID3D11Texture2D* ReprojectOtherEye(ID3D11DeviceContext* c, ID3D11Texture2D* scene, ID3D11Texture2D* bb, FrameTag* tag) {
    // g_frameCounter = Presents finished so far: the frame being presented copied its depth with
    // this value, so the other eye's depth from the frame before has g_frameCounter - 1 (0.3.30
    // incremented first and compared against the wrong frame: nothing was ever corrected).
    struct Bump {
        ~Bump() { g_frameCounter++; }
    } bump;
    {
        D3D11_TEXTURE2D_DESC b;
        bb->GetDesc(&b);
        g_bbW = b.Width;
        g_bbH = b.Height;
    }
    int eye = StereoFrameEye();
    if (!g_cfg.depthReproject || g_cfg.bothEyes || eye < 0) {
        g_eye[0].colorOk = g_eye[1].colorOk = false;
        return nullptr;
    }
    ID3D11Device* dev = nullptr;
    c->GetDevice(&dev);
    if (!dev) return nullptr;
    dev->Release();  // the context keeps it alive
    if (!Init(dev)) return nullptr;
    ID3D11Texture2D* src = scene ? scene : bb;
    D3D11_TEXTURE2D_DESC sd;
    src->GetDesc(&sd);
    if (!EnsureColor(dev, sd)) return nullptr;
    int other = 1 - eye;
    float T[16], proj[4];
    ID3D11Texture2D* result = nullptr;
    EyeData& o = g_eye[other];
    int why = !o.colorOk ? WHY_COLOR
              : !o.depthOk ? WHY_DEPTH
              : o.depthFrame + 1 != g_frameCounter ? WHY_OLD
              : (g_depthDesc.Width != sd.Width || g_depthDesc.Height != sd.Height) ? WHY_SIZE
              : !StereoReprojectParams(other, T, proj, tag) ? WHY_CAMERA : -1;
    bool ready = why < 0;
    if (why >= 0) g_why[why]++;
    if (ready) {
        struct {
            float T[16];
            float proj[4];
            float size[4];
            float edge[4];
        } p;
        static_assert(sizeof(p) == 112, "cbuffer P");
        const float step = 4;
        UINT gw = (sd.Width + 3) / 4, gh = (sd.Height + 3) / 4;
        memcpy(p.T, T, 64);
        memcpy(p.proj, proj, 16);
        p.size[0] = (float)sd.Width;
        p.size[1] = (float)sd.Height;
        p.size[2] = step;
        p.size[3] = (float)gw;
        // gaps: depth jumps by more than 3% inside a triangle that the motion stretched to more
        // than 2.5 grid steps (a normal triangle's longest side is 1.4 steps)
        p.edge[0] = g_cfg.reprojectEdges ? 1.03f : 0.f;
        p.edge[1] = 2.5f;
        p.edge[2] = p.edge[3] = 0;
        g_replaying = true;  // our own calls: the hooks pass them straight through
        StateSaveTemp(c);
        c->UpdateSubresource(g_cb, 0, nullptr, &p, 0, 0);
        const FLOAT black[4] = {0, 0, 0, 1};
        c->ClearRenderTargetView(g_outRtv, black);
        c->ClearDepthStencilView(g_outDsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
        c->ClearState();
        c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        c->VSSetShader(g_vs, nullptr, 0);
        c->PSSetShader(g_ps, nullptr, 0);
        c->VSSetConstantBuffers(0, 1, &g_cb);
        ID3D11ShaderResourceView* srvs[2] = {o.depthSrv, o.colorSrv};
        c->VSSetShaderResources(0, 2, srvs);
        c->PSSetShaderResources(0, 2, srvs);
        c->PSSetSamplers(0, 1, &g_smp);
        D3D11_VIEWPORT vp = {0, 0, (float)sd.Width, (float)sd.Height, 0, 1};
        c->RSSetViewports(1, &vp);
        c->RSSetState(g_rs);
        c->OMSetDepthStencilState(g_dss, 0);
        c->OMSetBlendState(g_bs, nullptr, 0xffffffff);
        c->OMSetRenderTargets(1, &g_outRtv, g_outDsv);
        c->Draw(gw * gh * 6, 0);
        StateRestoreTemp(c);
        g_replaying = false;
        result = g_out;
        g_warps++;
        if (!g_logged) {
            g_logged = true;
            Log("depth reprojection: working (%ux%u, depth format %d, grid every %.0f pixels); the older eye now follows "
                "the camera",
                sd.Width, sd.Height, (int)g_depthDesc.Format, (double)step);
        }
    } else {
        g_skips++;
    }
    // this eye's scene, for the next frame
    c->CopyResource(g_eye[eye].color, src);
    g_eye[eye].colorOk = true;
    if (GetTickCount() - g_reportTick > 30000) {
        g_reportTick = GetTickCount();
        if (g_warps || g_skips) {
            char why[300];
            size_t at = 0;
            why[0] = 0;
            for (int i = 0; i < WHY_COUNT; i++)
                if (g_why[i]) at += pure::format(why + at, sizeof(why) - at, "%s%u x %s", at ? ", " : "", g_why[i], kWhy[i]);
            Log("depth reprojection: %u images moved to the new camera, %u frames without (30 s)%s%s", g_warps, g_skips,
                at ? ": " : "", why);
        }
        memset(g_why, 0, sizeof(g_why));
        char info[160];
        pure::format(info, sizeof(info), "Depth reprojection: %u of %u frames in the last 30 s", g_warps, g_warps + g_skips);
        MenuSetInfo(10, info);
        g_warps = g_skips = 0;
    }
    return result;
}
