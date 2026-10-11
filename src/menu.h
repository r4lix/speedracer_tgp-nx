// The Dear ImGui settings menu: controls, audio, interface, cheats, debug. Opened by holding Minus for 1.5 s; L and R turn the tabs.
#pragma once
#include <cstdint>

class Shell;

namespace menu {
void init();
void shutdown();
bool isOpen();
void setOpen(bool open);
void showTab(int tab); // open on one of the tabs (0 controls, 1 audio, 2 interface, 3 cheats, 4 debug), for screenshots
// Pad state of this render frame: Switch buttons as HidNpadButton bits, left stick -32767..32767.
void feed(uint64_t held, int lx, int ly, double dt);
// A finger came down outside the menu: where, in screen pixels (1280x720). Each tap is handed out once.
bool takeTap(float& px, float& py);
// Minus was pressed and let go before the hold opened the menu: the game gets its own Minus press then.
bool takeMinusTap();
// Builds and renders the menu and the overlays (fps, stats). Call last in the frame, after the scene.
void draw(Shell& shell, double fps);
} // namespace menu
