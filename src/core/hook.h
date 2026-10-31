// hook.h - inline function hooks (MinHook).
#pragma once

bool HookFunction(void* target, void* detour, void** orig, const char* name);  // false: logged why

// Held by everything that suspends the other threads (MinHook's enable, the frame-rate code patches, the write
// watch): two of them suspending each other at the same moment would freeze the game.
void ThreadSuspendLock();
void ThreadSuspendUnlock();
