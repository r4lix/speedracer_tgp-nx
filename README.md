# Speed Racer: The Great Plan - revival

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

The front end follows the original main movie: the RACE!, VIDEO CLIPS, WALLPAPER and MINIGAMES tabs,
ABOUT THE MACH 5 (the seven steering-wheel buttons, a turning 3D car and each gadget's demo video) and
HOW TO PLAY. The original pages were Flash; here each page is drawn over a screen capture of the
original (`local/ref/*.png`, see below) with live parts on top: videos, the 3D car, the selection frame,
pad controls. In menus: d-pad or stick moves, A selects, B goes back, Minus mutes.

Differences from the original menu:
- Wallpapers are shown full screen instead of being set as a desktop background. The third one
  (Duggery) unlocks as in the original: win with 30000 points.
- Mini-games are rebuilt for a pad. Sliders is the same 4x4 sliding puzzle. Jigsaw is a swap puzzle
  on a 6x5 grid of square pieces, not free-form pieces dragged with a mouse. Solving one plays a
  related episode clip; the original revealed a video inside the puzzle.
- No high-score submission, buy button or print controls (the web services are gone); QUIT takes
  the print button's place.
- Result screens use captures of the original WIN and LOSE pages (the original shows the same page
  for losing and for wrecking the car). The lose page opens on a still of the original's cartoon
  panel instead of the animation with its sound; the win page's opening animation was not captured.
- Which episode clip sits behind which button is taken from the order of the file names in the
  Flash movie and may be off.

To make the page captures, run the original once on the PC and click through its pages
(`switch/tools/run-original.py` starts it on current Windows, see NOTES.md), saving each page as
`local/ref/t_main.png`, `t_clips`, `t_wall`, `t_mini`, `t_about`, `t_how`, `a_A`..`a_G`, and the
result pages `r_win`, `r_lose`, `r_die_anim`.
Without them the pages fall back to plain text labels.

The opening animation (`media/intro.flv`, A or B skips it) is built the same way as the result cartoons:
the intro movie is `XMED-4412` of the movie inside the projector exe (`tools/swfcapture`
renders its frames, `tools/introvideo.py` mixes its sounds); the Switch icon is the last frame of it.

The cartoons that open the result screens are optional too. To build them: `python tools/swfstrip.py` the
LOSE and DIE movies, render every movie's frames with the capture page in `tools/swfcapture`
(see `tools/cutscene.py`), run `python tools/cutscene.py`, then bake again. Without them the result
screen shows a still panel.

Debug aids, as empty files on the SD card root: `speedracer_autodrive` (the car follows the path
points by itself), `speedracer_nodamage`, `speedracer_gadgets` (fires each gadget on a schedule), `speedracer_menudemo` (walks through the menus),
`speedracer_hold` (with autodrive: wait on the start line, for comparing against the original),
`speedracer_nointro` (skip the opening animation),
`speedracer_throttle` (with autodrive: hold the throttle, no steering), `speedracer_straight` (with autodrive: steer to y 1500, then hands off), `speedracer_hoverlog` (per-frame hover numbers),
`speedracer_result` containing 1, 2 or 3 (with autodrive: end the race after two seconds as won, lost or wrecked),
and `speedracer_startpath` containing a path point number (1..209) to start from.
The game writes `speedracer.log` there.

Code: `src/game.cpp` is the port of the level's Lingo (PREP, PLAY, doMach5, doEnemy, handlePickups,
doCamera, HUD scripts), `src/physics.cpp` stands in for Havok, `src/render.cpp` draws, `src/audio.cpp`
mixes the eight sound channels, `src/shell.cpp` is the front end, `src/video.cpp` plays the FLV clips.

## Tools
- `tools/w3d.py` - Shockwave 3D (W3D/IFX) parser and CLOD mesh decoder, exports GLB. Ported from
  [dirplayer-rs](https://github.com/igorlira/dirplayer-rs) (GPL-3.0), with `tools/ifx_bitstream.py`.
- `tools/w3d_blocks.py` - lists the blocks of a W3D file.
- `tools/dirassets.py` - Director sound and bitmap members -> WAV / PNG / JPEG.
- `tools/viewer.html` - three.js GLB viewer.
- `tools/bake.py` - GLB scenes and media -> the files the Switch build loads from `romfs/`.
- `tools/swfdump.py` - lists the tags of the Flash movies inside a Director movie and extracts their bitmaps.

See NOTES.md for findings and the plan.
