#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

class MenuAnimation {
public:
    static MenuAnimation& get();

    void update(float delta_time);
    void configure(bool enabled, float speed);
    bool enabled() const { return enabled_; }
    float speed_scale() const { return speed_scale_; }
    float animate(std::uint32_t id, float target, float speed = 14.0f);
    float animate_from(std::uint32_t id, float target, float initial, float speed = 14.0f);
    // Force an animated value without interpolation (used to seed a card at its
    // open size so it does not visibly grow in from collapsed).
    void set(std::uint32_t id, float value);
    float animate_key(const std::string& key, float target, float speed = 14.0f);

private:
    float delta_time_ = 1.0f / 60.0f;
    bool enabled_ = true;
    float speed_scale_ = 1.0f;
    std::unordered_map<std::uint32_t, float> id_values_;
    std::unordered_map<std::string, float> key_values_;
};
