#include "settings.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

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
    {"accelA", Entry::Bool, &g_cfg.accelA},             {"accelZR", Entry::Bool, &g_cfg.accelZR},
    {"brakeB", Entry::Bool, &g_cfg.brakeB},             {"brakeZL", Entry::Bool, &g_cfg.brakeZL},
    {"autoAccelerate", Entry::Bool, &g_cfg.autoAccelerate}, {"mute", Entry::Bool, &g_cfg.mute},
    {"masterVolume", Entry::Float, &g_cfg.masterVolume}, {"hudAnchored", Entry::Bool, &g_cfg.hudAnchored},
    {"showHelp", Entry::Bool, &g_cfg.showHelp},         {"showFps", Entry::Bool, &g_cfg.showFps},
    {"showStats", Entry::Bool, &g_cfg.showStats},       {"skipIntro", Entry::Bool, &g_cfg.skipIntro},
    {"noDamage", Entry::Bool, &g_cfg.noDamage},         {"autopilot", Entry::Bool, &g_cfg.autopilot},
    {"jackLoop", Entry::Bool, &g_cfg.jackLoop},         {"timeScale", Entry::Float, &g_cfg.timeScale},
    {"startPath", Entry::Int, &g_cfg.startPath},        {"logHover", Entry::Bool, &g_cfg.logHover},
};
} // namespace

void loadSettings() {
    FILE* f = fopen(kFile, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
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
}

void saveSettings() {
    FILE* f = fopen(kFile, "w");
    if (!f) return;
    for (const Entry& e : entries) {
        if (e.type == Entry::Bool) fprintf(f, "%s=%d\n", e.key, *(bool*)e.ptr ? 1 : 0);
        else if (e.type == Entry::Int) fprintf(f, "%s=%d\n", e.key, *(int*)e.ptr);
        else fprintf(f, "%s=%.3f\n", e.key, *(float*)e.ptr);
    }
    fclose(f);
}
