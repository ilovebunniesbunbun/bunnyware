#pragma once
#include <memory>
#include <string>
#include <future>
#include <optional>
#include <chrono>
#include <unordered_map>
#include "config.hpp"
#include "overlay/overlay_client.hpp"
#include "menu/menu_client.hpp"
#include "RadarServer.hpp"
#include "renderer/gpu_chams.hpp"
#include "renderer/depth_prepass.hpp"
#include "renderer/esp_renderer.hpp"
#include "model_cache.hpp"
#include "overlay/game_reader.hpp"
#include "overlay/focus_monitor.hpp"
#include "overlay/global_input.hpp"
#include "VisibilityWorker.hpp"
#include "EdgeJump.hpp"
#include "LegitAim.hpp"
#include "GrenadeTracker.hpp"
#include "GrenadeRenderer.hpp"
#include "GrenadeHelperData.hpp"
#include "GrenadeHelperRenderer.hpp"
#include "GrenadeHelper.hpp"
#include "SvgCache.hpp"
#include "FrameScheduler.hpp"
#include "FrameInput.hpp"
#include "FrameState.hpp"
#include "renderer/gpu_profiler.hpp"
#include "renderer/Passes.hpp"
#include "CStrafeTrainer.hpp"
#include "AutoAccept.hpp"
#include "CrosshairOverlay.hpp"
#include "CrosshairDistance.hpp"
#include "NoiseEsp.hpp"
#include "ProximityBeep.hpp"
#include "BeepPlayer.hpp"
#include "DynamicHiding.hpp"
#include "HitReveal.hpp"
#include "HitEffects.hpp"
#include "DamageFx.hpp"
#include "KillVictimMatcher.hpp"

#include <glm/glm.hpp>

class App {
public:
    App(const OverlayConfig& initial_cfg);
    ~App();

    int run();
    static App* get_instance() { return g_instance; }
    LandingZoneManager& get_landing_zone_manager() { return landing_zone_manager; }
    const std::string& get_current_map() const { return current_map; }
    VisibilityWorker& get_visibility_worker() { return visibility_worker; }
    GrenadeHelperData& get_grenade_helper_data() { return grenade_helper_data; }
    const FrameInput& get_last_input() const { return last_input; }

private:
    bool initialize_system();
    void process_frame();
    void update_map_loading();
    void prepare_frame_input(FrameInput& input);
    void preprocess_frame_state(const FrameInput& input, FrameState& state);
    void render_frame(const FrameInput& input, FrameState& state);
    void render_menu();
    void set_game_activity_allowed(bool allowed);
    void start_input_modules();
    void stop_input_modules();

    // Zone creation UI state
    bool creating_zone_prompt_ = false;
    char zone_name_buf_[64] = "";
    int zone_grenade_type_ = 0;
    bool create_bind_was_pressed_ = false;

    OverlayConfig cfg;
    std::unique_ptr<OverlayClient> overlay;
    std::unique_ptr<MenuClient> menu;

    static App* g_instance;

    GpuChamsRenderer chams_renderer;
    DepthPrepassRenderer depth_prepass;
    EspRenderer esp_renderer;
    ModelCache model_cache;
    GameReader game_reader;
    GlobalInputReader global_input;
    bool quit_requested = false;

    // --- Embedded web radar (FC2observ-style) ---
    radar::RadarServer radar_server;
    bool radar_running_ = false;
    std::string radar_applied_host_;
    int radar_applied_http_port_ = 0;
    int radar_applied_ws_port_ = 0;
    double radar_last_publish_time_ = 0.0;
    std::unordered_map<uint32_t, double> radar_smoke_first_seen_;
    void process_radar();
    void update_radar_snapshot();
    bool start_radar();
    void stop_radar();
    static std::string radar_asset_root();
    std::unique_ptr<FocusMonitor> focus_monitor;
    bool game_activity_allowed_ = false;
    bool overlay_hidden_ = false;

    VisibilityWorker visibility_worker;
    GrenadeTracker grenade_tracker;
    GrenadeRenderer grenade_renderer;
    GrenadeHelperData grenade_helper_data;
    GrenadeHelperRenderer grenade_helper_renderer;
    LandingZoneManager landing_zone_manager;
    SvgCache svg_cache;
    FrameScheduler scheduler;
    GpuProfiler gpu_profiler;

    // Window dimensions and scaling geometries
    int window_width = 0;
    int window_height = 0;
    int window_x = 0;
    int window_y = 0;

    // IPC performance metrics
    double total_ipc_time_ms = 0.0;
    double total_cpu_time_ms = 0.0;
    double total_gpu_time_ms = 0.0;
    double max_ipc_time_ms = 0.0;
    double max_cpu_time_ms = 0.0;
    double max_gpu_time_ms = 0.0;
    uint32_t metrics_count = 0;
    uint32_t packet_count_since_print = 0;
    std::chrono::steady_clock::time_point last_debug_print;
    bool first_packet = true;

    FrameInput last_input{};
    bool has_valid_input = false;
    VischeckResult last_vischeck_result{};

    // Asynchronous map loading state
    std::future<MapParser::MapMesh> map_load_future;
    bool map_loading = false;
    std::string current_map;

    GLFWwindow* current_context = nullptr;
    void make_context_current(GLFWwindow* target);

    // --- 2D Bomb Bar State ---
    float bomb_bar_display_ratio = 1.0f;
    float bomb_bar_fade_alpha = 0.0f;
    float bomb_bar_fade_time_left = 0.0f;
    PlantedC4 bomb_bar_fade_c4{};
    std::string bomb_bar_last_map;
    void draw_static_bomb_bar(const FrameInput& input);
    void process_keybinds();

    // --- Keybinds Viewer State ---
    struct KeybindVisualState {
        std::string label;
        std::string status;
        bool active = false;
        float alpha = 0.0f;
    };
    std::unordered_map<std::string, KeybindVisualState> keybind_viewer_states;
    float keybind_box_fade_alpha = 0.0f;
    float keybind_box_height = 0.0f;
    void draw_keybinds_viewer(float dt);

    // --- Spectators Viewer State ---
    struct SpectatorVisualState {
        std::string name;
        bool is_first_person = false;
        float alpha = 0.0f;
        bool active = false;
    };
    std::unordered_map<std::string, SpectatorVisualState> spectator_viewer_states;
    float spectator_box_fade_alpha = 0.0f;
    float spectator_box_height = 0.0f;
    void draw_spectators_viewer(float dt);

    // --- Hitmarker & Kill FX State ---
    struct ActiveEffect3D {
        Vec3 world_pos;
        bool is_2d = false;
        enum Type { TYPE_HITMARKER, TYPE_KILL } type;
        std::string text;
        uint32_t color = 0;
        double start_time = 0.0;
        double duration = 0.0;
        bool is_headshot = false;
        float screen_offset_x = 0.0f;
        float screen_offset_y = 0.0f;
    };
    std::vector<ActiveEffect3D> active_effects_3d;
    std::unordered_map<std::string, int> previous_player_health;
    std::unordered_map<std::string, Vec3> last_known_position;

    struct PlayerFrameCache {
        unsigned int vao = 0;
        unsigned int ibo = 0;
        size_t index_count = 0;
        std::vector<source2::Mat3x4> skinning_palette;
        Vec3 position;
    };
    std::unordered_map<std::string, PlayerFrameCache> player_frame_caches;
    // A vanished enemy's last drawable state, waiting for the kill counter (Kill FX).
    struct KillVictim {
        std::optional<PlayerFrameCache> cache;
        Vec3 death_pos{};
    };
    killfx::KillVictimMatcher<KillVictim> kill_victims_;
    std::vector<DeadPlayerCham> active_kill_chams;
    // Hit FX ghost afterimage (Chams mode): a short-lived copy of the hit player.
    std::vector<DeadPlayerCham> active_hit_chams;
    // DamageFX world-space effects, one pool per half of the card.
    damagefx::DamageFx kill_fx_effects;
    damagefx::DamageFx hit_fx_effects;

    float previous_total_damage = -1.0f;
    int previous_total_kills = -1;
    int previous_total_kills_headshots = -1;
    double hitmarker_time = 0.0;       // glfwGetTime() of last hit
    double kill_time = 0.0;            // glfwGetTime() of last kill
    bool kill_was_headshot = false;    // Whether last kill was headshot
    std::string killed_player_name;    // Name of player killed
    std::unique_ptr<class TriggerBot> trigger_bot;
    std::unique_ptr<EdgeJump> edge_jump;
    std::unique_ptr<LegitAim> legit_aim;
    std::unique_ptr<GrenadeHelper> grenade_helper;
    std::unique_ptr<CStrafeTrainer> cstrafe_trainer;
    AutoAccept auto_accept;
    CrosshairOverlay crosshair_overlay;
    CrosshairDistance crosshair_distance;
    NoiseEsp noise_esp;
    ProximityBeep proximity_beep;
    BeepPlayer beep_player;

    HitReveal hit_reveal;
    hitfx::HitDetector hit_fx_detector;
    hitfx::HitFx hit_fx;
    impacts::Consumer bullet_impacts_consumer_;
    hitfx::ObservedFlashSelection bullet_impacts_selection_;
    std::unique_ptr<FrameInput> esp_reveal_frame;
    FrameState esp_reveal_state;
    OverlayConfig hidden_frame_cfg;
    void draw_reveal_passes(const FrameInput& input, const FrameState& state, const OverlayConfig& frame_cfg);
    void build_esp_reveal(const FrameInput& input, const FrameState& state, const OverlayConfig& frame_cfg,
                          const FrameInput*& esp_input, FrameState*& esp_state);
    void assign_frozen_reveal_slots(std::vector<float>& batch_bones, int& current_slot);
    void update_hit_effects(const FrameInput& input, const FrameState& state, const OverlayConfig& frame_cfg,
                            struct ImDrawList* draw_list);

    // --- Grenade Creator Recording State ---
    bool rec_pin_pulled_last_ = false;
    double rec_pin_pull_time_ = 0.0;
    double rec_movement_start_time_ = 0.0;
    glm::vec3 rec_start_pos_{ 0.0f };
    glm::vec3 rec_start_angles_{ 0.0f };
    uint8_t rec_grenade_type_ = 0;
    bool rec_crouch_detected_ = false;
    bool rec_shift_detected_ = false;
    std::string rec_movement_key_ = "";
    int rec_throw_type_idx_ = 0;

    // --- Spread Circle State ---
    float spread_circle_smoothed_radius = 0.0f;
    float spread_circle_smoothed_base_radius = 0.0f;
};
