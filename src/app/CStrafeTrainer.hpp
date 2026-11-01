#pragma once
#include <string>
#include <vector>
#include <deque>
#include <algorithm>
#include <cmath>
#include <GLFW/glfw3.h>
#include "config.hpp"
#include "logger.hpp"
#include "overlay/game_reader.hpp"
#include "overlay/global_input.hpp"
#include "external/imgui/imgui.h"
#include "menu/menu_theme.hpp"

class CStrafeTrainer {
private:
    struct AxisState {
        int key1 = -1; // e.g. W or A
        int key2 = -1; // e.g. S or D
        
        bool key1_held = false;
        bool key2_held = false;
        
        double key1_press_time = 0.0;
        double key2_press_time = 0.0;
        
        int cs_release_key = -1;
        double cs_release_time = 0.0;
        int cs_press_key = -1;
        double cs_press_time = 0.0;
        
        double overlap_start_time = -1.0;
        
        void reset() {
            cs_release_key = -1;
            cs_release_time = 0.0;
            cs_press_key = -1;
            cs_press_time = 0.0;
            overlap_start_time = -1.0;
        }
        
        void on_press(int key, double timestamp) {
            int other = (key == key1) ? key2 : key1;
            if (key == key1) {
                key1_held = true;
                key1_press_time = timestamp;
            } else {
                key2_held = true;
                key2_press_time = timestamp;
            }
            
            bool other_held = (other == key1) ? key1_held : key2_held;
            if (other_held && overlap_start_time < 0.0) {
                overlap_start_time = timestamp;
            }
            
            if (cs_release_key == other && cs_press_time <= 0.0) {
                cs_press_key = key;
                cs_press_time = timestamp;
            }
        }
        
        void on_release(int key, double timestamp) {
            if (key == key1) {
                key1_held = false;
            } else {
                key2_held = false;
            }
            cs_release_key = key;
            cs_release_time = timestamp;
            
            cs_press_key = -1;
            cs_press_time = 0.0;
        }
        
        struct AxisResult {
            std::string label = "Bad";
            double val1 = 0.0; // overlap_time OR cs_time
            double val2 = 0.0; // shot_delay
        };
        
        AxisResult classify_shot(double shot_time) {
            AxisResult res;
            if (overlap_start_time >= 0.0) {
                bool has_valid_cs = (cs_press_time > 0.0 && cs_release_time > 0.0 &&
                                     cs_release_time > overlap_start_time &&
                                     cs_press_time > cs_release_time);
                if (!has_valid_cs) {
                    res.label = "Overlap";
                    res.val1 = shot_time - overlap_start_time;
                    reset();
                    return res;
                }
            }
            if (cs_press_time > 0.0 && cs_release_time > 0.0 && cs_press_time > cs_release_time) {
                res.label = "Counter-strafe";
                res.val1 = cs_press_time - cs_release_time;
                res.val2 = shot_time - cs_press_time;
                reset();
                return res;
            }
            reset();
            res.label = "Bad";
            return res;
        }
    };
    
    AxisState vertical;
    AxisState horizontal;
    
    bool shot_was_held = false;
    
public:
    struct ShotResult {
        std::string label = "Bad";
        double cs_time = 0.0;
        double shot_delay = 0.0;
        double overlap_time = 0.0;
        float speed = 0.0f;
        bool accurate = false;
        double timestamp = 0.0;
    };
    
private:
    std::deque<ShotResult> shot_history;
    
    struct SpeedPoint {
        float speed = 0.0f;
        int event_type = 0; // 0 = none, 1 = release, 2 = press, 3 = shot, 4 = overlap
        std::string event_desc = "";
    };
    std::deque<SpeedPoint> speed_history;
    
    int pending_event_type = 0;
    std::string pending_event_desc = "";
    
    // Helper to extract configured keybinds
    int get_key(const OverlayConfig& cfg, const std::string& setting_name, int default_key) {
        for (const auto& kb : cfg.keybinds) {
            if (kb.setting_name == setting_name) {
                return kb.key;
            }
        }
        return default_key;
    }
    
public:
    CStrafeTrainer(const OverlayConfig& cfg) {
        vertical.key1 = get_key(cfg, "key_w", 87);
        vertical.key2 = get_key(cfg, "key_s", 83);
        horizontal.key1 = get_key(cfg, "key_a", 65);
        horizontal.key2 = get_key(cfg, "key_d", 68);
        
        // Populate speed history with zeros initially
        for (int i = 0; i < 120; ++i) {
            speed_history.push_back({0.0f, 0, ""});
        }
        BUNNYWARE_LOG_INFO("CStrafeTrainer: Initialized.");
    }
    
    void update(const FrameInput& input, const OverlayConfig& cfg, GlobalInputReader& global_input) {
        double current_time_ms = glfwGetTime() * 1000.0;
        
        // Synchronize keys dynamically in case config changes
        vertical.key1 = get_key(cfg, "key_w", 87);
        vertical.key2 = get_key(cfg, "key_s", 83);
        horizontal.key1 = get_key(cfg, "key_a", 65);
        horizontal.key2 = get_key(cfg, "key_d", 68);
        int shot_key = get_key(cfg, "key_left_mouse", 1000);
        
        // 1. Process key state transitions
        auto check_key = [&](int glfw_key, AxisState& axis, int key_id, const std::string& key_name) {
            bool now_held = global_input.is_key_pressed(glfw_key);
            bool& was_held = (key_id == axis.key1) ? axis.key1_held : axis.key2_held;
            if (now_held != was_held) {
                if (now_held) {
                    axis.on_press(key_id, current_time_ms);
                    pending_event_type = 2; // Press
                    pending_event_desc = key_name;
                } else {
                    axis.on_release(key_id, current_time_ms);
                    pending_event_type = 1; // Release
                    pending_event_desc = key_name + "^";
                }
                was_held = now_held;
            }
        };
        
        check_key(vertical.key1, vertical, vertical.key1, "W");
        check_key(vertical.key2, vertical, vertical.key2, "S");
        check_key(horizontal.key1, horizontal, horizontal.key1, "A");
        check_key(horizontal.key2, horizontal, horizontal.key2, "D");
        
        // Get horizontal speed of player
        glm::vec3 vel = input.packet.local_velocity;
        float speed = std::sqrt(vel.x * vel.x + vel.y * vel.y);
        
        // 2. Detect shots (Left Mouse click)
        bool shot_now_held = global_input.is_key_pressed(shot_key);
        if (shot_now_held && !shot_was_held) {
            AxisState::AxisResult v_res = vertical.classify_shot(current_time_ms);
            AxisState::AxisResult h_res = horizontal.classify_shot(current_time_ms);
            
            std::string label = "Bad";
            double val1 = 0.0;
            double val2 = 0.0;
            
            int v_score = (v_res.label == "Overlap") ? 2 : (v_res.label == "Counter-strafe") ? 1 : 0;
            int h_score = (h_res.label == "Overlap") ? 2 : (h_res.label == "Counter-strafe") ? 1 : 0;
            
            if (v_score > h_score) {
                label = v_res.label;
                val1 = v_res.val1;
                val2 = v_res.val2;
            } else if (h_score > v_score) {
                label = h_res.label;
                val1 = h_res.val1;
                val2 = h_res.val2;
            } else {
                if (v_res.val1 >= h_res.val1) {
                    label = v_res.label;
                    val1 = v_res.val1;
                    val2 = v_res.val2;
                } else {
                    label = h_res.label;
                    val1 = h_res.val1;
                    val2 = h_res.val2;
                }
            }
            
            ShotResult result;
            result.label = label;
            result.speed = speed;
            result.accurate = (speed < 80.0f);
            result.timestamp = current_time_ms;
            
            if (label == "Counter-strafe") {
                result.cs_time = val1;
                result.shot_delay = val2;
            } else if (label == "Overlap") {
                result.overlap_time = val1;
            }
            
            shot_history.push_front(result);
            if (shot_history.size() > 30) {
                shot_history.pop_back();
            }
            
            pending_event_type = 3;
            pending_event_desc = result.accurate ? "Shot" : "Shot (Bad)";
        }
        shot_was_held = shot_now_held;
        
        // 3. Push to speed history
        SpeedPoint pt;
        pt.speed = speed;
        pt.event_type = pending_event_type;
        pt.event_desc = pending_event_desc;
        
        pending_event_type = 0;
        pending_event_desc = "";
        
        speed_history.push_back(pt);
        if (speed_history.size() > 120) {
            speed_history.pop_front();
        }
    }
    
    void draw(const OverlayConfig& cfg, float /*dt*/) {
        if (!cfg.cstrafe_enabled) return;

        const float scale = std::clamp(cfg.cstrafe_scale, 0.75f, 2.0f);
        ImGui::SetNextWindowPos(ImVec2(cfg.cstrafe_x, cfg.cstrafe_y), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(220.0f * scale, 290.0f * scale), ImGuiCond_FirstUseEver);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, MenuTheme::panel_rounding * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, scale);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f * scale, 8.0f * scale));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f * scale, 6.0f * scale));

        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(MenuTheme::bg_card.x, MenuTheme::bg_card.y, MenuTheme::bg_card.z, 0.85f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(MenuTheme::border_subtle.x, MenuTheme::border_subtle.y, MenuTheme::border_subtle.z, std::max(MenuTheme::border_subtle.w, 0.35f)));

        ImGui::Begin("Counter-Strafe Coach", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::SetWindowFontScale(scale);

        ImVec2 pos = ImGui::GetCursorScreenPos();
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        float title_y = pos.y;

        draw_list->AddRectFilled(ImVec2(pos.x, title_y + 2.0f * scale),
                                 ImVec2(pos.x + 3.0f * scale, title_y + 15.0f * scale),
                                 ImGui::GetColorU32(MenuTheme::accent_primary), 1.5f * scale);

        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 8.0f * scale);
        ImGui::TextColored(MenuTheme::text_primary, "Counter-Strafe Coach");

        ImGui::Spacing();
        ImGui::Separator();
        
        if (!shot_history.empty()) {
            const auto& last = shot_history.front();
            
            ImVec4 label_col;
            if (last.label == "Counter-strafe") {
                label_col = ImVec4(0.45f, 0.85f, 0.55f, 1.0f); // Soft active green
            } else if (last.label == "Overlap") {
                label_col = MenuTheme::accent_primary;  // Theme accent
            } else {
                label_col = ImVec4(0.90f, 0.35f, 0.35f, 1.0f);    // Soft Red
            }
            
            ImGui::TextColored(MenuTheme::text_secondary, "Last Shot: ");
            ImGui::SameLine();
            ImGui::TextColored(label_col, "%s", last.label.c_str());
            
            if (last.label == "Counter-strafe") {
                ImGui::TextColored(MenuTheme::text_secondary, "Transition: %.0f ms", last.cs_time);
                if (last.cs_time < 50.0) {
                    ImGui::SameLine(); ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.55f, 1.0f), "[Perfect]");
                } else if (last.cs_time < 100.0) {
                    ImGui::SameLine(); ImGui::TextColored(MenuTheme::accent_primary, "[Good]");
                } else {
                    ImGui::SameLine(); ImGui::TextColored(ImVec4(0.90f, 0.45f, 0.45f, 1.0f), "[Slow]");
                }
                
                ImGui::TextColored(MenuTheme::text_secondary, "Shot Delay: %.0f ms", last.shot_delay);
            } else if (last.label == "Overlap") {
                ImGui::TextColored(MenuTheme::text_secondary, "Overlap: %.0f ms", last.overlap_time);
            }
            
            ImGui::TextColored(MenuTheme::text_secondary, "Speed: %.1f u/s", last.speed);
            ImGui::SameLine();
            if (last.accurate) {
                ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.55f, 1.0f), "[Accurate]");
            } else {
                ImGui::TextColored(ImVec4(0.90f, 0.35f, 0.35f, 1.0f), "[INACCURATE]");
            }
        } else {
            ImGui::TextColored(MenuTheme::text_muted, "Shoot while stopping...");
        }
        
        ImGui::Separator();
        
        if (cfg.cstrafe_show_graph) {
            ImVec2 start_pos = ImGui::GetCursorScreenPos();
            float graph_w = 200.0f * scale;
            float graph_h = 60.0f * scale;

            draw_list->AddRectFilled(start_pos, ImVec2(start_pos.x + graph_w, start_pos.y + graph_h), ImGui::GetColorU32(ImVec4(MenuTheme::bg_input.x, MenuTheme::bg_input.y, MenuTheme::bg_input.z, 0.6f)));
            draw_list->AddRect(start_pos, ImVec2(start_pos.x + graph_w, start_pos.y + graph_h),
                               ImGui::GetColorU32(MenuTheme::border_subtle), 0.0f, 0, scale);

            float max_speed_val = 260.0f;
            size_t count = speed_history.size();
            std::vector<ImVec2> line_points;
            line_points.reserve(count);
            
            for (size_t i = 0; i < count; ++i) {
                const auto& pt = speed_history[i];
                float x = start_pos.x + (static_cast<float>(i) / (count - 1)) * graph_w;
                float y = start_pos.y + graph_h - (std::min(pt.speed, max_speed_val) / max_speed_val) * graph_h;
                line_points.push_back(ImVec2(x, y));
                
                if (pt.event_type > 0) {
                    ImU32 ev_col;
                    if (pt.event_type == 1)      ev_col = ImGui::GetColorU32(MenuTheme::text_muted); // release
                    else if (pt.event_type == 2) ev_col = ImGui::GetColorU32(ImVec4(0.35f, 0.65f, 0.90f, 0.7f));  // press (blue)
                    else if (pt.event_type == 3) {
                        ev_col = pt.event_desc.find("Bad") == std::string::npos ? 
                                 ImGui::GetColorU32(ImVec4(0.45f, 0.85f, 0.55f, 1.0f)) : ImGui::GetColorU32(ImVec4(0.90f, 0.35f, 0.35f, 1.0f)); // shot
                    }
                    else ev_col = ImGui::GetColorU32(MenuTheme::accent_primary); // overlap
                    
                    draw_list->AddLine(ImVec2(x, start_pos.y), ImVec2(x, start_pos.y + graph_h), ev_col, scale);

                    if (!pt.event_desc.empty()) {
                        draw_list->AddText(ImGui::GetFont(), 10.0f * scale,
                                           ImVec2(x + 2.0f * scale, start_pos.y + 2.0f * scale), ev_col,
                                           pt.event_desc.c_str());
                    }
                }
            }
            
            if (line_points.size() > 1) {
                for (size_t i = 0; i < line_points.size() - 1; ++i) {
                    draw_list->AddLine(line_points[i], line_points[i+1], ImGui::GetColorU32(MenuTheme::text_primary),
                                       1.5f * scale);
                }
            }
            
            float threshold_y = start_pos.y + graph_h - (80.0f / max_speed_val) * graph_h;
            draw_list->AddLine(ImVec2(start_pos.x, threshold_y), ImVec2(start_pos.x + graph_w, threshold_y),
                               ImGui::GetColorU32(MenuTheme::accent_primary), scale);
            
            ImGui::Dummy(ImVec2(graph_w, graph_h));
        }
        
        ImGui::Separator();

        ImGui::TextColored(MenuTheme::text_secondary, "History (%d):", static_cast<int>(shot_history.size()));
        float dot_radius = 4.0f * scale;
        float spacing = 12.0f * scale;
        ImVec2 start_dots = ImGui::GetCursorScreenPos();
        
        int show_count = std::min(15, static_cast<int>(shot_history.size()));
        for (int i = 0; i < show_count; ++i) {
            const auto& shot = shot_history[i];
            float dx = start_dots.x + dot_radius + i * spacing;
            float dy = start_dots.y + dot_radius + 4.0f * scale;
            
            ImU32 col;
            if (shot.label == "Counter-strafe") {
                col = ImGui::GetColorU32(ImVec4(0.45f, 0.85f, 0.55f, 1.0f)); // green
            } else if (shot.label == "Overlap") {
                col = ImGui::GetColorU32(MenuTheme::accent_primary);  // accent
            } else {
                col = ImGui::GetColorU32(ImVec4(0.90f, 0.35f, 0.35f, 1.0f));   // red
            }
            
            draw_list->AddCircleFilled(ImVec2(dx, dy), dot_radius, col);
            if (shot.accurate) {
                draw_list->AddCircle(ImVec2(dx, dy), dot_radius + 1.5f * scale,
                                     ImGui::GetColorU32(MenuTheme::text_primary), 8, scale);
            }
        }

        ImGui::Dummy(ImVec2(200.0f * scale, 15.0f * scale));

        ImVec2 w_pos = ImGui::GetWindowPos();
        if (w_pos.x != cfg.cstrafe_x || w_pos.y != cfg.cstrafe_y) {
            cfg.cstrafe_x = w_pos.x;
            cfg.cstrafe_y = w_pos.y;
            cfg.mark_dirty();
        }
        
        ImGui::End();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(4);
    }
};
