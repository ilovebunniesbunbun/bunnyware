#include "GrenadeSolver.hpp"
#include <cmath>
#include <algorithm>
#include <iostream>

GrenadeSolver::GrenadeSolver() {}

bool GrenadeSolver::point_in_aabb(const glm::vec3& p, const glm::vec3& mins, const glm::vec3& maxs) {
    constexpr float z_tolerance_bottom = -15.0f;
    constexpr float z_tolerance_top = 10.0f;
    return p.x >= mins.x && p.x <= maxs.x &&
           p.y >= mins.y && p.y <= maxs.y &&
           p.z >= (mins.z + z_tolerance_bottom) && p.z <= (maxs.z + z_tolerance_top);
}

glm::vec3 GrenadeSolver::pitch_yaw_to_dir(float pitch, float yaw_rad) {
    float p_rad = pitch * 3.14159265f / 180.0f;
    return glm::vec3(
        std::cos(p_rad) * std::cos(yaw_rad),
        std::cos(p_rad) * std::sin(yaw_rad),
        -std::sin(p_rad)
    );
}

std::vector<float> GrenadeSolver::calculate_analytical_pitches(const glm::vec3& origin, const glm::vec3& target, float speed, float gravity) {
    std::vector<float> pitches;
    
    // Convert to 2D plane towards target
    glm::vec2 origin2d(0.0f, origin.z);
    float dx = target.x - origin.x;
    float dy = target.y - origin.y;
    float dist2d = std::sqrt(dx*dx + dy*dy);
    glm::vec2 target2d(dist2d, target.z);
    
    float x = target2d.x - origin2d.x;
    float y = target2d.y - origin2d.y;
    
    float v = speed;
    float g = gravity;
    
    // Parabola equation to hit target (x,y) with speed v and gravity g
    // v^4 - g * (g*x^2 + 2*y*v^2)
    float root = v*v*v*v - g * (g*x*x + 2.0f*y*v*v);
    
    if (root >= 0.0f) {
        float s = std::sqrt(root);
        float t1 = std::atan((v*v + s) / (g*x));
        float t2 = std::atan((v*v - s) / (g*x));
        
        // Convert rad to deg, and remember Source uses negative pitch for up
        pitches.push_back(-t1 * 180.0f / 3.14159265f);
        if (std::abs(t1 - t2) > 0.01f) {
            pitches.push_back(-t2 * 180.0f / 3.14159265f);
        }
    }
    
    return pitches;
}

bool GrenadeSolver::try_throw(
    const glm::vec3& eye,
    float display_pitch,
    float yaw_rad,
    ThrowType throw_type,
    uint8_t grenade_type,
    const LocalMapBVH& bvh,
    const std::vector<LocalMapBVH::Triangle>& door_tris,
    const LandingZone& zone,
    TrajectoryResult& out_trajectory
) {
    float effective_pitch = display_pitch - (90.0f - std::abs(display_pitch)) * 10.0f / 90.0f;
    glm::vec3 forward = pitch_yaw_to_dir(effective_pitch, yaw_rad);
    float strength = 1.0f; // Hardcoded full strength throw for now
    float clamped = std::max(15.0f, std::min(750.0f, 750.0f * 0.9f));
    float speed = (strength * 0.7f + 0.3f) * clamped; // 675.0f
    
    glm::vec3 origin = {
        eye.x + forward.x * 16.0f,
        eye.y + forward.y * 16.0f,
        eye.z + forward.z * 16.0f + strength * 12.0f - 12.0f
    };
    
    // Quick trace to move origin out of walls (same as game_reader.cpp / worker)
    glm::vec3 trace_end = origin + forward * 22.0f;
    TraceResult tr = bvh.trace_ray(origin, trace_end);
    if (tr.hit) {
        origin = tr.end_pos - forward * 6.0f;
    } else {
        origin = origin + forward * 16.0f;
    }
    
    glm::vec3 velocity;
    if (throw_type == ThrowType::JUMP) {
        velocity = {
            forward.x * speed,
            forward.y * speed,
            forward.z * speed + 273.5f
        };
    } else if (throw_type == ThrowType::W_JUMP) {
        // W_JUMP represents standard W-tapped jump throw (30.0f units/sec horizontal velocity, scaled by 1.25x inheritance)
        velocity = {
            forward.x * speed + std::cos(yaw_rad) * 30.0f * 1.25f,
            forward.y * speed + std::sin(yaw_rad) * 30.0f * 1.25f,
            forward.z * speed + 273.5f
        };
    } else {
        velocity = forward * speed;
    }
    
    out_trajectory = simulate_trajectory(origin, velocity, grenade_type, bvh, door_tris);
    
    if (out_trajectory.valid && out_trajectory.points.size() > 0) {
        glm::vec3 end_pos = out_trajectory.points.back();
        if (point_in_aabb(end_pos, zone.mins, zone.maxs)) {
            return true;
        }
    }
    
    return false;
}

std::vector<SolverSolution> GrenadeSolver::solve_all(
    const glm::vec3& player_eye,
    uint8_t grenade_type,
    const std::vector<LandingZone>& zones,
    const LocalMapBVH& bvh,
    const std::vector<LocalMapBVH::Triangle>& door_tris,
    float max_range,
    SolverProgressCallback progress_cb
) {
    std::vector<SolverSolution> solutions;
    
    for (int i = 0; i < (int)zones.size(); ++i) {
        const auto& zone = zones[i];
        
        if (zone.grenade_type != 0 && zone.grenade_type != grenade_type) {
            continue;
        }
        
        glm::vec3 zone_center = (zone.mins + zone.maxs) * 0.5f;
        float dist = glm::distance(player_eye, zone_center);
        if (dist > max_range) {
            continue;
        }
        
        float base_yaw = std::atan2(zone_center.y - player_eye.y, zone_center.x - player_eye.x);
        // Yaw offsets to try (in radians): 0, +/- 0.6 deg, +/- 1.2 deg, +/- 1.8 deg, +/- 2.4 deg, +/- 3.0 deg
        float yaw_offsets[] = { 0.0f, -0.01f, 0.01f, -0.02f, 0.02f, -0.03f, 0.03f, -0.04f, 0.04f, -0.05f, 0.05f };
        
        // sv_gravity is 800, scale is 0.4 in trajectory_sim
        float sim_gravity = 800.0f * 0.4f;
        
        auto try_throw_with_progress = [&](float pitch, float yaw, ThrowType t, TrajectoryResult& out_traj) -> bool {
            bool res = try_throw(player_eye, pitch, yaw, t, grenade_type, bvh, door_tris, zone, out_traj);
            if (progress_cb) {
                progress_cb(pitch, yaw, t, out_traj.points);
            }
            return res;
        };

        ThrowType types[] = {ThrowType::NORMAL, ThrowType::JUMP, ThrowType::W_JUMP};
        for (ThrowType t_type : types) {
            bool hit = false;
            float best_pitch = 0.0f;
            float best_yaw = base_yaw;
            TrajectoryResult best_traj;
            
            // 1. Try analytical pitches across all yaw offsets first
            for (float yaw_off : yaw_offsets) {
                float test_yaw = base_yaw + yaw_off;
                auto analytical_pitches = calculate_analytical_pitches(player_eye, zone_center, 675.0f, sim_gravity);
                for (float bp : analytical_pitches) {
                    for (float p = bp - 3.0f; p <= bp + 3.0f; p += 0.5f) {
                        float test_pitch = std::max(-89.0f, std::min(89.0f, p));
                        TrajectoryResult traj;
                        if (try_throw_with_progress(test_pitch, test_yaw, t_type, traj)) {
                            hit = true;
                            best_pitch = test_pitch;
                            best_yaw = test_yaw;
                            best_traj = traj;
                            break;
                        }
                    }
                    if (hit) break;
                }
                if (hit) break;
            }
            
            // 2. If analytical check failed, run full pitch sweep across yaw offsets (bottom-up: horizontal to vertical sky)
            if (!hit) {
                for (float yaw_off : yaw_offsets) {
                    float test_yaw = base_yaw + yaw_off;
                    for (float p = 0.0f; p >= -89.0f; p -= 1.5f) {
                        TrajectoryResult traj;
                        if (try_throw_with_progress(p, test_yaw, t_type, traj)) {
                            hit = true;
                            best_pitch = p;
                            best_yaw = test_yaw;
                            best_traj = traj;
                            break;
                        }
                    }
                    if (hit) break;
                }
            }
            
            if (hit) {
                // Refine the pitch to find the center
                float ref_pitch = best_pitch;
                TrajectoryResult ref_traj = best_traj;
                for (float p = best_pitch - 1.0f; p <= best_pitch + 1.0f; p += 0.1f) {
                    float test_pitch = std::max(-89.0f, std::min(89.0f, p));
                    TrajectoryResult traj;
                    if (try_throw_with_progress(test_pitch, best_yaw, t_type, traj)) {
                        // Compare distances to center
                        glm::vec3 c = (zone.mins + zone.maxs) * 0.5f;
                        if (glm::distance(traj.points.back(), c) < glm::distance(ref_traj.points.back(), c)) {
                            ref_pitch = test_pitch;
                            ref_traj = traj;
                        }
                    }
                }
                
                SolverSolution sol;
                sol.found = true;
                sol.display_pitch = ref_pitch;
                sol.yaw = best_yaw;
                sol.throw_type = t_type;
                sol.trajectory = ref_traj;
                sol.zone_index = i;
                sol.landing_error = glm::distance(ref_traj.points.back(), zone_center);
                solutions.push_back(sol);
                break; // Only need one throw type per zone
            }
        }
    }
    
    return solutions;
}
