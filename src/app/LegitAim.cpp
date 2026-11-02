#include "LegitAim.hpp"
#include "logger.hpp"
#include <chrono>
#include <cmath>
#include <algorithm>
#include <random>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

struct AimCapsule {
    glm::vec3 start;
    glm::vec3 end;
    float radius;
    int bone;
};



static std::vector<AimCapsule> build_aim_capsules(const PlayerData& player, float scale_factor, int enabled_groups) {
    std::vector<AimCapsule> capsules;
    if (player.bone_count < 24) return capsules;

    auto get_bone_pos = [&](int id) -> glm::vec3 {
        return player.bones[id].position;
    };

    auto add_fallback = [&](int b1, int b2, float radius, int ref_bone) {
        if (is_hitbox_group_enabled(ref_bone, enabled_groups)) {
            glm::vec3 pos1 = get_bone_pos(b1);
            glm::vec3 pos2 = get_bone_pos(b2);
            // Skip invalid/zeroed bone coordinates
            if (glm::length(pos1) < 1.0f || glm::length(pos2) < 1.0f) {
                return;
            }
            capsules.push_back({ pos1, pos2, radius * scale_factor, ref_bone });
        }
    };

    // 1. Head (Sphere at bone 7)
    add_fallback(7, 7, 4.2f, 7);

    // 2. Torso (Spine/Chest 23 to Pelvis 1)
    add_fallback(23, 1, 6.0f, 1);
    // 2b. Neck transition (Neck 6 to Spine/Chest 23)
    add_fallback(6, 23, 4.0f, 6);

    // 3. Left Leg Upper (17 to 18)
    add_fallback(17, 18, 4.2f, 17);
    // 4. Left Leg Lower (18 to 19)
    add_fallback(18, 19, 3.5f, 18);

    // 5. Right Leg Upper (20 to 21)
    add_fallback(20, 21, 4.2f, 20);
    // 6. Right Leg Lower (21 to 22)
    add_fallback(21, 22, 3.5f, 21);

    // 7. Left Arm Upper (9 to 10)
    add_fallback(9, 10, 3.0f, 9);
    // 8. Left Arm Lower (10 to 11)
    add_fallback(10, 11, 2.5f, 10);

    // 9. Right Arm Upper (13 to 14)
    add_fallback(9 + 4, 10 + 4, 3.0f, 13); // Bone 13 to 14
    // 10. Right Arm Lower (14 to 15)
    add_fallback(10 + 4, 11 + 4, 2.5f, 14); // Bone 14 to 15

    return capsules;
}

static bool get_closest_points(const glm::vec3& ray_origin, const glm::vec3& ray_dir,
                               const glm::vec3& capsule_start, const glm::vec3& capsule_end,
                               glm::vec3& out_point_on_ray, glm::vec3& out_point_on_capsule) {
    const auto capsule_vec = capsule_end - capsule_start;
    const auto capsule_length = glm::length(capsule_vec);

    if (capsule_length < 0.001f) {
        out_point_on_capsule = capsule_start;
        const auto to_center = capsule_start - ray_origin;
        const auto projection = glm::dot(to_center, ray_dir);
        out_point_on_ray = ray_origin + ray_dir * std::max(0.0f, projection);
        return true;
    }

    const auto capsule_dir = capsule_vec / capsule_length;
    const auto w = ray_origin - capsule_start;

    const auto a = glm::dot(ray_dir, ray_dir);
    const auto b = glm::dot(ray_dir, capsule_dir);
    const auto c = glm::dot(capsule_dir, capsule_dir);
    const auto d = glm::dot(ray_dir, w);
    const auto e = glm::dot(capsule_dir, w);

    const auto denom = a * c - b * b;

    float s, t;

    if (std::abs(denom) < 0.0001f) {
        s = 0.0f;
        t = (b > c ? d / b : e / c);
    } else {
        s = (b * e - c * d) / denom;
        t = (a * e - b * d) / denom;
    }

    t = std::clamp(t, 0.0f, capsule_length);
    s = std::max(0.0f, s);

    out_point_on_capsule = capsule_start + capsule_dir * t;
    out_point_on_ray = ray_origin + ray_dir * s;
    return true;
}

static float normalize_yaw(float yaw) {
    while (yaw > 180.0f) yaw -= 360.0f;
    while (yaw < -180.0f) yaw += 360.0f;
    return yaw;
}

LegitAim::LegitAim()
{
    try {
        evdev_input = std::make_unique<EvdevInput>();
        input_ok = true;
        BUNNYWARE_LOG_INFO("LegitAim: EvdevInput initialized successfully.");
    } catch (const std::exception& e) {
        init_error = e.what();
        input_ok = false;
        BUNNYWARE_LOG_ERROR("LegitAim: Failed to initialize EvdevInput: {}", init_error);
    }
}

LegitAim::~LegitAim()
{
}

glm::vec3 LegitAim::predict_target_pos(const glm::vec3& pos, const glm::vec3& vel, float tick_rate) const
{
    return pos + vel * tick_rate;
}

void LegitAim::inject_mouse_move(float dx, float dy)
{
    if (input_ok && evdev_input) {
        evdev_input->move_rel(dx, dy);
    }
}

void LegitAim::update(const FrameInput& input, const OverlayConfig& cfg, const VischeckResult& vis_result, const glm::vec3* override_angles)
{
    if (override_angles) {
        if (!input_ok) return;

        // Dynamic sensitivity scale from game memory
        float sensitivity = input.packet.local_sensitivity;
        float fov_adjust = input.packet.local_fov_sensitivity_adjust;
        if (sensitivity <= 0.01f) sensitivity = 1.25f;
        if (fov_adjust <= 0.01f) fov_adjust = 1.0f;

        // CS2 base yaw pitch scale
        constexpr float m_yaw = 0.022f;
        float deg_per_pixel = sensitivity * m_yaw * fov_adjust;

        glm::vec3 angles = {input.packet.local_angles.x, input.packet.local_angles.y, input.packet.local_angles.z};

        float pitch = override_angles->x;
        float yaw = override_angles->y;
        float delta_pitch = pitch - angles.x;
        float delta_yaw = normalize_yaw(yaw - angles.y);
        float fov_dist = std::sqrt(delta_pitch * delta_pitch + delta_yaw * delta_yaw);

        // Stop aiming if we are already right on target
        if (fov_dist < 0.05f) {
            return;
        }

        // Constant smooth for grenade helper overrides (aim_smooth_type == 0)
        float base_smooth = cfg.grenade_helper_aim_speed;
        if (base_smooth < 1.0f) base_smooth = 1.0f;

        float smooth_x = base_smooth * cfg.aim_horizontal_bias;
        float smooth_y = base_smooth;

        if (cfg.aim_random_smooth) {
            static std::mt19937 rng(std::random_device{}());
            float rx = std::clamp(cfg.aim_randomize_smooth_x, 0.0f, smooth_x - 1.0f);
            float ry = std::clamp(cfg.aim_randomize_smooth_y, 0.0f, smooth_y - 1.0f);

            if (rx > 0.0f) {
                std::uniform_real_distribution<float> dist_x(-rx, rx);
                smooth_x += dist_x(rng);
            }
            if (ry > 0.0f) {
                std::uniform_real_distribution<float> dist_y(-ry, ry);
                smooth_y += dist_y(rng);
            }

            if (smooth_x < 1.0f) smooth_x = base_smooth * cfg.aim_horizontal_bias;
            if (smooth_y < 1.0f) smooth_y = base_smooth;
        }

        delta_yaw /= smooth_x;
        delta_pitch /= smooth_y;

        // Muscle overshoot simulation
        if (cfg.aim_overshoot) {
            if (!is_overshooting && fov_dist > (cfg.grenade_helper_aim_fov * 0.3f)) {
                static std::mt19937 rng(std::random_device{}());
                std::uniform_real_distribution<float> dist_p(0.0f, 1.0f);
                if (dist_p(rng) < cfg.aim_overshoot_chance) {
                    is_overshooting = true;
                    current_overshoot_scale = cfg.aim_overshoot_scale;
                    std::uniform_int_distribution<int> dist_t(5, 12);
                    overshoot_ticks_left = dist_t(rng);
                }
            }

            if (is_overshooting) {
                if (overshoot_ticks_left > 0) {
                    delta_yaw *= current_overshoot_scale;
                    delta_pitch *= current_overshoot_scale;
                    overshoot_ticks_left--;
                } else {
                    is_overshooting = false;
                }
            }
        }

        // Translate angular deltas to mouse inputs
        float want_x = -delta_yaw + subpixel_error.x;
        float want_y = delta_pitch + subpixel_error.y;

        float counts_x = std::round(want_x / deg_per_pixel);
        float counts_y = std::round(want_y / deg_per_pixel);

        subpixel_error.x = want_x - counts_x * deg_per_pixel;
        subpixel_error.y = want_y - counts_y * deg_per_pixel;

        if (std::abs(counts_x) > 0.0f || std::abs(counts_y) > 0.0f) {
            inject_mouse_move(counts_x, counts_y);
        }
        return;
    }

    bool is_aim_active = cfg.aim_active;
    if (cfg.trigger_magnet && cfg.trigger_active && cfg.trigger_enabled) {
        is_aim_active = true;
    }

    if (!cfg.aim_enabled && !(cfg.trigger_magnet && cfg.trigger_enabled)) {
        locked_entity_index = -1;
        subpixel_error = glm::vec2(0.0f);
        is_overshooting = false;
        current_rcs_punch = glm::vec2(0.0f);
        return;
    }

    if (!is_aim_active || !input_ok || !is_shootable_weapon(input.packet.held_weapon_id)) {
        locked_entity_index = -1;
        subpixel_error = glm::vec2(0.0f);
        is_overshooting = false;
        current_rcs_punch = glm::vec2(0.0f);
        return;
    }

    // Dynamic sensitivity scale from game memory
    float sensitivity = input.packet.local_sensitivity;
    float fov_adjust = input.packet.local_fov_sensitivity_adjust;
    if (sensitivity <= 0.01f) sensitivity = 1.25f;
    if (fov_adjust <= 0.01f) fov_adjust = 1.0f;

    // CS2 base yaw pitch scale
    constexpr float m_yaw = 0.022f;
    float deg_per_pixel = sensitivity * m_yaw * fov_adjust;

    glm::vec3 eye = {input.packet.local_eye.x, input.packet.local_eye.y, input.packet.local_eye.z};
    glm::vec3 angles = {input.packet.local_angles.x, input.packet.local_angles.y, input.packet.local_angles.z};

    // Calculate view ray forward direction including local weapon recoil (aim punch)
    float deg2rad = M_PI / 180.0f;
    float view_x = (angles.x + input.packet.local_aim_punch.x) * deg2rad;
    float view_y = (angles.y + input.packet.local_aim_punch.y) * deg2rad;
    float sp = std::sin(view_x);
    float cp = std::cos(view_x);
    float sy = std::sin(view_y);
    float cy = std::cos(view_y);
    glm::vec3 ray_dir{ cp * cy, cp * sy, -sp };

    // Determine active weapon category configs
    uint16_t weapon_id = input.packet.held_weapon_id;
    WeaponGroup w_group = get_weapon_group(weapon_id);

    int active_hitbox_groups = 63;
    float active_hitbox_scale = 1.0f;
    float active_fov = cfg.aim_fov;
    float active_smooth = cfg.aim_smooth;
    bool active_rcs = cfg.aim_rcs;
    float active_rcs_scale_x = cfg.aim_rcs_scale_x;
    float active_rcs_scale_y = cfg.aim_rcs_scale_y;
    float active_rcs_smooth = cfg.aim_rcs_smooth;

    if (w_group == WEAPON_GROUP_LIGHT_PISTOL) {
        active_hitbox_groups = cfg.trigger_light_pistols_hitbox_groups;
        active_hitbox_scale = cfg.trigger_light_pistols_hitbox_scale;
        active_fov = cfg.aim_light_pistols_fov;
        active_smooth = cfg.aim_light_pistols_smooth;
        active_rcs = cfg.aim_light_pistols_rcs;
        active_rcs_scale_x = cfg.aim_light_pistols_rcs_scale_x;
        active_rcs_scale_y = cfg.aim_light_pistols_rcs_scale_y;
        active_rcs_smooth = cfg.aim_light_pistols_rcs_smooth;
    } else if (w_group == WEAPON_GROUP_HEAVY_PISTOL) {
        active_hitbox_groups = cfg.trigger_heavy_pistols_hitbox_groups;
        active_hitbox_scale = cfg.trigger_heavy_pistols_hitbox_scale;
        active_fov = cfg.aim_heavy_pistols_fov;
        active_smooth = cfg.aim_heavy_pistols_smooth;
        active_rcs = cfg.aim_heavy_pistols_rcs;
        active_rcs_scale_x = cfg.aim_heavy_pistols_rcs_scale_x;
        active_rcs_scale_y = cfg.aim_heavy_pistols_rcs_scale_y;
        active_rcs_smooth = cfg.aim_heavy_pistols_rcs_smooth;
    } else if (w_group == WEAPON_GROUP_RIFLE) {
        active_hitbox_groups = cfg.trigger_rifles_hitbox_groups;
        active_hitbox_scale = cfg.trigger_rifles_hitbox_scale;
        active_fov = cfg.aim_rifles_fov;
        active_smooth = cfg.aim_rifles_smooth;
        active_rcs = cfg.aim_rifles_rcs;
        active_rcs_scale_x = cfg.aim_rifles_rcs_scale_x;
        active_rcs_scale_y = cfg.aim_rifles_rcs_scale_y;
        active_rcs_smooth = cfg.aim_rifles_rcs_smooth;
    } else if (w_group == WEAPON_GROUP_SMG) {
        active_hitbox_groups = cfg.trigger_smgs_hitbox_groups;
        active_hitbox_scale = cfg.trigger_smgs_hitbox_scale;
        active_fov = cfg.aim_smgs_fov;
        active_smooth = cfg.aim_smgs_smooth;
        active_rcs = cfg.aim_smgs_rcs;
        active_rcs_scale_x = cfg.aim_smgs_rcs_scale_x;
        active_rcs_scale_y = cfg.aim_smgs_rcs_scale_y;
        active_rcs_smooth = cfg.aim_smgs_rcs_smooth;
    } else if (w_group == WEAPON_GROUP_SCOUT) {
        active_hitbox_groups = cfg.trigger_scout_hitbox_groups;
        active_hitbox_scale = cfg.trigger_scout_hitbox_scale;
        active_fov = cfg.aim_scout_fov;
        active_smooth = cfg.aim_scout_smooth;
        active_rcs = cfg.aim_scout_rcs;
        active_rcs_scale_x = cfg.aim_scout_rcs_scale_x;
        active_rcs_scale_y = cfg.aim_scout_rcs_scale_y;
        active_rcs_smooth = cfg.aim_scout_rcs_smooth;
    } else if (w_group == WEAPON_GROUP_AWP) {
        active_hitbox_groups = cfg.trigger_awp_hitbox_groups;
        active_hitbox_scale = cfg.trigger_awp_hitbox_scale;
        active_fov = cfg.aim_awp_fov;
        active_smooth = cfg.aim_awp_smooth;
        active_rcs = cfg.aim_awp_rcs;
        active_rcs_scale_x = cfg.aim_awp_rcs_scale_x;
        active_rcs_scale_y = cfg.aim_awp_rcs_scale_y;
        active_rcs_smooth = cfg.aim_awp_rcs_smooth;
    } else if (w_group == WEAPON_GROUP_SNIPER) {
        active_hitbox_groups = cfg.trigger_auto_snipers_hitbox_groups;
        active_hitbox_scale = cfg.trigger_auto_snipers_hitbox_scale;
        active_fov = cfg.aim_auto_snipers_fov;
        active_smooth = cfg.aim_auto_snipers_smooth;
        active_rcs = cfg.aim_auto_snipers_rcs;
        active_rcs_scale_x = cfg.aim_auto_snipers_rcs_scale_x;
        active_rcs_scale_y = cfg.aim_auto_snipers_rcs_scale_y;
        active_rcs_smooth = cfg.aim_auto_snipers_rcs_smooth;
    } else if (w_group == WEAPON_GROUP_SHOTGUN) {
        active_hitbox_groups = cfg.trigger_shotguns_hitbox_groups;
        active_hitbox_scale = cfg.trigger_shotguns_hitbox_scale;
        active_fov = cfg.aim_shotguns_fov;
        active_smooth = cfg.aim_shotguns_smooth;
        active_rcs = cfg.aim_shotguns_rcs;
        active_rcs_scale_x = cfg.aim_shotguns_rcs_scale_x;
        active_rcs_scale_y = cfg.aim_shotguns_rcs_scale_y;
        active_rcs_smooth = cfg.aim_shotguns_rcs_smooth;
    } else if (w_group == WEAPON_GROUP_MG) {
        active_hitbox_groups = cfg.trigger_lmgs_hitbox_groups;
        active_hitbox_scale = cfg.trigger_lmgs_hitbox_scale;
        active_fov = cfg.aim_lmgs_fov;
        active_smooth = cfg.aim_lmgs_smooth;
        active_rcs = cfg.aim_lmgs_rcs;
        active_rcs_scale_x = cfg.aim_lmgs_rcs_scale_x;
        active_rcs_scale_y = cfg.aim_lmgs_rcs_scale_y;
        active_rcs_smooth = cfg.aim_lmgs_rcs_smooth;
    } else {
        active_hitbox_groups = cfg.trigger_others_hitbox_groups;
        active_hitbox_scale = cfg.trigger_others_hitbox_scale;
        active_fov = cfg.aim_others_fov;
        active_smooth = cfg.aim_others_smooth;
        active_rcs = cfg.aim_others_rcs;
        active_rcs_scale_x = cfg.aim_others_rcs_scale_x;
        active_rcs_scale_y = cfg.aim_others_rcs_scale_y;
        active_rcs_smooth = cfg.aim_others_rcs_smooth;
    }
    active_hitbox_scale = std::clamp(active_hitbox_scale, 0.5f, 1.0f);

    // Find best target
    int best_idx = -1;
    float best_fov_distance = active_fov;
    glm::vec3 best_aim_pos = glm::vec3(0.0f);

    // Helper to evaluate and project the closest point on player's capsules
    auto get_best_capsule_aimpos = [&](const PlayerData& player, glm::vec3& out_aimpos, float& out_fov,
                                       int groups_override = -1) -> bool {
        const int groups = groups_override >= 0 ? groups_override : active_hitbox_groups;
        auto capsules = build_aim_capsules(player, 1.0f, groups);
        if (capsules.empty()) return false;

        float min_fov = 9999.0f;
        glm::vec3 best_pt(0.0f);
        bool found = false;

        for (const auto& cap : capsules) {
            glm::vec3 pt_on_ray, pt_on_cap;
            get_closest_points(eye, ray_dir, cap.start, cap.end, pt_on_ray, pt_on_cap);

            glm::vec3 to_ray = pt_on_ray - pt_on_cap;
            float dist = glm::length(to_ray);
            glm::vec3 pt = pt_on_cap;
            float scaled_radius = cap.radius * active_hitbox_scale;

            if (dist > 0.001f) {
                glm::vec3 dir_to_ray = to_ray / dist;
                pt += dir_to_ray * std::min(scaled_radius, dist);
            }

            // Calculate FOV distance to the projected target point
            glm::vec3 pt_dir = glm::normalize(pt - eye);
            float pitch = -glm::degrees(std::asin(pt_dir.z));
            float yaw = glm::degrees(std::atan2(pt_dir.y, pt_dir.x));

            float delta_pitch = pitch - angles.x;
            float delta_yaw = normalize_yaw(yaw - angles.y);
            float fov = std::sqrt(delta_pitch * delta_pitch + delta_yaw * delta_yaw);

            if (fov < min_fov) {
                min_fov = fov;
                best_pt = pt;
                found = true;
            }
        }

        if (found) {
            out_aimpos = best_pt;
            out_fov = min_fov;
            return true;
        }
        return false;
    };

    // If we have a locked target, keep tracking them if they are still valid
    if (locked_entity_index != -1) {
        int matched_idx = -1;
        for (int i = 0; i < input.packet.player_count; ++i) {
            if (input.packet.players[i].entity_index == locked_entity_index) {
                matched_idx = i;
                break;
            }
        }

        if (matched_idx != -1) {
            const auto& player = input.packet.players[matched_idx];
            bool matches_team = (player.team == input.packet.local_team);
            if (player.active && player.health > 0 && (!cfg.team_check || !matches_team)) {
                // Ensure they are visible or we ignore smoke check
                bool visible = true;
                if (cfg.vischeck) {
                    bool bvh_visible = false;
                    if (matched_idx < (int)vis_result.player_visible.size()) {
                        bvh_visible = vis_result.player_visible[matched_idx];
                        if (!cfg.smoke_check && vis_result.player_occluded_by_smoke[matched_idx]) {
                            bvh_visible = true;
                        }
                    }
                    visible = bvh_visible;
                }

                glm::vec3 aim_pos(0.0f);
                float fov = 0.0f;
                if (visible && get_best_capsule_aimpos(player, aim_pos, fov)) {
                    if (fov < active_fov) {
                        best_idx = matched_idx;
                        best_aim_pos = aim_pos;
                        best_fov_distance = fov;
                    }
                }
            }
        }
        
        if (best_idx == -1) {
            locked_entity_index = -1; // Lost lock
        }
    }

    // If no target locked, search for new target closest to crosshair
    if (best_idx == -1) {
        for (int i = 0; i < input.packet.player_count; ++i) {
            const auto& player = input.packet.players[i];
            bool matches_team = (player.team == input.packet.local_team);
            if (!player.active || player.health <= 0 || (cfg.team_check && matches_team)) {
                continue;
            }

            bool visible = true;
            if (cfg.vischeck) {
                bool bvh_visible = false;
                if (i < (int)vis_result.player_visible.size()) {
                    bvh_visible = vis_result.player_visible[i];
                    if (!cfg.smoke_check && vis_result.player_occluded_by_smoke[i]) {
                        bvh_visible = true;
                    }
                }
                visible = bvh_visible;
            }

            if (!visible) continue;

            glm::vec3 aim_pos(0.0f);
            float fov = 0.0f;
            if (get_best_capsule_aimpos(player, aim_pos, fov)) {
                if (fov < best_fov_distance) {
                    best_idx = i;
                    best_aim_pos = aim_pos;
                    best_fov_distance = fov;
                }
            }
        }
        
        if (best_idx != -1) {
            locked_entity_index = input.packet.players[best_idx].entity_index;
        }
    }

    if (best_idx == -1) {
        subpixel_error = glm::vec2(0.0f);
        is_overshooting = false;
        return;
    }

    // Calculate final required angular deltas to the selected target position
    glm::vec3 dir = glm::normalize(best_aim_pos - eye);
    float pitch = -glm::degrees(std::asin(dir.z));
    float yaw = glm::degrees(std::atan2(dir.y, dir.x));

    float delta_pitch = pitch - angles.x;
    float delta_yaw = normalize_yaw(yaw - angles.y);

    float fov_dist = std::sqrt(delta_pitch * delta_pitch + delta_yaw * delta_yaw);

    // Stop aiming if we are already right on target, and RCS is not actively compensating
    if (fov_dist < 0.05f) {
        bool rcs_active = active_rcs && (std::abs(current_rcs_punch.x) > 0.01f || std::abs(current_rcs_punch.y) > 0.01f || input.packet.local_recoil_index > 0.05f);
        if (!rcs_active) {
            return;
        }
    }

    // 1. Progressive distance-based smoothing (Itami/Catalyst hybrid)
    float base_smooth = active_smooth;
    if (base_smooth < 1.0f) base_smooth = 1.0f;
    
    float progressive_smooth = base_smooth;
    float distance_factor = std::clamp(fov_dist / active_fov, 0.0f, 1.0f);

    if (cfg.aim_smooth_type == 1) { // Slow-End (Linear Deceleration)
        progressive_smooth = base_smooth * (0.25f + (1.0f - distance_factor) * 0.75f);
    } else if (cfg.aim_smooth_type == 2) { // Fast-End (Linear Acceleration)
        progressive_smooth = base_smooth * (0.25f + distance_factor * 0.75f);
    } else if (cfg.aim_smooth_type == 3) { // Progressive (Hybrid/Quadratic)
        float ease = 1.0f - std::pow(distance_factor, 2.0f);
        progressive_smooth = (0.3f + ease * 0.7f) * base_smooth;
    } // cfg.aim_smooth_type == 0 is Constant (no modification)

    // 2. Horizontal bias (human hand moves faster horizontally)
    float smooth_x = progressive_smooth * cfg.aim_horizontal_bias;
    float smooth_y = progressive_smooth;

    // 2.5. Randomized smoothing jitter
    if (cfg.aim_random_smooth) {
        static std::mt19937 rng(std::random_device{}());
        float rx = std::clamp(cfg.aim_randomize_smooth_x, 0.0f, smooth_x - 1.0f);
        float ry = std::clamp(cfg.aim_randomize_smooth_y, 0.0f, smooth_y - 1.0f);

        if (rx > 0.0f) {
            std::uniform_real_distribution<float> dist_x(-rx, rx);
            smooth_x += dist_x(rng);
        }
        if (ry > 0.0f) {
            std::uniform_real_distribution<float> dist_y(-ry, ry);
            smooth_y += dist_y(rng);
        }

        if (smooth_x < 1.0f) smooth_x = progressive_smooth * cfg.aim_horizontal_bias;
        if (smooth_y < 1.0f) smooth_y = progressive_smooth;
    }

    delta_yaw /= smooth_x;
    delta_pitch /= smooth_y;

    // Apply decoupled aim punch compensation (RCS)
    if (active_rcs) {
        float rcs_smooth = active_rcs_smooth;
        if (rcs_smooth < 1.0f) rcs_smooth = 1.0f;

        float target_punch_x = 0.0f;
        float target_punch_y = 0.0f;
        if (input.packet.local_recoil_index > 0.05f) {
            target_punch_x = input.packet.local_aim_punch.x * 2.0f * active_rcs_scale_y;
            target_punch_y = input.packet.local_aim_punch.y * 2.0f * active_rcs_scale_x;
        }

        float prev_punch_x = current_rcs_punch.x;
        float prev_punch_y = current_rcs_punch.y;

        current_rcs_punch.x += (target_punch_x - current_rcs_punch.x) / rcs_smooth;
        current_rcs_punch.y += (target_punch_y - current_rcs_punch.y) / rcs_smooth;

        float rcs_delta_pitch = current_rcs_punch.x - prev_punch_x;
        float rcs_delta_yaw = current_rcs_punch.y - prev_punch_y;

        delta_pitch -= rcs_delta_pitch;
        delta_yaw -= rcs_delta_yaw;
        delta_yaw = normalize_yaw(delta_yaw);
    } else {
        current_rcs_punch = glm::vec2(0.0f);
    }

    // 3. Muscle overshoot simulation (Itami style)
    if (cfg.aim_overshoot) {
        if (!is_overshooting && fov_dist > (active_fov * 0.3f)) {
            static std::mt19937 rng(std::random_device{}());
            std::uniform_real_distribution<float> dist_p(0.0f, 1.0f);
            if (dist_p(rng) < cfg.aim_overshoot_chance) {
                is_overshooting = true;
                current_overshoot_scale = cfg.aim_overshoot_scale;
                std::uniform_int_distribution<int> dist_t(5, 12);
                overshoot_ticks_left = dist_t(rng);
            }
        }

        if (is_overshooting) {
            if (overshoot_ticks_left > 0) {
                delta_yaw *= current_overshoot_scale;
                delta_pitch *= current_overshoot_scale;
                overshoot_ticks_left--;
            } else {
                is_overshooting = false;
            }
        }
    }

    // Translate angular deltas to mouse inputs using the sub-pixel error accumulator
    float want_x = -delta_yaw + subpixel_error.x;
    float want_y = delta_pitch + subpixel_error.y;

    float counts_x = std::round(want_x / deg_per_pixel);
    float counts_y = std::round(want_y / deg_per_pixel);

    // Save fractional remainders for the next tick
    subpixel_error.x = want_x - counts_x * deg_per_pixel;
    subpixel_error.y = want_y - counts_y * deg_per_pixel;

    if (std::abs(counts_x) > 0.0f || std::abs(counts_y) > 0.0f) {
        inject_mouse_move(counts_x, counts_y);
    }
}
