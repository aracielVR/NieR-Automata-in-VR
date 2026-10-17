// Eye sync: the older eye's pose is turned by the game camera's turn since it was rendered.
#include <cstdio>
#include <cmath>
#include <initializer_list>
#include "pure.h"
static int fails;
static void check(bool c, const char* w) { if (!c) { std::printf("FAIL %s\n", w); fails++; } }
static pure::Q yawPitch(float yawDeg, float pitchDeg) {
    float y = yawDeg / 57.29578f, p = pitchDeg / 57.29578f;
    return pure::q_mul(pure::Q{0, std::sin(y / 2), 0, std::cos(y / 2)}, pure::Q{std::sin(p / 2), 0, 0, std::cos(p / 2)});
}
static bool qnear(pure::Q a, pure::Q b, float e = 1e-4f) {
    float d = std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
    return d > 1 - e;
}
int main() {
    // q_from_m4 round trip, including the w<0-ish branches (large angles)
    for (float y : {0.f, 30.f, 120.f, 179.f, -170.f})
        for (float p : {0.f, -40.f, 80.f}) {
            pure::Q q = yawPitch(y, p);
            check(qnear(pure::q_from_m4(pure::m4_from_pose(q, {1, 2, 3})), q), "q_from_m4 round trip");
        }
    pure::Q q180{1, 0, 0, 0};
    check(qnear(pure::q_from_m4(pure::m4_from_pose(q180, {0, 0, 0})), q180), "180 deg about x");
    // game camera turned 5 deg (yaw) between the old and the new frame; reference identity
    pure::Q ref{0, 0, 0, 1};
    for (float base : {0.f, 90.f, -135.f}) {
        pure::M4 cOld = pure::m4_from_pose(yawPitch(base, 0), {0, 0, 0}), cNew = pure::m4_from_pose(yawPitch(base + 5, 0), {5, 0, 1});
        pure::M4 R = pure::m4_from_pose(ref, {0, 0, 0});
        pure::Q qwOld = pure::q_from_m4(pure::m4_mul(cOld, pure::m4_rigid_inverse(R)));
        pure::Q qwNew = pure::q_from_m4(pure::m4_mul(cNew, pure::m4_rigid_inverse(R)));
        pure::Q eye = yawPitch(20, -10), out;
        check(pure::eye_sync_rotation(qwOld, qwNew, eye, 45, &out), "small turn applied");
        // expected: the tracking orientation that shows the old eye's world view under the new mapping
        pure::M4 ce = pure::m4_mul(cOld, pure::m4_from_pose(eye, {0, 0, 0}));
        pure::Q expect = pure::q_from_m4(pure::m4_mul(pure::m4_rigid_inverse(cNew), ce));
        check(qnear(out, expect), "matches the matrix form");
        check(qnear(out, pure::q_mul(yawPitch(-5, 0), eye)), "turned back by the camera's 5 deg");
    }
    // a non-identity reference (recentred) and a pitched camera
    {
        pure::Q refq = yawPitch(70, 0);
        pure::M4 R = pure::m4_from_pose(refq, {0, 0, 0});
        pure::M4 cOld = pure::m4_from_pose(yawPitch(10, -30), {0, 0, 0}), cNew = pure::m4_from_pose(yawPitch(14, -27), {0, 0, 0});
        pure::Q qwOld = pure::q_from_m4(pure::m4_mul(cOld, pure::m4_rigid_inverse(R)));
        pure::Q qwNew = pure::q_from_m4(pure::m4_mul(cNew, pure::m4_rigid_inverse(R)));
        pure::Q eye = yawPitch(75, 5), out;
        check(pure::eye_sync_rotation(qwOld, qwNew, eye, 45, &out), "recentred applied");
        // world orientation of the old eye: cOld * ref^-1 * eye; new mapping back: ref * cNew^-1 * that
        pure::M4 world = pure::m4_mul(pure::m4_mul(cOld, pure::m4_rigid_inverse(R)), pure::m4_from_pose(eye, {0, 0, 0}));
        pure::Q expect = pure::q_from_m4(pure::m4_mul(pure::m4_mul(R, pure::m4_rigid_inverse(cNew)), world));
        check(qnear(out, expect), "recentred matches");
    }
    // camera cut: refused
    {
        pure::Q out{0, 0, 0, 1};
        check(!pure::eye_sync_rotation(yawPitch(0, 0), yawPitch(90, 0), yawPitch(0, 0), 45, &out), "cut refused");
        check(pure::eye_sync_rotation(yawPitch(0, 0), yawPitch(0, 0), yawPitch(33, 0), 45, &out) && qnear(out, yawPitch(33, 0)),
              "no turn: unchanged");
    }
    check(std::fabs(pure::q_angle_deg(yawPitch(0, 0), yawPitch(7, 0)) - 7) < 0.01f, "angle 7");
    check(std::fabs(pure::q_angle_deg(yawPitch(10, 0), yawPitch(-30, 0)) - 40) < 0.01f, "angle 40");
    check(std::fabs(pure::q_angle_deg(pure::Q{0, 0, 0, 1}, pure::Q{0, 1, 0, 0}) - 180) < 0.01f, "angle 180");
    std::printf(fails ? "eye sync tests FAILED\n" : "eye sync tests passed\n");
    return fails != 0;
}
