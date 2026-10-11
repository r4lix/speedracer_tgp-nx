// Player and debug settings, edited in the Dear ImGui menu (src/menu.cpp) and kept in sdmc:/speedracer.cfg.
#pragma once

#include <cstdint>

// Pad buttons that can be given to an action (the d-pad's left and right steer, Plus is "back").
enum PadBtn { BtnA, BtnB, BtnX, BtnY, BtnL, BtnR, BtnZL, BtnZR, BtnL3, BtnR3, BtnMinus, BtnUp, BtnDown, BtnNone, BtnCount };
// What a button can be set to do. Drive and Brake are held; the others are presses. Menu is a long press (1.5 s).
enum Action { ActDrive, ActBrake, ActJacks, ActGrip, ActBlades, ActDeflector, ActLights, ActFrogger, ActRobot, ActHelp, ActMenu, ActionCount };

struct Settings {
    // controls
    bool dpadSteer = true;       // D-pad steers
    bool stickSteer = true;      // left stick steers
    bool invertSteer = false;
    float stickDeadzone = 0.37f; // 0..1 of the stick's travel (the original's 12000 / 32767)
    int bind[ActionCount] = {BtnZR, BtnZL, BtnA, BtnB, BtnY, BtnMinus, BtnUp, BtnDown, BtnR, BtnL3, BtnMinus};
    bool autoAccelerate = false; // throttle held without a button, brake still works
    // audio
    bool mute = false;
    float masterVolume = 1.0f;
    // interface
    bool hudAnchored = true;     // HUD pieces sit against the screen edges (off: the original's centred 4:3 layout)
    bool showHelp = true;        // the key list when a race is waiting for the start
    bool showFps = false;
    bool showStats = false;      // speed, position, surface, wheels
    bool skipIntro = false;      // next start
    // debug
    bool noDamage = false;
    bool autopilot = false;
    bool jackLoop = false;       // jacks again on every landing
    float timeScale = 1.0f;      // game speed, 0.25 .. 2
    int startPath = 0;           // path point (1..209) the next restart begins at, 0 = the grid
    bool logHover = false;       // per-frame wheel numbers into speedracer.log
};

extern Settings g_cfg;
void loadSettings();
void saveSettings();

// Button bindings
const char* actionName(int action);    // "Jacks"
const char* buttonName(int button);    // "ZR", "Minus"; button is a PadBtn
uint64_t buttonMask(int button);       // the HidNpadButton bit, 0 for BtnNone
inline uint64_t actionMask(int action) { return buttonMask(g_cfg.bind[action]); }
// Short label for on-screen lists: the bound button, upper case ("-" for Minus).
const char* actionLabel(int action);
// Gives the action the button. A button is used by one action only (the other one takes the old button),
// except that the menu button may share a button with a press action: its short press still reaches the game.
void assignButton(int action, int button);
void resetBindings();
