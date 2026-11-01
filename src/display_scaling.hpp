#pragma once

#include <cmath>
#include <cstdint>
#include <limits>
#include <string_view>

// Rendering and ImGui coordinates are physical pixels. Native window/surface
// sizes and native pointer positions remain logical until this boundary.
namespace display_scaling {
struct FramebufferRatios {
    double x = 1.0;
    double y = 1.0;
};

inline bool valid_dimensions(int width, int height) {
    return width > 0 && height > 0;
}

inline FramebufferRatios framebuffer_ratios(int logical_width, int logical_height,
                                            int framebuffer_width, int framebuffer_height) {
    if (!valid_dimensions(logical_width, logical_height) ||
        !valid_dimensions(framebuffer_width, framebuffer_height)) return {};
    return {static_cast<double>(framebuffer_width) / logical_width,
            static_cast<double>(framebuffer_height) / logical_height};
}

inline double logical_to_physical(double value, double ratio) {
    return value * ratio;
}

inline double physical_to_logical(double value, double ratio) {
    return ratio > 0.0 ? value / ratio : value;
}

inline int rounded_pixel_extent(double extent) {
    if (!(extent > 0.0 && extent <= std::numeric_limits<int>::max() - 0.5)) return 0;
    return static_cast<int>(std::fmax(1.0, std::round(extent)));
}

struct WaylandExtent {
    int buffer_width = 0;
    int buffer_height = 0;
    int source_width_fixed = 0;
    int source_height_fixed = 0;

    bool valid() const { return valid_dimensions(buffer_width, buffer_height); }
};

inline WaylandExtent wayland_extent(int logical_width, int logical_height, uint32_t scale_120) {
    if (!valid_dimensions(logical_width, logical_height) || scale_120 == 0) return {};
    // Work in 64-bit integers: round the exact scale to protocol 24.8 fixed
    // point, then allocate enough pixels to contain that source rectangle.
    const uint64_t width_scaled = static_cast<uint64_t>(logical_width) * scale_120;
    const uint64_t height_scaled = static_cast<uint64_t>(logical_height) * scale_120;
    constexpr uint64_t max_scaled = static_cast<uint64_t>(std::numeric_limits<int>::max()) * 120 / 256;
    if (width_scaled > max_scaled || height_scaled > max_scaled) return {};
    const int width_fixed = static_cast<int>((width_scaled * 256 + 60) / 120);
    const int height_fixed = static_cast<int>((height_scaled * 256 + 60) / 120);
    if (width_fixed == 0 || height_fixed == 0) return {};
    return {static_cast<int>((static_cast<int64_t>(width_fixed) + 255) / 256),
            static_cast<int>((static_cast<int64_t>(height_fixed) + 255) / 256),
            width_fixed, height_fixed};
}
struct OverlayGeometry {
    int width = 0;
    int height = 0;
    int x = 0;
    int y = 0;
};

inline OverlayGeometry overlay_geometry(int monitor_width, int monitor_height, std::string_view mode,
                                         int game_width, int game_height, int game_x, int game_y) {
    if (!valid_dimensions(monitor_width, monitor_height)) return {};
    if (mode == "custom" && valid_dimensions(game_width, game_height)) {
        return {game_width, game_height, game_x, game_y};
    }
    if (mode == "centered" && valid_dimensions(game_width, game_height)) {
        const double game_aspect = static_cast<double>(game_width) / game_height;
        int width = monitor_width;
        int height = monitor_height;
        if (game_aspect < static_cast<double>(monitor_width) / monitor_height) {
            width = static_cast<int>(monitor_height * game_aspect);
        } else {
            height = static_cast<int>(monitor_width / game_aspect);
        }
        return {width, height, (monitor_width - width) / 2, (monitor_height - height) / 2};
    }
    return {monitor_width, monitor_height, 0, 0};
}
} // namespace display_scaling
