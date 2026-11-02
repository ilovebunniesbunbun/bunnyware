#include "TriggerBot.hpp"
#include "TriggerBotMath.hpp"
#include "logger.hpp"
#include "VisibilityWorker.hpp"
#include <chrono>
#include <random>
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <glm/glm.hpp>

// Helper to get current steady time in seconds
static double get_current_time()
{
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(now.time_since_epoch()).count();
}

static std::string get_weapon_name_from_id(uint16_t id) {
    switch (id) {
        case 1: return "Desert Eagle";
        case 2: return "Dual Berettas";
        case 3: return "Five-SeveN";
        case 4: return "Glock-18";
        case 7: return "AK-47";
        case 8: return "AUG";
        case 9: return "AWP";
        case 10: return "FAMAS";
        case 11: return "G3SG1";
        case 13: return "Galil AR";
        case 14: return "M249";
        case 16: return "M4A4";
        case 17: return "MAC-10";
        case 19: return "P90";
        case 24: return "UMP-45";
        case 25: return "XM1014";
        case 26: return "PP-Bizon";
        case 27: return "MAG-7";
        case 28: return "Negev";
        case 29: return "Sawed-Off";
        case 30: return "Tec-9";
        case 31: return "Zeus x27";
        case 32: return "P2000";
        case 33: return "MP7";
        case 34: return "MP9";
        case 35: return "Nova";
        case 36: return "P250";
        case 38: return "SCAR-20";
        case 39: return "SG 553";
        case 40: return "SSG 08";
        case 60: return "M4A1-S";
        case 61: return "USP-S";
        case 63: return "CZ75-Auto";
        case 64: return "R8 Revolver";
        default: return "Weapon ID " + std::to_string(id);
    }
}

static glm::vec3 rotate_vector(const glm::vec3& v, const glm::vec4& q) {
    glm::vec3 q_vec(q.x, q.y, q.z);
    glm::vec3 t = 2.0f * glm::cross(q_vec, v);
    return v + q.w * t + glm::cross(q_vec, t);
}

// ─────────────────────────────────────────────────────────────────────────────
// Parametric line-line closest approach ray-capsule intersection
// ─────────────────────────────────────────────────────────────────────────────
static constexpr float kCapsuleDegenerate = 0.001f;

// Reconstruct capsules for the target player. The direction and length of the
// capsule axis are precomputed at build time so the (potentially thousands of)
// per-ray hit tests in the Hitchance/Both sampling loops don't have to redo a
// sqrt + divide for every ray against every capsule.
struct ConstructedCapsule {
    glm::vec3 start;
    glm::vec3 end;
    float radius;
    int bone;
    glm::vec3 dir;   // unit axis direction (start -> end); zero when degenerate
    float length;    // |end - start|; < kCapsuleDegenerate => treat as a sphere
};

static inline ConstructedCapsule make_capsule(const glm::vec3& start, const glm::vec3& end,
                                              float radius, int bone) {
    const glm::vec3 vec = end - start;
    const float len = glm::length(vec);
    const glm::vec3 dir = (len < kCapsuleDegenerate) ? glm::vec3(0.0f) : vec / len;
    return ConstructedCapsule{ start, end, radius, bone, dir, len };
}

// Fixed-capacity capsule container to avoid per-frame heap allocation.
// A player exposes at most 20 hitboxes (TargetHitboxData::entries) or 11
// fallback capsules, so 24 slots comfortably covers both.
struct CapsuleBuffer {
    static constexpr int kMax = 24;
    ConstructedCapsule data[kMax];
    int count = 0;
    void push(const ConstructedCapsule& c) { if (count < kMax) data[count++] = c; }
    bool empty() const { return count == 0; }
    int size() const { return count; }
    const ConstructedCapsule& operator[](int i) const { return data[i]; }
    const ConstructedCapsule* begin() const { return data; }
    const ConstructedCapsule* end() const { return data + count; }
};

static bool ray_hits_capsule(const glm::vec3& ray_origin, const glm::vec3& ray_dir,
                             const ConstructedCapsule& cap, float& out_ray_t) {
    const float radius = cap.radius;
    const float capsule_length = cap.length;

    if (capsule_length < kCapsuleDegenerate) {
        const auto to_center = cap.start - ray_origin;
        const auto projection = glm::dot(to_center, ray_dir);

        if (projection < 0.0f) {
            return false;
        }

        const auto closest = ray_origin + ray_dir * projection;
        glm::vec3 diff = closest - cap.start;
        if (glm::dot(diff, diff) <= radius * radius) {
            out_ray_t = projection;
            return true;
        }
        return false;
    }

    const glm::vec3& capsule_dir = cap.dir;
    const auto w = ray_origin - cap.start;

    const auto a = glm::dot(ray_dir, ray_dir);
    const auto b = glm::dot(ray_dir, capsule_dir);
    const auto c = glm::dot(capsule_dir, capsule_dir);
    const auto d = glm::dot(ray_dir, w);
    const auto e = glm::dot(capsule_dir, w);

    const auto denom = a * c - b * b;

    float t;

    if (std::abs(denom) < 0.0001f) {
        t = (b > c ? d / b : e / c);
    } else {
        t = (a * e - b * d) / denom;
    }

    t = std::clamp(t, 0.0f, capsule_length);
    const auto point_on_capsule = cap.start + capsule_dir * t;
    const float ray_distance = glm::dot(point_on_capsule - ray_origin, ray_dir);
    if (ray_distance < 0.0f) {
        return false;
    }

    const auto point_on_ray = ray_origin + ray_dir * ray_distance;

    glm::vec3 diff = point_on_ray - point_on_capsule;
    if (glm::dot(diff, diff) <= radius * radius) {
        out_ray_t = ray_distance;
        return true;
    }
    return false;
}

static CapsuleBuffer build_fallback_capsules(const PlayerData& player, float scale_factor, int enabled_groups) {
    CapsuleBuffer capsules;
    if (player.bone_count < 24) return capsules;

    auto get_bone_pos = [&](int id) -> glm::vec3 {
        return player.bones[id].position;
    };

    auto add_fallback = [&](int b1, int b2, float radius, int ref_bone) {
        if (is_hitbox_group_enabled(ref_bone, enabled_groups)) {
            capsules.push(make_capsule(get_bone_pos(b1), get_bone_pos(b2), radius * scale_factor, ref_bone));
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
    add_fallback(13, 14, 3.0f, 13);
    // 10. Right Arm Lower (14 to 15)
    add_fallback(14, 15, 2.5f, 14);

    return capsules;
}

static CapsuleBuffer build_capsules(const TargetHitboxData& hitbox_data, const PlayerData& target_player, float scale_factor, int enabled_groups) {
    if (hitbox_data.count == 0) {
        return build_fallback_capsules(target_player, scale_factor, enabled_groups);
    }
    CapsuleBuffer capsules;
    for (int i = 0; i < hitbox_data.count; ++i) {
        const auto& hb = hitbox_data.entries[i];
        if (hb.bone < 0 || hb.bone >= target_player.bone_count) continue;
        
        // Filter by hitbox group
        if (!is_hitbox_group_enabled(hb.bone, enabled_groups)) continue;
        
        const auto& bone = target_player.bones[hb.bone];
        const glm::vec4 rotation(bone.rotation.x, bone.rotation.y,
                                 bone.rotation.z, bone.rotation.w);
        const glm::vec3 full_start = bone.position + rotate_vector(hb.mins, rotation);
        const glm::vec3 full_end = bone.position + rotate_vector(hb.maxs, rotation);
        const glm::vec3 center = (full_start + full_end) * 0.5f;
        const glm::vec3 start = center + (full_start - center) * scale_factor;
        const glm::vec3 end = center + (full_end - center) * scale_factor;
        const float radius = (hb.radius > 0.0f ? hb.radius : 3.5f) * scale_factor;

        capsules.push(make_capsule(start, end, radius, hb.bone));
    }
    return capsules;
}

// Build capsules from per-player hitbox data (real game hitboxes, ported from SeedTrigger).
// Takes the TargetHitboxData from SHM and the player's bone transforms.
CapsuleBuffer TriggerBot::build_capsules_from_player_hitboxes(
    const TargetHitboxData& hitbox_data, const PlayerData& player,
    float scale_factor, int enabled_groups) {
    CapsuleBuffer capsules;
    if (hitbox_data.count == 0) {
        return build_fallback_capsules(player, scale_factor, enabled_groups);
    }
    for (int i = 0; i < hitbox_data.count; ++i) {
        const auto& hb = hitbox_data.entries[i];
        if (hb.bone < 0 || hb.bone >= player.bone_count) continue;
        if (!is_hitbox_group_enabled(hb.bone, enabled_groups)) continue;

        const auto& bone = player.bones[hb.bone];
        glm::vec3 full_start = bone.position + rotate_vector(
            glm::vec3(hb.mins.x, hb.mins.y, hb.mins.z), glm::vec4(bone.rotation.x, bone.rotation.y, bone.rotation.z, bone.rotation.w));
        glm::vec3 full_end = bone.position + rotate_vector(
            glm::vec3(hb.maxs.x, hb.maxs.y, hb.maxs.z), glm::vec4(bone.rotation.x, bone.rotation.y, bone.rotation.z, bone.rotation.w));
        glm::vec3 center = (full_start + full_end) * 0.5f;
        glm::vec3 start = center + (full_start - center) * scale_factor;
        glm::vec3 end = center + (full_end - center) * scale_factor;
        float radius = (hb.radius > 0.0f ? hb.radius : 3.5f) * scale_factor;

        capsules.push(make_capsule(start, end, radius, hb.bone));
    }
    return capsules;
}

// View basis (forward/right/up) for a given aim direction. Computed once per
// frame and shared across all seed checks so the 256-sample Hitchance/Both
// loops don't redo four sin/cos per sample.
struct ViewBasis {
    glm::vec3 forward;
    glm::vec3 right;
    glm::vec3 up;
};

static inline ViewBasis compute_view_basis(const glm::vec3& angles, const glm::vec3& aim_punch) {
    const float deg2rad = M_PI / 180.0f;
    const float view_x = (angles.x + aim_punch.x) * deg2rad;
    const float view_y = (angles.y + aim_punch.y) * deg2rad;
    const float sp = std::sin(view_x);
    const float cp = std::cos(view_x);
    const float sy = std::sin(view_y);
    const float cy = std::cos(view_y);
    return ViewBasis{
        glm::vec3{ cp * cy, cp * sy, -sp },
        glm::vec3{ -sy, cy, 0.0f },
        glm::vec3{ sp * cy, sp * sy, cp }
    };
}

// ─────────────────────────────────────────────────────────────────────────────
// Spread / Seed simulation helper
// ─────────────────────────────────────────────────────────────────────────────
static bool check_seed_hit_capsules(uint32_t seed, const glm::vec3& eye_pos, const ViewBasis& basis,
                                    const CapsuleBuffer& capsules,
                                    float inaccuracy, float weapon_spread, float recoil_index,
                                    uint16_t weapon_id, int num_bullets, int spread_seed_val, float& out_hit_dist, int& out_hit_bone) {
    const glm::vec3& forward = basis.forward;
    const glm::vec3& right = basis.right;
    const glm::vec3& up = basis.up;

    uint32_t rng_seed = seed + 1;
    int seed_hits = 0;
    float min_hit_dist = 999999.0f;
    ValveRandomStream prng;

    int closest_bone = 0;
    for (int i = 0; i < num_bullets; ++i) {
        uint32_t pellet_seed = rng_seed;
        if (num_bullets > 1 && spread_seed_val > 0) {
            pellet_seed = (rng_seed & 255) + i * spread_seed_val;
        }

        prng.set_seed(pellet_seed);
        float r1 = prng.random_float(0.0f, 1.0f);
        float r2 = prng.random_float(0.0f, M_PI * 2.0f);
        float r3 = prng.random_float(0.0f, 1.0f);
        float r4 = prng.random_float(0.0f, M_PI * 2.0f);

        if (weapon_id == 28 && recoil_index < 3.0f) {
            float v1 = r1;
            float v3 = r3;
            int c = 3;
            do {
                c--;
                v1 = v1 * v1;
                v3 = v3 * v3;
            } while (c > recoil_index);
            r1 = 1.0f - v1;
            r3 = 1.0f - v3;
        }

        if (weapon_id == 64 && num_bullets == 1) {
            r1 = 1.0f - (r1 * r1);
            r3 = 1.0f - (r3 * r3);
        }

        float inaccuracy_radius = r1 * inaccuracy;
        float spread_radius = r3 * weapon_spread;

        float total_spread_x = std::cos(r2) * inaccuracy_radius + std::cos(r4) * spread_radius;
        float total_spread_y = std::sin(r2) * inaccuracy_radius + std::sin(r4) * spread_radius;

        glm::vec3 dir = forward - right * total_spread_x + up * total_spread_y;
        dir = glm::normalize(dir);

        for (const auto& cap : capsules) {
            float hit_t = 0.0f;
            if (ray_hits_capsule(eye_pos, dir, cap, hit_t)) {
                seed_hits++;
                if (hit_t < min_hit_dist) {
                    min_hit_dist = hit_t;
                    closest_bone = cap.bone;
                }
                break; 
            }
        }
    }

    if (seed_hits > 0) {
        out_hit_dist = min_hit_dist;
        out_hit_bone = closest_bone;
        return true;
    }
    return false;
}

TriggerBot::TriggerBot()
{
    try {
        evdev_input = std::make_unique<EvdevInput>();
        input_ok = true;
    } catch (const std::exception& e) {
        init_error = e.what();
        input_ok = false;
    }
}

TriggerBot::~TriggerBot()
{
    // click_release() is a no-op unless the button is actually down, so this
    // guarantees no held shot survives shutdown regardless of the fire path.
    if (input_ok) {
        click_release();
    }
}

uint32_t TriggerBot::compute_spread_seed(float pitch, float yaw, int tick)
{
    const float qp = quantize_angle(pitch);
    const float qy = quantize_angle(yaw);
    if (seed_cache_valid_ && tick == cached_seed_tick_ &&
        qp == cached_seed_qpitch_ && qy == cached_seed_qyaw_) {
        return cached_seed_;
    }
    cached_seed_ = sha1_transform_12bytes(qp, qy, tick);
    cached_seed_qpitch_ = qp;
    cached_seed_qyaw_ = qy;
    cached_seed_tick_ = tick;
    seed_cache_valid_ = true;
    return cached_seed_;
}

void TriggerBot::update(const FrameInput& input, const OverlayConfig& cfg, GlobalInputReader& global_input, const VischeckResult& vischeck_result, const VisibilityWorker& vis_worker)
{
    (void)global_input;

    double now = get_current_time();

    // 1. Process damage registration from triggerbot shot (with kill-event safety fallback)
    if (is_waiting_for_damage) {
        bool damage_increased = (input.packet.local_total_damage > damage_before_shot);
        bool kill_occurred = (input.packet.local_total_kills > damage_before_kills);

        if (damage_increased || kill_occurred) {
            float diff = 0.0f;
            if (damage_increased) {
                diff = input.packet.local_total_damage - damage_before_shot;
            } else {
                diff = last_prediction.predicted_damage; // fallback to predicted damage if only kill registered
            }

            // Build bones string
            std::string bones_str = "";
            for (size_t b_idx = 0; b_idx < last_prediction.intersected_bones.size(); ++b_idx) {
                if (b_idx > 0) bones_str += ", ";
                int bone = last_prediction.intersected_bones[b_idx];
                bones_str += std::to_string(bone);
                
                // Add bone name/group helper
                HitboxGroupEnum grp = get_hitbox_group_from_bone(bone);
                std::string grp_name = "unknown";
                if (grp == HITBOX_GROUP_HEAD) grp_name = "head";
                else if (grp == HITBOX_GROUP_NECK) grp_name = "neck";
                else if (grp == HITBOX_GROUP_CHEST) grp_name = "chest";
                else if (grp == HITBOX_GROUP_STOMACH) grp_name = "stomach";
                else if (grp == HITBOX_GROUP_ARM) grp_name = "arm";
                else if (grp == HITBOX_GROUP_LEG) grp_name = "leg";
                bones_str += " (" + grp_name + ")";
            }
            if (bones_str.empty()) {
                bones_str = "None";
            }

            // Build armor string
            std::string armor_str = "no armor";
            if (last_prediction.armor_value > 0) {
                if (last_prediction.has_helmet) {
                    armor_str = "kevlar + helm";
                } else {
                    armor_str = "kevlar";
                }
            }

            // Log details
            BUNNYWARE_LOG_INFO("==================================================");
            BUNNYWARE_LOG_INFO("[TriggerBot] SHOT DETAILS:");
            BUNNYWARE_LOG_INFO("  Target: {} (ID: {})", last_prediction.target_name, last_prediction.target_entity_id);
            BUNNYWARE_LOG_INFO("  Weapon: {} (Base Damage: {:.1f}, Penetration: {:.2f})", last_prediction.weapon_name, last_prediction.base_dmg, last_prediction.pen_power);
            BUNNYWARE_LOG_INFO("  Armor Status: {} (Value: {})", armor_str, last_prediction.armor_value);
            BUNNYWARE_LOG_INFO("  Hitchance: Mode {} (Configured: {:.1f}%, Actual: {:.1f}%)", last_prediction.trigger_mode, last_prediction.active_hitchance, last_prediction.actual_hitchance);
            BUNNYWARE_LOG_INFO("  Predicted Damage: {:.1f}", last_prediction.predicted_damage);
            BUNNYWARE_LOG_INFO("  Actual Damage Dealt: {:.1f}", diff);
            BUNNYWARE_LOG_INFO("  Hitbox Bones Crossed: {}", bones_str);
            BUNNYWARE_LOG_INFO("==================================================");

            is_waiting_for_damage = false;
        } else if (input.packet.local_total_damage < damage_before_shot) {
            is_waiting_for_damage = false;
        } else if (now - last_shot_time > 1.5) {
            is_waiting_for_damage = false;
        }
    }

    // 2. Check for manual shot damage/kill registration
    bool manual_damage_increased = (input.packet.local_total_damage > last_known_damage && last_known_damage > 0.0f);
    bool manual_kill_occurred = (input.packet.local_total_kills > last_known_kills && last_known_kills > 0);

    if (manual_damage_increased || manual_kill_occurred) {
        float diff = 0.0f;
        if (manual_damage_increased) {
            diff = input.packet.local_total_damage - last_known_damage;
        } else {
            diff = 0.0f; // unknown / kill only
        }

        if (!is_waiting_for_damage) {
            // Find who the local player is aiming at currently or was aiming at when they did damage.
            const PlayerData* target_player = nullptr;
            for (int p_idx = 0; p_idx < input.packet.player_count; ++p_idx) {
                if (input.packet.players[p_idx].active && 
                    (input.packet.players[p_idx].entity_index == input.packet.local_in_crosshair_id ||
                     input.packet.local_in_crosshair_id <= 0)) {
                    target_player = &input.packet.players[p_idx];
                    if (input.packet.players[p_idx].entity_index == input.packet.local_in_crosshair_id) {
                        break; // exact crosshair match
                    }
                }
            }
            if (target_player) {
                // Generate current view ray
                glm::vec3 eye_pos{ input.packet.local_eye.x, input.packet.local_eye.y, input.packet.local_eye.z };
                glm::vec3 angles{ input.packet.local_angles.x, input.packet.local_angles.y, input.packet.local_angles.z };
                glm::vec3 aim_punch{ input.packet.local_aim_punch.x, input.packet.local_aim_punch.y, input.packet.local_aim_punch.z };
                float deg2rad = M_PI / 180.0f;
                float view_x = (angles.x + aim_punch.x) * deg2rad;
                float view_y = (angles.y + aim_punch.y) * deg2rad;
                float sp = std::sin(view_x);
                float cp = std::cos(view_x);
                float sy = std::sin(view_y);
                float cy = std::cos(view_y);
                glm::vec3 forward_dir{ cp * cy, cp * sy, -sp };

                auto capsules = build_fallback_capsules(*target_player, 1.0f, 63); // use full scale for manual trace
                populate_prediction(input, *target_player, eye_pos, forward_dir, capsules, vis_worker, 0.0f, 100.0f, 0);

                // Print the details
                std::string bones_str = "";
                for (size_t b_idx = 0; b_idx < last_prediction.intersected_bones.size(); ++b_idx) {
                    if (b_idx > 0) bones_str += ", ";
                    int bone = last_prediction.intersected_bones[b_idx];
                    bones_str += std::to_string(bone);
                    
                    HitboxGroupEnum grp = get_hitbox_group_from_bone(bone);
                    std::string grp_name = "unknown";
                    if (grp == HITBOX_GROUP_HEAD) grp_name = "head";
                    else if (grp == HITBOX_GROUP_NECK) grp_name = "neck";
                    else if (grp == HITBOX_GROUP_CHEST) grp_name = "chest";
                    else if (grp == HITBOX_GROUP_STOMACH) grp_name = "stomach";
                    else if (grp == HITBOX_GROUP_ARM) grp_name = "arm";
                    else if (grp == HITBOX_GROUP_LEG) grp_name = "leg";
                    bones_str += " (" + grp_name + ")";
                }
                if (bones_str.empty()) {
                    bones_str = "None";
                }

                std::string armor_str = "no armor";
                if (last_prediction.armor_value > 0) {
                    if (last_prediction.has_helmet) {
                        armor_str = "kevlar + helm";
                    } else {
                        armor_str = "kevlar";
                    }
                }

                BUNNYWARE_LOG_INFO("==================================================");
                BUNNYWARE_LOG_INFO("[TriggerBot] MANUAL SHOT DETAILS:");
                BUNNYWARE_LOG_INFO("  Target: {} (ID: {})", last_prediction.target_name, last_prediction.target_entity_id);
                BUNNYWARE_LOG_INFO("  Weapon: {} (Base Damage: {:.1f}, Penetration: {:.2f})", last_prediction.weapon_name, last_prediction.base_dmg, last_prediction.pen_power);
                BUNNYWARE_LOG_INFO("  Armor Status: {} (Value: {})", armor_str, last_prediction.armor_value);
                BUNNYWARE_LOG_INFO("  Hitchance: Mode {} (Configured: {:.1f}%, Actual: {:.1f}%)", last_prediction.trigger_mode, last_prediction.active_hitchance, last_prediction.actual_hitchance);
                BUNNYWARE_LOG_INFO("  Predicted Damage: {:.1f}", last_prediction.predicted_damage);
                if (manual_damage_increased) {
                    BUNNYWARE_LOG_INFO("  Actual Damage Dealt: {:.1f}", diff);
                } else {
                    BUNNYWARE_LOG_INFO("  Actual Damage Dealt: [Kill Shot / Disappeared]");
                }
                BUNNYWARE_LOG_INFO("  Hitbox Bones Crossed: {}", bones_str);
                BUNNYWARE_LOG_INFO("==================================================");
            }
        }
    }
    last_known_damage = input.packet.local_total_damage;
    last_known_kills = input.packet.local_total_kills;

    // 3. Early return checks for triggerbot logic
    if (!cfg.trigger_enabled || !cfg.trigger_active || !input_ok || !is_shootable_weapon(input.packet.held_weapon_id)) {
        if (held_) {
            click_release();
            held_ = false;
            release_at_ = 0.0;
        }
        if (state == STATE_SHOOTING) {
            state = STATE_IDLE;
        }
        return;
    }

    // SeedTrigger-style: release if hold duration expired (replaces STATE_SHOOTING timer)
    if (held_ && now >= release_at_) {
        click_release();
        held_ = false;
        release_at_ = 0.0;
        state = STATE_IDLE;
        next_allowed_shot_time = now + (cfg.trigger_delay_between_shots / 1000.0);
    }
    if (held_) return; // still holding fire, don't re-evaluate

    attack_button = cfg.trigger_attack_key;
    if (attack_button != 1 && attack_button != 2 && attack_button != 3) {
        attack_button = 1;
    }

    // Use the seed tick selected by GameReader (global/simulation tick, with
    // tick-base fallback) rather than assuming the command tick is always
    // the controller's raw m_nTickBase value.
    int seed_tick = input.packet.local_tick_base;
    int local_seed_tick = input.packet.local_seed_tick;
    if (local_seed_tick > 0) seed_tick = local_seed_tick;

    // A new match or map load restarts the tick count. Drop the stale pending
    // tick, or it blocks every shot until the new count catches up with it.
    // The margin keeps small backward steps between tick sources from
    // clearing the re-fire guard.
    static constexpr int kTickResetMargin = 64;
    if (pending_tick_ >= 0 && seed_tick + kTickResetMargin < pending_tick_) {
        pending_tick_ = -1;
        memo_tick_ = -1;
    }
    if (pending_tick_ >= 0 && seed_tick <= pending_tick_) return;

    uint16_t weapon_id = input.packet.held_weapon_id;
    WeaponGroup w_group = get_weapon_group(weapon_id);
    bool scoped = input.packet.local_is_scoped != 0;

    float active_hitchance = cfg.trigger_hitchance;
    float hitbox_scale = 1.0f;
    int active_hitbox_groups = 63;
    if (w_group == WEAPON_GROUP_LIGHT_PISTOL) {
        active_hitchance = cfg.trigger_light_pistols_hitchance;
        hitbox_scale = cfg.trigger_light_pistols_hitbox_scale;
        active_hitbox_groups = cfg.trigger_light_pistols_hitbox_groups;
    } else if (w_group == WEAPON_GROUP_HEAVY_PISTOL) {
        active_hitchance = cfg.trigger_heavy_pistols_hitchance;
        hitbox_scale = cfg.trigger_heavy_pistols_hitbox_scale;
        active_hitbox_groups = cfg.trigger_heavy_pistols_hitbox_groups;
    } else if (w_group == WEAPON_GROUP_RIFLE) {
        active_hitchance = cfg.trigger_rifles_hitchance;
        hitbox_scale = cfg.trigger_rifles_hitbox_scale;
        active_hitbox_groups = cfg.trigger_rifles_hitbox_groups;
    } else if (w_group == WEAPON_GROUP_SMG) {
        active_hitchance = cfg.trigger_smgs_hitchance;
        hitbox_scale = cfg.trigger_smgs_hitbox_scale;
        active_hitbox_groups = cfg.trigger_smgs_hitbox_groups;
    } else if (w_group == WEAPON_GROUP_SCOUT) {
        active_hitchance = scoped ? cfg.trigger_scout_hitchance : cfg.trigger_scout_noscope_hitchance;
        hitbox_scale = cfg.trigger_scout_hitbox_scale;
        active_hitbox_groups = cfg.trigger_scout_hitbox_groups;
    } else if (w_group == WEAPON_GROUP_AWP) {
        active_hitchance = scoped ? cfg.trigger_awp_hitchance : cfg.trigger_awp_noscope_hitchance;
        hitbox_scale = cfg.trigger_awp_hitbox_scale;
        active_hitbox_groups = cfg.trigger_awp_hitbox_groups;
    } else if (w_group == WEAPON_GROUP_SNIPER) {
        active_hitchance = scoped ? cfg.trigger_auto_snipers_hitchance : cfg.trigger_auto_snipers_noscope_hitchance;
        hitbox_scale = cfg.trigger_auto_snipers_hitbox_scale;
        active_hitbox_groups = cfg.trigger_auto_snipers_hitbox_groups;
    } else if (w_group == WEAPON_GROUP_SHOTGUN) {
        active_hitchance = cfg.trigger_shotguns_hitchance;
        hitbox_scale = cfg.trigger_shotguns_hitbox_scale;
        active_hitbox_groups = cfg.trigger_shotguns_hitbox_groups;
    } else if (w_group == WEAPON_GROUP_MG) {
        active_hitchance = cfg.trigger_lmgs_hitchance;
        hitbox_scale = cfg.trigger_lmgs_hitbox_scale;
        active_hitbox_groups = cfg.trigger_lmgs_hitbox_groups;
    } else {
        active_hitchance = cfg.trigger_others_hitchance;
        hitbox_scale = cfg.trigger_others_hitbox_scale;
        active_hitbox_groups = cfg.trigger_others_hitbox_groups;
    }
    hitbox_scale = std::clamp(hitbox_scale, 0.5f, 1.0f);

    // Simple Mode
    bool is_simple = cfg.trigger_simple_mode || (cfg.trigger_mode == 0);
    if (is_simple) {
        bool should_shoot = false;
        const PlayerData* target = nullptr;
        if (input.packet.local_in_crosshair_id > 0) {
            // Find crosshair target
            for (int i = 0; i < input.packet.player_count; ++i) {
                if (input.packet.players[i].active && 
                    input.packet.players[i].entity_index == input.packet.local_in_crosshair_id) {
                    target = &input.packet.players[i];
                    break;
                }
            }
            if (target) {
                bool matches_team = (target->team == input.packet.local_team);
                if (!cfg.team_check || !matches_team) {
                    should_shoot = true;
                }
            }
        }

        if (should_shoot && target) {
            if (now >= next_allowed_shot_time && cfg.trigger_enabled) {
                // Compute vectors for tracing
                glm::vec3 eye_pos{ input.packet.local_eye.x, input.packet.local_eye.y, input.packet.local_eye.z };
                glm::vec3 angles{ input.packet.local_angles.x, input.packet.local_angles.y, input.packet.local_angles.z };
                glm::vec3 aim_punch{ input.packet.local_aim_punch.x, input.packet.local_aim_punch.y, input.packet.local_aim_punch.z };
                float deg2rad = M_PI / 180.0f;
                float view_x = (angles.x + aim_punch.x) * deg2rad;
                float view_y = (angles.y + aim_punch.y) * deg2rad;
                float sp = std::sin(view_x);
                float cp = std::cos(view_x);
                float sy = std::sin(view_y);
                float cy = std::cos(view_y);
                glm::vec3 forward{ cp * cy, cp * sy, -sp };

                auto capsules = build_fallback_capsules(*target, hitbox_scale, active_hitbox_groups);

                populate_prediction(input, *target, eye_pos, forward, capsules, vis_worker, 0.0f, 100.0f, 0);

                // Log commented out to pause other logs
                // if (cfg.trigger_debug) {
                //     BUNNYWARE_LOG_INFO("[TriggerBot] Simple Mode: Target locked in crosshair. Shooting!");
                // }
                click_press();
                held_ = true;
                release_at_ = now + 0.03; // 30ms hold (Simple Mode, same mechanism as SeedTrigger)
                damage_before_shot = input.packet.local_total_damage;
                damage_before_kills = input.packet.local_total_kills;
                last_shot_time = now;
                is_waiting_for_damage = true;
            }
        }
        return;
    }

    // Only log per-frame debug info when trigger is active to avoid spam
    bool debug_active = cfg.trigger_debug && cfg.trigger_enabled;

    const PlayerData* crosshair_target = nullptr;

    if (input.packet.local_in_crosshair_id > 0) {
        for (int i = 0; i < input.packet.player_count; ++i) {
            if (input.packet.players[i].active && 
                input.packet.players[i].entity_index == input.packet.local_in_crosshair_id) {
                crosshair_target = &input.packet.players[i];
                break;
            }
        }
    }

    
    // Prepare spread constants and sanitize against NaN/Inf values
    float total_spread = input.packet.local_spread;
    float weapon_spread = input.packet.local_base_spread;
    if (std::isnan(total_spread) || std::isinf(total_spread)) total_spread = 0.0f;
    if (std::isnan(weapon_spread) || std::isinf(weapon_spread)) weapon_spread = 0.0f;

    float inaccuracy = std::max(0.0f, total_spread - weapon_spread);
    if (std::isnan(inaccuracy) || std::isinf(inaccuracy)) inaccuracy = 0.0f;

    float recoil_index = input.packet.local_recoil_index;
    if (std::isnan(recoil_index) || std::isinf(recoil_index)) recoil_index = 0.0f;

    int num_bullets = input.packet.local_num_bullets;
    int spread_seed_val = input.packet.local_spread_seed;

    glm::vec3 eye_pos = input.packet.local_eye;
    if (std::isnan(eye_pos.x) || std::isinf(eye_pos.x)) eye_pos.x = 0.0f;
    if (std::isnan(eye_pos.y) || std::isinf(eye_pos.y)) eye_pos.y = 0.0f;
    if (std::isnan(eye_pos.z) || std::isinf(eye_pos.z)) eye_pos.z = 0.0f;

    // SeedTrigger-style velocity prediction: extrapolate eye by one tick
    if (cfg.trigger_predict_origin) {
        glm::vec3 local_vel = input.packet.local_velocity;
        if (std::isfinite(local_vel.x) && std::isfinite(local_vel.y) && std::isfinite(local_vel.z)) {
            eye_pos += local_vel * (1.0f / 64.0f);
        }
    }

    glm::vec3 angles = input.packet.local_angles;
    if (std::isnan(angles.x) || std::isinf(angles.x)) angles.x = 0.0f;
    if (std::isnan(angles.y) || std::isinf(angles.y)) angles.y = 0.0f;
    if (std::isnan(angles.z) || std::isinf(angles.z)) angles.z = 0.0f;

    glm::vec3 aim_punch = input.packet.local_aim_punch * 2.0f; // Scale aim punch by 2x like CS2
    if (std::isnan(aim_punch.x) || std::isinf(aim_punch.x)) aim_punch.x = 0.0f;
    if (std::isnan(aim_punch.y) || std::isinf(aim_punch.y)) aim_punch.y = 0.0f;
    if (std::isnan(aim_punch.z) || std::isinf(aim_punch.z)) aim_punch.z = 0.0f;

    aim_punch.y = normalize_angle(aim_punch.y);
    aim_punch.x = normalize_angle(aim_punch.x);

    // The spread seed is keyed from the angles used by the firing command,
    // which include the weapon punch.  The camera basis below uses the same
    // compensated angles; hashing the uncorrected camera angles produces a
    // valid-looking ray for the wrong seed except when the view is nearly
    // perfectly centered (or punch happens to be zero).
    const glm::vec3 hash_angles = angles + aim_punch;

    // Shared aim basis for every seed check this frame (angles/aim_punch are
    // constant across all samples, so the trig is done exactly once).
    const ViewBasis view_basis = compute_view_basis(angles, aim_punch);

    // Track visibility acquisition and choose one delay for each visible target.
    // The timer remains valid while the target stays visible; it is reset as
    // soon as visibility is lost or the target dies.
    for (int i = 0; i < input.packet.player_count; ++i) {
        const auto& player = input.packet.players[i];
        if (!player.active || player.health <= 0) {
            player_first_visible_time[player.entity_index] = 0.0;
            player_delay[player.entity_index] = 0;
            continue;
        }

        bool matches_team = (player.team == input.packet.local_team);
        if (cfg.team_check && matches_team) {
            player_first_visible_time[player.entity_index] = 0.0;
            player_delay[player.entity_index] = 0;
            continue;
        }

        bool player_visible = true;
        if (cfg.vischeck) {
            bool bvh_visible = false;
            if (i < (int)vischeck_result.player_visible.size()) {
                bvh_visible = vischeck_result.player_visible[i];
                if (!cfg.smoke_check && vischeck_result.player_occluded_by_smoke[i]) {
                    bvh_visible = true;
                }
            }
            player_visible = bvh_visible;


        }

        if (player_visible) {
            if (player_first_visible_time[player.entity_index] == 0.0) {
                player_first_visible_time[player.entity_index] = now;
                
                std::random_device rd;
                std::mt19937 arng(rd());
                int min_d = std::min(cfg.trigger_delay_min, cfg.trigger_delay_max);
                int max_d = std::max(cfg.trigger_delay_min, cfg.trigger_delay_max);
                if (min_d < max_d) {
                    std::uniform_int_distribution<int> dist(min_d, max_d);
                    player_delay[player.entity_index] = dist(arng);
                } else {
                    player_delay[player.entity_index] = min_d;
                }

                if (cfg.trigger_debug) {
                    BUNNYWARE_LOG_INFO("[TriggerBot] Enemy player {} (index {}) became visible via bone raycast. Selected delay: {} ms", player.name, player.entity_index, player_delay[player.entity_index]);
                }
            }
        } else {
            if (player_first_visible_time[player.entity_index] > 0.0) {
                if (cfg.trigger_debug) {
                    BUNNYWARE_LOG_INFO("[TriggerBot] Enemy player {} (index {}) is no longer visible. Resetting visibility timer.", player.name, player.entity_index);
                }
                player_first_visible_time[player.entity_index] = 0.0;
                player_delay[player.entity_index] = 0;
            }
        }
    }

    bool conditions_met = false;
    int target_entity_id = -1;
    std::string target_player_name = "";

    // Target Validation Mode:
    // 0 = Crosshair, 1 = Crosshair + Hitbox, 2 = Hitbox (Auto-fire)
    int val_mode = cfg.trigger_validation_mode;


    // Mode: 0 = Simple/Threshold, 1 = Seed-Aware, 2 = Hitchance, 3 = Both
    int trigger_mode = cfg.trigger_mode;

    if (val_mode == 0 || val_mode == 1) {
        // Locked on crosshair target
        if (crosshair_target) {
            bool matches_team = (crosshair_target->team == input.packet.local_team);
            if (cfg.team_check && matches_team) {
                // Teammate check failed
            } else {
                // Reconstruct target capsules
                // If val_mode == 0 (Crosshair), we ignore scaling and use 1.0f, but the perturbed ray can hit anywhere.
                // If val_mode == 1 (Crosshair + Hitbox), we use the scaled hitbox size.
                float active_scale = (val_mode == 0) ? 1.0f : hitbox_scale;
                auto capsules = build_capsules(input.packet.target_hitboxes, *crosshair_target, active_scale, active_hitbox_groups);

                bool seed_hits = false;
                float current_tick_hit_dist = 0.0f;
                if (trigger_mode == 1 || trigger_mode == 3) { // Seed-Aware or Both
                    uint32_t current_seed = compute_spread_seed(
                        hash_angles.x, hash_angles.y, seed_tick);
                    int dummy_bone = 0;
                    seed_hits = check_seed_hit_capsules(current_seed, eye_pos, view_basis, capsules,
                                                         inaccuracy, weapon_spread, recoil_index, weapon_id, num_bullets, spread_seed_val, current_tick_hit_dist, dummy_bone);
                    if (debug_active) {
                        BUNNYWARE_LOG_INFO("[TriggerBot] Seed-Aware check: Seed: {}, Hit: {}, Distance: {:.2f}, Capsules: {}, InCrosshairID: {}", current_seed, seed_hits ? "YES" : "NO", current_tick_hit_dist, (int)capsules.size(), input.packet.local_in_crosshair_id);
                        BUNNYWARE_LOG_INFO("[TriggerBot] Spread values: inaccuracy={:.6f}, base_spread={:.6f}, recoil_index={:.4f}, num_bullets={}", inaccuracy, weapon_spread, recoil_index, num_bullets);
                        if (!capsules.empty()) {
                            BUNNYWARE_LOG_INFO("[TriggerBot] DEBUG POSITIONS: Eye: ({:.1f}, {:.1f}, {:.1f}), Angles: ({:.1f}, {:.1f}), Cap 0 Start: ({:.1f}, {:.1f}, {:.1f}), End: ({:.1f}, {:.1f}, {:.1f}), Rad: {:.1f}",
                                             eye_pos.x, eye_pos.y, eye_pos.z, angles.x, angles.y,
                                             capsules[0].start.x, capsules[0].start.y, capsules[0].start.z,
                                             capsules[0].end.x, capsules[0].end.y, capsules[0].end.z, capsules[0].radius);
                        }
                    }
                }

                float actual_hc = 0.0f;
                bool hitchance_ok = false;
                if (trigger_mode == 2 || trigger_mode == 3) { // Hitchance or Both
                    uint32_t hc_base_seed = compute_spread_seed(
                        hash_angles.x, hash_angles.y, seed_tick);
                    int hits = 0;
                    int samples = 256;
                    int needed_hits = static_cast<int>(std::ceil((active_hitchance / 100.0f) * samples));
                    int samples_checked = 0;
                    for (int i = 0; i < samples; ++i) {
                        samples_checked = i + 1;
                        float hit_d = 0.0f;
                        int dummy_bone = 0;
                        if (check_seed_hit_capsules(hc_base_seed + i, eye_pos, view_basis, capsules,
                                                     inaccuracy, weapon_spread, recoil_index, weapon_id, num_bullets, spread_seed_val, hit_d, dummy_bone)) {
                            hits++;
                        }
                        if (hits >= needed_hits) {
                            break;
                        }
                        if (hits + (samples - samples_checked) < needed_hits) {
                            break;
                        }
                    }
                    actual_hc = ((float)hits / (float)samples_checked) * 100.0f;
                    hitchance_ok = (hits >= needed_hits);
                    if (debug_active) {
                        BUNNYWARE_LOG_INFO("[TriggerBot] Hitchance check: Hits: {}/{} ({:.1f}%%), Required: {:.1f}%%, Status: {}", hits, samples_checked, actual_hc, active_hitchance, hitchance_ok ? "OK" : "BLOCKED");
                        BUNNYWARE_LOG_INFO("[TriggerBot] Spread values: inaccuracy={:.6f}, base_spread={:.6f}, recoil_index={:.4f}", inaccuracy, weapon_spread, recoil_index);
                    }
                } else {
                    actual_hc = seed_hits ? 100.0f : 0.0f;
                }



                if (trigger_mode == 1) {
                    conditions_met = seed_hits;
                } else if (trigger_mode == 2) {
                    conditions_met = hitchance_ok;
                } else if (trigger_mode == 3) {
                    conditions_met = (seed_hits && hitchance_ok);
                }

                if (conditions_met) {
                    target_entity_id = crosshair_target->entity_index;
                    target_player_name = crosshair_target->name;
                    current_target_active_hc = active_hitchance;
                    current_target_actual_hc = actual_hc;
                }
            }
        }
    }
    else if (val_mode == 2) {
        // Hitbox Mode: Scan all players to see if our ray hits them.
        // The spread seed depends only on the (quantized) view angles and tick,
        // not on which player we test, so compute it once for the whole scan
        // instead of re-hashing SHA-1 per player.
        uint32_t hitbox_seed = (trigger_mode == 1 || trigger_mode == 3)
            ? compute_spread_seed(hash_angles.x, hash_angles.y, seed_tick)
            : 0u;

        // Aim axis and worst-case spread deviation, used by the cheap angular
        // pre-filter below to discard players the bullet cone can't reach before
        // paying for a capsule build + seed trace.
        const float pf_deg2rad = M_PI / 180.0f;
        const float pf_vx = (angles.x + aim_punch.x) * pf_deg2rad;
        const float pf_vy = (angles.y + aim_punch.y) * pf_deg2rad;
        const glm::vec3 prefilter_forward{ std::cos(pf_vx) * std::cos(pf_vy),
                                           std::cos(pf_vx) * std::sin(pf_vy),
                                           -std::sin(pf_vx) };
        const float spread_envelope = inaccuracy + weapon_spread; // max lateral offset per unit distance

        int visible_candidates = 0;
        int checked_candidates = 0;
        int hit_checks_run = 0;
        for (int i = 0; i < input.packet.player_count; ++i) {
            const auto& player = input.packet.players[i];
            if (!player.active || player.health <= 0) continue;

            bool matches_team = (player.team == input.packet.local_team);
            if (cfg.team_check && matches_team) continue;

            // Get target visibility
            bool player_visible = true;
            if (cfg.vischeck) {
                bool bvh_visible = false;
                if (i < (int)vischeck_result.player_visible.size()) {
                    bvh_visible = vischeck_result.player_visible[i];
                    
                    if (!cfg.smoke_check && vischeck_result.player_occluded_by_smoke[i]) {
                        bvh_visible = true;
                    }
                }
                player_visible = bvh_visible;
            }
            if (player_visible) {
                visible_candidates++;
            }
            if (!player_visible) continue;

            // Cheap angular pre-filter: reject players the aim cone can't reach.
            // Conservative — it only skips a player when even the widest spread
            // deviation plus the player's body radius can't bring a bullet to the
            // body centre, so it never discards a genuinely hittable target.
            if (player.bone_count >= 24) {
                glm::vec3 center = (player.bones[7].position + player.bones[1].position) * 0.5f;
                glm::vec3 to_center = center - eye_pos;
                float dist = glm::length(to_center);
                if (dist > 1.0f) {
                    glm::vec3 to_dir = to_center / dist;
                    float cos_a = glm::dot(prefilter_forward, to_dir);
                    if (cos_a <= 0.0f) {
                        continue; // behind the view direction
                    }
                    float sin_a = std::sqrt(std::max(0.0f, 1.0f - cos_a * cos_a));
                    float perp = dist * sin_a; // ray-to-centre perpendicular distance
                    // body span (centre -> extended limb/head) + spread reach + margin
                    float max_reach = 70.0f + dist * spread_envelope + 8.0f;
                    if (perp > max_reach) {
                        continue;
                    }
                }
            }

            checked_candidates++;

            // Build capsules from real game hitboxes (same source as SeedTrigger), fall back
            // to skeletal bone-pair capsules if hitbox data is missing for this player.
            auto capsules = build_capsules_from_player_hitboxes(
                input.packet.player_hitboxes[i], player, hitbox_scale, active_hitbox_groups);

            if (capsules.empty()) continue;

            bool target_found = false;
            hit_checks_run++;

            float actual_hc = 0.0f;
            bool hitchance_ok = false;

            if (trigger_mode == 0) {
                // Simple Mode: trace unperturbed camera ray
                float deg2rad = M_PI / 180.0f;
                float view_x = (angles.x + aim_punch.x) * deg2rad;
                float view_y = (angles.y + aim_punch.y) * deg2rad;
                float sp = std::sin(view_x);
                float cp = std::cos(view_x);
                float sy = std::sin(view_y);
                float cy = std::cos(view_y);
                glm::vec3 forward{ cp * cy, cp * sy, -sp };

                float hit_t = 0.0f;
                for (const auto& cap : capsules) {
                    if (ray_hits_capsule(eye_pos, forward, cap, hit_t)) {
                        // Trace ray against map geometry to verify no wall occlusion or check autowall
                        TraceResult tr = vis_worker.trace_ray_bvh(eye_pos, eye_pos + forward * hit_t);
                        if (tr.hit && tr.distance < hit_t - 2.0f) {
                            continue; // Try other capsules or ignore
                        }
                        target_found = true;
                        break;
                    }
                }
            } else {
                bool seed_hits = false;
                float current_tick_hit_dist = 0.0f;
                glm::vec3 perturbed_dir{0.0f};

                if (trigger_mode == 1 || trigger_mode == 3) {
                    uint32_t current_seed = hitbox_seed;

                    int current_tick_hit_bone = 0;
                    seed_hits = check_seed_hit_capsules(current_seed, eye_pos, view_basis, capsules,
                                                         inaccuracy, weapon_spread, recoil_index, weapon_id, num_bullets, spread_seed_val, current_tick_hit_dist, current_tick_hit_bone);

                    if (seed_hits) {
                        float deg2rad = M_PI / 180.0f;
                        float view_x = (angles.x + aim_punch.x) * deg2rad;
                        float view_y = (angles.y + aim_punch.y) * deg2rad;
                        float sp = std::sin(view_x);
                        float cp = std::cos(view_x);
                        float sy = std::sin(view_y);
                        float cy = std::cos(view_y);
                        glm::vec3 forward{ cp * cy, cp * sy, -sp };
                        glm::vec3 right{ -sy, cy, 0.0f };
                        glm::vec3 up{ sp * cy, sp * sy, cp };

                        ValveRandomStream prng;
                        prng.set_seed(current_seed + 1);
                        float r1 = prng.random_float(0.0f, 1.0f);
                        float r2 = prng.random_float(0.0f, M_PI * 2.0f);
                        float r3 = prng.random_float(0.0f, 1.0f);
                        float r4 = prng.random_float(0.0f, M_PI * 2.0f);

                        if (weapon_id == 28 && recoil_index < 3.0f) {
                            float v1 = r1; float v3 = r3; int c = 3;
                            do { c--; v1 = v1 * v1; v3 = v3 * v3; } while (c > recoil_index);
                            r1 = 1.0f - v1; r3 = 1.0f - v3;
                        }
                        if (weapon_id == 64 && num_bullets == 1) {
                            r1 = 1.0f - (r1 * r1); r3 = 1.0f - (r3 * r3);
                        }
                        float total_spread_x = std::cos(r2) * (r1 * inaccuracy) + std::cos(r4) * (r3 * weapon_spread);
                        float total_spread_y = std::sin(r2) * (r1 * inaccuracy) + std::sin(r4) * (r3 * weapon_spread);
                        perturbed_dir = glm::normalize(forward - right * total_spread_x + up * total_spread_y);

                        // Trace ray against map geometry to verify no wall occlusion (only blocks if wall is closer than player)
                        TraceResult tr = vis_worker.trace_ray_bvh(eye_pos, eye_pos + perturbed_dir * current_tick_hit_dist);
                        if (tr.hit && tr.distance < current_tick_hit_dist - 2.0f) {
                            seed_hits = false;
                            if (debug_active) {
                                BUNNYWARE_LOG_INFO("[TriggerBot] Hitbox Mode: Perturbed ray occluded by wall at {:.2f} (Target at {:.2f})", tr.distance, current_tick_hit_dist);
                            }
                        }
                    }
                }

                actual_hc = 0.0f;
                if (trigger_mode == 2 || trigger_mode == 3) {
                    int hits = 0;
                    int samples = 256;
                    int needed_hits = static_cast<int>(std::ceil((active_hitchance / 100.0f) * samples));
                    int samples_checked = 0;
                    for (int seed = 0; seed < samples; ++seed) {
                        samples_checked = seed + 1;
                        float hit_d = 0.0f;
                        int dummy_bone = 0;
                        if (check_seed_hit_capsules(seed, eye_pos, view_basis, capsules,
                                                     inaccuracy, weapon_spread, recoil_index, weapon_id, num_bullets, spread_seed_val, hit_d, dummy_bone)) {
                            hits++;
                        }
                        // Decided already: enough hits to pass, or too few left to reach it.
                        if (hits >= needed_hits) {
                            break;
                        }
                        if (hits + (samples - samples_checked) < needed_hits) {
                            break;
                        }
                    }
                    actual_hc = ((float)hits / (float)samples_checked) * 100.0f;
                    hitchance_ok = (hits >= needed_hits);
                } else {
                    actual_hc = seed_hits ? 100.0f : 0.0f;
                }

                if (trigger_mode == 1) {
                    target_found = seed_hits;
                } else if (trigger_mode == 2) {
                    target_found = hitchance_ok;
                } else if (trigger_mode == 3) {
                    target_found = (seed_hits && hitchance_ok);
                }
            }

            if (target_found) {
                conditions_met = true;
                target_entity_id = player.entity_index;
                target_player_name = player.name;
                current_target_active_hc = active_hitchance;
                current_target_actual_hc = actual_hc;
                if (debug_active) {
                    BUNNYWARE_LOG_INFO("[TriggerBot] Hitbox Mode target locked on player {} (index {})", player.name, i);
                }
                break;
            }
        }

        if (debug_active && !conditions_met) {
            BUNNYWARE_LOG_INFO("[TriggerBot] Hitbox scan complete. Active Enemies: {}, Visible: {}, Checked: {}, RaycastChecks: {}", 
                             input.packet.player_count, visible_candidates, checked_candidates, hit_checks_run);
        }
    }

    if (conditions_met && target_entity_id >= 0) {
        // ── SeedTrigger-style safety checks ──
        // Bucket guard: abort if view angle is near a quantization boundary
        float hash_pitch = angles.x + aim_punch.x;
        float hash_yaw = angles.y + aim_punch.y;
        if (!bucket_is_safe(hash_pitch, cfg.trigger_bucket_guard_degrees) ||
            !bucket_is_safe(hash_yaw, cfg.trigger_bucket_guard_degrees)) {
            if (debug_active) {
                BUNNYWARE_LOG_INFO("[TriggerBot] Bucket guard blocked shot (pitch={:.4f}, yaw={:.4f})", hash_pitch, hash_yaw);
            }
            return;
        }

        // Tick-aware dedup: don't re-evaluate the same tick+angle
        float qp = quantize_angle(hash_pitch);
        float qy = quantize_angle(hash_yaw);
        if (memo_tick_ == seed_tick && memo_pitch_ == qp && memo_yaw_ == qy) return;
        memo_tick_ = seed_tick;
        memo_pitch_ = qp;
        memo_yaw_ = qy;

        if (state == STATE_IDLE || state == STATE_DELAYING) {
            const auto visible_since = player_first_visible_time.find(target_entity_id);
            const auto selected_delay = player_delay.find(target_entity_id);
            if (visible_since == player_first_visible_time.end() ||
                selected_delay == player_delay.end() || visible_since->second <= 0.0) {
                return;
            }

            const double visible_elapsed_ms = (now - visible_since->second) * 1000.0;
            if (visible_elapsed_ms < selected_delay->second) return;

            // Check delay between shots
            if (now < next_allowed_shot_time) return;

            // Populate prediction log for damage tracking
            if (cfg.trigger_debug) {
                int player_idx = -1;
                for (int p = 0; p < input.packet.player_count; ++p) {
                    if (input.packet.players[p].entity_index == target_entity_id) {
                        player_idx = p; break;
                    }
                }
                if (player_idx >= 0) {
                    glm::vec3 ray_dir{0.0f};
                    auto capsules = build_capsules_from_player_hitboxes(
                        input.packet.player_hitboxes[player_idx],
                        input.packet.players[player_idx],
                        hitbox_scale, active_hitbox_groups);
                    populate_prediction(input, input.packet.players[player_idx],
                        eye_pos, ray_dir, capsules, vis_worker,
                        current_target_active_hc, current_target_actual_hc, trigger_mode);
                }
            }

            if (cfg.trigger_debug) {
                BUNNYWARE_LOG_INFO("[TriggerBot] Firing at {} (tick={}, hc_actual={:.1f}%)",
                    target_player_name, seed_tick, current_target_actual_hc);
            }

            // Fire!
            click_press();
            held_ = true;
            release_at_ = now + 0.04; // 40ms hold (same as SeedTrigger default)
            pending_tick_ = seed_tick + 1;
            damage_before_shot = input.packet.local_total_damage;
            damage_before_kills = input.packet.local_total_kills;
            last_shot_time = now;
            is_waiting_for_damage = true;
        }
    }
}

void TriggerBot::populate_prediction(const FrameInput& input, const PlayerData& target_player, const glm::vec3& ray_origin, const glm::vec3& ray_dir, const CapsuleBuffer& capsules, const VisibilityWorker& vis_worker, float active_hitchance, float actual_hitchance, int trigger_mode) {
    last_prediction.target_entity_id = target_player.entity_index;
    last_prediction.target_name = target_player.name;
    last_prediction.armor_value = target_player.armor;
    last_prediction.has_helmet = target_player.has_helmet;
    last_prediction.pen_power = input.packet.local_weapon_penetration;
    last_prediction.base_dmg = input.packet.local_weapon_damage;

    last_prediction.weapon_name = get_weapon_name_from_id(input.packet.held_weapon_id);
    last_prediction.active_hitchance = active_hitchance;
    last_prediction.actual_hitchance = actual_hitchance;
    last_prediction.trigger_mode = trigger_mode;

    // Find intersected bones and sort them by ray parameter t
    struct BoneHit { int bone; float t; };
    std::vector<BoneHit> temp_hits;
    for (const auto& cap : capsules) {
        float t = 0.0f;
        if (ray_hits_capsule(ray_origin, ray_dir, cap, t)) {
            temp_hits.push_back({cap.bone, t});
        }
    }
    
    float hit_t = 0.0f;
    int hit_bone = 5; // default to chest
    std::vector<int> hit_bones;
    if (!temp_hits.empty()) {
        std::sort(temp_hits.begin(), temp_hits.end(), [](const BoneHit& a, const BoneHit& b) { return a.t < b.t; });
        hit_t = temp_hits[0].t;
        hit_bone = temp_hits[0].bone;
        for (const auto& h : temp_hits) {
            hit_bones.push_back(h.bone);
        }
    }
    last_prediction.intersected_bones = hit_bones;

    float thickness = 0.0f;
    float remaining = input.packet.local_weapon_damage;
    if (!temp_hits.empty()) {
        glm::vec3 target_hit_pt = ray_origin + ray_dir * hit_t;
        TraceResult tr = vis_worker.trace_ray_bvh(ray_origin, target_hit_pt);
        if (tr.hit && tr.distance < hit_t - 2.0f) {
            remaining = 0.0f;
            thickness = hit_t - tr.distance;
        }
    }
    last_prediction.thickness = thickness;
    last_prediction.remaining_damage = remaining;
    last_prediction.predicted_damage = scale_damage_by_hitgroup(hit_bone, target_player.armor, target_player.has_helmet, remaining, input.packet.local_weapon_armor_ratio, input.packet.local_weapon_headshot_multiplier);
}

void TriggerBot::click_press()
{
    if (input_ok && evdev_input) {
        evdev_input->clickp(attack_button);
    }
}

void TriggerBot::click_release()
{
    if (input_ok && evdev_input) {
        evdev_input->clickr(attack_button);
    }
}

// SeedTrigger-style bucket guard: aborts if view angle is within guard_degrees of
// a 0.5° quantization boundary. At boundaries the spread seed can flip between buckets.
bool TriggerBot::bucket_is_safe(float angle, float guard_degrees) {
    if (guard_degrees <= 0.0f) return true;
    constexpr float bucket_size = 0.5f;
    float normalized = normalize_angle(angle);
    float bucket_start = std::floor(normalized / bucket_size) * bucket_size;
    float inside = normalized - bucket_start;
    float distance_to_boundary = std::min(inside, bucket_size - inside);
    return distance_to_boundary >= guard_degrees;
}
