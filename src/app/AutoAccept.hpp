#pragma once

#include "config.hpp"
#include "overlay/game_reader.hpp"
#include "overlay/evdev_input.hpp"
#include "FrameInput.hpp"

#include <glm/glm.hpp>
#include <memory>
#include <string>

// Detects CS2 matchmaking accept via match session pointer (SetPlayerReady global).
// State at +0x90: 0 NotReady, 1 Pending (match found), 2 Accepted, 3 DoneAccepted.
// On Pending: wait delay, smooth-move to accept button, left click.
// Auto-disables when local player is detected in-game.
class AutoAccept {
public:
    AutoAccept();
    ~AutoAccept();

    void update(OverlayConfig& cfg, GameReader& reader, const FrameInput& input,
                int window_x, int window_y, int window_width, int window_height);

    // Returns the centre of CS2's accept-button region in the supplied screen
    // rectangle.  Keep input and overlay diagnostics on this same calculation.
    static glm::ivec2 target_position(const OverlayConfig& cfg, int origin_x, int origin_y,
                                      int width, int height);

    // True only while the synthetic left button is held down.
    bool is_clicking() const;
    void suspend_input();

private:
    enum AcceptState : int {
        NotReady = 0,
        Pending = 1,
        Accepted = 2,
        DoneAccepted = 3,
    };

    enum class Phase {
        Idle,
        WaitDelay,
        Moving,
        ClickDown,
        ClickHold,
        Done,
    };

    static bool is_local_in_game(const ShmPacket& packet);
    static bool query_pointer(int& out_x, int& out_y);
    void reset_pipeline();
    void reset_movement_state();
    void ensure_input();
    bool resync_virtual_cursor();
    void inject_mouse_move(float dx, float dy);

    std::unique_ptr<EvdevInput> evdev_input_;
    bool input_ok_ = false;
    std::string init_error_;

    uintptr_t last_instance_ = 0;
    int last_state_ = -1;
    bool was_enabled_ = false;
    bool logged_setup_ = false;
    double last_status_log_ = 0.0;
    bool debug_logging_ = false;

    Phase phase_ = Phase::Idle;
    double pending_since_ = 0.0;
    double phase_since_ = 0.0;
    uintptr_t active_instance_ = 0;
    bool clicked_this_session_ = false;

    // --- Humanized movement state (mirrors LegitAim aim methods) ---
    // Dead-reckoned OS cursor in screen space (pixels). Seeded from X11 once at
    // Moving entry, refreshed periodically to correct drift (user is AFK during
    // matchmaking, so external cursor motion is not expected).
    glm::vec2 virtual_cursor_{0.0f};
    bool cursor_seeded_ = false;
    double last_resync_time_ = 0.0;
    float approach_start_dist_ = 0.0f;

    // Sub-pixel accumulator: fractional pixel deltas that didn't round into a
    // whole mouse count this frame are carried into the next (anti-jitter).
    glm::vec2 subpixel_error_{0.0f};

    // Muscle overshoot simulation (Itami style, same as LegitAim).
    bool is_overshooting_ = false;
    float current_overshoot_scale_ = 1.0f;
    int overshoot_ticks_left_ = 0;
};
