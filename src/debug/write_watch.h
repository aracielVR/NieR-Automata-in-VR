// write_watch.h - [Debug] which instructions write a few addresses (hardware write breakpoints).
#pragma once
#include <stdint.h>
#include <windows.h>

void WriteWatchStart(const uintptr_t* addresses, int32_t count, DWORD ms);  // up to 4 addresses, on every thread
bool WriteWatchPoll();  // each game frame: logs and stops when the time is up (true: it just did)
// After a watch: addresses (a register + 0x50) that held something like the watched value, where the writes came from?
int32_t WriteWatchSources(uintptr_t* out, int32_t max);
