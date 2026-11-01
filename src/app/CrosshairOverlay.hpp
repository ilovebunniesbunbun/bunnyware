#pragma once

#include "CrosshairCode.hpp"

#include <string>

struct ImDrawList;
struct OverlayConfig;
struct ShmPacket;

class CrosshairOverlay {
public:
    void render(ImDrawList* draw_list, const ShmPacket& packet,
                OverlayConfig& cfg, int width, int height);

private:
    std::string cached_code_;
    int cached_width_ = 0;
    int cached_height_ = 0;
    int cached_target_width_ = 0;
    int cached_target_height_ = 0;
    float cached_gap_offset_ = 0.0f;
    float cached_size_offset_ = 0.0f;
    float cached_thickness_offset_ = 0.0f;
    crosshair::Geometry cached_geometry_;
    bool debug_was_enabled_ = false;
    std::string last_debug_state_;
};
