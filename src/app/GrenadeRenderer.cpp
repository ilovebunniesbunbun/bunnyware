#include "GrenadeRenderer.hpp"
#include "logger.hpp"
#include "overlay/esp_drawing.hpp"
#include "smoke_wireframe.hpp"
#include "external/imgui/imgui.h"
#include <GL/glew.h>
#include <algorithm>
#include <cmath>
#include <GLFW/glfw3.h>
#include <cstdio>

namespace {
    constexpr float kTimerFontSize = 10.0f;

    // Stencil reference used to scope the "Through Smokes" map pass to the
    // stamped smoke silhouette. Any unused reference works: the buffer is
    // cleared right before the stamp and all chams stencil work has finished.
    constexpr int kSmokeStencilRef = 1;

    inline uint32_t compact1(uint32_t v) {
        v &= 0x09249249U;
        v = (v ^ (v >> 2)) & 0x030C30C3U;
        v = (v ^ (v >> 4)) & 0x0300F00FU;
        v = (v ^ (v >> 8)) & 0x00000FFFU;
        return v;
    }

    inline void morton3_decode(uint32_t m, int& x, int& y, int& z) {
        x = static_cast<int>(compact1(m));
        y = static_cast<int>(compact1(m >> 1));
        z = static_cast<int>(compact1(m >> 2));
    }

    // Projects the smoke shell into window coordinates so the stencil, depth and
    // map-wireframe passes can be scissored to the smoke's screen bounds.
    // Returns 0 when the smoke is entirely behind the camera or outside the
    // viewport (nothing to draw), 1 when `rect` holds x, y, width, height for
    // glScissor, and 2 when part of the shell crosses the near plane so no safe
    // rect exists (draw without scissoring).
    int smoke_screen_bounds(const std::vector<Vec3>& points, const float* view_proj, int screen_w, int screen_h,
                            int* rect)
    {
        if (points.empty() || screen_w <= 0 || screen_h <= 0) return 2;

        constexpr float kNearEpsilon = 0.001f;
        constexpr int kMarginPx = 4;

        float min_x = 1e30f, min_y = 1e30f, max_x = -1e30f, max_y = -1e30f;
        int behind = 0;
        for (const Vec3& p : points) {
            const float clip_w = view_proj[3] * p.x + view_proj[7] * p.y + view_proj[11] * p.z + view_proj[15];
            if (clip_w <= kNearEpsilon) {
                behind++;
                continue;
            }
            const float clip_x = view_proj[0] * p.x + view_proj[4] * p.y + view_proj[8] * p.z + view_proj[12];
            const float clip_y = view_proj[1] * p.x + view_proj[5] * p.y + view_proj[9] * p.z + view_proj[13];
            const float sx = (clip_x / clip_w * 0.5f + 0.5f) * static_cast<float>(screen_w);
            const float sy = (clip_y / clip_w * 0.5f + 0.5f) * static_cast<float>(screen_h);
            min_x = std::min(min_x, sx);
            max_x = std::max(max_x, sx);
            min_y = std::min(min_y, sy);
            max_y = std::max(max_y, sy);
        }
        if (behind == static_cast<int>(points.size())) return 0;
        if (behind > 0) return 2;

        const int x0 = std::clamp(static_cast<int>(std::floor(min_x)) - kMarginPx, 0, screen_w);
        const int y0 = std::clamp(static_cast<int>(std::floor(min_y)) - kMarginPx, 0, screen_h);
        const int x1 = std::clamp(static_cast<int>(std::ceil(max_x)) + kMarginPx, 0, screen_w);
        const int y1 = std::clamp(static_cast<int>(std::ceil(max_y)) + kMarginPx, 0, screen_h);
        if (x0 >= x1 || y0 >= y1) return 0;

        rect[0] = x0;
        rect[1] = y0;
        rect[2] = x1 - x0;
        rect[3] = y1 - y0;
        return 1;
    }

    inline float cross_2d(const ImVec2& o, const ImVec2& a, const ImVec2& b) {
        return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
    }

    std::vector<ImVec2> get_convex_hull(std::vector<ImVec2> pts) {
        if (pts.size() < 3) return pts;
        std::sort(pts.begin(), pts.end(), [](const ImVec2& a, const ImVec2& b) {
            if (a.x != b.x) return a.x < b.x;
            return a.y < b.y;
        });
        std::vector<ImVec2> lower;
        for (const auto& p : pts) {
            while (lower.size() >= 2 && cross_2d(lower[lower.size() - 2], lower.back(), p) <= 0.0f) {
                lower.pop_back();
            }
            lower.push_back(p);
        }
        std::vector<ImVec2> upper;
        for (auto it = pts.rbegin(); it != pts.rend(); ++it) {
            const auto& p = *it;
            while (upper.size() >= 2 && cross_2d(upper[upper.size() - 2], upper.back(), p) <= 0.0f) {
                upper.pop_back();
            }
            upper.push_back(p);
        }
        if (!lower.empty()) lower.pop_back();
        if (!upper.empty()) upper.pop_back();
        std::vector<ImVec2> hull;
        hull.reserve(lower.size() + upper.size());
        hull.insert(hull.end(), lower.begin(), lower.end());
        hull.insert(hull.end(), upper.begin(), upper.end());
        return hull;
    }

    inline ImU32 imcol32_from_color(const Color& c, float alpha_factor = 1.0f) {
        return IM_COL32(
            static_cast<int>(c.r * 255.0f),
            static_cast<int>(c.g * 255.0f),
            static_cast<int>(c.b * 255.0f),
            static_cast<int>(c.a * 255.0f * alpha_factor)
        );
    }

    inline ImU32 lerp_color(const Color& low, const Color& high, float frac) {
        float r = low.r + (high.r - low.r) * frac;
        float g = low.g + (high.g - low.g) * frac;
        float b = low.b + (high.b - low.b) * frac;
        float a = low.a + (high.a - low.a) * frac;
        return IM_COL32(
            static_cast<int>(r * 255.0f),
            static_cast<int>(g * 255.0f),
            static_cast<int>(b * 255.0f),
            static_cast<int>(a * 255.0f)
        );
    }

    float distance_between(const Vec3& a, const Vec3& b) {
        const float dx = a.x - b.x;
        const float dy = a.y - b.y;
        const float dz = a.z - b.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    float timer_scale_for_distance(const Vec3& viewer_position, const Vec3& world_position,
                                   float base_scale) {
        constexpr float full_size_distance = 800.0f;
        constexpr float minimum_size_distance = 2800.0f;
        constexpr float far_scale = 0.75f;
        base_scale = std::clamp(base_scale, 0.50f, 1.50f);
        const float distance = distance_between(viewer_position, world_position);
        const float far_fraction = std::clamp(
            (distance - full_size_distance) / (minimum_size_distance - full_size_distance), 0.0f, 1.0f);
        const float distance_scale = 1.0f - (1.0f - far_scale) * far_fraction;
        return base_scale * distance_scale;
    }

    inline Color get_grenade_color(uint8_t type, const OverlayConfig& cfg) {
        switch (type) {
            case GRENADE_HE: return cfg.grenade_color_he;
            case GRENADE_FLASH: return cfg.grenade_color_flash;
            case GRENADE_SMOKE: return cfg.grenade_color_smoke;
            case GRENADE_MOLOTOV: return cfg.grenade_color_molotov;
            case GRENADE_DECOY: return cfg.grenade_color_decoy;
            case GRENADE_C4: return Color(1.0f, 0.267f, 0.267f, 0.8f);
            default: return Color(0.67f, 0.69f, 0.86f, 0.8f);
        }
    }

    inline const char* get_grenade_name(uint8_t type) {
        switch (type) {
            case GRENADE_HE: return "HE";
            case GRENADE_FLASH: return "FLASH";
            case GRENADE_SMOKE: return "SMOKE";
            case GRENADE_MOLOTOV: return "FIRE";
            case GRENADE_DECOY: return "DECOY";
            case GRENADE_C4: return "C4";
            default: return "GRENADE";
        }
    }

    inline const char* get_timer_label(uint8_t type) {
        switch (type) {
            case GRENADE_C4: return "BOMB";
            case GRENADE_SMOKE: return "SMOKE";
            case GRENADE_DECOY: return "DECOY";
            case GRENADE_MOLOTOV: return "MOLOTOV";
            case GRENADE_HE: return "HE";
            case GRENADE_FLASH: return "FLASH";
            default: return "GRENADE";
        }
    }

    void draw_minimal_timer_bar(ImDrawList* draw_list, float sx, float sy, uint8_t grenade_type,
                                float remaining, float max_duration, ImU32 bar_color, float fade_alpha,
                                float scale) {
        constexpr float base_width = 84.0f;
        constexpr float base_height = 6.0f;
        constexpr float base_label_gap = 4.0f;
        const float width = std::round(base_width * scale);
        const float height = std::round(base_height * scale);
        const float label_gap = std::round(base_label_gap * scale);
        const float fraction = max_duration > 0.0f
            ? std::clamp(remaining / max_duration, 0.0f, 1.0f)
            : 0.0f;
        const float x = std::round(sx - width * 0.5f);
        const float y = std::round(sy);
        ImFont* font = ImGui::GetFont();
        const ImVec2 label_size = font->CalcTextSizeA(
            kTimerFontSize, 9999.0f, 0.0f, get_timer_label(grenade_type));
        const ImVec2 label_pos(std::round(sx - label_size.x * 0.5f), std::round(y - label_size.y - label_gap));

        const uint32_t color_alpha = (bar_color >> 24) & 0xFF;
        const ImU32 faded_bar_color = (bar_color & 0x00FFFFFF)
            | (static_cast<uint32_t>(color_alpha * fade_alpha) << 24);
        const ImU32 text_color = IM_COL32(255, 255, 255, static_cast<int>(255.0f * fade_alpha));
        const ImU32 shadow_color = IM_COL32(0, 0, 0, static_cast<int>(255.0f * fade_alpha));

        draw_list->AddText(font, kTimerFontSize, ImVec2(label_pos.x + 1.0f, label_pos.y + 1.0f),
                           shadow_color, get_timer_label(grenade_type));
        draw_list->AddText(font, kTimerFontSize, label_pos, text_color, get_timer_label(grenade_type));
        draw_list->AddRectFilled(ImVec2(x, y), ImVec2(x + width, y + height),
                                 IM_COL32(0, 0, 0, static_cast<int>(190.0f * fade_alpha)));
        draw_list->AddRectFilled(ImVec2(x, y), ImVec2(std::round(x + width * fraction), y + height), faded_bar_color);
        draw_list->AddRect(ImVec2(x - 1.0f, y - 1.0f), ImVec2(x + width + 1.0f, y + height + 1.0f),
                           IM_COL32(0, 0, 0, static_cast<int>(255.0f * fade_alpha)), 0.0f, 0, 1.0f);
    }

    void draw_badge_with_timer(ImDrawList* draw_list, float sx, float sy, uint8_t grenade_type, float remaining, float max_duration, ImU32 badge_color, float fade_alpha, const SvgCache& svg_cache, float scale) {
        const float radius = 22.0f * scale;
        const float radial_radius = radius - 3.0f * scale;
        
        // Background circle
        ImU32 bg_col = IM_COL32(30, 30, 35, static_cast<int>(200.0f * fade_alpha));
        draw_list->AddCircleFilled(ImVec2(sx, sy), radius, bg_col, 32);
        
        // Radial backdrop circle
        ImU32 radial_bg_col = IM_COL32(15, 15, 20, static_cast<int>(200.0f * fade_alpha));
        draw_list->AddCircleFilled(ImVec2(sx, sy), radial_radius, radial_bg_col, 32);
        
        // Draw SVG icon
        SvgTexture tex = svg_cache.get_texture(grenade_type);
        if (tex.id != 0) {
            float max_dim = 20.0f * scale;
            float icon_scale = max_dim / std::max(tex.width, tex.height);
            float icon_w = tex.width * icon_scale;
            float icon_h = tex.height * icon_scale;

            ImVec2 icon_min(sx - icon_w * 0.5f, sy - icon_h * 0.5f - scale); // offset up slightly
            ImVec2 icon_max(sx + icon_w * 0.5f, sy + icon_h * 0.5f - scale);
            draw_list->AddImage((ImTextureID)(intptr_t)tex.id, icon_min, icon_max, ImVec2(0,0), ImVec2(1,1), IM_COL32(255,255,255, static_cast<int>(255.0f * fade_alpha)));
        }

        // Timer radial progress
        if (max_duration > 0.0f) {
            float frac = std::clamp(remaining / max_duration, 0.0f, 1.0f);
            
            // Adjust badge color alpha for the radial line
            uint32_t badge_a = (badge_color >> 24) & 0xFF;
            ImU32 arc_col = (badge_color & 0x00FFFFFF) | (static_cast<uint32_t>(badge_a * fade_alpha) << 24);

            float start_angle = -3.14159265f / 2.0f;
            float end_angle = start_angle + (frac * 3.14159265f * 2.0f);
            
            // Fallback for full circle because PathArcTo struggles with 360 deg
            if (frac >= 0.999f) {
                draw_list->AddCircle(ImVec2(sx, sy), radial_radius, arc_col, 32, 2.5f * scale);
            } else if (frac > 0.001f) {
                draw_list->PathArcTo(ImVec2(sx, sy), radial_radius, start_angle, end_angle, 32);
                draw_list->PathStroke(arc_col, 0, 2.5f * scale);
            }
        }

        // Draw timer text under the circle
        if (max_duration > 0.0f) {
            char text_buf[32];
            std::snprintf(text_buf, sizeof(text_buf), "%.1fs", remaining);
            ImFont* font = ImGui::GetFont();
            const ImVec2 text_size = font->CalcTextSizeA(kTimerFontSize, 9999.0f, 0.0f, text_buf);
            ImVec2 text_pos(std::round(sx - text_size.x * 0.5f),
                            std::round(sy + radius + 4.0f * scale));
            
            ImU32 text_shadow_col = IM_COL32(0, 0, 0, static_cast<int>(255.0f * fade_alpha));
            ImU32 text_col = IM_COL32(255, 255, 255, static_cast<int>(255.0f * fade_alpha));
            draw_list->AddText(font, kTimerFontSize, ImVec2(text_pos.x + 1.0f, text_pos.y + 1.0f),
                               text_shadow_col, text_buf);
            draw_list->AddText(font, kTimerFontSize, text_pos, text_col, text_buf);
        }
    }

    void draw_impact_warning(ImDrawList* draw_list, float sx, float sy, int count, float fade_alpha = 1.0f) {
        char text_buf[64];
        std::snprintf(text_buf, sizeof(text_buf), "%d ENEMY(S)", count);
        
        ImVec2 text_size = ImGui::CalcTextSize(text_buf);
        float pad_x = 6.0f;
        float pad_y = 3.0f;
        
        ImVec2 rect_min(sx - text_size.x * 0.5f - pad_x, sy - text_size.y * 0.5f - pad_y);
        ImVec2 rect_max(sx + text_size.x * 0.5f + pad_x, sy + text_size.y * 0.5f + pad_y);
        
        ImU32 bg_col = IM_COL32(30, 30, 35, static_cast<int>(245.0f * fade_alpha));
        draw_list->AddRectFilled(rect_min, rect_max, bg_col, 0.0f);
        
        ImVec2 text_pos(sx - text_size.x * 0.5f, sy - text_size.y * 0.5f);
        ImU32 text_shadow_col = IM_COL32(0, 0, 0, static_cast<int>(255.0f * fade_alpha));
        ImU32 text_col = IM_COL32(255, 255, 255, static_cast<int>(255.0f * fade_alpha));
        draw_list->AddText(ImVec2(text_pos.x + 1.0f, text_pos.y + 1.0f), text_shadow_col, text_buf);
        draw_list->AddText(text_pos, text_col, text_buf);
    }
}

// Smoke wireframe cage + through-smokes map rendering. The two features are
// independent: the cage only draws lines, through-smokes only stamps the smoke
// shape. Both share the contour ring builder.
//
// The cage lathes contour rings through the smoke's dense voxel layers and ties
// them with vertical struts, hugging the actual cloud shape. The same shape (as a
// filled shell) is stamped into the stencil buffer so the smoke's own depth can be
// erased inside the silhouette: the cage then renders through the cloud, and with
// "Through Smokes" the map collision wireframe is drawn depth-tested inside the
// silhouette so the first wall behind the smoke shows through it.
//
// Both are world geometry, so this runs before the player passes: chams, the
// skeleton and the ESP overlays must paint on top of them rather than being
// covered by them. Returns true when the stencil refresh rewrote the map depth
// inside the silhouette; the caller must re-render the depth prepass before
// player rendering in that case so smoke occlusion is unchanged.
bool GrenadeRenderer::render_smoke_visuals(const VischeckResult& render_result, EspRenderer& esp_renderer,
                                           DepthPrepassRenderer& depth_prepass, const float* gl_vp,
                                           const OverlayConfig& cfg, OverlayClient* overlay)
{
    if (!cfg.smoke_wireframe && !cfg.through_smokes) return false;

    bool depth_refreshed = false;

    std::vector<smoke_wireframe::CageLine> cage_lines;
    std::vector<Vec3> cage_shell;  // three vertices per triangle
    cage_lines.reserve(cfg.smoke_wireframe ? 2048 : 0);
    cage_shell.reserve(2048);

    // Build the contour rings once per smoke and feed every consumer.
    smoke_wireframe::CageRing rings[smoke_wireframe::kMaxCageRings];
    for (int i = 0; i < render_result.smoke_count; ++i) {
        const auto& smoke = render_result.smokes[i];
        if (!smoke.active) continue;

        const int ring_count = smoke_wireframe::build_cage_rings(smoke, rings, smoke_wireframe::kMaxCageRings);
        if (ring_count == 0) continue;
        if (cfg.smoke_wireframe) {
            smoke_wireframe::collect_cage_lines_from_rings(rings, ring_count, cage_lines,
                                                           smoke_wireframe::kMaxCageLines);
        }
        smoke_wireframe::collect_cage_triangles_from_rings(rings, ring_count, cage_shell,
                                                           smoke_wireframe::kMaxCageTriangles);
    }

    if (cfg.smoke_wireframe) {
        const float cage_color[4] = {cfg.smoke_wireframe_color.r, cfg.smoke_wireframe_color.g,
                                     cfg.smoke_wireframe_color.b, cfg.smoke_wireframe_color.a};
        esp_renderer.clear();
        esp_renderer.set_projection(gl_vp);
        for (const smoke_wireframe::CageLine& line : cage_lines) {
            esp_renderer.add_line_3d(line.a, line.b, cage_color, 1.0f);
        }
        esp_renderer.flush_lines();
        // flush_lines() keeps the batches so they can be re-drawn with an override
        // colour; drop them here so a later flush does not paint the cage again on
        // top of the player passes this call exists to stay under.
        esp_renderer.clear();
    }

    // Upload the shell as the stencil stamp geometry. Type id -1 marks full smoke
    // density: the depth program neither discards nor shifts it.
    if (!cage_shell.empty() && depth_prepass.has_geometry()) {
        const auto to_vertex = [](const Vec3& p) {
            return MapParser::Vertex{MapParser::Vec3{p.x, p.y, p.z}, -1.0f};
        };
        std::vector<MapParser::Triangle> shell_mesh;
        shell_mesh.reserve(cage_shell.size() / 3);
        for (std::size_t v = 0; v + 2 < cage_shell.size(); v += 3) {
            shell_mesh.push_back(MapParser::Triangle{to_vertex(cage_shell[v]), to_vertex(cage_shell[v + 1]),
                                                     to_vertex(cage_shell[v + 2])});
        }
        depth_prepass.upload_smoke_shell(shell_mesh);
    } else {
        depth_prepass.clear_smoke_shell();
    }

    if (!depth_prepass.has_smoke_shell() || !depth_prepass.has_geometry()) return depth_refreshed;

    // Scissor the stamp/refresh/wireframe passes to the smoke's screen bounds so
    // they stay cheap; skip everything when the smoke cannot be on screen at all.
    int scissor_rect[4] = {0, 0, 0, 0};
    int bounds_mode = 2;
    if (overlay != nullptr) {
        bounds_mode =
            smoke_screen_bounds(cage_shell, gl_vp, overlay->get_width(), overlay->get_height(), scissor_rect);
    }
    if (bounds_mode == 0) return depth_refreshed;

    if (bounds_mode == 1) {
        glEnable(GL_SCISSOR_TEST);
        glScissor(scissor_rect[0], scissor_rect[1], scissor_rect[2], scissor_rect[3]);
    }

    // Mark the smoke silhouette into the stencil buffer.
    glClear(GL_STENCIL_BUFFER_BIT);
    glEnable(GL_STENCIL_TEST);
    glStencilFunc(GL_ALWAYS, kSmokeStencilRef, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    depth_prepass.stamp_smoke_silhouette(gl_vp);

    glStencilFunc(GL_EQUAL, kSmokeStencilRef, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);

    // The occlusion prepass also writes the smoke's own (alpha-shifted) depth;
    // erase it inside the silhouette so the cage and the map wireframe are not
    // hidden by the cloud itself.
    if (cfg.esp_smoke_check) {
        depth_prepass.refresh_depth_in_stencil(gl_vp);
        depth_refreshed = true;
    }

    if (cfg.through_smokes) {
        const float through_color[4] = {cfg.through_smokes_color.r, cfg.through_smokes_color.g,
                                        cfg.through_smokes_color.b, cfg.through_smokes_color.a};
        depth_prepass.render_wireframe(gl_vp, through_color, true);
    }

    glDisable(GL_STENCIL_TEST);
    if (bounds_mode == 1) {
        glDisable(GL_SCISSOR_TEST);
    }

    return depth_refreshed;
}

void GrenadeRenderer::render(GrenadeTracker& tracker, EspRenderer& esp_renderer,
                             const VischeckResult& render_result, const float* gl_vp, const float* view_matrix,
                             const OverlayConfig& cfg, OverlayClient* overlay, const SvgCache& svg_cache,
                             const PlantedC4& planted_c4, const Vec3& viewer_position) {
    // Debug Visibility and the smoke features must not depend on the other
    // grenade features: the smoke-voxel debug view has to run even when
    // trajectory, inferno, warnings, and C4 rendering are all disabled. The smoke
    // cage and through-smokes passes are drawn by render_smoke_visuals() instead,
    // before the player passes.
    if (!cfg.draw_grenade_trajectory && !cfg.inferno_spread_enabled && !cfg.grenade_warnings_enabled &&
        !cfg.c4_render_in_world && !cfg.debug_visibility) {
        return;
    }

    auto& tracked_active_trajectories = tracker.get_tracked_active_trajectories();
    auto& fading_trajectories = tracker.get_fading_trajectories();

    esp_renderer.clear();
    esp_renderer.set_projection(gl_vp);

    if (cfg.trajectory_show_through_walls) {
        glDisable(GL_DEPTH_TEST);
    } else {
        glEnable(GL_DEPTH_TEST);
    }
    glDepthMask(GL_FALSE);

    if (cfg.draw_grenade_trajectory) {
        // B) Draw Held Grenade Trajectory
    bool is_held_visible = render_result.held_trajectory.valid && !render_result.held_trajectory.points.empty();
    if (is_held_visible) {
        const auto& traj = render_result.held_trajectory;
        
        if (!was_held_trajectory_visible || traj.type != last_held_grenade_type) {
            if (cfg.debug_visibility) {
                BUNNYWARE_LOG_INFO("[DEBUG-VISIBILITY] Held Grenade Trajectory went VISIBLE. "
                             "Reason: Player started aiming/holding a throw. "
                             "Type: {} ({}), Points: {}, Start Pos: ({:.2f}, {:.2f}, {:.2f}), End Pos: ({:.2f}, {:.2f}, {:.2f}), Affected enemies: {}",
                             traj.type, get_grenade_name(traj.type), traj.points.size(),
                             traj.points.empty() ? 0.0f : traj.points.front().x,
                             traj.points.empty() ? 0.0f : traj.points.front().y,
                             traj.points.empty() ? 0.0f : traj.points.front().z,
                             traj.end_pos.x, traj.end_pos.y, traj.end_pos.z,
                             traj.affected_count);
            }
        }
        was_held_trajectory_visible = true;
        last_held_grenade_type = traj.type;

        Color g_col = get_grenade_color(traj.type, cfg);
        float color_arr[4] = { g_col.r, g_col.g, g_col.b, g_col.a };
        
        // Draw main trajectory (675, no drag — matches game)
        for (size_t i = 0; i < traj.points.size() - 1; ++i) {
            esp_renderer.add_line_3d(traj.points[i], traj.points[i + 1], color_arr, cfg.trajectory_thickness);
        }
        for (const auto& bounce : traj.bounces) {
            esp_renderer.add_box_instance(bounce, cfg.trajectory_bounce_size, color_arr);
        }
        esp_renderer.add_circle_instance(traj.end_pos, cfg.trajectory_detonation_radius, color_arr);

        // Render predicted affected enemies count at detonation point
        if (traj.affected_count > 0 && overlay && overlay->get_imgui_context()) {
            float dsx = 0.0f, dsy = 0.0f;
            if (world_to_screen(traj.end_pos, &dsx, &dsy, view_matrix, overlay->get_width(), overlay->get_height())) {
                ImGui::SetCurrentContext(overlay->get_imgui_context());
                ImDrawList* draw_list = ImGui::GetBackgroundDrawList();
                draw_impact_warning(draw_list, dsx, dsy + 15.0f, traj.affected_count);
            }
        }
    } else {
        if (was_held_trajectory_visible) {
            was_held_trajectory_visible = false;
            last_held_grenade_type = 0;
        }
    }



    // C) Draw In-Flight Grenade Trajectories
    if (!tracked_active_trajectories.empty()) {
        for (auto& tracked : tracked_active_trajectories) {
            if (!tracked.still_active) continue;
            const auto& traj = tracked.traj;
            if (!traj.valid || traj.points.empty()) continue;

            Color g_col = get_grenade_color(traj.type, cfg);
            float color_arr[4] = { g_col.r, g_col.g, g_col.b, g_col.a };

            if (!tracked.uploaded) {
                esp_renderer.upload_trajectory(tracked.entity_handle, tracked.spawn_time, traj.points, color_arr);
                tracked.uploaded = true;
            }

            esp_renderer.draw_gpu_trajectory(tracked.entity_handle, tracked.spawn_time, 0.0f, 1.0f, cfg.trajectory_thickness);

            // Draw bounce boxes
            for (const auto& bounce : traj.bounces) {
                esp_renderer.add_box_instance(bounce, cfg.trajectory_bounce_size, color_arr);
            }
            esp_renderer.add_circle_instance(traj.end_pos, cfg.trajectory_detonation_radius, color_arr);
        }
    }

    // D) Draw Fading/Erasing Grenade Trajectories
    if (!fading_trajectories.empty()) {
        for (auto& fading : fading_trajectories) {
            const auto& traj = fading.traj;
            if (traj.points.empty()) continue;

            Color g_col = get_grenade_color(traj.type, cfg);

            // Calculate point where erasure has reached
            float float_idx = static_cast<float>(traj.points.size() - 1) * fading.erase_progress;
            size_t start_idx = static_cast<size_t>(float_idx);
            if (start_idx >= traj.points.size() - 1) continue;

            // Modulate colors by fade_alpha
            float color_mod[4] = {
                g_col.r,
                g_col.g,
                g_col.b,
                g_col.a * fading.fade_alpha
            };

            esp_renderer.draw_gpu_trajectory(traj.entity_handle, traj.spawn_time, fading.erase_progress, fading.fade_alpha, cfg.trajectory_thickness);

            // Draw remaining bounce boxes
            for (size_t b_idx = 0; b_idx < traj.bounces.size(); ++b_idx) {
                const auto& bounce = traj.bounces[b_idx];
                size_t closest_pt_idx = (b_idx < traj.bounce_indices.size()) ? traj.bounce_indices[b_idx] : 0;
                if (closest_pt_idx >= start_idx) {
                    esp_renderer.add_box_instance(bounce, cfg.trajectory_bounce_size, color_mod);
                }
            }

            // Draw detonation circle if it hasn't been erased yet
            if (start_idx < traj.points.size() - 1) {
                esp_renderer.add_circle_instance(traj.end_pos, cfg.trajectory_detonation_radius, color_mod);
            }
        }
    }

    // Draw active smoke voxels if debug_visibility is enabled
    if (cfg.debug_visibility) {
        if (render_result.smoke_count > 0) {
            static double last_smoke_draw_log = 0.0;
            const double now = glfwGetTime();
            if (now - last_smoke_draw_log > 1.0) {
                last_smoke_draw_log = now;
                BUNNYWARE_LOG_INFO("[DEBUG-VISIBILITY] Smoke voxel debug: drawing {} smoke volume(s)",
                                 render_result.smoke_count);
            }
        }

        ImDrawList* draw_list = nullptr;
        int screen_w = 0, screen_h = 0;
        if (overlay && overlay->get_imgui_context()) {
            ImGui::SetCurrentContext(overlay->get_imgui_context());
            draw_list = ImGui::GetBackgroundDrawList();
            screen_w = overlay->get_width();
            screen_h = overlay->get_height();
        }

        for (int i = 0; i < render_result.smoke_count; ++i) {
            const auto& smoke = render_result.smokes[i];
            if (!smoke.active) continue;

            glm::vec3 center = { smoke.center.x, smoke.center.y, smoke.center.z };
            constexpr float size_scale = 20.0f;
            constexpr float half_scale = 16.0f;

            for (uint32_t w = 0; w < 512; ++w) {
                uint64_t bits = smoke.bitset[w];
                while (bits) {
                    uint32_t bit_idx = __builtin_ctzll(bits);
                    bits &= (bits - 1);

                    uint32_t morton = (w << 6) | bit_idx;
                    int x = 0, y = 0, z = 0;
                    morton3_decode(morton, x, y, z);

                    int lin = x + 32 * (y + 32 * z);
                    uint8_t alpha = smoke.alphas[lin];
                    if (alpha < 15) continue;

                    glm::vec3 voxel_pos = {
                        center.x + (static_cast<float>(x) - half_scale) * size_scale,
                        center.y + (static_cast<float>(y) - half_scale) * size_scale,
                        center.z + (static_cast<float>(z) - half_scale) * size_scale
                    };

                    float norm_alpha = static_cast<float>(alpha) / 255.0f;
                    float smoke_color[4] = { 0.5f, 0.5f, 0.9f, norm_alpha * (15.0f / 255.0f) };

                    esp_renderer.add_box_instance(voxel_pos, 20.0f, smoke_color);

                    if (draw_list) {
                        float sx = 0.0f, sy = 0.0f;
                        Vec3 voxel_v3 = { voxel_pos.x, voxel_pos.y, voxel_pos.z };
                        if (world_to_screen(voxel_v3, &sx, &sy, view_matrix, screen_w, screen_h)) {
                            char alpha_str[16];
                            std::snprintf(alpha_str, sizeof(alpha_str), "%u", alpha);
                            ImVec2 text_size = ImGui::CalcTextSize(alpha_str);
                            
                            ImU32 text_shadow_col = IM_COL32(0, 0, 0, 200);
                            ImU32 text_col = IM_COL32(230, 240, 255, 220);
                            
                            draw_list->AddText(ImVec2(sx - text_size.x * 0.5f + 1.0f, sy - text_size.y * 0.5f + 1.0f), text_shadow_col, alpha_str);
                            draw_list->AddText(ImVec2(sx - text_size.x * 0.5f, sy - text_size.y * 0.5f), text_col, alpha_str);
                        }
                    }
                }
            }
        }
    }

    }

    esp_renderer.flush_lines();
    esp_renderer.flush_instances();
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);

    // E) Draw Warnings, Badges, Timers, and Inferno Polygons via ImGui context
    auto& tracked_infernos = tracker.get_tracked_infernos();
    auto& tracked_warnings = tracker.get_tracked_warnings();

    if (overlay && overlay->get_imgui_context()) {
        ImGui::SetCurrentContext(overlay->get_imgui_context());
        ImDrawList* draw_list = ImGui::GetBackgroundDrawList();
        double current_real_time = glfwGetTime();

        // 1. Render ground fire polygons and/or active Molotov timers.
        if (cfg.inferno_spread_enabled || cfg.grenade_warnings_enabled) {
            for (int i = 0; i < render_result.inferno_count; ++i) {
                const auto& inf_data = render_result.infernos[i];
                
                if (cfg.debug_visibility && inf_data.active && inf_data.fire_count > 0) {
                    static double last_inferno_render_log_time = 0;
                    if (current_real_time - last_inferno_render_log_time > 1.0) {
                        BUNNYWARE_LOG_INFO("[DEBUG-VISIBILITY] GrenadeRenderer: Rendering inferno {} (0x{:08X}), fire_count: {}, fire[0]: ({:.1f}, {:.1f}, {:.1f})",
                                     inf_data.entity_handle, inf_data.entity_handle, inf_data.fire_count,
                                     inf_data.fire_positions[0].x, inf_data.fire_positions[0].y, inf_data.fire_positions[0].z);
                        last_inferno_render_log_time = current_real_time;
                    }
                }
                
                if (!inf_data.active || inf_data.fire_count <= 0) continue;

                float remaining = 0.0f;
                for (const auto& inf : tracked_infernos) {
                    if (inf.entity_handle == inf_data.entity_handle) {
                        double elapsed = current_real_time - inf.first_seen_time;
                        remaining = std::max(0.0f, inf.duration - static_cast<float>(elapsed));
                        break;
                    }
                }

                if (cfg.inferno_spread_enabled) {
                    std::vector<ImVec2> screen_points;
                    screen_points.reserve(inf_data.fire_count * 8);
                    for (int j = 0; j < inf_data.fire_count; ++j) {
                        const auto& fp = inf_data.fire_positions[j];
                        for (int s = 0; s < 8; ++s) {
                            float angle = (static_cast<float>(s) / 8.0f) * 3.14159265f * 2.0f;
                            Vec3 wx(
                                fp.x + std::cos(angle) * 45.0f,
                                fp.y + std::sin(angle) * 45.0f,
                                fp.z
                            );
                            float sx = 0.0f, sy = 0.0f;
                            if (world_to_screen(wx, &sx, &sy, view_matrix, overlay->get_width(), overlay->get_height())) {
                                screen_points.push_back(ImVec2(sx, sy));
                            }
                        }
                    }

                    if (screen_points.size() >= 3) {
                        std::vector<ImVec2> hull = get_convex_hull(screen_points);
                        if (hull.size() >= 3) {
                            draw_list->AddConvexPolyFilled(hull.data(), hull.size(), imcol32_from_color(cfg.inferno_fill_color));
                            draw_list->AddPolyline(hull.data(), hull.size(), imcol32_from_color(cfg.inferno_outline_color), ImDrawFlags_Closed, 2.0f);
                        }
                    }
                }

                // Average fire positions for badge center
                Vec3 center(0.0f);
                for (int j = 0; j < inf_data.fire_count; ++j) {
                    center += inf_data.fire_positions[j];
                }
                if (inf_data.fire_count > 0) {
                    center /= static_cast<float>(inf_data.fire_count);
                }

                float csx = 0.0f, csy = 0.0f;
                const bool within_warning_distance =
                    distance_between(viewer_position, center) <= cfg.grenade_warning_distance;
                if (cfg.grenade_warnings_enabled && within_warning_distance
                    && world_to_screen(center, &csx, &csy, view_matrix, overlay->get_width(), overlay->get_height())) {
                    const ImU32 color = imcol32_from_color(cfg.grenade_color_molotov);
                    const float scale = timer_scale_for_distance(viewer_position, center, cfg.world_timer_scale);
                    if (cfg.world_timer_minimal_style) {
                        draw_minimal_timer_bar(draw_list, csx, csy, GRENADE_MOLOTOV, remaining, inf_data.duration, color, 1.0f, scale);
                    } else {
                        draw_badge_with_timer(draw_list, csx, csy, GRENADE_MOLOTOV, remaining, inf_data.duration, color, 1.0f, svg_cache, scale);
                    }
                }
            }
        }

        // 2. Render warning badges for in-flight active projectiles
        if (cfg.grenade_warnings_enabled) {
            for (const auto& warn : tracked_warnings) {
                if (warn.fade_alpha <= 0.0f) continue;
                if (distance_between(viewer_position, warn.position) > cfg.grenade_warning_distance) continue;

                float sx = 0.0f, sy = 0.0f;
                if (world_to_screen(warn.position, &sx, &sy, view_matrix, overlay->get_width(), overlay->get_height())) {
                    float remaining = 0.0f;
                    float max_duration = 0.0f;

                    if (warn.type == GRENADE_HE || warn.type == GRENADE_FLASH || warn.type == GRENADE_MOLOTOV) {
                        float duration = (warn.type == GRENADE_MOLOTOV) ? 2.02f : 1.5f;
                        remaining = std::max(0.0f, duration - static_cast<float>(current_real_time - warn.first_seen_time));
                        max_duration = duration;
                        if (remaining > cfg.grenade_warning_time) continue;
                    } else if (warn.type == GRENADE_SMOKE || warn.type == GRENADE_DECOY) {
                        if (warn.timer_trigger_real_time > 0.0) {
                            float elapsed = static_cast<float>(current_real_time - warn.timer_trigger_real_time);
                            remaining = std::max(0.0f, warn.timer_duration - elapsed);
                            max_duration = warn.timer_duration;
                        } else {
                            remaining = warn.timer_duration;
                            max_duration = warn.timer_duration;
                        }
                    }

                    ImU32 badge_color = imcol32_from_color(get_grenade_color(warn.type, cfg), warn.fade_alpha);
                    const float scale = timer_scale_for_distance(viewer_position, warn.position, cfg.world_timer_scale);
                    if (cfg.world_timer_minimal_style) {
                        draw_minimal_timer_bar(draw_list, sx, sy, warn.type, remaining, max_duration, badge_color, warn.fade_alpha, scale);
                    } else {
                        draw_badge_with_timer(draw_list, sx, sy, warn.type, remaining, max_duration, badge_color, warn.fade_alpha, svg_cache, scale);
                    }

                    if (warn.affected_count > 0 && (warn.type == GRENADE_HE || warn.type == GRENADE_FLASH || warn.type == GRENADE_MOLOTOV)) {
                        draw_impact_warning(draw_list, sx, sy + 48.0f, warn.affected_count, warn.fade_alpha);
                    }
                }
            }
        }

        // 3. Render planted C4 warning badge
        if (cfg.c4_render_in_world && planted_c4.active && !planted_c4.is_defused && !planted_c4.has_exploded && planted_c4.time_left > 0.0f) {
            float sx = 0.0f, sy = 0.0f;
            if (world_to_screen(planted_c4.position, &sx, &sy, view_matrix, overlay->get_width(), overlay->get_height())) {
                float remaining = planted_c4.time_left;
                float max_duration = 40.0f;

                ImU32 badge_color;
                if (planted_c4.is_being_defused) {
                    if (planted_c4.defuse_time_left > remaining) {
                        // Will explode! Show bright red
                        badge_color = IM_COL32(255, 0, 0, 255);
                    } else {
                        // Will be defused! Show light blue/cyan
                        badge_color = IM_COL32(0, 191, 255, 255);
                    }
                } else {
                    if (cfg.c4_timer_gradient_enabled) {
                        float t = std::clamp(remaining / max_duration, 0.0f, 1.0f);
                        float r = cfg.c4_timer_gradient_end.r + (cfg.c4_timer_gradient_start.r - cfg.c4_timer_gradient_end.r) * t;
                        float g = cfg.c4_timer_gradient_end.g + (cfg.c4_timer_gradient_start.g - cfg.c4_timer_gradient_end.g) * t;
                        float b = cfg.c4_timer_gradient_end.b + (cfg.c4_timer_gradient_start.b - cfg.c4_timer_gradient_end.b) * t;
                        float a = cfg.c4_timer_gradient_end.a + (cfg.c4_timer_gradient_start.a - cfg.c4_timer_gradient_end.a) * t;
                        badge_color = IM_COL32(
                            static_cast<int>(std::clamp(r, 0.0f, 1.0f) * 255.0f),
                            static_cast<int>(std::clamp(g, 0.0f, 1.0f) * 255.0f),
                            static_cast<int>(std::clamp(b, 0.0f, 1.0f) * 255.0f),
                            static_cast<int>(std::clamp(a, 0.0f, 1.0f) * 255.0f)
                        );
                    } else {
                        badge_color = IM_COL32(
                            static_cast<int>(std::clamp(cfg.c4_timer_color.r, 0.0f, 1.0f) * 255.0f),
                            static_cast<int>(std::clamp(cfg.c4_timer_color.g, 0.0f, 1.0f) * 255.0f),
                            static_cast<int>(std::clamp(cfg.c4_timer_color.b, 0.0f, 1.0f) * 255.0f),
                            static_cast<int>(std::clamp(cfg.c4_timer_color.a, 0.0f, 1.0f) * 255.0f)
                        );
                    }
                }

                if (cfg.world_timer_minimal_style) {
                    draw_minimal_timer_bar(draw_list, sx, sy, GRENADE_C4, remaining, max_duration, badge_color, 1.0f,
                                           timer_scale_for_distance(viewer_position, planted_c4.position, cfg.world_timer_scale));
                } else {
                    draw_badge_with_timer(draw_list, sx, sy, GRENADE_C4, remaining, max_duration, badge_color, 1.0f, svg_cache,
                                          timer_scale_for_distance(viewer_position, planted_c4.position, cfg.world_timer_scale));
                }
            }
        }
    }

    // Prune GPU trajectory buffers that are no longer active or fading
    std::vector<uint32_t> active_handles;
    for (const auto& tracked : tracked_active_trajectories) {
        if (tracked.still_active) {
            active_handles.push_back(tracked.entity_handle);
        }
    }
    for (const auto& fading : fading_trajectories) {
        active_handles.push_back(fading.traj.entity_handle);
    }
    esp_renderer.prune_gpu_trajectories(active_handles);
}
