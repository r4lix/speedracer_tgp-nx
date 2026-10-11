#include "settings.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <switch.h>

Settings g_cfg;

namespace {
const char* kFile = "sdmc:/speedracer.cfg";

struct Entry {
    const char* key;
    enum { Bool, Float, Int } type;
    void* ptr;
};

Entry entries[] = {
    {"dpadSteer", Entry::Bool, &g_cfg.dpadSteer},       {"stickSteer", Entry::Bool, &g_cfg.stickSteer},
    {"invertSteer", Entry::Bool, &g_cfg.invertSteer},   {"stickDeadzone", Entry::Float, &g_cfg.stickDeadzone},
    {"autoAccelerate", Entry::Bool, &g_cfg.autoAccelerate}, {"mute", Entry::Bool, &g_cfg.mute},
    {"masterVolume", Entry::Float, &g_cfg.masterVolume}, {"hudAnchored", Entry::Bool, &g_cfg.hudAnchored},
    {"showHelp", Entry::Bool, &g_cfg.showHelp},         {"showFps", Entry::Bool, &g_cfg.showFps},
    {"showStats", Entry::Bool, &g_cfg.showStats},       {"skipIntro", Entry::Bool, &g_cfg.skipIntro},
    {"noDamage", Entry::Bool, &g_cfg.noDamage},         {"autopilot", Entry::Bool, &g_cfg.autopilot},
    {"timeScale", Entry::Float, &g_cfg.timeScale},
    {"startPath", Entry::Int, &g_cfg.startPath},        {"logHover", Entry::Bool, &g_cfg.logHover},
    {"infiniteOxygen", Entry::Bool, &g_cfg.infiniteOxygen}, {"neverFall", Entry::Bool, &g_cfg.neverFall},
    {"unlimitedGadgets", Entry::Bool, &g_cfg.unlimitedGadgets}, {"airJacks", Entry::Bool, &g_cfg.airJacks},
    {"jackPower", Entry::Float, &g_cfg.jackPower},      {"topSpeed", Entry::Float, &g_cfg.topSpeed},
    {"turbo", Entry::Bool, &g_cfg.turbo},               {"turboPower", Entry::Float, &g_cfg.turboPower},
    {"fly", Entry::Bool, &g_cfg.fly},                   {"gravityScale", Entry::Float, &g_cfg.gravityScale},
    {"ghostCars", Entry::Bool, &g_cfg.ghostCars},       {"rivalSpeed", Entry::Float, &g_cfg.rivalSpeed},
    {"pickupReach", Entry::Float, &g_cfg.pickupReach},
};
struct ButtonInfo {
    const char* name;
    const char* label;
    uint64_t mask;
};
const ButtonInfo kButtons[BtnCount] = {
    {"A", "A", HidNpadButton_A},        {"B", "B", HidNpadButton_B},          {"X", "X", HidNpadButton_X},
    {"Y", "Y", HidNpadButton_Y},        {"L", "L", HidNpadButton_L},          {"R", "R", HidNpadButton_R},
    {"ZL", "ZL", HidNpadButton_ZL},     {"ZR", "ZR", HidNpadButton_ZR},       {"L3", "L3", HidNpadButton_StickL},
    {"R3", "R3", HidNpadButton_StickR}, {"Minus", "-", HidNpadButton_Minus},  {"Up", "UP", HidNpadButton_Up},
    {"Down", "DOWN", HidNpadButton_Down}, {"None", "-", 0},
};
const char* kActionNames[ActionCount] = {"Drive", "Brake / reverse", "Jacks", "Belt tires", "Cutter blades", "Deflector",
                                         "Illumination", "Frogger mode", "Homing robot", "Help panel", "Turbo (cheat)", "Fly up (cheat)",
                                         "Open settings (hold 1.5 s)"};
const char* kActionKeys[ActionCount] = {"drive", "brake", "jacks", "belt", "cutter", "deflector", "lights", "frogger", "robot", "help", "turbo", "flyup", "menu"};

bool pressAction(int a) { return a != ActDrive && a != ActBrake && a != ActMenu && a != ActBoost && a != ActFlyUp; }
} // namespace

const char* actionName(int action) { return kActionNames[action]; }
const char* buttonName(int button) { return kButtons[button].name; }
uint64_t buttonMask(int button) { return button >= 0 && button < BtnCount ? kButtons[button].mask : 0; }
const char* actionLabel(int action) { return kButtons[g_cfg.bind[action]].label; }

void assignButton(int action, int button) {
    const int old = g_cfg.bind[action];
    for (int other = 0; other < ActionCount; ++other) {
        if (other == action || g_cfg.bind[other] != button || button == BtnNone) continue;
        const bool share = (action == ActMenu && pressAction(other)) || (other == ActMenu && pressAction(action));
        if (!share) g_cfg.bind[other] = old;
    }
    g_cfg.bind[action] = button;
}

void resetCheats() {
    const Settings d;
    g_cfg.noDamage = d.noDamage;
    g_cfg.infiniteOxygen = d.infiniteOxygen;
    g_cfg.neverFall = d.neverFall;
    g_cfg.unlimitedGadgets = d.unlimitedGadgets;
    g_cfg.airJacks = d.airJacks;
    g_cfg.jackPower = d.jackPower;
    g_cfg.topSpeed = d.topSpeed;
    g_cfg.turbo = d.turbo;
    g_cfg.turboPower = d.turboPower;
    g_cfg.fly = d.fly;
    g_cfg.gravityScale = d.gravityScale;
    g_cfg.ghostCars = d.ghostCars;
    g_cfg.rivalSpeed = d.rivalSpeed;
    g_cfg.pickupReach = d.pickupReach;
}

void resetBindings() {
    const Settings def;
    for (int i = 0; i < ActionCount; ++i) g_cfg.bind[i] = def.bind[i];
}

void loadSettings() {
    FILE* f = fopen(kFile, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        if (strncmp(line, "bind_", 5) == 0) { // bind_<action>=<button name>
            char* v = eq + 1;
            v[strcspn(v, "\r\n")] = 0;
            for (int a = 0; a < ActionCount; ++a) {
                if (strcmp(line + 5, kActionKeys[a]) != 0) continue;
                for (int b = 0; b < BtnCount; ++b)
                    if (strcmp(v, kButtons[b].name) == 0) g_cfg.bind[a] = b;
            }
            continue;
        }
        for (Entry& e : entries) {
            if (strcmp(e.key, line) != 0) continue;
            if (e.type == Entry::Bool) *(bool*)e.ptr = atoi(eq + 1) != 0;
            else if (e.type == Entry::Int) *(int*)e.ptr = atoi(eq + 1);
            else *(float*)e.ptr = (float)atof(eq + 1);
        }
    }
    fclose(f);
    if (g_cfg.timeScale < 0.25f || g_cfg.timeScale > 2.0f) g_cfg.timeScale = 1;
    if (g_cfg.stickDeadzone < 0.05f || g_cfg.stickDeadzone > 0.95f) g_cfg.stickDeadzone = 0.37f;
    if (g_cfg.masterVolume < 0 || g_cfg.masterVolume > 1) g_cfg.masterVolume = 1;
    if (g_cfg.startPath < 0 || g_cfg.startPath > 209) g_cfg.startPath = 0;
    g_cfg.jackPower = std::min(4.0f, std::max(1.0f, g_cfg.jackPower));
    g_cfg.topSpeed = std::min(3.0f, std::max(1.0f, g_cfg.topSpeed));
    g_cfg.turboPower = std::min(3.0f, std::max(0.2f, g_cfg.turboPower));
    g_cfg.gravityScale = std::min(1.0f, std::max(0.1f, g_cfg.gravityScale));
    g_cfg.rivalSpeed = std::min(1.5f, std::max(0.0f, g_cfg.rivalSpeed));
    g_cfg.pickupReach = std::min(40.0f, std::max(0.0f, g_cfg.pickupReach));
}

void saveSettings() {
    FILE* f = fopen(kFile, "w");
    if (!f) return;
    for (const Entry& e : entries) {
        if (e.type == Entry::Bool) fprintf(f, "%s=%d\n", e.key, *(bool*)e.ptr ? 1 : 0);
        else if (e.type == Entry::Int) fprintf(f, "%s=%d\n", e.key, *(int*)e.ptr);
        else fprintf(f, "%s=%.3f\n", e.key, *(float*)e.ptr);
    }
    for (int a = 0; a < ActionCount; ++a) fprintf(f, "bind_%s=%s\n", kActionKeys[a], kButtons[g_cfg.bind[a]].name);
    fclose(f);
}
