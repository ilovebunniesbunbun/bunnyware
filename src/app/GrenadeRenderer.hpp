#pragma once

#include "app/GrenadeTracker.hpp"
#include "renderer/depth_prepass.hpp"
#include "renderer/esp_renderer.hpp"
#include "overlay/overlay_client.hpp"
#include "config.hpp"
#include "app/SvgCache.hpp"

struct PlantedC4;

class GrenadeRenderer {
public:
    void render(GrenadeTracker& tracker, EspRenderer& esp_renderer,
                const VischeckResult& render_result, const float* gl_vp, const float* view_matrix,
                const OverlayConfig& cfg, OverlayClient* overlay, const SvgCache& svg_cache,
                const PlantedC4& planted_c4, const Vec3& viewer_position);

    // Draws the smoke cage and the through-smokes map wireframe. Called before the
    // player passes so chams, the skeleton and the ESP overlays render over them.
    // Returns true when the map depth inside the smoke silhouette was rewritten and
    // the caller must restore it with another depth prepass.
    bool render_smoke_visuals(const VischeckResult& render_result, EspRenderer& esp_renderer,
                              DepthPrepassRenderer& depth_prepass, const float* gl_vp, const OverlayConfig& cfg,
                              OverlayClient* overlay);

private:
    bool was_held_trajectory_visible = false;
    uint8_t last_held_grenade_type = 0;
};
