#include "GrenadeHelper.hpp"
#include "logger.hpp"
#include <chrono>
#include <cmath>
#include <algorithm>
#include <random>

static double get_time_now() {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(now.time_since_epoch()).count();
}

static int get_keybind_key(const OverlayConfig& cfg, const std::string& setting_name, int default_key)
{
    for (const auto& kb : cfg.keybinds) {
        if (kb.setting_name == setting_name) {
            return kb.key;
        }
    }
    return default_key;
}

GrenadeHelper::GrenadeHelper(const OverlayConfig& /*cfg*/)
{
    try {
        virtual_input_ = std::make_unique<VirtualInput>();
        uinput_ok_ = true;
        BUNNYWARE_LOG_INFO("GrenadeHelper: VirtualInput initialized successfully.");
    } catch (const std::exception& e) {
        init_error_ = e.what();
        uinput_ok_ = false;
        BUNNYWARE_LOG_ERROR("GrenadeHelper: Failed to initialize VirtualInput: {}", init_error_);
    }
}

GrenadeHelper::~GrenadeHelper()
{
}

int GrenadeHelper::get_linux_key_helper(const OverlayConfig& cfg, const std::string& setting_name, int default_key, GlobalInputReader& global_input)
{
    int glfw_key = get_keybind_key(cfg, setting_name, default_key);
    return global_input.glfw_to_linux_key(glfw_key);
}

int GrenadeHelper::get_movement_key_code(const std::string& key_char, const OverlayConfig& cfg, GlobalInputReader& global_input)
{
    if (key_char == "w") return get_linux_key_helper(cfg, "key_w", 87, global_input);
    if (key_char == "a") return get_linux_key_helper(cfg, "key_a", 65, global_input);
    if (key_char == "s") return get_linux_key_helper(cfg, "key_s", 83, global_input);
    if (key_char == "d") return get_linux_key_helper(cfg, "key_d", 68, global_input);
    return -1;
}

int GrenadeHelper::get_modifier_key_code(const std::string& mod_name, const OverlayConfig& cfg, GlobalInputReader& global_input)
{
    if (mod_name == "crouch") return get_linux_key_helper(cfg, "key_crouch", 341, global_input);
    if (mod_name == "shift") return get_linux_key_helper(cfg, "key_shift", 340, global_input);
    return -1;
}

void GrenadeHelper::release_all_keys(const OverlayConfig& cfg, GlobalInputReader& global_input)
{
    if (!uinput_ok_ || !virtual_input_) return;

    // Release mouse buttons
    virtual_input_->clickr(1);
    virtual_input_->clickr(2);

    // Release movement keys
    int key_codes[] = {
        get_linux_key_helper(cfg, "key_w", 87, global_input),
        get_linux_key_helper(cfg, "key_a", 65, global_input),
        get_linux_key_helper(cfg, "key_s", 83, global_input),
        get_linux_key_helper(cfg, "key_d", 68, global_input),
        get_linux_key_helper(cfg, "key_jump", 32, global_input),
        get_linux_key_helper(cfg, "key_crouch", 341, global_input),
        get_linux_key_helper(cfg, "key_shift", 340, global_input)
    };

    for (int code : key_codes) {
        if (code >= 0) {
            virtual_input_->keyr(code);
        }
    }
}

static float normalize_yaw(float yaw) {
    while (yaw > 180.0f) yaw -= 360.0f;
    while (yaw < -180.0f) yaw += 360.0f;
    return yaw;
}

void GrenadeHelper::update(const FrameInput& input, OverlayConfig& cfg, GlobalInputReader& global_input, 
                           const std::vector<GrenadeLineup>& lineups)
{
    if (!cfg.grenade_helper_enabled) {
        aim_override_active_ = false;
        if (throw_step_ > 0) {
            release_all_keys(cfg, global_input);
            throw_step_ = 0;
        }
        return;
    }

    double now = get_time_now();
    bool can_throw = (now - last_throw_time_) >= 1.0;

    // 1. Process Throw State Machine
    if (throw_step_ > 0) {
        aim_override_active_ = true; // Lock view angles during throw sequence
        override_angles_ = { active_lineup_.viewangles.x, active_lineup_.viewangles.y, 0.0f };

        double elapsed = now - step_start_time_;
        if (elapsed >= step_duration_) {
            if (throw_step_ == 1) {
                // Modifiers (crouch, shift) are already pressed in State 1 initialization
                // Check if crouch is in modifiers to delay for crouch timing
                bool has_crouch = false;
                for (const auto& mod : active_lineup_.movement.modifiers) {
                    if (mod == "crouch") has_crouch = true;
                }
                if (has_crouch) {
                    throw_step_ = 2;
                    step_start_time_ = now;
                    double base_delay = active_lineup_.movement.jump ? 0.400 : 0.450;
                    double jitter = (std::rand() % 10) / 1000.0;
                    step_duration_ = base_delay + jitter;
                } else {
                    throw_step_ = 3;
                    step_start_time_ = now;
                    step_duration_ = 0.0;
                }
            }
            else if (throw_step_ == 2) {
                throw_step_ = 3;
                step_start_time_ = now;
                step_duration_ = 0.0;
            }
            else if (throw_step_ == 3) {
                // Click throw buttons
                if (uinput_ok_ && virtual_input_) {
                    if (active_lineup_.movement.throw_type == "left") {
                        virtual_input_->clickp(1);
                    } else if (active_lineup_.movement.throw_type == "right") {
                        virtual_input_->clickp(2);
                    } else if (active_lineup_.movement.throw_type == "middle") {
                        virtual_input_->clickp(1);
                        virtual_input_->clickp(2);
                    }
                }
                // Transition to movement hold
                throw_step_ = 5;
                step_start_time_ = now;
                double movement_sec = active_lineup_.movement.movement_ticks * 0.015625;
                double jitter = ((std::rand() % 10) - 5) / 1000.0;
                step_duration_ = std::max(0.0, movement_sec + jitter);

                if (uinput_ok_ && virtual_input_ && !active_lineup_.movement.movement_key.empty()) {
                    int move_key = get_movement_key_code(active_lineup_.movement.movement_key, cfg, global_input);
                    if (move_key >= 0) {
                        virtual_input_->keyp(move_key);
                    }
                }
            }
            else if (throw_step_ == 5) {
                // Movement hold finished. Transition to Jump
                throw_step_ = 6;
                step_start_time_ = now;
                step_duration_ = 0.0;

                if (uinput_ok_ && virtual_input_ && active_lineup_.movement.jump) {
                    int jump_key = get_linux_key_helper(cfg, "key_jump", 32, global_input);
                    if (jump_key >= 0) {
                        virtual_input_->keyp(jump_key);
                    }
                    step_duration_ = 0.020; // 20ms for jump registration
                }
            }
            else if (throw_step_ == 6) {
                // Release mouse button (throw!)
                if (uinput_ok_ && virtual_input_) {
                    if (active_lineup_.movement.throw_type == "left") {
                        virtual_input_->clickr(1);
                    } else if (active_lineup_.movement.throw_type == "right") {
                        virtual_input_->clickr(2);
                    } else if (active_lineup_.movement.throw_type == "middle") {
                        virtual_input_->clickr(1);
                        virtual_input_->clickr(2);
                    }
                }

                throw_step_ = 9;
                step_start_time_ = now;

                bool has_crouch = false;
                for (const auto& mod : active_lineup_.movement.modifiers) {
                    if (mod == "crouch") has_crouch = true;
                }
                double hold_sec = 0.080;
                if (has_crouch) {
                    hold_sec = active_lineup_.movement.jump ? 0.250 : 0.300;
                }
                double jitter = (std::rand() % 10) / 1000.0;
                step_duration_ = hold_sec + jitter;
            }
            else if (throw_step_ == 9) {
                release_all_keys(cfg, global_input);
                throw_step_ = 0;
                last_throw_time_ = now;
                BUNNYWARE_LOG_INFO("GrenadeHelper: Throw automation completed.");
            }
        }
        return;
    }

    // 2. Scan player weapons and check if holding a grenade
    uint8_t target_grenade_type = GRENADE_NONE;
    switch (input.packet.held_weapon_id) {
        case 44: target_grenade_type = GRENADE_HE; break;
        case 43: target_grenade_type = GRENADE_FLASH; break;
        case 45: target_grenade_type = GRENADE_SMOKE; break;
        case 46:
        case 48: target_grenade_type = GRENADE_MOLOTOV; break;
        case 47: target_grenade_type = GRENADE_DECOY; break;
    }

    if (target_grenade_type == GRENADE_NONE) {
        aim_override_active_ = false;
        is_hovering_ = false;
        return;
    }

    // Check key activation
    bool helper_key_down = false;
    for (const auto& kb : cfg.keybinds) {
        if (kb.setting_name == "grenade_helper_active") {
            helper_key_down = global_input.is_key_pressed(kb.key);
            break;
        }
    }

    // Find if player is standing on any lineup spot
    glm::vec3 local_pos = {input.packet.local_origin.x, input.packet.local_origin.y, input.packet.local_origin.z};
    const GrenadeLineup* best_lineup = nullptr;
    float best_fov = 9999.0f;

    for (const auto& lineup : lineups) {
        if (lineup.weapon != target_grenade_type) continue;

        glm::vec3 diff = {lineup.position.x - local_pos.x, lineup.position.y - local_pos.y, lineup.position.z - local_pos.z};
        float dist_2d = std::sqrt(diff.x * diff.x + diff.y * diff.y);
        float dist_z = std::abs(diff.z);

        if (dist_2d <= cfg.grenade_helper_aim_radius && dist_z <= 80.0f) {
            float delta_pitch = lineup.viewangles.x - input.packet.local_angles.x;
            float delta_yaw = normalize_yaw(lineup.viewangles.y - input.packet.local_angles.y);
            float fov = std::sqrt(delta_pitch * delta_pitch + delta_yaw * delta_yaw);

            // Only consider lineups within the configured auto-aim FOV (no full-360 pull)
            if (fov > cfg.grenade_helper_aim_fov) continue;

            if (fov < best_fov) {
                best_fov = fov;
                best_lineup = &lineup;
            }
        }
    }

    if (!best_lineup) {
        aim_override_active_ = false;
        is_hovering_ = false;
        return;
    }

    // Auto Aim
    if (cfg.grenade_helper_auto_aim && helper_key_down) {
        aim_override_active_ = true;
        override_angles_ = { best_lineup->viewangles.x, best_lineup->viewangles.y, 0.0f };
    } else {
        aim_override_active_ = false;
    }

    // Auto Throw
    if (cfg.grenade_helper_auto_throw && helper_key_down && can_throw) {
        if (best_fov <= 0.15f) {
            if (!is_hovering_) {
                is_hovering_ = true;
                hover_start_time_ = now;
            } else {
                if ((now - hover_start_time_) >= 0.050) { // 50ms hover delay
                    throw_step_ = 1;
                    active_lineup_ = *best_lineup;
                    step_start_time_ = now;
                    step_duration_ = 0.0;
                    is_hovering_ = false;

                    BUNNYWARE_LOG_INFO("GrenadeHelper: Triggering throw sequence for: {}", active_lineup_.title);

                    // Press modifiers immediately
                    if (uinput_ok_ && virtual_input_) {
                        for (const auto& mod : active_lineup_.movement.modifiers) {
                            int mod_key = get_modifier_key_code(mod, cfg, global_input);
                            if (mod_key >= 0) {
                                virtual_input_->keyp(mod_key);
                            }
                        }
                    }
                }
            }
        } else {
            is_hovering_ = false;
        }
    } else {
        is_hovering_ = false;
    }
}
