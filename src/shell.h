// Front end around the race, modelled on the original main movie: the RACE!, VIDEO CLIPS, WALLPAPER and
// MINIGAMES tabs, ABOUT THE MACH 5, HOW TO PLAY, the two mini-games, and the result screen.
// The original did this in Flash; the pages here are drawn over screen captures of it (romfs:/ui/ref_*),
// with a plain fallback when a capture is missing.
#pragma once
#include <string>
#include <vector>

#include "game.h"
#include "render.h"
#include "video.h"

class Shell {
public:
    enum class State { Main, Clips, Wallpaper, Mini, About, HowTo, ViewWallpaper, Jigsaw, Slider, Playing, Loading, Race, Result };

    bool init();                 // loads the game data and opens the main page
    void frame(const Input& in); // one 30 Hz step
    void draw(Renderer& renderer, double seconds);
    void startRace();
    void startIntro();           // the original's opening animation, if media/intro.flv is there

    Game game;
    State state = State::Main;
    bool quit = false;
    long frames = 0;

private:
    struct Hotspot {
        float x, y, w, h;
        int action;
    };
    void enter(State s);
    const std::vector<Hotspot>& hotspots() const;
    void navigate(const Input& in);
    void activate(int action);
    void playVideo(const std::string& file, bool fullScreen, bool loop = false);
    void stopVideo();
    void drawPage(const char* capture);
    void drawHotspots(const char* const* fallbackLabels);
    void drawLoadingCard(float alpha, float progress);
    void loadSave();
    void writeSave();
    // mini-games
    void resetPuzzle(bool jigsaw);
    void puzzleFrame(const Input& in, bool jigsaw);
    void drawPuzzle(bool jigsaw);

    int sel_ = 0;
    State returnTo_ = State::Main; // where a full-screen video or wallpaper goes back to
    Video video_;
    std::string videoFile_;
    bool videoFull_ = false, videoLoop_ = false;
    char gadget_ = 0;       // selected button on the About page, 'a'..'g'
    float spin_ = 0;        // turntable angle
    int wallpaper_ = 0;
    long resultStart_ = 0;  // frame the result screen opened on
    bool resultIntro_ = false; // the result screen's opening cartoon is playing
    long loadStart_ = 0;       // frame the loading card came up on
    float fadeBlend_ = 0;      // the loading card fading out over the first seconds of the race, 0..100
    // saved between runs (sdmc:/speedracer.sav)
    int bestScore_ = 0, solved_ = 0;
    bool bonusWallpaper_ = false, newUnlock_ = false;
    // puzzle state: tile i shows piece order_[i]
    std::vector<int> order_;
    int cols_ = 4, rows_ = 4, picture_ = 0, moves_ = 0, cursor_ = 0, held_ = -1;
    bool puzzleSolved_ = false, hint_ = false;
};
