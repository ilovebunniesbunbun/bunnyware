#include "NoiseEsp.hpp"
#include "logger.hpp"
#include "overlay/esp_drawing.hpp"

#include <GL/glew.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

bool weapon_is_silent(const char* name)
{
    if (!name || !name[0]) return false;
    return std::strcmp(name, "m4a1_silencer") == 0 || std::strcmp(name, "usp_silencer") == 0;
}

bool weapon_is_melee_or_nade(const char* name)
{
    if (!name || !name[0]) return true;
    if (std::strcmp(name, "knife") == 0 || std::strcmp(name, "knife_t") == 0) return true;
    if (std::strcmp(name, "bayonet") == 0) return true;
    if (std::strstr(name, "grenade") || std::strstr(name, "flashbang") ||
        std::strstr(name, "molotov") || std::strstr(name, "incgrenade") ||
        std::strstr(name, "decoy") || std::strcmp(name, "c4") == 0) {
        return true;
    }
    return false;
}

} // namespace

float NoiseEsp::dist_sq(const Vec3& a, const Vec3& b)
{
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

bool NoiseEsp::type_enabled(Type type, const OverlayConfig& cfg) const
{
    switch (type) {
        case Type::Movement: return cfg.esp_noise_movement;
        case Type::Shooting: return cfg.esp_noise_shooting;
        case Type::Reload: return cfg.esp_noise_reload;
        case Type::Landing: return cfg.esp_noise_landing;
        case Type::Scope: return cfg.esp_noise_scope;
        case Type::Plant: return cfg.esp_noise_plant;
        case Type::Defuse: return cfg.esp_noise_defuse;
        default: return false;
    }
}

const Color& NoiseEsp::type_color(Type type, const OverlayConfig& cfg) const
{
    switch (type) {
        case Type::Movement: return cfg.esp_noise_movement_color;
        case Type::Shooting: return cfg.esp_noise_shooting_color;
        case Type::Reload: return cfg.esp_noise_reload_color;
        case Type::Landing: return cfg.esp_noise_landing_color;
        case Type::Scope: return cfg.esp_noise_scope_color;
        case Type::Plant: return cfg.esp_noise_plant_color;
        case Type::Defuse: return cfg.esp_noise_defuse_color;
        default: return cfg.esp_noise_movement_color;
    }
}

float NoiseEsp::type_range(Type type, const char* weapon_name) const
{
    switch (type) {
        case Type::Movement: return kRangeMovement;
        case Type::Shooting:
            return weapon_is_silent(weapon_name) ? kRangeShootingSilenced : kRangeShooting;
        case Type::Reload: return kRangeReload;
        case Type::Landing: return kRangeLanding;
        case Type::Scope: return kRangeScope;
        case Type::Plant: return kRangePlant;
        case Type::Defuse: return kRangeDefuse;
        default: return kRangeMovement;
    }
}

void NoiseEsp::spawn(Type type, const Vec3& pos, const OverlayConfig& cfg, double now, const char* label)
{
    if (events_.size() >= 64) {
        events_.erase(events_.begin());
    }
    Event e;
    e.type = type;
    e.pos = pos;
    e.pos.z += 2.0f;
    e.born = now;
    e.duration = std::max(0.2f, cfg.esp_step_circles_max_age);
    e.show_label = false;
    e.label[0] = '\0';
    if (label && label[0]) {
        std::strncpy(e.label, label, sizeof(e.label) - 1);
        e.label[sizeof(e.label) - 1] = '\0';
        e.show_label = cfg.esp_noise_show_plant_defuse_text;
    }
    events_.push_back(e);
}

void NoiseEsp::detect(const ShmPacket& packet, const OverlayConfig& cfg, double now)
{
    const Vec3 local = packet.local_origin;

    for (int i = 0; i < packet.player_count && i < static_cast<int>(shm::MAX_PLAYERS); ++i) {
        const auto& p = packet.players[i];
        auto& st = player_states_[static_cast<size_t>(i)];

        if (!p.active || p.health <= 0) {
            st = {};
            continue;
        }
        if (cfg.team_check && p.team == packet.local_team) {
            st.has_state = true;
            st.last_emit = p.emit_sound_time;
            st.emit_valid = p.emit_sound_time > 1.0f;
            st.last_shots = p.shots_fired;
            st.was_scoped = p.is_scoped != 0;
            st.was_reloading = p.is_reloading != 0;
            st.was_on_ground = (p.flags & kFlOnGround) != 0;
            continue;
        }

        const bool on_ground = (p.flags & kFlOnGround) != 0;
        const float speed_sq = p.velocity.x * p.velocity.x + p.velocity.y * p.velocity.y;
        const float dist2 = dist_sq(p.origin, local);

        bool emit_edge = false;
        if (p.emit_sound_time > 1.0f) {
            if (!st.emit_valid) {
                st.last_emit = p.emit_sound_time;
                st.emit_valid = true;
            } else if (std::fabs(p.emit_sound_time - st.last_emit) > 1e-4f) {
                st.last_emit = p.emit_sound_time;
                emit_edge = true;
            }
        }

        auto in_range = [&](Type t) {
            const float r = type_range(t, p.active_weapon_name);
            return dist2 <= r * r;
        };

        if (type_enabled(Type::Shooting, cfg) && st.has_state) {
            if (p.shots_fired > st.last_shots && !weapon_is_melee_or_nade(p.active_weapon_name)) {
                if (emit_edge && in_range(Type::Shooting)) {
                    spawn(Type::Shooting, p.origin, cfg, now);
                }
            }
        }
        st.last_shots = p.shots_fired;

        if (type_enabled(Type::Reload, cfg) && st.has_state) {
            if (p.is_reloading && !st.was_reloading) {
                if (emit_edge && in_range(Type::Reload)) {
                    spawn(Type::Reload, p.origin, cfg, now);
                }
            }
        }
        st.was_reloading = p.is_reloading != 0;

        if (type_enabled(Type::Scope, cfg) && st.has_state) {
            if ((p.is_scoped != 0) != st.was_scoped) {
                if (emit_edge && in_range(Type::Scope)) {
                    spawn(Type::Scope, p.origin, cfg, now);
                }
            }
        }
        st.was_scoped = p.is_scoped != 0;

        if (type_enabled(Type::Landing, cfg) && st.has_state) {
            if (on_ground && !st.was_on_ground) {
                const float vz2 = p.velocity.z * p.velocity.z;
                if (vz2 > kMinLandingVzSq && emit_edge && in_range(Type::Landing)) {
                    spawn(Type::Landing, p.origin, cfg, now);
                }
            }
        }
        st.was_on_ground = on_ground;

        if (type_enabled(Type::Movement, cfg) && st.has_state) {
            const bool walking = p.is_walking != 0;
            const bool allow_walk = !cfg.esp_step_circles_hide_while_walking;
            if (on_ground && speed_sq > kMinMovementSpeedSq && (allow_walk || !walking)) {
                if (emit_edge && in_range(Type::Movement)) {
                    spawn(Type::Movement, p.origin, cfg, now);
                }
            }
        }

        st.has_state = true;
    }

    if (type_enabled(Type::Plant, cfg)) {
        const bool arming = packet.c4_arming.active != 0;
        if (arming && !was_arming_) {
            const float d2 = dist_sq(packet.c4_arming.position, local);
            if (d2 <= kRangePlant * kRangePlant) {
                spawn(Type::Plant, packet.c4_arming.position, cfg, now, "PLANTING");
            }
        }
        was_arming_ = arming;
    } else {
        was_arming_ = packet.c4_arming.active != 0;
    }

    if (type_enabled(Type::Defuse, cfg) && packet.planted_c4.active) {
        const bool defusing = packet.planted_c4.is_being_defused != 0;
        if (defusing && !was_defusing_) {
            const float d2 = dist_sq(packet.planted_c4.position, local);
            if (d2 <= kRangeDefuse * kRangeDefuse) {
                spawn(Type::Defuse, packet.planted_c4.position, cfg, now, "DEFUSING");
            }
        }
        was_defusing_ = defusing;
    } else {
        was_defusing_ = packet.planted_c4.active && packet.planted_c4.is_being_defused;
    }

    events_.erase(std::remove_if(events_.begin(), events_.end(),
                                 [&](const Event& e) {
                                     return (now - e.born) > static_cast<double>(e.duration);
                                 }),
                  events_.end());
}

void NoiseEsp::update(const ShmPacket& packet, const OverlayConfig& cfg, double now_sec)
{
    last_now_ = now_sec;
    if (!cfg.esp_enabled || !cfg.esp_step_circles) {
        events_.clear();
        last_detect_time_ = 0.0;
        return;
    }

    constexpr double kDetectInterval = 1.0 / 64.0;
    if (now_sec - last_detect_time_ >= kDetectInterval) {
        last_detect_time_ = now_sec;
        detect(packet, cfg, now_sec);
    } else {
        events_.erase(std::remove_if(events_.begin(), events_.end(),
                                     [&](const Event& e) {
                                         return (now_sec - e.born) > static_cast<double>(e.duration);
                                     }),
                      events_.end());
    }
}

void NoiseEsp::render(EspRenderer& esp, const OverlayConfig& cfg, const float* gl_vp,
                      const float* view_matrix, int screen_w, int screen_h)
{
    if (!cfg.esp_enabled || !cfg.esp_step_circles || events_.empty()) return;

    const double now = last_now_;
    const float start_r = std::max(1.0f, cfg.esp_step_circles_radius); // start radius
    const float max_r = std::max(start_r, cfg.esp_noise_max_radius);
    const float end_r = std::max(1.0f, start_r * 0.35f); // shrink target
    // Expand for first 40% of life, then shrink; fade alpha after 50%
    constexpr float kExpandFrac = 0.40f;
    constexpr float kFadeStartFrac = 0.50f;

    esp.set_projection(gl_vp);

    for (const auto& e : events_) {
        const float life = e.duration > 1e-4f
            ? static_cast<float>((now - e.born) / static_cast<double>(e.duration))
            : 1.0f;
        if (life < 0.0f || life > 1.0f) continue;

        // Radius: expand start → max, then shrink max → end
        float radius;
        if (life <= kExpandFrac) {
            float t = life / kExpandFrac;
            t = 1.0f - (1.0f - t) * (1.0f - t); // ease-out expand
            radius = start_r + (max_r - start_r) * t;
        } else {
            float t = (life - kExpandFrac) / (1.0f - kExpandFrac);
            t = t * t * (3.0f - 2.0f * t); // smoothstep shrink
            radius = max_r + (end_r - max_r) * t;
        }

        // Alpha: full until fade start, then linear to 0
        float alpha_mul = 1.0f;
        if (cfg.esp_step_circles_fade && life >= kFadeStartFrac) {
            alpha_mul = 1.0f - (life - kFadeStartFrac) / (1.0f - kFadeStartFrac);
            if (alpha_mul < 0.0f) alpha_mul = 0.0f;
        }

        const Color& base = type_color(e.type, cfg);
        float outline[4] = {base[0], base[1], base[2], base[3] * alpha_mul};
        if (cfg.esp_step_circles_filled) {
            float fill[4] = {outline[0], outline[1], outline[2], outline[3] * 0.45f};
            esp.add_filled_circle_3d(e.pos, radius, fill);
        }
        esp.add_circle_instance(e.pos, radius, outline);

        if (e.show_label && e.label[0] && view_matrix && screen_w > 0 && screen_h > 0) {
            float sx = 0.0f, sy = 0.0f;
            if (world_to_screen(e.pos, &sx, &sy, view_matrix, screen_w, screen_h)) {
                // Defer text to ImGui in App via a small side channel — store last labels
                LabelDraw ld;
                ld.x = sx;
                ld.y = sy;
                ld.alpha = outline[3];
                std::strncpy(ld.text, e.label, sizeof(ld.text) - 1);
                ld.text[sizeof(ld.text) - 1] = '\0';
                pending_labels_.push_back(ld);
            }
        }
    }

    GLboolean depth_was = glIsEnabled(GL_DEPTH_TEST);
    glDisable(GL_DEPTH_TEST);
    if (cfg.esp_step_circles_filled) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        esp.flush_triangles();
    }
    esp.flush_instances();
    if (depth_was) glEnable(GL_DEPTH_TEST);
}

const std::vector<NoiseEsp::LabelDraw>& NoiseEsp::pending_labels() const
{
    return pending_labels_;
}

void NoiseEsp::clear_pending_labels()
{
    pending_labels_.clear();
}
