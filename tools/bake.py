"""Bake the converted GLB scenes into the flat binary files the runtime loads (romfs/).

Usage: python tools/bake.py            (reads local/assets/level1/*.glb, writes romfs/)

Level file (level1.srl): static geometry merged per (track segment, material) in world space,
the objects the game moves or removes (trees, pickups, water) one by one in model space, the
collision triangles (track with its surface id, solids, trees with their object index), and the
marker nodes (path points, spawn).
Model file (*.srm): every model node of a car scene with its own meshes in model space.
Textures are copied out of the GLB (JPEG) into romfs/tex/. Shockwave 3D appends the alpha channel
after the JPEG as {u32 width, u32 height, u32 size, zlib bytes}; that part is written unpacked to
<name>.alpha as {u32 width, u32 height, width*height bytes}.
Coordinates stay as authored: Z up, the track runs along +Y, 300 units per segment.
"""
import json
import os
import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "local" / "assets" / "level1"
OUT = ROOT / "romfs"
SEG_LEN = 300.0


class Glb:
    def __init__(self, path):
        d = Path(path).read_bytes()
        jl = struct.unpack_from("<I", d, 12)[0]
        self.j = json.loads(d[20:20 + jl])
        self.bin = d[20 + jl + 8:]

    def view(self, i):
        bv = self.j["bufferViews"][i]
        off = bv.get("byteOffset", 0)
        return self.bin[off:off + bv["byteLength"]]

    def acc(self, i):
        a = self.j["accessors"][i]
        bv = self.j["bufferViews"][a["bufferView"]]
        off = bv.get("byteOffset", 0) + a.get("byteOffset", 0)
        n = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}[a["type"]]
        fmt = {5126: "f", 5123: "H", 5125: "I", 5121: "B"}[a["componentType"]]
        size = struct.calcsize(fmt) * n
        stride = bv.get("byteStride", size)
        if stride == size:
            flat = struct.unpack_from("<%d%s" % (n * a["count"], fmt), self.bin, off)
            return [flat[k:k + n] for k in range(0, len(flat), n)] if n > 1 else list(flat)
        out = []
        for k in range(a["count"]):
            v = struct.unpack_from("<%d%s" % (n, fmt), self.bin, off + k * stride)
            out.append(v if n > 1 else v[0])
        return out

    def primitive(self, p):
        """-> (positions, normals, uvs, indices, material index)"""
        pos = self.acc(p["attributes"]["POSITION"])
        nrm = self.acc(p["attributes"]["NORMAL"]) if "NORMAL" in p["attributes"] else [(0, 0, 1)] * len(pos)
        uv = self.acc(p["attributes"]["TEXCOORD_0"]) if "TEXCOORD_0" in p["attributes"] else [(0, 0)] * len(pos)
        # W3D texture coordinates are centred on the image: a quad that shows a whole texture runs from -0.5 to
        # 0.5 (0.5 to 1.5 for v after w3d.py's flip). Shift them to the usual 0..1. Tiling textures hide the
        # mistake; the Mach 5 body, the plate and the soft shadow quads do not.
        uv = [(t[0] + 0.5, t[1] - 0.5) for t in uv]
        idx = self.acc(p["indices"]) if "indices" in p else list(range(len(pos)))
        return pos, nrm, uv, idx, p.get("material", -1)


def xform_point(m, p):
    x, y, z = p
    return (m[0] * x + m[4] * y + m[8] * z + m[12],
            m[1] * x + m[5] * y + m[9] * z + m[13],
            m[2] * x + m[6] * y + m[10] * z + m[14])


def xform_dir(m, p):
    x, y, z = p
    v = (m[0] * x + m[4] * y + m[8] * z, m[1] * x + m[5] * y + m[9] * z, m[2] * x + m[6] * y + m[10] * z)
    l = (v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) ** 0.5 or 1.0
    return (v[0] / l, v[1] / l, v[2] / l)


IDENT = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]


class Textures:
    """Texture names shared by all baked files; images are written once."""

    def __init__(self):
        self.names = []
        self.alpha = set()

    def add(self, glb, image_index):
        img = glb.j["images"][image_index]
        data = glb.view(img["bufferView"])
        ext = ".png" if data[:4] == b"\x89PNG" else ".jpg"
        name = "".join(c if c.isalnum() or c in "_-" else "_" for c in os.path.splitext(img.get("name", "img%d" % image_index))[0]) + ext
        if name not in self.names:
            self.names.append(name)
            (OUT / "tex").mkdir(parents=True, exist_ok=True)
            end = data.find(b"\xff\xd9", data.find(b"\xff\xda")) + 2 if ext == ".jpg" else len(data)
            tail = data[end:]
            (OUT / "tex" / name).write_bytes(data[:end])
            if len(tail) > 12:
                w, h, size = struct.unpack_from("<3I", tail, 0)
                plane = zlib.decompress(tail[12:12 + size])
                if len(plane) == w * h:
                    (OUT / "tex" / (name + ".alpha")).write_bytes(struct.pack("<2I", w, h) + plane)
                    self.alpha.add(name)
        return name


def materials(glb, textures):
    """-> list of (rgba, texture file name or '', blend flag)"""
    out = []
    for m in glb.j.get("materials", []):
        pbr = m.get("pbrMetallicRoughness", {})
        rgba = pbr.get("baseColorFactor", [1, 1, 1, 1])
        tex = ""
        if "baseColorTexture" in pbr:
            tex = textures.add(glb, glb.j["textures"][pbr["baseColorTexture"]["index"]]["source"])
        out.append((rgba, tex, int(m.get("alphaMode") == "BLEND" or rgba[3] < 1 or tex in textures.alpha)))
    return out


def cstr(s, n):
    b = s.encode("latin-1", "replace")[:n - 1]
    return b + b"\0" * (n - len(b))


def write_materials(f, mats):
    f.write(struct.pack("<I", len(mats)))
    for rgba, tex, blend in mats:
        f.write(struct.pack("<4f", *rgba) + cstr(tex, 64) + struct.pack("<I", blend))


def write_batch(f, mat, verts, idx):
    f.write(struct.pack("<iII", mat, len(verts), len(idx)))
    f.write(struct.pack("<%df" % (len(verts) * 8), *[c for v in verts for c in v]))
    f.write(struct.pack("<%dI" % len(idx), *idx))


def bake_model(src, dst, textures):
    glb = Glb(src)
    mats = materials(glb, textures)
    nodes = [n for n in glb.j["nodes"] if "mesh" in n]
    with open(dst, "wb") as f:
        f.write(b"SRM1")
        write_materials(f, mats)
        f.write(struct.pack("<I", len(nodes)))
        for n in nodes:
            prims = glb.j["meshes"][n["mesh"]]["primitives"]
            f.write(cstr(n.get("name", ""), 48) + struct.pack("<16f", *n.get("matrix", IDENT)) + struct.pack("<I", len(prims)))
            for p in prims:
                pos, nrm, uv, idx, mat = glb.primitive(p)
                write_batch(f, mat, [pos[i] + nrm[i] + tuple(uv[i]) for i in range(len(pos))], idx)
    print("%s: %d nodes, %d materials" % (dst.name, len(nodes), len(mats)))


# What the PREP script does with each level model, keyed by name prefix.
def classify(name):
    p3 = name[:3].lower()
    if name == "Sphere01":
        return "sky"
    if p3 == "pat":
        return "path"
    if p3 == "car" or name.lower().startswith("finish"):
        return "marker"
    if p3 == "fal":
        return "trigger" if name[10:13] == "TGR" else "faller"
    if p3 == "inv":
        return "invisible_solid"
    if p3 in ("wal", "rai"):
        return "solid"
    if p3 == "tre":
        return "tree"
    if p3 == "seg":
        return "track"
    if p3 == "pic":
        return "pickup"
    return "scenery"


def bounding_sphere(points):
    mn = [min(p[i] for p in points) for i in range(3)]
    mx = [max(p[i] for p in points) for i in range(3)]
    c = [(mn[i] + mx[i]) * 0.5 for i in range(3)]
    r = max(sum((p[i] - c[i]) ** 2 for i in range(3)) for p in points) ** 0.5
    return c, r


def bake_level(src, dst, textures):
    glb = Glb(src)
    mats = materials(glb, textures)
    batches = {}   # (segment, material) -> [verts, idx]
    sky = {}       # material -> [verts, idx] in model space
    col = []       # (9 floats, surface id, kind, owner) kind: 0 track, 1 solid, 2 tree
    markers = []   # (name, kind, matrix)
    objects = []   # (name, kind, matrix, segment, centre, radius, [(mat, verts, idx)]) drawn and toggled one by one
    kinds = {}
    for n in glb.j["nodes"]:
        if "mesh" not in n:
            continue
        name = n.get("name", "")
        kind = classify(name)
        if kind == "scenery" and name[:3].lower() == "wat":
            kind = "water"
        kinds[kind] = kinds.get(kind, 0) + 1
        m = n.get("matrix", IDENT)
        if kind in ("path", "marker", "trigger", "sky"):
            markers.append((name, kind, m))
        if kind in ("path", "marker", "trigger", "faller"):
            continue
        visible = kind != "invisible_solid" and n.get("extras", {}).get("visibility", 1) != 0
        single = kind in ("tree", "pickup", "water")
        prims, allpos = [], []
        for pi, p in enumerate(glb.j["meshes"][n["mesh"]]["primitives"]):
            pos, nrm, uv, idx, mat = glb.primitive(p)
            if kind == "sky":
                b = sky.setdefault(mat, [[], []])
                base = len(b[0])
                b[0] += [pos[i] + nrm[i] + tuple(uv[i]) for i in range(len(pos))]
                b[1] += [base + i for i in idx]
                continue
            wpos = [xform_point(m, v) for v in pos]
            if kind in ("track", "solid", "invisible_solid", "tree"):
                for t in range(0, len(idx) - 2, 3):
                    a, b_, c = wpos[idx[t]], wpos[idx[t + 1]], wpos[idx[t + 2]]
                    col.append((a + b_ + c, pi + 1 if kind == "track" else 0,
                                0 if kind == "track" else 2 if kind == "tree" else 1,
                                len(objects) if kind == "tree" else -1))
            if single:
                prims.append((mat, [pos[i] + nrm[i] + tuple(uv[i]) for i in range(len(pos))], idx))
                allpos += pos
                continue
            if not visible:
                continue
            wnrm = [xform_dir(m, v) for v in nrm]
            seg = int(m[13] // SEG_LEN) if kind != "track" else int(name[4:6]) - 1
            b = batches.setdefault((seg, mat), [[], []])
            base = len(b[0])
            b[0] += [wpos[i] + wnrm[i] + tuple(uv[i]) for i in range(len(pos))]
            b[1] += [base + i for i in idx]
        if single:
            c, r = bounding_sphere(allpos)
            objects.append((name, kind, m, int(m[13] // SEG_LEN), c, r, prims))
    with open(dst, "wb") as f:
        f.write(b"SRL2")
        write_materials(f, mats)
        f.write(struct.pack("<I", len(sky)))
        for mat, (verts, idx) in sorted(sky.items()):
            write_batch(f, mat, verts, idx)
        f.write(struct.pack("<I", len(batches)))
        for (seg, mat), (verts, idx) in sorted(batches.items()):
            f.write(struct.pack("<i", seg))
            write_batch(f, mat, verts, idx)
        f.write(struct.pack("<I", len(objects)))
        for name, kind, m, seg, c, r, prims in objects:
            f.write(cstr(name, 48) + cstr(kind, 16) + struct.pack("<16f", *m) + struct.pack("<i4fI", seg, *c, r, len(prims)))
            for mat, verts, idx in prims:
                write_batch(f, mat, verts, idx)
        f.write(struct.pack("<I", len(col)))
        for tri, surf, kind, owner in col:
            f.write(struct.pack("<9fHHi", *tri, surf, kind, owner))
        f.write(struct.pack("<I", len(markers)))
        for name, kind, m in markers:
            f.write(cstr(name, 48) + cstr(kind, 16) + struct.pack("<16f", *m))
    print("%s: %d draw batches, %d objects, %d collision triangles, %d markers, kinds %s" % (
        dst.name, len(batches), len(objects), len(col), len(markers), kinds))


def copy_media():
    """HUD bitmaps -> romfs/ui, sounds -> romfs/snd (as extracted by tools/dirassets.py)."""
    n = 0
    for sub, dst, exts in (("bitmaps", "ui", (".png", ".jpg")), ("sounds", "snd", (".wav",))):
        (OUT / dst).mkdir(exist_ok=True)
        for f in sorted((SRC / sub).iterdir()):
            if f.suffix.lower() in exts:
                (OUT / dst / f.name).write_bytes(f.read_bytes())
                n += 1
    print("%d media files" % n)


# Flash movies of the main movie (cast members SR_MAIN and LOSE) and the bitmaps the front end uses from them.
MENU_ART = {
    "XMED-5196.bin": {90: "menu_banner", 100: "menu_mach5", 280: "menu_speed", 258: "menu_clips", 461: "menu_howto"},
    "XMED-5198.bin": {27: "menu_duggery", 69: "menu_button_red", 72: "menu_button_blue"},
}


def copy_main_movie():
    """Front-end art out of the main movie's Flash files, its sounds, and the FLV videos."""
    import swfdump
    main_out = ROOT / "local" / "out" / "main" / "SR_MAIN" / "chunks"
    n = 0
    for chunk, wanted in MENU_ART.items():
        src = main_out / chunk
        if not src.exists():
            print("missing", src)
            continue
        d = src.read_bytes()

        def walk(buf, p, end):
            nonlocal n
            for code, t in swfdump.tags(buf, p, end):
                if code == 39:
                    walk(t, 4, len(t))
                if code not in (21, 35):
                    continue
                cid = struct.unpack_from("<H", t, 0)[0]
                if cid not in wanted:
                    continue
                if code == 35:
                    alen = struct.unpack_from("<I", t, 2)[0]
                    j, alpha = swfdump.jpeg_fix(t[6:6 + alen]), zlib.decompress(t[6 + alen:])
                else:
                    j, alpha = swfdump.jpeg_fix(t[2:]), None
                (OUT / "ui" / (wanted[cid] + ".jpg")).write_bytes(j)
                if alpha:
                    s = j.find(bytes((0xFF, 0xC0)))
                    h, w = struct.unpack_from(">HH", j, s + 5)
                    if len(alpha) == w * h:
                        (OUT / "ui" / (wanted[cid] + ".jpg.alpha")).write_bytes(struct.pack("<2I", w, h) + alpha)
                n += 1

        walk(d, swfdump.body(d), len(d))
    for f in sorted((ROOT / "local" / "assets" / "main" / "sounds").glob("*.wav")):
        (OUT / "snd" / f.name).write_bytes(f.read_bytes())
        n += 1
    media = next((ROOT / "local" / "game").glob("*/MEDIA"), None)
    (OUT / "media").mkdir(exist_ok=True)
    if media:
        for f in sorted(media.glob("*.flv")):
            (OUT / "media" / f.name).write_bytes(f.read_bytes())
            n += 1
    # The clip beside PLAY! on the main page is a video stream embedded in the menu movie (XMED-5196).
    menu_movie = ROOT / "local" / "out" / "main" / "SR_MAIN" / "chunks" / "XMED-5196.bin"
    if menu_movie.exists():
        import subprocess
        subprocess.run([sys.executable, str(ROOT / "tools" / "swfvideo.py"), str(menu_movie), str(OUT / "media" / "menu_preview.flv")], check=True)
        n += 1
    # The loading card ("THE GREAT PLAN" on a checkerboard): the TITLE_1 Flash movie of the main movie,
    # rendered by tools/swfcapture (movie XMED-5206).
    title = ROOT / "local" / "cutscene" / "frames" / "T5206" / "0001.png"
    if title.exists():
        (OUT / "ui" / "title_card.png").write_bytes(title.read_bytes())
        n += 1
    # The result screens' opening cartoons and the pages' music loop (tools/cutscene.py).
    cut = ROOT / "local" / "cutscene" / "out"
    for f in sorted(cut.glob("*.flv")):
        (OUT / "media" / f.name).write_bytes(f.read_bytes())
        n += 1
    for f in sorted(cut.glob("*.wav")):
        (OUT / "snd" / f.name).write_bytes(f.read_bytes())
        n += 1
    # Screen captures of the original menu pages (local/ref, made by running the original game): page backdrops.
    ref = ROOT / "local" / "ref"
    pages = {"t_main": "ref_main", "t_clips": "ref_clips", "t_wall": "ref_wall", "t_mini": "ref_mini", "t_about": "ref_about",
             "t_how": "ref_how", "a_A": "ref_about_a", "a_B": "ref_about_b", "a_C": "ref_about_c", "a_D": "ref_about_d",
             "a_E": "ref_about_e", "a_F": "ref_about_f", "a_G": "ref_about_g",
             "r_win": "ref_result_win", "r_lose": "ref_result_lose", "r_die_anim": "ref_result_anim"}
    for src, dst in pages.items():
        f = ref / (src + ".png")
        if f.exists():
            (OUT / "ui" / (dst + ".png")).write_bytes(f.read_bytes())
            n += 1
        else:
            print("no capture", f.name, "- that page will use the plain fallback")
    # Mini-game pictures: the three JPEGs inside each mini-game's Flash movie, in tag order.
    for movie, prefix in (("JIGSAW", "jigsaw"), ("SLIDERS", "slider")):
        chunks = sorted((ROOT / "local" / "out" / movie).glob("*/chunks/XMED-*.bin"), key=lambda p: -p.stat().st_size)
        if not chunks:
            print("mini-game", movie, "not extracted")
            continue
        d = chunks[0].read_bytes()
        tables, pics = b"", []

        def walk2(buf, p, end):
            nonlocal tables
            for code, t in swfdump.tags(buf, p, end):
                if code == 39:
                    walk2(t, 4, len(t))
                elif code == 8:
                    tables = swfdump.jpeg_fix(t)
                elif code == 6:
                    pics.append(swfdump.join_tables(tables, swfdump.jpeg_fix(t[2:])))

        walk2(d, swfdump.body(d), len(d))
        for i, j in enumerate(pics[:3]):
            (OUT / "ui" / ("%s_%d.jpg" % (prefix, i + 1))).write_bytes(j)
            n += 1
    for name in ("speed_m5", "speed_blue", "speed_duggery"):
        f = ROOT / "local" / "assets" / "main" / "bitmaps" / (name + ".jpg")
        if f.exists():
            (OUT / "ui" / ("wall_" + name[6:] + ".jpg")).write_bytes(f.read_bytes())
            n += 1
    print("%d front-end files" % n)


def bake_collider(src, dst):
    """Contact shape for the cars: spheres that fill the "collider" mesh (a flat slab), as
    {u32 count, count x (x, y, z, radius)}. The rim follows the slab's outline closely; the inside is a coarse grid."""
    glb = Glb(src)
    node = next(n for n in glb.j["nodes"] if n.get("name") == "collider")
    pts = []
    for p in glb.j["meshes"][node["mesh"]]["primitives"]:
        pts += glb.primitive(p)[0]
    z0, z1 = min(p[2] for p in pts), max(p[2] for p in pts)
    r = (z1 - z0) * 0.5
    zc = (z0 + z1) * 0.5
    # convex hull of the outline (monotone chain), counter-clockwise
    xy = sorted(set((round(p[0], 4), round(p[1], 4)) for p in pts))

    def half(points):
        h = []
        for q in points:
            while len(h) >= 2 and (h[-1][0] - h[-2][0]) * (q[1] - h[-2][1]) - (h[-1][1] - h[-2][1]) * (q[0] - h[-2][0]) <= 0:
                h.pop()
            h.append(q)
        return h[:-1]

    hull = half(xy) + half(xy[::-1])

    def inside(x, y, margin):
        for i in range(len(hull)):
            ax, ay = hull[i]
            bx, by = hull[(i + 1) % len(hull)]
            ex, ey = bx - ax, by - ay
            l = (ex * ex + ey * ey) ** 0.5
            if l < 1e-9:
                continue
            if (ex * (y - ay) - ey * (x - ax)) / l < margin:  # distance to this edge, positive inside
                return False
        return True

    spheres = []
    # rim: walk each hull edge, centres pulled in by the radius so the sphere surface meets the outline
    step = r * 1.4
    for i in range(len(hull)):
        ax, ay = hull[i]
        bx, by = hull[(i + 1) % len(hull)]
        ex, ey = bx - ax, by - ay
        l = (ex * ex + ey * ey) ** 0.5
        if l < 1e-9:
            continue
        nx, ny = -ey / l, ex / l  # inward normal
        n = max(1, int(l / step + 0.5))
        for k in range(n):
            t = (k + 0.5) / n
            x, y = ax + ex * t + nx * r, ay + ey * t + ny * r
            if inside(x, y, r * 0.5):
                spheres.append((x, y, zc, r))
    grid = r * 2.4
    x = min(p[0] for p in hull)
    while x <= max(p[0] for p in hull):
        y = min(p[1] for p in hull)
        while y <= max(p[1] for p in hull):
            if inside(x, y, r * 2.2):
                spheres.append((x, y, zc, r))
            y += grid
        x += grid
    # the car is left-right symmetric but the fill above is not (an inner grid row sat 0.12 off centre, which made a
    # car landing flat take its impulse on one side and roll): keep each sphere and its mirror image
    seen, sym = set(), []
    for sx, sy, sz, sr in spheres:
        for mx in (sx, -sx):
            key = (round(mx, 2), round(sy, 2))
            if key not in seen:
                seen.add(key)
                sym.append((mx, sy, sz, sr))
    spheres = sym
    with open(dst, "wb") as f:
        f.write(struct.pack("<I", len(spheres)))
        for s in spheres:
            f.write(struct.pack("<4f", *s))
    print("%s: %d spheres of radius %.3f (hull of %d points)" % (dst.name, len(spheres), r, len(hull)))


def make_icon():
    """The Switch icon: the last frame of the original's intro (SPEED RACER over a wheel), captured into
    local/ref/intro_card.png; without it, the projector's own icon from the game exe (tools/exeicon.py)."""
    card = ROOT / "local" / "ref" / "intro_card.png"
    if card.exists():
        from PIL import Image
        im = Image.open(card).convert("RGB")
        # centre the wheel hub (268, 208 on the 512x384 card): a 400 px square around it runs 24 px past the
        # bottom of the card, so the bottom row is repeated down there
        side, cx, cy = 400, 268, 208
        x0, y0 = cx - side // 2, cy - side // 2
        canvas = Image.new("RGB", (im.width, im.height + 40))
        canvas.paste(im, (0, 0))
        canvas.paste(im.crop((0, im.height - 1, im.width, im.height)).resize((im.width, 40)), (0, im.height))
        canvas.crop((x0, y0, x0 + side, y0 + side)).resize((256, 256), Image.LANCZOS).save(ROOT / "local" / "icon.jpg", quality=95)
        return
    exe = next((ROOT / "local" / "game").glob("*/SR_GAME.exe"), None)
    if exe:
        import subprocess
        subprocess.run([sys.executable, str(ROOT / "tools" / "exeicon.py"), str(exe), str(ROOT / "local" / "icon.jpg")], check=True)


def main():
    if not SRC.exists():
        sys.exit("run tools/extract.sh first (missing %s)" % SRC)
    OUT.mkdir(exist_ok=True)
    textures = Textures()
    bake_level(SRC / "w3d-7587.glb", OUT / "level1.srl", textures)
    bake_model(SRC / "w3d-7589.glb", OUT / "mach5.srm", textures)
    bake_collider(SRC / "w3d-7589.glb", OUT / "collider.bin")
    bake_model(SRC / "w3d-7591.glb", OUT / "enemy.srm", textures)
    copy_media()
    copy_main_movie()
    make_icon()
    print("%d textures, %d with an alpha plane: %s" % (len(textures.names), len(textures.alpha), sorted(textures.alpha)))


if __name__ == "__main__":
    main()
