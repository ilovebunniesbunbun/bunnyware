#include "App.hpp"
#include "TriggerBot.hpp"
#include "external/imgui/imgui.h"
#include "smoke_wireframe.hpp"
#include "overlay/evdev_input.hpp"

#include <cmath>
#include <cstdio>
#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <thread>

#include <limits.h>
#include <unistd.h>

#include "overlay/esp_drawing.hpp"
#include "logger.hpp"
#include "math/BoneMath.hpp"
#include "math/Distance.hpp"
#include "math/ViewMatrix.hpp"
#include "renderer/Passes.hpp"
#include "renderer/HighlightTint.hpp"
#include "menu/menu_theme.hpp"

class DrawListTint {
public:
    DrawListTint(ImDrawList* list, float alpha, const float* tint, bool active)
        : list_(active ? list : nullptr), start_(list_ ? list_->VtxBuffer.Size : 0), alpha_(alpha), tint_(tint)
    {
    }
    ~DrawListTint()
    {
        if (!list_) return;
        for (int v = start_; v < list_->VtxBuffer.Size; ++v) {
            auto& vertex = list_->VtxBuffer[v];
            ImVec4 c = ImGui::ColorConvertU32ToFloat4(vertex.col);
            float color[4] = {c.x, c.y, c.z, c.w};
            tint_highlight(color, alpha_, tint_);
            vertex.col = ImGui::ColorConvertFloat4ToU32(ImVec4(color[0], color[1], color[2], color[3]));
        }
    }
    DrawListTint(const DrawListTint&) = delete;
    DrawListTint& operator=(const DrawListTint&) = delete;

private:
    ImDrawList* list_;
    int start_;
    float alpha_;
    const float* tint_;
};

static float get_player_ndc_z(const Vec3& world, const float* matrix) {
    float w = matrix[12] * world.x + matrix[13] * world.y + matrix[14] * world.z + matrix[15];
    if (w < 0.001f) return 1.0f;
    float z = matrix[8] * world.x + matrix[9] * world.y + matrix[10] * world.z + matrix[11];
    return z / w;
}

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

    void append_smoke_voxel_triangles(
        std::vector<MapParser::Triangle>& triangles,
        const glm::vec3& center,
        float size,
        float type_id)
    {
        float h = size / 2.0f;
        
        MapParser::Vertex v000 = { { center.x - h, center.y - h, center.z - h }, type_id };
        MapParser::Vertex v100 = { { center.x + h, center.y - h, center.z - h }, type_id };
        MapParser::Vertex v010 = { { center.x - h, center.y + h, center.z - h }, type_id };
        MapParser::Vertex v110 = { { center.x + h, center.y + h, center.z - h }, type_id };
        MapParser::Vertex v001 = { { center.x - h, center.y - h, center.z + h }, type_id };
        MapParser::Vertex v101 = { { center.x + h, center.y - h, center.z + h }, type_id };
        MapParser::Vertex v011 = { { center.x - h, center.y + h, center.z + h }, type_id };
        MapParser::Vertex v111 = { { center.x + h, center.y + h, center.z + h }, type_id };

        // Front (z = +h)
        triangles.push_back({ v001, v101, v111 });
        triangles.push_back({ v001, v111, v011 });

        // Back (z = -h)
        triangles.push_back({ v100, v000, v010 });
        triangles.push_back({ v100, v010, v110 });

        // Left (x = -h)
        triangles.push_back({ v000, v001, v011 });
        triangles.push_back({ v000, v011, v010 });

        // Right (x = +h)
        triangles.push_back({ v101, v100, v110 });
        triangles.push_back({ v101, v110, v111 });

        // Top (y = +h)
        triangles.push_back({ v011, v111, v110 });
        triangles.push_back({ v011, v110, v010 });

        // Bottom (y = -h)
        triangles.push_back({ v000, v100, v101 });
        triangles.push_back({ v000, v101, v001 });
    }
}

App* App::g_instance = nullptr;

App::App(const OverlayConfig& initial_cfg) : 
    cfg(initial_cfg), 
    last_debug_print(std::chrono::steady_clock::now()),
    bomb_bar_display_ratio(1.0f),
    bomb_bar_fade_alpha(0.0f),
    bomb_bar_fade_time_left(0.0f),
    bomb_bar_fade_c4{},
    previous_total_damage(-1.0f),
    previous_total_kills(-1),
    previous_total_kills_headshots(-1),
    hitmarker_time(0.0),
    kill_time(0.0),
    kill_was_headshot(false),
    killed_player_name(""),
    spread_circle_smoothed_radius(0.0f),
    spread_circle_smoothed_base_radius(0.0f)
{
    g_instance = this;
}

App::~App()
{
    set_game_activity_allowed(false);
    g_instance = nullptr;
    radar_server.stop();
    radar::set_active_server(nullptr);
    visibility_worker.stop();
    esp_renderer.cleanup();
    game_reader.shutdown();
}

void App::make_context_current(GLFWwindow* target)
{
    if (overlay && overlay->is_wayland()) {
        if (target == nullptr || target == overlay->get_window()) {
            overlay->make_current();
            current_context = nullptr; // Sentinel for overlay context
            return;
        }
    }
    if (current_context != target) {
        glfwMakeContextCurrent(target);
        current_context = target;
    }
}

static void glfw_error_callback(int error, const char* description) {
    BUNNYWARE_LOG_ERROR("GLFW Error ({}): {}", error, description);
}

bool App::initialize_system()
{
    // Detect if running under a Wayland session (--x11 forces the X11 backend)
    bool use_wayland = false;
    if (!cfg.force_x11) {
        const char* xdg_session = std::getenv("XDG_SESSION_TYPE");
        const char* wayland_disp = std::getenv("WAYLAND_DISPLAY");
        if ((xdg_session && std::strcmp(xdg_session, "wayland") == 0) || wayland_disp) {
            use_wayland = true;
        }
    }

    glfwSetErrorCallback(glfw_error_callback);

    // Initialize GLFW early to discover monitors
    if (use_wayland) {
        glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_WAYLAND);
    } else {
        glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
    }
    if (!glfwInit()) {
        BUNNYWARE_LOG_ERROR("Failed to initialize GLFW for monitor discovery.");
        return false;
    }

    AgentParser::SetCustomVpkPath(cfg.vpk_path);
    BUNNYWARE_LOG_INFO(
        "Loaded config: monitor={}x{} game={}x{} scaling={} fps={} show_fps={} monitor_index={} gpu_preference={}",
        cfg.monitor_w, cfg.monitor_h, cfg.game_w, cfg.game_h, cfg.scaling, cfg.fps, cfg.show_fps ? "true" : "false",
        cfg.monitor_index, cfg.gpu_preference);

    int monitor_w = cfg.monitor_w;
    int monitor_h = cfg.monitor_h;
    int offset_x = 0;
    int offset_y = 0;

    int monitor_count = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&monitor_count);
    if (monitors && monitor_count > 0) {
        int target_idx = cfg.monitor_index;
        if (target_idx < 0 || target_idx >= monitor_count) {
            target_idx = 0;
        }
        GLFWmonitor* monitor = monitors[target_idx];
        glfwGetMonitorPos(monitor, &offset_x, &offset_y);
        const GLFWvidmode* mode = glfwGetVideoMode(monitor);
        if (mode) {
            monitor_w = mode->width;
            monitor_h = mode->height;
        }
        BUNNYWARE_LOG_INFO("Target monitor [{}] ({}) size: {}x{} pos: ({}, {})", target_idx, glfwGetMonitorName(monitor),
                     monitor_w, monitor_h, offset_x, offset_y);
    }

    window_width = monitor_w;
    window_height = monitor_h;
    window_x = offset_x;
    window_y = offset_y;

    if (cfg.scaling == "centered") {
        float monitor_aspect = (float)monitor_w / monitor_h;
        float game_aspect = (float)cfg.game_w / cfg.game_h;
        if (game_aspect < monitor_aspect) {
            window_height = monitor_h;
            window_width = (int)(monitor_h * game_aspect);
            window_x = offset_x + (monitor_w - window_width) / 2;
            window_y = offset_y;
        } else {
            window_width = monitor_w;
            window_height = (int)(monitor_w / game_aspect);
            window_x = offset_x;
            window_y = offset_y + (monitor_h - window_height) / 2;
        }
    } else if (cfg.scaling == "custom") {
        window_width = cfg.game_w;
        window_height = cfg.game_h;
        window_x = offset_x + cfg.game_x;
        window_y = offset_y + cfg.game_y;
    } else {
        // stretched
        window_width = monitor_w;
        window_height = monitor_h;
        window_x = offset_x;
        window_y = offset_y;
    }

    if (window_width == monitor_w && window_height == monitor_h) {
        window_height -= 1;
    }

    BUNNYWARE_LOG_INFO("Window geometry: {}x{} at ({}, {})", window_width, window_height, window_x, window_y);

    overlay = std::make_unique<OverlayClient>(window_width, window_height, window_x, window_y, cfg.hyprland_support, cfg.monitor_index, cfg.force_x11, cfg.scaling, cfg.game_w, cfg.game_h, cfg.game_x, cfg.game_y);

    // Initialize GLEW to load modern OpenGL function pointers
    glewExperimental = GL_TRUE;
    GLenum err = glewInit();
    if (err != GLEW_OK) {
        if (use_wayland) {
            BUNNYWARE_LOG_WARN("GLEW initialization returned code {} inside Wayland context (safe to ignore).", (int)err);
        } else {
            BUNNYWARE_LOG_ERROR("Failed to initialize GLEW: {}", reinterpret_cast<const char*>(glewGetErrorString(err)));
            return false;
        }
    }
    BUNNYWARE_LOG_INFO("Loaded modern OpenGL core functions.");

    menu = std::make_unique<MenuClient>(cfg, game_reader, svg_cache, overlay ? overlay->get_window() : nullptr);
    menu->set_overlay(overlay.get());

    // Make overlay context current again so shaders and buffers are compiled on the overlay window context
    make_context_current(overlay->get_window());

    // Apply initial VSync setting from config
    overlay->set_vsync(cfg.vsync);

    if (!chams_renderer.init()) {
        BUNNYWARE_LOG_ERROR("Failed to compile and link chams shader programs.");
        return false;
    }

    if (!depth_prepass.init()) {
        BUNNYWARE_LOG_ERROR("Failed to initialize depth prepass shaders.");
        return false;
    }

    if (!esp_renderer.init()) {
        BUNNYWARE_LOG_ERROR("Failed to initialize ESP renderer.");
        return false;
    }

    if (!game_reader.initialize(&cfg)) {
        BUNNYWARE_LOG_ERROR("Failed to initialize game reader. Ensure cs2-linux output offsets/schemas are present.");
        return false;
    }
    BUNNYWARE_LOG_INFO("Shared memory bridge communication active.");

    svg_cache.initialize("assets");
    grenade_helper_data.initialize(get_config_path("assets/grenades/fgrenade_helper"));
    landing_zone_manager.initialize(get_config_path("assets/grenades/landing_zones"));

    visibility_worker.start();
    focus_monitor = std::make_unique<FocusMonitor>(overlay->is_wayland());
    set_game_activity_allowed(!cfg.focus_check_enabled);

    radar::set_active_server(&radar_server);
    if (cfg.radar_enabled && cfg.radar_autostart) {
        start_radar();
    }
    return true;
}

int App::run()
{
    if (!initialize_system()) {
        return 1;
    }

    while (!quit_requested && !overlay->should_close() && !menu->should_close()) {
        process_frame();
    }

    return 0;
}

static int get_keybind_key(const OverlayConfig& cfg, const std::string& setting_name, int default_key)
{
    for (const auto& kb : cfg.keybinds) {
        if (kb.setting_name == setting_name) {
            return kb.key;
        }
    }
    return default_key;
}

void App::start_input_modules()
{
    if (!trigger_bot) trigger_bot = std::make_unique<TriggerBot>();
    if (!edge_jump) edge_jump = std::make_unique<EdgeJump>();
    if (!legit_aim) legit_aim = std::make_unique<LegitAim>();
    if (!grenade_helper) grenade_helper = std::make_unique<GrenadeHelper>(cfg);
    if (!cstrafe_trainer) cstrafe_trainer = std::make_unique<CStrafeTrainer>(cfg);
}

void App::stop_input_modules()
{
    auto_accept.suspend_input();
    grenade_helper.reset();
    legit_aim.reset();
    edge_jump.reset();
    trigger_bot.reset();
    cstrafe_trainer.reset();

    rec_pin_pulled_last_ = false;
    rec_pin_pull_time_ = 0.0;
    rec_movement_start_time_ = 0.0;
    rec_movement_key_.clear();
}

void App::set_game_activity_allowed(bool allowed)
{
    if (allowed == game_activity_allowed_) return;

    EvdevInput::set_input_enabled(allowed);
    if (allowed) {
        start_input_modules();
    } else {
        stop_input_modules();
    }
    game_activity_allowed_ = allowed;
    BUNNYWARE_LOG_INFO("CS2 focus gate: overlay and game input {}.",
                     allowed ? "enabled" : "suspended");
}

void App::process_frame()
{
    scheduler.begin_frame();
    gpu_profiler.begin_frame(cfg.debug_gpu_profiling);

    // Ensure the overlay context is current for all uploads and rendering
    make_context_current(overlay->get_window());

    // Route GLFW event polling to the menu context so clicks register immediately
    if (menu && menu->get_imgui_context()) {
        ImGui::SetCurrentContext(menu->get_imgui_context());
    }
    overlay->poll_events();
    if (overlay->should_close()) {
        gpu_profiler.end_frame();
        return;
    }

    // Sync overlay dimensions so coordinates stay correct after overlay framebuffer changes.
    window_width = overlay->get_width();
    window_height = overlay->get_height();

    if (overlay && overlay->get_imgui_context()) {
        ImGui::SetCurrentContext(overlay->get_imgui_context());
    }

    // Handle config updates before the focus gate so the debug setting can take
    // effect while the game is not active.
    if (cfg.is_dirty()) {
        overlay->set_vsync(cfg.vsync);
        cfg.clear_dirty();
    }

    // Radar start/stop requests and settings are processed even while the game
    // is unfocused so the menu controls stay responsive.
    process_radar();

    const bool cs2_focused = focus_monitor && focus_monitor->is_cs2_focused();
    set_game_activity_allowed(!cfg.focus_check_enabled || cs2_focused);

    if (!game_activity_allowed_) {
        if (!overlay_hidden_) {
            overlay->begin_frame();
            overlay->end_frame();
            overlay_hidden_ = true;
        }
        // Keep the web radar fed while CS2 is unfocused so it can be watched in
        // a browser on another display. fetch_latest() only copies when the
        // reader thread has produced a fresh packet.
        if (radar_running_) {
            game_reader.fetch_latest(last_input.packet);
            update_radar_snapshot();
        }
        render_menu();
        scheduler.pace_frame(cfg.fps);
        gpu_profiler.end_frame();
        return;
    }
    overlay_hidden_ = false;

    // Snapshot global input device states once per frame; every is_key_pressed()
    // query below (keybinds, edge jump, grenade helper, viewers) reads from it,
    // reducing ~200 EVIOCGKEY ioctls per frame to one per input device.
    global_input.begin_frame();

    if (g_menu_preferences.quit_shortcut_enabled && global_input.is_key_pressed(GLFW_KEY_Q) &&
        (global_input.is_key_pressed(GLFW_KEY_LEFT_CONTROL) || global_input.is_key_pressed(GLFW_KEY_RIGHT_CONTROL))) {
        quit_requested = true;
        return;
    }

    process_keybinds();

    update_map_loading();

    glm::vec3 traced_pt;
    if (visibility_worker.get_traced_point(traced_pt)) {
        if (landing_zone_manager.pending_point_count() < 3) {
            landing_zone_manager.add_point(traced_pt);
            
            MenuClient* menu_cl = MenuClient::get_instance();
            if (menu_cl) {
                auto pts = landing_zone_manager.get_pending_points();
                if (pts.size() == 1) {
                    menu_cl->creator_mins[0] = pts[0].x;
                    menu_cl->creator_mins[1] = pts[0].y;
                    menu_cl->creator_mins[2] = pts[0].z;
                    
                    menu_cl->creator_maxs[0] = pts[0].x;
                    menu_cl->creator_maxs[1] = pts[0].y;
                    menu_cl->creator_maxs[2] = pts[0].z;
                } else if (pts.size() == 2) {
                    menu_cl->creator_mins[0] = std::min(pts[0].x, pts[1].x);
                    menu_cl->creator_mins[1] = std::min(pts[0].y, pts[1].y);
                    menu_cl->creator_mins[2] = pts[0].z;
                    
                    menu_cl->creator_maxs[0] = std::max(pts[0].x, pts[1].x);
                    menu_cl->creator_maxs[1] = std::max(pts[0].y, pts[1].y);
                    menu_cl->creator_maxs[2] = pts[0].z;
                } else if (pts.size() == 3) {
                    menu_cl->creator_mins[0] = std::min({pts[0].x, pts[1].x, pts[2].x});
                    menu_cl->creator_mins[1] = std::min({pts[0].y, pts[1].y, pts[2].y});
                    menu_cl->creator_mins[2] = std::min({pts[0].z, pts[1].z, pts[2].z});
                    
                    menu_cl->creator_maxs[0] = std::max({pts[0].x, pts[1].x, pts[2].x});
                    menu_cl->creator_maxs[1] = std::max({pts[0].y, pts[1].y, pts[2].y});
                    menu_cl->creator_maxs[2] = std::max({pts[0].z, pts[1].z, pts[2].z});
                    
                    snprintf(menu_cl->creator_name, sizeof(menu_cl->creator_name), "New Zone %d", (int)landing_zone_manager.get_zones(current_map).size() + 1);
                    
                    menu_cl->creator_grenade_type = 2; // Default to Smoke (index 2)
                    if (last_input.packet.held_weapon_id >= 43 && last_input.packet.held_weapon_id <= 48) {
                        switch (last_input.packet.held_weapon_id) {
                            case 44: menu_cl->creator_grenade_type = 0; break; // HE (index 0)
                            case 43: menu_cl->creator_grenade_type = 1; break; // Flash (index 1)
                            case 45: menu_cl->creator_grenade_type = 2; break; // Smoke (index 2)
                            case 46: case 48: menu_cl->creator_grenade_type = 3; break; // Molotov (index 3)
                            case 47: menu_cl->creator_grenade_type = 4; break; // Decoy (index 4)
                        }
                    }
                }
            }
        }
    }

    prepare_frame_input(last_input);

    update_radar_snapshot();

    if (trigger_bot) {
        trigger_bot->update(last_input, cfg, global_input, last_vischeck_result, visibility_worker);
    }

    if (edge_jump) {
        edge_jump->update(last_input, cfg, global_input, visibility_worker);
    }

    auto_accept.update(cfg, game_reader, last_input, window_x, window_y, window_width, window_height);

    if (grenade_helper) {
        std::string clean_map = current_map;
        if (clean_map.find("maps/") == 0) clean_map = clean_map.substr(5);
        if (clean_map.find(".vpk") != std::string::npos) clean_map = clean_map.substr(0, clean_map.find(".vpk"));
        
        std::vector<GrenadeLineup> current_lineups = grenade_helper_data.get_lineups(clean_map);
        if (cfg.grenade_creator_enabled && menu) {
            if (menu->gcreator_pos[0] != 0.0f || menu->gcreator_pos[1] != 0.0f || menu->gcreator_pos[2] != 0.0f) {
                GrenadeLineup temp_lineup;
                temp_lineup.title = std::string(menu->gcreator_title).empty() ? "Creator Preview" : menu->gcreator_title;
                temp_lineup.description = std::string(menu->gcreator_desc).empty() ? "Unsaved Lineup" : menu->gcreator_desc;
                temp_lineup.weapon = static_cast<uint8_t>(menu->gcreator_weapon_idx + 1);
                temp_lineup.position = Vec3(menu->gcreator_pos[0], menu->gcreator_pos[1], menu->gcreator_pos[2]);
                temp_lineup.viewangles = Vec3(menu->gcreator_angles[0], menu->gcreator_angles[1], 0.0f);

                const char* throw_methods[] = { "left", "right", "middle", "none" };
                temp_lineup.movement.throw_type = throw_methods[menu->gcreator_throw_type_idx];
                if (menu->gcreator_crouch) temp_lineup.movement.modifiers.push_back("crouch");
                if (menu->gcreator_shift) temp_lineup.movement.modifiers.push_back("shift");
                
                std::string mkey = menu->gcreator_movement_key;
                std::transform(mkey.begin(), mkey.end(), mkey.begin(), [](unsigned char c) { return std::tolower(c); });
                temp_lineup.movement.movement_key = mkey;
                temp_lineup.movement.jump = menu->gcreator_jump;
                temp_lineup.movement.movement_ticks = menu->gcreator_movement_ticks;

                std::vector<std::string> parts;
                if (temp_lineup.movement.throw_type != "none") parts.push_back(temp_lineup.movement.throw_type);
                for (const auto& mod : temp_lineup.movement.modifiers) parts.push_back(mod);
                if (!temp_lineup.movement.movement_key.empty()) parts.push_back(temp_lineup.movement.movement_key);
                if (temp_lineup.movement.jump) parts.push_back("jump");
                if (temp_lineup.movement.movement_ticks > 0) {
                    parts.push_back(std::to_string(temp_lineup.movement.movement_ticks) + " ticks");
                }
                if (parts.empty()) {
                    temp_lineup.throw_method = "manual";
                } else {
                    std::string res;
                    for (size_t i = 0; i < parts.size(); ++i) {
                        if (i > 0) res += " + ";
                        res += parts[i];
                    }
                    temp_lineup.throw_method = res;
                }
                current_lineups.push_back(temp_lineup);
            }
        }
        grenade_helper->update(last_input, cfg, global_input, current_lineups);
    }

    // Automatic Grenade Creator Movement Recording
    if (cfg.grenade_creator_enabled && menu) {
        bool helper_key_down = false;
        for (const auto& kb : cfg.keybinds) {
            if (kb.setting_name == "grenade_helper_active") {
                helper_key_down = global_input.is_key_pressed(kb.key);
                break;
            }
        }

        if (helper_key_down) {
            // Cancel and bypass recording when testing via automation key
            rec_pin_pull_time_ = 0.0;
            rec_movement_start_time_ = 0.0;
            rec_movement_key_ = "";
            rec_pin_pulled_last_ = false;
        } else {
            bool pin_pulled = (last_input.packet.pin_pulled == 1);
            uint8_t held_grenade_type = last_input.packet.held_grenade_type;

            if (pin_pulled && !rec_pin_pulled_last_ && held_grenade_type != GRENADE_NONE) {
                // Pin pulled manually - start recording
                rec_pin_pull_time_ = glfwGetTime();
                rec_start_pos_ = last_input.packet.local_origin;
                rec_start_angles_ = last_input.packet.local_angles;
                rec_grenade_type_ = held_grenade_type;
                rec_crouch_detected_ = false;
                rec_shift_detected_ = false;
                rec_movement_key_ = "";
                rec_movement_start_time_ = 0.0;
                rec_throw_type_idx_ = 0; // Default to left throw

                // Pre-populate starting position/angles and grenade type in UI immediately on pull
                menu->gcreator_pos[0] = rec_start_pos_.x;
                menu->gcreator_pos[1] = rec_start_pos_.y;
                menu->gcreator_pos[2] = rec_start_pos_.z;
                menu->gcreator_angles[0] = rec_start_angles_.x;
                menu->gcreator_angles[1] = rec_start_angles_.y;
                if (rec_grenade_type_ >= 1 && rec_grenade_type_ <= 5) {
                    menu->gcreator_weapon_idx = rec_grenade_type_ - 1;
                }

                // Pre-populate map name
                std::string map_name = current_map;
                if (map_name.find("maps/") == 0) map_name = map_name.substr(5);
                if (map_name.find(".vpk") != std::string::npos) map_name = map_name.substr(0, map_name.find(".vpk"));
                std::strncpy(menu->gcreator_map, map_name.c_str(), sizeof(menu->gcreator_map));
            }

            if (rec_pin_pull_time_ > 0.0 && pin_pulled) {
                // Track modifiers (crouch and sneak/shift)
                bool in_game_crouch = (last_input.packet.local_flags & (1 << 1)) != 0;
                if (in_game_crouch || global_input.is_key_pressed(get_keybind_key(cfg, "key_crouch", 341))) {
                    rec_crouch_detected_ = true;
                }

                if (global_input.is_key_pressed(get_keybind_key(cfg, "key_shift", 340))) {
                    rec_shift_detected_ = true;
                }

                // Track throw type during the hold using both throw strength and physical mouse clicks
                float ts = last_input.packet.throw_strength;
                if (std::abs(ts - 0.5f) < 0.08f) {
                    rec_throw_type_idx_ = 2; // middle
                } else if (std::abs(ts - 1.0f) < 0.08f) {
                    rec_throw_type_idx_ = 0; // left
                } else if (std::abs(ts - 0.0f) < 0.08f) {
                    rec_throw_type_idx_ = 1; // right
                } else {
                    // Fallback to mouse buttons query (1000 = Left Mouse, 1001 = Right Mouse)
                    bool left_held = global_input.is_key_pressed(1000);
                    bool right_held = global_input.is_key_pressed(1001);
                    if (left_held && right_held) {
                        rec_throw_type_idx_ = 2; // middle
                    } else if (left_held) {
                        rec_throw_type_idx_ = 0; // left
                    } else if (right_held) {
                        rec_throw_type_idx_ = 1; // right
                    }
                }

                // Detect movement key using actual keybinds
                if (rec_movement_key_.empty()) {
                    std::string detected_move = "";
                    if (global_input.is_key_pressed(get_keybind_key(cfg, "key_w", 87))) detected_move = "w";
                    else if (global_input.is_key_pressed(get_keybind_key(cfg, "key_a", 65))) detected_move = "a";
                    else if (global_input.is_key_pressed(get_keybind_key(cfg, "key_s", 83))) detected_move = "s";
                    else if (global_input.is_key_pressed(get_keybind_key(cfg, "key_d", 68))) detected_move = "d";

                    if (!detected_move.empty()) {
                        rec_movement_key_ = detected_move;
                        rec_movement_start_time_ = glfwGetTime();
                    }
                }
            }

            if (rec_pin_pull_time_ > 0.0 && !pin_pulled) {
                // Pin released - check if thrown
                if (held_grenade_type == GRENADE_NONE && last_input.packet.held_weapon_id != 0) {
                    bool in_air = !(last_input.packet.local_flags & (1 << 0));
                    bool jump_pressed = global_input.is_key_pressed(get_keybind_key(cfg, "key_jump", 32)) || in_air;

                    int ticks = 0;
                    if (!rec_movement_key_.empty() && rec_movement_start_time_ > 0.0) {
                        double duration_ms = (glfwGetTime() - rec_movement_start_time_) * 1000.0;
                        ticks = std::max(1, (int)std::round(duration_ms / 15.625));
                    }

                    menu->gcreator_crouch = rec_crouch_detected_;
                    menu->gcreator_shift = rec_shift_detected_;
                    menu->gcreator_jump = jump_pressed;
                    menu->gcreator_throw_type_idx = rec_throw_type_idx_;
                    
                    std::strncpy(menu->gcreator_movement_key, rec_movement_key_.c_str(), sizeof(menu->gcreator_movement_key));
                    menu->gcreator_movement_ticks = ticks;
                    
                    snprintf(menu->gcreator_title, sizeof(menu->gcreator_title), "Recorded Lineup");
                    snprintf(menu->gcreator_desc, sizeof(menu->gcreator_desc), "Custom throw recorded");

                    BUNNYWARE_LOG_INFO("GrenadeCreator: Recorded throw - Move key: {}, Ticks: {}, Jump: {}, Crouch: {}, Shift: {}, ThrowType: {}", 
                                     rec_movement_key_, ticks, jump_pressed, rec_crouch_detected_, rec_shift_detected_, rec_throw_type_idx_);
                }

                rec_pin_pull_time_ = 0.0;
                rec_movement_start_time_ = 0.0;
                rec_movement_key_ = "";
            }

            rec_pin_pulled_last_ = pin_pulled;
        }
    }

    if (cstrafe_trainer) {
        cstrafe_trainer->update(last_input, cfg, global_input);
    }

    if (legit_aim) {
        const glm::vec3* override_angles = nullptr;
        if (grenade_helper && grenade_helper->is_aim_override_active()) {
            override_angles = &grenade_helper->get_override_angles();
        }
        legit_aim->update(last_input, cfg, last_vischeck_result, override_angles);
    }

    if (last_input.packet.player_count >= 0 && static_cast<size_t>(last_input.packet.player_count) <= shm::MAX_PLAYERS) {
        FrameState state;
        preprocess_frame_state(last_input, state);
        render_frame(last_input, state);
    } else {
        // Prevent window freeze if game is completely unavailable
        overlay->begin_frame();
        overlay->end_frame();
    }

    render_menu();

    // Frame pacing limiter. fps == 0 means "match the monitor refresh rate":
    // the ESP/chams are re-drawn once per display frame, so on 240/360 Hz panels
    // they no longer update only ~143x/s (which makes them slide off fast-moving
    // players). A positive fps is used verbatim.
    int pace_fps = cfg.fps;
    if (pace_fps == 0 && overlay) {
        const int hz = overlay->get_refresh_hz();
        if (hz > 0) pace_fps = hz;
    }
    scheduler.pace_frame(pace_fps);

    gpu_profiler.end_frame();
}

void App::update_map_loading()
{
    if (map_loading && map_load_future.valid() &&
        map_load_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        auto mesh = map_load_future.get();
        map_loading = false;
        if (mesh.Valid && !mesh.Triangles.empty()) {
            depth_prepass.upload_geometry(mesh.VisualTriangles);

            // Signal worker thread to reload map geometry for BVH checks
            visibility_worker.update_map(std::move(mesh), cfg);

            BUNNYWARE_LOG_INFO("Async map geometry loaded ({} triangles).", mesh.Triangles.size());
        } else {
            BUNNYWARE_LOG_INFO("Async map geometry unavailable, clearing GPU prepass.");
            depth_prepass.clear_geometry();
        }
    }
}

void App::prepare_frame_input(FrameInput& input)
{
    auto ipc_start = std::chrono::high_resolution_clock::now();
    bool has_new_packet = game_reader.fetch_latest(input.packet);
    auto ipc_end = std::chrono::high_resolution_clock::now();
    double current_ipc_ms = std::chrono::duration<double, std::milli>(ipc_end - ipc_start).count();

    if (has_new_packet) {
        packet_count_since_print++;
        visibility_worker.submit_work(input.packet, cfg);

        if (first_packet) {
            BUNNYWARE_LOG_INFO("Game reader active.");
            first_packet = false;
        }

        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_debug_print).count() >= 5) {
            double avg_ipc = metrics_count > 0 ? total_ipc_time_ms / metrics_count : 0.0;
            double avg_cpu = metrics_count > 0 ? total_cpu_time_ms / metrics_count : 0.0;
            double avg_gpu = metrics_count > 0 ? total_gpu_time_ms / metrics_count : 0.0;
            double avg_total = avg_ipc + avg_cpu + avg_gpu;

            if (cfg.debug_game_reader && !cfg.debug_gpu_profiling) {
                BUNNYWARE_LOG_INFO("GameReader active - received {} packets in last 5s (frame index: {}, players tracked: {})",
                             packet_count_since_print, input.packet.frame_index, input.packet.player_count);
                BUNNYWARE_LOG_INFO("            Avg / Max Timeline (frame processing latency):");
                BUNNYWARE_LOG_INFO("            - Read Time:       {:.3f} ms / {:.3f} ms", avg_ipc, max_ipc_time_ms);
                BUNNYWARE_LOG_INFO("            - CPU Math Time:   {:.3f} ms / {:.3f} ms", avg_cpu, max_cpu_time_ms);
                BUNNYWARE_LOG_INFO("            - GPU Render Time: {:.3f} ms / {:.3f} ms", avg_gpu, max_gpu_time_ms);
                BUNNYWARE_LOG_INFO("            - Total Pipeline:  {:.3f} ms", avg_total);
            } else if (cfg.debug_gpu_profiling) {
                gpu_profiler.print_and_reset(packet_count_since_print, input.packet.frame_index, input.packet.player_count, avg_ipc, max_ipc_time_ms, avg_cpu, max_cpu_time_ms, avg_total);
            }
            last_debug_print = now;
            packet_count_since_print = 0;
            total_ipc_time_ms = 0.0;
            total_cpu_time_ms = 0.0;
            total_gpu_time_ms = 0.0;
            max_ipc_time_ms = 0.0;
            max_cpu_time_ms = 0.0;
            max_gpu_time_ms = 0.0;
            metrics_count = 0;
        }

        // Map change handling
        std::string shm_map = input.packet.map_name;
        if (current_map != shm_map) {
            current_map = shm_map;
            if (!current_map.empty()) {
                BUNNYWARE_LOG_INFO("Map change detected: {}", current_map);
                visibility_worker.set_landing_zones(landing_zone_manager.get_zones(current_map));
                crosshair_distance.clear_pin();

                // Clear previous map geometries immediately
                depth_prepass.clear_geometry();

                map_loading = true;
                std::string map_name_copy = current_map;
                map_load_future =
                    std::async(std::launch::async, [map_name_copy]() { return MapParser::LoadMesh(map_name_copy); });
            } else {
                BUNNYWARE_LOG_INFO("Left map, clearing map geometries.");
                depth_prepass.clear_geometry();
                map_loading = false;
            }
        }
    }

    // View matrix resolution (optional extrapolation)
    float latest_view_matrix[16];
    bool has_matrix = game_reader.read_view_matrix(latest_view_matrix);
    if (cfg.extrapolate) {
        static ViewMatrixExtrapolator extrapolator;
        if (has_matrix) {
            extrapolator.update(latest_view_matrix);
        } else {
            extrapolator.update(input.packet.view_matrix);
        }
        extrapolator.get_extrapolated_matrix(input.render_view_matrix);
    } else {
        if (has_matrix) {
            std::memcpy(input.render_view_matrix, latest_view_matrix, sizeof(float) * 16);
        } else {
            std::memcpy(input.render_view_matrix, input.packet.view_matrix, sizeof(float) * 16);
        }
    }

    input.dt = scheduler.get_delta_time();

    // Transpose row-major View-Proj to column-major for OpenGL shaders
    for (int c = 0; c < 4; c++) {
        input.gl_vp[c * 4 + 0] = input.render_view_matrix[0 * 4 + c];
        input.gl_vp[c * 4 + 1] = input.render_view_matrix[1 * 4 + c];
        input.gl_vp[c * 4 + 2] = input.render_view_matrix[2 * 4 + c];
        input.gl_vp[c * 4 + 3] = input.render_view_matrix[3 * 4 + c];
    }

    input.cam_pos = get_camera_position(input.render_view_matrix);
    input.cam_pos_arr[0] = input.cam_pos.x;
    input.cam_pos_arr[1] = input.cam_pos.y;
    input.cam_pos_arr[2] = input.cam_pos.z;

    // Detailed debug logging throttled to 30 seconds
    static auto last_detail_print = std::chrono::steady_clock::now();
    auto now_detail = std::chrono::steady_clock::now();
    if (cfg.debug_game_reader &&
        std::chrono::duration_cast<std::chrono::seconds>(now_detail - last_detail_print).count() >= 30) {
        last_detail_print = now_detail;
        BUNNYWARE_LOG_DEBUG("Window Size: {}x{}", window_width, window_height);
        BUNNYWARE_LOG_DEBUG("Camera Pos: ({:.3f}, {:.3f}, {:.3f})", input.cam_pos.x, input.cam_pos.y, input.cam_pos.z);

        std::stringstream ss;
        for (int i = 0; i < 16; ++i) ss << input.render_view_matrix[i] << " ";
        BUNNYWARE_LOG_DEBUG("ViewProj Matrix: {}", ss.str());

        BUNNYWARE_LOG_DEBUG("Players Tracked: {}", input.packet.player_count);
        for (int i = 0; i < std::min(input.packet.player_count, 3); ++i) {
            const auto& p = input.packet.players[i];
            BUNNYWARE_LOG_DEBUG("Player {}: active={} team={} hp={} name={} origin=({:.3f}, {:.3f}, {:.3f}) bone_count={}", i,
                          p.active, p.team, p.health, p.model_name, p.origin.x, p.origin.y, p.origin.z, p.bone_count);

            if (p.active) {
                if (p.bone_count > 0) {
                    BUNNYWARE_LOG_DEBUG("            Bone 0 Pos: ({:.3f}, {:.3f}, {:.3f})", p.bones[0].position.x,
                                  p.bones[0].position.y, p.bones[0].position.z);
                }
                if (p.bone_count > 7) {
                    BUNNYWARE_LOG_DEBUG("            Bone 7 Pos: ({:.3f}, {:.3f}, {:.3f})", p.bones[7].position.x,
                                  p.bones[7].position.y, p.bones[7].position.z);
                }

                float sx = 0.0f, sy = 0.0f;
                bool screen_ok =
                    world_to_screen(p.origin, &sx, &sy, input.render_view_matrix, window_width, window_height);
                BUNNYWARE_LOG_DEBUG("            WorldToScreen(Origin): ok={} screen=({:.3f}, {:.3f})",
                              screen_ok ? "true" : "false", sx, sy);
            }
        }
    }

    // Accumulate metric part 1 (only if we actually fetched a new packet)
    if (has_new_packet) {
        total_ipc_time_ms += current_ipc_ms;
        max_ipc_time_ms = (std::max)(max_ipc_time_ms, current_ipc_ms);
    }
}

void App::preprocess_frame_state(const FrameInput& input, FrameState& state)
{
    auto cpu_start = std::chrono::high_resolution_clock::now();

    // Fetch latest results from worker thread
    bool has_new_vis = visibility_worker.get_latest_result(last_vischeck_result);
    if (has_new_vis) {
        std::vector<ReaderAimSpot> spots;
        if (last_vischeck_result.solver_active) {
            auto zones = landing_zone_manager.get_zones(current_map);
            for (const auto& sol : last_vischeck_result.solver_solutions) {
                if (sol.found && sol.zone_index >= 0 && sol.zone_index < (int)zones.size()) {
                    ReaderAimSpot spot;
                    spot.pitch = sol.display_pitch;
                    spot.yaw = sol.yaw * 180.0f / 3.14159265f;
                    spot.zone_name = zones[sol.zone_index].name;
                    spot.grenade_type = zones[sol.zone_index].grenade_type;
                    spot.zone_mins = zones[sol.zone_index].mins;
                    spot.zone_maxs = zones[sol.zone_index].maxs;
                    spot.predicted_det_pos = sol.trajectory.end_pos;
                    spots.push_back(spot);
                }
            }
        }
        game_reader.set_active_aim_spots(spots);
    }
    state.vischeck_result = last_vischeck_result;

    // Persistent static scratch buffers to avoid per-player heap allocations
    static std::vector<source2::Mat3x4> world_bones_buf;
    static std::vector<source2::Mat3x4> skinning_palette_buf;
    static std::vector<glm::vec3> sanitized_bones_buf;
    static std::vector<float> batch_bones;

    // Only grow dummy_vis, never shrink; fill with 1.0f
    size_t needed_vis = static_cast<size_t>(input.packet.player_count) * shm::MAX_BONES;
    if (state.dummy_vis.size() < needed_vis) {
        state.dummy_vis.assign(needed_vis, 1.0f);
    }

    state.render_palettes.resize(input.packet.player_count);
    for (int i = 0; i < input.packet.player_count; ++i) {
        const auto& player = input.packet.players[i];
        if (!player.active || is_beyond_esp_distance(player, input.packet, cfg)) {
            state.render_palettes[i].model = nullptr;
            continue;
        }

        std::string model_key = player.model_name;
        if (player.has_defuser) {
            model_key += "#defuser";
        }

        const auto* model = model_cache.get_or_load(model_key);
        if (!model || !model->valid) {
            state.render_palettes[i].model = nullptr;
            continue;
        }

        // Convert raw bone transforms to 3x4 world matrices (reuse static buffer)
        const int bc = model->mesh.bone_count;
        world_bones_buf.resize(bc);
        for (int j = 0; j < bc; ++j) {
            if (j < player.bone_count) {
                world_bones_buf[j] = matrix_from_bone(player.bones[j].position, player.bones[j].rotation);
            } else {
                source2::Mat3x4 identity{};
                identity[0][0] = identity[1][1] = identity[2][2] = 1.0f;
                world_bones_buf[j] = identity;
            }
        }

        // Sanitise bones for LOD issues
        sanitize_lod_bones(world_bones_buf.data(), bc, model->mesh);

        // Extract sanitized world positions for ESP logic (reuse static buffer)
        sanitized_bones_buf.resize(bc);
        for (int j = 0; j < bc; ++j) {
            sanitized_bones_buf[j] = {world_bones_buf[j][0][3], world_bones_buf[j][1][3], world_bones_buf[j][2][3]};
        }

        // Multiply by inverse bind poses to build final skinned joint palette (reuse static buffer)
        skinning_palette_buf.resize(bc);
        for (int j = 0; j < bc; ++j) {
            if (j < static_cast<int>(model->mesh.inv_bind_poses.size())) {
                skinning_palette_buf[j] = multiply_3x4(world_bones_buf[j], model->mesh.inv_bind_poses[j]);
            } else {
                skinning_palette_buf[j] = world_bones_buf[j];
            }
        }

        bool is_player_visible = true;
        bool is_occluded_by_smoke = false;
        if (i < static_cast<int>(state.vischeck_result.player_visible.size())) {
            is_player_visible = state.vischeck_result.player_visible[i];
            is_occluded_by_smoke = state.vischeck_result.player_occluded_by_smoke[i];
        }

        state.render_palettes[i] = {
            model,
            skinning_palette_buf,
            is_player_visible,
            is_occluded_by_smoke,
            sanitized_bones_buf,
            esp_distance_alpha(player, input.packet, cfg),
        };
    }

    // Batch convert and upload bone matrices to UBO (write directly into static buffer)
    batch_bones.clear();
    size_t total_upload_count = static_cast<size_t>(input.packet.player_count) + active_kill_chams.size() +
        active_hit_chams.size();
    batch_bones.resize(total_upload_count * shm::MAX_BONES * 16, 0.0f);
    state.player_ubo_slots.assign(input.packet.player_count, -1);
    int current_slot = 0;

    for (int i = 0; i < input.packet.player_count; ++i) {
        const auto& rp = state.render_palettes[i];
        if (!rp.model) continue;

        float* slot_base = batch_bones.data() + static_cast<size_t>(current_slot) * shm::MAX_BONES * 16;

        for (size_t j = 0; j < rp.skinning_palette.size() && j < shm::MAX_BONES; ++j) {
            const auto& b = rp.skinning_palette[j];
            float* dest = slot_base + j * 16;
            // Column 0
            dest[0] = b[0][0];
            dest[1] = b[0][1];
            dest[2] = b[0][2];
            dest[3] = 0.0f;
            // Column 1
            dest[4] = b[1][0];
            dest[5] = b[1][1];
            dest[6] = b[1][2];
            dest[7] = 0.0f;
            // Column 2
            dest[8] = b[2][0];
            dest[9] = b[2][1];
            dest[10] = b[2][2];
            dest[11] = 0.0f;
            // Column 3
            dest[12] = b[0][3];
            dest[13] = b[1][3];
            dest[14] = b[2][3];
            dest[15] = 1.0f;
        }
        for (size_t j = rp.skinning_palette.size(); j < shm::MAX_BONES; ++j) {
            float* dest = slot_base + j * 16;
            dest[0] = dest[5] = dest[10] = dest[15] = 1.0f;
        }

        state.player_ubo_slots[i] = current_slot++;
    }

    // Append dead player chams bones. Hit FX ghosts share the same layout but
    // never float (Heaven Mode is a Kill FX-only flourish).
    double current_time = glfwGetTime();
    const auto upload_cham = [&](DeadPlayerCham& cham, float z_offset) {
        float* slot_base = batch_bones.data() + static_cast<size_t>(current_slot) * shm::MAX_BONES * 16;
        for (size_t j = 0; j < cham.skinning_palette.size() && j < shm::MAX_BONES; ++j) {
            const auto& b = cham.skinning_palette[j];
            float* dest = slot_base + j * 16;
            // Column 0
            dest[0] = b[0][0];
            dest[1] = b[0][1];
            dest[2] = b[0][2];
            dest[3] = 0.0f;
            // Column 1
            dest[4] = b[1][0];
            dest[5] = b[1][1];
            dest[6] = b[1][2];
            dest[7] = 0.0f;
            // Column 2
            dest[8] = b[2][0];
            dest[9] = b[2][1];
            dest[10] = b[2][2];
            dest[11] = 0.0f;
            // Column 3
            dest[12] = b[0][3];
            dest[13] = b[1][3];
            dest[14] = b[2][3] + z_offset;
            dest[15] = 1.0f;
        }
        for (size_t j = cham.skinning_palette.size(); j < shm::MAX_BONES; ++j) {
            float* dest = slot_base + j * 16;
            dest[0] = dest[5] = dest[10] = dest[15] = 1.0f;
        }
        cham.assigned_ubo_slot = current_slot++;
    };

    for (auto& cham : active_kill_chams) {
        float z_offset = 0.0f;
        if (cfg.kill_chams_heaven) {
            float elapsed = static_cast<float>(current_time - cham.kill_time);
            z_offset = elapsed * cfg.kill_chams_heaven_speed;
        }
        upload_cham(cham, z_offset);
    }
    for (auto& cham : active_hit_chams) {
        upload_cham(cham, 0.0f);
    }

    hit_reveal.update(input.packet, state, current_time, HitReveal::any_enabled(cfg, true),
                      HitReveal::any_enabled(cfg, false));
    assign_frozen_reveal_slots(batch_bones, current_slot);

    if (current_slot > 0) {
        chams_renderer.upload_bones_batch(batch_bones.data(), current_slot);
    }

    // Sort active player indices by model VAO to optimize state changes
    state.sorted_player_indices.clear();
    state.sorted_player_indices.reserve(input.packet.player_count);
    for (int i = 0; i < input.packet.player_count; ++i) {
        if (state.render_palettes[i].model) {
            state.sorted_player_indices.push_back(i);
        }
    }
    std::sort(state.sorted_player_indices.begin(), state.sorted_player_indices.end(),
              [&](int a, int b) { return state.render_palettes[a].model->vao < state.render_palettes[b].model->vao; });

    auto cpu_end = std::chrono::high_resolution_clock::now();
    double current_cpu_ms = std::chrono::duration<double, std::milli>(cpu_end - cpu_start).count();
    total_cpu_time_ms += current_cpu_ms;
    max_cpu_time_ms = (std::max)(max_cpu_time_ms, current_cpu_ms);
}
void App::render_frame(const FrameInput& input, FrameState& state)
{
    auto gpu_start = std::chrono::high_resolution_clock::now();

    const bool flashed = std::isfinite(input.packet.local_flash_alpha) && input.packet.local_flash_alpha > 0.001f;
    const bool shooting = input.packet.local_shots_fired > 0;
    if (flashed || shooting) {
        hidden_frame_cfg = this->cfg;
        apply_flash_visibility(hidden_frame_cfg, input.packet.local_flash_alpha);
        apply_shooting_visibility(hidden_frame_cfg, shooting);
    }
    const OverlayConfig& base_cfg = (flashed || shooting) ? hidden_frame_cfg : this->cfg;

    // Players tab master toggles: when Chams or DamageFX are switched off, mask
    // the underlying style/effect selections so nothing from them renders.
    const bool gate_chams = !base_cfg.chams_enabled && (base_cfg.style_vis.value != ChamsStyleEnum::DISABLED ||
                                                        base_cfg.style_invis.value != ChamsStyleEnum::DISABLED);
    const bool gate_damage_fx = !base_cfg.damage_fx_enabled &&
        (base_cfg.kill_effect.value != KillFxModeEnum::DISABLED ||
         base_cfg.hit_effect_mode.value != KillFxModeEnum::DISABLED);
    static OverlayConfig gated_frame_cfg;
    if (gate_chams || gate_damage_fx) {
        gated_frame_cfg = base_cfg;
        if (gate_chams) {
            gated_frame_cfg.style_vis = ChamsStyleEnum::DISABLED;
            gated_frame_cfg.style_invis = ChamsStyleEnum::DISABLED;
        }
        if (gate_damage_fx) {
            gated_frame_cfg.kill_effect = KillFxModeEnum::DISABLED;
            gated_frame_cfg.hit_effect_mode = KillFxModeEnum::DISABLED;
        }
    }
    const OverlayConfig& cfg = (gate_chams || gate_damage_fx) ? gated_frame_cfg : base_cfg;

    depth_prepass.begin_frame();

    // Transform active doors and upload them to depth prepass renderer
    std::vector<MapParser::Triangle> transformed_door_triangles;
    for (int i = 0; i < input.packet.door_count; ++i) {
        const auto& door = input.packet.doors[i];
        if (!door.active || door.model_name[0] == '\0') continue;
        
        std::vector<MapParser::Triangle> local_tris = MapParser::GetDoorMesh(door.model_name);
        if (local_tris.empty()) continue;
        
        float yaw_rad = door.yaw * 3.14159265f / 180.0f;
        float cos_yaw = std::cos(yaw_rad);
        float sin_yaw = std::sin(yaw_rad);
        
        for (const auto& tri : local_tris) {
            MapParser::Triangle transformed_tri = tri;
            
            // Transform v0
            {
                float x = tri.v0.pos.x * door.scale.x;
                float y = tri.v0.pos.y * door.scale.y;
                float z = tri.v0.pos.z * door.scale.z;
                transformed_tri.v0.pos.x = x * cos_yaw - y * sin_yaw + door.origin.x;
                transformed_tri.v0.pos.y = x * sin_yaw + y * cos_yaw + door.origin.y;
                transformed_tri.v0.pos.z = z + door.origin.z;
                transformed_tri.v0.type_id = 7.0f;
            }
            // Transform v1
            {
                float x = tri.v1.pos.x * door.scale.x;
                float y = tri.v1.pos.y * door.scale.y;
                float z = tri.v1.pos.z * door.scale.z;
                transformed_tri.v1.pos.x = x * cos_yaw - y * sin_yaw + door.origin.x;
                transformed_tri.v1.pos.y = x * sin_yaw + y * cos_yaw + door.origin.y;
                transformed_tri.v1.pos.z = z + door.origin.z;
                transformed_tri.v1.type_id = 7.0f;
            }
            // Transform v2
            {
                float x = tri.v2.pos.x * door.scale.x;
                float y = tri.v2.pos.y * door.scale.y;
                float z = tri.v2.pos.z * door.scale.z;
                transformed_tri.v2.pos.x = x * cos_yaw - y * sin_yaw + door.origin.x;
                transformed_tri.v2.pos.y = x * sin_yaw + y * cos_yaw + door.origin.y;
                transformed_tri.v2.pos.z = z + door.origin.z;
                transformed_tri.v2.type_id = 7.0f;
            }
            
            transformed_door_triangles.push_back(transformed_tri);
        }
    }

    // Append active smoke cloud voxels to the dynamic geometry (so they are
    // rendered into the depth prepass). Only dense voxels are emitted: the
    // vischeck cutoff is also the depth prepass shader's discard threshold, so
    // thinner voxels would never affect the occlusion depth.
    if (cfg.esp_smoke_check) {
        for (int i = 0; i < input.packet.smoke_count; ++i) {
            const auto& smoke = input.packet.smokes[i];
            if (!smoke.active) continue;

            glm::vec3 center = { smoke.center.x, smoke.center.y, smoke.center.z };
            constexpr float size_scale = 20.0f;
            constexpr float half_scale = 16.0f;

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
                    if (alpha < smoke_wireframe::kSmokeAlphaThreshold) continue;

                    glm::vec3 voxel_pos = {
                        center.x + (static_cast<float>(x) - half_scale) * size_scale,
                        center.y + (static_cast<float>(y) - half_scale) * size_scale,
                        center.z + (static_cast<float>(z) - half_scale) * size_scale
                    };

                    float norm_alpha = static_cast<float>(alpha) / 255.0f;
                    float type_id = -norm_alpha; // Negative Type ID signals smoke + normal alpha value to shader

                    append_smoke_voxel_triangles(transformed_door_triangles, voxel_pos, size_scale, type_id);
                }
            }
        }
    }

    depth_prepass.upload_dynamic_geometry(transformed_door_triangles);

    // Update grenade tracker simulation
    grenade_tracker.update(state.vischeck_result, glfwGetTime(), input.dt, cfg);

    overlay->begin_frame();

    // Anti Flash: while the local player is blinded, lay down an opaque black
    // backdrop so the game's white flash overlay is hidden. Drawn before the 3D
    // passes so chams, the map wireframe and every ESP overlay composite on top
    // of it instead of being covered by it.
    //
    // The trigger is latched with hysteresis: it engages once the blind alpha
    // reaches anti_flash_alpha_start and only releases once the alpha has decayed
    // back down to anti_flash_alpha_end. A single cutoff would flicker the
    // backdrop on and off while a fading flash sits on the threshold.
    static bool anti_flash_latched = false;
    static bool anti_flash_active_last = false;
    static float anti_flash_log_timer = 0.0f;

    const float anti_flash_alpha = input.packet.local_flash_alpha;
    if (!cfg.anti_flash) {
        anti_flash_latched = false;
    } else if (anti_flash_alpha >= cfg.anti_flash_alpha_start) {
        anti_flash_latched = true;
    } else if (anti_flash_alpha <= cfg.anti_flash_alpha_end) {
        anti_flash_latched = false;
    }
    const bool anti_flash_active = cfg.anti_flash && anti_flash_latched;

    // Backdrop tint: the picker supplies the colour, the opacity slider scales
    // its alpha so a partially transparent wash is possible.
    const Color& anti_flash_bg = cfg.anti_flash_background_color;
    const float anti_flash_bg_alpha = std::clamp(anti_flash_bg.a * cfg.anti_flash_background_opacity, 0.0f, 1.0f);

    // Debug: log every engage/release edge plus a 1 Hz heartbeat while the
    // feature is enabled so the raw flash alpha is always visible.
    if (cfg.anti_flash) {
        if (anti_flash_active != anti_flash_active_last) {
            BUNNYWARE_LOG_INFO(
                "AntiFlash: {} - local flash alpha {:.1f} (start >= {:.1f}, end <= {:.1f}); background "
                "({:.2f}, {:.2f}, {:.2f}) @ {:.2f}, wireframe {} {:.1f}px (map geometry {}), dot {}, chams {}",
                anti_flash_active ? "ENGAGED" : "RELEASED", anti_flash_alpha, cfg.anti_flash_alpha_start,
                cfg.anti_flash_alpha_end, anti_flash_bg.r, anti_flash_bg.g, anti_flash_bg.b,
                cfg.anti_flash_background_opacity, cfg.anti_flash_wireframe ? "on" : "off",
                cfg.anti_flash_wireframe_thickness, depth_prepass.has_geometry() ? "loaded" : "MISSING",
                cfg.anti_flash_crosshair ? "on" : "off", cfg.anti_flash_chams ? "on" : "off");
        }
        anti_flash_log_timer += input.dt;
        if (anti_flash_log_timer >= 1.0f) {
            anti_flash_log_timer = 0.0f;
            // Comparing against the per-player flag value makes it obvious whether
            // the local read or the overlay render is the failing side.
            float max_other_flash = 0.0f;
            for (int i = 0; i < input.packet.player_count; ++i) {
                max_other_flash = std::max(max_other_flash, input.packet.players[i].flash_alpha);
            }
            BUNNYWARE_LOG_INFO(
                "AntiFlash: local alpha {:.1f} (start >= {:.1f}, end <= {:.1f}) -> {}; max other-player "
                "alpha {:.1f}; overlay {}x{}, map geometry {}",
                anti_flash_alpha, cfg.anti_flash_alpha_start, cfg.anti_flash_alpha_end,
                anti_flash_active ? "active" : "idle", max_other_flash, window_width, window_height,
                depth_prepass.has_geometry() ? "loaded" : "MISSING");
        }
    } else {
        anti_flash_log_timer = 0.0f;
    }
    anti_flash_active_last = anti_flash_active;

    if (anti_flash_active && anti_flash_bg_alpha > 0.0f) {
        const float background[4] = {std::clamp(anti_flash_bg.r, 0.0f, 1.0f),
                                     std::clamp(anti_flash_bg.g, 0.0f, 1.0f),
                                     std::clamp(anti_flash_bg.b, 0.0f, 1.0f), anti_flash_bg_alpha};

        // A fullscreen 2D quad must not inherit state from the previous frame's 3D
        // passes: a stale stencil/scissor/cull test or a cleared colour mask
        // silently discards it. Snapshot the state, force a clean 2D state for the
        // draw, then restore exactly what was there.
        const GLboolean depth_test_was_enabled = glIsEnabled(GL_DEPTH_TEST);
        const GLboolean stencil_test_was_enabled = glIsEnabled(GL_STENCIL_TEST);
        const GLboolean scissor_test_was_enabled = glIsEnabled(GL_SCISSOR_TEST);
        const GLboolean cull_face_was_enabled = glIsEnabled(GL_CULL_FACE);
        GLboolean depth_mask_was_enabled = GL_TRUE;
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_mask_was_enabled);
        GLboolean color_mask_was_enabled[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
        glGetBooleanv(GL_COLOR_WRITEMASK, color_mask_was_enabled);

        if (stencil_test_was_enabled || scissor_test_was_enabled || cull_face_was_enabled ||
            !color_mask_was_enabled[0] || !color_mask_was_enabled[1] || !color_mask_was_enabled[2] ||
            !color_mask_was_enabled[3]) {
            static bool anti_flash_state_warned = false;
            if (!anti_flash_state_warned) {
                anti_flash_state_warned = true;
                BUNNYWARE_LOG_WARN(
                    "AntiFlash: cleared leaked GL state before the background draw (stencil={}, scissor={}, "
                    "cull={}, colour mask={}{}{}{})",
                    stencil_test_was_enabled ? "on" : "off", scissor_test_was_enabled ? "on" : "off",
                    cull_face_was_enabled ? "on" : "off", color_mask_was_enabled[0] ? "1" : "0",
                    color_mask_was_enabled[1] ? "1" : "0", color_mask_was_enabled[2] ? "1" : "0",
                    color_mask_was_enabled[3] ? "1" : "0");
            }
        }

        esp_renderer.clear();
        esp_renderer.set_ortho(0.0f, static_cast<float>(window_width), static_cast<float>(window_height), 0.0f);
        esp_renderer.add_rect_2d(0.0f, 0.0f, static_cast<float>(window_width), static_cast<float>(window_height),
                                 background);

        glDisable(GL_STENCIL_TEST);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_CULL_FACE);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glBlendEquation(GL_FUNC_ADD);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        esp_renderer.flush_triangles();
        const GLenum anti_flash_gl_error = glGetError();
        if (anti_flash_gl_error != GL_NO_ERROR) {
            BUNNYWARE_LOG_ERROR("AntiFlash: background draw raised GL error {:#x}", anti_flash_gl_error);
        }

        glDepthMask(depth_mask_was_enabled);
        glColorMask(color_mask_was_enabled[0], color_mask_was_enabled[1], color_mask_was_enabled[2],
                    color_mask_was_enabled[3]);
        if (cull_face_was_enabled) glEnable(GL_CULL_FACE);
        if (scissor_test_was_enabled) glEnable(GL_SCISSOR_TEST);
        if (stencil_test_was_enabled) glEnable(GL_STENCIL_TEST);
        if (depth_test_was_enabled) glEnable(GL_DEPTH_TEST);
    }

    // Overlay prompt window removed - creator fields now fill manual location creator in the GUI

    // Smoke visuals (cage + through-smokes map wireframe) are world geometry, so
    // draw them before the player passes: chams, the skeleton and every ESP
    // overlay must paint on top of them instead of being covered by them. Those
    // passes shade depth-tested geometry, so the map depth buffer has to exist
    // even when no chams path requested a prepass this frame.
    if ((cfg.smoke_wireframe || cfg.through_smokes) && depth_prepass.has_geometry() &&
        !depth_prepass.has_rendered_this_frame()) {
        depth_prepass.render(input.gl_vp);
    }
    bool smoke_depth_refreshed = false;
    if (cfg.smoke_wireframe || cfg.through_smokes) {
        smoke_depth_refreshed = grenade_renderer.render_smoke_visuals(state.vischeck_result, esp_renderer,
                                                                     depth_prepass, input.gl_vp, cfg, overlay.get());
    }
    // The stencil refresh rewrites the map depth inside the smoke silhouette to
    // reveal the first surface behind the cloud. Put the plain depth back so
    // player occlusion through smoke behaves exactly as it did before.
    if (smoke_depth_refreshed && depth_prepass.has_geometry()) {
        depth_prepass.render(input.gl_vp);
    }

    // 5. Render 3D Map Collision Wireframe Visualizer
    if (cfg.map_visualizer_enabled) {
        depth_prepass.render_wireframe(input.gl_vp, cfg.map_visualizer_color, cfg.map_visualizer_depth_tested);
    }

    // Anti Flash: reveal the map collision wireframe over the black backdrop so
    // the player can still read the geometry while blinded. Like every other map
    // wireframe this is world geometry, so it is drawn before the player passes:
    // chams, the skeleton and the ESP overlays composite on top of it.
    static bool anti_flash_wireframe_warned = false;
    if (anti_flash_active && cfg.anti_flash_wireframe) {
        if (!depth_prepass.has_geometry() && !depth_prepass.has_dynamic_geometry()) {
            if (!anti_flash_wireframe_warned) {
                anti_flash_wireframe_warned = true;
                BUNNYWARE_LOG_WARN("AntiFlash: map wireframe requested but no map geometry is uploaded "
                                 "(is maps_dir populated and the map loaded?)");
            }
        } else {
            anti_flash_wireframe_warned = false;
        }
        // The wireframe occludes against the map depth, so make sure the map
        // depth buffer exists even when no chams path requested a prepass.
        if (cfg.anti_flash_wireframe_depth_tested && depth_prepass.has_geometry() &&
            !depth_prepass.has_rendered_this_frame()) {
            depth_prepass.render(input.gl_vp);
        }
        depth_prepass.render_wireframe(input.gl_vp, cfg.anti_flash_wireframe_color,
                                       cfg.anti_flash_wireframe_depth_tested,
                                       cfg.anti_flash_wireframe_thickness);
    } else {
        anti_flash_wireframe_warned = false;
    }

    // Anti Flash: optionally force chams on for the blinded window so players stay
    // readable through the black backdrop. When chams are already enabled the
    // user's own settings win untouched; otherwise the configured material is
    // applied for the duration only, reusing the Players -> Chams colours.
    const OverlayConfig* chams_cfg = &cfg;
    static OverlayConfig anti_flash_chams_cfg;
    if (anti_flash_active && cfg.anti_flash_chams) {
        const bool chams_already_enabled =
            cfg.style_vis != ChamsStyleEnum::DISABLED || cfg.style_invis != ChamsStyleEnum::DISABLED;
        if (!chams_already_enabled) {
            anti_flash_chams_cfg = cfg;
            anti_flash_chams_cfg.style_vis = cfg.anti_flash_chams_style;
            anti_flash_chams_cfg.style_invis = cfg.anti_flash_chams_style;
            chams_cfg = &anti_flash_chams_cfg;
        }
    }

    // Orchestrate 3D player rendering passes
    ChamsPassInputs passes_inputs{input,         state,        *chams_cfg,   chams_renderer,
                                  depth_prepass, esp_renderer, &gpu_profiler, model_cache, window_width, window_height,
                                  active_kill_chams, active_hit_chams};
    ChamsPass::run(passes_inputs);
    draw_reveal_passes(input, state, *chams_cfg);

    // 6. Draw grenade trajectories
    grenade_renderer.render(grenade_tracker, esp_renderer, state.vischeck_result, input.gl_vp,
                            input.render_view_matrix, cfg, overlay.get(), svg_cache,
                            input.packet.planted_c4, input.packet.local_eye);

    // 6.5 Draw grenade helper lineups
    if (cfg.grenade_helper_enabled) {
        std::string clean_map = current_map;
        if (clean_map.find("maps/") == 0) clean_map = clean_map.substr(5);
        if (clean_map.find(".vpk") != std::string::npos) clean_map = clean_map.substr(0, clean_map.find(".vpk"));

        Vec3 local_origin = input.packet.local_eye;
        local_origin.z -= 64.0f; // Approximate ground position

        std::vector<GrenadeLineup> current_lineups = grenade_helper_data.get_lineups(clean_map);
        if (cfg.grenade_creator_enabled && menu) {
            if (menu->gcreator_pos[0] != 0.0f || menu->gcreator_pos[1] != 0.0f || menu->gcreator_pos[2] != 0.0f) {
                GrenadeLineup temp_lineup;
                temp_lineup.title = std::string(menu->gcreator_title).empty() ? "Creator Preview" : menu->gcreator_title;
                temp_lineup.description = std::string(menu->gcreator_desc).empty() ? "Unsaved Lineup" : menu->gcreator_desc;
                temp_lineup.weapon = static_cast<uint8_t>(menu->gcreator_weapon_idx + 1);
                temp_lineup.position = Vec3(menu->gcreator_pos[0], menu->gcreator_pos[1], menu->gcreator_pos[2]);
                temp_lineup.viewangles = Vec3(menu->gcreator_angles[0], menu->gcreator_angles[1], 0.0f);

                const char* throw_methods[] = { "left", "right", "middle", "none" };
                temp_lineup.movement.throw_type = throw_methods[menu->gcreator_throw_type_idx];
                if (menu->gcreator_crouch) temp_lineup.movement.modifiers.push_back("crouch");
                if (menu->gcreator_shift) temp_lineup.movement.modifiers.push_back("shift");
                
                std::string mkey = menu->gcreator_movement_key;
                std::transform(mkey.begin(), mkey.end(), mkey.begin(), [](unsigned char c) { return std::tolower(c); });
                temp_lineup.movement.movement_key = mkey;
                temp_lineup.movement.jump = menu->gcreator_jump;
                temp_lineup.movement.movement_ticks = menu->gcreator_movement_ticks;

                std::vector<std::string> parts;
                if (temp_lineup.movement.throw_type != "none") parts.push_back(temp_lineup.movement.throw_type);
                for (const auto& mod : temp_lineup.movement.modifiers) parts.push_back(mod);
                if (!temp_lineup.movement.movement_key.empty()) parts.push_back(temp_lineup.movement.movement_key);
                if (temp_lineup.movement.jump) parts.push_back("jump");
                if (temp_lineup.movement.movement_ticks > 0) {
                    parts.push_back(std::to_string(temp_lineup.movement.movement_ticks) + " ticks");
                }
                if (parts.empty()) {
                    temp_lineup.throw_method = "manual";
                } else {
                    std::string res;
                    for (size_t i = 0; i < parts.size(); ++i) {
                        if (i > 0) res += " + ";
                        res += parts[i];
                    }
                    temp_lineup.throw_method = res;
                }
                current_lineups.push_back(temp_lineup);
            }
        }

        grenade_helper_renderer.render(current_lineups, 
                                       state.vischeck_result,
                                       landing_zone_manager,
                                       clean_map,
                                       local_origin, 
                                       input.packet.local_eye,
                                       input.packet.local_angles,
                                       input.packet.held_weapon_id,
                                       input.render_view_matrix,
                                       cfg,
                                       overlay.get(),
                                       svg_cache,
                                       visibility_worker);
    }

    // Noise ESP (reNoiseESP-style multi-type ground markers; detect @ ~64 Hz)
    noise_esp.clear_pending_labels();
    noise_esp.update(input.packet, cfg, glfwGetTime());
    if (const std::optional<proximity_beep::Plan> beep = proximity_beep.update(input.packet, cfg, glfwGetTime())) {
        beep_player.play(beep->volume, beep->pitch_step);
    }
    if (cfg.esp_enabled && cfg.esp_step_circles) {
        noise_esp.render(esp_renderer, cfg, input.gl_vp, input.render_view_matrix,
                         window_width, window_height);
    } else {
        esp_renderer.flush_instances();
    }

    gpu_profiler.begin_section(GpuTimerSection::ESP_2D);

    // 7. Render 2D ESP Overlay (Skeleton, Box, Health Bar)
    if (cfg.esp_enabled) {
        const FrameInput* esp_input = &input;
        FrameState* esp_state = &state;
        build_esp_reveal(input, state, cfg, esp_input, esp_state);
        const FrameInput& input = *esp_input;
        FrameState& state = *esp_state;
        static float smoothed_health[shm::MAX_PLAYERS];
        static bool player_active_last[shm::MAX_PLAYERS] = {false};
        static bool head_was_exposed[shm::MAX_PLAYERS] = {false};
        static float head_peek_flash[shm::MAX_PLAYERS] = {0.0f};
        static Vec3 head_last_seen[shm::MAX_PLAYERS];
        static float head_ghost_age[shm::MAX_PLAYERS] = {0.0f};
        static bool first_init = true;
        if (first_init) {
            std::fill(std::begin(smoothed_health), std::end(smoothed_health), -1.0f);
            first_init = false;
        }

        if (cfg.esp_box || cfg.esp_health_bar || cfg.esp_head_circle) {
            bool draw_visible = cfg.esp_box_hp_visible;
            bool draw_occluded = cfg.esp_box_hp_occluded;

            if (draw_visible || draw_occluded) {
                esp_renderer.clear();
                esp_renderer.set_ortho(0, window_width, window_height, 0);

                auto queue_esp = [&](float z) {
                    for (int i = 0; i < input.packet.player_count; ++i) {
                        const auto& rp = state.render_palettes[i];
                        if (!rp.model) {
                            player_active_last[i] = false;
                            continue;
                        }
                        if (rp.hidden(2)) continue;
                        const auto& player = input.packet.players[i];

                        bool matches_team = (player.team == input.packet.local_team);
                        if (cfg.team_check && matches_team) {
                            player_active_last[i] = false;
                            continue;
                        }

                        bool is_visible = false;
                        if (i < static_cast<int>(state.vischeck_result.player_visible.size())) {
                            is_visible = state.vischeck_result.player_visible[i];
                        } else {
                            is_visible = true;
                        }

                        if (is_visible && !draw_visible) continue;
                        if (!is_visible && !draw_occluded) continue;

                        const bool head_exposed = is_visible && !(cfg.esp_smoke_check && rp.is_occluded_by_smoke);
                        if (!player_active_last[i]) {
                            head_was_exposed[i] = head_exposed;
                            head_peek_flash[i] = 0.0f;
                            head_ghost_age[i] = -1.0f;
                        } else if (head_exposed && !head_was_exposed[i]) {
                            head_peek_flash[i] = 1.0f;
                        }
                        head_was_exposed[i] = head_exposed;
                        head_peek_flash[i] = std::max(0.0f, head_peek_flash[i] - input.dt * 3.0f);

                        if (!player_active_last[i] || smoothed_health[i] < 0.0f) {
                            smoothed_health[i] = static_cast<float>(player.health);
                            player_active_last[i] = true;
                        } else {
                            smoothed_health[i] +=
                                (player.health - smoothed_health[i]) * std::clamp(10.0f * input.dt, 0.0f, 1.0f);
                            if (std::abs(player.health - smoothed_health[i]) > 50.0f) {
                                smoothed_health[i] = static_cast<float>(player.health);
                            }
                        }

                        const EspRenderer::VertexMark esp_mark = esp_renderer.mark();
                        const bool esp_highlighted = rp.highlighted(2);
                        float bx, by, bw, bh;
                        if (get_player_bounds(rp.sanitized_bones, player.origin, input.packet.local_eye,
                                              input.render_view_matrix, window_width, window_height, bx, by, bw, bh, cfg)) {
                            float player_z = 0.0f;
                            if (z != 0.0f) {
                                player_z = get_player_ndc_z(player.origin, input.render_view_matrix);
                            }

                            if (cfg.esp_box) {
                                esp_renderer.add_outlined_rect_2d(bx, by, bw, bh, cfg.esp_box_color, cfg.esp_box_thickness,
                                                                  cfg.esp_box_outline, player_z);
                            }
                            if (cfg.esp_health_bar) {
                                esp_renderer.add_health_bar_2d(bx, by, bw, bh, smoothed_health[i], cfg, player_z);
                            }
                            if (cfg.esp_head_circle && 7 < rp.sanitized_bones.size()) {
                                const Vec3 head_world = rp.sanitized_bones[7];
                                if (head_exposed) {
                                    head_last_seen[i] = head_world;
                                    head_ghost_age[i] = 0.0f;
                                } else if (head_ghost_age[i] >= 0.0f) {
                                    head_ghost_age[i] += input.dt;
                                }

                                const bool show_ghost = cfg.esp_head_circle_wall_aware && !head_exposed;
                                const Vec3 dot_world = show_ghost ? head_last_seen[i] : head_world;
                                const float ghost_seconds = std::max(0.1f, cfg.esp_head_circle_ghost_seconds);
                                const bool ghost_alive = head_ghost_age[i] >= 0.0f && head_ghost_age[i] < ghost_seconds;

                                float hx, hy;
                                if ((!show_ghost || ghost_alive) &&
                                    world_to_screen(dot_world, &hx, &hy, input.render_view_matrix, window_width, window_height)) {
                                    float radius = std::max(2.0f, bh / 14.0f);
                                    if (cfg.esp_head_circle_world_size) {
                                        const float world_radius = head_screen_radius(
                                            dot_world, input.packet.local_angles, input.render_view_matrix,
                                            window_width, window_height);
                                        if (world_radius > 0.0f) radius = std::max(1.5f, world_radius);
                                    }
                                    float head_color[4] = {cfg.esp_box_color[0], cfg.esp_box_color[1],
                                                           cfg.esp_box_color[2], cfg.esp_box_color[3]};
                                    if (cfg.esp_head_circle_health_color) {
                                        const float hp = std::clamp(smoothed_health[i] / 100.0f, 0.0f, 1.0f);
                                        for (int c = 0; c < 4; ++c) {
                                            head_color[c] = cfg.esp_health_bar_gradient_start[c] * hp +
                                                            cfg.esp_health_bar_gradient_end[c] * (1.0f - hp);
                                        }
                                    }
                                    const float base_alpha = head_color[3];
                                    head_color[3] = base_alpha * std::clamp(cfg.esp_head_circle_opacity, 0.0f, 1.0f);
                                    const int segments = std::clamp(static_cast<int>(radius * 2.0f), 12, 48);

                                    if (show_ghost) {
                                        const float fade = 1.0f - head_ghost_age[i] / ghost_seconds;
                                        head_color[3] = base_alpha * std::clamp(cfg.esp_head_circle_occluded_opacity, 0.0f, 1.0f) * fade;
                                        esp_renderer.add_circle_2d(hx, hy, radius, head_color, 1.5f, segments, player_z);
                                    } else if (cfg.esp_head_circle_filled) {
                                        if (cfg.esp_head_circle_outline) {
                                            const float black[4] = {0.0f, 0.0f, 0.0f, head_color[3]};
                                            esp_renderer.add_filled_circle_2d(hx, hy, radius + 1.0f, black, segments, player_z);
                                        }
                                        esp_renderer.add_filled_circle_2d(hx, hy, radius, head_color, segments, player_z);
                                    } else {
                                        esp_renderer.add_circle_2d(hx, hy, radius, head_color, cfg.esp_box_thickness, segments, player_z);
                                    }

                                    if (!show_ghost && cfg.esp_head_circle_wall_aware && cfg.esp_head_circle_peek_flash &&
                                        head_peek_flash[i] > 0.0f) {
                                        const float t = 1.0f - head_peek_flash[i];
                                        float flash_color[4] = {head_color[0], head_color[1], head_color[2],
                                                                base_alpha * head_peek_flash[i]};
                                        esp_renderer.add_circle_2d(hx, hy, radius * (1.0f + 1.5f * t), flash_color, 2.0f,
                                                                   segments, player_z);
                                    }
                                }
                            }
                        }
                        esp_renderer.multiply_alpha_since(esp_mark, rp.distance_alpha);
                        if (esp_highlighted) esp_renderer.tint_since(esp_mark, rp.highlight_alpha[2], rp.tint(2));
                    }
                };

                glDisable(GL_DEPTH_TEST);
                queue_esp(0.0f);
                esp_renderer.flush_triangles();
                esp_renderer.flush_lines();
                glEnable(GL_DEPTH_TEST);
            }
        }

        // Draw ImGui ESP elements: Name, Armor Bar, Flags, and Weapon SVG Icons
        ImDrawList* draw_list = ImGui::GetBackgroundDrawList();

        // Noise ESP plant/defuse labels (screen-space text from 3D events)
        if (cfg.esp_step_circles && cfg.esp_noise_show_plant_defuse_text) {
            for (const auto& ld : noise_esp.pending_labels()) {
                const float rx = std::round(ld.x);
                const float ry = std::round(ld.y - 14.0f);
                const int a = static_cast<int>(std::clamp(ld.alpha, 0.0f, 1.0f) * 255.0f);
                const bool is_plant = ld.text[0] == 'P';
                const Color& tc = is_plant ? cfg.esp_noise_plant_color : cfg.esp_noise_defuse_color;
                const ImU32 col = IM_COL32(
                    static_cast<int>(std::clamp(tc[0], 0.0f, 1.0f) * 255.0f),
                    static_cast<int>(std::clamp(tc[1], 0.0f, 1.0f) * 255.0f),
                    static_cast<int>(std::clamp(tc[2], 0.0f, 1.0f) * 255.0f),
                    a);
                const ImU32 outline = IM_COL32(0, 0, 0, a);
                const ImVec2 sz = ImGui::CalcTextSize(ld.text);
                const float tx = std::round(rx - sz.x * 0.5f);
                const float ty = ry;
                draw_list->AddText(ImVec2(tx + 1.0f, ty + 1.0f), outline, ld.text);
                draw_list->AddText(ImVec2(tx - 1.0f, ty + 1.0f), outline, ld.text);
                draw_list->AddText(ImVec2(tx + 1.0f, ty - 1.0f), outline, ld.text);
                draw_list->AddText(ImVec2(tx - 1.0f, ty - 1.0f), outline, ld.text);
                draw_list->AddText(ImVec2(tx, ty), col, ld.text);
            }
        }

        if (cfg.aim_enabled && cfg.aim_fov_circle) {
            float cx = window_width * 0.5f;
            float cy = window_height * 0.5f;
            
            uint16_t weapon_id = input.packet.held_weapon_id;
            WeaponGroup w_group = get_weapon_group(weapon_id);
            float active_fov = cfg.aim_fov;
            if (w_group == WEAPON_GROUP_LIGHT_PISTOL) active_fov = cfg.aim_light_pistols_fov;
            else if (w_group == WEAPON_GROUP_HEAVY_PISTOL) active_fov = cfg.aim_heavy_pistols_fov;
            else if (w_group == WEAPON_GROUP_RIFLE) active_fov = cfg.aim_rifles_fov;
            else if (w_group == WEAPON_GROUP_SMG) active_fov = cfg.aim_smgs_fov;
            else if (w_group == WEAPON_GROUP_SCOUT) active_fov = cfg.aim_scout_fov;
            else if (w_group == WEAPON_GROUP_AWP) active_fov = cfg.aim_awp_fov;
            else if (w_group == WEAPON_GROUP_SNIPER) active_fov = cfg.aim_auto_snipers_fov;
            else if (w_group == WEAPON_GROUP_SHOTGUN) active_fov = cfg.aim_shotguns_fov;
            else if (w_group == WEAPON_GROUP_MG) active_fov = cfg.aim_lmgs_fov;
            else active_fov = cfg.aim_others_fov;

            // Mathematically correct FOV to pixels projection using the view matrix
            float radius = 0.0f;
            glm::vec3 eye{ input.packet.local_eye.x, input.packet.local_eye.y, input.packet.local_eye.z };
            glm::vec3 angles{ input.packet.local_angles.x, input.packet.local_angles.y, input.packet.local_angles.z };
            
            float deg2rad = 3.1415926535f / 180.0f;
            float pitch_rad = angles.x * deg2rad;
            float yaw_rad = angles.y * deg2rad;
            
            float sin_p = std::sin(pitch_rad);
            float cos_p = std::cos(pitch_rad);
            float sin_y = std::sin(yaw_rad);
            float cos_y = std::cos(yaw_rad);
            
            glm::vec3 forward{ cos_p * cos_y, cos_p * sin_y, -sin_p };
            glm::vec3 right{ -sin_y, cos_y, 0.0f };
            
            float fov_rad = active_fov * deg2rad;
            glm::vec3 center_world = eye + forward * 100.0f;
            glm::vec3 offset_world = eye + (forward * std::cos(fov_rad) + right * std::sin(fov_rad)) * 100.0f;
            
            float screen_cx = 0.0f, screen_cy = 0.0f;
            float screen_ox = 0.0f, screen_oy = 0.0f;
            
            if (world_to_screen({center_world.x, center_world.y, center_world.z}, &screen_cx, &screen_cy, input.render_view_matrix, window_width, window_height) &&
                world_to_screen({offset_world.x, offset_world.y, offset_world.z}, &screen_ox, &screen_oy, input.render_view_matrix, window_width, window_height)) {
                radius = std::sqrt((screen_ox - screen_cx) * (screen_ox - screen_cx) + (screen_oy - screen_cy) * (screen_oy - screen_cy));
            } else {
                // Fallback to a better approximation if projection fails
                // default vertical FOV is ~60 degrees
                radius = active_fov * window_height / 60.0f;
            }

            ImColor col = ImColor(cfg.aim_fov_circle_color.r, cfg.aim_fov_circle_color.g, cfg.aim_fov_circle_color.b, cfg.aim_fov_circle_color.a);
            draw_list->AddCircle(ImVec2(cx, cy), radius, col, 64, 1.0f);
        }

        // Draw Spread Circle
        if (cfg.esp_spread_circle) {
            float cx = window_width * 0.5f;
            float cy = window_height * 0.5f;
            
            // Only draw when local player is alive
            if (input.packet.local_health > 0) {
                float current_spread = input.packet.local_spread;
                float base_spread = input.packet.local_base_spread;
                
                if (!std::isnan(current_spread) && !std::isinf(current_spread) && current_spread > 0.0f) {
                    glm::vec3 eye{ input.packet.local_eye.x, input.packet.local_eye.y, input.packet.local_eye.z };
                    glm::vec3 angles{ input.packet.local_angles.x, input.packet.local_angles.y, input.packet.local_angles.z };
                    
                    float deg2rad = 3.1415926535f / 180.0f;
                    float pitch_rad = angles.x * deg2rad;
                    float yaw_rad = angles.y * deg2rad;
                    
                    float sin_p = std::sin(pitch_rad);
                    float cos_p = std::cos(pitch_rad);
                    float sin_y = std::sin(yaw_rad);
                    float cos_y = std::cos(yaw_rad);
                    
                    glm::vec3 forward{ cos_p * cos_y, cos_p * sin_y, -sin_p };
                    glm::vec3 right{ -sin_y, cos_y, 0.0f };
                    
                    float screen_cx = 0.0f, screen_cy = 0.0f;
                    float screen_sx = 0.0f, screen_sy = 0.0f;
                    
                    glm::vec3 center_world = eye + forward * 100.0f;
                    glm::vec3 spread_world = eye + forward * 100.0f + right * (current_spread * 100.0f);
                    
                    float target_radius = 0.0f;
                    if (world_to_screen({center_world.x, center_world.y, center_world.z}, &screen_cx, &screen_cy, input.render_view_matrix, window_width, window_height) &&
                        world_to_screen({spread_world.x, spread_world.y, spread_world.z}, &screen_sx, &screen_sy, input.render_view_matrix, window_width, window_height)) {
                        target_radius = std::sqrt((screen_sx - screen_cx) * (screen_sx - screen_cx) + (screen_sy - screen_cy) * (screen_sy - screen_cy));
                    } else {
                        // fallback based on window height and FOV
                        target_radius = current_spread * window_height;
                    }
                    
                    // Smooth the radius
                    if (spread_circle_smoothed_radius <= 0.01f) {
                        spread_circle_smoothed_radius = target_radius;
                    } else {
                        float interpolation_speed = cfg.esp_spread_circle_animation_speed;
                        spread_circle_smoothed_radius += (target_radius - spread_circle_smoothed_radius) * std::clamp(input.dt * interpolation_speed, 0.0f, 1.0f);
                    }
                    
                    // Base spread radius
                    float base_target_radius = 0.0f;
                    if (!std::isnan(base_spread) && !std::isinf(base_spread) && base_spread > 0.0f) {
                        glm::vec3 base_spread_world = eye + forward * 100.0f + right * (base_spread * 100.0f);
                        float screen_bx = 0.0f, screen_by = 0.0f;
                        if (world_to_screen({base_spread_world.x, base_spread_world.y, base_spread_world.z}, &screen_bx, &screen_by, input.render_view_matrix, window_width, window_height)) {
                            base_target_radius = std::sqrt((screen_bx - screen_cx) * (screen_bx - screen_cx) + (screen_by - screen_cy) * (screen_by - screen_cy));
                        } else {
                            base_target_radius = base_spread * window_height;
                        }
                    }
                    
                    if (spread_circle_smoothed_base_radius <= 0.01f) {
                        spread_circle_smoothed_base_radius = base_target_radius;
                    } else {
                        float interpolation_speed = cfg.esp_spread_circle_animation_speed;
                        spread_circle_smoothed_base_radius += (base_target_radius - spread_circle_smoothed_base_radius) * std::clamp(input.dt * interpolation_speed, 0.0f, 1.0f);
                    }
                    
                    // Round the center coordinates for sharp rendering (rule #2)
                    ImVec2 rounded_center = ImVec2(std::round(cx), std::round(cy));
                    
                    // Calculate inaccuracy ratio for color shifting
                    float inaccuracy = (std::max)(0.0f, current_spread - base_spread);
                    float t = std::clamp(inaccuracy / 0.08f, 0.0f, 1.0f);
                    
                    // Interpolate color from configured color to a warning color (red/orange) based on inaccuracy
                    ImColor final_col;
                    if (t > 0.0f) {
                        float r = cfg.esp_spread_circle_color.r + (1.0f - cfg.esp_spread_circle_color.r) * t;
                        float g = cfg.esp_spread_circle_color.g * (1.0f - t);
                        float b = cfg.esp_spread_circle_color.b * (1.0f - t);
                        final_col = ImColor(r, g, b, cfg.esp_spread_circle_color.a);
                    } else {
                        final_col = ImColor(cfg.esp_spread_circle_color.r, cfg.esp_spread_circle_color.g, cfg.esp_spread_circle_color.b, cfg.esp_spread_circle_color.a);
                    }
                    
                    // Fill color
                    if (cfg.esp_spread_circle_fill_alpha > 0.0f) {
                        float fill_alpha = std::clamp(cfg.esp_spread_circle_fill_alpha * (1.0f + t * 0.5f), 0.0f, 1.0f);
                        ImColor fill_col = ImColor(final_col.Value.x, final_col.Value.y, final_col.Value.z, cfg.esp_spread_circle_color.a * fill_alpha);
                        draw_list->AddCircleFilled(rounded_center, spread_circle_smoothed_radius, fill_col, 64);
                    }
                    
                    // Main spread circle outline
                    if (cfg.esp_spread_circle_outline) {
                        float dynamic_thickness = cfg.esp_spread_circle_thickness + t * 1.0f;
                        draw_list->AddCircle(rounded_center, spread_circle_smoothed_radius, final_col, 64, dynamic_thickness);
                    }
                    
                    // Secondary base spread circle (if enabled)
                    if (cfg.esp_spread_circle_show_base && spread_circle_smoothed_base_radius > 0.0f) {
                        // Base spread circle can be lower alpha to distinguish it
                        ImColor base_col = ImColor(cfg.esp_spread_circle_color.r, cfg.esp_spread_circle_color.g, cfg.esp_spread_circle_color.b, cfg.esp_spread_circle_color.a * 0.35f);
                        draw_list->AddCircle(rounded_center, spread_circle_smoothed_base_radius, base_col, 64, 0.8f);
                    }
                }
            } else {
                // Reset smoothing if dead
                spread_circle_smoothed_radius = 0.0f;
                spread_circle_smoothed_base_radius = 0.0f;
            }
        }



        float font_size = 10.0f;

        static float smoothed_armor[shm::MAX_PLAYERS];
        static float smoothed_ammo[shm::MAX_PLAYERS];
        static bool player_active_last_extra[shm::MAX_PLAYERS] = {false};
        static bool first_init_extra = true;
        if (first_init_extra) {
            std::fill(std::begin(smoothed_armor), std::end(smoothed_armor), -1.0f);
            std::fill(std::begin(smoothed_ammo), std::end(smoothed_ammo), -1.0f);
            first_init_extra = false;
        }

        auto draw_outlined_text = [&](ImDrawList* dl, const char* text, const ImVec2& pos, ImU32 color) {
            float rx = std::round(pos.x);
            float ry = std::round(pos.y);
            dl->AddText(nullptr, font_size, ImVec2(rx + 1.0f, ry + 1.0f), IM_COL32(0, 0, 0, 220), text);
            dl->AddText(nullptr, font_size, ImVec2(rx - 1.0f, ry + 1.0f), IM_COL32(0, 0, 0, 220), text);
            dl->AddText(nullptr, font_size, ImVec2(rx + 1.0f, ry - 1.0f), IM_COL32(0, 0, 0, 220), text);
            dl->AddText(nullptr, font_size, ImVec2(rx - 1.0f, ry - 1.0f), IM_COL32(0, 0, 0, 220), text);
            dl->AddText(nullptr, font_size, ImVec2(rx, ry), color, text);
        };

        for (int i = 0; i < input.packet.player_count; ++i) {
            const auto& player = input.packet.players[i];
            const auto& rp = state.render_palettes[i];
            if (!player.active || !rp.model) {
                player_active_last_extra[i] = false;
                continue;
            }

            bool matches_team = (player.team == input.packet.local_team);
            if (cfg.team_check && matches_team) {
                player_active_last_extra[i] = false;
                continue;
            }
            if (rp.hidden(2)) continue;
            const bool esp_tinted = rp.highlighted(2) || rp.distance_alpha < 1.0f;
            DrawListTint esp_tint(draw_list, rp.highlight_alpha[2] * rp.distance_alpha, rp.tint(2), esp_tinted);

            bool is_visible = false;
            if (i < static_cast<int>(state.vischeck_result.player_visible.size())) {
                is_visible = state.vischeck_result.player_visible[i];
            } else {
                is_visible = true;
            }

            if (is_visible && !cfg.esp_box_hp_visible) continue;
            if (!is_visible && !cfg.esp_box_hp_occluded) continue;

            float bx, by, bw, bh;
            if (get_player_bounds(rp.sanitized_bones, player.origin, input.packet.local_eye,
                                  input.render_view_matrix, window_width, window_height, bx, by, bw, bh, cfg)) {
                
                // Update smoothed armor
                if (!player_active_last_extra[i] || smoothed_armor[i] < 0.0f) {
                    smoothed_armor[i] = static_cast<float>(player.armor);
                    player_active_last_extra[i] = true;
                } else {
                    smoothed_armor[i] += (player.armor - smoothed_armor[i]) * std::clamp(10.0f * input.dt, 0.0f, 1.0f);
                    if (std::abs(player.armor - smoothed_armor[i]) > 50.0f) {
                        smoothed_armor[i] = static_cast<float>(player.armor);
                    }
                }

                // Update smoothed ammo
                float current_ammo_val = player.max_ammo > 0 ? static_cast<float>(player.ammo) : 0.0f;
                if (!player_active_last_extra[i] || smoothed_ammo[i] < 0.0f) {
                    smoothed_ammo[i] = current_ammo_val;
                } else {
                    smoothed_ammo[i] += (current_ammo_val - smoothed_ammo[i]) * std::clamp(10.0f * input.dt, 0.0f, 1.0f);
                    if (std::abs(current_ammo_val - smoothed_ammo[i]) > (player.max_ammo * 0.5f)) {
                        smoothed_ammo[i] = current_ammo_val;
                    }
                }

                // 1. Draw Player Name
                if (cfg.esp_names && player.name[0] != '\0') {
                    ImVec2 tsize = ImGui::CalcTextSize(player.name);
                    float tx = bx + bw * 0.5f - tsize.x * 0.5f;
                    float ty = by - font_size - 2.0f;
                    draw_outlined_text(draw_list, player.name, ImVec2(tx, ty), IM_COL32(255, 255, 255, 255));
                }

                // 2. Draw Armor Bar
                if (cfg.esp_armor_bar && player.armor > 0) {
                    float gap = bw * 0.02f;
                    float bar_width = cfg.esp_armor_bar_thickness;
                    if (bar_width < 1.0f) bar_width = 2.0f;
                    
                    // Left side of health bar: health bar is at bx - bar_width - gap
                    float armor_x = bx - bar_width - gap - bar_width - 2.0f;
                    
                    // Outline background
                    if (cfg.esp_armor_bar_outline) {
                        draw_list->AddRectFilled(ImVec2(armor_x - 1.0f, by - 1.0f), ImVec2(armor_x + bar_width + 1.0f, by + bh + 1.0f), IM_COL32(0, 0, 0, 200));
                    }
                    
                    // Armor bar fill (custom/gradient color)
                    float fill_h = bh * (std::clamp(smoothed_armor[i], 0.0f, 100.0f) / 100.0f);
                    float armor_pct = std::clamp(smoothed_armor[i], 0.0f, 100.0f) / 100.0f;
                    float r, g, b, a;
                    if (cfg.esp_armor_bar_gradient) {
                        r = cfg.esp_armor_bar_gradient_start.r * armor_pct + cfg.esp_armor_bar_gradient_end.r * (1.0f - armor_pct);
                        g = cfg.esp_armor_bar_gradient_start.g * armor_pct + cfg.esp_armor_bar_gradient_end.g * (1.0f - armor_pct);
                        b = cfg.esp_armor_bar_gradient_start.b * armor_pct + cfg.esp_armor_bar_gradient_end.b * (1.0f - armor_pct);
                        a = cfg.esp_armor_bar_gradient_start.a * armor_pct + cfg.esp_armor_bar_gradient_end.a * (1.0f - armor_pct);
                    } else {
                        r = cfg.esp_armor_bar_color.r;
                        g = cfg.esp_armor_bar_color.g;
                        b = cfg.esp_armor_bar_color.b;
                        a = cfg.esp_armor_bar_color.a;
                    }
                    ImU32 armor_col = IM_COL32(
                        static_cast<int>(r * 255.0f),
                        static_cast<int>(g * 255.0f),
                        static_cast<int>(b * 255.0f),
                        static_cast<int>(a * 255.0f)
                    );
                    draw_list->AddRectFilled(ImVec2(armor_x, by + bh - fill_h), ImVec2(armor_x + bar_width, by + bh), armor_col);
                }

                // 2b. Draw Ammo Bar
                float ammo_offset = 0.0f;
                if (cfg.esp_ammo_bar && player.max_ammo > 0) {
                    float gap = bw * 0.02f;
                    float bar_height = cfg.esp_ammo_bar_thickness;
                    if (bar_height < 1.0f) bar_height = 2.0f;
                    float ammo_y = by + bh + gap;
                    
                    // Outline background
                    if (cfg.esp_ammo_bar_outline) {
                        draw_list->AddRectFilled(ImVec2(bx - 1.0f, ammo_y - 1.0f), ImVec2(bx + bw + 1.0f, ammo_y + bar_height + 1.0f), IM_COL32(0, 0, 0, 200));
                    }
                    
                    // Ammo bar fill (custom/gradient color)
                    float ammo_pct = std::clamp(smoothed_ammo[i] / player.max_ammo, 0.0f, 1.0f);
                    float r, g, b, a;
                    if (cfg.esp_ammo_bar_gradient) {
                        r = cfg.esp_ammo_bar_gradient_start.r * ammo_pct + cfg.esp_ammo_bar_gradient_end.r * (1.0f - ammo_pct);
                        g = cfg.esp_ammo_bar_gradient_start.g * ammo_pct + cfg.esp_ammo_bar_gradient_end.g * (1.0f - ammo_pct);
                        b = cfg.esp_ammo_bar_gradient_start.b * ammo_pct + cfg.esp_ammo_bar_gradient_end.b * (1.0f - ammo_pct);
                        a = cfg.esp_ammo_bar_gradient_start.a * ammo_pct + cfg.esp_ammo_bar_gradient_end.a * (1.0f - ammo_pct);
                    } else {
                        r = cfg.esp_ammo_bar_color.r;
                        g = cfg.esp_ammo_bar_color.g;
                        b = cfg.esp_ammo_bar_color.b;
                        a = cfg.esp_ammo_bar_color.a;
                    }
                    ImU32 ammo_col = IM_COL32(
                        static_cast<int>(r * 255.0f),
                        static_cast<int>(g * 255.0f),
                        static_cast<int>(b * 255.0f),
                        static_cast<int>(a * 255.0f)
                    );
                    draw_list->AddRectFilled(ImVec2(bx, ammo_y), ImVec2(bx + bw * ammo_pct, ammo_y + bar_height), ammo_col);
                    
                    ammo_offset = gap + bar_height + (cfg.esp_ammo_bar_outline ? 2.0f : 0.0f);
                }

                // 3. Draw Flags
                if (cfg.esp_flags) {
                    ImU32 flags_col = IM_COL32(
                        static_cast<int>(cfg.esp_flags_color.r * 255.0f),
                        static_cast<int>(cfg.esp_flags_color.g * 255.0f),
                        static_cast<int>(cfg.esp_flags_color.b * 255.0f),
                        static_cast<int>(cfg.esp_flags_color.a * 255.0f)
                    );
                    std::vector<std::pair<std::string, ImU32>> flags;
                    if (player.is_reloading && cfg.esp_flags_reload) {
                        flags.push_back({"RELOAD", flags_col});
                    }
                    if (player.is_scoped && cfg.esp_flags_scoped) {
                        flags.push_back({"SCOPE", flags_col});
                    }
                    if (player.has_bomb && cfg.esp_flags_bomb) {
                        ImU32 bomb_col = IM_COL32(
                            static_cast<int>(cfg.esp_flags_bomb_color.r * 255.0f),
                            static_cast<int>(cfg.esp_flags_bomb_color.g * 255.0f),
                            static_cast<int>(cfg.esp_flags_bomb_color.b * 255.0f),
                            static_cast<int>(cfg.esp_flags_bomb_color.a * 255.0f)
                        );
                        flags.push_back({"BOMB", bomb_col});
                    }
                    if (player.flash_alpha > 200.0f && cfg.esp_flags_flashed) {
                        ImU32 flashed_col = IM_COL32(
                            static_cast<int>(cfg.esp_flags_flashed_color.r * 255.0f),
                            static_cast<int>(cfg.esp_flags_flashed_color.g * 255.0f),
                            static_cast<int>(cfg.esp_flags_flashed_color.b * 255.0f),
                            static_cast<int>(cfg.esp_flags_flashed_color.a * 255.0f)
                        );
                        flags.push_back({"FLASHED", flashed_col});
                    }
                    if (player.has_defuser && cfg.esp_flags_defuser) {
                        flags.push_back({"DEF", flags_col});
                    }
                    if (player.has_helmet && cfg.esp_flags_helmet) {
                        flags.push_back({"HK", flags_col});
                    } else if (player.armor > 0 && cfg.esp_flags_kevlar) {
                        flags.push_back({"K", flags_col});
                    }
                    if (cfg.esp_flags_distance) {
                        int dist_m = static_cast<int>(std::round(distance::meters_between(player.origin, input.packet.local_eye)));
                        flags.push_back({std::to_string(dist_m) + "m", flags_col});
                    }
                    if (cfg.esp_flags_hp) {
                        flags.push_back({std::to_string(player.health) + "HP", flags_col});
                    }

                    float fx = bx + bw + 4.0f;
                    float fy = by;
                    for (const auto& flag : flags) {
                        draw_outlined_text(draw_list, flag.first.c_str(), ImVec2(fx, fy), flag.second);
                        fy += font_size + 1.0f;
                    }
                }

                // 4. Draw Weapon SVG Icons
                if (cfg.esp_weapon_icons && player.active_weapon_name[0] != '\0') {
                    SvgTexture tex = svg_cache.get_weapon_texture(player.active_weapon_name);
                    if (tex.id != 0 && tex.height > 0.0f && tex.width > 0.0f) {
                        float aspect = tex.width / tex.height;
                        float base_w = std::clamp(bw * 0.5f, 24.0f, 64.0f);
                        float icon_w = base_w;
                        float icon_h = icon_w / aspect;
                        
                        float max_h = std::clamp(bh * 0.1f, 14.0f, 28.0f);
                        if (icon_h > max_h) {
                            icon_h = max_h;
                            icon_w = icon_h * aspect;
                        }
                        float icon_x = std::round(bx + bw * 0.5f - icon_w * 0.5f);
                        float icon_y = std::round(by + bh + 4.0f + ammo_offset);
                        
                        const float icon_opacity = std::clamp(cfg.weapon_icon_opacity, 0.0f, 1.0f);
                        const Color& icon_bg = cfg.weapon_icon_background;
                        if (icon_bg[3] > 0.0f) {
                            draw_list->AddRectFilled(ImVec2(icon_x - 2.0f, icon_y - 2.0f),
                                                     ImVec2(icon_x + icon_w + 2.0f, icon_y + icon_h + 2.0f),
                                                     ImGui::ColorConvertFloat4ToU32(ImVec4(icon_bg[0], icon_bg[1], icon_bg[2], icon_bg[3])),
                                                     2.0f);
                        }
                        // Draw 4-way black outline
                        ImU32 outline_col = IM_COL32(0, 0, 0, static_cast<int>(225.0f * icon_opacity));
                        draw_list->AddImage((ImTextureID)(intptr_t)tex.id, ImVec2(icon_x + 1.0f, icon_y), ImVec2(icon_x + icon_w + 1.0f, icon_y + icon_h), ImVec2(0,0), ImVec2(1,1), outline_col);
                        draw_list->AddImage((ImTextureID)(intptr_t)tex.id, ImVec2(icon_x - 1.0f, icon_y), ImVec2(icon_x + icon_w - 1.0f, icon_y + icon_h), ImVec2(0,0), ImVec2(1,1), outline_col);
                        draw_list->AddImage((ImTextureID)(intptr_t)tex.id, ImVec2(icon_x, icon_y + 1.0f), ImVec2(icon_x + icon_w, icon_y + icon_h + 1.0f), ImVec2(0,0), ImVec2(1,1), outline_col);
                        draw_list->AddImage((ImTextureID)(intptr_t)tex.id, ImVec2(icon_x, icon_y - 1.0f), ImVec2(icon_x + icon_w, icon_y + icon_h - 1.0f), ImVec2(0,0), ImVec2(1,1), outline_col);
                        
                        // Draw main white weapon icon
                        draw_list->AddImage((ImTextureID)(intptr_t)tex.id, ImVec2(icon_x, icon_y), ImVec2(icon_x + icon_w, icon_y + icon_h), ImVec2(0,0), ImVec2(1,1), IM_COL32(255, 255, 255, static_cast<int>(230.0f * icon_opacity)));
                    }
                }

                if (cfg.trigger_show_hitboxes) {
                    static const int bone_ids[] = { 1, 6, 7, 9, 10, 11, 13, 14, 15, 17, 18, 19, 20, 21, 22 };
                    uint16_t weapon_id = input.packet.held_weapon_id;
                    WeaponGroup w_group = get_weapon_group(weapon_id);
                    int active_hitbox_groups = 63;
                    if (w_group == WEAPON_GROUP_LIGHT_PISTOL) {
                        active_hitbox_groups = cfg.trigger_light_pistols_hitbox_groups;
                    } else if (w_group == WEAPON_GROUP_HEAVY_PISTOL) {
                        active_hitbox_groups = cfg.trigger_heavy_pistols_hitbox_groups;
                    } else if (w_group == WEAPON_GROUP_RIFLE) {
                        active_hitbox_groups = cfg.trigger_rifles_hitbox_groups;
                    } else if (w_group == WEAPON_GROUP_SMG) {
                        active_hitbox_groups = cfg.trigger_smgs_hitbox_groups;
                    } else if (w_group == WEAPON_GROUP_SCOUT) {
                        active_hitbox_groups = cfg.trigger_scout_hitbox_groups;
                    } else if (w_group == WEAPON_GROUP_AWP) {
                        active_hitbox_groups = cfg.trigger_awp_hitbox_groups;
                    } else if (w_group == WEAPON_GROUP_SNIPER) {
                        active_hitbox_groups = cfg.trigger_auto_snipers_hitbox_groups;
                    } else if (w_group == WEAPON_GROUP_SHOTGUN) {
                        active_hitbox_groups = cfg.trigger_shotguns_hitbox_groups;
                    } else if (w_group == WEAPON_GROUP_MG) {
                        active_hitbox_groups = cfg.trigger_lmgs_hitbox_groups;
                    } else {
                        active_hitbox_groups = cfg.trigger_others_hitbox_groups;
                    }

                    for (int b_id : bone_ids) {
                        if (!is_hitbox_group_enabled(b_id, active_hitbox_groups)) continue;

                        if (b_id < static_cast<int>(rp.sanitized_bones.size())) {
                            Vec3 bone_world = rp.sanitized_bones[b_id];
                            float sx, sy;
                            if (world_to_screen(bone_world, &sx, &sy, input.render_view_matrix, window_width, window_height)) {
                                char text_buf[16];
                                std::snprintf(text_buf, sizeof(text_buf), "%d", b_id);
                                draw_outlined_text(draw_list, text_buf, ImVec2(sx, sy), IM_COL32(255, 255, 255, 255));
                            }
                        }
                    }
                }
            } else if (cfg.esp_offscreen_arrows) {
                // Player has no on-screen bounds: draw an edge arrow pointing at them
                Vec3 arrow_target = player.origin;
                arrow_target.z += 40.0f; // torso height for a stable direction

                const float* vm = input.render_view_matrix;
                float proj_w = vm[12] * arrow_target.x + vm[13] * arrow_target.y + vm[14] * arrow_target.z + vm[15];
                float proj_x = vm[0] * arrow_target.x + vm[1] * arrow_target.y + vm[2] * arrow_target.z + vm[3];
                float proj_y = vm[4] * arrow_target.x + vm[5] * arrow_target.y + vm[6] * arrow_target.z + vm[7];
                bool behind = (proj_w < 0.0f);
                if (std::fabs(proj_w) < 1e-4f) {
                    proj_w = behind ? -1e-4f : 1e-4f;
                }
                float ndc_x = proj_x / proj_w;
                float ndc_y = proj_y / proj_w;
                if (behind) {
                    ndc_x = -ndc_x;
                    ndc_y = -ndc_y;
                }
                float screen_cx = window_width * 0.5f;
                float screen_cy = window_height * 0.5f;
                float arrow_sx = screen_cx + ndc_x * screen_cx;
                float arrow_sy = screen_cy - ndc_y * screen_cy;

                bool offscreen = behind || arrow_sx < 0.0f || arrow_sx > window_width || arrow_sy < 0.0f || arrow_sy > window_height;
                if (offscreen) {
                    float dist_dx = player.origin.x - input.packet.local_eye.x;
                    float dist_dy = player.origin.y - input.packet.local_eye.y;
                    float dist_dz = player.origin.z - input.packet.local_eye.z;
                    float dist_m = std::sqrt(dist_dx * dist_dx + dist_dy * dist_dy + dist_dz * dist_dz) / 39.37f;
                    if (cfg.esp_offscreen_arrows_max_dist <= 0.0f || dist_m <= cfg.esp_offscreen_arrows_max_dist) {
                        float dir_x = arrow_sx - screen_cx;
                        float dir_y = arrow_sy - screen_cy;
                        float dir_len = std::sqrt(dir_x * dir_x + dir_y * dir_y);
                        if (dir_len > 1e-3f) {
                            dir_x /= dir_len;
                            dir_y /= dir_len;
                            // Place the arrow on a ring at a fixed distance from the crosshair
                            float radius = cfg.esp_offscreen_arrows_radius;
                            float edge_x = std::round(screen_cx + dir_x * radius);
                            float edge_y = std::round(screen_cy + dir_y * radius);

                            // Exposure tint: switch to the exposed color when this enemy can see us
                            bool exposure_known = i < static_cast<int>(state.vischeck_result.player_visible.size());
                            bool exposed = exposure_known && state.vischeck_result.player_visible[i];
                            const Color& base_col = exposed ? cfg.esp_offscreen_arrows_exposed_color : cfg.esp_offscreen_arrows_color;

                            // Fade distant arrows instead of popping at the max range
                            float fade = std::clamp(1.0f - (dist_m - 35.0f) / 55.0f, 0.4f, 1.0f);
                            ImU32 arrow_col = IM_COL32(
                                static_cast<int>(base_col.r * 255.0f),
                                static_cast<int>(base_col.g * 255.0f),
                                static_cast<int>(base_col.b * 255.0f),
                                static_cast<int>(base_col.a * 255.0f * fade)
                            );

                            float size = cfg.esp_offscreen_arrows_size;
                            const SvgTexture chevron =
                                svg_cache.get_weapon_texture("icons/ui/lucide/chevron-right");
                            auto draw_chevron_icon = [&](float half_extent, ImU32 color) {
                                const float axis_x = dir_x * half_extent;
                                const float axis_y = dir_y * half_extent;
                                const float perp_x = -dir_y * half_extent;
                                const float perp_y = dir_x * half_extent;
                                const ImVec2 top_left(
                                    std::round(edge_x - axis_x - perp_x),
                                    std::round(edge_y - axis_y - perp_y)
                                );
                                const ImVec2 top_right(
                                    std::round(edge_x + axis_x - perp_x),
                                    std::round(edge_y + axis_y - perp_y)
                                );
                                const ImVec2 bottom_right(
                                    std::round(edge_x + axis_x + perp_x),
                                    std::round(edge_y + axis_y + perp_y)
                                );
                                const ImVec2 bottom_left(
                                    std::round(edge_x - axis_x + perp_x),
                                    std::round(edge_y - axis_y + perp_y)
                                );
                                draw_list->AddImageQuad(
                                    reinterpret_cast<ImTextureID>(static_cast<intptr_t>(chevron.id)),
                                    top_left, top_right, bottom_right, bottom_left,
                                    ImVec2(0.0f, 0.0f), ImVec2(1.0f, 0.0f),
                                    ImVec2(1.0f, 1.0f), ImVec2(0.0f, 1.0f), color
                                );
                            };

                            if (chevron.id != 0) {
                                // Draw the same clean SVG slightly larger as a contrast outline.
                                draw_chevron_icon(
                                    size * 1.65f,
                                    IM_COL32(0, 0, 0, static_cast<int>(200.0f * fade))
                                );
                                draw_chevron_icon(size * 1.5f, arrow_col);
                            }

                            if (cfg.esp_offscreen_arrows_distance) {
                                char dist_buf[16];
                                std::snprintf(dist_buf, sizeof(dist_buf), "%dm", static_cast<int>(std::round(dist_m)));
                                ImVec2 dist_text_size = ImGui::CalcTextSize(dist_buf);
                                // Pull the label back toward screen center so it stays inside the margin
                                float label_x = std::round(edge_x - dir_x * (size + 6.0f) - dist_text_size.x * 0.5f);
                                float label_y = std::round(edge_y - dir_y * (size + 6.0f) - dist_text_size.y * 0.5f);
                                draw_outlined_text(draw_list, dist_buf, ImVec2(label_x, label_y), arrow_col);
                            }
                        }
                    }
                }
            }
        }
    }

    crosshair_overlay.render(ImGui::GetBackgroundDrawList(), input.packet, this->cfg,
                             window_width, window_height);

    if (cfg.esp_crosshair_distance) {
        crosshair_distance.render(ImGui::GetBackgroundDrawList(), input, cfg, visibility_worker,
                                  window_width, window_height);
    }

    // 7b. Draw Dropped Items ESP (text and icons only)
    if (cfg.esp_dropped_items && input.packet.dropped_item_count > 0) {
        ImDrawList* draw_list = ImGui::GetBackgroundDrawList();
        auto draw_outlined_text_local = [&](ImDrawList* dl, const char* text, const ImVec2& pos, ImU32 color, ImU32 outline_color) {
            ImU32 outline_col = outline_color;
            dl->AddText(ImVec2(pos.x + 1.0f, pos.y + 1.0f), outline_col, text);
            dl->AddText(ImVec2(pos.x - 1.0f, pos.y + 1.0f), outline_col, text);
            dl->AddText(ImVec2(pos.x + 1.0f, pos.y - 1.0f), outline_col, text);
            dl->AddText(ImVec2(pos.x - 1.0f, pos.y - 1.0f), outline_col, text);
            dl->AddText(pos, color, text);
        };

        auto get_display_name = [](const std::string& raw) -> std::string {
            static const std::unordered_map<std::string, std::string> clean_names = {
                {"ak47", "AK-47"}, {"aug", "AUG"}, {"awp", "AWP"}, {"bizon", "PP-Bizon"},
                {"cz75a", "CZ75-Auto"}, {"deagle", "Desert Eagle"}, {"elite", "Dual Berettas"},
                {"famas", "FAMAS"}, {"fiveseven", "Five-SeveN"}, {"g3sg1", "G3SG1"},
                {"galilar", "Galil AR"}, {"glock", "Glock-18"}, {"hkp2000", "P2000"},
                {"m249", "M249"}, {"m4a1", "M4A4"}, {"m4a1_silencer", "M4A1-S"},
                {"mac10", "MAC-10"}, {"mag7", "MAG-7"}, {"mp7", "MP7"}, {"mp9", "MP9"},
                {"negev", "Negev"}, {"nova", "Nova"}, {"p250", "P250"}, {"p90", "P90"},
                {"revolver", "R8 Revolver"}, {"sawedoff", "Sawed-Off"}, {"scar20", "SCAR-20"},
                {"sg556", "SG 553"}, {"ssg08", "SSG 08"}, {"tec9", "Tec-9"}, {"ump45", "UMP-45"},
                {"usp_silencer", "USP-S"}, {"xm1014", "XM1014"}, {"hegrenade", "HE Grenade"},
                {"flashbang", "Flashbang"}, {"smokegrenade", "Smoke Grenade"},
                {"incgrenade", "Incendiary Grenade"}, {"molotov", "Molotov"}, {"decoy", "Decoy Grenade"},
                {"c4", "C4 Explosive"}
            };
            auto it = clean_names.find(raw);
            return (it != clean_names.end()) ? it->second : raw;
        };

        for (int i = 0; i < input.packet.dropped_item_count; ++i) {
            const auto& item = input.packet.dropped_items[i];
            if (!item.active) continue;

            // Distance check
            float dx = item.origin.x - input.packet.local_eye.x;
            float dy = item.origin.y - input.packet.local_eye.y;
            float dz = item.origin.z - input.packet.local_eye.z;
            float dist = std::sqrt(dx*dx + dy*dy + dz*dz);
            if (dist > cfg.esp_dropped_items_max_dist) continue;

            // Calculate fade factor starting at 80% of max distance
            float fade_alpha = 1.0f;
            float fade_start = cfg.esp_dropped_items_max_dist * 0.8f;
            if (dist > fade_start) {
                fade_alpha = 1.0f - ((dist - fade_start) / (cfg.esp_dropped_items_max_dist - fade_start));
                fade_alpha = std::clamp(fade_alpha, 0.0f, 1.0f);
            }

            ImU32 text_col = ImGui::ColorConvertFloat4ToU32(ImVec4(
                cfg.esp_dropped_items_color.r,
                cfg.esp_dropped_items_color.g,
                cfg.esp_dropped_items_color.b,
                cfg.esp_dropped_items_color.a * fade_alpha
            ));

            ImU32 outline_col = IM_COL32(0, 0, 0, static_cast<int>(220.0f * fade_alpha));
            ImU32 outline_icon_col = IM_COL32(0, 0, 0, static_cast<int>(225.0f * fade_alpha));
            ImU32 main_icon_col = IM_COL32(255, 255, 255, static_cast<int>(230.0f * fade_alpha));

            float sx = 0.0f, sy = 0.0f;
            if (world_to_screen(item.origin, &sx, &sy, input.render_view_matrix, window_width, window_height)) {
                float y_offset = 0.0f;
                
                // Draw Icon
                if (cfg.esp_dropped_items_icons) {
                    SvgTexture tex = svg_cache.get_weapon_texture(item.name);
                    if (tex.id != 0) {
                        float aspect = tex.width / tex.height;
                        float icon_w = 40.0f;
                        float icon_h = icon_w / aspect;
                        
                        // Limit size if it gets too tall
                        if (icon_h > 20.0f) {
                            icon_h = 20.0f;
                            icon_w = icon_h * aspect;
                        }
                        
                        float ix = std::round(sx - icon_w * 0.5f);
                        float iy = std::round(sy - icon_h * 0.5f);

                        // Draw icon outline
                        draw_list->AddImage((ImTextureID)(intptr_t)tex.id, ImVec2(ix + 1.0f, iy), ImVec2(ix + icon_w + 1.0f, iy + icon_h), ImVec2(0,0), ImVec2(1,1), outline_icon_col);
                        draw_list->AddImage((ImTextureID)(intptr_t)tex.id, ImVec2(ix - 1.0f, iy), ImVec2(ix + icon_w - 1.0f, iy + icon_h), ImVec2(0,0), ImVec2(1,1), outline_icon_col);
                        draw_list->AddImage((ImTextureID)(intptr_t)tex.id, ImVec2(ix, iy + 1.0f), ImVec2(ix + icon_w, iy + icon_h + 1.0f), ImVec2(0,0), ImVec2(1,1), outline_icon_col);
                        draw_list->AddImage((ImTextureID)(intptr_t)tex.id, ImVec2(ix, iy - 1.0f), ImVec2(ix + icon_w, iy + icon_h - 1.0f), ImVec2(0,0), ImVec2(1,1), outline_icon_col);

                        // Draw main icon
                        draw_list->AddImage((ImTextureID)(intptr_t)tex.id, ImVec2(ix, iy), ImVec2(ix + icon_w, iy + icon_h), ImVec2(0,0), ImVec2(1,1), main_icon_col);
                        
                        y_offset = icon_h * 0.5f + 2.0f;
                    }
                }

                // Draw Text
                if (cfg.esp_dropped_items_text) {
                    std::string disp_name = get_display_name(item.name);
                    ImVec2 text_size = ImGui::GetFont()->CalcTextSizeA(13.0f, FLT_MAX, 0.0f, disp_name.c_str());
                    ImVec2 text_pos(sx - text_size.x * 0.5f, sy + y_offset);
                    draw_outlined_text_local(draw_list, disp_name.c_str(), text_pos, text_col, outline_col);
                }
            }
        }
    }


    // 8. Draw 2D FPS Counter Overlay
    if (cfg.show_fps) {
        static int frame_count = 0;
        static double last_fps_time = 0.0;
        static int current_fps = 0;

        double current_time = glfwGetTime();
        frame_count++;
        if (current_time - last_fps_time >= 1.0) {
            current_fps = frame_count;
            frame_count = 0;
            last_fps_time = current_time;
        }
        overlay->draw_fps(current_fps, cfg.fps_scale);
    }

    if (cfg.esp_bomb_bar) {
        draw_static_bomb_bar(input);
    }

    if (cfg.kb_viewer_enabled) {
        draw_keybinds_viewer(input.dt);
    }

    if (cfg.spec_list_enabled) {
        draw_spectators_viewer(input.dt);
    }

    update_hit_effects(input, state, cfg, ImGui::GetBackgroundDrawList());

    // --- Fortnite 3D Hitmarkers & Kill FX ---
    {
        double current_time = glfwGetTime();

        // 1. Reset / Initialization checks
        if (previous_total_damage < 0.0f || input.packet.local_total_damage < previous_total_damage) {
            previous_total_damage = input.packet.local_total_damage;
        }
        // An unreadable counter is zeroed in the packet: it neither resets nor counts kills.
        const bool kills_valid = input.packet.local_kills_valid != 0;
        if (kills_valid && (previous_total_kills < 0 || input.packet.local_total_kills < previous_total_kills)) {
            previous_total_kills = input.packet.local_total_kills;
        }
        if (kills_valid && (previous_total_kills_headshots < 0 || input.packet.local_total_kills_headshots < previous_total_kills_headshots)) {
            previous_total_kills_headshots = input.packet.local_total_kills_headshots;
        }

        // Build map of currently active enemy players
        std::unordered_map<std::string, const PlayerData*> active_enemies;
        for (int i = 0; i < input.packet.player_count; ++i) {
            const auto& player = input.packet.players[i];
            if (player.active && player.team != input.packet.local_team && player.name[0] != '\0') {
                active_enemies[player.name] = &player;
            }
        }

        // 2. Track health drops and spawn 3D hitmarkers directly from HP changes
        for (const auto& pair : active_enemies) {
            const std::string& name = pair.first;
            const auto& player = *pair.second;

            auto it = previous_player_health.find(name);
            if (it != previous_player_health.end()) {
                int prev_hp = it->second;
                if (prev_hp > 0 && player.health < prev_hp) {
                    BUNNYWARE_LOG_INFO("HP drop on '{}': {} -> {}", name, prev_hp, player.health);

                    // Spawn 3D hitmarker at this player's position
                    if (cfg.esp_enabled && cfg.hitmarker) {
                        Vec3 hit_pos = player.origin + Vec3(0.0f, 0.0f, 45.0f);
                        for (int idx = 0; idx < input.packet.player_count; ++idx) {
                            if (std::string(input.packet.players[idx].name) == name) {
                                const auto& rp = state.render_palettes[idx];
                                if (rp.model && 7 < static_cast<int>(rp.sanitized_bones.size())) {
                                    hit_pos = rp.sanitized_bones[6]; // Chest/Neck bone
                                }
                                break;
                            }
                        }

                        ActiveEffect3D fx;
                        fx.world_pos = hit_pos;
                        fx.is_2d = false;
                        fx.type = ActiveEffect3D::TYPE_HITMARKER;
                        fx.start_time = current_time;
                        fx.duration = 0.8; // 800ms duration for damage text popup
                        int damage = prev_hp - player.health;
                        fx.text = "-" + std::to_string(damage);

                        // Offset the damage text horizontally to render it off of the player's body
                        float offset_x = 45.0f + static_cast<float>(rand() % 15);
                        float offset_y = -20.0f - static_cast<float>(rand() % 15);
                        if (rand() % 2 == 0) {
                            offset_x = -offset_x;
                        }
                        fx.screen_offset_x = offset_x;
                        fx.screen_offset_y = offset_y;

                        bool is_kill_shot = (player.health <= 0);
                        if (is_kill_shot) {
                            fx.color = IM_COL32(255, 30, 30, 255); // Red on kill
                        } else {
                            fx.color = IM_COL32(245, 245, 245, 255); // White on body hit
                        }
                        fx.is_headshot = false;
                        active_effects_3d.push_back(fx);
                        BUNNYWARE_LOG_INFO("Spawned 3D damage popup '{}' on '{}' (offset x: {}, y: {}) at ({}, {}, {})", fx.text, name, offset_x, offset_y, hit_pos.x, hit_pos.y, hit_pos.z);
                    }
                }
            } else {
                BUNNYWARE_LOG_INFO("Player '{}' entered tracking with HP {}", name, player.health);
            }

            previous_player_health[name] = player.health;

            // Store head position if available, fallback to origin
            Vec3 current_pos = player.origin;
            for (int idx = 0; idx < input.packet.player_count; ++idx) {
                if (std::string(input.packet.players[idx].name) == name) {
                    const auto& rp = state.render_palettes[idx];
                    if (rp.model && 7 < static_cast<int>(rp.sanitized_bones.size())) {
                        current_pos = rp.sanitized_bones[7]; // Head bone
                    }
                    if (rp.model) {
                        PlayerFrameCache pcache;
                        pcache.vao = rp.model->vao;
                        pcache.ibo = rp.model->ibo;
                        pcache.index_count = rp.model->index_count;
                        pcache.skinning_palette = rp.skinning_palette;
                        pcache.position = player.origin;
                        player_frame_caches[name] = pcache;
                    }
                    break;
                }
            }
            last_known_position[name] = current_pos;
        }

        // 3. Enemies gone from the packet since last frame (killed, disconnected or a dropped
        // read) wait as kill victims with their last drawable state, for the kill counter.
        for (auto it = previous_player_health.begin(); it != previous_player_health.end(); ) {
            if (active_enemies.find(it->first) != active_enemies.end()) { ++it; continue; }
            if (it->second > 0) {
                KillVictim victim;
                if (auto pos_it = last_known_position.find(it->first); pos_it != last_known_position.end())
                    victim.death_pos = pos_it->second + Vec3(0.0f, 0.0f, 15.0f);
                if (auto cache_it = player_frame_caches.find(it->first); cache_it != player_frame_caches.end())
                    victim.cache = std::move(cache_it->second);
                kill_victims_.record_death(it->first, std::move(victim), current_time);
                BUNNYWARE_LOG_INFO("Player '{}' left the packet alive; waiting as a kill victim", it->first);
            }
            last_known_position.erase(it->first);
            player_frame_caches.erase(it->first);
            it = previous_player_health.erase(it);
        }
        for (const auto& pair : active_enemies) {
            const std::string& name = pair.first;
            const auto& player = *pair.second;
            if (player.health > 0) {
                if (kill_victims_.pending_deaths()) kill_victims_.forget(name);
                continue;
            }
            // A dead player still in the packet (the reader normally drops them first).
            if (kill_victims_.waiting(name)) continue;
            KillVictim victim;
            victim.death_pos = player.origin + Vec3(0.0f, 0.0f, 65.0f);
            for (int idx = 0; idx < input.packet.player_count; ++idx) {
                if (std::string(input.packet.players[idx].name) == name) {
                    const auto& rp = state.render_palettes[idx];
                    if (rp.model && 7 < static_cast<int>(rp.sanitized_bones.size())) {
                        victim.death_pos = rp.sanitized_bones[7];
                        victim.death_pos.z += 10.0f;
                    }
                    break;
                }
            }
            if (auto cache_it = player_frame_caches.find(name); cache_it != player_frame_caches.end())
                victim.cache = cache_it->second;
            kill_victims_.record_death(name, std::move(victim), current_time);
        }

        // 4. Kill count change detection (kill text FX). The counter and the victim's
        // disappearance may reach the app in different frames; the matcher pairs them.
        if (kills_valid && input.packet.local_total_kills > previous_total_kills && input.packet.local_total_kills > 0) {
            BUNNYWARE_LOG_INFO("Kill event detected: total_kills increased from {} to {}", previous_total_kills, input.packet.local_total_kills);
            kill_victims_.record_kills(input.packet.local_total_kills - previous_total_kills,
                input.packet.local_total_kills_headshots - previous_total_kills_headshots, current_time);
            previous_total_kills = input.packet.local_total_kills;
            previous_total_kills_headshots = input.packet.local_total_kills_headshots;
        }
        for (auto& kill : kill_victims_.take(current_time)) {
            const std::string& victim_name = kill.name;
            const Vec3 death_pos = kill.victim.death_pos;
            const PlayerFrameCache* victim_cache = kill.victim.cache ? &*kill.victim.cache : nullptr;
            BUNNYWARE_LOG_INFO("Correlated kill with disappeared player '{}'", victim_name);
            if (cfg.kill_effect != KillFxModeEnum::DISABLED && (kill.headshot ? cfg.kill_headshot_text : cfg.kill_show_text)) {
                BUNNYWARE_LOG_INFO("Eliminated player detected: '{}'", victim_name);
                const bool is_headshot = kill.headshot;

                ActiveEffect3D fx;
                fx.world_pos = death_pos;
                fx.is_2d = false;
                fx.type = ActiveEffect3D::TYPE_KILL;
                fx.start_time = current_time;
                fx.duration = 1.5;
                fx.text = is_headshot ? "HEADSHOT" : "KILL";
                fx.color = IM_COL32(255, 40, 40, 255); // Red
                fx.is_headshot = is_headshot;
                active_effects_3d.push_back(fx);
            }

            // Trigger mode-specific effect
            if (cfg.kill_effect == KillFxModeEnum::CHAMS) {
                if (victim_cache) {
                    DeadPlayerCham cham;
                    cham.vao = victim_cache->vao;
                    cham.ibo = victim_cache->ibo;
                    cham.index_count = victim_cache->index_count;
                    cham.skinning_palette = victim_cache->skinning_palette;
                    cham.position = victim_cache->position;
                    cham.kill_time = current_time;
                    cham.fade_time = cfg.kill_chams_fade_time;
                    cham.style_id = get_style_id(cfg.kill_chams_style);
                    std::memcpy(cham.color, cfg.kill_chams_color, sizeof(float) * 4);
                    std::memcpy(cham.secondary_color, cfg.kill_chams_color_sec, sizeof(float) * 4);
                    active_kill_chams.push_back(cham);
                    BUNNYWARE_LOG_INFO("Spawned Dead Player Cham for '{}'", victim_name);
                }
            }
            else if (cfg.kill_effect == KillFxModeEnum::PARTICLES) {
                Vec3 spawn_pos = death_pos - Vec3(0.0f, 0.0f, 35.0f); // Default to chest height by shifting down from head
                if (victim_cache) {
                    spawn_pos = victim_cache->position + Vec3(0.0f, 0.0f, 35.0f); // Torso height from cached origin
                }
                damagefx::ParticleSettings settings;
                settings.color = cfg.kill_particles_color;
                settings.count = cfg.kill_particles_count;
                settings.speed = cfg.kill_particles_speed;
                settings.lifetime = cfg.kill_particles_lifetime;
                settings.size = cfg.kill_particles_size;
                kill_fx_effects.spawn_particles(spawn_pos, settings);
                BUNNYWARE_LOG_INFO("Spawned {} particles for '{}'", settings.count, victim_name);
            }
            else if (cfg.kill_effect == KillFxModeEnum::PULSE) {
                damagefx::PulseSettings settings;
                settings.color = cfg.kill_pulse_color;
                settings.duration = cfg.kill_pulse_duration;
                settings.intensity = cfg.kill_pulse_intensity;
                kill_fx_effects.spawn_pulse(settings);
                BUNNYWARE_LOG_INFO("Triggered Screen Pulse for '{}'", victim_name);
            }
            else if (cfg.kill_effect == KillFxModeEnum::LIGHTNING) {
                Vec3 base_target = death_pos - Vec3(0.0f, 0.0f, 35.0f); // Default to chest/torso
                if (victim_cache) {
                    base_target = victim_cache->position + Vec3(0.0f, 0.0f, 35.0f); // Torso position
                }
                damagefx::LightningSettings settings;
                settings.color = cfg.kill_lightning_color;
                settings.duration = cfg.kill_lightning_duration;
                settings.width = cfg.kill_lightning_width;
                settings.count = cfg.kill_lightning_count;
                kill_fx_effects.spawn_lightning(base_target, settings);
                BUNNYWARE_LOG_INFO("Spawned {} Lightning Strikes for '{}'", settings.count, victim_name);
            }
            else if (cfg.kill_effect == KillFxModeEnum::SHOCKWAVE) {
                Vec3 shockwave_center;
                if (victim_cache) {
                    shockwave_center = victim_cache->position; // Feet height
                } else {
                    shockwave_center = death_pos - Vec3(0.0f, 0.0f, 70.0f); // Shift down from head height to feet
                }
                damagefx::ShockwaveSettings settings;
                settings.color = cfg.kill_shockwave_color;
                settings.duration = cfg.kill_shockwave_duration;
                settings.max_radius = cfg.kill_shockwave_max_radius;
                settings.width = cfg.kill_shockwave_width;
                kill_fx_effects.spawn_shockwave(shockwave_center, settings);
                BUNNYWARE_LOG_INFO("Spawned Shockwave Ring for '{}' at ({}, {}, {})", victim_name, shockwave_center.x, shockwave_center.y, shockwave_center.z);
            }
        }

        // Clean up active kill chams that have expired
        for (auto it = active_kill_chams.begin(); it != active_kill_chams.end(); ) {
            if (current_time - it->kill_time >= it->fade_time) {
                it = active_kill_chams.erase(it);
            } else {
                ++it;
            }
        }

        // 5. Update and Draw active effects
        ImDrawList* draw_list = ImGui::GetBackgroundDrawList();

        for (auto it = active_effects_3d.begin(); it != active_effects_3d.end(); ) {
            double elapsed = current_time - it->start_time;
            if (elapsed >= it->duration || elapsed < 0.0) {
                it = active_effects_3d.erase(it);
                continue;
            }

            // Hitmarkers belong to the ESP overlay: hide them immediately when
            // the ESP or hitmarker toggle is off, even mid-popup.
            if (it->type == ActiveEffect3D::TYPE_HITMARKER && !(cfg.esp_enabled && cfg.hitmarker)) {
                ++it;
                continue;
            }

            if (!it->is_2d && is_beyond_esp_distance(
                    distance::meters_between(it->world_pos, input.packet.local_eye), cfg)) {
                ++it;
                continue;
            }

            float screen_x = 0.0f;
            float screen_y = 0.0f;
            bool should_draw = false;

            if (it->is_2d) {
                screen_x = window_width * 0.5f;
                screen_y = window_height * 0.5f;
                should_draw = true;
            } else {
                // Float up the effect in 3D world space
                if (it->type == ActiveEffect3D::TYPE_KILL) {
                    it->world_pos.z += static_cast<float>(input.dt * 15.0f); // Float up 15 units/sec
                } else if (it->type == ActiveEffect3D::TYPE_HITMARKER) {
                    it->world_pos.z += static_cast<float>(input.dt * 20.0f); // Float up 20 units/sec
                }
                should_draw = world_to_screen(it->world_pos, &screen_x, &screen_y, input.render_view_matrix, window_width, window_height);
            }

            if (should_draw) {
                // Apply screen-space offsets to render the text off of the player
                screen_x += it->screen_offset_x;
                screen_y += it->screen_offset_y;

                float alpha = 1.0f - static_cast<float>(elapsed / it->duration);
                if (!it->is_2d) {
                    alpha *= esp_distance_alpha(
                        distance::meters_between(it->world_pos, input.packet.local_eye), cfg);
                }
                ImU32 col = (it->color & 0x00FFFFFF) | (static_cast<int>(alpha * 255) << 24);

                if (it->type == ActiveEffect3D::TYPE_HITMARKER) {
                    float scale = 1.0f;
                    if (elapsed < 0.15) {
                        float t = static_cast<float>(elapsed / 0.15);
                        scale = 1.3f - 0.3f * t; // Pop scale
                    }
                    float base_font_size = 16.0f;
                    float current_font_size = base_font_size * scale;

                    ImVec2 text_size = ImGui::GetFont()->CalcTextSizeA(current_font_size, 9999.0f, 0.0f, it->text.c_str());
                    ImVec2 text_pos(screen_x - text_size.x * 0.5f, screen_y - text_size.y * 0.5f);

                    ImU32 outline_col = IM_COL32(0, 0, 0, static_cast<int>(alpha * 220));

                    draw_list->AddText(nullptr, current_font_size, ImVec2(text_pos.x + 1.0f, text_pos.y + 1.0f), outline_col, it->text.c_str());
                    draw_list->AddText(nullptr, current_font_size, ImVec2(text_pos.x - 1.0f, text_pos.y + 1.0f), outline_col, it->text.c_str());
                    draw_list->AddText(nullptr, current_font_size, ImVec2(text_pos.x + 1.0f, text_pos.y - 1.0f), outline_col, it->text.c_str());
                    draw_list->AddText(nullptr, current_font_size, ImVec2(text_pos.x - 1.0f, text_pos.y - 1.0f), outline_col, it->text.c_str());
                    draw_list->AddText(nullptr, current_font_size, text_pos, col, it->text.c_str());
                } 
                else if (it->type == ActiveEffect3D::TYPE_KILL) {
                    float scale = 1.0f;
                    if (elapsed < 0.15) {
                        float t = static_cast<float>(elapsed / 0.15);
                        scale = 1.4f - 0.4f * t; // Pop-in scale
                    }

                    float base_font_size = 20.0f;
                    float current_font_size = base_font_size * scale;

                    ImVec2 text_size = ImGui::GetFont()->CalcTextSizeA(current_font_size, 9999.0f, 0.0f, it->text.c_str());
                    ImVec2 text_pos(screen_x - text_size.x * 0.5f, screen_y - text_size.y * 0.5f);

                    ImU32 outline_col = IM_COL32(0, 0, 0, static_cast<int>(alpha * 220));

                    // Clean 8-way outline at 1.0f offset to prevent gaps/blobby look
                    draw_list->AddText(nullptr, current_font_size, ImVec2(text_pos.x + 1.0f, text_pos.y + 1.0f), outline_col, it->text.c_str());
                    draw_list->AddText(nullptr, current_font_size, ImVec2(text_pos.x - 1.0f, text_pos.y + 1.0f), outline_col, it->text.c_str());
                    draw_list->AddText(nullptr, current_font_size, ImVec2(text_pos.x + 1.0f, text_pos.y - 1.0f), outline_col, it->text.c_str());
                    draw_list->AddText(nullptr, current_font_size, ImVec2(text_pos.x - 1.0f, text_pos.y - 1.0f), outline_col, it->text.c_str());
                    draw_list->AddText(nullptr, current_font_size, ImVec2(text_pos.x + 1.0f, text_pos.y), outline_col, it->text.c_str());
                    draw_list->AddText(nullptr, current_font_size, ImVec2(text_pos.x - 1.0f, text_pos.y), outline_col, it->text.c_str());
                    draw_list->AddText(nullptr, current_font_size, ImVec2(text_pos.x, text_pos.y + 1.0f), outline_col, it->text.c_str());
                    draw_list->AddText(nullptr, current_font_size, ImVec2(text_pos.x, text_pos.y - 1.0f), outline_col, it->text.c_str());
                    draw_list->AddText(nullptr, current_font_size, text_pos, col, it->text.c_str());
                }
            }
            ++it;
        }

        // 5. Update and draw the DamageFX world-space effects. Particles, the
        // screen pulse, lightning strikes and shockwave rings are shared with
        // the Hit FX half of the card via the DamageFx pool.
        kill_fx_effects.step(static_cast<float>(input.dt), cfg.kill_particles_gravity);
        kill_fx_effects.draw(draw_list, input.render_view_matrix, window_width, window_height);
    }

    if (cfg.auto_accept_enabled && cfg.auto_accept_show_target) {
        // The overlay uses coordinates local to its window; AutoAccept uses the
        // same helper with the root-window origin before injecting movement.
        const glm::ivec2 target = AutoAccept::target_position(cfg, 0, 0, window_width, window_height);
        const float scale = std::clamp(static_cast<float>(window_height) / 1080.0f, 0.75f, 1.5f);
        const float half_width = std::round(110.0f * scale);
        const float half_height = std::round(30.0f * scale);
        const float x = std::round(static_cast<float>(target.x));
        const float y = std::round(static_cast<float>(target.y));
        const ImU32 target_color = auto_accept.is_clicking()
            ? IM_COL32(255, 60, 60, 255)
            : IM_COL32(0, 255, 140, 255);
        ImDrawList* target_draw_list = ImGui::GetBackgroundDrawList();
        target_draw_list->AddRect(ImVec2(x - half_width, y - half_height),
                                  ImVec2(x + half_width, y + half_height),
                                  target_color, 0.0f, 0, 2.0f);
        target_draw_list->AddLine(ImVec2(x - 8.0f, y), ImVec2(x + 8.0f, y), target_color, 1.0f);
        target_draw_list->AddLine(ImVec2(x, y - 8.0f), ImVec2(x, y + 8.0f), target_color, 1.0f);
    }

    gpu_profiler.end_section();

    if (cstrafe_trainer) {
        cstrafe_trainer->draw(cfg, input.dt);
    }

    // Anti Flash: small center dot so the player can still aim while the black
    // backdrop hides the world. Drawn last so it sits above the black screen,
    // the map wireframe and every ESP overlay.
    if (anti_flash_active && cfg.anti_flash_crosshair) {
        ImDrawList* anti_flash_draw_list = ImGui::GetBackgroundDrawList();
        const float dot_x = std::round(static_cast<float>(window_width) * 0.5f);
        const float dot_y = std::round(static_cast<float>(window_height) * 0.5f);
        const float dot_radius = std::max(1.0f, cfg.anti_flash_crosshair_radius);
        const ImU32 dot_color = IM_COL32(
            static_cast<int>(std::clamp(cfg.anti_flash_crosshair_color.r, 0.0f, 1.0f) * 255.0f),
            static_cast<int>(std::clamp(cfg.anti_flash_crosshair_color.g, 0.0f, 1.0f) * 255.0f),
            static_cast<int>(std::clamp(cfg.anti_flash_crosshair_color.b, 0.0f, 1.0f) * 255.0f),
            static_cast<int>(std::clamp(cfg.anti_flash_crosshair_color.a, 0.0f, 1.0f) * 255.0f));
        anti_flash_draw_list->AddCircleFilled(ImVec2(dot_x, dot_y), dot_radius, dot_color, 16);
    }

    overlay->end_frame();

    auto gpu_end = std::chrono::high_resolution_clock::now();
    double current_gpu_ms = std::chrono::duration<double, std::milli>(gpu_end - gpu_start).count();
    total_gpu_time_ms += current_gpu_ms;
    max_gpu_time_ms = (std::max)(max_gpu_time_ms, current_gpu_ms);
    metrics_count++;
}

void App::render_menu()
{
    menu->render();
    current_context = menu->get_window();
}

static bool will_bomb_kill(int local_health, int local_armor, const Vec3& local_origin, const Vec3& bomb_pos, const std::string& map_name) {
    if (local_health <= 0) return false;

    // resolve map parameters
    float damage = 650.0f;
    float radius = 2275.0f;

    // strip map path
    std::string clean_map = map_name;
    size_t last_slash = clean_map.find_last_of("\\/");
    if (last_slash != std::string::npos) {
        clean_map = clean_map.substr(last_slash + 1);
    }
    
    // map values
    if (clean_map == "de_dust2" || clean_map == "de_vertigo") {
        damage = 500.0f;
        radius = 1750.0f;
    } else if (clean_map == "de_anubis") {
        damage = 450.0f;
        radius = 1575.0f;
    } else if (clean_map == "de_inferno") {
        damage = 620.0f;
        radius = 2170.0f;
    } else if (clean_map == "de_ancient" || clean_map == "de_mirage" || clean_map == "de_nuke" || clean_map == "de_overpass") {
        damage = 650.0f;
        radius = 2275.0f;
    }

    float dx = bomb_pos.x - local_origin.x;
    float dy = bomb_pos.y - local_origin.y;
    float dz = bomb_pos.z - local_origin.z;
    float dist = std::sqrt(dx * dx + dy * dy + dz * dz);

    if (dist > radius) {
        return false;
    }

    float sigma = radius / 3.0f;
    float damage_calc = damage * std::exp(- (dist * dist) / (2.0f * sigma * sigma));

    if (local_armor > 0) {
        float reduced = damage_calc / 2.0f;
        float armor_dmg = (damage_calc - reduced) / 2.0f;
        if (armor_dmg > (float)local_armor) {
            reduced = damage_calc - (float)local_armor * 2.0f;
        }
        damage_calc = reduced;
    }

    return (int)std::floor(damage_calc) >= local_health;
}

static ImU32 get_bomb_bar_color(const OverlayConfig& cfg, int local_team, float time_left, const PlantedC4& planted_c4, bool has_defuser) {
    (void)local_team;
    (void)planted_c4;
    (void)has_defuser;

    if (cfg.bomb_bar_gradient_enabled) {
        float t = std::clamp(time_left / 40.0f, 0.0f, 1.0f);
        float r = cfg.bomb_bar_gradient_end.r + (cfg.bomb_bar_gradient_start.r - cfg.bomb_bar_gradient_end.r) * t;
        float g = cfg.bomb_bar_gradient_end.g + (cfg.bomb_bar_gradient_start.g - cfg.bomb_bar_gradient_end.g) * t;
        float b = cfg.bomb_bar_gradient_end.b + (cfg.bomb_bar_gradient_start.b - cfg.bomb_bar_gradient_end.b) * t;
        float a = cfg.bomb_bar_gradient_end.a + (cfg.bomb_bar_gradient_start.a - cfg.bomb_bar_gradient_end.a) * t;
        return IM_COL32(
            static_cast<int>(std::clamp(r, 0.0f, 1.0f) * 255.0f),
            static_cast<int>(std::clamp(g, 0.0f, 1.0f) * 255.0f),
            static_cast<int>(std::clamp(b, 0.0f, 1.0f) * 255.0f),
            static_cast<int>(std::clamp(a, 0.0f, 1.0f) * 255.0f)
        );
    } else {
        return IM_COL32(
            static_cast<int>(std::clamp(cfg.bomb_bar_color.r, 0.0f, 1.0f) * 255.0f),
            static_cast<int>(std::clamp(cfg.bomb_bar_color.g, 0.0f, 1.0f) * 255.0f),
            static_cast<int>(std::clamp(cfg.bomb_bar_color.b, 0.0f, 1.0f) * 255.0f),
            static_cast<int>(std::clamp(cfg.bomb_bar_color.a, 0.0f, 1.0f) * 255.0f)
        );
    }
}

void App::draw_static_bomb_bar(const FrameInput& input) {
    // Map reset check
    std::string map_name_str(input.packet.map_name);
    if (map_name_str != bomb_bar_last_map) {
        bomb_bar_last_map = map_name_str;
        bomb_bar_fade_alpha = 0.0f;
        bomb_bar_fade_time_left = 0.0f;
        bomb_bar_fade_c4 = {};
        bomb_bar_display_ratio = 1.0f;
    }

    // Is active check
    bool is_active = (input.packet.planted_c4.active && 
                      !input.packet.planted_c4.is_defused && 
                      !input.packet.planted_c4.has_exploded && 
                      input.packet.planted_c4.time_left > 0.0f);

    if (input.packet.planted_c4.active) {
        bomb_bar_fade_c4 = input.packet.planted_c4;
    }

    if (is_active) {
        bomb_bar_fade_time_left = input.packet.planted_c4.time_left;
        bomb_bar_fade_alpha = std::min(1.0f, bomb_bar_fade_alpha + input.dt / 0.15f);
    } else {
        bomb_bar_fade_alpha = std::max(0.0f, bomb_bar_fade_alpha - input.dt / 0.50f);
        if (bomb_bar_fade_time_left > 0.0f) {
            bomb_bar_fade_time_left = std::max(0.0f, bomb_bar_fade_time_left - input.dt);
            if (bomb_bar_fade_c4.is_being_defused && bomb_bar_fade_c4.defuse_time_left > 0.0f) {
                bomb_bar_fade_c4.defuse_time_left = std::max(0.0f, bomb_bar_fade_c4.defuse_time_left - input.dt);
            }
        }
    }

    if (bomb_bar_fade_alpha <= 0.001f || !bomb_bar_fade_c4.active) {
        bomb_bar_display_ratio = 1.0f;
        return;
    }

    // Draw parameters
    float w = 100.0f;
    float h = 8.0f;
    float fs = 12.0f;
    float pad_x = 6.0f;
    float pad_y = 4.0f;
    float smoothing = 0.1f;

    float x = (window_width / 2.0f) - (w / 2.0f);
    float y = (window_height * 0.65f);

    // Calc smooth visual ratio
    float ratio = std::max(0.0f, bomb_bar_fade_time_left / 40.0f);
    if (ratio > 1.0f) ratio = 1.0f;
    bomb_bar_display_ratio = bomb_bar_display_ratio + (ratio - bomb_bar_display_ratio) * smoothing;

    float bar_width = bomb_bar_display_ratio * w;

    // Texts
    char timer_text[32];
    std::snprintf(timer_text, sizeof(timer_text), "%.1f", bomb_bar_fade_time_left);

    // Kills check
    bool kills = will_bomb_kill(input.packet.local_health, input.packet.local_armor, input.packet.local_origin, bomb_bar_fade_c4.position, map_name_str);
    const char* kill_text = kills ? "KILL" : "LIVE";
    const char* defuse_text = "DEFUSING";

    // Is CT defusing & T-side style
    bool is_defusing = false;
    if (input.packet.local_team == 2 && bomb_bar_fade_c4.is_being_defused) {
        if (bomb_bar_fade_c4.defuse_time_left <= bomb_bar_fade_time_left) {
            is_defusing = true;
        }
    }

    // Text sizes using ImGui
    float w_kill = ImGui::CalcTextSize(kill_text).x;
    float w_defuse = ImGui::CalcTextSize(defuse_text).x;

    // Layout
    float text_y = y - fs + 2.0f;
    float min_x = x - pad_x;
    float max_x = x + w + pad_x;
    float min_y = is_defusing ? (text_y - fs - 2.0f - pad_y) : (text_y - pad_y);
    float max_y = y + h + pad_y;

    ImVec2 capsule_min(min_x, min_y);
    ImVec2 capsule_max(max_x, max_y);

    ImDrawList* draw_list = ImGui::GetBackgroundDrawList();

    // Fade color function
    auto fade_color = [](ImU32 color, float alpha) -> ImU32 {
        int r = (color >> 0) & 0xFF;
        int g = (color >> 8) & 0xFF;
        int b = (color >> 16) & 0xFF;
        int a = (color >> 24) & 0xFF;
        a = static_cast<int>(a * alpha);
        return IM_COL32(r, g, b, a);
    };

    // Capsule background color from config
    ImU32 c_capsule = IM_COL32(
        static_cast<int>(cfg.bomb_bar_bg_color.r * 255.0f),
        static_cast<int>(cfg.bomb_bar_bg_color.g * 255.0f),
        static_cast<int>(cfg.bomb_bar_bg_color.b * 255.0f),
        static_cast<int>(cfg.bomb_bar_bg_color.a * 255.0f * bomb_bar_fade_alpha)
    );
    draw_list->AddRectFilled(capsule_min, capsule_max, c_capsule);

    // Draw outlined text function
    auto draw_outlined_text = [&](const ImVec2& pos, ImU32 text_col, const char* str) {
        ImU32 outline_col = fade_color(IM_COL32(0, 0, 0, 255), bomb_bar_fade_alpha);
        draw_list->AddText(ImVec2(pos.x - 1.0f, pos.y), outline_col, str);
        draw_list->AddText(ImVec2(pos.x + 1.0f, pos.y), outline_col, str);
        draw_list->AddText(ImVec2(pos.x, pos.y - 1.0f), outline_col, str);
        draw_list->AddText(ImVec2(pos.x, pos.y + 1.0f), outline_col, str);
        draw_list->AddText(pos, text_col, str);
    };

    // Get current bomb/timer color
    ImU32 bar_color = get_bomb_bar_color(cfg, input.packet.local_team, bomb_bar_fade_time_left, bomb_bar_fade_c4, input.packet.local_has_defuser);

    // Left: timer
    ImU32 c_timer = fade_color(bar_color, bomb_bar_fade_alpha);
    draw_outlined_text(ImVec2(x, text_y), c_timer, timer_text);

    // Right: KILL / LIVE
    ImU32 c_kill = fade_color(kills ? IM_COL32(255, 0, 0, 255) : IM_COL32(0, 255, 0, 255), bomb_bar_fade_alpha);
    float kill_y = is_defusing ? (text_y - fs - 2.0f) : text_y;
    draw_outlined_text(ImVec2(x + w - w_kill, kill_y), c_kill, kill_text);

    // Defuse text
    if (is_defusing) {
        ImU32 c_defuse = fade_color(IM_COL32(255, 0, 0, 255), bomb_bar_fade_alpha);
        draw_outlined_text(ImVec2(x + w - w_defuse, text_y), c_defuse, defuse_text);
    }

    // Black bar background
    ImU32 c_black = IM_COL32(0, 0, 0, static_cast<int>(255 * bomb_bar_fade_alpha));
    draw_list->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), c_black);

    // Progress bar
    ImU32 c_bar = fade_color(bar_color, bomb_bar_fade_alpha);
    draw_list->AddRectFilled(ImVec2(x, y), ImVec2(x + bar_width, y + h), c_bar);
}

void App::process_keybinds()
{
    for (auto& kb : cfg.keybinds) {
        if (kb.key < 0) continue;

        bool is_pressed = global_input.is_key_pressed(kb.key);
        bool pressed_transition = is_pressed && !kb.was_pressed;
        bool released_transition = !is_pressed && kb.was_pressed;

        if (is_setting_bool(kb.setting_name)) {
            bool current_val = get_config_bool(cfg, kb.setting_name);
            if (kb.mode == BindMode::TOGGLE) {
                if (pressed_transition) {
                    set_config_bool(cfg, kb.setting_name, !current_val);
                }
            } else if (kb.mode == BindMode::HOLD) {
                if (pressed_transition) {
                    kb.original_val_bool = current_val;
                    const bool hold_value = kb.setting_name == "esp_distance_override" ? true : !current_val;
                    set_config_bool(cfg, kb.setting_name, hold_value);
                    kb.is_active_hold = true;
                } else if (released_transition && kb.is_active_hold) {
                    set_config_bool(cfg, kb.setting_name, kb.original_val_bool);
                    kb.is_active_hold = false;
                }
            }
        }
        else if (is_setting_float(kb.setting_name)) {
            float current_val = get_config_float(cfg, kb.setting_name);
            if (kb.mode == BindMode::TOGGLE) {
                if (pressed_transition) {
                    if (std::abs(current_val - kb.slider_value) < 0.0001f) {
                        set_config_float(cfg, kb.setting_name, kb.original_val_float);
                    } else {
                        kb.original_val_float = current_val;
                        set_config_float(cfg, kb.setting_name, kb.slider_value);
                    }
                }
            } else if (kb.mode == BindMode::HOLD) {
                if (pressed_transition) {
                    kb.original_val_float = current_val;
                    set_config_float(cfg, kb.setting_name, kb.slider_value);
                    kb.is_active_hold = true;
                } else if (released_transition && kb.is_active_hold) {
                    set_config_float(cfg, kb.setting_name, kb.original_val_float);
                    kb.is_active_hold = false;
                }
            }
        }
        else if (is_setting_int(kb.setting_name)) {
            int current_val = get_config_int(cfg, kb.setting_name);
            if (kb.mode == BindMode::TOGGLE) {
                if (pressed_transition) {
                    if (current_val == kb.slider_value_int) {
                        set_config_int(cfg, kb.setting_name, kb.original_val_int);
                    } else {
                        kb.original_val_int = current_val;
                        set_config_int(cfg, kb.setting_name, kb.slider_value_int);
                    }
                }
            } else if (kb.mode == BindMode::HOLD) {
                if (pressed_transition) {
                    kb.original_val_int = current_val;
                    set_config_int(cfg, kb.setting_name, kb.slider_value_int);
                    kb.is_active_hold = true;
                } else if (released_transition && kb.is_active_hold) {
                    set_config_int(cfg, kb.setting_name, kb.original_val_int);
                    kb.is_active_hold = false;
                }
            }
        }

        kb.was_pressed = is_pressed;
    }

    if (cfg.esp_crosshair_distance) {
        const int pin_key = get_keybind_key(cfg, "crosshair_distance_pin_key", 1002);
        const int clear_key = get_keybind_key(cfg, "crosshair_distance_clear_pin_key", -1);
        crosshair_distance.handle_keys(last_input, visibility_worker,
                                       pin_key > 0 && global_input.is_key_pressed(pin_key),
                                       clear_key > 0 && global_input.is_key_pressed(clear_key));
    } else {
        crosshair_distance.clear_pin();
    }

    int create_bind_key = cfg.grenade_solver_create_bind;
    for (const auto& kb : cfg.keybinds) {
        if (kb.setting_name == "grenade_solver_create_bind") {
            create_bind_key = kb.key;
            break;
        }
    }

    if (cfg.grenade_solver_show_trajectory && cfg.grenade_solver_location_creator_enabled && create_bind_key > 0) {
        bool is_pressed = global_input.is_key_pressed(create_bind_key);
        if (is_pressed && !create_bind_was_pressed_) {
            if (!current_map.empty()) {
                if (landing_zone_manager.pending_point_count() == 3) {
                    landing_zone_manager.cancel_creation();
                }

                const float* mat = last_input.render_view_matrix;
                glm::vec3 fwd(mat[12], mat[13], mat[14]);
                if (glm::length(fwd) > 0.001f) {
                    fwd = glm::normalize(fwd);
                } else {
                    float p = last_input.packet.local_angles.x * 3.14159265f / 180.0f;
                    float y = last_input.packet.local_angles.y * 3.14159265f / 180.0f;
                    fwd = glm::vec3(std::cos(p) * std::cos(y), std::cos(p) * std::sin(y), -std::sin(p));
                }
                glm::vec3 eye(last_input.packet.local_eye.x, last_input.packet.local_eye.y, last_input.packet.local_eye.z);
                visibility_worker.request_point_trace(eye, fwd);
            }
        }
        create_bind_was_pressed_ = is_pressed;
    }
}

static ImU32 fade_theme_color(const ImVec4& color, float alpha) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(color.x, color.y, color.z, color.w * alpha));
}

void App::draw_keybinds_viewer(float dt)
{
    // 1. Mark all visual states as inactive by default
    for (auto& pair : keybind_viewer_states) {
        pair.second.active = false;
    }

    // 2. Query hardcoded Edge Jump keybind
    if (cfg.ej_enabled && cfg.edgebug_activation_key > 0) {
        bool ej_active = global_input.is_key_pressed(cfg.edgebug_activation_key);
        if (ej_active) {
            bool is_knife = (last_input.packet.held_weapon_id == 42 ||
                             last_input.packet.held_weapon_id == 59 ||
                             last_input.packet.held_weapon_id == 500 ||
                             (last_input.packet.held_weapon_id >= 505 && last_input.packet.held_weapon_id <= 526));
            if (!cfg.ej_knife_only || is_knife) {
                auto& state = keybind_viewer_states["edge_jump"];
                state.label = "Edge Jump";
                state.status = "Held";
                state.active = true;
            }
        }
    }

    // Helper to format setting names elegantly
    auto format_setting = [](const std::string& name) -> std::string {
        if (name == "ej_enabled" || name == "edgebug_activation_key") return "Edge Jump";
        if (name == "trigger_enabled") return "Triggerbot";
        if (name == "esp_enabled") return "ESP";
        if (name == "chams_enabled") return "Chams";
        if (name == "draw_grenade_trajectory") return "Trajectories";
        if (name == "grenade_helper_enabled") return "Grenade Helper";
        if (name == "map_visualizer_enabled") return "Map Visualizer";
        
        std::string formatted = "";
        bool cap = true;
        for (char c : name) {
            if (c == '_') {
                formatted += ' ';
                cap = true;
            } else {
                formatted += cap ? std::toupper(c) : c;
                cap = false;
            }
        }
        return formatted;
    };

    // 3. Query dynamic config keybinds
    for (const auto& kb : cfg.keybinds) {
        if (kb.key < 0 || kb.setting_name.empty()) continue;

        // Do not display physical key configuration settings (e.g. key_left_mouse, key_w, etc.)
        if (kb.setting_name.find("key_") == 0) continue;

        bool active = false;
        std::string status_str = "";
        if (kb.mode == BindMode::HOLD) {
            active = global_input.is_key_pressed(kb.key);
            status_str = "Held";
        } else {
            // TOGGLE mode
            if (is_setting_bool(kb.setting_name)) {
                active = get_config_bool(cfg, kb.setting_name);
            } else {
                // non-boolean fallback
                active = global_input.is_key_pressed(kb.key);
            }
            status_str = "Toggled";
        }

        if (active) {
            auto& state = keybind_viewer_states[kb.setting_name];
            state.label = format_setting(kb.setting_name);
            state.status = status_str;
            state.active = true;
        }
    }

    // 4. Update alphas for each state
    float anim_speed = 12.0f; // clean ease-out animation speed
    bool any_active = false;
    for (auto& pair : keybind_viewer_states) {
        auto& state = pair.second;
        float target = state.active ? 1.0f : 0.0f;
        state.alpha += (target - state.alpha) * anim_speed * dt;
        state.alpha = std::clamp(state.alpha, 0.0f, 1.0f);
        if (state.active || state.alpha > 0.001f) {
            any_active = true;
        }
    }

    // 5. Update overall box fade alpha
    float box_target = any_active ? 1.0f : 0.0f;
    keybind_box_fade_alpha += (box_target - keybind_box_fade_alpha) * anim_speed * dt;
    keybind_box_fade_alpha = std::clamp(keybind_box_fade_alpha, 0.0f, 1.0f);

    if (keybind_box_fade_alpha <= 0.001f) {
        return; // nothing to render
    }

    // 6. Gather all currently visible items (where alpha > 0) sorted by label for consistency
    struct RenderItem {
        std::string label;
        std::string status;
        float alpha;
    };
    std::vector<RenderItem> visible_items;
    for (const auto& pair : keybind_viewer_states) {
        if (pair.second.alpha > 0.005f) {
            visible_items.push_back({pair.second.label, pair.second.status, pair.second.alpha});
        }
    }
    std::sort(visible_items.begin(), visible_items.end(), [](const auto& a, const auto& b) {
        return a.label < b.label;
    });

    // 7. Calculate dynamic height based on active items' alphas
    const float scale = std::clamp(cfg.kb_viewer_scale, 0.75f, 2.0f);
    const float padding_top = 8.0f * scale;
    const float title_height = 20.0f * scale;
    const float padding_mid = 6.0f * scale;
    const float item_height = 18.0f * scale;
    const float padding_bottom = 8.0f * scale;

    float target_height = padding_top + title_height + padding_mid;
    for (const auto& item : visible_items) {
        target_height += item_height * item.alpha;
    }
    target_height += padding_bottom;

    // Smooth height transition
    keybind_box_height += (target_height - keybind_box_height) * anim_speed * dt;

    // 8. Render the panel
    ImDrawList* draw_list = ImGui::GetBackgroundDrawList();
    const float x = cfg.kb_viewer_x;
    const float y = cfg.kb_viewer_y;
    const float w = 200.0f * scale;
    const float h = keybind_box_height;
    const float rounding = MenuTheme::panel_rounding * scale;
    const ImVec2 panel_min(std::round(x), std::round(y));
    const ImVec2 panel_max(std::round(x + w), std::round(y + h));
    ImFont* font = ImGui::GetFont();
    const float font_size = ImGui::GetFontSize() * scale;

    ImU32 bg_color = fade_theme_color(MenuTheme::bg_card, 0.85f * keybind_box_fade_alpha);
    ImU32 border_color = fade_theme_color(MenuTheme::border_subtle, std::max(MenuTheme::border_subtle.w, 0.35f) * keybind_box_fade_alpha);

    draw_list->AddRectFilled(panel_min, panel_max, bg_color, rounding);
    draw_list->AddRect(panel_min, panel_max, border_color, rounding, 0, scale);

    // Draw left-aligned accent capsule indicator using theme accent color
    ImU32 accent_color = fade_theme_color(MenuTheme::accent_primary, keybind_box_fade_alpha);
    draw_list->AddRectFilled(ImVec2(std::round(x + 8.0f * scale), std::round(y + padding_top + 2.0f * scale)),
                             ImVec2(std::round(x + 11.0f * scale), std::round(y + padding_top + 15.0f * scale)),
                             accent_color, 1.5f * scale);

    // Draw Title: "Keybinds"
    ImU32 title_color = fade_theme_color(MenuTheme::text_primary, keybind_box_fade_alpha);
    draw_list->AddText(font, font_size, ImVec2(std::round(x + 16.0f * scale), std::round(y + padding_top)), title_color,
                       "Keybinds");

    // Separator line
    ImU32 sep_color = fade_theme_color(MenuTheme::border_subtle, std::max(MenuTheme::border_subtle.w, 0.25f) * keybind_box_fade_alpha);
    const float separator_y = std::round(y + padding_top + title_height + 2.0f * scale);
    draw_list->AddLine(ImVec2(std::round(x + 8.0f * scale), separator_y),
                       ImVec2(std::round(x + w - 8.0f * scale), separator_y), sep_color, scale);

    // Draw visible items
    float current_y = y + padding_top + title_height + padding_mid;
    for (const auto& item : visible_items) {
        float item_alpha = keybind_box_fade_alpha * item.alpha;
        ImU32 label_color = fade_theme_color(MenuTheme::text_secondary, item_alpha);
        
        ImU32 status_color;
        if (item.status == "Held") {
            status_color = fade_theme_color(MenuTheme::accent_primary, item_alpha);
        } else {
            status_color = fade_theme_color(ImVec4(0.45f, 0.85f, 0.55f, 1.0f), item_alpha);
        }

        // Draw label
        draw_list->AddText(font, font_size, ImVec2(std::round(x + 12.0f * scale), std::round(current_y)), label_color,
                           item.label.c_str());

        // Draw status (right-aligned)
        const float status_w = font->CalcTextSizeA(font_size, 9999.0f, 0.0f, item.status.c_str()).x;
        draw_list->AddText(font, font_size,
                           ImVec2(std::round(x + w - 12.0f * scale - status_w), std::round(current_y)), status_color,
                           item.status.c_str());

        current_y += item_height * item.alpha;
    }
}

void App::draw_spectators_viewer(float dt)
{
    // 1. Mark all spectator visual states as inactive
    for (auto& pair : spectator_viewer_states) {
        pair.second.active = false;
    }

    // 2. Populate from current frame input packet
    for (int i = 0; i < last_input.packet.spectator_count; ++i) {
        const auto& spec = last_input.packet.spectators[i];
        if (spec.name[0] == '\0') continue;

        std::string sname(spec.name);
        auto& state = spectator_viewer_states[sname];
        state.name = sname;
        state.is_first_person = (spec.is_first_person != 0);
        state.active = true;
    }

    // 3. Update alphas
    float anim_speed = 12.0f;
    bool any_active = false;
    for (auto& pair : spectator_viewer_states) {
        auto& state = pair.second;
        float target = state.active ? 1.0f : 0.0f;
        state.alpha += (target - state.alpha) * anim_speed * dt;
        state.alpha = std::clamp(state.alpha, 0.0f, 1.0f);
        if (state.active || state.alpha > 0.001f) {
            any_active = true;
        }
    }

    // 4. Clean up inactive states when fully faded out
    for (auto it = spectator_viewer_states.begin(); it != spectator_viewer_states.end();) {
        if (!it->second.active && it->second.alpha <= 0.001f) {
            it = spectator_viewer_states.erase(it);
        } else {
            ++it;
        }
    }

    // 5. Update box fade alpha
    float box_target = any_active ? 1.0f : 0.0f;
    spectator_box_fade_alpha += (box_target - spectator_box_fade_alpha) * anim_speed * dt;
    spectator_box_fade_alpha = std::clamp(spectator_box_fade_alpha, 0.0f, 1.0f);

    if (spectator_box_fade_alpha <= 0.001f) {
        return;
    }

    // 6. Gather visible items sorted by name
    struct RenderItem {
        std::string name;
        std::string mode_str;
        bool is_first_person;
        float alpha;
    };
    std::vector<RenderItem> visible_items;
    for (const auto& pair : spectator_viewer_states) {
        if (pair.second.alpha > 0.005f) {
            std::string mode_str = pair.second.is_first_person ? "1st Person" : "3rd Person";
            visible_items.push_back({pair.second.name, mode_str, pair.second.is_first_person, pair.second.alpha});
        }
    }
    std::sort(visible_items.begin(), visible_items.end(), [](const auto& a, const auto& b) {
        return a.name < b.name;
    });

    // 7. Calculate height
    const float scale = std::clamp(cfg.spec_list_scale, 0.75f, 2.0f);
    const float padding_top = 8.0f * scale;
    const float title_height = 20.0f * scale;
    const float padding_mid = 6.0f * scale;
    const float item_height = 18.0f * scale;
    const float padding_bottom = 8.0f * scale;

    float target_height = padding_top + title_height + padding_mid;
    for (const auto& item : visible_items) {
        target_height += item_height * item.alpha;
    }
    target_height += padding_bottom;

    spectator_box_height += (target_height - spectator_box_height) * anim_speed * dt;

    // 8. Render
    ImDrawList* draw_list = ImGui::GetBackgroundDrawList();
    const float x = cfg.spec_list_x;
    const float y = cfg.spec_list_y;
    const float w = 200.0f * scale;
    const float h = spectator_box_height;
    const float rounding = MenuTheme::panel_rounding * scale;
    const ImVec2 panel_min(std::round(x), std::round(y));
    const ImVec2 panel_max(std::round(x + w), std::round(y + h));
    ImFont* font = ImGui::GetFont();
    const float font_size = ImGui::GetFontSize() * scale;

    ImU32 bg_color = fade_theme_color(MenuTheme::bg_card, 0.85f * spectator_box_fade_alpha);
    ImU32 border_color = fade_theme_color(MenuTheme::border_subtle, std::max(MenuTheme::border_subtle.w, 0.35f) * spectator_box_fade_alpha);

    draw_list->AddRectFilled(panel_min, panel_max, bg_color, rounding);
    draw_list->AddRect(panel_min, panel_max, border_color, rounding, 0, scale);

    ImU32 accent_color = fade_theme_color(MenuTheme::accent_primary, spectator_box_fade_alpha);
    draw_list->AddRectFilled(ImVec2(std::round(x + 8.0f * scale), std::round(y + padding_top + 2.0f * scale)),
                             ImVec2(std::round(x + 11.0f * scale), std::round(y + padding_top + 15.0f * scale)),
                             accent_color, 1.5f * scale);

    ImU32 title_color = fade_theme_color(MenuTheme::text_primary, spectator_box_fade_alpha);
    draw_list->AddText(font, font_size, ImVec2(std::round(x + 16.0f * scale), std::round(y + padding_top)), title_color,
                       "Spectators");

    ImU32 sep_color = fade_theme_color(MenuTheme::border_subtle, std::max(MenuTheme::border_subtle.w, 0.25f) * spectator_box_fade_alpha);
    const float separator_y = std::round(y + padding_top + title_height + 2.0f * scale);
    draw_list->AddLine(ImVec2(std::round(x + 8.0f * scale), separator_y),
                       ImVec2(std::round(x + w - 8.0f * scale), separator_y), sep_color, scale);

    float current_y = y + padding_top + title_height + padding_mid;
    for (const auto& item : visible_items) {
        float item_alpha = spectator_box_fade_alpha * item.alpha;
        ImU32 label_color = fade_theme_color(MenuTheme::text_secondary, item_alpha);
        
        ImU32 status_color;
        if (item.is_first_person) {
            status_color = fade_theme_color(MenuTheme::accent_primary, item_alpha);
        } else {
            status_color = fade_theme_color(ImVec4(0.45f, 0.85f, 0.55f, 1.0f), item_alpha);
        }

        draw_list->AddText(font, font_size, ImVec2(std::round(x + 12.0f * scale), std::round(current_y)), label_color,
                           item.name.c_str());

        const float status_w = font->CalcTextSizeA(font_size, 9999.0f, 0.0f, item.mode_str.c_str()).x;
        draw_list->AddText(font, font_size,
                           ImVec2(std::round(x + w - 12.0f * scale - status_w), std::round(current_y)), status_color,
                           item.mode_str.c_str());

        current_y += item_height * item.alpha;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Embedded web radar (FC2observ-style)
// ─────────────────────────────────────────────────────────────────────────────
std::string App::radar_asset_root()
{
    // Prefer the copy shipped next to the executable; fall back to the
    // working-directory copy used during development.
    char buf[PATH_MAX];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        const std::string exe(buf);
        const auto slash = exe.find_last_of('/');
        if (slash != std::string::npos) {
            const std::string candidate = exe.substr(0, slash) + "/assets/radar";
            std::error_code ec;
            if (std::filesystem::is_directory(candidate, ec)) return candidate;
        }
    }
    return "assets/radar";
}

bool App::start_radar()
{
    const std::string host = cfg.radar_host.empty() ? "127.0.0.1" : cfg.radar_host;
    if (!radar_server.start(host, cfg.radar_http_port, cfg.radar_ws_port, radar_asset_root())) {
        BUNNYWARE_LOG_ERROR("Radar server failed to start: {}", radar_server.last_error());
        radar_running_ = false;
        return false;
    }
    radar_running_ = true;
    radar_applied_host_ = host;
    radar_applied_http_port_ = cfg.radar_http_port;
    radar_applied_ws_port_ = cfg.radar_ws_port;
    if (cfg.radar_open_browser) {
        radar::RadarServer::open_in_browser(cfg.radar_http_port);
    }
    return true;
}

void App::stop_radar()
{
    radar_server.stop();
    radar_running_ = false;
    radar_smoke_first_seen_.clear();
}

void App::process_radar()
{
    // Master toggle off always wins.
    if (!cfg.radar_enabled && radar_running_) {
        stop_radar();
    }

    if (radar::take_stop_request() && radar_running_) {
        stop_radar();
    }
    if (radar::take_start_request() && !radar_running_ && cfg.radar_enabled) {
        start_radar();
    }

    // Restart when the effective connection settings change while running.
    if (radar_running_) {
        const std::string host = cfg.radar_host.empty() ? "127.0.0.1" : cfg.radar_host;
        if (host != radar_applied_host_ || cfg.radar_http_port != radar_applied_http_port_ ||
            cfg.radar_ws_port != radar_applied_ws_port_) {
            stop_radar();
            start_radar();
        }
    }

    radar::WebConfig web;
    web.transparent = cfg.radar_transparent;
    web.show_name = cfg.radar_show_names ? "both" : "never";
    web.max_name_length = cfg.radar_max_name_length;
    web.show_buyzones = cfg.radar_show_buyzones ? "buytime" : "never";
    web.projectiles = cfg.radar_show_projectiles;
    web.player_dot_scale = cfg.radar_player_dot_scale;
    web.bomb_dot_scale = cfg.radar_bomb_dot_scale;
    web.autozoom = cfg.radar_autozoom;
    web.vert_indicator_type = cfg.radar_vert_indicator == 0   ? "none"
                              : cfg.radar_vert_indicator == 1 ? "color"
                                                              : "scale";
    radar_server.set_web_config(web);
}

void App::update_radar_snapshot()
{
    if (!radar_running_) return;

    // The browser smooths at its own requestAnimationFrame cadence, so 60 Hz of
    // telemetry is plenty and keeps JSON serialisation off the hot path.
    const double now = glfwGetTime();
    if (now - radar_last_publish_time_ < (1.0 / 60.0)) return;
    radar_last_publish_time_ = now;

    const ShmPacket& packet = last_input.packet;

    auto normalize_map = [](std::string name) {
        const auto slash = name.find_last_of('/');
        if (slash != std::string::npos) name = name.substr(slash + 1);
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".vpk") == 0) {
            name.resize(name.size() - 4);
        }
        return name;
    };

    radar::Snapshot snap;
    const std::string map_name = normalize_map(packet.map_name);
    const bool map_valid = !map_name.empty() && map_name != "menu" && map_name != "<empty>" &&
                           map_name != "unknown" && map_name != "invalid";
    snap.in_game = map_valid && packet.player_count > 0;
    snap.map = snap.in_game ? map_name : std::string("waiting");

    // --- Round / match state ------------------------------------------------
    snap.time_remaining = "0:00";
    if (packet.game_rules.active && snap.in_game) {
        snap.t_score = packet.game_rules.t_score;
        snap.ct_score = packet.game_rules.ct_score;
        const int rounds_played = packet.game_rules.total_rounds_played;
        snap.round = (rounds_played >= 0 && rounds_played < 100) ? rounds_played + 1 : 0;

        if (packet.game_rules.is_warmup) {
            snap.phase = "warmup";
        } else if (packet.game_rules.is_freeze_period) {
            snap.phase = "freezetime";
        } else if (snap.in_game) {
            snap.phase = "live";
        }

        int seconds = packet.game_rules.round_time;
        if (seconds < 0 || seconds > 600) seconds = 0;
        char time_buf[16];
        std::snprintf(time_buf, sizeof(time_buf), "%d:%02d", seconds / 60, seconds % 60);
        snap.time_remaining = time_buf;
    }

    // --- Players -------------------------------------------------------------
    int slot = 0;
    for (int i = 0; i < packet.player_count && i < static_cast<int>(shm::MAX_PLAYERS) && slot < 19; ++i) {
        const PlayerData& pl = packet.players[i];
        if (pl.team != 2 && pl.team != 3) continue;
        // Skip empty dead slots that never received a valid origin.
        if (pl.health <= 0 && pl.origin.x == 0.0f && pl.origin.y == 0.0f && pl.origin.z == 0.0f) continue;

        radar::PlayerSnapshot ps;
        ps.num = slot++;
        ps.name = std::string(pl.name);
        ps.team = pl.team;
        ps.health = pl.health;
        ps.flashed = pl.flash_alpha;
        ps.bomb = pl.has_bomb != 0;
        ps.disconnected = false;
        ps.x = pl.origin.x;
        ps.y = pl.origin.y;
        ps.z = pl.origin.z;
        ps.weapon = std::string(pl.active_weapon_name);
        ps.ammo = pl.ammo;

        // Radar angle: convert the pawn eye yaw into the frontend's 0..360 space.
        // Fall back to 0 when the eye angles were not readable (all-zero).
        if (pl.eye_angles.x == 0.0f && pl.eye_angles.y == 0.0f && pl.eye_angles.z == 0.0f) {
            ps.angle = 0.0f;
        } else {
            float angle = -pl.eye_angles.y + 90.0f;
            while (angle < 0.0f) angle += 360.0f;
            while (angle >= 360.0f) angle -= 360.0f;
            ps.angle = std::round(angle * 100.0f) / 100.0f;
        }

        snap.players.push_back(std::move(ps));
    }

    // The reader deliberately omits the local pawn from players[] (it is the
    // camera for the ESP), so build its radar dot from the local_* fields.
    if ((packet.local_team == 2 || packet.local_team == 3) && packet.local_health > 0 && slot < 19) {
        radar::PlayerSnapshot lp;
        lp.num = slot++;
        lp.name = (packet.local_name[0] != '\0') ? std::string(packet.local_name) : std::string("You");
        lp.team = packet.local_team;
        lp.health = packet.local_health;
        lp.active = true;
        lp.flashed = packet.local_flash_alpha;
        lp.bomb = false;
        lp.disconnected = false;
        lp.x = packet.local_origin.x;
        lp.y = packet.local_origin.y;
        lp.z = packet.local_origin.z;
        lp.ammo = -1;
        float local_angle = -packet.local_angles.y + 90.0f;
        while (local_angle < 0.0f) local_angle += 360.0f;
        while (local_angle >= 360.0f) local_angle -= 360.0f;
        lp.angle = std::round(local_angle * 100.0f) / 100.0f;
        snap.players.push_back(std::move(lp));
    }

    // --- Bomb ----------------------------------------------------------------
    const PlantedC4& c4 = packet.planted_c4;
    if (c4.active) {
        if (c4.has_exploded) {
            snap.bomb.state = "exploded";
        } else if (c4.is_defused) {
            snap.bomb.state = "defused";
        } else if (c4.is_being_defused) {
            snap.bomb.state = "defusing";
        } else {
            snap.bomb.state = "planted";
        }
        snap.bomb.x = c4.position.x;
        snap.bomb.y = c4.position.y;
        snap.bomb.z = c4.position.z;
        snap.bomb.blow_time = c4.time_left;
        snap.bomb.is_defusing = c4.is_being_defused != 0;
        snap.bomb.defuse_possible = true;
    } else {
        const PlayerData* carrier = nullptr;
        for (int i = 0; i < packet.player_count && i < static_cast<int>(shm::MAX_PLAYERS); ++i) {
            if (packet.players[i].has_bomb) {
                carrier = &packet.players[i];
                break;
            }
        }
        if (carrier) {
            snap.bomb.state = "carried";
            snap.bomb.x = carrier->origin.x;
            snap.bomb.y = carrier->origin.y;
            snap.bomb.z = carrier->origin.z;
        } else {
            for (int d = 0; d < packet.dropped_item_count && d < static_cast<int>(shm::MAX_DROPPED_ITEMS); ++d) {
                if (std::strstr(packet.dropped_items[d].name, "c4") != nullptr) {
                    snap.bomb.state = "dropped";
                    snap.bomb.x = packet.dropped_items[d].origin.x;
                    snap.bomb.y = packet.dropped_items[d].origin.y;
                    snap.bomb.z = packet.dropped_items[d].origin.z;
                    break;
                }
            }
        }
    }

    // --- Smokes / infernos ---------------------------------------------------
    if (cfg.radar_show_smokes) {
        for (int s = 0; s < packet.smoke_count && s < 4; ++s) {
            const ActiveSmokeData& sm = packet.smokes[s];
            if (!sm.active) continue;
            const auto it = radar_smoke_first_seen_.find(sm.entity_handle);
            const double first = (it == radar_smoke_first_seen_.end()) ? packet.curtime : it->second;
            radar_smoke_first_seen_[sm.entity_handle] = first;

            radar::SmokeSnapshot ss;
            ss.id = std::to_string(sm.entity_handle);
            ss.time = static_cast<float>(packet.curtime - first);
            ss.team = (sm.team == 3) ? "CT" : "T";
            ss.x = sm.center.x;
            ss.y = sm.center.y;
            ss.z = sm.center.z;
            snap.smokes.push_back(std::move(ss));
        }

        for (int n = 0; n < packet.inferno_count && n < 4; ++n) {
            const InfernoData& inf = packet.infernos[n];
            if (!inf.active || inf.fire_count <= 0) continue;
            radar::InfernoSnapshot is;
            is.id = std::to_string(inf.entity_handle);
            const int count = std::min(inf.fire_count, 64);
            for (int f = 0; f < count; ++f) {
                is.flames_x.push_back(inf.fire_positions[f].x);
                is.flames_y.push_back(inf.fire_positions[f].y);
                is.flames_z.push_back(inf.fire_positions[f].z);
            }
            snap.infernos.push_back(std::move(is));
        }

        // Drop stale smoke timers for entities that are gone.
        if (radar_smoke_first_seen_.size() > 32) {
            radar_smoke_first_seen_.clear();
        }
    }

    // --- Projectiles ---------------------------------------------------------
    if (cfg.radar_show_projectiles) {
        for (int i = 0; i < packet.projectile_count && i < static_cast<int>(shm::MAX_PROJECTILES); ++i) {
            const InFlightProjectile& pr = packet.projectiles[i];
            if (!pr.active) continue;

            std::string type;
            switch (pr.type) {
                case GRENADE_HE:
                    type = "frag";
                    break;
                case GRENADE_FLASH:
                    type = "flashbang";
                    break;
                case GRENADE_SMOKE:
                    type = "smoke";
                    break;
                case GRENADE_MOLOTOV:
                    type = "firebomb";
                    break;
                default:
                    continue;  // decoy / c4 / none have no radar sprite
            }

            radar::ProjectileSnapshot ps;
            ps.id = type + std::to_string(pr.entity_handle);
            ps.type = type;
            ps.team = (pr.team == 3) ? "CT" : "T";
            ps.x = pr.current_position.x;
            ps.y = pr.current_position.y;
            ps.z = pr.current_position.z;
            snap.projectiles.push_back(std::move(ps));
        }
    }

    radar_server.publish(snap);
}
