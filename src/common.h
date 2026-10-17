// common.h - shared declarations for the NieRVR mod.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include "pure.h"
struct FrameTag;

#define NIERVR_VERSION "0.5.4"

// ---------------------------------------------------------------------------------------------
// Logging (log.cpp)
// ---------------------------------------------------------------------------------------------
void LogOpen(const wchar_t* path);
void Log(const char* fmt, ...);
// Logs only when [Debug] Verbose=1
void LogV(const char* fmt, ...);

// ---------------------------------------------------------------------------------------------
// Configuration (config.cpp)  - NieRVR.ini next to the DLL
// ---------------------------------------------------------------------------------------------
struct Config {
    // [Loader]
    wchar_t chainLoad[MAX_PATH];  // optional DLL to load instead of System32\d3d11.dll
    // [VR]
    int enabled;
    // [OpenXR]
    wchar_t loaderPath[MAX_PATH];  // optional openxr_loader.dll; empty = auto
    // [Screen]
    float distance;      // metres in front of the head
    float width;         // metres
    float heightOffset;  // metres, relative to eye height
    // [Keys]  (Windows virtual-key codes)
    int keyRecenter, keySmaller, keyLarger, keyCloser, keyFarther;
    int keyCapture;  // [Debug] CaptureKey: camera probe frame capture
    int probe;       // [Debug] Probe: install the camera probe hooks
    // [Stereo]
    int stereo;          // start in stereo mode
    float worldScale;    // game units per metre of head movement / IPD
    int levelCamera;     // remove the game camera's pitch/roll
    float steepFrom, steepFull;  // camera pitch (deg) where levelling starts to fade out / is fully off
    float fovScale;      // multiplies the rendered frustum (>1 = margin for reprojection)
    int keyStereo;       // [Keys] ToggleStereo
    float verticalFill;  // 1 = render the headset's full vertical FOV; 0 = fit width (sharper, crops top/bottom)
    int uiSplit;         // HUD/menus on a separate flat layer
    int anamorphic;      // fill each eye's own FOV shape (non-square pixels) instead of a 16:9 cover
    float cameraDolly;   // metres to move the camera along its line of sight (towards the character)
    // [Render]
    int extraResolutions;
    int ambientOcclusion;
    int shadows;           // 0 = NieR's screen-space shadow mask cleared to "lit" (diagnostic)
    int vignette;
    int lowHealthFx;       // 0 = low-health grey/distortion (screen filter effects) removed in stereo
    int viewFix;
    int distortionFx;      // 0 = remove the screen-copy distortion/glitch effects (hits, low health) in stereo
    float cullFov;         // vertical field of view (deg) the game culls with during stereo; 0 = game default
    int lodFix;            // 1 = model detail keeps the game's own field of view while culling is widened
    float cullFovAsked;    // CullingFov from the ini when above the 170 maximum (for the log), else 0
    unsigned fovEasingCaller;  // exe offset of the camera-easing fov read, once found (0 = unknown)
    unsigned fovRealCallers[8];  // exe offsets of fov getter callers that always get the real fov
    int fovRealCount;
    int headCulling;       // turn the culling frustum the way the headset looks (0.3.22)
    float headCullMargin;  // degrees added around the headset's view for the head-culling frustum (0.3.24)
    int bothEyes;          // render both eyes every frame (record + replay, 0.3.26)
    int reuseShadows;      // BothEyes: the replay reuses the left eye's shadow map (0.3.29)
    int depthReproject;    // alternate eyes: move the older eye's image to the new camera with its depth (0.3.30)
    int reprojectEdges;    // fill the gaps it opens with background instead of stretching edges (0.4.1)
    int posePrediction;    // [Stereo] PosePrediction: predict the game's head pose for when its frame is shown (0.4.4)
    int lateWarp;          // [Stereo] LateWarp: submit the headset's pose/fov, eye images re-aimed to it (0.5.1)
    int maxFrameLatency;   // [Render] MaxFrameLatency: frames the game's CPU may queue ahead of the GPU; 0 = game's (0.4.5)
    int giVolumes;         // [Render] GIVolumes: GI volumes the game's GI pass reads (nearest first); >= 128 = all (0.4.2)
    // [Input]
    int vrControllers;     // VR controllers act as an XInput gamepad (0.4.0)
    int vrRumble;          // the game's rumble goes to the VR controllers
    int eyeSync;           // turn the older eye's image by the game camera's turn since (0.3.25)
    int wideProjection;    // 1 = the camera's own projection gets the wide fov too (0.3.22; Pod aims up)
    int iniAdded;          // settings added to an older ini at startup
    int flatBelowDraws;    // frames with fewer draws than this render mono (title screen); 0 = off
    int topDownFlat;       // 1 = steep top-down views are shown on the flat screen
    float topDownFrom, topDownUntil;  // TopDownFlat: camera pitch (deg) to go flat / back to stereo
    // [Debug]
    int verbose;
};
extern Config g_cfg;
extern wchar_t g_modDir[MAX_PATH];  // directory of this DLL, with trailing backslash
extern wchar_t g_iniPath[MAX_PATH];
void ConfigLoad();
void ConfigSaveScreen();  // writes back the [Screen] values after hotkey changes
void ConfigSaveEasingCaller(unsigned exeOffset);
void ConfigSaveStereo();
void ConfigWrite(const char* section, const char* key, const char* value);  // one ini value (menu)
// menu.cpp - the in-headset settings menu
// gpuprof.cpp - GPU timing of one frame every 5 s (diagnostic)
void GpuProfFrameStart(ID3D11DeviceContext* c);
void GpuProfMark(ID3D11DeviceContext* c, const char* label);
void GpuProfTargets(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv,
                    const char* prefix);
void GpuProfCompute(ID3D11DeviceContext* c, const char* prefix, int kind);
void GpuProfDraw();
void GpuProfFrameEnd(ID3D11DeviceContext* c);
void GpuProfPoll(ID3D11DeviceContext* c);
void CapturePollMidFrame();
void FrameLatencyTick();  // probe.cpp: applies [Render] MaxFrameLatency to the game's device  // present_hook.cpp: publish finished frames without waiting for Present
void MenuInstallInput();
void MenuTick(bool foreground);
void MenuSetInfo(int line, const char* text);
bool MenuIsOpen();
bool MenuTakeJustOpened();
bool MenuTakeRecenter();
// vrinput.cpp - VR controllers as a gamepad (0.4.0)
struct _XINPUT_GAMEPAD;
bool VrInputPad(struct _XINPUT_GAMEPAD* out, bool evenIfOff = false);  // false = no VR gamepad; evenIfOff: for the menu
void VrInputRumble(unsigned short left, unsigned short right);
void VrInputDetach();
void VrInputShutdown();
void VrInputSync(bool focused, bool foreground);
void VrInputProfileChanged();  // interaction profile changed event
const uint32_t* MenuImage(bool bgra, bool force, int* w, int* h);  // writes back the values the hotkeys change ([Stereo] WorldScale/CameraDolly/TopDownFlat, ...)

// ---------------------------------------------------------------------------------------------
// Startup (main.cpp)
// ---------------------------------------------------------------------------------------------
void EnsureInit();  // idempotent, thread safe; loads config, log, real d3d11
extern HMODULE g_hSelf;

// ---------------------------------------------------------------------------------------------
// Proxy (proxy.cpp)
// ---------------------------------------------------------------------------------------------
bool ProxyLoadReal();  // called from EnsureInit
extern PFN_D3D11_CREATE_DEVICE g_realCreateDevice;
extern PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN g_realCreateDeviceAndSwapChain;

// ---------------------------------------------------------------------------------------------
// Present hook + game-side frame capture (present_hook.cpp)
// ---------------------------------------------------------------------------------------------
void HookFromSwapChain(IDXGISwapChain* sc);
bool HookFunction(void* target, void* detour, void** orig, const char* name);  // MinHook wrapper

// Arguments of a draw call, for replaying it (kind: 0 Draw, 1 DrawIndexed, 2 DrawInstanced,
// 3 DrawIndexedInstanced; fields in the API's parameter order).
struct DrawArgs {
    int kind;
    UINT a, b;
    INT c;
    UINT d, e;
};

// UI split (uisplit.cpp): scene-only copy of each frame, so HUD/menus can go on a flat layer.
void UiSetBackbuffer(ID3D11Texture2D* bb);
void UiBeginFrame();
void UiOnSetVS(bool isUI);
bool UiBeforeDraw(ID3D11DeviceContext* c);  // true = skip this draw
void UiOnMap(ID3D11Resource* res, void* data, UINT size);
void UiBeforeUnmap(ID3D11Resource* res);
void UiOnUpdate(ID3D11Resource* res, const void* data, UINT size);
void UiOnSetVSConstantBuffers(UINT start, UINT n, ID3D11Buffer* const* bufs);
void UiAfterDraw(ID3D11DeviceContext* c, const DrawArgs& args);
ID3D11Texture2D* UiSceneTexture();  // this frame's scene without UI, or null
void ScreenFxBeginFrame();  // screenfx.cpp: low-health grey / distortion
// An effect-system draw (particles, screen filters) that samples the texture registers in `sampled`:
// true = skip it (a low-health screen filter while in stereo, LowHealthEffect=0).
bool ScreenFxSkipEffectDraw(ID3D11DeviceContext* c, uint32_t sampled, uint64_t shaderHash);
void ProbeReplayDraw(ID3D11DeviceContext* c, const DrawArgs& d);
void ProbeSetRenderTargets(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtvs,
                           ID3D11DepthStencilView* dsv);

// Camera probe (probe.cpp): records one frame of D3D11 activity on a hotkey, for analysis.
void ProbeInstall(ID3D11Device* gameDevice);
void ProbeSetTarget(ID3D11Device* presentingDevice);
void ProbeBeforePresent();  // end of a frame
void ProbeAfterPresent();   // start of the next frame
void HookWithDummySwapChain();
// Render resolution (display.cpp)
void DisplayInstallHooks();
void DisplayEarlyHooks();  // from DllMain: patches code only
extern volatile bool g_initDone;  // EnsureInit has finished
void DisplayHookSwapChain(IDXGISwapChain* sc);
void DisplayOnFirstPresent(IDXGISwapChain* sc);
void UiOnResizeBuffers();  // the swapchain's buffers are about to be replaced
void StereoCountDraw();
void CullingUpdate(bool wide);
void CullingObserveGameFov(float deg);
float CullingGameFov();  // the field of view (deg) the game last asked for, before widening
void CullingInstallHook();  // from EnsureInit: hooks the game camera's fov setter
void CullingWatch();  // diagnostic: hardware breakpoints on the camera fov for 2 s (Ctrl+F5)
void CullingToggleHeadCulling();  // Ctrl+Shift+F5: head-direction culling on/off (saved)
void CullingCycleTest();          // Shift+F5: which reads get the wide fov (test, not saved)
bool StereoGetViewFix(float T[16], float Tinv[16]);  // game view -> eye view (and back), this frame
void ViewFixBeforeDispatch(ID3D11DeviceContext* c, int kind);
void ViewFixAfterDispatch(ID3D11DeviceContext* c);
void ProbeRawDispatch(ID3D11DeviceContext* c, UINT x, UINT y, UINT z);
// Second-eye playback (replay.cpp): a recorded constant-buffer upload, camera patched for this eye.
const void* ProbePatchForReplay(ID3D11Resource* res, const void* data, UINT size, float* scratch /* 64 floats */);

// replay.cpp - BothEyes: record the left eye's frame, play it back for the right eye at Present.
extern volatile bool g_replaying;      // playback running: hooks pass calls straight through
extern volatile LONG g_recOn;
extern ID3D11DeviceContext* g_recCtx;
extern volatile LONG g_recSuspend;     // mod-internal calls (not recorded)
inline bool RecOn(ID3D11DeviceContext* c) { return g_recOn && c == g_recCtx && !g_recSuspend && !g_replaying; }
enum RecStage { RS_VS = 0, RS_HS, RS_DS, RS_GS, RS_PS, RS_CS };
void RecSetCB(int stage, UINT start, UINT n, ID3D11Buffer* const* b);
void RecSetSRV(int stage, UINT start, UINT n, ID3D11ShaderResourceView* const* v);
void RecSetShader(int stage, void* sh, ID3D11ClassInstance* const* ci, UINT n);
void RecCSSetUAV(UINT start, UINT n, ID3D11UnorderedAccessView* const* v, const UINT* counts);
void RecDraw(int kind, UINT a, UINT b, INT c, UINT d, UINT e);
void RecDispatch(UINT x, UINT y, UINT z, int viewFixKind);
void RecOMSetRT(UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv);
void RecViewports(UINT n, const D3D11_VIEWPORT* vp);
void RecBeforeMap(ID3D11Resource* res, D3D11_MAP type);
void RecMap(ID3D11Resource* res, UINT sub, D3D11_MAP type, UINT flags, const D3D11_MAPPED_SUBRESOURCE* m);
void RecUnmap(ID3D11Resource* res, UINT sub);
void RecUpdate(ID3D11Resource* res, UINT sub, const D3D11_BOX* box, const void* data, UINT rp, UINT dp);
void RecUnsupported(const char* what);
void ReplayInstall(ID3D11DeviceContext* ctx);
bool ReplayAvailable();
void ReplayFrameStart(ID3D11DeviceContext* c, bool bothEyes);
void ReplayRelease();  // before the swapchain is resized: hold nothing of the game's
void ReplayStop(bool fromUi);
void ReplayCaptureFinalPassState(ID3D11DeviceContext* c);
void ReplayApplyFinalPassState(ID3D11DeviceContext* c);
bool ReplaySecondEye(ID3D11DeviceContext* c, bool* inBackbuffer);
void ReplayReport();
bool UiSecondEyeScene(ID3D11DeviceContext* c);  // right eye's scene image from the replayed UI target
bool StereoBothEyesFrame();      // this frame renders the left eye and replays the right one
void StereoBeginSecondEye();     // switch the camera patching to the right eye (same head pose)
bool StereoSecondEyePatched();
bool StereoReprojectParams(int eye, float T[16], float proj[4], FrameTag* tag);  // depth reprojection
int StereoFrameEye();  // the eye this frame renders (-1 = mono)
// reproject.cpp - depth-based correction of the older eye (alternate-eye mode, 0.3.30)
void ReprojectNoteDsv(ID3D11DepthStencilView* dsv);   // a depth buffer the game draws with
void ReprojectAtUiStart(ID3D11DeviceContext* c);      // the scene is finished: keep its depth
void ReprojectRelease();                              // before a resize
// at Present after this frame's eye was captured: returns the warped other eye (or null)
ID3D11Texture2D* ReprojectOtherEye(ID3D11DeviceContext* c, ID3D11Texture2D* scene, ID3D11Texture2D* bb, FrameTag* tag);
void StateSaveTemp(ID3D11DeviceContext* c);           // replay.cpp: pipeline state around our own drawing
void StateRestoreTemp(ID3D11DeviceContext* c);   // the replay found and patched the camera

// ---------------------------------------------------------------------------------------------
// Stereo (stereo.cpp game side, xr_main.cpp VR side)
// ---------------------------------------------------------------------------------------------
struct StereoShared {  // written by the VR thread, read by the game thread
    SRWLOCK lock;
    bool enabled;      // stereo toggled on (hotkey)
    bool valid;        // eye poses below are current
    float eyePos[2][3], eyeRot[2][4];  // LOCAL space
    float eyeTan[2][4];                // tan of angleLeft, angleRight, angleUp, angleDown
    bool menuMode;                     // the UI covers the screen: render flat (mono)
    bool refValid;                     // recentred reference (yaw-only head pose)
    float refPos[3], refRot[4];
    bool singleEye;                    // diagnostic: render only the left eye, every frame (Ctrl+F12)
    long long poseBase;                // VR frame display time (XrTime) the poses were predicted from;
                                       // they are predicted for poseBase + the measured lag (0.4.4)
};
extern StereoShared g_stereo;

struct FrameTag {  // what a captured game frame shows
    int eye;       // 0 left, 1 right, -1 mono
    bool patched;  // the camera was rewritten for that eye
    bool hasScene; // a scene-only (no UI) image was captured too
    float pos[3], rot[4];  // eye pose (LOCAL) it was rendered from
    float tanHalfW, tanHalfH;
    unsigned pairId;       // BothEyes: left and right image of one game frame share it (0 = single)
    bool noUI;             // drawn without UI (the replayed second eye): don't key the HUD from it
    bool hasWorld;         // worldRot is valid (the scene camera was patched)
    float worldRot[4];     // game-world orientation of LOCAL space for this frame (eye sync, 0.3.25)
    long long poseBase;    // StereoShared::poseBase of the pose it was rendered with (0 = unknown)
};

void StereoSetAspect(float aspect);
void StereoBeginFrame();
void StereoGetFrameTag(FrameTag* tag);
bool StereoActive();
bool StereoGameCamera(float pos[3], float fwd[3], float up[3]);
bool StereoWatching();
bool StereoHeadRelative(pure::M4* rel);
float StereoHeadCullFov(float marginDeg);  // radians; 0 = not known yet  // eye pose relative to the game camera (head culling)
void StereoOnMap(ID3D11Resource* res, void* data, UINT size);
void StereoBeforeUnmap(ID3D11Resource* res);
const void* StereoPatchUpdate(UINT size, const void* data, float* scratch);
void StereoMemoryScan();

// Frame hand-off from the game's device to the VR thread's device.
// The VR thread owns the shared texture (created on its own device); the game thread asks for a
// size/format and opens whatever the VR thread publishes.
// Capture ring slots (8 since 0.3.27: BothEyes pushes two images per game frame).
const int kRingSize = 8;
struct FrameShare {
    CRITICAL_SECTION cs;
    // request: written by the game thread (under cs)
    LONG reqGen;  // bumps whenever the request changes
    UINT reqW, reqH;
    DXGI_FORMAT reqFmt;
    // reply: written by the VR thread (under cs): a ring of shared textures
    HANDLE ring[kRingSize];       // legacy shared handles: full frames
    HANDLE ringScene[kRingSize];  // and scene-only (no UI) frames
    LONG gen;        // bumps whenever a new ring is published
    LONG forReqGen;  // the request this ring answers
    UINT w, h;
    DXGI_FORMAT fmt;
    // completed frames, published by the game thread (under cs) once the GPU finished the copy
    struct Published {
        LONG seq;  // 0 = none yet
        int slot;  // ring index
        FrameTag tag;
    } latest[3];   // [0] left eye, [1] right eye, [2] mono
    LONG seq;      // last sequence number handed out
    // GPU that runs the game
    LUID gameLuid;
    bool gameLuidValid;
};
extern FrameShare g_share;

// main.cpp helpers
void DescribeAddress(void* addr, char* out, size_t cap);  // "module.dll+0x1234"
// Returns true while fewer than `max` messages were logged through this counter.
bool LogBudget(volatile LONG* counter, LONG max);

// ---------------------------------------------------------------------------------------------
// VR thread (xr_main.cpp)
// ---------------------------------------------------------------------------------------------
void XrStartThread();
extern volatile DWORD g_xrThreadId;
