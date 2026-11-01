#pragma once
#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>

namespace distance {

inline constexpr float UNITS_PER_METER = 39.37f;
inline constexpr float MISS = -1.0f;

/**
 * @name units_to_meters
 * @description Converts Source 2 world units to meters (1 m = 39.37 units).
 *
 * @example
 * float meters = distance::units_to_meters(393.7f); // 10.0f
 */
inline float units_to_meters(float units)
{
    return units / UNITS_PER_METER;
}

/**
 * @name meters_between
 * @description Straight-line distance between two world positions, in meters.
 *
 * @example
 * float meters = distance::meters_between(player.origin, packet.local_eye);
 */
inline float meters_between(const glm::vec3& a, const glm::vec3& b)
{
    return units_to_meters(glm::distance(a, b));
}

/**
 * @name view_forward
 * @description Unit forward vector for Source view angles given in degrees
 * (x = pitch, y = yaw).
 *
 * @example
 * glm::vec3 fwd = distance::view_forward(packet.local_angles);
 */
inline glm::vec3 view_forward(const glm::vec3& angles)
{
    const float pitch = glm::radians(angles.x);
    const float yaw = glm::radians(angles.y);
    return {std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), -std::sin(pitch)};
}

/**
 * @name ray_capsule_distance
 * @description Distance along a ray to the point where it passes closest to a
 * capsule axis, or `distance::MISS` when the ray stays outside `radius`.
 * `dir` must be normalized.
 *
 * @example
 * float t = distance::ray_capsule_distance(eye, fwd, feet, feet + glm::vec3(0, 0, 72), 16.0f);
 * if (t != distance::MISS) draw_label(distance::units_to_meters(t));
 */
inline float ray_capsule_distance(const glm::vec3& origin, const glm::vec3& dir, const glm::vec3& seg_a,
                                  const glm::vec3& seg_b, float radius)
{
    const glm::vec3 axis = seg_b - seg_a;
    const glm::vec3 w = origin - seg_a;
    const float axis_len2 = glm::dot(axis, axis);
    const float d_dot_axis = glm::dot(dir, axis);
    const float d_dot_w = glm::dot(dir, w);
    const float axis_dot_w = glm::dot(axis, w);
    const float denom = axis_len2 - d_dot_axis * d_dot_axis;

    float seg_t = denom > 1e-6f ? (axis_dot_w - d_dot_axis * d_dot_w) / denom : 0.0f;
    seg_t = std::clamp(seg_t, 0.0f, 1.0f);
    const float ray_t = std::max(0.0f, glm::dot(seg_a + axis * seg_t - origin, dir));
    if (axis_len2 > 1e-6f) {
        seg_t = std::clamp(glm::dot(origin + dir * ray_t - seg_a, axis) / axis_len2, 0.0f, 1.0f);
    }

    const glm::vec3 closest_on_ray = origin + dir * ray_t;
    const glm::vec3 closest_on_axis = seg_a + axis * seg_t;
    if (glm::distance(closest_on_ray, closest_on_axis) > radius) return MISS;
    return ray_t;
}

}
