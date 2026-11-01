#pragma once
#include "renderer/gl_loader.hpp"
#include "config.hpp"
#include "display_scaling.hpp"
#include "menu_search.hpp"

#include <string>
#include <vector>

class GameReader;
class SvgCache;
struct ImFont;

class MenuClient {
public:
    static MenuClient* g_instance;
    static MenuClient* get_instance() { return g_instance; }

    MenuClient(OverlayConfig& config, const GameReader& game_reader, const SvgCache& svg_cache,
               GLFWwindow* share_window = nullptr);
    ~MenuClient();

    bool should_close() const;
    void render();
    // Ask the window manager to restore the menu to its default (scale-aware) size.
    void reset_window_size();

    GLFWwindow* get_window() const { return window; }
    void set_overlay(class OverlayClient* o) { overlay = o; }
    class OverlayClient* get_overlay() const { return overlay; }
    struct ImGuiContext* get_imgui_context() const { return imgui_context; }
    OverlayConfig& get_cfg() { return cfg; }

private:
    void init_window(GLFWwindow* share_window = nullptr);
    void init_imgui();
    void UpdateMenuScale(float new_scale);
    void render_ui();

    GLFWwindow* window = nullptr;
    class OverlayClient* overlay = nullptr;
    struct ImGuiContext* imgui_context = nullptr;
    OverlayConfig& cfg;
    const GameReader& game_reader;
    const SvgCache& svg_cache;
    ImFont* bold_font = nullptr;
    float applied_menu_scale = -1.0f;
    // Preferred logical window size, applied at creation and on scale changes.
    // Once the window exists the window manager owns the actual size.
    int width = 820;
    int height = 620;

public:
    // Keybind assignment listening state
    KeyBind* listening_keybind = nullptr;
    bool is_listening_keybind() const { return listening_keybind != nullptr; }
    void start_listening(KeyBind* kb) { listening_keybind = kb; }
    void stop_listening() { listening_keybind = nullptr; }
    void set_listened_key(int key);

    void toggle_search();
    bool is_search_section_target(const char* section_id) const;
    bool is_search_section_highlighted(const char* section_id) const;
    bool is_search_setting_highlighted(const char* setting_name) const;
    void clear_search_section_target();

    // Location Creator state
    char creator_name[64] = "";
    float creator_mins[3] = { 0.0f, 0.0f, 0.0f };
    float creator_maxs[3] = { 0.0f, 0.0f, 0.0f };
    int creator_grenade_type = 2; // Default to Smoke (index 2)

    // Grenade Creator state
    bool gcreator_active = false;
    char gcreator_map[64] = "";
    char gcreator_title[64] = "";
    char gcreator_desc[128] = "";
    int gcreator_weapon_idx = 2; // Default to Smoke
    float gcreator_pos[3] = { 0.0f, 0.0f, 0.0f };
    float gcreator_angles[3] = { 0.0f, 0.0f, 0.0f };
    int gcreator_throw_type_idx = 0; // left, right, middle, none
    bool gcreator_crouch = false;
    bool gcreator_shift = false;
    char gcreator_movement_key[8] = "";
    bool gcreator_jump = false;
    int gcreator_movement_ticks = 0;

private:
    void close_search();
    void render_search_palette();
    void select_search_result(const MenuSearchEntry& entry);

    bool search_open = false;
    bool search_focus_requested = false;
    int search_selection = 0;
    int pending_search_tab = -1;
    char search_query[128] = "";
    bool search_results_dirty = true;
    std::vector<MenuSearchMatch> search_matches;
    std::string pending_search_section;
    std::string search_highlight_section;
    std::string search_highlight_setting;
    double search_highlight_until = 0.0;
};
