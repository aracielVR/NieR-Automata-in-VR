// write_watch.cpp - [Debug] which instructions write a few addresses (hardware write breakpoints, like Cheat Engine's "find out what writes to this address").
#include "core/hook.h"
#include "debug/write_watch.h"
#include "core/log.h"
#include <atomic>
#include <iterator>
#include <math.h>
#include <stdint.h>
#include <tlhelp32.h>
#include <windows.h>

namespace {
bool ReadOwn(uintptr_t at, void* to, size_t size) {
	SIZE_T got = 0;
	return ReadProcessMemory(GetCurrentProcess(), (const void*)at, to, size, &got) && got == size;
}

struct Hit {
	std::atomic<uint64_t> rip;  // 0: free
	std::atomic<uint64_t> caller;  // [rsp] at the hit (for a helper without a stack frame: who called it)
	std::atomic<uint32_t> count;
	uint32_t watched;  // which address (0-3)
	uint64_t regs[16];  // rax rcx rdx rbx rsp rbp rsi rdi r8-r15 at the first hit
};
Hit s_hits[64];
uintptr_t s_watched[4];
int32_t s_watchedCount;
std::atomic<bool> s_active;
DWORD s_untilTick;
PVOID s_handler;
uintptr_t s_sources[8];
int32_t s_sourceCount;
bool s_justFinished;

LONG CALLBACK OnException(EXCEPTION_POINTERS* info) {
	if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !s_active.load()) return EXCEPTION_CONTINUE_SEARCH;
	CONTEXT* c = info->ContextRecord;
	uint64_t which = c->Dr6 & 0xF;
	if (!which) return EXCEPTION_CONTINUE_SEARCH;  // a real single step, not ours
	c->Dr6 = 0;
	uint64_t caller = *(const uint64_t*)c->Rsp;  // (the stack is always readable)
	for (auto& hit : s_hits) {
		uint64_t expected = 0;
		if ((hit.rip.load() == c->Rip && hit.caller.load() == caller) || hit.rip.compare_exchange_strong(expected, c->Rip)) {
			if (expected == 0 && hit.count.load() == 0) hit.caller.store(caller);
			if (hit.count.fetch_add(1) == 0) {
				hit.watched = which & 1 ? 0 : which & 2 ? 1
										  : which & 4   ? 2
														: 3;
				const uint64_t regs[16] = {c->Rax, c->Rcx, c->Rdx, c->Rbx, c->Rsp, c->Rbp, c->Rsi, c->Rdi,
										   c->R8, c->R9, c->R10, c->R11, c->R12, c->R13, c->R14, c->R15};
				for (int i = 0; i < 16; i++) hit.regs[i] = regs[i];
			}
			break;
		}
	}
	return EXCEPTION_CONTINUE_EXECUTION;
}

// Sets (or clears, with no addresses) the debug registers of every thread but the calling one.
DWORD WINAPI SetAllThreads(LPVOID param) {
	bool on = param != nullptr;
	DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
	ThreadSuspendLock();  // never at the same time as another thread suspender (core/hook.h)
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	if (snapshot == INVALID_HANDLE_VALUE) {
		ThreadSuspendUnlock();
		return 0;
	}
	THREADENTRY32 entry = {sizeof(entry)};
	for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
		if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == self) continue;
		HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, entry.th32ThreadID);
		if (!thread) continue;
		if (SuspendThread(thread) != (DWORD)-1) {
			CONTEXT c = {};
			c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
			if (GetThreadContext(thread, &c)) {
				uint64_t dr7 = 0;
				uint64_t* drs[4] = {&c.Dr0, &c.Dr1, &c.Dr2, &c.Dr3};
				for (int32_t i = 0; i < 4; i++) {
					*drs[i] = on && i < s_watchedCount ? s_watched[i] : 0;
					if (on && i < s_watchedCount) dr7 |= (1ull << (2 * i)) | (1ull << (16 + 4 * i)) | (3ull << (18 + 4 * i));  // local, write, 4 bytes
				}
				c.Dr7 = dr7;
				SetThreadContext(thread, &c);
			}
			ResumeThread(thread);
		}
		CloseHandle(thread);
	}
	CloseHandle(snapshot);
	ThreadSuspendUnlock();
	return 0;
}

void RunOnHelper(bool on) {
	HANDLE helper = CreateThread(nullptr, 0, SetAllThreads, on ? (LPVOID)1 : nullptr, 0, nullptr);
	if (!helper) return;
	WaitForSingleObject(helper, 5000);
	CloseHandle(helper);
}

void Finish() {
	RunOnHelper(false);
	s_active.store(false);
	s_sourceCount = 0;
	s_justFinished = true;
	auto exe = (uintptr_t)GetModuleHandleW(nullptr);
	int32_t found = 0;
	for (auto& hit : s_hits) {
		uint64_t rip = hit.rip.load();
		if (!rip) continue;
		found++;
		const uint64_t* r = hit.regs;
		Log("write watch: the instruction before NieRAutomata.exe+0x%llx (%llx), called from NieRAutomata.exe+0x%llx, wrote #%u %u times; "
			"first time rax %llx rcx %llx rdx %llx "
			"rbx %llx rsp %llx rbp %llx rsi %llx rdi %llx r8 %llx r9 %llx r10 %llx r11 %llx r12 %llx r13 %llx r14 %llx r15 %llx",
			(unsigned long long)(rip - exe), (unsigned long long)rip, (unsigned long long)(hit.caller.load() - exe), hit.watched + 1,
			hit.count.load(), r[0], r[1], r[2], r[3], r[4], r[5],
			r[6], r[7], r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15]);
		// registers pointing at an object whose +0x50 holds something like the watched value (an object's position?)
		float watchedX;
		if (!ReadOwn(s_watched[hit.watched], &watchedX, 4)) continue;
		static const char* const kNames[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"};
		for (int32_t i = 0; i < 16; i++) {
			float at50[3];
			if (r[i] < 0x10000 || !ReadOwn((uintptr_t)r[i] + 0x50, at50, sizeof(at50)) || !(fabsf(at50[0] - watchedX) < 2)) continue;
			Log("write watch:     %s+0x50 = %.3f %.3f %.3f now (#%u's x is %.3f)", kNames[i], (double)at50[0], (double)at50[1], (double)at50[2],
				hit.watched + 1, (double)watchedX);
			uintptr_t source = (uintptr_t)r[i] + 0x50;
			bool known = false;
			for (int32_t k = 0; k < s_sourceCount; k++) known = known || s_sources[k] == source;
			if (!known && s_sourceCount < (int32_t)std::size(s_sources)) s_sources[s_sourceCount++] = source;
		}
	}
	if (!found) Log("write watch: nothing wrote them in that time");
}
}  // namespace

void WriteWatchStart(const uintptr_t* addresses, int32_t count, DWORD ms) {
	if (s_active.load()) return;
	for (auto& hit : s_hits) {
		hit.rip.store(0);
		hit.caller.store(0);
		hit.count.store(0);
	}
	s_watchedCount = count < 4 ? count : 4;
	for (int32_t i = 0; i < s_watchedCount; i++) s_watched[i] = addresses[i] & ~(uintptr_t)3;
	if (!s_handler) s_handler = AddVectoredExceptionHandler(1, OnException);
	s_active.store(true);
	RunOnHelper(true);
	s_untilTick = GetTickCount() + ms;
	Log("write watch: watching %d addresses for %u ms", s_watchedCount, (unsigned)ms);
}

bool WriteWatchPoll() {
	if (s_active.load() && (LONG)(GetTickCount() - s_untilTick) >= 0) Finish();
	bool finished = s_justFinished;
	s_justFinished = false;
	return finished;
}

int32_t WriteWatchSources(uintptr_t* out, int32_t max) {
	int32_t count = s_sourceCount < max ? s_sourceCount : max;
	for (int32_t i = 0; i < count; i++) out[i] = s_sources[i];
	return count;
}
