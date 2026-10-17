// vrinput.cpp - VR controllers as a gamepad (0.4.0).
//
// The VR controllers (Quest/Touch, Index, Vive wands, Windows MR, Pico via SteamVR/VD, ...) are read
// through OpenXR actions and handed to the game as an Xbox-style gamepad: the XInput hook in
// menu.cpp merges this state into what XInputGetState returns for the first pad, so a real gamepad
// keeps working at the same time. Rumble from the game (XInputSetState) is played on the
// controllers. VRControllers=0 (or the menu) turns it off.
//
// Layout (Quest-style controllers; Index uses its A/B buttons on both hands the same way):
//   left stick / right stick      -> left stick / right stick
//   left / right trigger          -> LT / RT
//   left / right grip             -> LB / RB
//   right A / B                   -> A / B
//   left X / Y                    -> X / Y
//   stick clicks                  -> L3 / R3
//   left menu button, tap         -> Start
//   left menu button, hold 0.5 s  -> Back (View)
//   left menu button + left stick -> D-pad (while the menu button is held)
//   right menu (Vive, WMR)        -> B (these have no B button)
// Index has no menu button: pressing its left trackpad acts as the menu button.
// Pico 4 controllers use the Touch layout (their own profile when the runtime offers
// XR_BD_controller_interaction). PSVR2 Sense, Pico via SteamVR etc. are mapped onto one of these
// profiles by the runtime (SteamVR does this for any controller).
#include "xr.h"
#include <xinput.h>

namespace {
#define VR_FUNCS(X)                                                                                         \
    X(xrStringToPath) X(xrCreateActionSet) X(xrDestroyActionSet) X(xrCreateAction)                         \
    X(xrSuggestInteractionProfileBindings) X(xrAttachSessionActionSets) X(xrSyncActions)                   \
    X(xrGetActionStateBoolean) X(xrGetActionStateFloat) X(xrGetActionStateVector2f) X(xrApplyHapticFeedback) \
    X(xrStopHapticFeedback) X(xrGetCurrentInteractionProfile) X(xrPathToString)
struct {
#define DECL(n) PFN_##n n;
    VR_FUNCS(DECL)
#undef DECL
} F;

enum Act { A_STICK, A_CLICK, A_TRIGGER, A_GRIP, A_A, A_B, A_MENU, A_HAPTIC, A_COUNT };
XrInstance g_inst;
XrActionSet g_set;
XrAction g_act[A_COUNT];
XrPath g_hand[2];
XrSession g_session;
bool g_ready, g_attached;

// shared with the game thread
SRWLOCK g_lock = SRWLOCK_INIT;
XINPUT_GAMEPAD g_pad;
DWORD g_padTick;       // when g_pad was last updated with an active controller
bool g_anyActive, g_everActive;  // everActive: for the log
volatile LONG g_hapL, g_hapR;  // 0..65535 from XInputSetState
float g_lastHap[2];
DWORD g_hapTick[2];

pure::MenuButton g_menuBtn;  // Start / Back / D-pad from the left menu button

// Keyboard / mouse handoff (0.4.1): a key or mouse button pressed in the game pauses the VR
// controllers (the game sees a neutral pad, no rumble) until a button, trigger or stick on them is
// used on purpose. Controllers strapped to your hands while you type (Index) send nothing then.
bool g_kbmMode;
bool g_keyWas[256];
WORD g_prevVrButtons;
float g_prevTrig[2];
unsigned g_kbmSwitches, g_vrSwitches;

bool HandoffKey(int vk) {
    if (vk >= VK_F1 && vk <= VK_F24) return false;  // the mod's hotkeys
    switch (vk) {
        case VK_INSERT:  // settings menu
        case VK_SHIFT: case VK_CONTROL: case VK_MENU: case VK_LSHIFT: case VK_RSHIFT: case VK_LCONTROL:
        case VK_RCONTROL: case VK_LMENU: case VK_RMENU: case VK_LWIN: case VK_RWIN: case VK_APPS:
        case VK_SNAPSHOT: case VK_CAPITAL: case VK_NUMLOCK: case VK_SCROLL:
            return false;
    }
    return true;
}
// true when a key or mouse button was newly pressed while the game has focus
bool KeyboardMouseUsed(bool foreground) {
    bool used = false;
    for (int vk = 1; vk < 256; vk++) {
        if (vk == VK_CANCEL || vk == 0x07) continue;
        bool d = (GetAsyncKeyState(vk) & 0x8000) != 0;
        if (d && !g_keyWas[vk] && foreground && HandoffKey(vk)) used = true;
        g_keyWas[vk] = d;
    }
    return used;
}
char g_profile[2][128];

XrPath Path(const char* s) {
    XrPath p = XR_NULL_PATH;
    F.xrStringToPath(g_inst, s, &p);
    return p;
}

bool MakeAction(Act a, XrActionType type, const char* name, const char* loc) {
    XrActionCreateInfo ci = {};
    ci.type = XR_TYPE_ACTION_CREATE_INFO;
    ci.actionType = type;
    lstrcpynA(ci.actionName, name, XR_MAX_ACTION_NAME_SIZE);
    lstrcpynA(ci.localizedActionName, loc, XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
    ci.countSubactionPaths = 2;
    ci.subactionPaths = g_hand;
    return XR_SUCCEEDED(F.xrCreateAction(g_set, &ci, &g_act[a]));
}

struct Bind {
    Act a;
    const char* path;
};
void Suggest(const char* profile, const Bind* b, int n) {
    XrActionSuggestedBinding sb[40];
    int k = 0;
    for (int i = 0; i < n && k < 40; i++) {
        XrPath p = Path(b[i].path);
        if (p != XR_NULL_PATH) sb[k++] = {g_act[b[i].a], p};
    }
    XrInteractionProfileSuggestedBinding s = {};
    s.type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING;
    s.interactionProfile = Path(profile);
    s.countSuggestedBindings = (uint32_t)k;
    s.suggestedBindings = sb;
    XrResult r = F.xrSuggestInteractionProfileBindings(g_inst, &s);
    if (XR_FAILED(r)) Log("vr controllers: bindings for %s refused (%d)", profile, (int)r);
}

#define L "/user/hand/left/"
#define R "/user/hand/right/"
const Bind kTouch[] = {{A_STICK, L "input/thumbstick"}, {A_STICK, R "input/thumbstick"},
                       {A_CLICK, L "input/thumbstick/click"}, {A_CLICK, R "input/thumbstick/click"},
                       {A_TRIGGER, L "input/trigger/value"}, {A_TRIGGER, R "input/trigger/value"},
                       {A_GRIP, L "input/squeeze/value"}, {A_GRIP, R "input/squeeze/value"},
                       {A_A, L "input/x/click"}, {A_A, R "input/a/click"}, {A_B, L "input/y/click"}, {A_B, R "input/b/click"},
                       {A_MENU, L "input/menu/click"}, {A_HAPTIC, L "output/haptic"}, {A_HAPTIC, R "output/haptic"}};
const Bind kIndex[] = {{A_STICK, L "input/thumbstick"}, {A_STICK, R "input/thumbstick"},
                       {A_CLICK, L "input/thumbstick/click"}, {A_CLICK, R "input/thumbstick/click"},
                       {A_TRIGGER, L "input/trigger/value"}, {A_TRIGGER, R "input/trigger/value"},
                       {A_GRIP, L "input/squeeze/value"}, {A_GRIP, R "input/squeeze/value"},
                       {A_A, L "input/a/click"}, {A_A, R "input/a/click"}, {A_B, L "input/b/click"}, {A_B, R "input/b/click"},
                       {A_MENU, L "input/trackpad/force"}, {A_HAPTIC, L "output/haptic"}, {A_HAPTIC, R "output/haptic"}};
const Bind kPico[] = {{A_STICK, L "input/thumbstick"}, {A_STICK, R "input/thumbstick"},
                      {A_CLICK, L "input/thumbstick/click"}, {A_CLICK, R "input/thumbstick/click"},
                      {A_TRIGGER, L "input/trigger/value"}, {A_TRIGGER, R "input/trigger/value"},
                      {A_GRIP, L "input/squeeze/value"}, {A_GRIP, R "input/squeeze/value"},
                      {A_A, L "input/x/click"}, {A_A, R "input/a/click"}, {A_B, L "input/y/click"}, {A_B, R "input/b/click"},
                      {A_MENU, L "input/menu/click"}, {A_HAPTIC, L "output/haptic"}, {A_HAPTIC, R "output/haptic"}};
// Vive wands have few buttons: right trackpad click = A, right menu = B, left trackpad click = L3.
const Bind kVive[] = {{A_STICK, L "input/trackpad"}, {A_STICK, R "input/trackpad"},
                      {A_CLICK, L "input/trackpad/click"}, {A_A, R "input/trackpad/click"},
                      {A_TRIGGER, L "input/trigger/value"}, {A_TRIGGER, R "input/trigger/value"},
                      {A_GRIP, L "input/squeeze/click"}, {A_GRIP, R "input/squeeze/click"},
                      {A_MENU, L "input/menu/click"}, {A_B, R "input/menu/click"},
                      {A_HAPTIC, L "output/haptic"}, {A_HAPTIC, R "output/haptic"}};
const Bind kWmr[] = {{A_STICK, L "input/thumbstick"}, {A_STICK, R "input/thumbstick"},
                     {A_CLICK, L "input/thumbstick/click"}, {A_CLICK, R "input/thumbstick/click"},
                     {A_TRIGGER, L "input/trigger/value"}, {A_TRIGGER, R "input/trigger/value"},
                     {A_GRIP, L "input/squeeze/click"}, {A_GRIP, R "input/squeeze/click"},
                     {A_A, L "input/trackpad/click"}, {A_A, R "input/trackpad/click"},
                     {A_MENU, L "input/menu/click"}, {A_B, R "input/menu/click"},
                     {A_HAPTIC, L "output/haptic"}, {A_HAPTIC, R "output/haptic"}};
const Bind kSimple[] = {{A_TRIGGER, L "input/select/click"}, {A_TRIGGER, R "input/select/click"},
                        {A_MENU, L "input/menu/click"}, {A_B, R "input/menu/click"},
                        {A_HAPTIC, L "output/haptic"}, {A_HAPTIC, R "output/haptic"}};
#undef L
#undef R

bool GetBool(Act a, int hand) {
    XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO, nullptr, g_act[a], g_hand[hand]};
    XrActionStateBoolean s = {XR_TYPE_ACTION_STATE_BOOLEAN};
    return XR_SUCCEEDED(F.xrGetActionStateBoolean(g_session, &gi, &s)) && s.isActive && s.currentState;
}
float GetFloat(Act a, int hand, bool* active = nullptr) {
    XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO, nullptr, g_act[a], g_hand[hand]};
    XrActionStateFloat s = {XR_TYPE_ACTION_STATE_FLOAT};
    bool ok = XR_SUCCEEDED(F.xrGetActionStateFloat(g_session, &gi, &s)) && s.isActive;
    if (active) *active = ok;
    return ok ? s.currentState : 0;
}
XrVector2f GetStick(int hand, bool* active) {
    XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO, nullptr, g_act[A_STICK], g_hand[hand]};
    XrActionStateVector2f s = {XR_TYPE_ACTION_STATE_VECTOR2F};
    bool ok = XR_SUCCEEDED(F.xrGetActionStateVector2f(g_session, &gi, &s)) && s.isActive;
    *active = ok;
    return ok ? s.currentState : XrVector2f{0, 0};
}
SHORT Axis(float v) {
    if (v > 1) v = 1;
    if (v < -1) v = -1;
    return (SHORT)(v * 32767.0f);
}
// A resting controller can report a little stick or trigger; that alone shouldn't make the game
// switch its button prompts away from the keyboard.
XrVector2f StickDead(XrVector2f v) {
    if (v.x * v.x + v.y * v.y < 0.06f * 0.06f) return {0, 0};
    return v;
}
float TriggerDead(float v) { return v < 0.04f ? 0 : v; }

void Haptics() {
    float amp[2] = {(float)g_hapL / 65535.0f, (float)g_hapR / 65535.0f};
    for (int h = 0; h < 2; h++) {
        XrHapticActionInfo hi = {XR_TYPE_HAPTIC_ACTION_INFO, nullptr, g_act[A_HAPTIC], g_hand[h]};
        DWORD t = GetTickCount();
        if (amp[h] > 0.02f) {
            // renew a 100 ms pulse every 50 ms, or at once when the strength changes
            float d = amp[h] - g_lastHap[h];
            if (d < 0.05f && d > -0.05f && t - g_hapTick[h] < 50) continue;
            g_hapTick[h] = t;
            XrHapticVibration v = {XR_TYPE_HAPTIC_VIBRATION};
            v.amplitude = amp[h];
            v.duration = 100 * 1000 * 1000;  // 100 ms (ns)
            v.frequency = XR_FREQUENCY_UNSPECIFIED;
            F.xrApplyHapticFeedback(g_session, &hi, (const XrHapticBaseHeader*)&v);
        } else if (g_lastHap[h] > 0.02f) {
            F.xrStopHapticFeedback(g_session, &hi);
        }
        g_lastHap[h] = amp[h];
    }
}

// Which controllers the runtime bound: read once after the actions are attached and again whenever
// the runtime reports XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED (0.5.0; was polled every 2 s).
bool g_profileDirty = true;
void LogProfiles() {
    if (!g_profileDirty) return;
    g_profileDirty = false;
    for (int h = 0; h < 2; h++) {
        XrInteractionProfileState st = {XR_TYPE_INTERACTION_PROFILE_STATE};
        if (XR_FAILED(F.xrGetCurrentInteractionProfile(g_session, g_hand[h], &st))) continue;
        char name[128] = "(none)";
        uint32_t n = 0;
        if (st.interactionProfile != XR_NULL_PATH)
            F.xrPathToString(g_inst, st.interactionProfile, sizeof(name), &n, name);
        if (lstrcmpA(name, g_profile[h])) {
            lstrcpynA(g_profile[h], name, sizeof(g_profile[h]));
            Log("vr controllers: %s hand uses %s", h ? "right" : "left", name);
            char info[160];
            const char* a = g_profile[0][0] ? g_profile[0] : "(none)";
            const char* b = g_profile[1][0] ? g_profile[1] : "(none)";
            if (strstr(a, "/interaction_profiles/") == a) a += 22;
            if (strstr(b, "/interaction_profiles/") == b) b += 22;
            pure::format(info, sizeof(info), "VR controllers: left %s, right %s", a, b);
            MenuSetInfo(11, info);
        }
    }
}
}  // namespace

// After xrCreateInstance: actions and suggested bindings (must come before the session attaches).
void VrInputInit(PFN_xrGetInstanceProcAddr gipa, XrInstance inst, bool pico) {
    g_ready = false;
    g_inst = inst;
    bool ok = true;
#define LOAD(n)                                                              \
    if (XR_FAILED(gipa(inst, #n, (PFN_xrVoidFunction*)&F.n)) || !F.n) ok = false;
    VR_FUNCS(LOAD)
#undef LOAD
    if (!ok) {
        Log("vr controllers: OpenXR input functions missing; controllers not used");
        return;
    }
    g_hand[0] = Path("/user/hand/left");
    g_hand[1] = Path("/user/hand/right");
    XrActionSetCreateInfo si = {};
    si.type = XR_TYPE_ACTION_SET_CREATE_INFO;
    lstrcpynA(si.actionSetName, "gamepad", XR_MAX_ACTION_SET_NAME_SIZE);
    lstrcpynA(si.localizedActionSetName, "Gamepad", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE);
    if (XR_FAILED(F.xrCreateActionSet(inst, &si, &g_set))) {
        Log("vr controllers: could not create the action set");
        return;
    }
    ok = MakeAction(A_STICK, XR_ACTION_TYPE_VECTOR2F_INPUT, "stick", "Stick") &&
         MakeAction(A_CLICK, XR_ACTION_TYPE_BOOLEAN_INPUT, "stick_click", "Stick click") &&
         MakeAction(A_TRIGGER, XR_ACTION_TYPE_FLOAT_INPUT, "trigger", "Trigger") &&
         MakeAction(A_GRIP, XR_ACTION_TYPE_FLOAT_INPUT, "grip", "Grip") &&
         MakeAction(A_A, XR_ACTION_TYPE_BOOLEAN_INPUT, "button_lower", "A / X") &&
         MakeAction(A_B, XR_ACTION_TYPE_BOOLEAN_INPUT, "button_upper", "B / Y") &&
         MakeAction(A_MENU, XR_ACTION_TYPE_BOOLEAN_INPUT, "menu", "Menu (Start / Back / D-pad)") &&
         MakeAction(A_HAPTIC, XR_ACTION_TYPE_VIBRATION_OUTPUT, "rumble", "Rumble");
    if (!ok) {
        Log("vr controllers: could not create the actions");
        return;
    }
    Suggest("/interaction_profiles/oculus/touch_controller", kTouch, sizeof(kTouch) / sizeof(Bind));
    Suggest("/interaction_profiles/valve/index_controller", kIndex, sizeof(kIndex) / sizeof(Bind));
    Suggest("/interaction_profiles/htc/vive_controller", kVive, sizeof(kVive) / sizeof(Bind));
    Suggest("/interaction_profiles/microsoft/motion_controller", kWmr, sizeof(kWmr) / sizeof(Bind));
    if (pico) Suggest("/interaction_profiles/bytedance/pico4_controller", kPico, sizeof(kPico) / sizeof(Bind));
    Suggest("/interaction_profiles/khr/simple_controller", kSimple, sizeof(kSimple) / sizeof(Bind));
    g_ready = true;
    Log("vr controllers: ready (VRControllers=%d)", g_cfg.vrControllers);
}

void VrInputAttach(XrSession s) {
    g_session = s;
    g_attached = false;
    if (!g_ready) return;
    XrSessionActionSetsAttachInfo ai = {};
    ai.type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO;
    ai.countActionSets = 1;
    ai.actionSets = &g_set;
    g_attached = XR_SUCCEEDED(F.xrAttachSessionActionSets(s, &ai));
    g_profileDirty = true;
    if (!g_attached) Log("vr controllers: could not attach the actions to the session");
}

void VrInputDetach() {
    g_attached = false;
    g_session = XR_NULL_HANDLE;
    g_lastHap[0] = g_lastHap[1] = 0;
    g_profile[0][0] = g_profile[1][0] = 0;
    AcquireSRWLockExclusive(&g_lock);
    g_anyActive = g_everActive = false;
    ReleaseSRWLockExclusive(&g_lock);
}

void VrInputShutdown() {
    VrInputDetach();
    if (g_set) F.xrDestroyActionSet(g_set);  // destroys its actions too
    g_set = XR_NULL_HANDLE;
    g_ready = false;
}

// VR thread, from the OpenXR event loop: the controllers in use changed.
void VrInputProfileChanged() { g_profileDirty = true; }

// Each VR frame while the session is focused.
void VrInputSync(bool focused, bool foreground) {
    bool kbm = KeyboardMouseUsed(foreground);  // polled every frame so key edges aren't missed
    if (!g_attached || !focused) return;
    XrActiveActionSet as = {g_set, XR_NULL_PATH};
    XrActionsSyncInfo si = {};
    si.type = XR_TYPE_ACTIONS_SYNC_INFO;
    si.countActiveActionSets = 1;
    si.activeActionSets = &as;
    if (XR_FAILED(F.xrSyncActions(g_session, &si))) return;
    LogProfiles();
    bool la = false, ra = false;
    XrVector2f ls = StickDead(GetStick(0, &la)), rs = StickDead(GetStick(1, &ra));
    bool lt = false, rt = false;
    float ltv = TriggerDead(GetFloat(A_TRIGGER, 0, &lt)), rtv = TriggerDead(GetFloat(A_TRIGGER, 1, &rt));
    bool active = la || ra || lt || rt;
    XINPUT_GAMEPAD p = {};
    WORD b = 0;
    if (GetBool(A_A, 1)) b |= XINPUT_GAMEPAD_A;
    if (GetBool(A_B, 1)) b |= XINPUT_GAMEPAD_B;
    if (GetBool(A_A, 0)) b |= XINPUT_GAMEPAD_X;
    if (GetBool(A_B, 0)) b |= XINPUT_GAMEPAD_Y;
    if (GetBool(A_CLICK, 0)) b |= XINPUT_GAMEPAD_LEFT_THUMB;
    if (GetBool(A_CLICK, 1)) b |= XINPUT_GAMEPAD_RIGHT_THUMB;
    if (GetFloat(A_GRIP, 0) > 0.5f) b |= XINPUT_GAMEPAD_LEFT_SHOULDER;
    if (GetFloat(A_GRIP, 1) > 0.5f) b |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
    p.bLeftTrigger = (BYTE)(ltv * 255.0f + 0.5f);
    p.bRightTrigger = (BYTE)(rtv * 255.0f + 0.5f);
    // left menu button: tap = Start, hold = Back, + left stick = D-pad (pure.h, test_vrinput)
    DWORD t = GetTickCount();
    b |= g_menuBtn.update(GetBool(A_MENU, 0), &ls.x, &ls.y, t);
    // handoff between keyboard/mouse and the VR controllers
    {
        WORD pressed = b & ~g_prevVrButtons & ~(XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER);
        bool trig = (ltv > 0.6f && g_prevTrig[0] <= 0.6f) || (rtv > 0.6f && g_prevTrig[1] <= 0.6f);
        bool stick = ls.x * ls.x + ls.y * ls.y > 0.7f * 0.7f || rs.x * rs.x + rs.y * rs.y > 0.7f * 0.7f;
        g_prevVrButtons = b;
        g_prevTrig[0] = ltv;
        g_prevTrig[1] = rtv;
        if (kbm && !g_kbmMode) {
            g_kbmMode = true;
            g_kbmSwitches++;
            if (g_kbmSwitches <= 5 && g_cfg.vrControllers)
                Log("vr controllers: keyboard/mouse in use - VR controllers paused until you use them again%s",
                    g_kbmSwitches == 5 ? " (not logged again)" : "");
        } else if (g_kbmMode && !kbm && (pressed || trig || stick)) {
            g_kbmMode = false;
            g_vrSwitches++;
            if (g_vrSwitches <= 5 && g_cfg.vrControllers) Log("vr controllers: in use again");
        }
    }
    p.wButtons = b;
    p.sThumbLX = Axis(ls.x);
    p.sThumbLY = Axis(ls.y);
    p.sThumbRX = Axis(rs.x);
    p.sThumbRY = Axis(rs.y);
    AcquireSRWLockExclusive(&g_lock);
    g_pad = p;
    g_anyActive = active;
    if (active) {
        g_padTick = t;
        if (!g_everActive) Log("vr controllers: in use as gamepad %s", g_cfg.vrControllers ? "(on)" : "(off in the settings)");
        g_everActive = true;
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (g_cfg.vrControllers && g_cfg.vrRumble && !g_kbmMode) Haptics();
    else if (g_lastHap[0] > 0.02f || g_lastHap[1] > 0.02f) {
        InterlockedExchange(&g_hapL, 0);
        InterlockedExchange(&g_hapR, 0);
        Haptics();  // stops them
    }
}

// Game thread (XInput hook): the controllers' gamepad state. Returns false when there is no VR
// gamepad (setting off, or the mod's VR off). Otherwise pad 0 is connected from the start, even
// before the headset session runs or while the controllers sleep (a neutral pad then): the game
// never sees a pad come and go, and finds it even if it only looks for pads at startup.
// The settings menu reads them even when they are off for the game, so they can be turned back on.
bool VrInputPad(XINPUT_GAMEPAD* out, bool evenIfOff) {
    memset(out, 0, sizeof(*out));
    if ((!g_cfg.vrControllers && !evenIfOff) || !g_cfg.enabled) return false;
    AcquireSRWLockShared(&g_lock);
    if (g_attached && g_anyActive && GetTickCount() - g_padTick < 300 && (evenIfOff || !g_kbmMode)) *out = g_pad;
    ReleaseSRWLockShared(&g_lock);
    return true;
}

// Game thread (XInputSetState hook): rumble.
void VrInputRumble(WORD left, WORD right) {
    InterlockedExchange(&g_hapL, left);
    InterlockedExchange(&g_hapR, right);
}
