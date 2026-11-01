#pragma once
#include <memory>
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include "config.hpp"
#include "FrameInput.hpp"
#include "VisibilityWorker.hpp"
#include "overlay/virtual_input.hpp"
#include "renderer/esp_renderer.hpp"

class LegitAim {
public:
    LegitAim();
    ~LegitAim();

    void update(const FrameInput& input, const OverlayConfig& cfg, const VischeckResult& vis_result, const glm::vec3* override_angles);

private:
    std::unique_ptr<VirtualInput> virtual_input;
    bool uinput_ok = false;
    std::string init_error;

    // Target tracking
    int locked_entity_index = -1;
    glm::vec3 last_target_pos = glm::vec3(0.0f);
    int locked_miss_ticks = 0;

    // Sub-pixel accumulator to prevent mouse jittering
    glm::vec2 subpixel_error = glm::vec2(0.0f);

    // Overshoot tracking
    bool is_overshooting = false;
    float current_overshoot_scale = 1.0f;
    int overshoot_ticks_left = 0;

    // RCS tracking
    glm::vec2 current_rcs_punch = glm::vec2(0.0f);

    // Key press state
    bool was_key_pressed = false;

    // Helper functions
    glm::vec3 predict_target_pos(const glm::vec3& pos, const glm::vec3& vel, float tick_rate) const;
    void inject_mouse_move(float dx, float dy);
};
