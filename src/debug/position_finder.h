// position_finder.h - [Debug] Shift+F11 finds where the game keeps 2B's position in memory, Ctrl+Shift+F11 tests what it found.
#pragma once

void PositionFinderPoll();  // each game frame (Present): reads the hotkeys, narrows a running search, watches a test
