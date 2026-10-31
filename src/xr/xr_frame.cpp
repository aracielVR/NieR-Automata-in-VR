// xr_frame.cpp - one headset frame: hotkeys, screen placement, the eye poses handed to the game,
// and the layers submitted (stereo eyes, flat screen, HUD, stats overlay, settings menu).
#include "game/frame_rate.h"
#include "xr/xr_thread.h"
#include "core/com.h"
#include "core/locks.h"
#include "core/config.h"
#include "core/log.h"
#include "input/vr_controllers.h"
#include "render/camera.h"
#include "render/culling.h"
#include "render/replay.h"
#include "render/upscale.h"
#include "ui/menu.h"
#include <stdio.h>
#include <string.h>

// Screen placement & hotkeys
bool GameIsForeground() {
	HWND window = GetForegroundWindow();
	DWORD pid = 0;
	if (window) GetWindowThreadProcessId(window, &pid);
	return pid == GetCurrentProcessId();
}

bool KeyPressed(int vk) {
	if (vk <= 0 || vk > 255) return false;
	bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
	bool pressed = down && !g_vr.keyDown[vk];
	g_vr.keyDown[vk] = down;
	return pressed;
}

namespace {
// The game resolution with the headset's full detail, for the headset-shaped sizes in the game's
// resolution list (extra_resolutions.cpp) from the next start. Saved when it changes by over 2%.
void SaveHeadsetSize(float width, float height) {
	int32_t w = ((int32_t)width + 4) & ~7, h = ((int32_t)height + 4) & ~7;
	if (w < 640 || h < 480 || w > 16384 || h > 16384) return;
	auto within2Percent = [](int32_t a, int32_t b) { return a * 50 > b * 49 && a * 50 < b * 51; };
	if (within2Percent(w, g_cfg.headsetWidth) && within2Percent(h, g_cfg.headsetHeight)) return;
	g_cfg.headsetWidth = w;
	g_cfg.headsetHeight = h;
	ConfigSave(&g_cfg.headsetWidth);
	ConfigSave(&g_cfg.headsetHeight);
	Log("headset: %dx%d gives it full detail; saved, the game's resolution list offers 50-100%% of it from the next start", w, h);
}

void HandleHotkeys() {
	bool foreground = GameIsForeground();
	bool changed = false;
	// Always poll so edge detection stays correct, only act while the game has focus.
	bool recenter = KeyPressed(g_cfg.keyRecenter), smaller = KeyPressed(g_cfg.keySmaller), larger = KeyPressed(g_cfg.keyLarger),
		 closer = KeyPressed(g_cfg.keyCloser), farther = KeyPressed(g_cfg.keyFarther), toggleStereo = KeyPressed(g_cfg.keyStereo),
		 f5 = KeyPressed(VK_F5), f4 = KeyPressed(VK_F4), toggleFirstPerson = KeyPressed(g_cfg.keyFirstPerson);
	if (!foreground) return;
	if (f4 && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000))
		CullingToggleHeadShadows();  // Ctrl+Shift+F4: sun shadows follow the head on/off
	if (toggleFirstPerson) {
		g_cfg.firstPerson = !g_cfg.firstPerson;
		Log("first person %s", g_cfg.firstPerson ? "ON" : "OFF");
		ConfigSave(&g_cfg.firstPerson);
	}
	if (f5) {
		bool ctrlDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0, shiftDown = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
		if (ctrlDown && shiftDown) CullingToggleHeadCulling();  // Ctrl+Shift+F5: head-direction culling on/off
		else if (!ctrlDown && !shiftDown) {                     // F5: alternating -> synchronized -> AFW
			g_cfg.synchronized = (g_cfg.synchronized + 1) % 3;
			Log("stereo mode: %s", StereoAfw()             ? "AFW (one eye per game frame, the other built from it by its depth)"
								   : !StereoSynchronized() ? "alternating (one eye per game frame)"
								   : ReplayAvailable()     ? "synchronized (the game's frame is drawn again for the right eye)"
														   : "synchronized, but not available (see the log above)");
			ConfigSave(&g_cfg.synchronized);
		}
	}
	if (toggleStereo && (GetAsyncKeyState(VK_SHIFT) & 0x8000)) {
		// Shift+F12: TopDownFlat on/off (steep top-down views on the flat screen)
		g_cfg.topDownFlat = !g_cfg.topDownFlat;
		Log("top-down views: %s", g_cfg.topDownFlat ? "flat screen (TopDownFlat=1)" : "stereo (TopDownFlat=0)");
		ConfigSave(&g_cfg.topDownFlat);
		toggleStereo = false;
	}
	if (toggleStereo) {
		AcquireSRWLockExclusive(&g_stereo.lock);
		g_stereo.enabled = !g_stereo.enabled;
		bool on = g_stereo.enabled;
		ReleaseSRWLockExclusive(&g_stereo.lock);
		Log("stereo %s", on ? "ON" : "OFF (flat screen)");
	}
	if (recenter) {
		g_vr.recenterRequested = true;
		Log("recenter requested");
	}
	bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
	if (ctrl && (smaller || larger)) {
		// Ctrl+F9: world bigger (less eye separation per game unit), Ctrl+F10: world smaller; 10% per press
		float factor = smaller ? 1 / 1.1f : 1.1f;
		float value = g_cfg.worldScale * factor;
		g_cfg.worldScale = value < 0.002f ? 0.002f : (value > 50 ? 50 : value);
		Log("world scale %.3f (the world now looks %.2fx its true size)", (double)g_cfg.worldScale,
			(double)(1 / g_cfg.worldScale));
		ConfigSave(&g_cfg.worldScale);
		smaller = larger = false;
	}
	if (ctrl && (closer || farther)) {
		// Ctrl+F6: camera closer to the character, Ctrl+F7: farther
		float value = g_cfg.cameraDolly + (closer ? 0.25f : -0.25f);
		g_cfg.cameraDolly = value < -5 ? -5 : (value > 5 ? 5 : value);
		Log("camera dolly %.2f m", (double)g_cfg.cameraDolly);
		ConfigSave(&g_cfg.cameraDolly);
		closer = farther = false;
	}
	if (smaller) {
		g_cfg.width = g_cfg.width / 1.1f < 0.2f ? 0.2f : g_cfg.width / 1.1f;
		changed = true;
	}
	if (larger) {
		g_cfg.width = g_cfg.width * 1.1f > 100.f ? 100.f : g_cfg.width * 1.1f;
		changed = true;
	}
	if (closer) {
		g_cfg.distance = g_cfg.distance - 0.25f < 0.3f ? 0.3f : g_cfg.distance - 0.25f;
		changed = true;
		g_vr.recenterRequested = true;
	}
	if (farther) {
		g_cfg.distance = g_cfg.distance + 0.25f > 50.f ? 50.f : g_cfg.distance + 0.25f;
		changed = true;
		g_vr.recenterRequested = true;
	}
	if (changed) {
		Log("screen: distance %.2f m, width %.2f m", (double)g_cfg.distance, (double)g_cfg.width);
		ConfigSave(&g_cfg.distance);
		ConfigSave(&g_cfg.width);
	}
}

// The runtime may change its tracking space without the REFERENCE_SPACE_CHANGE_PENDING event the
// spec asks for (Virtual Desktop through SteamVR: the space went from upside down to normal after a
// dashboard visit, and the picture stayed upside down until a manual recenter). Each headset frame: a
// head whose up points against the reference's up for a third of a second, or a head position jump of
// more than 0.75 m between two frames, re-levels everything.
void WatchSpace() {
	if (!g_vr.screenPlaced || !g_vr.displayPoseValid) {
		g_vr.lastHeadValid = false;
		return;
	}
	const XrPosef& leftEye = g_vr.displayViews[0].pose;
	const XrPosef& rightEye = g_vr.displayViews[1].pose;
	float head[3] = {(leftEye.position.x + rightEye.position.x) * 0.5f, (leftEye.position.y + rightEye.position.y) * 0.5f,
					 (leftEye.position.z + rightEye.position.z) * 0.5f};
	pure::V3 up = pure::rotate({leftEye.orientation.x, leftEye.orientation.y, leftEye.orientation.z, leftEye.orientation.w}, {0, 1, 0});
	float along = g_vr.spaceFlipped ? -up.y : up.y;  // head up against the reference's up
	g_vr.upsideDownFrames = along < -0.5f ? g_vr.upsideDownFrames + 1 : 0;
	bool jump = false;
	if (g_vr.lastHeadValid) {
		float dx = head[0] - g_vr.lastHeadPos[0], dy = head[1] - g_vr.lastHeadPos[1], dz = head[2] - g_vr.lastHeadPos[2];
		jump = dx * dx + dy * dy + dz * dz > 0.75f * 0.75f;
	}
	memcpy(g_vr.lastHeadPos, head, sizeof(head));
	g_vr.lastHeadValid = true;
	if (g_vr.upsideDownFrames >= 30 || jump) {
		Log("tracking space changed without notice (%s); re-levelling the view and the screen",
			jump ? "the head jumped more than 0.75 m in one frame" : "the head's up now points the other way");
		g_vr.upsideDownFrames = 0;
		g_vr.recenterRequested = true;
	}
}

void PlaceScreen(XrTime time) {
	XrSpaceLocation location = {};
	location.type = XR_TYPE_SPACE_LOCATION;
	if (XR_FAILED(g_vr.xrLocateSpace(g_vr.viewSpace, g_vr.localSpace, time, &location))) return;
	const XrSpaceLocationFlags need = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
	if ((location.locationFlags & need) != need) return;
	pure::Q head{location.pose.orientation.x, location.pose.orientation.y, location.pose.orientation.z, location.pose.orientation.w};
	pure::Q yaw;  // level reference: heading only, upright for the player (pure::level_reference)
	bool wasFlipped = g_vr.spaceFlipped;
	if (!pure::level_reference(head, &g_vr.spaceFlipped, &yaw)) yaw = g_vr.screenPlaced ? g_vr.lastScreenYaw : pure::Q{0, 0, 0, 1};
	if (g_vr.spaceFlipped != wasFlipped || (!g_vr.screenPlaced && g_vr.spaceFlipped))
		Log(g_vr.spaceFlipped ? "tracking space: the runtime's up axis points down (its +Y is the floor); the view, screen "
								"and menu are turned upright for you"
							  : "tracking space: normal (+Y up)");
	pure::V3 offset = pure::rotate(yaw, {0, g_cfg.heightOffset, -g_cfg.distance});
	g_vr.quadPose.orientation = {yaw.x, yaw.y, yaw.z, yaw.w};
	g_vr.quadPose.position = {location.pose.position.x + offset.x, location.pose.position.y + offset.y, location.pose.position.z + offset.z};
	g_vr.lastScreenYaw = yaw;
	g_vr.screenPlaced = true;
	g_vr.recenterRequested = false;
	// The stereo camera treats this (yaw-only) head pose as "looking along the game camera".
	{
		ExclusiveLock guard(&g_stereo.lock);
		g_stereo.refPos[0] = location.pose.position.x;
		g_stereo.refPos[1] = location.pose.position.y;
		g_stereo.refPos[2] = location.pose.position.z;
		g_stereo.refRot[0] = yaw.x;
		g_stereo.refRot[1] = yaw.y;
		g_stereo.refRot[2] = yaw.z;
		g_stereo.refRot[3] = yaw.w;
		g_stereo.refValid = true;
	}
	pure::V3 headUp = pure::rotate(head, {0, 1, 0});
	Log("screen placed at (%.2f, %.2f, %.2f), %.2f m ahead; head up vector (%.2f, %.2f, %.2f)%s",
		(double)g_vr.quadPose.position.x, (double)g_vr.quadPose.position.y, (double)g_vr.quadPose.position.z,
		(double)g_cfg.distance, (double)headUp.x, (double)headUp.y, (double)headUp.z, g_vr.spaceFlipped ? " (space flipped)" : "");
}

void LogStats() {
	double now = NowMs();
	double secs = (now - g_vr.statStartMs) / 1000;
	if (secs < 10) return;
	uint32_t received = g_vr.statGameFrames[0] + g_vr.statGameFrames[1] + g_vr.statGameFrames[2];
	Log("stats: headset %.1f fps (stereo %.1f, flat %.1f); game frames received: left %.1f, right %.1f, mono %.1f fps; "
		"HUD-free image in %.0f%% of them; UI coverage avg %.0f%% max %.0f%%",
		g_vr.statFrames / secs, g_vr.statStereoFrames / secs, g_vr.statQuadFrames / secs, g_vr.statGameFrames[0] / secs,
		g_vr.statGameFrames[1] / secs, g_vr.statGameFrames[2] / secs, received ? 100.0 * g_vr.statSceneFrames / received : 0.0,
		g_vr.statCoverageCount ? 100.0 * g_vr.statCoverageSum / g_vr.statCoverageCount : 0.0, 100.0 * g_vr.statCoverageMax);
	{
		char info[160];
		snprintf(info, sizeof(info), "Mode: %s", !g_vr.statStereoFrames ? "flat screen" : (StereoAfw() ? "stereo, AFW" : StereoSynchronized() ? "stereo, synchronized"
																																			  : "stereo, alternating"));
		MenuSetInfo(INFO_MODE, info);
		snprintf(info, sizeof(info), "Headset %.0f fps; game images per second: left %.0f, right %.0f, flat %.0f",
				 g_vr.statFrames / secs, g_vr.statGameFrames[0] / secs, g_vr.statGameFrames[1] / secs, g_vr.statGameFrames[2] / secs);
		MenuSetInfo(INFO_HEADSET, info);
	}
	if (g_vr.statLagCount || g_vr.statTurnCount) {
		Log("stats: head pose: game images were on screen on average %.1f ms after the time their pose was predicted "
			"from (prediction now %.1f ms ahead, PosePrediction=%d); the head had turned on average %.2f deg (max %.1f) "
			"from the shown image's pose, beyond the rendered margin of %.1f deg in %.1f%% of headset frames",
			g_vr.statLagCount ? g_vr.statLagSum / g_vr.statLagCount : 0.0, (double)g_vr.predictionNs / 1e6, g_cfg.posePrediction,
			g_vr.statTurnCount ? g_vr.statTurnSum / g_vr.statTurnCount : 0.0, (double)g_vr.statTurnMax, (double)g_vr.marginDeg,
			g_vr.statTurnCount ? 100.0 * g_vr.statTurnOverMargin / g_vr.statTurnCount : 0.0);
		char info[160];
		snprintf(info, sizeof(info), "Head: images shown %.0f ms after their pose; predicting %.0f ms; past margin %.1f%%",
				 g_vr.statLagCount ? g_vr.statLagSum / g_vr.statLagCount : 0.0, (double)g_vr.predictionNs / 1e6,
				 g_vr.statTurnCount ? 100.0 * g_vr.statTurnOverMargin / g_vr.statTurnCount : 0.0);
		MenuSetInfo(INFO_HEAD_POSE, info);
	}
	static UINT resLoggedW, resLoggedH;
	if (g_vr.sharpnessPctX > 0 && g_vr.statStereoFrames && (resLoggedW != g_vr.gameW || resLoggedH != g_vr.gameH)) {
		resLoggedW = g_vr.gameW;
		resLoggedH = g_vr.gameH;
		Log("stats: resolution: the game renders %ux%u%s: %.0f%% of the sharpness the headset recommends across, %.0f%% "
			"up-down; about %ux%u matches it in both directions",
			g_vr.gameW, g_vr.gameH, g_vr.sceneW != g_vr.gameW ? " (upscaled)" : "", (double)g_vr.sharpnessPctX, (double)g_vr.sharpnessPctY,
			g_vr.suggestedGameW, g_vr.suggestedGameH);
		char info[160];
		snprintf(info, sizeof(info), "Resolution %ux%u: %.0f%% / %.0f%% of recommended (across / up-down); ~%ux%u matches",
				 g_vr.gameW, g_vr.gameH, (double)g_vr.sharpnessPctX, (double)g_vr.sharpnessPctY, g_vr.suggestedGameW, g_vr.suggestedGameH);
		MenuSetInfo(INFO_RESOLUTION, info);
	}
	g_vr.statLagSum = g_vr.statTurnSum = 0;
	g_vr.statLagCount = g_vr.statTurnCount = g_vr.statTurnOverMargin = 0;
	g_vr.statTurnMax = 0;
	g_vr.statSceneFrames = 0;
	g_vr.statCoverageSum = g_vr.statCoverageMax = 0;
	g_vr.statCoverageCount = 0;
	g_vr.statFrames = g_vr.statQuadFrames = g_vr.statStereoFrames = 0;
	g_vr.statGameFrames[0] = g_vr.statGameFrames[1] = g_vr.statGameFrames[2] = 0;
	g_vr.statStartMs = now;
}

// Hands the latest predicted eye poses and fields of view to the game thread.
void UpdateEyePoses(XrTime time) {
	XrViewLocateInfo locateInfo = {};
	locateInfo.type = XR_TYPE_VIEW_LOCATE_INFO;
	locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	locateInfo.space = g_vr.localSpace;
	// where the eyes are when this VR frame is shown (diagnostic: how far the shown image is off)
	{
		locateInfo.displayTime = time;
		XrViewState atDisplayState = {};
		atDisplayState.type = XR_TYPE_VIEW_STATE;
		XrView atDisplay[2] = {};
		atDisplay[0].type = atDisplay[1].type = XR_TYPE_VIEW;
		uint32_t atDisplayCount = 0;
		g_vr.displayPoseValid = XR_SUCCEEDED(g_vr.xrLocateViews(g_vr.session, &locateInfo, &atDisplayState, 2, &atDisplayCount, atDisplay)) && atDisplayCount == 2 &&
								(atDisplayState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);
		if (g_vr.displayPoseValid && (atDisplayState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT)) {
			g_vr.displayViews[0] = atDisplay[0];
			g_vr.displayViews[1] = atDisplay[1];
			g_vr.displayViewsValid = true;
		}
		if (g_vr.displayPoseValid)
			for (int eye = 0; eye < 2; eye++) {
				g_vr.displayEyeRot[eye][0] = atDisplay[eye].pose.orientation.x;
				g_vr.displayEyeRot[eye][1] = atDisplay[eye].pose.orientation.y;
				g_vr.displayEyeRot[eye][2] = atDisplay[eye].pose.orientation.z;
				g_vr.displayEyeRot[eye][3] = atDisplay[eye].pose.orientation.w;
			}
	}
	// the game renders with the pose predicted for when its frame will really be shown
	g_vr.predictionNs = g_cfg.posePrediction && g_vr.poseLagValid ? (int64_t)(g_vr.poseLagMs * 1e6) : 0;
	locateInfo.displayTime = time + g_vr.predictionNs;
	locateInfo.space = g_vr.localSpace;
	XrViewState viewState = {};
	viewState.type = XR_TYPE_VIEW_STATE;
	XrView views[2] = {};
	views[0].type = views[1].type = XR_TYPE_VIEW;
	uint32_t count = 0;
	const XrViewStateFlags need = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
	bool ok = XR_SUCCEEDED(g_vr.xrLocateViews(g_vr.session, &locateInfo, &viewState, 2, &count, views)) && count == 2 && (viewState.viewStateFlags & need) == need;
	XrPosef hands[2];
	bool handValid[2] = {VrInputLocateHand(0, g_vr.localSpace, locateInfo.displayTime, &hands[0]),
						 VrInputLocateHand(1, g_vr.localSpace, locateInfo.displayTime, &hands[1])};
	{
		ExclusiveLock guard(&g_stereo.lock);
		for (int hand = 0; hand < 2; hand++) {
			g_stereo.handValid[hand] = ok && handValid[hand];
			if (!g_stereo.handValid[hand]) continue;
			const XrPosef& pose = hands[hand];
			const float pos[3] = {pose.position.x, pose.position.y, pose.position.z};
			const float rot[4] = {pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w};
			memcpy(g_stereo.handPos[hand], pos, sizeof(pos));
			memcpy(g_stereo.handRot[hand], rot, sizeof(rot));
		}
		g_stereo.valid = ok;
		if (ok) g_stereo.poseBase = time;
		if (ok) {
			for (int eye = 0; eye < 2; eye++) {
				g_stereo.eyePos[eye][0] = views[eye].pose.position.x;
				g_stereo.eyePos[eye][1] = views[eye].pose.position.y;
				g_stereo.eyePos[eye][2] = views[eye].pose.position.z;
				g_stereo.eyeRot[eye][0] = views[eye].pose.orientation.x;
				g_stereo.eyeRot[eye][1] = views[eye].pose.orientation.y;
				g_stereo.eyeRot[eye][2] = views[eye].pose.orientation.z;
				g_stereo.eyeRot[eye][3] = views[eye].pose.orientation.w;
				g_stereo.eyeTan[eye][0] = pure::ftan(views[eye].fov.angleLeft);
				g_stereo.eyeTan[eye][1] = pure::ftan(views[eye].fov.angleRight);
				g_stereo.eyeTan[eye][2] = pure::ftan(views[eye].fov.angleUp);
				g_stereo.eyeTan[eye][3] = pure::ftan(views[eye].fov.angleDown);
			}
		}
	}
	static bool logged = false;
	if (ok && !logged) {
		logged = true;
		for (int eye = 0; eye < 2; eye++)
			Log("eye %d: fov left %.1f right %.1f up %.1f down %.1f deg, position (%.4f, %.4f, %.4f)", eye,
				(double)(views[eye].fov.angleLeft * 57.29578f), (double)(views[eye].fov.angleRight * 57.29578f),
				(double)(views[eye].fov.angleUp * 57.29578f), (double)(views[eye].fov.angleDown * 57.29578f),
				(double)views[eye].pose.position.x, (double)views[eye].pose.position.y, (double)views[eye].pose.position.z);
		float dx = views[1].pose.position.x - views[0].pose.position.x, dy = views[1].pose.position.y - views[0].pose.position.y,
			  dz = views[1].pose.position.z - views[0].pose.position.z;
		Log("IPD %.1f mm", (double)(pure::fsqrt(dx * dx + dy * dy + dz * dz) * 1000));
	}
}

XrFovf FovFromTag(const FrameTag& tag) {
	XrFovf fov;  // the lens centre is 0 unless AsymmetricEyes rendered an off-centre frustum
	fov.angleLeft = pure::fatan(tag.tanCentreX - tag.tanHalfW);
	fov.angleRight = pure::fatan(tag.tanCentreX + tag.tanHalfW);
	fov.angleUp = pure::fatan(tag.tanCentreY + tag.tanHalfH);
	fov.angleDown = pure::fatan(tag.tanCentreY - tag.tanHalfH);
	return fov;
}

bool UpdateMenu(XrTime time);
bool UpdateOverlay();
}  // namespace

namespace {
// [Stereo] RealFrameRate: only NEW game pictures are handed to the VR software. Otherwise the mod hands
// it a picture every headset refresh, re-aiming the last game picture when no new one has come: the VR
// software's own counter then always shows the headset's rate, and its frame synthesis (Virtual Desktop's
// SSW) never engages because it never sees the game fall behind. Same idea as SKVR/AKVR FPSLOCK-SSW:
// skip refreshes by waiting, never by resubmitting.
void WaitForNewGameFrame() {
	if (!g_cfg.realFrameRate || g_vr.state != XR_SESSION_STATE_FOCUSED) return;
	const DWORD start = GetTickCount();
	DWORD firstEyeAt = 0;
	while (GetTickCount() - start < 40) {  // never hold the headset longer than this: loading screens, pauses
		int32_t latest[3];
		{
			SectionLock guard(&g_share.lock);
			for (int k = 0; k < 3; k++) latest[k] = g_share.latest[k].seq;
		}
		bool left = latest[0] > g_vr.views[0].seq, right = latest[1] > g_vr.views[1].seq, flat = latest[2] > g_vr.views[2].seq;
		if (flat || (left && right)) return;
		// one eye in: the other is published at the same Present, a moment later (a few ms at most)
		if (left || right) {
			if (!firstEyeAt) firstEyeAt = GetTickCount();
			else if (GetTickCount() - firstEyeAt > 3) return;
		}
		Sleep(1);
	}
}
}  // namespace

void Frame() {
	WaitForNewGameFrame();
	XrFrameState frameState = {};
	frameState.type = XR_TYPE_FRAME_STATE;
	XrFrameWaitInfo waitInfo = {};
	waitInfo.type = XR_TYPE_FRAME_WAIT_INFO;
	if (!XR_CHECK_FRAME(g_vr.xrWaitFrame(g_vr.session, &waitInfo, &frameState))) {
		Sleep(5);
		return;
	}
	FrameRateNoteHeadsetFrame(frameState.predictedDisplayPeriod);  // the headset's rhythm, for "Game frame rate: headset"
	XrFrameBeginInfo beginInfo = {};
	beginInfo.type = XR_TYPE_FRAME_BEGIN_INFO;
	if (!XR_CHECK_FRAME(g_vr.xrBeginFrame(g_vr.session, &beginInfo))) return;

	const XrCompositionLayerBaseHeader* layers[8];
	uint32_t layerCount = 0;
	XrCompositionLayerQuad quad = {}, hud = {}, menu = {}, overlay = {};
	XrCompositionLayerProjection proj = {};
	XrCompositionLayerProjectionView projViews[2] = {};

	HandleHotkeys();
	VrInputSync(g_vr.state == XR_SESSION_STATE_FOCUSED, GameIsForeground());
	VrInputMotion(g_vr.localSpace, g_vr.viewSpace, frameState.predictedDisplayTime, g_vr.state == XR_SESSION_STATE_FOCUSED, g_vr.spaceFlipped);
	VrInputFingers(g_vr.localSpace, frameState.predictedDisplayTime);
	MenuTick(GameIsForeground());
	if (MenuTakeRecenter()) g_vr.recenterRequested = true;
	UpdateEyePoses(frameState.predictedDisplayTime);
	WatchSpace();  // TODO: i dont think this function works perfectly
	if (frameState.shouldRender) {
		if (!g_vr.screenPlaced || g_vr.recenterRequested) PlaceScreen(frameState.predictedDisplayTime);
		UpdateContent();
		MeasureUICoverage();
		auto& left = g_vr.views[0];
		auto& right = g_vr.views[1];
		auto& mono = g_vr.views[2];
		// Stereo while both eyes are newer than any flat frame: a menu (or F12) turns the screen flat with the
		// game's first flat frame, not before (the eye frames still on their way would flash on the flat screen).
		bool stereo = left.hasFrame && right.hasFrame && left.tag.patched && right.tag.patched && left.seq > mono.seq && right.seq > mono.seq;
		// Late warp: both eyes are submitted with the headset's own pose pair and field
		// of view for this frame, each image re-aimed to it here. Runtimes that stream to the headset
		// (Pico Connect, Steam Link, ...) assume one head pose per frame and images that cover exactly
		// the eye's field of view: each eye with its own render pose (different moments in alternating
		// mode) made the right eye stutter, and the symmetric, wider rendered field of view looked
		// squeezed sideways and cross-eyed.
		bool lateWarp = stereo && g_cfg.lateWarp && g_vr.lateWarpPs && g_vr.displayViewsValid;
		// the image the eyes show: the scene-only one (upscaled or not) when both eyes have it
		const bool scene = left.tag.hasScene && right.tag.hasScene;
		// Eye swapchains sized from the scene image even for frames without one (stretched), so they aren't rebuilt
		const UINT imgW = g_vr.sceneW ? g_vr.sceneW : g_vr.gameW, imgH = g_vr.sceneH ? g_vr.sceneH : g_vr.gameH;
		uint32_t eyeW[2] = {imgW, imgW}, eyeH[2] = {imgH, imgH};
		if (lateWarp)
			for (int eye = 0; eye < 2; eye++) {
				// same pixel density as the rendered image, over the eye's own field of view
				const FrameTag& tag = (eye == 0 ? left : right).tag;
				const XrFovf& fov = g_vr.displayViews[eye].fov;
				float width = (pure::ftan(fov.angleRight) - pure::ftan(fov.angleLeft)) * imgW / (2 * tag.tanHalfW);
				float height = (pure::ftan(fov.angleUp) - pure::ftan(fov.angleDown)) * imgH / (2 * tag.tanHalfH);
				eyeW[eye] = ((uint32_t)width + 1) & ~1u;
				eyeH[eye] = ((uint32_t)height + 1) & ~1u;
				// a few pixels' change doesn't need a new swapchain
				if (g_vr.eyeSwap[eye].handle && eyeW[eye] + 16 > g_vr.eyeSwap[eye].w && eyeW[eye] < g_vr.eyeSwap[eye].w + 16 && eyeH[eye] + 16 > g_vr.eyeSwap[eye].h &&
					eyeH[eye] < g_vr.eyeSwap[eye].h + 16) {
					eyeW[eye] = g_vr.eyeSwap[eye].w;
					eyeH[eye] = g_vr.eyeSwap[eye].h;
				}
			}
		stereo = stereo && EnsureSwap(g_vr.eyeSwap[0], eyeW[0], eyeH[0], "left eye") && EnsureSwap(g_vr.eyeSwap[1], eyeW[1], eyeH[1], "right eye");
		if (stereo) {
			bool ok = true;
			for (int eye = 0; eye < 2 && ok; eye++) {
				auto& eyeView = eye == 0 ? left : right;
				const FrameTag& tag = eyeView.tag;
				// the 3D scene without HUD/menus when both eyes have one (never mix the two kinds:
				// that would show the UI in one eye and on the HUD layer at the same time)
				ID3D11ShaderResourceView* image = scene ? eyeView.sceneSrv : eyeView.srv;
				projViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
				if (lateWarp) {
					const XrView& shown = g_vr.displayViews[eye];
					WarpParams warp;
					warp.outTan[0] = pure::ftan(shown.fov.angleLeft);
					warp.outTan[1] = pure::ftan(shown.fov.angleRight);
					warp.outTan[2] = pure::ftan(shown.fov.angleUp);
					warp.outTan[3] = pure::ftan(shown.fov.angleDown);
					warp.srcTan[0] = tag.tanHalfW;
					warp.srcTan[1] = tag.tanHalfH;
					warp.srcTan[2] = tag.tanCentreX;
					warp.srcTan[3] = tag.tanCentreY;
					pure::M4 renderRot = pure::m4_from_pose({tag.rot[0], tag.rot[1], tag.rot[2], tag.rot[3]}, {0, 0, 0});
					pure::M4 shownRot = pure::m4_from_pose({shown.pose.orientation.x, shown.pose.orientation.y, shown.pose.orientation.z,
															shown.pose.orientation.w},
														   {0, 0, 0});
					for (int r = 0; r < 3; r++) {
						for (int c = 0; c < 3; c++)
							warp.m[r][c] = renderRot.at(0, r) * shownRot.at(0, c) + renderRot.at(1, r) * shownRot.at(1, c) + renderRot.at(2, r) * shownRot.at(2, c);
						warp.m[r][3] = 0;
					}
					ok = RenderInto(g_vr.eyeSwap[eye], image, nullptr, &warp);
					projViews[eye].pose = shown.pose;
					projViews[eye].fov = shown.fov;
				} else {
					ok = RenderInto(g_vr.eyeSwap[eye], image);
					projViews[eye].pose.position = {tag.pos[0], tag.pos[1], tag.pos[2]};
					projViews[eye].pose.orientation = {tag.rot[0], tag.rot[1], tag.rot[2], tag.rot[3]};
					projViews[eye].fov = FovFromTag(tag);
				}
				projViews[eye].subImage.swapchain = g_vr.eyeSwap[eye].handle;
				projViews[eye].subImage.imageRect.offset = {0, 0};
				projViews[eye].subImage.imageRect.extent = {(int32_t)g_vr.eyeSwap[eye].w, (int32_t)g_vr.eyeSwap[eye].h};
			}
			// resolution hint: the game picture's pixel density against the headset's recommended one
			if (g_vr.recommendedW && g_vr.displayViewsValid) {
				const XrFovf& fov = g_vr.displayViews[0].fov;
				float spanX = pure::ftan(fov.angleRight) - pure::ftan(fov.angleLeft);
				float needW = g_vr.recommendedW / spanX * 2 * left.tag.tanHalfW;  // game width with the recommended density
				float spanY = pure::ftan(fov.angleUp) - pure::ftan(fov.angleDown);
				float needH = g_vr.recommendedH / spanY * 2 * left.tag.tanHalfH;
				if (needW > 64 && needH > 64) {
					// the 16:9 picture is stretched over the eye's field of view (Anamorphic): the two
					// directions can differ a lot (on a Quest: 144% across, 79% up-down)
					g_vr.sharpnessPctX = 100.f * imgW / needW;
					g_vr.sharpnessPctY = 100.f * imgH / needH;
					// the game resolution with the headset's detail, in its shape (the game renders any
					// shape; the upscaler's output follows it)
					g_vr.suggestedGameW = ((UINT)needW + 4) & ~7u;
					g_vr.suggestedGameH = ((UINT)needH + 4) & ~7u;
					{
						SectionLock guard(&g_share.lock);
						g_share.nativeW = ((UINT)needW + 1) & ~1u;
						g_share.nativeH = ((UINT)needH + 1) & ~1u;
					}
					SaveHeadsetSize(needW, needH);
				}
			}
			static int lateLogged = -1;
			if (ok && lateLogged != (int)lateWarp) {
				lateLogged = (int)lateWarp;
				if (lateWarp)
					Log("late warp: eyes submitted with the headset's pose and field of view (left %.1f/%.1f, up %.1f/%.1f "
						"deg), %ux%u per eye from a %ux%u image rendered %.1f x %.1f deg",
						(double)(g_vr.displayViews[0].fov.angleLeft * 57.29578f), (double)(g_vr.displayViews[0].fov.angleRight * 57.29578f),
						(double)(g_vr.displayViews[0].fov.angleUp * 57.29578f), (double)(g_vr.displayViews[0].fov.angleDown * 57.29578f),
						g_vr.eyeSwap[0].w, g_vr.eyeSwap[0].h, g_vr.gameW, g_vr.gameH,
						(double)(2 * pure::fatan(left.tag.tanHalfW) * 57.29578f), (double)(2 * pure::fatan(left.tag.tanHalfH) * 57.29578f));
				else
					Log("late warp off: eyes submitted with the pose and field of view they were rendered with");
			}
			// head pose prediction: how long after the pose's base time the images are on screen
			for (int eye = 0; eye < 2 && ok; eye++) {
				const FrameTag& tag = (eye == 0 ? left : right).tag;
				if (!tag.poseBase) continue;
				double delayMs = (double)(frameState.predictedDisplayTime - tag.poseBase) / 1e6;
				if (delayMs <= 0 || delayMs > 250) continue;
				g_vr.poseLagMs = g_vr.poseLagValid ? g_vr.poseLagMs + (delayMs - g_vr.poseLagMs) * 0.01 : delayMs;
				if (g_vr.poseLagMs > 120) g_vr.poseLagMs = 120;  // runtimes extrapolate the head only so far
				g_vr.poseLagValid = true;
				g_vr.statLagSum += delayMs;
				g_vr.statLagCount++;
			}
			// how far the head has turned from the shown image's pose (the black edge, if beyond the margin)
			if (ok && g_vr.displayPoseValid) {
				const float* rot = left.tag.rot;
				float deg = pure::q_angle_deg({rot[0], rot[1], rot[2], rot[3]},
											  {g_vr.displayEyeRot[0][0], g_vr.displayEyeRot[0][1], g_vr.displayEyeRot[0][2], g_vr.displayEyeRot[0][3]});
				g_vr.statTurnSum += deg;
				g_vr.statTurnCount++;
				if (deg > g_vr.statTurnMax) g_vr.statTurnMax = deg;
				// margin: rendered half-width beyond the headset's own field of view (horizontal)
				AcquireSRWLockShared(&g_stereo.lock);
				float ownTan = -g_stereo.eyeTan[0][0] > g_stereo.eyeTan[0][1] ? -g_stereo.eyeTan[0][0] : g_stereo.eyeTan[0][1];
				ReleaseSRWLockShared(&g_stereo.lock);
				g_vr.marginDeg = (pure::fatan(left.tag.tanHalfW) - pure::fatan(ownTan)) * 57.29578f;
				if (deg > g_vr.marginDeg) g_vr.statTurnOverMargin++;
			}
			if (ok) {
				proj.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
				proj.space = g_vr.localSpace;
				proj.viewCount = 2;
				proj.views = projViews;
				layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&proj;
				g_vr.statStereoFrames++;
				WatchEyeBars(left.srv);  // logs black bands in the 3D picture (cutscenes)
				// HUD: keyed out of the newest eye frame, on a transparent flat layer in front.
				// (Synchronized: the right eye is replayed without UI, so the HUD comes from the left)
				auto& hudSource = (left.seq > right.seq && !left.tag.noUI) || right.tag.noUI ? left : right;
				if (scene && g_vr.screenPlaced && EnsureSwap(g_vr.hudSwap, g_vr.gameW, g_vr.gameH, "HUD") &&
					RenderInto(g_vr.hudSwap, hudSource.srv, KeySrv(hudSource))) {
					hud.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
					hud.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
					hud.space = g_vr.localSpace;
					hud.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
					hud.subImage.swapchain = g_vr.hudSwap.handle;
					hud.subImage.imageRect.offset = {0, 0};
					hud.subImage.imageRect.extent = {(int32_t)g_vr.hudSwap.w, (int32_t)g_vr.hudSwap.h};
					hud.pose = g_vr.quadPose;
					// the game lays out its UI for a 5:4 box at least (below that its final pass squeezes it in)
					float hudAspect = (float)g_vr.gameW / (float)g_vr.gameH;
					if (hudAspect < kGameMinAspect) hudAspect = kGameMinAspect;
					hud.size = {g_cfg.width, g_cfg.width / hudAspect};
					layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&hud;
				}
			}
		} else {
			// Flat screen: the flat frame while it is newer than an eye (switching to or from flat), else the newest eye
			int newest = mono.hasFrame && (mono.seq > left.seq || mono.seq > right.seq) ? 2 : -1;
			for (int i = 0; i < 2 && newest < 0; i++)
				if (g_vr.views[i].hasFrame && (newest < 0 || g_vr.views[i].seq > g_vr.views[newest].seq)) newest = i;
			// The screen as a window hanging in the room, drawn into both eyes (ScreenInEyes, the
			// default): a quad layer is composited from one picture for both eyes, and VR software
			// that streams to the headset smears and shears it while the head turns. Drawn here,
			// each eye sees the window through its own lens from where that eye really is.
			bool screenInEyes = g_cfg.screenInEyes && g_vr.screenPs && g_vr.displayViewsValid && g_vr.screenPlaced && newest >= 0 &&
								g_vr.recommendedW && g_vr.recommendedH;
			if (screenInEyes && EnsureSwap(g_vr.screenEyeSwap[0], g_vr.recommendedW, g_vr.recommendedH, "left eye screen") &&
				EnsureSwap(g_vr.screenEyeSwap[1], g_vr.recommendedW, g_vr.recommendedH, "right eye screen")) {
				float rowTop, rowBottom;  // the picture between black bands (CropBlackBars)
				ScreenPictureRows(g_vr.views[newest].srv, &rowTop, &rowBottom);
				float screenW = g_cfg.width, screenH = g_cfg.width * (float)g_vr.gameH * (rowBottom - rowTop) / (float)g_vr.gameW;
				pure::M4 screenPose = pure::m4_from_pose({g_vr.quadPose.orientation.x, g_vr.quadPose.orientation.y, g_vr.quadPose.orientation.z,
														  g_vr.quadPose.orientation.w},
														 {g_vr.quadPose.position.x, g_vr.quadPose.position.y, g_vr.quadPose.position.z});
				bool ok = true;
				for (int eye = 0; eye < 2 && ok; eye++) {
					const XrView& view = g_vr.displayViews[eye];
					ScreenParams screen = {};
					screen.outTan[0] = pure::ftan(view.fov.angleLeft);
					screen.outTan[1] = pure::ftan(view.fov.angleRight);
					screen.outTan[2] = pure::ftan(view.fov.angleUp);
					screen.outTan[3] = pure::ftan(view.fov.angleDown);
					screen.half[0] = screenW * 0.5f;
					screen.half[1] = screenH * 0.5f;
					screen.half[2] = rowTop;
					screen.half[3] = rowBottom;
					pure::M4 eyePose = pure::m4_from_pose({view.pose.orientation.x, view.pose.orientation.y, view.pose.orientation.z, view.pose.orientation.w},
														  {view.pose.position.x, view.pose.position.y, view.pose.position.z});
					// into the screen's own space: R_screen^T R_eye, and where this eye is from the screen
					pure::M4 toScreen = pure::m4_mul(pure::m4_rigid_inverse(screenPose), eyePose);
					for (int row = 0; row < 3; row++)
						for (int col = 0; col < 4; col++) screen.m[row][col] = toScreen.at(row, col);
					ok = RenderScreenInto(g_vr.screenEyeSwap[eye], g_vr.views[newest].srv, screen);
					projViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
					projViews[eye].pose = view.pose;
					projViews[eye].fov = view.fov;
					projViews[eye].subImage.swapchain = g_vr.screenEyeSwap[eye].handle;
					projViews[eye].subImage.imageRect.offset = {0, 0};
					projViews[eye].subImage.imageRect.extent = {(int32_t)g_vr.screenEyeSwap[eye].w, (int32_t)g_vr.screenEyeSwap[eye].h};
				}
				if (ok) {
					proj.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
					proj.space = g_vr.localSpace;
					proj.viewCount = 2;
					proj.views = projViews;
					layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&proj;
					g_vr.statQuadFrames++;
					newest = -1;  // done: no quad as well
					static int loggedScreen = -1;
					if (loggedScreen != 1) {
						loggedScreen = 1;
						Log("flat screen: drawn into both eyes as a %.2f x %.2f m window in the room, %ux%u per eye "
							"(ScreenInEyes=1; 0 hands it to the VR software as a quad layer)",
							(double)screenW, (double)screenH, g_vr.screenEyeSwap[0].w, g_vr.screenEyeSwap[0].h);
					}
				}
			}
			if (g_vr.screenPlaced && newest >= 0 && EnsureSwap(g_vr.screenSwap, g_vr.gameW, g_vr.gameH, "screen") &&
				RenderInto(g_vr.screenSwap, g_vr.views[newest].srv)) {
				quad.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
				quad.space = g_vr.localSpace;
				quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
				quad.subImage.swapchain = g_vr.screenSwap.handle;
				quad.subImage.imageRect.offset = {0, 0};
				quad.subImage.imageRect.extent = {(int32_t)g_vr.screenSwap.w, (int32_t)g_vr.screenSwap.h};
				quad.pose = g_vr.quadPose;
				quad.size = {g_cfg.width, g_cfg.width * (float)g_vr.gameH / (float)g_vr.gameW};
				layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&quad;
				g_vr.statQuadFrames++;
			}
		}
	}

	// the stats overlay: moves with your head (view space), upper left of your view
	if (frameState.shouldRender && g_cfg.statsOverlay && UpdateOverlay()) {
		// 0.32 m wide at 0.8 m; elsewhere scaled so it keeps its size and place in your view
		const float scale = g_cfg.overlayDistance / 0.8f;
		const float width = 0.32f * scale;
		overlay.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
		overlay.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;  // ImGui's output is premultiplied
		overlay.space = g_vr.viewSpace;
		overlay.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		overlay.subImage.swapchain = g_vr.overlayPanel.swap.handle;
		overlay.subImage.imageRect.offset = {0, 0};
		overlay.subImage.imageRect.extent = {(int32_t)g_vr.overlayPanel.swap.w, (int32_t)g_vr.overlayPanel.swap.h};
		overlay.pose.orientation = {0, 0, 0, 1};
		overlay.pose.position = {g_cfg.overlayX * scale, g_cfg.overlayY * scale, -g_cfg.overlayDistance};
		overlay.size = {width, width * (float)g_vr.overlayPanel.swap.h / (float)g_vr.overlayPanel.swap.w};
		layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&overlay;
	}

	// the settings menu, on top of everything
	if (frameState.shouldRender && MenuIsOpen() && UpdateMenu(frameState.predictedDisplayTime)) {
		menu.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
		menu.space = g_vr.localSpace;
		menu.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		menu.subImage.swapchain = g_vr.menuPanel.swap.handle;
		menu.subImage.imageRect.offset = {0, 0};
		menu.subImage.imageRect.extent = {(int32_t)g_vr.menuPanel.swap.w, (int32_t)g_vr.menuPanel.swap.h};
		menu.pose = g_vr.menuPose;
		const float menuWidth = 1.0f * g_cfg.menuDistance / 1.1f;  // 1 m wide at 1.1 m
		menu.size = {menuWidth, menuWidth * (float)g_vr.menuPanel.swap.h / (float)g_vr.menuPanel.swap.w};
		layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&menu;
	}

	XrFrameEndInfo endInfo = {};
	endInfo.type = XR_TYPE_FRAME_END_INFO;
	endInfo.displayTime = frameState.predictedDisplayTime;
	endInfo.environmentBlendMode = g_vr.blendMode;
	endInfo.layerCount = layerCount;
	endInfo.layers = layerCount ? layers : nullptr;
	XR_CHECK_FRAME(g_vr.xrEndFrame(g_vr.session, &endInfo));
	g_vr.statFrames++;
	OverlayCountHeadsetFrame();
	LogStats();
}

// Shows an ImGui panel (the menu, the stats overlay): draw() renders it into the panel's own render
// target, which is then copied into the swapchain. The render target uses the swapchain's format
// without sRGB, so the copy keeps ImGui's colours as they are (they are already sRGB values).
// True = the panel can be shown.
typedef bool (*PanelDraw)(ID3D11Device* dev, ID3D11DeviceContext* dc, ID3D11RenderTargetView* rtv, int w, int h);
namespace {
bool RenderPanel(Panel& panel, int width, int height, const char* what, PanelDraw draw) {
	if (!EnsureSwap(panel.swap, (uint32_t)width, (uint32_t)height, what, true)) return false;
	if (panel.swap.w != (uint32_t)width || panel.swap.h != (uint32_t)height) return false;  // runtime limits: not supported
	if (!panel.tex) {
		DXGI_FORMAT format = (DXGI_FORMAT)g_vr.swapFormat;
		if (format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) format = DXGI_FORMAT_R8G8B8A8_UNORM;
		if (format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) format = DXGI_FORMAT_B8G8R8A8_UNORM;
		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = (UINT)width;
		desc.Height = (UINT)height;
		desc.MipLevels = desc.ArraySize = 1;
		desc.Format = format;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_RENDER_TARGET;
		if (FAILED(g_vr.device->CreateTexture2D(&desc, nullptr, &panel.tex)) || FAILED(g_vr.device->CreateRenderTargetView(panel.tex, nullptr, &panel.rtv))) {
			SafeRelease(panel.tex);
			return false;
		}
		panel.shown = false;
	}
	if (!draw(g_vr.device, g_vr.context, panel.rtv, width, height)) return panel.shown;
	uint32_t imageIndex = 0;
	XrSwapchainImageAcquireInfo acquireInfo = {};
	acquireInfo.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
	if (!XR_CHECK_FRAME(g_vr.xrAcquireSwapchainImage(panel.swap.handle, &acquireInfo, &imageIndex))) return panel.shown;
	XrSwapchainImageWaitInfo waitInfo = {};
	waitInfo.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
	waitInfo.timeout = XR_INFINITE_DURATION;
	if (!XR_CHECK_FRAME(g_vr.xrWaitSwapchainImage(panel.swap.handle, &waitInfo))) return panel.shown;  // no release without a wait
	if (imageIndex < panel.swap.count) {
		ComPtr<ID3D11Resource> image;
		panel.swap.rtv[imageIndex]->GetResource(&image);
		if (image) {
			g_vr.context->CopyResource(image.Get(), panel.tex);
		}
	}
	XrSwapchainImageReleaseInfo releaseInfo = {};
	releaseInfo.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
	XR_CHECK_FRAME(g_vr.xrReleaseSwapchainImage(panel.swap.handle, &releaseInfo));
	panel.shown = true;
	return true;
}

// The settings menu: placed in front of your head when it opens ([Screen] MenuDistance, which moves
// it at once when changed), drawn every headset frame while open (ImGui's navigation runs per frame).
bool UpdateMenu(XrTime time) {
	if (MenuTakeJustOpened()) {
		XrSpaceLocation location = {};
		location.type = XR_TYPE_SPACE_LOCATION;
		if (XR_SUCCEEDED(g_vr.xrLocateSpace(g_vr.viewSpace, g_vr.localSpace, time, &location)) &&
			(location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
			pure::Q head{location.pose.orientation.x, location.pose.orientation.y, location.pose.orientation.z, location.pose.orientation.w};
			pure::Q yaw;
			bool flipped = g_vr.spaceFlipped;
			if (!pure::level_reference(head, &flipped, &yaw)) yaw = g_vr.lastScreenYaw;
			g_vr.menuYaw = yaw;
			g_vr.menuHeadPos = location.pose.position;
			g_vr.menuPlaced = true;
		}
	}
	if (g_vr.menuPlaced) {
		float scale = g_cfg.menuDistance / 1.1f;
		pure::V3 offset = pure::rotate(g_vr.menuYaw, {0, -0.12f * scale, -g_cfg.menuDistance});
		g_vr.menuPose.orientation = {g_vr.menuYaw.x, g_vr.menuYaw.y, g_vr.menuYaw.z, g_vr.menuYaw.w};
		g_vr.menuPose.position = {g_vr.menuHeadPos.x + offset.x, g_vr.menuHeadPos.y + offset.y, g_vr.menuHeadPos.z + offset.z};
	}
	return RenderPanel(g_vr.menuPanel, 1280, 960, "menu", MenuRender);
}

// write overlay stats to the struct passed in
// possibly move to own file later if necessary
void GatherOverlayNumbers(OverlayNumbers& numbers) {
	AcquireSRWLockShared(&g_stereo.lock);
	bool stereoOn = g_stereo.enabled;
	ReleaseSRWLockShared(&g_stereo.lock);
	snprintf(numbers.mode, sizeof(numbers.mode), "%s",
			 !stereoOn || g_vr.menuMode ? "flat screen" : (StereoAfw() ? "AFW" : StereoSynchronized() ? "synchronized"
																									  : "alternating"));
	numbers.gameW = g_vr.gameW;
	numbers.gameH = g_vr.gameH;
	numbers.sceneW = g_vr.sceneW;
	numbers.sceneH = g_vr.sceneH;
	UpscaleStatus(numbers.upscaler, sizeof(numbers.upscaler));
	numbers.sharpX = g_vr.sharpnessPctX;
	numbers.sharpY = g_vr.sharpnessPctY;
	numbers.poseDelayMs = g_vr.poseLagValid ? (float)g_vr.poseLagMs : -1.0f;
	numbers.predictMs = (float)(g_vr.predictionNs / 1e6);
	HandPeaks hands[2];
	VrInputTakeHandPeaks(hands);
	numbers.leftHand = {hands[0].speed, hands[0].spin, hands[0].accel, hands[0].spinAccel};
	numbers.rightHand = {hands[1].speed, hands[1].spin, hands[1].accel, hands[1].spinAccel};
}

// returns true when successfully drawn
// returns false if RenderPanel returned false
bool UpdateOverlay() {
	if (!OverlayRedrawDue(g_vr.overlayPanel.shown, GatherOverlayNumbers)) {
		return true;
	}
	return RenderPanel(g_vr.overlayPanel, 640, 420, "stats overlay", OverlayRender);
}
}  // namespace
