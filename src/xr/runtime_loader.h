// runtime_loader.h - finds the OpenXR runtime (runtime_loader.cpp).
#pragma once
#include "xr/openxr.h"

// Finds xrGetInstanceProcAddr: from the official OpenXR loader if one is provided next to the mod
// (or via [OpenXR] LoaderPath), otherwise directly from the system's active OpenXR runtime.
PFN_xrGetInstanceProcAddr XrLoadEntry();
