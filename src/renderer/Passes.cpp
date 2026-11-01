#include "Passes.hpp"
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <cstring>
#include <algorithm>
#include <cmath>
#include "math/BoneMath.hpp"
#include "math/ViewMatrix.hpp"
#include "logger.hpp"
#include "HighlightTint.hpp"
#include "overlay/esp_drawing.hpp"

void ChamsPass::run(const ChamsPassInputs& inputs) {
    const auto& input = inputs.input;
    const auto& state = inputs.state;
    const auto& cfg = inputs.cfg;
    auto& chams_renderer = inputs.chams_renderer;
    auto& depth_prepass = inputs.depth_prepass;
    auto& esp_renderer = inputs.esp_renderer;
    int window_width = inputs.window_width;
    int window_height = inputs.window_height;

    // 1. Run Map Depth Prepass only for mesh features that depth-test against the map.
    // Skeleton ESP uses BVH bone visibility (whole skeleton one color) — not GPU depth.
    int style_vis_id_early = get_style_id(cfg.style_vis);
    int style_invis_id_early = get_style_id(cfg.style_invis);
    bool mesh_needs_depth = (style_vis_id_early > 0) || (style_invis_id_early > 0)
        || (cfg.glow_enabled && cfg.outline_mode == "glow")
        || (cfg.glow_enabled && cfg.outline_mode == "stencil");

    bool needs_prepass = false;
    if (cfg.map_visualizer_enabled && cfg.map_visualizer_depth_tested) {
        needs_prepass = true;
    }
    if (!needs_prepass && !inputs.active_kill_chams.empty() && cfg.kill_effect.value == KillFxModeEnum::CHAMS) {
        needs_prepass = true;
    }
    if (!needs_prepass && !inputs.active_hit_chams.empty() && cfg.hit_effect_mode.value == KillFxModeEnum::CHAMS) {
        needs_prepass = true;
    }
    if (!needs_prepass && mesh_needs_depth) {
        for (int i : state.sorted_player_indices) {
            bool matches_team = (input.packet.players[i].team == input.packet.local_team);
            if (cfg.team_check && matches_team) continue;
            needs_prepass = true;
            break;
        }
    }

    bool has_prepass = cfg.use_depth_prepass && depth_prepass.has_geometry() && needs_prepass;
    if (has_prepass) {
        if (inputs.profiler) inputs.profiler->begin_section(GpuTimerSection::DEPTH_PREPASS);
        depth_prepass.render(input.gl_vp);
        if (inputs.profiler) inputs.profiler->end_section();
    }

    // Resolve styles in real-time
    int style_vis_id = get_style_id(cfg.style_vis);
    int style_invis_id = get_style_id(cfg.style_invis);

    // 2. Render Player Glow / Skeleton Silhouette
    bool run_glow = (cfg.glow_enabled && cfg.outline_mode == "glow") || (cfg.esp_enabled && cfg.esp_skeleton && cfg.esp_skeleton_glow);
    if (run_glow) {
        if (inputs.profiler) inputs.profiler->begin_section(GpuTimerSection::GLOW_SILHOUETTE);
        chams_renderer.begin_glow_pass(window_width, window_height, input.gl_vp, input.cam_pos_arr);
        
        if (cfg.glow_enabled && cfg.outline_mode == "glow") {
            size_t idx = 0;
            while (idx < state.sorted_player_indices.size()) {
                int first_player_idx = state.sorted_player_indices[idx];
                const auto& first_rp = state.render_palettes[first_player_idx];
                
                bool matches_team = (input.packet.players[first_player_idx].team == input.packet.local_team);
                if (cfg.team_check && matches_team) {
                    idx++;
                    continue;
                }
                
                if (!(first_rp.is_visible || (cfg.style_invis.value != ChamsStyleEnum::DISABLED)) || first_rp.hidden(1)) {
                    idx++;
                    continue;
                }

                unsigned int batch_vao = first_rp.model->vao;
                unsigned int batch_ibo = first_rp.model->ibo;
                size_t batch_index_count = first_rp.model->index_count;

                int ubo_slots[8];
                float colors[8 * 4];
                int count = 0;
                int base_slot = -1;

                size_t next_idx = idx;
                while (next_idx < state.sorted_player_indices.size() && count < 8) {
                    int p_idx = state.sorted_player_indices[next_idx];
                    const auto& rp = state.render_palettes[p_idx];

                    if (rp.model->vao != batch_vao) {
                        break;
                    }

                    bool p_matches_team = (input.packet.players[p_idx].team == input.packet.local_team);
                    if (cfg.team_check && p_matches_team) {
                        next_idx++;
                        continue;
                    }

                    if (!(rp.is_visible || (cfg.style_invis.value != ChamsStyleEnum::DISABLED)) || rp.hidden(1)) {
                        next_idx++;
                        continue;
                    }

                    int slot = state.player_ubo_slots[p_idx];
                    if (base_slot < 0) {
                        base_slot = slot;
                    }

                    int current_min = base_slot;
                    int current_max = base_slot;
                    for (int k = 0; k < count; ++k) {
                        if (ubo_slots[k] < current_min) current_min = ubo_slots[k];
                        if (ubo_slots[k] > current_max) current_max = ubo_slots[k];
                    }
                    int next_min = std::min(current_min, slot);
                    int next_max = std::max(current_max, slot);
                    if (next_max - next_min >= 8) {
                        break;
                    }

                    ubo_slots[count] = slot;

                    float custom_color[4];
                    if (cfg.glow_health_based) {
                        float hp_factor = static_cast<float>(input.packet.players[p_idx].health) / 100.0f;
                        if (hp_factor < 0.0f) hp_factor = 0.0f;
                        if (hp_factor > 1.0f) hp_factor = 1.0f;
                        for (int c = 0; c < 4; ++c) {
                            custom_color[c] = cfg.glow_health_end[c] + hp_factor * (cfg.glow_health_start[c] - cfg.glow_health_end[c]);
                        }
                    } else {
                        std::memcpy(custom_color, cfg.glow_color, sizeof(float) * 4);
                    }
                    std::memcpy(&colors[count * 4], custom_color, sizeof(float) * 4);
                    tint_highlight(&colors[count * 4], rp.highlight_alpha[1] * rp.distance_alpha, rp.tint(1));

                    count++;
                    next_idx++;
                }

                if (count > 0) {
                    chams_renderer.render_glow_silhouette_instanced(batch_vao, batch_ibo, batch_index_count,
                                                                    ubo_slots, colors, count);
                }
                idx = next_idx;
            }
        }

        if (cfg.esp_enabled && cfg.esp_skeleton && cfg.esp_skeleton_glow) {
            GLenum drawBuffer = GL_COLOR_ATTACHMENT0;
            glDrawBuffers(1, &drawBuffer);

            esp_renderer.clear();
            esp_renderer.set_projection(input.gl_vp);

            float pulse_factor = 1.0f;
            if (cfg.esp_skeleton_glow_pulse) {
                float time = static_cast<float>(glfwGetTime());
                pulse_factor = 0.625f + 0.375f * std::sin(time * cfg.esp_skeleton_glow_pulse_speed);
            }

            for (int i = 0; i < input.packet.player_count; ++i) {
                const auto& rp = state.render_palettes[i];
                if (!rp.model || rp.hidden(2)) continue;

                bool matches_team = (input.packet.players[i].team == input.packet.local_team);
                if (cfg.team_check && matches_team) continue;

                bool is_visible = false;
                if (i < static_cast<int>(state.vischeck_result.player_visible.size())) {
                    is_visible = state.vischeck_result.player_visible[i];
                } else {
                    is_visible = true;
                }

                if (is_visible && !cfg.esp_box_hp_visible) continue;
                if (!is_visible && !cfg.esp_box_hp_occluded) continue;

                float custom_glow_color[4];
                const float* glow_color_ptr = nullptr;
                if (cfg.esp_skeleton_glow_health_based) {
                    float hp_factor = static_cast<float>(input.packet.players[i].health) / 100.0f;
                    if (hp_factor < 0.0f) hp_factor = 0.0f;
                    if (hp_factor > 1.0f) hp_factor = 1.0f;

                    for (int c = 0; c < 4; ++c) {
                        custom_glow_color[c] = cfg.esp_skeleton_glow_health_end[c] + hp_factor * (cfg.esp_skeleton_glow_health_start[c] - cfg.esp_skeleton_glow_health_end[c]);
                    }
                    glow_color_ptr = custom_glow_color;
                }

                const auto mark = esp_renderer.mark();
                esp_renderer.add_skeleton_3d(rp.sanitized_bones, i, state.dummy_vis, cfg, true, pulse_factor, glow_color_ptr);
                esp_renderer.multiply_alpha_since(mark, rp.distance_alpha);
                if (rp.highlighted(2)) esp_renderer.tint_since(mark, rp.highlight_alpha[2], rp.tint(2));
            }

            esp_renderer.flush_lines();

            GLenum drawBuffers[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
            glDrawBuffers(2, drawBuffers);
        }

        float current_thickness = cfg.glow_enabled ? cfg.glow_thickness : cfg.esp_skeleton_glow_thickness;
        float current_intensity = cfg.glow_enabled ? cfg.glow_intensity : cfg.esp_skeleton_glow_intensity;
        bool should_pulse = cfg.glow_enabled ? cfg.glow_pulse : cfg.esp_skeleton_glow_pulse;
        float pulse_speed = cfg.glow_enabled ? cfg.glow_pulse_speed : cfg.esp_skeleton_glow_pulse_speed;

        if (should_pulse) {
            float time = static_cast<float>(glfwGetTime());
            float factor = 0.625f + 0.375f * std::sin(time * pulse_speed);
            current_intensity *= factor;
        }
        if (inputs.profiler) inputs.profiler->end_section();

        if (inputs.profiler) inputs.profiler->begin_section(GpuTimerSection::GLOW_POSTPROCESS);
        chams_renderer.end_glow_pass(window_width, window_height, current_thickness, current_intensity);
        if (inputs.profiler) inputs.profiler->end_section();
    }

    // 3. Body Pass
    bool is_stencil_outline = cfg.glow_enabled && cfg.outline_mode == "stencil";
    bool needs_stencil = cfg.flat_chams_no_overlap || is_stencil_outline;

    if (inputs.profiler) inputs.profiler->begin_section(GpuTimerSection::BODY_PASS);

    chams_renderer.begin_body_pass(input.gl_vp, input.cam_pos_arr, needs_stencil);

    auto draw_player_batches = [&](const std::vector<int>& target_indices, int style_id, const float* primary_color, const float* secondary_color) {
        if (target_indices.empty()) return;
        if (style_id <= 0 && !(cfg.glow_enabled && cfg.outline_mode == "stencil")) return;

        size_t idx = 0;
        while (idx < target_indices.size()) {
            int first_player_idx = target_indices[idx];
            const auto& first_rp = state.render_palettes[first_player_idx];

            unsigned int batch_vao = first_rp.model->vao;
            unsigned int batch_ibo = first_rp.model->ibo;
            size_t batch_index_count = first_rp.model->index_count;

            int ubo_slots[8];
            float colors[8 * 4];
            float secondary_colors[8 * 4];
            float outline_colors[8 * 4];
            int count = 0;
            int base_slot = -1;

            size_t next_idx = idx;
            while (next_idx < target_indices.size() && count < 8) {
                int p_idx = target_indices[next_idx];
                const auto& rp = state.render_palettes[p_idx];

                if (rp.model->vao != batch_vao) {
                    break;
                }

                int slot = state.player_ubo_slots[p_idx];
                if (base_slot < 0) {
                    base_slot = slot;
                }

                int current_min = base_slot;
                int current_max = base_slot;
                for (int k = 0; k < count; ++k) {
                    if (ubo_slots[k] < current_min) current_min = ubo_slots[k];
                    if (ubo_slots[k] > current_max) current_max = ubo_slots[k];
                }
                int next_min = std::min(current_min, slot);
                int next_max = std::max(current_max, slot);
                if (next_max - next_min >= 8) {
                    break;
                }

                ubo_slots[count] = slot;
                std::memcpy(&colors[count * 4], primary_color, sizeof(float) * 4);
                std::memcpy(&secondary_colors[count * 4],
                            secondary_color ? secondary_color : primary_color,
                            sizeof(float) * 4);
                float resolved_outline_color[4];
                if (cfg.glow_enabled && cfg.outline_mode == "stencil") {
                    if (cfg.glow_health_based) {
                        float hp_factor = static_cast<float>(input.packet.players[p_idx].health) / 100.0f;
                        if (hp_factor < 0.0f) hp_factor = 0.0f;
                        if (hp_factor > 1.0f) hp_factor = 1.0f;
                        for (int c = 0; c < 4; ++c) {
                            resolved_outline_color[c] = cfg.glow_health_end[c] + hp_factor * (cfg.glow_health_start[c] - cfg.glow_health_end[c]);
                        }
                    } else {
                        std::memcpy(resolved_outline_color, cfg.glow_color, sizeof(float) * 4);
                    }
                } else {
                    std::memcpy(resolved_outline_color, cfg.glow_color, sizeof(float) * 4);
                }
                std::memcpy(&outline_colors[count * 4], resolved_outline_color, sizeof(float) * 4);
                tint_highlight(&colors[count * 4], rp.highlight_alpha[0] * rp.distance_alpha, rp.tint(0));
                tint_highlight(&secondary_colors[count * 4], rp.highlight_alpha[0] * rp.distance_alpha, rp.tint(0));
                tint_highlight(&outline_colors[count * 4], rp.highlight_alpha[1] * rp.distance_alpha, rp.tint(1));

                count++;
                next_idx++;
            }

            if (count > 0) {
                bool is_stencil_outline = cfg.glow_enabled && cfg.outline_mode == "stencil";
                chams_renderer.render_mesh_instanced(
                    batch_vao, batch_ibo, batch_index_count,
                    style_id, is_stencil_outline ? cfg.glow_thickness : 0.0f,
                    cfg.flat_chams_no_overlap, ubo_slots, colors,
                    secondary_colors, outline_colors, count,
                    is_stencil_outline
                );
            }
            idx = next_idx;
        }
    };

    auto body_hidden = [&](const FrameState::RenderPalette& rp) {
        return rp.hidden(0) && (!is_stencil_outline || rp.hidden(1));
    };

    if (has_prepass) {
        static std::vector<int> chams_players;
        chams_players.clear();
        
        for (int i : state.sorted_player_indices) {
            bool matches_team = (input.packet.players[i].team == input.packet.local_team);
            if (cfg.team_check && matches_team) continue;
            if (body_hidden(state.render_palettes[i])) continue;
            chams_players.push_back(i);
        }

        if ((style_invis_id > 0 || is_stencil_outline) && (cfg.style_invis.value != ChamsStyleEnum::DISABLED)) {
            glDepthFunc(GL_GREATER);
            glDepthMask(GL_FALSE);
            draw_player_batches(chams_players, style_invis_id, cfg.color_invis, cfg.color_invis_sec);
        }
        
        if (style_vis_id > 0 || is_stencil_outline) {
            glDepthFunc(GL_LEQUAL);
            glDepthMask(GL_TRUE);
            draw_player_batches(chams_players, style_vis_id, cfg.color_vis, cfg.color_vis_sec);
        }
    } 
    else {
        static std::vector<int> vis_pass_vis_color;
        static std::vector<int> vis_pass_invis_color;
        vis_pass_vis_color.clear();
        vis_pass_invis_color.clear();
        
        for (int i : state.sorted_player_indices) {
            const auto& rp = state.render_palettes[i];
            bool matches_team = (input.packet.players[i].team == input.packet.local_team);
            if (cfg.team_check && matches_team) continue;
            if (body_hidden(rp)) continue;

            bool check_vis = rp.is_visible && !rp.is_occluded_by_smoke;
            
            if (check_vis) {
                vis_pass_vis_color.push_back(i);
            } else {
                vis_pass_invis_color.push_back(i);
            }
        }

        if (style_vis_id > 0 || is_stencil_outline) {
            glDepthFunc(GL_LEQUAL);
            glDepthMask(GL_TRUE);
            draw_player_batches(vis_pass_vis_color, style_vis_id, cfg.color_vis, cfg.color_vis_sec);
        }

        if ((style_invis_id > 0 || is_stencil_outline) && (cfg.style_invis.value != ChamsStyleEnum::DISABLED)) {
            glDepthFunc(GL_LEQUAL);
            glDepthMask(GL_TRUE);
            draw_player_batches(vis_pass_invis_color, style_invis_id, cfg.color_invis, cfg.color_invis_sec);
        }
    }

    // Render Dead Player Chams (Kill FX) and the Hit FX ghost afterimage.
    const auto render_fading_chams = [&](const std::vector<DeadPlayerCham>& chams, double current_time) {
        glDepthFunc(GL_LEQUAL);
        glDepthMask(GL_FALSE); // Disable depth writes for fading transparent chams to avoid depth artifacts
        for (const auto& cham : chams) {
            float elapsed = static_cast<float>(current_time - cham.kill_time);
            float alpha_factor = 1.0f - (elapsed / cham.fade_time);
            if (alpha_factor < 0.0f) alpha_factor = 0.0f;
            if (alpha_factor > 1.0f) alpha_factor = 1.0f;

            const float distance_meters = distance::meters_between(cham.position, input.packet.local_eye);
            if (is_beyond_esp_distance(distance_meters, cfg)) continue;
            const float distance_alpha = esp_distance_alpha(distance_meters, cfg);
            if (distance_alpha <= 0.0f) continue;

            float fade_color[4] = {
                cham.color[0],
                cham.color[1],
                cham.color[2],
                cham.color[3] * alpha_factor * distance_alpha
            };
            float fade_secondary_color[4] = {
                cham.secondary_color[0],
                cham.secondary_color[1],
                cham.secondary_color[2],
                cham.secondary_color[3] * alpha_factor * distance_alpha
            };

            chams_renderer.render_mesh(
                cham.vao, cham.ibo, cham.index_count,
                cham.skinning_palette, fade_color, cham.style_id,
                fade_secondary_color,
                0.0f, false,
                cham.assigned_ubo_slot
            );
        }
    };

    if (cfg.kill_effect == KillFxModeEnum::CHAMS) {
        render_fading_chams(inputs.active_kill_chams, glfwGetTime());
    }
    if (cfg.hit_effect_mode == KillFxModeEnum::CHAMS) {
        render_fading_chams(inputs.active_hit_chams, glfwGetTime());
    }

    chams_renderer.end_body_pass();

    glDepthMask(GL_TRUE); // Restore depth writes

    if (inputs.profiler) inputs.profiler->end_section();

    // 4. Draw 3D Skeleton — BVH key-bone visibility only (no map depth prepass).
    // If any visibility bone is clear, the whole skeleton uses the visible color.
    if ((cfg.esp_enabled && cfg.esp_skeleton) || cfg.trigger_show_hitboxes) {
        if (inputs.profiler) inputs.profiler->begin_section(GpuTimerSection::SKELETON_ESP);

        const bool both_layers = cfg.esp_box_hp_visible && cfg.esp_box_hp_occluded;
        const bool use_vis_delay = both_layers && cfg.esp_skeleton_vis_delay_enabled
            && cfg.esp_skeleton_vis_delay_ms > 0.0f;
        const double now = glfwGetTime();
        const double delay_s = static_cast<double>(cfg.esp_skeleton_vis_delay_ms) / 1000.0;

        // Per-player delayed raw_vis → shown_vis (only delays invisible → visible)
        static std::vector<double> skel_visible_since;
        static std::vector<uint8_t> skel_was_raw_vis;
        if (static_cast<int>(skel_visible_since.size()) < input.packet.player_count) {
            skel_visible_since.resize(input.packet.player_count, -1.0);
            skel_was_raw_vis.resize(input.packet.player_count, 0);
        }

        static std::vector<int> highlighted_skels;
        auto draw_skels = [&](bool for_vis) {
            esp_renderer.clear();
            esp_renderer.set_projection(input.gl_vp);
            highlighted_skels.clear();
            
            for (int i = 0; i < input.packet.player_count; ++i) {
                const auto& rp = state.render_palettes[i];
                if (!rp.model || rp.hidden(2)) continue;

                bool matches_team = (input.packet.players[i].team == input.packet.local_team);
                if (cfg.team_check && matches_team) continue;

                // BVH: any key bone visible ⇒ whole skeleton treated as visible
                const bool raw_vis = rp.is_visible && !rp.is_occluded_by_smoke;

                bool check_vis = raw_vis;
                if (use_vis_delay) {
                    if (!raw_vis) {
                        skel_was_raw_vis[i] = 0;
                        skel_visible_since[i] = -1.0;
                        check_vis = false;
                    } else {
                        if (!skel_was_raw_vis[i]) {
                            skel_was_raw_vis[i] = 1;
                            skel_visible_since[i] = now;
                        }
                        check_vis = (now - skel_visible_since[i]) >= delay_s;
                    }
                }

                if (for_vis && !check_vis) continue;
                if (!for_vis && check_vis) continue;

                if (check_vis) {
                    if (!cfg.esp_box_hp_visible) continue;
                } else {
                    if (!cfg.esp_box_hp_occluded) continue;
                }

                if (cfg.esp_enabled && cfg.esp_skeleton) {
                    if (rp.highlighted(2)) {
                        highlighted_skels.push_back(i);
                    } else {
                        const auto mark = esp_renderer.mark();
                        esp_renderer.add_skeleton_3d(rp.sanitized_bones, i, state.dummy_vis, cfg, false, 1.0f, nullptr);
                        esp_renderer.multiply_alpha_since(mark, rp.distance_alpha);
                    }
                }

                if (cfg.trigger_show_hitboxes) {
                    if (rp.sanitized_bones.size() >= 24) {
                        auto get_bone_pos = [&](int id) -> Vec3 {
                            return rp.sanitized_bones[id];
                        };

                        struct CapsuleDef {
                            int b1, b2;
                            float base_radius;
                            int ref_bone;
                        };

                        static const CapsuleDef defs[11] = {
                            {7, 7, 4.2f, 7},   // Head (Sphere)
                            {23, 1, 6.0f, 1},  // Torso (Main)
                            {6, 23, 4.0f, 6},  // Torso (Neck Transition)
                            {17, 18, 4.2f, 17}, // Left Leg Upper
                            {18, 19, 3.5f, 18}, // Left Leg Lower
                            {20, 21, 4.2f, 20}, // Right Leg Upper
                            {21, 22, 3.5f, 21}, // Right Leg Lower
                            {9, 10, 3.0f, 9},  // Left Arm Upper
                            {10, 11, 2.5f, 10}, // Left Arm Lower
                            {13, 14, 3.0f, 13}, // Right Arm Upper
                            {14, 15, 2.5f, 14}  // Right Arm Lower
                        };

                        uint16_t weapon_id = input.packet.held_weapon_id;
                        WeaponGroup w_group = get_weapon_group(weapon_id);
                        float active_scale = 1.0f;
                        int active_hitbox_groups = 63;
                        if (w_group == WEAPON_GROUP_LIGHT_PISTOL) {
                            active_scale = cfg.trigger_light_pistols_hitbox_scale;
                            active_hitbox_groups = cfg.trigger_light_pistols_hitbox_groups;
                        } else if (w_group == WEAPON_GROUP_HEAVY_PISTOL) {
                            active_scale = cfg.trigger_heavy_pistols_hitbox_scale;
                            active_hitbox_groups = cfg.trigger_heavy_pistols_hitbox_groups;
                        } else if (w_group == WEAPON_GROUP_RIFLE) {
                            active_scale = cfg.trigger_rifles_hitbox_scale;
                            active_hitbox_groups = cfg.trigger_rifles_hitbox_groups;
                        } else if (w_group == WEAPON_GROUP_SMG) {
                            active_scale = cfg.trigger_smgs_hitbox_scale;
                            active_hitbox_groups = cfg.trigger_smgs_hitbox_groups;
                        } else if (w_group == WEAPON_GROUP_SCOUT) {
                            active_scale = cfg.trigger_scout_hitbox_scale;
                            active_hitbox_groups = cfg.trigger_scout_hitbox_groups;
                        } else if (w_group == WEAPON_GROUP_AWP) {
                            active_scale = cfg.trigger_awp_hitbox_scale;
                            active_hitbox_groups = cfg.trigger_awp_hitbox_groups;
                        } else if (w_group == WEAPON_GROUP_SNIPER) {
                            active_scale = cfg.trigger_auto_snipers_hitbox_scale;
                            active_hitbox_groups = cfg.trigger_auto_snipers_hitbox_groups;
                        } else if (w_group == WEAPON_GROUP_SHOTGUN) {
                            active_scale = cfg.trigger_shotguns_hitbox_scale;
                            active_hitbox_groups = cfg.trigger_shotguns_hitbox_groups;
                        } else if (w_group == WEAPON_GROUP_MG) {
                            active_scale = cfg.trigger_lmgs_hitbox_scale;
                            active_hitbox_groups = cfg.trigger_lmgs_hitbox_groups;
                        } else {
                            active_scale = cfg.trigger_others_hitbox_scale;
                            active_hitbox_groups = cfg.trigger_others_hitbox_groups;
                        }
                        active_scale = std::clamp(active_scale, 0.5f, 1.0f);

                        const float* hb_color = check_vis ? cfg.esp_skeleton_color_vis : cfg.esp_skeleton_color_invis;

                        for (const auto& d : defs) {
                            if (!is_hitbox_group_enabled(d.ref_bone, active_hitbox_groups)) continue;

                            Vec3 p1 = get_bone_pos(d.b1);
                            Vec3 p2 = get_bone_pos(d.b2);
                            float rad = d.base_radius * active_scale;
                            esp_renderer.add_capsule_3d(p1, p2, rad, hb_color, 1.0f);
                        }
                    }
                }
            }

            esp_renderer.upload_lines();
            // Whole skeleton one color from BVH — no fragment depth split
            if (for_vis) {
                esp_renderer.draw_lines_override(cfg.esp_skeleton_color_vis);
            } else {
                esp_renderer.draw_lines_override(cfg.esp_skeleton_color_invis);
            }

            for (int i : highlighted_skels) {
                const auto& rp = state.render_palettes[i];
                float skel_color[4];
                std::memcpy(skel_color, for_vis ? cfg.esp_skeleton_color_vis : cfg.esp_skeleton_color_invis, sizeof(skel_color));
                tint_highlight(skel_color, rp.highlight_alpha[2], rp.tint(2));
                esp_renderer.clear();
                esp_renderer.set_projection(input.gl_vp);
                const auto mark = esp_renderer.mark();
                esp_renderer.add_skeleton_3d(rp.sanitized_bones, i, state.dummy_vis, cfg, false, 1.0f, nullptr);
                esp_renderer.multiply_alpha_since(mark, rp.distance_alpha);
                esp_renderer.upload_lines();
                esp_renderer.draw_lines_override(skel_color);
            }
        };

        glDisable(GL_DEPTH_TEST);
        draw_skels(false); // occluded (or delayed-still-occluded) color
        draw_skels(true);  // visible color
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);

        if (inputs.profiler) inputs.profiler->end_section();
    }

}
