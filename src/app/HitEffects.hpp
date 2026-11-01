#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <unordered_map>
#include <vector>

#include "SpreadSeed.hpp"

namespace hitfx {

struct P3 {
    float x = 0, y = 0, z = 0;
};
inline P3 operator+(P3 a, P3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline P3 operator-(P3 a, P3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline P3 operator*(P3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(P3 a, P3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline P3 cross(P3 a, P3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float length(P3 a) { return std::sqrt(dot(a, a)); }
inline bool finite(P3 a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }
inline P3 normalize(P3 a, P3 fallback = {0, 0, 1})
{
    const float l = length(a);
    return l > 1e-6f && std::isfinite(l) ? a * (1.0f / l) : fallback;
}

struct Rgba {
    float r = 1, g = 1, b = 1, a = 1;
};

struct Ray {
    P3 origin;
    P3 dir{1, 0, 0};
    bool valid = false;
};

inline P3 forward_from_angles(float pitch_deg, float yaw_deg)
{
    constexpr float k = 3.14159265358979f / 180.0f;
    const float p = pitch_deg * k, y = yaw_deg * k;
    return {std::cos(p) * std::cos(y), std::cos(p) * std::sin(y), -std::sin(p)};
}

inline Ray aim_ray(P3 eye, P3 angles, P3 aim_punch)
{
    Ray r;
    if (!finite(eye) || !finite(angles)) return r;
    const P3 punch = finite(aim_punch) ? aim_punch : P3{};
    r.origin = eye;
    r.dir = normalize(forward_from_angles(angles.x + punch.x * 2.0f, angles.y + punch.y * 2.0f));
    r.valid = std::abs(angles.x) <= 90.5f && length(eye) > 0.0f;
    return r;
}

constexpr int kMaxSpreadPellets = 64;
constexpr int kMaxFlashPellets = 4;
constexpr float kSeedBucketGuard = 0.01f;

struct AimSample {
    Ray ray;
    P3 hash_angles;
    bool seeded = false;
    std::int32_t tick = 0;
    spread::WeaponSpread weapon;
};

inline AimSample aim_sample(P3 eye, P3 angles, P3 aim_punch)
{
    AimSample s;
    s.ray = aim_ray(eye, angles, aim_punch);
    const P3 punch = finite(aim_punch) ? aim_punch : P3{};
    if (finite(angles))
        s.hash_angles = {angles.x + spread::normalize_angle(punch.x * 2.0f),
                         angles.y + spread::normalize_angle(punch.y * 2.0f), 0.0f};
    return s;
}

inline bool spread_payload_ok(const spread::WeaponSpread& w)
{
    return std::isfinite(w.total) && std::isfinite(w.base) && std::isfinite(w.recoil_index) && w.base >= 0.0f &&
        w.total >= w.base && w.total <= 1.0f && w.recoil_index >= 0.0f && w.num_bullets >= 1 &&
        w.num_bullets <= kMaxSpreadPellets && w.spread_seed >= 0;
}

struct ShotRays {
    std::vector<Ray> rays;
    bool seeded = false;
};

inline ShotRays shot_rays(const AimSample& s, int max_pellets = kMaxFlashPellets)
{
    ShotRays out;
    if (!s.ray.valid) return out;
    const bool usable = s.seeded && spread_payload_ok(s.weapon) && finite(s.hash_angles) &&
        spread::bucket_margin(s.hash_angles.x) >= kSeedBucketGuard &&
        spread::bucket_margin(s.hash_angles.y) >= kSeedBucketGuard;
    if (!usable) {
        out.rays.push_back(s.ray);
        return out;
    }
    const std::uint32_t seed = spread::shot_seed(s.hash_angles.x, s.hash_angles.y, s.tick);
    const auto basis = spread::view_basis(s.hash_angles.x, s.hash_angles.y);
    const int n = std::clamp(std::min(s.weapon.num_bullets, max_pellets), 1, kMaxSpreadPellets);
    for (int i = 0; i < n; ++i) {
        float d[3];
        spread::spread_direction(basis, spread::spread_offset(seed, i, s.weapon), d);
        Ray r = s.ray;
        r.dir = normalize(P3{d[0], d[1], d[2]}, s.ray.dir);
        out.rays.push_back(r);
    }
    out.seeded = true;
    return out;
}

struct BodyCapsule {
    int a, b;
    float radius;
};
inline const std::vector<BodyCapsule>& body_capsules()
{
    static const std::vector<BodyCapsule> caps = {
        {7, 7, 4.8f},  {7, 6, 3.6f},  {6, 23, 7.0f},  {23, 1, 7.5f},  {23, 10, 3.2f}, {10, 11, 2.6f},
        {23, 14, 3.2f}, {14, 15, 2.6f}, {1, 18, 4.6f},  {18, 19, 3.6f}, {1, 21, 4.6f},  {21, 22, 3.6f},
    };
    return caps;
}

struct Joints {
    const P3* positions = nullptr;
    const bool* available = nullptr;
    int count = 0;
    bool has(int j) const
    {
        return positions && j >= 0 && j < count && (!available || available[j]) && finite(positions[j]) &&
            length(positions[j]) > 0.0f;
    }
};

struct Impact {
    P3 point;
    P3 dir{1, 0, 0};
    bool on_ray = false;
    bool from_body = false;
};

inline float ray_segment_closest(const Ray& ray, P3 a, P3 b, float& t_out, float& s_out)
{
    const P3 d1 = ray.dir, d2 = b - a, r = ray.origin - a;
    const float aa = dot(d1, d1), e = dot(d2, d2), f = dot(d2, r);
    float s = 0, t = 0;
    if (e <= 1e-8f) {
        t = std::max(0.0f, -dot(d1, r) / aa);
        s = 0;
    } else {
        const float c = dot(d1, r), bb = dot(d1, d2), denom = aa * e - bb * bb;
        t = denom > 1e-8f ? std::max(0.0f, (bb * f - c * e) / denom) : 0.0f;
        s = std::clamp((bb * t + f) / e, 0.0f, 1.0f);
        t = std::max(0.0f, (bb * s - c) / aa);
        s = std::clamp((bb * t + f) / e, 0.0f, 1.0f);
    }
    const P3 diff = (ray.origin + d1 * t) - (a + d2 * s);
    t_out = t;
    s_out = s;
    return dot(diff, diff);
}

inline Impact estimate_impact(const Ray& ray, const Joints& joints, P3 fallback, float max_miss = 18.0f)
{
    Impact out;
    out.point = fallback;
    out.dir = ray.valid ? ray.dir : P3{1, 0, 0};
    float best_miss = 1e30f, best_t = 0, best_dist = 0, best_radius = 0;
    P3 best_axis{};
    bool any = false;
    P3 torso{};
    bool have_torso = false;
    for (const auto& c : body_capsules()) {
        if (!joints.has(c.a) || !joints.has(c.b)) continue;
        const P3 a = joints.positions[c.a], b = joints.positions[c.b];
        if (!have_torso && (c.a == 6 || c.a == 23)) {
            torso = (a + b) * 0.5f;
            have_torso = true;
        }
        any = true;
        if (!ray.valid) continue;
        float t = 0, s = 0;
        const float dist = std::sqrt(ray_segment_closest(ray, a, b, t, s));
        const float miss = dist - c.radius;
        if (miss < best_miss) {
            best_miss = miss;
            best_t = t;
            best_dist = dist;
            best_radius = c.radius;
            best_axis = a + (b - a) * s;
        }
    }
    if (!any) return out;
    out.from_body = true;
    if (ray.valid && best_miss <= max_miss) {
        out.on_ray = true;
        if (best_dist < best_radius) {
            const float back = std::sqrt(std::max(0.0f, best_radius * best_radius - best_dist * best_dist));
            out.point = ray.origin + ray.dir * std::max(0.0f, best_t - back);
        } else {
            const P3 on_ray = ray.origin + ray.dir * best_t;
            out.point = best_axis + normalize(on_ray - best_axis) * best_radius;
        }
        return out;
    }
    const P3 centre = have_torso ? torso : fallback;
    const P3 toward = ray.valid ? normalize(ray.origin - centre, P3{0, 0, 1}) : P3{0, 0, 0};
    out.point = centre + toward * 7.0f;
    if (ray.valid) out.dir = normalize(centre - ray.origin, ray.dir);
    return out;
}

inline float ray_body_entry(const Ray& ray, const Joints& joints)
{
    if (!ray.valid) return -1.0f;
    float best = -1.0f;
    for (const auto& c : body_capsules()) {
        if (!joints.has(c.a) || !joints.has(c.b)) continue;
        float t = 0, s = 0;
        const float dist = std::sqrt(ray_segment_closest(ray, joints.positions[c.a], joints.positions[c.b], t, s));
        if (dist >= c.radius) continue;
        const float entry = std::max(0.0f, t - std::sqrt(c.radius * c.radius - dist * dist));
        if (best < 0.0f || entry < best) best = entry;
    }
    return best;
}

struct EnemySample {
    int entity = -1;
    std::uint64_t identity = 0;
    int health = 0;
};

struct HitEvent {
    int entity = -1;
    int damage = 0;
    bool kill = false;
    Ray ray;
};

class HitDetector {
public:
    void reset()
    {
        previous_.clear();
        shots_ = -1;
        new_shots_ = 0;
        shot_at_ = -1e9;
        shot_ray_ = {};
        last_aim_ = {};
        shot_sample_ = {};
    }

    std::vector<HitEvent> update(double now, bool local_alive, int local_shots, const Ray& aim,
                                 const std::vector<EnemySample>& enemies, bool require_local_shot,
                                 double shot_window = 0.5)
    {
        AimSample sample;
        sample.ray = aim;
        return update(now, local_alive, local_shots, sample, enemies, require_local_shot, shot_window);
    }

    std::vector<HitEvent> update(double now, bool local_alive, int local_shots, const AimSample& sample,
                                 const std::vector<EnemySample>& enemies, bool require_local_shot,
                                 double shot_window = 0.5)
    {
        const Ray& aim = sample.ray;
        std::vector<HitEvent> events;
        if (!local_alive) {
            reset();
            return events;
        }
        new_shots_ = 0;
        if (shots_ >= 0 && local_shots > shots_) {
            shot_at_ = now;
            if (use_shot_sample_) {
                shot_ray_ = aim;
                shot_sample_ = sample;
            } else if (last_aim_.valid) {
                shot_ray_ = last_aim_;
                shot_sample_ = last_sample_;
            } else {
                shot_ray_ = aim;
                shot_sample_ = sample;
                shot_sample_.seeded = false;
            }
            new_shots_ = std::min(local_shots - shots_, 8);
        }
        shots_ = local_shots;
        last_aim_ = aim;
        last_sample_ = sample;
        const bool recent_shot = now - shot_at_ <= shot_window && now >= shot_at_;
        std::unordered_map<int, Tracked> next;
        next.reserve(enemies.size());
        for (const auto& e : enemies) {
            if (e.entity < 0) continue;
            auto it = previous_.find(e.entity);
            if (it != previous_.end() && it->second.identity == e.identity && it->second.health > 0 &&
                e.health < it->second.health && (!require_local_shot || recent_shot)) {
                HitEvent ev;
                ev.entity = e.entity;
                ev.damage = it->second.health - std::max(0, e.health);
                ev.kill = e.health <= 0;
                ev.ray = recent_shot && shot_ray_.valid ? shot_ray_ : aim;
                events.push_back(ev);
            }
            next[e.entity] = {e.identity, e.health};
        }
        previous_ = std::move(next);
        return events;
    }

    void set_use_shot_sample(bool on) { use_shot_sample_ = on; }

    int new_shots() const { return new_shots_; }
    const Ray& shot_ray() const { return shot_ray_; }
    const AimSample& shot_sample() const { return shot_sample_; }

private:
    struct Tracked {
        std::uint64_t identity = 0;
        int health = 0;
    };
    std::unordered_map<int, Tracked> previous_;
    int shots_ = -1, new_shots_ = 0;
    double shot_at_ = -1e9;
    Ray shot_ray_, last_aim_;
    AimSample shot_sample_, last_sample_;
    bool use_shot_sample_ = false;
};

struct EffectParams {
    Rgba color{1.0f, 0.15f, 0.15f, 1.0f};
    int count = 14;
    float size = 2.5f;
    float speed = 1.0f;
    float lifetime = 0.45f;
};

struct FlashParams {
    Rgba color{1, 1, 1, 1};
    float size = 3.0f;
    float duration = 0.06f;
    float hold_gap = 0.0f;
};

struct Particle {
    P3 pos, vel;
    float life = 0, max_life = 0, size = 0, drag = 0;
    Rgba color;
};
struct Flash {
    P3 pos;
    float age = 0, duration = 0, size = 0;
    float since = 0, hold_gap = 0;
    Rgba color;
};

// Presentation grouping of native records; these clocks are not claimed as shot IDs.
// Keep one impact by default, or up to four pellet/penetration impacts together.
class ObservedFlashSelection {
    bool grouped_ = false;
    float clock_ = 0, expires_ = 0;
    unsigned count_ = 0;
public:
    struct Choice { bool show = false, replace_hold = false; };
    void clear() noexcept { grouped_ = false; count_ = 0; }
    Choice select(float clock, float expires, bool additional) noexcept {
        if (!std::isfinite(clock) || !std::isfinite(expires)) return {};
        const bool first = !grouped_ || clock_ != clock || expires_ != expires;
        if (first) { grouped_ = true; clock_ = clock; expires_ = expires; count_ = 0; }
        if (count_ >= (additional ? 4u : 1u)) return {};
        ++count_;
        return {true, first};
    }
};

class HitFx {
public:
    static constexpr std::size_t kMaxParticles = 2048, kMaxFlashes = 128;

    explicit HitFx(std::uint32_t seed = 0x5eed1234u) : rng_(seed) {}

    void clear()
    {
        particles_.clear();
        flashes_.clear();
    }

    void clear_flashes() { flashes_.clear(); }

    void spawn_effect(P3 at, P3 dir, const EffectParams& p)
    {
        if (!finite(at)) return;
        dir = normalize(dir, P3{1, 0, 0});
        const int count = std::clamp(p.count, 0, 200);
        const float life = std::clamp(p.lifetime, 0.05f, 5.0f);
        const float speed = std::clamp(p.speed, 0.0f, 10.0f);
        for (int i = 0; i < count; ++i) {
            Particle s;
            s.pos = at;
            const P3 v = cone(normalize(dir * -0.4f + P3{0, 0, 0.6f}), 1.4f);
            s.vel = v * (speed * (40.0f + 90.0f * uniform()));
            s.life = s.max_life = life * (0.6f + 0.8f * uniform());
            s.size = std::max(0.3f, p.size) * (0.5f + 0.5f * uniform());
            s.drag = 1.0f;
            s.color = p.color;
            push(particles_, s, kMaxParticles);
        }
    }

    void spawn_flash(P3 at, const FlashParams& p, bool replace_hold = true)
    {
        if (!finite(at)) return;
        Flash f;
        f.pos = at;
        f.duration = std::clamp(p.duration, 0.02f, 2.0f);
        f.size = std::clamp(p.size, 0.5f, 64.0f);
        f.color = p.color;
        f.hold_gap = p.hold_gap > 0.0f && std::isfinite(p.hold_gap) ? std::clamp(p.hold_gap, 0.02f, 2.0f) : 0.0f;
        if (f.hold_gap > 0.0f && replace_hold)
            erase_if(flashes_, [](const Flash& q) { return q.hold_gap > 0.0f; });
        push(flashes_, f, kMaxFlashes);
    }

    void step(float dt, float gravity)
    {
        if (!(dt > 0.0f) || !std::isfinite(dt)) return;
        dt = std::min(dt, 0.1f);
        for (auto& q : particles_) {
            q.life -= dt;
            q.vel = q.vel * std::max(0.0f, 1.0f - q.drag * dt);
            q.vel.z += gravity * dt;
            q.pos = q.pos + q.vel * dt;
        }
        erase_if(particles_, [](const Particle& q) { return q.life <= 0.0f; });
        for (auto& q : flashes_) {
            q.since += dt;
            q.age = q.hold_gap > 0.0f ? std::max(0.0f, q.since - q.hold_gap) : q.since;
        }
        erase_if(flashes_, [](const Flash& q) { return q.age >= q.duration; });
    }

    const std::vector<Particle>& particles() const { return particles_; }
    const std::vector<Flash>& flashes() const { return flashes_; }
    bool empty() const { return particles_.empty() && flashes_.empty(); }

    static float flash_alpha(const Flash& f)
    {
        const float t = std::clamp(f.age / f.duration, 0.0f, 1.0f);
        if (t < 0.35f) return 1.0f;
        const float k = 1.0f - (t - 0.35f) / 0.65f;
        return k * k;
    }

private:
    float uniform() { return std::uniform_real_distribution<float>(0.0f, 1.0f)(rng_); }
    P3 cone(P3 axis, float spread)
    {
        const P3 r{uniform() * 2 - 1, uniform() * 2 - 1, uniform() * 2 - 1};
        return normalize(axis + r * spread, axis);
    }
    template <class T>
    static void push(std::vector<T>& v, const T& x, std::size_t cap)
    {
        if (v.size() >= cap) v.erase(v.begin());
        v.push_back(x);
    }
    template <class T, class F>
    static void erase_if(std::vector<T>& v, F f)
    {
        v.erase(std::remove_if(v.begin(), v.end(), f), v.end());
    }
    std::minstd_rand rng_;
    std::vector<Particle> particles_;
    std::vector<Flash> flashes_;
};

}
