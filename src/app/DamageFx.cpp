#include "DamageFx.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>

#include "external/imgui/imgui.h"
#include "overlay/esp_drawing.hpp"

namespace damagefx {
namespace {

constexpr float kPi = 3.14159265358979f;

float rand01()
{
    return static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX);
}

ImU32 to_u32(const Color& color, float alpha)
{
    return ImGui::ColorConvertFloat4ToU32(
        ImVec4(color.r, color.g, color.b, std::clamp(color.a * alpha, 0.0f, 1.0f)));
}

template <class T>
void push_capped(std::vector<T>& vec, const T& value, std::size_t cap)
{
    if (vec.size() >= cap) vec.erase(vec.begin());
    vec.push_back(value);
}

}  // namespace

void DamageFx::clear()
{
    particles_.clear();
    lightnings_.clear();
    shockwaves_.clear();
    pulse_ = {};
}

void DamageFx::spawn_particles(const glm::vec3& at, const ParticleSettings& settings)
{
    const int count = std::clamp(settings.count, 0, 500);
    const float speed_scale = std::max(0.0f, settings.speed);
    const float lifetime = std::max(0.05f, settings.lifetime);
    const float size = std::max(0.1f, settings.size);
    // Drop the oldest particles in one go rather than one erase per push.
    if (particles_.size() + static_cast<std::size_t>(count) > kMaxParticles) {
        const std::size_t overflow = particles_.size() + static_cast<std::size_t>(count) - kMaxParticles;
        particles_.erase(particles_.begin(), particles_.begin() + static_cast<std::ptrdiff_t>(overflow));
    }
    for (int i = 0; i < count; ++i) {
        Particle particle;
        particle.pos = at;

        // Radial spray with a slight upward bias.
        const float theta = rand01() * 2.0f * kPi;
        const float phi = rand01() * kPi;
        const float speed = speed_scale * (20.0f + rand01() * 80.0f);
        particle.vel.x = std::cos(theta) * std::sin(phi) * speed;
        particle.vel.y = std::sin(theta) * std::sin(phi) * speed;
        particle.vel.z = (std::cos(phi) + 1.2f) * speed;

        particle.lifetime = lifetime * (0.6f + rand01() * 0.8f);
        particle.max_lifetime = particle.lifetime;
        particle.color = settings.color;
        particle.size = size * (0.5f + rand01());
        particles_.push_back(particle);
    }
}

void DamageFx::spawn_pulse(const PulseSettings& settings)
{
    pulse_.active = true;
    pulse_.age = 0.0f;
    pulse_.color = settings.color;
    pulse_.duration = std::clamp(settings.duration, 0.02f, 5.0f);
    pulse_.intensity = settings.intensity;
}

void DamageFx::spawn_lightning(const glm::vec3& at, const LightningSettings& settings)
{
    const int strikes = std::max(1, settings.count);
    const float duration = std::clamp(settings.duration, 0.05f, 5.0f);
    for (int s = 0; s < strikes; ++s) {
        Lightning lightning;
        lightning.age = -static_cast<float>(s) * 0.05f;  // stagger start times
        lightning.max_lifetime = duration;
        lightning.color = settings.color;
        lightning.width = settings.width;

        // Distinct horizontal offset so multiple strikes do not overlap.
        const float offset_x = (s == 0) ? 0.0f : (rand01() * 2.0f - 1.0f) * 25.0f;
        const float offset_y = (s == 0) ? 0.0f : (rand01() * 2.0f - 1.0f) * 25.0f;
        const glm::vec3 target = at + glm::vec3(offset_x, offset_y, 0.0f);
        const glm::vec3 start = target + glm::vec3(offset_x * 0.5f, offset_y * 0.5f, 800.0f);

        constexpr int kSegments = 12;
        lightning.points.reserve(kSegments + 1);
        lightning.points.push_back(start);
        for (int i = 1; i < kSegments; ++i) {
            const float t = static_cast<float>(i) / kSegments;
            const glm::vec3 base = start + (target - start) * t;
            const float disp_scale = 50.0f * (1.0f - t);
            const float dx = (rand01() * 2.0f - 1.0f) * disp_scale;
            const float dy = (rand01() * 2.0f - 1.0f) * disp_scale;
            const float dz = (rand01() * 2.0f - 1.0f) * (disp_scale * 0.2f);
            lightning.points.push_back(base + glm::vec3(dx, dy, dz));
        }
        lightning.points.push_back(target);
        push_capped(lightnings_, lightning, kMaxLightnings);
    }
}

void DamageFx::spawn_shockwave(const glm::vec3& at, const ShockwaveSettings& settings)
{
    Shockwave shockwave;
    shockwave.center = at;
    shockwave.age = 0.0f;
    shockwave.max_lifetime = std::clamp(settings.duration, 0.05f, 5.0f);
    shockwave.color = settings.color;
    shockwave.max_radius = settings.max_radius;
    shockwave.width = settings.width;
    push_capped(shockwaves_, shockwave, kMaxShockwaves);
}

void DamageFx::step(float dt, float gravity)
{
    if (!(dt > 0.0f) || !std::isfinite(dt)) return;

    for (auto& particle : particles_) {
        particle.lifetime -= dt;
        particle.vel.z += gravity * dt;
        particle.pos += particle.vel * dt;
    }
    particles_.erase(std::remove_if(particles_.begin(), particles_.end(),
                                    [](const Particle& p) { return p.lifetime <= 0.0f; }),
                     particles_.end());

    for (auto& lightning : lightnings_) lightning.age += dt;
    lightnings_.erase(std::remove_if(lightnings_.begin(), lightnings_.end(),
                                     [](const Lightning& l) { return l.age >= l.max_lifetime; }),
                      lightnings_.end());

    for (auto& shockwave : shockwaves_) shockwave.age += dt;
    shockwaves_.erase(std::remove_if(shockwaves_.begin(), shockwaves_.end(),
                                     [](const Shockwave& s) { return s.age >= s.max_lifetime; }),
                      shockwaves_.end());

    if (pulse_.active) {
        pulse_.age += dt;
        if (pulse_.age >= pulse_.duration) pulse_.active = false;
    }
}

void DamageFx::draw(ImDrawList* draw_list, const float* view_matrix, int width, int height) const
{
    if (!draw_list) return;

    for (const auto& particle : particles_) {
        float sx = 0.0f, sy = 0.0f;
        if (!world_to_screen(particle.pos, &sx, &sy, view_matrix, width, height)) continue;
        const float life_pct = particle.max_lifetime > 0.0f ? particle.lifetime / particle.max_lifetime : 0.0f;
        const float size = particle.size * life_pct;
        if (size > 0.1f) draw_list->AddCircleFilled(ImVec2(sx, sy), size, to_u32(particle.color, life_pct));
    }

    if (pulse_.active && pulse_.duration > 0.0f) {
        const float progress = std::clamp(pulse_.age / pulse_.duration, 0.0f, 1.0f);
        const float alpha = std::pow(1.0f - progress, 1.5f);
        const float max_thickness = 40.0f * pulse_.intensity;
        constexpr int kSteps = 5;
        for (int i = 0; i < kSteps; ++i) {
            const float step_alpha = alpha * (1.0f - static_cast<float>(i) / kSteps);
            const float thickness = max_thickness * (static_cast<float>(i + 1) / kSteps);
            draw_list->AddRect(
                ImVec2(thickness * 0.5f, thickness * 0.5f),
                ImVec2(static_cast<float>(width) - thickness * 0.5f,
                       static_cast<float>(height) - thickness * 0.5f),
                to_u32(pulse_.color, step_alpha * 0.3f), 0.0f, 0, thickness);
        }
    }

    for (const auto& lightning : lightnings_) {
        if (lightning.age < 0.0f) continue;  // staggered strike not active yet
        const float life_pct = 1.0f - static_cast<float>(lightning.age / lightning.max_lifetime);

        float flicker = 1.0f;
        if (std::fmod(lightning.age, 0.08f) < 0.04f) flicker = 0.2f + 0.8f * rand01();

        const Color& color = lightning.color;
        const ImU32 glow_col = to_u32(color, 0.3f * life_pct * flicker);
        const Color core_color{std::min(color.r + 0.5f, 1.0f), std::min(color.g + 0.5f, 1.0f),
                               std::min(color.b + 0.5f, 1.0f), color.a};
        const ImU32 core_col = to_u32(core_color, life_pct * flicker);

        for (std::size_t i = 0; i + 1 < lightning.points.size(); ++i) {
            float sx0 = 0.0f, sy0 = 0.0f, sx1 = 0.0f, sy1 = 0.0f;
            const bool p0_ok = world_to_screen(lightning.points[i], &sx0, &sy0, view_matrix, width, height);
            const bool p1_ok = world_to_screen(lightning.points[i + 1], &sx1, &sy1, view_matrix, width, height);
            if (!p0_ok || !p1_ok) continue;
            draw_list->AddLine(ImVec2(sx0, sy0), ImVec2(sx1, sy1), glow_col, lightning.width * 3.0f);
            draw_list->AddLine(ImVec2(sx0, sy0), ImVec2(sx1, sy1), core_col, lightning.width);
        }
    }

    for (const auto& shockwave : shockwaves_) {
        if (shockwave.age < 0.0f) continue;
        const float progress = std::clamp(shockwave.age / shockwave.max_lifetime, 0.0f, 1.0f);
        const float life_pct = 1.0f - progress;
        const float current_radius = (progress * (2.0f - progress)) * shockwave.max_radius;

        const Color& color = shockwave.color;
        const ImU32 glow_col = to_u32(color, 0.25f * life_pct * life_pct);
        const ImU32 core_col = to_u32(color, life_pct);

        constexpr int kSegments = 36;
        std::vector<ImVec2> projected;
        projected.reserve(kSegments + 1);
        const auto flush = [&]() {
            for (std::size_t k = 0; k + 1 < projected.size(); ++k) {
                draw_list->AddLine(projected[k], projected[k + 1], glow_col, shockwave.width * 2.5f);
                draw_list->AddLine(projected[k], projected[k + 1], core_col, shockwave.width);
            }
            projected.clear();
        };
        for (int i = 0; i <= kSegments; ++i) {
            const float theta = static_cast<float>(i % kSegments) * (2.0f * kPi / kSegments);
            const glm::vec3 point =
                shockwave.center + glm::vec3(std::cos(theta) * current_radius, std::sin(theta) * current_radius, 0.0f);
            float sx = 0.0f, sy = 0.0f;
            if (world_to_screen(point, &sx, &sy, view_matrix, width, height)) {
                projected.push_back(ImVec2(sx, sy));
            } else {
                flush();
            }
        }
        flush();
    }
}

bool DamageFx::empty() const
{
    return particles_.empty() && lightnings_.empty() && shockwaves_.empty() && !pulse_.active;
}

}  // namespace damagefx
