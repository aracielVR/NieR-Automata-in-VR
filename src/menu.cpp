// menu.cpp - the in-headset settings menu (0.3.29).
//
// Opened with Insert (keyboard) or by holding both stick buttons (L3 + R3) for half a second on a
// gamepad or the VR controllers. Navigation: D-pad / left stick up-down to choose, left-right to change, A to switch,
// LB / RB for the pages, B (or Insert) to close. While it is open the game doesn't see the
// gamepad (XInputGetState is hooked and returns a neutral pad). Every change applies at once and
// is written to NieRVR.ini; a few settings only take effect after a restart and say so.
//
// The menu is drawn on the CPU into a small RGBA image (font: DejaVu Sans, pre-rendered into
// src/gen/font.h by tools/make_font.py) which the VR thread shows on a quad in front of you.
#include "common.h"
#include <xinput.h>
#include "gen/font.h"

namespace {
// ---------------------------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------------------------
enum Type { T_BOOL, T_INT, T_FLOAT, T_ACTION, T_TEXT };
enum Flags { F_RESTART = 1, F_LOG = 2, F_RECENTER = 4, F_ALTERNATE = 8, F_BOTHEYES = 16 };  // ALTERNATE/BOTHEYES: only used in that mode
enum Action { A_RECENTER = 1, A_BOTHEYES_STATS };
struct Item {
    const char* label;
    Type type;
    const char* sec;
    const char* key;
    void* ptr;
    float min, max, step;
    const char* fmt;
    int flags;
    const char* help;
};

Item kView[] = {
    {"Both eyes every frame", T_BOOL, "Stereo", "BothEyes", &g_cfg.bothEyes, 0, 1, 1, nullptr, 0,
     "Draws the game's frame a second time for the right eye: both eyes show the same moment, 60 times a second. "
     "About twice the graphics work. Off: the eyes take turns (30 images a second each). Also F5."},
    {"World scale", T_FLOAT, "Stereo", "WorldScale", &g_cfg.worldScale, 0.2f, 3.0f, 0.05f, "%.3f", F_LOG,
     "Eye distance in the game world. Lower = the world looks bigger and the 3D flatter; higher = smaller, "
     "deeper. 0.75 is the default. Also Ctrl+F9 / Ctrl+F10."},
    {"Camera dolly", T_FLOAT, "Stereo", "CameraDolly", &g_cfg.cameraDolly, -3.0f, 3.0f, 0.05f, "%.2f m", 0,
     "Moves the camera along its line of sight towards the character (positive) for stronger 3D. Also Ctrl+F6 / Ctrl+F7."},
    {"Level the horizon", T_BOOL, "Stereo", "LevelCamera", &g_cfg.levelCamera, 0, 1, 1, nullptr, 0,
     "Removes the game camera's up/down tilt so the horizon stays level (comfort). Steep top-down views keep "
     "their tilt (see below)."},
    {"Keep tilt from", T_FLOAT, "Stereo", "SteepViewFrom", &g_cfg.steepFrom, 10, 89, 1, "%.0f deg", 0,
     "Camera pitch where the game's tilt starts to be kept (top-down sections)."},
    {"Keep full tilt at", T_FLOAT, "Stereo", "SteepViewFull", &g_cfg.steepFull, 10, 90, 1, "%.0f deg", 0,
     "Camera pitch from which the game's tilt is kept completely."},
    {"Top-down views flat", T_BOOL, "Stereo", "TopDownFlat", &g_cfg.topDownFlat, 0, 1, 1, nullptr, 0,
     "Shows steep top-down views (flight unit shooting sections) on the flat screen instead of in 3D. Also Shift+F12."},
    {"Fill eye shape", T_BOOL, "Stereo", "Anamorphic", &g_cfg.anamorphic, 0, 1, 1, nullptr, 0,
     "Stretches the image to each eye's own field of view (sharper). Off: a 16:9 picture that covers it."},
    {"Rendered field of view", T_FLOAT, "Stereo", "FovScale", &g_cfg.fovScale, 0.8f, 1.4f, 0.01f, "%.2fx", 0,
     "More than 1 draws a margin around your view: fewer black edges when you turn your head quickly or the "
     "game falls behind, at some cost in sharpness. 1.10 is the default; 1.00 is the sharpest."},
    {"Vertical fill", T_FLOAT, "Stereo", "VerticalFill", &g_cfg.verticalFill, 0.5f, 1.0f, 0.01f, "%.2f", 0,
     "Fraction of your vertical view that is drawn. Lower is sharper with black bars at the top and bottom."},
    {"Head pose prediction", T_BOOL, "Stereo", "PosePrediction", &g_cfg.posePrediction, 0, 1, 1, nullptr, 0,
     "Renders each game frame from where your head will be when it is shown (measured), not where it was: fewer "
     "black edges when you turn your head. The Info page shows the delay it predicts for."},
    {"Late warp", T_BOOL, "Stereo", "LateWarp", &g_cfg.lateWarp, 0, 1, 1, nullptr, 0,
     "Each headset frame, both eye images are re-aimed to where your head is now and cut to each eye's own field of "
     "view. Needed by headsets that stream (Quest, Pico): without it alternate eyes stutter and a Quest looks "
     "cross-eyed. Off: images are sent as rendered (0.5.0)."},
    {"Depth reprojection (experimental)", T_BOOL, "Stereo", "DepthReprojection", &g_cfg.depthReproject, 0, 1, 1, nullptr,
     F_ALTERNATE,
     "Experimental, always starts off. Alternate eyes only: the other eye's last image is moved to where the camera is now, using its depth, so both "
     "eyes follow camera motion 60 times a second. Off: each eye updates 30 times a second. No effect with both eyes "
     "every frame."},
    {"Reprojection gap fill (exp.)", T_BOOL, "Stereo", "ReprojectionEdges", &g_cfg.reprojectEdges, 0, 1, 1, nullptr, F_ALTERNATE,
     "Depth reprojection: where moving the image uncovers the background (around 2B), fill it with the background "
     "instead of stretching 2B's edge over it. Off: the 0.4.0 look, to compare."},
    {"Recenter", T_ACTION, nullptr, nullptr, (void*)A_RECENTER, 0, 0, 0, nullptr, 0,
     "Places the screen and the view straight ahead of you again (also F8)."},
};
Item kRender[] = {
    {"Ambient occlusion", T_BOOL, "Render", "AmbientOcclusion", &g_cfg.ambientOcclusion, 0, 1, 1, nullptr, 0,
     "NieR's ambient occlusion (soft contact shadows). Off removes it (it is half resolution and can look blotchy "
     "up close). Also Ctrl+F11."},
    {"Shadows", T_BOOL, "Render", "Shadows", &g_cfg.shadows, 0, 1, 1, nullptr, 0,
     "Off skips the game's screen-space shadow pass (no shadows from the sun and lights): faster. Also Ctrl+Shift+F11."},
    {"Vignette", T_BOOL, "Render", "Vignette", &g_cfg.vignette, 0, 1, 1, nullptr, 0,
     "The game's darkened screen edges. In VR they look like a dark frame, so off by default."},
    {"Low-health effect", T_BOOL, "Render", "LowHealthEffect", &g_cfg.lowHealthFx, 0, 1, 1, nullptr, 0,
     "The grey filter and ripples at low health while in stereo."},
    {"Screen distortions", T_BOOL, "Render", "DistortionEffects", &g_cfg.distortionFx, 0, 1, 1, nullptr, 0,
     "The glitch/distortion effects when hit. Off removes them in stereo (they redraw a flat copy of the screen)."},
    {"Fix lighting", T_BOOL, "Render", "FixLighting", &g_cfg.viewFix, 0, 1, 1, nullptr, 0,
     "Moves the game's lights and reflections into each eye's view. Leave on (off only to compare)."},
    {"Frame queue", T_INT, "Render", "MaxFrameLatency", &g_cfg.maxFrameLatency, 0, 3, 1, "%d", 0,
     "How many frames the game may prepare ahead of the graphics card. Each one delays what you see after a head "
     "turn by a frame. 1 = lowest delay (default); 2-3 may run a little faster; 0 = the game's own (3)."},
    {"Global illumination volumes", T_INT, "Render", "GIVolumes", &g_cfg.giVolumes, 8, 128, 8, "%d", 0,
     "How many GI volumes (the boxes that give places their reflections and ambient light) stay on, nearest to you "
     "first. Fewer = faster. 128 = all: no visual change, only empty slots are skipped. The Info page shows how many "
     "the area uses and the GI pass time. Idea from the FAR mod."},
    {"Reuse shadows (both eyes)", T_BOOL, "Render", "ReuseShadows", &g_cfg.reuseShadows, 0, 1, 1, nullptr, F_BOTHEYES,
     "Both-eye mode: the right eye reuses the left eye's shadow map instead of drawing it again (saves graphics work)."},
    {"Flat below draws", T_INT, "Stereo", "FlatBelowDraws", &g_cfg.flatBelowDraws, 0, 1000, 10, "%d", 0,
     "Frames with fewer draws than this are shown on the flat screen (title and loading screens)."},
    {"Extra resolutions", T_INT, "Render", "ExtraResolutions", &g_cfg.extraResolutions, 0, 2, 1, "%d", F_RESTART,
     "Adds resolutions above your monitor's to the game's list (1), and also patches its table (2)."},
};
Item kCull[] = {
    {"Head culling", T_BOOL, "Render", "HeadCulling", &g_cfg.headCulling, 0, 1, 1, nullptr, 0,
     "Turns the game's culling view the way your headset looks, so nothing vanishes when you look around. Also Ctrl+Shift+F5."},
    {"Head culling margin", T_FLOAT, "Render", "HeadCullingMargin", &g_cfg.headCullMargin, 0, 60, 1, "%.0f deg", 0,
     "Extra degrees around your view that are still drawn. More is safer for fast head turns, less is faster."},
    {"Culling field of view", T_FLOAT, "Render", "CullingFov", &g_cfg.cullFov, 0, 170, 5, "%.0f deg", 0,
     "Height of the game's culling view while in stereo (used as is when head culling is off). 0 = the game's own."},
    {"Wide projection", T_BOOL, "Render", "WideProjection", &g_cfg.wideProjection, 0, 1, 1, nullptr, 0,
     "Also gives the game camera's own projection the wide view (0.3.22 behaviour). Makes Pod 042 aim at the sky."},
    {"Model detail fix", T_BOOL, "Render", "LodFix", &g_cfg.lodFix, 0, 1, 1, nullptr, F_RESTART,
     "Keeps model and texture detail at the game's own level while culling is widened."},
};
Item kScreen[] = {
    {"Screen distance", T_FLOAT, "Screen", "Distance", &g_cfg.distance, 0.3f, 10, 0.05f, "%.2f m", F_RECENTER,
     "Distance of the flat screen and the HUD (also F6 / F7)."},
    {"Screen width", T_FLOAT, "Screen", "Width", &g_cfg.width, 0.2f, 10, 0.05f, "%.2f m", 0,
     "Width of the flat screen and the HUD (also F9 / F10)."},
    {"Screen height", T_FLOAT, "Screen", "HeightOffset", &g_cfg.heightOffset, -2, 2, 0.05f, "%.2f m", F_RECENTER,
     "Moves the flat screen and the HUD up or down."},
    {"HUD on its own layer", T_BOOL, "Stereo", "SplitUI", &g_cfg.uiSplit, 0, 1, 1, nullptr, 0,
     "Shows the HUD and menus on a flat layer in front of you instead of inside the 3D image."},
    {"Recenter", T_ACTION, nullptr, nullptr, (void*)A_RECENTER, 0, 0, 0, nullptr, 0, "Places the screen straight ahead of you again (also F8)."},
};
Item kInput[] = {
    {"VR controllers as gamepad", T_BOOL, "Input", "VRControllers", &g_cfg.vrControllers, 0, 1, 1, nullptr, 0,
     "Your VR controllers work as an Xbox gamepad: sticks, triggers, grips = LB/RB, A/B/X/Y, stick clicks. Left menu "
     "button: tap = Start, hold = Back, hold + left stick = D-pad. A real gamepad and the keyboard keep working. "
     "Turn off to play with only those."},
    {"Controller rumble", T_BOOL, "Input", "ControllerRumble", &g_cfg.vrRumble, 0, 1, 1, nullptr, 0,
     "The game's rumble is played on the VR controllers."},
};
const int kInfoLines = 16;
Item kInfo[kInfoLines];

struct Page {
    const char* name;
    Item* items;
    int count;
};
Page g_pages[] = {{"View", kView, sizeof(kView) / sizeof(Item)},
                  {"Rendering", kRender, sizeof(kRender) / sizeof(Item)},
                  {"Culling", kCull, sizeof(kCull) / sizeof(Item)},
                  {"Screen", kScreen, sizeof(kScreen) / sizeof(Item)},
                  {"Controls", kInput, sizeof(kInput) / sizeof(Item)},
                  {"Info", kInfo, kInfoLines}};
const int kPages = sizeof(g_pages) / sizeof(Page);

char g_infoText[kInfoLines][160];
SRWLOCK g_infoLock = SRWLOCK_INIT;

// ---------------------------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------------------------
volatile bool g_open, g_justOpened, g_wantRecenter;
volatile LONG g_blockPad;  // the game sees a neutral gamepad
bool g_waitRelease;        // after closing: keep blocking until every button is released
int g_page, g_sel[kPages], g_scroll[kPages];
bool g_dirty = true;
DWORD g_infoTick;

// ---------------------------------------------------------------------------------------------
// XInput
// ---------------------------------------------------------------------------------------------
// The same hooks also hand the VR controllers to the game as the first gamepad (vrinput.cpp,
// 0.4.0): their state is merged into what pad 0 reports (a real pad keeps working), pad 0 reads as
// connected while the VR controllers are in use, and the game's rumble goes to them.
typedef DWORD(WINAPI* PFN_XInputGetState)(DWORD, XINPUT_STATE*);
typedef DWORD(WINAPI* PFN_XInputGetCapabilities)(DWORD, DWORD, XINPUT_CAPABILITIES*);
typedef DWORD(WINAPI* PFN_XInputSetState)(DWORD, XINPUT_VIBRATION*);
struct PadHook {
    const wchar_t* dll;
    PFN_XInputGetState orig, origEx;
    PFN_XInputGetCapabilities origCaps;
    PFN_XInputSetState origSet;
    bool done;
};
PadHook g_pads[] = {{L"xinput1_4.dll"}, {L"xinput1_3.dll"}, {L"xinput9_1_0.dll"}, {L"xinput1_2.dll"}, {L"xinput1_1.dll"}};
PFN_XInputGetState g_readPad;  // for our own reading (an original, never blocked)

// Adds the VR controllers' state to a gamepad state: buttons combined, the stronger trigger and
// the stick that is pushed further.
void MergeVr(XINPUT_GAMEPAD& g, const XINPUT_GAMEPAD& v) {
    g.wButtons |= v.wButtons;
    if (v.bLeftTrigger > g.bLeftTrigger) g.bLeftTrigger = v.bLeftTrigger;
    if (v.bRightTrigger > g.bRightTrigger) g.bRightTrigger = v.bRightTrigger;
    auto mag = [](int x, int y) { return (long long)x * x + (long long)y * y; };
    if (mag(v.sThumbLX, v.sThumbLY) > mag(g.sThumbLX, g.sThumbLY)) g.sThumbLX = v.sThumbLX, g.sThumbLY = v.sThumbLY;
    if (mag(v.sThumbRX, v.sThumbRY) > mag(g.sThumbRX, g.sThumbRY)) g.sThumbRX = v.sThumbRX, g.sThumbRY = v.sThumbRY;
}

DWORD Filter(DWORD user, DWORD r, XINPUT_STATE* s) {
    if (!s) return r;
    XINPUT_GAMEPAD v;
    if (user == 0 && VrInputPad(&v)) {
        if (r != ERROR_SUCCESS) {  // no real pad: the VR controllers are pad 0
            memset(s, 0, sizeof(*s));
            r = ERROR_SUCCESS;
        }
        MergeVr(s->Gamepad, v);
        // games skip a state whose packet number didn't change: count changes of the merged state
        static XINPUT_GAMEPAD last;
        static DWORD packet = 1;
        if (memcmp(&last, &s->Gamepad, sizeof(last))) {
            last = s->Gamepad;
            packet++;
        }
        s->dwPacketNumber = packet;
    }
    if (r == ERROR_SUCCESS && g_blockPad) memset(&s->Gamepad, 0, sizeof(s->Gamepad));
    return r;
}
template <int I>
DWORD WINAPI H_XInputGetState(DWORD user, XINPUT_STATE* s) { return Filter(user, g_pads[I].orig(user, s), s); }
template <int I>
DWORD WINAPI H_XInputGetStateEx(DWORD user, XINPUT_STATE* s) { return Filter(user, g_pads[I].origEx(user, s), s); }
template <int I>
DWORD WINAPI H_XInputGetCapabilities(DWORD user, DWORD flags, XINPUT_CAPABILITIES* c) {
    DWORD r = g_pads[I].origCaps(user, flags, c);
    XINPUT_GAMEPAD v;
    if (r != ERROR_SUCCESS && user == 0 && c && VrInputPad(&v)) {
        memset(c, 0, sizeof(*c));  // describe an ordinary Xbox controller
        c->Type = XINPUT_DEVTYPE_GAMEPAD;
        c->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
        c->Gamepad.wButtons = 0xF3FF;
        c->Gamepad.bLeftTrigger = c->Gamepad.bRightTrigger = 0xFF;
        c->Gamepad.sThumbLX = c->Gamepad.sThumbLY = c->Gamepad.sThumbRX = c->Gamepad.sThumbRY = (SHORT)0xFFC0;
        c->Vibration.wLeftMotorSpeed = c->Vibration.wRightMotorSpeed = 0xFFFF;
        r = ERROR_SUCCESS;
    }
    return r;
}
template <int I>
DWORD WINAPI H_XInputSetState(DWORD user, XINPUT_VIBRATION* vib) {
    DWORD r = g_pads[I].origSet(user, vib);
    if (user == 0 && vib) {
        VrInputRumble(vib->wLeftMotorSpeed, vib->wRightMotorSpeed);
        XINPUT_GAMEPAD v;
        if (r != ERROR_SUCCESS && VrInputPad(&v)) r = ERROR_SUCCESS;
    }
    return r;
}

template <int I>
void HookPad() {
    PadHook& p = g_pads[I];
    if (p.done) return;
    HMODULE m = GetModuleHandleW(p.dll);
    if (!m) return;
    p.done = true;
    void* fn = (void*)GetProcAddress(m, "XInputGetState");
    void* ex = (void*)GetProcAddress(m, (LPCSTR)100);  // XInputGetStateEx (also reports the Guide button)
    bool ok = fn && HookFunction(fn, (void*)&H_XInputGetState<I>, (void**)&p.orig, "XInputGetState");
    if (ex && ex != fn) HookFunction(ex, (void*)&H_XInputGetStateEx<I>, (void**)&p.origEx, "XInputGetStateEx");
    void* caps = (void*)GetProcAddress(m, "XInputGetCapabilities");
    void* set = (void*)GetProcAddress(m, "XInputSetState");
    bool okCaps = caps && HookFunction(caps, (void*)&H_XInputGetCapabilities<I>, (void**)&p.origCaps, "XInputGetCapabilities");
    bool okSet = set && HookFunction(set, (void*)&H_XInputSetState<I>, (void**)&p.origSet, "XInputSetState");
    if (ok && !g_readPad) g_readPad = p.orig;
    Log("menu: gamepad input of %S %s%s", p.dll,
        ok ? "hooked (blocked from the game while the menu is open; VR controllers merged into pad 0)" : "could not be hooked",
        ok && !(okCaps && okSet) ? " - capabilities/rumble not hooked" : "");
}

bool ReadPhysicalPad(XINPUT_GAMEPAD* g) {
    if (!g_readPad) {
        static bool tried;
        if (!tried) {
            tried = true;
            HMODULE m = LoadLibraryW(L"xinput1_4.dll");
            if (!m) m = LoadLibraryW(L"xinput1_3.dll");
            if (m) g_readPad = (PFN_XInputGetState)GetProcAddress(m, "XInputGetState");
        }
        if (!g_readPad) return false;
    }
    for (DWORD u = 0; u < 4; u++) {
        XINPUT_STATE s = {};
        if (g_readPad(u, &s) == ERROR_SUCCESS) {
            *g = s.Gamepad;
            return true;
        }
    }
    return false;
}
bool ReadPad(XINPUT_GAMEPAD* g) {
    bool pad = ReadPhysicalPad(g);
    if (!pad) memset(g, 0, sizeof(*g));
    XINPUT_GAMEPAD v;
    if (VrInputPad(&v, true)) {
        MergeVr(*g, v);
        pad = true;
    }
    return pad;
}

// ---------------------------------------------------------------------------------------------
// Values
// ---------------------------------------------------------------------------------------------
float GetF(const Item& it) {
    if (it.type == T_FLOAT) return *(float*)it.ptr;
    return (float)*(int*)it.ptr;
}
void SetF(Item& it, float v) {
    if (v < it.min) v = it.min;
    if (v > it.max) v = it.max;
    if (it.type == T_FLOAT)
        *(float*)it.ptr = v;
    else
        *(int*)it.ptr = (int)(v + (v >= 0 ? 0.5f : -0.5f));
}
void Save(const Item& it) {
    if (!it.sec) return;
    char v[32];
    if (it.type == T_FLOAT)
        pure::format(v, sizeof(v), "%.3f", (double)*(float*)it.ptr);
    else
        pure::format(v, sizeof(v), "%d", *(int*)it.ptr);
    ConfigWrite(it.sec, it.key, v);
}
void Change(Item& it, int dir, int accel) {
    if (it.type == T_ACTION) {
        if (dir == 0 && (intptr_t)it.ptr == A_RECENTER) g_wantRecenter = true;
        return;
    }
    if (it.type == T_TEXT) return;
    if (it.type == T_BOOL) {
        *(int*)it.ptr = !*(int*)it.ptr;
    } else {
        float v = GetF(it), step = it.step * (float)accel;
        if (it.flags & F_LOG)
            v = dir > 0 ? v * (1 + step) : v / (1 + step);
        else
            v += dir * step;
        SetF(it, v);
    }
    if (it.flags & F_RECENTER) g_wantRecenter = true;
    Log("menu: %s = %s%s", it.label,
        it.type == T_BOOL ? (*(int*)it.ptr ? "on" : "off") : "", it.flags & F_RESTART ? " (after a restart)" : "");
    if (it.type != T_BOOL) {
        char v[48];
        pure::format(v, sizeof(v), it.fmt ? it.fmt : "%.2f", it.type == T_FLOAT ? (double)GetF(it) : (double)0);
        if (it.type == T_INT) pure::format(v, sizeof(v), it.fmt ? it.fmt : "%d", *(int*)it.ptr);
        Log("menu:   value %s", v);
    }
    Save(it);
    g_dirty = true;
}

// ---------------------------------------------------------------------------------------------
// Drawing (RGBA8, sRGB-encoded, opaque)
// ---------------------------------------------------------------------------------------------
const int kW = 1280, kH = 960;
uint32_t* g_px;
bool g_bgra;
struct Font {
    int w, h, line, ascent;
    const short (*glyph)[7];
    const unsigned char* atlas;
};
const Font kFontBody = {kBodyW, kBodyH, kBodyLine, kBodyAscent, kBodyGlyph, kBodyAtlas};
const Font kFontSmall = {kSmallW, kSmallH, kSmallLine, kSmallAscent, kSmallGlyph, kSmallAtlas};
const Font kFontTitle = {kTitleW, kTitleH, kTitleLine, kTitleAscent, kTitleGlyph, kTitleAtlas};

struct Rgb {
    int r, g, b;
};
const Rgb kBg = {30, 29, 26}, kText = {218, 212, 187}, kDim = {150, 145, 128}, kSelBg = {218, 212, 187},
          kSelText = {48, 45, 39}, kAccent = {196, 168, 106}, kBar = {70, 67, 60}, kRestart = {214, 120, 90};

uint32_t Pack(Rgb c) {
    return g_bgra ? (0xFF000000u | (uint32_t)c.r << 16 | (uint32_t)c.g << 8 | (uint32_t)c.b)
                  : (0xFF000000u | (uint32_t)c.b << 16 | (uint32_t)c.g << 8 | (uint32_t)c.r);
}
void Fill(int x, int y, int w, int h, Rgb c) {
    uint32_t v = Pack(c);
    for (int yy = y < 0 ? 0 : y; yy < y + h && yy < kH; yy++)
        for (int xx = x < 0 ? 0 : x; xx < x + w && xx < kW; xx++) g_px[yy * kW + xx] = v;
}
void Blend(int x, int y, Rgb c, int a) {
    if (x < 0 || y < 0 || x >= kW || y >= kH || a <= 0) return;
    uint32_t d = g_px[y * kW + x];
    int dr = g_bgra ? (d >> 16) & 255 : d & 255, dg = (d >> 8) & 255, db = g_bgra ? d & 255 : (d >> 16) & 255;
    Rgb o = {dr + (c.r - dr) * a / 255, dg + (c.g - dg) * a / 255, db + (c.b - db) * a / 255};
    g_px[y * kW + x] = Pack(o);
}
int TextWidth(const Font& f, const char* s) {
    int w = 0;
    for (; *s; s++) {
        int c = (unsigned char)*s;
        if (c < 32 || c > 126) c = '?';
        w += f.glyph[c - 32][6];
    }
    return w;
}
// y = top of the line
int Text(const Font& f, int x, int y, const char* s, Rgb col, int maxW = 100000) {
    int x0 = x;
    for (; *s; s++) {
        int c = (unsigned char)*s;
        if (c < 32 || c > 126) c = '?';
        const short* g = f.glyph[c - 32];
        if (x + g[6] - x0 > maxW) break;
        for (int yy = 0; yy < g[3]; yy++)
            for (int xx = 0; xx < g[2]; xx++)
                Blend(x + g[4] + xx, y + g[5] + yy, col, f.atlas[(g[1] + yy) * f.w + g[0] + xx]);
        x += g[6];
    }
    return x - x0;
}
// Word-wrapped text; returns the y after the last line.
int Wrapped(const Font& f, int x, int y, int maxW, const char* s, Rgb col, int maxLines) {
    char line[256];
    int lines = 0;
    while (*s && lines < maxLines) {
        int n = 0, lastSpace = -1;
        while (s[n] && n < 255) {
            line[n] = s[n];
            line[n + 1] = 0;
            if (TextWidth(f, line) > maxW) break;
            if (s[n] == ' ') lastSpace = n;
            n++;
        }
        int take = (s[n] && lastSpace > 0) ? lastSpace : n;
        if (take <= 0) take = n > 0 ? n : 1;
        memcpy(line, s, take);
        line[take] = 0;
        Text(f, x, y, line, col);
        y += f.line;
        s += take;
        while (*s == ' ') s++;
        lines++;
    }
    return y;
}

const int kRowsTop = 150, kRowH = 52, kRowsMax = 11;

void Draw() {
    Fill(0, 0, kW, kH, kBg);
    Fill(0, 0, kW, 4, kAccent);
    Text(kFontTitle, 40, 22, "NieRVR settings", kText);
    {
        const char* v = NIERVR_VERSION;
        Text(kFontSmall, kW - 40 - TextWidth(kFontSmall, v), 34, v, kDim);
    }
    // page tabs
    int x = 40;
    for (int i = 0; i < kPages; i++) {
        int w = TextWidth(kFontBody, g_pages[i].name) + 32;
        if (i == g_page) Fill(x, 88, w, 44, kSelBg);
        Text(kFontBody, x + 16, 92, g_pages[i].name, i == g_page ? kSelText : kDim);
        x += w + 8;
    }
    Fill(40, 136, kW - 80, 2, kBar);
    Page& p = g_pages[g_page];
    int sel = g_sel[g_page];
    int& scroll = g_scroll[g_page];
    if (sel < scroll) scroll = sel;
    if (sel >= scroll + kRowsMax) scroll = sel - kRowsMax + 1;
    for (int r = 0; r < kRowsMax && scroll + r < p.count; r++) {
        Item& it = p.items[scroll + r];
        int y = kRowsTop + r * kRowH;
        bool s = scroll + r == sel && it.type != T_TEXT;
        if (s) Fill(30, y, kW - 60, kRowH - 4, kSelBg);
        Rgb tc = s ? kSelText : kText;
        if (it.type == T_TEXT) {
            Text(kFontBody, 50, y + 8, it.label ? it.label : "", kText, kW - 100);
            continue;
        }
        Text(kFontBody, 50, y + 8, it.label, tc, 600);
        if (it.flags & F_RESTART) Text(kFontSmall, 50 + TextWidth(kFontBody, it.label) + 10, y + 14, "(restart)", s ? kSelText : kRestart);
        // settings that do nothing in the current eye mode say so (0.4.1)
        const char* unused = (it.flags & F_ALTERNATE) && g_cfg.bothEyes    ? "(not used: both eyes is on)"
                             : (it.flags & F_BOTHEYES) && !g_cfg.bothEyes ? "(not used: alternate eyes)"
                                                                          : nullptr;
        if (unused) Text(kFontSmall, 50 + TextWidth(kFontBody, it.label) + 10, y + 14, unused, s ? kSelText : kRestart);
        if (it.type == T_BOOL) {
            bool on = *(int*)it.ptr != 0;
            const char* t = on ? "ON" : "OFF";
            int bx = kW - 220;
            Fill(bx, y + 8, 140, 34, on && !unused ? kAccent : kBar);
            Text(kFontBody, bx + 70 - TextWidth(kFontBody, t) / 2, y + 7, t, on && !unused ? kSelText : kText);
        } else if (it.type == T_ACTION) {
            Text(kFontBody, kW - 220, y + 8, s ? "press A" : "", tc);
        } else {
            float v = GetF(it), t = (v - it.min) / (it.max - it.min);
            t = t < 0 ? 0 : (t > 1 ? 1 : t);
            int bx = 680, bw = 360;
            Fill(bx, y + 21, bw, 8, s ? Rgb{170, 164, 142} : kBar);
            Fill(bx, y + 21, (int)(bw * t), 8, kAccent);
            Fill(bx + (int)(bw * t) - 5, y + 13, 10, 24, s ? kSelText : kText);
            char val[48];
            if (it.type == T_INT && it.ptr == &g_cfg.giVolumes && *(int*)it.ptr >= 128)
                pure::format(val, sizeof(val), "128 (all)");
            else if (it.type == T_INT)
                pure::format(val, sizeof(val), it.fmt ? it.fmt : "%d", *(int*)it.ptr);
            else
                pure::format(val, sizeof(val), it.fmt ? it.fmt : "%.2f", (double)v);
            Text(kFontBody, bx + bw + 24, y + 8, val, tc);
        }
    }
    if (p.count > kRowsMax) {
        char m[48];
        pure::format(m, sizeof(m), "%d / %d", sel + 1, p.count);
        Text(kFontSmall, kW - 40 - TextWidth(kFontSmall, m), 100, m, kDim);
    }
    // help for the selected item
    Fill(40, 740, kW - 80, 2, kBar);
    if (sel < p.count && p.items[sel].help) Wrapped(kFontSmall, 40, 752, kW - 80, p.items[sel].help, kText, 3);
    Fill(40, 846, kW - 80, 2, kBar);
    Text(kFontSmall, 40, 858, "Gamepad:  Up/Down choose   Left/Right change   A switch   LB/RB page   B close", kDim, kW - 80);
    Text(kFontSmall, 40, 890, "Keyboard:  arrows   Enter   PgUp/PgDn   Insert to close", kDim, kW - 80);
}

// Moves the selection to the next selectable row.
void Move(int dir) {
    Page& p = g_pages[g_page];
    int s = g_sel[g_page];
    for (int k = 0; k < p.count; k++) {
        s = (s + dir + p.count) % p.count;
        if (p.items[s].type != T_TEXT) break;
    }
    g_sel[g_page] = s;
    g_dirty = true;
}

// Repeat for held directions.
struct Repeat {
    DWORD since, next;
    bool down;
    int Tick(bool now, DWORD t) {  // returns number of steps to apply (0/1), and acceleration via held time
        if (!now) {
            down = false;
            return 0;
        }
        if (!down) {
            down = true;
            since = t;
            next = t + 350;
            return 1;
        }
        if ((LONG)(t - next) >= 0) {
            next = t + (t - since > 1500 ? 40 : 80);
            return 1;
        }
        return 0;
    }
    int Accel(DWORD t) const { return down && t - since > 2500 ? 10 : (down && t - since > 1200 ? 3 : 1); }
};
Repeat g_up, g_down, g_left, g_right;
WORD g_prevButtons;
DWORD g_comboSince;
bool g_comboFired;  // the L3+R3 hold already opened/closed the menu (0.4.0 re-fired it every frame)
bool g_keyPrev[256];

bool KeyEdge(int vk, bool fg) {
    bool d = (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool e = d && !g_keyPrev[vk];
    g_keyPrev[vk] = d;
    return e && fg;
}
bool KeyDown(int vk, bool fg) { return fg && (GetAsyncKeyState(vk) & 0x8000) != 0; }

void Open(bool on) {
    if (g_open == on) return;
    g_open = on;
    if (on) {
        g_justOpened = true;
        g_blockPad = 1;
        g_dirty = true;
        Log("menu: opened");
    } else {
        g_waitRelease = true;
        Log("menu: closed");
    }
}
}  // namespace

// ---------------------------------------------------------------------------------------------
// API (VR thread unless noted)
// ---------------------------------------------------------------------------------------------
void MenuInstallInput() {
    HookPad<0>();
    HookPad<1>();
    HookPad<2>();
    HookPad<3>();
    HookPad<4>();
}

void MenuSetInfo(int line, const char* text) {
    if (line < 0 || line >= kInfoLines) return;
    AcquireSRWLockExclusive(&g_infoLock);
    pure::format(g_infoText[line], sizeof(g_infoText[line]), "%s", text);
    ReleaseSRWLockExclusive(&g_infoLock);
}

void MenuTick(bool foreground) {
    DWORD t = GetTickCount();
    static DWORD lastHook;
    if (t - lastHook > 3000) {  // the game may load XInput late
        lastHook = t;
        MenuInstallInput();
    }
    XINPUT_GAMEPAD g = {};
    bool pad = ReadPad(&g);
    WORD b = pad ? g.wButtons : 0;
    WORD pressed = b & ~g_prevButtons;
    g_prevButtons = b;
    bool ly = pad && g.sThumbLY > 20000, lyd = pad && g.sThumbLY < -20000, lx = pad && g.sThumbLX > 20000,
         lxl = pad && g.sThumbLX < -20000;

    // open / close
    bool combo = (b & XINPUT_GAMEPAD_LEFT_THUMB) && (b & XINPUT_GAMEPAD_RIGHT_THUMB);
    if (!combo) {
        g_comboSince = 0;
        g_comboFired = false;
    } else if (!g_comboSince) {
        g_comboSince = t ? t : 1;
    }
    bool comboFire = combo && !g_comboFired && t - g_comboSince > 450;
    if (comboFire) g_comboFired = true;  // once per hold
    bool keyToggle = KeyEdge(VK_INSERT, foreground);
    if (!g_open) {
        if (keyToggle || comboFire) Open(true);
    } else if (keyToggle || comboFire || (pressed & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_START))) {
        Open(false);
    }
    if (!g_open) {
        if (g_waitRelease && !b && !combo) {
            g_waitRelease = false;
            g_blockPad = 0;
        }
        static const int kNav[] = {VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_RETURN, VK_PRIOR, VK_NEXT};
        for (int vk : kNav) KeyEdge(vk, foreground);
        return;
    }
    // navigation
    int up = g_up.Tick((b & XINPUT_GAMEPAD_DPAD_UP) || ly || KeyDown(VK_UP, foreground), t);
    int down = g_down.Tick((b & XINPUT_GAMEPAD_DPAD_DOWN) || lyd || KeyDown(VK_DOWN, foreground), t);
    int left = g_left.Tick((b & XINPUT_GAMEPAD_DPAD_LEFT) || lxl || KeyDown(VK_LEFT, foreground), t);
    int right = g_right.Tick((b & XINPUT_GAMEPAD_DPAD_RIGHT) || lx || KeyDown(VK_RIGHT, foreground), t);
    bool ok = (pressed & XINPUT_GAMEPAD_A) || KeyEdge(VK_RETURN, foreground);
    bool pgL = (pressed & XINPUT_GAMEPAD_LEFT_SHOULDER) || KeyEdge(VK_PRIOR, foreground);
    bool pgR = (pressed & XINPUT_GAMEPAD_RIGHT_SHOULDER) || KeyEdge(VK_NEXT, foreground);
    if (pgL || pgR) {
        g_page = (g_page + (pgR ? 1 : kPages - 1)) % kPages;
        if (g_pages[g_page].items[g_sel[g_page]].type == T_TEXT) Move(1);
        g_dirty = true;
    }
    if (up) Move(-1);
    if (down) Move(1);
    Page& p = g_pages[g_page];
    Item& it = p.items[g_sel[g_page]];
    if (it.type == T_BOOL || it.type == T_ACTION) {
        if (ok || ((left || right) && it.type == T_BOOL)) Change(it, 0, 1);
    } else if (it.type == T_INT || it.type == T_FLOAT) {
        if (right) Change(it, 1, g_right.Accel(t));
        if (left) Change(it, -1, g_left.Accel(t));
    }
    // the Info page refreshes twice a second
    if (g_page == kPages - 1 && t - g_infoTick > 500) {
        g_infoTick = t;
        AcquireSRWLockShared(&g_infoLock);
        for (int i = 0; i < kInfoLines; i++) {
            kInfo[i].type = T_TEXT;
            kInfo[i].label = g_infoText[i];
        }
        ReleaseSRWLockShared(&g_infoLock);
        g_dirty = true;
    }
}

bool MenuIsOpen() { return g_open; }
bool MenuTakeJustOpened() {
    bool j = g_justOpened;
    g_justOpened = false;
    return j;
}
bool MenuTakeRecenter() {
    bool r = g_wantRecenter;
    g_wantRecenter = false;
    return r;
}
// The menu image when it changed since the last call (else null). bgra: the swapchain's byte order.
const uint32_t* MenuImage(bool bgra, bool force, int* w, int* h) {
    *w = kW;
    *h = kH;
    if (!g_px) g_px = (uint32_t*)VirtualAlloc(nullptr, kW * kH * 4, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!g_px) return nullptr;
    if (bgra != g_bgra) {
        g_bgra = bgra;
        g_dirty = true;
    }
    if (!g_dirty && !force) return nullptr;
    g_dirty = false;
    for (int i = 0; i < kInfoLines; i++)
        if (!kInfo[i].label) {
            kInfo[i].type = T_TEXT;
            kInfo[i].label = g_infoText[i];
        }
    Draw();
    return g_px;
}
