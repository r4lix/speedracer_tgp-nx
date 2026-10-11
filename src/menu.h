// The Dear ImGui settings menu: controls, audio, interface, debug. Opened with the right stick's click (R3).
#pragma once
#include <cstdint>

class Shell;

namespace menu {
void init();
void shutdown();
bool isOpen();
void setOpen(bool open);
void showTab(int tab); // open on one of the tabs (0 controls, 1 audio, 2 interface, 3 debug), for screenshots
// Pad state of this render frame: Switch buttons as HidNpadButton bits, left stick -32767..32767.
void feed(uint64_t held, int lx, int ly, double dt);
// Builds and renders the menu and the overlays (fps, stats). Call last in the frame, after the scene.
void draw(Shell& shell, double fps);
} // namespace menu
