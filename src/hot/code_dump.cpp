// code_dump.cpp - [Debug] Ctrl+F11: writes the game's decrypted code to NieRVR_code_dump.exe.
#include "hot/code_dump.h"
#include "core/config.h"
#include "core/log.h"
#include "core/main.h"
#include <iterator>
#include <stdint.h>
#include <string.h>
#include <windows.h>

namespace {
constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
constexpr wchar_t kFileName[] = L"NieRVR_code_dump.exe";

bool s_keyWasDown;

// Copies what can be read of [from, from + size) to `to`; pages that can't be read stay zero.
void CopyReadable(uint8_t* to, const uint8_t* from, size_t size) {
	size_t pos = 0;
	while (pos < size) {
		MEMORY_BASIC_INFORMATION info;
		if (!VirtualQuery(from + pos, &info, sizeof(info))) break;
		size_t regionEnd = (size_t)((const uint8_t*)info.BaseAddress + info.RegionSize - from);
		if (regionEnd > size) regionEnd = size;
		bool readable = info.State == MEM_COMMIT && (info.Protect & kReadable) && !(info.Protect & PAGE_GUARD);
		if (readable) memcpy(to + pos, from + pos, regionEnd - pos);
		pos = regionEnd;
	}
}

void WriteDump() {
	const uint8_t* base = (const uint8_t*)GetModuleHandleW(nullptr);
	auto* nt = (const IMAGE_NT_HEADERS64*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
	DWORD size = nt->OptionalHeader.SizeOfImage;
	uint8_t* image = (uint8_t*)VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	if (!image) {
		Log("ERROR: code dump: could not allocate %u MB", (unsigned)(size >> 20));
		return;
	}
	CopyReadable(image, base, size);
	// each section's file offset becomes its address, and the base the one it's loaded at: addresses in
	// the disassembly are the running game's
	auto* copyNt = (IMAGE_NT_HEADERS64*)(image + ((IMAGE_DOS_HEADER*)image)->e_lfanew);
	copyNt->OptionalHeader.ImageBase = (ULONGLONG)(uintptr_t)base;
	IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(copyNt);
	for (WORD i = 0; i < copyNt->FileHeader.NumberOfSections; i++) {
		sections[i].PointerToRawData = sections[i].VirtualAddress;
		sections[i].SizeOfRawData = sections[i].Misc.VirtualSize;
	}

	wchar_t path[MAX_PATH];
	lstrcpynW(path, g_modDir, MAX_PATH - (int)std::size(kFileName));
	lstrcatW(path, kFileName);
	HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	DWORD written = 0;
	bool ok = file != INVALID_HANDLE_VALUE && WriteFile(file, image, size, &written, nullptr) && written == size;
	DWORD error = ok ? 0 : GetLastError();
	if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
	VirtualFree(image, 0, MEM_RELEASE);
	if (ok)
		Log("code dump: wrote NieRVR_code_dump.exe (%u MB, loaded at %p; NieRAutomata.exe+offsets in this log are its addresses minus that)",
			(unsigned)(size >> 20), (const void*)base);
	else
		Log("ERROR: code dump: cannot write NieRVR_code_dump.exe (error %u)", (unsigned)error);
}
}  // namespace

void CodeDumpPoll() {
	int vk = g_cfg.probe ? g_cfg.keyCapture : 0;
	if (vk <= 0 || vk >= 256) return;
	bool down = (GetAsyncKeyState(vk) & 0x8000) && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && !(GetAsyncKeyState(VK_SHIFT) & 0x8000);  // Ctrl+Shift: the position test
	if (down && !s_keyWasDown) {
		HWND window = GetForegroundWindow();
		DWORD pid = 0;
		if (window) GetWindowThreadProcessId(window, &pid);
		if (pid == GetCurrentProcessId()) WriteDump();
	}
	s_keyWasDown = down;
}
