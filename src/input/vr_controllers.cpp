// vr_controllers.cpp - the VR controllers as an XInput gamepad, plus motion controls.
#include "input/vr_controllers.h"
#include "core/locks.h"
#include "core/config.h"
#include "core/log.h"
#include "core/pure.h"
#include "input/finger_curl.h"
#include "input/waggle.h"
#include "ui/menu.h"
#include "xr/openxr.h"
#include <atomic>
#include <stdio.h>
#include <string.h>
#include <xinput.h>

namespace {
#define XR_INPUT_FUNCS(X)                  \
	X(xrStringToPath)                      \
	X(xrCreateActionSet)                   \
	X(xrDestroyActionSet)                  \
	X(xrCreateAction)                      \
	X(xrSuggestInteractionProfileBindings) \
	X(xrAttachSessionActionSets)           \
	X(xrSyncActions)                       \
	X(xrGetActionStateBoolean)             \
	X(xrGetActionStateFloat)               \
	X(xrGetActionStateVector2f)            \
	X(xrApplyHapticFeedback)               \
	X(xrStopHapticFeedback)                \
	X(xrGetCurrentInteractionProfile)      \
	X(xrPathToString)                      \
	X(xrCreateActionSpace)                 \
	X(xrLocateSpace)                       \
	X(xrDestroySpace)
struct {
#define DECL(n) PFN_##n n;
	XR_INPUT_FUNCS(DECL)
#undef DECL
} s_xr;

enum Action { ACT_STICK,
			  ACT_STICK_CLICK,
			  ACT_TRIGGER,
			  ACT_GRIP,
			  ACT_LOWER_BUTTON,
			  ACT_UPPER_BUTTON,
			  ACT_MENU,
			  ACT_RUMBLE,
			  ACT_HAND_POSE,
			  ACT_TRIGGER_TOUCH,  // finger curls (first person's hands)
			  ACT_THUMB_TOUCH,
			  ACT_COUNT };
XrInstance s_instance;
XrActionSet s_actionSet;
XrAction s_actions[ACT_COUNT];
XrPath s_handPaths[2];
XrSession s_session;
bool s_actionsReady, s_attached;
XrSpace s_handSpace[2];      // the grip pose of each hand (motion controls)
char s_profileName[2][128];  // the interaction profile bound to each hand

// shared with the game thread
SRWLOCK s_padLock = SRWLOCK_INIT;
XINPUT_GAMEPAD s_gamepad;
DWORD s_gamepadTick;                        // when s_gamepad was last updated with an active controller
bool s_anyActive, s_everActive;             // everActive: for the log
std::atomic<int32_t> s_rumbleLeft, s_rumbleRight;  // 0..65535 from XInputSetState
float s_lastAmplitude[2];
DWORD s_pulseTick[2];

pure::MenuButton s_menuButton;  // Start / Back / D-pad from the left menu button

// motion controls (input/waggle.h)
constexpr bool kStabAttacks = true;  // false: a forward poke never attacks, only slashes do
pure::SwingAnalyser s_swings[2];
DWORD s_attackHeldUntil[2];  // the attack button is held until then (100 ms per attack)
uint32_t s_attackCount[2];
DWORD s_infoTick;  // the Info page line, once a second
// a hand's pose last frame, for the velocities a runtime doesn't give
struct LastHand {
	bool valid;
	XrTime time;
	XrPosef pose;
};
LastHand s_lastHand[2];
// the stats overlay's hand numbers: peaks since VrInputTakeHandPeaks, and last frame's velocities
HandPeaks s_handPeaks[2] = {{-1, -1, -1, -1}, {-1, -1, -1, -1}};
XrVector3f s_lastVel[2], s_lastSpin[2];
bool s_lastVelValid[2], s_lastSpinValid[2];

// Finger curls (first person's hands): from hand tracking (XR_EXT_hand_tracking: SteamVR's Index finger
// tracking, hands without controllers) when the runtime gives it, else from the buttons.
struct {
	PFN_xrCreateHandTrackerEXT xrCreateHandTrackerEXT;
	PFN_xrDestroyHandTrackerEXT xrDestroyHandTrackerEXT;
	PFN_xrLocateHandJointsEXT xrLocateHandJointsEXT;
} s_handXr;
XrHandTrackerEXT s_handTracker[2];
constexpr float kButtonSmoothing = 0.35f;  // of the way to the buttons' curls each frame
constexpr DWORD kFingersFreshMs = 250;
SRWLOCK s_fingerLock = SRWLOCK_INIT;
struct Fingers {
	bool valid;
	bool fromJoints;
	pure::FingerCurls curls;
	DWORD tick;
} s_fingers[2];
int32_t s_loggedFingerSource[2] = {-1, -1};  // 0 buttons, 1 hand tracking

// Keyboard / mouse handoff: a key or mouse button pressed in the game pauses the VR
// controllers (the game sees a neutral pad, no rumble) until a button, trigger or stick on them is
// used on purpose. Controllers strapped to your hands while you type (Index) send nothing then.
bool s_keyboardMouseMode;
bool s_keyWasDown[256];
WORD s_prevVrButtons;
float s_prevTrigger[2];
uint32_t s_toKeyboardCount, s_toControllersCount;

bool IsHandoffKey(int vk) {
	if (vk >= VK_F1 && vk <= VK_F24) return false;  // the mod's hotkeys
	switch (vk) {
		case VK_INSERT:  // settings menu
		case VK_SHIFT:
		case VK_CONTROL:
		case VK_MENU:
		case VK_LSHIFT:
		case VK_RSHIFT:
		case VK_LCONTROL:
		case VK_RCONTROL:
		case VK_LMENU:
		case VK_RMENU:
		case VK_LWIN:
		case VK_RWIN:
		case VK_APPS:
		case VK_SNAPSHOT:
		case VK_CAPITAL:
		case VK_NUMLOCK:
		case VK_SCROLL:
			return false;
	}
	return true;
}
// true when a key or mouse button was newly pressed while the game has focus
bool KeyboardMouseUsed(bool foreground) {
	bool used = false;
	for (int vk = 1; vk < 256; vk++) {
		if (vk == VK_CANCEL || vk == 0x07) continue;
		bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
		if (down && !s_keyWasDown[vk] && foreground && IsHandoffKey(vk)) used = true;
		s_keyWasDown[vk] = down;
	}
	return used;
}

XrPath ToPath(const char* text) {
	XrPath path = XR_NULL_PATH;
	s_xr.xrStringToPath(s_instance, text, &path);
	return path;
}

bool MakeAction(Action action, XrActionType type, const char* name, const char* localizedName) {
	XrActionCreateInfo info = {};
	info.type = XR_TYPE_ACTION_CREATE_INFO;
	info.actionType = type;
	lstrcpynA(info.actionName, name, XR_MAX_ACTION_NAME_SIZE);
	lstrcpynA(info.localizedActionName, localizedName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
	info.countSubactionPaths = 2;
	info.subactionPaths = s_handPaths;
	return XR_SUCCEEDED(s_xr.xrCreateAction(s_actionSet, &info, &s_actions[action]));
}

struct Binding {
	Action action;
	const char* path;
};
// A profile's bindings, plus its finger touches (a path a profile doesn't have refuses all of them: kept apart).
template <int N, int F = 1>
void SuggestBindings(const char* profile, const Binding (&bindings)[N], const Binding (*fingers)[F] = nullptr) {
	XrActionSuggestedBinding suggested[N + F];
	int count = 0;
	for (int i = 0; i < N + F; i++) {
		if (i >= N && !fingers) break;
		const Binding& binding = i < N ? bindings[i] : (*fingers)[i - N];
		XrPath path = ToPath(binding.path);
		if (path != XR_NULL_PATH) suggested[count++] = {s_actions[binding.action], path};
	}
	XrInteractionProfileSuggestedBinding info = {};
	info.type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING;
	info.interactionProfile = ToPath(profile);
	info.countSuggestedBindings = (uint32_t)count;
	info.suggestedBindings = suggested;
	XrResult result = s_xr.xrSuggestInteractionProfileBindings(s_instance, &info);
	if (XR_FAILED(result)) Log("vr controllers: bindings for %s refused (%d)", profile, (int)result);
}

#define L "/user/hand/left/"
#define R "/user/hand/right/"

constexpr Binding kTouch[] = {
	{ACT_STICK, L "input/thumbstick"},
	{ACT_STICK, R "input/thumbstick"},
	{ACT_STICK_CLICK, L "input/thumbstick/click"},
	{ACT_STICK_CLICK, R "input/thumbstick/click"},
	{ACT_TRIGGER, L "input/trigger/value"},
	{ACT_TRIGGER, R "input/trigger/value"},
	{ACT_GRIP, L "input/squeeze/value"},
	{ACT_GRIP, R "input/squeeze/value"},
	{ACT_LOWER_BUTTON, L "input/x/click"},
	{ACT_LOWER_BUTTON, R "input/a/click"},
	{ACT_UPPER_BUTTON, L "input/y/click"},
	{ACT_UPPER_BUTTON, R "input/b/click"},
	{ACT_MENU, L "input/menu/click"},
	{ACT_RUMBLE, L "output/haptic"},
	{ACT_RUMBLE, R "output/haptic"},
	{ACT_HAND_POSE, L "input/grip/pose"},
	{ACT_HAND_POSE, R "input/grip/pose"},
};
constexpr Binding kTouchFingers[] = {
	{ACT_TRIGGER_TOUCH, L "input/trigger/touch"},
	{ACT_TRIGGER_TOUCH, R "input/trigger/touch"},
	{ACT_THUMB_TOUCH, L "input/thumbstick/touch"},
	{ACT_THUMB_TOUCH, R "input/thumbstick/touch"},
	{ACT_THUMB_TOUCH, L "input/x/touch"},
	{ACT_THUMB_TOUCH, L "input/y/touch"},
	{ACT_THUMB_TOUCH, R "input/a/touch"},
	{ACT_THUMB_TOUCH, R "input/b/touch"},
	{ACT_THUMB_TOUCH, L "input/thumbrest/touch"},
	{ACT_THUMB_TOUCH, R "input/thumbrest/touch"},
};
constexpr Binding kIndex[] = {
	{ACT_STICK, L "input/thumbstick"},
	{ACT_STICK, R "input/thumbstick"},
	{ACT_STICK_CLICK, L "input/thumbstick/click"},
	{ACT_STICK_CLICK, R "input/thumbstick/click"},
	{ACT_TRIGGER, L "input/trigger/value"},
	{ACT_TRIGGER, R "input/trigger/value"},
	{ACT_GRIP, L "input/squeeze/value"},
	{ACT_GRIP, R "input/squeeze/value"},
	{ACT_LOWER_BUTTON, L "input/a/click"},
	{ACT_LOWER_BUTTON, R "input/a/click"},
	{ACT_UPPER_BUTTON, L "input/b/click"},
	{ACT_UPPER_BUTTON, R "input/b/click"},
	{ACT_MENU, L "input/trackpad/force"},
	{ACT_RUMBLE, L "output/haptic"},
	{ACT_RUMBLE, R "output/haptic"},
	{ACT_HAND_POSE, L "input/grip/pose"},
	{ACT_HAND_POSE, R "input/grip/pose"},
};
constexpr Binding kIndexFingers[] = {
	{ACT_TRIGGER_TOUCH, L "input/trigger/touch"},
	{ACT_TRIGGER_TOUCH, R "input/trigger/touch"},
	{ACT_THUMB_TOUCH, L "input/thumbstick/touch"},
	{ACT_THUMB_TOUCH, R "input/thumbstick/touch"},
	{ACT_THUMB_TOUCH, L "input/a/touch"},
	{ACT_THUMB_TOUCH, R "input/a/touch"},
	{ACT_THUMB_TOUCH, L "input/b/touch"},
	{ACT_THUMB_TOUCH, R "input/b/touch"},
	{ACT_THUMB_TOUCH, L "input/trackpad/touch"},
	{ACT_THUMB_TOUCH, R "input/trackpad/touch"},
};
// Vive wands and WMR controllers: only the trackpad says where your thumb is.
constexpr Binding kTrackpadFingers[] = {
	{ACT_THUMB_TOUCH, L "input/trackpad/touch"},
	{ACT_THUMB_TOUCH, R "input/trackpad/touch"},
};

constexpr Binding kVive[] = {
	{ACT_STICK, L "input/trackpad"},
	{ACT_STICK, R "input/trackpad"},
	{ACT_STICK_CLICK, L "input/trackpad/click"},
	{ACT_LOWER_BUTTON, R "input/trackpad/click"},
	{ACT_TRIGGER, L "input/trigger/value"},
	{ACT_TRIGGER, R "input/trigger/value"},
	{ACT_GRIP, L "input/squeeze/click"},
	{ACT_GRIP, R "input/squeeze/click"},
	{ACT_MENU, L "input/menu/click"},
	{ACT_UPPER_BUTTON, R "input/menu/click"},
	{ACT_RUMBLE, L "output/haptic"},
	{ACT_RUMBLE, R "output/haptic"},
	{ACT_HAND_POSE, L "input/grip/pose"},
	{ACT_HAND_POSE, R "input/grip/pose"},
};
constexpr Binding kWmr[] = {
	{ACT_STICK, L "input/thumbstick"},
	{ACT_STICK, R "input/thumbstick"},
	{ACT_STICK_CLICK, L "input/thumbstick/click"},
	{ACT_STICK_CLICK, R "input/thumbstick/click"},
	{ACT_TRIGGER, L "input/trigger/value"},
	{ACT_TRIGGER, R "input/trigger/value"},
	{ACT_GRIP, L "input/squeeze/click"},
	{ACT_GRIP, R "input/squeeze/click"},
	{ACT_LOWER_BUTTON, L "input/trackpad/click"},
	{ACT_LOWER_BUTTON, R "input/trackpad/click"},
	{ACT_MENU, L "input/menu/click"},
	{ACT_UPPER_BUTTON, R "input/menu/click"},
	{ACT_RUMBLE, L "output/haptic"},
	{ACT_RUMBLE, R "output/haptic"},
	{ACT_HAND_POSE, L "input/grip/pose"},
	{ACT_HAND_POSE, R "input/grip/pose"},
};
constexpr Binding kSimple[] = {
	{ACT_TRIGGER, L "input/select/click"},
	{ACT_TRIGGER, R "input/select/click"},
	{ACT_MENU, L "input/menu/click"},
	{ACT_UPPER_BUTTON, R "input/menu/click"},
	{ACT_RUMBLE, L "output/haptic"},
	{ACT_RUMBLE, R "output/haptic"},
	{ACT_HAND_POSE, L "input/grip/pose"},
	{ACT_HAND_POSE, R "input/grip/pose"},
};
#undef L
#undef R

bool GetBool(Action action, int hand) {
	XrActionStateGetInfo getInfo = {XR_TYPE_ACTION_STATE_GET_INFO, nullptr, s_actions[action], s_handPaths[hand]};
	XrActionStateBoolean state = {XR_TYPE_ACTION_STATE_BOOLEAN};
	return XR_SUCCEEDED(s_xr.xrGetActionStateBoolean(s_session, &getInfo, &state)) && state.isActive && state.currentState;
}
float GetFloat(Action action, int hand, bool* active = nullptr) {
	XrActionStateGetInfo getInfo = {XR_TYPE_ACTION_STATE_GET_INFO, nullptr, s_actions[action], s_handPaths[hand]};
	XrActionStateFloat state = {XR_TYPE_ACTION_STATE_FLOAT};
	bool ok = XR_SUCCEEDED(s_xr.xrGetActionStateFloat(s_session, &getInfo, &state)) && state.isActive;
	if (active) *active = ok;
	return ok ? state.currentState : 0;
}
XrVector2f GetStick(int hand, bool* active) {
	XrActionStateGetInfo getInfo = {XR_TYPE_ACTION_STATE_GET_INFO, nullptr, s_actions[ACT_STICK], s_handPaths[hand]};
	XrActionStateVector2f state = {XR_TYPE_ACTION_STATE_VECTOR2F};
	bool ok = XR_SUCCEEDED(s_xr.xrGetActionStateVector2f(s_session, &getInfo, &state)) && state.isActive;
	*active = ok;
	return ok ? state.currentState : XrVector2f{0, 0};
}
SHORT ToAxis(float v) {
	if (v > 1) v = 1;
	if (v < -1) v = -1;
	return (SHORT)(v * 32767.0f);
}
// A resting controller can report a little stick or trigger; that alone shouldn't make the game
// switch its button prompts away from the keyboard.
XrVector2f StickDeadzone(XrVector2f v) {
	if (v.x * v.x + v.y * v.y < 0.06f * 0.06f) return {0, 0};
	return v;
}
float TriggerDeadzone(float v) { return v < 0.04f ? 0 : v; }

void UpdateRumble() {
	float amplitude[2] = {(float)s_rumbleLeft / 65535.0f, (float)s_rumbleRight / 65535.0f};
	for (int hand = 0; hand < 2; hand++) {
		XrHapticActionInfo hapticInfo = {XR_TYPE_HAPTIC_ACTION_INFO, nullptr, s_actions[ACT_RUMBLE], s_handPaths[hand]};
		DWORD now = GetTickCount();
		if (amplitude[hand] > 0.02f) {
			// renew a 100 ms pulse every 50 ms, or at once when the strength changes
			float change = amplitude[hand] - s_lastAmplitude[hand];
			if (change < 0.05f && change > -0.05f && now - s_pulseTick[hand] < 50) continue;
			s_pulseTick[hand] = now;
			XrHapticVibration vibration = {XR_TYPE_HAPTIC_VIBRATION};
			vibration.amplitude = amplitude[hand];
			vibration.duration = 100 * 1000 * 1000;  // 100 ms (ns)
			vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
			s_xr.xrApplyHapticFeedback(s_session, &hapticInfo, (const XrHapticBaseHeader*)&vibration);
		} else if (s_lastAmplitude[hand] > 0.02f) {
			s_xr.xrStopHapticFeedback(s_session, &hapticInfo);
		}
		s_lastAmplitude[hand] = amplitude[hand];
	}
}

// "/interaction_profiles/oculus/touch_controller" -> "oculus/touch_controller"; "(none)" when empty.
const char* ShortProfileName(const char* name) {
	const char* prefix = "/interaction_profiles/";
	if (!name[0]) return "(none)";
	return strstr(name, prefix) == name ? name + strlen(prefix) : name;
}

// Which controllers the runtime bound: read once after the actions are attached and again whenever
// the runtime reports XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED.
bool s_profileDirty = true;
void LogProfiles() {
	if (!s_profileDirty) return;
	s_profileDirty = false;
	for (int hand = 0; hand < 2; hand++) {
		XrInteractionProfileState state = {XR_TYPE_INTERACTION_PROFILE_STATE};
		if (XR_FAILED(s_xr.xrGetCurrentInteractionProfile(s_session, s_handPaths[hand], &state))) continue;
		char name[128] = "(none)";
		uint32_t length = 0;
		if (state.interactionProfile != XR_NULL_PATH)
			s_xr.xrPathToString(s_instance, state.interactionProfile, sizeof(name), &length, name);
		if (lstrcmpA(name, s_profileName[hand])) {
			lstrcpynA(s_profileName[hand], name, sizeof(s_profileName[hand]));
			Log("vr controllers: %s hand uses %s", hand ? "right" : "left", name);
			char info[160];
			snprintf(info, sizeof(info), "VR controllers: left %s, right %s", ShortProfileName(s_profileName[0]),
					 ShortProfileName(s_profileName[1]));
			MenuSetInfo(INFO_CONTROLLERS, info);
		}
	}
}
}  // namespace

// After xrCreateInstance: actions and suggested bindings (must come before the session attaches).
void VrInputInit(PFN_xrGetInstanceProcAddr getProcAddr, XrInstance instance, bool pico, bool handTracking) {
	s_actionsReady = false;
	s_instance = instance;
	bool ok = true;
#define LOAD(n) \
	if (XR_FAILED(getProcAddr(instance, #n, (PFN_xrVoidFunction*)&s_xr.n)) || !s_xr.n) ok = false;
	XR_INPUT_FUNCS(LOAD)
#undef LOAD
	s_handXr = {};
	if (handTracking) {
		bool loaded = true;
#define LOAD(n) \
	if (XR_FAILED(getProcAddr(instance, #n, (PFN_xrVoidFunction*)&s_handXr.n)) || !s_handXr.n) loaded = false;
		LOAD(xrCreateHandTrackerEXT)
		LOAD(xrDestroyHandTrackerEXT)
		LOAD(xrLocateHandJointsEXT)
#undef LOAD
		if (!loaded) s_handXr = {};
		Log("vr controllers: hand tracking %s", loaded ? "available (finger curls from it)" : "functions missing (finger curls from the buttons)");
	}
	if (!ok) {
		Log("vr controllers: OpenXR input functions missing; controllers not used");
		return;
	}
	s_handPaths[0] = ToPath("/user/hand/left");
	s_handPaths[1] = ToPath("/user/hand/right");
	XrActionSetCreateInfo info = {};
	info.type = XR_TYPE_ACTION_SET_CREATE_INFO;
	lstrcpynA(info.actionSetName, "gamepad", XR_MAX_ACTION_SET_NAME_SIZE);
	lstrcpynA(info.localizedActionSetName, "Gamepad", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE);
	if (XR_FAILED(s_xr.xrCreateActionSet(instance, &info, &s_actionSet))) {
		Log("vr controllers: could not create the action set");
		return;
	}
	ok = MakeAction(ACT_STICK, XR_ACTION_TYPE_VECTOR2F_INPUT, "stick", "Stick") &&
		 MakeAction(ACT_STICK_CLICK, XR_ACTION_TYPE_BOOLEAN_INPUT, "stick_click", "Stick click") &&
		 MakeAction(ACT_TRIGGER, XR_ACTION_TYPE_FLOAT_INPUT, "trigger", "Trigger") &&
		 MakeAction(ACT_GRIP, XR_ACTION_TYPE_FLOAT_INPUT, "grip", "Grip") &&
		 MakeAction(ACT_LOWER_BUTTON, XR_ACTION_TYPE_BOOLEAN_INPUT, "button_lower", "A / X") &&
		 MakeAction(ACT_UPPER_BUTTON, XR_ACTION_TYPE_BOOLEAN_INPUT, "button_upper", "B / Y") &&
		 MakeAction(ACT_MENU, XR_ACTION_TYPE_BOOLEAN_INPUT, "menu", "Menu (Start / Back / D-pad)") &&
		 MakeAction(ACT_RUMBLE, XR_ACTION_TYPE_VIBRATION_OUTPUT, "rumble", "Rumble") &&
		 MakeAction(ACT_HAND_POSE, XR_ACTION_TYPE_POSE_INPUT, "hand", "Hand position (motion controls)") &&
		 MakeAction(ACT_TRIGGER_TOUCH, XR_ACTION_TYPE_BOOLEAN_INPUT, "trigger_touch", "Finger on the trigger (first person's hands)") &&
		 MakeAction(ACT_THUMB_TOUCH, XR_ACTION_TYPE_BOOLEAN_INPUT, "thumb_touch", "Thumb on a button or stick (first person's hands)");
	if (!ok) {
		Log("vr controllers: could not create the actions");
		return;
	}
	SuggestBindings("/interaction_profiles/oculus/touch_controller", kTouch, &kTouchFingers);
	SuggestBindings("/interaction_profiles/valve/index_controller", kIndex, &kIndexFingers);
	SuggestBindings("/interaction_profiles/htc/vive_controller", kVive, &kTrackpadFingers);
	SuggestBindings("/interaction_profiles/microsoft/motion_controller", kWmr, &kTrackpadFingers);
	if (pico) SuggestBindings("/interaction_profiles/bytedance/pico4_controller", kTouch);
	SuggestBindings("/interaction_profiles/khr/simple_controller", kSimple);
	s_actionsReady = true;
	Log("vr controllers: ready (VRControllers=%d)", g_cfg.vrControllers);
}

void VrInputAttach(XrSession session) {
	s_session = session;
	s_attached = false;
	if (!s_actionsReady) return;
	XrSessionActionSetsAttachInfo info = {};
	info.type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO;
	info.countActionSets = 1;
	info.actionSets = &s_actionSet;
	s_attached = XR_SUCCEEDED(s_xr.xrAttachSessionActionSets(session, &info));
	s_profileDirty = true;
	if (!s_attached) {
		Log("vr controllers: could not attach the actions to the session");
		return;
	}
	for (int hand = 0; hand < 2; hand++) {
		XrActionSpaceCreateInfo spaceInfo = {XR_TYPE_ACTION_SPACE_CREATE_INFO};
		spaceInfo.action = s_actions[ACT_HAND_POSE];
		spaceInfo.subactionPath = s_handPaths[hand];
		spaceInfo.poseInActionSpace.orientation.w = 1;
		if (XR_FAILED(s_xr.xrCreateActionSpace(session, &spaceInfo, &s_handSpace[hand]))) {
			s_handSpace[hand] = XR_NULL_HANDLE;
			Log("vr controllers: no %s hand position (motion controls won't work)", hand ? "right" : "left");
		}
	}
	for (int hand = 0; hand < 2; hand++) {
		s_swings[hand] = {};
		s_lastHand[hand] = {};
	}
	if (s_handXr.xrCreateHandTrackerEXT) {
		for (int hand = 0; hand < 2; hand++) {
			XrHandTrackerCreateInfoEXT trackerInfo = {XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT};
			trackerInfo.hand = hand ? XR_HAND_RIGHT_EXT : XR_HAND_LEFT_EXT;
			trackerInfo.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
			XrResult result = s_handXr.xrCreateHandTrackerEXT(session, &trackerInfo, &s_handTracker[hand]);
			if (XR_FAILED(result)) {
				s_handTracker[hand] = XR_NULL_HANDLE;
				Log("vr controllers: no %s hand tracker (%d): its finger curls come from the buttons", hand ? "right" : "left", (int)result);
			}
		}
	}
	Log("motion controls: %s, %s sensitivity, %s", g_cfg.motionControls ? "on" : "off", g_cfg.swingSensitivity ? "normal" : "relaxed",
		kStabAttacks ? "slashes and stabs attack" : "only slashes attack (stabs off)");
}

void VrInputDetach() {
	for (int hand = 0; hand < 2; hand++) {
		if (s_handSpace[hand]) s_xr.xrDestroySpace(s_handSpace[hand]);
		s_handSpace[hand] = XR_NULL_HANDLE;
		s_attackHeldUntil[hand] = 0;
		if (s_handTracker[hand]) s_handXr.xrDestroyHandTrackerEXT(s_handTracker[hand]);
		s_handTracker[hand] = XR_NULL_HANDLE;
	}
	{
		ExclusiveLock guard(&s_fingerLock);
		s_fingers[0].valid = s_fingers[1].valid = false;
	}
	s_attached = false;
	s_session = XR_NULL_HANDLE;
	s_lastAmplitude[0] = s_lastAmplitude[1] = 0;
	s_profileName[0][0] = s_profileName[1][0] = 0;
	ExclusiveLock guard(&s_padLock);
	s_anyActive = s_everActive = false;
}

void VrInputShutdown() {
	VrInputDetach();
	if (s_actionSet) s_xr.xrDestroyActionSet(s_actionSet);  // destroys its actions too
	s_actionSet = XR_NULL_HANDLE;
	s_actionsReady = false;
}

// VR thread, from the OpenXR event loop: the controllers in use changed.
void VrInputProfileChanged() { s_profileDirty = true; }

// Each VR frame while the session is focused.
void VrInputSync(bool focused, bool foreground) {
	bool keyboardMouseUsed = KeyboardMouseUsed(foreground);  // polled every frame so key edges aren't missed
	if (!s_attached) return;
	XrActiveActionSet activeSet = {s_actionSet, XR_NULL_PATH};
	XrActionsSyncInfo syncInfo = {};
	syncInfo.type = XR_TYPE_ACTIONS_SYNC_INFO;
	syncInfo.countActiveActionSets = 1;
	syncInfo.activeActionSets = &activeSet;
	if (XR_FAILED(s_xr.xrSyncActions(s_session, &syncInfo))) return;
	LogProfiles();
	bool leftStickActive = false, rightStickActive = false;
	XrVector2f leftStick = StickDeadzone(GetStick(0, &leftStickActive)), rightStick = StickDeadzone(GetStick(1, &rightStickActive));
	bool leftTriggerActive = false, rightTriggerActive = false;
	float leftTrigger = TriggerDeadzone(GetFloat(ACT_TRIGGER, 0, &leftTriggerActive)), rightTrigger = TriggerDeadzone(GetFloat(ACT_TRIGGER, 1, &rightTriggerActive));
	bool active = leftStickActive || rightStickActive || leftTriggerActive || rightTriggerActive;
	XINPUT_GAMEPAD pad = {};
	WORD buttons = 0;
	if (GetBool(ACT_LOWER_BUTTON, 1)) buttons |= XINPUT_GAMEPAD_A;
	if (GetBool(ACT_UPPER_BUTTON, 1)) buttons |= XINPUT_GAMEPAD_B;
	if (GetBool(ACT_LOWER_BUTTON, 0)) buttons |= XINPUT_GAMEPAD_X;
	if (GetBool(ACT_UPPER_BUTTON, 0)) buttons |= XINPUT_GAMEPAD_Y;
	if (GetBool(ACT_STICK_CLICK, 0)) buttons |= XINPUT_GAMEPAD_LEFT_THUMB;
	if (GetBool(ACT_STICK_CLICK, 1)) buttons |= XINPUT_GAMEPAD_RIGHT_THUMB;
	if (GetFloat(ACT_GRIP, 0) > 0.5f) buttons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
	if (GetFloat(ACT_GRIP, 1) > 0.5f) buttons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
	pad.bLeftTrigger = (BYTE)(leftTrigger * 255.0f + 0.5f);
	pad.bRightTrigger = (BYTE)(rightTrigger * 255.0f + 0.5f);
	// left menu button: tap = Start, hold = Back, + left stick = D-pad (pure.h, test_vr_controllers)
	DWORD now = GetTickCount();
	buttons |= s_menuButton.update(GetBool(ACT_MENU, 0), &leftStick.x, &leftStick.y, now);
	if ((int32_t)(s_attackHeldUntil[1] - now) > 0) buttons |= XINPUT_GAMEPAD_X;  // motion controls (VrInputMotion)
	if ((int32_t)(s_attackHeldUntil[0] - now) > 0) buttons |= XINPUT_GAMEPAD_Y;
	// handoff between keyboard/mouse and the VR controllers
	{
		WORD pressed = buttons & ~s_prevVrButtons & ~(XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER);
		bool triggerPulled = (leftTrigger > 0.6f && s_prevTrigger[0] <= 0.6f) || (rightTrigger > 0.6f && s_prevTrigger[1] <= 0.6f);
		bool stickPushed = leftStick.x * leftStick.x + leftStick.y * leftStick.y > 0.7f * 0.7f || rightStick.x * rightStick.x + rightStick.y * rightStick.y > 0.7f * 0.7f;
		s_prevVrButtons = buttons;
		s_prevTrigger[0] = leftTrigger;
		s_prevTrigger[1] = rightTrigger;
		if (keyboardMouseUsed && !s_keyboardMouseMode) {
			s_keyboardMouseMode = true;
			s_toKeyboardCount++;
			if (s_toKeyboardCount <= 5 && g_cfg.vrControllers)
				Log("vr controllers: keyboard/mouse in use - VR controllers paused until you use them again%s",
					s_toKeyboardCount == 5 ? " (not logged again)" : "");
		} else if (s_keyboardMouseMode && !keyboardMouseUsed && (pressed || triggerPulled || stickPushed)) {
			s_keyboardMouseMode = false;
			s_toControllersCount++;
			if (s_toControllersCount <= 5 && g_cfg.vrControllers) Log("vr controllers: in use again");
		}
	}
	pad.wButtons = buttons;
	pad.sThumbLX = ToAxis(leftStick.x);
	pad.sThumbLY = ToAxis(leftStick.y);
	pad.sThumbRX = ToAxis(rightStick.x);
	pad.sThumbRY = ToAxis(rightStick.y);
	{
		ExclusiveLock guard(&s_padLock);
		s_gamepad = pad;
		s_anyActive = active;
		if (active) {
			s_gamepadTick = now;
			if (!s_everActive) Log("vr controllers: in use as gamepad %s", g_cfg.vrControllers ? "(on)" : "(off in the settings)");
			s_everActive = true;
		}
	}
	if (g_cfg.vrControllers && g_cfg.vrRumble && !s_keyboardMouseMode) UpdateRumble();
	else if (s_lastAmplitude[0] > 0.02f || s_lastAmplitude[1] > 0.02f) {
		s_rumbleLeft = 0;
		s_rumbleRight = 0;
		UpdateRumble();  // stops them
	}
}

// Game thread (XInput hook): the controllers' gamepad state. Returns false when there is no VR
// gamepad (setting off, or the mod's VR off). Otherwise pad 0 is connected from the start, even
// before the headset session runs or while the controllers sleep (a neutral pad then): the game
// never sees a pad come and go, and finds it even if it only looks for pads at startup.
// The settings menu reads them even when they are off for the game, so they can be turned back on.
bool VrInputPad(XINPUT_GAMEPAD* out, bool evenIfOff) {
	*out = {};
	if ((!g_cfg.vrControllers && !evenIfOff) || !g_cfg.enabled) return false;
	{
		SharedLock guard(&s_padLock);
		if (s_attached && s_anyActive && GetTickCount() - s_gamepadTick < 300 && (evenIfOff || !s_keyboardMouseMode)) *out = s_gamepad;
	}
	return true;
}

// Game thread (XInputSetState hook): rumble.
void VrInputRumble(WORD left, WORD right) {
	s_rumbleLeft = left;
	s_rumbleRight = right;
}

namespace {
// One space located at one time, with its velocities.
struct Located {
	bool valid;    // position and orientation known
	bool tracked;  // and actually tracked, not guessed
	XrPosef pose;
	XrVector3f linear, angular;  // m/s, rad/s in the base space
	bool linearValid, angularValid;
};

Located Locate(XrSpace space, XrSpace base, XrTime time) {
	XrSpaceVelocity velocity = {XR_TYPE_SPACE_VELOCITY};
	XrSpaceLocation location = {XR_TYPE_SPACE_LOCATION, &velocity};
	Located out = {};
	if (XR_FAILED(s_xr.xrLocateSpace(space, base, time, &location))) return out;
	const XrSpaceLocationFlags known = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
	out.valid = (location.locationFlags & known) == known;
	out.tracked = (location.locationFlags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT) != 0;
	out.pose = location.pose;
	out.linear = velocity.linearVelocity;
	out.angular = velocity.angularVelocity;
	out.linearValid = (velocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) != 0;
	out.angularValid = (velocity.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) != 0;
	return out;
}

pure::V3 ToV3(XrVector3f v) { return {v.x, v.y, v.z}; }
pure::Q ToQ(XrQuaternionf q) { return {q.x, q.y, q.z, q.w}; }

// The velocities a runtime didn't give, from the change since last frame (zero when there is none).
void FillMissingVelocities(Located& hand, const LastHand& last, double dt) {
	bool usable = last.valid && dt > 0.001 && dt < 0.1;
	if (!hand.linearValid) {
		pure::V3 v = usable ? (ToV3(hand.pose.position) - ToV3(last.pose.position)) / (float)dt : pure::V3{};
		hand.linear = {v.x, v.y, v.z};
	}
	if (!hand.angularValid) {
		// the turn from last frame's orientation to this one; for small angles its axis times the angle is
		// twice its vector part
		pure::Q turn = pure::q_mul(ToQ(hand.pose.orientation), pure::conj(ToQ(last.pose.orientation)));
		float shorterWay = turn.w < 0 ? -1.0f : 1.0f;
		pure::V3 w = usable ? pure::V3{turn.x, turn.y, turn.z} * (2 * shorterWay / (float)dt) : pure::V3{};
		hand.angular = {w.x, w.y, w.z};
	}
}

float Length(XrVector3f v) { return pure::fsqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
XrVector3f Sub(XrVector3f a, XrVector3f b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
void Raise(float& peak, float value) {
	if (value > peak) peak = value;
}

// The stats overlay's peaks for one hand this frame. dt = seconds since the last frame (0 = unknown);
// spin = null when the runtime gave no angular velocity.
void TrackHandPeaks(int hand, XrVector3f vel, const XrVector3f* spin, double dt) {
	constexpr float kDeg = 57.29578f;
	HandPeaks& peaks = s_handPeaks[hand];
	bool dtUsable = dt > 0.001 && dt < 0.1;
	Raise(peaks.speed, Length(vel));
	if (dtUsable && s_lastVelValid[hand]) Raise(peaks.accel, Length(Sub(vel, s_lastVel[hand])) / (float)dt);
	s_lastVel[hand] = vel;
	s_lastVelValid[hand] = true;
	if (spin) {
		Raise(peaks.spin, Length(*spin) * kDeg);
		if (dtUsable && s_lastSpinValid[hand]) Raise(peaks.spinAccel, Length(Sub(*spin, s_lastSpin[hand])) * kDeg / (float)dt);
		s_lastSpin[hand] = *spin;
	}
	s_lastSpinValid[hand] = spin != nullptr;
}

void SwingPulse(int hand) {
	XrHapticActionInfo hapticInfo = {XR_TYPE_HAPTIC_ACTION_INFO, nullptr, s_actions[ACT_RUMBLE], s_handPaths[hand]};
	XrHapticVibration vibration = {XR_TYPE_HAPTIC_VIBRATION};
	vibration.amplitude = 0.6f;
	vibration.duration = 30 * 1000 * 1000;  // 30 ms (ns)
	vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
	s_xr.xrApplyHapticFeedback(s_session, &hapticInfo, (const XrHapticBaseHeader*)&vibration);
}

// An attack: the hand's button (right = light attack X, left = heavy attack Y) for 100 ms, and a pulse.
void Attack(int hand, DWORD now) {
	s_attackHeldUntil[hand] = now + 100;
	s_attackCount[hand]++;
	SwingPulse(hand);
	ExclusiveLock guard(&s_padLock);  // into the pad at once (VrInputSync adds it from then on)
	s_gamepad.wButtons |= hand ? XINPUT_GAMEPAD_X : XINPUT_GAMEPAD_Y;
	s_gamepadTick = now;
}

// "motion controls: right slash locked (angular acceleration 23.1, needs 20.0)" and so on: the log
// shows each attack and why the ones that didn't fire were rejected.
void LogSwing(int hand, const pure::SwingStep& step, float armLength) {
	const char* side = hand ? "right" : "left";
	const char* kind = pure::swing_kind_name(step.kind);
	bool slash = step.kind == pure::SWING_SLASH;
	float travelScale = slash ? 57.29578f : 100.0f;  // degrees or cm
	const char* travelUnit = slash ? "deg" : "cm";
	if (step.locked) Log("motion controls: %s %s locked (%s %.1f, needs %.1f)", side, kind, step.lockedBy, (double)step.value, (double)step.limit);
	if (step.fired)
		Log("motion controls: %s %s ATTACK after %.0f %s (needs %.0f), power %.2f, arm %.2f m", side, kind, (double)(step.travel * travelScale),
			travelUnit, (double)(step.travelNeeded * travelScale), (double)step.power, (double)armLength);
	if (step.ended && step.hadFired)
		Log("motion controls: %s %s over (%s %.2f, limit %.2f), %.0f %s, power %.2f", side, kind, step.endedBy, (double)step.value,
			(double)step.limit, (double)(step.travel * travelScale), travelUnit, (double)step.power);
	else if (step.ended)
		Log("motion controls: %s %s rejected: %s %.2f (limit %.2f) after %.0f of %.0f %s", side, kind, step.endedBy, (double)step.value,
			(double)step.limit, (double)(step.travel * travelScale), (double)(step.travelNeeded * travelScale), travelUnit);
}
}  // namespace

void VrInputTakeHandPeaks(HandPeaks out[2]) {
	for (int hand = 0; hand < 2; hand++) {
		out[hand] = s_handPeaks[hand];
		s_handPeaks[hand] = {-1, -1, -1, -1};
	}
}

namespace {
// A hand's finger curls from its hand tracker (false: no tracker, or it isn't tracking the hand now).
bool CurlsFromJoints(int hand, XrSpace base, XrTime time, pure::FingerCurls* out) {
	if (!s_handTracker[hand] || !base) return false;
	XrHandJointLocationEXT joints[XR_HAND_JOINT_COUNT_EXT];
	XrHandJointLocationsEXT locations = {XR_TYPE_HAND_JOINT_LOCATIONS_EXT};
	locations.jointCount = XR_HAND_JOINT_COUNT_EXT;
	locations.jointLocations = joints;
	XrHandJointsLocateInfoEXT locateInfo = {XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT};
	locateInfo.baseSpace = base;
	locateInfo.time = time;
	if (XR_FAILED(s_handXr.xrLocateHandJointsEXT(s_handTracker[hand], &locateInfo, &locations)) || !locations.isActive) return false;
	pure::V3 positions[pure::kHandJoints];
	for (int joint = 0; joint < pure::kHandJoints; joint++) {
		if (!(joints[joint].locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) return false;
		positions[joint] = ToV3(joints[joint].pose.position);
	}
	*out = pure::finger_curls_from_joints(positions);
	return true;
}

// A hand's finger curls from its buttons (false: the controller isn't active).
bool CurlsFromButtons(int hand, pure::FingerCurls* out) {
	bool active = false;
	pure::FingerButtons buttons = {};
	buttons.trigger = GetFloat(ACT_TRIGGER, hand, &active);
	if (!active) return false;
	buttons.squeeze = GetFloat(ACT_GRIP, hand);
	buttons.triggerTouch = GetBool(ACT_TRIGGER_TOUCH, hand);
	buttons.thumbTouch = GetBool(ACT_THUMB_TOUCH, hand);
	*out = pure::finger_curls_from_buttons(buttons);
	return true;
}
}  // namespace

void VrInputFingers(XrSpace base, XrTime time) {
	if (!s_attached) return;
	for (int hand = 0; hand < 2; hand++) {
		pure::FingerCurls curls;
		bool fromJoints = CurlsFromJoints(hand, base, time, &curls);
		if (!fromJoints && !CurlsFromButtons(hand, &curls)) continue;
		if (s_loggedFingerSource[hand] != (int32_t)fromJoints) {
			s_loggedFingerSource[hand] = fromJoints;
			Log("vr controllers: %s hand's fingers from %s", hand ? "right" : "left", fromJoints ? "hand tracking" : "the buttons");
		}
		ExclusiveLock guard(&s_fingerLock);
		Fingers& fingers = s_fingers[hand];
		bool fresh = fingers.valid && fingers.fromJoints == fromJoints && GetTickCount() - fingers.tick < kFingersFreshMs;
		fingers.curls = fresh && !fromJoints ? pure::smooth_curls(fingers.curls, curls, kButtonSmoothing) : curls;
		fingers.valid = true;
		fingers.fromJoints = fromJoints;
		fingers.tick = GetTickCount();
	}
}

bool VrInputFingerCurls(int hand, float out[5]) {
	if (hand < 0 || hand > 1) return false;
	SharedLock guard(&s_fingerLock);
	const Fingers& fingers = s_fingers[hand];
	if (!fingers.valid || GetTickCount() - fingers.tick > kFingersFreshMs) return false;
	for (int finger = 0; finger < pure::FINGER_COUNT; finger++) out[finger] = fingers.curls.curl[finger];
	return true;
}

bool VrInputLocateHand(int hand, XrSpace base, XrTime time, XrPosef* out) {
	if (!s_attached || hand < 0 || hand > 1 || !s_handSpace[hand] || !base) return false;
	Located located = Locate(s_handSpace[hand], base, time);
	if (!located.valid) return false;
	*out = located.pose;
	return true;
}

// Each VR frame after VrInputSync: motion controls. Both hands and the head are located in `local` at
// the frame's display time; each hand's analyser turns its motion into slashes and stabs.
void VrInputMotion(XrSpace local, XrSpace view, XrTime time, bool focused, bool flipped) {
	if (!s_attached || !focused || !s_handSpace[0] || !s_handSpace[1] || !local || !view) return;
	Located head = Locate(view, local, time);
	if (!head.valid) return;
	pure::SwingProfile profile = pure::swing_profile(g_cfg.swingSensitivity);
	profile.stabs = kStabAttacks;
	bool attacksOn = g_cfg.motionControls && g_cfg.vrControllers && !MenuIsOpen() && !s_keyboardMouseMode;
	DWORD now = GetTickCount();
	for (int hand = 0; hand < 2; hand++) {
		Located located = Locate(s_handSpace[hand], local, time);
		LastHand& last = s_lastHand[hand];
		if (!located.valid) {
			last.valid = false;
			s_lastVelValid[hand] = s_lastSpinValid[hand] = false;
			continue;
		}
		double dt = last.valid ? (double)(time - last.time) / 1e9 : 0;
		FillMissingVelocities(located, last, dt);
		last = {true, time, located.pose};
		TrackHandPeaks(hand, located.linear, &located.angular, dt);
		pure::SwingSample sample = {time,
									located.tracked,
									ToV3(located.pose.position),
									ToQ(located.pose.orientation),
									ToV3(located.linear),
									ToV3(located.angular),
									ToV3(head.pose.position)};
		pure::SwingStep step = s_swings[hand].update(sample, profile);
		if (!attacksOn) continue;
		if (step.locked || step.fired || step.ended) LogSwing(hand, step, s_swings[hand].armLength());
		if (step.fired) Attack(hand, now);
	}
	if (now - s_infoTick >= 1000) {
		s_infoTick = now;
		char info[160];
		snprintf(info, sizeof(info), "Motion controls %s (%s): attacks left %u, right %u; arm length %.2f / %.2f m",
				 g_cfg.motionControls ? "on" : "off", g_cfg.swingSensitivity ? "normal" : "relaxed", s_attackCount[0], s_attackCount[1],
				 (double)s_swings[0].armLength(), (double)s_swings[1].armLength());
		MenuSetInfo(INFO_MOTION_CONTROLS, info);
	}
}
