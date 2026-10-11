#include "assets.h"

#include <cstdio>
#include <cstring>
#include <map>

#include "stb_image.h"

void logf(const char* fmt, ...); // main.cpp

namespace {

struct Reader {
    FILE* f = nullptr;
    bool ok = true;
    explicit Reader(const char* path) : f(fopen(path, "rb")) { ok = f != nullptr; }
    ~Reader() { if (f) fclose(f); }
    void read(void* p, size_t n) { if (ok && fread(p, 1, n, f) != n) ok = false; }
    template <class T> T get() { T v{}; read(&v, sizeof v); return v; }
    std::string str(size_t n) { std::vector<char> b(n + 1, 0); read(b.data(), n); return std::string(b.data()); }
};

std::map<std::string, GLuint> g_textures;

GLuint loadTexture(const std::string& name) {
    if (name.empty()) return 0;
    auto it = g_textures.find(name);
    if (it != g_textures.end()) return it->second;
    std::string path = "romfs:/tex/" + name;
    int w, h, n;
    stbi_uc* px = stbi_load(path.c_str(), &w, &h, &n, 4);
    GLuint tex = 0;
    if (!px) {
        logf("texture %s: %s", path.c_str(), stbi_failure_reason());
    } else {
        // Optional alpha plane written by the bake next to the JPEG.
        if (FILE* af = fopen((path + ".alpha").c_str(), "rb")) {
            uint32_t dim[2] = {};
            std::vector<unsigned char> plane((size_t)w * h);
            if (fread(dim, 4, 2, af) == 2 && (int)dim[0] == w && (int)dim[1] == h &&
                fread(plane.data(), 1, plane.size(), af) == plane.size())
                for (size_t i = 0; i < plane.size(); ++i) px[i * 4 + 3] = plane[i];
            fclose(af);
        }
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        stbi_image_free(px);
    }
    g_textures[name] = tex;
    return tex;
}

void readMaterials(Reader& r, std::vector<Material>& out) {
    uint32_t n = r.get<uint32_t>();
    for (uint32_t i = 0; r.ok && i < n; ++i) {
        Material m;
        r.read(&m.color, sizeof(float) * 4);
        m.texture = loadTexture(r.str(64));
        m.blend = r.get<uint32_t>() != 0;
        out.push_back(m);
    }
}

// Reads one batch (vertices are 8 floats: position, normal, uv) and uploads it.
Batch readBatch(Reader& r) {
    Batch b;
    b.material = r.get<int32_t>();
    uint32_t vc = r.get<uint32_t>(), ic = r.get<uint32_t>();
    if (!r.ok || vc > 50000000u || ic > 50000000u) { r.ok = false; return b; }
    std::vector<float> verts((size_t)vc * 8);
    std::vector<uint32_t> idx(ic);
    r.read(verts.data(), verts.size() * sizeof(float));
    r.read(idx.data(), idx.size() * sizeof(uint32_t));
    if (!r.ok) return b;
    GLuint vbo, ebo;
    glGenVertexArrays(1, &b.vao);
    glBindVertexArray(b.vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_STATIC_DRAW);
    glGenBuffers(1, &ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx.size() * sizeof(uint32_t), idx.data(), GL_STATIC_DRAW);
    const GLsizei stride = 8 * sizeof(float);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void*)(3 * sizeof(float)));
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (void*)(6 * sizeof(float)));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glBindVertexArray(0);
    b.indexCount = (GLsizei)ic;
    return b;
}

bool checkMagic(Reader& r, const char* magic, const char* path) {
    char m[4] = {};
    r.read(m, 4);
    if (!r.ok || memcmp(m, magic, 4) != 0) {
        logf("%s: missing or not a %s file", path, magic);
        return false;
    }
    return true;
}

} // namespace

const ModelNode* Model::find(const char* name) const {
    for (const ModelNode& n : nodes)
        if (n.name == name) return &n;
    return nullptr;
}

const Marker* Level::find(const char* name) const {
    for (const Marker& m : markers)
        if (m.name == name) return &m;
    return nullptr;
}

bool loadModel(const char* path, Model& out) {
    Reader r(path);
    if (!checkMagic(r, "SRM1", path)) return false;
    readMaterials(r, out.materials);
    uint32_t n = r.get<uint32_t>();
    for (uint32_t i = 0; r.ok && i < n; ++i) {
        ModelNode node;
        node.name = r.str(48);
        r.read(&node.matrix, sizeof(float) * 16);
        uint32_t prims = r.get<uint32_t>();
        for (uint32_t p = 0; r.ok && p < prims; ++p) node.batches.push_back(readBatch(r));
        out.nodes.push_back(std::move(node));
    }
    logf("%s: %d nodes %s", path, (int)out.nodes.size(), r.ok ? "ok" : "TRUNCATED");
    return r.ok;
}

bool loadLevel(const char* path, Level& out) {
    Reader r(path);
    if (!checkMagic(r, "SRL2", path)) return false;
    readMaterials(r, out.materials);
    uint32_t n = r.get<uint32_t>();
    for (uint32_t i = 0; r.ok && i < n; ++i) out.sky.push_back(readBatch(r));
    n = r.get<uint32_t>();
    for (uint32_t i = 0; r.ok && i < n; ++i) {
        LevelBatch lb;
        lb.segment = r.get<int32_t>();
        lb.batch = readBatch(r);
        out.batches.push_back(lb);
    }
    n = r.get<uint32_t>();
    for (uint32_t i = 0; r.ok && i < n; ++i) {
        LevelObject o;
        o.name = r.str(48);
        o.kind = r.str(16);
        r.read(&o.matrix, sizeof(float) * 16);
        o.segment = r.get<int32_t>();
        r.read(&o.center, 12);
        o.radius = r.get<float>();
        uint32_t prims = r.get<uint32_t>();
        for (uint32_t p = 0; r.ok && p < prims; ++p) o.batches.push_back(readBatch(r));
        out.objects.push_back(std::move(o));
    }
    n = r.get<uint32_t>();
    std::vector<Triangle> tris;
    tris.reserve(n);
    for (uint32_t i = 0; r.ok && i < n; ++i) {
        Triangle t;
        r.read(&t.a, 12);
        r.read(&t.b, 12);
        r.read(&t.c, 12);
        t.surface = r.get<uint16_t>();
        t.kind = r.get<uint16_t>();
        t.owner = r.get<int32_t>();
        tris.push_back(t);
    }
    n = r.get<uint32_t>();
    for (uint32_t i = 0; r.ok && i < n; ++i) {
        Marker m;
        m.name = r.str(48);
        m.kind = r.str(16);
        r.read(&m.matrix, sizeof(float) * 16);
        out.markers.push_back(std::move(m));
    }
    out.collision.build(std::move(tris));
    logf("%s: %d batches, %d objects, %d collision triangles, %d markers %s", path, (int)out.batches.size(),
         (int)out.objects.size(), (int)out.collision.size(), (int)out.markers.size(), r.ok ? "ok" : "TRUNCATED");
    return r.ok;
}

const UiTexture& uiTexture(const std::string& name) {
    static std::map<std::string, UiTexture> cache;
    auto it = cache.find(name);
    if (it != cache.end()) return it->second;
    UiTexture t;
    int w = 0, h = 0, n = 0;
    std::string base = "romfs:/ui/" + name;
    stbi_uc* px = stbi_load((base + ".png").c_str(), &w, &h, &n, 4);
    if (!px) {
        px = stbi_load((base + ".jpg").c_str(), &w, &h, &n, 4);
        if (px) {
            int aw = 0, ah = 0, an = 0;
            if (stbi_uc* a = stbi_load((base + ".alpha.png").c_str(), &aw, &ah, &an, 1)) {
                if (aw == w && ah == h)
                    for (int i = 0; i < w * h; ++i) px[i * 4 + 3] = a[i];
                stbi_image_free(a);
            } else if (FILE* af = fopen((base + ".jpg.alpha").c_str(), "rb")) { // {u32 w, u32 h, bytes}, from a Flash JPEG
                uint32_t dim[2] = {};
                std::vector<unsigned char> plane((size_t)w * h);
                if (fread(dim, 4, 2, af) == 2 && (int)dim[0] == w && (int)dim[1] == h &&
                    fread(plane.data(), 1, plane.size(), af) == plane.size())
                    for (size_t i = 0; i < plane.size(); ++i) px[i * 4 + 3] = plane[i];
                fclose(af);
            }
        }
    }
    if (!px) {
        logf("ui texture %s not found", name.c_str());
    } else {
        glGenTextures(1, &t.texture);
        glBindTexture(GL_TEXTURE_2D, t.texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        stbi_image_free(px);
        t.width = w;
        t.height = h;
    }
    return cache[name] = t;
}
