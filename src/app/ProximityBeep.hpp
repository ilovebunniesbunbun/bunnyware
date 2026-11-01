#pragma once

#include "config.hpp"
#include "math/Distance.hpp"
#include "overlay/game_reader.hpp"

#include <algorithm>
#include <cmath>
#include <optional>

namespace proximity_beep {

struct Plan {
    float interval_sec = 0.0f;
    float volume = 0.0f;
    int pitch_step = 0;
};

inline constexpr float kQuietestFraction = 0.25f;
inline constexpr float kFastestIntervalFloor = 0.15f;
inline constexpr float kMinRangeMeters = 1.0f;
inline constexpr float kMaxRangeMeters = 60.0f;
inline constexpr float kFullIntensityMeters = 3.0f;
inline constexpr float kFalloffCurve = 1.3f;
inline constexpr float kMinFovDegrees = 10.0f;
inline constexpr float kMaxFovDegrees = 180.0f;
inline constexpr int kPitchSteps = 16;
inline constexpr float kPitchFarHz = 500.0f;
inline constexpr float kPitchNearHz = 1100.0f;

/**
 * @name pitch_step_hz
 * @description Pitch in Hz of the pre-rendered beep sample for a pitch step.
 * Steps run from the far pitch (step 0) to the near pitch (last step) on a
 * logarithmic scale, matching the files in assets/sounds.
 *
 * @example
 * const float hz = proximity_beep::pitch_step_hz(plan.pitch_step);
 */
inline float pitch_step_hz(int step)
{
    const float amount = static_cast<float>(std::clamp(step, 0, kPitchSteps - 1)) / static_cast<float>(kPitchSteps - 1);
    return kPitchFarHz * std::pow(kPitchNearHz / kPitchFarHz, amount);
}

/**
 * @name inside_view_cone
 * @description True when `target` lies within half of `fov_degrees` either
 * side of the horizontal direction the viewer faces. Height is ignored. The
 * cone is clamped to the supported 10 to 180 degrees, so a surround (360)
 * setting can never be reached. A target standing on the viewer counts as
 * inside.
 *
 * @example
 * if (proximity_beep::inside_view_cone(packet.local_origin, packet.local_angles.y, enemy.origin, 110.0f)) {
 *     beep_for(enemy);
 * }
 */
inline bool inside_view_cone(const glm::vec3& viewer, float yaw_degrees, const glm::vec3& target, float fov_degrees)
{
    const float dx = target.x - viewer.x;
    const float dy = target.y - viewer.y;
    const float length = std::sqrt(dx * dx + dy * dy);
    if (length < 1e-3f) return true;

    const float yaw = glm::radians(yaw_degrees);
    const float facing = (dx * std::cos(yaw) + dy * std::sin(yaw)) / length;
    const float half_fov = glm::radians(std::clamp(fov_degrees, kMinFovDegrees, kMaxFovDegrees)) * 0.5f;
    return facing >= std::cos(half_fov);
}

/**
 * @name nearest_enemy_in_cone_meters
 * @description Distance in meters from the local player to the closest living
 * enemy inside the beep cone, or nothing when no enemy qualifies. Enemies
 * behind or beside the player are never considered. Teammates are skipped
 * when team_check is on.
 *
 * @example
 * if (const auto meters = proximity_beep::nearest_enemy_in_cone_meters(packet, cfg)) {
 *     use(*meters);
 * }
 */
inline std::optional<float> nearest_enemy_in_cone_meters(const ShmPacket& packet, const OverlayConfig& cfg)
{
    std::optional<float> nearest;
    const int count = std::min(packet.player_count, static_cast<int>(shm::MAX_PLAYERS));
    for (int i = 0; i < count; ++i) {
        const PlayerData& player = packet.players[i];
        if (!player.active || player.health <= 0) continue;
        if (cfg.team_check && player.team == packet.local_team) continue;
        if (!inside_view_cone(packet.local_origin, packet.local_angles.y, player.origin, cfg.proximity_beep_fov)) continue;
        const float meters = distance::meters_between(player.origin, packet.local_origin);
        if (!nearest || meters < *nearest) nearest = meters;
    }
    return nearest;
}

/**
 * @name make_plan
 * @description Maps an enemy distance to a beep interval, volume and pitch
 * step. At the configured max range the beep is slowest, quietest and lowest;
 * from 3 m inward it is fastest, loudest and highest. The ramp between them
 * follows a 1.3 power curve, so the beep stays calm until the enemy is close.
 * Returns nothing beyond max range.
 *
 * @example
 * if (const auto plan = proximity_beep::make_plan(12.0f, cfg)) {
 *     schedule(plan->interval_sec, plan->volume, plan->pitch_step);
 * }
 */
inline std::optional<Plan> make_plan(float meters, const OverlayConfig& cfg)
{
    const float max_range = std::max(cfg.proximity_beep_max_range, kMinRangeMeters);
    if (meters < 0.0f || meters > max_range) return std::nullopt;

    const float full_intensity = std::min(kFullIntensityMeters, max_range * 0.5f);
    const float closeness = std::clamp((max_range - meters) / (max_range - full_intensity), 0.0f, 1.0f);
    const float strength = std::pow(closeness, kFalloffCurve);
    const float fast = std::max(cfg.proximity_beep_fast_interval, kFastestIntervalFloor);
    const float slow = std::max(cfg.proximity_beep_slow_interval, fast);
    const float master = std::clamp(cfg.proximity_beep_volume, 0.0f, 1.0f);

    Plan plan;
    plan.interval_sec = slow + (fast - slow) * strength;
    plan.volume = master * (kQuietestFraction + (1.0f - kQuietestFraction) * strength);
    plan.pitch_step = static_cast<int>(std::lround(strength * static_cast<float>(kPitchSteps - 1)));
    return plan;
}

}

class ProximityBeep {
public:
    /**
     * @name update
     * @description Returns the beep to play when one is due this frame, or
     * nothing. The first beep fires as soon as an enemy enters the cone and
     * range.
     *
     * @example
     * if (const auto beep = proximity_beep.update(packet, cfg, now)) {
     *     player.play(beep->volume, beep->pitch_step);
     * }
     */
    std::optional<proximity_beep::Plan> update(const ShmPacket& packet, const OverlayConfig& cfg, double now_sec)
    {
        const std::optional<proximity_beep::Plan> plan = current_plan(packet, cfg);
        if (!plan) {
            armed_ = false;
            return std::nullopt;
        }
        if (armed_ && now_sec - last_beep_sec_ < plan->interval_sec) return std::nullopt;

        armed_ = true;
        last_beep_sec_ = now_sec;
        return plan;
    }

private:
    static std::optional<proximity_beep::Plan> current_plan(const ShmPacket& packet, const OverlayConfig& cfg)
    {
        if (!cfg.proximity_beep_enabled || packet.local_health <= 0) return std::nullopt;
        const std::optional<float> meters = proximity_beep::nearest_enemy_in_cone_meters(packet, cfg);
        if (!meters) return std::nullopt;
        return proximity_beep::make_plan(*meters, cfg);
    }

    double last_beep_sec_ = 0.0;
    bool armed_ = false;
};
