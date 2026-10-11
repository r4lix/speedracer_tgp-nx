#include "shell.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include <glad/glad.h>

#include "audio.h"

void logf(const char* fmt, ...); // main.cpp

namespace {

// Hotspot actions
enum {
    ActPlay = 1, ActTabRace, ActTabClips, ActTabWall, ActTabMini, ActAbout, ActHow, ActQuit,
    ActClip0 = 20,       // ..27
    ActWall0 = 40,       // 40, 41, 42 (bonus)
    ActJigsaw = 50, ActSlider,
    ActGadget0 = 60,     // 'a'..'g'
    ActInAction = 70,
};

struct GadgetInfo {
    char key;
    const char* name;
    const char* button;
};
const GadgetInfo kGadgets[] = {{'a', "AUTO JACKS", "X"}, {'b', "GRIP TIRES", "L"}, {'c', "CUTTER BLADES", "Y"},
                               {'d', "DEFLECTOR", "MINUS"}, {'e', "ILLUMINATION", "D-PAD UP"},
                               {'f', "FROGGER MODE", "D-PAD DOWN"}, {'g', "HOMING ROBOT", "R"}};

// Episode clips in the order of the page's buttons (MACH 5, GO!, POPS, DECISION, SECRET, GARAGE, GET SET, DUGGERY);
// the file order is the one the Flash movie lists them in.
const char* kClips[] = {"gp1a", "gp4", "gp1", "gp8", "gp11", "gp19", "gp13", "gp7"};
const char* kClipNames[] = {"MACH 5", "GO!", "POPS", "DECISION", "SECRET", "GARAGE", "GET SET", "DUGGERY"};
const char* kWallpapers[] = {"wall_m5", "wall_blue", "wall_duggery"};
const char* kJigsawNames[] = {"MACH 5", "DUGGERY", "SPRITLE & CHIM CHIM"};
const char* kSliderNames[] = {"SPEED JUMPS", "CHIM CHIM & SPRITLE", "TRIXIE"};
// What solving a puzzle "reveals": the closest clip to its picture (the original played a video inside the puzzle).
const char* kJigsawClips[] = {"gp1a", "gp7", "gp19"};
const char* kSliderClips[] = {"gp1", "gp13", "gp11"};

const glm::vec4 kWhite(1, 1, 1, 1), kYellow(1, 0.85f, 0.15f, 1), kRed(0.62f, 0.06f, 0.07f, 1), kDarkRed(0.42f, 0.03f, 0.05f, 1),
    kBlue(0.12f, 0.27f, 0.55f, 1);

const std::vector<int> kTabs{ActTabRace, ActTabClips, ActTabWall, ActTabMini};

std::string ordinal(int n) {
    const char* suffix = n % 100 >= 11 && n % 100 <= 13 ? "TH" : n % 10 == 1 ? "ST" : n % 10 == 2 ? "ND" : n % 10 == 3 ? "RD" : "TH";
    return std::to_string(n) + suffix;
}

} // namespace

// ---------------------------------------------------------------- setup and save

bool Shell::init() {
    if (!game.load()) return false;
    loadSave();
    state = State::Result; // so that enter() starts the menu music
    enter(State::Main);
    return true;
}

void Shell::loadSave() {
    if (FILE* f = fopen("sdmc:/speedracer.sav", "r")) {
        int bonus = 0;
        if (fscanf(f, "best=%d bonus=%d solved=%d", &bestScore_, &bonus, &solved_) >= 2) bonusWallpaper_ = bonus != 0;
        fclose(f);
    }
}

void Shell::writeSave() {
    if (FILE* f = fopen("sdmc:/speedracer.sav", "w")) {
        fprintf(f, "best=%d bonus=%d solved=%d\n", bestScore_, bonusWallpaper_ ? 1 : 0, solved_);
        fclose(f);
    }
}

void Shell::enter(State s) {
    State from = state;
    state = s;
    sel_ = 0;
    logf("shell: state %d -> %d", (int)from, (int)s);
    const bool fromGame = from == State::Race || from == State::Result;
    if (s != State::Race && s != State::Result && s != State::Playing && fromGame) {
        for (int c = 1; c <= 7; ++c) audio::stop(c);
        audio::volume(1, 160);
        audio::play(1, "music_intro", 0);
        audio::volume(6, 255);
        audio::play(6, "announcer_StandBy");
    }
    if (s == State::About) gadget_ = 0;
}

void Shell::startIntro() {
    playVideo("intro", true); // returns to the main page when it ends or on A / B
}

// PLAY shows the original's loading card ("THE GREAT PLAN" over a checkerboard, a "preparing level..." bar) for
// a moment while the race is set up, then fades it out over the race (the level's TITLE overlay, blend -5 a frame).
void Shell::startRace() {
    stopVideo();
    resultIntro_ = false;
    loadStart_ = frames;
    enter(State::Loading);
}

// ---------------------------------------------------------------- video

void Shell::playVideo(const std::string& file, bool fullScreen, bool loop) {
    // "file:" keeps ffmpeg from reading "romfs" as a protocol name.
    std::string path = "file:romfs:/media/" + file + ".flv";
    if (!video_.open(path.c_str())) return;
    videoFile_ = file;
    videoFull_ = fullScreen;
    videoLoop_ = loop;
    audio::fadeTo(1, 0, 300);
    if (fullScreen) {
        returnTo_ = state;
        int keep = sel_;
        enter(State::Playing);
        sel_ = keep;
    }
}

void Shell::stopVideo() {
    if (!video_.isOpen()) return;
    video_.close();
    audio::fadeTo(1, 160, 500);
}

// ---------------------------------------------------------------- pages and their hotspots (512x384 stage coordinates)

const std::vector<Shell::Hotspot>& Shell::hotspots() const {
    static const std::vector<Hotspot> tabs{{6, 119, 93, 15, ActTabRace}, {103, 119, 93, 15, ActTabClips},
                                           {200, 119, 93, 15, ActTabWall}, {297, 119, 93, 15, ActTabMini}};
    static std::vector<Hotspot> main, clips, wall, wallBonus, mini, about, how, none;
    static const std::vector<Hotspot> result{{165, 274, 100, 80, ActPlay}, {28, 274, 100, 80, ActTabRace}};
    if (main.empty()) {
        main = {{401, 226, 101, 30, ActPlay}, {103, 119, 93, 15, ActTabClips}, {200, 119, 93, 15, ActTabWall},
                {297, 119, 93, 15, ActTabMini}, {22, 340, 168, 17, ActAbout}, {206, 340, 126, 17, ActHow}, {348, 340, 152, 17, ActQuit}};
        for (int i = 0; i < 8; ++i) clips.push_back({27.0f + 57 * (i % 4), i < 4 ? 230.0f : 300.0f, 52, 56, ActClip0 + i});
        clips.insert(clips.end(), tabs.begin(), tabs.end());
        wall = {{84, 329, 101, 28, ActWall0}, {322, 329, 101, 28, ActWall0 + 1}};
        wall.insert(wall.end(), tabs.begin(), tabs.end());
        wallBonus = wall;
        wallBonus.push_back({176, 364, 160, 15, ActWall0 + 2});
        mini = {{84, 329, 101, 28, ActJigsaw}, {322, 329, 101, 28, ActSlider}};
        mini.insert(mini.end(), tabs.begin(), tabs.end());
        const float bx[] = {114, 162, 162, 114, 66, 66, 113}, by[] = {141, 165, 215, 240, 215, 165, 190};
        for (int i = 0; i < 7; ++i) about.push_back({bx[i] - 17, by[i] - 17, 34, 34, ActGadget0 + i});
        how = {{232, 281, 100, 78, ActInAction}};
    }
    switch (state) {
    case State::Main: return main;
    case State::Clips: return clips;
    case State::Wallpaper: return bonusWallpaper_ ? wallBonus : wall;
    case State::Mini: return mini;
    case State::About: return about;
    case State::HowTo: return how;
    case State::Result: return result;
    default: return none;
    }
}

// Moves the selection to the nearest hotspot in the pressed direction.
void Shell::navigate(const Input& in) {
    const std::vector<Hotspot>& hs = hotspots();
    if (hs.empty()) return;
    sel_ = std::min(sel_, (int)hs.size() - 1);
    float dx = (in.rightPressed ? 1.0f : 0.0f) - (in.leftPressed ? 1.0f : 0.0f);
    float dy = (in.down ? 1.0f : 0.0f) - (in.up ? 1.0f : 0.0f);
    if (dx == 0 && dy == 0) return;
    const Hotspot& c = hs[sel_];
    float cx = c.x + c.w * 0.5f, cy = c.y + c.h * 0.5f;
    int best = -1;
    float bestScore = 1e9f;
    for (int i = 0; i < (int)hs.size(); ++i) {
        if (i == sel_) continue;
        float ox = hs[i].x + hs[i].w * 0.5f - cx, oy = hs[i].y + hs[i].h * 0.5f - cy;
        float along = ox * dx + oy * dy, across = std::fabs(ox * dy) + std::fabs(oy * dx);
        if (along < 4) continue;
        float score = along + across * 2.5f;
        if (score < bestScore) {
            bestScore = score;
            best = i;
        }
    }
    if (best >= 0) {
        sel_ = best;
        audio::play(8, "Snd_rollover");
    }
}

void Shell::activate(int action) {
    audio::play(8, "Snd_Click");
    switch (action) {
    case ActPlay: startRace(); return;
    case ActTabRace: stopVideo(); enter(State::Main); return;
    case ActTabClips: stopVideo(); enter(State::Clips); return;
    case ActTabWall: stopVideo(); enter(State::Wallpaper); return;
    case ActTabMini: stopVideo(); enter(State::Mini); return;
    case ActAbout: enter(State::About); return;
    case ActHow: enter(State::HowTo); return;
    case ActQuit: quit = true; return;
    case ActJigsaw: resetPuzzle(true); enter(State::Jigsaw); return;
    case ActSlider: resetPuzzle(false); enter(State::Slider); return;
    case ActInAction: playVideo("gp2", true); return;
    default: break;
    }
    if (action >= ActClip0 && action < ActClip0 + 8) {
        // First press plays the clip in the page's screen, a second press on the same clip goes full screen.
        const char* file = kClips[action - ActClip0];
        if (video_.isOpen() && videoFile_ == file) playVideo(file, true);
        else playVideo(file, false);
    } else if (action >= ActWall0 && action < ActWall0 + 3) {
        wallpaper_ = action - ActWall0;
        returnTo_ = state;
        int keep = sel_;
        enter(State::ViewWallpaper);
        sel_ = keep;
    } else if (action >= ActGadget0 && action < ActGadget0 + 7) {
        gadget_ = kGadgets[action - ActGadget0].key;
        playVideo(std::string("control_") + gadget_, false, true);
    }
}

// ---------------------------------------------------------------- mini-games

void Shell::resetPuzzle(bool jigsaw) {
    cols_ = jigsaw ? 6 : 4;
    rows_ = jigsaw ? 5 : 4;
    const int n = cols_ * rows_;
    order_.resize(n);
    for (int i = 0; i < n; ++i) order_[i] = i;
    moves_ = 0;
    held_ = -1;
    puzzleSolved_ = false;
    hint_ = false;
    if (jigsaw) {
        // any arrangement can be solved by swapping
        for (int i = n - 1; i > 0; --i) std::swap(order_[i], order_[rand() % (i + 1)]);
        cursor_ = 0;
    } else {
        // slide the gap around so the puzzle stays solvable; the gap is the last piece
        int gap = n - 1;
        for (int k = 0; k < 400; ++k) {
            const int d[4] = {-1, 1, -cols_, cols_};
            int to = gap + d[rand() % 4];
            if (to < 0 || to >= n) continue;
            if ((to % cols_ != gap % cols_) && (to / cols_ != gap / cols_)) continue;
            std::swap(order_[gap], order_[to]);
            gap = to;
        }
        cursor_ = gap;
    }
}

void Shell::puzzleFrame(const Input& in, bool jigsaw) {
    const int n = cols_ * rows_;
    if (in.cancel) {
        stopVideo();
        enter(State::Mini);
        sel_ = jigsaw ? 0 : 1;
        return;
    }
    if (in.grip || in.robot) { // L / R: previous / next picture
        picture_ = (picture_ + (in.robot ? 1 : 2)) % 3;
        resetPuzzle(jigsaw);
        return;
    }
    if (in.jacks) { // X: scramble
        resetPuzzle(jigsaw);
        return;
    }
    if (in.blades) hint_ = !hint_; // Y: show the finished picture
    if (puzzleSolved_) {
        if (in.confirm) playVideo(jigsaw ? kJigsawClips[picture_] : kSliderClips[picture_], true);
        return;
    }
    int dx = (in.rightPressed ? 1 : 0) - (in.leftPressed ? 1 : 0), dy = (in.down ? 1 : 0) - (in.up ? 1 : 0);
    bool changed = false;
    if (jigsaw) {
        int cx = cursor_ % cols_ + dx, cy = cursor_ / cols_ + dy;
        if (cx >= 0 && cx < cols_ && cy >= 0 && cy < rows_ && (dx || dy)) {
            cursor_ = cy * cols_ + cx;
            audio::play(8, "Snd_rollover");
        }
        if (in.confirm) {
            if (held_ < 0) {
                held_ = cursor_;
            } else {
                if (held_ != cursor_) {
                    std::swap(order_[held_], order_[cursor_]);
                    ++moves_;
                    changed = true;
                }
                held_ = -1;
            }
            audio::play(8, "Snd_Click");
        }
    } else if (dx || dy) {
        // the tile next to the gap moves in the pressed direction, into the gap
        int gx = cursor_ % cols_ - dx, gy = cursor_ / cols_ - dy;
        if (gx >= 0 && gx < cols_ && gy >= 0 && gy < rows_) {
            int from = gy * cols_ + gx;
            std::swap(order_[cursor_], order_[from]);
            cursor_ = from;
            ++moves_;
            changed = true;
            audio::play(8, "Snd_Click");
        }
    }
    if (changed) {
        bool ok = true;
        for (int i = 0; i < n; ++i) ok = ok && order_[i] == i;
        if (ok) {
            puzzleSolved_ = true;
            solved_ |= 1 << (picture_ + (jigsaw ? 0 : 3));
            writeSave();
            audio::play(6, "got_points");
            logf("shell: %s puzzle %d solved in %d moves", jigsaw ? "jigsaw" : "slider", picture_ + 1, moves_);
        }
    }
}

void Shell::drawPuzzle(bool jigsaw) {
    const float bx = 14, by = 52, bw = 300, bh = 300;
    const std::string pic = std::string(jigsaw ? "jigsaw_" : "slider_") + std::to_string(picture_ + 1);
    ui::fill(glm::vec4(0.05f, 0.1f, 0.25f, 1));
    ui::rect(ui::left(), 0, ui::right() - ui::left(), 384, kBlue);
    ui::rect(ui::left(), 14, ui::right() - ui::left(), 26, kDarkRed);
    ui::text(jigsaw ? "SPEED RACER JIGSAW" : "SPEED RACER SLIDERS", 498, 18, 18, kWhite, 2);
    ui::text((jigsaw ? kJigsawNames : kSliderNames)[picture_], 16, 21, 12, kYellow);
    ui::rect(bx - 2, by - 2, bw + 4, bh + 4, glm::vec4(0.08f, 0.15f, 0.3f, 1));
    const float cw = bw / cols_, ch = bh / rows_;
    const int n = cols_ * rows_;
    if (hint_ || puzzleSolved_) {
        ui::image(pic, bx, by, bw, bh);
    } else {
        for (int i = 0; i < n; ++i) {
            int piece = order_[i];
            if (!jigsaw && piece == n - 1) continue; // the gap
            float x = bx + (i % cols_) * cw, y = by + (i / cols_) * ch;
            float u0 = (float)(piece % cols_) / cols_, v0 = (float)(piece / cols_) / rows_;
            ui::imageRegion(pic, x + 0.5f, y + 0.5f, cw - 1, ch - 1, u0, v0, u0 + 1.0f / cols_, v0 + 1.0f / rows_);
        }
        if (jigsaw) {
            if (held_ >= 0) ui::frame(bx + (held_ % cols_) * cw, by + (held_ / cols_) * ch, cw, ch, 2, kWhite);
            float pulse = 0.6f + 0.4f * std::sin(frames * 0.3f);
            ui::frame(bx + (cursor_ % cols_) * cw, by + (cursor_ / cols_) * ch, cw, ch, 2, glm::vec4(1, 0.85f, 0.15f, pulse));
        }
    }
    const float tx = 330;
    if (jigsaw) {
        ui::text("SWAP THE PIECES TO", tx, 70, 10, kWhite);
        ui::text("REBUILD THE PICTURE.", tx, 83, 10, kWhite);
        ui::text("A: PICK UP, THEN PUT DOWN", tx, 106, 9, kWhite);
    } else {
        ui::text("SLIDE THE PIECES TO", tx, 70, 10, kWhite);
        ui::text("UNSCRAMBLE THE IMAGE.", tx, 83, 10, kWhite);
        ui::text("D-PAD: SLIDE A PIECE", tx, 106, 9, kWhite);
    }
    ui::text("FINISH THE PUZZLE TO REVEAL", tx, 126, 9, kWhite);
    ui::text("A SPEED RACER VIDEO CLIP.", tx, 138, 9, kWhite);
    ui::text("MOVES:", tx + 80, 168, 16, kWhite, 1);
    ui::text(std::to_string(moves_), tx + 80, 190, 40, kWhite, 1);
    ui::image("menu_button_red", tx + 8, 262, 150, 26);
    ui::text("X: SCRAMBLE", tx + 83, 268, 13, kWhite, 1);
    ui::image("menu_button_red", tx + 8, 294, 150, 26);
    ui::text("Y: HINT", tx + 83, 300, 13, kWhite, 1);
    ui::text("L / R: PICTURE     B: BACK", tx + 83, 334, 9, kWhite, 1);
    if (puzzleSolved_) {
        ui::rect(bx, by + bh - 44, bw, 44, glm::vec4(0, 0, 0, 0.65f));
        ui::text("SOLVED!", bx + bw * 0.5f, by + bh - 42, 20, kYellow, 1);
        ui::text("PRESS A TO WATCH THE CLIP", bx + bw * 0.5f, by + bh - 18, 10, kWhite, 1);
    }
}

// ---------------------------------------------------------------- per-frame logic

void Shell::frame(const Input& in) {
    ++frames;
    spin_ -= 2; // doSpinner: 2 degrees per frame
    if (state != State::Race && in.deflector) audio::setMuted(!audio::muted()); // Minus: mute
    if (in.menu) { // +: one step back, and out of the program from the main page
        if (state == State::Main && !video_.isOpen()) {
            quit = true;
        } else {
            stopVideo();
            resultIntro_ = false;
            fadeBlend_ = 0;
            enter(State::Main);
        }
        return;
    }
    switch (state) {
    case State::Main:
    case State::Clips:
    case State::Wallpaper:
    case State::Mini:
    case State::About:
    case State::HowTo: {
        navigate(in);
        const std::vector<Hotspot>& hs = hotspots();
        if (in.confirm && !hs.empty()) {
            activate(hs[std::min(sel_, (int)hs.size() - 1)].action);
            break;
        }
        if (in.cancel && state != State::Main) {
            stopVideo();
            enter(State::Main);
            break;
        }
        if (video_.isOpen() && video_.finished()) {
            if (videoLoop_) playVideo(videoFile_, false, true);
            else stopVideo();
        }
        break;
    }
    case State::ViewWallpaper:
        if (in.confirm || in.cancel) {
            int keep = sel_;
            enter(returnTo_);
            sel_ = keep;
        }
        break;
    case State::Jigsaw:
        puzzleFrame(in, true);
        break;
    case State::Slider:
        puzzleFrame(in, false);
        break;
    case State::Playing:
        if (in.confirm || in.cancel || video_.finished() || !video_.isOpen()) {
            stopVideo();
            int keep = sel_;
            enter(returnTo_);
            sel_ = keep;
        }
        break;
    case State::Loading: {
        const long t = frames - loadStart_;
        if (t == 3) game.start(); // the card is on screen by now
        if (t >= 75) {
            enter(State::Race);
            fadeBlend_ = 100;
        }
        break;
    }
    case State::Race:
        if (fadeBlend_ > 0) fadeBlend_ = std::max(0.0f, fadeBlend_ - 5);
        game.frame(in);
        if (game.screen == Game::Screen::Result) {
            enter(State::Result);
            resultStart_ = frames;
            newUnlock_ = false;
            bestScore_ = std::max(bestScore_, game.gScore);
            // reportScore: the bonus wallpaper needs first place and 30000 points
            if (game.outcome == Game::Outcome::Win && game.gScore >= 30000 && !bonusWallpaper_) bonusWallpaper_ = newUnlock_ = true;
            writeSave();
            if (game.outcome == Game::Outcome::Win) audio::play(1, "music_gospeed");
            // Each result movie opens on a short cartoon: the family cheering after a win, Pops shouting at
            // Speed otherwise (media/result_*.flv, rendered from the Flash movies by tools/cutscene.py).
            playVideo(game.outcome == Game::Outcome::Win ? "result_win" : game.outcome == Game::Outcome::Lose ? "result_lose" : "result_die", false);
            resultIntro_ = video_.isOpen();
        }
        break;
    case State::Result:
        if (resultIntro_) {
            if (in.confirm || in.cancel || video_.finished() || !video_.isOpen()) {
                stopVideo();
                resultIntro_ = false;
                resultStart_ = frames - 1000; // no still panel after the real thing
                if (game.outcome != Game::Outcome::Win) { // the page's own music loop
                    audio::volume(1, 160);
                    audio::play(1, "music_result", 0);
                }
            }
            break;
        }
        if (in.up || in.down || in.leftPressed || in.rightPressed) {
            sel_ = 1 - sel_;
            audio::play(8, "Snd_rollover");
        }
        if (in.confirm) {
            audio::play(8, "Snd_Click");
            if (sel_ == 0) startRace();
            else enter(State::Main);
        }
        break;
    }
}

// ---------------------------------------------------------------- drawing

void Shell::drawPage(const char* capture) {
    ui::fill(glm::vec4(kDarkRed.r, kDarkRed.g, kDarkRed.b, 1));
    if (ui::hasImage(capture)) {
        // the captures are 514x386 with a one-pixel window border
        ui::imageRegion(capture, 0, 0, 512, 384, 1 / 514.0f, 1 / 386.0f, 513 / 514.0f, 385 / 386.0f);
        ui::rect(484, 0, 28, 13, kRed);   // the window's minimise and close buttons
        ui::rect(398, 14, 114, 22, kRed); // VIEW HIGH SCORES: the score server is long gone
    } else {
        ui::rect(0, 0, 512, 118, kRed);
        ui::text("SPEED RACER", 20, 20, 44, kYellow);
        ui::text("DEMON ON WHEELS", 24, 76, 16, kYellow);
    }
    ui::text(audio::muted() ? "SOUND OFF  ( - )" : "SOUND ON  ( - )", 504, 18, 9, kWhite, 2);
}

void Shell::drawLoadingCard(float alpha, float progress) {
    const float l = ui::left(), r = ui::right();
    if (ui::hasImage("title_card")) {
        // the card is 4:3; on the wider screen it covers the width and loses a strip at the top and bottom
        const float w = r - l, h = w * 0.75f;
        ui::image("title_card", l, (384 - h) * 0.5f, w, h, alpha);
    } else {
        ui::rect(l, 0, r - l, 384, glm::vec4(0.98f, 0.78f, 0.08f, alpha));
        ui::text("THE GREAT PLAN", 256, 168, 36, glm::vec4(1, 1, 1, alpha), 1);
    }
    if (progress < 1 || alpha >= 1) {
        ui::rect(l, 379, r - l, 5, glm::vec4(0, 0, 0, alpha));
        ui::rect(l, 379, (r - l) * progress, 5, glm::vec4(1, 1, 1, alpha));
        ui::text("preparing level...", l + 3, 366, 9, glm::vec4(1, 1, 1, alpha));
    }
}

void Shell::drawHotspots(const char* const* fallbackLabels) {
    const std::vector<Hotspot>& hs = hotspots();
    for (int i = 0; i < (int)hs.size(); ++i) {
        if (fallbackLabels && fallbackLabels[i]) ui::text(fallbackLabels[i], hs[i].x + hs[i].w * 0.5f, hs[i].y + 1, 11, kWhite, 1);
        if (i != sel_) continue;
        float pulse = 0.55f + 0.45f * std::sin(frames * 0.3f);
        ui::frame(hs[i].x - 2, hs[i].y - 2, hs[i].w + 4, hs[i].h + 4, 2, glm::vec4(1, 0.9f, 0.2f, pulse));
    }
}

void Shell::draw(Renderer& renderer, double seconds) {
    if (video_.isOpen()) video_.update(seconds);
    if (state == State::Race) {
        renderer.draw(game);
        if (fadeBlend_ > 0) {
            glViewport(0, 0, 1280, 720);
            ui::begin();
            drawLoadingCard(fadeBlend_ / 100.0f, 1);
        }
        return;
    }
    glViewport(0, 0, 1280, 720);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    ui::begin();
    static const char* tabLabels[] = {"RACE!", "VIDEO CLIPS", "WALLPAPER", "MINIGAMES"};
    switch (state) {
    case State::Loading:
        drawLoadingCard(1, std::min(1.0f, (frames - loadStart_) / 72.0f));
        break;
    case State::Main: {
        const bool cap = ui::hasImage("ref_main");
        drawPage("ref_main");
        ui::rect(346, 338, 160, 21, kRed); // PRINT CONTROLS has no use here: the slot becomes QUIT
        ui::text(">>  QUIT", 356, 342, 12, kWhite);
        static const char* labels[] = {"PLAY!", "VIDEO CLIPS", "WALLPAPER", "MINIGAMES", ">> ABOUT THE MACH 5", ">> HOW TO PLAY", nullptr};
        drawHotspots(cap ? nullptr : labels);
        if (bestScore_ > 0) {
            char best[40];
            snprintf(best, sizeof best, "BEST SCORE %06d", bestScore_);
            ui::text(best, 504, 204, 9, kDarkRed, 2);
        }
        break;
    }
    case State::Clips: {
        const bool cap = ui::hasImage("ref_clips");
        drawPage("ref_clips");
        if (video_.isOpen() && video_.texture()) {
            ui::rect(272, 193, 225, 163, glm::vec4(0, 0, 0, 1));
            ui::texture(video_.texture(), 274, 195, 221, 159);
        }
        static const char* labels[12] = {};
        if (!cap)
            for (int i = 0; i < 12; ++i) labels[i] = i < 8 ? kClipNames[i] : tabLabels[i - 8];
        drawHotspots(cap ? nullptr : labels);
        ui::text("A: PLAY     A AGAIN: FULL SCREEN     B: MAIN MENU", 256, 370, 8, kWhite, 1);
        break;
    }
    case State::Wallpaper: {
        const bool cap = ui::hasImage("ref_wall");
        drawPage("ref_wall");
        // on a console there is no desktop to set: the buttons show the picture full screen
        ui::image("menu_button_red", 84, 329, 101, 28);
        ui::text("VIEW", 134, 335, 14, kWhite, 1);
        ui::image("menu_button_red", 322, 329, 101, 28);
        ui::text("VIEW", 372, 335, 14, kWhite, 1);
        if (!cap) {
            ui::image("wall_m5", 27, 170, 218, 150);
            ui::image("wall_blue", 264, 170, 218, 150);
        }
        if (bonusWallpaper_) ui::text("BONUS: DUGGERY", 256, 366, 11, kYellow, 1);
        else ui::text("WIN WITH 30000 POINTS FOR A BONUS WALLPAPER", 256, 368, 8, kWhite, 1);
        static const char* labels[] = {nullptr, nullptr, "RACE!", "VIDEO CLIPS", "WALLPAPER", "MINIGAMES", nullptr};
        drawHotspots(cap ? nullptr : labels);
        break;
    }
    case State::Mini: {
        const bool cap = ui::hasImage("ref_mini");
        drawPage("ref_mini");
        static const char* labels[] = {"JIGSAW: PLAY!", "SLIDERS: PLAY!", "RACE!", "VIDEO CLIPS", "WALLPAPER", "MINIGAMES"};
        drawHotspots(cap ? nullptr : labels);
        break;
    }
    case State::About: {
        std::string capture = gadget_ ? std::string("ref_about_") + gadget_ : "ref_about";
        const bool cap = ui::hasImage(capture);
        drawPage(capture.c_str());
        ui::rect(398, 36, 114, 22, kRed); // MAIN MENU label: B does that here
        // the captured car is a still: a live turntable goes over it
        ui::rect(246, 64, 262, 148, kRed);
        ui::carSpinner(game.mach5Model, 246, 64, 262, 148, spin_, gadget_);
        if (video_.isOpen() && video_.texture()) ui::texture(video_.texture(), 386, 281, 100, 80);
        if (gadget_) {
            // the original text names the keyboard letter; say which pad button it is
            for (const GadgetInfo& g : kGadgets)
                if (g.key == gadget_) ui::text(std::string("ON THE PAD: ") + g.button, 30, 366, 10, kYellow);
        }
        static const char* labels[] = {"A", "B", "C", "D", "E", "F", "G"};
        drawHotspots(cap ? nullptr : labels);
        ui::text("B: MAIN MENU", 504, 44, 9, kWhite, 2);
        break;
    }
    case State::HowTo: {
        drawPage("ref_how");
        ui::rect(398, 36, 114, 22, kRed);
        ui::text("B: MAIN MENU", 504, 44, 9, kWhite, 2);
        // the captured list shows keyboard keys: replace it with the pad
        ui::rect(18, 110, 180, 264, kRed);
        ui::text("CONTROLS", 26, 112, 14, kWhite);
        static const char* rows[][2] = {{"STICK / D-PAD", "STEER"}, {"A / ZR", "ACCELERATE"}, {"B / ZL", "BRAKE, REVERSE"},
                                        {"X", "AUTO JACKS"}, {"L", "GRIP TIRES"}, {"Y", "CUTTER BLADES"}, {"MINUS", "DEFLECTOR"},
                                        {"D-PAD UP", "ILLUMINATION"}, {"D-PAD DOWN", "FROGGER MODE"}, {"R", "HOMING ROBOT"},
                                        {"PLUS", "QUIT"}};
        for (int i = 0; i < 11; ++i) {
            ui::text(rows[i][0], 26, 138 + 21 * i, 9, kYellow);
            ui::text(rows[i][1], 98, 138 + 21 * i, 9, kWhite);
        }
        ui::rect(360, 279, 104, 82, kRed); // PRINT CONTROLS
        static const char* labels[] = {"THE MACH 5 IN ACTION"};
        drawHotspots(ui::hasImage("ref_how") ? nullptr : labels);
        break;
    }
    case State::ViewWallpaper: {
        // 1280x1024 pictures: fit the height
        float w = 384.0f * 1280 / 1024;
        ui::image(kWallpapers[wallpaper_], 256 - w * 0.5f, 0, w, 384);
        break;
    }
    case State::Jigsaw:
        drawPuzzle(true);
        break;
    case State::Slider:
        drawPuzzle(false);
        break;
    case State::Playing:
        if (video_.texture() && video_.height() > 0) {
            float w = 384.0f * video_.width() / video_.height();
            ui::texture(video_.texture(), 256 - w * 0.5f, 0, w, 384);
        }
        break;
    case State::Result: {
        const bool win = game.outcome == Game::Outcome::Win;
        const char* capture = win ? "ref_result_win" : "ref_result_lose";
        if (resultIntro_) {
            ui::fill(glm::vec4(0, 0, 0, 1));
            if (video_.texture() && video_.height() > 0) {
                float w = 384.0f * video_.width() / video_.height();
                ui::texture(video_.texture(), 256 - w * 0.5f, 0, w, 384);
            }
            break;
        }
        if (ui::hasImage(capture)) {
            // The original opens the lose and wrecked screens on a cartoon panel of Pops shouting at Speed.
            if (!win && frames - resultStart_ < 70 && ui::hasImage("ref_result_anim")) {
                ui::imageRegion("ref_result_anim", 0, 0, 512, 384, 1 / 514.0f, 1 / 386.0f, 513 / 514.0f, 385 / 386.0f);
                ui::rect(484, 0, 28, 13, glm::vec4(0.35f, 0.6f, 0.85f, 1));
                break;
            }
            drawPage(capture);
            // the score, over the captured "0"
            const glm::vec4 panel(0.43f, 0.09f, 0.1f, 1);
            ui::rect(88, 158, 120, 26, panel);
            char score[40];
            snprintf(score, sizeof score, "%d", game.gScore);
            ui::text(score, 148, 158, 24, kWhite, 1);
            // score submission is gone: that block tells how the race ended instead
            ui::rect(26, 188, 244, 70, glm::vec4(panel.r, panel.g, panel.b, 0.96f));
            std::string line;
            switch (game.outcome) {
            case Game::Outcome::Win: line = "FIRST PLACE!"; break;
            case Game::Outcome::Lose: line = "YOU FINISHED " + ordinal(game.gPosition); break;
            case Game::Outcome::Cliff: line = "THE MACH 5 WENT OFF THE TRACK"; break;
            default: line = "THE MACH 5 WAS WRECKED"; break;
            }
            ui::text(line, 148, 198, 12, kYellow, 1);
            snprintf(score, sizeof score, "BEST SCORE  %d", bestScore_);
            ui::text(score, 148, 222, 12, kWhite, 1);
            if (bonusWallpaper_) {
                ui::rect(284, 148, 222, 44, panel);
                ui::text(newUnlock_ ? "BONUS UNLOCKED!" : "BONUS WALLPAPER", 395, 152, 13, kYellow, 1);
                ui::text("SEE IT ON THE WALLPAPER PAGE", 395, 172, 9, kWhite, 1);
                ui::image("wall_duggery", 288, 196, 210, 157);
            }
            drawHotspots(nullptr);
            break;
        }
        // plain fallback without the captures
        ui::fill(win ? glm::vec4(0.05f, 0.1f, 0.3f, 1) : glm::vec4(0.12f, 0.03f, 0.08f, 1));
        const char* headline = "";
        std::string line;
        switch (game.outcome) {
        case Game::Outcome::Win:
            headline = "YOU WIN!";
            line = "The Mach 5 takes the race.";
            break;
        case Game::Outcome::Lose:
            headline = "RACE OVER";
            line = "You finished " + ordinal(game.gPosition) + ".";
            break;
        case Game::Outcome::Cliff:
            headline = "OFF THE TRACK";
            line = "The Mach 5 went over the edge.";
            break;
        default:
            headline = "WRECKED";
            line = "The Mach 5 took too much damage.";
            break;
        }
        ui::image(win ? "menu_speed" : "menu_duggery", 30, 70, 222, 170);
        ui::text(headline, 270, 80, 30, win ? kYellow : glm::vec4(1, 0.45f, 0.1f, 1));
        ui::text(line, 270, 122, 11, kWhite);
        char score[40];
        snprintf(score, sizeof score, "SCORE  %06d", game.gScore);
        ui::text(score, 270, 150, 18, kWhite);
        if (game.hasFinished) ui::text("PLACE  " + ordinal(game.gPosition), 270, 176, 18, kWhite);
        snprintf(score, sizeof score, "BEST  %06d", bestScore_);
        ui::text(score, 270, 204, 11, kWhite);
        if (newUnlock_) ui::text("BONUS WALLPAPER UNLOCKED!", 256, 250, 14, kYellow, 1);
        static const char* items[] = {"RACE AGAIN", "MAIN MENU"};
        for (int i = 0; i < 2; ++i) {
            if (i == sel_) ui::image("menu_button_red", 166, 277 + 26 * i, 180, 23);
            ui::text(items[i], 256, 280 + 26 * i, 15, i == sel_ ? kWhite : glm::vec4(0.65f, 0.65f, 0.7f, 1), 1);
        }
        break;
    }
    default:
        break;
    }
}
