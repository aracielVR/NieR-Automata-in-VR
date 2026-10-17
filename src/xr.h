// xr.h - OpenXR includes and runtime entry-point discovery.
#pragma once
#include "common.h"

#define XR_NO_PROTOTYPES
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>

// Finds xrGetInstanceProcAddr: from the official OpenXR loader if one is provided next to the mod
// (or via [OpenXR] LoaderPath), otherwise directly from the system's active OpenXR runtime.
PFN_xrGetInstanceProcAddr XrLoadEntry();

// vrinput.cpp
void VrInputInit(PFN_xrGetInstanceProcAddr gipa, XrInstance inst, bool pico);
void VrInputAttach(XrSession s);
