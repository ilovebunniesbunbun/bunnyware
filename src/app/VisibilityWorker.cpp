#include "VisibilityWorker.hpp"
#include "overlay/bvh_parser.hpp"
#include "logger.hpp"
#include <algorithm>
#include <sys/resource.h>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/norm.hpp>

namespace {

    inline uint32_t compact1(uint32_t v) {
        v &= 0x09249249U;
        v = (v ^ (v >> 2)) & 0x030C30C3U;
        v = (v ^ (v >> 4)) & 0x0300F00FU;
        v = (v ^ (v >> 8)) & 0x00000FFFU;
        return v;
    }

    inline void morton3_decode(uint32_t m, int& x, int& y, int& z) {
        x = static_cast<int>(compact1(m));
        y = static_cast<int>(compact1(m >> 1));
        z = static_cast<int>(compact1(m >> 2));
    }

    inline bool segment_aabb_intersect(const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& bmin, const glm::vec3& bmax, float& t0, float& t1) {
        glm::vec3 d = p1 - p0;
        t0 = 0.0f;
        t1 = 1.0f;

        auto check_axis = [&](float p0a, float da, float min_a, float max_a) -> bool {
            const float epsilon = 1e-6f;
            if (std::abs(da) < epsilon) {
                return p0a >= min_a && p0a <= max_a;
            }

            float inv = 1.0f / da;
            float t_near = (min_a - p0a) * inv;
            float t_far = (max_a - p0a) * inv;

            if (t_near > t_far) {
                std::swap(t_near, t_far);
            }

            t0 = std::max(t0, t_near);
            t1 = std::min(t1, t_far);

            return t0 <= t1;
        };

        if (!check_axis(p0.x, d.x, bmin.x, bmax.x)) return false;
        if (!check_axis(p0.y, d.y, bmin.y, bmax.y)) return false;
        if (!check_axis(p0.z, d.z, bmin.z, bmax.z)) return false;

        return true;
    }

    inline bool line_goes_through_smoke(const glm::vec3& start, const glm::vec3& end, const ShmPacket& packet) {
        if (packet.smoke_count == 0) return false;

        constexpr float half = 10.0f;
        constexpr float extinction = 6.0f;
        constexpr float limit = 12.0f;

        float seg_len = glm::length(end - start);
        if (seg_len < 1e-6f) return false;

        for (int i = 0; i < packet.smoke_count; ++i) {
            const auto& smoke = packet.smokes[i];
            if (!smoke.active) continue;

            glm::vec3 center = { smoke.center.x, smoke.center.y, smoke.center.z };
            float tau = 0.0f;

            for (uint32_t w = 0; w < 512; ++w) {
                uint64_t bits = smoke.bitset[w];
                while (bits) {
                    uint32_t bit_idx = __builtin_ctzll(bits);
                    bits &= (bits - 1);

                    uint32_t morton = (w << 6) | bit_idx;
                    int x = 0, y = 0, z = 0;
                    morton3_decode(morton, x, y, z);

                    int lin = x + 32 * (y + 32 * z);
                    uint8_t alpha = smoke.alphas[lin];
                    if (alpha < 15) continue;

                    glm::vec3 voxel_pos = {
                        center.x + (static_cast<float>(x) - 16.0f) * 20.0f,
                        center.y + (static_cast<float>(y) - 16.0f) * 20.0f,
                        center.z + (static_cast<float>(z) - 16.0f) * 20.0f
                    };

                    glm::vec3 bmin = voxel_pos - half;
                    glm::vec3 bmax = voxel_pos + half;

                    float t0 = 0.0f, t1 = 0.0f;
                    if (segment_aabb_intersect(start, end, bmin, bmax, t0, t1)) {
                        float inside_len = (t1 - t0) * seg_len;
                        if (inside_len > 0.0f) {
                            tau += extinction * (static_cast<float>(alpha) / 255.0f) * (inside_len / 20.0f);
                            if (tau >= limit) {
                                return true;
                            }
                        }
                    }
                }
            }
        }
        return false;
    }

} // namespace

VisibilityWorker::VisibilityWorker() {}

VisibilityWorker::~VisibilityWorker() {
    stop();
}

void VisibilityWorker::start() {
    std::lock_guard<std::mutex> lock(mutex);
    if (!worker_thread.joinable()) {
        quit = false;
        has_work = false;
        solver_quit_ = false;
        solver_has_work_ = false;
        worker_thread = std::thread(&VisibilityWorker::run, this);
        solver_thread_ = std::thread(&VisibilityWorker::solver_run, this);
    }
}

void VisibilityWorker::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (worker_thread.joinable()) {
            quit = true;
            has_work = true;
            cv.notify_one();
        }
    }
    {
        std::lock_guard<std::mutex> lock(solver_mutex_);
        solver_quit_ = true;
        solver_has_work_ = true;
        solver_cv_.notify_one();
    }
    if (worker_thread.joinable()) worker_thread.join();
    if (solver_thread_.joinable()) solver_thread_.join();
}

void VisibilityWorker::submit_work(const ShmPacket& packet, const OverlayConfig& cfg) {
    std::lock_guard<std::mutex> lock(mutex);
    current_packet = packet;
    cfg_input = cfg;
    has_work = true;
    cv.notify_one();
}

void VisibilityWorker::update_map(MapParser::MapMesh mesh, const OverlayConfig& cfg) {
    std::lock_guard<std::mutex> lock(mutex);
    pending_map_mesh = std::move(mesh);
    map_needs_reload = true;
    cfg_input = cfg;
    has_work = true;
    cv.notify_one();
}

bool VisibilityWorker::get_latest_result(VischeckResult& out_result) {
    std::lock_guard<std::mutex> lock(mutex);
    if (has_new_result) {
        out_result = std::move(latest_result);
        has_new_result = false;
        return true;
    }
    return false;
}

void VisibilityWorker::request_point_trace(const glm::vec3& eye, const glm::vec3& forward) {
    std::lock_guard<std::mutex> lock(mutex);
    trace_eye_ = eye;
    trace_forward_ = forward;
    trace_requested_.store(true);
    has_work = true;
    cv.notify_one();
}

bool VisibilityWorker::get_traced_point(glm::vec3& out_point) {
    if (trace_result_ready_.exchange(false)) {
        out_point = traced_point_;
        return true;
    }
    return false;
}

void VisibilityWorker::set_landing_zones(const std::vector<LandingZone>& zones) {
    std::lock_guard<std::mutex> lock(zones_mutex_);
    current_zones_ = zones;
}

TraceResult VisibilityWorker::trace_ray_bvh(const glm::vec3& start, const glm::vec3& end) const {
    std::lock_guard<std::mutex> lock(bvh_mutex);
    return local_bvh.trace_ray(start, end);
}

// ============================================================================
// Solver thread — runs independently, never blocks the main worker
// ============================================================================
void VisibilityWorker::solver_run() {
    // Lowest scheduling priority so we never starve game or overlay
    setpriority(PRIO_PROCESS, 0, 19);

    LocalMapBVH solver_bvh;

    while (true) {
        glm::vec3 eye;
        uint8_t grenade_type;
        std::vector<LandingZone> zones;
        std::vector<LocalMapBVH::Triangle> door_tris;
        float max_range;

        {
            std::unique_lock<std::mutex> lock(solver_mutex_);
            solver_cv_.wait(lock, [this]() { return solver_quit_ || solver_has_work_; });

            if (solver_quit_) break;

            eye = solver_eye_;
            grenade_type = solver_grenade_type_;
            zones = solver_zones_;
            door_tris = solver_door_tris_;
            max_range = solver_max_range_;

            // Copy BVH if pointer is set
            if (solver_bvh_ptr_) {
                solver_bvh = *solver_bvh_ptr_;
                solver_bvh_ptr_ = nullptr;
            }

            solver_has_work_ = false;
        }

        // Update progress callback
        auto progress_cb = [&](float pitch, float yaw, ThrowType /*type*/, const std::vector<glm::vec3>& /*points*/) {
            std::lock_guard<std::mutex> plock(progress_mutex_);
            solver_progress_.active = true;
            for (int j = 4; j > 0; --j) {
                solver_progress_.recent_tests[j] = solver_progress_.recent_tests[j - 1];
            }
            solver_progress_.recent_tests[0] = { pitch, yaw };
            solver_progress_.test_count = std::min(5, solver_progress_.test_count + 1);
        };

        // Run the expensive solve
        auto solutions = solver_.solve_all(
            eye, grenade_type,
            zones, solver_bvh, door_tris, max_range,
            progress_cb
        );

        // Turn off active progress when done
        {
            std::lock_guard<std::mutex> plock(progress_mutex_);
            solver_progress_.active = false;
            solver_progress_.test_count = 0;
        }

        // Publish results
        {
            std::lock_guard<std::mutex> lock(solver_result_mutex_);
            solver_latest_solutions_ = std::move(solutions);
            solver_result_active_ = true;
        }
    }
}

// ============================================================================
// Main worker thread — fast path: trajectories, inflight, infernos, traces
// ============================================================================
void VisibilityWorker::run() {
    ShmPacket local_packet;
    OverlayConfig local_cfg;
    std::vector<TrajectoryResult> cached_trajectories;
    
    while (true) {
        {
            std::unique_lock<std::mutex> lock(mutex);
            cv.wait(lock, [this]() { return quit || has_work; });
            
            if (quit) {
                break;
            }
            
            if (map_needs_reload) {
                std::lock_guard<std::mutex> bvh_lock(bvh_mutex);
                local_bvh.triangles.clear();
                for (const auto& tri : pending_map_mesh.Triangles) {
                    LocalMapBVH::Triangle t;
                    t.v0 = { tri.v0.pos.x, tri.v0.pos.y, tri.v0.pos.z };
                    t.v1 = { tri.v1.pos.x, tri.v1.pos.y, tri.v1.pos.z };
                    t.v2 = { tri.v2.pos.x, tri.v2.pos.y, tri.v2.pos.z };
                    local_bvh.triangles.push_back(t);
                }
                local_bvh.build();

                local_visual_bvh.triangles.clear();
                for (const auto& tri : pending_map_mesh.VisualTriangles) {
                    LocalMapBVH::Triangle t;
                    t.v0 = { tri.v0.pos.x, tri.v0.pos.y, tri.v0.pos.z };
                    t.v1 = { tri.v1.pos.x, tri.v1.pos.y, tri.v1.pos.z };
                    t.v2 = { tri.v2.pos.x, tri.v2.pos.y, tri.v2.pos.z };
                    local_visual_bvh.triangles.push_back(t);
                }
                local_visual_bvh.build();

                // Give the solver thread a fresh copy of the BVH
                {
                    std::lock_guard<std::mutex> slock(solver_mutex_);
                    solver_bvh_ptr_ = &local_bvh;
                }

                map_needs_reload = false;
                pending_map_mesh.Triangles.clear();
                pending_map_mesh.VisualTriangles.clear();
                cached_trajectories.clear();
            }
            
            local_packet = current_packet;
            local_cfg = cfg_input;
            has_work = false;
        }
        
        std::vector<LocalMapBVH::Triangle> world_door_triangles;
        for (int i = 0; i < local_packet.door_count; ++i) {
            const auto& door = local_packet.doors[i];
            if (!door.active || door.model_name[0] == '\0') continue;
            
            std::vector<MapParser::Triangle> local_tris = MapParser::GetDoorMesh(door.model_name);
            if (local_tris.empty()) continue;
            
            float yaw_rad = door.yaw * 3.14159265f / 180.0f;
            float cos_yaw = std::cos(yaw_rad);
            float sin_yaw = std::sin(yaw_rad);
            
            for (const auto& tri : local_tris) {
                LocalMapBVH::Triangle wt;
                // Transform v0
                {
                    float x = tri.v0.pos.x * door.scale.x;
                    float y = tri.v0.pos.y * door.scale.y;
                    float z = tri.v0.pos.z * door.scale.z;
                    wt.v0.x = x * cos_yaw - y * sin_yaw + door.origin.x;
                    wt.v0.y = x * sin_yaw + y * cos_yaw + door.origin.y;
                    wt.v0.z = z + door.origin.z;
                }
                // Transform v1
                {
                    float x = tri.v1.pos.x * door.scale.x;
                    float y = tri.v1.pos.y * door.scale.y;
                    float z = tri.v1.pos.z * door.scale.z;
                    wt.v1.x = x * cos_yaw - y * sin_yaw + door.origin.x;
                    wt.v1.y = x * sin_yaw + y * cos_yaw + door.origin.y;
                    wt.v1.z = z + door.origin.z;
                }
                // Transform v2
                {
                    float x = tri.v2.pos.x * door.scale.x;
                    float y = tri.v2.pos.y * door.scale.y;
                    float z = tri.v2.pos.z * door.scale.z;
                    wt.v2.x = x * cos_yaw - y * sin_yaw + door.origin.x;
                    wt.v2.y = x * sin_yaw + y * cos_yaw + door.origin.y;
                    wt.v2.z = z + door.origin.z;
                }
                world_door_triangles.push_back(wt);
            }
        }
        
        VischeckResult result;
        
        // 0. Perform BVH player skeleton bone visibility checks
        result.player_visible.assign(local_packet.player_count, false);
        result.player_occluded_by_smoke.assign(local_packet.player_count, false);
        for (int i = 0; i < local_packet.player_count; ++i) {
            const auto& player = local_packet.players[i];
            if (!player.active) continue;
            
            // By default, if we haven't hit smoke, it's false
            result.player_occluded_by_smoke[i] = false;
            
            // Check key bones for visibility:
            // 7: head
            // 5: chest/torso
            // 11, 15: shoulders/hands
            // 22, 23: feet/legs
            static const int key_bones[] = {7, 5, 11, 15, 22, 23};
            bool any_bone_visible = false;
            
            for (int b_id : key_bones) {
                if (b_id < player.bone_count) {
                    glm::vec3 bone_pos = player.bones[b_id].position;
                    
                    // Trace ray from local eye to bone position
                    TraceResult tr = local_bvh.trace_ray(local_packet.local_eye, bone_pos);
                    
                    // Check against dynamic doors
                    if (!tr.hit) {
                        glm::vec3 r_delta = bone_pos - local_packet.local_eye;
                        float r_max_dist = glm::length(r_delta);
                        if (r_max_dist > 1e-6f) {
                            glm::vec3 dir = r_delta / r_max_dist;
                            glm::vec3 origin = local_packet.local_eye;
                            float closest_t = r_max_dist;
                            bool hit_door = false;
                            for (const auto& tri : world_door_triangles) {
                                glm::vec3 e1 = tri.v1 - tri.v0;
                                glm::vec3 e2 = tri.v2 - tri.v0;
                                glm::vec3 h = glm::cross(dir, e2);
                                float a = glm::dot(e1, h);
                                if (a > -1e-6f && a < 1e-6f) continue;
                                float f = 1.0f / a;
                                glm::vec3 s = origin - tri.v0;
                                float u = f * glm::dot(s, h);
                                if (u < 0.0f || u > 1.0f) continue;
                                glm::vec3 q = glm::cross(s, e1);
                                float v = f * glm::dot(dir, q);
                                if (v < 0.0f || u + v > 1.0f) continue;
                                float t = f * glm::dot(e2, q);
                                if (t > 1e-4f && t < closest_t) {
                                    closest_t = t;
                                    hit_door = true;
                                }
                            }
                            if (hit_door) {
                                tr.hit = true;
                            }
                        }
                    }
                    
                    if (!tr.hit) {
                        bool bone_blocked_by_smoke = line_goes_through_smoke(local_packet.local_eye, bone_pos, local_packet);
                        if (bone_blocked_by_smoke) {
                            result.player_occluded_by_smoke[i] = true;
                            if (local_cfg.esp_smoke_check) {
                                tr.hit = true;
                            }
                        }

                        if (!tr.hit) {
                            any_bone_visible = true;
                            if (!bone_blocked_by_smoke) {
                                result.player_occluded_by_smoke[i] = false; // if ANY bone is visible and not in smoke, not fully occluded
                            }
                            break; // At least one bone is visible, player is visible!
                        }
                    }
                }
            }
            result.player_visible[i] = any_bone_visible;
        }
        
        // Helper to check for enemy impact scans using map BVH occlusion
        auto count_affected_enemies = [&](const glm::vec3& detonation_pos, uint8_t weapon_type) -> int {
            float radius_sqr = 0.0f;
            bool is_flash = false;
            
            if (weapon_type == GRENADE_HE) radius_sqr = 350.0f * 350.0f;
            else if (weapon_type == GRENADE_MOLOTOV) radius_sqr = 150.0f * 150.0f;
            else if (weapon_type == GRENADE_FLASH) {
                radius_sqr = 1000.0f * 1000.0f;
                is_flash = true;
            } else {
                return 0;
            }
            
            int affected_count = 0;
            for (int i = 0; i < local_packet.player_count; ++i) {
                const auto& player = local_packet.players[i];
                if (!player.active || player.health <= 0) continue;
                
                // Read local player's team to ensure we only count enemies
                // (Lua code checks local team. Our packet contains the enemy players list already,
                // but let's double check distance and LoS)
                glm::vec3 head_pos = player.bones[7].position; // bone 7 is head/eye
                glm::vec3 delta = head_pos - detonation_pos;
                float dist_sq = glm::dot(delta, delta);
                if (dist_sq <= radius_sqr) {
                    // Trace occlusion ray from detonation center to player head
                    TraceResult tr = local_bvh.trace_ray(detonation_pos, head_pos);
                    
                    // Trace against door triangles if map geometry wasn't hit
                    if (!tr.hit) {
                        glm::vec3 r_delta = head_pos - detonation_pos;
                        float r_max_dist = glm::length(r_delta);
                        if (r_max_dist > 1e-6f) {
                            glm::vec3 dir = r_delta / r_max_dist;
                            glm::vec3 origin = detonation_pos;
                            float closest_t = r_max_dist;
                            bool hit_door = false;
                            for (const auto& tri : world_door_triangles) {
                                glm::vec3 e1 = tri.v1 - tri.v0;
                                glm::vec3 e2 = tri.v2 - tri.v0;
                                glm::vec3 h = glm::cross(dir, e2);
                                float a = glm::dot(e1, h);
                                if (a > -1e-6f && a < 1e-6f) continue;
                                float f = 1.0f / a;
                                glm::vec3 s = origin - tri.v0;
                                float u = f * glm::dot(s, h);
                                if (u < 0.0f || u > 1.0f) continue;
                                glm::vec3 q = glm::cross(s, e1);
                                float v = f * glm::dot(dir, q);
                                if (v < 0.0f || u + v > 1.0f) continue;
                                float t = f * glm::dot(e2, q);
                                if (t > 1e-4f && t < closest_t) {
                                    closest_t = t;
                                    hit_door = true;
                                }
                            }
                            if (hit_door) {
                                tr.hit = true;
                                tr.distance = closest_t;
                                tr.end_pos = detonation_pos + dir * closest_t;
                            }
                        }
                    }

                    if (!tr.hit || glm::distance2(tr.end_pos, head_pos) < 100.0f) {
                        if (is_flash) {
                            // Derive player's view vector from head bone rotation quaternion
                            // (Rotation rotation is a quaternion vec4_t: x,y,z,w)
                            glm::quat q(player.bones[7].rotation.w, player.bones[7].rotation.x, player.bones[7].rotation.y, player.bones[7].rotation.z);
                            glm::vec3 forward = q * glm::vec3(0.0f, 0.0f, -1.0f); // Default forward vector
                            glm::vec3 to_nade = glm::normalize(detonation_pos - head_pos);
                            if (glm::dot(forward, to_nade) > -0.5f) { // ~120 degrees FOV check
                                affected_count++;
                            }
                        } else {
                            affected_count++;
                        }
                    }
                }
            }
            return affected_count;
        };

        // 1. Local player held grenade trajectory simulation
        if (local_packet.held_grenade_type != GRENADE_NONE && local_packet.pin_pulled) {
            float pitch = local_packet.local_angles.x;
            float yaw = local_packet.local_angles.y;

            // Normalize pitch to [-90, 90]
            if (pitch > 90.0f) pitch -= 360.0f;
            else if (pitch < -90.0f) pitch += 360.0f;

            // Apply CS2 pitch throw offset correction
            pitch = pitch - (90.0f - std::abs(pitch)) * 10.0f / 90.0f;

            float p_rad = pitch * 3.14159265f / 180.0f;
            float y_rad = yaw * 3.14159265f / 180.0f;
            
            glm::vec3 forward(
                std::cos(p_rad) * std::cos(y_rad),
                std::cos(p_rad) * std::sin(y_rad),
                -std::sin(p_rad)
            );

            float strength = local_packet.throw_strength;
            if (std::abs(strength - 0.5f) <= 0.1f) {
                strength = 0.5f;
            }

            float clamped_uncorrected = std::max(15.0f, std::min(750.0f, 750.0f * 0.9f));
            float speed_uncorrected = (strength * 0.7f + 0.3f) * clamped_uncorrected;
            
            float clamped_corrected = std::max(15.0f, std::min(750.0f, 750.0f));
            float speed_corrected = (strength * 0.7f + 0.3f) * clamped_corrected;
            
            glm::vec3 origin = {
                local_packet.local_eye.x + forward.x * 16.0f,
                local_packet.local_eye.y + forward.y * 16.0f,
                local_packet.local_eye.z + forward.z * 16.0f + strength * 12.0f - 12.0f
            };

            // Raytrace checks to prevent throw origin clipping into walls
            glm::vec3 trace_end = origin + forward * 22.0f;
            TraceResult tr = local_bvh.trace_ray(origin, trace_end);
            
            // Trace against door triangles if map geometry wasn't hit
            if (!tr.hit) {
                glm::vec3 r_delta = trace_end - origin;
                float r_max_dist = glm::length(r_delta);
                if (r_max_dist > 1e-6f) {
                    glm::vec3 dir = r_delta / r_max_dist;
                    float closest_t = r_max_dist;
                    bool hit_door = false;
                    for (const auto& tri : world_door_triangles) {
                        glm::vec3 e1 = tri.v1 - tri.v0;
                        glm::vec3 e2 = tri.v2 - tri.v0;
                        glm::vec3 h = glm::cross(dir, e2);
                        float a = glm::dot(e1, h);
                        if (a > -1e-6f && a < 1e-6f) continue;
                        float f = 1.0f / a;
                        glm::vec3 s = origin - tri.v0;
                        float u = f * glm::dot(s, h);
                        if (u < 0.0f || u > 1.0f) continue;
                        glm::vec3 q = glm::cross(s, e1);
                        float v = f * glm::dot(dir, q);
                        if (v < 0.0f || u + v > 1.0f) continue;
                        float t = f * glm::dot(e2, q);
                        if (t > 1e-4f && t < closest_t) {
                            closest_t = t;
                            hit_door = true;
                        }
                    }
                    if (hit_door) {
                        tr.hit = true;
                        tr.distance = closest_t;
                        tr.end_pos = origin + dir * closest_t;
                    }
                }
            }

            if (tr.hit) {
                origin = tr.end_pos - forward * 6.0f;
            } else {
                origin = origin + forward * 16.0f;
            }
            
            glm::vec3 velocity_uncorrected = {
                forward.x * speed_uncorrected + local_packet.local_velocity.x * 1.25f,
                forward.y * speed_uncorrected + local_packet.local_velocity.y * 1.25f,
                forward.z * speed_uncorrected + local_packet.local_velocity.z * 1.0f
            };
            
            glm::vec3 velocity_corrected = {
                forward.x * speed_corrected + local_packet.local_velocity.x * 1.25f,
                forward.y * speed_corrected + local_packet.local_velocity.y * 1.25f,
                forward.z * speed_corrected + local_packet.local_velocity.z * 1.0f
            };
            
            // Main trajectory (matches game's m_vInitialVelocity which already bakes in 0.9x)
            result.held_trajectory = simulate_trajectory(origin, velocity_uncorrected, local_packet.held_grenade_type, local_bvh, world_door_triangles, 0.0f);
            result.held_trajectory.type = local_packet.held_grenade_type;
            if (result.held_trajectory.valid && result.held_trajectory.points.size() >= 2) {
                result.held_trajectory.affected_count = count_affected_enemies(result.held_trajectory.end_pos, local_packet.held_grenade_type);
            }
            
            // Ghost comparison trajectory (750 + drag + 1.25x model)
            result.held_trajectory_corrected = simulate_trajectory(origin, velocity_corrected, local_packet.held_grenade_type, local_bvh, world_door_triangles, 0.2f);
            result.held_trajectory_corrected.type = local_packet.held_grenade_type;
            if (result.held_trajectory_corrected.valid && result.held_trajectory_corrected.points.size() >= 2) {
                result.held_trajectory_corrected.affected_count = count_affected_enemies(result.held_trajectory_corrected.end_pos, local_packet.held_grenade_type);
            }
        }
        
        // 2. In-flight projectile trajectory simulation
        std::vector<TrajectoryResult> next_cache;
        int proj_count = std::min(local_packet.projectile_count, static_cast<int>(shm::MAX_PROJECTILES));
        for (int i = 0; i < proj_count; ++i) {
            const auto& proj = local_packet.projectiles[i];
            if (!proj.active || proj.entity_handle < 0x1000) continue;
            
            // Wait until the projectile has a valid, non-zero initial velocity before simulating
            float speed_sq = proj.initial_velocity.x * proj.initial_velocity.x + 
                             proj.initial_velocity.y * proj.initial_velocity.y + 
                             proj.initial_velocity.z * proj.initial_velocity.z;
            if (speed_sq < 10.0f) {
                continue;
            }

            bool found_cached = false;
            TrajectoryResult traj;
            for (const auto& cached_traj : cached_trajectories) {
                if (cached_traj.entity_handle == proj.entity_handle) {
                    traj = cached_traj;
                    found_cached = true;
                    break;
                }
            }

            if (!found_cached) {
                traj = simulate_trajectory(proj.initial_position, proj.initial_velocity, proj.type, local_bvh, world_door_triangles);
                traj.has_current_pos = true;
                traj.entity_handle = proj.entity_handle;
                traj.spawn_time = proj.spawn_time;
            }

            traj.type = proj.type;
            traj.current_pos = proj.current_position;
            traj.timer_start_time = proj.timer_start_time;
            traj.timer_duration = proj.duration;

            // Re-calculate affected enemies at predicted detonation point
            if (traj.valid && traj.points.size() >= 2) {
                traj.affected_count = count_affected_enemies(traj.end_pos, proj.type);
            }

            next_cache.push_back(traj);
            result.inflight_trajectories.push_back(traj);
        }
        cached_trajectories = std::move(next_cache);
        
        // 3. Ground Inferno Zones copy
        result.inferno_count = std::min(local_packet.inferno_count, 4);
        for (int i = 0; i < result.inferno_count; ++i) {
            result.infernos[i] = local_packet.infernos[i];
        }

        // 3.5. Active Smokes copy
        result.smoke_count = std::min(local_packet.smoke_count, 4);
        for (int i = 0; i < result.smoke_count; ++i) {
            result.smokes[i] = local_packet.smokes[i];
        }

        // 4. Point trace for landing zone creation
        if (trace_requested_.load()) {
            glm::vec3 end = trace_eye_ + trace_forward_ * 10000.0f;
            // Trace against the visual BVH first so we land on actual visible structures/walls
            TraceResult tr = local_visual_bvh.trace_ray(trace_eye_, end);
            if (!tr.hit) {
                tr = local_bvh.trace_ray(trace_eye_, end);
            }
            
            if (tr.hit) {
                traced_point_ = tr.end_pos;
            } else {
                traced_point_ = end;
            }
            trace_result_ready_.store(true);
            trace_requested_.store(false);
        }

        // 5. Grenade solver — dispatch to solver thread (non-blocking)
        glm::vec2 player_pos_2d = {local_packet.local_origin.x, local_packet.local_origin.y};
        float move_dist = glm::distance(player_pos_2d, glm::vec2(last_player_pos_.x, last_player_pos_.y));
        last_player_pos_ = {player_pos_2d.x, player_pos_2d.y, local_packet.local_origin.z};

        uint64_t current_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        float dt = (current_time_ms - last_packet_time_ms_) / 1000.0f;
        last_packet_time_ms_ = current_time_ms;
        if (dt > 1.0f) dt = 0.0f;

        bool player_still = (move_dist < 2.0f);
        if (player_still) {
            still_timer_ += dt;
        } else {
            still_timer_ = 0.0f;
        }

        std::vector<LandingZone> active_zones;
        {
            std::lock_guard<std::mutex> lock(zones_mutex_);
            active_zones = current_zones_;
        }

        bool should_solve = local_cfg.grenade_helper_enabled
                         && local_cfg.grenade_solver_show_trajectory
                         && (local_packet.held_weapon_id >= 43 && local_packet.held_weapon_id <= 48) // A grenade is held
                         && (still_timer_ >= local_cfg.grenade_solver_still_time)
                         && !active_zones.empty();

        if (should_solve) {
            glm::vec3 eye = {local_packet.local_eye.x, local_packet.local_eye.y, local_packet.local_eye.z};
            uint8_t target_grenade_type = 0;
            switch (local_packet.held_weapon_id) {
                case 44: target_grenade_type = GRENADE_HE; break;
                case 43: target_grenade_type = GRENADE_FLASH; break;
                case 45: target_grenade_type = GRENADE_SMOKE; break;
                case 46: case 48: target_grenade_type = GRENADE_MOLOTOV; break;
                case 47: target_grenade_type = GRENADE_DECOY; break;
            }

            // Check if we can reuse cached solutions (Only compare X and Y coordinates to preserve cache on jumping)
            bool reuse_cache = has_cached_solutions_
                && (target_grenade_type == last_solved_grenade_type_)
                && (glm::distance(glm::vec2(eye.x, eye.y), glm::vec2(last_solved_pos_.x, last_solved_pos_.y)) < 0.5f)
                && (active_zones.size() == last_solved_zones_.size());
            
            if (reuse_cache) {
                for (size_t idx = 0; idx < active_zones.size(); ++idx) {
                    if (active_zones[idx].name != last_solved_zones_[idx].name ||
                        active_zones[idx].grenade_type != last_solved_zones_[idx].grenade_type ||
                        active_zones[idx].mins != last_solved_zones_[idx].mins ||
                        active_zones[idx].maxs != last_solved_zones_[idx].maxs) {
                        reuse_cache = false;
                        break;
                    }
                }
            }

            if (reuse_cache) {
                result.solver_solutions = cached_solutions_;
            } else {
                // Clear cache immediately on movement
                cached_solutions_.clear();
                has_cached_solutions_ = false;

                // Dispatch to solver thread (non-blocking)
                {
                    std::lock_guard<std::mutex> slock(solver_mutex_);
                    solver_eye_ = eye;
                    solver_grenade_type_ = target_grenade_type;
                    solver_zones_ = active_zones;
                    solver_door_tris_ = world_door_triangles;
                    solver_max_range_ = local_cfg.grenade_solver_max_range;
                    // Give the solver thread its own copy of the BVH
                    solver_bvh_ptr_ = &local_bvh;
                    solver_has_work_ = true;
                    solver_cv_.notify_one();
                }
            }

            // Pick up any completed solver results
            {
                std::lock_guard<std::mutex> slock(solver_result_mutex_);
                if (solver_result_active_) {
                    cached_solutions_ = solver_latest_solutions_;
                    result.solver_solutions = cached_solutions_;
                    last_solved_pos_ = eye;
                    last_solved_grenade_type_ = target_grenade_type;
                    last_solved_zones_ = active_zones;
                    has_cached_solutions_ = true;
                    solver_result_active_ = false;
                }
            }

            result.solver_active = true;
        } else {
            result.solver_active = false;
            has_cached_solutions_ = false;
            cached_solutions_.clear();
        }

        // Copy latest solver progress animation data
        {
            std::lock_guard<std::mutex> plock(progress_mutex_);
            result.solver_progress = solver_progress_;
        }

        {
            std::lock_guard<std::mutex> lock(mutex);
            latest_result = std::move(result);
            has_new_result = true;
        }
    }
}
