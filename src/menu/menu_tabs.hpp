#pragma once

struct GLFWwindow;
struct OverlayConfig;

// Internal per-feature render functions
void render_chams_tab(OverlayConfig& cfg);
void render_esp_tab(OverlayConfig& cfg);
void render_trajectories_tab(OverlayConfig& cfg);
void render_grenade_helper_tab(OverlayConfig& cfg);
void render_vpk_tab(OverlayConfig& cfg);
void render_overlay_tab(OverlayConfig& cfg, GLFWwindow* overlay_window, GLFWwindow* menu_window);
void render_gamereader_tab(OverlayConfig& cfg, const class GameReader& reader);

// Composite tab render functions (called from menu_client)
void render_visuals_tab(OverlayConfig& cfg);
void render_world_tab(OverlayConfig& cfg);
void render_grenades_tab(OverlayConfig& cfg);
void render_combat_tab(OverlayConfig& cfg, const class SvgCache& svg_cache);
void render_misc_tab(OverlayConfig& cfg);
void render_config_tab(OverlayConfig& cfg);
void render_radar_tab(OverlayConfig& cfg);
void render_debug_tab(OverlayConfig& cfg, class OverlayClient* overlay, GLFWwindow* menu_window, const class GameReader& reader);

// Section helpers shared with menu_client
void set_tab_content_transition(float phase, bool entering, bool active);
void BeginSection(const char* title, const char* id);
void EndSection();
