#include "menu.h"

#include <cfloat>
#include <cmath>
#include <cstdio>

#include <switch.h>

#include "audio.h"
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "settings.h"
#include "shell.h"

void logf(const char* fmt, ...); // main.cpp

namespace {
bool g_open = false;
bool g_ready = false;
uint64_t g_prevHeld = 0;
bool g_touching = false;
int g_forceTab = -1;

void padKey(ImGuiIO& io, ImGuiKey key, uint64_t held, uint64_t bit) { io.AddKeyEvent(key, (held & bit) != 0); }

void analog(ImGuiIO& io, ImGuiKey neg, ImGuiKey pos, int v) {
    float f = v / 32767.0f;
    io.AddKeyAnalogEvent(neg, f < -0.25f, f < 0 ? -f : 0);
    io.AddKeyAnalogEvent(pos, f > 0.25f, f > 0 ? f : 0);
}

void style() {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 6;
    s.FrameRounding = 4;
    s.TabRounding = 4;
    s.ItemSpacing = ImVec2(10, 8);
    s.FramePadding = ImVec2(10, 8);    // finger-sized
    s.GrabMinSize = 34;
    s.ScrollbarSize = 26;
    s.TouchExtraPadding = ImVec2(6, 6);
    s.Colors[ImGuiCol_Header] = ImVec4(0.62f, 0.07f, 0.09f, 0.75f);
    s.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.8f, 0.1f, 0.12f, 0.9f);
    s.Colors[ImGuiCol_HeaderActive] = ImVec4(0.9f, 0.15f, 0.15f, 1);
    s.Colors[ImGuiCol_CheckMark] = ImVec4(1, 0.85f, 0.15f, 1);
    s.Colors[ImGuiCol_SliderGrab] = ImVec4(1, 0.85f, 0.15f, 1);
    s.Colors[ImGuiCol_Tab] = ImVec4(0.35f, 0.05f, 0.07f, 1);
    s.Colors[ImGuiCol_TabHovered] = ImVec4(0.8f, 0.1f, 0.12f, 1);
    s.Colors[ImGuiCol_TabActive] = ImVec4(0.65f, 0.08f, 0.1f, 1);
    s.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.5f, 0.06f, 0.08f, 1);
}

void tip(const char* text) {
    if (ImGui::IsItemHovered() || ImGui::IsItemFocused()) ImGui::SetTooltip("%s", text);
}

void controls() {
    ImGui::SeparatorText("Steering");
    ImGui::Checkbox("D-pad steers", &g_cfg.dpadSteer);
    ImGui::Checkbox("Left stick steers", &g_cfg.stickSteer);
    ImGui::SliderFloat("Stick dead zone", &g_cfg.stickDeadzone, 0.05f, 0.9f, "%.2f");
    tip("How far the stick has to move before the car turns.");
    ImGui::Checkbox("Invert steering", &g_cfg.invertSteer);
    ImGui::SeparatorText("Pedals");
    ImGui::Checkbox("A accelerates", &g_cfg.accelA);
    ImGui::SameLine();
    ImGui::Checkbox("ZR accelerates", &g_cfg.accelZR);
    ImGui::Checkbox("B brakes", &g_cfg.brakeB);
    ImGui::SameLine();
    ImGui::Checkbox("ZL brakes", &g_cfg.brakeZL);
    ImGui::Checkbox("Throttle always on", &g_cfg.autoAccelerate);
    tip("The car accelerates without a button. Braking still works.");
    ImGui::TextDisabled("Gadgets: X jacks  Y blades  L grip  R robot  Up lights  Down frogger  - deflector  L3 help");
}

void audioTab() {
    if (ImGui::Checkbox("Mute", &g_cfg.mute)) audio::setMuted(g_cfg.mute);
    if (ImGui::SliderFloat("Master volume", &g_cfg.masterVolume, 0.0f, 1.0f, "%.2f")) audio::setMasterVolume(g_cfg.masterVolume);
}

void interfaceTab() {
    ImGui::Checkbox("HUD against the screen edges", &g_cfg.hudAnchored);
    tip("Off: the original's centred 4:3 layout.");
    ImGui::Checkbox("Key list while waiting for the start", &g_cfg.showHelp);
    ImGui::Checkbox("Skip the opening animation", &g_cfg.skipIntro);
    ImGui::SeparatorText("Overlays");
    ImGui::Checkbox("Frame rate", &g_cfg.showFps);
    ImGui::Checkbox("Car stats", &g_cfg.showStats);
}

void debugTab(Shell& shell) {
    ImGui::SeparatorText("Cheats");
    ImGui::Checkbox("No damage", &g_cfg.noDamage);
    ImGui::Checkbox("Autopilot", &g_cfg.autopilot);
    tip("The car follows the path points by itself.");
    ImGui::Checkbox("Jacks on every landing", &g_cfg.jackLoop);
    ImGui::SliderFloat("Game speed", &g_cfg.timeScale, 0.25f, 2.0f, "%.2fx");
    ImGui::SeparatorText("Race");
    ImGui::SliderInt("Start at path point", &g_cfg.startPath, 0, 209, g_cfg.startPath ? "%d" : "the grid");
    if (ImGui::Button("Restart race")) {
        shell.game.debugStartPath = g_cfg.startPath;
        shell.startRace();
        g_open = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Back to main page")) {
        shell.goToMainPage();
        g_open = false;
    }
    ImGui::SeparatorText("Log");
    ImGui::Checkbox("Wheel numbers every frame", &g_cfg.logHover);
    tip("speedracer.log gets one line per frame: height, speed, wheel distances, attitude.");
    if (ImGui::Button("Reset all settings")) {
        g_cfg = Settings();
        audio::setMuted(false);
        audio::setMasterVolume(1);
    }
}

void overlays(Shell& shell, double fps) {
    const bool racing = shell.state == Shell::State::Race;
    if (!g_cfg.showFps && !(g_cfg.showStats && racing)) return;
    ImGui::SetNextWindowPos(ImVec2(8, 8));
    ImGui::SetNextWindowBgAlpha(0.45f);
    ImGuiWindowFlags f = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##overlay", nullptr, f)) {
        if (g_cfg.showFps) ImGui::Text("%.0f fps", fps);
        if (g_cfg.showStats && racing) {
            const Game& g = shell.game;
            glm::vec3 p = g.car.body.origin();
            ImGui::Text("speed %.0f   place %d   damage %.0f", g.car.forwardVel, g.gPosition, g.gDamage);
            ImGui::Text("x %.0f  y %.0f  z %.1f   surface %d", p.x, p.y, p.z, g.trackFace);
            ImGui::Text("wheels %.0f/4  %.2f %.2f %.2f %.2f", g.car.powerCoeff * 4, g.car.hoverDist[0], g.car.hoverDist[1],
                        g.car.hoverDist[2], g.car.hoverDist[3]);
        }
    }
    ImGui::End();
}
} // namespace

namespace menu {

void init() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.DisplaySize = ImVec2(1280, 720);
    io.FontGlobalScale = 1.4f;
    style();
    g_ready = ImGui_ImplOpenGL3_Init("#version 330 core");
    logf("menu: Dear ImGui %s, backend %s", ImGui::GetVersion(), g_ready ? "ready" : "FAILED");
}

void shutdown() {
    if (!g_ready) return;
    ImGui_ImplOpenGL3_Shutdown();
    ImGui::DestroyContext();
    g_ready = false;
}

bool isOpen() { return g_open; }

void showTab(int tab) {
    g_forceTab = tab;
    g_open = true;
}

void setOpen(bool open) {
    if (g_open && !open) saveSettings();
    g_open = open;
}

void feed(uint64_t held, int lx, int ly, double dt) {
    if (!g_ready) return;
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = dt > 1e-4 ? (float)dt : 1.0f / 60;
    // Switch A is the right-hand face button: it goes to ImGui's "activate" (Xbox A, the bottom one).
    padKey(io, ImGuiKey_GamepadFaceDown, held, HidNpadButton_A);
    padKey(io, ImGuiKey_GamepadFaceRight, held, HidNpadButton_B);
    padKey(io, ImGuiKey_GamepadFaceLeft, held, HidNpadButton_X);
    padKey(io, ImGuiKey_GamepadFaceUp, held, HidNpadButton_Y);
    padKey(io, ImGuiKey_GamepadDpadUp, held, HidNpadButton_Up);
    padKey(io, ImGuiKey_GamepadDpadDown, held, HidNpadButton_Down);
    padKey(io, ImGuiKey_GamepadDpadLeft, held, HidNpadButton_Left);
    padKey(io, ImGuiKey_GamepadDpadRight, held, HidNpadButton_Right);
    padKey(io, ImGuiKey_GamepadL1, held, HidNpadButton_L);
    padKey(io, ImGuiKey_GamepadR1, held, HidNpadButton_R);
    analog(io, ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight, lx);
    analog(io, ImGuiKey_GamepadLStickUp, ImGuiKey_GamepadLStickDown, -ly);
    // The touch screen is the mouse: a finger down is the left button held at that spot, up releases it and
    // moves the pointer away so nothing stays highlighted.
    {
        HidTouchScreenState ts = {};
        hidGetTouchScreenStates(&ts, 1);
        io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
        if (ts.count > 0) {
            io.AddMousePosEvent((float)ts.touches[0].x, (float)ts.touches[0].y);
            io.AddMouseButtonEvent(0, true);
            g_touching = true;
        } else if (g_touching) {
            io.AddMouseButtonEvent(0, false);
            io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
            g_touching = false;
        }
    }
    // R3 opens and closes; B closes unless a popup has it
    uint64_t pressed = held & ~g_prevHeld;
    g_prevHeld = held;
    if (pressed & HidNpadButton_StickR) setOpen(!g_open);
    else if (g_open && (pressed & HidNpadButton_B) && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) setOpen(false);
}

void draw(Shell& shell, double fps) {
    if (!g_ready) return;
    glViewport(0, 0, 1280, 720);
    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
    overlays(shell, fps);
    if (!g_open && shell.state != Shell::State::Race) {
        // a way in for touch play: a small button in the corner, outside the 4:3 page
        ImGui::SetNextWindowPos(ImVec2(1280 - 8, 8), ImGuiCond_Always, ImVec2(1, 0));
        ImGui::SetNextWindowBgAlpha(0.0f);
        ImGuiWindowFlags bf = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                              ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBackground;
        if (ImGui::Begin("##opener", nullptr, bf)) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.45f, 0.05f, 0.07f, 0.8f));
            if (ImGui::Button("Settings")) setOpen(true);
            ImGui::PopStyleColor();
        }
        ImGui::End();
    }
    if (g_open) {
        ImGui::SetNextWindowSize(ImVec2(900, 640), ImGuiCond_Appearing);
        ImGui::SetNextWindowPos(ImVec2(640, 360), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        bool open = true;
        if (ImGui::Begin("Speed Racer - settings   (R3 or B closes)", &open, ImGuiWindowFlags_NoCollapse)) {
            if (ImGui::BeginTabBar("tabs")) {
                auto flag = [](int t) { return g_forceTab == t ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None; };
                if (ImGui::BeginTabItem("Controls", nullptr, flag(0))) {
                    controls();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Audio", nullptr, flag(1))) {
                    audioTab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Interface", nullptr, flag(2))) {
                    interfaceTab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Debug", nullptr, flag(3))) {
                    debugTab(shell);
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
                g_forceTab = -1;
            }
            ImGui::Separator();
            if (ImGui::Button("Close", ImVec2(220, 0))) setOpen(false);
        }
        ImGui::End();
        if (!open) setOpen(false);
    }
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

} // namespace menu
