#include "LandingZoneManager.hpp"
#include "logger.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <algorithm>

using json = nlohmann::json;

void LandingZoneManager::initialize(const std::string& resources_dir) {
    resources_dir_ = resources_dir;
    json_path_ = resources_dir_ + "/landing_zones.json";
    
    std::filesystem::create_directories(resources_dir_);
    
    if (!std::filesystem::exists(json_path_)) {
        std::ofstream outfile(json_path_);
        outfile << "{}" << std::endl;
        outfile.close();
    }
    
    load_all();
}

void LandingZoneManager::add_point(const glm::vec3& world_point) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_points_.size() < 3) {
        pending_points_.push_back(world_point);
    }
}

int LandingZoneManager::pending_point_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int>(pending_points_.size());
}

static std::string clean_map_name(const std::string& map_name) {
    std::string clean = map_name;
    if (clean.find("maps/") == 0) {
        clean = clean.substr(5);
    }
    size_t dot_pos = clean.find(".vpk");
    if (dot_pos != std::string::npos) {
        clean = clean.substr(0, dot_pos);
    }
    return clean;
}

void LandingZoneManager::finalize_zone(const std::string& map_name, const std::string& name, uint8_t type) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_points_.size() == 3) {
        LandingZone zone;
        zone.name = name;
        zone.grenade_type = type;
        
        // Compute AABB
        zone.mins.x = std::min({pending_points_[0].x, pending_points_[1].x, pending_points_[2].x});
        zone.mins.y = std::min({pending_points_[0].y, pending_points_[1].y, pending_points_[2].y});
        zone.mins.z = std::min({pending_points_[0].z, pending_points_[1].z, pending_points_[2].z});
        
        zone.maxs.x = std::max({pending_points_[0].x, pending_points_[1].x, pending_points_[2].x});
        zone.maxs.y = std::max({pending_points_[0].y, pending_points_[1].y, pending_points_[2].y});
        zone.maxs.z = std::max({pending_points_[0].z, pending_points_[1].z, pending_points_[2].z});
        
        map_zones_[clean_map_name(map_name)].push_back(zone);
        pending_points_.clear();
    }
}

void LandingZoneManager::add_zone(const std::string& map_name, const LandingZone& zone) {
    std::lock_guard<std::mutex> lock(mutex_);
    map_zones_[clean_map_name(map_name)].push_back(zone);
}

void LandingZoneManager::cancel_creation() {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_points_.clear();
}

std::vector<glm::vec3> LandingZoneManager::get_pending_points() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_points_;
}

std::vector<LandingZone> LandingZoneManager::get_zones(const std::string& map_name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = map_zones_.find(clean_map_name(map_name));
    if (it != map_zones_.end()) {
        return it->second;
    }
    return {};
}

void LandingZoneManager::delete_zone(const std::string& map_name, int index) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = map_zones_.find(clean_map_name(map_name));
    if (it != map_zones_.end()) {
        if (index >= 0 && static_cast<size_t>(index) < it->second.size()) {
            it->second.erase(it->second.begin() + index);
        }
    }
}

void LandingZoneManager::load_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    map_zones_.clear();
    
    if (!std::filesystem::exists(json_path_)) return;

    std::ifstream file(json_path_);
    if (!file.is_open()) return;

    json j;
    try {
        file >> j;
    } catch (const std::exception& e) {
        BUNNYWARE_LOG_ERROR("Failed to parse JSON file {}: {}", json_path_, e.what());
        return;
    }

    for (auto& [map_name, zone_list] : j.items()) {
        if (!zone_list.is_array()) continue;

        for (auto& zone_data : zone_list) {
            LandingZone zone;
            
            if (zone_data.contains("name") && zone_data["name"].is_string()) {
                zone.name = zone_data["name"].get<std::string>();
            }
            if (zone_data.contains("grenade_type") && zone_data["grenade_type"].is_number()) {
                zone.grenade_type = zone_data["grenade_type"].get<uint8_t>();
            } else {
                zone.grenade_type = 0;
            }
            
            if (zone_data.contains("mins") && zone_data["mins"].is_array() && zone_data["mins"].size() >= 3) {
                zone.mins = glm::vec3(zone_data["mins"][0].get<float>(), zone_data["mins"][1].get<float>(), zone_data["mins"][2].get<float>());
            }
            if (zone_data.contains("maxs") && zone_data["maxs"].is_array() && zone_data["maxs"].size() >= 3) {
                zone.maxs = glm::vec3(zone_data["maxs"][0].get<float>(), zone_data["maxs"][1].get<float>(), zone_data["maxs"][2].get<float>());
            }
            
            map_zones_[map_name].push_back(zone);
        }
    }
}

void LandingZoneManager::save() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    json j;
    for (const auto& [map_name, zones] : map_zones_) {
        json zone_array = json::array();
        for (const auto& zone : zones) {
            json zone_obj;
            zone_obj["name"] = zone.name;
            zone_obj["grenade_type"] = zone.grenade_type;
            zone_obj["mins"] = {zone.mins.x, zone.mins.y, zone.mins.z};
            zone_obj["maxs"] = {zone.maxs.x, zone.maxs.y, zone.maxs.z};
            zone_array.push_back(zone_obj);
        }
        j[map_name] = zone_array;
    }
    
    std::ofstream file(json_path_);
    if (file.is_open()) {
        file << j.dump(4) << std::endl;
        file.close();
    } else {
        BUNNYWARE_LOG_ERROR("Failed to write to JSON file {}", json_path_);
    }
}
