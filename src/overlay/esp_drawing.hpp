#pragma once
#include <vector>
#include <cmath>
#include <algorithm>
#include "overlay/game_reader.hpp"
#include "config.hpp"
#include "math/Distance.hpp"

inline bool world_to_screen(const Vec3& world, float* screen_x, float* screen_y, const float* matrix, int width, int height) {
    float w = matrix[12] * world.x + matrix[13] * world.y + matrix[14] * world.z + matrix[15];
    if (w < 0.001f) return false;
    float x = matrix[0] * world.x + matrix[1] * world.y + matrix[2] * world.z + matrix[3];
    float y = matrix[4] * world.x + matrix[5] * world.y + matrix[6] * world.z + matrix[7];

    float inv_w = 1.0f / w;
    x *= inv_w;
    y *= inv_w;

    *screen_x = (width * 0.5f) + (x * width * 0.5f);
    *screen_y = (height * 0.5f) - (y * height * 0.5f);
    return true;
}

/**
 * @name is_beyond_esp_distance
 * @description True when a distance exceeds the configured ESP max distance
 * in meters. Distance limiting is off (unlimited) unless the ESP "Limit
 * Distance" toggle is enabled; the override key also disables the limit.
 *
 * @example
 * if (is_beyond_esp_distance(distance_meters, cfg)) continue;
 */
inline bool is_beyond_esp_distance(float distance_meters, const OverlayConfig& cfg)
{
    if (!cfg.esp_distance_limit_enabled || cfg.esp_distance_override || cfg.esp_max_distance <= 0.0f) return false;
    return distance_meters > cfg.esp_max_distance;
}

inline bool is_beyond_esp_distance(const PlayerData& player, const ShmPacket& packet, const OverlayConfig& cfg)
{
    return is_beyond_esp_distance(distance::meters_between(player.origin, packet.local_eye), cfg);
}

/**
 * @name esp_distance_alpha
 * @description Smooth distance opacity multiplier for player ESP visuals.
 *
 * @example
 * float alpha = esp_distance_alpha(distance_meters, cfg);
 */
inline float esp_distance_alpha(float distance_meters, const OverlayConfig& cfg)
{
    if (!cfg.esp_distance_limit_enabled || cfg.esp_distance_override || !cfg.esp_distance_fade_enabled ||
        cfg.esp_max_distance <= 0.0f)
        return 1.0f;

    const float cutoff = std::max(cfg.esp_max_distance, 0.001f);
    const float fade_start = std::clamp(
        cfg.esp_distance_fade_start > 0.0f ? cfg.esp_distance_fade_start : cutoff * 0.8f,
        0.0f,
        cutoff);
    const float fade_span = cutoff - fade_start;
    if (fade_span <= 0.001f || distance_meters <= fade_start) return 1.0f;

    const float progress = std::clamp((distance_meters - fade_start) / fade_span, 0.0f, 1.0f);
    const float eased = progress * progress * (3.0f - 2.0f * progress);
    const float min_alpha = std::clamp(cfg.esp_distance_fade_min_alpha, 0.0f, 1.0f);
    return 1.0f + (min_alpha - 1.0f) * eased;
}

inline float esp_distance_alpha(const PlayerData& player, const ShmPacket& packet, const OverlayConfig& cfg)
{
    return esp_distance_alpha(distance::meters_between(player.origin, packet.local_eye), cfg);
}

inline constexpr float HEAD_RADIUS_UNITS = 4.5f;

/**
 * @name head_screen_radius
 * @description Projects a sphere of the real head size onto the screen and
 * returns its radius in pixels, or 0 when it cannot be projected.
 *
 * @example
 * float r = head_screen_radius(head, packet.local_angles, view_matrix, width, height);
 */
inline float head_screen_radius(const Vec3& head, const Vec3& view_angles, const float* matrix, int width, int height) {
    const glm::vec3 fwd = distance::view_forward(view_angles);
    glm::vec3 right = glm::cross(fwd, glm::vec3(0.0f, 0.0f, 1.0f));
    const float len = glm::length(right);
    right = len > 1e-4f ? right / len : glm::vec3(0.0f, 1.0f, 0.0f);

    float cx = 0.0f, cy = 0.0f, ex = 0.0f, ey = 0.0f;
    if (!world_to_screen(head, &cx, &cy, matrix, width, height)) return 0.0f;
    if (!world_to_screen(head + right * HEAD_RADIUS_UNITS, &ex, &ey, matrix, width, height)) return 0.0f;
    return std::hypot(ex - cx, ey - cy);
}

inline bool get_player_bounds(const std::vector<Vec3>& sanitized_positions, const Vec3& origin, const Vec3& cam_pos, const float* matrix, int width, int height, float& box_x, float& box_y, float& box_w, float& box_h, const OverlayConfig& cfg) {
    int active_mode = cfg.esp_box_mode;

    if (active_mode == 2) {
        Vec3 delta = origin - cam_pos;
        float dist = std::sqrt(delta.x*delta.x + delta.y*delta.y + delta.z*delta.z);
        if (dist < 10.0f) dist = 10.0f;

        float bw = cfg.esp_box_static_w / dist;
        float bh = cfg.esp_box_static_h / dist;

        float sx, sy;
        if (world_to_screen(origin, &sx, &sy, matrix, width, height)) {
            box_x = sx - bw * 0.5f;
            box_y = sy - bh;
            box_w = bw;
            box_h = bh;
            return true;
        }
        return false;
    }

    if (active_mode == 1) {
        if (7 >= static_cast<int>(sanitized_positions.size())) return false;
        const Vec3& head_pos = sanitized_positions[7];

        Vec3 top_pos = head_pos;
        top_pos.z += 6.0f;

        float feet_x, feet_y;
        float head_x, head_y;
        if (!world_to_screen(origin, &feet_x, &feet_y, matrix, width, height)) return false;
        if (!world_to_screen(top_pos, &head_x, &head_y, matrix, width, height)) return false;

        float h = feet_y - head_y;
        if (h < 2.0f) return false;

        float w = h * 0.54f;
        float center_x = (feet_x + head_x) * 0.5f;

        box_x = center_x - w * 0.5f;
        box_y = head_y;
        box_w = w;
        box_h = h;
        return true;
    }

    float min_x = 99999.0f;
    float max_x = -99999.0f;
    float min_y = 99999.0f;
    float max_y = -99999.0f;
    bool any_visible = false;

    const int bones_to_check[] = {7, 6, 23, 1, 10, 11, 14, 15, 18, 19, 21, 22};
    for (int b : bones_to_check) {
        if (b >= static_cast<int>(sanitized_positions.size())) continue;
        float sx, sy;
        if (world_to_screen(sanitized_positions[b], &sx, &sy, matrix, width, height)) {
            if (sx < min_x) min_x = sx;
            if (sx > max_x) max_x = sx;
            if (sy < min_y) min_y = sy;
            if (sy > max_y) max_y = sy;
            any_visible = true;
        }
    }

    if (!any_visible) return false;

    float pad_w = (max_x - min_x) * 0.15f;
    float pad_h = (max_y - min_y) * 0.10f;

    if (pad_w < 5.0f) pad_w = 5.0f;
    if (pad_h < 5.0f) pad_h = 5.0f;

    box_x = min_x - pad_w;
    box_y = min_y - pad_h;
    box_w = (max_x - min_x) + 2.0f * pad_w;
    box_h = (max_y - min_y) + 2.0f * pad_h;
    return true;
}
