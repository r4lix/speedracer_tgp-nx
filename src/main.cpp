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
#include "menu.h"
#include "settings.h"
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
    if (slippery) {
        if (speed > 24) in.forward = false;
        in.backward = speed > 32;
    }
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
    hidInitializeTouchScreen();

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

    loadSettings();
    audio::setMuted(g_cfg.mute);
    audio::setMasterVolume(g_cfg.masterVolume);
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
    menu::init();
    if (FILE* mf = fopen("sdmc:/speedracer_menu", "r")) { // debug: open the settings menu on a tab at start
        int tab = 0;
        if (fscanf(mf, "%d", &tab) != 1) tab = 0;
        fclose(mf);
        menu::showTab(tab);
    }
    if (ok && autoDrive) shell.startRace();
    else if (ok && !menuTour && !noIntro && !g_cfg.skipIntro) shell.startIntro();
    logf(ok ? "assets loaded, entering main loop" : "STARTUP FAILED, showing a red screen");

    const double tickHz = (double)armGetSystemTickFreq();
    u64 lastTick = armGetSystemTick();
    double accumulator = 0;
    long drawn = 0;
    u64 pressed = 0; // buttons pressed since the last game frame
    int stickY = 0, stickX = 0; // -1, 0, 1: for menu navigation with the stick
    Game::Screen lastScreen = Game::Screen::Race;
    bool tapPending = false; // a touch waiting for the next game step
    float tapStageX = 0, tapStageY = 0;

    while (appletMainLoop() && !shell.quit) {
        padUpdate(&pad);
        u64 held = padGetButtons(&pad);
        u64 down = padGetButtonsDown(&pad);
        u64 now = armGetSystemTick();
        const double elapsed = (double)(now - lastTick) / tickHz;
        lastTick = now;
        {
            HidAnalogStickState ls = padGetStickPos(&pad, 0);
            menu::feed(held, ls.x, ls.y, elapsed);
        }
        // Minus is the menu's hold-to-open button: the game sees it when it is let go early
        down &= ~(u64)HidNpadButton_Minus;
        if (menu::takeMinusTap()) down |= HidNpadButton_Minus;
        float tapPx = 0, tapPy = 0;
        const bool tapped = menu::takeTap(tapPx, tapPy);
        if (menu::isOpen()) down = 0;
        pressed |= down;
        if (tapped) { // the 4:3 page sits in the middle of the 16:9 screen
            tapStageX = (tapPx - 160.0f) / 1.875f;
            tapStageY = tapPy / 1.875f;
            tapPending = true;
            logf("touch at %.0f,%.0f px = stage %.0f,%.0f", tapPx, tapPy, tapStageX, tapStageY);
        }

        accumulator += elapsed;
        if (accumulator > 0.2) accumulator = 0.2;
        if (menu::isOpen()) accumulator = 0; // the game stands still behind the menu
        const double tick = 1.0 / (30.0 * g_cfg.timeScale);

        // The original runs its scripts and one Havok step per frame at 30 fps.
        while (ok && accumulator >= tick) {
            accumulator -= tick;
            HidAnalogStickState stick = padGetStickPos(&pad, 0);
            int sy = stick.y > 16000 ? 1 : stick.y < -16000 ? -1 : 0;
            Input in;
            const int dz = (int)(g_cfg.stickDeadzone * 32767);
            in.left = ((g_cfg.dpadSteer && (held & HidNpadButton_Left)) || (g_cfg.stickSteer && stick.x < -dz));
            in.right = ((g_cfg.dpadSteer && (held & HidNpadButton_Right)) || (g_cfg.stickSteer && stick.x > dz));
            if (g_cfg.invertSteer) std::swap(in.left, in.right);
            in.forward = ((g_cfg.accelA && (held & HidNpadButton_A)) || (g_cfg.accelZR && (held & HidNpadButton_ZR)) ||
                          g_cfg.autoAccelerate);
            in.backward = ((g_cfg.brakeB && (held & HidNpadButton_B)) || (g_cfg.brakeZL && (held & HidNpadButton_ZL)));
            if (in.backward && g_cfg.autoAccelerate) in.forward = false;
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
            in.menu = pressed & HidNpadButton_Plus;
            in.up = (pressed & HidNpadButton_Up) || (sy == 1 && stickY != 1);
            in.down = (pressed & HidNpadButton_Down) || (sy == -1 && stickY != -1);
            int sx = stick.x > 16000 ? 1 : stick.x < -16000 ? -1 : 0;
            in.leftPressed = (pressed & HidNpadButton_Left) || (sx == -1 && stickX != -1);
            in.rightPressed = (pressed & HidNpadButton_Right) || (sx == 1 && stickX != 1);
            stickX = sx;
            stickY = sy;
            in.tap = tapPending;
            in.tapX = tapStageX;
            in.tapY = tapStageY;
            tapPending = false;
            pressed = 0;

            const bool racing = shell.state == Shell::State::Race;
            if ((autoDrive || g_cfg.autopilot) && racing) {
                if (holdStart) {
                    in = Input{};
                    if (fileExists("sdmc:/speedracer_jackloop")) { // start the race with one tap, then jump again whenever the car is down
                        in.forward = game.frameCount > 90 && game.frameCount < 94;
                        in.jacks = game.frameCount > 100;
                    }
                }
                else if (fileExists("sdmc:/speedracer_throttle")) { in = Input{}; in.forward = true; } // straight ahead, flat out
                else if (fileExists("sdmc:/speedracer_straight")) {
                    // steer along the path at 50 u/s up to y 1500 (the top of the long descent), then hands off, throttle down
                    if (game.car.body.origin().y < 1500) {
                        autoPilot(game, in);
                        if (game.car.forwardVel > 50) in.forward = false;
                    } else {
                        in = Input{};
                        in.forward = true;
                    }
                }
                else autoPilot(game, in);
                in.confirm = false;
            }
            if (noDamage || g_cfg.noDamage) game.gDamage = 0;
            if (g_cfg.jackLoop && racing && game.isPlaying) in.jacks = true;
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
            if (racing && (g_cfg.logHover || fileExists("sdmc:/speedracer_hoverlog"))) {
                const Vehicle& cv = game.car;
                glm::mat3 rm = cv.body.rotation();
                glm::vec3 fwd = rm * glm::vec3(0, 1, 0), rgt = rm * glm::vec3(1, 0, 0);
                logf("hov %ld z %.3f vz %.2f d %.3f %.3f %.3f %.3f hit %d%d%d%d x %.1f y %.1f yaw %.1f fv %.1f lat %.1f wz %.2f pc %.2f rx %.1f ry %.1f wx %.2f wy %.2f",
                     game.frameCount, cv.body.origin().z, cv.body.v.z, cv.hoverDist[0], cv.hoverDist[1], cv.hoverDist[2],
                     cv.hoverDist[3], cv.isHovering[0], cv.isHovering[1], cv.isHovering[2], cv.isHovering[3], cv.body.origin().x,
                     cv.body.origin().y, cv.rotationDeg().z, glm::dot(cv.body.v, fwd), glm::dot(cv.body.v, rgt), cv.body.w.z,
                     cv.powerCoeff, cv.rotationDeg().x, cv.rotationDeg().y, cv.body.w.x, cv.body.w.y);
            }
            if (autoDrive && racing && game.frameCount % (game.debugStartPath ? 6 : 60) == 0) {
                glm::vec3 p = game.car.body.origin();
                glm::vec3 rd = game.car.rotationDeg();
                logf("frame %ld pos %.0f %.0f %.1f speed %.0f surface %d damage %.0f place %d score %d left %d pitch %.0f roll %.0f hover %.2f vz %.1f trk %.1f",
                     game.frameCount, p.x, p.y, p.z, game.car.forwardVel, game.trackFace, game.gDamage, game.gPosition,
                     game.gScore, game.enemiesRemaining, rd.x, rd.y, game.car.powerCoeff, game.car.body.v.z, game.car.worldHit.trackSpeed);
            }
        }

        if (!ok) {
            glViewport(0, 0, 1280, 720);
            glClearColor(0.6f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
        } else {
            shell.draw(renderer, elapsed);
            menu::draw(shell, elapsed > 1e-4 ? 1.0 / elapsed : 0.0);
        }
        eglSwapBuffers(s_display, s_surface);
        if (++drawn == 1) logf("first frame presented");
    }

    menu::shutdown();
    logf("exiting after %ld front-end frames", shell.frames);
    audio::shutdown();
    deinitEgl();
    romfsExit();
    if (g_log) fclose(g_log);
    return 0;
}
