#pragma once
#include "GrenadeHelperData.hpp"
#include "config.hpp"
#include "overlay/overlay_client.hpp"
#include "app/SvgCache.hpp"
#include "VisibilityWorker.hpp"
#include "LandingZoneManager.hpp"

class GrenadeHelperRenderer {
public:
    void render(const std::vector<GrenadeLineup>& lineups, 
                const VischeckResult& vis_result,
                LandingZoneManager& zone_manager,
                const std::string& current_map,
                const Vec3& local_pos,
                const Vec3& local_eye,
                const Vec3& local_angles,
                uint16_t local_weapon_id,
                const float* view_matrix,
                const OverlayConfig& cfg,
                OverlayClient* overlay,
                const SvgCache& svg_cache,
                const VisibilityWorker& vis_worker);
};
