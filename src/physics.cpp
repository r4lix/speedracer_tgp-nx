#include "physics.h"

#include <algorithm>
#include <glm/gtc/matrix_transform.hpp>

void CollisionWorld::build(std::vector<Triangle> tris) {
    tris_ = std::move(tris);
    grid_.clear();
    for (int i = 0; i < (int)tris_.size(); ++i) {
        const Triangle& t = tris_[i];
        int x0 = cell(std::min({t.a.x, t.b.x, t.c.x})), x1 = cell(std::max({t.a.x, t.b.x, t.c.x}));
        int y0 = cell(std::min({t.a.y, t.b.y, t.c.y})), y1 = cell(std::max({t.a.y, t.b.y, t.c.y}));
        for (int x = x0; x <= x1; ++x)
            for (int y = y0; y <= y1; ++y) grid_[key(x, y)].push_back(i);
    }
}

bool CollisionWorld::raycast(const glm::vec3& o, const glm::vec3& d, float maxDist, bool trackOnly, RayHit& out,
                             bool ignoreTrees) const {
    glm::vec3 e = o + d * maxDist;
    int x0 = cell(std::min(o.x, e.x)), x1 = cell(std::max(o.x, e.x));
    int y0 = cell(std::min(o.y, e.y)), y1 = cell(std::max(o.y, e.y));
    float best = maxDist;
    bool hit = false;
    for (int x = x0; x <= x1; ++x)
        for (int y = y0; y <= y1; ++y) {
            auto it = grid_.find(key(x, y));
            if (it == grid_.end()) continue;
            for (int i : it->second) {
                const Triangle& t = tris_[i];
                if (trackOnly && t.kind != 0) continue;
                if (ignoreTrees && t.kind == 2) continue;
                if (t.owner >= 0 && t.owner < (int)ownerOff_.size() && ownerOff_[t.owner]) continue;
                // Moller-Trumbore, both sides
                glm::vec3 e1 = t.b - t.a, e2 = t.c - t.a, p = glm::cross(d, e2);
                float det = glm::dot(e1, p);
                if (glm::abs(det) < 1e-9f) continue;
                float inv = 1.0f / det;
                glm::vec3 s = o - t.a;
                float u = glm::dot(s, p) * inv;
                if (u < 0 || u > 1) continue;
                glm::vec3 qv = glm::cross(s, e1);
                float v = glm::dot(d, qv) * inv;
                if (v < 0 || u + v > 1) continue;
                float dist = glm::dot(e2, qv) * inv;
                if (dist < 0 || dist > best) continue;
                best = dist;
                hit = true;
                out.dist = dist;
                out.point = o + d * dist;
                out.normal = glm::normalize(glm::cross(e1, e2));
                out.surface = t.surface;
                out.kind = t.kind;
                out.owner = t.owner;
            }
        }
    return hit;
}

// Ericson, Real-Time Collision Detection 5.1.5
glm::vec3 closestPointOnTriangle(const glm::vec3& p, const Triangle& t) {
    glm::vec3 ab = t.b - t.a, ac = t.c - t.a, ap = p - t.a;
    float d1 = glm::dot(ab, ap), d2 = glm::dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) return t.a;
    glm::vec3 bp = p - t.b;
    float d3 = glm::dot(ab, bp), d4 = glm::dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) return t.b;
    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) return t.a + ab * (d1 / (d1 - d3));
    glm::vec3 cp = p - t.c;
    float d5 = glm::dot(ab, cp), d6 = glm::dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) return t.c;
    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) return t.a + ac * (d2 / (d2 - d6));
    float va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) return t.b + (t.c - t.b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    float denom = 1.0f / (va + vb + vc);
    return t.a + ab * (vb * denom) + ac * (vc * denom);
}

glm::mat4 RigidBody::transform() const {
    glm::mat4 m = glm::mat4(rotation());
    m[3] = glm::vec4(origin(), 1.0f);
    return m;
}

void RigidBody::applyImpulseAtPoint(const glm::vec3& j, const glm::vec3& localPoint) {
    glm::vec3 r = rotation() * (localPoint - comLocal);
    v += j * invMass;
    w += invInertiaWorld() * glm::cross(r, j);
}

void RigidBody::substep(float h, const glm::vec3& gravity, const CollisionWorld& world, const std::vector<glm::vec4>& spheres,
                        Contact* hit, bool ignoreTrees) {
    const glm::vec3 xOld = x;
    const glm::mat3 rOld = rotation();
    // Explicit Euler on (x, q, P, L): positions advance with the old velocities.
    x += v * h;
    glm::quat dq = glm::quat(0, w.x, w.y, w.z) * q;
    q = glm::normalize(q + dq * (0.5f * h));
    v += (gravity + force * invMass) * h;
    force = glm::vec3(0);

    glm::mat3 r = rotation();
    glm::mat3 iinv = r * invInertiaBody * glm::transpose(r);
    auto resolve = [&](const glm::vec3& cp, const glm::vec3& n, int kind, int owner) {
        glm::vec3 rc = cp - x;
        float vn = glm::dot(v + glm::cross(w, rc), n);
        if (vn >= 0) return;
        if (hit && kind != 0 && -vn > hit->speed) {
            hit->kind = kind;
            hit->owner = owner;
            hit->speed = -vn;
            hit->normal = n;
        }
        float k = invMass + glm::dot(glm::cross(iinv * glm::cross(rc, n), rc), n);
        glm::vec3 j = n * (-vn / k);
        v += j * invMass;
        w += iinv * glm::cross(rc, j);
    };
    for (const glm::vec4& s : spheres) {
        glm::vec3 local = glm::vec3(s) - comLocal;
        glm::vec3 c = x + r * local;
        float rad = s.w;
        // Sweep: did the centre cross a surface on the way here?
        glm::vec3 from = xOld + rOld * local, move = c - from;
        float dist = glm::length(move);
        if (dist > rad * 0.5f) {
            RayHit rh;
            glm::vec3 dir = move / dist;
            if (world.raycast(from, dir, dist, false, rh, ignoreTrees)) {
                glm::vec3 n = glm::dot(rh.normal, dir) > 0 ? -rh.normal : rh.normal;
                glm::vec3 back = rh.point + n * rad - c; // put the sphere back on the near side
                x += back;
                c += back;
                resolve(rh.point, n, rh.kind, rh.owner);
            }
        }
        glm::vec3 push(0);
        world.forEachNear(c, rad, [&](const Triangle& t) {
            if (ignoreTrees && t.kind == 2) return;
            glm::vec3 cp = closestPointOnTriangle(c + push, t);
            glm::vec3 d = c + push - cp;
            float d2 = glm::dot(d, d);
            if (d2 >= rad * rad || d2 < 1e-12f) return;
            float dist = glm::sqrt(d2);
            glm::vec3 n = d / dist;
            push += n * (rad - dist);
            resolve(cp, n, t.kind, t.owner);
        });
        x += push;
    }
}

float collideBodies(RigidBody& a, const std::vector<glm::vec4>& sa, RigidBody& b, const std::vector<glm::vec4>& sb,
                    float restitution, glm::vec3* normalOnA) {
    // Cheap reject: the cars are under 4 units long.
    if (glm::distance(a.x, b.x) > 6.0f) return 0;
    glm::mat3 ra = a.rotation(), rb = b.rotation();
    glm::mat3 ia = a.invInertiaWorld(), ib = b.invInertiaWorld();
    float hardest = 0;
    for (const glm::vec4& s1 : sa) {
        glm::vec3 c1 = a.x + ra * (glm::vec3(s1) - a.comLocal);
        for (const glm::vec4& s2 : sb) {
            glm::vec3 c2 = b.x + rb * (glm::vec3(s2) - b.comLocal);
            glm::vec3 d = c1 - c2;
            float rsum = s1.w + s2.w, d2 = glm::dot(d, d);
            if (d2 >= rsum * rsum || d2 < 1e-10f) continue;
            float dist = glm::sqrt(d2);
            glm::vec3 n = d / dist; // pushes a away from b
            float pen = rsum - dist, wa = a.invMass / (a.invMass + b.invMass);
            a.x += n * pen * wa;
            b.x -= n * pen * (1 - wa);
            glm::vec3 p = c2 + n * s2.w, r1 = p - a.x, r2 = p - b.x;
            float vn = glm::dot(a.pointVelocity(p) - b.pointVelocity(p), n);
            if (vn >= 0) continue;
            float k = a.invMass + b.invMass + glm::dot(glm::cross(ia * glm::cross(r1, n), r1), n) +
                      glm::dot(glm::cross(ib * glm::cross(r2, n), r2), n);
            glm::vec3 j = n * (-(1 + restitution) * vn / k);
            a.v += j * a.invMass;
            a.w += ia * glm::cross(r1, j);
            b.v -= j * b.invMass;
            b.w -= ib * glm::cross(r2, j);
            if (-vn > hardest) {
                hardest = -vn;
                if (normalOnA) *normalOnA = n;
            }
        }
    }
    return hardest;
}
