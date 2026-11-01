#pragma once

// Embedded web radar server.
//
// Re-implements the FC2observ (https://gitlab.com/vacmeharder/fc2observ)
// backend in plain C++ so the radar can be served straight from BUNNYWARE's own
// memory reader instead of a separate FC2 process.
//
// The server exposes two POSIX TCP listeners:
//   * an HTTP listener that serves the vendored web frontend (html/css/js/maps)
//     plus the /doorknock discovery endpoint, and
//   * a WebSocket listener that streams live game state as JSON.
//
// All sockets are handled on dedicated background threads; publish() is the
// only entry point touched by the game loop and is safe to call every frame.

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace radar {

// One player dot on the radar. Angles are expected already converted into the
// frontend's 0..360 radar space (see App's conversion) and teams use the CS2
// convention (2 = T, 3 = CT).
struct PlayerSnapshot {
    int num = 0;
    std::string name;
    int team = 0;
    int health = 0;
    bool active = false;
    float flashed = 0.0f;
    bool bomb = false;
    bool disconnected = false;
    float angle = 0.0f;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    // Optional muzzle-flash data. When weapon is empty the frontend simply
    // skips the "shooting" indicator for this player.
    std::string weapon;
    int ammo = -1;
};

struct BombSnapshot {
    std::string state = "carried";  // carried|dropped|planted|defusing|defused|exploded
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float blow_time = 0.0f;
    bool defuse_possible = false;
    bool is_defusing = false;
};

struct SmokeSnapshot {
    std::string id;
    float time = 0.0f;  // seconds the smoke has been active
    std::string team;   // "T" or "CT"
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct InfernoSnapshot {
    std::string id;
    std::vector<float> flames_x;
    std::vector<float> flames_y;
    std::vector<float> flames_z;
};

struct ProjectileSnapshot {
    std::string id;
    std::string type;  // frag|firebomb|flashbang|smoke
    std::string team;  // "T" or "CT"
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

// Full radar frame produced by the game reader each tick.
struct Snapshot {
    bool in_game = false;
    std::string map = "waiting";
    std::string phase;
    int t_score = 0;
    int ct_score = 0;
    int round = 0;
    std::string time_remaining = "0:00";
    std::vector<PlayerSnapshot> players;
    BombSnapshot bomb;
    std::vector<SmokeSnapshot> smokes;
    std::vector<InfernoSnapshot> infernos;
    std::vector<ProjectileSnapshot> projectiles;
};

// Presentation options handed to the browser on the "welcome" packet.
struct WebConfig {
    bool transparent = false;
    bool highest_player_on_top = true;
    std::string show_buyzones = "buytime";  // never|buytime|always
    std::string show_name = "both";         // never|both|always
    int max_name_length = 8;
    bool shooting = true;
    bool damage = true;
    bool flashes = true;
    bool projectiles = true;
    bool smoke_colors = true;
    bool plain_projectiles = false;
    int player_smoothing = 13;
    int projectile_smoothing = 5;
    float tombstone_opacity = 0.4f;
    float player_dot_scale = 0.6f;
    float bomb_dot_scale = 0.7f;
    std::string vert_indicator_type = "scale";  // none|color|scale
    float vert_indicator_scale_delta = 1.0f;
    bool autozoom = false;
    int autozoom_smoothing = 32;
    float autozoom_padding = 0.3f;
    float autozoom_min_zoom = 1.3f;
};

struct ServerStatus {
    bool running = false;
    int http_port = 0;
    int ws_port = 0;
    int clients = 0;
    uint64_t updates = 0;
    std::string last_error;
};

class RadarServer {
public:
    RadarServer();
    ~RadarServer();

    RadarServer(const RadarServer&) = delete;
    RadarServer& operator=(const RadarServer&) = delete;

    // Binds the HTTP and WebSocket listeners. asset_root points at the folder
    // containing the vendored html/css/renderers/img/maps tree. Returns false
    // (with last_error() set) when a port is unavailable.
    bool start(const std::string& host, int http_port, int ws_port, const std::string& asset_root);

    // Stops both listeners and disconnects every client. Safe to call when not
    // running.
    void stop();

    bool is_running() const { return running_.load(std::memory_order_acquire); }
    std::string last_error() const;
    ServerStatus status() const;

    void set_web_config(const WebConfig& config);

    // Called from the game loop. Serialises the snapshot and pushes it to every
    // connected browser. Cheap no-op when nobody is connected.
    void publish(const Snapshot& snapshot);

    // Opens http://127.0.0.1:<port>/ in the user's default browser.
    static bool open_in_browser(int http_port);

    static const char* version();

    // Opaque implementation state (defined in RadarServer.cpp). Public so the
    // translation unit's internal helper functions can name it.
    struct Impl;

private:
    std::shared_ptr<Impl> impl_;
    std::atomic<bool> running_{ false };
};

// -----------------------------------------------------------------------------
// Menu <-> App control bridge
// -----------------------------------------------------------------------------
// The settings menu is rendered on the UI thread and cannot reach App's private
// members, so start/stop intents are posted here and consumed by App once per
// frame. active_server() exposes the live server for status read-outs.
RadarServer* active_server();
void set_active_server(RadarServer* server);
void request_start();
void request_stop();
bool take_start_request();
bool take_stop_request();

}  // namespace radar
