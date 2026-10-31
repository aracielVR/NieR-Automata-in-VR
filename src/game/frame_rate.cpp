// frame_rate.cpp - the game's frame rate above its 60 cap, with the game kept at its real speed.
//
// Found in the game's own code (decrypted exe, NOTES "A8 RE - second pass", JJ's 2021 Steam build; addresses are RVAs):
// - The frame limiter, fn 0x24a150, runs once per frame on the game thread: it waits (Sleep, then a QPC spin) until the
//   frame period has passed, then runs the frame. The period is an integer in thirds of a millisecond at
//   [[0x177f1a0] + 0x308]: 50 = 60 fps, 100 = 30 fps. The game sets it itself whenever its frame-rate mode
//   [0x177f5d4] changes (0x24a7ff: mode 1 -> `mov eax, 0x64` at 0x24a81e, else `mov eax, 0x32` at 0x24a817).
// - The game advances a FIXED 1/60 s per frame: dt = clock step (frames) x 1/60 (0x48f8c0). The step comes from the
//   step getter 0x14b990 (574 callers): the object's own clock (+0x1c, rebuilt every frame in 0x14bfa0 as
//   1.0 x scale x speed x group factor, the 1.0 loaded by `movss xmm2, [0xdba840]` at 0x14bffe), else the global
//   group factor [[0x176dea0] + 0xa4], else 1.0.
// Above 60 the game would run that much faster. So with a higher rate set: the period is lowered, the clock base
// at 0x14bffe reads our own value instead of the shared 1.0, and the getter's two other paths are scaled by it. That
// value is the real frame time in 60ths of a second (at most 1, so below the cap the game slows down as it always did).
// - The game ALSO measures each frame (fn 0x14bdb0, called once per frame from 0x75eb4f on the global at 0x1029f20):
//   QPC time per frame in 60ths, stored at 0x1029fd0 and read in ~40 places (animation, movement, effects), but clamped
//   to 1..2: below 1 it is set back to 1 (`jae` at 0x14beb4 skips that only when the step is at least 1). Above 60 it
//   stayed 1 and the running animation ran too fast (JJ 2026-10-28 at 75 fps). With a higher rate set, that jump
//   always skips the minimum (`jmp`), so the game's own measured step goes below 1 (the 2-frame maximum stays).
//   The clocks' group factor ([[0x176dea0]+0xa4]) follows that measured step (log: 0.673 vs 0.673 at 89 fps), so with
//   it allowed below 1 the game's own variable timestep covers the clocks too: our clock scale then stays 1 (it would
//   count the frame time twice). Our clock scale only acts with GameStepFix=0.
// - Things the game does once per FRAME, not per unit of time (found by reading the game's code at the places WAX's
//   frame-rate code points to - JJ: "look at what it fixes, don't copy how"; our own fix), each scaled by the measured
//   step while a higher rate is set: a 0.14 blend per frame (0x4b3abd, rip constant -> ours: 1 - 0.86^step), an easing
//   x += (target - x) * rate with a per-object rate (0x4bc4dc, 0x4bc69b: detours multiply by the step - the linear form
//   of 1 - (1 - rate)^step), a sway phase stepped per frame (0x527693: the increment x step) and the 0.1 deg per frame
//   turn limit above it (0x527677 / 0x52767f: rip constants -> ours, x step). Around the hacking minigame (WAX's
//   messages name "hacking phase constants"): a phase + pi/100 per frame (0x6da744, 0x6da824; the shared constant is
//   read by six other functions too, so only these two loads are pointed at ours), a 3 deg turn per frame (0x6da969),
//   and a frame counter that fires at 15 (0x839402 `cmp eax, 15` -> 15 x frame rate / 60). The player moves and
//   turns toward targets by a fixed fraction per frame (blend factor in xmm2, then the lerp 0x265f80 - at 0x56019f,
//   0x56029c, 0x560452, 0x561291; factors up to 1 = snap): detours call BlendForStep (1 - (1 - t)^step, exact), with every
//   volatile register saved. Timed clock effects (slow motion) count their delay (+0x28) and duration (+0x24) in frames
//   (0x14c0d2, 0x14c0e0): lowered by the whole 60ths that really passed (0 or 1 a frame above 60, always 1 at 60).
// - The limiter skips its wait while the game's window is minimised (`test byte [0x177f6c4], 4; jne` at 0x24a1ae: bit 4
//   = the window procedure's WM_SIZE with SIZE_MINIMIZED, block +0x34, copied into the flags after the message pump at
//   0x26e444). An exclusive-fullscreen game minimises when it loses focus, and BackgroundInput keeps it running, so it
//   ran uncapped there - still a 60th of a second per frame, so too fast - and capped again once focused (Ethan, PR 1).
//   The jump is turned into a no-op: the chosen rate holds whether the window is focused, covered or minimised.
// Not covered yet (NOTES A8): three inline copies of the getter's fallback paths (0x19f81c, 0x1d5c60, 0x223afa), and
// anything counted in whole frames (slow-motion countdowns are, `dec [clock+0x24]`).
#include "game/frame_rate.h"
#include "core/config.h"
#include "core/hook.h"
#include "core/log.h"
#include <windows.h>
#include <tlhelp32.h>
#include <atomic>
#include <initializer_list>
#include <math.h>
#include <string.h>

namespace {
constexpr uintptr_t kLimiter = 0x24a150, kPeriod60Imm = 0x24a817, kPeriod30Imm = 0x24a81e, kClockBaseLoad = 0x14bffe,
					kOneConst = 0xdba840, kStepGetter = 0x14b990, kGamePtr = 0x177f1a0, kTimeScalePtr = 0x176dea0,
					kStepClampJump = 0x14beb4, kStepClampContext = 0x14beac, kMeasuredStep = 0x1029fd0;
constexpr uint32_t kPeriodOffset = 0x308, kGameUnitsPer60 = 50;  // thirds of a millisecond
// [Render] GameFrameRate: 0 = the game's own 60; -1 = the headset's rate, paced by us (PaceToHeadset); 250 = unlimited
// (as fast as the graphics card goes, kept below 250 so menus and loading screens don't spin at thousands)
constexpr int kRates[] = {0, 72, 80, 90, 120, -1, 250};
constexpr int32_t kOutOfTheWayUnits = 12;  // the game's own limiter at 250 fps: it never waits while we pace

// What the code must look like before anything is touched (JJ's build; anything else: left alone)
constexpr uint8_t kLimiterBytes[] = {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x48, 0x08, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57};
constexpr uint8_t kPeriod60Bytes[] = {0xb8, 0x32, 0x00, 0x00, 0x00};
constexpr uint8_t kPeriod30Bytes[] = {0xb8, 0x64, 0x00, 0x00, 0x00};
constexpr uintptr_t kMinimisedSkip = 0x24a1b5;
constexpr uint8_t kMinimisedSkipBytes[] = {0x0f, 0x85, 0x0b, 0x01, 0x00, 0x00};  // jne 0x24a2c6: past the Sleep and spin
constexpr uint8_t kNop6[] = {0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00};
constexpr uint8_t kClockBaseBytes[] = {0xf3, 0x0f, 0x10, 0x15};  // movss xmm2, [rip + disp32] -> the shared 1.0
constexpr uint8_t kStepGetterBytes[] = {0x48, 0x8b, 0x01, 0x48, 0x85, 0xc0, 0x75, 0x1e};
// movss [rbx+0xb0], xmm1; jae +0x0f; mov dword [rbx+0xb0], 1.0 (the measured step's minimum)
constexpr uint8_t kStepClampBytes[] = {0xf3, 0x0f, 0x11, 0x8b, 0xb0, 0x00, 0x00, 0x00, 0x73, 0x0f,
									   0xc7, 0x83, 0xb0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3f};

uint8_t* s_exe;
uint8_t* s_near;                  // a page near the exe (rip-relative reach): our values, then small code detours
float* s_clockBase;               // our value the clock base loads
// our values the per-frame sites read (in s_near after the clock base)
enum NearValue { NV_CLOCK_BASE,
				 NV_STEP,
				 NV_BLEND,
				 NV_TURN_MAX,
				 NV_TURN_MIN,
				 NV_HACK_PHASE,
				 NV_HACK_TURN,
				 NV_FRAME_SECONDS,  // a 60th of a second x the frame's real length (at most a 60th)
				 NV_DEG1,           // 1 deg x step
				 NV_HACK_PHASE2,    // 2 pi / 100 x step
				 NV_COUNT };
static_assert(NV_COUNT * sizeof(float) <= 56, "our values end before the frame ticks");
constexpr size_t kNearTicks = 56;  // int32 in s_near: whole 60ths passed this frame (for the frame countdowns)
constexpr size_t kNearCodeStart = 0x1000;  // detours on their own page: the values above change every frame
std::atomic<float> s_scale{1.0f};  // real frame time in 60ths of a second, at most 1; 1 = the game's own behaviour
bool s_basePatched;
bool s_clampOk;           // the measured step's minimum is where it was expected
uint8_t s_clampOpcode = 0x73;  // what the minimum's jump is now: 0x73 jae (the game's), 0xeb jmp (minimum skipped)
std::atomic<int64_t> s_headsetPeriodNs, s_headsetFrameQpc;  // from the VR thread (FrameRateNoteHeadsetFrame)
bool HeadsetMode();
int32_t s_appliedUnits;  // the period we last wrote (0 = the game's own)
bool s_minimisedSkipOk;  // the limiter's skip-while-minimised jump is where expected (written once, from the game thread)

typedef uint64_t(__fastcall* PFN_Limiter)(void*, void*, void*, void*);
typedef float(__fastcall* PFN_Step)(void**);
PFN_Limiter o_Limiter;
PFN_Step o_Step;

LARGE_INTEGER s_freq, s_lastEntry;
struct Stats {
	uint32_t frames;
	double seconds;
	float minScale = 9, maxScale, sumScale;
	float minStep = 9, maxStep, sumStep, sumGroup;  // the game's measured step (0x1029fd0), its group factor
	uint32_t groupSamples;
	DWORD since;
} s_stats;

bool Matches(uintptr_t rva, const uint8_t* bytes, size_t n) { return memcmp(s_exe + rva, bytes, n) == 0; }

void WriteCode(void* at, const void* bytes, size_t n) {
	DWORD old;
	if (!VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old)) return;
	memcpy(at, bytes, n);
	VirtualProtect(at, n, old, &old);
	FlushInstructionCache(GetCurrentProcess(), at, n);
}

// Two pages within rip-relative reach (2 GB) below the exe: our values (read/write, changed every frame), then the
// detours (executable). Code next to data written every frame makes the CPU flush its pipeline (review 2026-10-28).
uint8_t* AllocNearExe() {
	uintptr_t base = (uintptr_t)s_exe & ~(uintptr_t)0xffff;
	for (uintptr_t step = 1; step < 0x7000; step++) {
		uintptr_t at = base - step * 0x10000;
		void* p = VirtualAlloc((void*)at, 0x2000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
		if (!p) continue;
		DWORD old;
		VirtualProtect(p, 0x1000, PAGE_READWRITE, &old);
		return (uint8_t*)p;
	}
	return nullptr;
}

int32_t s_clockBaseGameDisp;  // the game's own displacement at the clock base load (to the shared 1.0)
bool PauseOthersClean();     // below: the other threads suspended, none inside a place about to be rewritten
void ResumeOthers();

// The clock base load reads our value only while our clock scale is used (GameSpeedFix without the step fix); at the
// default it is the game's own bytes (review 2026-10-28).
void SetClockBase(bool on) {
	if (on == s_basePatched || !s_clockBase) return;
	uint8_t* site = s_exe + kClockBaseLoad;
	int64_t disp = (int64_t)((uintptr_t)s_clockBase - (uintptr_t)(site + 8));
	if (on && (disp < INT32_MIN || disp > INT32_MAX)) return;
	int32_t d = on ? (int32_t)disp : s_clockBaseGameDisp;
	if (!PauseOthersClean()) return;  // tried again next frame
	WriteCode(site + 4, &d, 4);
	ResumeOthers();
	s_basePatched = on;
}

// --- per-frame sites ----------------------------------------------------------------------------------------------
// Each site: the game's bytes (checked at install), and ours, applied while the measured step may go below a 60th.
struct FrameSite {
	const char* what;
	uintptr_t rva;
	uint8_t len;
	uint8_t game[16];
	uint8_t ours[16];
};
FrameSite s_sites[256];
int s_siteCount;
bool s_sitesOk, s_sitesOn;

int32_t Rel32(const uint8_t* from, const void* to) { return (int32_t)((const uint8_t*)to - from); }
bool Reaches(const uint8_t* from, const void* to) {
	int64_t d = (const uint8_t*)to - from;
	return d >= INT32_MIN && d <= INT32_MAX;
}

// A rip-relative load whose 4-byte displacement (at `dispAt` in the instruction) is pointed at one of our values.
void AddRipSite(const char* what, uintptr_t rva, uint8_t len, uint8_t dispAt, NearValue value) {
	FrameSite& s = s_sites[s_siteCount++];
	s = {what, rva, len, {}, {}};
	memcpy(s.game, s_exe + rva, len);
	memcpy(s.ours, s.game, len);
	int32_t d = Rel32(s_exe + rva + len, (float*)s_near + value);
	memcpy(s.ours + dispAt, &d, 4);
}

// An 8-byte instruction replaced by a jump to: that instruction, then `mulss xmm<reg>, [step]`, then back.
uint8_t* s_codeEnd;
void AddScaleDetour(const char* what, uintptr_t rva, int reg) {
	FrameSite& s = s_sites[s_siteCount++];
	s = {what, rva, 8, {}, {}};
	memcpy(s.game, s_exe + rva, 8);
	uint8_t* code = s_codeEnd;
	memcpy(code, s.game, 8);  // the game's own instruction (no rip-relative operand: checked by the byte match)
	uint8_t mul[8] = {0xf3, 0x0f, 0x59, (uint8_t)(0x05 | reg << 3)};  // mulss xmm<reg>, [rip + disp32]
	int32_t d = Rel32(code + 16, (float*)s_near + NV_STEP);
	memcpy(mul + 4, &d, 4);
	memcpy(code + 8, mul, 8);
	code[16] = 0xe9;  // jmp back after the replaced instruction
	int32_t back = Rel32(code + 21, s_exe + rva + 8);
	memcpy(code + 17, &back, 4);
	s_codeEnd = code + 24;
	s.ours[0] = 0xe9;  // jmp to the detour, then a 3-byte nop
	int32_t to = Rel32(s_exe + rva + 5, code);
	memcpy(s.ours + 1, &to, 4);
	s.ours[5] = 0x0f, s.ours[6] = 0x1f, s.ours[7] = 0x00;
}

float BlendForStep(float t) {
	float step = ((float*)s_near)[NV_STEP];
	if (!(t > 0)) return t;
	if (t >= 1) return 1;
	return 1.0f - powf(1.0f - t, step);
}

void Emit(uint8_t*& p, std::initializer_list<uint8_t> bytes) {
	for (uint8_t b : bytes) *p++ = b;
}

// `total` bytes at rva (position-independent instructions) replaced by a jump to: the first `before` of them, then
// xmm2 = BlendForStep(xmm2) with every volatile register kept, then the rest of them, then back.
void AddBlendDetour(const char* what, uintptr_t rva, uint8_t before, uint8_t total) {
	FrameSite& s = s_sites[s_siteCount++];
	s = {what, rva, total, {}, {}};
	memcpy(s.game, s_exe + rva, total);
	uint8_t* p = s_codeEnd;
	uint8_t* start = p;
	memcpy(p, s.game, before);
	p += before;
	Emit(p, {0x50, 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53});  // push rax rcx rdx r8-r11
	Emit(p, {0x48, 0x81, 0xec, 0x88, 0x00, 0x00, 0x00});                          // sub rsp, 0x88 (16-aligned again)
	Emit(p, {0xf3, 0x0f, 0x7f, 0x44, 0x24, 0x20, 0xf3, 0x0f, 0x7f, 0x4c, 0x24, 0x30, 0xf3, 0x0f, 0x7f, 0x5c, 0x24, 0x40,
			 0xf3, 0x0f, 0x7f, 0x64, 0x24, 0x50, 0xf3, 0x0f, 0x7f, 0x6c, 0x24, 0x60});  // movdqu [rsp+..], xmm0 1 3 4 5
	Emit(p, {0x0f, 0x28, 0xc2, 0x48, 0xb8});                                         // movaps xmm0, xmm2; mov rax, imm64
	uint64_t fn = (uint64_t)(uintptr_t)&BlendForStep;
	memcpy(p, &fn, 8);
	p += 8;
	Emit(p, {0xff, 0xd0, 0x0f, 0x28, 0xd0});  // call rax; movaps xmm2, xmm0
	Emit(p, {0xf3, 0x0f, 0x6f, 0x44, 0x24, 0x20, 0xf3, 0x0f, 0x6f, 0x4c, 0x24, 0x30, 0xf3, 0x0f, 0x6f, 0x5c, 0x24, 0x40,
			 0xf3, 0x0f, 0x6f, 0x64, 0x24, 0x50, 0xf3, 0x0f, 0x6f, 0x6c, 0x24, 0x60});  // movdqu xmm0 1 3 4 5, [rsp+..]
	Emit(p, {0x48, 0x81, 0xc4, 0x88, 0x00, 0x00, 0x00});                          // add rsp, 0x88
	Emit(p, {0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58, 0x5a, 0x59, 0x58});  // pop r11-r8 rdx rcx rax
	memcpy(p, s.game + before, total - before);
	p += total - before;
	*p++ = 0xe9;
	int32_t back = Rel32(p + 4, s_exe + rva + total);
	memcpy(p, &back, 4);
	p += 4;
	s_codeEnd = (uint8_t*)(((uintptr_t)p + 15) & ~(uintptr_t)15);
	s.ours[0] = 0xe9;
	int32_t to = Rel32(s_exe + rva + 5, start);
	memcpy(s.ours + 1, &to, 4);
	for (int i = 5; i < total; i++) s.ours[i] = 0x90;
}

// The frame countdowns of timed clock effects: lowered by the whole 60ths passed (an int32 at s_near + kNearTicks).
void AddCountdownDetours() {
	// 0x14c0d2: dec eax; mov dword [rdx+0x14], 1.0 (9 bytes) -> sub eax, [ticks]; mov dword [rdx+0x14], 1.0; jmp back
	{
		FrameSite& s = s_sites[s_siteCount++];
		s = {"slow motion delay (frames)", 0x14c0d2, 9, {}, {}};
		memcpy(s.game, s_exe + 0x14c0d2, 9);
		uint8_t* p = s_codeEnd;
		uint8_t* start = p;
		Emit(p, {0x2b, 0x05});  // sub eax, [rip + disp32]
		int32_t d = Rel32(p + 4, s_near + kNearTicks);
		memcpy(p, &d, 4);
		p += 4;
		memcpy(p, s.game + 2, 7);  // mov dword [rdx+0x14], 1.0
		p += 7;
		*p++ = 0xe9;
		int32_t back = Rel32(p + 4, s_exe + 0x14c0db);
		memcpy(p, &back, 4);
		p += 4;
		s_codeEnd = (uint8_t*)(((uintptr_t)p + 15) & ~(uintptr_t)15);
		s.ours[0] = 0xe9;
		int32_t to = Rel32(s_exe + 0x14c0d2 + 5, start);
		memcpy(s.ours + 1, &to, 4);
		for (int i = 5; i < 9; i++) s.ours[i] = 0x90;
	}
	// 0x14c0e0: dec dword [rdx+0x24]; cmp [rdx+0x24], r10d (7 bytes) -> push rax; mov eax, [ticks];
	// sub [rdx+0x24], eax; pop rax; cmp [rdx+0x24], r10d; jmp back (the flags of the cmp go to the game's jg)
	{
		FrameSite& s = s_sites[s_siteCount++];
		s = {"slow motion duration (frames)", 0x14c0e0, 7, {}, {}};
		memcpy(s.game, s_exe + 0x14c0e0, 7);
		uint8_t* p = s_codeEnd;
		uint8_t* start = p;
		Emit(p, {0x50, 0x8b, 0x05});  // push rax; mov eax, [rip + disp32]
		int32_t d = Rel32(p + 4, s_near + kNearTicks);
		memcpy(p, &d, 4);
		p += 4;
		Emit(p, {0x29, 0x42, 0x24, 0x58});  // sub [rdx+0x24], eax; pop rax
		memcpy(p, s.game + 3, 4);           // cmp [rdx+0x24], r10d
		p += 4;
		*p++ = 0xe9;
		int32_t back = Rel32(p + 4, s_exe + 0x14c0e7);
		memcpy(p, &back, 4);
		p += 4;
		s_codeEnd = (uint8_t*)(((uintptr_t)p + 15) & ~(uintptr_t)15);
		s.ours[0] = 0xe9;
		int32_t to = Rel32(s_exe + 0x14c0e0 + 5, start);
		memcpy(s.ours + 1, &to, 4);
		for (int i = 5; i < 7; i++) s.ours[i] = 0x90;
	}
}

constexpr uint8_t kLerpLoadBytes[] = {0xf3, 0x0f, 0x10, 0x97, 0xcc, 0x75, 0x02, 0x00};  // movss xmm2, [rdi+0x275cc]
constexpr uint8_t kLerpXmm14Bytes[] = {0x41, 0x0f, 0x28, 0xd6, 0x48, 0x8d, 0x54, 0x24, 0x60};  // movaps xmm2, xmm14; lea
constexpr uint8_t kLerpLeasBytes[] = {0x48, 0x8d, 0x55, 0x90, 0x48, 0x8d, 0x4c, 0x24, 0x40};   // lea rdx; lea rcx
constexpr uint8_t kLerpCall[] = {0x48, 0x8d, 0x54, 0x24, 0x60, 0x48, 0x8d, 0x4d, 0xc0, 0xe8};  // lea; lea; call (lerp)
constexpr uint8_t kCountdownDelayBytes[] = {0xff, 0xc8, 0xc7, 0x42, 0x14, 0x00, 0x00, 0x80, 0x3f};
constexpr uint8_t kCountdownDurationBytes[] = {0xff, 0x4a, 0x24, 0x44, 0x39, 0x52, 0x24};

constexpr uint8_t kBlendBytes[] = {0xf3, 0x0f, 0x10, 0x15};       // 0x4b3abd movss xmm2, [rip] (0.14)
constexpr uint8_t kEaseXmm1Bytes[] = {0xf3, 0x0f, 0x59, 0x8f, 0x80, 0x28, 0x01, 0x00};  // mulss xmm1, [rdi+0x12880]
constexpr uint8_t kEaseXmm0Bytes[] = {0xf3, 0x0f, 0x59, 0x87, 0x80, 0x28, 0x01, 0x00};  // mulss xmm0, [rdi+0x12880]
constexpr uint8_t kTurnMinBytes[] = {0xf3, 0x0f, 0x5d, 0x0d};   // 0x527677 minss xmm1, [rip] (+0.1 deg)
constexpr uint8_t kTurnMaxBytes[] = {0xf3, 0x0f, 0x5f, 0x0d};   // 0x52767f maxss xmm1, [rip] (-0.1 deg)
constexpr uint8_t kPhaseBytes[] = {0xf3, 0x0f, 0x10, 0x87, 0x9c, 0x6b, 0x01, 0x00};  // movss xmm0, [rdi+0x16b9c]
constexpr uint8_t kHackPhaseBytes[] = {0xf3, 0x0f, 0x58, 0x35};  // addss xmm6, [rip] (pi/100 per frame)
constexpr uint8_t kHackAddBytes[] = {0xf3, 0x0f, 0x58, 0x05};   // addss xmm0, [rip] (1 deg per frame)
constexpr uintptr_t kOneSixtieth = 0xdba75c;                     // the game's shared 1/60
constexpr uint8_t kHackTurnBytes[] = {0xf3, 0x0f, 0x10, 0x15};   // movss xmm2, [rip] (3 deg per frame)
constexpr uint8_t kCounterBytes[] = {0x83, 0xf8, 0x0f};          // cmp eax, 15 (frames)
constexpr uintptr_t kCounterImm = 0x839404;
bool s_counterOk;
uint8_t s_counterFrames = 15;

bool RipTargets(uintptr_t rva, uint8_t len, uintptr_t target) {
	int32_t d;
	memcpy(&d, s_exe + rva + len - 4, 4);
	return s_exe + rva + len + d == s_exe + target;
}

// Every `addss/subss xmm, [1/60]` in the game (the shared constant at 0xdba75c): timers that move a fixed 60th of a
// second per frame (whole-exe scan, NOTES A8 stage 5). [Render] FrameTimers=0 leaves them alone.
constexpr uint32_t kFrameTimers[] = {
	0x2f59e6,
	0x2f73ae,
	0x2fdecd,
	0x3a22f5,
	0x3a2368,
	0x3a4330,
	0x3a44cf,
	0x3aa935,
	0x41980b,
	0x41a921,
	0x424f5d,
	0x425c31,
	0x425d08,
	0x43ea89,
	0x43ec29,
	0x4551ef,
	0x456510,
	0x4567a3,
	0x456966,
	0x469571,
	0x46f8a7,
	0x5b3e00,
	0x5b441f,
	0x5d4b58,
	0x5d4eb9,
	0x5d4f1e,
	0x5d500e,
	0x5d51e1,
	0x5ec53e,
	0x5ec92b,
	0x601fc5,
	0x60229b,
	0x602c88,
	0x602f63,
	0x6032c9,
	0x6039d5,
	0x604fd5,
	0x6054ac,
	0x673082,
	0x679972,
	0x67aca9,
	0x67d439,
	0x67d590,
	0x69d514,
	0x69d619,
	0x6d869c,
	0x6d874d,
	0x6fdedf,
	0x722286,
	0x827dc1,
	0x8284b2,
	0x828bb6,
	0x82b906,
	0x82b99d,
	0x85460a,
	0x862b91,
	0x8bb831,
	0x8bb95d,
	0x930817,
	0x9383d0,
	0x9384f0,
	0x93ff19,
	0x94d4f0,
	0x94d99c,
	0x95286b,
	0x984bdb,
	0x984df1,
};

// The 1/60 timers: each checked on its own (an `addss/subss xmm0/xmm1, [rip]` reading the 1/60), a mismatch skipped.
void AddFrameTimers() {
	int added = 0, skipped = 0;
	for (uint32_t rva : kFrameTimers) {
		const uint8_t* b = s_exe + rva;
		bool shape = b[0] == 0xf3 && b[1] == 0x0f && (b[2] == 0x58 || b[2] == 0x5c) && (b[3] & 0xc7) == 0x05;
		if (!shape || !RipTargets(rva, 8, kOneSixtieth) || s_siteCount >= (int)(sizeof(s_sites) / sizeof(s_sites[0]))) {
			skipped++;
			continue;
		}
		AddRipSite("timer: a 60th per frame", rva, 8, 4, NV_FRAME_SECONDS);
		added++;
	}
	Log("game frame rate: %d timers that move a 60th of a second per frame found%s", added,
		skipped ? " (some places weren't as expected and are left alone)" : "");
}

// Per-frame constants found by a whole-game scan (NOTES A8 stage 6): `addss/subss/mulss xmm, [rip -> constant]`
// reading a fixed amount, easing factor or decay meant for one 60th of a second. Each site is pointed at our own value
// for its constant: amount x t, easing 1 - (1 - k)^t, decay k^t, with t = min(step, 1) - unchanged at 60 and below.
// [Render] FrameConstants=0 leaves them alone. 113 sites.
constexpr uint32_t kFrameConstants[] = {
	// camera mode blend +0x39c += 0.02 per frame
	0x72293e,
	0x722e3c,
	0x722f9d,
	0x72a29a,
	0x72a50a,
	0x72a8ea,
	0x72b3b5,
	0x72cd27,
	0x72de7f,
	0x72eae1,
	0x732903,
	0x7339c1,
	0x733fdc,
	0x7345b2,
	0x734bbc,
	0x735291,
	0x7358b1,
	0x735ed1,
	0x7364c2,
	// camera easing
	0x723284,
	0x726106,
	0x7277bd,
	0x736c58,
	0x739cfd,
	0x739d2f,
	// player position easing / height
	0x4e32db,
	0x4c1186,
	0x4c7a20,
	0x4b2720,
	// player easing
	0x4a11bb,
	0x4a3e27,
	0x4a4235,
	0x4a9bf8,
	0x4b857d,
	0x4b85a0,
	0x4c0f3a,
	0x4c2bf2,
	0x4c4885,
	0x4c9c9e,
	0x4ca133,
	0x4ce59c,
	0x4d40ac,
	0x4d43fd,
	0x51493d,
	0x516c7d,
	0x520e9d,
	0x520ebd,
	0x526217,
	0x526d73,
	// player ramps
	0x4ccb7e,
	0x526c3e,
	0x4c82e1,
	0x4d81c9,
	0x51ef9a,
	0x4ffc2b,
	0x4bfff7,
	// player countdown
	0x524469,
	// per-character component (player and enemies)
	0x410ac3,
	0x41109c,
	0x4110bf,
	// player decay
	0x4a651b,
	0x4a6533,
	// Pod
	0x675e47,
	0x676637,
	0x595fb5,
	0x5971f7,
	// bullet lifetimes
	0x6c01a4,
	0x5f55f1,
	// hacking enemies
	0x573801,
	0x573951,
	0x573aaf,
	0x573b8f,
	0x68d93e,
	// enemy turning
	0x5681a2,
	0x5681cd,
	0x56829e,
	0x5682c2,
	0x56a840,
	0x56a86b,
	0x56a929,
	0x56a94d,
	0x56c455,
	0x56c480,
	0x56c53e,
	0x56c562,
	0x56d01f,
	0x56d04a,
	0x56d102,
	0x56d125,
	0x6a1c67,
	0x6a1d4b,
	0x6a1d76,
	// enemy turn rate
	0x5b2a3e,
	0x5b32e3,
	0x5b59ba,
	0x5c4d98,
	0x6a3969,
	0x6a58b8,
	0x6a75c0,
	// dropped items countdown
	0x5eb5e8,
	0x5ebde9,
	0x5ebeb9,
	0x5ec0bf,
	0x5ec2ea,
	// items fade and spin
	0x5ee891,
	0x5ee985,
	0x5eea82,
	0x5efdcc,
	// flight unit easing
	0x4815cc,
	0x481655,
	0x4816e2,
	// UI hold counter
	0x954cea,
	0x954d8a,
	// timers adding a separate -1/60 constant (0xce8f28; REA audit)
	0x827d8b,
	0x82d3d9,
	0x83e081,
};

// Multipliers that size a per-frame movement (not an easing): x t. The hacking-area bob, pos += sin(phase) * 0.1 * amp
// each frame - with only its phase scaled the bob grew with the frame rate (REA audit).
constexpr uint32_t kFrameScales[] = {0x6da73c, 0x6da82c, 0x6da872};

enum ConstKind : uint8_t { CK_SCALE,
						   CK_ADD,
						   CK_EASE,
						   CK_DECAY };
struct ConstSlot {
	uint32_t bits;  // the game's constant
	ConstKind kind;
	float game;
};
constexpr size_t kNearConstSlots = 256;  // our values for them, in s_near from here (data page)
constexpr int kMaxConstSlots = (int)((0x1000 - kNearConstSlots) / sizeof(float));
ConstSlot s_constSlots[kMaxConstSlots];
int s_constSlotCount;

// A rip-relative load pointed at a float of ours anywhere in s_near.
void AddRipSiteTo(const char* what, uintptr_t rva, float* target) {
	FrameSite& s = s_sites[s_siteCount++];
	s = {what, rva, 8, {}, {}};
	memcpy(s.game, s_exe + rva, 8);
	memcpy(s.ours, s.game, 8);
	int32_t d = Rel32(s_exe + rva + 8, target);
	memcpy(s.ours + 4, &d, 4);
}

void AddFrameConstants() {
	int added = 0, skipped = 0;
	constexpr int kAll = (int)(sizeof(kFrameConstants) / sizeof(kFrameConstants[0]) + sizeof(kFrameScales) / sizeof(kFrameScales[0]));
	for (int n = 0; n < kAll; n++) {
		bool scale = n >= (int)(sizeof(kFrameConstants) / sizeof(kFrameConstants[0]));
		uint32_t rva = scale ? kFrameScales[n - sizeof(kFrameConstants) / sizeof(kFrameConstants[0])] : kFrameConstants[n];
		const uint8_t* b = s_exe + rva;
		bool shape = b[0] == 0xf3 && b[1] == 0x0f && (b[2] == 0x58 || b[2] == 0x5c || b[2] == 0x59) && (b[3] & 0xc7) == 0x05;
		int32_t disp;
		memcpy(&disp, b + 4, 4);
		uintptr_t target = rva + 8 + (intptr_t)disp;
		if (!shape || target < 0xc1b000 || target >= 0xf04000 ||
			s_siteCount >= (int)(sizeof(s_sites) / sizeof(s_sites[0]))) {
			skipped++;
			continue;
		}
		float c;
		uint32_t bits;
		memcpy(&c, s_exe + target, 4);
		memcpy(&bits, s_exe + target, 4);
		ConstKind kind = CK_ADD;
		if (scale && b[2] == 0x59) kind = CK_SCALE;
		else if (b[2] == 0x59) {  // a multiply: an easing factor (small) or a decay (close to 1)
			if (c > 0 && c < 0.5f) kind = CK_EASE;
			else if (c >= 0.5f && c < 1.0f) kind = CK_DECAY;
			else {
				skipped++;
				continue;
			}
		}
		int slot = -1;
		for (int i = 0; i < s_constSlotCount && slot < 0; i++)
			if (s_constSlots[i].bits == bits && s_constSlots[i].kind == kind) slot = i;
		if (slot < 0) {
			if (s_constSlotCount >= kMaxConstSlots) {
				skipped++;
				continue;
			}
			slot = s_constSlotCount++;
			s_constSlots[slot] = {bits, kind, c};
			((float*)(s_near + kNearConstSlots))[slot] = c;
		}
		AddRipSiteTo("per-frame constant", rva, (float*)(s_near + kNearConstSlots) + slot);
		added++;
	}
	Log("game frame rate: %d per-frame constants found (%d distinct values)%s", added, s_constSlotCount,
		skipped ? "; some places weren't as expected and are left alone" : "");
}

void UpdateConstSlots(float step) {
	float t = step > 1.0f ? 1.0f : step;
	float* v = (float*)(s_near + kNearConstSlots);
	for (int i = 0; i < s_constSlotCount; i++) {
		const ConstSlot& c = s_constSlots[i];
		v[i] = c.kind == CK_ADD || c.kind == CK_SCALE ? c.game * t
													  : (c.kind == CK_EASE ? 1.0f - powf(1.0f - c.game, t) : powf(c.game, t));
	}
}

// Frame counts in the game's code, as immediates: the menu auto-repeat at 0x828b8e (counts held frames: nothing up to
// 10, fires from 13, then steps back 3 - so it repeated faster above 60; REA audit). Each = its count at 60 / step.
struct FrameCount {
	uintptr_t at;  // the imm8
	int8_t game;   // the game's count (signed: the step back is -3)
	int8_t now;
	bool ok;
} s_frameCounts[] = {{0x828b90, 10, 10, false}, {0x828b95, 13, 13, false}, {0x828b9a, -3, -3, false}};

void PrepareSites() {
	if (!s_near) return;
	bool ok = Matches(0x4b3abd, kBlendBytes, 4) && RipTargets(0x4b3abd, 8, 0xc943e8) && Matches(0x4bc4dc, kEaseXmm1Bytes, 8) &&
			  Matches(0x4bc69b, kEaseXmm0Bytes, 8) && Matches(0x527677, kTurnMinBytes, 4) && RipTargets(0x527677, 8, 0xdba728) &&
			  Matches(0x52767f, kTurnMaxBytes, 4) && RipTargets(0x52767f, 8, 0xc94780) && Matches(0x527693, kPhaseBytes, 8) &&
			  Matches(0x6da744, kHackPhaseBytes, 4) && RipTargets(0x6da744, 8, 0xca4eec) && Matches(0x6da824, kHackPhaseBytes, 4) &&
			  RipTargets(0x6da824, 8, 0xca4eec) && Matches(0x6da969, kHackTurnBytes, 4) && RipTargets(0x6da969, 8, 0xc943a4);
	s_counterOk = Matches(0x839402, kCounterBytes, 3);
	// the menu auto-repeat: cmp edx, 10 / cmp edx, 13 / lea eax, [rdx-3]
	s_frameCounts[0].ok = s_exe[0x828b8e] == 0x83 && s_exe[0x828b8f] == 0xfa && s_exe[0x828b90] == 0x0a;
	s_frameCounts[1].ok = s_exe[0x828b93] == 0x83 && s_exe[0x828b94] == 0xfa && s_exe[0x828b95] == 0x0d;
	s_frameCounts[2].ok = s_exe[0x828b98] == 0x8d && s_exe[0x828b99] == 0x42 && s_exe[0x828b9a] == 0xfd;
	bool lerpsOk = Matches(0x56019f, kLerpLoadBytes, 8) && Matches(0x5601a7, kLerpCall, 10) &&
				   Matches(0x56029c, kLerpLoadBytes, 8) && Matches(0x5602a4, kLerpCall, 10) &&
				   Matches(0x560452, kLerpXmm14Bytes, 9) && Matches(0x561291, kLerpLeasBytes, 9);
	bool countdownsOk = Matches(0x14c0d2, kCountdownDelayBytes, 9) && Matches(0x14c0e0, kCountdownDurationBytes, 7);
	if (!lerpsOk) Log("game frame rate: the player's per-frame blends aren't as expected: left as they are");
	if (!countdownsOk) Log("game frame rate: the timed clock effects' countdowns aren't as expected: left as they are");
	if (!ok || !Reaches(s_exe, s_near) || !Reaches(s_exe + 0x600000, s_near)) {
		Log("game frame rate: the per-frame places in the game aren't as expected: left as they are");
		return;
	}
	s_codeEnd = s_near + kNearCodeStart;
	AddRipSite("0.14 blend per frame", 0x4b3abd, 8, 4, NV_BLEND);
	AddScaleDetour("easing per frame (a)", 0x4bc4dc, 1);
	AddScaleDetour("easing per frame (b)", 0x4bc69b, 0);
	AddRipSite("turn limit per frame (max)", 0x527677, 8, 4, NV_TURN_MAX);
	AddRipSite("turn limit per frame (min)", 0x52767f, 8, 4, NV_TURN_MIN);
	AddScaleDetour("sway phase per frame", 0x527693, 0);
	AddRipSite("hacking: phase per frame (a)", 0x6da744, 8, 4, NV_HACK_PHASE);
	AddRipSite("hacking: phase per frame (b)", 0x6da824, 8, 4, NV_HACK_PHASE);
	AddRipSite("hacking: turn per frame", 0x6da969, 8, 4, NV_HACK_TURN);
	if (lerpsOk) {
		AddBlendDetour("player blend per frame (a)", 0x56019f, 8, 8);
		AddBlendDetour("player blend per frame (b)", 0x56029c, 8, 8);
		AddBlendDetour("player blend per frame (c)", 0x560452, 4, 9);
		AddBlendDetour("player blend per frame (d)", 0x561291, 0, 9);
	}
	if (countdownsOk) AddCountdownDetours();
	if (Matches(0x6d9468, kHackAddBytes, 4) && RipTargets(0x6d9468, 8, 0xdba764))
		AddRipSite("1 deg turn per frame", 0x6d9468, 8, 4, NV_DEG1);
	if (Matches(0x6da86a, kHackPhaseBytes, 4) && RipTargets(0x6da86a, 8, 0xca4f08))
		AddRipSite("hacking: phase per frame (c)", 0x6da86a, 8, 4, NV_HACK_PHASE2);
	if (g_cfg.frameTimers) {
		AddFrameTimers();
		// the same timers where the 1/60 is first loaded into a register (movss xmmN, [1/60]; then subss), and one
		// timer's 1/60 threshold (comiss) kept consistent with its subtraction (REA audit)
		constexpr uint32_t kTimerLoads[] = {0x3a46bc, 0x43bfac, 0x742311, 0x82670d, 0x8306a4, 0x97ca6a};
		int loads = 0;
		for (uint32_t rva : kTimerLoads) {
			const uint8_t* b = s_exe + rva;
			if (b[0] == 0xf3 && b[1] == 0x0f && b[2] == 0x10 && (b[3] & 0xc7) == 0x05 && RipTargets(rva, 8, kOneSixtieth)) {
				AddRipSite("timer: a 60th per frame (register)", rva, 8, 4, NV_FRAME_SECONDS);
				loads++;
			}
		}
		constexpr uint8_t kThresholdBytes[] = {0x0f, 0x2f, 0x0d};  // comiss xmm1, [rip]
		if (Matches(0x94d4e6, kThresholdBytes, 3) && RipTargets(0x94d4e6, 7, kOneSixtieth)) {
			AddRipSite("timer threshold: a 60th", 0x94d4e6, 7, 3, NV_FRAME_SECONDS);
			loads++;
		}
		Log("game frame rate: %d more timer places (register loads, a threshold)", loads);
	}
	// a smoothed speed (+0x128d8) applied as a per-frame movement: x step (REA audit; next to the easing at 0x4bc4dc)
	constexpr uint8_t kSpeedBytes[] = {0xf3, 0x0f, 0x10, 0x97, 0xd8, 0x28, 0x01, 0x00};  // movss xmm2, [rdi+0x128d8]
	if (g_cfg.frameConstants && Matches(0x4bc522, kSpeedBytes, 8) && Matches(0x4bc6dd, kSpeedBytes, 8)) {
		AddScaleDetour("speed per frame (a)", 0x4bc522, 2);
		AddScaleDetour("speed per frame (b)", 0x4bc6dd, 2);
	}
	if (g_cfg.frameConstants) AddFrameConstants();
	s_sitesOk = true;
}

// Called on the game thread between frames (the limiter), where none of these sites is running.
// Other threads paused while the sites are rewritten, none of them stopped inside one (a thread part-way through a
// rewritten instruction would run garbage): a thread found inside one runs on a moment and the check repeats.
struct PausedThreads {
	HANDLE handles[1024];
	int count = 0;
	bool Pause() {
		DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
		HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
		if (snap == INVALID_HANDLE_VALUE) return false;
		THREADENTRY32 te = {sizeof(te)};
		bool all = true;
		for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
			if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
			if (count >= 1024) {
				all = false;
				break;
			}
			HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
			if (!h) continue;  // already gone
			if (SuspendThread(h) == (DWORD)-1) {
				CloseHandle(h);
				continue;
			}
			handles[count++] = h;
		}
		CloseHandle(snap);
		return all;
	}
	bool Inside(uintptr_t rip) {
		uintptr_t clock = (uintptr_t)s_exe + kClockBaseLoad;
		if (rip > clock && rip < clock + 8) return true;
		for (int i = 0; i < s_siteCount; i++) {
			uintptr_t at = (uintptr_t)s_exe + s_sites[i].rva;
			if (rip > at && rip < at + s_sites[i].len) return true;
		}
		return false;
	}
	bool AnyInside() {
		for (int t = 0; t < count; t++) {
			CONTEXT ctx = {};
			ctx.ContextFlags = CONTEXT_CONTROL;
			if (!GetThreadContext(handles[t], &ctx) || Inside(ctx.Rip)) return true;  // unknown = not safe
		}
		return false;
	}
	void Resume() {
		for (int t = 0; t < count; t++) {
			ResumeThread(handles[t]);
			CloseHandle(handles[t]);
		}
		count = 0;
	}
} s_paused;

// The other threads suspended (under the lock every thread suspender holds: core/hook.h), none of them stopped inside a
// place about to be rewritten (a thread part-way through a rewritten instruction would run garbage). False: nothing is
// suspended any more and nothing may be written - the caller tries again next frame (review 2026-10-28).
bool PauseOthersClean() {
	ThreadSuspendLock();
	for (int attempt = 0; attempt < 50; attempt++) {
		bool all = s_paused.Pause();
		if (all && !s_paused.AnyInside()) return true;
		s_paused.Resume();
		if (!all) break;  // couldn't list or hold every thread: give up for this frame
		Sleep(1);
	}
	ThreadSuspendUnlock();
	return false;
}

void ResumeOthers() {
	s_paused.Resume();
	ThreadSuspendUnlock();
}

void SetSites(bool on) {
	if (!s_sitesOk || on == s_sitesOn) return;
	if (!PauseOthersClean()) return;  // tried again next frame
	for (int i = 0; i < s_siteCount; i++) WriteCode(s_exe + s_sites[i].rva, on ? s_sites[i].ours : s_sites[i].game, s_sites[i].len);
	ResumeOthers();
	s_sitesOn = on;
	if (!on && s_counterOk && s_counterFrames != 15) {
		s_counterFrames = 15;
		WriteCode(s_exe + kCounterImm, &s_counterFrames, 1);
	}
	Log("game frame rate: %d per-frame places in the game %s", s_siteCount, on ? "now follow the frame time" : "back to the game's own");
}

void UpdateSiteValues(float step) {
	if (!s_near) return;
	float* v = (float*)s_near;
	step = step < 0.05f ? 0.05f : (step > 2.0f ? 2.0f : step);
	v[NV_STEP] = step;
	v[NV_BLEND] = 1.0f - powf(1.0f - 0.14f, step);
	v[NV_TURN_MAX] = 0.0017453292f * step;
	v[NV_TURN_MIN] = -0.0017453292f * step;
	static float carry;
	carry += step > 1.0f ? 1.0f : step;
	int32_t ticks = carry >= 1.0f ? 1 : 0;
	carry -= (float)ticks;
	*(int32_t*)(s_near + kNearTicks) = ticks;
	v[NV_HACK_PHASE] = 0.031415927f * step;
	v[NV_HACK_TURN] = 0.052359877f * step;
	v[NV_FRAME_SECONDS] = (step > 1.0f ? 1.0f : step) / 60.0f;  // below 60 they move a 60th, as the game always did
	v[NV_DEG1] = 0.017453292f * step;
	v[NV_HACK_PHASE2] = 0.062831853f * step;
	UpdateConstSlots(step);
}

void SetFrameCounts(float average, bool on) {
	for (FrameCount& f : s_frameCounts) {
		if (!f.ok) continue;
		int want = on ? (int)lround(f.game / (average > 0.25f ? average : 0.25f)) : f.game;
		want = want > 120 ? 120 : (want < -120 ? -120 : want);
		if ((int8_t)want == f.now) continue;
		f.now = (int8_t)want;
		WriteCode(s_exe + f.at, &f.now, 1);  // one byte: written whole
	}
}

// The 15-frame counter: 15 frames at 60 = a quarter second, so 15 / (the average measured step) frames - the rate the
// game really runs at (not the one asked for: the game may fall short, or be in its own 30 fps mode).
void UpdateCounter(float step) {
	static float average = 1.0f;
	average += (step - average) * 0.02f;
	SetFrameCounts(average, s_sitesOn && g_cfg.frameConstants);
	if (!s_counterOk || !s_sitesOn) return;
	int frames = (int)lround(15.0 / (average > 0.05f ? average : 0.05f));
	frames = frames < 15 ? 15 : (frames > 60 ? 60 : frames);
	if ((uint8_t)frames == s_counterFrames) return;
	s_counterFrames = (uint8_t)frames;
	WriteCode(s_exe + kCounterImm, &s_counterFrames, 1);
	Log("game frame rate: the game's 15-frame counter now counts %d frames (the same quarter second)", frames);
}

// --- headset rate ------------------------------------------------------------------------------------------------
// The game's own limiter only knows thirds of a millisecond: 90 fps is 33 = 90.9 fps against a 90 Hz headset, a hitch
// about once a second. With "headset rate" the game frames are paced by us at the headset's exact refresh period
// (xrWaitFrame's predictedDisplayPeriod), on a drift-free schedule, and pulled gently (a tenth of the error a frame)
// toward the moments xrWaitFrame returns, which follow the headset's refreshes. The game never waits ON the VR thread:
// with RealFrameRate the VR thread waits for the game, and the two would wait for each other.
double s_deadline;  // QPC ticks: when the next game frame may start
struct PaceStats {
	uint32_t frames, resyncs;
	double sumPhase, waitedMs;
} s_pace;

bool HeadsetMode() {
	int choice = g_cfg.gameFrameRate;
	return choice > 0 && choice < (int)(sizeof(kRates) / sizeof(kRates[0])) && kRates[choice] < 0;
}

int32_t TargetUnits() {
	int choice = g_cfg.gameFrameRate;
	if (choice <= 0 || choice >= (int)(sizeof(kRates) / sizeof(kRates[0]))) return 0;
	if (kRates[choice] < 0) return s_headsetPeriodNs.load() > 0 ? kOutOfTheWayUnits : 0;  // no headset yet: the game's 60
	return (int32_t)(3000.0 / kRates[choice]);  // rounded down: 72 -> 73.2 fps, 80 -> 81.1, just above a headset at that rate
}

void PaceToHeadset() {
	int64_t periodNs = s_headsetPeriodNs.load();
	if (periodNs <= 0) return;
	double period = periodNs * 1e-9 * (double)s_freq.QuadPart;  // QPC ticks
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	if (s_deadline <= 0 || now.QuadPart > s_deadline + (double)s_freq.QuadPart) {  // the first frame, or a load: restart
		s_deadline = (double)now.QuadPart;
		s_pace.resyncs++;
	} else if (now.QuadPart > s_deadline) {
		// late: skip the slots already missed instead of running the next frames back to back (review 2026-10-28)
		s_deadline += ceil(((double)now.QuadPart - s_deadline) / period) * period;
	}
	// phase: toward the headset's refreshes (the last xrWaitFrame return, plus whole periods)
	int64_t headset = s_headsetFrameQpc.load();
	if (headset > 0) {
		double phase = fmod(s_deadline - (double)headset, period);
		if (phase < 0) phase += period;
		if (phase > period / 2) phase -= period;
		s_deadline -= phase * 0.1;
		s_pace.sumPhase += fabs(phase) / (double)s_freq.QuadPart * 1000.0;
	}
	// wait: a high-resolution timer for all but the last millisecond, then spin. Plain Sleep can take 15.6 ms when
	// Windows lowers a covered or minimised window's timer resolution (review 2026-10-28).
	static HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */,
												 TIMER_ALL_ACCESS);
	double start = (double)now.QuadPart;
	for (;;) {
		QueryPerformanceCounter(&now);
		double left = (s_deadline - (double)now.QuadPart) / (double)s_freq.QuadPart * 1000.0;
		if (left <= 0) break;
		if (left > 1.5) {
			if (timer) {
				LARGE_INTEGER due;
				due.QuadPart = -(LONGLONG)((left - 1.0) * 10000.0);  // relative, 100 ns units
				if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, 50);
			} else {
				Sleep(1);
			}
		} else {
			YieldProcessor();
		}
	}
	s_pace.waitedMs += ((double)now.QuadPart - start) / (double)s_freq.QuadPart * 1000.0;
	s_pace.frames++;
	s_deadline += period;
}

// The period: our value in the code the game uses for its 60 fps mode, and in the live period while that mode is on.
void ApplyPeriod(int32_t units) {
	if (units == s_appliedUnits) return;
	int32_t want = units ? units : (int32_t)kGameUnitsPer60;
	uint8_t imm[5] = {0xb8, (uint8_t)want, (uint8_t)(want >> 8), 0, 0};
	WriteCode(s_exe + kPeriod60Imm, imm, 5);
	uint8_t* game = *(uint8_t**)(s_exe + kGamePtr);
	if (game) {
		int32_t* period = (int32_t*)(game + kPeriodOffset);
		int32_t was = s_appliedUnits ? s_appliedUnits : (int32_t)kGameUnitsPer60;
		if (*period == was) *period = want;  // the 30 fps mode (100) is the game's: left alone
	}
	if (units && HeadsetMode())
		Log("game frame rate: paced by the headset (%.2f Hz); the game's own limiter set out of the way",
			1e9 / (double)s_headsetPeriodNs.load());
	else
		Log("game frame rate: %s (frame period %d/3 ms = %.1f fps)", units ? "raised" : "the game's own 60", want, 3000.0 / want);
	s_appliedUnits = units;
}

uint64_t __fastcall H_Limiter(void* a, void* b, void* c, void* d) {
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	double dt = s_lastEntry.QuadPart ? (double)(now.QuadPart - s_lastEntry.QuadPart) / (double)s_freq.QuadPart : 0;
	s_lastEntry = now;
	if (s_minimisedSkipOk) {  // here, on the only thread that runs the limiter: it can't be inside the jump being written
		WriteCode(s_exe + kMinimisedSkip, kNop6, sizeof(kNop6));
		s_minimisedSkipOk = false;
		Log("game frame rate: the game's limiter now also waits while its window is minimised (same rate focused or not)");
	}
	// the settings once per frame: the panel (VR thread) may change them meanwhile
	const bool stepFixWanted = g_cfg.gameStepFix != 0, speedFixWanted = g_cfg.gameSpeedFix != 0;
	int32_t units = TargetUnits();
	ApplyPeriod(units);
	if (units && HeadsetMode()) PaceToHeadset();
	// the game's own measured step may go below one 60th while a higher rate is set
	uint8_t opcode = units && stepFixWanted && s_clampOk ? 0xeb : 0x73;
	if (opcode != s_clampOpcode) {
		WriteCode(s_exe + kStepClampJump, &opcode, 1);
		s_clampOpcode = opcode;
		Log("game frame rate: the game's measured frame step %s", opcode == 0xeb ? "may now go below a 60th (GameStepFix=1)"
																				 : "is held at a 60th or more again (the game's)");
	}
	bool stepFix = opcode == 0xeb;
	UpdateSiteValues(*(float*)(s_exe + kMeasuredStep));
	SetSites(stepFix);
	UpdateCounter(*(float*)(s_exe + kMeasuredStep));
	SetClockBase(units && speedFixWanted && !stepFix);
	// game speed: one frame advances the game by the real time it took (in 60ths of a second, at most one 60th) -
	// only without the step fix: with it the clocks already follow the game's own measured step
	float scale = 1.0f;
	if (units && speedFixWanted && !stepFix && dt > 0) {
		scale = (float)(dt * 60.0);
		scale = scale < 0.05f ? 0.05f : (scale > 1.0f ? 1.0f : scale);
	}
	s_scale.store(scale);
	if (s_clockBase) *s_clockBase = scale;
	float measured = *(float*)(s_exe + kMeasuredStep);
	uint8_t* timeScale = *(uint8_t**)(s_exe + kTimeScalePtr);
	if (dt > 0 && dt < 1) {
		s_stats.frames++;
		s_stats.seconds += dt;
		s_stats.sumScale += scale;
		s_stats.minScale = scale < s_stats.minScale ? scale : s_stats.minScale;
		s_stats.maxScale = scale > s_stats.maxScale ? scale : s_stats.maxScale;
		s_stats.sumStep += measured;
		s_stats.minStep = measured < s_stats.minStep ? measured : s_stats.minStep;
		s_stats.maxStep = measured > s_stats.maxStep ? measured : s_stats.maxStep;
		if (timeScale) {
			s_stats.sumGroup += *(float*)(timeScale + 0xa4);
			s_stats.groupSamples++;
		}
	}
	if (!s_stats.since) s_stats.since = GetTickCount();
	if (GetTickCount() - s_stats.since >= 10000) {
		if (s_stats.frames && units)
			Log("game frame rate (10 s): %.1f fps; our clock scale avg %.3f min %.3f max %.3f; the game's measured step avg "
				"%.3f min %.3f max %.3f; group factor avg %.3f (1 = a 60th of a second)",
				s_stats.frames / s_stats.seconds, (double)(s_stats.sumScale / s_stats.frames), (double)s_stats.minScale,
				(double)s_stats.maxScale, (double)(s_stats.sumStep / s_stats.frames), (double)s_stats.minStep,
				(double)s_stats.maxStep, (double)(s_stats.groupSamples ? s_stats.sumGroup / s_stats.groupSamples : 0));
		if (s_pace.frames)
			Log("game frame rate: paced to the headset: %u frames, waited avg %.2f ms, off the headset's rhythm avg %.2f ms, "
				"%u restarts",
				s_pace.frames, s_pace.waitedMs / s_pace.frames, s_pace.sumPhase / s_pace.frames, s_pace.resyncs);
		s_pace = {};
		s_stats = {};
		s_stats.since = GetTickCount();
	}
	return o_Limiter(a, b, c, d);
}

// The step getter: the object's own clock already carries the scale (its base); the two other paths get it here.
float __fastcall H_Step(void** clockRef) {
	uint8_t* clock = (uint8_t*)*clockRef;
	if (clock) return *(float*)(clock + 0x1c);
	uint8_t* global = *(uint8_t**)(s_exe + kTimeScalePtr);
	float step = global ? *(float*)(global + 0xa4) : 1.0f;
	return step * s_scale.load(std::memory_order_relaxed);
}
}  // namespace

void FrameRateInstall() {
	s_exe = (uint8_t*)GetModuleHandleW(nullptr);
	QueryPerformanceFrequency(&s_freq);
	int32_t disp;
	memcpy(&disp, s_exe + kClockBaseLoad + 4, 4);
	s_clockBaseGameDisp = disp;
	bool clockOk = Matches(kClockBaseLoad, kClockBaseBytes, sizeof(kClockBaseBytes)) &&
				   s_exe + kClockBaseLoad + 8 + disp == s_exe + kOneConst;
	if (!Matches(kLimiter, kLimiterBytes, sizeof(kLimiterBytes)) || !Matches(kPeriod60Imm, kPeriod60Bytes, 5) ||
		!Matches(kPeriod30Imm, kPeriod30Bytes, 5) || !clockOk || !Matches(kStepGetter, kStepGetterBytes, sizeof(kStepGetterBytes))) {
		Log("game frame rate: the game's code isn't the build this was made for: the 60 fps cap stays (send me the log)");
		return;
	}
	s_clampOk = Matches(kStepClampContext, kStepClampBytes, sizeof(kStepClampBytes));
	if (!s_clampOk) Log("game frame rate: the measured step's minimum isn't where expected: left as it is");
	s_minimisedSkipOk = Matches(kMinimisedSkip, kMinimisedSkipBytes, sizeof(kMinimisedSkipBytes));
	if (!s_minimisedSkipOk) Log("game frame rate: the limiter's minimised-window jump isn't where expected: left as it is");
	s_near = AllocNearExe();
	s_clockBase = s_near ? (float*)s_near + NV_CLOCK_BASE : nullptr;
	if (s_clockBase) *s_clockBase = 1.0f;
	PrepareSites();
	bool limiter = HookFunction(s_exe + kLimiter, (void*)&H_Limiter, (void**)&o_Limiter, "game frame limiter");
	bool step = HookFunction(s_exe + kStepGetter, (void*)&H_Step, (void**)&o_Step, "game clock step");
	Log("game frame rate: limiter %s, clock step %s; GameFrameRate=%d (%d fps), GameSpeedFix=%d", limiter ? "hooked" : "NOT hooked",
		step ? "hooked" : "NOT hooked", g_cfg.gameFrameRate, TargetUnits() ? kRates[g_cfg.gameFrameRate] : 60, g_cfg.gameSpeedFix);
}

void FrameRateNoteHeadsetFrame(int64_t periodNs) {
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	if (periodNs > 2000000 && periodNs < 50000000) s_headsetPeriodNs.store(periodNs);  // 20-500 Hz
	s_headsetFrameQpc.store(now.QuadPart);
}
