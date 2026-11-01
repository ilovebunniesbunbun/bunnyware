#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <limits>
#include <unordered_map>
#include "config.hpp"
#include "FrameInput.hpp"
#include "overlay/virtual_input.hpp"
#include "overlay/global_input.hpp"

#include <unordered_map>

class TriggerBot {
public:
    TriggerBot();
    ~TriggerBot();

    void update(const FrameInput& input, const OverlayConfig& cfg, GlobalInputReader& global_input, const struct VischeckResult& vischeck_result, const class VisibilityWorker& vis_worker);
    bool is_initialized() const { return uinput_ok; }
    const std::string& get_init_error() const { return init_error; }

private:
    enum State {
        STATE_IDLE,
        STATE_DELAYING,
        STATE_SHOOTING
    };

    State state = STATE_IDLE;
    double next_allowed_shot_time = 0.0;
    int current_delay = 0;
    int attack_button = 1; // 1 = Left Click (BTN_LEFT)

    std::unique_ptr<VirtualInput> virtual_input;
    bool uinput_ok = false;
    std::string init_error;

    int last_target_entity_index = -1;
    double first_visible_time = 0.0;
    double last_visible_time = 0.0;

    std::unordered_map<int, double> player_first_visible_time;
    std::unordered_map<int, int> player_delay;

    // SeedTrigger-style reliability primitives
    bool held_ = false;                          // Whether primary fire is currently pressed
    double release_at_ = 0.0;                    // When to release the held click
    int memo_tick_ = -1;                         // Last evaluated tick (dedup)
    float memo_pitch_ = std::numeric_limits<float>::quiet_NaN();
    float memo_yaw_ = std::numeric_limits<float>::quiet_NaN();
    int pending_tick_ = -1;                      // Tick after firing; prevents re-fire until next tick

    double last_shot_time = 0.0;
    float damage_before_shot = -1.0f;
    int damage_before_kills = -1;
    bool is_waiting_for_damage = false;
    float last_known_damage = 0.0f;
    int last_known_kills = 0;
    float current_target_active_hc = 0.0f;
    float current_target_actual_hc = 0.0f;

    struct PredictionLogData {
        float predicted_damage = 0.0f;
        float remaining_damage = 0.0f;
        float thickness = 0.0f;
        float pen_power = 0.0f;
        float base_dmg = 0.0f;
        int target_entity_id = -1;
        std::string target_name;
        int armor_value = 0;
        bool has_helmet = false;
        std::string weapon_name;
        float active_hitchance = 0.0f;
        float actual_hitchance = 0.0f;
        int trigger_mode = 0;
        std::vector<int> intersected_bones;
    };
    PredictionLogData last_prediction;

    void populate_prediction(const FrameInput& input, const PlayerData& target_player, const glm::vec3& ray_origin, const glm::vec3& ray_dir, const struct CapsuleBuffer& capsules, const class VisibilityWorker& vis_worker, float active_hitchance, float actual_hitchance, int trigger_mode);

    bool is_key_down(int key);
    void click_press();
    void click_release();

    // SeedTrigger-style bucket guard: prevents firing when view angle is near a
    // 0.5-degree quantization boundary where the seed could flip.
    static bool bucket_is_safe(float angle, float guard_degrees);

    // Build capsules from per-player hitbox data (real game hitboxes from SHM).
    static struct CapsuleBuffer build_capsules_from_player_hitboxes(
        const TargetHitboxData& hitbox_data, const PlayerData& player,
        float scale_factor, int enabled_groups);

    // Per-tick spread-seed cache. The game only mints a new spread seed when the
    // quantized view angles (0.5 deg steps) or the tick change, but update() runs
    // once per overlay frame (hundreds of FPS). Caching lets the Seeded/Hitchance
    // paths reuse the last SHA-1 result instead of recomputing it every frame and
    // once per player in the hitbox-scan loop.
    uint32_t compute_spread_seed(float pitch, float yaw, int tick);
    float cached_seed_qpitch_ = 0.0f;
    float cached_seed_qyaw_ = 0.0f;
    int cached_seed_tick_ = -1;
    uint32_t cached_seed_ = 0;
    bool seed_cache_valid_ = false;
};
