// position_finder.cpp - [Debug] Shift+F11 finds where the game keeps 2B's position in memory, Ctrl+Shift+F11 tests what it found.
#include "debug/position_finder.h"
#include "core/config.h"
#include "core/log.h"
#include "debug/write_watch.h"
#include "game/first_person.h"
#include <iterator>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <windows.h>

namespace {
constexpr float kNearXZ = 0.3f, kBelow = 0.6f, kAbove = 2.0f;  // a candidate this close to her feet, at the scan
constexpr float kKeepsOffset = 0.12f;           // drops out when its offset from her changes this much
constexpr float kEnoughWalking = 4.0f;          // metres she has to move before the report
constexpr DWORD kGiveUpMs = 60000, kCheckEveryMs = 50;
constexpr size_t kChunk = 1 << 20;
constexpr DWORD kWritable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;

struct Candidate {
	uintptr_t at;     // its x
	int32_t stride;   // floats from x to y and y to z (1, or 4 in a matrix)
	pure::V3 offset;  // from her feet, at the scan
};
Candidate s_candidates[32768];
int32_t s_count;
bool s_running, s_scanKeyWasDown, s_nudgeKeyWasDown;
DWORD s_startTick, s_checkTick;
pure::V3 s_last;  // her feet at the last check
float s_walked;   // how far she has moved since the scan
uint8_t s_chunk[kChunk];
constexpr float kNudge = 0.5f;
constexpr DWORD kNudgeWatchMs = 500;
constexpr int32_t kAll = -1, kNone = -2;
int32_t s_nudgeNext = kAll, s_nudged = kNone;  // the candidate to push next, the one pushed
DWORD s_nudgeTick;
pure::V3 s_nudgeFeet;  // her feet just before the push

bool ReadOwn(uintptr_t at, void* to, size_t size) {
	SIZE_T got = 0;
	return ReadProcessMemory(GetCurrentProcess(), (const void*)at, to, size, &got) && got == size;
}

bool ReadPosition(const Candidate& c, pure::V3* out) {
	float x, y, z;
	if (!ReadOwn(c.at, &x, 4) || !ReadOwn(c.at + c.stride * 4, &y, 4) || !ReadOwn(c.at + c.stride * 8, &z, 4)) return false;
	*out = {x, y, z};
	return true;
}

bool InModule(uintptr_t at, uintptr_t* offset) {
	auto base = (uintptr_t)GetModuleHandleW(nullptr);
	auto* nt = (const IMAGE_NT_HEADERS64*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
	*offset = at - base;
	return at >= base && at < base + nt->OptionalHeader.SizeOfImage;
}

// Every committed writable region, `visit(base, size)`: the game's heap and its exe's data; not the mod's,
// not this thread's stack (the mod's own copies of her position are there), not GPU-mapped memory.
template <typename Visit>
void ForEachWritableRegion(Visit visit) {
	HMODULE mod = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&s_candidates, &mod);
	MEMORY_BASIC_INFORMATION stack = {};
	VirtualQuery(&stack, &stack, sizeof(stack));
	SYSTEM_INFO info;
	GetSystemInfo(&info);
	auto at = (uintptr_t)info.lpMinimumApplicationAddress;
	auto end = (uintptr_t)info.lpMaximumApplicationAddress;
	while (at < end) {
		MEMORY_BASIC_INFORMATION region;
		if (!VirtualQuery((const void*)at, &region, sizeof(region))) break;
		auto base = (uintptr_t)region.BaseAddress;
		if (region.State == MEM_COMMIT && (region.Protect & kWritable) && !(region.Protect & (PAGE_GUARD | PAGE_NOACCESS | PAGE_NOCACHE | PAGE_WRITECOMBINE)) &&
			region.AllocationBase != (void*)mod && region.AllocationBase != stack.AllocationBase)
			visit(base, (size_t)region.RegionSize);
		at = base + region.RegionSize;
	}
}

void Scan(pure::V3 feet) {
	s_count = 0;
	size_t scanned = 0;
	double startMs = NowMs();
	ForEachWritableRegion([&](uintptr_t base, size_t size) {
		for (size_t pos = 0; pos < size && s_count < (int32_t)std::size(s_candidates); pos += kChunk) {
			size_t bytes = size - pos < kChunk ? size - pos : kChunk;
			if (!ReadOwn(base + pos, s_chunk, bytes)) continue;
			scanned += bytes;
			const auto* f = (const float*)s_chunk;
			size_t floats = bytes / 4;
			for (size_t i = 0; i < floats && s_count < (int32_t)std::size(s_candidates); i++) {
				if (!(fabsf(f[i] - feet.x) < kNearXZ)) continue;  // (almost every float)
				for (int32_t stride : {1, 4}) {
					if (i + 2 * stride >= floats) continue;
					float y = f[i + stride], z = f[i + 2 * stride];
					if (!(fabsf(z - feet.z) < kNearXZ) || !(y > feet.y - kBelow && y < feet.y + kAbove)) continue;
					s_candidates[s_count++] = {base + pos + i * 4, stride, pure::V3{f[i], y, z} - feet};
					break;
				}
			}
		}
	});
	Log("position finder: her feet at %.2f %.2f %.2f; %d places in %u MB of memory hold something near them (%.0f ms). "
		"Now walk, run and jump around for a while (%.0f m)",
		(double)feet.x, (double)feet.y, (double)feet.z, s_count, (unsigned)(scanned >> 20), NowMs() - startMs, (double)kEnoughWalking);
}

// Keeps the candidates that kept their offset from her.
void Narrow(pure::V3 feet) {
	int32_t kept = 0;
	for (int32_t i = 0; i < s_count; i++) {
		pure::V3 now;
		if (ReadPosition(s_candidates[i], &now) && pure::length(now - feet - s_candidates[i].offset) < kKeepsOffset) s_candidates[kept++] = s_candidates[i];
	}
	s_count = kept;
}

// Pointer paths to the candidates: a pointer to somewhere a little before one (the object holding it),
// held in the exe's own memory, or in an object that the exe's memory points to. Wide while looking,
// tight once a single place moved her (her object starts just before her position).
struct Reach {
	uint64_t object, holder;  // how far before the position its object may start, and before the pointer its holder
};
constexpr Reach kWide = {0x10000, 0x10000}, kTight = {0x200, 0x2000};
Reach s_reach = kWide;
struct Holder {
	uintptr_t at;    // where the pointer is
	uint64_t value;  // where it points
	int32_t target;  // which candidate
};
Holder s_holders[512];
int32_t s_holderCount;

// `visit(at, value)` for each 8-byte value in [from, from + size), read in chunks
template <typename Visit>
void ForEachPointer(uintptr_t from, size_t size, Visit visit) {
	for (size_t pos = 0; pos < size; pos += kChunk) {
		size_t bytes = size - pos < kChunk ? size - pos : kChunk;
		if (!ReadOwn(from + pos, s_chunk, bytes)) continue;
		const auto* values = (const uint64_t*)s_chunk;
		for (size_t i = 0; i < bytes / 8; i++) visit(from + pos + i * 8, values[i]);
	}
}

// `visit(from, size)` for the exe's writable sections
template <typename Visit>
void ForEachExeDataSection(Visit visit) {
	auto base = (uintptr_t)GetModuleHandleW(nullptr);
	auto* nt = (const IMAGE_NT_HEADERS64*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
	const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
	for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, section++)
		if (section->Characteristics & IMAGE_SCN_MEM_WRITE) visit(base + section->VirtualAddress, (size_t)section->Misc.VirtualSize);
}

void FindHolders(int32_t first, int32_t targets) {
	s_holderCount = 0;
	uintptr_t lowest = UINTPTR_MAX, highest = 0;
	for (int32_t t = first; t < first + targets; t++) {
		if (s_candidates[t].at < lowest) lowest = s_candidates[t].at;
		if (s_candidates[t].at > highest) highest = s_candidates[t].at;
	}
	ForEachWritableRegion([&](uintptr_t base, size_t size) {
		ForEachPointer(base, size, [&](uintptr_t at, uint64_t value) {
			if (value > highest || value + s_reach.object < lowest) return;  // (most values)
			for (int32_t t = first; t < first + targets && s_holderCount < (int32_t)std::size(s_holders); t++)
				if (value <= s_candidates[t].at && s_candidates[t].at - value <= s_reach.object) s_holders[s_holderCount++] = {at, value, t};
		});
	});
}

void LogPaths(int32_t target, int32_t maxPaths) {
	auto exe = (uintptr_t)GetModuleHandleW(nullptr);
	uintptr_t at = s_candidates[target].at, offset;
	int32_t logged = 0;
	for (int32_t h = 0; h < s_holderCount && logged < maxPaths; h++) {
		const Holder& holder = s_holders[h];
		if (holder.target != target || !InModule(holder.at, &offset)) continue;
		Log("position finder:     [NieRAutomata.exe+0x%llx] +0x%llx", (unsigned long long)offset, (unsigned long long)(at - holder.value));
		logged++;
	}
	// two steps: the exe points to the object holding the pointer (one pass over the exe's data)
	uintptr_t lowest = UINTPTR_MAX, highest = 0;
	for (int32_t h = 0; h < s_holderCount; h++)
		if (s_holders[h].target == target) {
			if (s_holders[h].at < lowest) lowest = s_holders[h].at;
			if (s_holders[h].at > highest) highest = s_holders[h].at;
		}
	ForEachExeDataSection([&](uintptr_t from, size_t size) {
		ForEachPointer(from, size, [&](uintptr_t ptrAt, uint64_t value) {
			if (logged >= maxPaths || value > highest || value + s_reach.holder < lowest) return;
			for (int32_t h = 0; h < s_holderCount && logged < maxPaths; h++) {
				const Holder& holder = s_holders[h];
				if (holder.target != target || value > holder.at || holder.at - value > s_reach.holder || InModule(holder.at, &offset)) continue;
				Log("position finder:     [[NieRAutomata.exe+0x%llx] +0x%llx] +0x%llx", (unsigned long long)(ptrAt - exe),
					(unsigned long long)(holder.at - value), (unsigned long long)(at - holder.value));
				logged++;
			}
		});
	});
	if (!logged) Log("position finder:     no path from the exe's memory in two steps");
}

constexpr int32_t kShown = 16;  // candidates listed, and tested one at a time

void Report(pure::V3 feet) {
	Log("position finder: she moved %.1f m; %d places followed her the whole time%s", (double)s_walked, s_count,
		s_count > kShown ? " (the first 16 below)" : "");
	int32_t shown = s_count < kShown ? s_count : kShown;
	double startMs = NowMs();
	s_reach = kWide;
	FindHolders(0, shown);
	Log("position finder: %d pointers lead near them (%.0f ms); paths from the exe ([x] = the pointer at x, then + offset):", s_holderCount,
		NowMs() - startMs);
	for (int32_t i = 0; i < shown; i++) {
		const Candidate& c = s_candidates[i];
		pure::V3 now = {};
		ReadPosition(c, &now);
		const char* layout = c.stride == 1 ? "x y z" : "matrix";
		uintptr_t offset;
		if (InModule(c.at, &offset))
			Log("position finder:   %d: NieRAutomata.exe+0x%llx (%s) = %.3f %.3f %.3f (her feet %.3f %.3f %.3f)", i + 1, (unsigned long long)offset,
				layout, (double)now.x, (double)now.y, (double)now.z, (double)feet.x, (double)feet.y, (double)feet.z);
		else {
			MEMORY_BASIC_INFORMATION region = {};
			VirtualQuery((const void*)c.at, &region, sizeof(region));
			Log("position finder:   %d: %llx (+0x%llx in a %llu KB block, %s) = %.3f %.3f %.3f (her feet %.3f %.3f %.3f)", i + 1,
				(unsigned long long)c.at, (unsigned long long)(c.at - (uintptr_t)region.AllocationBase), (unsigned long long)(region.RegionSize >> 10),
				layout, (double)now.x, (double)now.y, (double)now.z, (double)feet.x, (double)feet.y, (double)feet.z);
		}
		LogPaths(i, 4);
	}
}
bool Pressed(int vk, bool ctrl, bool* wasDown) {
	bool down = (GetAsyncKeyState(vk) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) && ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) == ctrl;
	bool pressed = down && !*wasDown;
	*wasDown = down;
	if (!pressed) return false;
	HWND window = GetForegroundWindow();
	DWORD pid = 0;
	if (window) GetWindowThreadProcessId(window, &pid);
	return pid == GetCurrentProcessId();
}

// After a write watch: where the writes came from become the first candidates (side by side, offset
// from her as they are now), and the next test pushes them all again.
void AddSources(pure::V3 feet) {
	uintptr_t sources[8];
	int32_t added = WriteWatchSources(sources, (int32_t)std::size(sources));
	if (!added) return;
	int32_t keep = s_count + added <= (int32_t)std::size(s_candidates) ? s_count : (int32_t)std::size(s_candidates) - added;
	memmove(&s_candidates[added], &s_candidates[0], (size_t)keep * sizeof(Candidate));
	for (int32_t i = 0; i < added; i++) {
		s_candidates[i] = {sources[i], 1, {}};
		pure::V3 now;
		if (ReadPosition(s_candidates[i], &now)) s_candidates[i].offset = now - feet;
		Log("position finder: %d: %llx (where the writes came from) = %.3f %.3f %.3f", i + 1, (unsigned long long)sources[i], (double)now.x,
			(double)now.y, (double)now.z);
	}
	s_count = keep + added;
	s_nudgeNext = kAll;
	Log("position finder: %d places added from the write watch (now 1 to %d); Ctrl+Shift+F11 tests all again, then one by one", added, added);
}

// Pushes candidate `i` kNudge along x, if it still holds her position.
bool Push(int32_t i, pure::V3 feet) {
	const Candidate& c = s_candidates[i];
	pure::V3 now;
	if (!ReadPosition(c, &now) || pure::length(now - feet - c.offset) >= kKeepsOffset) return false;
	float x = now.x + kNudge;
	SIZE_T wrote = 0;
	return WriteProcessMemory(GetCurrentProcess(), (void*)c.at, &x, 4, &wrote) && wrote == 4;
}

// The first test pushes every survivor, the next ones one at a time.
void Nudge(pure::V3 feet) {
	int32_t tested = s_count < kShown ? s_count : kShown;
	if (s_running || tested == 0) {
		Log("position finder: nothing to test yet (Shift+F11 first, and let it finish)");
		return;
	}
	if (s_nudgeNext == kAll) {
		int32_t pushed = 0;
		for (int32_t i = 0; i < s_count; i++) pushed += Push(i, feet) ? 1 : 0;
		Log("position finder: test all: pushed %d of the %d places %.1f m along x", pushed, s_count, (double)kNudge);
		s_nudged = kAll;
		s_nudgeNext = 0;
	} else {
		int32_t i = s_nudgeNext;
		s_nudgeNext = (s_nudgeNext + 1) % tested;
		if (!Push(i, feet)) {
			Log("position finder: test %d of %d: it doesn't hold her position any more: not touched", i + 1, tested);
			return;
		}
		Log("position finder: test %d of %d: pushed %llx %.1f m along x", i + 1, tested, (unsigned long long)s_candidates[i].at, (double)kNudge);
		s_nudged = i;
	}
	s_nudgeTick = GetTickCount();
	s_nudgeFeet = feet;
}

void WatchNudge(pure::V3 feet) {
	if (s_nudged == kNone || GetTickCount() - s_nudgeTick < kNudgeWatchMs) return;
	float moved = feet.x - s_nudgeFeet.x;
	bool followed = fabsf(moved - kNudge) < 0.15f;
	char which[16] = "all";
	if (s_nudged != kAll) wsprintfA(which, "%d", (int)(s_nudged + 1));
	Log("position finder: test %s: her feet moved %.2f m along x (%.2f m in all)%s", which, (double)moved, (double)pure::length(feet - s_nudgeFeet),
		followed ? ": SHE MOVED WITH IT" : ": she didn't move with it");
	if (s_nudged >= 0 && followed) {  // her position: the paths to her object, tight
		const Candidate& c = s_candidates[s_nudged];
		s_reach = kTight;
		FindHolders(s_nudged, 1);
		Log("position finder: her position is %llx; paths to her object (it starts at most 0x%llx before it; [x] = the pointer at x):",
			(unsigned long long)c.at, (unsigned long long)s_reach.object);
		LogPaths(s_nudged, 12);
	}
	if (s_nudged == kAll && !followed) {  // the game rebuilds them all: who writes them, and from where?
		uintptr_t watched[4];
		int32_t count = s_count < 4 ? s_count : 4;
		for (int32_t i = 0; i < count; i++) watched[i] = s_candidates[i].at;
		Log("position finder: watching what writes places 1 to %d", count);
		WriteWatchStart(watched, count, 1000);
	}
	s_nudged = kNone;
}
}  // namespace

void PositionFinderPoll() {
	int vk = g_cfg.probe ? g_cfg.keyCapture : 0;
	if (vk <= 0 || vk >= 256) return;
	bool scan = Pressed(vk, false, &s_scanKeyWasDown), nudge = Pressed(vk, true, &s_nudgeKeyWasDown);
	pure::V3 feet;
	bool known = FirstPersonFeet(&feet);
	if (WriteWatchPoll() && known) AddSources(feet);
	if ((scan || nudge) && !known) {
		Log("position finder: 2B's bones aren't found right now (play as her, in first person)");
		return;
	}
	if (nudge) {
		Nudge(feet);
		return;
	}
	if (known) WatchNudge(feet);
	if (scan) {
		Scan(feet);
		s_running = s_count > 0;
		s_startTick = s_checkTick = GetTickCount();
		s_last = feet;
		s_walked = 0;
		s_nudgeNext = kAll;
		s_nudged = kNone;
		return;
	}
	if (!s_running || !known || GetTickCount() - s_checkTick < kCheckEveryMs) return;
	s_checkTick = GetTickCount();
	s_walked += pure::length(feet - s_last);
	s_last = feet;
	Narrow(feet);
	if (s_count == 0 || s_walked >= kEnoughWalking || GetTickCount() - s_startTick > kGiveUpMs) {
		s_running = false;
		Report(feet);
		if (s_count) Log("position finder: now stand still and press Ctrl+Shift+F11: first all of them at once, then one by one");
	}
}
