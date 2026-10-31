// locks.h - scoped locks: the lock is held from the guard's line to the end of its { } block.
#pragma once
#include <windows.h>

struct ExclusiveLock {  // SRWLOCK, exclusive (writers)
	SRWLOCK* lock;
	explicit ExclusiveLock(SRWLOCK* l) : lock(l) { AcquireSRWLockExclusive(lock); }
	~ExclusiveLock() { ReleaseSRWLockExclusive(lock); }
	ExclusiveLock(const ExclusiveLock&) = delete;
	ExclusiveLock& operator=(const ExclusiveLock&) = delete;
};

struct SharedLock {  // SRWLOCK, shared (readers)
	SRWLOCK* lock;
	explicit SharedLock(SRWLOCK* l) : lock(l) { AcquireSRWLockShared(lock); }
	~SharedLock() { ReleaseSRWLockShared(lock); }
	SharedLock(const SharedLock&) = delete;
	SharedLock& operator=(const SharedLock&) = delete;
};

struct SectionLock {  // CRITICAL_SECTION
	CRITICAL_SECTION* section;
	explicit SectionLock(CRITICAL_SECTION* s) : section(s) { EnterCriticalSection(section); }
	~SectionLock() { LeaveCriticalSection(section); }
	SectionLock(const SectionLock&) = delete;
	SectionLock& operator=(const SectionLock&) = delete;
};
