// Player and debug settings, edited in the Dear ImGui menu (src/menu.cpp) and kept in sdmc:/speedracer.cfg.
#pragma once

struct Settings {
    // controls
    bool dpadSteer = true;       // D-pad steers
    bool stickSteer = true;      // left stick steers
    bool invertSteer = false;
    float stickDeadzone = 0.37f; // 0..1 of the stick's travel (the original's 12000 / 32767)
    bool accelA = true, accelZR = true, brakeB = true, brakeZL = true;
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
