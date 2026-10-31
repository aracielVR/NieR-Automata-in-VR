// state_diff.h - [Debug] F2 / F4 (Ctrl+F2 clears): snapshots the exe's globals during scripted scenes and normal play, and logs the bytes that tell them apart.
#pragma once

void StateDiffPoll();  // each game frame (Present): reads the hotkeys
