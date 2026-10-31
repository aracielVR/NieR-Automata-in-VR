// game_state.h - what the game is doing (cutscene, loading), read from its memory with the NieR:Automata autosplitter's offsets.
#pragma once

void GameStatePoll();  // each game frame (frame_handoff.cpp): reads the state and logs what changed
