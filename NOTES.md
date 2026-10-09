# Findings (2026-10-09)

- `SR_GAME.exe` = stock Director MX 2004 projector (DIRAPI/IML32, Xtras: Shockwave 3D Asset, Havok, Flash Asset, DirectSound/MacroMix, fullscreen). No game logic in the exe -> recompiling is pointless.
- Game = `SR_MAIN.dcr` (menus/shell) + `SR_level_1.dcr` (the level) + `MEDIA/` (control_*.flv, gp*.flv cutscenes, JIGSAW.dcr and SLIDERS.dcr mini-games). Director 10.1#11, Afterburner (XFIR) format.
- Level 1 chunks: 193 CASt, 79 ALFA, 63 snd, 63 ediM, 39 BITD, 21 Lscr, 6 XMED. The Shockwave 3D scene is `XMED-7587.bin` (2.55 MB, magic `3DEM` then `IFX`, i.e. W3D / Intel-IFX container).
- Lingo: 30 scripts, ~4000 lines, decompiles cleanly. Key: `BehaviorScript 25 - doMach5` (1071 lines, car), `26 - doEnemy`, `MovieScript 29 doCamera`, `33 handlePickups`, `34 doUpdateScore`, `37/38 doDisplayDamage/Oxygen`, `19 INIT`, `20 PREP`, `21 PLAY`, `2 - Havok Physics`.
- Car physics: Havok rigid body (`pChassisRB`, `pCarMass` 30000) with hover/raycast wheels (`pStrength` 3.0, `pDamping` 0.05, `pHoverDist` 1.2, `pMaxSpeed` 69, `pAccGain` 0.1, `pDecGain` 0.02, `pBrakeGain` 0.3). Surface effects keyed by track face (grass, dirt, mud, snow, water, deep water). Gadgets: blades, grip, lights, scope, jacks, shield, robot. Controls (archive.org page): arrows to drive, A/space jacks, B/2 grip, C/3 blades, D/4 deflector, E/5 lights, F/6 frogger, G/7 homing robot.

# Plan
1. W3D (IFX) parser: meshes, textures, node tree, animation -> export OBJ/glTF, view it.
2. Asset extractor: snd chunks -> WAV, BITD/ALFA -> PNG, FLV playback.
3. Reimplement gameplay in C++ from the Lingo (car, enemies, pickups, oxygen, camera, HUD); replace Havok with simple rigid body + ray suspension.
4. PC build (SDL + GL/Vulkan), then Switch via nxvk.
5. Publish code only (BYO copy).

# Rules
- Never commit game files, extracted assets or decompiled Lingo (`local/` is gitignored).
