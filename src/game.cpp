#include "game.h"
#include "settings.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include "audio.h"

void logf(const char* fmt, ...); // main.cpp

namespace {

// Havok behaviour properties from the score, and the gravity the script sets.
const float kTimeStep = 0.0333f;
const int kSubSteps = 5;
const glm::vec3 kGravity(0, 0, -90.81f);
const glm::vec3 kLocalDown(0, 0, -1), kLocalForward(0, 1, 0);

// Sound channels (PREP: SndC_Music = sound(1) ...)
enum { Music = 1, M5Grnd = 2, M5FX = 3, M5Engine = 4, EnemyFX = 5, VO = 6, M5FX2 = 7 };

const char* kHitReactions[] = {"sp_aawww", "spr_aww", "spr_look_out", "spr_their_up_to"};
const char* kCandy[] = {"spr_isnt_that_delicious", "spr_laugh", "spr_save_some", "spr_yummy"};

// The "saw" model's bounding sphere; the scripts use it as the car's pickup and tree sensor.
const glm::vec3 kSawCenter(0.0f, 2.34f, -0.52f);
const float kSawRadius = 1.96f;

glm::mat4 rotateDeg(float x, float y, float z) { // Director applies x, then y, then z
    glm::mat4 m(1);
    m = glm::rotate(m, glm::radians(z), glm::vec3(0, 0, 1));
    m = glm::rotate(m, glm::radians(y), glm::vec3(0, 1, 0));
    m = glm::rotate(m, glm::radians(x), glm::vec3(1, 0, 0));
    return m;
}

float getAngle(float x1, float y1, float x2, float y2) { // MOVIE script: compass angle from +Y, clockwise
    float xoff = x2 - x1, yoff = y2 - y1;
    if (xoff > 0) return 90 - glm::degrees(std::atan(yoff / xoff));
    if (xoff < 0) return -90 - glm::degrees(std::atan(yoff / xoff));
    return yoff >= 0 ? 0.0f : 180.0f;
}

glm::quat yaw(float degrees) { return glm::angleAxis(glm::radians(degrees), glm::vec3(0, 0, 1)); }

std::vector<glm::vec4> fallbackCollider() {
    // Used only if collider.bin is missing: a few spheres around the 1.74 x 3.41 x 0.31 slab.
    std::vector<glm::vec4> s;
    for (float y : {-1.4f, 0.0f, 1.4f})
        for (float x : {-0.57f, 0.57f}) s.push_back(glm::vec4(x, y, 0.2f, 0.3f));
    s.push_back(glm::vec4(0, 1.4f, 0.2f, 0.3f));
    s.push_back(glm::vec4(0, -1.4f, 0.2f, 0.3f));
    return s;
}

float approach(float cur, float target, float step) {
    return cur < target ? std::min(target, cur + step) : std::max(target, cur - step);
}

} // namespace

// ---------------------------------------------------------------- Vehicle

void Vehicle::setup(float mass, const glm::vec3& comIn, const glm::vec3& inertia, const std::vector<glm::vec4>& collider) {
    // The centre of mass goes under the middle of the contact spheres. The original's value (the model's centre,
    // 0.08 behind the middle of the slab) is fine for its hull, but with these spheres a car landing flat took its
    // impulse off the centre of mass and pitched or rolled away, so a jack jump drifted across the road.
    glm::vec3 com = comIn;
    if (!collider.empty()) {
        float cy = 0;
        for (const glm::vec4& sp : collider) cy += sp.y;
        com.y = cy / (float)collider.size();
    }
    body = RigidBody();
    body.mass = mass;
    body.invMass = 1.0f / mass;
    body.comLocal = com;
    body.invInertiaBody = glm::mat3(1.0f / inertia.x, 0, 0, 0, 1.0f / inertia.y, 0, 0, 0, 1.0f / inertia.z);
    glm::vec3 right = glm::normalize(glm::cross(kLocalDown, kLocalForward));
    wheelPoint[0] = com + pFrontWheelBase * kLocalForward + pWidth * 0.5f * -right;
    wheelPoint[1] = com + pFrontWheelBase * kLocalForward + pWidth * 0.5f * right;
    wheelPoint[2] = com + pBackWheelBase * -kLocalForward + pWidth * 0.5f * right;
    wheelPoint[3] = com + pBackWheelBase * -kLocalForward + pWidth * 0.5f * -right;
    spheres = collider;
    for (int i = 0; i < 4; ++i) {
        isHovering[i] = false;
        hoverDist[i] = pHoverDist;
        wheelMatrix[i] = glm::mat4(1);
    }
    forwardVel = powerCoeff = wheelTurn = wheelSpin = 0;
    ready = false;
    worldHit = Contact();
}

glm::vec3 Vehicle::rotationDeg() const {
    glm::mat3 r = body.rotation(); // r[col][row]
    float y = std::asin(glm::clamp(-r[0][2], -1.0f, 1.0f));
    float x = std::atan2(r[1][2], r[2][2]);
    float z = std::atan2(r[0][1], r[0][0]);
    return glm::degrees(glm::vec3(x, y, z));
}

// The script asks modelsUnderRay for #maxDistance 1 yet tests the hit against pHoverDist 1.2, and its car rides on
// the road without ever losing contact on slopes. With a 1.0 ray the port's wheels let go on every drop (a third of
// the samples on level 1's first hills, bounce, and at the valley floor y 985 a frame with no contact is the
// "fell off the track" condition). The ray therefore reaches pHoverDist, which is what the test implies.
int Vehicle::hover(const Level& level, bool keepDistOnMiss) {
    glm::mat3 rot = body.rotation();
    glm::mat4 xf = body.transform();
    glm::vec3 worldDown = glm::normalize(rot * kLocalDown);
    glm::vec3 linVel = body.v, angVel = body.w;
    glm::vec3 chassisPos = body.origin() + body.comLocal;
    int face = 99;
    for (int i = 0; i < 4; ++i) {
        glm::vec3 wp = glm::vec3(xf * glm::vec4(wheelPoint[i], 1));
        RayHit hit;
        isHovering[i] = false;
        if (level.collision.raycast(wp, worldDown, pHoverDist, true, hit)) {
            if (i == 3) face = hit.surface;
            if (hit.dist < pHoverDist) {
                glm::vec3 pointVel = linVel + glm::cross(angVel, wp - chassisPos);
                glm::vec3 diff = hit.point - wp;
                float f = -pStrength * (pHoverDist - hit.dist);
                f -= pDamping * glm::dot(pointVel, diff);
                // The script multiplies the spring by the distance (|diff|), so it fades out as the car comes down:
                // below d = 0.37 the four wheels cannot even carry the car's weight and, after a hard landing (the
                // jacks), it can stay on its hull with the wheels in the road. Below 0.6 the lever stays at 0.6,
                // the strongest the spring gets, so a compressed car always climbs back to its ride height.
                const float kMinLever = 0.6f;
                if (hit.dist < kMinLever && hit.dist > 1e-4f) diff *= kMinLever / hit.dist;
                body.applyImpulseAtPoint(diff * f * body.mass, wheelPoint[i]);
                isHovering[i] = true;
                hoverDist[i] = hit.dist;
                continue;
            }
        }
        if (!keepDistOnMiss) hoverDist[i] = pHoverDist;
    }
    return face;
}

void Vehicle::placeWheels(float minDrop) {
    glm::mat3 rot = body.rotation();
    glm::mat4 xf = body.transform(), rot4 = glm::mat4(rot);
    glm::vec3 worldDown = glm::normalize(rot * kLocalDown);
    glm::vec3 pos[4];
    for (int i = 0; i < 4; ++i) {
        glm::vec3 wp = glm::vec3(xf * glm::vec4(wheelPoint[i], 1));
        pos[i] = wp + std::max(minDrop, hoverDist[i]) * worldDown - pWheelRadius * worldDown;
        glm::mat4 m = rot4 * rotateDeg(wheelSpin, 0, i < 2 ? -wheelTurn * pMaxWheelTurn : 0.0f);
        m[3] = glm::vec4(pos[i], 1);
        wheelMatrix[i] = m;
    }
    shadowMatrix = rot4;
    shadowMatrix[3] = glm::vec4((pos[2] + pos[3]) * 0.5f + glm::vec3(0, 0, -0.2f), 1);
}

// ---------------------------------------------------------------- setup

int Game::rnd(int n) {
    rng_ = rng_ * 1664525u + 1013904223u;
    return n <= 1 ? 1 : (int)((rng_ >> 8) % (unsigned)n) + 1;
}

bool Game::load() {
    if (!loadLevel("romfs:/level1.srl", level)) return false;
    if (!loadModel("romfs:/mach5.srm", mach5Model)) return false;
    if (!loadModel("romfs:/enemy.srm", enemyModel)) return false;
    collider.clear();
    if (FILE* f = fopen("romfs:/collider.bin", "rb")) {
        uint32_t n = 0;
        if (fread(&n, 4, 1, f) == 1 && n > 0 && n < 4096) {
            collider.resize(n);
            if (fread(collider.data(), sizeof(glm::vec4), n, f) != n) collider.clear();
        }
        fclose(f);
    }
    if (collider.empty()) {
        logf("collider.bin missing, using the rough fallback shape");
        collider = fallbackCollider();
    }

    // PREP 50: path_NN models become pathList[NN] = [point(integer x, integer y), 0, ceil(z)]
    pathList.assign(1, glm::vec3(0));
    for (const Marker& m : level.markers) {
        if (m.kind != "path" || m.name.size() < 6) continue;
        int n = atoi(m.name.c_str() + 5);
        if (n < 1) continue;
        if ((int)pathList.size() <= n) pathList.resize(n + 1, glm::vec3(0));
        pathList[n] = glm::vec3(std::round(m.matrix[3].x), std::round(m.matrix[3].y), std::ceil(m.matrix[3].z));
    }
    for (int i = 0; i < (int)level.objects.size(); ++i) {
        const LevelObject& o = level.objects[i];
        if (o.kind == "water") waterObjects.push_back(i);
        if (o.kind != "pickup" || o.name.size() < 8) continue;
        Pickup p;
        p.object = i;
        const char* k = o.name.c_str() + 7;
        if (!strncmp(k, "Wrench", 6)) p.kind = Pickup::Wrench;
        else if (!strncmp(k, "CandyPurple", 11)) p.kind = Pickup::CandyPurple;
        else if (!strncmp(k, "CandyRed", 8)) p.kind = Pickup::CandyRed;
        else if (!strncmp(k, "Oxegen", 6)) p.kind = Pickup::Oxygen;
        else if (!strncmp(k, "Robot", 5)) p.kind = Pickup::Robot;
        pickups.push_back(p);
    }
    logf("game data: %d path points, %d pickups, %d water objects, %d collider spheres", (int)pathList.size() - 1,
         (int)pickups.size(), (int)waterObjects.size(), (int)collider.size());
    return pathList.size() > 5 && level.find("carPos_Mach5");
}

bool Game::trackUnder(const glm::vec3& from, float maxDist, glm::vec3& hit) const {
    RayHit h;
    if (!level.collision.raycast(from, glm::vec3(0, 0, -1), maxDist, true, h)) return false;
    hit = h.point;
    return true;
}

void Game::nextSkin(Enemy& e) {
    // skinID = random(count - 2); the chosen skin moves to the end of the list
    int idx = rnd((int)skinRing.size() - 2) - 1;
    e.skin = skinRing[idx];
    skinRing.erase(skinRing.begin() + idx);
    skinRing.push_back(e.skin);
}

void Game::start() {
    for (int c = 1; c <= 7; ++c) {
        audio::stop(c);
        audio::volume(c, 255);
    }
    screen = Screen::Race;
    outcome = Outcome::None;
    frameCount = 0;
    isPlaying = isStarted = isGameOver = hasFinished = gGoPlayed = allReady = false;
    gDamage = gDamageDisplayed = 0;
    gOxygen = 100;
    gOxygenDisplayed = 1;
    gOxygenMODE = 0;
    gScore = 0;
    enemiesRemaining = 19;
    gPosition = enemiesRemaining + 1;
    enemiesFinished = 0;
    duggaryCreated = false;
    isUnderWater = wasUnderWater = false;
    cDirection = 0;
    blackBlend = 0;
    skinRing = {1, 2, 3, 4};

    level.collision.enableAllOwners();
    treeAlive.assign(level.objects.size(), 1);
    deadTrees.clear();
    for (Pickup& p : pickups) {
        p.visible = true;
        p.spin = 0;
    }

    // Player: prepareCar. Mass properties of the "collider" mesh at mass 30000 (NOTES.md).
    car = Vehicle();
    car.setup(30000.0f, glm::vec3(0.0f, -0.0762f, 0.1558f), glm::vec3(25769.0f, 6992.0f, 32311.0f), collider);
    glm::vec3 spawn(level.find("carPos_Mach5")->matrix[3]), ground;
    // The script asks for a hit within 2 units; the marker sits higher than that, so look further.
    glm::vec3 pos = trackUnder(spawn, 50.0f, ground) ? ground + glm::vec3(0, 0, 1) : spawn;
    glm::quat facing(1, 0, 0, 0);
    if (debugStartPath > 0 && debugStartPath + 1 < (int)pathList.size()) {
        glm::vec3 a = pathList[debugStartPath], b = pathList[debugStartPath + 1];
        spawn = a;
        pos = trackUnder(a + glm::vec3(0, 0, 5), 50.0f, ground) ? ground + glm::vec3(0, 0, 1) : a;
        facing = yaw(-getAngle(a.x, a.y, b.x, b.y));
    }
    car.body.teleport(pos, facing);
    car.placeWheels(0.75f);
    pMaxSpeed = 69;
    pDrag = 0.1f;
    pTurnBuild = 0;
    pIsLanding = false;
    trackFace = effectID = 0;
    tMaxY = 0;
    stepPos = glm::vec3(0);
    pCollisionTimeout = -1000;
    pHasBlades = pHasGrip = pHasLights = pHasScope = pHasJacks = pHasShield = false;
    pSheildRotate = 80;
    pBladeFactor = 0.1f;
    lightsBlend = 0;
    shieldVisible = sawsVisible = lightsVisible = shadowVisible = false;
    correctorOn = false;
    dirt = ParticleSet();
    dirtVisible = false;
    dirtMinSpeed = dirtMaxSpeed = pEffectFactor = 0;
    gROBOTMODE = 0;
    robotVisible = false;
    SCOPEMODE = 0;
    scopeViewOn = false;
    scopeUnderBlend = 0;
    gSEG = (int)std::lround(spawn.y) / 300 + 1;

    // Enemies: prepareCar. Starting path steps 2, 3 and 4 come from the score.
    for (int i = 0; i < 3; ++i) {
        Enemy& e = enemies[i];
        e = Enemy();
        e.id = i + 1;
        // makeMovableRigidBody(name, 20000, 1, #box): a solid box the size of the collider
        e.setup(20000.0f, glm::vec3(0, 0, 0.155f), glm::vec3(19553.0f, 5213.0f, 24443.0f), collider);
        e.pStartingPathStep = 2 + i;
        e.pNextPathStep = e.pStartingPathStep + 1;
        if (e.pNextPathStep >= (int)pathList.size()) e.pNextPathStep = 1;
        glm::vec3 cur = pathList[e.pStartingPathStep], next = pathList[e.pNextPathStep];
        e.pNextPnt = glm::vec2(next);
        e.pSEG = (int)cur.y / 300 + 1;
        glm::vec3 p = trackUnder(cur, 50.0f, ground) ? ground + glm::vec3(0, 0, 1) : cur;
        e.body.teleport(p, yaw(-getAngle(cur.x, cur.y, next.x, next.y)));
        e.placeWheels(0);
        e.pMaxSpeed = gM5MaxSpeed * 0.8f;
        e.isStuckTIMER = e.duggerySmartTimer = 0;
        e.explosionFrame = -1;
        nextSkin(e);
        enemyPositionsList[i] = e.pStartingPathStep;
        --enemiesRemaining;
    }

    cameraStarted = false;
    InterfaceTxtMODE = InterfacePicMODE = 0;
    txtBlend = bracketLBlend = bracketRBlend = picBlend = oxygenPanelBlend = 0;
    placeBlend = 100;
    helpBlend = 100;
    gHelpToggle = 1;
    doCamera();
    logf("level started: car at %.1f %.1f %.1f, %d enemies still to come", pos.x, pos.y, pos.z, enemiesRemaining);
}

// ---------------------------------------------------------------- small helpers

bool Game::onScreen(const glm::vec3& p) const { // worldSpaceToSpriteSpace(p) <> VOID
    glm::vec4 c = viewProj * glm::vec4(p, 1);
    return c.w > 0.01f && std::fabs(c.x) <= c.w && std::fabs(c.y) <= c.w;
}

glm::mat4 Game::objectMatrix(int object) const {
    const LevelObject& o = level.objects[object];
    glm::mat4 m = o.matrix;
    if (o.kind == "water") m[3].x += std::sin(cDirection / 180.0f * 3.14f); // PLAY: water sways along x
    return m;
}

glm::vec3 Game::objectPosition(int object) const { return glm::vec3(objectMatrix(object)[3]); }

void Game::showTxt(const char* text, const char* pic) {
    if (text) {
        InterfaceTxtTEXT = text;
        InterfaceTxtMODE = 1;
    }
    if (pic) {
        InterfacepicIMG = pic;
        InterfacePicMODE = 1;
    }
}

void Game::doUpdatePlace() { placeBlend = 100; }

// ---------------------------------------------------------------- particles

float Game::frand() { return (rnd(10001) - 1) / 10000.0f; }

void Game::emit(ParticleSet& set, const glm::mat4& emitter, float halfWidth, float coneDegrees, float minSpeed, float maxSpeed,
                float life, int count, bool burst) {
    int n = count;
    if (!burst) {
        // keep `count` alive: count / life new particles per second
        set.spawnDebt += count * kTimeStep / std::max(0.05f, life);
        n = (int)set.spawnDebt;
        set.spawnDebt -= n;
        if ((int)set.particles.size() + n > count) n = std::max(0, count - (int)set.particles.size());
    }
    const float cone = glm::radians(coneDegrees);
    for (int i = 0; i < n; ++i) {
        // emitter.direction is +Z in the emitter's frame; region is a line along its X
        float tilt = frand() * cone, turn = frand() * 6.2831853f;
        glm::vec3 dir(std::sin(tilt) * std::cos(turn), std::sin(tilt) * std::sin(turn), std::cos(tilt));
        Particle p;
        p.pos = glm::vec3(emitter * glm::vec4((frand() * 2 - 1) * halfWidth, 0, 0, 1));
        p.vel = glm::mat3(emitter) * dir * (minSpeed + (maxSpeed - minSpeed) * frand());
        p.age = 0;
        p.life = life * (0.7f + 0.3f * frand());
        set.particles.push_back(p);
    }
}

void Game::stepParticles(ParticleSet& set) {
    for (size_t i = 0; i < set.particles.size();) {
        Particle& p = set.particles[i];
        p.age += kTimeStep;
        if (p.age >= p.life) {
            set.particles[i] = set.particles.back();
            set.particles.pop_back();
            continue;
        }
        p.vel += set.gravity;
        p.pos += p.vel * kTimeStep;
        ++i;
    }
}

// ---------------------------------------------------------------- gadgets (doMach5 keyDown)

void Game::keyDown(const Input& in) {
    if (!isPlaying) return;
    if (in.robot) {
        if (gROBOTMODE == 0) {
            int best = -1;
            float bestDist = 1e9f;
            for (int i = 0; i < 3; ++i)
                if (enemies[i].alive && enemies[i].distToPlayer < bestDist) {
                    bestDist = enemies[i].distToPlayer;
                    best = i;
                }
            if (best >= 0 && !onScreen(enemies[best].body.origin())) {
                best = -1;
                for (int i = 0; i < 3; ++i)
                    if (enemies[i].alive && onScreen(enemies[i].body.origin())) {
                        best = i;
                        break;
                    }
            }
            if (best < 0) {
                audio::play(VO, "sp_where_is_car");
            } else {
                pRobotTargetID = best;
                audio::play(M5FX, "robot_launch");
                audio::play(M5FX2, "robot_fly");
                gROBOTMODE = 1;
                robotVisible = true;
                robotRot = car.body.q;
                robotPos = car.body.origin() + car.body.rotation() * glm::vec3(0, 1.2f, 0.6f);
                showTxt("Robot", "Robot");
            }
        } else {
            audio::play(VO, "sp_now_its_gone");
        }
    }
    if (in.deflector) {
        if (!pHasShield) {
            audio::play(M5FX, "m5_parascope");
            pDeflectorTIMER = ticks() + 500;
            pHasShield = true;
            showTxt("Deflector", "Deflector");
        } else {
            pHasShield = false;
        }
    }
    if (in.frogger) {
        if (!pHasScope) {
            audio::play(M5FX, "m5_parascope");
            pHasScope = true;
            pHasShield = true;
            if (SCOPEMODE == 0) {
                if (!isUnderWater) gOxygen = 100;
                SCOPEMODE = 1;
                showTxt("Frogger", "Frogger");
            }
        } else {
            pHasScope = false;
            pHasShield = false;
            if (SCOPEMODE != 0) SCOPEMODE = 4;
        }
    }
    if (in.blades) {
        if (!pHasBlades) {
            pBladesTIMER = ticks() + 240;
            audio::play(M5FX, "m5_blades_on");
            audio::queue(M5FX, "m5_blades_loop");
            pBladeFactor = 0.1f;
            pHasBlades = true;
            showTxt("Blades", "Blades");
        } else {
            audio::play(M5FX, "m5_blades_off");
            pHasBlades = false;
        }
    }
    if (in.grip) {
        if (!pHasGrip) {
            pGripTIMER = ticks() + 500;
            pHasGrip = true;
            audio::play(M5FX, "m5_grip_tires");
            showTxt("Grip", "Grip");
        } else {
            pHasGrip = false;
        }
    }
    if (in.lights) {
        if (!pHasLights) {
            audio::play(M5FX, "m5_headlights");
            pHasLights = true;
            lightsBlend = 30;
            lightsVisible = true;
            showTxt("Lights", "Lights");
        } else {
            pHasLights = false;
        }
    }
    if (in.jacks && (!pHasJacks || g_cfg.unlimitedGadgets) && (car.powerCoeff == 1 || g_cfg.airJacks)) {
        pHasJacks = true;
        audio::play(M5FX, "m5_jump");
        car.body.applyImpulse(glm::vec3(0, 0, car.body.mass * 40 * g_cfg.jackPower));
        jacksTIMER = ticks() + 80;
        showTxt("Jacks", "Jacks");
    }
}

// ---------------------------------------------------------------- physics (the Havok step)

void Game::physicsStep() {
    const float h = kTimeStep / kSubSteps;
    car.worldHit = Contact();
    float carEnemy[3] = {};
    float enemyEnemy[3][3] = {};
    glm::vec3 carEnemyN[3], enemyEnemyN[3][3];
    for (Enemy& e : enemies) e.worldHit = Contact();

    for (int s = 0; s < kSubSteps; ++s) {
        if (correctorOn) {
            // Havok 1.x angular dashpot, one body: J = -200 h (strength * angle * axis + damping * w)
            glm::quat err = car.body.q * glm::conjugate(correctorTarget);
            if (err.w < 0) err = -err;
            float angle = 2.0f * std::acos(glm::clamp(err.w, -1.0f, 1.0f));
            glm::vec3 axis(err.x, err.y, err.z);
            float len = glm::length(axis);
            glm::vec3 spring = angle > 0.001f && len > 1e-6f ? axis / len * angle : glm::vec3(0);
            car.body.applyAngularImpulse(-(200.0f * h) * (correctorStrength * spring + 1000.0f * car.body.w));
        }
        // With the blades out the script switches Havok off between the car and the trees.
        // cheats: the Gravity slider, and flight floats the car
        const float carGravity = g_cfg.fly ? std::min(g_cfg.gravityScale, 0.2f) : g_cfg.gravityScale;
        car.body.substep(h, kGravity * carGravity, level.collision, car.spheres, &car.worldHit, pHasBlades);
        for (Enemy& e : enemies) {
            if (e.alive) e.body.substep(h, kGravity, level.collision, e.spheres, &e.worldHit);
            if (e.deadVisible) {
                e.deadBody.substep(h, kGravity, level.collision, e.spheres);
                static const std::vector<glm::vec4> wheelSphere{glm::vec4(0, 0, 0, 0.32f)};
                e.deadWheel.substep(h, kGravity, level.collision, wheelSphere);
            }
        }
        for (int i = 0; i < 3; ++i) {
            if (!enemies[i].alive) continue;
            glm::vec3 n(0);
            float f = g_cfg.ghostCars ? 0.0f : collideBodies(car.body, car.spheres, enemies[i].body, enemies[i].spheres, 0.2f, &n);
            if (f > carEnemy[i]) {
                carEnemy[i] = f;
                carEnemyN[i] = n;
            }
            for (int j = i + 1; j < 3; ++j) {
                if (!enemies[j].alive) continue;
                f = collideBodies(enemies[i].body, enemies[i].spheres, enemies[j].body, enemies[j].spheres, 0.2f, &n);
                if (f > enemyEnemy[i][j]) {
                    enemyEnemy[i][j] = f;
                    enemyEnemyN[i][j] = n;
                }
            }
        }
    }

    // Player collisionHandler: registerInterest(car, #all, 5, 0.5, ...)
    int hitEnemy = -1;
    for (int i = 0; i < 3; ++i)
        if (carEnemy[i] > 0.5f && (hitEnemy < 0 || carEnemy[i] > carEnemy[hitEnemy])) hitEnemy = i;
    if ((hitEnemy >= 0 || car.worldHit.speed > 0.5f) && ticks() >= pCollisionTimeout + 60) {
        if (hitEnemy >= 0) {
            float hitforce = carEnemy[hitEnemy];
            if (pHasBlades) {
                gDamage += std::min(5.0f, hitforce * 10);
            } else {
                gDamage += std::min(30.0f, hitforce * 10);
                if (!audio::busy(VO)) audio::play(VO, kHitReactions[rnd(4) - 1]);
            }
            audio::volume(M5FX, 255);
            audio::play(M5FX, "crash_sm");
        } else {
            car.body.w.z *= 0.2f;
            if (!kBackwardIsDown) {
                float tHitMag = std::fabs(car.forwardVel / 90.0f);
                gDamage += std::min(10.0f, 30 * tHitMag);
                if (!audio::busy(M5FX)) {
                    audio::volume(M5FX, (int)std::min(255.0f, 255 * tHitMag * 4));
                    audio::play(M5FX, "crash_sm");
                }
            }
        }
        pCollisionTimeout = ticks();
    }

    // Enemy collisionHandler: registerInterest(car, #all, 0, 1, ...)
    for (int i = 0; i < 3; ++i) {
        Enemy& e = enemies[i];
        if (!e.alive) continue;
        if (carEnemy[i] > 1.0f) enemyHit(e, 0, carEnemy[i], -carEnemyN[i], nullptr);
        for (int j = 0; j < 3; ++j) {
            if (j == i || !enemies[j].alive) continue;
            float f = i < j ? enemyEnemy[i][j] : enemyEnemy[j][i];
            if (f > 1.0f) enemyHit(e, 1, f, i < j ? enemyEnemyN[i][j] : -enemyEnemyN[j][i], &enemies[j]);
        }
        if (e.alive && e.worldHit.speed > 1.0f) enemyHit(e, 2, e.worldHit.speed, e.worldHit.normal, nullptr);
    }
}

// hitKind: 0 the player's car, 1 another enemy, 2 a wall or tree
void Game::enemyHit(Enemy& e, int hitKind, float hitforce, const glm::vec3& normal, const Enemy* other) {
    if (hitKind == 0) {
        if (e.isDuggary) {
            if (!audio::busy(VO)) audio::play(VO, ("HitDuggary_" + std::to_string(rnd(3))).c_str());
        } else if (rnd(3) == 1 && !audio::busy(VO)) {
            audio::play(VO, kHitReactions[rnd(4) - 1]);
        }
    }
    if (e.isDuggary) return;
    auto addDamage = [&](float d) {
        e.damage += d;
        if (e.damage >= 100) e.explosionMODE = 5;
        else if (e.damage >= 30 && e.explosionMODE == 0) e.explosionMODE = 1;
    };
    if (e.controlMODE == 0) e.explosionMODE = 5; // out of control after the robot: any knock finishes it
    if (hitKind == 1) {
        if (other && e.body.origin().y < other->body.origin().y) {
            e.pSmartEnemy = false;
            addDamage(hitforce * 0.5f);
        }
    } else if (hitKind == 0) {
        addDamage(pHasBlades ? 15.0f : 2.5f);
    } else if (hitforce > 10 && milliSeconds() > e.pExplosionTIMER + 1000) {
        e.damage = 100;
        e.explosionMODE = 5;
    }
    e.body.applyImpulse(normal * e.body.mass * 2.0f);
}

// ---------------------------------------------------------------- player (doMach5 updateCar)

void Game::updateMach5(const Input& in) {
    Vehicle& c = car;
    RigidBody& rb = c.body;
    pMaxSpeed = 69 * g_cfg.topSpeed; // cheat: the Top speed slider
    glm::vec3 tPosition = rb.origin();
    glm::mat3 rot = rb.rotation();
    float ty = tPosition.y;
    tMaxY = std::max(ty, tMaxY);
    if (ty < tMaxY - 100 && InterfaceTxtMODE == 0) {
        tMaxY = ty;
        showTxt("WrongWay", nullptr);
        audio::play(VO, "sp_get_back_into_race");
    }
    if (ty > stepPos.y + 20) stepPos = tPosition;
    gSEG = (int)std::lround(ty) / 300 + 1;
    if (ty >= 10921 && isPlaying) {
        isPlaying = false;
        doFinish();
    }

    // Homing robot
    if (gROBOTMODE >= 1 && gROBOTMODE <= 3) {
        Enemy& target = enemies[pRobotTargetID];
        glm::vec3 targetPos = target.body.origin();
        if (gROBOTMODE == 1) {
            robotStep = tPosition + rot * glm::vec3(0, 7, 3);
            gROBOTMODE = 2;
        } else {
            bool closing = gROBOTMODE == 3;
            glm::vec3 ideal = targetPos + glm::vec3(0, 0, closing ? 0.2f : 2.0f);
            robotStep = (ideal - robotStep) * (closing ? 0.6f : 0.9f) + robotStep;
            glm::vec3 dir = robotStep - robotPos;
            float len = glm::length(dir);
            if (len > 1e-4f) {
                dir /= len;
                robotPos += dir * (closing ? 5.0f : 3.0f);
                robotRot = glm::quatLookAtRH(glm::vec3(-dir.x, -dir.y, -dir.z), glm::vec3(0, 0, 1)) *
                           glm::angleAxis(glm::radians(90.0f), glm::vec3(1, 0, 0));
            }
            if (!closing && glm::distance(robotPos, targetPos + glm::vec3(0, 0, 2)) < 2) gROBOTMODE = 3;
            if (closing && glm::distance(robotPos, targetPos) < 5) {
                robotVisible = false;
                float tvol = std::max(10.0f, 100 - glm::distance(tPosition, targetPos));
                audio::volume(EnemyFX, (int)(255 * tvol / 100));
                target.controlMODE = 0;
                if (target.isDuggary) audio::play(VO, "duggery_scream");
                else audio::play(EnemyFX, "enemy_scream");
                enemyContolTIMER = ticks();
                gROBOTMODE = 4;
            }
            if (!target.alive) { // the target left the race before the robot arrived
                robotVisible = false;
                gROBOTMODE = 4;
            }
        }
    }

    if (g_cfg.unlimitedGadgets) { // cheat: nothing runs out
        if (pHasBlades) pBladesTIMER = ticks() + 240;
        if (pHasGrip) pGripTIMER = ticks() + 500;
        if (pHasShield) pDeflectorTIMER = ticks() + 500;
        if (gROBOTMODE == 4) gROBOTMODE = 0;
    }

    // Periscope canopy: oxygen runs down once the shield is fully closed
    if (pSheildRotate == 0 && pHasScope) {
        gOxygenMODE = 1;
        gOxygen = std::max(0.0f, gOxygen - 0.2f);
    } else {
        gOxygenMODE = 2;
    }
    if (g_cfg.infiniteOxygen) gOxygen = 100;
    if (pHasShield || pHasScope) {
        shieldVisible = true;
        pSheildRotate = std::max(0.0f, pSheildRotate - 5);
        if (ticks() >= pDeflectorTIMER && !pHasScope) pHasShield = false;
    } else if (shieldVisible) {
        pSheildRotate = std::min(80.0f, pSheildRotate + 5);
        if (pSheildRotate == 80) shieldVisible = false;
    }
    if (pHasBlades) {
        sawsVisible = true;
        pBladeFactor = std::min(1.0f, pBladeFactor + 0.1f);
        if (ticks() >= pBladesTIMER) {
            audio::volume(M5FX, 255);
            audio::play(M5FX, "m5_blades_off");
            pHasBlades = false;
        }
    } else if (sawsVisible) {
        pBladeFactor = std::max(0.1f, pBladeFactor - 0.1f);
        if (pBladeFactor == 0.1f) sawsVisible = false;
    }
    if (!pHasLights && lightsVisible) {
        lightsBlend *= 0.7f;
        if (lightsBlend <= 2) {
            lightsBlend = 0;
            lightsVisible = false;
        }
    }
    if (pHasJacks && ticks() > jacksTIMER && c.powerCoeff > 0) pHasJacks = false;
    if (pHasGrip) {
        c.pGrip = 1;
        if (ticks() >= pGripTIMER) pHasGrip = false;
    }

    glm::vec3 worldAxis = glm::normalize(rot * -kLocalDown);
    glm::vec3 worldForward = glm::normalize(rot * kLocalForward);
    glm::vec3 worldRight = glm::normalize(glm::cross(worldForward, worldAxis));
    const float mass = rb.mass;
    glm::vec3 chassisPos = tPosition + rb.comLocal;
    glm::vec3 linVel = rb.v;
    c.forwardVel = glm::dot(linVel, worldForward);
    const float forwardVel = c.forwardVel;
    float tAngVel = rb.w.z;
    gM5Speed = forwardVel;

    c.powerCoeff = (float)(c.isHovering[0] + c.isHovering[1] + c.isHovering[2] + c.isHovering[3]);
    if (g_cfg.neverFall && chassisPos.z <= -10 && c.powerCoeff == 0 && trackFace == 99 && safeValid) {
        // cheat: back to where the car last had all four wheels on the road, facing the way it was going
        rb.x = safeX;
        rb.q = safeQ;
        rb.v = glm::vec3(0);
        rb.w = glm::vec3(0);
        audio::play(M5FX, "m5_chirp");
        return;
    }
    if (chassisPos.z <= -10 && c.powerCoeff == 0 && trackFace == 99 && !isGameOver) {
        // go("CLIFF"): fell off the track
        isGameOver = true;
        isPlaying = false;
        audio::fadeOut(Music, 1000);
        audio::fadeOut(M5Engine, 1000);
        audio::fadeOut(M5Grnd, 1000);
        audio::play(VO, "music_cliff");
        outcome = Outcome::Cliff;
        screen = Screen::Result;
        logf("frame %ld: fell off the track at y %.0f", frameCount, ty);
        return;
    }
    if (c.powerCoeff != 0) {
        c.ready = true;
        c.powerCoeff /= 4.0f;
    }
    const float powerCoeff = c.powerCoeff;
    if (powerCoeff == 1 && forwardVel > 3 && trackFace != 99) {
        safeX = rb.x + glm::vec3(0, 0, 0.3f);
        safeQ = rb.q;
        safeValid = true;
    }

    auto percentAng = [&](float denom) {
        if (tAngVel == 0) return 1.0f;
        float t = tAngVel / denom;
        if (std::isnan(t)) return 1.0f;
        return std::min(1.0f, 1.0f - std::min(1.0f, t));
    };
    auto coast = [&]() {
        float k = std::fabs(forwardVel) > 1 ? 0.06f : 0.9f;
        rb.applyImpulse(-(forwardVel * mass * worldForward * k * powerCoeff));
    };

    if (isPlaying) {
        if (forwardVel >= 0) {
            if (in.right) {
                pTurnBuild = std::min(0.75f, pTurnBuild + pTurnBuildFactor * 0.03f);
                rb.applyAngularImpulse(glm::vec3(0, 0, -mass * percentAng(-forwardVel * 0.07f) * pTurnBuild));
            } else if (in.left) {
                pTurnBuild = std::min(0.75f, pTurnBuild + pTurnBuildFactor * 0.03f);
                rb.applyAngularImpulse(glm::vec3(0, 0, mass * percentAng(forwardVel * 0.07f) * pTurnBuild));
            } else {
                pTurnBuild = std::max(0.2f, pTurnBuild - 0.2f);
            }
        } else if (powerCoeff > 0 && !in.forward) {
            if (in.right) {
                pTurnBuild = std::min(1.0f, pTurnBuild + 0.2f);
                rb.applyAngularImpulse(glm::vec3(0, 0, mass * percentAng(-forwardVel * 0.07f) * pTurnBuild));
            } else if (in.left) {
                pTurnBuild = std::min(1.0f, pTurnBuild + 0.2f);
                rb.applyAngularImpulse(glm::vec3(0, 0, -mass * percentAng(forwardVel * 0.07f) * pTurnBuild));
            } else {
                pTurnBuild = std::max(0.2f, pTurnBuild - 0.2f);
            }
        }
    } else {
        coast();
    }

    const bool fwd = in.forward && isPlaying, back = in.backward && isPlaying;
    if (powerCoeff > 0 && fwd) {
        if (forwardVel < pMaxSpeed - 5 && !audio::isPlaying(M5Engine, "drive_takeoff")) {
            audio::volume(M5Engine, 255);
            audio::play(M5Engine, "drive_takeoff");
            if (forwardVel >= 20) audio::fadeIn(M5Engine, 2000);
        }
        glm::vec3 imp;
        if (forwardVel < 0) imp = pAccGain * 2 * pMaxSpeed * mass * worldForward;
        else imp = (pMaxSpeed * pDrag - forwardVel) * pAccGain * mass * worldForward;
        rb.applyForce(imp * std::min(1.0f, c.pGrip) * 60.0f);
    } else if (powerCoeff > 0 && back) {
        float diff = -pMaxSpeed * pDrag - forwardVel;
        rb.applyImpulse(diff * (pAccGain * 1.1f) * mass * worldForward);
    } else {
        coast();
    }

    if (isPlaying && g_cfg.turbo && in.boost) rb.applyImpulse(worldForward * mass * 1.5f * g_cfg.turboPower); // cheat: turbo
    if (isPlaying && g_cfg.fly) { // cheat: flight
        if (in.flyUp) {
            rb.applyImpulse(glm::vec3(0, 0, mass * 2.0f));
            if (rb.v.z > 28) rb.v.z = 28;
        }
        if (powerCoeff == 0) { // in the air the drive pushes along the nose and the stick turns
            if (fwd && forwardVel < pMaxSpeed) rb.applyImpulse(worldForward * mass * 0.8f);
            if (in.right) rb.applyAngularImpulse(glm::vec3(0, 0, -mass * 0.6f));
            else if (in.left) rb.applyAngularImpulse(glm::vec3(0, 0, mass * 0.6f));
        }
    }

    glm::vec3 r = c.rotationDeg();
    if (powerCoeff < 1 && (std::fabs(r.x) > 80 || std::fabs(r.y) > 45)) {
        // "m5Corrector" angular dashpot: rotation = [vector(50, 0, rotz), 1], strength = mass * tilt, damping 1000
        correctorOn = true;
        correctorTarget = glm::angleAxis(glm::radians(1.0f), glm::normalize(glm::vec3(50, 0, r.z)));
        correctorStrength = mass * std::max(std::fabs(r.x), std::fabs(r.y)) / 90.0f;
    } else {
        correctorOn = false;
    }
    if (powerCoeff < 1 && (std::fabs(r.x) > 30 || std::fabs(r.y) > 30)) {
        rb.w *= 0.1f;
        rb.applyAngularImpulse(-rb.angularMomentum() * 0.5f);
    }

    float slideSpeed = 0;
    if (powerCoeff == 0) {
        pIsLanding = true;
    } else {
        if (pIsLanding) {
            rb.w = glm::vec3(0);
            if (!audio::isPlaying(M5FX2, "m5_chirp") && isPlaying) {
                audio::volume(M5FX2, 255);
                audio::play(M5FX2, "m5_chirp");
            }
            pIsLanding = false;
        }
        slideSpeed = glm::dot(worldRight, linVel);
        rb.applyImpulse(worldRight * (-slideSpeed * mass * (forwardVel < 5 ? 1.0f : c.pGrip)));
    }
    rb.w.z *= 0.84f;

    if (isPlaying) {
        // pDirtKick: the spray's settings follow the surface; on plain road they die away
        pEffectFactor = powerCoeff > 0 && forwardVel > 0 ? forwardVel / pMaxSpeed : pEffectFactor * 0.85f;
        auto spray = [&](const char* tex, int count, float sizeEnd) {
            dirt.texture = tex;
            dirtCount = count;
            dirtVisible = true;
            dirtLife = 0.4f;
            dirt.gravity = glm::vec3(0, 0, -0.03f);
            dirtMinSpeed = pEffectFactor * 0.5f;
            dirtMaxSpeed = pEffectFactor * 1.0f;
            dirt.sizeStart = pEffectFactor * 0.5f;
            dirt.sizeEnd = pEffectFactor * sizeEnd;
            dirt.blendStart = pEffectFactor * 20;
            dirt.blendEnd = 0;
        };
        auto sprayFade = [&]() {
            dirtMinSpeed *= 0.9f;
            dirtMaxSpeed *= 0.9f;
            dirt.sizeStart *= 0.9f;
            dirt.sizeEnd *= 0.9f;
            dirt.blendStart *= 0.9f;
            dirt.blendEnd *= 0.9f;
        };
        int prev = effectID;
        effectID = trackFace;
        if (wasUnderWater && (effectID != 12 || eye.z > 1)) {
            isUnderWater = false;
            audio::volume(M5Grnd, 255);
            audio::volume(Music, 100);
            audio::volume(M5Engine, 255);
            if (!audio::isPlaying(M5Grnd, "_inwater_splash")) audio::play(M5Grnd, "_inwater_splash");
            wasUnderWater = false;
        }
        auto skid = [&](float full) {
            if (effectID != prev) {
                audio::volume(M5Grnd, 0);
                if (!audio::isPlaying(M5Grnd, "loop_skid_pavement")) audio::play(M5Grnd, "loop_skid_pavement");
            }
            audio::volume(M5Grnd, (int)(std::min(full, std::fabs(slideSpeed)) / full * 255));
        };
        switch (effectID) {
        case 8: case 9: case 14: case 15: // snow and ice
            pDrag = 1;
            if (effectID != prev) c.pGrip = 0.016f;
            skid(40);
            spray("particle_water", 30, 0.8f);
            break;
        case 4: // dirt
            pDrag = 1;
            if (effectID != prev) c.pGrip = 0.8f;
            skid(30);
            spray("particle_dirt", 20, 1.5f);
            break;
        case 12: // water
            c.pGrip = 1;
            pDrag = 0.75f;
            if (effectID != prev && !audio::isPlaying(M5Grnd, "_inwater_splash")) {
                audio::volume(M5Grnd, 255);
                audio::play(M5Grnd, "_inwater_splash");
            }
            if (tPosition.z <= 0) {
                isUnderWater = true;
                if (!pHasScope) gDamage = std::min(100.0f, gDamage + 0.3f);
                // bubbles: slow, long-lived, rising
                dirt.texture = "particle_bubble";
                dirtCount = 8;
                dirtVisible = true;
                dirtLife = 1.8f;
                dirt.gravity = glm::vec3(0, 0, 0.05f);
                dirtMinSpeed = dirtMaxSpeed = 0;
                dirt.sizeStart = 0.05f;
                dirt.sizeEnd = 0.15f;
                dirt.blendStart = dirt.blendEnd = 80;
                if (!wasUnderWater) {
                    audio::queue(M5Grnd, "loop_underwater");
                    wasUnderWater = true;
                    audio::volume(Music, 0);
                    audio::volume(M5Engine, 0);
                }
            }
            break;
        case 1: case 2: case 6: case 7: case 10: case 13: // road
            if (effectID == 7 && prev != 7) {
                audio::fadeTo(Music, 60, 500);
            } else if (effectID != 7 && prev == 7) {
                bool boss = audio::isPlaying(Music, "duggaryBuild") || audio::isPlaying(Music, "duggaryIntro");
                if (!boss) audio::play(Music, chassisPos.y > 4900 && chassisPos.y < 7300 ? "sr_loop_1_2" : "sr_loop_1_1", 0);
                audio::volume(Music, 100);
            }
            skid(8);
            c.pGrip = 1;
            pDrag = 1;
            sprayFade();
            break;
        case 3: // grass
            c.pGrip = 1;
            pDrag = 0.5f;
            sprayFade();
            audio::fadeOut(M5Grnd, 200);
            break;
        default:
            c.pGrip = 1;
            pDrag = 1;
            sprayFade();
            audio::fadeOut(M5Grnd, 200);
            break;
        }
        if (pHasGrip) pDrag = 1;
        if (dirtVisible && dirt.sizeEnd > 0.02f) {
            // pParticlePosDirt: 0.8 behind the rear axle, 0.3 lower, facing up in the car's frame
            glm::mat4 em = rb.transform();
            em[3] = em * glm::vec4(rb.comLocal + glm::vec3(0, -(c.pBackWheelBase + 0.8f), 0), 1);
            em[3].z -= 0.3f;
            emit(dirt, em, 0.8f, 90, dirtMinSpeed, dirtMaxSpeed, dirtLife, dirtCount, false);
        }
    }

    if (in.right) c.wheelTurn += (1 - c.wheelTurn) * 0.2f;
    else if (in.left) c.wheelTurn += (-1 - c.wheelTurn) * 0.2f;
    else c.wheelTurn += (0 - c.wheelTurn) * 0.2f;
    c.wheelSpin -= forwardVel * c.pWheelSpinCoeff;
    if (c.wheelSpin > 360) c.wheelSpin -= 360;
    else if (c.wheelSpin < -360) c.wheelSpin += 360;
    c.placeWheels(0.75f); // as the script: the wheels never retract (0.25 hid them in the body just before every landing)
    shadowVisible = powerCoeff == 1;
}

// ---------------------------------------------------------------- enemies (doEnemy)

void Game::explodeEnemy(Enemy& e) { // explosionMODE 5
    gScore += 1000;
    audio::stop(M5FX2);
    e.robotVisible = false;
    e.deadSkin = e.skin;
    e.deadIsDuggary = e.isDuggary;
    glm::vec3 wreckPos = e.body.origin();
    glm::quat wreckRot = e.body.q;
    glm::vec3 wheelPos = glm::vec3(e.wheelMatrix[rnd(4) - 1][3]) + glm::vec3(0, 0, 1);
    enemyJump(e);
    e.deadVisible = true;
    e.deadBody = RigidBody();
    e.deadBody.mass = 20000;
    e.deadBody.invMass = 1.0f / 20000;
    e.deadBody.comLocal = glm::vec3(0, 0, 0.155f);
    e.deadBody.invInertiaBody = glm::mat3(1.0f / 19553.0f, 0, 0, 0, 1.0f / 5213.0f, 0, 0, 0, 1.0f / 24443.0f);
    e.deadBody.teleport(wreckPos, wreckRot);
    e.deadBody.applyImpulse(glm::vec3(0, 0, e.deadBody.mass * 15));
    e.deadBody.applyAngularImpulse(glm::vec3(e.deadBody.mass * 10, 0, 0));
    e.deadWheel = RigidBody();
    e.deadWheel.mass = 50;
    e.deadWheel.invMass = 1.0f / 50;
    e.deadWheel.invInertiaBody = glm::mat3(1.0f / 2.0f);
    e.deadWheel.teleport(wheelPos, glm::quat(1, 0, 0, 0));
    e.deadWheel.applyImpulse(glm::vec3(-50, -50, 20000) * 0.02f); // the script's impulse, scaled to stay in the scene
    e.explosionFrame = 0;
    e.explosionSize = 12;
    e.explosionBlend = 80;
    e.explosionMaxSpeed = 6;
    // pmrExplosion: one burst (the script asks for 150 particles in a 60 degree cone, 5 seconds)
    e.explosion = ParticleSet();
    e.explosion.additive = true;
    e.explosion.sizeEnd = 2;
    e.explosion.blendEnd = 0;
    {
        glm::mat4 em(1);
        em[3] = glm::vec4(wreckPos + glm::vec3(0, 0, 0.5f), 1);
        emit(e.explosion, em, 0.8f, 60, 0, 6, 5.0f, 48, true);
    }
    e.pExplosionTIMER = milliSeconds();
    e.smoking = false;
    audio::volume(EnemyFX, 255);
    audio::play(EnemyFX, "crash_lg");
    e.explosionMODE = 6;
}

void Game::enemyJump(Enemy& e) { // doJump: the car leaves, a fresh opponent appears further up the road
    if (!e.isBehind) {
        --gPosition;
        doUpdatePlace();
    }
    int nextStep = std::max({enemyPositionsList[0], enemyPositionsList[1], enemyPositionsList[2]}) + 2;
    e.damage = 0;
    e.pSmartEnemy = true;
    e.controlMODE = 1;
    e.isPassed = false;
    e.smoking = false;
    const int pathCount = (int)pathList.size() - 1;
    if (enemiesRemaining <= 0) {
        e.alive = false;
        return;
    }
    if (nextStep >= pathCount) {
        ++enemiesFinished;
        e.alive = false;
    } else {
        glm::vec3 cur = pathList[nextStep];
        e.pSEG = (int)cur.y / 300 + 1;
        e.pNextPathStep = nextStep + 1;
        glm::vec3 np = pathList[e.pNextPathStep];
        e.pNextPnt = glm::vec2(np.x + (1 - rnd(2)), np.y + (1 - rnd(2)));
        glm::vec3 ground, tPos = cur;
        if (trackUnder(cur, 3.0f, ground)) tPos = ground + glm::vec3(0, 0, 1);
        e.body.v = e.body.w = glm::vec3(0);
        e.body.teleport(tPos, yaw(-getAngle(cur.x, cur.y, e.pNextPnt.x, e.pNextPnt.y)));
        if (enemiesRemaining == 1 && !duggaryCreated) {
            duggaryCreated = true;
            e.isDuggary = true;
        } else {
            nextSkin(e);
        }
        e.shellVisible = true;
        e.isBehind = false;
        e.pIsJumping = true;
        e.pJustJumped = true;
        enemyPositionsList[e.id - 1] = e.pNextPathStep;
    }
    --enemiesRemaining;
}

void Game::resetDuggary(Enemy& e) { // doResetDuggary: put the boss back on the road near the player
    const int pathCount = (int)pathList.size() - 1;
    int nextStep = 1;
    int tCheckPos = (int)std::lround(stepPos.y);
    for (int t = 1; t < pathCount; ++t)
        if (pathList[t].y >= tCheckPos || pathList[t].y >= car.body.origin().y) {
            nextStep = std::max(1, t - 1);
            break;
        }
    glm::vec3 cur = pathList[nextStep];
    e.pNextPathStep = nextStep + 1;
    glm::vec3 np = pathList[e.pNextPathStep];
    e.pNextPnt = glm::vec2(np.x + (1 - rnd(2)), np.y + (1 - rnd(2)));
    e.pSEG = (int)e.pNextPnt.y / 300 + 1;
    glm::vec3 ground, tPos = cur;
    if (trackUnder(cur, 3.0f, ground)) tPos = ground + glm::vec3(0, 0, 1);
    e.body.v = e.body.w = glm::vec3(0);
    e.body.teleport(tPos, yaw(-getAngle(cur.x, cur.y, e.pNextPnt.x, e.pNextPnt.y)));
    e.controlMODE = 1;
    e.robotVisible = false;
}

void Game::updateEnemy(Enemy& e) {
    RigidBody& rb = e.body;
    glm::mat3 rot = rb.rotation();
    glm::vec3 worldAxis = glm::normalize(rot * -kLocalDown);
    glm::vec3 worldForward = glm::normalize(rot * kLocalForward);
    glm::vec3 worldRight = glm::normalize(glm::cross(worldForward, worldAxis));
    const float mass = rb.mass;
    glm::vec3 origin = rb.origin();
    glm::vec3 chassisPos = origin + rb.comLocal;
    glm::vec3 linVel = rb.v;
    e.forwardVel = glm::dot(linVel, worldForward);
    float tAngVel = rb.w.z;
    float tAngMom = rb.angularMomentum().z;
    e.distToPlayer = onScreen(origin) ? glm::distance(origin, car.body.origin()) : 1000.0f;

    e.powerCoeff = (float)(e.isHovering[0] + e.isHovering[1] + e.isHovering[2] + e.isHovering[3]);
    if (e.powerCoeff != 0) {
        e.ready = true;
        e.powerCoeff /= 4.0f;
    }
    e.lastYPos = e.thisYPos;
    e.thisYPos = (int)std::lround(chassisPos.y);
    if (e.isDuggary) {
        if (e.thisYPos <= e.lastYPos + 2 || chassisPos.z <= -30) {
            if (ticks() > e.isStuckTIMER + 60 * 5) e.isStuck = true;
        } else {
            e.isStuck = false;
            e.isStuckTIMER = ticks();
        }
    }
    e.pSEG = e.thisYPos / 300 + 1;

    const int pathCount = (int)pathList.size() - 1;
    if (e.controlMODE == 1) {
        float gotoRot = -getAngle(chassisPos.x, chassisPos.y, e.pNextPnt.x, e.pNextPnt.y);
        float gotoDist = glm::distance(glm::vec2(chassisPos), e.pNextPnt);
        if (gotoDist < 10) {
            ++e.pNextPathStep;
            enemyPositionsList[e.id - 1] = e.pNextPathStep;
            if (e.pNextPathStep > pathCount) {
                --e.pNextPathStep;
                ++enemiesFinished;
                e.alive = false;
                return;
            }
            e.pNextPnt = glm::vec2(pathList[e.pNextPathStep]);
        }
        float dif = gotoRot - e.rotationDeg().z;
        if (dif > 180) dif = 180 - dif;
        else if (dif < -180) dif += 360;
        e.turnFactor = std::min(40.0f, std::fabs(dif)) / 40;
        e.distFactor = 1 - std::min(50.0f, gotoDist) / 50;
        if (dif < 0) rb.applyAngularImpulse(glm::vec3(0, 0, -mass * e.turnFactor - tAngMom * 0.25f));
        else if (dif > 0) rb.applyAngularImpulse(glm::vec3(0, 0, mass * e.turnFactor - tAngMom * 0.25f));
    } else {
        // The robot has it: swerve left and right
        bool firstHalf = ticks() % 60 < 30;
        e.pRobotRotationY += firstHalf ? 5 : -5;
        e.turnFactor = firstHalf ? std::min(1.0f, e.turnFactor + 0.2f) : std::max(-1.0f, e.turnFactor - 0.2f);
        rb.applyAngularImpulse(glm::vec3(0, 0, mass * e.turnFactor * (e.isDuggary ? 0.3f : 1.0f) - tAngMom * 0.25f));
        e.pRobotRotation += 20;
        e.robotVisible = true;
        if (e.isDuggary) {
            e.pPassMODE = 2;
            if (ticks() > enemyContolTIMER + (long)(60 * 0.4f)) {
                e.controlMODE = 1;
                e.robotVisible = false;
            }
        }
    }
    if (e.ready && isPlaying) rb.applyImpulse((e.pMaxSpeed * g_cfg.rivalSpeed - e.forwardVel) * e.pAccGain * mass * worldForward * e.powerCoeff);
    if (e.powerCoeff == 0) {
        glm::vec3 r = e.rotationDeg();
        if (r.x < -10) rb.applyAngularImpulse(worldRight * mass);
        else if (r.x > 10) rb.applyAngularImpulse(-worldRight * mass);
        if (r.y < -10) rb.applyAngularImpulse(worldForward * mass);
        else if (r.y > 10) rb.applyAngularImpulse(-worldForward * mass);
        rb.applyAngularImpulse(-rb.angularMomentum() * 0.3f);
    } else {
        float slideSpeed = glm::dot(worldRight, linVel);
        rb.applyImpulse(worldRight * (-slideSpeed * mass * e.pGrip * e.powerCoeff));
    }

    e.hover(level, true);

    float tWheelTurn = tAngVel >= 0 ? -std::min(0.3f, tAngVel) : -std::max(-0.3f, tAngVel);
    e.wheelTurn = approach(e.wheelTurn, tWheelTurn, 2);
    e.wheelSpin -= e.forwardVel * e.pWheelSpinCoeff;
    if (e.wheelSpin > 360) e.wheelSpin -= 360;
    else if (e.wheelSpin < -360) e.wheelSpin += 360;
    e.placeWheels(0);

    switch (e.explosionMODE) {
    case 1:
        audio::volume(EnemyFX, 200);
        audio::play(EnemyFX, "enemy_scream");
        e.smoking = true;
        e.smokeFrame = 28;
        e.explosionMODE = 2;
        break;
    case 2: {
        if (++e.smokeFrame > 50) e.smokeFrame = 28;
        // pmrSmoke: 40 puffs that stay where they were born, growing and fading, behind the bumper
        e.smoke.sizeStart = 0.5f;
        e.smoke.sizeEnd = 5;
        e.smoke.blendStart = std::max(10.0f, e.damage * 0.01f * 20);
        e.smoke.blendEnd = 0;
        glm::mat4 em = rb.transform();
        em[3] = em * glm::vec4(rb.comLocal + glm::vec3(0, -(e.pBackWheelBase + 1.0f), 0), 1);
        emit(e.smoke, em, 0.8f, 0, 0, 0, 0.4f, 40, false);
        break;
    }
    case 5:
        explodeEnemy(e);
        break;
    case 6:
        ++e.explosionFrame;
        e.explosionSize = std::max(4.0f, e.explosionSize - 1);
        e.explosionBlend = std::max(20.0f, e.explosionBlend - 1);
        e.deadBody.v *= 0.8f;
        e.deadBody.w *= 0.8f;
        if (e.explosionFrame >= 32) e.explosionMODE = 7;
        break;
    case 7:
        e.explosionFrame = -1;
        e.deadBody.v *= 0.8f;
        e.deadBody.w *= 0.8f;
        if (milliSeconds() > e.pExplosionTIMER + 8000) e.explosionMODE = 8;
        break;
    case 8:
        e.deadVisible = false;
        e.explosionMODE = 0;
        break;
    default:
        break;
    }
    if (!e.alive) return;

    float tYOffset = chassisPos.y - car.body.origin().y;
    float tXOffset = chassisPos.x - car.body.origin().x;
    if (!isPlaying) return;
    auto speedFor = [&](float ts) { return ts - ts * e.turnFactor * e.distFactor; };
    if (e.isDuggary) {
        if (e.pUsePassMODE) {
            if (tYOffset < -50) e.pPassMODE = 3;
            else if (tYOffset < 1) e.pPassMODE = 1;
            else if (tYOffset > 100) e.pPassMODE = 2;
            else if (tYOffset > 15) e.pPassMODE = 0;
            else e.pPassMODE = 4;
            if (e.pPassMODE == 2 || !e.pSmartEnemy) e.pMaxSpeed = speedFor(gM5MaxSpeed * 0.5f);
            else if (e.pPassMODE == 1) e.pMaxSpeed = speedFor(gM5MaxSpeed * 1.2f);
            else if (e.pPassMODE == 3) e.pMaxSpeed = speedFor(gM5MaxSpeed * 1.3f);
            else if (e.pPassMODE == 0) e.pMaxSpeed = speedFor(gM5MaxSpeed * 0.85f);
            else if (ticks() > e.duggerySmartTimer + 60 * 4) {
                e.pMaxSpeed = speedFor(gM5Speed - 5 + rnd(12));
                e.duggerySmartTimer = ticks();
            }
        } else {
            e.pMaxSpeed = speedFor(gM5MaxSpeed * 0.8f);
        }
    } else {
        if (!e.pSmartEnemy) {
            e.pPassMODE = 2;
        } else if (tYOffset < 1) {
            if (e.pPassMODE != 1) e.pSmartTIMER = ticks() + rnd(100) + 100;
            e.pPassMODE = 1;
        } else {
            e.pPassMODE = 0;
        }
        if (e.pPassMODE == 1 && ticks() < e.pSmartTIMER) {
            e.pMaxSpeed = speedFor(gM5MaxSpeed * 1.03f);
            if (tYOffset > 5) e.pPassMODE = 0;
        } else if (e.pPassMODE == 2) {
            e.pMaxSpeed = speedFor(gM5MaxSpeed * 0.6f);
        } else {
            e.pMaxSpeed = speedFor(gM5MaxSpeed * 0.73f);
        }
    }
    const bool bossMusic = audio::isPlaying(Music, "duggaryBuild") || audio::isPlaying(Music, "duggaryIntro");
    if (tYOffset < 30 && !e.pJustJumped && e.isDuggary && !bossMusic && !e.pUsePassMODE) {
        audio::play(Music, "duggaryBuild", 4);
        audio::fadeIn(Music, 500);
        audio::queue(Music, "duggaryIntro", 1);
        audio::queue(Music, "duggaryBuild", 0);
        audio::play(VO, "spark_duggary");
        e.pUsePassMODE = true;
    }
    if (tYOffset < 0) {
        if (!e.isBehind) {
            e.isPassed = true;
            --gPosition;
            doUpdatePlace();
            if (std::fabs(tXOffset) <= 10) {
                audio::volume(M5FX, 255);
                audio::play(M5FX, "m5_drive_by2");
            }
        }
        e.isBehind = true;
    } else if (e.isBehind) {
        e.isPassed = false;
        if (!e.pIsJumping) {
            ++gPosition;
            doUpdatePlace();
            if (std::fabs(tXOffset) <= 10) {
                audio::volume(M5FX, 255);
                audio::play(M5FX, "m5_drive_by1");
                if (e.isDuggary && e.pPassMODE == 1 && rnd(2) == 1)
                    audio::play(VO, ("duggaryPass_" + std::to_string(rnd(2))).c_str());
            }
        }
        e.isBehind = false;
    }
    e.pIsJumping = false;
    if (tYOffset < -40 || chassisPos.z < -30) {
        if (!e.isDuggary) {
            audio::stop(M5FX2);
            e.controlMODE = 1;
            e.robotVisible = false;
            enemyJump(e);
        } else if (e.isStuck) {
            e.isStuck = false;
            e.isStuckTIMER = ticks();
            resetDuggary(e);
        }
    }
}

// ---------------------------------------------------------------- frame script (PLAY) and camera

void Game::doFinish() {
    hasFinished = true;
    if (gPosition == 1) {
        audio::volume(VO, 255);
        audio::play(VO, "announcer_m5_wins");
        gScore += 5000;
    } else {
        audio::volume(VO, 255);
        audio::play(VO, "duggery_too_bad");
    }
    isGameOver = true;
    isPlaying = false;
    audio::fadeOut(Music, 1000);
    audio::fadeOut(M5Engine, 1000);
    audio::fadeOut(M5Grnd, 1000);
    logf("frame %ld: finished in place %d with %d points", frameCount, gPosition, gScore);
}

void Game::doCamera() { // CAMERAMODE 1
    glm::vec3 target = car.body.origin();
    glm::mat3 rot = car.body.rotation();
    float carRotZ = car.rotationDeg().z;
    if (!cameraStarted) {
        cameraDummyPos = target + glm::vec3(-10, 0, 2.0f);
        cameraDummyRotZ = carRotZ;
        cameraStarted = true;
    }
    float curDist = glm::distance(cameraDummyPos, target);
    float stepDist = (5.0f - curDist) * 0.3f + curDist;
    float idealZ = (target + rot * glm::vec3(0, -5, 2.5f)).z;
    float stepZ = (idealZ - cameraDummyPos.z) * 0.5f + cameraDummyPos.z;
    // getRotStep(cur, ideal, 0.2)
    float cur = cameraDummyRotZ > 180 ? cameraDummyRotZ - 360 : cameraDummyRotZ;
    float stepNeg = 0, stepPos = 0;
    if (cur >= carRotZ) {
        stepNeg = cur - carRotZ;
        stepPos = 360 - stepNeg;
    } else {
        stepPos = carRotZ - cur;
        stepNeg = 360 - stepPos;
    }
    cameraDummyRotZ = cur + (stepNeg < stepPos ? stepNeg * -0.2f : stepPos * 0.2f);
    float a = glm::radians(cameraDummyRotZ);
    cameraDummyPos = glm::vec3(target.x, target.y, stepZ) + glm::vec3(std::sin(a), -std::cos(a), 0) * stepDist;
    eye = cameraDummyPos;
    lookAt = target;

    // Periscope (SCOPEMODE): the mast rises, then a second view opens in the corner
    switch (SCOPEMODE) {
    case 1:
        scopeFactor = 0.01f;
        scopeRotate = 0;
        scopeStaticBlend = 100;
        scopeUnderBlend = 100;
        scopeViewOn = true;
        SCOPEMODE = 2;
        break;
    case 2:
        if (scopeFactor >= 1) SCOPEMODE = 3;
        scopeFactor = std::min(1.0f, scopeFactor + 0.05f);
        break;
    case 3:
        if (scopeStaticBlend > 0) scopeStaticBlend -= 2.5f;
        scopeRotate = std::min(360.0f, scopeRotate + 20);
        if (scopeRotate == 360) scopeFactor = isUnderWater ? std::min(3.0f, scopeFactor + 0.05f) : std::max(1.0f, scopeFactor - 0.05f);
        break;
    case 4:
        if (scopeFactor > 0.01f) {
            scopeStaticBlend = std::min(100.0f, scopeStaticBlend + 10);
            scopeFactor = std::max(0.01f, scopeFactor - 0.08f);
        } else {
            scopeViewOn = false;
            SCOPEMODE = 5;
        }
        break;
    case 5:
        if (scopeUnderBlend > 0) scopeUnderBlend -= 20;
        else SCOPEMODE = 0;
        break;
    default:
        break;
    }
}

void Game::playFrameScript(const Input& in) {
    helpBlend = gHelpToggle ? std::min(100.0f, helpBlend + 10) : std::max(0.0f, helpBlend - 8);
    if (in.help) {
        if (gHelpToggle == 1 && helpBlend == 100) gHelpToggle = 0;
        else if (gHelpToggle == 0 && helpBlend == 0) gHelpToggle = 1;
    }
    if (!isPlaying && !isStarted && allReady) {
        if (!gGoPlayed) {
            audio::volume(VO, 255);
            audio::play(VO, "announcer_Go");
            audio::volume(Music, 100);
            audio::play(Music, "sr_loop_1_1", 0);
            gGoPlayed = true;
        }
        if (in.forward) {
            isPlaying = true;
            isStarted = true;
            gHelpToggle = 0;
            logf("frame %ld: race started", frameCount);
        }
    }
    doCamera();
    cDirection += 8;
    if (cDirection >= 360) cDirection -= 360;

    // Pickups of the current segment spin and can be collected (handlePickups)
    glm::mat4 carXf = car.body.transform();
    float sawScale = sawsVisible ? pBladeFactor : 1.0f;
    glm::vec3 sawPos = glm::vec3(carXf * glm::vec4(kSawCenter * sawScale, 1));
    float sawRadius = kSawRadius * sawScale;
    for (Pickup& p : pickups) {
        const LevelObject& o = level.objects[p.object];
        if (o.segment != gSEG - 1) continue;
        p.spin += 10;
        if (!p.visible) continue;
        float scale = glm::length(glm::vec3(o.matrix[0]));
        glm::vec3 c = glm::vec3(o.matrix * glm::vec4(o.center, 1));
        if (glm::distance(c, sawPos) > o.radius * scale + sawRadius + g_cfg.pickupReach) continue;
        p.visible = false;
        const bool sparkTalking = audio::isPlaying(VO, "spark_duggary");
        audio::volume(VO, 255);
        switch (p.kind) {
        case Pickup::Wrench:
            showTxt(nullptr, "Sparky");
            gDamage = std::max(0.0f, gDamage - 50);
            if (!sparkTalking) audio::play(VO, gDamage == 0 ? "sp_its_fine_now" : "spa_fixing_is_my_job");
            gScore += 500;
            break;
        case Pickup::Robot:
            if (!sparkTalking) audio::play(VO, "sp_a_remote_robot");
            if (gROBOTMODE == 4) gROBOTMODE = 0;
            gScore += 1000;
            break;
        case Pickup::CandyPurple:
        case Pickup::CandyRed:
            if (!sparkTalking) audio::play(VO, kCandy[rnd(4) - 1]);
            showTxt(nullptr, "Candy");
            gScore += p.kind == Pickup::CandyPurple ? 1000 : 500;
            break;
        case Pickup::Oxygen:
            audio::play(VO, "effect");
            showTxt(nullptr, "O2");
            gOxygen = std::min(100.0f, gOxygen + 50);
            gScore += 500;
            break;
        }
    }

    // Trees: the blades cut the ones in this segment (doHitTree), cut trees fly up and vanish
    if (pHasBlades) {
        glm::vec3 carPos = car.body.origin();
        for (int i = 0; i < (int)level.objects.size(); ++i) {
            const LevelObject& o = level.objects[i];
            if (o.kind != "tree" || !treeAlive[i] || o.segment != gSEG - 1) continue;
            glm::vec3 treePos(o.matrix[3]);
            if (glm::distance(carPos, treePos) > 4) continue;
            float scale = glm::length(glm::vec3(o.matrix[0]));
            glm::vec3 c = glm::vec3(o.matrix * glm::vec4(o.center, 1));
            if (glm::distance(c, sawPos) > o.radius * scale + sawRadius) continue;
            audio::volume(M5FX, 255);
            audio::play(M5FX, "saw");
            audio::queue(M5FX, "m5_blades_loop");
            treeAlive[i] = 0;
            level.collision.disableOwner(i);
            deadTrees.push_back({i, 0, 0});
        }
    }
    for (size_t i = 0; i < deadTrees.size();) {
        deadTrees[i].spin += 10;
        deadTrees[i].rise += 0.75f;
        if (deadTrees[i].rise >= 10) deadTrees.erase(deadTrees.begin() + i);
        else ++i;
    }

    // doInterfaceTxt
    const float bracketWidth = 325;
    switch (InterfaceTxtMODE) {
    case 1:
        bracketX = 8;
        txtBlend = bracketLBlend = bracketRBlend = 0;
        InterfaceTxtMODE = 2;
        break;
    case 2:
        bracketX += std::min(30.0f, (bracketWidth - bracketX) * 0.3f);
        bracketLBlend = std::min(60.0f, bracketLBlend + 10);
        bracketRBlend = std::min(60.0f, bracketLBlend + 10);
        if (bracketX >= bracketWidth - 10) txtBlend = std::min(80.0f, txtBlend + 20);
        if (bracketX >= bracketWidth - 1) InterfaceTxtMODE = 3;
        break;
    case 3:
        txtBlend = std::min(80.0f, txtBlend + 20);
        if (txtBlend >= 80) {
            InterfaceTxtMODE = 4;
            InterfaceTxtTIMER = ticks() + 60;
        }
        break;
    case 4:
        if (ticks() >= InterfaceTxtTIMER) InterfaceTxtMODE = 5;
        break;
    case 5:
        bracketLBlend = std::max(0.0f, bracketLBlend - 5);
        bracketRBlend = std::max(0.0f, bracketRBlend - 5);
        txtBlend = std::max(0.0f, txtBlend - 5);
        if (txtBlend <= 0) {
            bracketLBlend = bracketRBlend = 0;
            InterfaceTxtMODE = 0;
        }
        break;
    default:
        break;
    }
    // doInterfacePic
    switch (InterfacePicMODE) {
    case 1:
        picX = 284;
        picBlend = 100;
        InterfacePicMODE = 2;
        break;
    case 2:
        picX = std::max(284.0f, picX - 40);
        if (picX == 284) {
            InterfacePicTIMER = ticks() + 90;
            InterfacePicMODE = 3;
        }
        break;
    case 3:
        if (ticks() >= InterfacePicTIMER) InterfacePicMODE = 4;
        break;
    case 4:
        picBlend = std::max(0.0f, picBlend - 5);
        if (picBlend <= 0) InterfacePicMODE = 0;
        break;
    default:
        break;
    }
    if (placeBlend > 0) placeBlend = std::max(0.0f, placeBlend - 10);

    // doDisplayOxygen
    if (gOxygenMODE == 1) {
        oxygenPanelBlend = std::min(100.0f, oxygenPanelBlend + 30);
        float before = gOxygenDisplayed;
        gOxygenDisplayed = glm::clamp(gOxygenDisplayed + (gOxygen - gOxygenDisplayed) * 0.3f, 0.0f, 100.0f);
        if (std::fabs(gOxygenDisplayed - gOxygen) <= 0.5f) gOxygenDisplayed = gOxygen;
        if (gOxygenDisplayed < 25 && before > 25) audio::play(VO, "sp_o2_low");
    } else if (gOxygenMODE == 2) {
        oxygenPanelBlend = std::max(0.0f, oxygenPanelBlend - 10);
        if (oxygenPanelBlend <= 0) {
            gOxygenDisplayed = 1;
            gOxygenMODE = 0;
        }
    }
    if (gOxygenMODE != 0 && gOxygenDisplayed <= 0) gDamage = std::max(0.0f, gDamage + 0.3f);

    // doDisplayDamage, and the end of the race
    if (gDamage != gDamageDisplayed) {
        gDamageDisplayed = glm::clamp(gDamageDisplayed + (gDamage - gDamageDisplayed) * 0.3f, 0.0f, 100.0f);
        if (std::fabs(gDamageDisplayed - gDamage) <= 0.5f) gDamageDisplayed = std::min(100.0f, gDamage);
        if (gDamageDisplayed >= 100 && !isGameOver) {
            isGameOver = true;
            isPlaying = false;
            audio::fadeOut(Music, 1000);
            audio::fadeOut(M5Engine, 1000);
            audio::fadeOut(M5Grnd, 1000);
            logf("frame %ld: the car is wrecked", frameCount);
        }
    }
    if (isGameOver) {
        blackBlend = std::min(100.0f, blackBlend + 5);
        if (blackBlend == 100 && !audio::busy(VO)) {
            outcome = hasFinished ? (gPosition == 1 ? Outcome::Win : Outcome::Lose) : Outcome::Die;
            screen = Screen::Result;
        }
    }
}

void Game::frame(const Input& in) {
    if (screen == Screen::Result) return; // the shell shows the result and restarts
    ++frameCount;
    kBackwardIsDown = in.backward;
    keyDown(in);

    // enterFrame: the player's hover impulses. exitFrame: Havok steps, then the car scripts, then the frame script.
    trackFace = car.hover(level, false);
    physicsStep();
    {   // diagnostics: the car sitting on its hull (every wheel closer than 0.35 for most of a second)?
        static int lowFrames = 0;
        bool low = true;
        for (int i = 0; i < 4; ++i) low = low && car.hoverDist[i] < 0.35f;
        lowFrames = low && car.isHovering[0] ? lowFrames + 1 : 0;
        if (lowFrames == 20) logf("frame %ld: STUCK ON THE HULL at %.1f %.1f %.1f (all wheels under 0.35 for 20 frames)", frameCount,
                                  car.body.origin().x, car.body.origin().y, car.body.origin().z);
    }
    {   // diagnostics: is the car under a track surface (it went through the road)?
        RayHit up;
        glm::vec3 o = car.body.origin() + glm::vec3(0, 0, 0.3f);
        if (level.collision.raycast(o, glm::vec3(0, 0, 1), 3.0f, true, up)) {
            static long lastLogged = -100;
            if (frameCount - lastLogged > 20) {
                logf("frame %ld: UNDER THE ROAD at %.1f %.1f %.1f (surface %.2f above), speed %.0f vz %.1f", frameCount, o.x, o.y,
                     o.z, up.dist, car.forwardVel, car.body.v.z);
                lastLogged = frameCount;
            }
        }
    }
    updateMach5(in);
    if (screen == Screen::Result) return;
    for (Enemy& e : enemies) {
        e.pJustJumped = false;
        if (e.alive) updateEnemy(e);
        else if (e.deadVisible || e.explosionMODE >= 6) {
            // the wreck keeps burning after the car itself left the race
            if (e.explosionMODE == 6 && ++e.explosionFrame >= 32) e.explosionMODE = 7;
            if (e.explosionMODE == 7) {
                e.explosionFrame = -1;
                if (milliSeconds() > e.pExplosionTIMER + 8000) {
                    e.deadVisible = false;
                    e.explosionMODE = 0;
                }
            }
            e.deadBody.v *= 0.8f;
            e.deadBody.w *= 0.8f;
        }
    }
    if (!allReady) {
        allReady = car.ready;
        for (const Enemy& e : enemies) allReady = allReady && (e.ready || !e.alive);
    }
    playFrameScript(in);

    stepParticles(dirt);
    for (Enemy& e : enemies) {
        stepParticles(e.smoke);
        stepParticles(e.explosion);
        e.smoke.texture = "kaboom00" + std::to_string(glm::clamp(e.smokeFrame, 18, 50));
        if (e.explosionFrame >= 0) e.explosion.texture = "kaboom00" + std::to_string(18 + glm::clamp(e.explosionFrame, 0, 32));
        e.explosion.sizeStart = e.explosionSize;
        e.explosion.blendStart = e.explosionBlend;
    }
}

// ---------------------------------------------------------------- HUD layout

std::vector<Overlay> Game::overlays() const {
    std::vector<Overlay> o;
    const int L = -1, R = 1; // the stage edge an element belongs to
    auto add = [&](const std::string& tex, float x, float y, float blend, int anchor) {
        if (blend <= 0) return;
        Overlay v{tex, x, y, blend};
        v.anchor = anchor;
        o.push_back(v);
    };
    add("Interface_bracket_L", 8, 9, bracketLBlend, L);
    add("Interface_bracket_R", bracketX, 9, bracketRBlend, L);
    add("Interface_Txt_" + InterfaceTxtTEXT, 20, 24, txtBlend, L);
    // Interface_Info is one stage-wide strip: "DAMAGE" at its left end, "SCORE" at its right
    Overlay info{"Interface_Info", 6, 350, 100};
    const float split = 400; // between the damage bar's track and the score label
    info.u1 = split / 512;
    info.anchor = L;
    o.push_back(info);
    info.x = 6 + split;
    info.u0 = split / 512;
    info.u1 = 1;
    info.anchor = R;
    o.push_back(info);
    auto bar = [&](const char* tex, const char* end, float value, float y, float blend) {
        float w = value * 0.01f * 366;
        if (blend <= 0) return;
        if (w >= 1) {
            Overlay b{tex, 8, y, blend};
            b.u0 = (366 - w) / 366;
            b.width = w;
            b.anchor = L;
            o.push_back(b);
        }
        add(end, 8 + w, y, blend, L);
    };
    bar("Interface_DamageBar", "Interface_DamageBarEnd", gDamageDisplayed, 365, 100);
    add("Interface_OxygenPanel", 6, 320, oxygenPanelBlend, L);
    bar("Interface_OxygenBar", "Interface_OxygenBarEnd", gOxygenDisplayed, 335, oxygenPanelBlend);
    add("Interface_Pic_" + InterfacepicIMG, picX, 230, picBlend, R);
    int score = gScore;
    for (int i = 0; i < 6; ++i) {
        add("Digit_" + std::to_string(score % 10), 491 - 13 * i, 365, 100, R);
        score /= 10;
    }
    if (gPosition >= 10) add("BigDigit_" + std::to_string(gPosition / 10 % 10), 430, 24, 100, R);
    add("BigDigit_" + std::to_string(std::max(0, gPosition) % 10), 455, 24, 100, R);
    add(gPosition == 1 ? "BigDigit_st" : gPosition == 2 ? "BigDigit_nd" : gPosition == 3 ? "BigDigit_rd" : "BigDigit_th", 479, 24, 100, R);
    add("Interface_bracket_L", 420, 9, placeBlend, R);
    add("Interface_bracket_R", 496, 9, placeBlend, R);
    return o;
}
