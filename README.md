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

| Button | Action |
|---|---|
| A or ZR | accelerate (also starts the race, and restarts from the result screen) |
| B or ZL | brake / reverse |
| Left stick or d-pad left/right | steer |
| X | jacks (jump) |
| Y | cutter blades |
| L | belt (grip) tyres |
| R | homing robot |
| D-pad up | lights |
| D-pad down | frogger mode (periscope and canopy) |
| Minus | deflector |
| Left stick click | show/hide the help panel |
| Plus | back to the main page (quit from the main page) |

## Tools
- `tools/w3d.py` - Shockwave 3D (W3D/IFX) parser and CLOD mesh decoder, exports GLB. Ported from
  [dirplayer-rs](https://github.com/igorlira/dirplayer-rs) (GPL-3.0), with `tools/ifx_bitstream.py`.
- `tools/w3d_blocks.py` - lists the blocks of a W3D file.
- `tools/dirassets.py` - Director sound and bitmap members -> WAV / PNG / JPEG.
- `tools/viewer.html` - three.js GLB viewer.
- `tools/bake.py` - GLB scenes and media -> the files the Switch build loads from `romfs/`.
- `tools/swfdump.py` - lists the tags of the Flash movies inside a Director movie and extracts their bitmaps.

See NOTES.md for findings and the plan.
