// Speed Racer: The Great Plan - revival. Nintendo Switch entry point: window, input and the main loop.
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

#include <switch.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <glad/glad.h>

#include "audio.h"
#include "render.h"
#include "shell.h"

// ---------------------------------------------------------------- logging
static FILE* g_log = nullptr;

void logf(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    svcOutputDebugString(buf, strlen(buf));
    if (g_log) {
        fprintf(g_log, "%s\n", buf);
        fflush(g_log);
    }
}

// ---------------------------------------------------------------- EGL
static EGLDisplay s_display;
static EGLContext s_context;
static EGLSurface s_surface;

static bool initEgl(NWindow* win) {
    s_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!s_display) return false;
    eglInitialize(s_display, nullptr, nullptr);
    if (eglBindAPI(EGL_OPENGL_API) == EGL_FALSE) return false;
    EGLConfig config;
    EGLint numConfigs;
    static const EGLint fbAttrs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                                     EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_NONE};
    eglChooseConfig(s_display, fbAttrs, &config, 1, &numConfigs);
    if (numConfigs == 0) return false;
    s_surface = eglCreateWindowSurface(s_display, config, win, nullptr);
    if (!s_surface) return false;
    static const EGLint ctxAttrs[] = {EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,
                                      EGL_CONTEXT_MAJOR_VERSION_KHR, 4, EGL_CONTEXT_MINOR_VERSION_KHR, 3, EGL_NONE};
    s_context = eglCreateContext(s_display, config, EGL_NO_CONTEXT, ctxAttrs);
    if (!s_context) return false;
    eglMakeCurrent(s_display, s_surface, s_surface, s_context);
    return true;
}

static void deinitEgl() {
    if (!s_display) return;
    eglMakeCurrent(s_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (s_context) eglDestroyContext(s_display, s_context);
    if (s_surface) eglDestroySurface(s_display, s_surface);
    eglTerminate(s_display);
    s_display = nullptr;
}

static bool fileExists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

// ---------------------------------------------------------------- debug aids
// Steers along the enemies' path points and holds the accelerator, so a race can run unattended.
static void autoPilot(const Game& g, Input& in) {
    glm::vec3 pos = g.car.body.origin();
    // Follow the path points in order (picking them by y alone cuts across the hairpins).
    static size_t next = 0;
    static long lastFrame = -1;
    const size_t count = g.pathList.size();
    if (next == 0 || g.frameCount < lastFrame) {
        next = 1;
        float best = 1e9f;
        for (size_t i = 1; i < count; ++i) {
            float d = glm::distance(glm::vec2(g.pathList[i]), glm::vec2(pos));
            if (d < best) {
                best = d;
                next = i;
            }
        }
    }
    lastFrame = g.frameCount;
    while (next + 1 < count && glm::distance(glm::vec2(g.pathList[next]), glm::vec2(pos)) < 14) ++next;
    glm::vec3 target = g.pathList[next], after = next + 1 < count ? g.pathList[next + 1] : target + glm::vec3(0, 30, 0);
    // How sharply the road bends at the point we are heading for.
    glm::vec2 d1 = glm::vec2(target) - glm::vec2(pos), d2 = glm::vec2(after) - glm::vec2(target);
    float bend = glm::length(d1) > 0.1f && glm::length(d2) > 0.1f
                     ? glm::degrees(std::acos(glm::clamp(glm::dot(glm::normalize(d1), glm::normalize(d2)), -1.0f, 1.0f)))
                     : 0.0f;
    float want = glm::degrees(std::atan2(-(target.x - pos.x), target.y - pos.y)); // yaw, 0 = +Y, counter-clockwise
    float diff = want - g.car.rotationDeg().z;
    while (diff > 180) diff -= 360;
    while (diff < -180) diff += 360;
    // Lift off, or brake, when the next point is well off the nose or the road is about to bend.
    float speed = g.car.forwardVel, off = std::fabs(diff);
    in.forward = !(off > 20 && speed > 45);
    in.backward = off > 35 && speed > 30;
    if (bend > 30 && glm::length(d1) < 45 && speed > 30) {
        in.forward = false;
        in.backward = speed > 38;
    }
    in.left = diff > 3;
    in.right = diff < -3;
    // Snow and ice: put the grip tyres on, and take it slower.
    const bool slippery = g.trackFace == 8 || g.trackFace == 9 || g.trackFace == 14 || g.trackFace == 15;
    in.grip = slippery && !g.pHasGrip;
    if (slippery && speed > 40) in.forward = false;
    // Nose against a tree or a wall: back out for a second and a half.
    static int slow = 0, reversing = 0;
    slow = g.isPlaying && std::fabs(speed) < 3 ? slow + 1 : 0;
    if (slow > 45) {
        reversing = 45;
        slow = 0;
    }
    if (reversing > 0) {
        --reversing;
        in.forward = false;
        in.backward = true;
        std::swap(in.left, in.right);
    }
}

// Walks through the front end on a schedule: mini-games, wallpapers, how to play, about, clips, then a race.
static void menuDemo(long frame, Input& in) {
    struct Step {
        long frame;
        char key; // u d l r: directions, a: confirm, b: cancel, x y: buttons
    };
    static const Step steps[] = {
        {60, 'u'}, {70, 'a'},                          // MINIGAMES tab
        {90, 'a'}, {110, 'a'}, {118, 'r'}, {126, 'a'}, // jigsaw: swap two pieces
        {150, 'y'}, {165, 'y'}, {175, 'b'},
        {190, 'r'}, {200, 'a'}, {215, 'l'}, {225, 'u'}, {235, 'r'}, {255, 'b'}, // sliders
        {270, 'u'}, {278, 'l'}, {286, 'a'},            // WALLPAPER tab
        {300, 'a'}, {335, 'a'}, {350, 'b'},            // view one, back to the main page
        {365, 'd'}, {373, 'l'}, {381, 'a'}, {410, 'b'}, // HOW TO PLAY
        {425, 'd'}, {433, 'l'}, {441, 'l'}, {449, 'a'}, // ABOUT THE MACH 5
        {470, 'a'}, {530, 'r'}, {538, 'a'}, {590, 'b'},
        {605, 'u'}, {613, 'l'}, {621, 'l'}, {629, 'a'}, // VIDEO CLIPS tab
        {650, 'a'}, {720, 'a'}, {790, 'b'}, {805, 'b'},
        {830, 'a'},                                    // PLAY!
    };
    for (const Step& st : steps) {
        if (st.frame != frame) continue;
        switch (st.key) {
        case 'u': in.up = true; break;
        case 'd': in.down = true; break;
        case 'l': in.leftPressed = true; break;
        case 'r': in.rightPressed = true; break;
        case 'a': in.confirm = true; break;
        case 'b': in.cancel = true; break;
        case 'x': in.jacks = true; break;
        case 'y': in.blades = true; break;
        default: break;
        }
    }
}

int main(int, char**) {
    g_log = fopen("sdmc:/speedracer.log", "w");
    logf("speedracer-revival starting");
    Result rc = romfsInit();
    if (R_FAILED(rc)) logf("romfsInit failed: 0x%x", rc);

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    if (!initEgl(nwindowGetDefault())) {
        logf("EGL init failed");
        return 1;
    }
    gladLoadGL();
    logf("GL %s / %s", (const char*)glGetString(GL_VERSION), (const char*)glGetString(GL_RENDERER));
    audio::init();

    // Debug aids, as files on the SD card root (see README).
    const bool autoDrive = fileExists("sdmc:/speedracer_autodrive");   // the car drives itself, the menu is skipped
    const bool noDamage = fileExists("sdmc:/speedracer_nodamage");
    const bool gadgetDemo = fileExists("sdmc:/speedracer_gadgets");     // fires each gadget in turn
    const bool menuTour = fileExists("sdmc:/speedracer_menudemo");      // walks through the front end
    const bool noIntro = fileExists("sdmc:/speedracer_nointro");        // skip the opening animation
    const bool holdStart = fileExists("sdmc:/speedracer_hold");         // with autodrive: wait on the start line

    static Shell shell;
    Game& game = shell.game;
    if (FILE* f = fopen("sdmc:/speedracer_startpath", "r")) { // a path point number to start the race from
        if (fscanf(f, "%d", &game.debugStartPath) != 1) game.debugStartPath = 0;
        fclose(f);
    }
    int forceResult = -1; // an outcome number (Game::Outcome) to end the race with after two seconds
    if (FILE* f = fopen("sdmc:/speedracer_result", "r")) {
        if (fscanf(f, "%d", &forceResult) != 1) forceResult = -1;
        fclose(f);
    }
    Renderer renderer;
    bool ok = renderer.init() && shell.init();
    if (ok && autoDrive) shell.startRace();
    else if (ok && !menuTour && !noIntro) shell.startIntro();
    logf(ok ? "assets loaded, entering main loop" : "STARTUP FAILED, showing a red screen");

    const double tickHz = (double)armGetSystemTickFreq();
    u64 lastTick = armGetSystemTick();
    double accumulator = 0;
    long drawn = 0;
    u64 pressed = 0; // buttons pressed since the last game frame
    int stickY = 0, stickX = 0; // -1, 0, 1: for menu navigation with the stick
    Game::Screen lastScreen = Game::Screen::Race;

    while (appletMainLoop() && !shell.quit) {
        padUpdate(&pad);
        u64 held = padGetButtons(&pad);
        u64 down = padGetButtonsDown(&pad);
        if (down & HidNpadButton_Plus) break;
        pressed |= down;

        u64 now = armGetSystemTick();
        const double elapsed = (double)(now - lastTick) / tickHz;
        accumulator += elapsed;
        lastTick = now;
        if (accumulator > 0.2) accumulator = 0.2;

        // The original runs its scripts and one Havok step per frame at 30 fps.
        while (ok && accumulator >= 1.0 / 30.0) {
            accumulator -= 1.0 / 30.0;
            HidAnalogStickState stick = padGetStickPos(&pad, 0);
            int sy = stick.y > 16000 ? 1 : stick.y < -16000 ? -1 : 0;
            Input in;
            in.left = (held & HidNpadButton_Left) || stick.x < -12000;
            in.right = (held & HidNpadButton_Right) || stick.x > 12000;
            in.forward = held & (HidNpadButton_A | HidNpadButton_ZR);
            in.backward = held & (HidNpadButton_B | HidNpadButton_ZL);
            in.jacks = pressed & HidNpadButton_X;
            in.blades = pressed & HidNpadButton_Y;
            in.grip = pressed & HidNpadButton_L;
            in.robot = pressed & HidNpadButton_R;
            in.lights = pressed & HidNpadButton_Up;
            in.frogger = pressed & HidNpadButton_Down;
            in.deflector = pressed & HidNpadButton_Minus;
            in.help = pressed & HidNpadButton_StickL;
            in.confirm = pressed & HidNpadButton_A;
            in.cancel = pressed & HidNpadButton_B;
            in.up = (pressed & HidNpadButton_Up) || (sy == 1 && stickY != 1);
            in.down = (pressed & HidNpadButton_Down) || (sy == -1 && stickY != -1);
            int sx = stick.x > 16000 ? 1 : stick.x < -16000 ? -1 : 0;
            in.leftPressed = (pressed & HidNpadButton_Left) || (sx == -1 && stickX != -1);
            in.rightPressed = (pressed & HidNpadButton_Right) || (sx == 1 && stickX != 1);
            stickX = sx;
            stickY = sy;
            pressed = 0;

            const bool racing = shell.state == Shell::State::Race;
            if (autoDrive && racing) {
                if (holdStart) in = Input{};
                else autoPilot(game, in);
                in.confirm = false;
            }
            if (noDamage) game.gDamage = 0;
            if (forceResult >= 0 && racing && game.frameCount == 60) {
                game.outcome = (Game::Outcome)forceResult;
                if (game.outcome == Game::Outcome::Win) game.gPosition = 1;
                game.screen = Game::Screen::Result;
            }
            if (gadgetDemo && racing) {
                switch (game.frameCount) {
                case 90: in.blades = true; break;
                case 240: in.lights = true; break;
                case 300: in.frogger = true; break;
                case 480: in.jacks = true; break;
                case 540: in.robot = true; break;
                case 600: in.frogger = true; break;
                case 690: in.deflector = true; break;
                case 150: game.enemies[0].explosionMODE = 5; break; // blow up an opponent to check the effect
                default: break;
                }
            }
            static int onResult = 0;
            onResult = shell.state == Shell::State::Result ? onResult + 1 : 0;
            if ((gadgetDemo || menuTour) && onResult == 90) in.confirm = true; // "race again"
            if (menuTour) menuDemo(shell.frames, in);

            shell.frame(in);

            if (game.screen != lastScreen) {
                lastScreen = game.screen;
                if (game.screen == Game::Screen::Result)
                    logf("result screen: outcome %d, place %d, score %d, frame %ld", (int)game.outcome, game.gPosition,
                         game.gScore, game.frameCount);
            }
            if (autoDrive && racing && game.frameCount % (game.debugStartPath ? 6 : 60) == 0) {
                glm::vec3 p = game.car.body.origin();
                logf("frame %ld pos %.0f %.0f %.1f speed %.0f surface %d damage %.0f place %d score %d left %d",
                     game.frameCount, p.x, p.y, p.z, game.car.forwardVel, game.trackFace, game.gDamage, game.gPosition,
                     game.gScore, game.enemiesRemaining);
            }
        }

        if (!ok) {
            glViewport(0, 0, 1280, 720);
            glClearColor(0.6f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
        } else {
            shell.draw(renderer, elapsed);
        }
        eglSwapBuffers(s_display, s_surface);
        if (++drawn == 1) logf("first frame presented");
    }

    logf("exiting after %ld front-end frames", shell.frames);
    audio::shutdown();
    deinitEgl();
    romfsExit();
    if (g_log) fclose(g_log);
    return 0;
}
