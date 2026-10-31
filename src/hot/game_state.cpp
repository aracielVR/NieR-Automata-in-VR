// game_state.cpp - what the game is doing (cutscene, loading), read from its memory with the NieR:Automata autosplitter's offsets.
#include "hot/game_state.h"
#include "core/log.h"
#include "hot/module.h"
#include "core/safe_memory.h"
#include <stdint.h>
#include <string.h>
#include <windows.h>

namespace {
// Offsets from NieRAutomata.exe, by build, from the NieR:Automata autosplitter:
// https://github.com/rodg/drakenier-splitters (nier_automata_autosplitter.asl)
struct Build {
	const char* name;
	uint32_t imageSize;  // NieRAutomata.exe's size in memory: tells the builds apart
	uintptr_t worldLoaded, loading, cutscenePlaying;
	uintptr_t cutsceneNamePointer, cutsceneNameOffset;  // [exe + pointer] + offset: up to 32 characters
	// The camera's fields (found with state_diff.cpp; 0: not known for this build)
	uintptr_t scriptedCamera;  // camera +0xB4: non-zero while something other than the follow camera has it
	uintptr_t followReady;     // camera +0x398: 1 once the follow camera is set up, 0 after something else took over
	// What makes the camera's tick skip a frame (FUN_00725100 in 1.02), as (exe offset, bits)
	uintptr_t cameraPause[2][2];  // flags words: the tick is skipped while a bit is set
	uintptr_t phase, phaseStep;   // the tick is also skipped in phase 5 at step 6 or below
};
constexpr int32_t kPausedPhase = 5, kLastPausedStep = 6;
constexpr Build kBuilds[] = {
	{"1.0", 113471488, 0x18F3978, 0x18DD444, 0x146A1AC, 0x1978868, 0x1F4},
	{"1.01", 106266624, 0x110ADC0, 0x147BF50, 0x1483974, 0x19925E8, 0x1F4},
	{"1.02", 26177536, 0xF6E240, 0x14005F4, 0x102A244, 0x17805F0, 0x294, 0x1020924, 0x1020C08, {{0x102987C, 0x200}, {0x10297C0, 0x800}}, 0x11EA8AC, 0x11EA8C8},
	{"Become as Gods (2021)", 26476544, 0xFE7F20, 0x11435C0, 0xFA54E8, 0x17FD648, 0x294},
};
constexpr int32_t kNameLength = 32;

struct State {
	bool worldLoaded, loading, cutscenePlaying, followReady;
	int32_t scriptedCamera;
	bool cameraPause[2];
	int32_t phase, phaseStep;
	char cutsceneName[kNameLength + 1];
};

bool s_checked;
const Build* s_build;
uintptr_t s_exe;
State s_last;

uint32_t ImageSize(uintptr_t exe) {
	const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)exe;
	const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(exe + dos->e_lfanew);
	return nt->OptionalHeader.SizeOfImage;
}

void FindBuild() {
	s_checked = true;
	s_exe = (uintptr_t)GetModuleHandleW(nullptr);
	uint32_t size = ImageSize(s_exe);
	for (const Build& build : kBuilds)
		if (build.imageSize == size) s_build = &build;
	if (s_build) Log("game state: build %s (exe %u bytes in memory): cutscenes and loading are logged", s_build->name, size);
	else Log("game state: unknown build (exe %u bytes in memory): no cutscene or loading state", size);
}

bool ReadFlag(uintptr_t offset) {
	uint8_t value = 0;
	return SafeRead(s_exe + offset, &value, sizeof(value)) && value;
}

int32_t ReadInt(uintptr_t offset) {
	int32_t value = 0;
	return offset && SafeRead(s_exe + offset, &value, sizeof(value)) ? value : 0;
}

// The cutscene's name, printable characters only ("" if it can't be read).
void ReadCutsceneName(char out[kNameLength + 1]) {
	out[0] = 0;
	uintptr_t names = 0;
	if (!SafeRead(s_exe + s_build->cutsceneNamePointer, &names, sizeof(names)) || !names) return;
	if (!SafeRead(names + s_build->cutsceneNameOffset, out, kNameLength)) {
		out[0] = 0;
		return;
	}
	out[kNameLength] = 0;
	for (int32_t i = 0; out[i]; i++)
		if (out[i] < 32 || out[i] > 126) out[i] = 0;
}

DWORD s_cutsceneStartTick;  // a 3D cutscene (not a film) started, not yet captured (NOTES A17)

void LogChanges(const State& now) {
	if (now.loading != s_last.loading) Log("game state: loading %s", now.loading ? "started" : "finished");
	if (now.worldLoaded != s_last.worldLoaded) Log("game state: world %s", now.worldLoaded ? "loaded" : "unloaded");
	if (now.cutscenePlaying != s_last.cutscenePlaying) {
		Log("game state: cutscene %s (%s)", now.cutscenePlaying ? "started" : "ended", now.cutsceneName[0] ? now.cutsceneName : "no name");
		s_cutsceneStartTick = now.cutscenePlaying && !strstr(now.cutsceneName, ".usm") ? GetTickCount() : 0;
	}
	// the bands may slide in: capture 1.5 s into it, if it is still playing
	if (s_cutsceneStartTick && now.cutscenePlaying && GetTickCount() - s_cutsceneStartTick > 1500) {
		s_cutsceneStartTick = 0;
		RequestCapture("1.5 s into a 3D cutscene");
	} else if (strcmp(now.cutsceneName, s_last.cutsceneName))
		Log("game state: cutscene name now %s%s", now.cutsceneName[0] ? now.cutsceneName : "(none)", now.cutscenePlaying ? " (playing)" : "");
	if (now.scriptedCamera != s_last.scriptedCamera) Log("game state: scripted camera %d -> %d", s_last.scriptedCamera, now.scriptedCamera);
	if (now.followReady != s_last.followReady) Log("game state: follow camera %s", now.followReady ? "ready" : "reset");
	for (int32_t i = 0; i < 2; i++)
		if (now.cameraPause[i] != s_last.cameraPause[i])
			Log("game state: camera pause bit 0x%X of exe+0x%X %s", (unsigned)s_build->cameraPause[i][1], (unsigned)s_build->cameraPause[i][0],
				now.cameraPause[i] ? "set" : "cleared");
	if (now.phase != s_last.phase || now.phaseStep != s_last.phaseStep)
		Log("game state: phase %d step %d%s", now.phase, now.phaseStep, now.phase == kPausedPhase && now.phaseStep <= kLastPausedStep ? " (camera paused)" : "");
}
}  // namespace

void GameStatePoll() {
	if (!s_checked) FindBuild();
	if (!s_build) return;
	State now = {};
	now.worldLoaded = ReadFlag(s_build->worldLoaded);
	now.loading = ReadFlag(s_build->loading);
	now.cutscenePlaying = ReadFlag(s_build->cutscenePlaying);
	now.scriptedCamera = ReadInt(s_build->scriptedCamera);
	now.followReady = ReadInt(s_build->followReady) != 0;
	for (int32_t i = 0; i < 2; i++) now.cameraPause[i] = (ReadInt(s_build->cameraPause[i][0]) & s_build->cameraPause[i][1]) != 0;
	now.phase = ReadInt(s_build->phase);
	now.phaseStep = ReadInt(s_build->phaseStep);
	ReadCutsceneName(now.cutsceneName);
	LogChanges(now);
	s_last = now;
}
