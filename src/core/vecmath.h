// vecmath.h - small vector, quaternion and 4x4 matrix math.
// No Windows dependencies; unit-tested on the build host with pure.h, and in the same namespace.
#pragma once

namespace pure {

// Math (OpenXR conventions: right-handed, +Y up, -Z forward)
struct V3 {
	float x, y, z;
};
struct Q {
	float x, y, z, w;
};

inline bool near0(float v, float eps = 1e-4f) { return v > -eps && v < eps; }
inline float fsqrt(float v) { return v <= 0 ? 0 : __builtin_sqrtf(v); }
inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(V3 v, float s) { return {v.x * s, v.y * s, v.z * s}; }
inline V3 operator/(V3 v, float s) { return {v.x / s, v.y / s, v.z / s}; }
inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float length(V3 v) { return fsqrt(dot(v, v)); }
inline Q conj(Q q) { return {-q.x, -q.y, -q.z, q.w}; }  // the inverse rotation (unit quaternions)
inline V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline V3 rotate(Q q, V3 v) {
	V3 u{q.x, q.y, q.z};
	V3 t = cross(u, v);
	t = {t.x + q.w * v.x, t.y + q.w * v.y, t.z + q.w * v.z};
	V3 c = cross(u, t);
	return {v.x + 2 * c.x, v.y + 2 * c.y, v.z + 2 * c.z};
}

// Yaw-only rotation (about +Y) whose forward (-Z) points along the horizontal part of `fwd`.
// Returns false if `fwd` is (nearly) vertical.
inline bool yaw_from_forward(V3 fwd, Q* out) {
	float h = fsqrt(fwd.x * fwd.x + fwd.z * fwd.z);
	if (h < 1e-4f) return false;
	float c = -fwd.z / h;  // cos(theta)
	float s = -fwd.x / h;  // sin(theta)
	float hw = fsqrt((1 + c) * 0.5f);
	float hy = fsqrt((1 - c) * 0.5f);
	if (s < 0) hy = -hy;
	*out = {0, hy, 0, hw};
	return true;
}

// 4x4 matrices in NieR's convention: column vectors (p' = M p), stored row by row
// (m[r*4+c]); translation in m[3], m[7], m[11]. Right-handed, +Y up, camera looks down -Z,
// which matches OpenXR's conventions.
struct M4 {
	float m[16];
	float& at(int r, int c) { return m[r * 4 + c]; }
	float at(int r, int c) const { return m[r * 4 + c]; }
};

inline M4 m4_identity() {
	M4 r = {};
	r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1;
	return r;
}

inline M4 m4_mul(const M4& a, const M4& b) {
	M4 r;
	for (int i = 0; i < 4; i++)
		for (int j = 0; j < 4; j++) {
			float s = 0;
			for (int k = 0; k < 4; k++) s += a.at(i, k) * b.at(k, j);
			r.at(i, j) = s;
		}
	return r;
}

// Rotation + translation from a pose (OpenXR quaternion x,y,z,w).
inline M4 m4_from_pose(Q q, V3 p) {
	float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
	float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
	float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
	M4 r = m4_identity();
	r.at(0, 0) = 1 - 2 * (yy + zz);
	r.at(0, 1) = 2 * (xy - wz);
	r.at(0, 2) = 2 * (xz + wy);
	r.at(1, 0) = 2 * (xy + wz);
	r.at(1, 1) = 1 - 2 * (xx + zz);
	r.at(1, 2) = 2 * (yz - wx);
	r.at(2, 0) = 2 * (xz - wy);
	r.at(2, 1) = 2 * (yz + wx);
	r.at(2, 2) = 1 - 2 * (xx + yy);
	r.at(0, 3) = p.x;
	r.at(1, 3) = p.y;
	r.at(2, 3) = p.z;
	return r;
}

// Unit quaternion of the rotation part of m (Shepperd's method; m need not be exactly orthonormal).
inline Q q_from_m4(const M4& m) {
	float t = m.at(0, 0) + m.at(1, 1) + m.at(2, 2);
	Q q;
	if (t > 0) {
		float s = fsqrt(t + 1) * 2;
		q = {(m.at(2, 1) - m.at(1, 2)) / s, (m.at(0, 2) - m.at(2, 0)) / s, (m.at(1, 0) - m.at(0, 1)) / s, s / 4};
	} else if (m.at(0, 0) > m.at(1, 1) && m.at(0, 0) > m.at(2, 2)) {
		float s = fsqrt(1 + m.at(0, 0) - m.at(1, 1) - m.at(2, 2)) * 2;
		q = {s / 4, (m.at(0, 1) + m.at(1, 0)) / s, (m.at(0, 2) + m.at(2, 0)) / s, (m.at(2, 1) - m.at(1, 2)) / s};
	} else if (m.at(1, 1) > m.at(2, 2)) {
		float s = fsqrt(1 + m.at(1, 1) - m.at(0, 0) - m.at(2, 2)) * 2;
		q = {(m.at(0, 1) + m.at(1, 0)) / s, s / 4, (m.at(1, 2) + m.at(2, 1)) / s, (m.at(0, 2) - m.at(2, 0)) / s};
	} else {
		float s = fsqrt(1 + m.at(2, 2) - m.at(0, 0) - m.at(1, 1)) * 2;
		q = {(m.at(0, 2) + m.at(2, 0)) / s, (m.at(1, 2) + m.at(2, 1)) / s, s / 4, (m.at(1, 0) - m.at(0, 1)) / s};
	}
	float n = fsqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
	if (n > 0) q = {q.x / n, q.y / n, q.z / n, q.w / n};
	return q;
}

// Inverse of a rotation+translation matrix.
inline M4 m4_rigid_inverse(const M4& a) {
	M4 r = m4_identity();
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++) r.at(i, j) = a.at(j, i);
	for (int i = 0; i < 3; i++) r.at(i, 3) = -(r.at(i, 0) * a.at(0, 3) + r.at(i, 1) * a.at(1, 3) + r.at(i, 2) * a.at(2, 3));
	return r;
}

inline bool m4_is_rigid(const M4& a, float eps = 2e-3f) {
	if (!near0(a.at(3, 0), eps) || !near0(a.at(3, 1), eps) || !near0(a.at(3, 2), eps) || !near0(a.at(3, 3) - 1, eps))
		return false;
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++) {
			float d = a.at(0, i) * a.at(0, j) + a.at(1, i) * a.at(1, j) + a.at(2, i) * a.at(2, j);
			if (!near0(d - (i == j ? 1.f : 0.f), eps)) return false;
		}
	return true;
}

// Camera-to-world with pitch and roll removed (yaw kept, position kept): a level horizon.
inline M4 m4_level_camera(const M4& cam) {
	V3 fwd{-cam.at(0, 2), -cam.at(1, 2), -cam.at(2, 2)};
	Q yaw;
	if (!yaw_from_forward(fwd, &yaw)) {
		// looking straight up/down: derive yaw from the camera's up vector instead
		V3 up{cam.at(0, 1), cam.at(1, 1), cam.at(2, 1)};
		V3 f2 = fwd.y < 0 ? up : V3{-up.x, -up.y, -up.z};
		if (!yaw_from_forward(f2, &yaw)) yaw = Q{0, 0, 0, 1};
	}
	return m4_from_pose(yaw, {cam.at(0, 3), cam.at(1, 3), cam.at(2, 3)});
}

// Symmetric perspective with the given half-extent tangents, keeping the game's depth mapping
// (row 2) and right-handed w = -z (row 3).
inline M4 m4_symmetric_projection(const M4& game, float tanHalfW, float tanHalfH) {
	M4 r = {};
	r.at(0, 0) = 1 / tanHalfW;
	r.at(1, 1) = 1 / tanHalfH;
	r.at(2, 2) = game.at(2, 2);
	r.at(2, 3) = game.at(2, 3);
	r.at(3, 2) = game.at(3, 2);
	return r;
}

// Off-centre ("asymmetric") version of the above: the frustum still reaches tanHalfW/tanHalfH to
// each side, but around (centreX, centreY) in tangent units instead of around straight ahead. That
// lets an eye's own lopsided field of view be rendered without the pixels a symmetric frustum
// wastes on its narrow sides. The centre goes into the same clip-space terms the upscaler's jitter
// shifts, so a point at tangent (centreX, centreY) lands in the middle of the image.
inline M4 m4_frustum(const M4& game, float tanHalfW, float tanHalfH, float centreX, float centreY) {
	M4 r = m4_symmetric_projection(game, tanHalfW, tanHalfH);
	r.at(0, 2) = -game.at(3, 2) * centreX / tanHalfW;  // game.at(3,2) = -1: w = -z
	r.at(1, 2) = -game.at(3, 2) * centreY / tanHalfH;
	return r;
}

// Rotation that turns a camera to look at (tanX, tanY) in its own frame: yaw about its up axis,
// then pitch. Straight ahead (0, 0, -1) ends up at that direction, so a symmetric frustum built
// around it covers a field of view the eye sees lopsidedly. Built from the tangents alone
// (cos = 1 / sqrt(1 + tan^2)), no trig.
inline M4 m4_turn_to(float tanX, float tanY) {
	float cosYaw = 1 / fsqrt(1 + tanX * tanX), sinYaw = tanX * cosYaw;
	// the yaw happens first, so the pitch needed to reach tanY is measured in the yawed frame
	float pitch = tanY * cosYaw;
	float cosPitch = 1 / fsqrt(1 + pitch * pitch), sinPitch = pitch * cosPitch;
	M4 r = m4_identity();
	r.at(0, 0) = cosYaw;
	r.at(0, 1) = -sinYaw * sinPitch;
	r.at(0, 2) = -sinYaw * cosPitch;
	r.at(1, 1) = cosPitch;
	r.at(1, 2) = -sinPitch;
	r.at(2, 0) = sinYaw;
	r.at(2, 1) = cosYaw * sinPitch;
	r.at(2, 2) = cosYaw * cosPitch;
	return r;
}

// Pose of `eye` relative to `ref` (both in the same space): inverse(ref) * eye.
inline M4 m4_relative(const M4& ref, const M4& eye) { return m4_mul(m4_rigid_inverse(ref), eye); }

// Smallest symmetric tangent box with aspect `aspect` (= width/height) that contains the
// (possibly asymmetric, canted-display) eye field of view given as tangents l<0<r, d<0<u.
inline void cover_fov(float l, float r, float u, float d, float aspect, float* halfW, float* halfH) {
	float hw = -l > r ? -l : r;
	float hh = u > -d ? u : -d;
	if (hw / hh < aspect)
		hw = hh * aspect;
	else
		hh = hw / aspect;
	*halfW = hw;
	*halfH = hh;
}

// tan for |x| < pi/2 via Taylor series of sin and cos (|x| <= 1.57: rel. error < 1e-6).
inline float ftan(float xf) {
	double x = xf, x2 = x * x;
	double s = x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72 * (1 - x2 / 110 * (1 - x2 / 156 * (1 - x2 / 210)))))));
	double c = 1 - x2 / 2 * (1 - x2 / 12 * (1 - x2 / 30 * (1 - x2 / 56 * (1 - x2 / 90 * (1 - x2 / 132 * (1 - x2 / 182 * (1 - x2 / 240)))))));
	return (float)(s / c);
}

// atan approximation (max error ~1e-5 rad), avoids needing a math library.
inline float fatan(float x) {
	bool neg = x < 0;
	if (neg) x = -x;
	bool inv = x > 1;
	if (inv) x = 1 / x;
	// minimax polynomial on [0,1]
	float x2 = x * x;
	float r = x * (0.99997726f + x2 * (-0.33262347f + x2 * (0.19354346f + x2 * (-0.11643287f + x2 * (0.05265332f + x2 * -0.01172120f)))));
	if (inv) r = 1.5707963f - r;
	return neg ? -r : r;
}

inline constexpr float kPi = 3.14159265f;
inline float wrap_angle(float radians) {  // to -pi..pi
	while (radians > kPi) radians -= 2 * kPi;
	while (radians < -kPi) radians += 2 * kPi;
	return radians;
}
// The yaw of a direction (radians about +y; 0 = +z, a character's forward in their bind pose; positive turns towards +x).
inline float yaw_of(V3 forward) {
	float x = forward.x, z = forward.z;
	if ((z < 0 ? -z : z) < 1e-6f) return x > 0 ? kPi / 2 : (x < 0 ? -kPi / 2 : 0);
	float yaw = fatan(x / z);
	return z > 0 ? yaw : wrap_angle(yaw + kPi);
}

// sin and cos for |x| <= pi/2 (Taylor series, rel. error < 1e-6).
inline void fsincos(float xf, float* sn, float* cs) {
	double x = xf, x2 = x * x;
	*sn = (float)(x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72 * (1 - x2 / 110 * (1 - x2 / 156)))))));
	*cs = (float)(1 - x2 / 2 * (1 - x2 / 12 * (1 - x2 / 30 * (1 - x2 / 56 * (1 - x2 / 90 * (1 - x2 / 132 * (1 - x2 / 182)))))));
}

inline Q q_mul(Q a, Q b) {
	return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
			a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

// Angle (degrees) of the rotation between two unit quaternions.
inline float q_angle_deg(Q a, Q b) {
	float w = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
	w = w < 0 ? -w : w;
	if (w >= 1) return 0;
	if (w < 1e-6f) return 180;
	return 2 * fatan(fsqrt(1 - w * w) / w) * 57.29578f;
}

// `now` turned on by what it turned since `prev`, times k (k = 1: the same turn again), the extra
// turn clamped to maxDeg; now's position is kept. Leads a head pose that is used later than it was taken.
inline M4 m4_rotation_ahead(const M4& prev, const M4& now, float k, float maxDeg) {
	Q q = q_from_m4(m4_mul(m4_rigid_inverse(prev), now));
	if (q.w < 0) q = {-q.x, -q.y, -q.z, -q.w};
	float s = fsqrt(q.x * q.x + q.y * q.y + q.z * q.z);
	if (s < 1e-7f || !(k > 0) || q.w < 1e-6f) return now;
	float angle = 2 * fatan(s / q.w) * k, maxAngle = maxDeg * 0.0174533f;
	if (angle > maxAngle) angle = maxAngle;
	float sn, cs;
	fsincos(angle * 0.5f, &sn, &cs);
	return m4_mul(now, m4_from_pose({q.x / s * sn, q.y / s * sn, q.z / s * sn, cs}, {0, 0, 0}));
}

// Pitch of a camera-to-world matrix's line of sight (-z), degrees: negative = looking down.
inline float camera_pitch_deg(const M4& cam) {
	float fx = -cam.at(0, 2), fy = -cam.at(1, 2), fz = -cam.at(2, 2);
	float h = fsqrt(fx * fx + fz * fz);
	float pitch = h > 1e-6f ? fatan(fy / h) : (fy > 0 ? 1.5707963f : -1.5707963f);
	return pitch * 57.29578f;
}

// Camera levelling that adapts to steep views. Pitch and roll are removed while the camera looks
// roughly horizontally or UP; looking down more than lo degrees, more and more of the pitch is kept,
// all of it from hi degrees (top-down sections: the arena then lies in front of you instead of at your
// feet). Upward views stay level: low behind the character the game camera tilts up 45-60 deg, and
// keeping part of that tilt made the view nod with every bob of the camera near the ground (JJ 2026-10-28). The heading comes from forward and up together, so it stays stable even when the camera
// looks straight down. *pitchDeg / *kept report the camera pitch and the fraction kept.
inline M4 m4_level_camera_adaptive(const M4& cam, float lo, float hi, float* pitchDeg, float* kept) {
	V3 fwd{-cam.at(0, 2), -cam.at(1, 2), -cam.at(2, 2)};
	V3 up{cam.at(0, 1), cam.at(1, 1), cam.at(2, 1)};
	float h = fsqrt(fwd.x * fwd.x + fwd.z * fwd.z);
	float pitch = h > 1e-6f ? fatan(fwd.y / h) : (fwd.y > 0 ? 1.5707963f : -1.5707963f);
	float sgn = pitch >= 0 ? 1.0f : -1.0f;
	V3 dir{fwd.x - sgn * up.x, fwd.y - sgn * up.y, fwd.z - sgn * up.z};
	Q yaw;
	if (!yaw_from_forward(dir, &yaw) && !yaw_from_forward(fwd, &yaw)) yaw = Q{0, 0, 0, 1};
	float deg = pitch * 57.29578f, a = deg < 0 ? -deg : 0;  // only looking down counts as steep
	float t = hi > lo ? (a - lo) / (hi - lo) : (a >= hi ? 1.0f : 0.0f);
	t = t < 0 ? 0 : (t > 1 ? 1 : t);
	float k = t * t * (3 - 2 * t);  // smoothstep
	float sn, cs;
	fsincos(pitch * k * 0.5f, &sn, &cs);
	Q q = q_mul(yaw, Q{sn, 0, 0, cs});
	if (pitchDeg) *pitchDeg = deg;
	if (kept) *kept = k;
	return m4_from_pose(q, {cam.at(0, 3), cam.at(1, 3), cam.at(2, 3)});
}

}  // namespace pure
