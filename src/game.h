// Level 1 game state and rules: a port of the level movie's Lingo (PREP, PLAY, doMach5, doEnemy,
// handlePickups, doCamera and the HUD scripts). Everything here advances once per 30 Hz game frame.
#pragma once
#include <string>
#include <vector>

#include "assets.h"
#include "physics.h"

struct Input {
    // held
    bool left = false, right = false, forward = false, backward = false;
    // pressed this frame
    bool jacks = false, grip = false, blades = false, deflector = false, lights = false, frogger = false,
         robot = false, help = false, confirm = false, cancel = false, up = false, down = false,
         leftPressed = false, rightPressed = false;
};

// What the two car scripts share: the chassis body, four hover points and the wheel/shadow placement.
struct Vehicle {
    RigidBody body;
    std::vector<glm::vec4> spheres; // stand-in for the collider hull
    glm::vec3 wheelPoint[4];
    bool isHovering[4] = {};
    float hoverDist[4] = {};
    float pWidth = 1.55f, pFrontWheelBase = 1.2f, pBackWheelBase = 1.2f, pWheelRadius = 0.35f;
    float pStrength = 3.0f, pDamping = 0.05f, pHoverDist = 1.2f, pMaxWheelTurn = 45, pWheelSpinCoeff = 2;
    float pGrip = 0.5f; // from the score
    float forwardVel = 0, powerCoeff = 0, wheelTurn = 0, wheelSpin = 0;
    bool ready = false;
    glm::mat4 wheelMatrix[4];
    glm::mat4 shadowMatrix{1};
    Contact worldHit; // hardest non-track contact of the last step

    void setup(float mass, const glm::vec3& com, const glm::vec3& inertiaDiag, const std::vector<glm::vec4>& collider);
    glm::vec3 rotationDeg() const; // Director-style x, y, z Euler angles
    // Hover impulses for the four wheels. Returns the surface id under the last wheel that hit, 99 if none did.
    int hover(const Level& level, bool resetFlags);
    void placeWheels(float minDrop);
};

// A Shockwave 3D #particle model resource, reduced to what the scripts use: particles are born in a
// line across the emitter, fly inside a cone, grow or shrink and fade over their life.
struct Particle {
    glm::vec3 pos, vel;
    float age, life; // seconds
};

struct ParticleSet {
    std::vector<Particle> particles;
    std::string texture;                 // romfs:/ui bitmap shared by all particles
    float sizeStart = 1, sizeEnd = 1;    // world units
    float blendStart = 100, blendEnd = 0; // percent
    glm::vec3 gravity{0};
    bool additive = false;
    float spawnDebt = 0;                 // fractional particles owed by a looping emitter
};

struct Enemy : Vehicle {
    int id = 0;                 // 1..3
    bool alive = true, shellVisible = true, isDuggary = false;
    int skin = 1;               // EnemyMap_<skin>
    int pStartingPathStep = 2, pNextPathStep = 3;
    glm::vec2 pNextPnt{0};
    float pMaxSpeed = 55.2f, pAccGain = 0.07f;
    float damage = 0;
    int controlMODE = 1, explosionMODE = 0, pPassMODE = 0, pSEG = 0;
    bool pSmartEnemy = true, pUsePassMODE = false, isBehind = false, isPassed = false, pIsJumping = false,
         pJustJumped = false, isStuck = false;
    long pSmartTIMER = 0, isStuckTIMER = 0, duggerySmartTimer = 0, pExplosionTIMER = -100000;
    float turnFactor = 0, distFactor = 0;
    int thisYPos = 0, lastYPos = 0;
    float distToPlayer = 1000;
    bool robotVisible = false;
    float pRobotRotation = 0, pRobotRotationY = 0;
    // wreck thrown out by an explosion
    bool deadVisible = false;
    RigidBody deadBody, deadWheel;
    int deadSkin = 1;
    bool deadIsDuggary = false;
    int explosionFrame = 0;     // index into the kaboom frames, -1 = none
    float explosionSize = 12, explosionBlend = 80, explosionMaxSpeed = 6;
    bool smoking = false;
    int smokeFrame = 28;
    ParticleSet smoke, explosion;
};

struct Pickup {
    int object = -1; // index into level.objects
    enum Kind { Wrench, CandyPurple, CandyRed, Oxygen, Robot } kind = CandyRed;
    bool visible = true;
    float spin = 0;
};

struct DeadTree {
    int object;
    float rise = 0, spin = 0;
};

struct Overlay { // one HUD element in 512x384 stage coordinates
    std::string texture;
    float x = 0, y = 0, blend = 0;
    float u0 = 0, u1 = 1; // horizontal crop, as fractions of the texture
    float width = -1;     // drawn width in stage pixels, -1 = texture width
    int anchor = 0;       // on a screen wider than the stage: -1 keeps the distance to the left edge, 1 to the right
};

class Game {
public:
    bool load();                 // assets
    void start();                // (re)start level 1
    void frame(const Input& in); // one game frame

    int debugStartPath = 0;      // debug aid: start the player at this path point instead of the grid
    enum class Screen { Race, Result } screen = Screen::Race;
    enum class Outcome { None, Win, Lose, Die, Cliff } outcome = Outcome::None;

    // --- assets
    Level level;
    Model mach5Model, enemyModel;
    std::vector<glm::vec4> collider; // spheres filling the cars' collider mesh (romfs:/collider.bin)

    // --- race state (names follow the scripts' globals)
    long frameCount = 0;
    long ticks() const { return frameCount * 2; }         // Director ticks are 1/60 s
    long milliSeconds() const { return frameCount * 100 / 3; }
    bool isPlaying = false, isStarted = false, isGameOver = false, hasFinished = false, gGoPlayed = false;
    bool allReady = false;
    float gDamage = 0, gDamageDisplayed = 0, gOxygen = 100, gOxygenDisplayed = 1;
    int gOxygenMODE = 0, gScore = 0, gPosition = 20, enemiesRemaining = 19, enemiesFinished = 0;
    int enemyPositionsList[3] = {};
    bool duggaryCreated = false;
    int gSEG = 1;
    bool isUnderWater = false, wasUnderWater = false;
    float gM5Speed = 0;
    const float gM5MaxSpeed = 69;
    float cDirection = 0;
    float blackBlend = 0;
    std::vector<glm::vec3> pathList; // 1-based: [0] unused; x, y as integers, z rounded up
    std::vector<int> skinRing{1, 2, 3, 4};

    // --- player (doMach5)
    Vehicle car;
    float pMaxSpeed = 69, pTurnBuildFactor = 0.3f, pAccGain = 0.07f, pDrag = 0.1f, pTurnBuild = 0;
    bool pIsLanding = false, kBackwardIsDown = false;
    int trackFace = 0, effectID = 0;
    float tMaxY = 0;
    glm::vec3 stepPos{0};
    long pCollisionTimeout = -1000;
    bool pHasBlades = false, pHasGrip = false, pHasLights = false, pHasScope = false, pHasJacks = false, pHasShield = false;
    long pBladesTIMER = 0, pGripTIMER = 0, pDeflectorTIMER = 0, jacksTIMER = 0;
    float pSheildRotate = 80, pBladeFactor = 0.1f, lightsBlend = 0;
    bool shieldVisible = false, sawsVisible = false, lightsVisible = false, shadowVisible = false;
    bool correctorOn = false;
    float correctorStrength = 0;
    glm::quat correctorTarget{1, 0, 0, 0};
    // pDirtKick: spray behind the car (dirt, snow, bubbles)
    ParticleSet dirt;
    bool dirtVisible = false;
    int dirtCount = 20;
    float dirtMinSpeed = 0, dirtMaxSpeed = 0, dirtLife = 0.4f, pEffectFactor = 0;
    // homing robot
    int gROBOTMODE = 0, pRobotTargetID = 0;
    bool robotVisible = false;
    glm::vec3 robotPos{0}, robotStep{0};
    glm::quat robotRot{1, 0, 0, 0};
    long enemyContolTIMER = 0;
    // periscope ("frogger")
    int SCOPEMODE = 0;
    float scopeFactor = 0.01f, scopeRotate = 0, scopeStaticBlend = 100, scopeUnderBlend = 0;
    bool scopeViewOn = false;

    Enemy enemies[3];
    std::vector<Pickup> pickups;
    std::vector<uint8_t> treeAlive;     // per level object
    std::vector<DeadTree> deadTrees;
    std::vector<int> waterObjects;

    // --- camera (doCamera)
    bool cameraStarted = false;
    glm::vec3 cameraDummyPos{0};
    float cameraDummyRotZ = 0;
    glm::vec3 eye{0}, lookAt{0, 1, 0};
    glm::mat4 viewProj{1}; // set by the renderer each frame, used for "is it on screen" tests

    // --- HUD (doInterfaceTxt, doInterfacePic, doDisplayDamage, doDisplayOxygen, doUpdatePlace)
    int InterfaceTxtMODE = 0, InterfacePicMODE = 0;
    std::string InterfaceTxtTEXT = "Blades", InterfacepicIMG = "Candy";
    long InterfaceTxtTIMER = 0, InterfacePicTIMER = 0;
    float bracketX = 8, picX = 284;
    float txtBlend = 0, bracketLBlend = 0, bracketRBlend = 0, picBlend = 0;
    float oxygenPanelBlend = 0, placeBlend = 100, helpBlend = 100;
    int gHelpToggle = 1;
    std::vector<Overlay> overlays() const; // what to draw this frame, back to front

    glm::vec3 objectPosition(int object) const; // current world position of a level object (water sways)
    glm::mat4 objectMatrix(int object) const;

private:
    void keyDown(const Input& in);
    void physicsStep();
    void updateMach5(const Input& in);
    void updateEnemy(Enemy& e);
    void enemyJump(Enemy& e);
    void resetDuggary(Enemy& e);
    void explodeEnemy(Enemy& e);
    void enemyHit(Enemy& e, int hitKind, float hitforce, const glm::vec3& normal, const Enemy* other);
    void playFrameScript(const Input& in);
    void doCamera();
    void doFinish();
    void doUpdatePlace();
    void showTxt(const char* text, const char* pic);
    // Looping emitter: keeps about `count` particles alive. One-shot bursts pass burst = true.
    void emit(ParticleSet& set, const glm::mat4& emitter, float halfWidth, float coneDegrees, float minSpeed, float maxSpeed,
              float life, int count, bool burst);
    void stepParticles(ParticleSet& set);
    float frand(); // 0..1
    void nextSkin(Enemy& e);
    bool onScreen(const glm::vec3& p) const;
    bool trackUnder(const glm::vec3& from, float maxDist, glm::vec3& hit) const;
    int rnd(int n); // Lingo random(n): 1..n
    unsigned rng_ = 12345;
};
