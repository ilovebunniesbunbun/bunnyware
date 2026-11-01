#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <atomic>
#include "overlay/game_reader.hpp" // For Vec3 and GrenadeType

struct GrenadeMovement {
    std::string throw_type = "none";
    std::vector<std::string> modifiers;
    std::string movement_key;
    bool jump = false;
    int movement_ticks = 0;
};

struct GrenadeLineup {
    std::string title;
    std::string description;
    std::string throw_method;
    Vec3 position;
    Vec3 viewangles;
    uint8_t weapon;
    GrenadeMovement movement;
};

class GrenadeHelperData {
public:
    GrenadeHelperData();
    ~GrenadeHelperData();

    void initialize(const std::string& resources_dir);
    void reload();

    // Gets lineups for a specific map. Thread-safe copy.
    std::vector<GrenadeLineup> get_lineups(const std::string& map_name) const;

    // Add a lineup to local config
    void add_local_lineup(const std::string& map_name, const GrenadeLineup& lineup);

private:
    void download_and_parse();
    void parse_file(const std::string& path, std::unordered_map<std::string, std::vector<GrenadeLineup>>& out_map);

    std::string resources_dir_;
    std::string web_json_path_;
    std::string local_json_path_;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::vector<GrenadeLineup>> map_lineups_;

    std::thread download_thread_;
    std::atomic<bool> is_downloading_{false};
};
