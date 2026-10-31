// state_diff.cpp - [Debug] F2 / F4 (Ctrl+F2 clears): snapshots the exe's globals during scripted scenes and normal play, and logs the bytes that tell them apart.
#include "hot/state_diff.h"
#include "core/config.h"
#include "core/log.h"
#include "core/safe_memory.h"
#include "hot/module.h"
#include "hot/snapshot_diff.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <vector>
#include <windows.h>

namespace {
constexpr int32_t kScriptedKey = VK_F2, kPlayableKey = VK_F4;
constexpr uint32_t kPage = 4096;
constexpr size_t kMaxRunsLogged = 40;
constexpr uint32_t kMaxBytesShown = 8;

struct Range {
	uint32_t offset, size;  // in the exe
};
std::vector<Range> s_ranges;
uint32_t s_snapshotSize;
pure::SnapshotDiff s_diff;
bool s_scriptedWasDown, s_playableWasDown;

uintptr_t Exe() { return (uintptr_t)GetModuleHandleW(nullptr); }

// The exe's writable sections (.data, .bss): where its globals are
void FindRanges() {
	const uint8_t* base = (const uint8_t*)Exe();
	auto* nt = (const IMAGE_NT_HEADERS64*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
	const IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
	for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++) {
		if (!(sections[i].Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
		s_ranges.push_back({sections[i].VirtualAddress, sections[i].Misc.VirtualSize});
		s_snapshotSize += sections[i].Misc.VirtualSize;
		char name[9] = {};
		memcpy(name, sections[i].Name, 8);
		Log("state diff: watching %s at exe+0x%X (%u KB)", name, (unsigned)sections[i].VirtualAddress, (unsigned)(sections[i].Misc.VirtualSize >> 10));
	}
}

// Unreadable pages stay zero
std::vector<uint8_t> Snapshot() {
	std::vector<uint8_t> bytes(s_snapshotSize);
	uint32_t at = 0;
	for (const Range& range : s_ranges) {
		for (uint32_t pos = 0; pos < range.size; pos += kPage) {
			uint32_t size = range.size - pos < kPage ? range.size - pos : kPage;
			if (!SafeRead(Exe() + range.offset + pos, &bytes[at + pos], size)) memset(&bytes[at + pos], 0, size);
		}
		at += range.size;
	}
	return bytes;
}

uint32_t ExeOffset(uint32_t at) {
	for (const Range& range : s_ranges) {
		if (at < range.size) return range.offset + at;
		at -= range.size;
	}
	return 0;
}

void HexBytes(char* out, size_t outSize, const std::vector<uint8_t>& bytes, const pure::SnapshotDiff::Run& run) {
	out[0] = 0;
	uint32_t shown = run.size < kMaxBytesShown ? run.size : kMaxBytesShown;
	size_t used = 0;
	for (uint32_t i = 0; i < shown && used + 4 < outSize; i++) used += (size_t)snprintf(out + used, outSize - used, "%s%02X", i ? " " : "", bytes[run.at + i]);
	if (shown < run.size && used + 4 < outSize) snprintf(out + used, outSize - used, " ..");
}

// "scripted 6, playable 2": the snapshots (numbered from 1, as logged when taken) that disagree
void MissList(char* out, size_t outSize, const pure::SnapshotDiff::Run& run) {
	out[0] = 0;
	size_t used = 0;
	const uint64_t masks[2] = {run.scriptedMisses, run.playableMisses};
	for (int32_t kind = 0; kind < 2; kind++)
		for (uint32_t n = 0; n < pure::SnapshotDiff::kMaxSnapshots && used + 16 < outSize; n++)
			if (masks[kind] >> n & 1) used += (size_t)snprintf(out + used, outSize - used, "%s%s %u", used ? ", " : "", kind ? "playable" : "scripted", (unsigned)n + 1);
}

void LogCandidates() {
	auto runs = s_diff.find();
	uint32_t exact = 0;
	for (const auto& run : runs) exact += !run.misses;
	Log("state diff: %zu scripted, %zu playable snapshots: %zu candidates, %u matching every snapshot (up to %u misses allowed)", s_diff.scripted.size(),
		s_diff.playable.size(), runs.size(), (unsigned)exact, (unsigned)s_diff.allowedMisses());
	for (size_t i = 0; i < runs.size(); i++) {
		if (i == kMaxRunsLogged) {
			Log("state diff: (%zu more not shown: take more snapshots to narrow it down)", runs.size() - i);
			return;
		}
		const auto& run = runs[i];
		char scripted[40], playable[40], misses[128];
		HexBytes(scripted, sizeof(scripted), s_diff.scripted[run.scriptedSample], run);
		HexBytes(playable, sizeof(playable), s_diff.playable[run.playableSample], run);
		MissList(misses, sizeof(misses), run);
		Log("state diff:   exe+0x%X (%u bytes): scripted %s, playable %s%s%s%s", (unsigned)ExeOffset(run.at), (unsigned)run.size, scripted, playable,
			run.steady ? "" : " (varies)", run.misses ? "; misses: " : "", misses);
	}
}

void Take(bool scripted) {
	if (s_ranges.empty()) FindRanges();
	if (!s_diff.add(scripted, Snapshot())) {
		Log("state diff: %u %s snapshots is the most: Ctrl+F2 clears them", (unsigned)pure::SnapshotDiff::kMaxSnapshots, scripted ? "scripted" : "playable");
		return;
	}
	if (s_diff.scripted.empty() || s_diff.playable.empty())
		Log("state diff: %s snapshot %zu taken; take a %s one too (%s)", scripted ? "scripted" : "playable",
			scripted ? s_diff.scripted.size() : s_diff.playable.size(), scripted ? "playable" : "scripted", scripted ? "F4" : "F2");
	else
		LogCandidates();
}

bool Pressed(int32_t vk, bool* wasDown) {
	bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
	bool pressed = down && !*wasDown;
	*wasDown = down;
	return pressed;
}
}  // namespace

void StateDiffPoll() {
	if (!g_cfg.probe) return;
	bool scripted = Pressed(kScriptedKey, &s_scriptedWasDown), playable = Pressed(kPlayableKey, &s_playableWasDown);
	if (!(scripted || playable) || !GameIsForeground()) return;
	if (scripted && (GetAsyncKeyState(VK_CONTROL) & 0x8000)) {
		s_diff = {};
		Log("state diff: snapshots cleared");
		return;
	}
	Take(scripted);
}
