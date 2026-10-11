// Rigid body and static collision world. Stands in for the Havok 1.x Xtra the game used;
// the numbers and the integration scheme follow NOTES.md ("Havok Xtra").
#pragma once
#include <cstdint>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

struct Triangle {
    glm::vec3 a, b, c;
    uint16_t surface; // track mesh id (1..16), 0 for solids
    uint16_t kind;    // 0 track, 1 solid, 2 tree
    int32_t owner;    // level object index for trees, -1 otherwise
};

struct RayHit {
    float dist;
    glm::vec3 point;
    glm::vec3 normal;
    int surface;
    int kind;
    int owner;
};

class CollisionWorld {
public:
    void build(std::vector<Triangle> tris);
    // Nearest hit within maxDist. trackOnly limits the test to track triangles (modelsUnderRay on SEG models).
    bool raycast(const glm::vec3& origin, const glm::vec3& dir, float maxDist, bool trackOnly, RayHit& out,
                 bool ignoreTrees = false) const;
    // Calls fn(triangle) for every triangle whose cell overlaps the sphere's XY bounds.
    template <class F> void forEachNear(const glm::vec3& p, float r, F fn) const {
        int x0 = cell(p.x - r), x1 = cell(p.x + r), y0 = cell(p.y - r), y1 = cell(p.y + r);
        for (int x = x0; x <= x1; ++x)
            for (int y = y0; y <= y1; ++y) {
                auto it = grid_.find(key(x, y));
                if (it == grid_.end()) continue;
                for (int i : it->second) {
                    const Triangle& t = tris_[i];
                    if (t.owner >= 0 && t.owner < (int)ownerOff_.size() && ownerOff_[t.owner]) continue;
                    fn(t);
                }
            }
    }
    size_t size() const { return tris_.size(); }
    // Removes one object's triangles from all later tests (a sawn tree).
    void disableOwner(int owner) {
        if (owner >= (int)ownerOff_.size()) ownerOff_.resize(owner + 1, 0);
        ownerOff_[owner] = 1;
    }
    void enableAllOwners() { ownerOff_.clear(); }

private:
    static constexpr float kCell = 8.0f;
    static int cell(float v) { return (int)glm::floor(v / kCell); }
    static int64_t key(int x, int y) { return ((int64_t)x << 32) ^ (uint32_t)y; }
    std::vector<Triangle> tris_;
    std::vector<uint8_t> ownerOff_;
    std::unordered_map<int64_t, std::vector<int>> grid_;
};

glm::vec3 closestPointOnTriangle(const glm::vec3& p, const Triangle& t);

// What a body ran into during a step; speed is how fast it was closing along the normal.
struct Contact {
    int kind = -1;  // Triangle::kind
    int owner = -1;
    float speed = 0;
    glm::vec3 normal{0};
};

struct RigidBody {
    float mass = 1, invMass = 1;
    glm::vec3 comLocal{0};      // centre of mass in model space
    glm::mat3 invInertiaBody{1};
    glm::vec3 x{0};             // centre of mass, world
    glm::quat q{1, 0, 0, 0};
    glm::vec3 v{0}, w{0};
    glm::vec3 force{0};         // consumed by the next substep only, like Havok's accumulator

    glm::mat3 rotation() const { return glm::mat3_cast(q); }
    glm::mat3 invInertiaWorld() const { glm::mat3 r = rotation(); return r * invInertiaBody * glm::transpose(r); }
    glm::vec3 origin() const { return x - rotation() * comLocal; }      // model position
    glm::mat4 transform() const;                                        // model matrix
    glm::vec3 pointVelocity(const glm::vec3& worldPoint) const { return v + glm::cross(w, worldPoint - x); }

    void applyImpulse(const glm::vec3& j) { v += j * invMass; }
    void applyAngularImpulse(const glm::vec3& j) { w += invInertiaWorld() * j; }
    // localPoint is in model space, as the Lingo passes it.
    void applyImpulseAtPoint(const glm::vec3& j, const glm::vec3& localPoint);
    void applyForce(const glm::vec3& f) { force += f; }
    glm::vec3 angularMomentum() const { return glm::inverse(invInertiaWorld()) * w; }

    // One explicit Euler substep, then contact resolution against the world (restitution 0, friction 0).
    // Each sphere is also swept from its old to its new position, so thin walls are not skipped at speed.
    // The hardest hit on something other than the track is kept in *hit; trees are skipped when ignoreTrees.
    void substep(float h, const glm::vec3& gravity, const CollisionWorld& world,
                 const std::vector<glm::vec4>& spheres /* xyz model space, w radius */,
                 Contact* hit = nullptr, bool ignoreTrees = false);
    void teleport(const glm::vec3& modelPos, const glm::quat& rot) {
        q = rot;
        x = modelPos + rotation() * comLocal;
    }
};

// Sphere-set against sphere-set between two bodies. Returns the closing speed of the hardest contact (0 if none).
float collideBodies(RigidBody& a, const std::vector<glm::vec4>& sa, RigidBody& b, const std::vector<glm::vec4>& sb,
                    float restitution, glm::vec3* normalOnA = nullptr);
