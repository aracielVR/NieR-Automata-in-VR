// safe_memory.h - reading and writing the game's memory without crashing on freed pages (Read/WriteProcessMemory on our own process).
#pragma once
#include <stdint.h>
#include <windows.h>

inline bool SafeRead(uintptr_t at, void* to, size_t size) {
	SIZE_T got = 0;
	return ReadProcessMemory(GetCurrentProcess(), (const void*)at, to, size, &got) && got == size;
}

inline bool SafeWrite(uintptr_t at, const void* from, size_t size) {
	SIZE_T wrote = 0;
	return WriteProcessMemory(GetCurrentProcess(), (void*)at, from, size, &wrote) && wrote == size;
}
