# NieR-Automata-in-VR
A mod that converts the game NieR: Automata into a VR mod!

NieR:Automata in VR Features:

Full VR conversion

Real stereo 3D with full 6DoF head tracking during gameplay
Menus and cutscenes on a large virtual screen in front of you
HUD and menus on their own flat layer, so they stay readable instead of being baked into the 3D image
Horizon levelling for comfort, with the camera’s tilt kept in steep top-down sections
Head-direction culling, so objects don’t vanish when you look around
Lighting and reflections corrected for each eye

Smooth and comfortable

Low-latency head tracking: each frame is rendered from where your head will be when it’s shown
Late warp: every headset frame, both eyes are re-aimed to your current head pose
Adjustable render margin to prevent black edges on fast head turns
Two rendering modes:
Alternating eyes: light on the GPU
Both eyes every frame: the best image, at about twice the GPU cost

Play with your VR controllers

Quest/Touch, Valve Index, Pico, Vive, Windows Mixed Reality and PSVR2 (through SteamVR) work as a standard gamepad
Rumble on the controllers
Automatic switching between VR controllers, keyboard/mouse and a real gamepad

In-headset settings menu

Change every setting while playing, with instant effect and auto-save
Live performance info: frame rates, GPU timings, head-tracking delay and a resolution recommendation for your headset

Performance options

Resolutions up to 300% of your monitor’s, with a hint that suggests the right one for your headset
Global illumination volume limit (inspired by the FAR mod)
Toggles for shadows and ambient occlusion
Shadow reuse in both-eyes mode
Frame queue control for lower latency

Broad headset support

Works with any OpenXR runtime: SteamVR, Virtual Desktop, Quest Link/Steam Link, Pico Connect, Pimax Play and more
Tested on Pimax Dream Air, Quest 3 (Steam Link and Virtual Desktop), Pico 4 and PSVR2
Handles asymmetric fields of view and runtimes with non-standard tracking spaces

Easy to install

Drop one file (d3d11.dll) into the game folder; no injector needed

Install
-------
Download the "NieRVR-0.5.4.zip" file (Not the source files). Open the game folder: in Steam, right-click NieR:Automata > Manage >
Browse local files. You should see NieRAutomata.exe.

Unzip the "NieRVR-0.5.4.zip" file, then copy d3d11.dll inside into your NieR: Automata Steam folder.
Start your VR runtime (SteamVR) first, then the game from Steam.

In the game's settings, use windowed or borderless mode and pick a high
resolution: the headset gets the game's full resolution (see
"Resolution" below).

Uninstall: delete d3d11.dll (and NieRVR.ini / NieRVR*.log if you like).
[VR] Enabled=0 in NieRVR.ini turns the mod off without uninstalling it.


Settings menu
-------------
Open it with Insert, or by holding both stick buttons (L3 + R3) for half a
second on a gamepad or the VR controllers. Up/Down choose, Left/Right
change, A switch, LB/RB change page, B (or Insert) closes. Changes apply at
once and are saved to NieRVR.ini. The Info page shows frame rates, GPU
timing and the head-tracking delay.

Resolution
----------
The headset gets the game's own render resolution, so higher is sharper.
With [Render] ExtraResolutions=1 (default) the game's resolution list
offers 125, 150, 175, 200, 250 and 300% of your monitor's resolution (up to
16384 pixels). Use windowed or borderless mode with them: Windows scales
the picture down for the monitor. Each eye is rendered from the game's
picture, so the cost is the game's at that resolution. The menu's Info page
(and the log) shows how the game's resolution compares with what your
headset recommends, and which resolution would match it: more than that
costs GPU time for little gain.


Performance tips
----------------
- Alternating eyes (default) draws one eye per game frame: 30 images per
  second per eye, light on the graphics card. Both eyes every frame (F5)
  looks best but needs about twice the GPU time.
- Rendering page: Global illumination volumes 16-32 (about 1 ms per frame
  at 4.8K), Ambient occlusion off (about +1.5 fps), Shadows off.
- Frame queue 1 (default) gives the lowest delay; 2 may run slightly faster.


VR controllers
--------------
On by default (menu > Controls). A real gamepad and keyboard/mouse keep
working at the same time.

    Quest / Pico / Index          Game (Xbox gamepad)
    --------------------          -------------------
    left stick, right stick       left stick, right stick
    left / right trigger          LT / RT
    left / right grip             LB / RB
    A, B (right hand)             A, B
    X, Y (left hand)              X, Y   (Index: left A, B)
    stick clicks                  L3, R3
    left menu button, tap         Start
    left menu button, hold        Back (View)
    left menu button + left stick D-pad (keep the menu button held)
    (Index: press the left trackpad firmly for the menu button)

Vive wands have few buttons: right trackpad click = A, right menu = B,
trackpads = sticks.


Keys (while the game window has focus)
--------------------------------------
  Insert          settings menu
  F8              recenter the view and the screen in front of you
  F12             stereo 3D / flat screen
  F5              both eyes every frame on/off
  Shift+F12       steep top-down views flat on/off
  F6 / F7         screen and HUD closer / farther
  F9 / F10        screen and HUD smaller / larger
  Ctrl+F6 / F7    camera closer / farther (stronger / weaker 3D)
  Ctrl+F9 / F10   world bigger / smaller
  Ctrl+F11        ambient occlusion on/off
  Ctrl+Shift+F11  shadows on/off
  Ctrl+Shift+F5   head culling on/off
Diagnostics: F11 capture, Ctrl+F12 single-eye test, Ctrl+F5 camera watch,
Shift+F5 culling test steps.


Settings (NieRVR.ini)
---------------------
[Stereo]
Enabled=1           start in stereo (F12 toggles)
WorldScale=0.75     eye distance in game units per metre (bigger = deeper, world smaller)
CameraDolly=0.0     metres the camera moves towards the character (stronger 3D)
LevelCamera=1       keeps the horizon level (removes the game camera's tilt)
SteepViewFrom=45 / SteepViewFull=60   camera pitch where the tilt starts to be
                    kept / is fully kept (top-down sections)
TopDownFlat=0       1 = steep top-down views on the flat screen (Shift+F12)
Anamorphic=1        stretch the image to each eye's field of view (sharper)
VerticalFill=1.0    fraction of your vertical view rendered (lower = sharper)
FovScale=1.10       rendered field of view relative to the headset's (margin for head turns)
PosePrediction=1    render each frame from where your head will be when it is shown
LateWarp=1          each headset frame, re-aim both eyes to the head pose and the eyes' own field of view
SplitUI=1           HUD and menus on a flat layer; 0 = HUD in the 3D image
BothEyes=0          1 = both eyes every frame (F5), 0 = alternate eyes
FlatBelowDraws=250  frames with fewer draws are shown flat (title screens)
Experimental, always starts off: DepthReprojection.

[Render]
MaxFrameLatency=1   frames the game may prepare ahead of the GPU (1 = lowest delay, 0 = game's)
GIVolumes=128       GI volumes per pixel, nearest first (fewer = faster; 128 = all)
AmbientOcclusion=1, Shadows=1, Vignette=0, LowHealthEffect=0, DistortionEffects=0
FixLighting=1       fix NieR's tiled lighting and reflections for the VR camera
ReuseShadows=1      both eyes: the right eye reuses the left eye's shadow map
HeadCulling=1       the game's culling follows your headset (Ctrl+Shift+F5)
HeadCullingMargin=20, CullingFov=170, LodFix=1, WideProjection=0
ExtraResolutions=1  resolutions up to 300% (2 = also patch the game's built-in table)

[Input]
VRControllers=1     VR controllers act as a gamepad
ControllerRumble=1  the game's rumble on the VR controllers

[Screen]            distance / width / height of the virtual screen and HUD
[Keys]              hotkeys (Windows virtual-key codes)
[OpenXR] LoaderPath=  optional: Khronos' openxr_loader.dll next to the mod (or
                    its path here) enables OpenXR API layers such as OpenXR
                    Toolkit. Without it the mod talks to the active runtime
                    directly and API layers are not loaded.
[Loader] ChainLoad= for another mod that also wants to be d3d11.dll: rename
                    its file (e.g. d3d11_reshade.dll) and put the name here.
[Debug] Verbose=1   more detail in the log

Problems?
---------
Send NieRVR.log (the previous run's is NieRVR.prev.log), NieRVR_crash.dmp
if the game crashed, and a sentence on what you saw.


Known limitations
-----------------
- Alternating eyes: things that move by themselves (2B, enemies) update 30
  times per second per eye, so fast motion can look doubled. Both eyes
  every frame avoids it at about twice the GPU cost.


