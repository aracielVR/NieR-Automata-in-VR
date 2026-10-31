# NieR-Automata-in-VR
A mod that converts the game NieR: Automata into a VR mod!

## Features

- Easily install by dragging and dropping one dll and launching the game

- Easy resolution and performance control with dlss

- First person mode (work in progress)

- Vr controller support with optional waggle controls (work in progress)

- In-headset mod menu

- In game performance overlay

- Broad headset and runtime support

- A correct implementation of the headset's asymmetric field of view, for the same sharpness with about a third
  less graphics work: each eye's camera faces the middle of what that eye can
  see, so no part of the picture is drawn without being seen

- Menus, loading screens and the title as a steady window in the room, drawn into both eyes

- Game resolution and DLSS quality set from the VR panel

- Frame rates above 60 (experimental): 72, 80, 90, 120, the headset's own rate or unlimited, with the game kept at its normal speed, focused or not

- Lens flare off by default, sun glow correct in both eyes
- Sun shadows worked out for where your head looks, not where the game camera points, so they don't go missing when you turn your head
- No black bars: films and menus show on a screen trimmed to the picture, and 3D cutscenes fill your view without the dark bands above and below (panel: Screen > Hide black bars)
- The game's own anti-aliasing and motion blur are switched off for you at start (DLSS smooths the edges better, and the game's MSAA would stop DLSS), and its V-sync is ignored so the monitor can't hold back the headset's frame rate. `GameAntiAliasing=1`, `GameMotionBlur=1` or `GameVsync=1` in NieRVR.ini keep the game's own setting

## Install
1. Download the the latest release. Open the game folder: in Steam, right-click NieR:Automata > Manage > Browse local files

2. Unzip the release, then copy d3d11.dll inside into your NieR: Automata Steam folder next to NieRAutomata.exe

3. Launch the game, VR should work automatically as long as your headset is connected and steamvr is running like normal

4. Motion blur, the game's anti-aliasing and V-sync are turned off by the mod (blur would ruin the image, and DLSS smooths better than the game's own anti-aliasing)

5. In the game's settings, use borderless mode and pick a resolution that your computer
can comfortably run. If performance is poor, try a lower resolution and upscale with dlss

6. Open the mod menu by pressing (and holding) B then press A. Changes are automatically saved to the ini file

7. Uninstall: delete d3d11.dll (and NieRVR.ini / NieRVR*.log if you like).
[VR] Enabled=0 in NieRVR.ini turns the mod off without uninstalling it.

## Performance tips

- Alternating mode (the default) is the fastest; Synchronized (F5) looks smoother but needs about twice the GPU work;
  AFW draws one eye per frame and builds the other from it, so both eyes show the same moment
- Try DLSS to improve resolution. The fast DLSS model (the default) costs 2-3 ms less per frame than "best"
- Above 60 fps: back up your saves first. Set the headset to a rate your graphics card holds, then pick
  "Headset rate" in the panel (Rendering > Game frame rate). In the alternating modes (one eye per frame), "Unlimited"
  lets the game run as fast as your graphics card goes


## VR controllers

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


## Keyboard controls

  Insert          settings menu
  F8              recenter the view and the screen in front of you
  F12             stereo 3D / flat screen
  F5              stereo mode: alternating / synchronized / AFW
  Shift+F12       steep top-down views flat on/off
  F6 / F7         screen and HUD closer / farther
  F9 / F10        screen and HUD smaller / larger
  Ctrl+F6 / F7    camera closer / farther (stronger / weaker 3D)
  Ctrl+F9 / F10   world bigger / smaller
  Ctrl+Shift+F5   head culling on/off
  Ctrl+Shift+F4   sun shadows follow your head on/off
Diagnostics: F11 frame capture, Ctrl+F11 dump of the game's decrypted code.

## Useful resources

- https://github.com/aracielVR/NieR-Automata-in-VR
- https://github.com/Crementif/BotW-BetterVR
- https://github.com/Asiern/NieRHook