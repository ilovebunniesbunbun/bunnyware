#include "menu_animation.hpp"

#include <algorithm>
#include <cmath>

namespace {
float interpolate(float current, float target, float delta_time, float speed)
{
    const float amount = std::min(1.0f, delta_time * speed);
    const float next = current + (target - current) * amount;
    return std::abs(next - target) < 0.0001f ? target : next;
}
}  // namespace

MenuAnimation& MenuAnimation::get()
{
    static MenuAnimation instance;
    return instance;
}

void MenuAnimation::update(float delta_time)
{
    delta_time_ = delta_time <= 0.0001f ? 1.0f / 60.0f : std::min(delta_time, 0.05f);
}

void MenuAnimation::configure(bool enabled, float speed)
{
    enabled_ = enabled;
    speed_scale_ = std::clamp(speed, 4.0f, 30.0f) / 16.0f;
}

float MenuAnimation::animate(std::uint32_t id, float target, float speed)
{
    auto [it, inserted] = id_values_.try_emplace(id, target);
    if (!enabled_) {
        it->second = target;
    } else if (!inserted) {
        it->second = interpolate(it->second, target, delta_time_, speed * speed_scale_);
    }
    return it->second;
}

float MenuAnimation::animate_from(std::uint32_t id, float target, float initial, float speed)
{
    auto [it, inserted] = id_values_.try_emplace(id, initial);
    if (!enabled_) {
        it->second = target;
    } else if (!inserted) {
        it->second = interpolate(it->second, target, delta_time_, speed * speed_scale_);
    }
    return it->second;
}

void MenuAnimation::set(std::uint32_t id, float value)
{
    id_values_[id] = value;
}

float MenuAnimation::animate_key(const std::string& key, float target, float speed)
{
    auto [it, inserted] = key_values_.try_emplace(key, target);
    if (!enabled_) {
        it->second = target;
    } else if (!inserted) {
        it->second = interpolate(it->second, target, delta_time_, speed * speed_scale_);
    }
    return it->second;
}
