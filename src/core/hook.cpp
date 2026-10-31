// hook.cpp - inline function hooks through MinHook, shared by every hook the mod installs.
#include "core/hook.h"
#include "core/log.h"
#include "core/main.h"
#include "MinHook.h"

namespace {
SRWLOCK s_suspendLock = SRWLOCK_INIT;
}
void ThreadSuspendLock() { AcquireSRWLockExclusive(&s_suspendLock); }
void ThreadSuspendUnlock() { ReleaseSRWLockExclusive(&s_suspendLock); }

bool HookFunction(void* target, void* detour, void** orig, const char* name) {
	static bool ready = false;
	ThreadSuspendLock();  // MH_EnableHook suspends the other threads
	if (!ready) {
		MH_STATUS st = MH_Initialize();
		ready = st == MH_OK || st == MH_ERROR_ALREADY_INITIALIZED;
		if (!ready) Log("ERROR: MinHook init failed (%d)", (int)st);
	}
	MH_STATUS st = ready ? MH_CreateHook(target, detour, orig) : MH_ERROR_NOT_INITIALIZED;
	if (st == MH_OK) st = MH_EnableHook(target);
	ThreadSuspendUnlock();
	if (st != MH_OK) {
		char where[300];
		DescribeAddress(target, where, sizeof(where));
		Log("ERROR: could not hook %s at %s (MinHook %d)", name, where, (int)st);
		return false;
	}
	return true;
}
