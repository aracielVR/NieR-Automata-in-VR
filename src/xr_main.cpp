// xr_main.cpp - the VR thread: OpenXR instance/session, frame loop, stereo and the virtual screen.
//
// The VR thread runs at the headset's refresh rate, independent of the game's frame rate.
// Stereo: the game renders left and right eyes on alternate frames (stereo.cpp); each headset
// frame submits the newest left and right images in a projection layer, each with the eye pose it
// was rendered from, and the runtime reprojects them to the current head pose.
// Flat: the newest game frame on a quad layer floating in front of the user (menus, F12, startup).
#include "xr.h"
#include <d3dcompiler.h>

volatile DWORD g_xrThreadId;
void InstallCrashHandler();

#define XR_INSTANCE_FUNCS(X)                                                                               \
    X(xrGetInstanceProperties) X(xrGetSystem) X(xrGetSystemProperties) X(xrDestroyInstance)                \
    X(xrResultToString) X(xrPollEvent) X(xrCreateSession) X(xrDestroySession) X(xrBeginSession)            \
    X(xrEndSession) X(xrWaitFrame) X(xrBeginFrame) X(xrEndFrame) X(xrCreateReferenceSpace)                 \
    X(xrDestroySpace) X(xrLocateSpace) X(xrEnumerateSwapchainFormats) X(xrCreateSwapchain)                 \
    X(xrDestroySwapchain) X(xrEnumerateSwapchainImages) X(xrAcquireSwapchainImage)                         \
    X(xrWaitSwapchainImage) X(xrReleaseSwapchainImage) X(xrEnumerateEnvironmentBlendModes)                 \
    X(xrGetD3D11GraphicsRequirementsKHR) X(xrLocateViews) X(xrEnumerateViewConfigurations)                \
    X(xrEnumerateViewConfigurationViews)

static const uint32_t kMaxImages = 8;

static struct {
    PFN_xrGetInstanceProcAddr gipa;
#define DECL_FN(n) PFN_##n n;
    XR_INSTANCE_FUNCS(DECL_FN)
#undef DECL_FN

    XrInstance instance;
    XrSystemId system;
    LUID xrLuid;
    D3D_FEATURE_LEVEL minFeatureLevel;
    uint32_t maxSwapW, maxSwapH;

    // VR thread's own D3D11 device (on the game's GPU)
    LUID devLuid;
    D3D_FEATURE_LEVEL devLevel;
    ID3D11Device* dev;
    ID3D11DeviceContext* ctx;
    ID3D11VertexShader* vs;
    ID3D11PixelShader* ps;
    ID3D11PixelShader* psKey;  // HUD/menu extraction: alpha from |full - scene|
    ID3D11PixelShader* psWarp; // late warp: an eye image re-aimed to the headset's pose of this frame (0.5.1)
    ID3D11Buffer* cbWarp;
    ID3D11SamplerState* sampler;
    ID3D11Buffer* cbuf;
    int cbufMode;

    XrSession session;
    XrSessionState state;
    bool running;
    bool exitRequested;  // runtime ended the session (EXITING); don't restart it
    bool sessionLost;    // destroy & recreate the session
    bool instanceLost;   // destroy & recreate everything
    XrSpace local, view;
    XrEnvironmentBlendMode blend;

    int64_t swapFormat;
    struct Swap {
        XrSwapchain sc;
        uint32_t w, h, count;
        ID3D11RenderTargetView* rtv[kMaxImages];
        DWORD retryAt;
    } quadSwap, eyeSwap[2], hudSwap, menuSwap;
    // settings menu (menu.cpp): CPU image -> texture -> its own quad layer
    ID3D11Texture2D* menuTex;
    bool menuShown;  // the menu swapchain has a released image
    XrPosef menuPose;

    // game frames, on our device
    LONG servedReqGen;  // last game request we answered
    LONG publishGen;
    ID3D11Texture2D* ring[kRingSize];       // shared with the game device: full frames
    ID3D11Texture2D* ringScene[kRingSize];  // scene-only frames (UI removed)
    struct View {              // [0] left, [1] right, [2] mono
        ID3D11Texture2D* tex;
        ID3D11ShaderResourceView* srv;
        ID3D11Texture2D* sceneTex;
        ID3D11ShaderResourceView* sceneSrv;
        LONG seq;
        FrameTag tag;
        bool has;
    } views[3];
    UINT contentW, contentH;
    bool srcLinear;

    // UI coverage measurement (decides menu mode)
    ID3D11Texture2D* covTex;
    ID3D11RenderTargetView* covRtv;
    ID3D11Texture2D* covStaging[3];
    bool covPending[3];
    int covNext;
    LONG covMeasuredSeq;
    int menuVotes;  // >0 towards menu, <0 towards gameplay
    int noSceneRun;  // consecutive eye frames without a scene-only image
    unsigned statSceneFrames;
    float statCovSum, statCovMax;
    unsigned statCovN;
    bool menuMode;

    // screen placement
    bool placed;
    bool recenterRequested;
    XrPosef quadPose;
    pure::Q lastYaw;
    bool spaceFlipped;  // the runtime's LOCAL space has its +Y pointing down (0.5.2, Virtual Desktop)
    int upsideFrames;   // consecutive headset frames the head's up points against the reference's up
    float lastHead[3];  // head position last frame (silent space changes show as jumps)
    bool lastHeadValid;
    bool keyDown[256];

    // stats
    unsigned statFrames, statQuadFrames, statStereoFrames, statGameFrames[3];
    LARGE_INTEGER statStart, qpcFreq;
    unsigned errorCount;

    // head pose prediction (0.4.4): how long after the VR frame whose display time the game's pose
    // was predicted from (poseBase) its images are actually on screen, averaged
    double lagMs;          // running average
    bool lagHave;
    long long predictNs;   // extra prediction handed to the game thread
    double statLagSum;
    unsigned statLagN;
    float dispRot[2][4];   // eye orientations at this VR frame's display time
    bool dispValid;
    XrView disp[2];        // both eyes at this VR frame's display time (late warp submits these)
    uint32_t recW, recH;   // runtime's recommended per-eye image size
    float resPct, resPctH; // game picture's sharpness relative to that, horizontally / vertically (0 = unknown)
    UINT resSuggestW, resSuggestH;
    bool dispHave;         // disp holds a valid pair (kept from the last good one while tracking drops)
    // how far the shown image's pose is from the head at display (what the margin must cover)
    double statTurnSum;
    float statTurnMax;
    unsigned statTurnN, statTurnOver;  // over: frames where it exceeded the rendered margin
    float marginDeg;
} X;

// ---------------------------------------------------------------------------------------------
// Error helpers
// ---------------------------------------------------------------------------------------------
static const char* XrStr(XrResult r) {
    static char buf[XR_MAX_RESULT_STRING_SIZE + 16];
    if (X.instance && X.xrResultToString && XR_SUCCEEDED(X.xrResultToString(X.instance, r, buf))) return buf;
    pure::format(buf, sizeof(buf), "XrResult(%d)", (int)r);
    return buf;
}

// Logs failures; per-frame calls are rate-limited so a persistent error can't flood the log.
static bool Check(XrResult r, const char* what, bool perFrame = false) {
    if (XR_SUCCEEDED(r)) return true;
    if (!perFrame || X.errorCount < 20 || (X.errorCount % 1000) == 0)
        Log("OpenXR error: %s -> %s%s", what, XrStr(r), perFrame && X.errorCount == 19 ? " (further per-frame errors throttled)" : "");
    if (perFrame) X.errorCount++;
    if (r == XR_ERROR_SESSION_LOST) X.sessionLost = true;
    if (r == XR_ERROR_INSTANCE_LOST || r == XR_ERROR_RUNTIME_FAILURE) X.instanceLost = true;
    return false;
}
#define XR_CHECK(call) Check((call), #call)
#define XR_CHECK_FRAME(call) Check((call), #call, true)

template <typename T>
static void SafeRelease(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

static bool IsSrgb(int64_t f) {
    return f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
           f == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
}

// ---------------------------------------------------------------------------------------------
// Instance & system
// ---------------------------------------------------------------------------------------------
static bool InitInstance() {
    X.gipa = XrLoadEntry();
    if (!X.gipa) return false;

    PFN_xrEnumerateInstanceExtensionProperties enumExt = nullptr;
    PFN_xrCreateInstance createInstance = nullptr;
    X.gipa(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", (PFN_xrVoidFunction*)&enumExt);
    X.gipa(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction*)&createInstance);
    if (!enumExt || !createInstance) {
        Log("ERROR: OpenXR: runtime is missing core functions");
        return false;
    }
    uint32_t n = 0;
    if (!XR_CHECK(enumExt(nullptr, 0, &n, nullptr))) return false;
    static XrExtensionProperties exts[256];
    if (n > 256) n = 256;
    for (uint32_t i = 0; i < n; i++) {
        exts[i] = {};
        exts[i].type = XR_TYPE_EXTENSION_PROPERTIES;
    }
    if (!XR_CHECK(enumExt(nullptr, n, &n, exts))) return false;
    bool hasD3D11 = false, hasPico = false;
    for (uint32_t i = 0; i < n; i++) {
        LogV("OpenXR extension: %s v%u", exts[i].extensionName, exts[i].extensionVersion);
        if (!lstrcmpA(exts[i].extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME)) hasD3D11 = true;
        if (!lstrcmpA(exts[i].extensionName, "XR_BD_controller_interaction")) hasPico = true;  // Pico 4 / Neo3 controllers
    }
    if (!hasD3D11) {
        Log("ERROR: OpenXR runtime does not support Direct3D 11 (XR_KHR_D3D11_enable)");
        return false;
    }

    const char* enabled[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME, "XR_BD_controller_interaction"};
    XrInstanceCreateInfo ci = {};
    ci.type = XR_TYPE_INSTANCE_CREATE_INFO;
    lstrcpynA(ci.applicationInfo.applicationName, "NieRVR", XR_MAX_APPLICATION_NAME_SIZE);
    ci.applicationInfo.applicationVersion = 1;
    lstrcpynA(ci.applicationInfo.engineName, "NieRVR", XR_MAX_ENGINE_NAME_SIZE);
    ci.applicationInfo.engineVersion = 1;
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ci.enabledExtensionCount = hasPico ? 2 : 1;
    ci.enabledExtensionNames = enabled;
    XrResult r = createInstance(&ci, &X.instance);
    if (r == XR_ERROR_API_VERSION_UNSUPPORTED) {
        Log("OpenXR: runtime rejected API 1.0, retrying with 1.1");
        ci.applicationInfo.apiVersion = XR_API_VERSION_1_1;
        r = createInstance(&ci, &X.instance);
    }
    if (!Check(r, "xrCreateInstance")) {
        X.instance = XR_NULL_HANDLE;
        return false;
    }

    bool ok = true;
#define LOAD_FN(n)                                                                     \
    if (XR_FAILED(X.gipa(X.instance, #n, (PFN_xrVoidFunction*)&X.n)) || !X.n) { \
        Log("ERROR: OpenXR: missing function " #n);                                   \
        ok = false;                                                                    \
    }
    XR_INSTANCE_FUNCS(LOAD_FN)
#undef LOAD_FN
    if (!ok) return false;
    VrInputInit(X.gipa, X.instance, hasPico);

    XrInstanceProperties ip = {};
    ip.type = XR_TYPE_INSTANCE_PROPERTIES;
    if (XR_SUCCEEDED(X.xrGetInstanceProperties(X.instance, &ip)))
        Log("OpenXR runtime: %s %u.%u.%u", ip.runtimeName, (unsigned)XR_VERSION_MAJOR(ip.runtimeVersion),
            (unsigned)XR_VERSION_MINOR(ip.runtimeVersion), (unsigned)XR_VERSION_PATCH(ip.runtimeVersion));
    return true;
}

static bool InitSystem() {
    static bool loggedWaiting = false;
    XrSystemGetInfo gi = {};
    gi.type = XR_TYPE_SYSTEM_GET_INFO;
    gi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrResult r = X.xrGetSystem(X.instance, &gi, &X.system);
    if (r == XR_ERROR_FORM_FACTOR_UNAVAILABLE) {
        if (!loggedWaiting) Log("OpenXR: headset not available yet (is it connected and awake?) - waiting");
        loggedWaiting = true;
        return false;
    }
    if (!XR_CHECK(r)) return false;
    loggedWaiting = false;

    XrSystemProperties sp = {};
    sp.type = XR_TYPE_SYSTEM_PROPERTIES;
    if (XR_SUCCEEDED(X.xrGetSystemProperties(X.instance, X.system, &sp))) {
        Log("headset: %s (vendor 0x%x), max swapchain %ux%u, max layers %u, position tracking %d", sp.systemName,
            sp.vendorId, sp.graphicsProperties.maxSwapchainImageWidth, sp.graphicsProperties.maxSwapchainImageHeight,
            sp.graphicsProperties.maxLayerCount, (int)sp.trackingProperties.positionTracking);
        X.maxSwapW = sp.graphicsProperties.maxSwapchainImageWidth;
        X.maxSwapH = sp.graphicsProperties.maxSwapchainImageHeight;
    }

    // the mod renders a stereo pair: the system must offer the primary stereo view configuration
    {
        XrViewConfigurationType types[16];
        uint32_t nt = 0;
        bool stereo = false;
        if (XR_SUCCEEDED(X.xrEnumerateViewConfigurations(X.instance, X.system, 16, &nt, types)))
            for (uint32_t i = 0; i < nt; i++) stereo |= types[i] == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        if (!stereo) {
            Log("ERROR: this OpenXR system has no stereo view configuration (%u others)", nt);
            return false;
        }
    }

    // the per-eye image size the runtime recommends (for the resolution hint)
    {
        XrViewConfigurationView vv[2] = {};
        vv[0].type = vv[1].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
        uint32_t nv = 0;
        if (XR_SUCCEEDED(X.xrEnumerateViewConfigurationViews(X.instance, X.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2,
                                                             &nv, vv)) && nv == 2) {
            X.recW = vv[0].recommendedImageRectWidth;
            X.recH = vv[0].recommendedImageRectHeight;
            Log("headset recommends %ux%u per eye (max %ux%u)", X.recW, X.recH, vv[0].maxImageRectWidth,
                vv[0].maxImageRectHeight);
        }
    }

    XrGraphicsRequirementsD3D11KHR req = {};
    req.type = XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR;
    if (!XR_CHECK(X.xrGetD3D11GraphicsRequirementsKHR(X.instance, X.system, &req))) return false;
    X.xrLuid = req.adapterLuid;
    X.minFeatureLevel = req.minFeatureLevel;
    Log("headset GPU LUID %08x:%08x, min feature level 0x%x", (unsigned)req.adapterLuid.HighPart,
        (unsigned)req.adapterLuid.LowPart, (unsigned)req.minFeatureLevel);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Our D3D11 device and the blit shader
// ---------------------------------------------------------------------------------------------
static const char kShader[] = R"(
cbuffer Params : register(b0) { float4 params; };   // params.x: 0 = copy, 1 = sRGB->linear, 2 = linear->sRGB
Texture2D src : register(t0);
Texture2D src2 : register(t1);
SamplerState samp : register(s0);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut VS(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float3 ToLinear(float3 c) { return lerp(pow((c + 0.055) / 1.055, 2.4), c / 12.92, step(c, 0.04045)); }
float3 ToSrgb(float3 c) { c = saturate(c); return lerp(1.055 * pow(c, 1.0 / 2.4) - 0.055, c * 12.92, step(c, 0.0031308)); }
float3 Convert(float3 c) {
    if (params.x > 1.5) return ToSrgb(c);
    if (params.x > 0.5) return ToLinear(c);
    return c;
}
float4 PS(VSOut i) : SV_Target {
    return float4(Convert(src.Sample(samp, i.uv).rgb), 1);
}
// Late warp (0.5.1): the eye image re-aimed to the headset's pose for this frame, covering exactly
// the eye's own field of view. Output pixel -> direction in the shown eye (outTan: tangents of left,
// right, up, down) -> rotated into the eye the image was rendered from (m0..m2 = R_render^T R_shown)
// -> its symmetric frustum (srcTan: tangent half-width, half-height). Rotation is exact; the small
// eye translation between the two poses is ignored, as a compositor's rotational reprojection does.
cbuffer Warp : register(b1) { float4 outTan; float4 srcTan; float4 m0; float4 m1; float4 m2; };
float4 PSWarp(VSOut i) : SV_Target {
    float3 d = float3(lerp(outTan.x, outTan.y, i.uv.x), lerp(outTan.z, outTan.w, i.uv.y), -1);
    float3 r = float3(dot(m0.xyz, d), dot(m1.xyz, d), dot(m2.xyz, d));
    if (r.z > -1e-4) return float4(0, 0, 0, 1);
    float2 t = r.xy / -r.z;
    float2 uv = float2(t.x / srcTan.x * 0.5 + 0.5, 0.5 - t.y / srcTan.y * 0.5);
    if (any(uv < 0) || any(uv > 1)) return float4(0, 0, 0, 1);
    return float4(Convert(src.SampleLevel(samp, uv, 0).rgb), 1);
}
// HUD/menus. src = full frame, src2 = scene without UI, whose alpha channel holds the UI's real
// coverage (accumulated on the game side from the UI draws' own alpha). Since
// full = ui + (1 - a) * scene, the HUD layer is ui = full - (1 - a) * scene, premultiplied:
// translucent panels darken the 3D scene instead of carrying a flat copy of it.
// params.y/z: where nothing was drawn (a = 0), differences below y are noise, above z kept
// (additive glows).
float4 PSKey(VSOut i) : SV_Target {
    float3 f = src.Sample(samp, i.uv).rgb;
    float4 s = src2.Sample(samp, i.uv);
    float a = saturate(s.a);
    float3 d = abs(f - s.rgb);
    float3 pm = max(f - (1 - a) * s.rgb, 0);
    pm *= max(step(0.004, a), smoothstep(params.y, params.z, max(d.r, max(d.g, d.b))));
    float3 c = a > 0.01 ? Convert(saturate(pm / a)) * a : Convert(pm);
    return float4(c, a);
}
)";

static bool CompileShaders() {
    HMODULE comp = LoadLibraryW(L"d3dcompiler_47.dll");
    pD3DCompile compile = comp ? (pD3DCompile)(void*)GetProcAddress(comp, "D3DCompile") : nullptr;
    if (!compile) {
        Log("ERROR: d3dcompiler_47.dll not available (error %u)", (unsigned)GetLastError());
        return false;
    }
    ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
    HRESULT hr = compile(kShader, sizeof(kShader) - 1, "niervr_blit", nullptr, nullptr, "VS", "vs_4_0",
                         D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsb, &err);
    if (FAILED(hr)) {
        Log("ERROR: VS compile 0x%08x: %s", (unsigned)hr, err ? (const char*)err->GetBufferPointer() : "");
        SafeRelease(err);
        return false;
    }
    hr = compile(kShader, sizeof(kShader) - 1, "niervr_blit", nullptr, nullptr, "PS", "ps_4_0",
                 D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psb, &err);
    if (FAILED(hr)) {
        Log("ERROR: PS compile 0x%08x: %s", (unsigned)hr, err ? (const char*)err->GetBufferPointer() : "");
        SafeRelease(err);
        SafeRelease(vsb);
        return false;
    }
    hr = X.dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &X.vs);
    if (SUCCEEDED(hr)) hr = X.dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &X.ps);
    SafeRelease(vsb);
    SafeRelease(psb);
    if (SUCCEEDED(hr)) {
        hr = compile(kShader, sizeof(kShader) - 1, "niervr_key", nullptr, nullptr, "PSKey", "ps_4_0",
                     D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psb, &err);
        if (FAILED(hr)) Log("ERROR: PSKey compile 0x%08x: %s", (unsigned)hr, err ? (const char*)err->GetBufferPointer() : "");
        SafeRelease(err);
        if (SUCCEEDED(hr)) hr = X.dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &X.psKey);
        SafeRelease(psb);
    }
    if (SUCCEEDED(hr)) {  // optional: without it the eyes are submitted as rendered (LateWarp off)
        HRESULT he = compile(kShader, sizeof(kShader) - 1, "niervr_warp", nullptr, nullptr, "PSWarp", "ps_4_0",
                             D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psb, &err);
        if (FAILED(he)) Log("late warp: shader compile 0x%08x: %s", (unsigned)he, err ? (const char*)err->GetBufferPointer() : "");
        SafeRelease(err);
        if (SUCCEEDED(he)) X.dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &X.psWarp);
        SafeRelease(psb);
        D3D11_BUFFER_DESC wb = {};
        wb.ByteWidth = 80;
        wb.Usage = D3D11_USAGE_DEFAULT;
        wb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (X.psWarp && FAILED(X.dev->CreateBuffer(&wb, nullptr, &X.cbWarp))) SafeRelease(X.psWarp);
    }
    if (FAILED(hr)) {
        Log("ERROR: CreateShader 0x%08x", (unsigned)hr);
        return false;
    }
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    hr = X.dev->CreateSamplerState(&sd, &X.sampler);
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = 16;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (SUCCEEDED(hr)) hr = X.dev->CreateBuffer(&bd, nullptr, &X.cbuf);
    if (FAILED(hr)) Log("ERROR: sampler/cbuffer 0x%08x", (unsigned)hr);
    return SUCCEEDED(hr);
}

// Created on the game's GPU as soon as the VR thread starts, before OpenXR is involved, because it
// also owns the texture the game copies its frames into.
static bool InitDevice() {
    // Use the system d3d11.dll directly so a chain-loaded mod never wraps our private device.
    wchar_t path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH - 16);
    lstrcpyW(path + n, L"\\d3d11.dll");
    HMODULE d3d11 = LoadLibraryW(path);
    PFN_D3D11_CREATE_DEVICE createDevice =
        d3d11 ? (PFN_D3D11_CREATE_DEVICE)(void*)GetProcAddress(d3d11, "D3D11CreateDevice") : nullptr;
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    typedef HRESULT(WINAPI * PFN_CreateDXGIFactory1)(REFIID, void**);
    PFN_CreateDXGIFactory1 createFactory =
        dxgi ? (PFN_CreateDXGIFactory1)(void*)GetProcAddress(dxgi, "CreateDXGIFactory1") : nullptr;
    if (!createDevice || !createFactory) {
        Log("ERROR: cannot load D3D11/DXGI entry points");
        return false;
    }
    IDXGIFactory1* factory = nullptr;
    if (FAILED(createFactory(__uuidof(IDXGIFactory1), (void**)&factory))) {
        Log("ERROR: CreateDXGIFactory1 failed");
        return false;
    }
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 d;
        bool match = SUCCEEDED(adapter->GetDesc1(&d)) &&
                     (!g_share.gameLuidValid || (d.AdapterLuid.LowPart == g_share.gameLuid.LowPart &&
                                                 d.AdapterLuid.HighPart == g_share.gameLuid.HighPart));
        if (match) {
            Log("VR device on GPU: %S", d.Description);
            X.devLuid = d.AdapterLuid;
            break;
        }
        SafeRelease(adapter);
    }
    factory->Release();
    if (!adapter) {
        Log("ERROR: could not find the game's GPU for the VR device");
        return false;
    }
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                        D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got;
    HRESULT hr = createDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 4,
                              D3D11_SDK_VERSION, &X.dev, &got, &X.ctx);
    if (hr == E_INVALIDARG)  // Windows without 11.1 support
        hr = createDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels + 1, 3,
                          D3D11_SDK_VERSION, &X.dev, &got, &X.ctx);
    adapter->Release();
    if (FAILED(hr)) {
        Log("ERROR: VR D3D11CreateDevice failed 0x%08x", (unsigned)hr);
        return false;
    }
    X.devLevel = got;
    Log("VR device created (feature level 0x%x)", (unsigned)got);
    // 0.5.1: the VR thread's few small GPU jobs per headset frame must not wait behind the game's
    // frame on the same GPU, or the runtime sees late frames and throttles the app (0.5.0 Pico log:
    // headset 27-32 fps in both-eyes mode while the GPU needed only 15-19 ms per game frame)
    {
        IDXGIDevice* xd = nullptr;
        if (SUCCEEDED(X.dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&xd)) && xd) {
            HRESULT ph = E_FAIL;
            int prio = 7;
            for (; prio > 0 && FAILED(ph = xd->SetGPUThreadPriority(prio)); prio -= 3) {}
            Log("VR device GPU priority %d%s (0x%08x)", SUCCEEDED(ph) ? prio : 0,
                SUCCEEDED(ph) ? " (above the game)" : ": not allowed, normal", (unsigned)ph);
            xd->Release();
        }
    }
    return CompileShaders();
}

// After OpenXR tells us which GPU drives the headset: it must be the game's GPU.
static bool CheckHeadsetGpu() {
    if (X.xrLuid.LowPart != X.devLuid.LowPart || X.xrLuid.HighPart != X.devLuid.HighPart) {
        Log("ERROR: the headset is driven by a different GPU than the game. On laptops/multi-GPU systems, set "
            "NieRAutomata.exe to the high-performance GPU in Windows Settings > Display > Graphics.");
        return false;
    }
    if (X.devLevel < X.minFeatureLevel) {
        Log("ERROR: GPU feature level 0x%x is below the runtime's minimum 0x%x", (unsigned)X.devLevel,
            (unsigned)X.minFeatureLevel);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------------------------
typedef decltype(X.quadSwap) Swap;

static void DestroySwap(Swap& s) {
    for (uint32_t i = 0; i < kMaxImages; i++) SafeRelease(s.rtv[i]);
    if (s.sc) X.xrDestroySwapchain(s.sc);
    s.sc = XR_NULL_HANDLE;
    s.count = s.w = s.h = 0;
}

static void DestroyAllSwaps() {
    DestroySwap(X.quadSwap);
    DestroySwap(X.eyeSwap[0]);
    DestroySwap(X.eyeSwap[1]);
    DestroySwap(X.hudSwap);
    DestroySwap(X.menuSwap);
    X.menuShown = false;
    SafeRelease(X.menuTex);
}

static void DestroySession() {
    VrInputDetach();
    DestroyAllSwaps();
    AcquireSRWLockExclusive(&g_stereo.lock);
    g_stereo.valid = false;
    ReleaseSRWLockExclusive(&g_stereo.lock);
    if (X.local) X.xrDestroySpace(X.local);
    if (X.view) X.xrDestroySpace(X.view);
    if (X.session) X.xrDestroySession(X.session);
    X.local = X.view = XR_NULL_HANDLE;
    X.session = XR_NULL_HANDLE;
    X.running = false;
    X.sessionLost = false;
    X.placed = false;
    X.state = XR_SESSION_STATE_UNKNOWN;
}

static void DestroyInstance() {
    DestroySession();
    VrInputShutdown();
    if (X.instance) X.xrDestroyInstance(X.instance);
    X.instance = XR_NULL_HANDLE;
    X.system = XR_NULL_SYSTEM_ID;
    X.instanceLost = false;
}

static bool InitSession() {
    XrGraphicsBindingD3D11KHR binding = {};
    binding.type = XR_TYPE_GRAPHICS_BINDING_D3D11_KHR;
    binding.device = X.dev;
    XrSessionCreateInfo ci = {};
    ci.type = XR_TYPE_SESSION_CREATE_INFO;
    ci.next = &binding;
    ci.systemId = X.system;
    if (!XR_CHECK(X.xrCreateSession(X.instance, &ci, &X.session))) {
        X.session = XR_NULL_HANDLE;
        return false;
    }
    XrReferenceSpaceCreateInfo rs = {};
    rs.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
    rs.poseInReferenceSpace.orientation.w = 1;
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    if (!XR_CHECK(X.xrCreateReferenceSpace(X.session, &rs, &X.local))) return false;
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    if (!XR_CHECK(X.xrCreateReferenceSpace(X.session, &rs, &X.view))) return false;
    VrInputAttach(X.session);

    uint32_t n = 0;
    XrEnvironmentBlendMode modes[8];
    X.blend = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    if (XR_SUCCEEDED(X.xrEnumerateEnvironmentBlendModes(X.instance, X.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                         8, &n, modes)) &&
        n > 0)
        X.blend = modes[0];

    // Pick a swapchain format: prefer sRGB so the runtime displays the game's colours exactly.
    int64_t formats[64];
    n = 0;
    if (!XR_CHECK(X.xrEnumerateSwapchainFormats(X.session, 64, &n, formats))) return false;
    const int64_t preferred[] = {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
                                 DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM};
    X.swapFormat = 0;
    for (int64_t p : preferred) {
        for (uint32_t i = 0; i < n && !X.swapFormat; i++)
            if (formats[i] == p) X.swapFormat = p;
        if (X.swapFormat) break;
    }
    if (!X.swapFormat) {
        Log("ERROR: runtime offers no usable 8-bit swapchain format (%u formats)", n);
        return false;
    }
    Log("OpenXR session created (blend mode %d, swapchain format %d)", (int)X.blend, (int)X.swapFormat);
    X.errorCount = 0;
    return true;
}

static bool EnsureSwap(Swap& s, uint32_t w, uint32_t h, const char* what, bool copyTarget = false) {
    // Respect runtime limits, keep aspect ratio
    uint32_t maxW = X.maxSwapW ? X.maxSwapW : 4096, maxH = X.maxSwapH ? X.maxSwapH : 4096;
    if (w > maxW) {
        h = (uint32_t)((uint64_t)h * maxW / w);
        w = maxW;
    }
    if (h > maxH) {
        w = (uint32_t)((uint64_t)w * maxH / h);
        h = maxH;
    }
    if (s.sc && s.w == w && s.h == h) return true;
    if ((LONG)(GetTickCount() - s.retryAt) < 0) return false;
    s.retryAt = GetTickCount() + 2000;  // after a failure, retry at most every 2 s
    DestroySwap(s);

    XrSwapchainCreateInfo ci = {};
    ci.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                    (copyTarget ? XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT : 0);
    ci.format = X.swapFormat;
    ci.sampleCount = 1;
    ci.width = w;
    ci.height = h;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    if (!XR_CHECK(X.xrCreateSwapchain(X.session, &ci, &s.sc))) {
        s.sc = XR_NULL_HANDLE;
        return false;
    }
    XrSwapchainImageD3D11KHR images[kMaxImages];
    for (uint32_t i = 0; i < kMaxImages; i++) {
        images[i] = {};
        images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
    }
    uint32_t n = 0;
    if (!XR_CHECK(X.xrEnumerateSwapchainImages(s.sc, kMaxImages, &n, (XrSwapchainImageBaseHeader*)images))) {
        DestroySwap(s);
        return false;
    }
    for (uint32_t i = 0; i < n; i++) {
        D3D11_RENDER_TARGET_VIEW_DESC rd = {};
        rd.Format = (DXGI_FORMAT)X.swapFormat;
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        HRESULT hr = X.dev->CreateRenderTargetView(images[i].texture, &rd, &s.rtv[i]);
        if (FAILED(hr)) {
            Log("ERROR: CreateRenderTargetView on swapchain image %u failed 0x%08x", i, (unsigned)hr);
            DestroySwap(s);
            return false;
        }
    }
    s.count = n;
    s.w = w;
    s.h = h;
    Log("%s swapchain %ux%u, %u images", what, w, h, n);
    return true;
}

// Draws `srv` into the next image of `s`. Returns false if nothing could be submitted.
struct WarpParams {
    float outTan[4];  // shown eye: tangents of left, right, up, down
    float srcTan[4];  // rendered image: tangent half-width, half-height
    float m[3][4];    // rows of R_render^T * R_shown
};
static void Blit(ID3D11RenderTargetView* rtv, uint32_t w, uint32_t h, ID3D11ShaderResourceView* srv,
                 ID3D11ShaderResourceView* scene, bool dstSrgb, const WarpParams* warp = nullptr);
// Draws `srv` (or, with `scene`, the keyed-out UI of `srv`; with `warp`, the late-warped eye image)
// into the next image of `s`.
static bool RenderInto(Swap& s, ID3D11ShaderResourceView* srv, ID3D11ShaderResourceView* scene = nullptr,
                       const WarpParams* warp = nullptr) {
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai = {};
    ai.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
    if (!XR_CHECK_FRAME(X.xrAcquireSwapchainImage(s.sc, &ai, &idx))) return false;
    XrSwapchainImageWaitInfo wi = {};
    wi.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
    wi.timeout = XR_INFINITE_DURATION;
    bool waited = XR_CHECK_FRAME(X.xrWaitSwapchainImage(s.sc, &wi));
    if (!waited) return false;  // spec: an image may only be released after a successful wait
    if (idx < s.count) Blit(s.rtv[idx], s.w, s.h, srv, scene, IsSrgb(X.swapFormat), warp);
    XrSwapchainImageReleaseInfo ri = {};
    ri.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
    XR_CHECK_FRAME(X.xrReleaseSwapchainImage(s.sc, &ri));
    return true;
}

// ---------------------------------------------------------------------------------------------
// Game frame -> our device
// ---------------------------------------------------------------------------------------------
static void ReleaseContent() {
    for (auto& r : X.ring) SafeRelease(r);
    for (auto& r : X.ringScene) SafeRelease(r);
    for (auto& v : X.views) {
        SafeRelease(v.srv);
        SafeRelease(v.tex);
        SafeRelease(v.sceneSrv);
        SafeRelease(v.sceneTex);
        v.has = false;
        v.seq = 0;
    }
}

static void TypelessFor(DXGI_FORMAT f, DXGI_FORMAT* tex, DXGI_FORMAT* view, bool* linear) {
    *linear = false;
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            *tex = DXGI_FORMAT_R8G8B8A8_TYPELESS;
            *view = DXGI_FORMAT_R8G8B8A8_UNORM;
            return;
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            *tex = DXGI_FORMAT_B8G8R8A8_TYPELESS;
            *view = DXGI_FORMAT_B8G8R8A8_UNORM;
            return;
        case DXGI_FORMAT_B8G8R8X8_UNORM:
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
            *tex = DXGI_FORMAT_B8G8R8X8_TYPELESS;
            *view = DXGI_FORMAT_B8G8R8X8_UNORM;
            return;
        case DXGI_FORMAT_R10G10B10A2_UNORM:
            *tex = DXGI_FORMAT_R10G10B10A2_TYPELESS;
            *view = DXGI_FORMAT_R10G10B10A2_UNORM;
            return;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
            *tex = DXGI_FORMAT_R16G16B16A16_TYPELESS;
            *view = DXGI_FORMAT_R16G16B16A16_FLOAT;
            *linear = true;  // scRGB
            return;
        default:
            *tex = *view = f;
            return;
    }
}

// Answers the game thread's request for capture textures: a ring of kRingSize created here, on our
// device, and shared with the game's device by handle. Called continuously from the VR loop.
static void ServiceShare() {
    EnterCriticalSection(&g_share.cs);
    LONG reqGen = g_share.reqGen;
    UINT w = g_share.reqW, h = g_share.reqH;
    DXGI_FORMAT fmt = g_share.reqFmt;
    LeaveCriticalSection(&g_share.cs);
    if (reqGen == X.servedReqGen || w == 0 || h == 0) return;
    X.servedReqGen = reqGen;  // one attempt per request

    ReleaseContent();
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    d.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    HANDLE handles[2 * kRingSize] = {};
    for (int i = 0; i < 2 * kRingSize; i++) {
        ID3D11Texture2D** t = i < kRingSize ? &X.ring[i] : &X.ringScene[i - kRingSize];
        HRESULT hr = X.dev->CreateTexture2D(&d, nullptr, t);
        IDXGIResource* res = nullptr;
        if (SUCCEEDED(hr)) hr = (*t)->QueryInterface(__uuidof(IDXGIResource), (void**)&res);
        if (SUCCEEDED(hr)) {
            hr = res->GetSharedHandle(&handles[i]);
            res->Release();
        }
        if (FAILED(hr) || !handles[i]) {
            Log("ERROR: capture texture %ux%u format %d could not be created/shared (0x%08x)", w, h, (int)fmt,
                (unsigned)hr);
            ReleaseContent();
            return;
        }
    }

    // Local copies we sample from (left, right, mono), so the ring can be reused.
    DXGI_FORMAT texFmt, viewFmt;
    TypelessFor(fmt, &texFmt, &viewFmt, &X.srcLinear);
    D3D11_TEXTURE2D_DESC ld = d;
    ld.Format = texFmt;
    ld.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ld.MiscFlags = 0;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = viewFmt;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    for (auto& v : X.views) {
        HRESULT hr = X.dev->CreateTexture2D(&ld, nullptr, &v.tex);
        if (SUCCEEDED(hr)) hr = X.dev->CreateShaderResourceView(v.tex, &sd, &v.srv);
        if (SUCCEEDED(hr)) hr = X.dev->CreateTexture2D(&ld, nullptr, &v.sceneTex);
        if (SUCCEEDED(hr)) hr = X.dev->CreateShaderResourceView(v.sceneTex, &sd, &v.sceneSrv);
        if (FAILED(hr)) {
            Log("ERROR: VR-side copy texture %ux%u fmt %d failed 0x%08x", w, h, (int)texFmt, (unsigned)hr);
            ReleaseContent();
            return;
        }
    }
    X.contentW = w;
    X.contentH = h;

    EnterCriticalSection(&g_share.cs);
    memcpy(g_share.ring, handles, sizeof(g_share.ring));
    memcpy(g_share.ringScene, handles + kRingSize, sizeof(g_share.ringScene));
    g_share.w = w;
    g_share.h = h;
    g_share.fmt = fmt;
    g_share.forReqGen = reqGen;
    g_share.gen = ++X.publishGen;
    for (auto& l : g_share.latest) l.seq = 0;
    LeaveCriticalSection(&g_share.cs);
    Log("capture ring published: %d x %ux%u format %d%s", kRingSize, w, h, (int)fmt, X.srcLinear ? " (linear/HDR)" : "");
}

static void SetMenuMode(bool on, float frac);

// Copies newly finished game frames (left / right / mono) into our local textures.
static void UpdateContent() {
    if (!X.ring[0]) return;
    FrameShare::Published latest[3];
    EnterCriticalSection(&g_share.cs);
    bool current = g_share.gen == X.publishGen;
    memcpy(latest, g_share.latest, sizeof(latest));
    LeaveCriticalSection(&g_share.cs);
    if (!current) return;
    for (int k = 0; k < 3; k++) {
        if (!latest[k].seq || latest[k].seq == X.views[k].seq || latest[k].slot < 0 || latest[k].slot >= kRingSize) continue;  // 0.3.27 still said > 3: half the frames lost
        X.ctx->CopyResource(X.views[k].tex, X.ring[latest[k].slot]);
        if (latest[k].tag.hasScene) X.ctx->CopyResource(X.views[k].sceneTex, X.ringScene[latest[k].slot]);
        // Eye frames without a scene-only image (the UI split didn't work on this screen, e.g.
        // during some damage effects): stay in stereo with the HUD inside the 3D image. (0.3.4-0.3.11
        // switched to the flat screen here, which made the view flicker to flat when hit; the title
        // screen that needed it is handled by FlatBelowDraws now.)
        if (k < 2 && g_cfg.uiSplit) {
            if (latest[k].tag.hasScene) {
                X.noSceneRun = 0;
            } else if (++X.noSceneRun == 30) {
                static volatile LONG budget;
                if (LogBudget(&budget, 3)) Log("ui split: no HUD-free image for a moment; HUD shown inside the 3D view");
            }
        }
        X.statSceneFrames += latest[k].tag.hasScene ? 1 : 0;
        X.views[k].seq = latest[k].seq;
        X.views[k].tag = latest[k].tag;
        X.views[k].has = true;
        X.statGameFrames[k]++;
    }
}

static void Blit(ID3D11RenderTargetView* rtv, uint32_t w, uint32_t h, ID3D11ShaderResourceView* srv,
                 ID3D11ShaderResourceView* scene, bool dstSrgb, const WarpParams* warp) {
    int mode = (!X.srcLinear && dstSrgb) ? 1 : ((X.srcLinear && !dstSrgb) ? 2 : 0);
    // key thresholds on the difference between full and scene-only frames (0..1 colour units)
    float p[4] = {(float)mode, 0.004f, 0.03f, 0};
    static float last[4] = {-1, -1, -1, -1};
    if (memcmp(p, last, sizeof(p)) != 0) {
        X.ctx->UpdateSubresource(X.cbuf, 0, nullptr, p, 0, 0);
        memcpy(last, p, sizeof(p));
    }
    D3D11_VIEWPORT vp = {0, 0, (float)w, (float)h, 0, 1};
    X.ctx->OMSetRenderTargets(1, &rtv, nullptr);
    X.ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    X.ctx->OMSetDepthStencilState(nullptr, 0);
    X.ctx->RSSetState(nullptr);
    X.ctx->RSSetViewports(1, &vp);
    X.ctx->IASetInputLayout(nullptr);
    X.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    X.ctx->VSSetShader(X.vs, nullptr, 0);
    if (warp) {
        static_assert(sizeof(WarpParams) == 80, "matches the Warp cbuffer");
        X.ctx->UpdateSubresource(X.cbWarp, 0, nullptr, warp, 0, 0);
        X.ctx->PSSetConstantBuffers(1, 1, &X.cbWarp);
    }
    X.ctx->PSSetShader(warp ? X.psWarp : scene ? X.psKey : X.ps, nullptr, 0);
    ID3D11ShaderResourceView* srvs[2] = {srv, scene};
    X.ctx->PSSetShaderResources(0, 2, srvs);
    X.ctx->PSSetSamplers(0, 1, &X.sampler);
    X.ctx->PSSetConstantBuffers(0, 1, &X.cbuf);
    X.ctx->Draw(3, 0);
    ID3D11ShaderResourceView* nulls[2] = {nullptr, nullptr};
    X.ctx->PSSetShaderResources(0, 2, nulls);
    X.ctx->OMSetRenderTargets(0, nullptr, nullptr);
}

// ---------------------------------------------------------------------------------------------
// Menu detection: how much of the screen is UI? Rendered at 64x36 and read back a few frames
// later without blocking. Mostly-UI frames (pause menu, map, full-screen menus) switch the game
// to flat rendering until the UI is mostly gone again.
// ---------------------------------------------------------------------------------------------
static const int kCovW = 64, kCovH = 36;

static bool EnsureCoverageTargets() {
    if (X.covTex) return true;
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = kCovW;
    d.Height = kCovH;
    d.MipLevels = d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET;
    HRESULT hr = X.dev->CreateTexture2D(&d, nullptr, &X.covTex);
    if (SUCCEEDED(hr)) hr = X.dev->CreateRenderTargetView(X.covTex, nullptr, &X.covRtv);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (int i = 0; i < 3 && SUCCEEDED(hr); i++) hr = X.dev->CreateTexture2D(&d, nullptr, &X.covStaging[i]);
    if (FAILED(hr)) {
        Log("ERROR: UI coverage targets failed 0x%08x", (unsigned)hr);
        SafeRelease(X.covRtv);
        SafeRelease(X.covTex);
        for (auto& t : X.covStaging) SafeRelease(t);
        return false;
    }
    return true;
}

static void SetMenuMode(bool on, float frac) {
    if (on == X.menuMode) return;
    X.menuMode = on;
    AcquireSRWLockExclusive(&g_stereo.lock);
    g_stereo.menuMode = on;
    ReleaseSRWLockExclusive(&g_stereo.lock);
    if (frac >= 0)
        Log("%s (UI covers %.0f%% of the screen)", on ? "menu detected: flat screen" : "menu closed: back to stereo",
            (double)(frac * 100));
}

static void MeasureUICoverage() {
    if (!g_cfg.uiSplit || !X.psKey || !EnsureCoverageTargets()) return;
    for (int i = 0; i < 3; i++) {
        if (!X.covPending[i]) continue;
        D3D11_MAPPED_SUBRESOURCE m;
        if (X.ctx->Map(X.covStaging[i], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) != S_OK) continue;
        int n = 0;
        for (int y = 0; y < kCovH; y++) {
            const uint8_t* row = (const uint8_t*)m.pData + y * m.RowPitch;
            for (int x = 0; x < kCovW; x++) n += row[x * 4 + 3] > 127;
        }
        X.ctx->Unmap(X.covStaging[i], 0);
        X.covPending[i] = false;
        float frac = (float)n / (kCovW * kCovH);
        X.statCovSum += frac;
        X.statCovN++;
        if (frac > X.statCovMax) X.statCovMax = frac;
        // Coverage is now the UI's real opacity (> 50% per sample point), not "differs from the
        // scene", so these thresholds are lower than 0.3.2's 50% / 30%.
        if (frac > 0.30f)
            X.menuVotes = X.menuVotes < 0 ? 1 : X.menuVotes + 1;
        else if (frac < 0.15f)
            X.menuVotes = X.menuVotes > 0 ? -1 : X.menuVotes - 1;
        // Into the flat menu only after ~0.75 s of mostly-UI frames (full-screen damage flashes
        // are shorter), out again quickly.
        if (!X.menuMode && X.menuVotes >= 45) SetMenuMode(true, frac);
        if (X.menuMode && X.menuVotes <= -4) SetMenuMode(false, frac);
    }
    // measure the newest frame that has a scene-only image
    int k = -1;
    for (int i = 0; i < 3; i++)
        if (X.views[i].has && X.views[i].tag.hasScene && (k < 0 || X.views[i].seq > X.views[k].seq)) k = i;
    if (k < 0 || X.views[k].seq == X.covMeasuredSeq || X.covPending[X.covNext]) return;
    Blit(X.covRtv, kCovW, kCovH, X.views[k].srv, X.views[k].sceneSrv, false);
    X.ctx->CopyResource(X.covStaging[X.covNext], X.covTex);
    X.covPending[X.covNext] = true;
    X.covNext = (X.covNext + 1) % 3;
    X.covMeasuredSeq = X.views[k].seq;
}

// ---------------------------------------------------------------------------------------------
// Screen placement & hotkeys
// ---------------------------------------------------------------------------------------------
static bool GameIsForeground() {
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

static bool KeyPressed(int vk) {
    if (vk <= 0 || vk > 255) return false;
    bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool pressed = down && !X.keyDown[vk];
    X.keyDown[vk] = down;
    return pressed;
}

static void HandleHotkeys() {
    bool fg = GameIsForeground();
    bool changed = false;
    // Always poll so edge detection stays correct, only act while the game has focus.
    bool rc = KeyPressed(g_cfg.keyRecenter), sm = KeyPressed(g_cfg.keySmaller), lg = KeyPressed(g_cfg.keyLarger),
         cl = KeyPressed(g_cfg.keyCloser), fa = KeyPressed(g_cfg.keyFarther), st = KeyPressed(g_cfg.keyStereo),
         f11 = KeyPressed(VK_F11), f5 = KeyPressed(VK_F5);
    if (!fg) return;
    if (f5) {
        bool c = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0, s = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        if (c && s) CullingToggleHeadCulling();  // Ctrl+Shift+F5: head-direction culling on/off
        else if (c) CullingWatch();               // Ctrl+F5: diagnostic watch
        else if (s) CullingCycleTest();           // Shift+F5: wide-fov test steps
        else {                                    // F5: both eyes every frame on/off
            g_cfg.bothEyes = !g_cfg.bothEyes;
            Log("both eyes every frame %s", !g_cfg.bothEyes ? "OFF (alternate eyes)"
                                            : ReplayAvailable() ? "ON (the game's frame is drawn again for the right eye)"
                                                                : "ON, but not available (see the log above)");
            ConfigSaveStereo();
        }
    }
    if (f11 && (GetAsyncKeyState(VK_CONTROL) & 0x8000)) {
        if (GetAsyncKeyState(VK_SHIFT) & 0x8000) {
            // Ctrl+Shift+F11: shadows on/off (test switch)
            g_cfg.shadows = !g_cfg.shadows;
            Log("shadows %s", g_cfg.shadows ? "ON" : "OFF");
        } else {
            // Ctrl+F11: NieR's ambient occlusion on/off (half-resolution SAO looks blotchy up close)
            g_cfg.ambientOcclusion = !g_cfg.ambientOcclusion;
            Log("ambient occlusion %s", g_cfg.ambientOcclusion ? "ON" : "OFF");
        }
        ConfigSaveStereo();
    }
    if (st && (GetAsyncKeyState(VK_SHIFT) & 0x8000)) {
        // Shift+F12: TopDownFlat on/off (steep top-down views on the flat screen)
        g_cfg.topDownFlat = !g_cfg.topDownFlat;
        Log("top-down views: %s", g_cfg.topDownFlat ? "flat screen (TopDownFlat=1)" : "stereo (TopDownFlat=0)");
        ConfigSaveStereo();
        st = false;
    }
    if (st && (GetAsyncKeyState(VK_CONTROL) & 0x8000)) {
        // Ctrl+F12: diagnostic single-eye mode (the left eye's image every frame, shown to both
        // eyes: no 3D, but also no alternating between eyes)
        AcquireSRWLockExclusive(&g_stereo.lock);
        g_stereo.singleEye = !g_stereo.singleEye;
        bool on = g_stereo.singleEye;
        ReleaseSRWLockExclusive(&g_stereo.lock);
        Log("single-eye test %s", on ? "ON (left eye every frame, no 3D)" : "OFF");
        st = false;
    }
    if (st) {
        AcquireSRWLockExclusive(&g_stereo.lock);
        g_stereo.enabled = !g_stereo.enabled;
        bool on = g_stereo.enabled;
        ReleaseSRWLockExclusive(&g_stereo.lock);
        Log("stereo %s", on ? "ON" : "OFF (flat screen)");
    }
    if (rc) {
        X.recenterRequested = true;
        Log("recenter requested");
    }
    bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    if (ctrl && (sm || lg)) {
        // Ctrl+F9: world bigger (less eye separation per game unit), Ctrl+F10: world smaller
        // (10% per press: 0.3.1/0.3.2 used 2%, too small a step to see)
        float f = sm ? 1 / 1.1f : 1.1f;
        float v = g_cfg.worldScale * f;
        g_cfg.worldScale = v < 0.002f ? 0.002f : (v > 50 ? 50 : v);
        Log("world scale %.3f (the world now looks %.2fx its true size)", (double)g_cfg.worldScale,
            (double)(1 / g_cfg.worldScale));
        ConfigSaveStereo();
        sm = lg = false;
    }
    if (ctrl && (cl || fa)) {
        // Ctrl+F6: camera closer to the character, Ctrl+F7: farther
        float v = g_cfg.cameraDolly + (cl ? 0.25f : -0.25f);
        g_cfg.cameraDolly = v < -5 ? -5 : (v > 5 ? 5 : v);
        Log("camera dolly %.2f m", (double)g_cfg.cameraDolly);
        ConfigSaveStereo();
        cl = fa = false;
    }
    if (sm) { g_cfg.width = g_cfg.width / 1.1f < 0.2f ? 0.2f : g_cfg.width / 1.1f; changed = true; }
    if (lg) { g_cfg.width = g_cfg.width * 1.1f > 100.f ? 100.f : g_cfg.width * 1.1f; changed = true; }
    if (cl) { g_cfg.distance = g_cfg.distance - 0.25f < 0.3f ? 0.3f : g_cfg.distance - 0.25f; changed = true; X.recenterRequested = true; }
    if (fa) { g_cfg.distance = g_cfg.distance + 0.25f > 50.f ? 50.f : g_cfg.distance + 0.25f; changed = true; X.recenterRequested = true; }
    if (changed) {
        Log("screen: distance %.2f m, width %.2f m", (double)g_cfg.distance, (double)g_cfg.width);
        ConfigSaveScreen();
    }
}

// The runtime may change its tracking space without the REFERENCE_SPACE_CHANGE_PENDING event the
// spec asks for (0.5.2 log, Virtual Desktop through SteamVR: the space went from upside down to normal
// after a dashboard visit; the old correction then turned the picture upside down until a manual
// recenter). Each headset frame: a head whose up points against the reference's up for a third of a
// second, or a head position jump of more than 0.75 m between two frames, re-levels everything.
static void WatchSpace() {
    if (!X.placed || !X.dispValid) {
        X.lastHeadValid = false;
        return;
    }
    const XrPosef& e0 = X.disp[0].pose;
    const XrPosef& e1 = X.disp[1].pose;
    float head[3] = {(e0.position.x + e1.position.x) * 0.5f, (e0.position.y + e1.position.y) * 0.5f,
                     (e0.position.z + e1.position.z) * 0.5f};
    pure::V3 up = pure::rotate({e0.orientation.x, e0.orientation.y, e0.orientation.z, e0.orientation.w}, {0, 1, 0});
    float along = X.spaceFlipped ? -up.y : up.y;  // head up against the reference's up
    X.upsideFrames = along < -0.5f ? X.upsideFrames + 1 : 0;
    bool jump = false;
    if (X.lastHeadValid) {
        float dx = head[0] - X.lastHead[0], dy = head[1] - X.lastHead[1], dz = head[2] - X.lastHead[2];
        jump = dx * dx + dy * dy + dz * dz > 0.75f * 0.75f;
    }
    memcpy(X.lastHead, head, sizeof(head));
    X.lastHeadValid = true;
    if (X.upsideFrames >= 30 || jump) {
        Log("tracking space changed without notice (%s); re-levelling the view and the screen",
            jump ? "the head jumped more than 0.75 m in one frame" : "the head's up now points the other way");
        X.upsideFrames = 0;
        X.recenterRequested = true;
    }
}

static void PlaceScreen(XrTime time) {
    XrSpaceLocation loc = {};
    loc.type = XR_TYPE_SPACE_LOCATION;
    if (XR_FAILED(X.xrLocateSpace(X.view, X.local, time, &loc))) return;
    const XrSpaceLocationFlags need = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    if ((loc.locationFlags & need) != need) return;
    pure::Q head{loc.pose.orientation.x, loc.pose.orientation.y, loc.pose.orientation.z, loc.pose.orientation.w};
    pure::Q yaw;  // level reference: heading only, upright for the player (pure::level_reference)
    bool wasFlipped = X.spaceFlipped;
    if (!pure::level_reference(head, &X.spaceFlipped, &yaw)) yaw = X.placed ? X.lastYaw : pure::Q{0, 0, 0, 1};
    if (X.spaceFlipped != wasFlipped || (!X.placed && X.spaceFlipped))
        Log(X.spaceFlipped ? "tracking space: the runtime's up axis points down (its +Y is the floor); the view, screen "
                             "and menu are turned upright for you"
                           : "tracking space: normal (+Y up)");
    pure::V3 off = pure::rotate(yaw, {0, g_cfg.heightOffset, -g_cfg.distance});
    X.quadPose.orientation = {yaw.x, yaw.y, yaw.z, yaw.w};
    X.quadPose.position = {loc.pose.position.x + off.x, loc.pose.position.y + off.y, loc.pose.position.z + off.z};
    X.lastYaw = yaw;
    X.placed = true;
    X.recenterRequested = false;
    // The stereo camera treats this (yaw-only) head pose as "looking along the game camera".
    AcquireSRWLockExclusive(&g_stereo.lock);
    g_stereo.refPos[0] = loc.pose.position.x;
    g_stereo.refPos[1] = loc.pose.position.y;
    g_stereo.refPos[2] = loc.pose.position.z;
    g_stereo.refRot[0] = yaw.x;
    g_stereo.refRot[1] = yaw.y;
    g_stereo.refRot[2] = yaw.z;
    g_stereo.refRot[3] = yaw.w;
    g_stereo.refValid = true;
    ReleaseSRWLockExclusive(&g_stereo.lock);
    pure::V3 hu = pure::rotate(head, {0, 1, 0});
    Log("screen placed at (%.2f, %.2f, %.2f), %.2f m ahead; head up vector (%.2f, %.2f, %.2f)%s",
        (double)X.quadPose.position.x, (double)X.quadPose.position.y, (double)X.quadPose.position.z,
        (double)g_cfg.distance, (double)hu.x, (double)hu.y, (double)hu.z, X.spaceFlipped ? " (space flipped)" : "");
}

// ---------------------------------------------------------------------------------------------
// Events & frame loop
// ---------------------------------------------------------------------------------------------
static const char* StateName(XrSessionState s) {
    switch (s) {
        case XR_SESSION_STATE_IDLE: return "IDLE";
        case XR_SESSION_STATE_READY: return "READY";
        case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
        case XR_SESSION_STATE_VISIBLE: return "VISIBLE";
        case XR_SESSION_STATE_FOCUSED: return "FOCUSED";
        case XR_SESSION_STATE_STOPPING: return "STOPPING";
        case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
        case XR_SESSION_STATE_EXITING: return "EXITING";
        default: return "UNKNOWN";
    }
}

static void PollEvents() {
    XrEventDataBuffer ev;
    for (;;) {
        // the runtime overwrites the type with the event's own: it must be reset before every call
        // (0.5.0; earlier versions passed the previous event's type back in)
        ev = {};
        ev.type = XR_TYPE_EVENT_DATA_BUFFER;
        if (X.xrPollEvent(X.instance, &ev) != XR_SUCCESS) break;
        switch (ev.type) {
            case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
                const XrEventDataSessionStateChanged* e = (const XrEventDataSessionStateChanged*)&ev;
                X.state = e->state;
                Log("session state -> %s", StateName(e->state));
                if (e->state == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo bi = {};
                    bi.type = XR_TYPE_SESSION_BEGIN_INFO;
                    bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    if (XR_CHECK(X.xrBeginSession(X.session, &bi))) X.running = true;
                } else if (e->state == XR_SESSION_STATE_STOPPING) {
                    XR_CHECK(X.xrEndSession(X.session));
                    X.running = false;
                } else if (e->state == XR_SESSION_STATE_EXITING) {
                    Log("the VR runtime closed our session; restart the game to re-enter VR");
                    X.exitRequested = true;
                    X.sessionLost = true;
                } else if (e->state == XR_SESSION_STATE_LOSS_PENDING) {
                    X.sessionLost = true;
                }
                break;
            }
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                Log("OpenXR instance loss pending (runtime shutting down?)");
                X.instanceLost = true;
                break;
            case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING: {
                const XrEventDataReferenceSpaceChangePending* e = (const XrEventDataReferenceSpaceChangePending*)&ev;
                if (e->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL) {
                    Log("runtime recentered the view; re-placing the screen");
                    X.recenterRequested = true;
                }
                break;
            }
            case XR_TYPE_EVENT_DATA_EVENTS_LOST:
                Log("OpenXR: events lost");
                VrInputProfileChanged();  // one of them may have been a controller change
                break;
            case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
                VrInputProfileChanged();
                break;
            default:
                break;
        }
        ev = {};
        ev.type = XR_TYPE_EVENT_DATA_BUFFER;
    }
}

static void LogStats() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double secs = (double)(now.QuadPart - X.statStart.QuadPart) / (double)X.qpcFreq.QuadPart;
    if (secs < 10) return;
    unsigned got = X.statGameFrames[0] + X.statGameFrames[1] + X.statGameFrames[2];
    Log("stats: headset %.1f fps (stereo %.1f, flat %.1f); game frames received: left %.1f, right %.1f, mono %.1f fps; "
        "HUD-free image in %.0f%% of them; UI coverage avg %.0f%% max %.0f%%",
        X.statFrames / secs, X.statStereoFrames / secs, X.statQuadFrames / secs, X.statGameFrames[0] / secs,
        X.statGameFrames[1] / secs, X.statGameFrames[2] / secs, got ? 100.0 * X.statSceneFrames / got : 0.0,
        X.statCovN ? 100.0 * X.statCovSum / X.statCovN : 0.0, 100.0 * X.statCovMax);
    {
        char info[160];
        pure::format(info, sizeof(info), "Mode: %s", !X.statStereoFrames ? "flat screen"
                                                     : g_cfg.bothEyes ? "stereo, both eyes every frame"
                                                                      : "stereo, alternate eyes");
        MenuSetInfo(0, info);
        pure::format(info, sizeof(info), "Headset %.0f fps; game images per second: left %.0f, right %.0f, flat %.0f",
                     X.statFrames / secs, X.statGameFrames[0] / secs, X.statGameFrames[1] / secs, X.statGameFrames[2] / secs);
        MenuSetInfo(1, info);
    }
    if (X.statLagN || X.statTurnN) {
        Log("stats: head pose: game images were on screen on average %.1f ms after the time their pose was predicted "
            "from (prediction now %.1f ms ahead, PosePrediction=%d); the head had turned on average %.2f deg (max %.1f) "
            "from the shown image's pose, beyond the rendered margin of %.1f deg in %.1f%% of headset frames",
            X.statLagN ? X.statLagSum / X.statLagN : 0.0, (double)X.predictNs / 1e6, g_cfg.posePrediction,
            X.statTurnN ? X.statTurnSum / X.statTurnN : 0.0, (double)X.statTurnMax, (double)X.marginDeg,
            X.statTurnN ? 100.0 * X.statTurnOver / X.statTurnN : 0.0);
        char info[160];
        pure::format(info, sizeof(info), "Head: images shown %.0f ms after their pose; predicting %.0f ms; past margin %.1f%%",
                     X.statLagN ? X.statLagSum / X.statLagN : 0.0, (double)X.predictNs / 1e6,
                     X.statTurnN ? 100.0 * X.statTurnOver / X.statTurnN : 0.0);
        MenuSetInfo(13, info);
    }
    static UINT resLoggedW, resLoggedH;
    if (X.resPct > 0 && X.statStereoFrames && (resLoggedW != X.contentW || resLoggedH != X.contentH)) {
        resLoggedW = X.contentW;
        resLoggedH = X.contentH;
        Log("stats: resolution: the game renders %ux%u: %.0f%% of the sharpness the headset recommends across, %.0f%% "
            "up-down; about %ux%u matches it in both directions",
            X.contentW, X.contentH, (double)X.resPct, (double)X.resPctH, X.resSuggestW, X.resSuggestH);
        char info[160];
        pure::format(info, sizeof(info), "Resolution %ux%u: %.0f%% / %.0f%% of recommended (across / up-down); ~%ux%u matches",
                     X.contentW, X.contentH, (double)X.resPct, (double)X.resPctH, X.resSuggestW, X.resSuggestH);
        MenuSetInfo(14, info);
    }
    X.statLagSum = X.statTurnSum = 0;
    X.statLagN = X.statTurnN = X.statTurnOver = 0;
    X.statTurnMax = 0;
    X.statSceneFrames = 0;
    X.statCovSum = X.statCovMax = 0;
    X.statCovN = 0;
    X.statFrames = X.statQuadFrames = X.statStereoFrames = 0;
    X.statGameFrames[0] = X.statGameFrames[1] = X.statGameFrames[2] = 0;
    X.statStart = now;
}

// Hands the latest predicted eye poses and fields of view to the game thread.
static void UpdateEyePoses(XrTime time) {
    XrViewLocateInfo li = {};
    li.type = XR_TYPE_VIEW_LOCATE_INFO;
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.space = X.local;
    // where the eyes are when this VR frame is shown (diagnostic: how far the shown image is off)
    {
        li.displayTime = time;
        XrViewState ds = {};
        ds.type = XR_TYPE_VIEW_STATE;
        XrView dv[2] = {};
        dv[0].type = dv[1].type = XR_TYPE_VIEW;
        uint32_t dn = 0;
        X.dispValid = XR_SUCCEEDED(X.xrLocateViews(X.session, &li, &ds, 2, &dn, dv)) && dn == 2 &&
                      (ds.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);
        if (X.dispValid && (ds.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT)) {
            X.disp[0] = dv[0];
            X.disp[1] = dv[1];
            X.dispHave = true;
        }
        if (X.dispValid)
            for (int e = 0; e < 2; e++) {
                X.dispRot[e][0] = dv[e].pose.orientation.x;
                X.dispRot[e][1] = dv[e].pose.orientation.y;
                X.dispRot[e][2] = dv[e].pose.orientation.z;
                X.dispRot[e][3] = dv[e].pose.orientation.w;
            }
    }
    // the game renders with the pose predicted for when its frame will really be shown
    X.predictNs = g_cfg.posePrediction && X.lagHave ? (long long)(X.lagMs * 1e6) : 0;
    li.displayTime = time + X.predictNs;
    li.space = X.local;
    XrViewState vs = {};
    vs.type = XR_TYPE_VIEW_STATE;
    XrView v[2] = {};
    v[0].type = v[1].type = XR_TYPE_VIEW;
    uint32_t n = 0;
    const XrViewStateFlags need = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
    bool ok = XR_SUCCEEDED(X.xrLocateViews(X.session, &li, &vs, 2, &n, v)) && n == 2 && (vs.viewStateFlags & need) == need;
    AcquireSRWLockExclusive(&g_stereo.lock);
    g_stereo.valid = ok;
    if (ok) g_stereo.poseBase = time;
    if (ok) {
        for (int e = 0; e < 2; e++) {
            g_stereo.eyePos[e][0] = v[e].pose.position.x;
            g_stereo.eyePos[e][1] = v[e].pose.position.y;
            g_stereo.eyePos[e][2] = v[e].pose.position.z;
            g_stereo.eyeRot[e][0] = v[e].pose.orientation.x;
            g_stereo.eyeRot[e][1] = v[e].pose.orientation.y;
            g_stereo.eyeRot[e][2] = v[e].pose.orientation.z;
            g_stereo.eyeRot[e][3] = v[e].pose.orientation.w;
            g_stereo.eyeTan[e][0] = pure::ftan(v[e].fov.angleLeft);
            g_stereo.eyeTan[e][1] = pure::ftan(v[e].fov.angleRight);
            g_stereo.eyeTan[e][2] = pure::ftan(v[e].fov.angleUp);
            g_stereo.eyeTan[e][3] = pure::ftan(v[e].fov.angleDown);
        }
    }
    ReleaseSRWLockExclusive(&g_stereo.lock);
    static bool logged = false;
    if (ok && !logged) {
        logged = true;
        for (int e = 0; e < 2; e++)
            Log("eye %d: fov left %.1f right %.1f up %.1f down %.1f deg, position (%.4f, %.4f, %.4f)", e,
                (double)(v[e].fov.angleLeft * 57.29578f), (double)(v[e].fov.angleRight * 57.29578f),
                (double)(v[e].fov.angleUp * 57.29578f), (double)(v[e].fov.angleDown * 57.29578f),
                (double)v[e].pose.position.x, (double)v[e].pose.position.y, (double)v[e].pose.position.z);
        float dx = v[1].pose.position.x - v[0].pose.position.x, dy = v[1].pose.position.y - v[0].pose.position.y,
              dz = v[1].pose.position.z - v[0].pose.position.z;
        Log("IPD %.1f mm", (double)(pure::fsqrt(dx * dx + dy * dy + dz * dz) * 1000));
    }
}

static XrFovf FovFromTag(const FrameTag& t) {
    XrFovf f;
    f.angleLeft = -pure::fatan(t.tanHalfW);
    f.angleRight = pure::fatan(t.tanHalfW);
    f.angleUp = pure::fatan(t.tanHalfH);
    f.angleDown = -pure::fatan(t.tanHalfH);
    return f;
}

static bool UpdateMenu(XrTime time);

static void Frame() {
    XrFrameState fs = {};
    fs.type = XR_TYPE_FRAME_STATE;
    XrFrameWaitInfo wi = {};
    wi.type = XR_TYPE_FRAME_WAIT_INFO;
    if (!XR_CHECK_FRAME(X.xrWaitFrame(X.session, &wi, &fs))) {
        Sleep(5);
        return;
    }
    XrFrameBeginInfo bi = {};
    bi.type = XR_TYPE_FRAME_BEGIN_INFO;
    if (!XR_CHECK_FRAME(X.xrBeginFrame(X.session, &bi))) return;

    const XrCompositionLayerBaseHeader* layers[6];
    uint32_t layerCount = 0;
    XrCompositionLayerQuad quad = {}, hud = {}, menu = {};
    XrCompositionLayerProjection proj = {};
    XrCompositionLayerProjectionView pv[2] = {};

    HandleHotkeys();
    VrInputSync(X.state == XR_SESSION_STATE_FOCUSED, GameIsForeground());
    MenuTick(GameIsForeground());
    if (MenuTakeRecenter()) X.recenterRequested = true;
    UpdateEyePoses(fs.predictedDisplayTime);
    WatchSpace();
    if (fs.shouldRender) {
        if (!X.placed || X.recenterRequested) PlaceScreen(fs.predictedDisplayTime);
        UpdateContent();
        MeasureUICoverage();
        AcquireSRWLockShared(&g_stereo.lock);
        bool stereoOn = g_stereo.enabled;
        ReleaseSRWLockShared(&g_stereo.lock);
        auto& L = X.views[0];
        AcquireSRWLockShared(&g_stereo.lock);
        bool single = g_stereo.singleEye;
        ReleaseSRWLockShared(&g_stereo.lock);
        auto& R = single ? X.views[0] : X.views[1];  // single-eye test: the left image for both eyes
        auto& M = X.views[2];
        // Stereo when both eyes have rendered recently and are newer than any flat frame.
        bool stereo = stereoOn && !X.menuMode && L.has && R.has && L.tag.patched && R.tag.patched && L.seq > M.seq && R.seq > M.seq;
        // Late warp (0.5.1, default): both eyes are submitted with the headset's own pose pair and field
        // of view for this frame, each image re-aimed to it here. Runtimes that stream to the headset
        // (Pico Connect, Steam Link, ...) assume one head pose per frame and images that cover exactly
        // the eye's field of view: 0.5.0 sent each eye with its render pose (different moments in
        // alternate-eye mode: the right eye stuttered) and a symmetric, wider field of view (Quest 3,
        // 54/40 deg per eye: squeezed sideways, cross-eyed).
        bool late = stereo && g_cfg.lateWarp && X.psWarp && X.dispHave;
        uint32_t ew[2] = {X.contentW, X.contentW}, eh[2] = {X.contentH, X.contentH};
        if (late)
            for (int e = 0; e < 2; e++) {
                // same pixel density as the rendered image, over the eye's own field of view
                const FrameTag& t = (e == 0 ? L : R).tag;
                const XrFovf& f = X.disp[e].fov;
                float w = (pure::ftan(f.angleRight) - pure::ftan(f.angleLeft)) * X.contentW / (2 * t.tanHalfW);
                float h = (pure::ftan(f.angleUp) - pure::ftan(f.angleDown)) * X.contentH / (2 * t.tanHalfH);
                ew[e] = ((uint32_t)w + 1) & ~1u;
                eh[e] = ((uint32_t)h + 1) & ~1u;
                // a few pixels' change doesn't need a new swapchain
                if (X.eyeSwap[e].sc && ew[e] + 16 > X.eyeSwap[e].w && ew[e] < X.eyeSwap[e].w + 16 && eh[e] + 16 > X.eyeSwap[e].h &&
                    eh[e] < X.eyeSwap[e].h + 16) {
                    ew[e] = X.eyeSwap[e].w;
                    eh[e] = X.eyeSwap[e].h;
                }
            }
        stereo = stereo && EnsureSwap(X.eyeSwap[0], ew[0], eh[0], "left eye") && EnsureSwap(X.eyeSwap[1], ew[1], eh[1], "right eye");
        if (stereo) {
            bool ok = true;
            bool bothScene = L.tag.hasScene && R.tag.hasScene;
            for (int e = 0; e < 2 && ok; e++) {
                auto& V = e == 0 ? L : R;
                const FrameTag& t = V.tag;
                // the 3D scene without HUD/menus when both eyes have one (never mix the two kinds:
                // that would show the UI in one eye and on the HUD layer at the same time)
                ID3D11ShaderResourceView* img = bothScene ? V.sceneSrv : V.srv;
                pv[e].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                if (late) {
                    const XrView& d = X.disp[e];
                    WarpParams w;
                    w.outTan[0] = pure::ftan(d.fov.angleLeft);
                    w.outTan[1] = pure::ftan(d.fov.angleRight);
                    w.outTan[2] = pure::ftan(d.fov.angleUp);
                    w.outTan[3] = pure::ftan(d.fov.angleDown);
                    w.srcTan[0] = t.tanHalfW;
                    w.srcTan[1] = t.tanHalfH;
                    w.srcTan[2] = w.srcTan[3] = 0;
                    pure::M4 rr = pure::m4_from_pose({t.rot[0], t.rot[1], t.rot[2], t.rot[3]}, {0, 0, 0});
                    pure::M4 rd = pure::m4_from_pose({d.pose.orientation.x, d.pose.orientation.y, d.pose.orientation.z,
                                                      d.pose.orientation.w}, {0, 0, 0});
                    for (int r = 0; r < 3; r++) {
                        for (int c = 0; c < 3; c++)
                            w.m[r][c] = rr.at(0, r) * rd.at(0, c) + rr.at(1, r) * rd.at(1, c) + rr.at(2, r) * rd.at(2, c);
                        w.m[r][3] = 0;
                    }
                    ok = RenderInto(X.eyeSwap[e], img, nullptr, &w);
                    pv[e].pose = d.pose;
                    pv[e].fov = d.fov;
                } else {
                    ok = RenderInto(X.eyeSwap[e], img);
                    pv[e].pose.position = {t.pos[0], t.pos[1], t.pos[2]};
                    pv[e].pose.orientation = {t.rot[0], t.rot[1], t.rot[2], t.rot[3]};
                    pv[e].fov = FovFromTag(t);
                }
                pv[e].subImage.swapchain = X.eyeSwap[e].sc;
                pv[e].subImage.imageRect.offset = {0, 0};
                pv[e].subImage.imageRect.extent = {(int32_t)X.eyeSwap[e].w, (int32_t)X.eyeSwap[e].h};
            }
            // resolution hint: the game picture's pixel density against the headset's recommended one
            if (X.recW && X.dispHave) {
                const XrFovf& f = X.disp[0].fov;
                float span = pure::ftan(f.angleRight) - pure::ftan(f.angleLeft);
                float need = X.recW / span * 2 * L.tag.tanHalfW;  // game width with the recommended density
                float spanH = pure::ftan(f.angleUp) - pure::ftan(f.angleDown);
                float needH = X.recH / spanH * 2 * L.tag.tanHalfH;
                if (need > 64 && needH > 64) {
                    // the 16:9 picture is stretched over the eye's field of view (Anamorphic): the two
                    // directions can differ a lot (0.5.1 Quest log: 144% across, 79% up-down)
                    X.resPct = 100.f * X.contentW / need;
                    X.resPctH = 100.f * X.contentH / needH;
                    // keep the game's 16:9 shape: the larger of the two needs decides
                    float scale = need / X.contentW > needH / X.contentH ? need / X.contentW : needH / X.contentH;
                    X.resSuggestW = ((UINT)(X.contentW * scale) + 4) & ~7u;
                    X.resSuggestH = ((UINT)(X.contentH * scale) + 4) & ~7u;
                }
            }
            static int lateLogged = -1;
            if (ok && lateLogged != (int)late) {
                lateLogged = (int)late;
                if (late)
                    Log("late warp: eyes submitted with the headset's pose and field of view (left %.1f/%.1f, up %.1f/%.1f "
                        "deg), %ux%u per eye from a %ux%u image rendered %.1f x %.1f deg",
                        (double)(X.disp[0].fov.angleLeft * 57.29578f), (double)(X.disp[0].fov.angleRight * 57.29578f),
                        (double)(X.disp[0].fov.angleUp * 57.29578f), (double)(X.disp[0].fov.angleDown * 57.29578f),
                        X.eyeSwap[0].w, X.eyeSwap[0].h, X.contentW, X.contentH,
                        (double)(2 * pure::fatan(L.tag.tanHalfW) * 57.29578f), (double)(2 * pure::fatan(L.tag.tanHalfH) * 57.29578f));
                else
                    Log("late warp off: eyes submitted with the pose and field of view they were rendered with");
            }
            // head pose prediction: how long after the pose's base time the images are on screen
            for (int e = 0; e < 2 && ok; e++) {
                const FrameTag& t = (e == 0 ? L : R).tag;
                if (!t.poseBase) continue;
                double ms = (double)(fs.predictedDisplayTime - t.poseBase) / 1e6;
                if (ms <= 0 || ms > 250) continue;
                X.lagMs = X.lagHave ? X.lagMs + (ms - X.lagMs) * 0.01 : ms;
                if (X.lagMs > 120) X.lagMs = 120;  // runtimes extrapolate the head only so far
                X.lagHave = true;
                X.statLagSum += ms;
                X.statLagN++;
            }
            // how far the head has turned from the shown image's pose (the black edge, if beyond the margin)
            if (ok && X.dispValid) {
                const float* r = L.tag.rot;
                float deg = pure::q_angle_deg({r[0], r[1], r[2], r[3]},
                                              {X.dispRot[0][0], X.dispRot[0][1], X.dispRot[0][2], X.dispRot[0][3]});
                X.statTurnSum += deg;
                X.statTurnN++;
                if (deg > X.statTurnMax) X.statTurnMax = deg;
                // margin: rendered half-width beyond the headset's own field of view (horizontal)
                AcquireSRWLockShared(&g_stereo.lock);
                float own = -g_stereo.eyeTan[0][0] > g_stereo.eyeTan[0][1] ? -g_stereo.eyeTan[0][0] : g_stereo.eyeTan[0][1];
                ReleaseSRWLockShared(&g_stereo.lock);
                X.marginDeg = (pure::fatan(L.tag.tanHalfW) - pure::fatan(own)) * 57.29578f;
                if (deg > X.marginDeg) X.statTurnOver++;
            }
            if (ok) {
                proj.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
                proj.space = X.local;
                proj.viewCount = 2;
                proj.views = pv;
                layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&proj;
                X.statStereoFrames++;
                // HUD: keyed out of the newest eye frame, on a transparent flat layer in front.
                // (BothEyes: the right eye is replayed without UI, so the HUD comes from the left)
                auto& H = (L.seq > R.seq && !L.tag.noUI) || R.tag.noUI ? L : R;
                if (bothScene && X.placed && EnsureSwap(X.hudSwap, X.contentW, X.contentH, "HUD") &&
                    RenderInto(X.hudSwap, H.srv, H.sceneSrv)) {
                    hud.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
                    hud.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                    hud.space = X.local;
                    hud.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                    hud.subImage.swapchain = X.hudSwap.sc;
                    hud.subImage.imageRect.offset = {0, 0};
                    hud.subImage.imageRect.extent = {(int32_t)X.hudSwap.w, (int32_t)X.hudSwap.h};
                    hud.pose = X.quadPose;
                    hud.size = {g_cfg.width, g_cfg.width * (float)X.contentH / (float)X.contentW};
                    layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&hud;
                }
            }
        } else {
            // Flat screen: the most recent frame of any kind.
            int k = -1;
            for (int i = 0; i < 3; i++)
                if (X.views[i].has && (k < 0 || X.views[i].seq > X.views[k].seq)) k = i;
            if (X.placed && k >= 0 && EnsureSwap(X.quadSwap, X.contentW, X.contentH, "screen") &&
                RenderInto(X.quadSwap, X.views[k].srv)) {
                quad.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
                quad.space = X.local;
                quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                quad.subImage.swapchain = X.quadSwap.sc;
                quad.subImage.imageRect.offset = {0, 0};
                quad.subImage.imageRect.extent = {(int32_t)X.quadSwap.w, (int32_t)X.quadSwap.h};
                quad.pose = X.quadPose;
                quad.size = {g_cfg.width, g_cfg.width * (float)X.contentH / (float)X.contentW};
                layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&quad;
                X.statQuadFrames++;
            }
        }
    }

    // the settings menu, on top of everything
    if (fs.shouldRender && MenuIsOpen() && UpdateMenu(fs.predictedDisplayTime)) {
        menu.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
        menu.space = X.local;
        menu.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        menu.subImage.swapchain = X.menuSwap.sc;
        menu.subImage.imageRect.offset = {0, 0};
        menu.subImage.imageRect.extent = {(int32_t)X.menuSwap.w, (int32_t)X.menuSwap.h};
        menu.pose = X.menuPose;
        menu.size = {1.0f, 1.0f * (float)X.menuSwap.h / (float)X.menuSwap.w};
        layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&menu;
    }

    XrFrameEndInfo ei = {};
    ei.type = XR_TYPE_FRAME_END_INFO;
    ei.displayTime = fs.predictedDisplayTime;
    ei.environmentBlendMode = X.blend;
    ei.layerCount = layerCount;
    ei.layers = layerCount ? layers : nullptr;
    XR_CHECK_FRAME(X.xrEndFrame(X.session, &ei));
    X.statFrames++;
    LogStats();
}

// The settings menu: placed 1.1 m in front of your head when it opens, redrawn when it changes.
static bool UpdateMenu(XrTime time) {
    if (MenuTakeJustOpened()) {
        XrSpaceLocation loc = {};
        loc.type = XR_TYPE_SPACE_LOCATION;
        if (XR_SUCCEEDED(X.xrLocateSpace(X.view, X.local, time, &loc)) &&
            (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
            pure::Q head{loc.pose.orientation.x, loc.pose.orientation.y, loc.pose.orientation.z, loc.pose.orientation.w};
            pure::Q yaw;
            bool flipped = X.spaceFlipped;
            if (!pure::level_reference(head, &flipped, &yaw)) yaw = X.lastYaw;
            pure::V3 off = pure::rotate(yaw, {0, -0.12f, -1.1f});
            X.menuPose.orientation = {yaw.x, yaw.y, yaw.z, yaw.w};
            X.menuPose.position = {loc.pose.position.x + off.x, loc.pose.position.y + off.y, loc.pose.position.z + off.z};
        }
    }
    int w = 0, h = 0;
    bool bgra = X.swapFormat == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || X.swapFormat == DXGI_FORMAT_B8G8R8A8_UNORM;
    const uint32_t* px = MenuImage(bgra, !X.menuShown, &w, &h);
    if (!EnsureSwap(X.menuSwap, (uint32_t)w, (uint32_t)h, "menu", true)) return false;
    if (X.menuSwap.w != (uint32_t)w || X.menuSwap.h != (uint32_t)h) return false;  // runtime limits: not supported
    if (!X.menuTex) {
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = (UINT)w;
        d.Height = (UINT)h;
        d.MipLevels = d.ArraySize = 1;
        d.Format = (DXGI_FORMAT)X.swapFormat;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        if (FAILED(X.dev->CreateTexture2D(&d, nullptr, &X.menuTex))) return false;
        X.menuShown = false;
    }
    if (!px) return X.menuShown;  // unchanged: the last image stays
    X.ctx->UpdateSubresource(X.menuTex, 0, nullptr, px, (UINT)w * 4, 0);
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai = {};
    ai.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
    if (!XR_CHECK_FRAME(X.xrAcquireSwapchainImage(X.menuSwap.sc, &ai, &idx))) return X.menuShown;
    XrSwapchainImageWaitInfo wi = {};
    wi.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
    wi.timeout = XR_INFINITE_DURATION;
    if (!XR_CHECK_FRAME(X.xrWaitSwapchainImage(X.menuSwap.sc, &wi))) return X.menuShown;  // no release without a wait
    if (idx < X.menuSwap.count) {
        ID3D11Resource* img = nullptr;
        X.menuSwap.rtv[idx]->GetResource(&img);
        if (img) {
            X.ctx->CopyResource(img, X.menuTex);
            img->Release();
        }
    }
    XrSwapchainImageReleaseInfo ri = {};
    ri.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
    XR_CHECK_FRAME(X.xrReleaseSwapchainImage(X.menuSwap.sc, &ri));
    X.menuShown = true;
    return true;
}

static bool TimeReached(DWORD at) { return (LONG)(GetTickCount() - at) >= 0; }

static DWORD WINAPI XrThreadMain(void*) {
    g_xrThreadId = GetCurrentThreadId();
    InstallCrashHandler();  // in case the game replaced it during startup
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    QueryPerformanceFrequency(&X.qpcFreq);
    QueryPerformanceCounter(&X.statStart);
    Log("VR thread started");
    g_stereo.enabled = g_cfg.stereo != 0;
    Log("stereo is %s at start (toggle key 0x%x)", g_stereo.enabled ? "ON" : "OFF", (unsigned)g_cfg.keyStereo);

    if (!InitDevice()) {
        Log("VR disabled for this session (see errors above). The game keeps running normally.");
        return 0;
    }

    // Every wait in this loop is short so the capture texture is always served promptly.
    DWORD instanceRetryAt = 0, systemRetryAt = 0, sessionRetryAt = 0, instanceRetryDelay = 10000;
    bool xrDisabled = false;
    for (;;) {
        ServiceShare();
        if (xrDisabled) {
            Sleep(50);
            continue;
        }
        if (!X.instance) {
            if (!TimeReached(instanceRetryAt)) {
                if (KeyPressed(g_cfg.keyRecenter) && GameIsForeground()) {
                    Log("retrying OpenXR now (recenter key)");
                    instanceRetryAt = GetTickCount();
                }
                Sleep(50);
                continue;
            }
            if (!InitInstance()) {
                if (X.instance) DestroyInstance();
                // Back off (10 s, 20 s, 40 s ... up to 5 min): each attempt may (re)start the VR
                // runtime, which is unwelcome when playing without a headset. F8 retries at once.
                DWORD wait = instanceRetryDelay;
                instanceRetryDelay = instanceRetryDelay >= 150000 ? 300000 : instanceRetryDelay * 2;
                Log("OpenXR not available - retrying in %u s (or press the recenter key to retry now)",
                    (unsigned)(wait / 1000));
                instanceRetryAt = GetTickCount() + wait;
                continue;
            }
            instanceRetryDelay = 10000;
        }
        if (X.system == XR_NULL_SYSTEM_ID) {
            if (!TimeReached(systemRetryAt)) {
                Sleep(50);
                continue;
            }
            if (!InitSystem()) {
                X.system = XR_NULL_SYSTEM_ID;
                systemRetryAt = GetTickCount() + 2000;
                continue;
            }
            if (!CheckHeadsetGpu()) {
                xrDisabled = true;
                Log("VR disabled for this session. The game keeps running normally.");
                continue;
            }
        }
        if (!X.session) {
            if (X.exitRequested || !TimeReached(sessionRetryAt)) {
                Sleep(50);
                continue;
            }
            if (!InitSession()) {
                DestroySession();
                if (X.instanceLost) DestroyInstance();
                sessionRetryAt = GetTickCount() + 3000;
                continue;
            }
        }

        PollEvents();
        if (X.instanceLost) {
            DestroyInstance();
            instanceRetryAt = GetTickCount() + 3000;
            continue;
        }
        if (X.sessionLost) {
            DestroySession();
            // spec: after a lost session the system may have changed (headset unplugged, another one):
            // get it (and its graphics requirements) again before creating a new session
            X.system = XR_NULL_SYSTEM_ID;
            sessionRetryAt = GetTickCount() + 1000;
            systemRetryAt = GetTickCount() + 1000;
            continue;
        }
        if (!X.running) {
            Sleep(10);
            continue;
        }
        Frame();
    }
}

void XrStartThread() {
    static volatile LONG started = 0;
    if (InterlockedCompareExchange(&started, 1, 0) != 0) return;
    HANDLE t = CreateThread(nullptr, 0, XrThreadMain, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
    else Log("ERROR: could not create VR thread (%u)", (unsigned)GetLastError());
}
