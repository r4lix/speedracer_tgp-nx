# Speed Racer: The Great Plan (Switch port) - advanced notes

For building from source, extracting the assets and debugging. The player-facing readme is [README.md](README.md);
findings about the original game are in [NOTES.md](NOTES.md).

Bring your own copy of the game: this repo contains only our code.

Game source (archived): https://archive.org/details/SpeedRacerDemonOnWheels
File: `Speed Racer Demon on Wheels The Great Plan Full.zip` (24,370,170 bytes, MD5 13e04edf49e8eb7ac981fb5f5d83e341)
Tool: ProjectorRays v0.2.0 https://github.com/ProjectorRays/ProjectorRays/releases (Lingo decompiler/chunk dumper)

## Extracting the assets
1. Put the zip at `local/dl/full.zip` and ProjectorRays at `local/dl/projectorrays-0.2.0.exe`.
2. `tools/extract.sh` -> `local/out/` (chunks + `.ls`/`.lasm` Lingo) and `local/assets/`
   (WAV sounds, PNG/JPEG bitmaps, the Shockwave 3D scenes as GLB). Takes a few minutes.
3. To look at a scene: `python -m http.server 8765` in this folder, then open
   `http://localhost:8765/tools/viewer.html?glb=local/assets/level1/w3d-7587.glb`.

## Building
Needs devkitPro with `switch-mesa`, `switch-glad`, `switch-glm`, `switch-ffmpeg` and `switch-freetype`.
1. `python tools/bake.py` turns `local/assets/level1/` into `romfs/` (level, car models, textures, HUD bitmaps,
   sounds, menu art, videos, the icon).
2. `make` builds `speedracer-nx.nro` with `romfs/` inside it (close the emulator first, it locks the file;
   or `make TARGET=speedracer-dbg`).
3. Run it in an emulator (`eden.exe speedracer-nx.nro`) or send it with nxlink.

The libraries are Dear ImGui 1.91.5 (MIT, `src/third_party/imgui`), glad, glm, ffmpeg (FLV playback) and
FreeType. Rendering is OpenGL 4.3 core through switch-mesa.

## Settings file
`sdmc:/speedracer.cfg` is a plain `key=value` file written by the settings menu (R3). The keys are the fields of
`struct Settings` in `src/settings.h`: steering (`dpadSteer`, `stickSteer`, `invertSteer`, `stickDeadzone`),
buttons (`accelA`, `accelZR`, `brakeB`, `brakeZL`, `autoAccelerate`), audio (`mute`, `masterVolume`),
interface (`hudAnchored`, `showHelp`, `showFps`, `showStats`, `skipIntro`) and debug (`noDamage`, `autopilot`,
`jackLoop`, `timeScale`, `startPath`, `logHover`). Edit it by hand if you prefer.

## Debug switches
Empty files on the SD card root that `src/main.cpp` looks for at start-up (the race log goes to
`sdmc:/speedracer.log`):

| File | Effect |
|---|---|
| `speedracer_autodrive` | the car drives itself along the recorded path, the front end is skipped |
| `speedracer_hold` | with autodrive: wait on the start line |
| `speedracer_throttle` | autodrive: straight ahead, flat out |
| `speedracer_straight` | autodrive: straight ahead, hands off |
| `speedracer_jackloop` | tap the throttle once, then jacks on every landing |
| `speedracer_nodamage` | no damage |
| `speedracer_gadgets` | fires each gadget in turn |
| `speedracer_menudemo` | walks through the front end |
| `speedracer_nointro` | skip the opening animation |
| `speedracer_hoverlog` | logs position, speed and wheel state for every frame |
| `speedracer_startpath` | contains a path point number to start the race from |
| `speedracer_result` | contains a result to show on the result screen |
| `speedracer_menu` | contains a tab number: open the settings menu on that tab |

The same behaviour is available without files from the settings menu's Debug tab.

## Tools
- `tools/w3d.py` - Shockwave 3D (W3D/IFX) parser and CLOD mesh decoder, exports GLB. Ported from
  [dirplayer-rs](https://github.com/igorlira/dirplayer-rs) (GPL-3.0), with `tools/ifx_bitstream.py`.
- `tools/w3d_blocks.py` - lists the blocks of a W3D file.
- `tools/dirassets.py` - Director sound and bitmap members -> WAV / PNG / JPEG.
- `tools/viewer.html` - three.js GLB viewer.
- `tools/bake.py` - GLB scenes and media -> the files the Switch build loads from `romfs/`.
- `tools/swfdump.py`, `swftimeline.py`, `swfaudio.py`, `swfstrip.py`, `swfvideo.py` - inspect and extract the Flash
  movies inside the Director movie (tags, bitmaps, timelines, audio, video streams).
- `tools/cutscene.py`, `tools/introvideo.py`, `tools/menubg.py` - render the result cartoons, the intro and the main page's
  looping background panel to FLV (Ruffle web build driven by `tools/swfcapture/`; `swfstrip.py` makes the sound-less copy).
- `tools/exeicon.py` - pulls the icon out of the original executable.
- `tools/telemetry/` - reads the original game's physics state from process memory and drives it, to compare
  against the port.

## Controls (full list)

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
| Minus held 1.5 s | settings menu; L / R change tab, B or another long Minus closes (a quick Minus press is the deflector) |

Touch works in the settings menu and on the front-end pages (tap what the page shows); it does not steer the car.
