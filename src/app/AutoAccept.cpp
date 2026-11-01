#include "AutoAccept.hpp"
#include "logger.hpp"

#include <X11/Xlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <random>

// Auto Accept diagnostics follow the Debug Game Reader setting.  Keep the
// gating local to this translation unit so every existing log path is covered.
#undef BUNNYWARE_LOG_INFO
#undef BUNNYWARE_LOG_WARN
#undef BUNNYWARE_LOG_ERROR
#define BUNNYWARE_LOG_INFO(...)  do { if (debug_logging_) spdlog::info("[BUNNYWARE Chams] " __VA_ARGS__); } while (false)
#define BUNNYWARE_LOG_WARN(...)  do { if (debug_logging_) spdlog::warn("[BUNNYWARE Chams] " __VA_ARGS__); } while (false)
#define BUNNYWARE_LOG_ERROR(...) do { if (debug_logging_) spdlog::error("[BUNNYWARE Chams] " __VA_ARGS__); } while (false)

static double now_sec()
{
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

static const char* state_name(int state)
{
    switch (state) {
        case 0: return "NotReady";
        case 1: return "Pending";
        case 2: return "Accepted";
        case 3: return "DoneAccepted";
        default: return "Unknown";
    }
}

AutoAccept::AutoAccept() = default;

AutoAccept::~AutoAccept()
{
    if (uinput_ok_ && virtual_input_) {
        virtual_input_->clickr(1);
    }
}

glm::ivec2 AutoAccept::target_position(const OverlayConfig& cfg, int origin_x, int origin_y,
                                       int width, int height)
{
    const float x_percent = std::clamp(cfg.auto_accept_target_x_percent, 0.0f, 100.0f);
    const float y_percent = std::clamp(cfg.auto_accept_target_y_percent, 0.0f, 100.0f);
    return {
        origin_x + static_cast<int>(std::lround(static_cast<float>(width) * x_percent / 100.0f)),
        origin_y + static_cast<int>(std::lround(static_cast<float>(height) * y_percent / 100.0f)),
    };
}

bool AutoAccept::is_clicking() const
{
    return phase_ == Phase::ClickDown;
}

void AutoAccept::suspend_input()
{
    reset_pipeline();
    virtual_input_.reset();
    uinput_ok_ = false;
    was_enabled_ = false;
    logged_setup_ = false;
}

void AutoAccept::ensure_input()
{
    if (uinput_ok_ || !init_error_.empty()) return;
    try {
        virtual_input_ = std::make_unique<VirtualInput>();
        uinput_ok_ = true;
        BUNNYWARE_LOG_INFO("AutoAccept: VirtualInput ready");
    } catch (const std::exception& e) {
        init_error_ = e.what();
        uinput_ok_ = false;
        BUNNYWARE_LOG_ERROR("AutoAccept: VirtualInput failed: {}", init_error_);
    }
}

void AutoAccept::reset_pipeline()
{
    if (uinput_ok_ && virtual_input_ && phase_ == Phase::ClickDown) {
        virtual_input_->clickr(1);
    }
    phase_ = Phase::Idle;
    pending_since_ = 0.0;
    phase_since_ = 0.0;
    active_instance_ = 0;
    clicked_this_session_ = false;
    reset_movement_state();
}

void AutoAccept::reset_movement_state()
{
    // Clear all humanized-movement accumulators so a fresh session starts clean.
    virtual_cursor_ = glm::vec2(0.0f);
    cursor_seeded_ = false;
    last_resync_time_ = 0.0;
    approach_start_dist_ = 0.0f;
    subpixel_error_ = glm::vec2(0.0f);
    is_overshooting_ = false;
    current_overshoot_scale_ = 1.0f;
    overshoot_ticks_left_ = 0;
}

bool AutoAccept::resync_virtual_cursor()
{
    // Best-effort X11 read to (re)seed the dead-reckoned cursor. Returns true on
    // success. Failure is non-fatal: we keep the last known virtual position and
    // continue dead-reckoning from there.
    int x = 0, y = 0;
    if (!query_pointer(x, y)) {
        return false;
    }
    virtual_cursor_ = glm::vec2(static_cast<float>(x), static_cast<float>(y));
    cursor_seeded_ = true;
    last_resync_time_ = now_sec();
    return true;
}

void AutoAccept::inject_mouse_move(float dx, float dy)
{
    // Mirrors LegitAim::inject_mouse_move: guard on uinput then delegate to the
    // shared VirtualInput (relative move with its own internal sub-pixel
    // remainder accumulation). Advances the dead-reckoned cursor by the integer
    // counts we actually emitted so the next frame's delta is accurate.
    if (!uinput_ok_ || !virtual_input_) return;
    virtual_input_->move_rel(dx, dy);
    // move_rel rounds to whole counts internally; reflect the rounded step back
    // into the virtual cursor so subsequent frames measure the real remaining
    // distance rather than the fractional "want".
    virtual_cursor_.x += std::round(dx);
    virtual_cursor_.y += std::round(dy);
}

bool AutoAccept::is_local_in_game(const ShmPacket& packet)
{
    // Real competitive/casual maps, not empty lobby/menu
    if (packet.map_name[0] == '\0') return false;
    if (std::strcmp(packet.map_name, "<empty>") == 0) return false;

    // Local pawn alive on T/CT
    if (packet.local_health <= 0) return false;
    if (packet.local_team != 2 && packet.local_team != 3) return false;

    return true;
}

bool AutoAccept::query_pointer(int& out_x, int& out_y)
{
    static Display* display = nullptr;
    if (!display) {
        display = XOpenDisplay(nullptr);
        if (!display) return false;
    }

    Window root = DefaultRootWindow(display);
    Window root_ret, child_ret;
    int root_x = 0, root_y = 0, win_x = 0, win_y = 0;
    unsigned int mask = 0;
    if (!XQueryPointer(display, root, &root_ret, &child_ret, &root_x, &root_y, &win_x, &win_y, &mask)) {
        return false;
    }
    out_x = root_x;
    out_y = root_y;
    return true;
}

void AutoAccept::update(OverlayConfig& cfg, GameReader& reader, const FrameInput& input,
                        int window_x, int window_y, int window_width, int window_height)
{
    debug_logging_ = cfg.debug_game_reader;

    if (!cfg.auto_accept_enabled) {
        if (was_enabled_) {
            BUNNYWARE_LOG_INFO("AutoAccept: disabled");
            was_enabled_ = false;
            reset_pipeline();
            logged_setup_ = false;
        }
        last_instance_ = 0;
        last_state_ = -1;
        last_status_log_ = 0.0;
        return;
    }

    if (!was_enabled_) {
        BUNNYWARE_LOG_INFO("AutoAccept: enabled — watching match session");
        was_enabled_ = true;
        logged_setup_ = false;
        ensure_input();
    }

    // Auto-disable once we're actually in a match as the local player
    if (is_local_in_game(input.packet)) {
        BUNNYWARE_LOG_INFO(
            "AutoAccept: local player in-game (map={} team={} hp={}) — disabling module",
            input.packet.map_name, input.packet.local_team, input.packet.local_health);
        reset_pipeline();
        cfg.auto_accept_enabled = false;
        was_enabled_ = false;
        logged_setup_ = false;
        last_instance_ = 0;
        last_state_ = -1;
        return;
    }

    if (!reader.is_attached()) {
        const double t = now_sec();
        if (t - last_status_log_ >= 5.0) {
            BUNNYWARE_LOG_WARN("AutoAccept: game reader not attached to cs2 yet");
            last_status_log_ = t;
        }
        return;
    }

    const uintptr_t client = reader.get_client_base();
    const uintptr_t session_rva = reader.get_dw_match_session_pointer();
    const uint32_t state_off = reader.get_dw_match_session_state_offset();

    if (client == 0 || session_rva == 0) {
        const double t = now_sec();
        if (t - last_status_log_ >= 5.0) {
            BUNNYWARE_LOG_WARN(
                "AutoAccept: missing bases (client={:#x} session_rva={:#x})",
                client, session_rva);
            last_status_log_ = t;
        }
        return;
    }

    if (!logged_setup_) {
        BUNNYWARE_LOG_INFO(
            "AutoAccept: setup client={:#x} dwMatchSessionPointer={:#x} state_off={:#x} "
            "delay={:.0f}ms smooth={:.1f} uinput={}",
            client, session_rva, state_off,
            cfg.auto_accept_delay_ms, cfg.auto_accept_smooth,
            uinput_ok_ ? "ok" : "fail");
        logged_setup_ = true;
    }

    uintptr_t instance = 0;
    if (!reader.try_read(client + session_rva, &instance, sizeof(instance))) {
        return;
    }

    int state = AcceptState::NotReady;
    if (instance != 0) {
        if (!reader.try_read(instance + state_off, &state, sizeof(state))) {
            return;
        }
    }

    const double t = now_sec();
    const bool instance_changed = (instance != last_instance_);
    const bool state_changed = (state != last_state_);

    if (instance_changed || (instance != 0 && state_changed && last_state_ != -1)) {
        BUNNYWARE_LOG_INFO(
            "AutoAccept: session update instance={:#x} state={} ({})",
            instance, state, state_name(state));
    }

    // New pending session → arm delay pipeline.
    const bool is_pending = (instance != 0 && state == AcceptState::Pending);
    if (is_pending && !clicked_this_session_) {
        if (phase_ == Phase::Idle || active_instance_ != instance) {
            active_instance_ = instance;
            pending_since_ = t;
            phase_ = Phase::WaitDelay;
            phase_since_ = t;
            BUNNYWARE_LOG_INFO(
                "AutoAccept: MATCH FOUND (Pending) — instance={:#x}, waiting {:.0f}ms then smooth-click",
                instance, cfg.auto_accept_delay_ms);
        }
    } else if (!is_pending && phase_ != Phase::Idle && phase_ != Phase::Done) {
        // Left pending before we finished (timeout / manual accept / abandon)
        if (state == AcceptState::Accepted || state == AcceptState::DoneAccepted) {
            BUNNYWARE_LOG_INFO("AutoAccept: session already accepted/done (state={})", state_name(state));
            phase_ = Phase::Done;
            clicked_this_session_ = true;
        } else if (instance == 0) {
            BUNNYWARE_LOG_INFO("AutoAccept: session cleared mid-pipeline — resetting");
            reset_pipeline();
        }
    }

    if (instance == 0 && last_instance_ != 0 && phase_ == Phase::Idle) {
        BUNNYWARE_LOG_INFO("AutoAccept: session cleared (was instance={:#x})", last_instance_);
    }

    // --- Click pipeline ---
    if (uinput_ok_ && virtual_input_ && window_width > 0 && window_height > 0) {
        // The accept button sits below centre in the current CS2 match-found
        // dialog.  Target coordinates are in root-screen space, just like
        // XQueryPointer's result, so multi-monitor offsets are preserved.
        const glm::ivec2 target = target_position(cfg, window_x, window_y, window_width, window_height);
        const int target_x = target.x;
        const int target_y = target.y;

        if (phase_ == Phase::WaitDelay) {
            const double delay_s = std::max(0.0, static_cast<double>(cfg.auto_accept_delay_ms) / 1000.0);
            if (t - pending_since_ >= delay_s) {
                phase_ = Phase::Moving;
                phase_since_ = t;
                BUNNYWARE_LOG_INFO("AutoAccept: delay done — moving to accept button ({}, {})",
                                 target_x, target_y);
            }
        } else if (phase_ == Phase::Moving) {
            // Seed dead-reckoned cursor from X11 once, then refresh ~4Hz to
            // correct drift (user is AFK during matchmaking).
            if (!cursor_seeded_) {
                if (!resync_virtual_cursor()) {
                    virtual_cursor_ = glm::vec2(static_cast<float>(target_x),
                                                static_cast<float>(target_y));
                    cursor_seeded_ = true;
                    last_resync_time_ = t;
                    BUNNYWARE_LOG_WARN("AutoAccept: X11 pointer unavailable — seeding cursor at target");
                }
            } else if (t - last_resync_time_ >= 0.25) {
                (void)resync_virtual_cursor();
            }

            const float dx = static_cast<float>(target_x) - virtual_cursor_.x;
            const float dy = static_cast<float>(target_y) - virtual_cursor_.y;
            const float dist = std::sqrt(dx * dx + dy * dy);

            if (approach_start_dist_ <= 0.0f && dist > 0.0f)
                approach_start_dist_ = dist;

            // Watchdog: abort rather than click blind if we stall too long.
            if (t - phase_since_ > 5.0) {
                BUNNYWARE_LOG_WARN("AutoAccept: movement watchdog (cursor=({:.0f},{:.0f}) "
                                 "target=({},{}) dist={:.1f}) — aborting session",
                                 virtual_cursor_.x, virtual_cursor_.y, target_x, target_y, dist);
                reset_pipeline();
                return;
            }

            if (std::fabs(dx) <= 15.0f && std::fabs(dy) <= 10.0f) {
                phase_ = Phase::ClickDown;
                phase_since_ = t;
                virtual_input_->clickp(1);
                BUNNYWARE_LOG_INFO(
                    "AutoAccept: on button (cursor=({:.0f},{:.0f}) target=({},{}) dist={:.1f}) — click down",
                    virtual_cursor_.x, virtual_cursor_.y, target_x, target_y, dist);
            } else {
                // --- Humanized movement (mirrors LegitAim aim methods) ---
                float base_smooth = cfg.auto_accept_smooth;
                if (base_smooth < 1.0f) base_smooth = 1.0f;

                // 1. Progressive smooth: ease in near the target.
                float smooth = base_smooth;
                if (cfg.auto_accept_progressive_smooth && approach_start_dist_ > 1.0f) {
                    const float prog = std::clamp(dist / approach_start_dist_, 0.0f, 1.0f);
                    smooth = base_smooth * (0.4f + 0.6f * prog);
                    if (smooth < 1.0f) smooth = 1.0f;
                }

                // 2. Random smooth jitter (LegitAim style).
                if (cfg.auto_accept_random_smooth) {
                    static std::mt19937 rng(std::random_device{}());
                    float rx = std::clamp(cfg.auto_accept_randomize_smooth_x, 0.0f, smooth - 1.0f);
                    float ry = std::clamp(cfg.auto_accept_randomize_smooth_y, 0.0f, smooth - 1.0f);
                    if (rx > 0.0f) { std::uniform_real_distribution<float> d(-rx, rx); smooth += d(rng); }
                    if (ry > 0.0f) { std::uniform_real_distribution<float> d(-ry, ry); smooth += d(rng); }
                    if (smooth < 1.0f) smooth = 1.0f;
                }

                float step_x = dx / smooth;
                float step_y = dy / smooth;

                const float max_step = 40.0f;
                step_x = std::clamp(step_x, -max_step, max_step);
                step_y = std::clamp(step_y, -max_step, max_step);

                if (std::fabs(step_x) < 1.0f && std::fabs(dx) > 1.0f)
                    step_x = (dx > 0.0f) ? 1.0f : -1.0f;
                if (std::fabs(step_y) < 1.0f && std::fabs(dy) > 1.0f)
                    step_y = (dy > 0.0f) ? 1.0f : -1.0f;

                // 3. Muscle overshoot (Itami style, LegitAim pattern).
                if (cfg.auto_accept_overshoot) {
                    if (!is_overshooting_ && dist > (approach_start_dist_ * 0.3f)) {
                        static std::mt19937 rng(std::random_device{}());
                        std::uniform_real_distribution<float> dp(0.0f, 1.0f);
                        if (dp(rng) < cfg.auto_accept_overshoot_chance) {
                            is_overshooting_ = true;
                            current_overshoot_scale_ = cfg.auto_accept_overshoot_scale;
                            std::uniform_int_distribution<int> dt(5, 12);
                            overshoot_ticks_left_ = dt(rng);
                        }
                    }
                    if (is_overshooting_) {
                        if (overshoot_ticks_left_ > 0) {
                            step_x *= current_overshoot_scale_;
                            step_y *= current_overshoot_scale_;
                            --overshoot_ticks_left_;
                        } else {
                            is_overshooting_ = false;
                        }
                    }
                }

                // 4. Sub-pixel accumulator (anti-jitter, LegitAim pattern).
                float want_x = step_x + subpixel_error_.x;
                float want_y = step_y + subpixel_error_.y;
                float counts_x = std::round(want_x);
                float counts_y = std::round(want_y);
                subpixel_error_.x = want_x - counts_x;
                subpixel_error_.y = want_y - counts_y;

                if (std::fabs(counts_x) > 0.0f || std::fabs(counts_y) > 0.0f) {
                    inject_mouse_move(counts_x, counts_y);
                }
            }
        } else if (phase_ == Phase::ClickDown) {
            // Hold briefly then release (4–20ms style + a bit more for reliability)
            if (t - phase_since_ >= 0.020) {
                virtual_input_->clickr(1);
                phase_ = Phase::ClickHold;
                phase_since_ = t;
                clicked_this_session_ = true;
                BUNNYWARE_LOG_INFO("AutoAccept: click up — accept sent");
            }
        } else if (phase_ == Phase::ClickHold) {
            // Brief settle, then done for this session
            if (t - phase_since_ >= 0.050) {
                phase_ = Phase::Done;
                phase_since_ = t;
            }
        }
    } else if (phase_ == Phase::WaitDelay || phase_ == Phase::Moving) {
        const double tt = now_sec();
        if (tt - last_status_log_ >= 2.0) {
            BUNNYWARE_LOG_WARN(
                "AutoAccept: cannot click (uinput={} size={}x{})",
                uinput_ok_ ? "ok" : "fail", window_width, window_height);
            last_status_log_ = tt;
        }
    }

    if (t - last_status_log_ >= 10.0) {
        BUNNYWARE_LOG_INFO(
            "AutoAccept: polling… instance={:#x} state={} ({}) phase={}",
            instance, state, state_name(state), static_cast<int>(phase_));
        last_status_log_ = t;
    }

    last_instance_ = instance;
    last_state_ = instance ? state : AcceptState::NotReady;
}
