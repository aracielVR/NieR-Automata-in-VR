// Finds the bytes that hold one value in the "scripted" snapshots and a different one in the "playable" snapshots, ranked by how many snapshots disagree
#pragma once
#include <algorithm>
#include <stdint.h>
#include <utility>
#include <vector>

namespace pure {

struct SnapshotDiff {
	static constexpr uint32_t kMaxSnapshots = 64;          // of each kind: the misses are bit masks
	static constexpr uint32_t kSnapshotsPerMiss = 5;       // one miss allowed per this many snapshots...
	static constexpr uint32_t kMaxMisses = 2;              // ...up to this many
	struct Run {
		uint32_t at, size;                               // bytes into the snapshots
		uint32_t misses;                                 // snapshots that disagree
		uint64_t scriptedMisses, playableMisses;         // which ones (bit n: snapshot n)
		uint32_t scriptedSample, playableSample;         // snapshots that agree, to show the values from
		bool steady;                                     // the agreeing playable snapshots also agree with each other
	};
	std::vector<std::vector<uint8_t>> scripted, playable;

	// false: already kMaxSnapshots of that kind
	bool add(bool isScripted, std::vector<uint8_t> bytes) {
		auto& list = isScripted ? scripted : playable;
		if (list.size() >= kMaxSnapshots) return false;
		list.push_back(std::move(bytes));
		return true;
	}

	uint32_t allowedMisses() const { return std::min<uint32_t>(kMaxMisses, (uint32_t)(scripted.size() + playable.size()) / kSnapshotsPerMiss); }

	// Candidate bytes, fewest misses first, joined into runs of neighbours that miss in the same snapshots
	std::vector<Run> find() const {
		std::vector<Run> runs;
		if (scripted.empty() || playable.empty()) return runs;
		size_t size = scripted[0].size();
		for (const auto& bytes : scripted) size = std::min(size, bytes.size());
		for (const auto& bytes : playable) size = std::min(size, bytes.size());
		uint32_t allowed = allowedMisses();
		for (uint32_t i = 0; i < size; i++) {
			Run byte;
			if (!score(i, allowed, &byte)) continue;
			Run* last = runs.empty() ? nullptr : &runs.back();
			if (last && last->at + last->size == i && last->steady == byte.steady && last->scriptedMisses == byte.scriptedMisses &&
				last->playableMisses == byte.playableMisses)
				last->size++;
			else
				runs.push_back(byte);
		}
		std::stable_sort(runs.begin(), runs.end(), [](const Run& a, const Run& b) { return a.misses != b.misses ? a.misses < b.misses : a.steady > b.steady; });
		return runs;
	}

	// Byte i, as the scripted value that misses least. With up to `allowed` misses that value is one of the
	// first allowed + 1 scripted snapshots'.
	bool score(uint32_t i, uint32_t allowed, Run* out) const {
		bool found = false;
		uint32_t tries = std::min<uint32_t>(allowed + 1, (uint32_t)scripted.size());
		for (uint32_t t = 0; t < tries; t++) {
			uint8_t value = scripted[t][i];
			Run run = {i, 1, 0, 0, 0, t, 0, true};
			for (uint32_t s = 0; s < scripted.size() && run.misses <= allowed; s++)
				if (scripted[s][i] != value) {
					run.misses++;
					run.scriptedMisses |= 1ull << s;
				}
			int32_t firstAgreeing = -1;
			for (uint32_t p = 0; p < playable.size() && run.misses <= allowed; p++) {
				if (playable[p][i] == value) {
					run.misses++;
					run.playableMisses |= 1ull << p;
				} else if (firstAgreeing < 0) {
					firstAgreeing = (int32_t)p;
				} else if (playable[p][i] != playable[firstAgreeing][i]) {
					run.steady = false;
				}
			}
			if (run.misses > allowed || firstAgreeing < 0 || (found && run.misses >= out->misses)) continue;
			run.playableSample = (uint32_t)firstAgreeing;
			*out = run;
			found = true;
		}
		return found;
	}
};

}  // namespace pure
