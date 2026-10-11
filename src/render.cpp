// Draws a Game: the 3D scene (twice when the periscope view is open), then the HUD.
#include "render.h"
#include "settings.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H
#include <glad/glad.h>
#include <switch.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

void logf(const char* fmt, ...); // main.cpp

namespace {

const int kScreenW = 1280, kScreenH = 720;
// The stage was 512x384; pages keep that layout, scaled to the screen height and centred.
const float kHudScale = kScreenH / 384.0f;
const float kHudLeft = (kScreenW - 512 * kHudScale) * 0.5f;
// In the race the HUD sat against the stage edges; on the wider screen each element keeps its distance to its
// own edge, so it moves out by this many stage pixels.
const float kHudEdgeFull = kHudLeft / kHudScale;
inline float hudEdge() { return g_cfg.hudAnchored ? kHudEdgeFull : 0.0f; } // menu switch: off = centred 4:3 layout

const char* kVertex = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNrm;
layout(location = 2) in vec2 aUv;
uniform mat4 uMvp;
uniform mat3 uNormal;
uniform vec4 uUv; // scale xy, offset zw
out vec3 vNrm;
out vec2 vUv;
out float vDepth;
void main() {
    vNrm = uNormal * aNrm;
    vUv = aUv * uUv.xy + uUv.zw;
    gl_Position = uMvp * vec4(aPos, 1.0);
    vDepth = gl_Position.w;
}
)";

const char* kFragment = R"(#version 330 core
in vec3 vNrm;
in vec2 vUv;
in float vDepth;
uniform sampler2D uTex;
uniform vec4 uColor;
uniform int uUseTex;
uniform int uLit;
uniform vec3 uAmbient;
uniform vec3 uSunDir;
uniform int uFog;
uniform vec3 uFogColor;
uniform vec2 uFogRange;
out vec4 frag;
void main() {
    vec4 c = uColor;
    if (uUseTex != 0) c *= texture(uTex, vUv);
    if (uLit != 0) {
        vec3 n = normalize(vNrm);
        if (!gl_FrontFacing) n = -n;
        c.rgb *= min(uAmbient * 0.8 + vec3(0.6) * max(dot(n, uSunDir), 0.0), vec3(1.0));
    }
    if (uFog != 0) c.rgb = mix(c.rgb, uFogColor, clamp((vDepth - uFogRange.x) / (uFogRange.y - uFogRange.x), 0.0, 1.0));
    frag = c;
}
)";

GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char buf[512];
        glGetShaderInfoLog(s, sizeof buf, nullptr, buf);
        logf("shader compile: %s", buf);
    }
    return s;
}

GLuint g_program = 0, g_quad = 0;
GLint uMvp, uNormal, uColor, uUseTex, uLit, uAmbient, uSunDir, uUv, uFog, uFogColor, uFogRange;
glm::mat4 g_viewProj(1);

void setUv(float sx = 1, float sy = 1, float ox = 0, float oy = 0) { glUniform4f(uUv, sx, sy, ox, oy); }

// overrideTex replaces the texture of material overrideMat (the enemy skins); alpha scales the colour's alpha.
void drawBatch(const Batch& b, const std::vector<Material>& mats, const glm::mat4& model, int overrideMat = -1,
               GLuint overrideTex = 0, float alpha = 1) {
    static const Material kDefault;
    const Material& m = b.material >= 0 && b.material < (int)mats.size() ? mats[b.material] : kDefault;
    GLuint tex = b.material == overrideMat && overrideTex ? overrideTex : m.texture;
    glm::mat4 mvp = g_viewProj * model;
    glUniformMatrix4fv(uMvp, 1, GL_FALSE, glm::value_ptr(mvp));
    glUniformMatrix3fv(uNormal, 1, GL_FALSE, glm::value_ptr(glm::mat3(model)));
    glm::vec4 color = tex ? glm::vec4(1, 1, 1, m.color.a) : m.color;
    color.a *= alpha;
    glUniform4fv(uColor, 1, glm::value_ptr(color));
    glUniform1i(uUseTex, tex != 0);
    if (tex) glBindTexture(GL_TEXTURE_2D, tex);
    glBindVertexArray(b.vao);
    glDrawElements(GL_TRIANGLES, b.indexCount, GL_UNSIGNED_INT, nullptr);
}

bool isBlend(const Batch& b, const std::vector<Material>& mats) {
    return b.material >= 0 && b.material < (int)mats.size() && mats[b.material].blend;
}

glm::vec3 nodeScale(const ModelNode& n) {
    return glm::vec3(glm::length(glm::vec3(n.matrix[0])), glm::length(glm::vec3(n.matrix[1])), glm::length(glm::vec3(n.matrix[2])));
}

struct NodeDraw {
    bool keepScale = false; // the script moves this part with .position/.rotation only, so the node keeps its scale
    int overrideMat = -1;
    GLuint overrideTex = 0;
    float alpha = 1;
};

// pass: 0 draws the opaque batches of the node, 1 the blended ones
void drawNode(const Model& model, const char* name, glm::mat4 m, int pass, const NodeDraw& d = NodeDraw()) {
    const ModelNode* n = model.find(name);
    if (!n) return;
    if (d.keepScale) m = glm::scale(m, nodeScale(*n));
    for (const Batch& b : n->batches)
        if ((isBlend(b, model.materials) || d.alpha < 1) == (pass == 1)) drawBatch(b, model.materials, m, d.overrideMat, d.overrideTex, d.alpha);
}

glm::mat4 rotateDeg(float x, float y, float z) {
    glm::mat4 m(1);
    m = glm::rotate(m, glm::radians(z), glm::vec3(0, 0, 1));
    m = glm::rotate(m, glm::radians(y), glm::vec3(0, 1, 0));
    m = glm::rotate(m, glm::radians(x), glm::vec3(1, 0, 0));
    return m;
}

void billboard(const glm::vec3& pos, float size, GLuint tex, float alpha, const glm::mat4& view) {
    if (!tex) return;
    glm::vec3 right(view[0][0], view[1][0], view[2][0]), up(view[0][1], view[1][1], view[2][1]);
    glm::mat4 m(1);
    m[0] = glm::vec4(right * size, 0);
    m[1] = glm::vec4(-up * size, 0); // the quad's v runs down
    m[2] = glm::vec4(glm::cross(right, up), 0);
    m[3] = glm::vec4(pos - right * (size * 0.5f) + up * (size * 0.5f), 1);
    glm::mat4 mvp = g_viewProj * m;
    glUniformMatrix4fv(uMvp, 1, GL_FALSE, glm::value_ptr(mvp));
    glUniform4f(uColor, 1, 1, 1, alpha);
    glUniform1i(uUseTex, 1);
    glBindTexture(GL_TEXTURE_2D, tex);
    glBindVertexArray(g_quad);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void drawParticles(const ParticleSet& set, const glm::mat4& view) {
    if (set.particles.empty() || set.texture.empty()) return;
    GLuint tex = uiTexture(set.texture).texture;
    if (set.additive) glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    for (const Particle& p : set.particles) {
        float t = p.age / p.life;
        float size = set.sizeStart + (set.sizeEnd - set.sizeStart) * t;
        float blend = set.blendStart + (set.blendEnd - set.blendStart) * t;
        // additive sets overlap heavily (an explosion is dozens of big quads): keep them from burning out to white
        if (size > 0.01f && blend > 0.5f) billboard(p.pos, size, tex, blend / 100.0f * (set.additive ? 0.3f : 1.0f), view);
    }
    if (set.additive) glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

const char* kWheels[4] = {"mach5_wheel_FL", "mach5_wheel_FR", "mach5_wheel_RR", "mach5_wheel_RL"};
const char* kGrips[4] = {"mach5_grip_FL", "mach5_grip_FR", "mach5_grip_RR", "mach5_grip_RL"};

void drawCars(Game& g, int pass, const glm::mat4& view) {
    const Model& m5 = g.mach5Model;
    const Model& en = g.enemyModel;
    glm::mat4 chassis = g.car.body.transform();
    drawNode(m5, "mach5_body", chassis, pass);
    if (!g.scopeViewOn) drawNode(m5, "scope_door", chassis, pass, {true});
    for (int i = 0; i < 4; ++i) {
        drawNode(m5, kWheels[i], g.car.wheelMatrix[i], pass);
        if (g.pHasGrip) drawNode(m5, kGrips[i], g.car.wheelMatrix[i], pass);
    }
    if (g.pHasJacks) drawNode(m5, "jacks", chassis, pass);
    if (g.sawsVisible) drawNode(m5, "saw", glm::scale(chassis, glm::vec3(g.pBladeFactor)), pass);
    if (g.scopeViewOn) {
        drawNode(m5, "scope_top", glm::translate(chassis, glm::vec3(-0.3f, -0.9f, 0.246f + g.scopeFactor)) * rotateDeg(0, 0, g.scopeRotate), pass);
        drawNode(m5, "scope_bar", glm::scale(glm::translate(chassis, glm::vec3(-0.3f, -0.9f, 0.246f)), glm::vec3(1, 1, g.scopeFactor)), pass);
    }
    if (g.robotVisible) {
        glm::mat4 r = glm::mat4_cast(g.robotRot);
        r[3] = glm::vec4(g.robotPos, 1);
        drawNode(m5, "ROBOT", glm::scale(r, glm::vec3(2)), pass, {true});
    }
    if (pass == 1) {
        if (g.shadowVisible) {
            glUniform1i(uLit, 0);
            drawNode(m5, "car_shadow", g.car.shadowMatrix, 1, {true});
            glUniform1i(uLit, 1);
        }
        drawNode(m5, "windshield", chassis, 1);
        if (g.shieldVisible)
            drawNode(m5, "windshield_top", glm::translate(chassis, glm::vec3(0, -0.238f, -0.313f)) * rotateDeg(g.pSheildRotate, 0, 0), 1);
        if (g.lightsVisible) {
            NodeDraw d;
            d.alpha = g.lightsBlend / 100.0f;
            glUniform1i(uLit, 0);
            drawNode(m5, "lights", chassis, 1, d);
            glUniform1i(uLit, 1);
        }
    }

    for (const Enemy& e : g.enemies) {
        const int skinMat = 5; // "enemyCar", the body paint
        if (e.alive && e.shellVisible) {
            glm::mat4 xf = e.body.transform();
            NodeDraw skin;
            skin.overrideMat = skinMat;
            skin.overrideTex = uiTexture("EnemyMap_" + std::to_string(e.skin)).texture;
            drawNode(en, e.isDuggary ? "car_bodyDuggery" : "car_body", xf, pass, skin);
            for (int i = 0; i < 4; ++i) drawNode(en, kWheels[i], e.wheelMatrix[i], pass);
            if (e.robotVisible) drawNode(en, "robot", xf * rotateDeg(0, e.pRobotRotationY, e.pRobotRotation), pass, {true});
            if (pass == 1) {
                glUniform1i(uLit, 0);
                drawNode(en, "car_shadow", e.shadowMatrix, 1, {true});
                glUniform1i(uLit, 1);
                drawNode(en, "car_windshield", xf, 1);
            }
        }
        if (e.deadVisible) {
            NodeDraw skin;
            skin.overrideMat = skinMat;
            skin.overrideTex = uiTexture("EnemyMap_" + std::to_string(e.deadSkin)).texture;
            drawNode(en, e.deadIsDuggary ? "car_bodyDuggery" : "car_body", e.deadBody.transform(), pass, skin);
            drawNode(en, kWheels[0], e.deadWheel.transform(), pass);
        }
        if (pass == 1) {
            glUniform1i(uLit, 0);
            setUv();
            drawParticles(e.smoke, view);
            drawParticles(e.explosion, view);
            glUniform1i(uLit, 1);
        }
    }
}

void drawWorld(Game& g, const glm::mat4& view, const glm::mat4& proj, const glm::vec3& eye) {
    const Level& level = g.level;
    g_viewProj = proj * view;
    setUv();
    glUniform1i(uFog, 0);

    // Sky sphere follows the camera, unlit, behind everything.
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glUniform1i(uLit, 0);
    if (!g.isUnderWater)
        if (const Marker* sky = level.find("Sphere01")) {
            glm::mat4 m = glm::mat4(glm::mat3(sky->matrix));
            m[3] = glm::vec4(eye, 1);
            for (const Batch& b : level.sky) drawBatch(b, level.materials, m);
        }
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glUniform1i(uLit, 1);
    if (g.isUnderWater) { // doMach5: fog while the car is under water, further with the lights on
        glUniform1i(uFog, 1);
        glUniform3f(uFogColor, 100 / 255.0f, 150 / 255.0f, 1.0f);
        glUniform2f(uFogRange, g.pHasLights ? 40.0f : 0.0f, g.pHasLights ? 200.0f : 40.0f);
    }

    int seg = (int)std::floor(eye.y / 300.0f);
    auto inRange = [&](int s) { return s >= seg - 1 && s <= seg + 2; };
    auto objectMatrix = [&](int i, glm::mat4& m) -> bool {
        const LevelObject& o = level.objects[i];
        if (!inRange(o.segment)) return false;
        m = g.objectMatrix(i);
        if (o.kind == "tree" && !g.treeAlive[i]) return false;
        return true;
    };
    for (int pass = 0; pass < 2; ++pass) {
        if (pass == 1) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);
        }
        for (const LevelBatch& lb : level.batches)
            if (inRange(lb.segment) && isBlend(lb.batch, level.materials) == (pass == 1)) drawBatch(lb.batch, level.materials, glm::mat4(1));
        for (int i = 0; i < (int)level.objects.size(); ++i) {
            const LevelObject& o = level.objects[i];
            if (o.kind == "pickup") continue;
            glm::mat4 m;
            if (!objectMatrix(i, m)) continue;
            for (const Batch& b : o.batches)
                if (isBlend(b, level.materials) == (pass == 1)) drawBatch(b, level.materials, m);
        }
        for (const Pickup& p : g.pickups) {
            const LevelObject& o = level.objects[p.object];
            if (!p.visible || !inRange(o.segment)) continue;
            glm::mat4 m = o.matrix * rotateDeg(0, 0, p.spin);
            for (const Batch& b : o.batches)
                if (isBlend(b, level.materials) == (pass == 1)) drawBatch(b, level.materials, m);
        }
        for (const DeadTree& t : g.deadTrees) {
            const LevelObject& o = level.objects[t.object];
            glm::mat4 m = o.matrix;
            m[3].z += t.rise;
            m = m * rotateDeg(t.spin, t.spin, 0);
            for (const Batch& b : o.batches)
                if (isBlend(b, level.materials) == (pass == 1)) drawBatch(b, level.materials, m);
        }
        drawCars(g, pass, view);
        if (pass == 1) {
            glUniform1i(uLit, 0);
            setUv();
            drawParticles(g.dirt, view);
            glUniform1i(uLit, 1);
        }
    }
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glUniform1i(uFog, 0);
}

// HUD quad in stage coordinates. width/height < 0 use the texture's own size.
void hudQuad(const std::string& texName, float x, float y, float blend, float u0 = 0, float u1 = 1, float width = -1,
             float height = -1, float scale = 1) {
    const UiTexture& t = uiTexture(texName);
    if (!t.texture || blend <= 0) return;
    float w = (width >= 0 ? width : t.width * (u1 - u0)) * scale, h = (height >= 0 ? height : t.height) * scale;
    glm::mat4 m = glm::translate(glm::mat4(1), glm::vec3(kHudLeft + x * kHudScale, y * kHudScale, 0));
    m = glm::scale(m, glm::vec3(w * kHudScale, h * kHudScale, 1));
    glm::mat4 mvp = glm::ortho(0.0f, (float)kScreenW, (float)kScreenH, 0.0f) * m;
    glUniformMatrix4fv(uMvp, 1, GL_FALSE, glm::value_ptr(mvp));
    glUniform4f(uColor, 1, 1, 1, blend / 100.0f);
    glUniform1i(uUseTex, 1);
    setUv(u1 - u0, 1, u0, 0);
    glBindTexture(GL_TEXTURE_2D, t.texture);
    glBindVertexArray(g_quad);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void fullScreen(float r, float gr, float b, float alpha) {
    glm::mat4 m = glm::scale(glm::mat4(1), glm::vec3(kScreenW, kScreenH, 1));
    glm::mat4 mvp = glm::ortho(0.0f, (float)kScreenW, (float)kScreenH, 0.0f) * m;
    glUniformMatrix4fv(uMvp, 1, GL_FALSE, glm::value_ptr(mvp));
    glUniform4f(uColor, r, gr, b, alpha);
    glUniform1i(uUseTex, 0);
    glBindVertexArray(g_quad);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void beginHud() {
    glViewport(0, 0, kScreenW, kScreenH);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUniform1i(uLit, 0);
    glUniform1i(uFog, 0);
}

void drawNumber(int value, int digits, float rightX, float y, float scale) {
    for (int i = 0; i < digits; ++i) {
        hudQuad("Digit_" + std::to_string(value % 10), rightX - 13 * scale * (i + 1), y, 100, 0, 1, -1, -1, scale);
        value /= 10;
    }
}


// ---------------------------------------------------------------- text (system font through FreeType)
struct Glyph {
    float u, v, w, h; // atlas rectangle, pixels
    float left, top, advance;
};
const int kFontPx = 40, kAtlas = 512;
Glyph g_glyphs[128];
GLuint g_fontTex = 0;

void initFont() {
    PlFontData font;
    FT_Library lib;
    FT_Face face;
    if (R_FAILED(plInitialize(PlServiceType_User)) || R_FAILED(plGetSharedFontByType(&font, PlSharedFontType_Standard))) {
        logf("no system font: text is disabled");
        return;
    }
    if (FT_Init_FreeType(&lib) || FT_New_Memory_Face(lib, (const FT_Byte*)font.address, font.size, 0, &face)) {
        logf("FreeType could not open the system font: text is disabled");
        return;
    }
    FT_Set_Pixel_Sizes(face, 0, kFontPx);
    std::vector<unsigned char> px((size_t)kAtlas * kAtlas * 4, 0);
    int x = 1, y = 1, rowH = 0;
    for (int c = 32; c < 127; ++c) {
        if (FT_Load_Char(face, c, FT_LOAD_RENDER)) continue;
        FT_GlyphSlot gs = face->glyph;
        int w = gs->bitmap.width, h = gs->bitmap.rows;
        if (x + w + 1 >= kAtlas) {
            x = 1;
            y += rowH + 1;
            rowH = 0;
        }
        if (y + h + 1 >= kAtlas) break;
        for (int r = 0; r < h; ++r)
            for (int i = 0; i < w; ++i) {
                unsigned char* d = &px[((size_t)(y + r) * kAtlas + x + i) * 4];
                d[0] = d[1] = d[2] = 255;
                d[3] = gs->bitmap.buffer[r * gs->bitmap.pitch + i];
            }
        g_glyphs[c] = {(float)x, (float)y, (float)w, (float)h, (float)gs->bitmap_left, (float)gs->bitmap_top, gs->advance.x / 64.0f};
        x += w + 1;
        rowH = h > rowH ? h : rowH;
    }
    glGenTextures(1, &g_fontTex);
    glBindTexture(GL_TEXTURE_2D, g_fontTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kAtlas, kAtlas, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    FT_Done_Face(face);
    FT_Done_FreeType(lib);
    logf("system font loaded");
}

// Textured quad in stage coordinates with an explicit uv rectangle (0..1).
void stageQuad(GLuint tex, float x, float y, float w, float h, const glm::vec4& color, float u0 = 0, float v0 = 0,
               float u1 = 1, float v1 = 1) {
    glm::mat4 m = glm::translate(glm::mat4(1), glm::vec3(kHudLeft + x * kHudScale, y * kHudScale, 0));
    m = glm::scale(m, glm::vec3(w * kHudScale, h * kHudScale, 1));
    glm::mat4 mvp = glm::ortho(0.0f, (float)kScreenW, (float)kScreenH, 0.0f) * m;
    glUniformMatrix4fv(uMvp, 1, GL_FALSE, glm::value_ptr(mvp));
    glUniform4fv(uColor, 1, glm::value_ptr(color));
    glUniform1i(uUseTex, tex != 0);
    setUv(u1 - u0, v1 - v0, u0, v0);
    if (tex) glBindTexture(GL_TEXTURE_2D, tex);
    glBindVertexArray(g_quad);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

} // namespace

namespace ui {

void begin() {
    glUseProgram(g_program);
    glActiveTexture(GL_TEXTURE0);
    beginHud();
}

float left() { return -kHudLeft / kHudScale; }
float right() { return 512 + kHudLeft / kHudScale; }

void fill(const glm::vec4& color) { fullScreen(color.r, color.g, color.b, color.a); }

void rect(float x, float y, float w, float h, const glm::vec4& color) { stageQuad(0, x, y, w, h, color); }

void image(const std::string& name, float x, float y, float w, float h, float alpha) {
    const UiTexture& t = uiTexture(name);
    if (!t.texture) return;
    if (w < 0) w = (float)t.width;
    if (h < 0) h = (float)t.height;
    stageQuad(t.texture, x, y, w, h, glm::vec4(1, 1, 1, alpha));
}

void texture(unsigned tex, float x, float y, float w, float h, float alpha) {
    if (tex) stageQuad(tex, x, y, w, h, glm::vec4(1, 1, 1, alpha));
}

void imageRegion(const std::string& name, float x, float y, float w, float h, float u0, float v0, float u1, float v1, float alpha) {
    const UiTexture& t = uiTexture(name);
    if (t.texture) stageQuad(t.texture, x, y, w, h, glm::vec4(1, 1, 1, alpha), u0, v0, u1, v1);
}

bool hasImage(const std::string& name) { return uiTexture(name).texture != 0; }

void frame(float x, float y, float w, float h, float t, const glm::vec4& color) {
    rect(x - t, y - t, w + 2 * t, t, color);
    rect(x - t, y + h, w + 2 * t, t, color);
    rect(x - t, y, t, h, color);
    rect(x + w, y, t, h, color);
}

void carSpinner(const Model& m5, float x, float y, float w, float h, float angle, char gadget) {
    // doSpinner: camera at (-3, -3, 1.5) looking at the group's origin lowered by 0.5, white ambient light
    int vx = (int)(kHudLeft + x * kHudScale), vw = (int)(w * kHudScale), vh = (int)(h * kHudScale);
    int vy = kScreenH - (int)(y * kHudScale) - vh;
    glViewport(vx, vy, vw, vh);
    glEnable(GL_SCISSOR_TEST);
    glScissor(vx, vy, vw, vh);
    glClear(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glUniform1i(uLit, 1);
    glUniform3f(uAmbient, 1, 1, 1);
    setUv();
    glm::mat4 view = glm::lookAt(glm::vec3(-3, -3, 1.5f), glm::vec3(0, 0, -0.5f), glm::vec3(0, 0, 1));
    g_viewProj = glm::perspective(glm::radians(50.0f), w / h, 0.3f, 50.0f) * view;
    glm::mat4 spin = glm::rotate(glm::mat4(1), glm::radians(angle), glm::vec3(0, 0, 1));
    static const char* always[] = {"mach5_body", "scope_door", "mach5_wheel_FL", "mach5_wheel_FR", "mach5_wheel_RR", "mach5_wheel_RL"};
    auto part = [&](const char* name, int pass) {
        if (const ModelNode* n = m5.find(name))
            for (const Batch& b : n->batches)
                if (isBlend(b, m5.materials) == (pass == 1)) drawBatch(b, m5.materials, spin * n->matrix);
    };
    for (int pass = 0; pass < 2; ++pass) {
        if (pass == 1) {
            glEnable(GL_BLEND);
            glDepthMask(GL_FALSE);
        } else {
            glDisable(GL_BLEND);
        }
        for (const char* n : always) part(n, pass);
        part("windshield", pass);
        switch (gadget) {
        case 'a': part("jacks", pass); break;
        case 'b': part("mach5_grip_FL", pass); part("mach5_grip_FR", pass); part("mach5_grip_RR", pass); part("mach5_grip_RL", pass); break;
        case 'c': part("saw", pass); break;
        case 'd': part("windshield_top", pass); break;
        case 'e': part("lights", pass); break;
        case 'f': part("scope_top", pass); part("scope_bar", pass); break;
        case 'g': part("ROBOT", pass); break;
        default: break;
        }
    }
    glDepthMask(GL_TRUE);
    glUniform3f(uAmbient, 185 / 255.0f, 185 / 255.0f, 1.0f);
    beginHud();
}

float textWidth(const std::string& s, float size) {
    float w = 0;
    for (unsigned char c : s)
        if (c < 127) w += g_glyphs[c].advance;
    return w * size / kFontPx;
}

float text(const std::string& s, float x, float y, float size, const glm::vec4& color, int align) {
    float width = textWidth(s, size);
    if (!g_fontTex) return width;
    const float k = size / kFontPx;
    if (align == 1) x -= width * 0.5f;
    else if (align == 2) x -= width;
    for (unsigned char c : s) {
        if (c >= 127) continue;
        const Glyph& g = g_glyphs[c];
        if (g.w > 0)
            stageQuad(g_fontTex, x + g.left * k, y + (kFontPx * 0.8f - g.top) * k, g.w * k, g.h * k, color, g.u / kAtlas,
                      g.v / kAtlas, (g.u + g.w) / kAtlas, (g.v + g.h) / kAtlas);
        x += g.advance * k;
    }
    return width;
}

} // namespace ui

bool Renderer::init() {
    GLuint vs = compile(GL_VERTEX_SHADER, kVertex), fs = compile(GL_FRAGMENT_SHADER, kFragment);
    g_program = glCreateProgram();
    glAttachShader(g_program, vs);
    glAttachShader(g_program, fs);
    glLinkProgram(g_program);
    GLint ok = 0;
    glGetProgramiv(g_program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char buf[512];
        glGetProgramInfoLog(g_program, sizeof buf, nullptr, buf);
        logf("shader link: %s", buf);
        return false;
    }
    uMvp = glGetUniformLocation(g_program, "uMvp");
    uNormal = glGetUniformLocation(g_program, "uNormal");
    uColor = glGetUniformLocation(g_program, "uColor");
    uUseTex = glGetUniformLocation(g_program, "uUseTex");
    uLit = glGetUniformLocation(g_program, "uLit");
    uAmbient = glGetUniformLocation(g_program, "uAmbient");
    uSunDir = glGetUniformLocation(g_program, "uSunDir");
    uUv = glGetUniformLocation(g_program, "uUv");
    uFog = glGetUniformLocation(g_program, "uFog");
    uFogColor = glGetUniformLocation(g_program, "uFogColor");
    uFogRange = glGetUniformLocation(g_program, "uFogRange");

    // Unit quad (0..1) with uv, used for the HUD and for billboards.
    static const float quad[] = {0, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0, 1, 1, 0,
                                 0, 1, 0, 0, 0, 1, 0, 1, 1, 1, 0, 0, 0, 1, 1, 1};
    GLuint vbo;
    glGenVertexArrays(1, &g_quad);
    glBindVertexArray(g_quad);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 32, (void*)0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 32, (void*)12);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 32, (void*)24);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glBindVertexArray(0);
    initFont();
    return true;
}

void Renderer::draw(Game& g) {
    glUseProgram(g_program);
    glActiveTexture(GL_TEXTURE0);
    glUniform3f(uAmbient, 185 / 255.0f, 185 / 255.0f, 1.0f); // PREP: ambient light rgb(185, 185, 255)
    glm::vec3 sun = glm::normalize(glm::vec3(-0.5f, -0.3f, 0.8f));
    glUniform3fv(uSunDir, 1, glm::value_ptr(sun));
    glViewport(0, 0, kScreenW, kScreenH);

    glm::vec3 fog(100 / 255.0f, 150 / 255.0f, 1.0f);
    if (g.isUnderWater) glClearColor(fog.r, fog.g, fog.b, 1);
    else glClearColor(0.45f, 0.62f, 0.85f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // PREP: projectionAngle 40, yon 300
    glm::mat4 view = glm::lookAt(g.eye, g.lookAt, glm::vec3(0, 0, 1));
    glm::mat4 proj = glm::perspective(glm::radians(40.0f), (float)kScreenW / kScreenH, 0.5f, 300.0f);
    g.viewProj = proj * view;
    drawWorld(g, view, proj, g.eye);

    beginHud();
    if (g.SCOPEMODE != 0) hudQuad("ov_scopecam_under", 347 + hudEdge(), 0, g.scopeUnderBlend);
    if (g.scopeViewOn) {
        // doCamera: the second camera's rect(362, 12, 499, 115), projectionAngle 60, 2 units above the car
        const float sx = 362 + hudEdge(), sy = 12, sw = 137, sh = 103;
        int vx = (int)(kHudLeft + sx * kHudScale), vw = (int)(sw * kHudScale), vh = (int)(sh * kHudScale);
        int vy = kScreenH - (int)(sy * kHudScale) - vh;
        glViewport(vx, vy, vw, vh);
        glEnable(GL_SCISSOR_TEST);
        glScissor(vx, vy, vw, vh);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glm::vec3 pos = g.car.body.origin() + glm::vec3(0, 0, 2);
        glm::vec3 at = pos + g.car.body.rotation() * glm::vec3(0, 6, 0);
        drawWorld(g, glm::lookAt(pos, at, glm::vec3(0, 0, 1)), glm::perspective(glm::radians(60.0f), sw / sh, 0.5f, 300.0f), pos);
        g_viewProj = g.viewProj;
        beginHud();
        hudQuad("ov_scopecam_static", sx, sy, g.scopeStaticBlend, 0, sw / 256, sw, sh);
        hudQuad("ov_scopecam_frame", sx, sy, 100, 0, sw / 256, sw, sh);
    }
    for (const Overlay& o : g.overlays()) hudQuad(o.texture, o.x + o.anchor * hudEdge(), o.y, o.blend, o.u0, o.u1, o.width);
    setUv();
    if (g.helpBlend > 0 && g_cfg.showHelp) { // the original panel listed keyboard keys; this one lists the pad
        static const char* rows[][2] = {{"A / ZR", "DRIVE"}, {"B / ZL", "BRAKE"}, {"X", "JACKS"}, {"L", "BELT TIRES"},
                                        {"Y", "CUTTER BLADES"}, {"-", "DEFLECTOR"}, {"UP", "ILLUMINATION"},
                                        {"DOWN", "FROGGER MODE"}, {"R", "HOMING ROBOT"}, {"L3", "SHOW/HIDE HELP"}};
        float a = g.helpBlend / 100.0f;
        const float hx = 8 - hudEdge(), hy = 180; // PREP: ovHelp at point(8, 180), against the left edge
        ui::rect(hx, hy, 150, 14 + 13 * 10, glm::vec4(0, 0, 0.1f, 0.6f * a));
        for (int i = 0; i < 10; ++i) {
            ui::text(rows[i][0], hx + 6, hy + 6 + 13 * i, 10, glm::vec4(1, 0.85f, 0.2f, a));
            ui::text(rows[i][1], hx + 50, hy + 6 + 13 * i, 10, glm::vec4(1, 1, 1, a));
        }
        if (!g.isStarted && g.allReady) ui::text("PRESS A TO START", 256, 112, 16, glm::vec4(1, 1, 1, a), 1);
    }
    if (g.blackBlend > 0) fullScreen(0, 0, 0, g.blackBlend / 100.0f);
}
