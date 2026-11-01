#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <glm/glm.hpp>
#include <cstdint>

struct LandingZone {
    std::string name;
    glm::vec3 mins;
    glm::vec3 maxs;
    uint8_t grenade_type; // e.g., GRENADE_SMOKE, GRENADE_FLASH, etc.
};

class LandingZoneManager {
public:
    void initialize(const std::string& resources_dir);

    // --- Zone creation flow ---
    void add_point(const glm::vec3& world_point);
    int pending_point_count() const;
    void finalize_zone(const std::string& map_name, const std::string& name, uint8_t type);
    void add_zone(const std::string& map_name, const LandingZone& zone);
    void cancel_creation();
    
    // For previewing the points currently being placed
    std::vector<glm::vec3> get_pending_points() const;

    // --- Zone management ---
    std::vector<LandingZone> get_zones(const std::string& map_name) const;
    void delete_zone(const std::string& map_name, int index);
    
    // Load/Save
    void load_all();
    void save();

private:
    std::string resources_dir_;
    std::string json_path_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::vector<LandingZone>> map_zones_;

    std::vector<glm::vec3> pending_points_;
};
