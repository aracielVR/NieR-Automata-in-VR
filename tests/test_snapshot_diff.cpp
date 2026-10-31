// The snapshot diff (hot/snapshot_diff.h): bytes that tell scripted scenes from normal play.
#include <cstdio>
#include "hot/snapshot_diff.h"
static int fails;
static void check(bool c, const char* w) {
	if (!c) {
		std::printf("FAIL %s\n", w);
		fails++;
	}
}
int main() {
	using D = pure::SnapshotDiff;
	// byte 2: the flag (1 scripted, 0 playing); bytes 5-6: a two-byte state (7 7 scripted, varies playing);
	// byte 8: a timer; byte 9: matches the flag only sometimes
	auto snapshot = [](uint8_t flag, uint8_t state, uint8_t timer, uint8_t noise) {
		return std::vector<uint8_t>{0, 0, flag, 0, 0, state, state, 0, timer, noise};
	};
	{
		D diff;
		check(diff.find().empty(), "nothing without snapshots");
		diff.add(true, snapshot(1, 7, 10, 1));
		check(diff.find().empty(), "nothing with scripted snapshots only");
		diff.add(false, snapshot(0, 3, 20, 0));
		diff.add(true, snapshot(1, 7, 30, 1));
		diff.add(false, snapshot(0, 4, 40, 1));
		auto runs = diff.find();
		check(runs.size() == 2, "two runs");
		check(runs.size() > 0 && runs[0].at == 2 && runs[0].size == 1 && runs[0].steady, "the flag, steady");
		check(runs.size() > 1 && runs[1].at == 5 && runs[1].size == 2 && !runs[1].steady, "the state, joined, playable values vary");
	}
	{
		D diff;
		diff.add(true, {1, 2, 3});
		diff.add(false, {0, 0});
		auto runs = diff.find();
		check(runs.size() == 1 && runs[0].at == 0 && runs[0].size == 2, "shorter snapshots: only the shared bytes");
	}
	{
		// 6 scripted, 6 playable (two misses allowed); scripted snapshot 4 was really playable
		D diff;
		for (int i = 0; i < 6; i++) {
			diff.add(true, snapshot(i == 3 ? 0 : 1, 7, (uint8_t)i, 1));
			diff.add(false, snapshot(0, (uint8_t)(20 + i), (uint8_t)(10 + i), (uint8_t)(i & 1)));
		}
		check(diff.allowedMisses() == 2, "two misses allowed with 12 snapshots");
		auto runs = diff.find();
		bool flag = false, state = false;
		for (const auto& run : runs) {
			if (run.at == 2) flag = run.misses == 1 && run.scriptedMisses == 1u << 3 && !run.playableMisses && run.steady;
			if (run.at == 5) state = run.size == 2 && !run.misses && !run.steady;
			check(run.at != 9, "noise: misses too often");
		}
		check(flag, "the flag, missing only in the mislabelled snapshot");
		check(state, "the state, matching every snapshot");
		check(!runs.empty() && runs[0].misses == 0, "exact matches first");
	}
	if (!fails) std::printf("ok\n");
	return fails ? 1 : 0;
}
