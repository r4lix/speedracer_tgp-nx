# Speed Racer: The Great Plan - revival

Goal: native reimplementation (PC first, then Nintendo Switch via nxvk) of the 2005
Iron Monkey / Blit Interactive game. **Bring your own copy**: this repo contains only our code.

Game source (archived): https://archive.org/details/SpeedRacerDemonOnWheels
File: `Speed Racer Demon on Wheels The Great Plan Full.zip` (24,370,170 bytes, MD5 13e04edf49e8eb7ac981fb5f5d83e341)
Tool: ProjectorRays v0.2.0 https://github.com/ProjectorRays/ProjectorRays/releases (Lingo decompiler/chunk dumper)

## Setup
1. Put the zip at `local/dl/full.zip` and ProjectorRays at `local/dl/projectorrays-0.2.0.exe`
   (already there if you copied this whole folder).
2. `tools/extract.sh` -> `local/out/` (chunks + `.ls`/`.lasm` Lingo).

See NOTES.md for findings and the plan.
