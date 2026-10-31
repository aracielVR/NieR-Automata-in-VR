// xr_thread.h - state shared by the VR thread's files (xr_main.cpp, xr_device.cpp, xr_frame.cpp).
#pragma once
#include "core/pure.h"
#include "core/com.h"
#include "render/frame_share.h"
#include "ui/stats_overlay.h"
#include "xr/openxr.h"

// clang-format off
#define XR_INSTANCE_FUNCS(X)          \
	X(xrGetInstanceProperties)        \
	X(xrGetSystem)                    \
	X(xrGetSystemProperties)          \
	X(xrDestroyInstance)              \
	X(xrResultToString)               \
	X(xrPollEvent)                    \
	X(xrCreateSession)                \
	X(xrDestroySession)               \
	X(xrBeginSession)                 \
	X(xrEndSession)                   \
	X(xrWaitFrame)                    \
	X(xrBeginFrame)                   \
	X(xrEndFrame)                     \
	X(xrCreateReferenceSpace)         \
	X(xrDestroySpace)                 \
	X(xrLocateSpace)                  \
	X(xrEnumerateSwapchainFormats)    \
	X(xrCreateSwapchain)              \
	X(xrDestroySwapchain)             \
	X(xrEnumerateSwapchainImages)     \
	X(xrAcquireSwapchainImage)        \
	X(xrWaitSwapchainImage)           \
	X(xrReleaseSwapchainImage)        \
	X(xrEnumerateEnvironmentBlendModes)\
	X(xrGetD3D11GraphicsRequirementsKHR)\
	X(xrLocateViews)                  \
	X(xrEnumerateViewConfigurations)  \
	X(xrEnumerateViewConfigurationViews)
                  

constexpr uint32_t kMaxImages = 8;

struct Swapchain {
	XrSwapchain handle;
	uint32_t w, h, count;
	ID3D11RenderTargetView* rtv[kMaxImages];
	DWORD retryAt;
};

// A flat ImGui panel on its own quad layer: drawn into tex through rtv, then copied into the swapchain.
struct Panel {
	Swapchain swap;
	ID3D11Texture2D* tex;
	ID3D11RenderTargetView* rtv;
	bool shown;  // the swapchain has a released image
};

// The newest game frame of one kind ([0] left eye, [1] right eye, [2] mono), copied onto our device.
struct GameView {
	ID3D11Texture2D* tex;
	ID3D11ShaderResourceView* srv;
	ID3D11Texture2D* sceneTex;
	ID3D11ShaderResourceView* sceneSrv;
	ID3D11Texture2D* keyTex;  // upscaling: the scene image the HUD is cut out against (else null)
	ID3D11ShaderResourceView* keySrv;
	int32_t seq;
	FrameTag tag;
	bool hasFrame;
};

struct VrState {
	PFN_xrGetInstanceProcAddr getProcAddr;
#define DECL_FN(n) PFN_##n n;
	XR_INSTANCE_FUNCS(DECL_FN)
#undef DECL_FN

	XrInstance instance;
	XrSystemId system;
	LUID headsetGpuLuid;
	D3D_FEATURE_LEVEL minFeatureLevel;
	uint32_t maxSwapchainW, maxSwapchainH;

	// VR thread's own D3D11 device (on the game's GPU)
	LUID deviceLuid;
	D3D_FEATURE_LEVEL deviceFeatureLevel;
	ID3D11Device* device;
	ID3D11DeviceContext* context;
	ID3D11VertexShader* blitVs;
	ID3D11PixelShader* blitPs;
	ID3D11PixelShader* hudKeyPs;    // HUD/menu extraction: alpha from |full - scene|
	ID3D11PixelShader* lateWarpPs;  // late warp: an eye image re-aimed to the headset's pose of this frame
	ID3D11PixelShader* screenPs;    // the flat screen as a window in the room, drawn into both eyes
	ID3D11Buffer* lateWarpConstants;
	ID3D11SamplerState* linearSampler;
	ID3D11Buffer* blitConstants;

	XrSession session;
	XrSessionState state;
	bool running;
	bool exitRequested;  // runtime ended the session (EXITING); don't restart it
	bool sessionLost;    // destroy & recreate the session
	bool instanceLost;   // destroy & recreate everything
	XrSpace localSpace, viewSpace;
	XrEnvironmentBlendMode blendMode;

	int64_t swapFormat;
	Swapchain screenSwap, eyeSwap[2], hudSwap;
	Swapchain screenEyeSwap[2];  // the flat screen drawn into the eyes (kept at the headset's own size)
	Panel menuPanel, overlayPanel;  // the settings menu (ui/menu.cpp), the stats overlay (ui/stats_overlay.cpp)
	XrPosef menuPose;
	pure::Q menuYaw;         // the menu faces where you looked when it opened...
	XrVector3f menuHeadPos;  // ...from where your head was; MenuDistance away (live)
	bool menuPlaced;

	// game frames, on our device
	int32_t servedRequestGen;  // last game request we answered
	int32_t publishedGen;
	ID3D11Texture2D* ring[kRingSize];       // shared with the game device: full frames
	ID3D11Texture2D* ringScene[kRingSize];  // scene-only frames (UI removed)
	ID3D11Texture2D* ringKey[kRingSize];    // upscaling: scene-only frames before upscaling (HUD key)
	GameView views[3];                      // [0] left, [1] right, [2] mono
	UINT gameW, gameH;                      // the game's frames
	UINT sceneW, sceneH;                    // its scene-only images (larger when upscaling, upscale.cpp)
	bool gameImageLinear;

	// UI coverage measurement (decides menu mode)
	ID3D11Texture2D* coverageTex;
	ID3D11RenderTargetView* coverageRtv;
	ID3D11Texture2D* coverageStaging[3];
	bool coveragePending[3];
	int32_t coverageNext;
	int32_t coverageMeasuredSeq;
	int32_t menuVotes;           // >0 towards menu, <0 towards gameplay
	int32_t framesWithoutScene;  // consecutive eye frames without a scene-only image
	uint32_t statSceneFrames;
	float statCoverageSum, statCoverageMax;
	uint32_t statCoverageCount;
	bool menuMode;

	// screen placement
	bool screenPlaced;
	bool recenterRequested;
	XrPosef quadPose;
	pure::Q lastScreenYaw;
	bool spaceFlipped;         // the runtime's LOCAL space has its +Y pointing down (Virtual Desktop)
	int32_t upsideDownFrames;  // consecutive headset frames the head's up points against the reference's up
	float lastHeadPos[3];      // head position last frame (silent space changes show as jumps)
	bool lastHeadValid;
	bool keyDown[256];

	// stats
	uint32_t statFrames, statQuadFrames, statStereoFrames, statGameFrames[3];
	double statStartMs;  // NowMs() when these counts started
	uint32_t errorCount;

	// head pose prediction: how long after the VR frame whose display time the game's pose was
	// predicted from (poseBase) its images are actually on screen, averaged
	double poseLagMs;  // running average
	bool poseLagValid;
	int64_t predictionNs;  // extra prediction handed to the game thread
	double statLagSum;
	uint32_t statLagCount;
	float displayEyeRot[2][4];  // eye orientations at this VR frame's display time
	bool displayPoseValid;
	XrView displayViews[2];               // both eyes at this VR frame's display time (late warp submits these)
	uint32_t recommendedW, recommendedH;  // runtime's recommended per-eye image size
	float sharpnessPctX, sharpnessPctY;   // game picture's sharpness relative to that, horizontally / vertically (0 = unknown)
	UINT suggestedGameW, suggestedGameH;
	bool displayViewsValid;  // disp holds a valid pair (kept from the last good one while tracking drops)
	// how far the shown image's pose is from the head at display (what the margin must cover)
	double statTurnSum;
	float statTurnMax;
	uint32_t statTurnCount, statTurnOverMargin;  // over: frames where it exceeded the rendered margin
	float marginDeg;
};
extern VrState g_vr;

// Logs failures; per-frame calls are rate-limited so a persistent error can't flood the log.
bool XrCheck(XrResult result, const char* what, bool perFrame = false);
#define XR_CHECK(call) XrCheck((call), #call)
#define XR_CHECK_FRAME(call) XrCheck((call), #call, true)

inline bool IsSrgb(int64_t format) {
	return format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
		   format == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
}

// The flat screen drawn into an eye: the same 80-byte constant buffer as WarpParams.
struct ScreenParams {
	float outTan[4];  // shown eye: tangents of left, right, up, down
	float half[4];    // the screen: half width, half height in metres; z, w: the picture's rows shown (0..1, w > z, else all)
	float m[3][4];    // rows of R_screen^T * R_eye, and in .w the eye's position in the screen's space
};

struct WarpParams {
	float outTan[4];  // shown eye: tangents of left, right, up, down
	float srcTan[4];  // rendered image: tangent half-width, half-height, and its lens centre (x, y)
	float m[3][4];    // rows of R_render^T * R_shown
};

// xr_main.cpp: swapchains
bool EnsureSwap(Swapchain& swap, uint32_t width, uint32_t height, const char* what, bool copyTarget = false);
// Draws `srv` (or, with `scene`, the keyed-out UI of `srv`; with `warp`, the late-warped eye image)
// into the next image of `swap`. Returns false if nothing could be submitted.
bool RenderInto(Swapchain& swap, ID3D11ShaderResourceView* srv, ID3D11ShaderResourceView* scene = nullptr,
				const WarpParams* warp = nullptr, bool asScreen = false);
// Draws the flat screen as a window in the room into one eye of `swap`.
bool RenderScreenInto(Swapchain& swap, ID3D11ShaderResourceView* srv, const ScreenParams& screen);

// xr_device.cpp
bool InitDevice();     // created on the game's GPU as soon as the VR thread starts
void ServiceShare();   // answers the game thread's request for capture textures
void UpdateContent();  // copies newly finished game frames into views[]
void MeasureUICoverage();
// The flat screen's picture between black bands above and below (rows 0..1 from the top; 0, 1 = none or CropBlackBars=0)
void ScreenPictureRows(ID3D11ShaderResourceView* srv, float* v0, float* v1);
void WatchEyeBars(ID3D11ShaderResourceView* srv);  // logs black bands in the 3D picture (cutscenes)
void Blit(ID3D11RenderTargetView* rtv, uint32_t width, uint32_t height, ID3D11ShaderResourceView* srv,
		  ID3D11ShaderResourceView* scene, bool dstSrgb, const WarpParams* warp = nullptr, bool asScreen = false);
ID3D11ShaderResourceView* KeySrv(const GameView& view);  // the scene image the HUD is cut out against

// xr_frame.cpp
void Frame();  // one headset frame
bool GameIsForeground();
bool KeyPressed(int vk);  // edge-triggered
