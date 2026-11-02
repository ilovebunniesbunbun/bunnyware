#include "EdgeJump.hpp"
#include "logger.hpp"
#include "renderer/esp_renderer.hpp"
#include <chrono>
#include <cmath>
#include <algorithm>

static double get_current_time()
{
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

static int get_linux_key(const OverlayConfig& cfg, const std::string& setting_name, int default_key, GlobalInputReader& global_input)
{
    int glfw_key = get_keybind_key(cfg, setting_name, default_key);
    return global_input.glfw_to_linux_key(glfw_key);
}

static float predict_future_z(float origin_z, float vel_z, bool on_ground, float speed, float distance)
{
    if (on_ground) {
        return origin_z;
    }
    if (speed < 0.001f) return origin_z;
    float t = distance / speed;
    float gravity = 800.0f; // CS2 standard gravity is 800 units/s^2
    return origin_z + vel_z * t - 0.5f * gravity * t * t;
}

EdgeJump::EdgeJump()
{
    try {
        evdev_input = std::make_unique<EvdevInput>();
        input_ok = true;
    } catch (const std::exception& e) {
        init_error = e.what();
        input_ok = false;
        BUNNYWARE_LOG_ERROR("EdgeJump: Failed to initialize evdev input: {}", init_error);
    }
}

EdgeJump::~EdgeJump()
{
    if (input_ok) {
        // Safe releases
        if (is_crouching) {
            // Note: Cannot call get_linux_key here easily, so we manually check standard Ctrl
            // or we rely on EvdevInput's destructor to release the held key when it is torn down.
        }
        release_strafe_key();
    }
}

bool EdgeJump::has_ground(const glm::vec3& p, float tolerance, float ground_check_height, const VisibilityWorker& visibility_worker)
{
    glm::vec3 trace_start(p.x, p.y, p.z + ground_check_height);
    glm::vec3 trace_end(p.x, p.y, p.z - tolerance);
    TraceResult res = visibility_worker.trace_ray_bvh(trace_start, trace_end);
    return res.hit;
}

bool EdgeJump::edges_clear(const glm::vec3& p, const VisibilityWorker& visibility_worker)
{
    // Offset Z slightly upwards to prevent the wall-clearance checks from intersecting the floor mesh
    float check_z = p.z + 18.0f;
    glm::vec3 corners[4] = {
        {p.x + 16.0f, p.y - 16.0f, check_z},
        {p.x + 16.0f, p.y + 16.0f, check_z},
        {p.x - 16.0f, p.y + 16.0f, check_z},
        {p.x - 16.0f, p.y - 16.0f, check_z}
    };
    for (int i = 0; i < 4; ++i) {
        glm::vec3 start = corners[i];
        glm::vec3 finish = corners[(i + 1) % 4];
        TraceResult res = visibility_worker.trace_ray_bvh(start, finish);
        if (res.hit) {
            return false;
        }
    }
    return true;
}

bool EdgeJump::all_traces_no_ground(const glm::vec3& p, float tolerance, float ground_check_height, const VisibilityWorker& visibility_worker)
{
    glm::vec2 offsets[9] = {
        {0.0f, 0.0f},
        {16.0f, -16.0f},
        {16.0f, 16.0f},
        {-16.0f, 16.0f},
        {-16.0f, -16.0f},
        {-16.0f, 0.0f},
        {16.0f, 0.0f},
        {0.0f, 16.0f},
        {0.0f, -16.0f}
    };

    debug_circles.clear();
    bool any_ground = false;

    for (int i = 0; i < 9; ++i) {
        glm::vec3 pt(p.x + offsets[i].x, p.y + offsets[i].y, p.z);
        bool ground_found = has_ground(pt, tolerance, ground_check_height, visibility_worker);
        
        debug_circles.push_back({pt, ground_found});
        if (ground_found) {
            any_ground = true;
        }
    }

    return !any_ground; // Returns true if all points have NO ground
}

void EdgeJump::release_crouch(const OverlayConfig& cfg, GlobalInputReader& global_input)
{
    if (is_crouching) {
        int crouch_key = get_linux_key(cfg, "key_crouch", 341, global_input);
        if (crouch_key >= 0) {
            evdev_input->keyr(crouch_key);
            is_crouching = false;
            if (cfg.ej_debug) {
                BUNNYWARE_LOG_INFO("[EdgeJump] Crouch Released.");
            }
        }
    }
}

void EdgeJump::release_strafe_key()
{
    if (current_strafe_key_code >= 0) {
        evdev_input->keyr(current_strafe_key_code);
    }
    current_strafe_key_held = "";
    current_strafe_key_code = -1;
}

void EdgeJump::press_strafe_key(const std::string& direction, const OverlayConfig& cfg, GlobalInputReader& global_input)
{
    if (current_strafe_key_held == direction) {
        return;
    }
    if (!current_strafe_key_held.empty()) {
        release_strafe_key();
    }

    std::string actual_direction = direction;
    if (was_moving_backwards) {
        actual_direction = (direction == "left") ? "right" : "left";
    }

    if (actual_direction == "left") {
        int left_key = get_linux_key(cfg, "key_a", 65, global_input);
        if (left_key >= 0) {
            evdev_input->keyp(left_key);
            current_strafe_key_code = left_key;
            if (cfg.ej_debug) {
                BUNNYWARE_LOG_INFO("[EdgeJump] Pressing LEFT (requested: {}, backwards: {})", direction, was_moving_backwards);
            }
        }
    } else if (actual_direction == "right") {
        int right_key = get_linux_key(cfg, "key_d", 68, global_input);
        if (right_key >= 0) {
            evdev_input->keyp(right_key);
            current_strafe_key_code = right_key;
            if (cfg.ej_debug) {
                BUNNYWARE_LOG_INFO("[EdgeJump] Pressing RIGHT (requested: {}, backwards: {})", direction, was_moving_backwards);
            }
        }
    }
    current_strafe_key_held = direction;
}

void EdgeJump::cleanup_strafe(const OverlayConfig& cfg)
{
    if (!current_strafe_key_held.empty()) {
        if (cfg.ej_debug) {
            BUNNYWARE_LOG_INFO("[EdgeJump] Strafe Cleanup - releasing keys");
        }
    }
    release_strafe_key();
    strafe_active = false;
    edge_jump_performed = false;
}

void EdgeJump::update(const FrameInput& input, const OverlayConfig& cfg, GlobalInputReader& global_input, const VisibilityWorker& vis_worker)
{
    double now = get_current_time();
    int activation_key = get_keybind_key(cfg, "edgebug_activation_key", 1004);
    
    bool key_down = false;
    if (activation_key >= 0) {
        key_down = global_input.is_key_pressed(activation_key);
    } else {
        key_down = true;
    }

    static double last_debug_log_time = 0.0;
    if (cfg.ej_debug && (now - last_debug_log_time >= 1.0)) {
        last_debug_log_time = now;
        
        bool on_ground = (input.packet.local_flags & (1 << 0)) != 0;
        glm::vec3 velocity = input.packet.local_velocity;
        float actual_speed = std::sqrt(velocity.x * velocity.x + velocity.y * velocity.y);
        
        // Test trace directly downwards 200 units from local player position
        glm::vec3 test_start = input.packet.local_origin;
        glm::vec3 test_end(test_start.x, test_start.y, test_start.z - 200.0f);
        TraceResult test_res = vis_worker.trace_ray_bvh(test_start, test_end);

        BUNNYWARE_LOG_INFO("[EdgeJump] Diagnostics | Enabled: {} | input: {} | Key Bind: {} | Pressed: {} | Ground: {} | Speed: {:.1f}/{} | Pos: ({:.1f}, {:.1f}, {:.1f}) | Test Ground Trace Hit: {} (Dist: {:.1f}) | Debug Circles: {}",
                         cfg.ej_enabled, input_ok, activation_key, key_down,
                         on_ground, actual_speed, cfg.ej_min_movement_speed,
                         input.packet.local_origin.x, input.packet.local_origin.y, input.packet.local_origin.z,
                         test_res.hit, test_res.distance, debug_circles.size());
    }

    if (!input_ok || !cfg.ej_enabled) return;

    // Handle keyup cleanups
    if (key_down != last_key_state) {
        last_key_state = key_down;
        if (!key_down) {
            waiting_for_jump = false;
            release_crouch(cfg, global_input);
            cleanup_strafe(cfg);
        }
    }

    if (!key_down) return;

    // Knife Only check
    {
        bool is_knife = (input.packet.held_weapon_id == 42 ||
                         input.packet.held_weapon_id == 59 ||
                         input.packet.held_weapon_id == 500 ||
                         (input.packet.held_weapon_id >= 505 && input.packet.held_weapon_id <= 526));
        if (cfg.ej_knife_only && !is_knife) {
            if (waiting_for_jump || is_crouching || strafe_active) {
                waiting_for_jump = false;
                release_crouch(cfg, global_input);
                cleanup_strafe(cfg);
            }
            return;
        }
    }

    // Retrieve local player pawn state
    bool on_ground = (input.packet.local_flags & (1 << 0)) != 0;
    if (on_ground && is_crouching) {
        release_crouch(cfg, global_input);
    }
    if (on_ground && !current_strafe_key_held.empty()) {
        cleanup_strafe(cfg);
    }

    // Get current velocity and speed
    glm::vec3 velocity = input.packet.local_velocity;
    float speed = std::sqrt(velocity.x * velocity.x + velocity.y * velocity.y);

    // View direction helper
    float view_yaw = input.packet.local_angles.y;
    float yaw_rad = view_yaw * (3.14159265f / 180.0f);
    float view_dir_x = std::cos(yaw_rad);
    float view_dir_y = std::sin(yaw_rad);
    float view_perp_x = -view_dir_y;
    float view_perp_y = view_dir_x;

    float dir_x = 0.0f;
    float dir_y = 0.0f;
    if (speed > 0.1f) {
        dir_x = velocity.x / speed;
        dir_y = velocity.y / speed;
    } else {
        dir_x = view_dir_x;
        dir_y = view_dir_y;
    }
    float ahead_x = input.packet.local_origin.x + dir_x * cfg.ej_check_distance;
    float ahead_y = input.packet.local_origin.y + dir_y * cfg.ej_check_distance;

    float future_z = predict_future_z(input.packet.local_origin.z, velocity.z, on_ground, speed, cfg.ej_check_distance);
    debug_future_z = future_z;

    // Populate debug circles
    bool no_ground_ahead = all_traces_no_ground(glm::vec3(ahead_x, ahead_y, future_z), cfg.ej_ground_tolerance, cfg.ej_ground_check_height, vis_worker);

    // Non-blocking simulated key releases
    static double jump_release_time = 0.0;
    static int active_jump_key = -1;
    if (active_jump_key >= 0 && now >= jump_release_time) {
        evdev_input->keyr(active_jump_key);
        active_jump_key = -1;
    }

    // Edge Detection
    if (on_ground && 
        speed >= cfg.ej_min_movement_speed &&
        no_ground_ahead &&
        edges_clear(glm::vec3(ahead_x, ahead_y, future_z), vis_worker) && 
        !waiting_for_jump) 
    {
        edge_detected_time = now;
        waiting_for_jump = true;

        if (cfg.ej_debug) {
            BUNNYWARE_LOG_INFO("[EdgeJump] Edge Found! Speed: {:.1f} | Pos: ({:.1f}, {:.1f}, {:.1f}) | Ahead: ({:.1f}, {:.1f}) | Future Z: {:.1f}",
                             speed, input.packet.local_origin.x, input.packet.local_origin.y, input.packet.local_origin.z, ahead_x, ahead_y, future_z);
        }
    }

    // Jump Execution
    if (waiting_for_jump && ((now - edge_detected_time) * 1000.0 >= cfg.ej_jump_delay_ms)) {
        int jump_key = get_linux_key(cfg, "key_jump", 32, global_input);
        if (jump_key >= 0) {
            evdev_input->keyp(jump_key);
            jump_release_time = now + 0.03; // Release in 30ms
            active_jump_key = jump_key;

            if (cfg.ej_crouch_on_jump) {
                int crouch_key = get_linux_key(cfg, "key_crouch", 341, global_input);
                if (crouch_key >= 0) {
                    scheduled_crouch_time = now + 0.005; // Schedule crouch in 5ms
                    scheduled_crouch_code = crouch_key;
                }
            }

            if (cfg.ej_strafe_assist) {
                edge_jump_performed = true;
                jump_time = now;

                float forward_velocity = velocity.x * view_dir_x + velocity.y * view_dir_y;
                float sideways_velocity = velocity.x * view_perp_x + velocity.y * view_perp_y;
                initial_forward_velocity = forward_velocity;
                initial_sideways_velocity = sideways_velocity;
                was_moving_backwards = forward_velocity < 0.0f;
                last_delta_yaw = 0.0f;
                previous_yaw = view_yaw;
            }

            waiting_for_jump = false;
            if (cfg.ej_debug) {
                BUNNYWARE_LOG_INFO("[EdgeJump] Jump Executed.");
            }
        }
    }

    // Crouch Schedule Execution
    if (scheduled_crouch_time > 0.0 && now >= scheduled_crouch_time) {
        if (scheduled_crouch_code >= 0) {
            evdev_input->keyp(scheduled_crouch_code);
            is_crouching = true;
            if (cfg.ej_debug) {
                BUNNYWARE_LOG_INFO("[EdgeJump] Crouch Held.");
            }
        }
        scheduled_crouch_time = 0.0;
        scheduled_crouch_code = -1;
    }

    // Strafe Assist Logic
    if (cfg.ej_strafe_assist && edge_jump_performed && key_down) {
        player_yaw = view_yaw;

        if (!on_ground || (now - jump_time < 0.05)) {
            // Failsafe: if sideways velocity is higher than forward velocity, release strafe keys
            if (std::abs(initial_sideways_velocity) > std::abs(initial_forward_velocity)) {
                release_strafe_key();
            } else {
                strafe_active = true;

                float delta_yaw = player_yaw - previous_yaw;
                if (delta_yaw > 180.0f) {
                    delta_yaw -= 360.0f;
                } else if (delta_yaw < -180.0f) {
                    delta_yaw += 360.0f;
                }

                bool has_yaw_movement = std::abs(delta_yaw) > 0.1f;
                bool direction_flipped = (last_delta_yaw > 0.0f && delta_yaw < 0.0f) || (last_delta_yaw < 0.0f && delta_yaw > 0.0f);

                if (cfg.ej_debug && (has_yaw_movement || direction_flipped)) {
                    BUNNYWARE_LOG_INFO("[EdgeJump] Delta Yaw: {:.2f} | Has Movement: {} | Flipped: {} | Last Delta: {:.2f} | Current Key: {}",
                                     delta_yaw, has_yaw_movement, direction_flipped, last_delta_yaw, current_strafe_key_held);
                }

                if (std::abs(delta_yaw) > 0.1f) {
                    if (has_yaw_movement || direction_flipped) {
                        std::string desired_direction = "";
                        if (delta_yaw > 0.0f) {
                            desired_direction = "left";
                        } else if (delta_yaw < 0.0f) {
                            desired_direction = "right";
                        }

                        if (!desired_direction.empty()) {
                            press_strafe_key(desired_direction, cfg, global_input);
                        } else {
                            release_strafe_key();
                        }

                        last_delta_yaw = delta_yaw;
                    }
                } else if (!current_strafe_key_held.empty()) {
                    if (cfg.ej_debug) {
                        BUNNYWARE_LOG_INFO("[EdgeJump] Dead zone ({:.2f}) - keeping {} held", delta_yaw, current_strafe_key_held);
                    }
                }
            }
        } else {
            if (strafe_active) {
                if (cfg.ej_debug) {
                    BUNNYWARE_LOG_INFO("[EdgeJump] Back on ground or past jump window - cleaning up");
                }
                cleanup_strafe(cfg);
            }
        }

        previous_yaw = player_yaw;
    } else if (cfg.ej_strafe_assist && strafe_active) {
        if (cfg.ej_debug) {
            BUNNYWARE_LOG_INFO("[EdgeJump] Strafe assist deactivated - key released");
        }
        cleanup_strafe(cfg);
    }
}
