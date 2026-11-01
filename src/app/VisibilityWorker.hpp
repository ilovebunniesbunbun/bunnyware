#pragma once
#include <mutex>
#include <condition_variable>
#include <thread>
#include <vector>
#include <atomic>
#include "overlay/game_reader.hpp"
#include "config.hpp"
#include "overlay/trajectory_sim.hpp"
#include "vpk/vmdl/maps/map_parser.hpp"
#include "GrenadeSolver.hpp"
#include "LandingZoneManager.hpp"

struct SolverProgress {
    bool active = false;
    struct AngleProgress {
        float pitch = 0.0f;
        float yaw = 0.0f;
    };
    AngleProgress recent_tests[5];
    int test_count = 0;
};

struct VischeckResult {
    TrajectoryResult held_trajectory;
    TrajectoryResult held_trajectory_corrected;
    std::vector<TrajectoryResult> inflight_trajectories;
    int inferno_count = 0;
    InfernoData infernos[4];
    int smoke_count = 0;
    ActiveSmokeData smokes[4];
    std::vector<SolverSolution> solver_solutions;
    bool solver_active = false;
    SolverProgress solver_progress;
    std::vector<bool> player_visible;
    std::vector<bool> player_occluded_by_smoke;
};

class VisibilityWorker {
public:
    VisibilityWorker();
    ~VisibilityWorker();

    void start();
    void stop();

    // Submit new packet and config to process
    void submit_work(const ShmPacket& packet, const OverlayConfig& cfg);

    // Signal map geometry reload
    void update_map(MapParser::MapMesh mesh, const OverlayConfig& cfg);

    // Retrieve latest results if available. Returns true if there was a new result.
    bool get_latest_result(VischeckResult& out_result);

    // Request a BVH raytrace for landing zone point creation
    void request_point_trace(const glm::vec3& eye, const glm::vec3& forward);
    bool get_traced_point(glm::vec3& out_point);

    // Set current landing zones for solver
    void set_landing_zones(const std::vector<LandingZone>& zones);

    // Thread-safe BVH ray trace query
    TraceResult trace_ray_bvh(const glm::vec3& start, const glm::vec3& end) const;

private:
    mutable std::mutex bvh_mutex;
    LocalMapBVH local_bvh;
    LocalMapBVH local_visual_bvh;
    void run();
    void solver_run();

    std::thread worker_thread;
    std::mutex mutex;
    std::condition_variable cv;
    bool quit = false;
    bool has_work = false;

    // Inputs
    ShmPacket current_packet;
    MapParser::MapMesh pending_map_mesh;
    bool map_needs_reload = false;
    OverlayConfig cfg_input;

    // Outputs
    VischeckResult latest_result;
    bool has_new_result = false;

    // Point trace state
    std::atomic<bool> trace_requested_{false};
    glm::vec3 trace_eye_, trace_forward_;
    glm::vec3 traced_point_;
    std::atomic<bool> trace_result_ready_{false};

    // Solver state — runs on its own thread
    std::thread solver_thread_;
    std::mutex solver_mutex_;
    std::condition_variable solver_cv_;
    bool solver_quit_ = false;
    bool solver_has_work_ = false;

    // Solver inputs (written by main worker, read by solver thread)
    glm::vec3 solver_eye_{0,0,0};
    uint8_t solver_grenade_type_ = 0;
    std::vector<LandingZone> solver_zones_;
    LocalMapBVH* solver_bvh_ptr_ = nullptr;
    std::vector<LocalMapBVH::Triangle> solver_door_tris_;
    float solver_max_range_ = 0.0f;

    // Solver outputs (written by solver thread, read by main worker)
    std::mutex solver_result_mutex_;
    std::vector<SolverSolution> solver_latest_solutions_;
    bool solver_result_active_ = false;

    std::mutex progress_mutex_;
    SolverProgress solver_progress_;

    GrenadeSolver solver_;
    std::vector<LandingZone> current_zones_;
    std::mutex zones_mutex_;
    float still_timer_ = 0.0f;
    glm::vec3 last_player_pos_{0,0,0};
    uint64_t last_packet_time_ms_ = 0;

    // Cache to prevent re-solving when standing still
    std::vector<SolverSolution> cached_solutions_;
    glm::vec3 last_solved_pos_{0,0,0};
    uint8_t last_solved_grenade_type_ = 0;
    std::vector<LandingZone> last_solved_zones_;
    bool has_cached_solutions_ = false;
};
