#pragma once
#include <vector>

#include <glm/glm.hpp>

#include "config.hpp"

struct ImDrawList;

// World-space damage effects shared by the Kill FX and Hit FX halves of the
// DamageFX card. Chams are rendered through the GPU mesh pass and stay in App.
namespace damagefx {

struct Particle {
    glm::vec3 pos{0.0f};
    glm::vec3 vel{0.0f};
    float lifetime = 0.0f;
    float max_lifetime = 0.0f;
    Color color;
    float size = 1.0f;
};

struct Lightning {
    std::vector<glm::vec3> points;
    float age = 0.0f;  // negative while a staggered strike is not yet active
    float max_lifetime = 0.4f;
    Color color;
    float width = 4.0f;
};

struct Shockwave {
    glm::vec3 center{0.0f};
    float age = 0.0f;
    float max_lifetime = 0.5f;
    Color color;
    float max_radius = 120.0f;
    float width = 3.0f;
};

struct Pulse {
    bool active = false;
    float age = 0.0f;
    Color color;
    float duration = 0.6f;
    float intensity = 1.0f;
};

struct ParticleSettings {
    Color color;
    int count = 30;
    float speed = 2.0f;
    float lifetime = 1.2f;
    float size = 3.0f;
};

struct PulseSettings {
    Color color;
    float duration = 0.6f;
    float intensity = 1.0f;
};

struct LightningSettings {
    Color color;
    float duration = 0.4f;
    float width = 4.0f;
    int count = 3;
};

struct ShockwaveSettings {
    Color color;
    float duration = 0.5f;
    float max_radius = 120.0f;
    float width = 3.0f;
};

class DamageFx {
public:
    static constexpr std::size_t kMaxParticles = 4096;
    static constexpr std::size_t kMaxLightnings = 64;
    static constexpr std::size_t kMaxShockwaves = 32;

    void clear();

    void spawn_particles(const glm::vec3& at, const ParticleSettings& settings);
    void spawn_pulse(const PulseSettings& settings);
    void spawn_lightning(const glm::vec3& at, const LightningSettings& settings);
    void spawn_shockwave(const glm::vec3& at, const ShockwaveSettings& settings);

    // Advances every effect by dt. `gravity` applies to particles only.
    void step(float dt, float gravity);

    // Projects and draws particles, lightning, shockwaves and the screen pulse.
    void draw(ImDrawList* draw_list, const float* view_matrix, int width, int height) const;

    bool empty() const;

    const std::vector<Particle>& particles() const { return particles_; }
    const std::vector<Lightning>& lightnings() const { return lightnings_; }
    const std::vector<Shockwave>& shockwaves() const { return shockwaves_; }
    const Pulse& pulse() const { return pulse_; }

private:
    std::vector<Particle> particles_;
    std::vector<Lightning> lightnings_;
    std::vector<Shockwave> shockwaves_;
    Pulse pulse_;
};

}  // namespace damagefx
