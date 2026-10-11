# Speed Racer: The Great Plan running natively on the Nintendo Switch

Goal: native reimplementation (PC first, then Nintendo Switch via nxvk) of the 2005
Iron Monkey / Blit Interactive game. **Bring your own copy**: this repo contains only our code.

Game source (archived): https://archive.org/details/SpeedRacerDemonOnWheels
File: `Speed Racer Demon on Wheels The Great Plan Full.zip` (24,370,170 bytes, MD5 13e04edf49e8eb7ac981fb5f5d83e341)
Tool: ProjectorRays v0.2.0 https://github.com/ProjectorRays/ProjectorRays/releases (Lingo decompiler/chunk dumper)

## Setup
1. Put the zip at `local/dl/full.zip` and ProjectorRays at `local/dl/projectorrays-0.2.0.exe`
   (already there if you copied this whole folder).
2. `tools/extract.sh` -> `local/out/` (chunks + `.ls`/`.lasm` Lingo) and `local/assets/`
   (WAV sounds, PNG/JPEG bitmaps, the Shockwave 3D scenes as GLB). Takes a few minutes.
3. To look at a scene: `python -m http.server 8765` in this folder, then open
   `http://localhost:8765/tools/viewer.html?glb=local/assets/level1/w3d-7587.glb`.

## Switch build
Needs devkitPro with `switch-mesa`, `switch-glad`, `switch-glm`, `switch-ffmpeg` and `switch-freetype`.
1. `python tools/bake.py` turns `local/assets/level1/` into `romfs/` (level, car models, textures, HUD bitmaps, sounds, menu art, videos).
2. `make` builds `speedracer-nx.nro` with `romfs/` inside it (close the emulator first, it locks the file).
3. Run it in an emulator (`eden.exe speedracer-nx.nro`) or send it with nxlink.

Level 1 is playable from the starting grid to the finish line: 19 opponents (the last one is Duggery),
pickups, all seven gadgets, damage, oxygen, score, place, the original HUD and sounds, then a result
screen. Best score, the bonus wallpaper and solved puzzles are kept in `speedracer.sav` on the SD card.

| Button (default) | Action |
|---|---|
| ZR | accelerate (also starts the race, and restarts from the result screen) |
| ZL | brake / reverse |
| Left stick or d-pad left/right | steer |
| A | jacks (jump) |
| B | belt (grip) tyres |
| Y | cutter blades |
| R | homing robot |
| D-pad up | lights |
| D-pad down | frogger mode (periscope and canopy) |
| Minus | deflector (sound on/off on the menu pages) |
| Left stick click (L3) | show/hide the help panel |
| Plus | back to the main page (quit from the main page) |
| Minus, held 1.5 seconds | settings menu (Dear ImGui): controls, audio, interface, debug; L and R change tab, B or another long hold closes |

Every one of these buttons except steering and Plus can be changed in the settings menu (Controls tab, **Buttons**), including the button
that opens the settings. A button belongs to one action: giving it to another swaps them. The menu button may share a
button with a press action (as Minus does with the deflector): a quick press does the action, a long press opens the menu.

## Tools
- `tools/w3d.py` - Shockwave 3D (W3D/IFX) parser and CLOD mesh decoder, exports GLB. Ported from
  [dirplayer-rs](https://github.com/igorlira/dirplayer-rs) (GPL-3.0), with `tools/ifx_bitstream.py`.
- `tools/w3d_blocks.py` - lists the blocks of a W3D file.
- `tools/dirassets.py` - Director sound and bitmap members -> WAV / PNG / JPEG.
- `tools/viewer.html` - three.js GLB viewer.
- `tools/bake.py` - GLB scenes and media -> the files the Switch build loads from `romfs/`.
- `tools/swfdump.py` - lists the tags of the Flash movies inside a Director movie and extracts their bitmaps.

See NOTES.md for findings and the plan.

More debug switches: `speedracer_jackloop` (with autodrive + hold: tap the throttle once, then jacks on every landing).

## Settings menu

Holding Minus for 1.5 seconds opens a Dear ImGui menu over the front end and the race (the game stands still while it is open; L and R change tab). Five tabs:
**Controls** (D-pad and stick steering, dead zone, invert, throttle always on, and the button for every action),
**Audio** (mute, master volume), **HUD** (Original (centred 4:3) or Full (against the screen edges),
the control hint at the start of the race, skip the opening animation, frame-rate and car-stats overlays) and **Cheats** (godmode, infinite oxygen, never fall off, unlimited gadgets, jacks in mid-air and their power, top speed, turbo, fly, gravity, ghost car, rival speed, pickup reach) and **Debug** (autopilot,
game speed, start at a path point, restart race, per-frame wheel log). Settings are kept in
`sdmc:/speedracer.cfg`. The touch screen works in the settings menu (finger-sized controls, a **Close** button) and on the
front-end pages: tap PLAY!, the tabs, the clips, the wallpapers, the mini-game pieces and their buttons, the SOUND switch. Dear ImGui 1.91.5 (MIT) lives in `src/third_party/imgui`.
