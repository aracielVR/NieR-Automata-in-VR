// menu.cpp - the in-headset settings menu.
#include "ui/menu.h"
#include "render/game_resolution.h"
#include "core/locks.h"
#include "core/config.h"
#include "core/log.h"
#include "render/dlss.h"
#include <atomic>
#include <iterator>
#include <stdio.h>
#include <string.h>
#include "input/gamepad.h"
#include "ui/imgui_panel.h"
#include "imgui.h"
#include "imgui_internal.h"  // FocusItem

namespace {
// Settings
enum ItemType { T_BOOL,
				T_INT,
				T_FLOAT,
				T_ACTION,
				T_TEXT,
				T_CHOICE };  // T_CHOICE: format holds the names, "Off|On"
enum ItemFlags { F_RESTART = 1,
				 F_LOG = 2,
				 F_RECENTER = 4,
				 F_ALTERNATING = 8,
				 F_SYNCHRONIZED = 16 };  // ALTERNATING/SYNCHRONIZED: only used in that mode
constexpr DWORD kStickHoldMs = 600;  // both sticks held this long opens or closes the panel
enum ItemAction { A_RECENTER = 1,
				  A_SYNCHRONIZED_STATS,
				  A_DLSS_FILE };
struct Item {
	const char* label;
	ItemType type;
	void* value;     // the g_cfg field (its ini section and key come from config.cpp)
	float min, max;  // slider range
	const char* format;
	int32_t flags;
	const char* help;
};

Item kView[] = {
	{
		"Stereo mode",
		T_CHOICE,
		&g_cfg.synchronized,
		0,
		2,
		"Alternating|Synchronized|AFW",
		0,
		"Alternating draws one eye per game frame and moves the other eye's last picture to the new camera. "
		"Synchronized draws both, so they always match, for about twice the graphics work. AFW draws one eye per frame "
		"and builds the other from it by its depth, so both show the same moment. Also F5 (cycles).",
	},
	{
		"Show VR software the real frame rate",
		T_BOOL,
		&g_cfg.realFrameRate,
		0,
		1,
		nullptr,
		0,
		"Off: the mod hands your VR software a picture every headset refresh, re-aiming the last game picture to your "
		"head when there is no new one - so Virtual Desktop's overlay always shows the headset's rate. On: only new game "
		"pictures are handed over, so its overlay shows the game's real rate, and with SSW turned on in Virtual Desktop "
		"it fills in the frames in between itself.",
	},
	{
		"World scale",
		T_FLOAT,
		&g_cfg.worldScale,
		0.2f,
		3.0f,
		"%.3f",
		F_LOG,
		"Eye distance in the game world. Lower = the world looks bigger and the 3D flatter; higher = smaller, "
		"deeper. 0.75 is the default. Also Ctrl+F9 / Ctrl+F10.",
	},
	{
		"Camera dolly",
		T_FLOAT,
		&g_cfg.cameraDolly,
		-3.0f,
		3.0f,
		"%.2f m",
		0,
		"Moves the camera along its line of sight towards the character (positive) for stronger 3D. Also Ctrl+F6 / Ctrl+F7.",
	},
	{
		"First person",
		T_BOOL,
		&g_cfg.firstPerson,
		0,
		1,
		nullptr,
		0,
		"You see through the character's eyes; the right stick and your headset turn you. Also F3.",
	},
	{
		"Enable tracking",
		T_BOOL,
		&g_cfg.fpTracking,
		0,
		1,
		nullptr,
		0,
		"Your real steps move the character in the game, their upper body follows your head and their arms your "
		"controllers, and their head is hidden from your eyes. Where they can't go (a wall), you step out of them. "
		"Off: walk around freely and see all of them.",
	},
	{
		"Steady eyes",
		T_BOOL,
		&g_cfg.steadyEyes,
		0,
		1,
		nullptr,
		0,
		"First person: the camera follows the character's body, not their head: running bob doesn't move you up and "
		"down, and animations that swing their head (the double jump's backflip) don't move you sideways. Off: the "
		"camera is exactly at their eyes.",
	},
	{
		"Level the horizon",
		T_BOOL,
		&g_cfg.levelCamera,
		0,
		1,
		nullptr,
		0,
		"Removes the game camera's up/down tilt so the horizon stays level (comfort). Steep views looking down "
		"(top-down sections) keep their tilt (see below). Off: the right stick tilts your view up and down.",
	},
	{
		"Keep tilt from",
		T_FLOAT,
		&g_cfg.steepFrom,
		10,
		89,
		"%.0f deg",
		0,
		"Camera pitch where the game's tilt starts to be kept (top-down sections).",
	},
	{
		"Keep full tilt at",
		T_FLOAT,
		&g_cfg.steepFull,
		10,
		90,
		"%.0f deg",
		0,
		"Camera pitch from which the game's tilt is kept completely.",
	},
	{
		"Top-down views flat",
		T_BOOL,
		&g_cfg.topDownFlat,
		0,
		1,
		nullptr,
		0,
		"Shows steep top-down views (flight unit shooting sections) on the flat screen instead of in 3D. Also Shift+F12.",
	},
	{
		"Rendered field of view",
		T_FLOAT,
		&g_cfg.fovScale,
		0.8f,
		1.4f,
		"%.2fx",
		0,
		"More than 1 draws a margin around your view: fewer black edges when you turn your head quickly or the "
		"game falls behind. Less is faster: every pixel of the game and the upscaler costs. 1.10 is the default.",
	},
	{
		"Fit the lens to each eye",
		T_BOOL,
		&g_cfg.asymmetricEyes,
		0,
		1,
		nullptr,
		0,
		"Your eyes look outwards, so drawing each one straight ahead needs a picture about a third wider than that eye "
		"can see. This turns each eye camera to the middle of its own view instead, which spends those pixels on what "
		"you actually see: pick a resolution a step or two lower for the same sharpness, and the game draws fewer pixels.",
	},
	{
		"Recenter",
		T_ACTION,
		(void*)A_RECENTER,
		0,
		0,
		nullptr,
		0,
		"Places the screen and the view straight ahead of you again (also F8).",
	},
};
Item kRender[] = {
	{
		"Foveated shading",
		T_CHOICE,
		&g_cfg.foveated,
		0,
		2,
		"Off|Subtle|Strong",
		0,
		"Shades the edges of each eye's 3D image at a lower rate, where the lenses blur them anyway: faster. Subtle keeps "
		"full detail within 30 degrees of the centre; Strong within 22, with a quarter of the detail at the far edges. "
		"Needs an NVIDIA RTX or GTX 16 card.",
	},
	{
		"Game frame rate (experimental)",
		T_CHOICE,
		&g_cfg.gameFrameRate,
		0,
		6,
		"60 (the game's own)|72|80|90|120|Headset rate|Unlimited",
		0,
		"Lets the game run faster than its 60 frames a second, so the world moves more smoothly in the headset. Some "
		"things in the game may run too fast at a higher rate: back up your saves first. Your graphics card must also "
		"keep up: if the rate shown in the headset stays lower, pick a lower rate or a lower resolution. Headset rate: the "
		"game runs at exactly your headset's refresh rate and in step with it, so no hitches - set the headset to a rate "
		"your graphics card can hold (72 or 80 Hz in Virtual Desktop, or 90 with the OFXR bridge). Unlimited: as fast as "
		"your graphics card goes (up to 250) - for the alternating modes, where each frame draws only one eye.",
	},
	{
		"Keep game speed",
		T_BOOL,
		&g_cfg.gameSpeedFix,
		0,
		1,
		nullptr,
		0,
		"With a higher frame rate: the game moves at its normal speed. Off: everything runs faster, as the game expects "
		"exactly 60 frames a second (to compare).",
	},
	{
		"Keep animation speed",
		T_BOOL,
		&g_cfg.gameStepFix,
		0,
		1,
		nullptr,
		0,
		"With a higher frame rate: 2B's running and other animations play at their normal speed. Off: they play too "
		"fast (to compare).",
	},
	{
		"Upscaler (experimental)",
		T_CHOICE,
		&g_cfg.upscaler,
		0,
		1,
		"Off|DLSS",
		0,
		"DLSS turns the game's image into a sharp one at the size your headset wants, without jagged, shimmering edges. "
		"The mod sets the resolution the game renders for you, from the two settings below - nothing to change in the "
		"game's own settings. Needs an NVIDIA RTX card and nvngx_dlss.dll in the game's folder.",
	},
	{
		"DLSS quality",
		T_CHOICE,
		&g_cfg.upscaleQuality,
		0,
		3,
		"Sharpest|Quality|Balanced|Fastest",
		F_RESTART,
		"How much work the graphics card does, without changing the Resolution you get: Sharpest draws every pixel "
		"(DLSS only cleans up the edges); the others draw fewer and let DLSS fill them in - Fastest draws a quarter as "
		"many. The mod sets the game's own resolution to match; it takes effect when the game next starts.",
	},
	{
		"DLSS file",
		T_ACTION,
		(void*)A_DLSS_FILE,
		0,
		0,
		nullptr,
		0,
		"Download nvngx_dlss.dll from the DLSS repo, since apparently im not allowed to ship it. It will get placed next to the exe is about 60 mb",
	},
	{
		"Resolution",
		T_FLOAT,
		&g_cfg.upscaleOutput,
		50,
		150,
		"%.0f%% of the headset's detail",
		F_RESTART,
		"How sharp the picture in your headset is, as a share of the detail it asks for (100% = its own; more is "
		"supersampling). Independent of DLSS quality: changing one never changes the other. Takes effect when the "
		"game next starts.",
	},
	{
		"DLSS model",
		T_CHOICE,
		&g_cfg.dlssModel,
		0,
		1,
		"Fast (CNN)|Best (transformer)",
		0,
		"Fast: about 1 ms per eye on an RTX 2070. Best: sharper in motion, about 3 ms per eye on an RTX 2070 (much "
		"less on RTX 30 cards and newer).",
	},
	{
		"Ambient occlusion",
		T_BOOL,
		&g_cfg.ambientOcclusion,
		0,
		1,
		nullptr,
		0,
		"NieR's ambient occlusion (soft contact shadows). Off removes it (it is half resolution and can look blotchy "
		"up close).",
	},
	{
		"Shadows",
		T_BOOL,
		&g_cfg.shadows,
		0,
		1,
		nullptr,
		0,
		"Off skips the game's screen-space shadow pass (no shadows from the sun and lights): faster.",
	},
	{
		"Vignette",
		T_BOOL,
		&g_cfg.vignette,
		0,
		1,
		nullptr,
		0,
		"The game's darkened screen edges. In VR they look like a dark frame, so off by default.",
	},
	{
		"Low-health effect",
		T_BOOL,
		&g_cfg.lowHealthFx,
		0,
		1,
		nullptr,
		0,
		"The grey filter and ripples at low health while in stereo.",
	},
	{
		"Screen distortions",
		T_BOOL,
		&g_cfg.distortionFx,
		0,
		1,
		nullptr,
		0,
		"The glitch/distortion effects when hit. Off removes them in stereo (they redraw a flat copy of the screen).",
	},
	{
		"Frame queue",
		T_INT,
		&g_cfg.maxFrameLatency,
		0,
		3,
		"%d",
		0,
		"How many frames the game may prepare ahead of the graphics card. Each one delays what you see after a head "
		"turn by a frame. 1 = lowest delay (default); 2-3 may run a little faster; 0 = the game's own (3).",
	},
	{
		"Global illumination volumes",
		T_INT,
		&g_cfg.giVolumes,
		8,
		128,
		"%d",
		0,
		"How many GI volumes (the boxes that give places their reflections and ambient light) stay on, nearest to you "
		"first. Fewer = faster. 128 = all: no visual change, only empty slots are skipped. The Info page shows how many "
		"the area uses and the GI pass time. Idea from the FAR mod.",
	},
	{
		"Reuse shadows (synchronized)",
		T_BOOL,
		&g_cfg.reuseShadows,
		0,
		1,
		nullptr,
		F_SYNCHRONIZED,
		"Synchronized mode only: the right eye reuses the left eye's shadow map instead of drawing it again (saves graphics work).",
	},
	{
		"Flat below draws",
		T_INT,
		&g_cfg.flatBelowDraws,
		0,
		1000,
		"%d",
		0,
		"Frames with fewer draws than this are shown on the flat screen (title and loading screens).",
	},
	{
		"Extra resolutions",
		T_INT,
		&g_cfg.extraResolutions,
		0,
		2,
		"%d",
		F_RESTART,
		"Adds resolutions in your headset's shape to the game's list (1), and also patches its table (2).",
	},
};
Item kScreen[] = {
	{
		"Steady flat screen",
		T_BOOL,
		&g_cfg.screenInEyes,
		0,
		1,
		nullptr,
		0,
		"Draws the flat screen (menus, loading screens, the title) into both of your eyes as a window hanging in the "
		"room, so it stays still while you turn your head. Off hands it to your VR software instead, which costs less "
		"but can smear and shear it as you turn.",
	},
	{
		"Hide black bars",
		T_BOOL,
		&g_cfg.cropBlackBars,
		0,
		1,
		nullptr,
		0,
		"No black bands: films and menus show on a screen trimmed to the picture, and 3D cutscenes fill your view "
		"without the dark bands above and below.",
	},
	{
		"Screen distance",
		T_FLOAT,
		&g_cfg.distance,
		0.3f,
		10,
		"%.2f m",
		F_RECENTER,
		"Distance of the flat screen and the HUD (also F6 / F7).",
	},
	{
		"Screen width",
		T_FLOAT,
		&g_cfg.width,
		0.2f,
		10,
		"%.2f m",
		0,
		"Width of the flat screen and the HUD (also F9 / F10).",
	},
	{
		"Screen height",
		T_FLOAT,
		&g_cfg.heightOffset,
		-2,
		2,
		"%.2f m",
		F_RECENTER,
		"Moves the flat screen and the HUD up or down.",
	},
	{
		"Stats overlay",
		T_BOOL,
		&g_cfg.statsOverlay,
		0,
		1,
		nullptr,
		0,
		"Live numbers in the corner of your view (placed with the two settings below): frame rates, the game's resolution and how sharp it is "
		"compared with what the headset recommends, and the head-pose delay.",
	},
	{
		"Overlay left / right",
		T_FLOAT,
		&g_cfg.overlayX,
		-0.6f,
		0.6f,
		"%.2f m",
		0,
		"Moves the stats overlay sideways: negative = left (metres at 0.8 m away). It moves with your head.",
	},
	{
		"Overlay up / down",
		T_FLOAT,
		&g_cfg.overlayY,
		-0.6f,
		0.6f,
		"%.2f m",
		0,
		"Moves the stats overlay up (positive) or down.",
	},
	{
		"Overlay distance",
		T_FLOAT,
		&g_cfg.overlayDistance,
		0.3f,
		3.0f,
		"%.2f m",
		0,
		"How far in front of your eyes the stats overlay floats. It keeps its size and place in your view.",
	},
	{
		"Menu distance",
		T_FLOAT,
		&g_cfg.menuDistance,
		0.4f,
		3.0f,
		"%.2f m",
		0,
		"How far in front of you this menu opens. It keeps its size in your view, and moves as you change this.",
	},
	{
		"Recenter",
		T_ACTION,
		(void*)A_RECENTER,
		0,
		0,
		nullptr,
		0,
		"Places the screen straight ahead of you again (also F8).",
	},
};
Item kInput[] = {
	{
		"VR controllers as gamepad",
		T_BOOL,
		&g_cfg.vrControllers,
		0,
		1,
		nullptr,
		0,
		"Your VR controllers work as an Xbox gamepad: sticks, triggers, grips = LB/RB, A/B/X/Y, stick clicks. Left menu "
		"button: tap = Start, hold = Back, hold + left stick = D-pad. A real gamepad and the keyboard keep working. "
		"Turn off to play with only those.",
	},
	{
		"Controller rumble",
		T_BOOL,
		&g_cfg.vrRumble,
		0,
		1,
		nullptr,
		0,
		"The game's rumble is played on the VR controllers.",
	},
	{
		"Stick moves where you look",
		T_BOOL,
		&g_cfg.stickFollowsHead,
		0,
		1,
		nullptr,
		0,
		"First person: pushing the left stick forward moves you where your headset faces. Off: forward is the way you faced when "
		"you last recentred (your play space's forward).",
	},
	{
		"Motion controls",
		T_BOOL,
		&g_cfg.motionControls,
		0,
		1,
		nullptr,
		0,
		"Swing a controller to attack: right hand = light attack, left hand = heavy attack (2B's two weapons). "
		"A short rumble confirms each swing. The buttons keep working too.",
	},
	{
		"Swing sensitivity",
		T_CHOICE,
		&g_cfg.swingSensitivity,
		0,
		1,
		"Relaxed|Normal",
		0,
		"Relaxed also counts smaller, sloppier slashes and stabs; Normal wants clean ones. A slash is a turn of the "
		"wrist and arm, a stab a push along where your fist points.",
	},
};

struct Page {
	const char* name;
	Item* items;
	int32_t count;
	int32_t noteFirst;  // live Info lines shown above the settings (-1: none)
	int32_t noteCount;
};
Page s_pages[] = {{"View", kView, std::size(kView), -1, 0},
				  // the upscaler's state and the game resolution for each of its modes, where the
				  // upscaler settings are: it takes two numbers to land on a mode, so show both
				  {"Rendering", kRender, std::size(kRender), INFO_PANEL_RESOLUTION, 2},
				  {"Screen", kScreen, std::size(kScreen), -1, 0},
				  {"Controls", kInput, std::size(kInput), -1, 0},
				  {"Info", nullptr, 0, -1, 0}};
constexpr int32_t kPageCount = std::size(s_pages);

char s_infoText[INFO_LINE_COUNT][160];
SRWLOCK s_infoLock = SRWLOCK_INIT;

// State
UiPanel s_panel = {"menu", 30.0f, true};
constexpr float kSmall = 22.0f;  // font size of tags and hints

std::atomic<bool> s_open, s_justOpened, s_wantRecenter;
bool s_waitRelease;  // after closing: keep blocking the game's gamepad until every button is released
int32_t s_page;
float s_infoScroll;  // the Info page: -1 down to 1 up (the sticks and D-pad)
constexpr float kInfoScrollSpeed = 900;  // pixels per second at full tilt
bool s_pageChanged;        // select s_page's tab on the next frames (paging buttons, opening)
bool s_focusFirst;         // put the navigation cursor on the page's first setting
bool s_focusWindow;        // the menu window takes keyboard/gamepad focus (when it opens)
bool s_sliderActive;       // a slider is being adjusted: B (finish) and LB/RB (finer/coarser) go to ImGui
XINPUT_GAMEPAD s_gamepad;  // this frame's gamepad (a real one and the VR controllers), fed to ImGui
bool s_gamepadOk, s_foreground;
bool s_ignoreA;  // the A press that opened the menu: kept from ImGui until A is let go
WORD s_prevButtons;
bool s_comboLocked;  // B closed the menu: B + A can't open it again until B is let go
bool s_keyWasDown[256];
const char* s_help;      // help text of the focused setting
char s_helpDefault[96];  // and its default value, "" for actions
bool s_resetPending;     // X / Delete was pressed: the focused setting goes back to its default

// Keys
// A key's name as the keyboard layout calls it ("Insert", "F1", ...).
void KeyName(int vk, char* out, size_t cap) {
	UINT scanCode = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC);
	switch (vk) {  // keys on the extended part of the keyboard (else the name of the numpad key)
		case VK_INSERT:
		case VK_DELETE:
		case VK_HOME:
		case VK_END:
		case VK_PRIOR:
		case VK_NEXT:
		case VK_LEFT:
		case VK_RIGHT:
		case VK_UP:
		case VK_DOWN:
		case VK_DIVIDE:
		case VK_NUMLOCK:
		case VK_RCONTROL:
		case VK_RMENU:
		case VK_SNAPSHOT:
			scanCode |= 0x100;
			break;
	}
	wchar_t name[64];
	if (vk <= 0 || !GetKeyNameTextW((LONG)(scanCode << 16), name, 64)) {
		snprintf(out, cap, vk > 0 ? "key 0x%X" : "(no key)", (unsigned)vk);
		return;
	}
	snprintf(out, cap, "%s", Utf8(name).s);
}

bool KeyEdge(int vk, bool foreground) {
	if (vk <= 0 || vk > 255) return false;
	bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
	bool pressed = down && !s_keyWasDown[vk];
	s_keyWasDown[vk] = down;
	return pressed && foreground;
}
bool KeyDown(int vk) { return s_foreground && (GetAsyncKeyState(vk) & 0x8000) != 0; }

void SetOpen(bool open) {
	if (s_open == open) return;
	s_open = open;
	if (open) {
		s_justOpened = true;
		g_blockPad = true;
		s_pageChanged = s_focusFirst = s_focusWindow = true;
		s_ignoreA = true;
		s_sliderActive = false;
		Log("menu: opened");
	} else {
		s_waitRelease = true;
		Log("menu: closed");
	}
}

// Settings
// After a setting was changed in the menu: log it and save it to NieRVR.ini.
void SaveItem(const Item& item) {
	char value[48];
	if (item.type == T_BOOL)
		snprintf(value, sizeof(value), "%s", *(int*)item.value ? "on" : "off");
	else if (item.type == T_INT || item.type == T_CHOICE)
		snprintf(value, sizeof(value), "%d", *(int*)item.value);
	else
		snprintf(value, sizeof(value), "%.3f", (double)*(float*)item.value);
	Log("menu: %s = %s%s", item.label, value, item.flags & F_RESTART ? " (after a restart)" : "");
	ConfigSave(item.value);
}

// Settings that do nothing in the current eye mode say so.
const char* ItemTag(const Item& item) {
	if (item.type == T_ACTION && (intptr_t)item.value == A_DLSS_FILE) {
		static DlssFileState file;
		DlssFileGet(&file);
		if (file.state == DF_UPDATE_AVAILABLE) return "(update available)";
		return file.state == DF_MISSING && file.text[0] ? file.text : nullptr;  // why the last download failed
	}
	if ((item.flags & F_ALTERNATING) && StereoSynchronized()) return "(not used: synchronized mode)";
	if ((item.flags & F_SYNCHRONIZED) && !StereoSynchronized()) return "(not used: alternating or AFW mode)";
	if (item.flags & F_RESTART) return "(restart)";
	return nullptr;
}

// Drawing (ImGui; runs on the VR thread every headset frame while the menu is open)
ImVec4 ToImVec4(const float* color) { return ImVec4(color[0], color[1], color[2], color[3]); }

// The v-th name of a T_CHOICE item ("Off|DLSS").
void ChoiceName(const Item& item, int index, char* out, size_t cap) {
	snprintf(out, cap, "?");
	const char* name = item.format;
	for (int i = 0; name && i < index; i++) {  // skip v names
		name = strchr(name, '|');
		if (name) name++;
	}
	if (name && index >= 0) snprintf(out, cap, "%.*s", (int)strcspn(name, "|"), name);
}

// A value of a setting as the menu shows it.
void ValueText(const Item& item, float value, char* out, size_t cap) {
	switch (item.type) {
		case T_BOOL: snprintf(out, cap, "%s", value != 0 ? "On" : "Off"); return;
		case T_CHOICE: ChoiceName(item, (int)value, out, cap); return;
		case T_INT: snprintf(out, cap, item.format ? item.format : "%d", (int)value); return;
		case T_FLOAT: snprintf(out, cap, item.format ? item.format : "%.2f", (double)value); return;
		default: out[0] = 0;
	}
}

// Puts a setting back to its default (NieRVR.ini's table) and saves it.
void ResetToDefault(Item& item) {
	float defaultValue;
	if ((item.type != T_BOOL && item.type != T_INT && item.type != T_FLOAT && item.type != T_CHOICE) || !ConfigDefault(item.value, &defaultValue))
		return;
	if (item.type == T_FLOAT) {
		if (*(float*)item.value == defaultValue) return;
		*(float*)item.value = defaultValue;
	} else {
		if (*(int*)item.value == (int)defaultValue) return;
		*(int*)item.value = (int)defaultValue;
	}
	if (item.flags & F_RECENTER) s_wantRecenter = true;
	SaveItem(item);
}

// While downloading, the button is the progress bar; pressing it cancels.
bool DownloadProgressButton(const DlssFileState& file) {
	float progress = file.total > 0 ? (float)((double)file.got / (double)file.total) : 0;
	char label[64];
	snprintf(label, sizeof(label), "%.0f / %.0f MB  (%.0f%%)", file.got / 1e6, file.total / 1e6, (double)(progress * 100));
	ImVec2 pos = ImGui::GetCursorScreenPos();
	float width = ImGui::GetContentRegionAvail().x, height = ImGui::GetFrameHeight();
	bool pressed = ImGui::Button("##dlss", ImVec2(width, height));
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	const float* accent = ui::Colors::accent;
	drawList->AddRectFilled(pos, ImVec2(pos.x + width * progress, pos.y + height),
							ImGui::GetColorU32(ImVec4(accent[0], accent[1], accent[2], 0.55f)), ImGui::GetStyle().FrameRounding);
	ImVec2 textSize = ImGui::CalcTextSize(label);
	drawList->AddText(ImVec2(pos.x + (width - textSize.x) * 0.5f, pos.y + (height - textSize.y) * 0.5f),
					  ImGui::GetColorU32(ImGuiCol_Text), label);
	return pressed;
}

void DlssFileButton() {
	DlssFileState file;
	DlssFileGet(&file);
	if (file.state == DF_DOWNLOADING) {
		if (DownloadProgressButton(file)) DlssFileCancel();
		return;
	}
	const char* label = "Download (59 MB)###dlss";
	if (file.state == DF_INSTALLED)
		label = "Redownload###dlss";
	else if (file.state == DF_UPDATE_AVAILABLE)
		label = "Update (59 MB)###dlss";
	else if (file.text[0])
		label = "Try again (59 MB)###dlss";
	if (ImGui::Button(label, ImVec2(-FLT_MIN, 0))) DlssFileDownload();
}

// The widget of one setting; true if it changed.
bool ItemWidget(Item& item) {
	ImGui::SetNextItemWidth(-FLT_MIN);
	const ImGuiSliderFlags sliderFlags = ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoInput |
										 (item.flags & F_LOG ? ImGuiSliderFlags_Logarithmic : 0);
	switch (item.type) {
		case T_BOOL: {
			bool value = *(int*)item.value != 0;
			if (!ImGui::Checkbox("##v", &value)) return false;
			*(int*)item.value = value;
			return true;
		}
		case T_INT: {
			int value = *(int*)item.value;
			const char* format = item.format ? item.format : "%d";
			if (item.value == &g_cfg.giVolumes && value >= 128) format = "128 (all)";
			if (!ImGui::SliderInt("##v", &value, (int)item.min, (int)item.max, format, sliderFlags)) return false;
			*(int*)item.value = value;
			return true;
		}
		case T_FLOAT: {
			float value = *(float*)item.value;
			if (!ImGui::SliderFloat("##v", &value, item.min, item.max, item.format ? item.format : "%.2f", sliderFlags)) return false;
			*(float*)item.value = value;
			return true;
		}
		case T_CHOICE: {  // a slider over the names
			int value = *(int*)item.value;
			char name[64];
			ChoiceName(item, value, name, sizeof(name));
			if (!ImGui::SliderInt("##v", &value, (int)item.min, (int)item.max, name, sliderFlags)) return false;
			*(int*)item.value = value;
			return true;
		}
		case T_ACTION:
			if ((intptr_t)item.value == A_DLSS_FILE) {
				DlssFileButton();
				return false;
			}
			if (ImGui::Button("Do it", ImVec2(-FLT_MIN, 0)) && (intptr_t)item.value == A_RECENTER) s_wantRecenter = true;
			return false;
		default:
			return false;
	}
}

// Resolution and DLSS quality are a percentage and a name: say under them what they come to in pixels.
const char* ItemDetail(const Item& item) {
	static char text[96];
	uint32_t drawW, drawH, headsetW, headsetH;
	if (item.value != &g_cfg.upscaleOutput && item.value != &g_cfg.upscaleQuality) return nullptr;
	if (!GameResolutionSizes(&drawW, &drawH, &headsetW, &headsetH)) return nullptr;
	if (item.value == &g_cfg.upscaleOutput) snprintf(text, sizeof(text), "your headset gets %ux%u", headsetW, headsetH);
	else snprintf(text, sizeof(text), "the game draws %ux%u", drawW, drawH);
	return text;
}

void ItemRow(Item& item, int index) {
	ImGui::TableNextRow();
	ImGui::TableSetColumnIndex(0);
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(item.label);
	if (const char* detail = ItemDetail(item)) {
		ImGui::PushFont(nullptr, kSmall);
		ImGui::TextDisabled("%s", detail);
		ImGui::PopFont();
	}
	if (const char* tag = ItemTag(item)) {
		ImGui::SameLine();
		ImGui::PushFont(nullptr, kSmall);
		ImGui::TextColored(ToImVec4(ui::Colors::warn), "%s", tag);
		ImGui::PopFont();
	}
	ImGui::TableSetColumnIndex(1);
	ImGui::PushID(index);
	bool changed = ItemWidget(item);
	if (s_focusFirst) {  // the page was just opened: start on its first setting
		ImGui::FocusItem();
		ImGui::SetNavCursorVisible(true);
		s_focusFirst = false;
	}
	bool focused = ImGui::IsItemFocused();
	// back on the first setting: scroll to the very top, so the page's info lines above it come back into view
	// (navigation only scrolls far enough to show the setting itself - JJ 2026-10-28)
	static int focusedRow = -1;
	if (focused && index == 0 && focusedRow != 0) ImGui::SetScrollY(0.0f);
	if (focused) focusedRow = index;
	bool done = item.type == T_BOOL ? changed : ImGui::IsItemDeactivatedAfterEdit();  // sliders: when let go
	if (item.type == T_BOOL) {
		ImGui::SameLine();
		ImGui::TextUnformatted(*(int*)item.value ? "On" : "Off");
	}
	ImGui::PopID();
	if (changed && (item.flags & F_RECENTER)) s_wantRecenter = true;
	if (done) SaveItem(item);
	if (focused) {
		if (s_resetPending) {
			s_resetPending = false;
			ResetToDefault(item);
		}
		s_help = item.help;
		float defaultValue;
		char valueText[64];
		s_helpDefault[0] = 0;
		if (item.type != T_ACTION && ConfigDefault(item.value, &defaultValue)) {
			ValueText(item, defaultValue, valueText, sizeof(valueText));
			snprintf(s_helpDefault, sizeof(s_helpDefault), "Default: %s  (X or Delete: back to it)", valueText);
		}
		ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ToImVec4(ui::Colors::bar)));
	}
}

void PageContents(const Page& page, bool isInfo) {
	if (isInfo) {  // the Info page: lines of live text set by the other parts of the mod
		{
			SharedLock guard(&s_infoLock);
			for (int i = 0; i < INFO_LINE_COUNT; i++)
				if (s_infoText[i][0]) ImGui::TextWrapped("%s", s_infoText[i]);
		}
		// nothing on it to move to, so the sticks and D-pad scroll it here (ImGui's own scrolling goes to the menu window)
		ImGui::SetScrollY(ImGui::GetScrollY() - s_infoScroll * kInfoScrollSpeed * ImGui::GetIO().DeltaTime);
		s_focusFirst = false;
		return;
	}
	if (page.noteFirst >= 0) {
		SharedLock guard(&s_infoLock);
		for (int32_t i = page.noteFirst; i < page.noteFirst + page.noteCount && i < INFO_LINE_COUNT; i++)
			if (s_infoText[i][0]) ImGui::TextWrapped("%s", s_infoText[i]);
	}
	if (!ImGui::BeginTable("items", 2, ImGuiTableFlags_None)) return;
	ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch);
	ImGui::TableSetupColumn("control", ImGuiTableColumnFlags_WidthFixed, 440);
	for (int i = 0; i < page.count; i++) ItemRow(page.items[i], i);
	ImGui::EndTable();
}

void Draw() {
	// title and version
	ImGui::PushFont(nullptr, 42.0f);
	ImGui::TextColored(ToImVec4(ui::Colors::accent), "NieRVR settings");
	ImGui::PopFont();
	ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - ImGui::CalcTextSize(NIERVR_VERSION).x);
	ImGui::TextDisabled("%s", NIERVR_VERSION);

	// footer size, so the page gets the rest
	const float smallLine = kSmall * 1.35f;
	const float footer = ImGui::GetFontSize() * 1.3f * 3 + smallLine * 3 + ImGui::GetStyle().ItemSpacing.y * 6;

	s_help = nullptr;
	s_helpDefault[0] = 0;
	int shown = -1;
	if (ImGui::BeginTabBar("pages")) {
		for (int i = 0; i < kPageCount; i++) {
			ImGuiTabItemFlags tabFlags = s_pageChanged && i == s_page ? ImGuiTabItemFlags_SetSelected : 0;
			if (!ImGui::BeginTabItem(s_pages[i].name, nullptr, tabFlags)) continue;
			shown = i;
			ImGui::BeginChild("page", ImVec2(0, ImGui::GetContentRegionAvail().y - footer), ImGuiChildFlags_NavFlattened);
			if (!s_pageChanged || i == s_page) PageContents(s_pages[i], i == kPageCount - 1);
			ImGui::EndChild();
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}
	// the shown tab: ours once a page change has taken effect, else the one ImGui navigation chose
	if (shown >= 0) {
		if (s_pageChanged) {
			if (shown == s_page) s_pageChanged = false;
		} else if (shown != s_page) {
			s_page = shown;
		}
	}

	// help for the focused setting, then the controls
	ImGui::Separator();
	ImGui::BeginChild("help", ImVec2(0, ImGui::GetFontSize() * 1.3f * 3 + smallLine), ImGuiChildFlags_None, ImGuiWindowFlags_NoNav);
	if (s_help) ImGui::TextWrapped("%s", s_help);
	if (s_help && s_helpDefault[0]) {
		ImGui::PushFont(nullptr, kSmall);
		ImGui::TextDisabled("%s", s_helpDefault);
		ImGui::PopFont();
	}
	ImGui::EndChild();
	s_resetPending = false;  // a reset press with nothing to reset
	ImGui::Separator();
	ImGui::PushFont(nullptr, kSmall);
	ImGui::TextDisabled("Gamepad:  left stick / D-pad move   right stick scroll   A switch / adjust a slider (A again: done)");
	ImGui::TextDisabled("X default   LB/RB or triggers page   B close");
	char key[64];
	KeyName(g_cfg.keyMenu, key, sizeof(key));
	ImGui::SameLine();
	ImGui::TextDisabled("      Keyboard:  arrows   Enter   Delete   PgUp/PgDn   %s close", key);
	ImGui::PopFont();
}

// The menu's input for ImGui's navigation: the gamepad (a real one and the VR controllers) and
// the keyboard. B and LB/RB only while a slider is being adjusted (else they close the menu and
// change pages, MenuTick).
void FeedInput() {
	ImGuiIO& io = ImGui::GetIO();
	WORD buttons = s_gamepadOk ? s_gamepad.wButtons : 0;
	if (!(buttons & XINPUT_GAMEPAD_A)) s_ignoreA = false;
	// The left stick works like the D-pad (ImGui would scroll with it; on the VR controllers the
	// thumbstick is what you move with). On past half way, off again below a third.
	static bool stickUp, stickDown, stickLeft, stickRight;
	auto stickDirection = [](bool& on, float v) { on = on ? v > 0.33f : v > 0.5f; return on; };
	float stickX = s_gamepadOk ? (float)s_gamepad.sThumbLX / 32767.0f : 0.0f, stickY = s_gamepadOk ? (float)s_gamepad.sThumbLY / 32767.0f : 0.0f;
	stickDirection(stickUp, stickY);
	stickDirection(stickDown, -stickY);
	stickDirection(stickLeft, -stickX);
	stickDirection(stickRight, stickX);
	io.AddKeyEvent(ImGuiKey_GamepadDpadUp, (buttons & XINPUT_GAMEPAD_DPAD_UP) || stickUp);
	io.AddKeyEvent(ImGuiKey_GamepadDpadDown, (buttons & XINPUT_GAMEPAD_DPAD_DOWN) || stickDown);
	io.AddKeyEvent(ImGuiKey_GamepadDpadLeft, (buttons & XINPUT_GAMEPAD_DPAD_LEFT) || stickLeft);
	io.AddKeyEvent(ImGuiKey_GamepadDpadRight, (buttons & XINPUT_GAMEPAD_DPAD_RIGHT) || stickRight);
	// The right stick scrolls the page (ImGui's stick scrolling, which it reads from its "left stick").
	float rightX = s_gamepadOk ? (float)s_gamepad.sThumbRX / 32767.0f : 0.0f, rightY = s_gamepadOk ? (float)s_gamepad.sThumbRY / 32767.0f : 0.0f;
	auto scroll = [&](ImGuiKey neg, ImGuiKey pos, float v) {
		io.AddKeyAnalogEvent(neg, v < -0.25f, v < -0.25f ? -v : 0.0f);
		io.AddKeyAnalogEvent(pos, v > 0.25f, v > 0.25f ? v : 0.0f);
	};
	scroll(ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight, rightX);
	scroll(ImGuiKey_GamepadLStickDown, ImGuiKey_GamepadLStickUp, rightY);
	float dpad = (buttons & XINPUT_GAMEPAD_DPAD_UP) || stickUp ? 1.0f : (buttons & XINPUT_GAMEPAD_DPAD_DOWN) || stickDown ? -1.0f
																														  : 0.0f;
	s_infoScroll = fabsf(rightY) > 0.25f ? rightY : dpad;
	io.AddKeyEvent(ImGuiKey_GamepadFaceDown, (buttons & XINPUT_GAMEPAD_A) && !s_ignoreA);
	io.AddKeyEvent(ImGuiKey_GamepadFaceRight, (buttons & XINPUT_GAMEPAD_B) && s_sliderActive);
	io.AddKeyEvent(ImGuiKey_GamepadL1, (buttons & XINPUT_GAMEPAD_LEFT_SHOULDER) && s_sliderActive);
	io.AddKeyEvent(ImGuiKey_GamepadR1, (buttons & XINPUT_GAMEPAD_RIGHT_SHOULDER) && s_sliderActive);
	io.AddKeyEvent(ImGuiKey_UpArrow, KeyDown(VK_UP));
	io.AddKeyEvent(ImGuiKey_DownArrow, KeyDown(VK_DOWN));
	io.AddKeyEvent(ImGuiKey_LeftArrow, KeyDown(VK_LEFT));
	io.AddKeyEvent(ImGuiKey_RightArrow, KeyDown(VK_RIGHT));
	io.AddKeyEvent(ImGuiKey_Enter, KeyDown(VK_RETURN));
	io.AddKeyEvent(ImGuiKey_Space, KeyDown(VK_SPACE));
	io.AddKeyEvent(ImGuiKey_Escape, KeyDown(VK_ESCAPE));
}
}  // namespace

// API (VR thread unless noted)
// Any thread: one line of the Info page.
void MenuSetInfo(int line, const char* text) {
	if (line < 0 || line >= INFO_LINE_COUNT) return;
	ExclusiveLock guard(&s_infoLock);
	snprintf(s_infoText[line], sizeof(s_infoText[line]), "%s", text);
}

// Every headset frame: opening and closing, page buttons, and the gamepad state for ImGui.
void MenuTick(bool foreground) {
	DWORD now = GetTickCount();
	static DWORD lastHook;
	if (now - lastHook > 3000) {  // the game may load XInput late
		lastHook = now;
		InstallGamepad();
	}
	XINPUT_GAMEPAD pad = {};
	bool havePad = ReadGamepad(&pad);
	s_gamepad = pad;
	s_gamepadOk = havePad;
	s_foreground = foreground;
	WORD buttons = havePad ? pad.wButtons : 0;
	WORD pressed = buttons & ~s_prevButtons;  // newly pressed buttons
	s_prevButtons = buttons;
	// Triggers also change pages (VR controllers: the grips are LB/RB, which is easy to miss).
	// A pull counts once: above 160, and it must drop below 64 before it counts again.
	static bool leftTriggerHeld, rightTriggerHeld;
	bool leftTriggerPulled = havePad && !leftTriggerHeld && pad.bLeftTrigger > 160;
	bool rightTriggerPulled = havePad && !rightTriggerHeld && pad.bRightTrigger > 160;
	if (havePad && pad.bLeftTrigger > 160) leftTriggerHeld = true;
	else if (!havePad || pad.bLeftTrigger < 64) leftTriggerHeld = false;
	if (havePad && pad.bRightTrigger > 160) rightTriggerHeld = true;
	else if (!havePad || pad.bRightTrigger < 64) rightTriggerHeld = false;

	// open / close. Gamepad: press both sticks in and hold, or hold B then press A. Closing: B, Start
	// or the menu key. Both sticks held is the same gesture as the other VR mods in this family.
	constexpr DWORD kBothSticks = XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB;
	static DWORD s_bothSticksSince;  // GetTickCount when both went down (0 = not held)
	bool bothSticks = (buttons & kBothSticks) == kBothSticks;
	bool sticksFire = false;
	if (!bothSticks) {
		s_bothSticksSince = 0;
	} else if (!s_bothSticksSince) {
		s_bothSticksSince = GetTickCount() | 1;
	} else if (s_bothSticksSince != 1 && GetTickCount() - s_bothSticksSince > kStickHoldMs) {
		sticksFire = true;
		s_bothSticksSince = 1;  // fired: wait for a release before it can fire again
	}
	if (!(buttons & XINPUT_GAMEPAD_B)) s_comboLocked = false;
	bool comboFire = !s_comboLocked && (buttons & XINPUT_GAMEPAD_B) && (pressed & XINPUT_GAMEPAD_A);
	bool keyToggle = KeyEdge(g_cfg.keyMenu, foreground);
	bool pageUpKey = KeyEdge(VK_PRIOR, foreground), pageDownKey = KeyEdge(VK_NEXT, foreground);
	bool deleteKey = KeyEdge(VK_DELETE, foreground), backKey = KeyEdge(VK_BACK, foreground);  // both read every frame
	bool resetKey = deleteKey || backKey;
	if (!s_open) {
		if (keyToggle || comboFire || sticksFire) SetOpen(true);
	} else if (keyToggle || sticksFire || (!s_sliderActive && (pressed & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_START)))) {
		SetOpen(false);
		if (buttons & XINPUT_GAMEPAD_B) s_comboLocked = true;  // else A with B still down reopens it at once
	}
	if (!s_open) {
		if (s_waitRelease && !buttons) {
			s_waitRelease = false;
			g_blockPad = false;
		}
		return;
	}
	// X / Delete: the highlighted setting back to its default (not while a slider is being adjusted)
	if (!s_sliderActive && ((pressed & XINPUT_GAMEPAD_X) || resetKey)) s_resetPending = true;
	// pages (while a slider is being adjusted, LB/RB make it finer/coarser instead)
	bool pageLeft = pageUpKey || (!s_sliderActive && ((pressed & XINPUT_GAMEPAD_LEFT_SHOULDER) || leftTriggerPulled));
	bool pageRight = pageDownKey || (!s_sliderActive && ((pressed & XINPUT_GAMEPAD_RIGHT_SHOULDER) || rightTriggerPulled));
	if (pageLeft || pageRight) {
		s_page = (s_page + (pageRight ? 1 : kPageCount - 1)) % kPageCount;
		s_pageChanged = s_focusFirst = true;
	}
}

bool MenuIsOpen() { return s_open; }
// Returns whether the menu was just opened, and clears it.
bool MenuTakeJustOpened() {
	bool justOpened = s_justOpened.exchange(false);
	return justOpened;
}
bool MenuTakeRecenter() {
	bool recenter = s_wantRecenter.exchange(false);
	return recenter;
}

// Draws the menu into rtv (w x h pixels). False if ImGui couldn't start.
bool MenuRender(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11RenderTargetView* rtv, int width, int height) {
	if (!UiPrepare(s_panel, device, context, width, height)) return false;
	FeedInput();
	UiNewFrame();
	ImGui::SetNextWindowPos(ImVec2(0, 0));
	ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
	if (s_focusWindow) {
		ImGui::SetNextWindowFocus();
		s_focusWindow = false;
	}
	ImGui::Begin("NieRVR settings", nullptr,
				 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
	Draw();
	ImGui::End();
	s_sliderActive = ImGui::IsAnyItemActive();
	UiRender(context, rtv);
	return true;
}
