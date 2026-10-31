// module.h - NieRVR_debug.dll's side of d3d11.dll's services (module.cpp).
#pragma once

bool GameIsForeground();
void RequestCapture(const char* why);  // a frame capture taken by the mod itself (d3d11.dll's CaptureRequest)
