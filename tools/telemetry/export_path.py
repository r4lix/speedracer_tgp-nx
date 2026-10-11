"""Write local/path_points.json (the level's path_NN markers) for orig-drive.py. Run from the repo root."""
import json
import sys

sys.path.insert(0, "tools")
import bake  # noqa: E402

g = bake.Glb(bake.SRC / "w3d-7587.glb")
pts = {}
for n in g.j["nodes"]:
    nm = n.get("name", "")
    if nm.lower().startswith("path_") and nm[5:].isdigit():
        m = n.get("matrix")
        pts[int(nm[5:])] = (m[12], m[13], m[14]) if m else tuple(n.get("translation") or (0, 0, 0))
json.dump({k: pts[k] for k in sorted(pts)}, open(bake.ROOT / "local" / "path_points.json", "w"))
print(len(pts), "path points")
