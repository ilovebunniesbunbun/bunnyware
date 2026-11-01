#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace crosshair {

struct Settings {
    float gap = 0.0f;
    float outline_thickness = 0.0f;
    uint8_t red = 0;
    uint8_t green = 0;
    uint8_t blue = 0;
    uint8_t alpha = 0;
    uint8_t dynamic_split_distance = 0;
    bool recoil = false;
    float fixed_gap = 0.0f;
    uint8_t color_index = 0;
    bool draw_outline = false;
    float dynamic_split_alpha_inner = 0.0f;
    float dynamic_split_alpha_outer = 0.0f;
    float dynamic_max_distance_split_ratio = 0.0f;
    float thickness = 0.0f;
    uint8_t style = 0;
    bool dot = false;
    bool gap_uses_weapon_value = false;
    bool use_alpha = false;
    bool t_style = false;
    float size = 0.0f;
};

struct Rect {
    int x1 = 0;
    int y1 = 0;
    int x2 = 0;
    int y2 = 0;
};

struct Geometry {
    std::array<uint8_t, 4> color{};
    std::vector<Rect> rectangles;
};

std::optional<Settings> decode(std::string_view code);
Geometry build_geometry(const Settings& settings, int width, int height);

} // namespace crosshair
