// config.h - the mod's settings (g_cfg), loaded from NieRVR.ini (config.cpp).
#pragma once
#include <stdint.h>
#include <windows.h>

struct Config {
	// [Loader]
	wchar_t chainLoad[MAX_PATH];  // optional DLL to load instead of System32\d3d11.dll
	// [VR]
	int32_t enabled;
	// [OpenXR]
	wchar_t loaderPath[MAX_PATH];  // optional openxr_loader.dll; empty = auto
	// [Screen]
	float distance;            // metres in front of the head
	float width;               // metres
	float heightOffset;        // metres, relative to eye height
	int32_t statsOverlay;      // live stats in the corner of your view (ui/stats_overlay.cpp)
	float overlayX, overlayY;  // its position: metres right / up of straight ahead (at 0.8 m; scaled with the distance)
	float overlayDistance;     // metres in front of your eyes (it keeps its size in your view)
	float menuDistance;        // the settings menu: metres in front of you when it opens (keeps its size in your view)
	// [Keys]  (Windows virtual-key codes)
	int32_t keyRecenter, keySmaller, keyLarger, keyCloser, keyFarther;
	int32_t keyCapture;  // [Debug] CaptureKey: frame capture
	int32_t probe;       // [Debug] Probe: the frame capture key is on
	// [Stereo]
	int32_t stereo;              // start in stereo mode
	float worldScale;            // game units per metre of head movement / IPD
	int32_t levelCamera;         // remove the game camera's pitch/roll
	float steepFrom, steepFull;  // camera pitch (deg) where levelling starts to fade out / is fully off
	float fovScale;              // multiplies the rendered frustum (>1 = margin for reprojection)
	int32_t keyStereo;           // [Keys] ToggleStereo
	int32_t keyMenu;             // [Keys] Menu: opens/closes the settings menu (0 = no key)
	float verticalFill;          // 1 = render the headset's full vertical FOV; 0 = fit width (sharper, crops top/bottom)
	int32_t anamorphic;          // fill each eye's own FOV shape (non-square pixels) instead of a 16:9 cover
	int32_t realFrameRate;       // 1 = hand the VR software only new game pictures (its counter and SSW then see the real rate)
	int32_t afwMode;             // AFW stereo mode only: 0 combined (default), 1 this-frame warp only (gaps stretched)
	int32_t asymmetricEyes;      // render each eye's own off-centre lens instead of a symmetric frustum covering it
	int32_t screenInEyes;        // flat screen drawn into both eyes as a window in the room (0 = hand it over as a quad layer)
	int32_t cropBlackBars;       // flat screen trimmed to the picture's rows (black bands above and below cut off)
	float cameraDolly;           // metres to move the camera along its line of sight (towards the character)
	int32_t firstPerson;         // eyes at the character's eyes (or the game camera's look-at target), not behind them
	int32_t fpTracking;          // first person: the character tracks your headset and controllers, its head hidden
	int32_t steadyEyes;          // first person: sideways the camera follows their body, not their head's animations
	int32_t keyFirstPerson;      // [Keys] FirstPerson
	// [Render]
	int32_t extraResolutions;
	int32_t headsetWidth, headsetHeight;  // [Render] the game resolution with the headset's full detail (saved when seen)
	int32_t ambientOcclusion;
	int32_t shadows;  // 0 = NieR's screen-space shadow mask cleared to "lit" (diagnostic)
	int32_t vignette;
	int32_t lowHealthFx;         // 0 = low-health grey/distortion (screen filter effects) removed in stereo
	int32_t distortionFx;        // 0 = remove the screen-copy distortion/glitch effects (hits, low health) in stereo
	float cullFov;               // vertical field of view (deg) the game culls with during stereo; 0 = game default
	float cullFovAsked;          // CullingFov from the ini when above the 170 maximum (for the log), else 0
	uint32_t fovEasingCaller;    // exe offset of the camera-easing fov read, once found (0 = unknown)
	uint32_t fovRealCallers[8];  // exe offsets of fov getter callers that always get the real fov
	int32_t fovRealCount;
	int32_t headCulling;      // turn the culling frustum the way the headset looks
	int32_t headShadows;      // turn the sun shadow cascades' camera the way the headset looks
	float shadowLeadMs;       // the sun shadows' aim leads a turning head by this much
	float shadowExtraFov;     // degrees added to the sun shadows' field of view (wider reach, softer shadows)
	float headCullMargin;     // degrees added around the headset's view for the head-culling frustum
	int32_t synchronized;     // both eyes rendered every frame (record + replay)
	int32_t reuseShadows;     // Synchronized: the replay reuses the left eye's shadow map
	int32_t depthReproject;   // alternating mode: move the older eye's image to the new camera with its depth
	int32_t posePrediction;   // [Stereo] PosePrediction: predict the game's head pose for when its frame is shown
	int32_t lateWarp;         // [Stereo] LateWarp: submit the headset's pose/fov, eye images re-aimed to it
	int32_t maxFrameLatency;  // [Render] MaxFrameLatency: frames the game's CPU may queue ahead of the GPU; 0 = game's
	int32_t giVolumes;        // [Render] GIVolumes: GI volumes the game's GI pass reads (nearest first); >= 128 = all
	// [Upscaler] (render/upscale.cpp)
	int32_t upscaler;     // 0 off, 1 DLSS
	int32_t foveated;     // [Render] FoveatedShading: 0 off, 1 subtle, 2 strong (render/foveated.cpp)
	float upscaleOutput;   // the upscaled image's size: % of the detail the headset recommends
	int32_t upscaleQuality;  // 0 sharpest (DLAA), 1 quality, 2 balanced, 3 fastest: how much DLSS enlarges
	int32_t panelResolution;  // the mod sets the size the game renders (from the two settings above)
	int32_t sunGlare;         // 0 = the game's glare pass (the bloom around the sun) draws nothing
	int32_t lensFlare;        // 0 = the lens flare's draws (found by their textures) draw nothing
	int32_t gameVsync;        // 0 = the game's Present never waits for the monitor (the headset sets the pace)
	int32_t gameAntiAliasing; // 0 = the game's own anti-aliasing is set to off in its settings (DLSS smooths)
	int32_t gameMotionBlur;   // 0 = the game's motion blur is set to off in its settings
	int32_t scatterPerEye;    // the light scattering's sun direction turned into each eye's view
	int32_t gameFrameRate;    // 0 = the game's own 60, 1..4 = 72 / 80 / 90 / 120, 5 = headset, 6 = unlimited (game/frame_rate.cpp)
	int32_t gameSpeedFix;     // above 60: the game advances by the real frame time, not a 60th per frame
	int32_t gameStepFix;      // above 60: the game's own measured frame step may go below a 60th (animation)
	int32_t frameTimers;      // above 60: the game's timers that move a 60th per frame follow the real time
	int32_t frameConstants;   // above 60: per-frame amounts / easing / decay constants follow the real time
	uint32_t lensFlareExtra[16];  // [Render] LensFlareTextures: more flare textures (3Dmigoto texture hashes)
	int32_t lensFlareExtraCount;
	int32_t gameWidth, gameHeight;  // the size the game last rendered at: always kept in its own list
	int32_t dlssModel;    // 0 CNN (preset E, fast), 1 transformer (preset K)
	// [Input]
	int32_t vrControllers;            // VR controllers act as an XInput gamepad
	int32_t vrRumble;                 // the game's rumble goes to the VR controllers
	int32_t motionControls;           // swing a hand to attack (vr_controllers.cpp)
	int32_t stickFollowsHead;         // the left stick's forward is where your headset faces, not the recentred forward
	int32_t swingSensitivity;         // motion controls: 0 = relaxed, 1 = normal (input/waggle.h)
	int32_t iniAdded;                 // settings added to an older ini at startup
	int32_t flatBelowDraws;           // frames with fewer draws than this render mono (title screen); 0 = off
	int32_t topDownFlat;              // 1 = steep top-down views are shown on the flat screen
	float topDownFrom, topDownUntil;  // TopDownFlat: camera pitch (deg) to go flat / back to stereo
	// [Debug]
	int32_t verbose;
};
extern Config g_cfg;

void ConfigLoad();
bool ConfigSave(const void* field);                   // writes one setting to NieRVR.ini: ConfigSave(&g_cfg.worldScale)
bool ConfigDefault(const void* field, float* value);  // a setting's default (ints as floats); false if not in the table
void ConfigSaveEasingCaller(uint32_t exeOffset);

// [Stereo] Synchronized holds the stereo mode: 0 alternating (one eye per game frame, the other eye's previous
// picture moved to the new camera), 1 synchronized (both eyes every frame), 2 AFW (one eye per frame, the
// other built from it by its depth). Read it through these, never as a yes/no.
inline bool StereoSynchronized() { return g_cfg.synchronized == 1; }
inline bool StereoAfw() { return g_cfg.synchronized == 2; }
