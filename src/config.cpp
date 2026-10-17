// config.cpp - NieRVR.ini handling.
#include "common.h"

Config g_cfg;

static const char kDefaultIni[] =
    "; NieRVR configuration. Delete this file to restore defaults.\r\n"
    "\r\n"
    "[VR]\r\n"
    "; 0 = mod stays passive (game runs normally, nothing is sent to the headset)\r\n"
    "Enabled=1\r\n"
    "\r\n"
    "[Input]\r\n"
    "; 1 = your VR controllers work as a gamepad (Quest/Touch, Index, Pico, Vive, WMR...). A real\r\n"
    "; gamepad and the keyboard keep working too. 0 = off (also in the in-headset menu, Insert)\r\n"
    "VRControllers=1\r\n"
    "; 1 = the game's rumble is played on the VR controllers\r\n"
    "ControllerRumble=1\r\n"
    "\r\n"
    "[Screen]\r\n"
    "; Virtual screen placement, in metres. Also adjustable in game with the hotkeys below.\r\n"
    "Distance=2.5\r\n"
    "Width=3.2\r\n"
    "HeightOffset=0.0\r\n"
    "\r\n"
    "[Stereo]\r\n"
    "; 1 = start in stereo 3D (F12 toggles between stereo and the flat screen)\r\n"
    "Enabled=1\r\n"
    "; Game units per metre: scales eye separation and head movement (bigger = world looks smaller)\r\n"
    "WorldScale=0.75\r\n"
    "; 1 = keep the horizon level (removes the game camera's up/down tilt; more comfortable)\r\n"
    "LevelCamera=1\r\n"
    "; ...except in steep views (top-down sections): from this camera pitch (degrees) the tilt is\r\n"
    "; kept more and more, fully from SteepViewFull, so the arena is in front of you\r\n"
    "SteepViewFrom=45\r\n"
    "SteepViewFull=60\r\n"
    "; EXPERIMENTAL, alternate eyes only: the eye drawn in the previous game frame is moved to the new\r\n"
    "; camera using its depth. Always starts OFF (switch it on in the in-headset menu for a test)\r\n"
    "DepthReprojection=0\r\n"
    "; Depth reprojection: 1 = fill the gaps it opens behind 2B and other near things with the\r\n"
    "; background; 0 = stretch their edges over the gap (0.3.30/0.4.0 look)\r\n"
    "ReprojectionEdges=1\r\n"
    "; Experimental: 1 = both eyes every game frame (the frame is drawn a second time for the right\r\n"
    "; eye: no alternating, but about twice the GPU work). 0 = alternate eyes. F5 switches it in game\r\n"
    "BothEyes=0\r\n"
    "; 1 = predict the head pose each game frame is rendered with for the moment it will actually be\r\n"
    "; shown (measured), so fast head turns show fewer black edges. 0 = as before 0.4.4\r\n"
    "PosePrediction=1\r\n"
    "; 1 = each headset frame, both eye images are re-aimed to the headset's own pose and cut to each\r\n"
    "; eye's own field of view before they are sent (needed by streaming headsets: Quest, Pico, ...).\r\n"
    "; 0 = sent as rendered (0.5.0 and earlier)\r\n"
    "LateWarp=1\r\n"
    "; 1 = show steep top-down views (e.g. the flight-unit shooting sections) on the flat screen\r\n"
    "; instead of in 3D. Shift+F12 in game. Goes flat after 1 s at TopDownFlatFrom degrees or more,\r\n"
    "; back to 3D after 2 s below TopDownFlatUntil.\r\n"
    "TopDownFlat=0\r\n"
    "TopDownFlatFrom=50\r\n"
    "TopDownFlatUntil=35\r\n"
    "; Rendered field of view relative to the headset's. 1.10 (default) leaves a little picture beyond\r\n"
    "; your view so fast head turns don't show black edges; 1.0 = exact (sharpest)\r\n"
    "FovScale=1.10\r\n"
    "; Metres to move the camera towards the character along its line of sight: closer = stronger 3D\r\n"
    "; without shrinking the world (Ctrl+F6 / Ctrl+F7 in game)\r\n"
    "CameraDolly=0.0\r\n"
    "; 1 = stretch the game image to each eye's own field of view (sharper, no cropping); 0 = off\r\n"
    "Anamorphic=1\r\n"
    "; Fraction of your vertical field of view to render (1 = all; lower = sharper, black top/bottom edges)\r\n"
    "VerticalFill=1.0\r\n"
    "; 1 = HUD and menus on a flat layer in front of you, 3D scene in stereo\r\n"
    "SplitUI=1\r\n"
    "; Frames with fewer draw calls than this are shown flat (title and New Game screens have up to ~190,\r\n"
    "; gameplay 500+). 0 = never\r\n"
    "FlatBelowDraws=250\r\n"
    "\r\n"
    "[Render]\r\n"
    "; EXPERIMENTAL supersampling: 1 = offer resolutions above your monitor's (125%-300%) in the game's\r\n"
    "; settings; 2 = also rewrite the game's built-in resolution list (try if 1 shows nothing new).\r\n"
    "; Use the game's windowed/borderless mode with them. See the README.\r\n"
    "ExtraResolutions=1\r\n"
    "; 0 = turn off ambient occlusion (NieR's half-resolution SAO: blotchy/pixelated on things close to\r\n"
    "; the camera, e.g. 2B with CameraDolly). Ctrl+F11 toggles it in game.\r\n"
    "AmbientOcclusion=1\r\n"
    "; 0 = no shadows (test switch for the shadow pass; Ctrl+Shift+F11 in game)\r\n"
    "Shadows=1\r\n"
    "; 0 = remove the game's vignette (darker screen edges: in VR they show as a dark frame)\r\n"
    "Vignette=0\r\n"
    "; 0 = no grey colour filter at low health while in stereo (1 = keep the game's effect)\r\n"
    "LowHealthEffect=0\r\n"
    "; 0 = no screen distortion/glitch effects while in stereo (when hit, at low health): they redraw\r\n"
    "; a flat copy of the whole screen. 1 = keep them (drawn into the 3D image)\r\n"
    "DistortionEffects=0\r\n"
    "; 1 = fix NieR's tiled lighting and reflections for the VR camera (0 only for comparison)\r\n"
    "FixLighting=1\r\n"
    "; Global illumination volumes that stay on, nearest to the camera first: fewer = faster. 128 = all\r\n"
    "; (no visual change; empty slots are still skipped). Idea from the FAR mod (DrDaxxy, Kaldaien)\r\n"
    "GIVolumes=128\r\n"
    "; Frames the game may prepare ahead of the graphics card. Each one delays what you see after a\r\n"
    "; head turn by a frame. 1 = lowest delay (default), 2-3 = a little smoother, 0 = the game's own\r\n"
    "MaxFrameLatency=1\r\n"
    "; Vertical field of view (degrees) the game uses to decide what to draw while in stereo, so things\r\n"
    "; at the edges of your view and where you look don't vanish. 0 = the game's own (~50).\r\n"
    "; At most 170: a field of view of 180 or more doesn't exist (170 covers ~88 deg to each side).\r\n"
    "CullingFov=170\r\n"
    "; 1 = only the camera's projection and culling use the wider view above; the camera itself and\r\n"
    "; model/texture detail keep the game's own (0 = old way: low-detail models in stereo)\r\n"
    "LodFix=1\r\n"
    "; 1 = the culling view (above) is turned the way your headset looks, not where the game camera\r\n"
    "; points, so turning your head doesn't make things vanish. Ctrl+Shift+F5 switches it in game\r\n"
    "HeadCulling=1\r\n"
    "; Places in the game (exe offsets, comma separated) that always get the game's own field of view:\r\n"
    "; 0x729b49 = the camera code that also aims Pod 042 (the wide view made it shoot at the sky)\r\n"
    "FovRealCallers=0x729b49\r\n"
    "; 1 = the game camera's own projection also gets the wide view (0.3.22; makes Pod 042 aim at the sky)\r\n"
    "WideProjection=0\r\n"
    "; Both eyes: the right eye reuses the left eye's shadow map (saves graphics work; 0 = draw it again)\r\n"
    "ReuseShadows=1\r\n"
    "; With HeadCulling=1 the culling view only needs to hold your headset's view: this many degrees\r\n"
    "; are added on every side (more = safer when turning your head fast, less = faster).\r\n"
    "; 0 = use CullingFov as before.\r\n"
    "HeadCullingMargin=20\r\n"
    "\r\n"
    "[Keys]\r\n"
    "; Windows virtual-key codes (https://learn.microsoft.com/windows/win32/inputdev/virtual-key-codes)\r\n"
    "; Defaults: F8 recenter, F9 smaller, F10 larger, F6 closer, F7 farther, F12 stereo on/off\r\n"
    "; Ctrl+F9 / Ctrl+F10: world bigger (flatter) / smaller (deeper) - WorldScale, saved automatically\r\n"
    "; Ctrl+F6 / Ctrl+F7: camera closer to / farther from the character - CameraDolly, saved automatically\r\n"
    "Recenter=0x77\r\n"
    "Smaller=0x78\r\n"
    "Larger=0x79\r\n"
    "Closer=0x75\r\n"
    "Farther=0x76\r\n"
    "ToggleStereo=0x7B\r\n"
    "\r\n"
    "[OpenXR]\r\n"
    "; Leave empty for automatic: uses openxr_loader.dll if it sits next to this mod\r\n"
    "; (enables API layers such as OpenXR Toolkit), otherwise talks to the active runtime directly.\r\n"
    "LoaderPath=\r\n"
    "\r\n"
    "[Loader]\r\n"
    "; Another d3d11.dll-based mod (e.g. ReShade renamed to d3d11_reshade.dll) to load in a chain.\r\n"
    "; Leave empty to use Windows' own d3d11.dll.\r\n"
    "ChainLoad=\r\n"
    "\r\n"
    "[Debug]\r\n"
    "; 1 = more detail in NieRVR.log\r\n"
    "Verbose=0\r\n"
    "; Camera probe: press this key in game to record one frame for analysis (default F11)\r\n"
    "CaptureKey=0x7A\r\n"
    "; 0 = disable the capture key\r\n"
    "Probe=1\r\n";

static float ReadFloat(const wchar_t* sec, const wchar_t* key, float def) {
    wchar_t buf[64];
    GetPrivateProfileStringW(sec, key, L"", buf, 64, g_iniPath);
    float v;
    return pure::parse_float(buf, &v) ? v : def;
}

static int ReadInt(const wchar_t* sec, const wchar_t* key, int def) {
    wchar_t buf[64];
    GetPrivateProfileStringW(sec, key, L"", buf, 64, g_iniPath);
    int v;
    return pure::parse_int(buf, &v) ? v : def;
}

static float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// An ini from an older version keeps working, but lacks the settings added since: copy every
// "key=value" of the default ini whose key the file doesn't have yet (without the comments).
static int AddMissingSettings() {
    int added = 0;
    wchar_t section[64] = L"";
    const char* p = kDefaultIni;
    while (*p) {
        const char* e = p;
        while (*e && *e != '\r' && *e != '\n') e++;
        size_t n = (size_t)(e - p);
        if (n > 2 && p[0] == '[') {
            size_t k = 0;
            for (const char* q = p + 1; q < e && *q != ']' && k < 63; q++) section[k++] = (wchar_t)*q;
            section[k] = 0;
        } else if (n > 1 && p[0] != ';' && section[0]) {
            const char* eq = p;
            while (eq < e && *eq != '=') eq++;
            if (eq < e) {
                wchar_t key[64], val[256], have[8];
                size_t k = 0;
                for (const char* q = p; q < eq && k < 63; q++) key[k++] = (wchar_t)*q;
                key[k] = 0;
                k = 0;
                for (const char* q = eq + 1; q < e && k < 255; q++) val[k++] = (wchar_t)*q;
                val[k] = 0;
                GetPrivateProfileStringW(section, key, L"\x01", have, 8, g_iniPath);
                if (have[0] == 1 && have[1] == 0) {
                    WritePrivateProfileStringW(section, key, val, g_iniPath);
                    added++;
                }
            }
        }
        p = e;
        while (*p == '\r' || *p == '\n') p++;
    }
    return added;
}

void ConfigLoad() {
    if (GetFileAttributesW(g_iniPath) == INVALID_FILE_ATTRIBUTES) {
        HANDLE f = CreateFileW(g_iniPath, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD w;
            WriteFile(f, kDefaultIni, sizeof(kDefaultIni) - 1, &w, nullptr);
            CloseHandle(f);
        }
    }
    g_cfg.iniAdded = AddMissingSettings();
    GetPrivateProfileStringW(L"Loader", L"ChainLoad", L"", g_cfg.chainLoad, MAX_PATH, g_iniPath);
    GetPrivateProfileStringW(L"OpenXR", L"LoaderPath", L"", g_cfg.loaderPath, MAX_PATH, g_iniPath);
    g_cfg.enabled = ReadInt(L"VR", L"Enabled", 1);
    g_cfg.distance = Clamp(ReadFloat(L"Screen", L"Distance", 2.5f), 0.3f, 50.f);
    g_cfg.width = Clamp(ReadFloat(L"Screen", L"Width", 3.2f), 0.2f, 100.f);
    g_cfg.heightOffset = Clamp(ReadFloat(L"Screen", L"HeightOffset", 0.f), -10.f, 10.f);
    g_cfg.keyRecenter = ReadInt(L"Keys", L"Recenter", VK_F8);
    g_cfg.keySmaller = ReadInt(L"Keys", L"Smaller", VK_F9);
    g_cfg.keyLarger = ReadInt(L"Keys", L"Larger", VK_F10);
    g_cfg.keyCloser = ReadInt(L"Keys", L"Closer", VK_F6);
    g_cfg.keyFarther = ReadInt(L"Keys", L"Farther", VK_F7);
    g_cfg.verbose = ReadInt(L"Debug", L"Verbose", 0);
    g_cfg.keyCapture = ReadInt(L"Debug", L"CaptureKey", VK_F11);
    g_cfg.probe = ReadInt(L"Debug", L"Probe", 1);
    // 0.5.0: new defaults (user-tested): an ini from before 0.5.0 that still holds the old default
    // gets the new one; values that were changed are kept. Runs once ([VR] SettingsVersion=50).
    if (ReadInt(L"VR", L"SettingsVersion", 0) < 50) {
        float fs = ReadFloat(L"Stereo", L"FovScale", 1.1f), ws = ReadFloat(L"Stereo", L"WorldScale", 0.75f);
        if (fs > 0.999f && fs < 1.001f) {
            WritePrivateProfileStringW(L"Stereo", L"FovScale", L"1.10", g_iniPath);
            Log("config: FovScale 1.0 (the old default) -> 1.10 (the new default)");
        }
        if (ws > 0.929f && ws < 0.931f) {
            WritePrivateProfileStringW(L"Stereo", L"WorldScale", L"0.75", g_iniPath);
            Log("config: WorldScale 0.93 (the old default) -> 0.75 (the new default)");
        }
        WritePrivateProfileStringW(L"VR", L"SettingsVersion", L"50", g_iniPath);
    }
    g_cfg.stereo = ReadInt(L"Stereo", L"Enabled", 1);
    g_cfg.worldScale = Clamp(ReadFloat(L"Stereo", L"WorldScale", 0.75f), 0.002f, 50.f);
    g_cfg.levelCamera = ReadInt(L"Stereo", L"LevelCamera", 1);
    // 0.3.12's defaults (55/75) left the flight-unit view half levelled: files still holding exactly
    // those get the new defaults.
    float sf = ReadFloat(L"Stereo", L"SteepViewFrom", 45.f), sl = ReadFloat(L"Stereo", L"SteepViewFull", 60.f);
    if (sf > 54.9f && sf < 55.1f && sl > 74.9f && sl < 75.1f) {
        sf = 45.f;
        sl = 60.f;
    }
    g_cfg.steepFrom = Clamp(sf, 10.f, 89.f);
    g_cfg.steepFull = Clamp(sl, g_cfg.steepFrom, 90.f);
    g_cfg.topDownFlat = ReadInt(L"Stereo", L"TopDownFlat", 0);
    g_cfg.topDownFrom = Clamp(ReadFloat(L"Stereo", L"TopDownFlatFrom", 50.f), 10.f, 89.f);
    g_cfg.topDownUntil = Clamp(ReadFloat(L"Stereo", L"TopDownFlatUntil", 35.f), 0.f, g_cfg.topDownFrom);
    g_cfg.fovScale = Clamp(ReadFloat(L"Stereo", L"FovScale", 1.1f), 0.5f, 2.f);
    g_cfg.keyStereo = ReadInt(L"Keys", L"ToggleStereo", VK_F12);
    g_cfg.verticalFill = Clamp(ReadFloat(L"Stereo", L"VerticalFill", 1.f), 0.f, 1.f);
    g_cfg.uiSplit = ReadInt(L"Stereo", L"SplitUI", 1);
    g_cfg.anamorphic = ReadFloat(L"Stereo", L"Anamorphic", 1.f) >= 0.5f;  // 0.8 etc. counts as on
    g_cfg.cameraDolly = Clamp(ReadFloat(L"Stereo", L"CameraDolly", 0.f), -5.f, 5.f);
    g_cfg.extraResolutions = ReadInt(L"Render", L"ExtraResolutions", 1);
    g_cfg.ambientOcclusion = ReadInt(L"Render", L"AmbientOcclusion", 1);
    g_cfg.shadows = ReadInt(L"Render", L"Shadows", 1);
    g_cfg.vignette = ReadInt(L"Render", L"Vignette", 0);
    g_cfg.lowHealthFx = ReadInt(L"Render", L"LowHealthEffect", 0);
    g_cfg.viewFix = ReadInt(L"Render", L"FixLighting", 1);
    g_cfg.lodFix = ReadInt(L"Render", L"LodFix", 1);
    {
        wchar_t buf[32];
        GetPrivateProfileStringW(L"Render", L"FovEasingCaller", L"", buf, 32, g_iniPath);
        unsigned v = 0;
        for (int i = (buf[0] == '0' && (buf[1] == 'x' || buf[1] == 'X')) ? 2 : 0; buf[i]; i++) {
            wchar_t c = buf[i];
            int d = c >= '0' && c <= '9' ? c - '0' : (c >= 'a' && c <= 'f' ? c - 'a' + 10 : (c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1));
            if (d < 0) break;
            v = v * 16 + (unsigned)d;
        }
        // 0.3.23: the Steam build's value as the default (checked in game: a wrong one is dropped
        // and searched again); the 0.3.22 log searched again although 0.3.21 had saved it
        g_cfg.fovEasingCaller = v ? v : 0x7397de;
    }
    {
        float cf = ReadFloat(L"Render", L"CullingFov", 170.f);
        bool rewrite = false;
        if ((cf > 119.9f && cf < 120.1f) || (cf > 149.9f && cf < 150.1f)) cf = 170.f, rewrite = true;  // old defaults
        if (cf > 170.f) g_cfg.cullFovAsked = cf, rewrite = true;  // logged: more than a field of view can be
        g_cfg.cullFov = Clamp(cf, 0.f, 170.f);
        // 0.3.22: the ini shows the value in use (0.3.21 kept 210 in the file and used 170)
        if (rewrite) WritePrivateProfileStringW(L"Render", L"CullingFov", L"170", g_iniPath);
    }
    g_cfg.headCulling = ReadInt(L"Render", L"HeadCulling", 1);
    g_cfg.wideProjection = ReadInt(L"Render", L"WideProjection", 0);
    g_cfg.eyeSync = 0;  // removed in 0.5.1 (late warp re-aims both eyes)
    g_cfg.lateWarp = ReadInt(L"Stereo", L"LateWarp", 1);
    g_cfg.bothEyes = ReadInt(L"Stereo", L"BothEyes", 0);
    g_cfg.reuseShadows = ReadInt(L"Render", L"ReuseShadows", 1);
    // 0.4.2: experimental, always starts off (user: smears; "reset to off upon every start")
    if (ReadInt(L"Stereo", L"DepthReprojection", 0)) WritePrivateProfileStringW(L"Stereo", L"DepthReprojection", L"0", g_iniPath);
    g_cfg.depthReproject = 0;
    g_cfg.reprojectEdges = ReadInt(L"Stereo", L"ReprojectionEdges", 1);
    g_cfg.vrControllers = ReadInt(L"Input", L"VRControllers", 1);
    g_cfg.vrRumble = ReadInt(L"Input", L"ControllerRumble", 1);  // 0.3.25 user test: worse with 1
    g_cfg.posePrediction = ReadInt(L"Stereo", L"PosePrediction", 1);
    g_cfg.maxFrameLatency = ReadInt(L"Render", L"MaxFrameLatency", 1);
    if (g_cfg.maxFrameLatency < 0) g_cfg.maxFrameLatency = 0;
    if (g_cfg.maxFrameLatency > 3) g_cfg.maxFrameLatency = 3;
    g_cfg.giVolumes = ReadInt(L"Render", L"GIVolumes", 128);
    if (g_cfg.giVolumes < 8) g_cfg.giVolumes = 8;
    if (g_cfg.giVolumes > 128) g_cfg.giVolumes = 128;
    g_cfg.headCullMargin = Clamp(ReadFloat(L"Render", L"HeadCullingMargin", 20.f), 0.f, 60.f);
    {
        wchar_t buf[200];
        GetPrivateProfileStringW(L"Render", L"FovRealCallers", L"0x729b49", buf, 200, g_iniPath);
        g_cfg.fovRealCount = 0;
        for (int i = 0; buf[i] && g_cfg.fovRealCount < 8;) {
            while (buf[i] == ' ' || buf[i] == ',' || buf[i] == ';') i++;
            if (buf[i] == '0' && (buf[i + 1] == 'x' || buf[i + 1] == 'X')) i += 2;
            unsigned v = 0;
            int digits = 0;
            for (;; i++, digits++) {
                wchar_t c = buf[i];
                int d = c >= '0' && c <= '9' ? c - '0' : (c >= 'a' && c <= 'f' ? c - 'a' + 10 : (c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1));
                if (d < 0) break;
                v = v * 16 + (unsigned)d;
            }
            if (digits && v) g_cfg.fovRealCallers[g_cfg.fovRealCount++] = v;
            if (!digits && buf[i]) i++;  // skip anything else
        }
    }
    g_cfg.distortionFx = ReadInt(L"Render", L"DistortionEffects", 0);
    g_cfg.flatBelowDraws = ReadInt(L"Stereo", L"FlatBelowDraws", 250);
    if (g_cfg.flatBelowDraws == 150) g_cfg.flatBelowDraws = 250;  // the old default missed the New Game screen
}

static void WriteFloat(const wchar_t* sec, const wchar_t* key, float v) {
    char a[32];
    pure::format(a, sizeof(a), "%.3f", (double)v);
    wchar_t w[32];
    int i = 0;
    for (; a[i] && i < 31; i++) w[i] = (wchar_t)a[i];
    w[i] = 0;
    WritePrivateProfileStringW(sec, key, w, g_iniPath);
}

void ConfigWrite(const char* section, const char* key, const char* value) {
    wchar_t s[64], k[64], v[64];
    int i;
    for (i = 0; section[i] && i < 63; i++) s[i] = (wchar_t)section[i];
    s[i] = 0;
    for (i = 0; key[i] && i < 63; i++) k[i] = (wchar_t)key[i];
    k[i] = 0;
    for (i = 0; value[i] && i < 63; i++) v[i] = (wchar_t)value[i];
    v[i] = 0;
    WritePrivateProfileStringW(s, k, v, g_iniPath);
}

void ConfigSaveScreen() {
    WriteFloat(L"Screen", L"Distance", g_cfg.distance);
    WriteFloat(L"Screen", L"Width", g_cfg.width);
}

// The camera-easing fov reader found in game (offset in the exe), so the next start skips the search.
void ConfigSaveEasingCaller(unsigned offset) {
    char a[24];
    pure::format(a, sizeof(a), "0x%x", offset);
    wchar_t w[24];
    int i = 0;
    for (; a[i] && i < 23; i++) w[i] = (wchar_t)a[i];
    w[i] = 0;
    WritePrivateProfileStringW(L"Render", L"FovEasingCaller", w, g_iniPath);
}

void ConfigSaveStereo() {
    WriteFloat(L"Stereo", L"WorldScale", g_cfg.worldScale);
    WriteFloat(L"Stereo", L"CameraDolly", g_cfg.cameraDolly);
    WritePrivateProfileStringW(L"Render", L"AmbientOcclusion", g_cfg.ambientOcclusion ? L"1" : L"0", g_iniPath);
    WritePrivateProfileStringW(L"Render", L"Shadows", g_cfg.shadows ? L"1" : L"0", g_iniPath);
    WritePrivateProfileStringW(L"Stereo", L"TopDownFlat", g_cfg.topDownFlat ? L"1" : L"0", g_iniPath);
    WritePrivateProfileStringW(L"Stereo", L"BothEyes", g_cfg.bothEyes ? L"1" : L"0", g_iniPath);
    WritePrivateProfileStringW(L"Render", L"HeadCulling", g_cfg.headCulling ? L"1" : L"0", g_iniPath);
}
