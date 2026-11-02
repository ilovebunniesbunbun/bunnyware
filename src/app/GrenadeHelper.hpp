#pragma once
#include <memory>
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include "config.hpp"
#include "FrameInput.hpp"
#include "VisibilityWorker.hpp"
#include "overlay/evdev_input.hpp"
#include "overlay/global_input.hpp"
#include "GrenadeHelperData.hpp"
#include "LegitAim.hpp"

class GrenadeHelper {
public:
    GrenadeHelper(const OverlayConfig& cfg);
    ~GrenadeHelper();

    void update(const FrameInput& input, OverlayConfig& cfg, GlobalInputReader& global_input, 
                const std::vector<GrenadeLineup>& lineups);

    // Returns true while the grenade helper is overriding legit aim targeting
    bool is_aim_override_active() const { return aim_override_active_; }
    const glm::vec3& get_override_angles() const { return override_angles_; }

private:
    std::unique_ptr<EvdevInput> evdev_input_;
    bool input_ok_ = false;
    std::string init_error_;

    // State machine fields
    int throw_step_ = 0; // 0 = idle
    double step_start_time_ = 0.0;
    double step_duration_ = 0.0;
    double hover_start_time_ = 0.0;
    double last_throw_time_ = 0.0;
    bool is_hovering_ = false;

    // Lineup being thrown
    GrenadeLineup active_lineup_;

    // Target tracking
    bool aim_override_active_ = false;
    glm::vec3 override_angles_ = glm::vec3(0.0f);

    // Helper keys helper
    int get_linux_key_helper(const OverlayConfig& cfg, const std::string& setting_name, int default_key, GlobalInputReader& global_input);
    int get_movement_key_code(const std::string& key_char, const OverlayConfig& cfg, GlobalInputReader& global_input);
    int get_modifier_key_code(const std::string& mod_name, const OverlayConfig& cfg, GlobalInputReader& global_input);

    void release_all_keys(const OverlayConfig& cfg, GlobalInputReader& global_input);
};
