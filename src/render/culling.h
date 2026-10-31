// culling.h - widens the field of view NieR culls its scene with (culling.cpp).
#pragma once

void CullingInstallHook();  // from EnsureInit: hooks the game camera's fov setter
void CullingUpdate(bool wide);
void CullingObserveGameFov(float deg);
float CullingGameFov();  // the field of view (deg) the game last asked for, before widening
// First person: the game camera's last look-at (eye, target), from the frame whose eye is at camPos
// if there is one (*matched), else the newest. False if none in the last 250 ms.
bool CullingLookAt(const float camPos[3], float eye[3], float target[3], bool* matched);
void CullingToggleHeadCulling();  // Ctrl+Shift+F5: head-direction culling on/off (saved)
void CullingToggleHeadShadows();  // Ctrl+Shift+F4: sun shadows fitted to the head on/off (saved)
