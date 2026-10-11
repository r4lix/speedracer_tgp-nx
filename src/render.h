#pragma once
#include <string>

#include "game.h"

class Renderer {
public:
    bool init();        // shader, shared geometry and the font; needs a current GL context
    void draw(Game& g); // the race: 3D scene and HUD for one 1280x720 frame; also refreshes g.viewProj
};

// 2D drawing for the front end, in the original 512x384 stage coordinates (the stage is centred on the
// 16:9 screen, so x runs from left() to right()). Call begin() once per frame before the others.
namespace ui {
void begin();
float left();
float right();
void fill(const glm::vec4& color); // whole screen
void rect(float x, float y, float w, float h, const glm::vec4& color);
void image(const std::string& name, float x, float y, float w = -1, float h = -1, float alpha = 1); // romfs:/ui bitmap
void texture(unsigned tex, float x, float y, float w, float h, float alpha = 1);
// Part of a bitmap: u/v are fractions of the image.
void imageRegion(const std::string& name, float x, float y, float w, float h, float u0, float v0, float u1, float v1,
                 float alpha = 1);
bool hasImage(const std::string& name);
void frame(float x, float y, float w, float h, float thickness, const glm::vec4& color); // outline
// The "About the Mach 5" turntable: the car model in a stage rectangle, turned by angle degrees, with the
// part that belongs to gadget 'a'..'g' shown (0 = none).
void carSpinner(const Model& mach5, float x, float y, float w, float h, float angle, char gadget);
float textWidth(const std::string& s, float size);
// size is the line height in stage pixels; align 0 left, 1 centred, 2 right. Returns the width.
float text(const std::string& s, float x, float y, float size, const glm::vec4& color, int align = 0);
} // namespace ui
