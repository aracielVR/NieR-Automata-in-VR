// Player position and facing hooks.
#pragma once
#include "core/vecmath.h"

// Moves the character by `step` (world, horizontal) if their object is found and is where their bones are; false if not.
// `from`: where they were.
bool PlayerMove(pure::V3 step, pure::V3* from);
// The character's position (their object's, where their bones are); false if not found.
bool PlayerPosition(pure::V3* position);
// Finds the facing value and turns the character toward lookForward after an attack starts.
void PlayerUpdate(pure::V3 lookForward);
