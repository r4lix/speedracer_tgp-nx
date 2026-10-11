#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Shockwave 3D (W3D / IFX) scene parser: nodes, materials, shaders, textures, CLOD meshes.

Block layouts and the CLOD decoder are a Python port of the W3D parser in dirplayer-rs
(https://github.com/igorlira/dirplayer-rs, vm-rust/src/director/chunks/w3d, GPL-3.0).

Usage: w3d.py <XMED chunk or .w3d> [--glb out.glb] [--textures dir]
"""
import json, math, os, struct, sys
from dataclasses import dataclass, field

from ifx_bitstream import BitStream
from w3d_blocks import blocks, ifx_payload

MATERIAL_V1, MATERIAL = 0xFFFFFF10, 0xFFFFFF11
TEXTURE_CONT = 0xFFFFFF21
SHADER_LIT_V1, SHADER_LIT = 0xFFFFFF36, 0xFFFFFF38
MODEL_RESOURCE, SYNC_TABLE, COMPRESSED_GEOM = 0xFFFFFF45, 0xFFFFFF47, 0xFFFFFF49
LIGHT_RESOURCE = 0xFFFFFF50
NODE_TYPES = {  # type -> (kind, has bounds)
    0xFFFFFF70: ("group", False), 0xFFFFFF71: ("light", False), 0xFFFFFF72: ("model", False),
    0xFFFFFF74: ("view", False), 0xFFFFFF75: ("view", True), 0xFFFFFF76: ("light", True),
    0xFFFFFF77: ("group", True), 0xFFFFFF78: ("model", True),
}


class Reader:
    def __init__(self, data):
        self.d, self.p = data, 0

    def remaining(self):
        return len(self.d) - self.p

    def unpack(self, fmt):
        v = struct.unpack_from("<" + fmt, self.d, self.p)
        self.p += struct.calcsize("<" + fmt)
        return v if len(v) > 1 else v[0]

    def u8(self): return self.unpack("B")
    def u32(self): return self.unpack("I")
    def f32(self): return self.unpack("f")

    def string(self):
        n = self.unpack("H")
        s = self.d[self.p:self.p + n]
        self.p += n
        return s.decode("latin-1")


@dataclass
class Node:
    kind: str
    name: str
    parent: str
    matrix: tuple  # 16 floats, translation in elements 12..14
    resource: str = ""
    visibility: int = 3
    shader: str = ""


@dataclass
class Mesh:
    vertex_attributes: int
    num_updates: int
    num_vertices: int = 0  # declared full-resolution counts
    num_faces: int = 0
    positions: list = field(default_factory=list)
    uvs: list = field(default_factory=list)  # one list per texcoord layer
    faces: list = field(default_factory=list)
    history: dict = field(default_factory=dict)  # (face, corner) -> [(resolution, vertex)]
    sorted_faces: list = field(default_factory=list)
    new_verts: int = 0

    def corner_as_of(self, face, corner, max_res):
        """Vertex at a face corner as it was at resolution max_res (see dirplayer clod_types.rs)."""
        hist = self.history.get((face, corner))
        if hist:
            val = hist[0][1]
            for res, v in hist:
                if res > max_res:
                    break
                val = v
            return val
        return self.faces[face][corner] if face < len(self.faces) else 0


@dataclass
class ModelResource:
    name: str
    neighbor_mesh: bool
    meshes: list
    shaders: list  # [(binding name, [shader per mesh])]
    crease_iq: float = 1.0
    pos_iq: float = 1.0
    norm_iq: float = 1.0
    tc_iq: float = 1.0
    sync_table: list = None
    resolution: int = 0
    cursors: list = None


class Scene:
    def __init__(self, data):
        self.nodes, self.materials, self.shaders, self.lights = [], {}, {}, {}
        self.textures, self.resources = {}, {}
        self.errors = []
        for typ, off, p in blocks(ifx_payload(data)):
            try:
                self._block(typ, p)
            except Exception as e:  # keep whatever decoded so far
                self.errors.append(f"{typ:08x}@{off:x}: {e!r}")

    def _block(self, typ, p):
        r = Reader(p)
        if typ in NODE_TYPES:
            self._node(r, *NODE_TYPES[typ])
        elif typ in (MATERIAL_V1, MATERIAL):
            self._material(r)
        elif typ in (SHADER_LIT_V1, SHADER_LIT):
            self._shader(r, typ == SHADER_LIT)
        elif typ == TEXTURE_CONT:
            name = r.string()
            r.u8()  # continuation index
            self.textures[name] = self.textures.get(name, b"") + bytes(p[r.p:])
        elif typ == LIGHT_RESOURCE:
            name = r.string()
            kind, _ = r.unpack("BB")
            self.lights[name] = {"type": ("ambient", "directional", "point", "spot")[min(kind, 3)],
                                 "color": r.unpack("4f")}
        elif typ == MODEL_RESOURCE:
            self._model_resource(r)
        elif typ == SYNC_TABLE:
            bs = BitStream(p)
            res = self.resources.get(bs.string())
            if res:
                res.sync_table = []
                for m in res.meshes:
                    acc, table = 0, []
                    for _ in range(m.num_updates):
                        acc += bs.cu32(1)
                        table.append(acc)
                    res.sync_table.append(table)
        elif typ == COMPRESSED_GEOM:
            bs = BitStream(p)
            res = self.resources.get(bs.string())
            if res:
                decode_clod(res, bs)

    def _node(self, r, kind, has_bounds):
        name, parent, resource = r.string(), r.string(), r.string()
        node = Node(kind, name, parent, r.unpack("16f"))
        if has_bounds:
            r.unpack("8f")
        if kind == "model":
            if r.remaining() >= 2: node.resource = r.string()
            if r.remaining() >= 2: r.string()  # style
            if r.remaining() >= 4: node.visibility = r.u32() & 3
            if r.remaining() >= 2: node.shader = r.string()
        elif kind == "light":
            if r.remaining() >= 2: node.resource = r.string()
        self.nodes.append(node)

    def _material(self, r):
        name, attrs = r.string(), r.u32()
        m = {"ambient": None, "diffuse": (1, 1, 1, 1), "specular": None, "emissive": None, "opacity": 1.0}
        for bit, key in ((1, "ambient"), (2, "diffuse"), (4, "specular"), (8, "emissive")):
            if attrs & bit: m[key] = r.unpack("4f")
        if attrs & 0x10: m["reflectivity"] = r.f32()
        if attrs & 0x20: m["opacity"] = r.f32()
        if attrs & 0x40: r.f32()
        if attrs & 0x80: m["shininess"] = r.f32()
        self.materials[name] = m

    def _shader(self, r, v200):
        name, attrs, _render_pass = r.string(), r.u32(), r.u32()
        s = {"material": "", "textures": []}
        if attrs & 1:
            s["material"] = r.string()
        for layer in range(8):
            if attrs & (1 << (16 + layer)):
                tex = r.string()
                intensity, blend_func, blend_src, blend_const, tex_mode = r.unpack("fBBfB")
                r.unpack("32f")  # texture and wrap transforms
                r.u8()  # repeat S
                if v200: r.u8()
                s["textures"].append({"name": tex, "blend_func": blend_func, "blend_src": blend_src,
                                      "blend_const": blend_const, "mode": tex_mode})
        self.shaders[name] = s

    def _model_resource(self, r):
        name, attrs, num_meshes = r.string(), r.u32(), r.u32()
        meshes = []
        for _ in range(num_meshes):
            nv, nf, _update_data, num_updates, _bias, vattrs = r.unpack("IIIIfI")
            meshes.append(Mesh(vattrs, num_updates, nv, nf, uvs=[[] for _ in range(vattrs >> 4)]))
        shaders = []
        for _ in range(r.u32()):
            shaders.append((r.string(), [r.string() for _ in range(num_meshes)]))
        res = ModelResource(name, bool(attrs & 2), meshes, shaders)
        if r.remaining() >= 16: r.unpack("4f")  # bounding sphere
        if r.remaining() >= 24:
            res.crease_iq, res.pos_iq, res.norm_iq, res.tc_iq, _diff, _spec = r.unpack("6f")
        self.resources[name] = res


def decode_clod(res, bs):
    """Apply one compressed geometry block (a run of progressive-mesh updates) to a resource."""
    update_count = bs.u32()
    if update_count > 100000:
        raise ValueError(f"{res.name}: update count {update_count}")
    if len(res.meshes) <= 1:
        for u in range(update_count):
            _mesh_update(res, bs, res.meshes[0], res.resolution + u + 1)
    else:
        if res.cursors is None:
            res.cursors = [0] * len(res.meshes)
        for u in range(update_count):
            gres = res.resolution + u + 1
            for mi, table in enumerate(res.sync_table or []):
                while res.cursors[mi] < len(table) and table[res.cursors[mi]] <= gres:
                    _mesh_update(res, bs, res.meshes[mi], gres)
                    res.cursors[mi] += 1
    res.resolution += update_count


def _mesh_update(res, bs, mesh, gres):
    num_verts, num_patches, num_faces, num_sorted = bs.cu32(1), bs.cu32(3), bs.cu32(2), bs.cu32(4)
    if max(num_verts, num_patches, num_faces, num_sorted) > 50000:
        raise ValueError(f"{res.name}: bad update header")

    base_ctx = len(mesh.faces) + 1024
    sorted_faces = mesh.sorted_faces = []
    for s in range(num_sorted):
        if s == 0:
            sorted_faces.append(bs.cu32(base_ctx))
        else:
            prev = sorted_faces[-1]
            sorted_faces.append(prev + bs.cu32(max(base_ctx - prev, 1)))
    mesh.new_verts = num_verts
    boundary = gres - 2

    def predicted(mode, idx, source):
        if idx < len(sorted_faces) and mode < 3:
            v = mesh.corner_as_of(sorted_faces[idx], mode, boundary)
            if v < len(source):
                return source[v]
        return None

    for _ in range(num_verts):
        mode = bs.cu8(6)
        pred = (predicted(mode, bs.cu32(5), mesh.positions) if mode != 4 else None) or (0.0, 0.0, 0.0)
        sign = bs.cu8(7)
        mags = bs.cu32(8), bs.cu32(8), bs.cu32(8)
        mesh.positions.append(tuple(
            pred[i] + (-1.0 if sign & (1 << i) else 1.0) * mags[i] * res.pos_iq for i in range(3)))

        # Normal: decoded only to keep the stream in sync; normals are rebuilt from geometry.
        if bs.cu8(10) != 4:
            bs.cu32(9)
        bs.cu8(11)
        theta = min(bs.cu32(12) * float(res.norm_iq), 1.0)
        sin_theta = math.sqrt((1.0 - theta) * (theta + 1.0))
        bs.cu32((int(sin_theta * (math.pi / 2) / res.norm_iq + 0.5) & 0xFFFFFFFF) + 1025)

        for layer in mesh.uvs:
            mode = bs.cu8(14)
            pred = (predicted(mode, bs.cu32(15), layer) if mode != 4 else None) or (0.0, 0.0)
            sign = bs.cu8(16)
            mu, mv = bs.cu32(17), bs.cu32(17)
            layer.append((pred[0] + (-1.0 if sign & 1 else 1.0) * mu * res.tc_iq,
                          pred[1] + (-1.0 if sign & 2 else 1.0) * mv * res.tc_iq))

        bones = bs.cu32(18)
        if bones > 256:
            raise ValueError(f"{res.name}: bone count {bones}")
        for n in range(bones):
            bs.cu32(19)
            if n > 0:
                bs.cu32(20)

    patches = []
    for _ in range(num_patches):
        idx = bs.cu32(21)
        face = sorted_faces[idx] if idx < len(sorted_faces) else 0
        corner = bs.cu8(22)
        param = bs.cu32(23)
        vertex = bs.u32() if param == 0 else len(mesh.positions) - num_verts + param - 1
        if bs.cu8(24) == 3:
            bs.u32()  # old vertex (only needed to undo the update)
        patches.append((face, corner, vertex))

    for _ in range(num_faces):
        corners = [0, 0, 0]
        for c in range(3):
            kind = bs.cu8(25)
            if kind == 0:
                corners[c] = bs.u32()
            elif kind == 1:
                corners[c] = bs.cu32(26) + len(mesh.positions) - num_verts
            elif kind in (2, 3, 4):
                idx, offset = bs.cu32(27), bs.cu32(28)
                if idx < len(sorted_faces):
                    corners[c] = offset + mesh.corner_as_of(sorted_faces[idx], kind - 2, boundary)
        fi = len(mesh.faces)
        mesh.faces.append(corners)
        for c in range(3):
            mesh.history[(fi, c)] = [(gres, corners[c])]
        if res.neighbor_mesh:
            bs.cu8(29)
            for _ in range(3):
                bs.cu8(30)
            for _ in range(3):
                flag = bs.cu8(31)
                if flag in (0, 1, 2):
                    bs.cu32(34); bs.cu32(35)
                elif flag == 4:
                    bs.cu32(32); bs.cu32(len(mesh.faces) + 1024)
                elif flag == 5:
                    bs.cu32(32); bs.cu32(33)
                else:
                    bs.cu32(32); bs.cu32(36)

    for face, corner, vertex in patches:
        if face < len(mesh.faces) and corner < 3:
            old = mesh.faces[face][corner]
            mesh.faces[face][corner] = vertex
            mesh.history.setdefault((face, corner), [(0, old)]).append((gres, vertex))


# --- glTF export ---

def write_glb(scene, path):
    """Export the scene as a binary glTF. Coordinates are left in Director space."""
    buf = bytearray()
    views, accessors, meshes, materials, images, textures = [], [], [], [], [], []
    mat_index, tex_index, mesh_index = {}, {}, {}

    def view(data, target=None):
        while len(buf) % 4:
            buf.append(0)
        v = {"buffer": 0, "byteOffset": len(buf), "byteLength": len(data)}
        if target:
            v["target"] = target
        buf.extend(data)
        views.append(v)
        return len(views) - 1

    def accessor(fmt, kind, rows, target, bounds=False):
        flat = [x for row in rows for x in row] if kind != "SCALAR" else rows
        a = {"bufferView": view(struct.pack(f"<{len(flat)}{fmt}", *flat), target),
             "componentType": {"f": 5126, "I": 5125}[fmt], "count": len(rows), "type": kind}
        if bounds:
            a["min"] = [min(r[i] for r in rows) for i in range(3)]
            a["max"] = [max(r[i] for r in rows) for i in range(3)]
        accessors.append(a)
        return len(accessors) - 1

    def texture(name):
        if name not in tex_index:
            data = scene.textures.get(name, b"")
            mime = "image/jpeg" if data[:2] == b"\xff\xd8" else "image/png" if data[:4] == b"\x89PNG" else None
            if mime is None:
                tex_index[name] = None
            else:
                images.append({"name": name, "mimeType": mime, "bufferView": view(data)})
                textures.append({"source": len(images) - 1})
                tex_index[name] = len(textures) - 1
        return tex_index[name]

    def material(shader_name):
        if shader_name not in mat_index:
            shader = scene.shaders.get(shader_name, {"material": "", "textures": []})
            mat = scene.materials.get(shader["material"], {"diffuse": (1, 1, 1, 1), "opacity": 1.0})
            color = list(mat["diffuse"][:3]) + [mat["opacity"]]
            pbr = {"baseColorFactor": color, "metallicFactor": 0.0, "roughnessFactor": 1.0}
            for layer in shader["textures"][:1]:
                t = texture(layer["name"])
                if t is not None:
                    pbr["baseColorTexture"] = {"index": t}
                    pbr["baseColorFactor"] = [1, 1, 1, mat["opacity"]]
            m = {"name": shader_name, "pbrMetallicRoughness": pbr, "doubleSided": True}
            if mat["opacity"] < 1.0:
                m["alphaMode"] = "BLEND"
            materials.append(m)
            mat_index[shader_name] = len(materials) - 1
        return mat_index[shader_name]

    def mesh(res_name):
        if res_name not in mesh_index:
            mesh_index[res_name] = None
            res = scene.resources.get(res_name)
            prims = []
            for mi, m in enumerate(res.meshes if res else []):
                n = len(m.positions)
                faces = [f for f in m.faces if max(f) < n and len(set(f)) == 3]
                if not faces:
                    continue
                attrs = {"POSITION": accessor("f", "VEC3", m.positions, 34962, bounds=True),
                         "NORMAL": accessor("f", "VEC3", smooth_normals(m.positions, faces), 34962)}
                if m.uvs and len(m.uvs[0]) == n:
                    attrs["TEXCOORD_0"] = accessor("f", "VEC2", [(u, 1.0 - v) for u, v in m.uvs[0]], 34962)
                prim = {"attributes": attrs,
                        "indices": accessor("I", "SCALAR", [i for f in faces for i in f], 34963)}
                # The last binding is the model's own shader list; the first is the resource default.
                if res.shaders and mi < len(res.shaders[-1][1]):
                    prim["material"] = material(res.shaders[-1][1][mi])
                prims.append(prim)
            if prims:
                meshes.append({"name": res_name, "primitives": prims})
                mesh_index[res_name] = len(meshes) - 1
        return mesh_index[res_name]

    nodes, by_name = [], {}
    for n in scene.nodes:
        g = {"name": n.name, "extras": {"kind": n.kind, "visibility": n.visibility}}
        if n.matrix != (1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1):
            g["matrix"] = list(n.matrix)
        if n.kind == "model" and (mi := mesh(n.resource)) is not None:
            g["mesh"] = mi
        by_name[n.name] = len(nodes)
        nodes.append(g)
    roots = []
    for i, n in enumerate(scene.nodes):
        parent = by_name.get(n.parent)
        if parent is None or parent == i:
            roots.append(i)
        else:
            nodes[parent].setdefault("children", []).append(i)

    while len(buf) % 4:
        buf.append(0)
    gltf = {"asset": {"version": "2.0", "generator": "speedracer-revival w3d.py"},
            "scene": 0, "scenes": [{"nodes": roots}], "nodes": nodes, "buffers": [{"byteLength": len(buf)}],
            "bufferViews": views, "accessors": accessors}
    for key, val in (("meshes", meshes), ("materials", materials), ("images", images), ("textures", textures)):
        if val:
            gltf[key] = val
    js = json.dumps(gltf, separators=(",", ":")).encode()
    js += b" " * (-len(js) % 4)
    with open(path, "wb") as f:
        f.write(struct.pack("<4sII", b"glTF", 2, 28 + len(js) + len(buf)))
        f.write(struct.pack("<I4s", len(js), b"JSON") + js)
        f.write(struct.pack("<I4s", len(buf), b"BIN\0") + buf)


def smooth_normals(positions, faces):
    acc = [[0.0, 0.0, 0.0] for _ in positions]
    for a, b, c in faces:
        p0, p1, p2 = positions[a], positions[b], positions[c]
        e1 = (p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2])
        e2 = (p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2])
        n = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
        for i in (a, b, c):
            acc[i][0] += n[0]; acc[i][1] += n[1]; acc[i][2] += n[2]
    out = []
    for x, y, z in acc:
        length = math.sqrt(x * x + y * y + z * z)
        out.append((x / length, y / length, z / length) if length > 1e-8 else (0.0, 0.0, 1.0))
    return out


def main(argv):
    scene = Scene(open(argv[1], "rb").read())
    verts = sum(len(m.positions) for r in scene.resources.values() for m in r.meshes)
    faces = sum(len(m.faces) for r in scene.resources.values() for m in r.meshes)
    print(f"{len(scene.nodes)} nodes, {len(scene.resources)} model resources ({verts} verts, {faces} faces), "
          f"{len(scene.shaders)} shaders, {len(scene.materials)} materials, {len(scene.textures)} textures")
    for e in scene.errors:
        print("error:", e)
    for r in scene.resources.values():
        for i, m in enumerate(r.meshes):
            if (len(m.positions), len(m.faces)) != (m.num_vertices, m.num_faces):
                print(f"incomplete: {r.name}[{i}] {len(m.positions)}/{m.num_vertices} verts, "
                      f"{len(m.faces)}/{m.num_faces} faces")
    if "--glb" in argv:
        out = argv[argv.index("--glb") + 1]
        os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
        write_glb(scene, out)
    if "--textures" in argv:
        out = argv[argv.index("--textures") + 1]
        os.makedirs(out, exist_ok=True)
        for name, data in scene.textures.items():
            ext = ".jpg" if data[:2] == b"\xff\xd8" else ".png" if data[:4] == b"\x89PNG" else ".bin"
            with open(os.path.join(out, os.path.splitext(name)[0] + ext), "wb") as f:
                f.write(data)


if __name__ == "__main__":
    main(sys.argv)
