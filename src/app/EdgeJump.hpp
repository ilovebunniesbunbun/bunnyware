#pragma once
#include <memory>
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include "config.hpp"
#include "FrameInput.hpp"
#include "overlay/virtual_input.hpp"
#include "overlay/global_input.hpp"
#include "VisibilityWorker.hpp"

class EdgeJump {
public:
    EdgeJump();
    ~EdgeJump();

    void update(const FrameInput& input, const OverlayConfig& cfg, GlobalInputReader& global_input, const VisibilityWorker& vis_worker);
    bool is_initialized() const { return uinput_ok; }
    const std::string& get_init_error() const { return init_error; }

private:
    std::unique_ptr<VirtualInput> virtual_input;
    bool uinput_ok = false;
    std::string init_error;

    // Edge Jump State
    bool last_key_state = false;
    bool is_crouching = false;
    double edge_detected_time = 0.0;
    bool waiting_for_jump = false;
    double scheduled_crouch_time = 0.0;
    int scheduled_crouch_code = -1;
    bool edge_jump_performed = false;
    bool was_moving_backwards = false;
    bool strafe_active = false;
    std::string current_strafe_key_held; // "left", "right", or ""
    int current_strafe_key_code = -1;
    float player_yaw = 0.0f;
    float previous_yaw = 0.0f;
    float initial_forward_velocity = 0.0f;
    float initial_sideways_velocity = 0.0f;
    float last_delta_yaw = 0.0f;
    double jump_time = 0.0;

    // Last trace results cached for drawing debug
    struct DebugCircle {
        glm::vec3 pos;
        bool has_ground;
    };
    std::vector<DebugCircle> debug_circles;
    float debug_future_z = 0.0f;

    void release_crouch(const OverlayConfig& cfg, GlobalInputReader& global_input);
    void release_strafe_key();
    void press_strafe_key(const std::string& direction, const OverlayConfig& cfg, GlobalInputReader& global_input);
    void cleanup_strafe(const OverlayConfig& cfg);

    bool has_ground(const glm::vec3& p, float tolerance, float ground_check_height, const VisibilityWorker& visibility_worker);
    bool edges_clear(const glm::vec3& p, const VisibilityWorker& visibility_worker);
    bool all_traces_no_ground(const glm::vec3& p, float tolerance, float ground_check_height, const VisibilityWorker& visibility_worker);
};
