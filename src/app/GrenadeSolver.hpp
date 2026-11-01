#pragma once
#include "overlay/trajectory_sim.hpp"
#include "LandingZoneManager.hpp"
#include "overlay/bvh_parser.hpp"
#include <vector>
#include <glm/glm.hpp>
#include <functional>

enum class ThrowType : uint8_t {
    NORMAL = 0,
    JUMP = 1,
    W_JUMP = 2
};

struct SolverSolution {
    bool found = false;
    float display_pitch;              // The pitch the player must aim at (pre-correction)
    float yaw;                        // The yaw toward the target
    ThrowType throw_type;
    TrajectoryResult trajectory;      // Predicted path for preview rendering
    float landing_error;              // Distance from AABB center to actual landing
    int zone_index;                   // Which landing zone this solves
};

using SolverProgressCallback = std::function<void(float pitch, float yaw, ThrowType type, const std::vector<glm::vec3>& points)>;

class GrenadeSolver {
public:
    GrenadeSolver();

    // Solve for all reachable zones from current position
    std::vector<SolverSolution> solve_all(
        const glm::vec3& player_eye,
        uint8_t grenade_type,
        const std::vector<LandingZone>& zones,
        const LocalMapBVH& bvh,
        const std::vector<LocalMapBVH::Triangle>& door_tris,
        float max_range = 1500.0f,
        SolverProgressCallback progress_cb = nullptr
    );

private:
    std::vector<float> calculate_analytical_pitches(
        const glm::vec3& origin, 
        const glm::vec3& target, 
        float speed, 
        float gravity
    );

    bool try_throw(
        const glm::vec3& eye,
        float display_pitch,
        float yaw_rad,
        ThrowType throw_type,
        uint8_t grenade_type,
        const LocalMapBVH& bvh,
        const std::vector<LocalMapBVH::Triangle>& door_tris,
        const LandingZone& zone,
        TrajectoryResult& out_trajectory
    );

    static bool point_in_aabb(const glm::vec3& p, const glm::vec3& mins, const glm::vec3& maxs);
    static glm::vec3 pitch_yaw_to_dir(float pitch, float yaw_rad);
};
