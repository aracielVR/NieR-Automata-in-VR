// stereo.cpp - alternate-eye stereo on the game side.
//
// NieR uploads its camera once per frame in a 256-byte cbuffer "SceneBuffer" (b0):
//   +0 g_View, +64 g_Proj, +128 g_ViewProjection (= Proj*View), +192 g_ViewInverseMatrix
// (column-vector matrices stored row by row; right-handed, +Y up, camera looks down -Z), and a
// 32-byte "CamParam_HPixel_Buffer" (b13): g_CameraParam = (near, far, tan(hfov/2), tan(vfov/2)),
// g_CameraVec = camera position. On alternate frames we rewrite both for the left or right eye,
// using the headset's eye pose (relative to the recentred reference) on top of the game camera.
// See DEVELOPMENT.md and tools/analyze_capture.py for how these were found.
#include "common.h"

StereoShared g_stereo;

namespace {

struct FrameState {
    bool active;     // this frame renders an eye
    int eye;         // 0 left, 1 right
    pure::M4 eyeRel; // eye pose relative to the reference (metres)
    float halfW, halfH;
    float localPos[3], localRot[4];  // eye pose in LOCAL space, for submission
    bool patchedScene, patchedCam;
    bool observe;  // stereo is held off only by TopDownFlat: watch the game camera's pitch, don't patch
    float ipd;  // distance between the two eye positions of the same head pose (metres)
    float refRot[4];  // the reference (recentre) orientation this frame was rendered with
    long long poseBase;  // g_stereo.poseBase of the head pose used
};
FrameState g_frame;
FrameState g_frame2;
// Depth reprojection (0.3.30): what each eye was last rendered with, to move its image to a later
// camera. Ce = eye-to-world, Pe = its projection, E = eye relative to the camera (scaled).
struct Shot {
    bool valid;
    unsigned frameNo;
    pure::M4 Ce, Pe, E;
    float localPos[3], localRot[4], halfW, halfH;
};
Shot g_shot[2];  // BothEyes: the right eye, same head pose, for the replay at Present
bool g_both;          // this frame: left eye live, right eye replayed
bool g_secondEye;     // the replay is running (g_frame = the right eye)
unsigned g_frameNo;
float g_bbAspect = 16.f / 9.f;
pure::M4 g_lastCam;      // camera used for the last eye (after levelling), camera-to-world
pure::M4 g_lastGameCam;  // the game's own camera, camera-to-world
// Head-direction culling (cull.cpp, logic thread): the eye's pose relative to the game camera,
// written by PatchScene, with the tick it was written at.
SRWLOCK g_relLock = SRWLOCK_INIT;
pure::M4 g_headRel;
DWORD g_headRelTick;
pure::M4 g_viewFix, g_viewFixInv;  // game view -> eye view, and back (this frame)
float g_lastDolly[3];    // world-space offset applied for CameraDolly last frame
bool g_haveCam;
bool g_steepLogged;
// TopDownFlat: the game camera's pitch, once per frame, with hysteresis (about 1 s each way).
unsigned g_pitchFrame;
int g_steepRun, g_levelRun;
bool g_topDownFlat;
float g_lastPitch;

void NotePitch(float pitch) {
    if (g_pitchFrame == g_frameNo) return;
    g_pitchFrame = g_frameNo;
    g_lastPitch = pitch;
    // TopDownFlat: enter at TopDownFlatFrom (50), leave only below TopDownFlatUntil (35). The flight
    // unit's views range from ~40 (near-isometric) to ~68 deg (0.3.15 log), and normal play rarely
    // stays below -50 for a second.
    float a = pitch < 0 ? -pitch : pitch;
    g_steepRun = a >= g_cfg.topDownFrom ? g_steepRun + 1 : 0;
    g_levelRun = a < g_cfg.topDownUntil ? g_levelRun + 1 : 0;
}
// Simple scenes (the title screen: ~90 draws per frame vs 1000+ in gameplay) are shown flat.
volatile LONG g_drawsThisFrame;
int g_lowRun, g_highRun;
bool g_simpleScene;
float g_loggedSep;
volatile LONG g_sepBudget;
unsigned g_patchCount;
bool g_loggedFirstPatch, g_loggedMismatch;

struct Pending {
    ID3D11Resource* res;
    float* data;
    int kind;  // 1 SceneBuffer, 2 CamParam
};
Pending g_pending[8];

bool Near(float a, float b, float rel) {
    float d = a - b;
    if (d < 0) d = -d;
    float m = b < 0 ? -b : b;
    return d <= rel * (m > 1e-6f ? m : 1e-6f);
}

pure::M4 Load(const float* p) {
    pure::M4 m;
    memcpy(m.m, p, 64);
    return m;
}

pure::M4 EyeMatrixScaled() {
    pure::M4 e = g_frame.eyeRel;
    e.at(0, 3) *= g_cfg.worldScale;
    e.at(1, 3) *= g_cfg.worldScale;
    e.at(2, 3) *= g_cfg.worldScale;
    return e;
}

bool IsMainScene(const float* d) {
    pure::ProjInfo pi;
    if (!pure::classify_projection(d + 16, &pi) || pi.rowMajor || pi.wSign > 0) return false;
    if (!Near(pi.aspect, g_bbAspect, 0.03f)) return false;  // e.g. the 1:1 menu/model cameras
    return pure::m4_is_rigid(Load(d), 1e-2f);
}

bool IsCamParam(const float* d) {
    // tangents up to 200: with CullingFov=170 the game uploads tan(hfov/2) ~ 20 (0.3.20 rejected it)
    return d[0] > 0.001f && d[0] < 10 && d[1] > 100 && d[2] > 0.05f && d[2] < 200 && d[3] > 0.05f && d[3] < 200 &&
           Near(d[2] / d[3], g_bbAspect, 0.03f);
}

void PatchScene(float* d) {
    pure::M4 V = Load(d), P = Load(d + 16);
    pure::M4 C = pure::m4_rigid_inverse(V);
    g_lastGameCam = C;
    pure::M4 C0 = C;
    if (g_cfg.levelCamera) {
        // Levelled while the game camera looks roughly horizontally; in steep (top-down) views its
        // pitch is kept, so the arena lies in front of you instead of at your feet.
        float pitch = 0, kept = 0;
        C0 = pure::m4_level_camera_adaptive(C, g_cfg.steepFrom, g_cfg.steepFull, &pitch, &kept);
        bool steep = kept > 0.5f;
        if (steep != g_steepLogged) {
            g_steepLogged = steep;
            Log("stereo: game camera pitch %.0f deg (game fov %.0f deg, camera at %.1f %.1f %.1f): %s", (double)pitch,
                (double)CullingGameFov(), (double)C.at(0, 3), (double)C.at(1, 3), (double)C.at(2, 3),
                steep ? "steep (top-down) view, keeping the camera's tilt" : "levelled again");
        }
    }
    if (!g_secondEye) NotePitch(pure::camera_pitch_deg(C));
    // CameraDolly: move along the game camera's own line of sight (towards the character it
    // follows) so nearby things get real depth without changing the world's scale.
    for (int k = 0; k < 3; k++) {
        g_lastDolly[k] = -C.at(k, 2) * g_cfg.cameraDolly;
        C0.at(k, 3) += g_lastDolly[k];
    }
    g_lastCam = C0;
    g_haveCam = true;
    pure::M4 Ce = pure::m4_mul(C0, EyeMatrixScaled());
    // Diagnostics: the eye separation applied (both eyes from the same head pose, in game units),
    // logged when it changes, i.e. after Ctrl+F9/F10.
    {
        float sep = g_frame.ipd * g_cfg.worldScale;
        if (sep > 0 && (g_loggedSep <= 0 || sep > g_loggedSep * 1.02f || sep < g_loggedSep / 1.02f) &&
            LogBudget(&g_sepBudget, 60)) {
            g_loggedSep = sep;
            Log("stereo: eye separation in the game world now %.2f cm (world scale %.3f)", (double)(sep * 100),
                (double)g_cfg.worldScale);
        }
    }
    {
        pure::M4 rel = pure::m4_relative(C, Ce);
        AcquireSRWLockExclusive(&g_relLock);
        g_headRel = rel;
        g_headRelTick = GetTickCount();
        ReleaseSRWLockExclusive(&g_relLock);
    }
    pure::M4 Ve = pure::m4_rigid_inverse(Ce);
    pure::M4 Pe = pure::m4_symmetric_projection(P, g_frame.halfW, g_frame.halfH);
    pure::M4 VPe = pure::m4_mul(Pe, Ve);
    CullingObserveGameFov(2 * pure::fatan(1 / P.at(1, 1)) * 57.29578f);
    g_viewFix = pure::m4_mul(Ve, C);     // game view space -> world -> eye view space
    g_viewFixInv = pure::m4_mul(V, Ce);  // and back
    memcpy(d, Ve.m, 64);
    memcpy(d + 16, Pe.m, 64);
    memcpy(d + 32, VPe.m, 64);
    memcpy(d + 48, Ce.m, 64);
    g_frame.patchedScene = true;
    if (g_frame.eye >= 0 && g_frame.eye < 2 && !g_secondEye) {
        Shot& s = g_shot[g_frame.eye];
        s.valid = true;
        s.frameNo = g_frameNo;
        s.Ce = Ce;
        s.Pe = Pe;
        s.E = EyeMatrixScaled();
        memcpy(s.localPos, g_frame.localPos, 12);
        memcpy(s.localRot, g_frame.localRot, 16);
        s.halfW = g_frame.halfW;
        s.halfH = g_frame.halfH;
    }
    g_patchCount++;
    if (!g_loggedFirstPatch) {
        g_loggedFirstPatch = true;
        Log("stereo: first camera patch. game camera at (%.2f, %.2f, %.2f), game vfov %.1f deg; eye frustum "
            "tan %.3f x %.3f (%.1f x %.1f deg%s), world scale %.2f, level camera %d",
            (double)C.at(0, 3), (double)C.at(1, 3), (double)C.at(2, 3),
            (double)(2 * pure::fatan(1 / P.at(1, 1)) * 57.29578f), (double)g_frame.halfW, (double)g_frame.halfH,
            (double)(2 * pure::fatan(g_frame.halfW) * 57.29578f), (double)(2 * pure::fatan(g_frame.halfH) * 57.29578f),
            g_cfg.anamorphic ? ", anamorphic" : "", (double)g_cfg.worldScale, g_cfg.levelCamera);
        // for depth-based reprojection: how the game maps view depth to the depth buffer
        Log("stereo: scene projection depth terms: P22 %.6f P23 %.6f P32 %.3f P33 %.3f (column-vector layout)",
            (double)P.at(2, 2), (double)P.at(2, 3), (double)P.at(3, 2), (double)P.at(3, 3));
    }
}

void PatchCamParam(float* d) {
    d[2] = g_frame.halfW;
    d[3] = g_frame.halfH;
    if (g_haveCam) {
        // Uploaded before this frame's SceneBuffer: use last frame's orientation with this
        // frame's position (the difference is far below a pixel).
        pure::M4 E = EyeMatrixScaled();
        float t[3] = {E.at(0, 3), E.at(1, 3), E.at(2, 3)};
        for (int k = 0; k < 3; k++)
            d[4 + k] += g_lastCam.at(k, 0) * t[0] + g_lastCam.at(k, 1) * t[1] + g_lastCam.at(k, 2) * t[2] +
                        g_lastDolly[k];
    }
    g_frame.patchedCam = true;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Game thread API
// ---------------------------------------------------------------------------------------------
void StereoSetAspect(float aspect) {
    if (aspect > 0.1f && aspect < 10) g_bbAspect = aspect;
}

// Called right after the game's Present: decides what the next frame renders.
void StereoCountDraw() { g_drawsThisFrame++; }

// The eye's pose and frustum for this frame (g_stereo.lock held, shared).
static void FillEye(FrameState& f, int which) {
    f.eye = which;
    pure::M4 ref = pure::m4_from_pose({g_stereo.refRot[0], g_stereo.refRot[1], g_stereo.refRot[2], g_stereo.refRot[3]},
                                      {g_stereo.refPos[0], g_stereo.refPos[1], g_stereo.refPos[2]});
    const float* er = g_stereo.eyeRot[f.eye];
    const float* ep = g_stereo.eyePos[f.eye];
    pure::M4 eye = pure::m4_from_pose({er[0], er[1], er[2], er[3]}, {ep[0], ep[1], ep[2]});
    f.eyeRel = pure::m4_relative(ref, eye);
    memcpy(f.refRot, g_stereo.refRot, 16);
    {
        const float* p0 = g_stereo.eyePos[0];
        const float* p1 = g_stereo.eyePos[1];
        float dx = p0[0] - p1[0], dy = p0[1] - p1[1], dz = p0[2] - p1[2];
        f.ipd = pure::fsqrt(dx * dx + dy * dy + dz * dz);
    }
    memcpy(f.localPos, ep, 12);
    memcpy(f.localRot, er, 16);
    f.poseBase = g_stereo.poseBase;
    const float* t = g_stereo.eyeTan[f.eye];  // tan of left, right, up, down
    if (g_cfg.anamorphic) {
        // Fill the eye's own field of view in both directions: the 16:9 image is stretched to
        // the eye's shape (non-square pixels) instead of wasting width. CamParam stores the
        // horizontal and vertical tangents separately, so depth-based effects stay correct.
        f.halfW = -t[0] > t[1] ? -t[0] : t[1];
        f.halfH = t[2] > -t[3] ? t[2] : -t[3];
        if (g_cfg.verticalFill < 1) {  // optional: crop the vertical angle for more detail
            float vf = g_cfg.verticalFill < 0.5f ? 0.5f : g_cfg.verticalFill;
            f.halfH = pure::ftan(pure::fatan(f.halfH) * vf);
        }
    } else {
        pure::cover_fov(t[0], t[1], t[2], t[3], g_bbAspect, &f.halfW, &f.halfH);
        // VerticalFill < 1 trades vertical field of view for sharpness: at 0 the frustum just
        // covers the eye's width, cropping the top and bottom of the view.
        float wide = -t[0] > t[1] ? -t[0] : t[1];
        if (f.halfW > wide && g_cfg.verticalFill < 1) {
            float hw = wide + (f.halfW - wide) * (g_cfg.verticalFill < 0 ? 0 : g_cfg.verticalFill);
            f.halfH *= hw / f.halfW;
            f.halfW = hw;
        }
    }
    f.halfW *= g_cfg.fovScale;
    f.halfH *= g_cfg.fovScale;
}

void StereoBeginFrame() {
    g_frameNo++;
    {
        LONG draws = g_drawsThisFrame;
        g_drawsThisFrame = 0;
        bool low = g_cfg.flatBelowDraws > 0 && draws < g_cfg.flatBelowDraws;
        g_lowRun = low ? g_lowRun + 1 : 0;
        g_highRun = low ? 0 : g_highRun + 1;
        if (!g_simpleScene && g_lowRun >= 20) {
            g_simpleScene = true;
            Log("stereo: simple scene (%d draws per frame, e.g. the title screen): shown flat", (int)draws);
        } else if (g_simpleScene && g_highRun >= 20) {
            g_simpleScene = false;
            Log("stereo: full scene again (%d draws per frame): stereo", (int)draws);
        }
    }
    FrameState f = {};
    AcquireSRWLockShared(&g_stereo.lock);
    bool on = g_stereo.enabled && g_stereo.valid && g_stereo.refValid && !g_stereo.menuMode && !g_simpleScene;
    // TopDownFlat: steep top-down views (e.g. the flight unit) go to the flat screen about 1 s after
    // the camera tilts down TopDownFlatFrom or more, and back about 2 s after it is below
    // TopDownFlatUntil.
    bool wasTop = g_topDownFlat;
    if (!g_cfg.topDownFlat) g_topDownFlat = false;
    else if (!g_topDownFlat && g_steepRun >= 60) g_topDownFlat = true;
    else if (g_topDownFlat && g_levelRun >= 120) g_topDownFlat = false;
    if (g_topDownFlat != wasTop)
        Log("stereo: %s (camera pitch %.0f deg)", g_topDownFlat ? "top-down view: flat screen (TopDownFlat=1)" : "top-down view ended: stereo",
            (double)g_lastPitch);
    f.observe = on && g_topDownFlat;
    on = on && !g_topDownFlat;
    if (on) {
        bool both = g_cfg.bothEyes && !g_stereo.singleEye && ReplayAvailable();
        FillEye(f, both ? 0 : (g_stereo.singleEye ? 0 : (int)(g_frameNo & 1)));
        g_both = both;
        if (both) {
            g_frame2 = f;
            FillEye(g_frame2, 1);
        }
    }
    ReleaseSRWLockShared(&g_stereo.lock);
    f.active = on;
    g_frame = f;
    if (!on) g_both = false;
    g_frame2.active = on;
    g_frame2.observe = false;
    g_secondEye = false;
    CullingUpdate(on);
    for (auto& p : g_pending) p.res = nullptr;
}

// Describes the frame that is being presented now.
void StereoGetFrameTag(FrameTag* tag) {
    tag->eye = g_frame.active ? g_frame.eye : -1;
    tag->hasScene = false;
    tag->patched = g_frame.active && g_frame.patchedScene;
    memcpy(tag->pos, g_frame.localPos, 12);
    memcpy(tag->rot, g_frame.localRot, 16);
    tag->tanHalfW = g_frame.halfW;
    tag->tanHalfH = g_frame.halfH;
    tag->poseBase = g_frame.active ? g_frame.poseBase : 0;
    tag->hasWorld = false;
    if (g_frame.active && g_frame.patchedScene && g_haveCam) {
        // eye world orientation = C0 * eyeRel = C0 * ref^-1 * eye: LOCAL -> world is C0 * ref^-1
        const float* r = g_frame.refRot;
        pure::M4 ref = pure::m4_from_pose({r[0], r[1], r[2], r[3]}, {0, 0, 0});
        pure::M4 c = g_lastCam;
        c.at(0, 3) = c.at(1, 3) = c.at(2, 3) = 0;
        pure::Q q = pure::q_from_m4(pure::m4_mul(c, pure::m4_rigid_inverse(ref)));
        tag->worldRot[0] = q.x;
        tag->worldRot[1] = q.y;
        tag->worldRot[2] = q.z;
        tag->worldRot[3] = q.w;
        tag->hasWorld = true;
    }
    if (g_frame.active && !g_frame.patchedScene && !g_loggedMismatch && g_frameNo > 600) {
        g_loggedMismatch = true;
        Log("stereo: a frame finished without the main camera being found (menu/loading screen, or the game "
            "changed how it uploads the camera)");
    }
}

bool StereoActive() { return g_frame.active; }
bool StereoBothEyesFrame() { return g_both && g_frame.active; }
void StereoBeginSecondEye() {
    FrameState f = g_frame2;
    f.patchedScene = f.patchedCam = false;
    g_frame = f;
    g_secondEye = true;
    for (auto& p : g_pending) p.res = nullptr;
}
// Depth reprojection: the other eye's image from the previous frame, moved to this frame's camera.
// T = that eye's view as it would be now (same head pose relative to the camera) <- its view then;
// proj = its projection's x/y scale and depth terms. Returns false when there is no such image.
bool StereoReprojectParams(int eye, float T[16], float proj[4], FrameTag* tag) {
    if (eye < 0 || eye > 1 || !g_frame.active || !g_frame.patchedScene || g_frame.eye == eye || !g_haveCam) return false;
    const Shot& s = g_shot[eye];
    if (!s.valid || s.frameNo + 1 != g_frameNo) return false;
    pure::M4 now = pure::m4_mul(g_lastCam, s.E);
    pure::M4 t = pure::m4_mul(pure::m4_rigid_inverse(now), s.Ce);
    memcpy(T, t.m, 64);
    proj[0] = s.Pe.at(0, 0);
    proj[1] = s.Pe.at(1, 1);
    proj[2] = s.Pe.at(2, 2);
    proj[3] = s.Pe.at(2, 3);
    *tag = {};
    tag->eye = eye;
    tag->patched = true;
    tag->hasScene = true;
    tag->noUI = true;
    memcpy(tag->pos, s.localPos, 12);
    memcpy(tag->rot, s.localRot, 16);
    tag->tanHalfW = s.halfW;
    tag->tanHalfH = s.halfH;
    return true;
}
int StereoFrameEye() { return g_frame.active ? g_frame.eye : -1; }

bool StereoSecondEyePatched() { return g_secondEye && g_frame.patchedScene; }
// The game's own camera (camera-to-world, before levelling): position, forward, up.
bool StereoGameCamera(float pos[3], float fwd[3], float up[3]) {
    if (!g_haveCam) return false;
    for (int k = 0; k < 3; k++) {
        pos[k] = g_lastGameCam.at(k, 3);
        fwd[k] = -g_lastGameCam.at(k, 2);
        up[k] = g_lastGameCam.at(k, 1);
    }
    return true;
}
// The eye's orientation relative to the game camera, if an eye was rendered in the last 250 ms.
bool StereoHeadRelative(pure::M4* rel) {
    AcquireSRWLockShared(&g_relLock);
    bool ok = g_headRelTick && GetTickCount() - g_headRelTick < 250;
    if (ok) *rel = g_headRel;
    ReleaseSRWLockShared(&g_relLock);
    return ok && g_frame.active;
}
// Head culling (0.3.24): vertical fov (radians) of a culling view, centred on the eye's direction,
// that holds the eye's whole view plus marginDeg on every side (game frames lag the head a little).
// 0 if no eye has been rendered yet.
float StereoHeadCullFov(float marginDeg) {
    float w = g_frame.halfW, h = g_frame.halfH;
    if (!(w > 0.05f && h > 0.05f && g_bbAspect > 0.1f)) return 0;
    float v = h > w / g_bbAspect ? h : w / g_bbAspect;  // the frustum has the game's aspect
    float half = pure::fatan(v) + marginDeg * 0.0174533f;
    return 2 * half;
}
// The camera buffers need looking at: an eye is rendered, or TopDownFlat waits for the view to level.
bool StereoWatching() { return g_frame.active || g_frame.observe; }

bool StereoGetViewFix(float T[16], float Tinv[16]) {
    if (!g_frame.active || !g_frame.patchedScene) return false;
    memcpy(T, g_viewFix.m, 64);
    memcpy(Tinv, g_viewFixInv.m, 64);
    return true;
}

// From the D3D11 hooks (game's immediate context only).
// TopDownFlat: the frame renders mono, but the game camera is still watched to know when the
// steep view ends.
void ObserveScene(const float* d) {
    if (IsMainScene(d)) NotePitch(pure::camera_pitch_deg(pure::m4_rigid_inverse(Load(d))));
}

void StereoOnMap(ID3D11Resource* res, void* data, UINT size) {
    if (!(g_frame.active || g_frame.observe) || !data || (size != 256 && size != 32)) return;
    for (auto& p : g_pending)
        if (!p.res) {
            p = {res, (float*)data, size == 256 ? 1 : 2};
            return;
        }
}

void StereoBeforeUnmap(ID3D11Resource* res) {
    if (!(g_frame.active || g_frame.observe)) return;
    for (auto& p : g_pending) {
        if (p.res != res) continue;
        // The mapped memory may be write-combined: read it once into a local copy.
        float local[64];
        UINT n = p.kind == 1 ? 64 : 8;
        memcpy(local, p.data, n * 4);
        if (!g_frame.active) {
            if (p.kind == 1) ObserveScene(local);
        } else if (p.kind == 1 && IsMainScene(local)) {
            PatchScene(local);
            memcpy(p.data, local, n * 4);
        } else if (p.kind == 2 && IsCamParam(local)) {
            PatchCamParam(local);
            memcpy(p.data, local, n * 4);
        }
        p.res = nullptr;
        return;
    }
}

// UpdateSubresource path: returns the data to pass on (possibly a patched copy in `scratch`).
const void* StereoPatchUpdate(UINT size, const void* data, float* scratch /* 64 floats */) {
    if (g_frame.observe && data && size == 256) ObserveScene((const float*)data);
    if (!g_frame.active || !data || (size != 256 && size != 32)) return data;
    memcpy(scratch, data, size);
    if (size == 256 && IsMainScene(scratch)) {
        PatchScene(scratch);
        return scratch;
    }
    if (size == 32 && IsCamParam(scratch)) {
        PatchCamParam(scratch);
        return scratch;
    }
    return data;
}

// ---------------------------------------------------------------------------------------------
// Camera memory scan (runs with the probe capture key). Finds where the game keeps its camera
// position on the CPU side and reports FOV-looking values nearby: the next step is widening the
// game's own (culling) field of view so objects don't vanish at the edges of the headset view.
// ---------------------------------------------------------------------------------------------
void StereoMemoryScan() {
    if (!g_haveCam) {
        Log("memscan: no camera seen yet");
        return;
    }
    float cx = g_lastGameCam.at(0, 3), cy = g_lastGameCam.at(1, 3), cz = g_lastGameCam.at(2, 3);
    Log("memscan: searching game memory for the camera position (%.3f, %.3f, %.3f)", (double)cx, (double)cy, (double)cz);
    const float fovValues[] = {0.872665f /*50 deg rad*/, 50.0f, 0.436332f /*25 deg rad*/, 0.466308f /*tan 25*/,
                               2.144507f /*1/tan 25*/, 1.206285f /*P00*/, 0.828991f /*tan hfov/2*/};
    static uint8_t chunk[1 << 20];
    int hits = 0;
    uint64_t scanned = 0;
    DWORD t0 = GetTickCount();
    MEMORY_BASIC_INFORMATION mbi;
    for (uint8_t* addr = nullptr; VirtualQuery(addr, &mbi, sizeof(mbi)) == sizeof(mbi);
         addr = (uint8_t*)mbi.BaseAddress + mbi.RegionSize) {
        if ((uintptr_t)addr >= 0x7FF000000000ull) break;
        bool rw = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
                  (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY));
        if (!rw || mbi.RegionSize > (1ull << 31)) continue;
        if (GetTickCount() - t0 > 15000) {
            Log("memscan: stopped after 15 s");
            break;
        }
        uint8_t* base = (uint8_t*)mbi.BaseAddress;
        if (base <= (uint8_t*)chunk && (uint8_t*)chunk < base + mbi.RegionSize) continue;  // our own buffer
        for (size_t off = 0; off < mbi.RegionSize; off += sizeof(chunk) - 8) {
            size_t len = mbi.RegionSize - off < sizeof(chunk) ? mbi.RegionSize - off : sizeof(chunk);
            SIZE_T got = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), base + off, chunk, len, &got) || got < 12) break;
            scanned += got;
            const float* f = (const float*)chunk;
            size_t n = got / 4;
            for (size_t i = 0; i + 2 < n; i++) {
                if (!(f[i] > cx - 0.01f && f[i] < cx + 0.01f)) continue;
                if (!(f[i + 1] > cy - 0.01f && f[i + 1] < cy + 0.01f) || !(f[i + 2] > cz - 0.01f && f[i + 2] < cz + 0.01f))
                    continue;
                uint8_t* at = base + off + i * 4;
                char where[300];
                DescribeAddress(at, where, sizeof(where));
                char nearBuf[600];
                int nl = 0;
                nearBuf[0] = 0;
                // look for FOV-like values within +-1 KB (or just after, at the start of a region)
                float around[512];
                SIZE_T g2 = 0;
                int startOff = -1024;
                if (!ReadProcessMemory(GetCurrentProcess(), at - 1024, around, sizeof(around), &g2)) {
                    startOff = 0;
                    if (!ReadProcessMemory(GetCurrentProcess(), at, around, sizeof(around) / 2, &g2)) g2 = 0;
                }
                for (size_t k = 0; k < g2 / 4 && nl < 500; k++)
                    for (float v : fovValues)
                        if (around[k] > v * 0.9999f && around[k] < v * 1.0001f)
                            nl += pure::format(nearBuf + nl, sizeof(nearBuf) - nl, " %d:%.5f",
                                               (int)(k * 4) + startOff, (double)around[k]);
                Log("memscan: camera position at %p (%s)%s%s", at, where, nl ? "; fov-like values at offsets" : "",
                    nearBuf);
                if (++hits >= 24) break;
            }
            if (hits >= 24) break;
        }
        if (hits >= 24) break;
    }
    Log("memscan: %d hits in %u MB (%u ms)", hits, (unsigned)(scanned >> 20), (unsigned)(GetTickCount() - t0));
}
