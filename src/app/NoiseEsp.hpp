#pragma once

#include "config.hpp"
#include "overlay/game_reader.hpp"
#include "renderer/esp_renderer.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// reNoiseESP-style multi-type noise markers (ground circles + optional plant/defuse text).
// Detection runs at ~64 Hz; rendering every frame for smooth expand/fade.
class NoiseEsp {
public:
    enum class Type : uint8_t {
        Movement = 0,
        Shooting,
        Reload,
        Landing,
        Scope,
        Plant,
        Defuse,
        Count
    };

    void update(const ShmPacket& packet, const OverlayConfig& cfg, double now_sec);
    void render(EspRenderer& esp, const OverlayConfig& cfg, const float* gl_vp,
                const float* view_matrix, int screen_w, int screen_h);

    struct LabelDraw {
        float x = 0.0f;
        float y = 0.0f;
        float alpha = 1.0f;
        char text[16]{};
    };
    const std::vector<LabelDraw>& pending_labels() const;
    void clear_pending_labels();

private:
    struct PlayerState {
        float last_emit = 0.0f;
        bool emit_valid = false;
        int last_shots = 0;
        bool was_scoped = false;
        bool was_reloading = false;
        bool was_on_ground = true;
        bool has_state = false;
    };

    struct Event {
        Type type = Type::Movement;
        Vec3 pos{};
        double born = 0.0;
        float duration = 3.0f;
        char label[16]{};
        bool show_label = false;
    };

    static constexpr float kRangeMovement = 1100.0f;
    static constexpr float kRangeShooting = 3000.0f;
    static constexpr float kRangeShootingSilenced = 800.0f;
    static constexpr float kRangeReload = 1100.0f;
    static constexpr float kRangeLanding = 1100.0f;
    static constexpr float kRangeScope = 600.0f;
    static constexpr float kRangePlant = 1500.0f;
    static constexpr float kRangeDefuse = 1500.0f;
    static constexpr float kMinMovementSpeedSq = 12544.0f; // ~112 u/s
    static constexpr float kMinLandingVzSq = 40000.0f;
    static constexpr uint32_t kFlOnGround = 1u;

    void detect(const ShmPacket& packet, const OverlayConfig& cfg, double now);
    void spawn(Type type, const Vec3& pos, const OverlayConfig& cfg, double now, const char* label = nullptr);
    bool type_enabled(Type type, const OverlayConfig& cfg) const;
    const Color& type_color(Type type, const OverlayConfig& cfg) const;
    float type_range(Type type, const char* weapon_name) const;
    static float dist_sq(const Vec3& a, const Vec3& b);

    std::array<PlayerState, shm::MAX_PLAYERS> player_states_{};
    std::vector<Event> events_;
    std::vector<LabelDraw> pending_labels_;
    double last_detect_time_ = 0.0;
    double last_now_ = 0.0;
    bool was_defusing_ = false;
    bool was_arming_ = false;
};
