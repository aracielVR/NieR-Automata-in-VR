// waggle.h - motion controls: a slash or a stab of one hand (vr_controllers.cpp); after BetterVR's motion analyser, https://github.com/Crementif/BotW-BetterVR, with love.
#pragma once
#include <math.h>
#include <stdint.h>
#include "core/vecmath.h"

namespace pure {

enum SwingKind { SWING_NONE,
				 SWING_SLASH,
				 SWING_STAB };

inline const char* swing_kind_name(SwingKind kind) { return kind == SWING_SLASH ? "slash" : kind == SWING_STAB ? "stab"
																											   : "none"; }

// Thresholds, from BetterVR's two presets ([Input] SwingSensitivity).
struct SwingProfile {
	// stab
	float stabSpeed;           // m/s forward to keep a stab going
	float stabAccel;           // m/s^2 forward push (smoothed) that starts one
	float stabForwardSpeed;    // m/s forward speed that also starts one
	float stabCone;            // cos of the widest angle between the movement and the fist's -Z
	float stabMaxTurn;         // rad/s of turning (about X or Y) a stab may have
	float stabTravel;          // m forward before it fires (for the reference arm length)
	float stabAccelSmoothing;  // s, time constant of the smoothed push

	// slash
	float slashSpeed;       // rad/s of turning to keep a slash going
	float slashAccel;       // rad/s^2 (smoothed) that starts one
	float slashFastSpeed;   // rad/s that also starts one
	float slashMaxDrift;    // rad/s^2: how fast the turning axis may swing round before it ends
	float slashTravel;      // rad turned before it fires
	float driftMinSpeed;    // rad/s below which the axis is too noisy to measure drift

	// timing
	float minGoodStab, minGoodSlash;  // s of good samples that lock an attack
	int64_t goodGraceNs;              // a gap in the good samples this long restarts the streak
	float maxBad;                     // s of bad samples (minus good ones) that end an attack
	float smoothing;                  // s, time constant of the smoothed signals
	int64_t refractoryNs;             // after an attack: no new one from an acceleration spike

	float referenceArmLength;  // m: stabTravel is scaled by the measured arm length over this
	bool stabs;                // false: only slashes attack
};

// 0 = relaxed (easier), 1 = normal.
inline SwingProfile swing_profile(int sensitivity) {
	SwingProfile p = {};
	p.stabSpeed = 0.05f;
	p.stabAccel = 4.0f;
	p.stabForwardSpeed = 1.0f;
	p.stabCone = 0.5f;  // 60 degrees: a bent wrist still counts
	p.stabMaxTurn = 4.5f;
	p.stabTravel = 0.12f;
	p.stabAccelSmoothing = 0.050f;
	p.slashSpeed = 1.0f;
	p.slashAccel = 14.0f;
	p.slashFastSpeed = 5.0f;
	p.slashMaxDrift = 10.0f;
	p.slashTravel = 0.5235988f;  // 30 degrees
	p.driftMinSpeed = 0.5f;
	p.minGoodStab = p.minGoodSlash = 0.025f;
	p.goodGraceNs = 80000000;  // 80 ms
	p.maxBad = 0.033f;
	p.smoothing = 0.030f;
	p.refractoryNs = 250000000;  // 250 ms
	p.referenceArmLength = 0.55f;
	p.stabs = true;
	if (sensitivity >= 1) {
		p.stabCone = 0.7071068f;  // 45 degrees
		p.slashAccel = 20.0f;
		p.slashFastSpeed = 7.0f;
		p.slashSpeed = 1.5f;
		p.slashTravel = 0.6283185f;  // 36 degrees
		p.maxBad = 0.022f;
		p.goodGraceNs = 40000000;  // 40 ms
		p.minGoodSlash = 0.040f;
		p.minGoodStab = 0.030f;
		p.smoothing = 0.020f;
	}
	return p;
}

// One hand at one headset frame, all in the tracking space.
struct SwingSample {
	int64_t timeNs;
	bool tracked;  // position valid and tracked: other samples only keep the derivatives going
	V3 position;
	Q rotation;
	V3 linearVelocity;   // m/s
	V3 angularVelocity;  // rad/s
	V3 headPosition;
};

// What one sample changed, for the caller (fired = press the attack button) and its log.
struct SwingStep {
	bool locked, fired, ended;
	SwingKind kind;        // of the attack that locked, fired or ended
	bool hadFired;         // ended: it had fired (a finished attack) or not (rejected before travelling far enough)
	const char* lockedBy;  // locked: what set it off
	const char* endedBy;   // ended: what failed
	float value, limit;    // locked / ended: the measurement that decided it, and its threshold
	float power;           // fired / ended after firing: the attack's peak speed over a strong one, 0..1
	float travel;          // ended: how far it got (rad for a slash, m for a stab), and the travel needed
	float travelNeeded;
};

struct SwingAnalyser {
	SwingStep update(const SwingSample& sample, const SwingProfile& profile);
	float armLength() const { return m_armLength; }

private:
	// current sample, in the controller's frame
	V3 m_linear = {}, m_angular = {}, m_linearAccel = {};
	float m_flatAngularAccel = 0, m_drift = 0;
	// smoothed (exponential moving averages)
	V3 m_slowLinearAccel = {};
	float m_smoothFlatAngularAccel = 0, m_smoothDrift = 0, m_smoothFrameDt = 0;
	// last sample
	V3 m_prevLinear = {}, m_prevFlatAngular = {}, m_prevWorldAngular = {};
	int64_t m_prevTimeNs = 0;
	// the attack
	SwingKind m_locked = SWING_NONE;
	bool m_active = false;  // fired, still going
	float m_goodSlash = 0, m_goodStab = 0, m_bad = 0;
	int64_t m_lastGoodSlashNs = 0, m_lastGoodStabNs = 0, m_lockNs = 0, m_refractoryUntilNs = 0;
	V3 m_lockPosition = {}, m_lockForward = {};
	float m_peakSpeed = 0, m_travel = 0;
	float m_armLength = 0.55f;  // measured: the upper range of hand-to-head distances

	static constexpr float kStrongSlash = 10.0f;  // rad/s: power 1
	static constexpr float kStrongStab = 2.0f;    // m/s: power 1

	static float FlatLength(V3 v) { return fsqrt(v.x * v.x + v.y * v.y); }
	static float Blend(float dt, float timeConstant) { return dt > 0 ? 1.0f - expf(-dt / timeConstant) : 0.0f; }
	void Measure(const SwingSample& s, float dt, const SwingProfile& p);
	void Detect(const SwingSample& s, float dt, const SwingProfile& p, SwingStep& step);
	void CheckSteadiness(const SwingSample& s, float dt, const SwingProfile& p, SwingStep& step);
	void CheckTravel(const SwingSample& s, const SwingProfile& p, SwingStep& step);
	float CurrentSpeed() const { return m_locked == SWING_SLASH ? FlatLength(m_angular) : fabsf(m_linear.z); }
	float TravelNeeded(const SwingProfile& p) const {
		return m_locked == SWING_SLASH ? p.slashTravel : p.stabTravel * m_armLength / p.referenceArmLength;
	}
	float Power() const { return fminf(m_peakSpeed / (m_locked == SWING_SLASH ? kStrongSlash : kStrongStab), 1.0f); }
	void Lock(SwingKind kind, int64_t timeNs);
};

inline void SwingAnalyser::Measure(const SwingSample& s, float dt, const SwingProfile& p) {
	Q toHand = conj(s.rotation);
	m_linear = rotate(toHand, s.linearVelocity);
	m_angular = rotate(toHand, s.angularVelocity);
	m_linearAccel = dt > 0 ? (m_linear - m_prevLinear) / dt : V3{};
	V3 flatAngular = {m_angular.x, m_angular.y, 0};  // without the twist about the forearm
	m_flatAngularAccel = dt > 0 ? length(flatAngular - m_prevFlatAngular) / dt : 0;
	m_prevFlatAngular = flatAngular;
	// drift: how fast the turning axis itself swings round (a clean slash turns about a steady axis)
	float speedNow = length(s.angularVelocity), speedBefore = length(m_prevWorldAngular);
	m_drift = 0;
	if (dt > 0 && speedNow >= p.driftMinSpeed && speedBefore >= p.driftMinSpeed) {
		float cosAngle = dot(s.angularVelocity, m_prevWorldAngular) / (speedNow * speedBefore);
		m_drift = acosf(fmaxf(-1.0f, fminf(cosAngle, 1.0f))) / dt;
	}
	float a = Blend(dt, p.smoothing);
	m_smoothFlatAngularAccel += (m_flatAngularAccel - m_smoothFlatAngularAccel) * a;
	m_smoothDrift += (m_drift - m_smoothDrift) * a;
	m_slowLinearAccel = m_slowLinearAccel + (m_linearAccel - m_slowLinearAccel) * Blend(dt, p.stabAccelSmoothing);
	if (dt > 0) m_smoothFrameDt = m_smoothFrameDt == 0 ? dt : m_smoothFrameDt + (dt - m_smoothFrameDt) * Blend(dt, 0.5f);
}

inline void SwingAnalyser::Lock(SwingKind kind, int64_t timeNs) {
	m_locked = kind;
	m_lockNs = timeNs;
	m_peakSpeed = 0;
	m_travel = 0;
}

inline void SwingAnalyser::Detect(const SwingSample& s, float dt, const SwingProfile& p, SwingStep& step) {
	if (m_locked != SWING_NONE) return;
	bool refractory = s.timeNs < m_refractoryUntilNs;
	// stab: moving along the fist's -Z, not turning much, pushed forward (or already fast)
	bool stabGood = false;
	float speed = length(m_linear);
	if (p.stabs && speed > 1e-4f && !refractory) {
		bool inCone = fabsf(m_linear.z / speed) > p.stabCone;
		bool steady = fabsf(m_angular.x) < p.stabMaxTurn && fabsf(m_angular.y) < p.stabMaxTurn;
		bool pushed = -m_slowLinearAccel.z > p.stabAccel, fast = -m_linear.z > p.stabForwardSpeed;
		stabGood = inCone && steady && (pushed || fast);
		if (stabGood) {
			if (m_goodStab == 0) m_lockPosition = s.position;  // the whole thrust counts towards the travel
			m_goodStab += dt;
			m_lastGoodStabNs = s.timeNs;
			if (m_goodStab > p.minGoodStab) {
				Lock(SWING_STAB, s.timeNs);
				step.locked = true;
				step.kind = SWING_STAB;
				step.lockedBy = pushed ? "forward push" : "forward speed";
				step.value = pushed ? -m_slowLinearAccel.z : -m_linear.z;
				step.limit = pushed ? p.stabAccel : p.stabForwardSpeed;
			}
		} else if (s.timeNs - m_lastGoodStabNs > p.goodGraceNs) {
			m_goodStab = 0;
		}
	}
	// slash: a spike of angular acceleration, or fast turning (a stab-like sample is left to the stab)
	if (stabGood || m_locked == SWING_STAB) return;
	float turning = FlatLength(m_angular);
	bool byAccel = !refractory && m_smoothFlatAngularAccel > p.slashAccel, bySpeed = turning > p.slashFastSpeed;
	if (byAccel || bySpeed) {
		if (m_goodSlash == 0) m_lockForward = rotate(s.rotation, V3{0, 0, 1});
		m_goodSlash += dt;
		m_lastGoodSlashNs = s.timeNs;
		if (m_goodSlash > p.minGoodSlash) {
			Lock(SWING_SLASH, s.timeNs);
			step.locked = true;
			step.kind = SWING_SLASH;
			step.lockedBy = byAccel ? "angular acceleration" : "angular speed";
			step.value = byAccel ? m_smoothFlatAngularAccel : turning;
			step.limit = byAccel ? p.slashAccel : p.slashFastSpeed;
		}
	} else if (s.timeNs - m_lastGoodSlashNs > p.goodGraceNs) {
		m_goodSlash = 0;
	}
}

inline void SwingAnalyser::CheckSteadiness(const SwingSample& s, float dt, const SwingProfile& p, SwingStep& step) {
	if (m_locked == SWING_NONE) return;
	const char* failed = nullptr;
	float value = 0, limit = 0;
	auto fail = [&](const char* what, float measured, float threshold) {
		failed = what;
		value = measured;
		limit = threshold;
	};
	float turning = FlatLength(m_angular);
	if (m_locked == SWING_STAB) {
		float speed = length(m_linear);
		float inCone = speed > 1e-4f ? fabsf(m_linear.z / speed) : 0;
		if (inCone < p.stabCone) fail("direction", inCone, p.stabCone);
		else if (-m_linear.z < p.stabSpeed) fail("forward speed", -m_linear.z, p.stabSpeed);
		else if (turning > p.stabMaxTurn) fail("turning", turning, p.stabMaxTurn);
	} else {
		if (m_smoothDrift > p.slashMaxDrift) fail("drift", m_smoothDrift, p.slashMaxDrift);
		else if (turning < p.slashSpeed) fail("angular speed", turning, p.slashSpeed);
	}
	// right after the lock the samples that set it off can't end it again
	float lockGrace = fmaxf(0.033f, 2.5f * m_smoothFrameDt);
	if ((float)(s.timeNs - m_lockNs) / 1e9f < lockGrace) failed = nullptr;
	// bad samples add up, good ones heal
	m_bad = failed ? m_bad + dt : fmaxf(0.0f, m_bad - dt);
	if (m_bad < fmaxf(p.maxBad, 2.5f * m_smoothFrameDt)) return;
	SwingStep ended = {};
	ended.ended = true;
	ended.kind = m_locked;
	ended.hadFired = m_active;
	ended.endedBy = failed ? failed : "wobble";
	ended.value = value;
	ended.limit = limit;
	ended.power = m_active ? Power() : 0;
	ended.travel = m_travel;
	ended.travelNeeded = TravelNeeded(p);
	if (m_active) m_refractoryUntilNs = s.timeNs + p.refractoryNs;
	if (m_locked == SWING_SLASH) {
		m_goodSlash = 0;
		m_lastGoodSlashNs = 0;
	} else {
		m_goodStab = 0;
		m_lastGoodStabNs = 0;
	}
	m_locked = SWING_NONE;
	m_active = false;
	m_bad = 0;
	step = ended;  // can't share a sample with a lock: the lock grace above
}

inline void SwingAnalyser::CheckTravel(const SwingSample& s, const SwingProfile& p, SwingStep& step) {
	if (m_locked == SWING_NONE) return;
	float speed = CurrentSpeed();
	if (speed > m_peakSpeed) m_peakSpeed = speed;
	if (m_locked == SWING_STAB) {
		m_travel = length(s.position - m_lockPosition);
	} else {
		float cosAngle = dot(rotate(s.rotation, V3{0, 0, 1}), m_lockForward);
		m_travel = acosf(fmaxf(-1.0f, fminf(cosAngle, 1.0f)));
	}
	if (m_active || m_travel <= TravelNeeded(p)) return;
	m_active = true;
	step.fired = true;
	step.kind = m_locked;
	step.power = Power();
	step.travel = m_travel;
	step.travelNeeded = TravelNeeded(p);
}

inline SwingStep SwingAnalyser::update(const SwingSample& s, const SwingProfile& p) {
	SwingStep step = {};
	float dt = m_prevTimeNs > 0 ? (float)(s.timeNs - m_prevTimeNs) / 1e9f : 0;
	Measure(s, dt, p);
	m_prevLinear = m_linear;
	m_prevWorldAngular = s.angularVelocity;
	m_prevTimeNs = s.timeNs;
	if (!s.tracked) return step;  // positions of untracked samples would spoil the travel and the arm length
	float handToHead = length(s.position - s.headPosition);
	if (handToHead <= 1.2f) m_armLength += (handToHead - m_armLength) * (handToHead > m_armLength ? 0.05f : 0.002f);
	float accumDt = dt < 0.05f ? dt : 0.05f;  // a pause or a long frame can't fill the streaks at once
	Detect(s, accumDt, p, step);
	CheckSteadiness(s, accumDt, p, step);
	CheckTravel(s, p, step);
	return step;
}

}  // namespace pure
