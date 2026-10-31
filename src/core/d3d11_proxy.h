// d3d11_proxy.h - stands in for d3d11.dll: loads the real one and forwards every export (d3d11_proxy.cpp).
#pragma once
#include <d3d11.h>

bool ProxyLoadReal();  // called from EnsureInit
extern PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN g_realCreateDeviceAndSwapChain;
