// com.h - COM references: ComPtr for local temporaries, SafeRelease for long-lived raw pointers (globals stay raw).
#pragma once
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

template <typename T>
void SafeRelease(T*& p) {
	if (p) p->Release();
	p = nullptr;
}
