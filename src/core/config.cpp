// config.cpp - every setting with its ini section, key, default, range and comment; reads and writes NieRVR.ini.
#include "core/config.h"
#include "core/main.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Config g_cfg;

namespace {
enum Kind {
	K_INT,
	K_HEX,
	K_FLOAT,
	K_PATH,
	K_TEXT,
};

struct Setting {
	const char* section;
	const char* key;
	Kind kind;
	void* field;                   // in g_cfg
	float defaultValue, min, max;  // K_INT, K_HEX, K_FLOAT
	const char* comment;           // written above the key, "; " before each line ('\n' separates lines)
	const char* text;              // K_PATH, K_TEXT: the default
};

#define S_INT(sec, key, field, def, lo, hi, c) {sec, key, K_INT, &g_cfg.field, def, lo, hi, c, nullptr}
#define S_HEX(sec, key, field, def, c) {sec, key, K_HEX, &g_cfg.field, def, 0, 255, c, nullptr}
#define S_FLT(sec, key, field, def, lo, hi, c) {sec, key, K_FLOAT, &g_cfg.field, def, lo, hi, c, nullptr}
#define S_PATH(sec, key, field, c) {sec, key, K_PATH, g_cfg.field, 0, 0, 0, c, ""}
#define S_TEXT(sec, key, def, c) {sec, key, K_TEXT, nullptr, 0, 0, 0, c, def}

constexpr Setting kSettings[] = {
	S_INT(
		"VR",
		"Enabled",
		enabled,
		1,
		0,
		1,
		"0 = mod stays passive (game runs normally, nothing is sent to the headset)"),

	S_INT(
		"Input",
		"VRControllers",
		vrControllers,
		1,
		0,
		1,
		"1 = your VR controllers work as a gamepad (Quest/Touch, Index, Pico, Vive, WMR...). A real\n"
		"gamepad and the keyboard keep working too. 0 = off (also in the in-headset menu)"),
	S_INT(
		"Input",
		"ControllerRumble",
		vrRumble,
		1,
		0,
		1,
		"1 = the game's rumble is played on the VR controllers"),
	S_INT(
		"Input",
		"StickFollowsHeadset",
		stickFollowsHead,
		0,
		0,
		1,
		"First person: 1 = pushing the left stick forward moves you where your headset faces. 0 = forward is the way you\n"
		"faced when you last recentred (your play space's forward)"),
	S_INT(
		"Input",
		"MotionControls",
		motionControls,
		0,
		0,
		1,
		"1 = swing a VR controller to attack: right hand = light attack (X), left hand = heavy attack (Y).\n"
		"The buttons keep working too"),
	S_INT(
		"Input",
		"SwingSensitivity",
		swingSensitivity,
		1,
		0,
		1,
		"Motion controls: 0 = relaxed (smaller, sloppier swings and stabs count), 1 = normal.\n"
		"The log shows each attack, and why a swing that didn't attack was rejected"),
	S_INT(
		"Screen",
		"ScreenInEyes",
		screenInEyes,
		1,
		0,
		1,
		"1 = the flat screen (menus, loading screens, the title) is drawn into both of your eyes as a window\n"
		"hanging in the room, so it stays still and square-on while you turn your head. 0 = hand it to your VR\n"
		"software as a flat layer, which is cheaper but smears and shears on head turns with some of them"),
	S_INT(
		"Screen",
		"CropBlackBars",
		cropBlackBars,
		1,
		0,
		1,
		"1 = no black bands: the flat screen is trimmed to the picture of a film or menu (the game fits them into\n"
		"its taller picture), and the dark bands of 3D cutscenes aren't drawn. 0 = the game's bands, as it draws them"),
	S_FLT(
		"Screen",
		"Distance",
		distance,
		2.5f,
		0.3f,
		50,
		"Virtual screen placement, in metres. Also adjustable in game with the hotkeys below."),
	S_FLT(
		"Screen",
		"Width",
		width,
		3.2f,
		0.2f,
		100,
		nullptr),
	S_FLT(
		"Screen",
		"HeightOffset",
		heightOffset,
		0,
		-10,
		10,
		nullptr),
	S_INT(
		"Screen",
		"StatsOverlay",
		statsOverlay,
		1,
		0,
		1,
		"1 = live stats (frame rates, resolution, head-pose delay) in the corner of your view"),
	S_FLT(
		"Screen",
		"OverlayX",
		overlayX,
		-0.22f,
		-0.6f,
		0.6f,
		"Stats overlay position: metres to the right (negative: left) and up (negative: down) of\n"
		"straight ahead, 0.8 m in front of your eyes; it moves with your head"),
	S_FLT(
		"Screen",
		"OverlayY",
		overlayY,
		0.14f,
		-0.6f,
		0.6f,
		nullptr),
	S_FLT(
		"Screen",
		"OverlayDistance",
		overlayDistance,
		0.8f,
		0.3f,
		3.0f,
		"Stats overlay distance in metres; it keeps its size and place in your view (OverlayX/Y scale with it)"),
	S_FLT(
		"Screen",
		"MenuDistance",
		menuDistance,
		1.1f,
		0.4f,
		3.0f,
		"Settings menu distance in metres (placed when it opens); it keeps its size in your view"),

	S_INT(
		"Stereo",
		"Enabled",
		stereo,
		1,
		0,
		1,
		"1 = start in stereo 3D (F12 toggles between stereo and the flat screen)"),
	S_FLT(
		"Stereo",
		"WorldScale",
		worldScale,
		0.75f,
		0.002f,
		50,
		"Game units per metre: scales eye separation and head movement (bigger = world looks smaller)"),
	S_INT(
		"Stereo",
		"LevelCamera",
		levelCamera,
		1,
		0,
		1,
		"1 = keep the horizon level (removes the game camera's up/down tilt; more comfortable)"),
	S_FLT(
		"Stereo",
		"SteepViewFrom",
		steepFrom,
		45,
		10,
		89,
		"...except in steep views looking DOWN (top-down sections): from this camera pitch (degrees) the tilt is\n"
		"kept more and more, fully from SteepViewFull, so the arena is in front of you"),
	S_FLT(
		"Stereo",
		"SteepViewFull",
		steepFull,
		60,
		10,
		90,
		nullptr),  // and at least SteepViewFrom (below)
	S_INT(
		"Stereo",
		"DepthReprojection",
		depthReproject,
		1,
		0,
		1,
		"EXPERIMENTAL, alternating mode only: the eye drawn in the previous game frame is moved to the new\n"
		"camera using its depth"),
	S_INT(
		"Stereo",
		"Synchronized",
		synchronized,
		0,
		0,
		2,
		"Stereo mode: 0 = alternating (one eye per game frame; the other eye\'s last picture is moved to the new\n"
		"camera), 1 = synchronized (both eyes every game frame: they always match, for about twice the GPU work),\n"
		"2 = AFW (one eye per game frame; the other is built from it by its depth, so both show the same moment).\n"
		"F5 cycles through them in game"),
	S_INT(
		"Stereo",
		"PosePrediction",
		posePrediction,
		1,
		0,
		1,
		"1 = predict the head pose each game frame is rendered with for the moment it will actually be\n"
		"shown (measured), so fast head turns show fewer black edges. 0 = off"),
	S_INT(
		"Stereo",
		"LateWarp",
		lateWarp,
		1,
		0,
		1,
		"1 = each headset frame, both eye images are re-aimed to the headset's own pose and cut to each\n"
		"eye's own field of view before they are sent (needed by streaming headsets: Quest, Pico, ...).\n"
		"0 = sent as rendered"),
	S_INT(
		"Stereo",
		"TopDownFlat",
		topDownFlat,
		0,
		0,
		1,
		"1 = show steep top-down views (e.g. the flight-unit shooting sections) on the flat screen\n"
		"instead of in 3D. Shift+F12 in game. Goes flat after 1 s at TopDownFlatFrom degrees or more,\n"
		"back to 3D after 2 s below TopDownFlatUntil."),
	S_FLT(
		"Stereo",
		"TopDownFlatFrom",
		topDownFrom,
		50,
		10,
		89,
		nullptr),
	S_FLT(
		"Stereo",
		"TopDownFlatUntil",
		topDownUntil,
		35,
		0,
		89,
		nullptr),  // and at most TopDownFlatFrom (below)
	S_FLT(
		"Stereo",
		"FovScale",
		fovScale,
		1.1f,
		0.5f,
		2,
		"Rendered field of view relative to the headset's. 1.10 (default) leaves a little picture beyond\n"
		"your view so fast head turns don't show black edges; 1.0 = exact (sharpest)"),
	S_FLT(
		"Stereo",
		"CameraDolly",
		cameraDolly,
		0,
		-5,
		5,
		"Metres to move the camera towards the character along its line of sight: closer = stronger 3D\n"
		"without shrinking the world (Ctrl+F6 / Ctrl+F7 in game)"),
	S_INT(
		"Stereo",
		"FirstPerson",
		firstPerson,
		0,
		0,
		1,
		"1 = first person: you see through the character's eyes. The right stick and your headset turn you.\n"
		"F3 switches it. If their bones aren't found (only 2B's skeleton is known so far), your eyes go to the\n"
		"point the game camera follows instead"),
	S_INT(
		"Stereo",
		"FirstPersonTracking",
		fpTracking,
		1,
		0,
		1,
		"First person: 1 = the character tracks your headset: your real steps move them in the game, their upper\n"
		"body follows your head and their arms your controllers, and their head is hidden from your eyes.\n"
		"0 = the camera starts at their eyes but you walk around freely and see all of them, head included"),
	S_INT(
		"Stereo",
		"SteadyEyes",
		steadyEyes,
		0,
		0,
		1,
		"First person: 1 = the camera follows the character's body, not their head: running bob doesn't move you\n"
		"up and down (jumps do), and animations that swing their head (the double jump's backflip) don't move you\n"
		"sideways. 0 = the camera is exactly at their eyes"),
	S_INT(
		"Stereo",
		"Anamorphic",
		anamorphic,
		1,
		0,
		1,
		"1 = stretch the game image to each eye's own field of view (sharper, no cropping); 0 = off"),
	S_INT(
		"Stereo",
		"RealFrameRate",
		realFrameRate,
		1,
		0,
		1,
		"1 = your VR software is only handed new game pictures, so its own frame counter (e.g. Virtual Desktop's\n"
		"overlay) shows the game's real rate, and its frame synthesis (Virtual Desktop's SSW) can fill in the frames\n"
		"in between. 0 = a picture every headset refresh, the last game picture re-aimed to your head when no new one\n"
		"has come (its counter then always shows the headset's rate)"),
	S_INT(
		"Stereo",
		"AfwMode",
		afwMode,
		0,
		0,
		1,
		"AFW stereo mode only (Synchronized=2): 0 = combined - the drawn eye moved into the other eye\'s view,\n"
		"the edges it cannot see filled from that eye\'s previous picture; 1 = the drawn eye only (edges stretched)"),
	S_INT(
		"Stereo",
		"AsymmetricEyes",
		asymmetricEyes,
		1,
		0,
		2,
		"Your eyes look outwards, so drawing each of them straight ahead needs a picture about a third wider than\n"
		"that eye can see. 1 = turn each eye camera to the middle of its own view instead: the same picture over\n"
		"fewer pixels, so a smaller resolution looks as sharp. 0 = off. 2 = an off-centre lens, which is cleaner\n"
		"geometry but tilts the game's own shadows and glows against the picture (they slide as you move)"),
	S_FLT(
		"Stereo",
		"VerticalFill",
		verticalFill,
		1,
		0,
		1,
		"Fraction of your vertical field of view to render (1 = all; lower = sharper, black top/bottom edges)"),
	S_INT(
		"Stereo",
		"FlatBelowDraws",
		flatBelowDraws,
		250,
		0,
		100000,
		"Frames with fewer draw calls than this are shown flat (title and New Game screens have up to ~190,\n"
		"gameplay 500+). 0 = never"),

	S_INT(
		"Render",
		"ExtraResolutions",
		extraResolutions,
		1,
		0,
		2,
		"1 = offer resolutions in your headset's shape (50%-100% of its full detail, see HeadsetWidth) in the\n"
		"game's settings; 2 = also rewrite the game's built-in resolution list (try if 1 shows nothing new).\n"
		"Use the game's windowed/borderless mode with them. See the README."),
	S_INT(
		"Render",
		"HeadsetWidth",
		headsetWidth,
		2160,
		640,
		16384,
		"The game resolution that looks native in your headset (its full detail). It is larger than the\n"
		"resolution your VR runtime asks for, because the game picture also covers a margin around your\n"
		"view (FovScale) and the wider side of each eye's view. The game's resolution list offers 50%-100%\n"
		"of it (your headset's shape, lighter sizes for slower graphics cards) when ExtraResolutions is on,\n"
		"and DLSS enlarges to it. Written automatically each time the headset is used, so the list fits it\n"
		"from the next start"),
	S_INT(
		"Render",
		"HeadsetHeight",
		headsetHeight,
		2304,
		480,
		16384,
		nullptr),
	S_INT(
		"Render",
		"AmbientOcclusion",
		ambientOcclusion,
		1,
		0,
		1,
		"0 = turn off ambient occlusion (NieR's half-resolution SAO: blotchy/pixelated on things close to\n"
		"the camera, e.g. 2B with CameraDolly)"),
	S_INT(
		"Render",
		"Shadows",
		shadows,
		1,
		0,
		1,
		"0 = skip the game's screen-space shadow pass (no shadows from the sun and lights): faster"),
	S_INT(
		"Render",
		"Vignette",
		vignette,
		0,
		0,
		1,
		"0 = remove the game's vignette (darker screen edges: in VR they show as a dark frame)"),
	S_INT(
		"Render",
		"LowHealthEffect",
		lowHealthFx,
		0,
		0,
		1,
		"0 = no grey colour filter at low health while in stereo (1 = keep the game's effect)"),
	S_INT(
		"Render",
		"DistortionEffects",
		distortionFx,
		0,
		0,
		1,
		"0 = no screen distortion/glitch effects while in stereo (when hit, at low health): they redraw\n"
		"a flat copy of the whole screen. 1 = keep them (drawn into the 3D image)"),
	S_INT(
		"Render",
		"GIVolumes",
		giVolumes,
		128,
		8,
		128,
		"Global illumination volumes that stay on, nearest to the camera first: fewer = faster. 128 = all\n"
		"(no visual change; empty slots are still skipped). Idea from the FAR mod (DrDaxxy, Kaldaien)"),
	S_INT(
		"Render",
		"MaxFrameLatency",
		maxFrameLatency,
		1,
		0,
		3,
		"Frames the game may prepare ahead of the graphics card. Each one delays what you see after a\n"
		"head turn by a frame. 1 = lowest delay (default), 2-3 = a little smoother, 0 = the game's own"),
	S_FLT(
		"Render",
		"CullingFov",
		cullFov,
		170,
		0,
		170,
		"Vertical field of view (degrees) the game uses to decide what to draw while in stereo, so things\n"
		"at the edges of your view and where you look don't vanish. 0 = the game's own (~50).\n"
		"At most 170: a field of view of 180 or more doesn't exist (170 covers ~88 deg to each side)."),
	S_INT(
		"Render",
		"HeadCulling",
		headCulling,
		1,
		0,
		1,
		"1 = the culling view (above) is turned the way your headset looks, not where the game camera\n"
		"points, so turning your head doesn't make things vanish. Ctrl+Shift+F5 switches it in game"),
	S_INT(
		"Render",
		"HeadShadows",
		headShadows,
		1,
		0,
		1,
		"1 = the sun's shadows are worked out for where your headset looks, not where the game camera\n"
		"points, so they don't go missing when you turn your head. Ctrl+Shift+F4 switches it in game"),
	S_FLT(
		"Render",
		"ShadowLeadMs",
		shadowLeadMs,
		30,
		0,
		100,
		"HeadShadows: the shadows are aimed this many milliseconds ahead of a turning head, so the edge of where\n"
		"they are worked out stays outside your view on quick turns (0 = where the head was last seen)"),
	S_FLT(
		"Render",
		"ShadowExtraFov",
		shadowExtraFov,
		0,
		0,
		30,
		"HeadShadows: degrees added to how far around your view the sun's shadows reach. More reach spreads the\n"
		"same shadow detail over a bigger area, so every shadow gets a little softer (10 = about a third softer)"),
	S_TEXT(
		"Render",
		"FovRealCallers",
		"0x729b49",
		"Places in the game (exe offsets, comma separated) that always get the game's own field of view:\n"
		"0x729b49 = the camera code that also aims Pod 042 (the wide view made it shoot at the sky)"),
	S_INT(
		"Render",
		"ReuseShadows",
		reuseShadows,
		1,
		0,
		1,
		"Synchronized mode: the right eye reuses the left eye's shadow map (saves graphics work; 0 = draw it again)"),
	S_FLT(
		"Render",
		"HeadCullingMargin",
		headCullMargin,
		20,
		0,
		60,
		"With HeadCulling=1 the culling view only needs to hold your headset's view: this many degrees\n"
		"are added on every side (more = safer when turning your head fast, less = faster).\n"
		"0 = use CullingFov as before."),

	S_INT(
		"Render",
		"FoveatedShading",
		foveated,
		1,
		0,
		2,
		"The edges of each eye's 3D image are shaded at a lower rate, where the lenses blur them anyway: faster.\n"
		"0 = off, 1 = subtle (full detail within 30 degrees of the centre), 2 = strong (full within 22, then half,\n"
		"a quarter beyond 35). Needs an NVIDIA RTX or GTX 16 card (variable rate shading); does nothing on others"),
	S_INT(
		"Upscaler",
		"Mode",
		upscaler,
		0,
		0,
		1,
		"Experimental. Sharper, smoother 3D image at a lower game resolution: the game renders smaller and\n"
		"each eye's image is enlarged with detail gathered over several frames (anti-aliasing included).\n"
		"0 = off, 1 = NVIDIA DLSS (RTX graphics cards; nvngx_dlss.dll must be in the game's folder).\n"
		"The game's resolution picks the DLSS mode (see Output below); the stats overlay shows both sizes"),
	S_INT(
		"Upscaler",
		"Quality",
		upscaleQuality,
		1,
		0,
		3,
		"How much DLSS enlarges the picture, which is what decides how hard the graphics card works:\n"
		"0 = sharpest (no enlargement, DLAA), 1 = quality, 2 = balanced, 3 = fastest. With PanelResolution=1\n"
		"the mod works out the resolution the game has to render for this and sets it itself"),
	S_INT(
		"Render",
		"GameWidth",
		gameWidth,
		0,
		0,
		16384,
		"The size the game last rendered at, remembered by the mod. It is always offered in the game's own\n"
		"resolution list, so the size the game has saved can never disappear from it - a game that cannot find\n"
		"its own resolution in the list crashes. Written automatically; 0 = nothing remembered yet"),
	S_INT(
		"Render",
		"GameHeight",
		gameHeight,
		0,
		0,
		16384,
		"Height for GameWidth above"),
	S_INT(
		"Render",
		"SunGlare",
		sunGlare,
		1,
		0,
		1,
		"1 = keep the game\'s glare: the bright bloom around the sun. 0 = remove it. This is NOT the lens flare (the\n"
		"rings and streaks across the picture), which is a separate effect"),
	S_INT(
		"Render",
		"LensFlare",
		lensFlare,
		0,
		0,
		1,
		"0 = remove the lens flare: the rings and streaks across the picture when you look towards the sun (in a\n"
		"headset they can\'t be seen in 3D, and in AFW mode they flicker). 1 = keep it"),
	S_INT(
		"Render",
		"GameVsync",
		gameVsync,
		0,
		0,
		1,
		"0 = the game's V-sync is ignored: its desktop window never waits for the monitor, so the monitor's\n"
		"refresh can't hold back the headset's frame rate. 1 = use the game's own V-sync setting"),
	S_INT(
		"Render",
		"GameAntiAliasing",
		gameAntiAliasing,
		0,
		0,
		1,
		"0 = the mod sets the game's own anti-aliasing to off at start (DLSS smooths the edges better, and with\n"
		"the game's MSAA DLSS cannot run). 1 = leave the game's setting alone"),
	S_INT(
		"Render",
		"GameMotionBlur",
		gameMotionBlur,
		0,
		0,
		1,
		"0 = the mod sets the game's motion blur to off at start (it smears the picture in a headset).\n"
		"1 = leave the game's setting alone"),
	S_INT(
		"Render",
		"GameFrameRate",
		gameFrameRate,
		0,
		0,
		6,
		"EXPERIMENTAL. The game's frame rate: 0 = its own 60 (default), 1 = 72, 2 = 80, 3 = 90, 4 = 120, 5 = the\n"
		"headset's own rate, in step with it (no hitches: set the headset to a rate your graphics card holds),\n"
		"6 = unlimited (as fast as your graphics card goes, up to 250; suits the alternating modes, one eye per frame).\n"
		"Above 60 some things in the game may run too fast; back up your saves first"),
	S_INT(
		"Render",
		"GameSpeedFix",
		gameSpeedFix,
		1,
		0,
		1,
		"With GameFrameRate above: 1 = the game moves at its normal speed (each frame advances it by the time the\n"
		"frame really took). 0 = each frame still advances it a 60th of a second, so everything runs faster (a test)"),
	S_INT(
		"Render",
		"GameStepFix",
		gameStepFix,
		1,
		0,
		1,
		"With GameFrameRate above: 1 = the game's own frame timing (animation, movement) follows the real frame\n"
		"time. 0 = it never goes below a 60th, so 2B's running and other animations play too fast (a test)"),
	S_INT(
		"Render",
		"FrameTimers",
		frameTimers,
		1,
		0,
		1,
		"With GameFrameRate above 60: 1 = the game's timers that count a 60th of a second per frame (67 found in\n"
		"its code) follow the real time. 0 = left alone (they run faster). Read at start"),
	S_INT(
		"Render",
		"FrameConstants",
		frameConstants,
		1,
		0,
		1,
		"With GameFrameRate above 60: 1 = fixed per-frame amounts, easing and fades in the game's code (camera, 2B,\n"
		"Pod, bullets, enemies, items, hacking - about 110 found) follow the real time. 0 = left alone. Read at start"),
	S_INT(
		"Render",
		"ScatterPerEye",
		scatterPerEye,
		1,
		0,
		1,
		"1 = the haze and glow around the sun (light scattering) is placed for each eye. The game gives it the\n"
		"sun's direction as seen from its own camera, so without this the glow sits where the game camera would\n"
		"see the sun: it vanishes when you turn your head and lands at the same spot in both eyes. 0 = the game's"),
	S_TEXT(
		"Render",
		"LensFlareTextures",
		"0",
		"More lens flare textures, as 3Dmigoto texture hashes (comma separated), for LensFlare=0 above. The 17 that\n"
		"HelixMod\'s 3D fix lists are built in; 0 = just those"),
	S_INT(
		"Render",
		"PanelResolution",
		panelResolution,
		1,
		0,
		1,
		"1 = the mod sets the game's own resolution from Output and Quality above (Resolution and DLSS quality in\n"
		"the panel) by writing it into the game's saved settings, SystemData.dat in My Games under Documents (a\n"
		"copy of the original is kept next to it). The game picks it up when it next starts. 0 = the game's own\n"
		"setting stands"),
	S_FLT(
		"Upscaler",
		"Output",
		upscaleOutput,
		100,
		50,
		150,
		"Size of the upscaled image, in % of the detail your headset recommends (100 = its own). It stays\n"
		"the same; as in other games, the game's resolution decides the DLSS mode: the same size = DLAA,\n"
		"1.5x smaller = Quality, 1.7x = Balanced, 2x = Performance, 3x = Ultra performance. The Info page\n"
		"lists the game resolutions for each mode"),
	S_INT(
		"Upscaler",
		"Model",
		dlssModel,
		0,
		0,
		1,
		"DLSS model: 0 = fast (CNN, preset E: about 1 ms per eye on an RTX 2070), 1 = best (transformer,\n"
		"preset K: about 3 ms per eye on an RTX 2070, cheaper on RTX 30/40/50 cards)"),

	S_HEX(
		"Keys",
		"Recenter",
		keyRecenter,
		VK_F8,
		"Windows virtual-key codes (https://learn.microsoft.com/windows/win32/inputdev/virtual-key-codes)\n"
		"Defaults: F8 recenter, F9 smaller, F10 larger, F6 closer, F7 farther, F12 stereo on/off\n"
		"Ctrl+F9 / Ctrl+F10: world bigger (flatter) / smaller (deeper) - WorldScale, saved automatically\n"
		"Ctrl+F6 / Ctrl+F7: camera closer to / farther from the character - CameraDolly, saved automatically"),
	S_HEX(
		"Keys",
		"Smaller",
		keySmaller,
		VK_F9,
		nullptr),
	S_HEX(
		"Keys",
		"Larger",
		keyLarger,
		VK_F10,
		nullptr),
	S_HEX(
		"Keys",
		"Closer",
		keyCloser,
		VK_F6,
		nullptr),
	S_HEX(
		"Keys",
		"Farther",
		keyFarther,
		VK_F7,
		nullptr),
	S_HEX(
		"Keys",
		"ToggleStereo",
		keyStereo,
		VK_F12,
		nullptr),
	S_HEX(
		"Keys",
		"FirstPerson",
		keyFirstPerson,
		VK_F3,
		"First person on/off (prototype, F3)"),
	S_HEX(
		"Keys",
		"Menu",
		keyMenu,
		VK_INSERT,
		"Opens and closes the in-headset settings menu (default Insert = 0x2D; 0 = no key)"),

	S_PATH(
		"OpenXR",
		"LoaderPath",
		loaderPath,
		"Leave empty for automatic: uses openxr_loader.dll if it sits next to this mod\n"
		"(enables API layers such as OpenXR Toolkit), otherwise talks to the active runtime directly."),

	S_PATH(
		"Loader",
		"ChainLoad",
		chainLoad,
		"Another d3d11.dll-based mod (e.g. ReShade renamed to d3d11_reshade.dll) to load in a chain.\n"
		"Leave empty to use Windows' own d3d11.dll."),

	S_INT(
		"Debug",
		"Verbose",
		verbose,
		0,
		0,
		1,
		"1 = more detail in NieRVR.log"),
	S_HEX(
		"Debug",
		"CaptureKey",
		keyCapture,
		VK_F11,
		"Camera probe: press this key in game to record one frame for analysis (default F11).\n"
		"Ctrl+this key writes the game's decrypted code to NieRVR_code_dump.exe, for disassembly"),
	S_INT(
		"Debug",
		"Probe",
		probe,
		1,
		0,
		1,
		"0 = disable the capture key"),
};
#undef S_INT
#undef S_HEX
#undef S_FLT
#undef S_PATH
#undef S_TEXT

// Helpers
void Widen(const char* text, wchar_t* out, int cap) {
	int i = 0;
	for (; text[i] && i < cap - 1; i++) out[i] = (wchar_t)(unsigned char)text[i];
	out[i] = 0;
}

float Clamp(float value, float lo, float hi) { return value < lo ? lo : (value > hi ? hi : value); }

// "2.500" -> "2.5", "50.000" -> "50"
void FormatFloat(char* buf, size_t cap, float value) {
	snprintf(buf, cap, "%.3f", (double)value);
	size_t length = strlen(buf);
	while (length > 0 && buf[length - 1] == '0') buf[--length] = 0;
	if (length > 0 && buf[length - 1] == '.') buf[--length] = 0;
	if (!strcmp(buf, "-0")) snprintf(buf, cap, "0");
}

// A number's text as written to the ini.
void ValueText(const Setting& setting, float value, char* buf, size_t cap) {
	switch (setting.kind) {
		case K_INT: snprintf(buf, cap, "%d", (int)value); break;
		case K_HEX: snprintf(buf, cap, "0x%X", (unsigned)(int)value); break;
		case K_FLOAT: FormatFloat(buf, cap, value); break;
		default: snprintf(buf, cap, "%s", setting.text ? setting.text : ""); break;
	}
}

bool ReadRaw(const char* section, const char* key, wchar_t* buf, int cap) {
	wchar_t sectionW[64], keyW[64];
	Widen(section, sectionW, 64);
	Widen(key, keyW, 64);
	GetPrivateProfileStringW(sectionW, keyW, L"\x01", buf, cap, g_iniPath);
	return !(buf[0] == 1 && buf[1] == 0);  // false: the key isn't in the file
}

void WriteRaw(const char* section, const char* key, const char* value) {
	wchar_t sectionW[64], keyW[64], valueW[256];
	Widen(section, sectionW, 64);
	Widen(key, keyW, 64);
	Widen(value, valueW, 256);
	WritePrivateProfileStringW(sectionW, keyW, valueW, g_iniPath);
}

// A number from the ini: whole numbers (also hex, 0x..) or decimals ("0.8" for a switch rounds to 1).
bool ReadNumber(const char* section, const char* key, float* out) {
	wchar_t buf[64];
	if (!ReadRaw(section, key, buf, 64)) return false;
	wchar_t* end = nullptr;
	if (wcschr(buf, L'.')) {
		float value = wcstof(buf, &end);  // the C locale: always a '.' decimal point
		if (end == buf) return false;
		*out = value;
	} else {
		long value = wcstol(buf, &end, 0);  // base 0: "0x77" is hex
		if (end == buf) return false;
		*out = (float)value;
	}
	return true;
}

void WriteNumber(const char* section, const char* key, Kind kind, float value) {
	Setting setting = {section, key, kind, nullptr, 0, 0, 0, nullptr, nullptr};
	char buf[32];
	ValueText(setting, value, buf, sizeof(buf));
	WriteRaw(section, key, buf);
}

// The ini file
// Writes NieRVR.ini with every setting at its default, grouped by section, comments included.
void WriteDefaultIni() {
	static char text[16384];
	size_t used = 0;
	auto put = [&](const char* setting) {
		size_t length = strlen(setting);
		if (used + length < sizeof(text)) memcpy(text + used, setting, length), used += length;
	};
	put("; NieRVR configuration. Delete this file to restore defaults.\r\n");
	const char* section = "";
	for (const Setting& setting : kSettings) {
		if (strcmp(section, setting.section)) {
			section = setting.section;
			put("\r\n[");
			put(section);
			put("]\r\n");
		}
		for (const char* c = setting.comment; c && *c;) {
			const char* lineEnd = c;
			while (*lineEnd && *lineEnd != '\n') lineEnd++;
			char line[200];
			size_t length = (size_t)(lineEnd - c) < sizeof(line) - 1 ? (size_t)(lineEnd - c) : sizeof(line) - 1;
			memcpy(line, c, length);
			line[length] = 0;
			put("; ");
			put(line);
			put("\r\n");
			c = *lineEnd ? lineEnd + 1 : lineEnd;
		}
		char val[64];
		ValueText(setting, setting.defaultValue, val, sizeof(val));
		put(setting.key);
		put("=");
		put(val);
		put("\r\n");
	}
	HANDLE file = CreateFileW(g_iniPath, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) return;
	DWORD written;
	WriteFile(file, text, (DWORD)used, &written, nullptr);
	CloseHandle(file);
}

// An ini from an older version keeps working, but lacks the settings added since: add each missing
// key with its default (without the comments). Returns how many were added.
int AddMissingSettings() {
	int added = 0;
	for (const Setting& setting : kSettings) {
		wchar_t have[8];
		if (ReadRaw(setting.section, setting.key, have, 8)) continue;
		char val[64];
		ValueText(setting, setting.defaultValue, val, sizeof(val));
		WriteRaw(setting.section, setting.key, val);
		added++;
	}
	return added;
}

// "0x729b49, 0x123" -> offsets
void ParseHexList(const wchar_t* buf, uint32_t* out, int* count, int max) {
	*count = 0;
	for (int i = 0; buf[i] && *count < max;) {
		while (buf[i] == ' ' || buf[i] == ',' || buf[i] == ';') i++;
		if (buf[i] == '0' && (buf[i + 1] == 'x' || buf[i + 1] == 'X')) i += 2;
		uint32_t value = 0;
		int digits = 0;
		for (;; i++, digits++) {
			wchar_t c = buf[i];
			int digit = c >= '0' && c <= '9' ? c - '0' : (c >= 'a' && c <= 'f' ? c - 'a' + 10 : (c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1));
			if (digit < 0) break;
			value = value * 16 + (unsigned)digit;
		}
		if (digits && value) out[(*count)++] = value;
		if (!digits && buf[i]) i++;  // skip anything else
	}
}

const Setting* Find(const void* field) {
	for (const Setting& setting : kSettings)
		if (setting.field && setting.field == field) return &setting;
	return nullptr;
}
}  // namespace

// API
void ConfigLoad() {
	if (GetFileAttributesW(g_iniPath) == INVALID_FILE_ATTRIBUTES) WriteDefaultIni();
	g_cfg.iniAdded = AddMissingSettings();

	for (const Setting& setting : kSettings) {
		if (setting.kind == K_PATH) {
			wchar_t sectionW[64], keyW[64];
			Widen(setting.section, sectionW, 64);
			Widen(setting.key, keyW, 64);
			GetPrivateProfileStringW(sectionW, keyW, L"", (wchar_t*)setting.field, MAX_PATH, g_iniPath);
			continue;
		}
		if (setting.kind == K_TEXT) continue;
		float value;
		if (!ReadNumber(setting.section, setting.key, &value)) value = setting.defaultValue;
		value = Clamp(value, setting.min, setting.max);
		if (setting.kind == K_FLOAT)
			*(float*)setting.field = value;
		else
			*(int*)setting.field = (int)(value + (value >= 0 ? 0.5f : -0.5f));
	}

	// limits that depend on another setting
	g_cfg.steepFull = Clamp(g_cfg.steepFull, g_cfg.steepFrom, 90);
	g_cfg.topDownUntil = Clamp(g_cfg.topDownUntil, 0, g_cfg.topDownFrom);

	// settings with their own format
	wchar_t buf[200];
	if (!ReadRaw("Render", "FovRealCallers", buf, 200)) Widen("0x729b49", buf, 200);
	ParseHexList(buf, g_cfg.fovRealCallers, &g_cfg.fovRealCount, 8);
	g_cfg.lensFlareExtraCount = 0;
	if (ReadRaw("Render", "LensFlareTextures", buf, 200)) ParseHexList(buf, g_cfg.lensFlareExtra, &g_cfg.lensFlareExtraCount, 16);
	// the camera-easing fov reader found in game; defaults to the Steam build's (checked in game:
	// a wrong one is dropped and searched again)
	uint32_t easing[1];
	int count = 0;
	if (ReadRaw("Render", "FovEasingCaller", buf, 200)) ParseHexList(buf, easing, &count, 1);
	g_cfg.fovEasingCaller = count ? easing[0] : 0x7397de;
}

// Writes one setting of g_cfg (pass its field: &g_cfg.worldScale) to NieRVR.ini.
bool ConfigSave(const void* field) {
	const Setting* setting = Find(field);
	if (!setting || setting->kind == K_PATH || setting->kind == K_TEXT) return false;
	float value = setting->kind == K_FLOAT ? *(const float*)field : (float)*(const int*)field;
	WriteNumber(setting->section, setting->key, setting->kind, value);
	return true;
}

bool ConfigDefault(const void* field, float* value) {
	const Setting* setting = Find(field);
	if (!setting || setting->kind == K_PATH || setting->kind == K_TEXT) return false;
	*value = setting->defaultValue;
	return true;
}

// The camera-easing fov reader found in game (offset in the exe), so the next start skips the search.
void ConfigSaveEasingCaller(uint32_t offset) {
	char text[24];
	snprintf(text, sizeof(text), "0x%x", offset);
	WriteRaw("Render", "FovEasingCaller", text);
}
