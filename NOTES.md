# Findings (2026-10-09)

- `SR_GAME.exe` = stock Director MX 2004 projector (DIRAPI/IML32, Xtras: Shockwave 3D Asset, Havok, Flash Asset, DirectSound/MacroMix, fullscreen). No game logic in the exe -> recompiling is pointless.
- Game = `SR_MAIN.dcr` (menus/shell) + `SR_level_1.dcr` (the level) + `MEDIA/` (control_*.flv, gp*.flv cutscenes, JIGSAW.dcr and SLIDERS.dcr mini-games). Director 10.1#11, Afterburner (XFIR) format.
- Level 1 chunks: 193 CASt, 79 ALFA, 63 snd, 63 ediM, 39 BITD, 21 Lscr, 6 XMED. The Shockwave 3D scene is `XMED-7587.bin` (2.55 MB, magic `3DEM` then `IFX`, i.e. W3D / Intel-IFX container).
- Lingo: 30 scripts, ~4000 lines, decompiles cleanly. Key: `BehaviorScript 25 - doMach5` (1071 lines, car), `26 - doEnemy`, `MovieScript 29 doCamera`, `33 handlePickups`, `34 doUpdateScore`, `37/38 doDisplayDamage/Oxygen`, `19 INIT`, `20 PREP`, `21 PLAY`, `2 - Havok Physics`.
- Car physics: Havok rigid body (`pChassisRB`, `pCarMass` 30000) with hover/raycast wheels (`pStrength` 3.0, `pDamping` 0.05, `pHoverDist` 1.2, `pMaxSpeed` 69, `pAccGain` 0.1, `pDecGain` 0.02, `pBrakeGain` 0.3). Surface effects keyed by track face (grass, dirt, mud, snow, water, deep water). Gadgets: blades, grip, lights, scope, jacks, shield, robot. Controls (archive.org page): arrows to drive, A/space jacks, B/2 grip, C/3 blades, D/4 deflector, E/5 lights, F/6 frogger, G/7 homing robot.

# W3D format (2026-10-10)

- XMED = `3DEM` wrapper + `IFX\0` file: 4-byte-aligned blocks of `type u32, size u32, payload`. Types seen: 72 model node (name, parent, 4x4 matrix with translation in elements 12..14, resource, visibility, shader), 74 view, 71/50 light node/resource, 10 material, 36 lit-texture shader, 45 model resource (mesh counts, shader bindings, inverse-quantisation factors), 47 sync table, 49 compressed geometry, 20/21 texture declaration/data (plain JPEG).
- Geometry (49) is an arithmetic-coded progressive mesh (CLOD), same coder as U3D / ECMA-363. dirplayer-rs (GPL-3.0) has a working decoder; `tools/ifx_bitstream.py` + `tools/w3d.py` are a Python port of it. Result: all 760 level-1 resources decode to exactly their declared vertex/face counts.
- Level 1 (`XMED-7587`): Z-up, one point-to-point track ~11,000 units along +Y, 763 nodes, 88k verts / 67k faces, 29 textures. Node name prefixes: `SEG` road segments, `WALL`, `RAIL`, `WATER`, `MISC`, `Tree` (355), `INV` invisible colliders, `path_NN` (209 AI/progress waypoints), `pickup_{CandyRed,CandyPurple,Wrench,Oxegen,Robot}NN`, `carPos_Mach5` spawn, `Sphere01` sky dome.
- Other scenes: level1 `XMED-7589` and main `XMED-5143` = the Mach 5 (`collider`, `mach5_body`, 4 wheels, 4 grip tyres, `jacks`, `saw`, `scope_*`, `lights`, `windshield`, `ROBOT`, `car_shadow`); level1 `XMED-7591` = the enemy car (`car_body`, `car_bodyDuggery`, wheels, `robot`). The remaining XMEDs are Flash (`FWS`) UI.
- Director members: `snd ` = Mac snd resource, 16-bit BE PCM 22050 Hz mono; bitmaps are either `BITD` (32-bit, PackBits, per-row A/R/G/B planes) or `ediM` JPEG + `ALFA` (PackBits 8-bit alpha). Chunk -> member via `KEY_` (little endian), name from the `CASt` info list. The four 8-bit window buttons are skipped (unused in a port).

# Havok Xtra (2026-10-10)

Source: `Havok.x32` (573,440 bytes, sha256 `90694ddbad361fbf...`), one of 12 zlib-packed Xtras in the projector's `XFIR`/`LPPA` container; unpack with `switch/tools/projector_xtras.py` into `local/out/xtras/`. Read with Ghidra (project `switch/tools/ghidra-projects/speedracer`, program `Havok.x32`, functions named). It is Havok 1.x: HKE scene importer, Euler and RK45 solver classes.

- `havok.step(timeStep, subSteps)` (0x1000d083): `simTime += timeStep`, then each subspace runs `h = (simTime - t) / subSteps` and takes `subSteps` substeps of `h` (0x10017560).
- One substep (0x100175c0) is tentative: integrate, run actions (drag, dashpots, springs), detect collisions. On interpenetration it restores state and halves the step (floor 1e-5, at most 30 retries); otherwise it resolves contacts and commits. No fixed-penalty contact model.
- Rigid body fields: `+0x50` linear velocity, `+0x60` angular velocity, `+0xbc` mass, `+0xc0` inverse mass, `+0x18c` 3x3 matrix used for angular impulses (inverse inertia; world vs body space not confirmed), `+0x1bc` force accumulator, `+0x1cc` torque accumulator.
- `applyImpulse(J)`: `v += invMass * J`. `applyAngularImpulse(J)`: `w += M(+0x18c) * J`. `applyForce(f)`: adds to the accumulator. All three wake the body. Linear inputs are multiplied by the world scale, the angular impulse is not; velocity getters divide by the scale.
- Drag action (0x10075c30): `force -= linearDrag * v`, `torque -= angularDrag * w` (not mass-scaled). A blank world gets drag (0, 0) and gravity (0, 0, -9.81); gravity set from Lingo is multiplied by the world scale.
- `deactivationParameters = [a, b]` is stored as `1/a`, `1/b` on the deactivator.
- Angular dashpot, one body (0x1001d2d0), per substep with `k = 200 * h`: angular impulse `J = -(k * strength * angle * axis + k * damping * w)`, where angle/axis is the rotation between body orientation and the target. Defaults from `makeAngularDashpot`: strength 1.0, damping 0.1 (the game sets strength = mass * tilt%, damping 1000).
- Integrator: a blank world creates solver type 1 = explicit Euler (0x10057f10, step 0x100581c0: `y += h * dy`); RK45 is only reachable through an HKE `ODE_SOLVER` entry. State per body is 13 floats: position, quaternion, linear momentum, angular momentum. Derivatives (0x100147c0): `dx = v`, `dq = 0.5 * w (x) q` (w in world frame, checked numerically), `dP = force`, `dL = torque`. Quaternion is renormalised after every step.
- Quirk, as decompiled and not checked at runtime: the integrator maps `L <-> w` with the body-frame inertia (`+0xc4`) and inverse inertia (`+0xf4`) without rotating them, while `applyAngularImpulse` and the dashpot use the rotated inverse inertia (`+0x18c = R * Iinv * Rt`, rebuilt each step). With zero torque this means `w` stays constant between impulses: no gyroscopic wobble.
- Inertia (0x10015990): each primitive gets exact uniform-density polyhedron integrals of its collision geometry (Mirtich's volInt: 0x1001e470, 0x1001e750) with density = mass / volume; a sphere gets `2/5 m r^2`; primitives are combined about the common centre of mass by the parallel-axis rule and inverted. `#box` builds an 8-vertex bounding box of the model, `#sphere` a sphere; the default is the model's mesh. Mass 0 makes a fixed body.
- Mach 5 (`collider` in `w3d-7589.glb`, 104 verts, closed, mass 30000), from the raw mesh: volume 1.646, centre of mass (0.000, -0.076, 0.156), inertia diag (25769, 6992, 32311), off-diagonals under 25. The mesh is slightly concave (0.099 units) and the game passes isConvex = 1, so Havok may use its hull: expect values between these and the solid bounding box (29336, 7820, 36673). World scale is not applied here (lengths x s gives inertia x s^2).
- Score values (level 1, `VWSC-8369`, behaviour property lists are stored as plain Lingo text; all five are dumped to `local/out/level1/score_behaviour_properties.txt`): Havok behaviour `pTolerance 0.45, pTimeStep 0.0333, pSubSteps 5, pScale 1.0`. So one `step(0.0333, 5)` per frame, substep 0.00666, scale 1 (the scale factors above are all no-ops), gravity (0, 0, -90.81). Movie frame rate in `DRCF` is 30 and no script changes the tempo, so 0.0333 per frame is roughly real time; the score's tempo channel was not parsed.
- Havok member (`CASt-5`, "Havok Physics Scene"): `KEY_` lists no media chunk for it and the script calls the 3-argument `Initialize(member, tolerance, scale)`, so the world is blank: Euler solver, drag (0, 0), deactivator (5.0, 0.1) which INIT sets again to the same values. The Xtra only sets its has-HKE flag when media is imported (0x1000748c).
- Car properties in the score are mostly dead: `beginSprite` overwrites them. Mach 5 score says mass 1200, strength 6, damping 0.2, hover 0.75, max speed 85; the script sets 30000, 3.0, 0.05, 1.2, 69. Enemies: score mass 800, script 20000 and max speed = 0.8 x Mach 5. Still taken from the score: `pGrip 0.5`, `pDrag 0.1`, `pSourceMember`, model names, `pCarName`, enemy `pStartingPathPoint`/`pStartingPathStep` (3/2, 2/3, 1/4), Mach 5 `pCameraCar 1`, `pUserCar 1` (check each against the script before relying on it).
- Not determined yet: what isConvex does to the geometry (hull or not), friction/restitution combination, `registerInterest` thresholds, `attemptMoveTo`, the score tempo channel.

# Port findings (2026-10-10)

- Frame order that matches the original: `updateHover` (enterFrame), Havok step, then `updateCar` (exitFrame). With the step last the wheels trail the car by one frame (2 units at full speed).
- `applyForce` only acts during the first of the 5 substeps (Havok clears the accumulator after each one), which would explain why the script multiplies the drive force by 60 (read from the decompilation, not checked against the running game; with it the car reaches top speed in a few seconds). `applyImpulseAtPoint(J, p)` takes `p` in model space: `r = R * (p - centerOfMass)`.
- Hover rays use `#maxDistance: 1` although `pHoverDist` is 1.2 (the port now casts pHoverDist, see "Hills" below). The car settles about 0.83 above the road. The spawn ray asks for 2 units but `carPos_Mach5` is 3.6 above the track, so the port casts further.
- Track surface = mesh index inside each `SEG_nn` model (1..16): 1 finish, 2 brown road, 3 grass, 4 dirt, 5 brown rock, 6 blue road, 7 dark blue road, 8/9 snow, 10 blue dirt, 11 blue rock, 12 underwater sand, 13 blue road variant, 14/15 ice, 16 tree.
- Shockwave 3D textures are a JPEG followed by an optional alpha plane `{u32 w, u32 h, u32 size, zlib}`; `w3d.py` keeps it glued to the JPEG, `bake.py` splits it (5 textures: car shadow, blades, light beam, icon shadow).
- Car parts are placed with the collider's transform, ignoring each node's own offset; `scope_door` and `ROBOT` keep a node scale (0.00056, 0.00129). The round thing behind the windshield is the driver's helmet.

- Level 1 design notes found while testing: hits cost `min(30, closing speed * 10)` damage with a one-second cooldown, so ramming opponents wrecks the car in four hits; wrenches give back 50. The mountain section around y 5800-6000 is ice hairpins with no walls, meant for the grip tyres. Opponents are three cars recycled 19 times: one that falls 40 units behind (or explodes) reappears two path points ahead of the leading opponent.
- Pickups and trees are detected with the bounding sphere of the `saw` model (centre 2.3 ahead of the car, radius about 2), not with the chassis.
- The car shadow texture really is a soft-edged rectangle.
- Missing from the extracted data: `drive_loop` (engine loop), `enemy_scream3`, `music_gameover` and the `TITLE_1` card; the port skips them.

- Main movie (`SR_MAIN.dcr`): its menu, WIN, LOSE and DIE screens are Flash 7 movies in `XMED` chunks (12-byte header, then `FWS`). They are mostly vector shapes and text, but carry bitmaps (45 in the menu) that `tools/swfdump.py` extracts. The FLVs are menu extras, not cutscenes: `control_a..g.flv` are the how-to demos of the seven gadgets (100x80), `gp*.flv` the nine cartoon clips (215x161); all Sorenson H.263 with 22 kHz mono MP3. ffmpeg needs a `file:` prefix to open `romfs:/` paths.

- Collision shape (2026-10-10): `bake.py` fills the `collider` slab with 70 spheres of radius 0.155 (half its thickness): a rim that follows the convex outline plus a coarse inside grid. Each sphere is also swept from its old to its new centre every substep, because at 69 units/s a substep moves 0.46 units, more than a sphere's diameter. Still not Havok's hull-vs-mesh test, and friction and restitution are zero everywhere.
- Particles: the three `#particle` resources are ported as billboards (`pDirtKick` spray behind the car on dirt, snow and under water, opponent smoke from 30 damage, the explosion burst). Assumed units, not confirmed against the original: emitter speeds in units per second, gravity applied per frame, wind ignored (Shockwave only applies wind with drag, which the scripts never set). The explosion uses 48 particles instead of the script's 150.

# Running the original on current Windows (2026-10-10)

- `SR_GAME.exe` crashes about 15 seconds in, when the menu loads, with an access violation at `iml32.dll+0x6cb9a` (a list unlink in Director's memory manager). No compatibility mode helps.
- Cause: heap overflow in `Shockwave 3D Asset.x32`, routine at `+0xfa1af` (a display-driver info query). It allocates 0x104 bytes, calls `GetSystemDirectoryA`, then `strcat`s the driver file name from the registry to read its version. Current NVIDIA drivers list four full `DriverStore` paths there (about 390 characters).
- Workaround: `switch/tools/run-original.py` starts the game under the debug API, waits for the Xtra to load, checks the routine's first bytes and replaces them in memory with `xor eax,eax / ret 0x10` ("no driver info"), then detaches. Nothing on disk changes. With it the menu, the mini-games and their windows all run.
- `switch/tools/crashstack.py` is the small debugger used to find this (crash registers, stack scan as module+offset, memory around the faulting argument).
- The game only reacts to the real mouse, not to posted window messages.

# Original menu, as observed running

- Main page: tabs RACE!, VIDEO CLIPS, WALLPAPER, MINIGAMES; PLAY!; links ABOUT THE MACH 5, HOW TO PLAY, PRINT CONTROLS; VIEW HIGH SCORES.
- VIDEO CLIPS: "Episode 1, The Great Plan", eight clips named MACH 5, GO!, POPS, DECISION, SECRET, GARAGE, GET SET, DUGGERY, played in a screen on the page.
- WALLPAPER: two desktop wallpapers with SET buttons (1280x1024 JPEGs in the main cast; a third, Duggery, is the win reward).
- MINIGAMES: Jigsaw (pictures Mach 5, Duggery, Spritle & Chim Chim) and Sliders (Speed Jumps, Chim Chim & Spritle, Trixie; 4x4, move counter, SCRAMBLE, HINT). Each is its own Director movie wrapping one Flash movie, opened in a second window; the pictures are the three `DefineBits` JPEGs inside (they need the movie's `JPEGTables`).
- ABOUT THE MACH 5: the steering wheel's buttons A-G; each shows a text, the gadget's demo video and the car turning with that gadget out.
- HOW TO PLAY: key list, objective, the four pickups, THE MACH 5 IN ACTION (a clip; assumed to be `gp2.flv`, the one file no other page references).

- Result pages, as observed: wrecking the car fades to black, then a cartoon panel of Pops shouting at Speed, then "YOU EARNED A SCORE OF n", a SUBMIT button, MAIN MENU / PLAY AGAIN and the locked Duggery wallpaper ("win with a score over 30,000"). WIN says "YOU WON! AND EARNED A SCORE OF n" over a different backdrop. LOSE looks the same as the wrecked page.
- How they were reached without driving a lap: the level's Lingo literals sit in memory as {u32 length, text}; `switch/tools/memscan.py` rewrote `SR_MAIN.dcr#DIE` to `SR_MAIN.dcr#WIN` (same length) or `SR_MAIN#LOSE` (shorter; Director finds the movie without its extension), then the car was driven into the scenery with the Up key held (`win-key.ps1`). Holding Up from the grid wrecks the car in about 80 seconds.
- Texture coordinates: W3D texture coordinates are centred on the image (a quad showing a whole texture runs -0.5..0.5). `bake.py` shifts them by half a texture; `w3d.py` leaves them as stored. An earlier attempt flipped v instead, which made the Mach 5 white by accident but put the M on the tail and the shadows as hard black boxes. Checked against the original on the start line: body, plate, wheels, shadow and the road's lane lines now line up. Tiling textures hide the mistake.
- Camera: `doCamera` as ported already matches. On the start line the Mach 5 covers the same 170 of 512 stage pixels in the original and in the port. The camera falls back as speed rises (in both), so shots taken at different speeds look like different cameras. Not compared at a known equal speed.
- HUD: the original stage is 4:3 with the HUD against its edges. The port draws 16:9, and each HUD element keeps its distance to its own edge (`Overlay::anchor`); `Interface_Info` is split at x = 400 for that. The help panel sits at the left edge like `ovHelp` (8, 180), listing pad buttons instead of keys.
- Result cartoons: the three result movies in SR_MAIN are `XMED-5198` (LOSE, 141 frames), `XMED-5204` (DIE, 120) and `XMED-5200` (WIN, 161 at 31 fps). Each opens on an animation (Pops shouting at Speed with lip-synced speech as a streaming MP3 from frame 26; the family cheering with an event sound at frame 10), fades to red, then shows the page and starts a quiet looping tune (envelope up to 15 % volume). The port plays them as FLVs: frames rendered by Ruffle 0.7.1 (web build) stepped on a fake `requestAnimationFrame` clock by `tools/swfcapture`, sound cut from the movies, joined by `tools/cutscene.py`. Ruffle follows a streaming sound's real-time position (it skips frames with audio, stalls without an audio device), so those two movies are captured from copies without the stream tags (`tools/swfstrip.py`). Ruffle's own `exporter` could not be built here: the Rust MSVC toolchain has no Windows SDK to link against. Which of 5198/5204 is LOSE and which DIE is taken from an earlier note, not re-checked; both show Pops.

# Plan
1. ~~W3D (IFX) parser: meshes, textures, node tree -> glTF, view it.~~ Done: `tools/w3d.py`, `tools/viewer.html`. No animation/bones blocks exist in these files.
2. ~~Asset extractor: snd chunks -> WAV, BITD/ALFA -> PNG.~~ Done: `tools/dirassets.py`. FLV cutscene playback still open.
3. Reimplement gameplay in C++ from the Lingo (car, enemies, pickups, oxygen, camera, HUD); replace Havok with simple rigid body + ray suspension.
4. ~~Switch build.~~ Done 2026-10-10 with OpenGL 4.3 through mesa/EGL (not nxvk): level 1 is playable start to finish in Eden at about 55 fps (see README for what is and is not ported). PC build skipped. Next: a play-through with a real controller.
5. Publish code only (BYO copy).

# Rules
- Never commit game files, extracted assets or decompiled Lingo (`local/` is gitignored).
- `tools/ifx_bitstream.py` and `tools/w3d.py` are derived from dirplayer-rs, so they are GPL-3.0-or-later. Publishing them means the repo (or at least those tools) ships under GPL-3.0; a C++ runtime that only loads the converted GLB is not affected.

- Startup intro: the projector exe holds its own small Director movie (an XFIR at offset 1671299 inside the data after the PJ01 header at 0x288159, 209 KB; `ProjectorRays decompile --dump-chunks` works on it) whose only real content is `XMED-4412`, a 705-frame Flash movie at 30 fps with events for the engine loop (frame 53 to 300), a gear sound, a voice and the theme (frame 301, 73 % volume), ending on `gotoLoad()` and the SPEED RACER card over a wheel. Rendered through Ruffle as for the result cartoons; `tools/introvideo.py` mixes it. The card is also what `bake.py` crops (wheel hub centred at 268,208 of the 512x384 stage) for the Switch icon, `local/ref/intro_card.png` (a screenshot of the original running).

## Hills, measured (2026-10-11)

- Telemetry from the original: `tools/telemetry/floatscan.py` finds the car's (x, y, z) in the running game (three floats that
  start at the grid position (42.0, 38.8, 13.8-14.0)) and reads it at 20-30 Hz; `orig-drive.py` presses Left/Right/Up on the
  real keyboard to follow the level's path points while sampling (run it from a normal PowerShell so the mouse click and key
  events reach the game; from other shells they were dropped). The memory holds many stale copies, so it picks the address whose
  y grows once the throttle is down and keeps growing 0.4 s later.
- Original on flat road: z stays at 13.82 (+-0.02) at any speed, the car never bobs. Speed from rest: about 50 u/s after 1.3 s,
  55-65 u/s cruising on the plateau, up to 75 u/s on the first descent (y 740-890, z 13.8 -> -4.5). Climb at y 1110-1310
  (z -4 -> 26, up to 14 degrees): speed falls from 63 to 37-50 and recovers on the flat top. z(y) tracks the road closely.
- Port on the same stretch (autopilot from path point 13): same z(y) within about 0.5 units, same speeds (60 -> 48-53 on the climb,
  63-70 on the descent). Differences found: (1) the sphere collider touches the track at the foot of the first steep climb (impact
  speed 12 u/s at y 1128, `trk` in the log) and the car is thrown 5 units up; the original shows no such jump, its z rises smoothly;
  (2) with only some wheels in range the port drifts through 0.25-0.75 "power coefficient" on slopes, in the original z is smooth
  to 0.06 per half second; (3) standing still on a slope the port settles into a 17-frame hover bounce (d 0.84 <-> 1.0, the ray's
  limit) instead of coming to rest, where on flat ground it settles within 60 frames.
- Debug switches added for this: `speedracer_throttle` (autodrive: hold the throttle, no steering), `speedracer_hoverlog` (one line per
  frame with z, vz and each wheel's hover distance), `speedracer_nointro`, `speedracer_result`.
- Hills, fixed (2026-10-11): with a 1.0 ray the port bounced on slopes, a third of the samples on the first hills (y 700-1330)
  had fewer than four wheels in contact, and the valley at y 985 (road z about -10.7, cliff test `chassisPos.z <= -10` and no wheel
  contact) ended the race when the car was airborne for a frame. The original's z(y) there is smooth and its car stays on the
  road. Casting `pHoverDist` (1.2, which is what the script's `hoverDst < pHoverDist` test implies) removes the partial contacts
  (33 % -> 0 %) and keeps z(y) within 0.1-0.4 of the original's along the whole stretch. Flat-road ride height is unchanged
  (z 13.8). The 1.0 value in the script may be an unreachable `#maxDistance`, not a measured one; if some place needs the
  shorter ray it can be reverted in `Vehicle::hover`.
- Long descent (y 1500-1850, z 26 -> -1.7, 11 degrees), original vs port with the 1.2 ray: z(y) agrees within 0.3 along the whole
  drop (original 1595: 22.0, 1678: 11.2, 1744: 0.7; port 1601: 21.3, 1673: 11.9, 1751: -0.3) and the port keeps four wheels on the
  road throughout (pitch -3..-10, never airborne). Speed is not comparable yet: the port's autopilot reaches 80-82 u/s at the bottom;
  the original driver in two runs (steering with key presses, throttle capped at 48 for the first run and uncapped down the
  descent in the second) stayed at 38-53 u/s, which may be lost to its constant steering corrections or may be a real difference in
  how speed builds up on a descent. Unresolved; `orig-drive.py` now follows the path polyline with a speed-dependent lookahead and
  gets through the climb and the descent (to y 2100) reliably.
- Full lap (2026-10-11): the autopilot drives start to finish with `speedracer_autodrive` + `speedracer_nodamage` in about 8420
  frames (4 minutes of game time): through the first hills, the ice hairpins at y 5800-6000 (it now keeps to 24-32 u/s on snow and
  ice; at 40 it fell off at y 5885), the bridge/ramp near the end and the finish line at y 10921, logging "finished in place 18
  with 15500 points" and playing the lose cartoon. Not tested: a finish in first place (the win page and the 30000-point bonus
  unlock) because the autopilot is slow, and the race with damage on.
- Jacks and landing (2026-10-11): the script's spring impulse is `diff * f * mass` with `|diff|` = the hit distance d, so it fades
  out as the car comes down. Four wheels carry the car's weight only for d > 0.37; below that the car can rest on its hull
  with the wheels in the road (hull bottom 0.156 under the wheel points). A jack jump (`mass * 40`, 22 frames in the air, 4 units
  up) landing hard, especially on a downhill, ended in 40 frames of bouncing at d = 0.15-0.25 with the wheel meshes sunk in the road
  (the script also floors the visual drop at 0.75). Two deliberate deviations: the spring lever is kept at 0.6 below d = 0.6 (no
  change above, so ride height and hills are untouched), and (the wheel-mesh floor stays at the script's 0.75: 0.25 hid the wheels in the body for the last frames of every jump and they popped out on landing).
- Jack jumps drifted (2026-10-11, reported with a side-by-side against the original, where a standing car jumps on the spot):
  logging position, attitude and angular rate every frame (`speedracer_jackloop` + `speedracer_hoverlog` with autodrive and
  hold) on the flat start line showed the car pitching 15 degrees at the first landing and then tumbling in the air. The cause was
  the contact solver, not the jack: contacts were resolved one at a time without accumulated impulses, so the first sphere
  to be solved (the car's rear edge) took the whole impact, spun the body at 10 rad/s and the front spheres, which then moved
  upward, could never take it back. Fixes: all spheres that touch (with a 0.04 margin) are collected, solved in 12 Gauss-Seidel
  passes with accumulated impulses (an offline model of the same set of contacts converges to zero spin in 5 passes), the contact
  sphere set is mirrored left-right (the baked fill was 0.12 off centre), and the centre of mass sits under the middle of the
  spheres (y -0.126; the script's -0.076 is for the original's hull). Result: ten consecutive jumps on the spot, max pitch/roll 0,
  position change under 0.3 units in 10 seconds (was 4-5 units with 45 degree tumbles).
