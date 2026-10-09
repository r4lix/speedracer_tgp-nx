#!/usr/bin/env bash
# Rebuilds local/ from the archive.org zip. Needs 7-Zip (Deflate64) and ProjectorRays.
# Usage: tools/extract.sh   (expects local/dl/full.zip and local/dl/projectorrays-0.2.0.exe)
set -e
cd "$(dirname "$0")/.."
Z7="${Z7:-/c/Program Files/7-Zip/7z.exe}"
PR=local/dl/projectorrays-0.2.0.exe
"$Z7" x -y -olocal/game local/dl/full.zip >/dev/null
G="local/game/Speed Racer Demon on Wheels The Great Plan Full"
for m in SR_MAIN:main SR_level_1:level1; do
  mkdir -p "local/out/${m#*:}"
  "$PR" decompile "$G/${m%%:*}.dcr" -o "local/out/${m#*:}" --dump-scripts --dump-chunks
done
echo "done: local/out/{main,level1}"
