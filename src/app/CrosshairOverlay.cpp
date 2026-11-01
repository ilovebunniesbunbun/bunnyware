#include "CrosshairOverlay.hpp"

#include "config.hpp"
#include "external/imgui/imgui.h"
#include "logger.hpp"
#include "overlay/game_reader.hpp"
#include "overlay/esp_drawing.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string_view>

void CrosshairOverlay::render(ImDrawList* draw_list, const ShmPacket& packet,
                              OverlayConfig& cfg, int width, int height)
{
    const bool debug_enabled = cfg.debug_game_reader;
    const bool debug_just_enabled = debug_enabled && !debug_was_enabled_;
    debug_was_enabled_ = debug_enabled;
    if (!debug_enabled) {
        last_debug_state_.clear();
    }

    auto log_state_once = [&](const char* state, const char* message) {
        if (debug_enabled && (debug_just_enabled || last_debug_state_ != state)) {
            BUNNYWARE_LOG_INFO("[CrosshairOverlay] {}", message);
        }
        last_debug_state_ = state;
    };

    if (!draw_list) {
        log_state_once("no_draw_list", "skipped: ImGui background draw list is unavailable");
        return;
    }
    if (!cfg.crosshair_overlay_enabled) {
        log_state_once("disabled", "skipped: feature is disabled");
        return;
    }
    if (!cfg.crosshair_overlay_force) {
        if (packet.local_health <= 0) {
            log_state_once("dead", "skipped: local player is not alive");
            return;
        }
        if (packet.local_is_scoped) {
            log_state_once("scoped", "skipped: local player is scoped");
            return;
        }
    }
    if (width <= 0 || height <= 0) {
        log_state_once("invalid_dimensions", "skipped: overlay dimensions are invalid");
        return;
    }

    const WeaponGroup weapon_group = get_weapon_group(packet.held_weapon_id);
    const bool is_sniper = weapon_group == WEAPON_GROUP_AWP ||
                           weapon_group == WEAPON_GROUP_SCOUT ||
                           weapon_group == WEAPON_GROUP_SNIPER;

    float recoil_x = 0.0f;
    float recoil_y = 0.0f;
    const int source_width =
        packet.game_window_width > 0 ? packet.game_window_width : width;
    const int source_height =
        packet.game_window_height > 0 ? packet.game_window_height : height;

    // A fullscreen overlay is intentionally created one row shorter than the
    // monitor in App::initialize().  That row must not become part of the CS2
    // framebuffer stretch calculation: 1280x960 on a 2560x1440 monitor is an
    // exact 2.0x1.5 stretch, not 2.0x1.498958.  Drawing near screen center is
    // still safely inside the one-row-short overlay drawable.
    int stretch_width = width;
    int stretch_height = height;
    if (cfg.monitor_w == width && cfg.monitor_h == height + 1) {
        stretch_width = cfg.monitor_w;
        stretch_height = cfg.monitor_h;
    }

    const float scale_x =
        static_cast<float>(stretch_width) / static_cast<float>(source_width);
    const float scale_y =
        static_cast<float>(stretch_height) / static_cast<float>(source_height);
    bool should_render = false;
    if (cfg.crosshair_overlay_force) {
        should_render = true;
    } else if (is_sniper && cfg.crosshair_overlay_snipers_enabled) {
        should_render = true;
    } else if (cfg.crosshair_overlay_recoil_enabled &&
               is_shootable_weapon(packet.held_weapon_id)) {
        should_render = true;
        if (cfg.crosshair_overlay_only_shooting &&
            packet.local_shots_fired <= cfg.crosshair_overlay_shot_delay) {
            log_state_once("shot_delay", "skipped: waiting for configured shot count");
            return;
        }

        if (packet.local_aim_punch.x != 0.0f || packet.local_aim_punch.y != 0.0f) {
            constexpr float kPi = 3.14159265358979323846f;
            constexpr float kBaseFovRadians = 73.74f * kPi / 180.0f;
            const float pixels_per_degree =
                (static_cast<float>(source_height) * kPi) /
                (360.0f * std::tan(kBaseFovRadians / 2.0f));
            recoil_x = -packet.local_aim_punch.y * pixels_per_degree;
            recoil_y = packet.local_aim_punch.x * pixels_per_degree;
        }
    }

    if (!should_render) {
        log_state_once("weapon_ineligible",
                       "skipped: weapon is not an enabled sniper and recoil mode is inactive");
        return;
    }
    last_debug_state_ = "active";

    const size_t code_length =
        strnlen(packet.local_crosshair_code, sizeof(packet.local_crosshair_code));
    const std::string_view code(packet.local_crosshair_code, code_length);

    if (code != cached_code_ || source_width != cached_width_ ||
        source_height != cached_height_ ||
        stretch_width != cached_target_width_ ||
        stretch_height != cached_target_height_ ||
        cfg.crosshair_overlay_gap != cached_gap_offset_ ||
        cfg.crosshair_overlay_size != cached_size_offset_ ||
        cfg.crosshair_overlay_thickness != cached_thickness_offset_ ||
        debug_just_enabled) {
        const bool code_changed = (code != cached_code_);
        cached_code_.assign(code);
        cached_width_ = source_width;
        cached_height_ = source_height;
        cached_target_width_ = stretch_width;
        cached_target_height_ = stretch_height;
        cached_gap_offset_ = cfg.crosshair_overlay_gap;
        cached_size_offset_ = cfg.crosshair_overlay_size;
        cached_thickness_offset_ = cfg.crosshair_overlay_thickness;

        std::optional<crosshair::Settings> settings = crosshair::decode(code);
        if (settings) {
            settings->gap += cfg.crosshair_overlay_gap;
            settings->size = std::max(0.0f, settings->size + cfg.crosshair_overlay_size);
            settings->thickness = std::max(0.0f, settings->thickness + cfg.crosshair_overlay_thickness);
        }
        cached_geometry_ = settings
            ? crosshair::build_geometry(*settings, source_width, source_height)
            : crosshair::Geometry{};

        if (code_changed) {
            const auto& c = cached_geometry_.color;
            cfg.crosshair_overlay_color = Color(
                static_cast<float>(c[0]) / 255.0f,
                static_cast<float>(c[1]) / 255.0f,
                static_cast<float>(c[2]) / 255.0f,
                static_cast<float>(c[3]) / 255.0f
            );
        }

        if (debug_enabled) {
            BUNNYWARE_LOG_INFO(
                "[CrosshairOverlay] packet code='{}' length={} game_window={}x{} "
                "overlay_drawable={}x{} stretch_target={}x{} scale=({:.4f},{:.4f}) "
                "weapon_id={} health={} scoped={} shots={} aim_punch=({:.3f},{:.3f})",
                code, code_length, source_width, source_height, width, height,
                stretch_width, stretch_height, scale_x, scale_y, packet.held_weapon_id,
                packet.local_health, packet.local_is_scoped, packet.local_shots_fired,
                packet.local_aim_punch.x, packet.local_aim_punch.y);
            if (settings) {
                BUNNYWARE_LOG_INFO(
                    "[CrosshairOverlay] decoded style={} size={:.2f} thickness={:.2f} "
                    "gap={:.2f} dot={} t_style={} outline={} outline_thickness={:.2f} "
                    "use_alpha={} rgba=({},{},{},{}) rectangles={}",
                    settings->style, settings->size, settings->thickness,
                    settings->gap, settings->dot, settings->t_style,
                    settings->draw_outline, settings->outline_thickness,
                    settings->use_alpha, settings->red, settings->green, settings->blue,
                    settings->alpha,
                    cached_geometry_.rectangles.size());
            } else {
                BUNNYWARE_LOG_INFO(
                    "[CrosshairOverlay] decode failed: share code is empty, malformed, "
                    "uses an invalid character, or has a checksum mismatch");
            }
        }

        const bool has_visible_rectangle =
            std::any_of(cached_geometry_.rectangles.begin(),
                        cached_geometry_.rectangles.end(),
                        [](const crosshair::Rect& rectangle) {
                            return rectangle.x2 > rectangle.x1 &&
                                   rectangle.y2 > rectangle.y1;
                        });
        if (!has_visible_rectangle || cached_geometry_.color[3] == 0) {
            if (debug_enabled) {
                const char* reason = !settings
                    ? "decode failed"
                    : (!has_visible_rectangle ? "decoded rectangles have zero area"
                                              : "decoded alpha is zero");
                BUNNYWARE_LOG_INFO(
                    "[CrosshairOverlay] using white fallback dot: {}", reason);
            }
            const int center_x = source_width / 2;
            const int center_y = source_height / 2;
            cached_geometry_.color = {255, 255, 255, 255};
            cached_geometry_.rectangles = {
                {center_x - 2, center_y - 2, center_x + 2, center_y + 2},
            };
        }

        if (debug_enabled) {
            const int center_x = source_width / 2;
            const int center_y = source_height / 2;
            for (size_t index = 0; index < cached_geometry_.rectangles.size(); ++index) {
                const crosshair::Rect& rectangle =
                    cached_geometry_.rectangles[index];
                const int destination_x1 = static_cast<int>(std::round(
                    static_cast<float>(rectangle.x1) * scale_x +
                    cfg.crosshair_overlay_offset_x));
                const int destination_y1 = static_cast<int>(std::round(
                    static_cast<float>(rectangle.y1) * scale_y +
                    cfg.crosshair_overlay_offset_y));
                const int destination_x2 = static_cast<int>(std::round(
                    static_cast<float>(rectangle.x2) * scale_x +
                    cfg.crosshair_overlay_offset_x));
                const int destination_y2 = static_cast<int>(std::round(
                    static_cast<float>(rectangle.y2) * scale_y +
                    cfg.crosshair_overlay_offset_y));
                BUNNYWARE_LOG_INFO(
                    "[CrosshairOverlay] render rect[{}] source=({},{})->({},{}) "
                    "local=({},{})->({},{}) size={}x{} destination=({},{})->({},{}) "
                    "size={}x{}",
                    index, rectangle.x1, rectangle.y1, rectangle.x2, rectangle.y2,
                    rectangle.x1 - center_x, rectangle.y1 - center_y,
                    rectangle.x2 - center_x, rectangle.y2 - center_y,
                    rectangle.x2 - rectangle.x1, rectangle.y2 - rectangle.y1,
                    destination_x1, destination_y1, destination_x2, destination_y2,
                    destination_x2 - destination_x1,
                    destination_y2 - destination_y1);
            }
        }
    }

    Color render_color = cfg.crosshair_overlay_color;

    if (cfg.crosshair_esp_enabled && cfg.crosshair_esp_fov > 0.0f) {
        constexpr float deg2rad = 3.14159265358979323846f / 180.0f;
        Vec3 forward(packet.view_matrix[2], packet.view_matrix[6], packet.view_matrix[10]);
        Vec3 right(packet.view_matrix[0], packet.view_matrix[4], packet.view_matrix[8]);
        float fov_rad = cfg.crosshair_esp_fov * deg2rad;
        Vec3 eye = packet.local_eye;

        Vec3 offset_world = eye + (forward * std::cos(fov_rad) + right * std::sin(fov_rad)) * 100.0f;
        float screen_cx = 0.0f, screen_cy = 0.0f;
        float screen_ox = 0.0f, screen_oy = 0.0f;
        float max_radius_px = 0.0f;

        if (world_to_screen(eye + forward * 100.0f, &screen_cx, &screen_cy, packet.view_matrix, source_width, source_height) &&
            world_to_screen(offset_world, &screen_ox, &screen_oy, packet.view_matrix, source_width, source_height)) {
            float r_dx = (screen_ox - screen_cx) * scale_x;
            float r_dy = (screen_oy - screen_cy) * scale_y;
            max_radius_px = std::sqrt(r_dx * r_dx + r_dy * r_dy);
        }

        if (max_radius_px <= 0.0f) {
            max_radius_px = (cfg.crosshair_esp_fov * static_cast<float>(height)) / 60.0f;
        }

        float center_x = static_cast<float>(width) * 0.5f + cfg.crosshair_overlay_offset_x;
        float center_y = static_cast<float>(height) * 0.5f + cfg.crosshair_overlay_offset_y;
        float min_dist_sq = max_radius_px * max_radius_px;
        bool found_enemy = false;

        for (int i = 0; i < packet.player_count; ++i) {
            const PlayerData& p = packet.players[i];
            if (!p.active || p.health <= 0 || p.team == packet.local_team) {
                continue;
            }

            Vec3 target_pos = p.origin + Vec3(0.0f, 0.0f, 40.0f);
            if (p.bone_count > 7) {
                target_pos = p.bones[7].position;
            }

            float sx = 0.0f, sy = 0.0f;
            if (world_to_screen(target_pos, &sx, &sy, packet.view_matrix, source_width, source_height)) {
                float dest_sx = sx * scale_x;
                float dest_sy = sy * scale_y;
                float dx = dest_sx - center_x;
                float dy = dest_sy - center_y;
                float dist_sq = dx * dx + dy * dy;
                if (dist_sq < min_dist_sq) {
                    min_dist_sq = dist_sq;
                    found_enemy = true;
                }
            }
        }

        if (found_enemy) {
            float min_dist = std::sqrt(min_dist_sq);
            float t = std::clamp(1.0f - (min_dist / max_radius_px), 0.0f, 1.0f);
            render_color.r = cfg.crosshair_overlay_color.r + (cfg.crosshair_esp_color.r - cfg.crosshair_overlay_color.r) * t;
            render_color.g = cfg.crosshair_overlay_color.g + (cfg.crosshair_esp_color.g - cfg.crosshair_overlay_color.g) * t;
            render_color.b = cfg.crosshair_overlay_color.b + (cfg.crosshair_esp_color.b - cfg.crosshair_overlay_color.b) * t;
            render_color.a = cfg.crosshair_overlay_color.a + (cfg.crosshair_esp_color.a - cfg.crosshair_overlay_color.a) * t;
        }
    }

    const ImU32 draw_color = IM_COL32(
        static_cast<int>(std::clamp(render_color.r, 0.0f, 1.0f) * 255.0f),
        static_cast<int>(std::clamp(render_color.g, 0.0f, 1.0f) * 255.0f),
        static_cast<int>(std::clamp(render_color.b, 0.0f, 1.0f) * 255.0f),
        static_cast<int>(std::clamp(render_color.a, 0.0f, 1.0f) * 255.0f)
    );
    for (const crosshair::Rect& rectangle : cached_geometry_.rectangles) {
        float x1 = std::round(
            (static_cast<float>(rectangle.x1) + recoil_x) * scale_x +
                                    cfg.crosshair_overlay_offset_x);
        float y1 = std::round(
            (static_cast<float>(rectangle.y1) + recoil_y) * scale_y +
                                    cfg.crosshair_overlay_offset_y);
        float x2 = std::round(
            (static_cast<float>(rectangle.x2) + recoil_x) * scale_x +
                                    cfg.crosshair_overlay_offset_x);
        float y2 = std::round(
            (static_cast<float>(rectangle.y2) + recoil_y) * scale_y +
                                    cfg.crosshair_overlay_offset_y);
        if (x2 <= x1) x2 = x1 + 1.0f;
        if (y2 <= y1) y2 = y1 + 1.0f;
        draw_list->AddRectFilled(ImVec2(x1, y1), ImVec2(x2, y2), draw_color);
    }
}
