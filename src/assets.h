// Loaders for the files written by tools/bake.py, plus the GL objects made from them.
#pragma once
#include <string>
#include <vector>

#include <glad/glad.h>
#include <glm/glm.hpp>

#include "physics.h"

struct Material {
    glm::vec4 color{1};
    GLuint texture = 0;
    bool blend = false; // translucent colour or a texture with an alpha plane
};

struct Batch {
    int material = -1;
    GLuint vao = 0;
    GLsizei indexCount = 0;
};

struct ModelNode {
    std::string name;
    glm::mat4 matrix{1};
    std::vector<Batch> batches;
};

struct Model {
    std::vector<Material> materials;
    std::vector<ModelNode> nodes;
    const ModelNode* find(const char* name) const;
};

struct Marker {
    std::string name, kind;
    glm::mat4 matrix{1};
};

struct LevelBatch {
    int segment = 0; // zero-based track segment (300 units of Y each)
    Batch batch;
};

// A level model the game moves, hides or removes on its own: trees, pickups, water.
struct LevelObject {
    std::string name, kind;
    glm::mat4 matrix{1};
    int segment = 0;
    glm::vec3 center{0}; // bounding sphere, model space
    float radius = 0;
    std::vector<Batch> batches;
};

struct Level {
    std::vector<Material> materials;
    std::vector<Batch> sky;          // model space, drawn around the camera
    std::vector<LevelBatch> batches; // world space
    std::vector<LevelObject> objects;
    std::vector<Marker> markers;
    CollisionWorld collision;
    const Marker* find(const char* name) const;
};

struct UiTexture {
    GLuint texture = 0;
    int width = 0, height = 0;
};

bool loadModel(const char* path, Model& out);
bool loadLevel(const char* path, Level& out);
// HUD bitmap from romfs:/ui: "<name>.png", or "<name>.jpg" with its alpha in "<name>.alpha.png". Cached.
const UiTexture& uiTexture(const std::string& name);
