#include "GrenadeHelperRenderer.hpp"
#include "menu/menu_theme.hpp"
#include "overlay/esp_drawing.hpp"
#include "external/imgui/imgui.h"
#include <GL/glew.h>
#include <cmath>
#include <algorithm>

namespace {
    // Static configuration constants for layout and distance checking
    constexpr float MAX_VERTICAL_DISTANCE = 80.0f;          // Ignore lineups on different levels/floors
    constexpr float FLOATING_BADGE_Y_OFFSET = 40.0f;        // Offset of the floating badge above the lineup point
    constexpr float CIRCLE_ACTIVATION_DISTANCE = 37.5f;     // Radius at which the precise ground standing circle is drawn
    constexpr float GRENADE_STAND_CIRCLE_RADIUS = 1.025f;   // Physical radius of the player standing spot circle
    constexpr float MIN_TRANSITION_RANGE = 50.0f;           // Minimum transition distance range for UI fades/animations
    constexpr float BADGE_RADIUS = 18.0f;                  // Radius of the floating lineup badge
    constexpr float VISCHECK_BODY_HEIGHT = 36.0f;          // Body-centre height used when tracing to a lineup's ground spot

    inline ImU32 imcol32_from_color(const Color& c, float alpha_factor = 1.0f) {
        return IM_COL32(
            static_cast<int>(c.r * 255.0f),
            static_cast<int>(c.g * 255.0f),
            static_cast<int>(c.b * 255.0f),
            static_cast<int>(c.a * 255.0f * alpha_factor)
        );
    }

    // Menu panel colours, faded for the overlay. Mirrors the helper used by the
    // keybind/spectator boxes in App.cpp so the lineup panels match the menu.
    inline ImU32 fade_theme_color(const ImVec4& color, float alpha) {
        return ImGui::ColorConvertFloat4ToU32(ImVec4(color.x, color.y, color.z, color.w * alpha));
    }

    inline Color get_grenade_color(uint8_t type, const OverlayConfig& cfg) {
        switch (type) {
            case GRENADE_HE: return cfg.grenade_color_he;
            case GRENADE_FLASH: return cfg.grenade_color_flash;
            case GRENADE_SMOKE: return cfg.grenade_color_smoke;
            case GRENADE_MOLOTOV: return cfg.grenade_color_molotov;
            case GRENADE_DECOY: return cfg.grenade_color_decoy;
            default: return Color(0.67f, 0.69f, 0.86f, 0.8f);
        }
    }

    inline ImVec2 calc_scaled_text_size(ImFont* font, float font_size, const std::string& text) {
        if (!font) return ImVec2(0.0f, 0.0f);
        return font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, text.c_str(), text.c_str() + text.size());
    }

    void draw_grouped_badge(ImDrawList* draw_list, float sx, float sy, uint8_t grenade_type, const std::vector<std::string>& titles, ImU32 badge_color, float fade_alpha, const SvgCache& svg_cache, float progress, float scale, float& out_box_height_half) {
        float radius = std::round(BADGE_RADIUS * scale);
        const float rounding = MenuTheme::panel_rounding * scale;
        
        ImU32 bg_col = fade_theme_color(MenuTheme::bg_card, 0.85f * fade_alpha);
        ImU32 radial_bg_col = fade_theme_color(MenuTheme::bg_main, 0.85f * fade_alpha);
        
        uint32_t badge_a = (badge_color >> 24) & 0xFF;
        ImU32 arc_col = (badge_color & 0x00FFFFFF) | (static_cast<uint32_t>(badge_a * fade_alpha) << 24);

        float square_width = radius * 2.0f;
        float rect_width = square_width;

        std::vector<ImVec2> text_sizes;
        float max_text_width = 0.0f;
        float total_text_height = 0.0f;
        float padding_left = std::round(4.0f * scale);
        float padding_right = std::round(8.0f * scale);
        float line_spacing = std::round(4.0f * scale);

        bool has_text = !titles.empty() && progress >= 0.99f;

        if (has_text) {
            for (const auto& title : titles) {
                ImVec2 t_size = calc_scaled_text_size(ImGui::GetFont(), 10.0f * scale, title);
                t_size.x = std::round(t_size.x);
                t_size.y = std::round(t_size.y);
                text_sizes.push_back(t_size);
                if (t_size.x > max_text_width) {
                    max_text_width = t_size.x;
                }
                total_text_height += t_size.y;
            }
            if (!titles.empty()) {
                total_text_height += line_spacing * (titles.size() - 1);
            }
            rect_width = radius * 2.0f + padding_left + max_text_width + padding_right;
        }

        float box_width = std::round(square_width + (rect_width - square_width) * progress);

        // Adjust badge height if there are multiple lines of text
        float box_height_half = radius;
        if (has_text && titles.size() > 1) {
            float padding_y = std::round(6.0f * scale);
            float needed_height = total_text_height + padding_y * 2.0f;
            if (needed_height > radius * 2.0f) {
                box_height_half = std::round(needed_height * 0.5f);
            }
        }
        out_box_height_half = box_height_half;

        float left = std::round(sx - radius);
        float right = std::round(sx - radius + box_width);
        float top = std::round(sy - box_height_half);
        float bottom = std::round(sy + box_height_half);
        
        draw_list->AddRectFilled(ImVec2(left, top), ImVec2(right, bottom), bg_col, rounding);
        draw_list->AddRectFilled(ImVec2(left + 2.0f, top + 2.0f), ImVec2(right - 2.0f, bottom - 2.0f), radial_bg_col,
                                 std::max(0.0f, rounding - 2.0f));
        
        SvgTexture tex = svg_cache.get_texture(grenade_type);
        if (tex.id != 0) {
            float max_dim = radius;
            float scale_icon = max_dim / std::max(tex.width, tex.height);
            float icon_w = std::round(tex.width * scale_icon);
            float icon_h = std::round(tex.height * scale_icon);

            ImVec2 icon_min(std::round(sx - icon_w * 0.5f), std::round(sy - icon_h * 0.5f));
            ImVec2 icon_max(std::round(sx + icon_w * 0.5f), std::round(sy + icon_h * 0.5f));
            draw_list->AddImage((ImTextureID)(intptr_t)tex.id, icon_min, icon_max, ImVec2(0,0), ImVec2(1,1), IM_COL32(255,255,255, static_cast<int>(255.0f * fade_alpha)));
        }
        
        draw_list->AddRect(ImVec2(left, top), ImVec2(right, bottom), arc_col, rounding, 0, std::round(1.0f * scale));
        
        if (has_text) {
            float start_y = sy - total_text_height * 0.5f;
            ImU32 text_shadow_col = IM_COL32(0, 0, 0, static_cast<int>(255.0f * fade_alpha));
            ImU32 text_col = IM_COL32(255, 255, 255, static_cast<int>(255.0f * fade_alpha));
            float shadow_offset = std::round(1.0f * scale);
            if (shadow_offset < 1.0f) shadow_offset = 1.0f;

            for (size_t i = 0; i < titles.size(); ++i) {
                ImVec2 text_pos(std::round(sx + radius + padding_left), std::round(start_y));
                draw_list->AddText(ImGui::GetFont(), 10.0f * scale, ImVec2(text_pos.x + shadow_offset, text_pos.y + shadow_offset), text_shadow_col, titles[i].c_str());
                draw_list->AddText(ImGui::GetFont(), 10.0f * scale, text_pos, text_col, titles[i].c_str());
                start_y += text_sizes[i].y + line_spacing;
            }
        }
    }

    void draw_world_circle(ImDrawList* draw_list, const Vec3& center, float radius, ImU32 color, float thickness, const float* view_matrix, float screen_w, float screen_h, bool filled) {
        const int num_segments = 32;
        std::vector<ImVec2> points;
        points.reserve(num_segments);
        for (int i = 0; i < num_segments; ++i) {
            float angle = i * (2.0f * 3.14159265f) / num_segments;
            Vec3 world_pt = {
                center.x + radius * std::cos(angle),
                center.y + radius * std::sin(angle),
                center.z
            };
            float sx = 0.0f, sy = 0.0f;
            if (world_to_screen(world_pt, &sx, &sy, view_matrix, screen_w, screen_h)) {
                points.push_back(ImVec2(std::round(sx), std::round(sy)));
            }
        }
        if (points.size() >= 3) {
            if (filled) {
                ImU32 fill_color = (color & 0x00FFFFFF) | 0x20000000;
                draw_list->AddConvexPolyFilled(points.data(), points.size(), fill_color);
            }
            draw_list->AddPolyline(points.data(), points.size(), color, ImDrawFlags_Closed, thickness);
        }
    }

    void draw_world_2d_box(ImDrawList* draw_list, const glm::vec3& p0, const glm::vec3& p1, ImU32 color, float thickness, const float* view_matrix, float screen_w, float screen_h) {
        glm::vec3 corners[4] = {
            p0,
            {p1.x, p0.y, p0.z},
            {p1.x, p1.y, p0.z},
            {p0.x, p1.y, p0.z}
        };

        ImVec2 screen_corners[4];
        bool valid[4];

        for (int i = 0; i < 4; ++i) {
            float sx, sy;
            valid[i] = world_to_screen({corners[i].x, corners[i].y, corners[i].z}, &sx, &sy, view_matrix, screen_w, screen_h);
            screen_corners[i] = ImVec2(std::round(sx), std::round(sy));
        }

        int edges[4][2] = {
            {0, 1}, {1, 2}, {2, 3}, {3, 0}
        };

        for (int i = 0; i < 4; ++i) {
            int u = edges[i][0];
            int v = edges[i][1];
            if (valid[u] && valid[v]) {
                draw_list->AddLine(screen_corners[u], screen_corners[v], color, thickness);
            }
        }
    }

    void draw_world_aabb(ImDrawList* draw_list, const glm::vec3& mins, const glm::vec3& maxs, ImU32 color, float thickness, const float* view_matrix, float screen_w, float screen_h) {
        glm::vec3 corners[8] = {
            {mins.x, mins.y, mins.z},
            {maxs.x, mins.y, mins.z},
            {maxs.x, maxs.y, mins.z},
            {mins.x, maxs.y, mins.z},
            {mins.x, mins.y, maxs.z},
            {maxs.x, mins.y, maxs.z},
            {maxs.x, maxs.y, maxs.z},
            {mins.x, maxs.y, maxs.z}
        };

        ImVec2 screen_corners[8];
        bool valid[8];

        for (int i = 0; i < 8; ++i) {
            float sx, sy;
            valid[i] = world_to_screen({corners[i].x, corners[i].y, corners[i].z}, &sx, &sy, view_matrix, screen_w, screen_h);
            screen_corners[i] = ImVec2(std::round(sx), std::round(sy));
        }

        int edges[12][2] = {
            {0, 1}, {1, 2}, {2, 3}, {3, 0}, // Bottom face
            {4, 5}, {5, 6}, {6, 7}, {7, 4}, // Top face
            {0, 4}, {1, 5}, {2, 6}, {3, 7}  // Vertical pillars
        };

        for (int i = 0; i < 12; ++i) {
            int u = edges[i][0];
            int v = edges[i][1];
            if (valid[u] && valid[v]) {
                draw_list->AddLine(screen_corners[u], screen_corners[v], color, thickness);
            }
        }
    }
}

void GrenadeHelperRenderer::render(const std::vector<GrenadeLineup>& lineups, 
                                   const VischeckResult& vis_result,
                                   LandingZoneManager& zone_manager,
                                   const std::string& current_map,
                                   const Vec3& local_pos,
                                   const Vec3& local_eye,
                                   const Vec3& /*local_angles*/,
                                   uint16_t local_weapon_id,
                                   const float* view_matrix,
                                   const OverlayConfig& cfg,
                                   OverlayClient* overlay,
                                   const SvgCache& svg_cache,
                                   const VisibilityWorker& vis_worker) {
    // 1. Safety checks: Ensure the feature is enabled and ImGui contexts/overlay structures are valid.
    if (!cfg.grenade_helper_enabled || !overlay || !overlay->get_imgui_context()) return;

    // 2. Identify the active grenade held by the local player based on current weapon ID.
    uint8_t target_grenade_type = GRENADE_NONE;
    switch (local_weapon_id) {
        case 44: target_grenade_type = GRENADE_HE; break;         // weapon_hegrenade
        case 43: target_grenade_type = GRENADE_FLASH; break;      // weapon_flashbang
        case 45: target_grenade_type = GRENADE_SMOKE; break;      // weapon_smokegrenade
        case 46: // weapon_molotov
        case 48: target_grenade_type = GRENADE_MOLOTOV; break;    // weapon_incgrenade
        case 47: target_grenade_type = GRENADE_DECOY; break;      // weapon_decoy
    }

    // Only process and render lineups if the player is holding a valid grenade.
    if (target_grenade_type == GRENADE_NONE) {
        return;
    }

    ImGui::SetCurrentContext(overlay->get_imgui_context());
    ImDrawList* draw_list = ImGui::GetBackgroundDrawList();

    float screen_w = overlay->get_width();
    float screen_h = overlay->get_height();

    struct ActiveBadge {
        const GrenadeLineup* lineup;
        float sx;
        float sy;
        float desired_y;
        float dist_2d;
        float dist_3d;
        float fade_alpha;
        float progress;
        float badge_fade;
        float badge_scale;
        float box_width;
    };

    std::vector<ActiveBadge> active_badges;

    // Aim-point HUD entries are collected first so that multiple lineups sharing a
    // standing spot can have their info popups de-overlapped before drawing.
    struct AimHud {
        const GrenadeLineup* lineup;
        float ax;
        float ay;
        float scale;
        float fade_alpha;
        float text_w;
        float text_h;
        ImVec2 desc_size;
        ImVec2 throw_size;
        ImU32 badge_color;
        Color g_col;
        float popup_top;
    };

    std::vector<AimHud> active_aim_huds;

    // Iterate through all known lineups on the map.
    for (const auto& lineup : lineups) {
        // Filter out lineups that don't match the currently equipped grenade type.
        if (lineup.weapon != target_grenade_type) {
            continue;
        }

        // Calculate 2D distance (horizontal flat plane) and vertical distance (Z elevation) separately.
        Vec3 diff = { lineup.position.x - local_pos.x, lineup.position.y - local_pos.y, lineup.position.z - local_pos.z };
        float dist_2d = std::sqrt(diff.x * diff.x + diff.y * diff.y);
        float dist_z = std::abs(diff.z);

        // Distance Filters:
        // Ignore lineups that are too far away horizontally.
        if (dist_2d > cfg.grenade_helper_render_distance) continue;
        // Ignore lineups on different levels/floors (vertical tolerance offset).
        if (dist_z > cfg.grenade_helper_render_distance) continue; 

        // BVH visibility check: hide guidance whose standing spot is occluded by map
        // geometry. Traced from the eye to the body centre above the ground spot.
        // The spot the player is standing on is never culled.
        if (cfg.grenade_helper_vischeck && dist_2d > cfg.grenade_helper_aim_radius) {
            const Vec3 vis_target = {
                lineup.position.x,
                lineup.position.y,
                lineup.position.z + VISCHECK_BODY_HEIGHT
            };
            if (vis_worker.trace_ray_bvh(local_eye, vis_target).hit) {
                continue;
            }
        }

        // Smoothly fade out the lineup marker as it approaches the maximum render distance.
        float fade_alpha = 1.0f;
        float fade_start = cfg.grenade_helper_render_distance * 0.7f;
        if (dist_2d > fade_start) {
            fade_alpha = 1.0f - ((dist_2d - fade_start) / (cfg.grenade_helper_render_distance - fade_start));
        }
        fade_alpha = std::clamp(fade_alpha, 0.0f, 1.0f);

        Color g_col = get_grenade_color(lineup.weapon, cfg);
        ImU32 badge_color = imcol32_from_color(g_col, fade_alpha);

        // Calculate progression for expanding details on the floating badge when getting closer.
        float rect_start_dist = cfg.grenade_helper_render_distance * 0.5f;
        float transition_range = cfg.grenade_helper_render_distance * 0.15f;
        if (transition_range < MIN_TRANSITION_RANGE) transition_range = MIN_TRANSITION_RANGE;
        
        float progress = 0.0f;
        if (dist_2d <= rect_start_dist) {
            progress = (rect_start_dist - dist_2d) / transition_range;
            progress = std::clamp(progress, 0.0f, 1.0f);
        }

        float sx = 0.0f, sy = 0.0f;
        if (world_to_screen(lineup.position, &sx, &sy, view_matrix, screen_w, screen_h)) {
            float circle_activation_dist = CIRCLE_ACTIVATION_DISTANCE;
            float circle_radius = GRENADE_STAND_CIRCLE_RADIUS;

            // Precise standing circle triggers when the player gets extremely close to the center spot.
            if (dist_2d <= circle_activation_dist) {
                // Check if player is perfectly aligned inside the throwing spot.
                bool is_inside = (dist_2d <= cfg.grenade_helper_aim_radius);
                ImU32 ground_circle_col = is_inside ? IM_COL32(46, 204, 113, static_cast<int>(255.0f * fade_alpha)) : badge_color;
                draw_world_circle(draw_list, lineup.position, circle_radius, ground_circle_col, 1.0f, view_matrix, screen_w, screen_h, is_inside);
            }

            // Close-proximity scale factor: scale up the badge and text to ensure readability (from 1.0x to 1.6x)
            float badge_scale = 1.0f;
            if (dist_2d < 400.0f) {
                badge_scale = 1.0f + (400.0f - dist_2d) / 400.0f * 0.6f;
            }

            // Keep the floating badge visible (no close range fade out) so the label doesn't disappear
            float badge_fade = 1.0f;

            // Calculate box width for overlap check
            float radius = BADGE_RADIUS * badge_scale;
            float square_width = radius * 2.0f;
            float rect_width = square_width;
            if (!lineup.title.empty()) {
                ImVec2 text_size = calc_scaled_text_size(ImGui::GetFont(), 10.0f * badge_scale, lineup.title);
                rect_width = radius * 2.0f + 4.0f * badge_scale + text_size.x + 8.0f * badge_scale;
            }
            float box_width = square_width + (rect_width - square_width) * progress;

            float desired_y = sy - FLOATING_BADGE_Y_OFFSET * badge_scale;
            float dist_3d = std::sqrt(diff.x * diff.x + diff.y * diff.y + diff.z * diff.z);

            active_badges.push_back({
                &lineup,
                sx,
                sy,
                desired_y,
                dist_2d,
                dist_3d,
                fade_alpha,
                progress,
                badge_fade,
                badge_scale,
                box_width
            });
        }

        // 4. Collect the Aim Target HUD Elements (Triggered when player is standing inside the aim radius)
        if (dist_2d <= cfg.grenade_helper_aim_radius) {
            // Convert lineup pitch/yaw to a 3D forward direction vector.
            float pitch = lineup.viewangles.x * (3.14159265f / 180.0f);
            float yaw = lineup.viewangles.y * (3.14159265f / 180.0f);

            Vec3 forward;
            forward.x = std::cos(pitch) * std::cos(yaw);
            forward.y = std::cos(pitch) * std::sin(yaw);
            forward.z = -std::sin(pitch); // Pitch direction convention in Source engine (-Z for down)

            // Cast a target point along the look vector.
            Vec3 aim_point;
            aim_point.x = local_eye.x + forward.x * 2000.0f;
            aim_point.y = local_eye.y + forward.y * 2000.0f;
            aim_point.z = local_eye.z + forward.z * 2000.0f;

            float ax = 0.0f, ay = 0.0f;
            if (world_to_screen(aim_point, &ax, &ay, view_matrix, screen_w, screen_h)) {
                // Apply a nice scaling to the aim target HUD as well
                const float scale = 1.25f;

                // Calculate dimensions for the description and throwing instructions tooltip box.
                ImVec2 desc_size = calc_scaled_text_size(ImGui::GetFont(), 10.0f * scale, lineup.description);
                ImVec2 throw_size = calc_scaled_text_size(ImGui::GetFont(), 10.0f * scale, lineup.throw_method);
                desc_size.x = std::round(desc_size.x);
                desc_size.y = std::round(desc_size.y);
                throw_size.x = std::round(throw_size.x);
                throw_size.y = std::round(throw_size.y);

                float text_w = 0.0f;
                float text_h = 0.0f;

                if (!lineup.description.empty()) {
                    text_w = desc_size.x;
                    text_h += desc_size.y;
                }
                if (!lineup.throw_method.empty()) {
                    text_w = std::max(text_w, throw_size.x);
                    if (text_h > 0.0f) text_h += std::round(4.0f * scale); // Line separation padding
                    text_h += throw_size.y;
                }

                active_aim_huds.push_back({
                    &lineup,
                    std::round(ax),
                    std::round(ay),
                    scale,
                    fade_alpha,
                    text_w,
                    text_h,
                    desc_size,
                    throw_size,
                    badge_color,
                    g_col,
                    std::round(ay + 18.0f * scale)
                });
            }
        }
    }

    // De-overlap the aim-point popups. Multiple lineups can share one standing spot
    // (different aim points), and their info boxes would otherwise stack on top of
    // each other. Keep the rings at the true aim points and push the boxes apart.
    std::sort(active_aim_huds.begin(), active_aim_huds.end(), [](const AimHud& a, const AimHud& b) {
        if (a.ay != b.ay) return a.ay < b.ay;
        return a.ax < b.ax;
    });

    for (size_t i = 0; i < active_aim_huds.size(); ++i) {
        AimHud& hud = active_aim_huds[i];
        if (hud.text_w <= 0.0f) continue;

        const float pad_x = std::round(8.0f * hud.scale);
        const float pad_y = std::round(6.0f * hud.scale);
        const float left_x = std::round(hud.ax - hud.text_w * 0.5f - pad_x);
        const float right_x = std::round(hud.ax + hud.text_w * 0.5f + pad_x);

        float top_y = hud.popup_top;
        bool shifted = true;
        while (shifted) {
            shifted = false;
            const float bottom_y = std::round(top_y + hud.text_h + 2.0f * pad_y);

            for (size_t j = 0; j < i; ++j) {
                const AimHud& other = active_aim_huds[j];
                if (other.text_w <= 0.0f) continue;

                const float o_pad_x = std::round(8.0f * other.scale);
                const float o_pad_y = std::round(6.0f * other.scale);
                const float o_left = std::round(other.ax - other.text_w * 0.5f - o_pad_x);
                const float o_right = std::round(other.ax + other.text_w * 0.5f + o_pad_x);
                const float o_top = other.popup_top;
                const float o_bottom = std::round(o_top + other.text_h + 2.0f * o_pad_y);

                const bool x_overlaps = (left_x < o_right) && (right_x > o_left);
                const bool y_overlaps = (top_y < o_bottom) && (bottom_y > o_top);
                if (x_overlaps && y_overlaps) {
                    top_y = std::round(o_bottom + 4.0f * hud.scale);
                    shifted = true;
                    break;
                }
            }
        }
        hud.popup_top = top_y;
    }

    // Draw the de-overlapped aim HUDs (before the badges so badges stay on top).
    for (const auto& hud : active_aim_huds) {
        const float ax_r = hud.ax;
        const float ay_r = hud.ay;

        // Draw target spot rings (aim crosshair).
        draw_list->AddCircleFilled(ImVec2(ax_r, ay_r), std::round(3.0f * hud.scale), hud.badge_color, 16);
        draw_list->AddCircle(ImVec2(ax_r, ay_r), std::round(8.0f * hud.scale), hud.badge_color, 16, std::round(1.0f * hud.scale));

        // Render the info popup near the aim target.
        if (hud.text_w > 0.0f) {
            const float pad_x = std::round(8.0f * hud.scale);
            const float pad_y = std::round(6.0f * hud.scale);

            const float top_y = hud.popup_top;
            const float bottom_y = std::round(top_y + hud.text_h + 2.0f * pad_y);
            const float left_x = std::round(ax_r - hud.text_w * 0.5f - pad_x);
            const float right_x = std::round(ax_r + hud.text_w * 0.5f + pad_x);

            ImU32 bg_col = fade_theme_color(MenuTheme::bg_card, 0.96f * hud.fade_alpha);
            ImU32 border_col = fade_theme_color(MenuTheme::border_subtle,
                                                std::max(MenuTheme::border_subtle.w, 0.35f) * hud.fade_alpha);
            const float rounding = MenuTheme::panel_rounding * hud.scale;

            // Info box background & borders
            draw_list->AddRectFilled(ImVec2(left_x, top_y), ImVec2(right_x, bottom_y), bg_col, rounding);
            draw_list->AddRect(ImVec2(left_x, top_y), ImVec2(right_x, bottom_y), border_col, rounding, 0, std::round(1.0f * hud.scale));

            // Connector from the aim ring down to a popup that had to be shifted.
            const float desired_top = std::round(ay_r + 18.0f * hud.scale);
            if (top_y > desired_top + 0.5f) {
                draw_list->AddLine(ImVec2(ax_r, std::round(ay_r + 10.0f * hud.scale)),
                                   ImVec2(ax_r, top_y), border_col, std::round(1.0f * hud.scale));
            }

            float current_y = std::round(top_y + pad_y);
            if (!hud.lineup->description.empty()) {
                ImVec2 pos1(std::round(ax_r - hud.desc_size.x * 0.5f), current_y);
                draw_list->AddText(ImGui::GetFont(), 10.0f * hud.scale, pos1, IM_COL32(220, 220, 220, static_cast<int>(255.0f * hud.fade_alpha)), hud.lineup->description.c_str());
                current_y += hud.desc_size.y + std::round(4.0f * hud.scale);
            }
            if (!hud.lineup->throw_method.empty()) {
                ImVec2 pos2(std::round(ax_r - hud.throw_size.x * 0.5f), current_y);
                draw_list->AddText(ImGui::GetFont(), 10.0f * hud.scale, pos2, IM_COL32(255, 255, 255, static_cast<int>(255.0f * hud.fade_alpha)), hud.lineup->throw_method.c_str());
            }
        }

        // Draw a guide line connecting screen center (player crosshair) to target aim location.
        float cx = std::round(screen_w * 0.5f);
        float cy = std::round(screen_h * 0.5f);

        float crosshair_dist = std::sqrt((ax_r - cx)*(ax_r - cx) + (ay_r - cy)*(ay_r - cy));
        float line_alpha = std::clamp(crosshair_dist / 100.0f, 0.0f, 1.0f);

        ImU32 line_col = imcol32_from_color(hud.g_col, line_alpha * 0.5f * hud.fade_alpha);
        if (cfg.grenade_helper_show_trace_lines && line_alpha > 0.01f) {
            draw_list->AddLine(ImVec2(cx, cy), ImVec2(ax_r, ay_r), line_col, std::round(1.0f * hud.scale));
        }
    }

    // Now group close-by active badges to prevent overlap by putting them in the same box.
    struct BadgeGroup {
        ActiveBadge representative;
        std::vector<ActiveBadge> members;
    };

    std::vector<BadgeGroup> groups;
    float group_threshold = 40.0f; // Group badges within 40 pixels on screen

    // Sort active badges by 3D distance so the closest ones are processed first (making them the representative)
    std::sort(active_badges.begin(), active_badges.end(), [](const ActiveBadge& a, const ActiveBadge& b) {
        return a.dist_3d < b.dist_3d;
    });

    for (const auto& badge : active_badges) {
        bool added = false;
        for (auto& group : groups) {
            float dx = std::abs(badge.sx - group.representative.sx);
            float dy = std::abs(badge.desired_y - group.representative.desired_y);
            if (dx < group_threshold && dy < group_threshold) {
                group.members.push_back(badge);
                added = true;
                break;
            }
        }
        if (!added) {
            groups.push_back({ badge, { badge } });
        }
    }

    // Now define dimensions and do layout vertical stacking for any separate badge groups that still overlap on screen
    struct RenderedGroup {
        BadgeGroup group;
        float current_y;
        float box_width;
        float box_height_half;
    };

    auto get_group_dimensions = [](const BadgeGroup& g, float scale, float progress, float& out_width, float& out_height_half) {
        float radius = BADGE_RADIUS * scale;
        float square_width = radius * 2.0f;
        float rect_width = square_width;

        float max_text_width = 0.0f;
        float total_text_height = 0.0f;
        float padding_left = 4.0f * scale;
        float padding_right = 8.0f * scale;
        float line_spacing = 4.0f * scale;

        bool has_text = !g.members.empty() && progress >= 0.99f;

        if (has_text) {
            for (const auto& member : g.members) {
                ImVec2 t_size = calc_scaled_text_size(ImGui::GetFont(), 10.0f * scale, member.lineup->title);
                if (t_size.x > max_text_width) {
                    max_text_width = t_size.x;
                }
                total_text_height += t_size.y;
            }
            if (!g.members.empty()) {
                total_text_height += line_spacing * (g.members.size() - 1);
            }
            rect_width = radius * 2.0f + padding_left + max_text_width + padding_right;
        }

        out_width = square_width + (rect_width - square_width) * progress;

        float box_height_half = radius;
        if (has_text && g.members.size() > 1) {
            float padding_y = 6.0f * scale;
            float needed_height = total_text_height + padding_y * 2.0f;
            if (needed_height > radius * 2.0f) {
                box_height_half = needed_height * 0.5f;
            }
        }
        out_height_half = box_height_half;
    };

    std::vector<RenderedGroup> rendered_groups;
    rendered_groups.reserve(groups.size());

    for (const auto& group : groups) {
        float width = 0.0f;
        float height_half = 0.0f;
        get_group_dimensions(group, group.representative.badge_scale, group.representative.progress, width, height_half);
        rendered_groups.push_back({
            group,
            group.representative.desired_y,
            width,
            height_half
        });
    }

    // Resolve overlaps between different groups (vertical stacking)
    for (size_t i = 1; i < rendered_groups.size(); ++i) {
        bool shifted = true;
        while (shifted) {
            shifted = false;
            float scale_i = rendered_groups[i].group.representative.badge_scale;
            float radius_i = BADGE_RADIUS * scale_i;
            float left_i = rendered_groups[i].group.representative.sx - radius_i;
            float right_i = left_i + rendered_groups[i].box_width;
            float top_i = rendered_groups[i].current_y - rendered_groups[i].box_height_half;
            float bottom_i = rendered_groups[i].current_y + rendered_groups[i].box_height_half;

            for (size_t j = 0; j < i; ++j) {
                float scale_j = rendered_groups[j].group.representative.badge_scale;
                float radius_j = BADGE_RADIUS * scale_j;
                float left_j = rendered_groups[j].group.representative.sx - radius_j;
                float right_j = left_j + rendered_groups[j].box_width;
                float top_j = rendered_groups[j].current_y - rendered_groups[j].box_height_half;
                float bottom_j = rendered_groups[j].current_y + rendered_groups[j].box_height_half;

                bool x_overlaps = (left_i < right_j) && (right_i > left_j);
                bool y_overlaps = (top_i < bottom_j) && (bottom_i > top_j);

                if (x_overlaps && y_overlaps) {
                    rendered_groups[i].current_y = top_j - rendered_groups[i].box_height_half - 2.0f;
                    shifted = true;
                    break;
                }
            }
        }
    }

    // Render all groups and draw connector lines to their corresponding ground spots
    for (const auto& rg : rendered_groups) {
        std::vector<std::string> group_titles;
        for (const auto& member : rg.group.members) {
            group_titles.push_back(member.lineup->title);
        }

        Color g_col = get_grenade_color(rg.group.representative.lineup->weapon, cfg);
        ImU32 badge_color = imcol32_from_color(g_col, rg.group.representative.fade_alpha);

        float actual_height_half = 0.0f;
        draw_grouped_badge(
            draw_list,
            rg.group.representative.sx,
            rg.current_y,
            rg.group.representative.lineup->weapon,
            group_titles,
            badge_color,
            rg.group.representative.fade_alpha,
            svg_cache,
            rg.group.representative.progress,
            rg.group.representative.badge_scale,
            actual_height_half
        );

        // Draw connector lines
        for (const auto& member : rg.group.members) {
            if (rg.current_y < member.desired_y - 1.0f || member.lineup != rg.group.representative.lineup) {
                draw_list->AddLine(
                    ImVec2(std::round(member.sx), std::round(member.sy)),
                    ImVec2(std::round(rg.group.representative.sx), std::round(rg.current_y + actual_height_half)),
                    imcol32_from_color(g_col, 0.4f * member.fade_alpha * member.badge_fade),
                    1.0f
                );
            }
        }
    }

    if (cfg.grenade_solver_show_trajectory && cfg.grenade_solver_show_zones) {
        auto zones = zone_manager.get_zones(current_map);
        for (const auto& zone : zones) {
            glm::vec3 center = (zone.mins + zone.maxs) * 0.5f;
            float sx = 0.0f, sy = 0.0f;
            Vec3 c_vec{center.x, center.y, center.z};
            if (world_to_screen(c_vec, &sx, &sy, view_matrix, screen_w, screen_h)) {
                ImU32 col = IM_COL32(200, 200, 50, 150);
                if (zone.grenade_type != 0) {
                    col = imcol32_from_color(get_grenade_color(zone.grenade_type, cfg), 0.6f);
                }
                
                // Draw 3D AABB box for saved zones
                draw_world_aabb(draw_list, zone.mins, zone.maxs, col, 1.5f, view_matrix, screen_w, screen_h);
                
                ImVec2 t_size = ImGui::CalcTextSize(zone.name.c_str());
                draw_list->AddText(ImVec2(sx - t_size.x * 0.5f, sy - t_size.y * 0.5f), IM_COL32(255, 255, 255, 255), zone.name.c_str());
            }
        }
    }

    // Draw pending creator points (2D flat box or 3D cube)
    if (cfg.grenade_solver_show_trajectory && cfg.grenade_solver_location_creator_enabled) {
        auto pending_pts = zone_manager.get_pending_points();
        if (!pending_pts.empty()) {
        ImU32 creation_color = IM_COL32(0, 255, 255, 220); // Cyan (like the photo)
        if (pending_pts.size() == 1) {
            float sx, sy;
            if (world_to_screen({pending_pts[0].x, pending_pts[0].y, pending_pts[0].z}, &sx, &sy, view_matrix, screen_w, screen_h)) {
                draw_list->AddCircleFilled(ImVec2(sx, sy), 6.0f, creation_color);
                draw_list->AddCircle(ImVec2(sx, sy), 8.0f, IM_COL32(0, 0, 0, 255), 12, 1.0f);
            }
        } else if (pending_pts.size() == 2) {
            draw_world_2d_box(draw_list, pending_pts[0], pending_pts[1], creation_color, 2.0f, view_matrix, screen_w, screen_h);
            for (const auto& pt : pending_pts) {
                float sx, sy;
                if (world_to_screen({pt.x, pt.y, pt.z}, &sx, &sy, view_matrix, screen_w, screen_h)) {
                    draw_list->AddCircleFilled(ImVec2(sx, sy), 4.0f, creation_color);
                }
            }
        } else if (pending_pts.size() == 3) {
            glm::vec3 mins(
                std::min({pending_pts[0].x, pending_pts[1].x, pending_pts[2].x}),
                std::min({pending_pts[0].y, pending_pts[1].y, pending_pts[2].y}),
                std::min({pending_pts[0].z, pending_pts[1].z, pending_pts[2].z})
            );
            glm::vec3 maxs(
                std::max({pending_pts[0].x, pending_pts[1].x, pending_pts[2].x}),
                std::max({pending_pts[0].y, pending_pts[1].y, pending_pts[2].y}),
                std::max({pending_pts[0].z, pending_pts[1].z, pending_pts[2].z})
            );
            draw_world_aabb(draw_list, mins, maxs, creation_color, 2.5f, view_matrix, screen_w, screen_h);
            for (const auto& pt : pending_pts) {
                float sx, sy;
                if (world_to_screen({pt.x, pt.y, pt.z}, &sx, &sy, view_matrix, screen_w, screen_h)) {
                    draw_list->AddCircleFilled(ImVec2(sx, sy), 4.0f, creation_color);
                }
            }
        }
    }
}

    if (cfg.grenade_solver_show_trajectory && vis_result.solver_active) {
        auto zones = zone_manager.get_zones(current_map);
        for (const auto& sol : vis_result.solver_solutions) {
            if (!sol.found || sol.trajectory.points.empty()) continue;

            ImU32 t_col = imcol32_from_color(get_grenade_color(target_grenade_type, cfg), 1.0f);

            // Draw aim point
            float p_rad = sol.display_pitch * 3.14159265f / 180.0f;
            float yaw_rad = sol.yaw;
            float cx_fwd = std::cos(p_rad) * std::cos(yaw_rad);
            float cy_fwd = std::cos(p_rad) * std::sin(yaw_rad);
            float cz_fwd = -std::sin(p_rad);

            Vec3 aim_world = {
                local_eye.x + cx_fwd * 500.0f,
                local_eye.y + cy_fwd * 500.0f,
                local_eye.z + cz_fwd * 500.0f
            };

            float ax, ay;
            if (world_to_screen(aim_world, &ax, &ay, view_matrix, screen_w, screen_h)) {
                // Draw target spot rings (matches helper aimpoint style)
                draw_list->AddCircleFilled(ImVec2(ax, ay), 3.0f, t_col, 16);
                draw_list->AddCircle(ImVec2(ax, ay), 8.0f, t_col, 16, 1.0f);

                // Draw guide line connecting screen center (player crosshair) to target aim location
                float cx = screen_w * 0.5f;
                float cy = screen_h * 0.5f;
                float crosshair_dist = std::sqrt((ax - cx)*(ax - cx) + (ay - cy)*(ay - cy));
                float line_alpha = std::clamp(crosshair_dist / 100.0f, 0.0f, 1.0f);
                ImU32 line_col = imcol32_from_color(get_grenade_color(target_grenade_type, cfg), line_alpha * 0.4f);
                if (cfg.grenade_helper_show_trace_lines && line_alpha > 0.01f) {
                    draw_list->AddLine(ImVec2(cx, cy), ImVec2(ax, ay), line_col, 1.0f);
                }

                // Render the info popup near the aim target (matches helper popup style)
                std::string zone_name = "Grenade Zone";
                if (sol.zone_index >= 0 && sol.zone_index < (int)zones.size()) {
                    zone_name = zones[sol.zone_index].name;
                }

                std::string throw_str = "NORMAL THROW";
                if (sol.throw_type == ThrowType::JUMP) throw_str = "JUMP THROW";
                else if (sol.throw_type == ThrowType::W_JUMP) throw_str = "W+JUMP THROW";

                ImVec2 desc_size = ImGui::CalcTextSize(zone_name.c_str());
                ImVec2 throw_size = ImGui::CalcTextSize(throw_str.c_str());
                float text_w = std::max(desc_size.x, throw_size.x);
                float text_h = desc_size.y + throw_size.y + 4.0f;

                float pad_x = 8.0f;
                float pad_y = 6.0f;
                
                float top_y = ay + 18.0f;
                float bottom_y = top_y + text_h + 2.0f * pad_y;
                float left_x = ax - text_w * 0.5f - pad_x;
                float right_x = ax + text_w * 0.5f + pad_x;

                ImU32 bg_col = fade_theme_color(MenuTheme::bg_card, 0.96f);
                ImU32 border_col = fade_theme_color(MenuTheme::border_subtle,
                                                    std::max(MenuTheme::border_subtle.w, 0.35f));
                const float rounding = MenuTheme::panel_rounding;

                // Draw info box background & borders
                draw_list->AddRectFilled(ImVec2(left_x, top_y), ImVec2(right_x, bottom_y), bg_col, rounding);
                draw_list->AddRect(ImVec2(left_x, top_y), ImVec2(right_x, bottom_y), border_col, rounding, 0, 1.0f);

                // Draw text
                draw_list->AddText(ImVec2(ax - desc_size.x * 0.5f, top_y + pad_y), IM_COL32(220, 220, 220, 255), zone_name.c_str());
                draw_list->AddText(ImVec2(ax - throw_size.x * 0.5f, top_y + pad_y + desc_size.y + 4.0f), IM_COL32(255, 255, 255, 255), throw_str.c_str());
            }
        }
    }
}
