#include "renderer/gl_loader.hpp"
#include "menu_tabs.hpp"
#include "menu_client.hpp"
#include "overlay/overlay_client.hpp"
#include "app/App.hpp"
#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "config.hpp"
#include "app/RadarServer.hpp"
#include "external/imgui/imgui.h"
#include "external/imgui/imgui_internal.h"
#include "logger.hpp"
#include "menu_animation.hpp"
#include "menu_tooltip.hpp"
#include "menu_theme.hpp"
#include "menu/color_clipboard.hpp"
#include "math/Distance.hpp"
#include "overlay/esp_drawing.hpp"
#include "overlay/game_reader.hpp"
#include "overlay/offset_refresher.hpp"
#include "app/SvgCache.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Section helpers — title as external header, body as elevated card on page bg
// ─────────────────────────────────────────────────────────────────────────────

namespace {
struct SectionAnimationState {
    float target_height = 0.0f;
};

struct SectionAnimationScope {
    ImGuiID id = 0;
    bool auto_sized = false;
    bool content_visible = true;
};

struct TabContentTransition {
    float phase = 1.0f;
    bool entering = true;
    bool active = false;
};

std::unordered_map<ImGuiID, SectionAnimationState> g_section_animation_states;
std::vector<SectionAnimationScope> g_section_animation_scopes;
TabContentTransition g_tab_content_transition;
}  // namespace

void set_tab_content_transition(float phase, bool entering, bool active)
{
    g_tab_content_transition.phase = std::clamp(phase, 0.0f, 1.0f);
    g_tab_content_transition.entering = entering;
    g_tab_content_transition.active = active;
}

static bool RenderSectionHeader(const char* title, const char* id, bool collapsed, bool highlighted,
                                float& open_progress)
{
    const ImGuiID header_id = ImGui::GetID(id) ^ 0x5a17c3e9U;
    open_progress = MenuAnimation::get().animate_from(header_id, collapsed ? 0.0f : 1.0f,
                                                      collapsed ? 0.0f : 1.0f, 18.0f);

    const float row_height = ImGui::GetTextLineHeight();
    const float row_width = std::max(1.0f, ImGui::GetContentRegionAvail().x - 8.0f);
    const ImVec2 row_min = ImGui::GetCursorScreenPos();
    ImGui::PushID(id);
    const bool clicked = ImGui::InvisibleButton("##section_header", ImVec2(row_width, row_height));
    ImGui::PopID();
    const bool hovered = ImGui::IsItemHovered();
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    const float hover_progress = MenuAnimation::get().animate(header_id ^ 0x7f3a1b55U, hovered ? 1.0f : 0.0f, 18.0f);
    ImVec4 text_color = MenuTheme::text_secondary;
    text_color.x += (MenuTheme::text_primary.x - text_color.x) * hover_progress;
    text_color.y += (MenuTheme::text_primary.y - text_color.y) * hover_progress;
    text_color.z += (MenuTheme::text_primary.z - text_color.z) * hover_progress;
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    if (highlighted) {
        ImVec4 highlight_color = MenuTheme::accent_primary;
        highlight_color.w = 0.08f;
        draw_list->AddRectFilled(
            ImVec2(row_min.x - 4.0f, row_min.y - 2.0f),
            ImVec2(row_min.x + row_width, row_min.y + row_height + 2.0f),
            ImGui::GetColorU32(highlight_color), 4.0f);
        text_color = MenuTheme::text_primary;
    }
    draw_list->AddText(row_min, ImGui::GetColorU32(text_color), title);

    const float scale = ImGui::GetFontSize() / 13.0f;
    const float half_width = 3.5f * scale;
    const float half_height = 2.5f * scale;
    const float angle = -1.5707963f * (1.0f - open_progress);
    const float cos_a = std::cos(angle);
    const float sin_a = std::sin(angle);
    const ImVec2 center(std::round(row_min.x + row_width - half_width - 4.0f * scale),
                        std::round(row_min.y + row_height * 0.5f));
    const auto rotate = [&](float x, float y) {
        return ImVec2(center.x + x * cos_a - y * sin_a, center.y + x * sin_a + y * cos_a);
    };
    const ImU32 chevron_color = ImGui::GetColorU32(text_color);
    draw_list->AddLine(rotate(-half_width, -half_height), rotate(0.0f, half_height), chevron_color, 1.5f * scale);
    draw_list->AddLine(rotate(0.0f, half_height), rotate(half_width, -half_height), chevron_color, 1.5f * scale);
    return clicked;
}

void BeginSection(const char* title, const char* id)
{
    MenuClient* menu = MenuClient::get_instance();
    const char* section_key = id ? id : "";
    while (*section_key == '#') ++section_key;
    const bool search_target = menu && menu->is_search_section_target(section_key);
    const bool search_highlight = menu && menu->is_search_section_highlighted(section_key);
    float transition_alpha = 1.0f;
    if (g_tab_content_transition.active) {
        const ImGuiWindow* window = ImGui::GetCurrentWindow();
        const float window_height = std::max(1.0f, window->Size.y);
        const float relative_y =
            std::clamp((ImGui::GetCursorScreenPos().y - window->Pos.y) / window_height, 0.0f, 1.0f);
        const float delay = relative_y * 0.10f;
        const float local_phase =
            std::clamp((g_tab_content_transition.phase - delay) / (1.0f - delay), 0.0f, 1.0f);
        const float eased_phase = local_phase * local_phase * (3.0f - 2.0f * local_phase);
        transition_alpha = g_tab_content_transition.entering ? eased_phase : 1.0f - eased_phase;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * transition_alpha);

    // Header sits outside the card so page bg (bg_main) shows between groups
    // and the card itself can use bg_card without a "cutout" title strip.
    const bool has_header = title && title[0] != '\0' && title[0] != '#';
    bool collapsed = false;
    bool was_collapsed = false;
    float open_progress = 1.0f;
    if (has_header) {
        was_collapsed = is_section_collapsed(section_key);
        collapsed = was_collapsed;
        if (search_target) collapsed = false;
        if (RenderSectionHeader(title, id, collapsed, search_target || search_highlight, open_progress)) {
            collapsed = !collapsed;
            set_section_collapsed(section_key, collapsed);
            schedule_menu_preferences_save();
        }
        // Pull the card up slightly under the header (standalone card spacing).
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 2.0f);
    }

    const ImGuiID section_id = ImGui::GetID(id);
    SectionAnimationState& animation_state = g_section_animation_states[section_id];
    const bool auto_sized = !collapsed && animation_state.target_height <= 0.0f;
    const float height_target = collapsed ? 0.0f : animation_state.target_height;
    const float animated_height =
        auto_sized ? 0.0f
                   : MenuAnimation::get().animate_from(section_id ^ 0x34eb92d1U, height_target, height_target, 18.0f);
    const bool card_visible = auto_sized || animated_height >= 1.0f;

    ImVec4 section_background = MenuTheme::bg_card;
    if (search_target || search_highlight) {
        const float tint = 0.10f;
        section_background.x += (MenuTheme::accent_primary.x - section_background.x) * tint;
        section_background.y += (MenuTheme::accent_primary.y - section_background.y) * tint;
        section_background.z += (MenuTheme::accent_primary.z - section_background.z) * tint;
    }
    ImGui::PushStyleColor(ImGuiCol_ChildBg,
                          card_visible ? section_background : ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, MenuTheme::border_subtle);
    // Padding only — ChildRounding comes from apply_dark_theme + menu_scale.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 10.0f));

    ImGuiChildFlags child_flags = ImGuiChildFlags_AlwaysUseWindowPadding;
    if (card_visible) child_flags |= ImGuiChildFlags_Border;
    if (auto_sized) child_flags |= ImGuiChildFlags_AutoResizeY;
    const bool content_visible =
        ImGui::BeginChild(id, ImVec2(-8.0f, auto_sized ? 0.0f : std::max(animated_height, 1.0f)), child_flags,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    g_section_animation_scopes.push_back({section_id, auto_sized, content_visible});
    if (search_target && content_visible) {
        if (was_collapsed) {
            set_section_collapsed(section_key, false);
        }
        ImGuiWindow* section_window = ImGui::GetCurrentWindow();
        if (section_window->ParentWindow) {
            const float local_y = section_window->Pos.y - section_window->ParentWindow->Pos.y;
            ImGui::SetScrollFromPosY(section_window->ParentWindow, local_y, 0.0f);
        }
        menu->clear_search_section_target();
    }
}

void EndSection()
{
    IM_ASSERT(!g_section_animation_scopes.empty());
    const SectionAnimationScope scope = g_section_animation_scopes.back();
    g_section_animation_scopes.pop_back();

    // CursorPos includes trailing ItemSpacing after the final setting. Measure
    // the actual content boundary instead, then add exactly the same padding
    // used above the first setting.
    const ImGuiWindow* section_window = ImGui::GetCurrentWindow();
    const float content_bottom = section_window->DC.CursorMaxPos.y - section_window->Pos.y;
    const float measured_height = content_bottom + ImGui::GetStyle().WindowPadding.y;
    if ((scope.auto_sized || scope.content_visible) && measured_height > 1.0f)
        g_section_animation_states[scope.id].target_height = measured_height;

    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);

    // Breathing room between section header+card blocks
    ImGui::Dummy(ImVec2(0.0f, 3.0f));
}

namespace {
struct AnimatedCardState {
    float full_height = 0.0f;
    bool was_visible = false;
};

struct AnimatedCardScope {
    ImGuiID id = 0;
    float content_start_y = 0.0f;
    float parent_start_y = 0.0f;
    float logical_height = 0.0f;
    float eased_progress = 1.0f;
    bool clipped = false;
    bool content_visible = true;
};

std::unordered_map<ImGuiID, AnimatedCardState> g_animated_card_states;
std::vector<AnimatedCardScope> g_animated_card_scopes;

float smoothstep(float value)
{
    value = std::clamp(value, 0.0f, 1.0f);
    return value * value * (3.0f - 2.0f * value);
}

bool BeginAnimatedCard(const char* id, bool visible, bool start_expanded = false)
{
    const ImGuiID card_id = ImGui::GetID(id);
    MenuAnimation& animation = MenuAnimation::get();
    AnimatedCardState& state = g_animated_card_states[card_id];
    const std::uint32_t anim_id = card_id ^ 0x7a41c29dU;

    // Cards normally grow in from a collapsed sliver. `start_expanded` seeds the
    // animation at its open value on the hidden -> visible edge, so the card is
    // already expanded the moment it appears while still animating closed.
    if (start_expanded && visible && !state.was_visible) animation.set(anim_id, 1.0f);
    state.was_visible = visible;

    const float progress = animation.animate_from(anim_id, visible ? 1.0f : 0.0f, 0.0f, 18.0f);

    if (!visible && progress <= 0.002f) return false;

    ImGui::PushID(static_cast<int>(card_id));

    if (!animation.enabled()) {
        g_animated_card_scopes.push_back(
            {card_id, ImGui::GetCursorPosY(), ImGui::GetCursorPosY(), 0.0f, 1.0f, false, true});
        return true;
    }

    const float eased_progress = smoothstep(progress);
    const float fallback_height = ImGui::GetFrameHeightWithSpacing() * 4.0f;
    const float measured_height = state.full_height > 0.0f ? state.full_height : fallback_height;
    const float logical_height = measured_height * eased_progress;
    const float child_height = std::max(1.0f, logical_height);
    const float parent_start_y = ImGui::GetCursorPosY();
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * eased_progress);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground;
    if (!visible) flags |= ImGuiWindowFlags_NoInputs;
    const bool content_visible =
        ImGui::BeginChild("##animated_card_clip", ImVec2(0.0f, child_height), ImGuiChildFlags_None, flags);

    g_animated_card_scopes.push_back(
        {card_id, ImGui::GetCursorPosY(), parent_start_y, logical_height, eased_progress, true, content_visible});
    return true;
}

void EndAnimatedCard()
{
    IM_ASSERT(!g_animated_card_scopes.empty());
    const AnimatedCardScope scope = g_animated_card_scopes.back();
    g_animated_card_scopes.pop_back();

    const float measured_height = ImGui::GetCursorPosY() - scope.content_start_y;
    if ((!scope.clipped || scope.content_visible) && measured_height > 8.0f)
        g_animated_card_states[scope.id].full_height = measured_height;

    if (scope.clipped) {
        ImGui::EndChild();
        ImGui::PopStyleVar();

        // BeginChild() enforces a four-pixel minimum and EndChild() always adds
        // ItemSpacing. Re-submit a zero-size item at the animated endpoint so
        // the following card tracks the animation exactly down to zero.
        const float spacing = ImGui::GetStyle().ItemSpacing.y;
        ImGui::SetCursorPosY(scope.parent_start_y + scope.logical_height -
                             spacing * (1.0f - scope.eased_progress));
        ImGui::Dummy(ImVec2(0.0f, 0.0f));
    }
    ImGui::PopID();
    MenuTooltip::show_item("Selects the hitbox groups that can be targeted.");
}
}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Two-column layout helpers
// ─────────────────────────────────────────────────────────────────────────────

// BeginTable() returns false when the table is clipped (e.g. during a tab slide
// transition or while scrolled out of view). On that path g.CurrentTable is left
// untouched -- and because we are inside a child window, SetCurrentWindow() has
// already set it to NULL -- so every Table* helper below must be skipped or it
// will dereference a null table.
static bool g_two_column_layout_active = false;

static void BeginTwoColumnLayout(const char* id)
{
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(4.0f, 0.0f));
    g_two_column_layout_active = ImGui::BeginTable(id, 2, ImGuiTableFlags_None);
    if (!g_two_column_layout_active)
        return;
    ImGui::TableSetupColumn("Col1", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Col2", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
}

static void SwitchColumn()
{
    if (g_two_column_layout_active)
        ImGui::TableNextColumn();
}

static void EndTwoColumnLayout()
{
    if (g_two_column_layout_active)
        ImGui::EndTable();
    g_two_column_layout_active = false;
    ImGui::PopStyleVar();
}

// ─────────────────────────────────────────────────────────────────────────────
// Layout helpers
// ─────────────────────────────────────────────────────────────────────────────

static float get_section_width()
{
    return ImGui::GetContentRegionAvail().x;
}

static const char* get_key_name(int key)
{
    if (key < 0) return "-";
    if (key >= 1000 && key <= 1007) {
        switch (key) {
            case 1000: return "M1";
            case 1001: return "M2";
            case 1002: return "M3";
            case 1003: return "M4";
            case 1004: return "M5";
            default: return "Mouse";
        }
    }
    const char* name = glfwGetKeyName(key, 0);
    if (name) return name;

    switch (key) {
        case GLFW_KEY_INSERT: return "INS";
        case GLFW_KEY_DELETE: return "DEL";
        case GLFW_KEY_ESCAPE: return "ESC";
        case GLFW_KEY_SPACE: return "SPACE";
        case GLFW_KEY_ENTER: return "ENTER";
        case GLFW_KEY_TAB: return "TAB";
        case GLFW_KEY_BACKSPACE: return "BACK";
        case GLFW_KEY_CAPS_LOCK: return "CAPS";
        case GLFW_KEY_LEFT_SHIFT: return "LSHIFT";
        case GLFW_KEY_RIGHT_SHIFT: return "RSHIFT";
        case GLFW_KEY_LEFT_CONTROL: return "LCTRL";
        case GLFW_KEY_RIGHT_CONTROL: return "RCTRL";
        case GLFW_KEY_LEFT_ALT: return "LALT";
        case GLFW_KEY_RIGHT_ALT: return "RALT";
        case GLFW_KEY_F1: return "F1";
        case GLFW_KEY_F2: return "F2";
        case GLFW_KEY_F3: return "F3";
        case GLFW_KEY_F4: return "F4";
        case GLFW_KEY_F5: return "F5";
        case GLFW_KEY_F6: return "F6";
        case GLFW_KEY_F7: return "F7";
        case GLFW_KEY_F8: return "F8";
        case GLFW_KEY_F9: return "F9";
        case GLFW_KEY_F10: return "F10";
        case GLFW_KEY_F11: return "F11";
        case GLFW_KEY_F12: return "F12";
        default: return "Key";
    }
}

static std::string GetKeybindLabel(MenuClient* menu, KeyBind* kb)
{
    if (menu->listening_keybind == kb) {
        return "[...]";
    } else if (kb->key < 0) {
        return "[-]";
    } else {
        return std::string("[") + get_key_name(kb->key) + "]";
    }
}

static void DrawKeybindButton(const char* setting_name)
{
    MenuClient* menu = MenuClient::get_instance();
    if (!menu || !setting_name) return;

    OverlayConfig& cfg = menu->get_cfg();

    KeyBind* kb = nullptr;
    for (auto& bind : cfg.keybinds) {
        if (bind.setting_name == setting_name) {
            kb = &bind;
            break;
        }
    }
    if (!kb) return;

    std::string btn_id = std::string("##bind_") + setting_name;
    std::string btn_label = GetKeybindLabel(menu, kb);

    bool is_listening = (menu->listening_keybind == kb);
    if (is_listening) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.0f, 1.0f));
    } else if (kb->key < 0) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_Text]);
    }

    ImGui::Text("%s", btn_label.c_str());
    ImGui::PopStyleColor();

    MenuTooltip::show_item("Left-click to bind key\nRight-click for options");
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        if (is_listening) {
            menu->stop_listening();
        } else {
            menu->start_listening(kb);
        }
    }

    if (ImGui::BeginPopupContextItem(btn_id.c_str(), ImGuiPopupFlags_MouseButtonRight)) {
        bool is_toggle = kb->mode == BindMode::TOGGLE;
        bool is_hold = kb->mode == BindMode::HOLD;
        if (ImGui::MenuItem("Toggle", nullptr, &is_toggle)) {
            kb->mode = BindMode::TOGGLE;
        }
        if (ImGui::MenuItem("Hold", nullptr, &is_hold)) {
            kb->mode = BindMode::HOLD;
        }

        if (kb->is_slider) {
            if (is_setting_bool(setting_name)) { // Fallback, shouldn't happen
            } else if (is_setting_float(setting_name)) {
                ImGui::InputFloat("##swap_val", &kb->slider_value, 0.1f, 1.0f, "%.2f");
            } else if (is_setting_int(setting_name)) {
                ImGui::InputInt("##swap_val_int", &kb->slider_value_int);
            }
        }

        ImGui::EndPopup();
    }
}

static ImVec4 LerpToggleColor(const ImVec4& from, const ImVec4& to, float amount)
{
    return ImVec4(from.x + (to.x - from.x) * amount, from.y + (to.y - from.y) * amount,
                  from.z + (to.z - from.z) * amount, from.w + (to.w - from.w) * amount);
}

static void DrawSearchSettingHighlight(const char* setting_name, const ImVec2& min, const ImVec2& max)
{
    MenuClient* menu = MenuClient::get_instance();
    if (!menu || !menu->is_search_setting_highlighted(setting_name)) return;

    ImVec4 highlight_color = MenuTheme::accent_primary;
    highlight_color.w = 0.08f;
    ImGui::GetWindowDrawList()->AddRectFilled(min, max, ImGui::GetColorU32(highlight_color), 4.0f);
}

static bool RenderDangerHoverButton(const char* label, const char* id, const ImVec2& size)
{
    const ImVec2 button_pos = ImGui::GetCursorScreenPos();
    const ImVec2 button_size = ImGui::CalcItemSize(size, 0.0f, 0.0f);
    const ImVec2 button_max(button_pos.x + button_size.x, button_pos.y + button_size.y);
    const bool hovered = ImGui::IsMouseHoveringRect(button_pos, button_max);
    const float hover_animation = MenuAnimation::get().animate(ImGui::GetID(id), hovered ? 1.0f : 0.0f, 20.0f);

    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec4 hover_red(0.70f, 0.20f, 0.20f, 1.0f);
    const ImVec4 active_red(0.50f, 0.12f, 0.12f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, style.Colors[ImGuiCol_Button]);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          LerpToggleColor(style.Colors[ImGuiCol_ButtonHovered], hover_red, hover_animation));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                          LerpToggleColor(style.Colors[ImGuiCol_ButtonActive], active_red, hover_animation));
    const bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor(3);
    return pressed;
}

// Toggle/slider keybind label: renders nothing when unbound (no "[-]").
static void DrawSettingKeybindLabel(const char* setting_name)
{
    MenuClient* menu = MenuClient::get_instance();
    if (!menu || !setting_name) return;
    OverlayConfig& cfg = menu->get_cfg();
    KeyBind* kb = nullptr;
    for (auto& bind : cfg.keybinds) {
        if (bind.setting_name == setting_name) { kb = &bind; break; }
    }
    if (!kb) return;

    std::string btn_label = GetKeybindLabel(menu, kb);
    if (btn_label.empty() || btn_label == "[-]") return;

    const bool is_listening = (menu->listening_keybind == kb);
    if (is_listening) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.0f, 1.0f));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_Text]);
    }
    ImGui::Text("%s", btn_label.c_str());
    ImGui::PopStyleColor();

    MenuTooltip::show_item("Left-click to bind key\nRight-click the row for options");
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        if (is_listening) menu->stop_listening();
        else menu->start_listening(kb);
    }
}

// Hold/Toggle option row with animated selection highlight (radio behaviour).
static void RenderBindModeOption(const char* label, bool selected, BindMode target_mode, KeyBind* kb)
{
    const ImGuiID opt_id = ImGui::GetID(label);
    const float t = smoothstep(MenuAnimation::get().animate(opt_id ^ 0x5e7a1101U, selected ? 1.0f : 0.0f, 22.0f));
    const ImVec4 bg = LerpToggleColor(MenuTheme::bg_card, MenuTheme::accent_primary, t * 0.30f);
    const ImVec4 text_col = LerpToggleColor(MenuTheme::text_secondary, MenuTheme::text_primary, std::max(t, 0.6f));
    ImGui::PushStyleColor(ImGuiCol_Header, bg);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, MenuTheme::bg_card_hover);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, bg);
    ImGui::PushStyleColor(ImGuiCol_Text, text_col);
    const bool clicked = ImGui::Selectable(label, selected, ImGuiSelectableFlags_None,
                                           ImVec2(ImGui::GetContentRegionAvail().x, 0.0f));
    ImGui::PopStyleColor(4);
    if (clicked) kb->mode = target_mode;
}

// Bind-key button inside the popup; click toggles listen mode.
static void RenderBindKeyButton(KeyBind* kb, MenuClient* menu)
{
    const bool is_listening = (menu->listening_keybind == kb);
    std::string label;
    if (is_listening) label = "[...]  Press a key";
    else if (kb->key >= 0) label = std::string("[") + get_key_name(kb->key) + "]";
    else label = "Bind Key";

    const float btn_w = ImGui::GetContentRegionAvail().x;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool hovered = ImGui::IsMouseHoveringRect(pos, ImVec2(pos.x + btn_w, pos.y + ImGui::GetFrameHeight()));
    const ImGuiID btn_id = ImGui::GetID("##kb_bind_btn");
    const float hover_t = smoothstep(MenuAnimation::get().animate(btn_id, hovered ? 1.0f : 0.0f, 22.0f));
    const ImVec4 bg = LerpToggleColor(MenuTheme::bg_input, MenuTheme::bg_card_hover, hover_t);
    const ImVec4 text_col = is_listening ? ImVec4(1.0f, 0.8f, 0.0f, 1.0f)
        : LerpToggleColor(MenuTheme::text_secondary, MenuTheme::accent_primary, hover_t);
    ImGui::PushStyleColor(ImGuiCol_Button, bg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, MenuTheme::bg_card_hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, MenuTheme::accent_primary);
    ImGui::PushStyleColor(ImGuiCol_Text, text_col);
    const bool pressed = ImGui::Button(label.c_str(), ImVec2(btn_w, 0.0f));
    ImGui::PopStyleColor(4);
    if (pressed) {
        if (is_listening) menu->stop_listening();
        else menu->start_listening(kb);
    }
}

// Right-click anywhere on a setting row opens an animated keybind popup.
// Layout: Hold/Toggle, slider value (sliders only), bind-key button.
static void DrawKeybindPopupForRow(const char* setting_name, const ImVec2& row_min, const ImVec2& row_max)
{
    MenuClient* menu = MenuClient::get_instance();
    if (!menu || !setting_name) return;
    OverlayConfig& cfg = menu->get_cfg();
    KeyBind* kb = nullptr;
    for (auto& bind : cfg.keybinds) {
        if (bind.setting_name == setting_name) { kb = &bind; break; }
    }
    if (!kb) return;

    const std::string popup_id_str = "##kb_popup_" + std::string(setting_name);
    const ImGuiID popup_id = ImGui::GetID(popup_id_str.c_str());
    const bool was_open = ImGui::IsPopupOpen(popup_id_str.c_str(), ImGuiPopupFlags_None);

    if (!was_open && ImGui::IsMouseHoveringRect(row_min, row_max) &&
        ImGui::IsMouseReleased(ImGuiMouseButton_Right) && !ImGui::IsAnyItemActive()) {
        ImGui::OpenPopup(popup_id_str.c_str());
    }

    const float progress = smoothstep(MenuAnimation::get().animate_from(
        popup_id ^ 0x4b1b0000U, was_open ? 1.0f : 0.0f, 0.0f, 22.0f));

    const float scale = ImGui::GetFontSize() / 13.0f;
    const float min_w = 170.0f * scale;

    // Fixed width; height auto-sizes to content (no scrollbar). Default
    // WindowPadding is used (matches the color picker) so there's no extra
    // right/left padding. Rows below fill the content width for alignment.
    ImGui::SetNextWindowSizeConstraints(ImVec2(min_w, 0.0f), ImVec2(min_w, FLT_MAX));
    ImGui::SetNextWindowBgAlpha(progress);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * std::max(progress, 0.05f));

    if (ImGui::BeginPopup(popup_id_str.c_str(), ImGuiWindowFlags_AlwaysAutoResize)) {
        const bool is_toggle = (kb->mode == BindMode::TOGGLE);
        const bool is_hold = (kb->mode == BindMode::HOLD);
        RenderBindModeOption("Hold", is_hold, BindMode::HOLD, kb);
        RenderBindModeOption("Toggle", is_toggle, BindMode::TOGGLE, kb);

        if (kb->is_slider) {
            ImGui::Spacing();
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            if (is_setting_bool(setting_name)) {
            } else if (is_setting_float(setting_name)) {
                ImGui::InputFloat("##swap_val", &kb->slider_value, 0.1f, 1.0f, "%.2f");
            } else if (is_setting_int(setting_name)) {
                ImGui::InputInt("##swap_val_int", &kb->slider_value_int);
            }
        }

        ImGui::Spacing();
        RenderBindKeyButton(kb, menu);
        ImGui::EndPopup();
    }

    ImGui::PopStyleVar();
}

static bool RenderAnimatedToggleSwitch(const char* id, bool* value, const ImVec2& size)
{
    const ImGuiID toggle_id = ImGui::GetID(id);
    const bool pressed = ImGui::InvisibleButton(id, size);
    if (pressed) {
        *value = !*value;
        ImGui::MarkItemEdited(toggle_id);
    }

    const float raw_animation = MenuAnimation::get().animate(toggle_id, *value ? 1.0f : 0.0f, 26.0f);
    const float animation = smoothstep(raw_animation);
    const ImVec4 track_off(0.220f, 0.208f, 0.200f, 1.0f);
    const ImVec4 track_on = MenuTheme::accent_primary;
    const ImVec4 knob_off = MenuTheme::accent_hover;
    const ImVec4 knob_on = MenuTheme::bg_main;

    ImVec2 min = ImGui::GetItemRectMin();
    ImVec2 max = ImGui::GetItemRectMax();
    min.x = std::round(min.x);
    min.y = std::round(min.y);
    max.x = std::round(max.x);
    max.y = std::round(max.y);

    const float radius = size.y * 0.5f;
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(min, max, ImGui::GetColorU32(LerpToggleColor(track_off, track_on, animation)),
                             radius);

    const float knob_travel = size.x - size.y;
    float knob_animation = animation;
    // Settle once rounding already places the knob at its endpoint. This keeps
    // the final state exact without producing a visible one-frame snap.
    if (*value && (1.0f - knob_animation) * knob_travel < 0.5f)
        knob_animation = 1.0f;
    else if (!*value && knob_animation * knob_travel < 0.5f)
        knob_animation = 0.0f;
    const float knob_x = std::round(min.x + radius + knob_animation * knob_travel);
    const float knob_y = std::round(min.y + radius);
    const float knob_inset = size.y * (3.0f / 22.0f);
    draw_list->AddCircleFilled(ImVec2(knob_x, knob_y), radius - knob_inset,
                               ImGui::GetColorU32(LerpToggleColor(knob_off, knob_on, animation)));

    return pressed;
}

// Right-justified animated toggle: Label                 [key] (switch)
static bool RenderToggle(const char* label, const char* id, bool* val)
{
    const float scale = ImGui::GetFontSize() / 13.0f;
    constexpr float toggle_scale = 0.80f;
    const float toggle_w = 42.0f * scale * toggle_scale;
    const float toggle_h = 22.0f * scale * toggle_scale;
    const float spacing = 4.0f * scale;
    const float content_right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    MenuClient* menu = MenuClient::get_instance();
    const char* setting_name = menu ? get_setting_name_by_ptr(menu->get_cfg(), val) : nullptr;
    const ImVec2 row_min = ImGui::GetCursorScreenPos();
    DrawSearchSettingHighlight(
        setting_name, ImVec2(row_min.x - 4.0f, row_min.y - 2.0f),
        ImVec2(row_min.x + ImGui::GetContentRegionAvail().x,
               row_min.y + std::max(ImGui::GetTextLineHeight(), toggle_h) + 2.0f));

    ImGui::AlignTextToFramePadding();
    ImGui::Text("%s", label);
    const ImVec2 label_min = ImGui::GetItemRectMin();
    const ImVec2 label_max = ImGui::GetItemRectMax();
    const float toggle_y = std::round((label_min.y + label_max.y - toggle_h) * 0.5f);

    if (setting_name) {
        OverlayConfig& cfg = menu->get_cfg();
        KeyBind* kb = nullptr;
        for (auto& bind : cfg.keybinds) {
            if (bind.setting_name == setting_name) {
                kb = &bind;
                break;
            }
        }
        if (!kb) {
            KeyBind new_kb;
            new_kb.setting_name = setting_name;
            new_kb.is_slider = false;
            cfg.keybinds.push_back(new_kb);
            kb = &cfg.keybinds.back();
        }

        std::string btn_label = GetKeybindLabel(menu, kb);
        const bool has_key_label = !btn_label.empty() && btn_label != "[-]";
        const float text_w = has_key_label ? ImGui::CalcTextSize(btn_label.c_str()).x : 0.0f;
        ImGui::SameLine();
        ImGui::SetCursorPosX(content_right - toggle_w - text_w - spacing);
        DrawSettingKeybindLabel(setting_name);
    }

    ImGui::SameLine();
    ImGui::SetCursorPosX(content_right - toggle_w);
    ImVec2 toggle_pos = ImGui::GetCursorScreenPos();
    toggle_pos.y = toggle_y;
    ImGui::SetCursorScreenPos(toggle_pos);
    const bool changed = RenderAnimatedToggleSwitch(id, val, ImVec2(toggle_w, toggle_h));

    // Right-click anywhere on the label/key/toggle row opens the keybind popup.
    if (setting_name) {
        const ImVec2 toggle_min = ImGui::GetItemRectMin();
        const ImVec2 toggle_max = ImGui::GetItemRectMax();
        const ImVec2 row_min(label_min.x, std::min(label_min.y, toggle_min.y));
        const ImVec2 row_max(toggle_max.x, std::max(label_max.y, toggle_max.y));
        DrawKeybindPopupForRow(setting_name, row_min, row_max);
    }
    return changed;
}

// Right-justified animated toggle with tooltip
static void RenderToggleTip(const char* label, const char* id, bool* val, const char* tooltip)
{
    RenderToggle(label, id, val);
    MenuTooltip::show_item(tooltip);
}

static void RenderKeybindConfig(const char* label, const char* setting_name, int default_key)
{
    const ImVec2 row_min = ImGui::GetCursorScreenPos();
    DrawSearchSettingHighlight(
        setting_name, ImVec2(row_min.x - 4.0f, row_min.y - 2.0f),
        ImVec2(row_min.x + ImGui::GetContentRegionAvail().x,
               row_min.y + std::max(ImGui::GetTextLineHeight(), ImGui::GetFrameHeight()) + 2.0f));
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%s", label);
    ImGui::SameLine();
    
    MenuClient* menu = MenuClient::get_instance();
    if (menu) {
        OverlayConfig& cfg = menu->get_cfg();
        KeyBind* kb = nullptr;
        for (auto& bind : cfg.keybinds) {
            if (bind.setting_name == setting_name) {
                kb = &bind;
                break;
            }
        }
        if (!kb) {
            KeyBind new_kb;
            new_kb.setting_name = setting_name;
            new_kb.key = default_key;
            new_kb.mode = BindMode::HOLD;
            cfg.keybinds.push_back(new_kb);
            kb = &cfg.keybinds.back();
        }
        
        float avail = ImGui::GetContentRegionAvail().x;
        std::string btn_label = GetKeybindLabel(menu, kb);
        float text_w = ImGui::CalcTextSize(btn_label.c_str()).x;
        
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - text_w - 5.0f);
        DrawKeybindButton(setting_name);
    }
}

static void RenderSliderFloat(const char* label, const char* id, float* val, float min_v, float max_v,
                              const char* fmt);

static void RenderSliderHeader(const char* label, const char* setting_name, const char* value_text)
{
    MenuClient* menu = MenuClient::get_instance();
    const float scale = ImGui::GetFontSize() / 13.0f;
    const float content_right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    const float value_width = ImGui::CalcTextSize(value_text).x;
    const float spacing = 8.0f * scale;

    const ImVec2 header_start_screen = ImGui::GetCursorScreenPos();
    const float content_right_screen = header_start_screen.x + ImGui::GetContentRegionAvail().x;
    DrawSearchSettingHighlight(
        setting_name, ImVec2(header_start_screen.x - 4.0f, header_start_screen.y - 2.0f),
        ImVec2(content_right_screen,
               header_start_screen.y + ImGui::GetTextLineHeight() + 20.0f * scale +
                   ImGui::GetStyle().ItemSpacing.y + 2.0f));

    ImGui::TextColored(MenuTheme::text_secondary, "%s", label);
    const ImVec2 label_min = ImGui::GetItemRectMin();
    const ImVec2 label_max = ImGui::GetItemRectMax();

    KeyBind* kb = nullptr;
    if (menu && setting_name) {
        OverlayConfig& cfg = menu->get_cfg();
        for (auto& bind : cfg.keybinds) {
            if (bind.setting_name == setting_name) {
                kb = &bind;
                break;
            }
        }
        if (!kb) {
            KeyBind new_kb;
            new_kb.setting_name = setting_name;
            new_kb.is_slider = true;
            cfg.keybinds.push_back(new_kb);
            kb = &cfg.keybinds.back();
        }

        std::string btn_label = GetKeybindLabel(menu, kb);
        const bool has_key_label = !btn_label.empty() && btn_label != "[-]";
        const float keybind_width = has_key_label ? ImGui::CalcTextSize(btn_label.c_str()).x : 0.0f;
        ImGui::SameLine();
        ImGui::SetCursorPosX(content_right - value_width - spacing - keybind_width);
        DrawSettingKeybindLabel(setting_name);
    }

    ImGui::SameLine();
    ImGui::SetCursorPosX(content_right - value_width);
    ImGui::TextColored(MenuTheme::text_primary, "%s", value_text);
    const ImVec2 value_max = ImGui::GetItemRectMax();

    // Right-click anywhere on the setting row opens the keybind popup. For
    // sliders the interactive track sits directly below the header, so extend
    // the capture rect downward to cover it too.
    if (menu && setting_name) {
        float row_bottom = std::max(label_max.y, value_max.y);
        if (kb && kb->is_slider) {
            row_bottom += 20.0f * scale + ImGui::GetStyle().ItemSpacing.y;
        }
        const ImVec2 row_min(label_min.x, label_min.y);
        const ImVec2 row_max(content_right_screen, row_bottom);
        DrawKeybindPopupForRow(setting_name, row_min, row_max);
    }
}

static bool RenderSmoothSliderTrack(const char* id, float* value, float min_value, float max_value,
                                    bool snap_to_integer)
{
    const float scale = ImGui::GetFontSize() / 13.0f;
    const float width = ImGui::GetContentRegionAvail().x;
    const float height = 20.0f * scale;
    const float horizontal_inset = 8.0f * scale;
    const ImGuiID slider_id = ImGui::GetID(id);
    const float previous_value = *value;

    ImGui::InvisibleButton(id, ImVec2(width, height));
    if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left) && max_value > min_value) {
        const ImVec2 item_min = ImGui::GetItemRectMin();
        const float track_start = item_min.x + horizontal_inset;
        const float track_width = std::max(1.0f, width - (horizontal_inset * 2.0f));
        const float normalized =
            std::clamp((ImGui::GetIO().MousePos.x - track_start) / track_width, 0.0f, 1.0f);
        float next_value = min_value + normalized * (max_value - min_value);
        if (snap_to_integer) next_value = std::round(next_value);
        *value = std::clamp(next_value, min_value, max_value);
        ImGui::MarkItemEdited(slider_id);
    }

    const float animated_value =
        MenuAnimation::get().animate(slider_id ^ 0x51ed270bU, std::clamp(*value, min_value, max_value), 24.0f);
    const float fraction = max_value > min_value
                               ? std::clamp((animated_value - min_value) / (max_value - min_value), 0.0f, 1.0f)
                               : 0.0f;

    const ImVec2 item_min = ImGui::GetItemRectMin();
    const ImVec2 item_max = ImGui::GetItemRectMax();
    const float track_y = std::round((item_min.y + item_max.y) * 0.5f);
    const float track_start = std::round(item_min.x + horizontal_inset);
    const float track_end = std::round(item_max.x - horizontal_inset);
    const float grab_x = std::round(track_start + fraction * (track_end - track_start));
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddLine(ImVec2(track_start, track_y), ImVec2(track_end, track_y),
                       ImGui::GetColorU32(MenuTheme::bg_input), 4.0f * scale);
    draw_list->AddLine(ImVec2(track_start, track_y), ImVec2(grab_x, track_y),
                       ImGui::GetColorU32(MenuTheme::accent_primary), 4.0f * scale);
    draw_list->AddCircleFilled(ImVec2(grab_x, track_y), 6.0f * scale,
                               ImGui::GetColorU32(MenuTheme::text_primary), 20);

    return previous_value != *value;
}

// Standalone-style slider: label, keybind, and value on the header; animated track below.
static void RenderSliderFloat(const char* label, const char* id, float* val, float min_v, float max_v,
                              const char* fmt)
{
    char value_text[64];
    std::snprintf(value_text, sizeof(value_text), fmt ? fmt : "%.1f", *val);

    MenuClient* menu = MenuClient::get_instance();
    const char* setting_name = menu ? get_setting_name_by_ptr(menu->get_cfg(), val) : nullptr;
    RenderSliderHeader(label, setting_name, value_text);
    RenderSmoothSliderTrack(id, val, min_v, max_v, false);
}

static void RenderSliderInt(const char* label, const char* id, int* val, int min_v, int max_v, const char* fmt)
{
    char value_text[64];
    std::snprintf(value_text, sizeof(value_text), fmt ? fmt : "%d", *val);

    MenuClient* menu = MenuClient::get_instance();
    const char* setting_name = menu ? get_setting_name_by_ptr(menu->get_cfg(), val) : nullptr;
    RenderSliderHeader(label, setting_name, value_text);

    float float_value = static_cast<float>(*val);
    if (RenderSmoothSliderTrack(id, &float_value, static_cast<float>(min_v), static_cast<float>(max_v), true)) {
        *val = static_cast<int>(std::round(float_value));
    }
}

static bool BeginPaddedCombo(const char* id, const char* preview_value,
                             ImGuiComboFlags flags = ImGuiComboFlags_None);
static void EndPaddedCombo();
static bool RenderPaddedCombo(const char* id, int* current_item, const char* const items[], int items_count);

// Stacked combobox: label on top, full-width combo below
static void RenderCombo(const char* label, const char* id, int* current_item, const char* const items[], int items_count)
{
    MenuClient* menu = MenuClient::get_instance();
    const char* setting_name = menu ? get_setting_name_by_ptr(menu->get_cfg(), current_item) : nullptr;
    const ImVec2 row_min = ImGui::GetCursorScreenPos();
    DrawSearchSettingHighlight(
        setting_name, ImVec2(row_min.x - 4.0f, row_min.y - 2.0f),
        ImVec2(row_min.x + ImGui::GetContentRegionAvail().x,
               row_min.y + ImGui::GetTextLineHeight() + ImGui::GetFrameHeight() +
                   ImGui::GetStyle().ItemSpacing.y + 2.0f));
    ImGui::Text("%s", label);
    ImGui::SetNextItemWidth(-1.0f);
    RenderPaddedCombo(id, current_item, items, items_count);
}

static bool BeginPaddedCombo(const char* id, const char* preview_value, ImGuiComboFlags flags)
{
    ImGuiContext& context = *ImGui::GetCurrentContext();
    ImGuiWindow* parent_window = ImGui::GetCurrentWindow();
    const ImGuiID combo_id = ImGui::GetID(id);
    const ImVec2 combo_min = ImGui::GetCursorScreenPos();
    const ImRect combo_rect(
        combo_min,
        ImVec2(combo_min.x + ImGui::CalcItemWidth(), combo_min.y + ImGui::GetFrameHeight()));
    const ImGuiID popup_id = ImHashStr("##ComboPopup", 0, combo_id);
    const bool popup_was_open = ImGui::IsPopupOpen(popup_id, ImGuiPopupFlags_None);
    const float raw_progress =
        MenuAnimation::get().animate_from(combo_id ^ 0x6c8e9cf5U, popup_was_open ? 1.0f : 0.0f, 0.0f, 24.0f);
    const float progress = smoothstep(raw_progress);

    const float popup_padding_y = 2.0f * (ImGui::GetFontSize() / 13.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(ImGui::GetStyle().WindowPadding.x, popup_padding_y));

    int popup_items = 8;
    if (flags & ImGuiComboFlags_HeightSmall)
        popup_items = 4;
    else if (flags & ImGuiComboFlags_HeightLarge)
        popup_items = 20;
    const float full_popup_height =
        (ImGui::GetFontSize() + ImGui::GetStyle().ItemSpacing.y) * popup_items +
        popup_padding_y * 2.0f;
    const float animated_popup_height =
        std::max(ImGui::GetFrameHeight(), full_popup_height * std::max(progress, 0.05f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 1.0f), ImVec2(100000.0f, animated_popup_height));
    ImGui::SetNextWindowBgAlpha(progress);

    const bool popup_open = ImGui::BeginCombo(id, preview_value, flags);

    // Replace ImGui's static down arrow with an eased chevron that rotates up
    // as the popup opens and continues rotating back after it closes.
    const float arrow_size = ImGui::GetFrameHeight();
    const bool hovered = (context.LastItemData.StatusFlags & ImGuiItemStatusFlags_HoveredRect) != 0;
    const ImU32 arrow_bg =
        ImGui::GetColorU32((popup_open || hovered) ? ImGuiCol_ButtonHovered : ImGuiCol_Button);
    const ImVec2 arrow_min(std::round(combo_rect.Max.x - arrow_size + 1.0f),
                           std::round(combo_rect.Min.y + 1.0f));
    const ImVec2 arrow_max(std::round(combo_rect.Max.x - 1.0f),
                           std::round(combo_rect.Max.y - 1.0f));
    parent_window->DrawList->AddRectFilled(
        arrow_min, arrow_max, arrow_bg, std::max(0.0f, ImGui::GetStyle().FrameRounding - 1.0f),
        ImDrawFlags_RoundCornersRight);

    const float scale = ImGui::GetFontSize() / 13.0f;
    const float half_width = 3.5f * scale;
    const float half_height = 2.5f * scale;
    const float direction = 1.0f - progress * 2.0f;
    const ImVec2 center(std::round(combo_rect.Max.x - arrow_size * 0.5f),
                        std::round((combo_rect.Min.y + combo_rect.Max.y) * 0.5f));
    // Derive both arms from one signed offset: rounding each endpoint
    // independently made one arm 1px longer than the other (lopsided chevron).
    const ImVec2 left(center.x - half_width, center.y - half_height * direction);
    const ImVec2 middle(center.x, center.y + half_height * direction);
    const ImVec2 right(center.x + half_width, center.y - half_height * direction);
    // Match the section-header chevron: a thin anti-aliased stroke in the muted
    // text colour that brightens while the combo is hovered or open. AddPolyline
    // does not anti-alias, so build the path and let PathStroke stroke it.
    const ImVec4 chevron_color = (popup_open || hovered) ? MenuTheme::text_primary : MenuTheme::text_secondary;
    parent_window->DrawList->PathLineTo(left);
    parent_window->DrawList->PathLineTo(middle);
    parent_window->DrawList->PathLineTo(right);
    parent_window->DrawList->PathStroke(ImGui::GetColorU32(chevron_color), ImDrawFlags_None, 1.5f * scale);

    if (popup_open) {
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha,
                            ImGui::GetStyle().Alpha * std::max(progress, 0.05f));
        return true;
    }

    ImGui::PopStyleVar();
    return false;
}

static void EndPaddedCombo()
{
    ImGui::PopStyleVar();
    ImGui::EndCombo();
    ImGui::PopStyleVar();
}

static bool RenderPaddedCombo(const char* id, int* current_item, const char* const items[], int items_count)
{
    if (!current_item || items_count <= 0) return false;
    *current_item = std::clamp(*current_item, 0, items_count - 1);

    const ImGuiID combo_id = ImGui::GetID(id);
    bool changed = false;
    if (BeginPaddedCombo(id, items[*current_item])) {
        for (int i = 0; i < items_count; ++i) {
            const bool selected = *current_item == i;
            if (ImGui::Selectable(items[i], selected) && !selected) {
                *current_item = i;
                changed = true;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        EndPaddedCombo();
    }
    if (changed) ImGui::MarkItemEdited(combo_id);
    return changed;
}

// Stacked input int: label on top, full-width input below
static void RenderInputInt(const char* label, const char* id, int* val)
{
    ImGui::Text("%s", label);
    MenuClient* menu = MenuClient::get_instance();
    const char* setting_name = menu ? get_setting_name_by_ptr(menu->get_cfg(), val) : nullptr;
    if (setting_name) {
        OverlayConfig& cfg = menu->get_cfg();
        KeyBind* kb = nullptr;
        for (auto& bind : cfg.keybinds) {
            if (bind.setting_name == setting_name) {
                kb = &bind;
                break;
            }
        }
        if (!kb) {
            KeyBind new_kb;
            new_kb.setting_name = setting_name;
            new_kb.is_slider = true;
            cfg.keybinds.push_back(new_kb);
            kb = &cfg.keybinds.back();
        }

        std::string btn_label = GetKeybindLabel(menu, kb);
        float text_w = ImGui::CalcTextSize(btn_label.c_str()).x;

        ImGui::SameLine();
        float avail = ImGui::GetContentRegionAvail().x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - text_w);
        DrawKeybindButton(setting_name);
    }
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputInt(id, val);
}

// Stacked text input: label on top, full-width text field below
static bool RenderInputText(const char* label, const char* id, char* buf, size_t buf_size)
{
    ImGui::Text("%s", label);
    ImGui::SetNextItemWidth(-1.0f);
    return ImGui::InputText(id, buf, buf_size);
}

// ─────────────────────────────────────────────────────────────────────────────
// Color picker helpers
// ─────────────────────────────────────────────────────────────────────────────

static const ImGuiColorEditFlags kPickerFlags = ImGuiColorEditFlags_NoSmallPreview | ImGuiColorEditFlags_AlphaBar |
                                                ImGuiColorEditFlags_AlphaPreview | ImGuiColorEditFlags_NoTooltip |
                                                ImGuiColorEditFlags_DisplayRGB | ImGuiColorEditFlags_NoLabel;

static bool RenderCircularColorButton(const char* id, float* color, float size)
{
    const ImGuiID item_id = ImGui::GetID(id);
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(size, size));
    const bool hovered = ImGui::IsItemHovered();
    // Subtle outline that fades in on hover and out when unhovered.
    const float hover = MenuAnimation::get().animate(item_id, hovered ? 1.0f : 0.0f, 22.0f);

    ImVec2 item_min = ImGui::GetItemRectMin();
    ImVec2 item_max = ImGui::GetItemRectMax();
    const ImVec2 center(std::round((item_min.x + item_max.x) * 0.5f),
                        std::round((item_min.y + item_max.y) * 0.5f));
    const float radius = size * 0.5f;
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddCircleFilled(center, radius, ImGui::GetColorU32(ImVec4(color[0], color[1], color[2], color[3])),
                               24);
    // Resting subtle border brightens toward the accent as the hover fades in.
    // Capped well below full opacity for a subtle maximum.
    const ImVec4 hover_outline(MenuTheme::accent_primary.x, MenuTheme::accent_primary.y,
                               MenuTheme::accent_primary.z, 0.525f);
    const ImVec4 outline_color = LerpToggleColor(MenuTheme::border_subtle, hover_outline, hover);
    draw_list->AddCircle(center, radius, ImGui::GetColorU32(outline_color), 24, 1.5f);

    // M2 (right-click) context menu for copy/paste color directly
    const std::string ctx_id = std::string("##ctx_col_") + id;
    ColorClipboard::RenderColorContextMenu(ctx_id.c_str(), color);

    return pressed;
}

static void FormatHexColor(const float* color, char* buffer, size_t buffer_size)
{
    ColorClipboard::FormatHexColor(color, buffer, buffer_size);
}

static bool ApplyHexColor(const char* text, float* color)
{
    return ColorClipboard::ApplyHexColor(text, color);
}

static void RenderColorPickerPopup(const char* popup_id, float* color)
{
    const ImGuiID id = ImGui::GetID(popup_id);
    const bool popup_open = ImGui::IsPopupOpen(id, ImGuiPopupFlags_None);
    const float raw_progress =
        MenuAnimation::get().animate_from(id ^ 0xb24e13a7U, popup_open ? 1.0f : 0.0f, 0.0f, 22.0f);
    const float progress = smoothstep(raw_progress);

    ImGui::SetNextWindowBgAlpha(progress);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * std::max(progress, 0.05f));
    if (!ImGui::BeginPopup(popup_id)) {
        ImGui::PopStyleVar();
        return;
    }

    ImGui::PushID(popup_id);
    static std::unordered_map<ImGuiID, std::array<char, 9>> hex_buffers;
    const ImGuiID hex_id = ImGui::GetID("hex_value");
    auto& hex_buffer = hex_buffers[hex_id];

    const bool picker_changed = ImGui::ColorPicker4("##picker", color, kPickerFlags);
    if (ImGui::IsWindowAppearing() || picker_changed || hex_buffer[0] == '\0') {
        FormatHexColor(color, hex_buffer.data(), hex_buffer.size());
    }

    ImGui::TextColored(MenuTheme::text_secondary, "Hex (RRGGBBAA)");
    ImGui::SetNextItemWidth(-1.0f);
    const ImGuiInputTextFlags input_flags = ImGuiInputTextFlags_CharsHexadecimal |
                                            ImGuiInputTextFlags_CharsUppercase |
                                            ImGuiInputTextFlags_EnterReturnsTrue;
    const bool submitted =
        ImGui::InputText("##hex_value", hex_buffer.data(), hex_buffer.size(), input_flags);
    MenuTooltip::show_item("Enter a color as hexadecimal RRGGBBAA.");
    if ((submitted || ImGui::IsItemDeactivatedAfterEdit()) &&
        !ApplyHexColor(hex_buffer.data(), color)) {
        FormatHexColor(color, hex_buffer.data(), hex_buffer.size());
    }

    ImGui::Spacing();
    const float avail_w = ImGui::GetContentRegionAvail().x;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float btn_w = std::floor((avail_w - spacing) * 0.5f);

    if (ImGui::Button("Copy Color", ImVec2(btn_w, 0.0f))) {
        ColorClipboard::CopyColor(color);
        FormatHexColor(color, hex_buffer.data(), hex_buffer.size());
    }
    MenuTooltip::show_item("Copies the current color to the clipboard.");
    ImGui::SameLine();
    if (ImGui::Button("Paste Color", ImVec2(btn_w, 0.0f))) {
        if (ColorClipboard::PasteColor(color)) {
            FormatHexColor(color, hex_buffer.data(), hex_buffer.size());
        }
    }
    MenuTooltip::show_item("Pastes a color from the clipboard.");

    ImGui::PopID();
    ImGui::EndPopup();
    ImGui::PopStyleVar();
}

// Label on the left, circular color swatch right-justified.
static void RenderColorButton(const char* label, const char* btn_id, const char* popup_id, float* color)
{
    const float swatch_size = 18.0f * (ImGui::GetFontSize() / 13.0f);
    MenuClient* menu = MenuClient::get_instance();
    const char* setting_name = menu ? get_setting_name_by_ptr(menu->get_cfg(), color) : nullptr;
    const ImVec2 row_min = ImGui::GetCursorScreenPos();
    DrawSearchSettingHighlight(
        setting_name, ImVec2(row_min.x - 4.0f, row_min.y - 2.0f),
        ImVec2(row_min.x + ImGui::GetContentRegionAvail().x,
               row_min.y + std::max(ImGui::GetTextLineHeight(), swatch_size) + 2.0f));
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%s", label);
    const ImVec2 label_min = ImGui::GetItemRectMin();
    const ImVec2 label_max = ImGui::GetItemRectMax();
    ImGui::SameLine();
    const float avail = ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - swatch_size);
    ImVec2 swatch_pos = ImGui::GetCursorScreenPos();
    swatch_pos.y = std::round((label_min.y + label_max.y - swatch_size) * 0.5f);
    ImGui::SetCursorScreenPos(swatch_pos);
    if (RenderCircularColorButton(btn_id, color, swatch_size)) ImGui::OpenPopup(popup_id);
    RenderColorPickerPopup(popup_id, color);
}

// Label on the left, dual color swatches right-justified: Label    [VIS ■] [OCC ■]
static void RenderDualColor(const char* label, const char* vis_btn_id, const char* vis_popup_id, float* vis_color,
                            const char* occ_btn_id, const char* occ_popup_id, float* occ_color)
{
    const float swatch_size = 18.0f * (ImGui::GetFontSize() / 13.0f);
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%s", label);
    const ImVec2 label_min = ImGui::GetItemRectMin();
    const ImVec2 label_max = ImGui::GetItemRectMax();
    ImGui::SameLine();
    const float avail = ImGui::GetContentRegionAvail().x;
    const float content_right = ImGui::GetCursorScreenPos().x + avail;
    const float total_w = swatch_size + spacing + swatch_size;
    ImVec2 swatch_pos(content_right - total_w, ImGui::GetCursorScreenPos().y);
    swatch_pos.y = std::round((label_min.y + label_max.y - swatch_size) * 0.5f);
    ImGui::SetCursorScreenPos(swatch_pos);
    if (RenderCircularColorButton(vis_btn_id, vis_color, swatch_size)) ImGui::OpenPopup(vis_popup_id);
    MenuTooltip::show_item("Visible");
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::SetCursorScreenPos(ImVec2(content_right - swatch_size, swatch_pos.y));
    if (RenderCircularColorButton(occ_btn_id, occ_color, swatch_size)) ImGui::OpenPopup(occ_popup_id);
    MenuTooltip::show_item("Occluded");

    RenderColorPickerPopup(vis_popup_id, vis_color);
    RenderColorPickerPopup(occ_popup_id, occ_color);
}

// ─────────────────────────────────────────────────────────────────────────────
// Style combo helper for chams styles (stacked: label on top, combo below)
// ─────────────────────────────────────────────────────────────────────────────

static const std::vector<std::pair<std::string, std::string>> style_options = {
    {"disabled", "Disabled"},
    {"flat", "Flat"},
    {"glass", "Fresnel Glass"},
    {"metallic", "Metallic"},
    {"solid", "Solid"},
};

static void render_style_combo(const char* label, const char* id, ChamsStyle& current_style)
{
    std::string current_label = "Disabled";
    for (const auto& opt : style_options) {
        if (opt.first == (std::string)current_style) {
            current_label = opt.second;
            break;
        }
    }

    ImGui::Text("%s", label);
    ImGui::SetNextItemWidth(-1.0f);
    if (BeginPaddedCombo(id, current_label.c_str())) {
        for (const auto& opt : style_options) {
            bool is_selected = (opt.first == (std::string)current_style);
            if (ImGui::Selectable(opt.second.c_str(), is_selected)) {
                current_style = ChamsStyle(opt.first);
            }
            if (is_selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        EndPaddedCombo();
    }
    MenuTooltip::show_item("Selects the material style used for this effect.");
}

// ─────────────────────────────────────────────────────────────────────────────
// Individual section render functions (no column logic — pure content)
// ─────────────────────────────────────────────────────────────────────────────

static void RenderRevealControls(const char* title, const char* id, bool& enabled, float& seconds, bool& dynamic,
                                 bool& fade, Color* color = nullptr, float* thickness = nullptr)
{
    ImGui::PushID(id);
    RenderToggleTip(title, "##enabled", &enabled,
                    "Enables this reveal effect.");
    if (enabled) {
        seconds = std::clamp(seconds, 0.1f, 3.0f);
        RenderSliderFloat("Display Time", "##seconds", &seconds, 0.1f, 3.0f, "%.1f s");
        MenuTooltip::show_item("How long the reveal remains visible.");
        RenderToggleTip(std::string(id).find("damage") != std::string::npos ? "Track Position On Damage"
                                                                              : "Track Position After Smoke Hit",
                        "##dynamic", &dynamic,
                        "Keeps the reveal at the target's last known position.");
        RenderToggleTip("Fade Out", "##fade", &fade,
                        "Fades the reveal instead of removing it abruptly.");
        if (color) {
            RenderColorButton("Reveal Color", "##reveal_color", "##reveal_pick", *color);
            MenuTooltip::show_item("Sets the color used for this reveal effect.");
        }
        if (thickness) {
            RenderSliderFloat("Reveal Thickness", "##reveal_thickness", thickness, 0.05f, 6.0f, "%.2f");
            MenuTooltip::show_item("Sets the reveal outline thickness.");
        }
    }
    ImGui::PopID();
}

static void render_section_chams(OverlayConfig& cfg)
{
    // Unified Chams section — visible + occluded in one box
    BeginSection("Chams", "##sec_chams");
    render_style_combo("Visible Material", "##VisStyle", cfg.style_vis);

    if (cfg.style_vis != "disabled") {
        RenderColorButton("Visible Color", "##vis_body_btn", "##vis_body_pick", cfg.color_vis);
        MenuTooltip::show_item("Sets the visible player material color.");
        if (chams_style_uses_secondary_color(cfg.style_vis)) {
            RenderColorButton("Visible Color 2", "##vis_body2_btn", "##vis_body2_pick", cfg.color_vis_sec);
            MenuTooltip::show_item("Sets the secondary visible player material color.");
        }
    }

    ImGui::Spacing();

    render_style_combo("Hidden Material", "##HidStyle", cfg.style_invis);
    if (cfg.style_invis != "disabled") {
        RenderColorButton("Hidden Color", "##invis_body_btn", "##invis_body_pick", cfg.color_invis);
        MenuTooltip::show_item("Sets the hidden player material color.");
        if (chams_style_uses_secondary_color(cfg.style_invis)) {
            RenderColorButton("Hidden Color 2", "##invis_body2_btn", "##invis_body2_pick", cfg.color_invis_sec);
            MenuTooltip::show_item("Sets the secondary hidden player material color.");
        }
    }

    if (cfg.style_vis == "flat" || (cfg.style_invis == "flat")) {
        RenderToggleTip("No Depth", "##FlatNoOverlap", &cfg.flat_chams_no_overlap,
                          "Eliminates internal overlapping details when flat chams are semi-transparent.");
    }
    ImGui::Spacing();
    RenderRevealControls("Reveal After Hit Through Smoke", "chams_smoke", cfg.chams_hit_reveal,
                         cfg.chams_hit_reveal_seconds, cfg.chams_hit_reveal_dynamic, cfg.chams_hit_reveal_fade);
    RenderRevealControls("Reveal On Damage", "chams_damage", cfg.chams_damage_reveal, cfg.chams_damage_reveal_seconds,
                         cfg.chams_damage_reveal_dynamic, cfg.chams_damage_reveal_fade);
    ImGui::Spacing();
    RenderToggleTip("Flash Check", "##chams_flash", &cfg.chams_hide_when_flashed,
                    "Hides chams while the local player is flashed.");
    RenderToggleTip("Hide While Shooting", "##chams_shoot", &cfg.chams_hide_while_shooting,
                    "Hides chams while the local player is shooting.");
    EndSection();
}

static void render_section_outline(OverlayConfig& cfg)
{
    BeginSection("Glow", "##sec_outline");
    ImGui::Spacing();

    static const std::vector<std::pair<std::string, std::string>> outline_mode_options = {{"glow", "Glow"},
                                                                                          {"stencil", "Stencil"}};

    std::string current_mode_label = "Glow";
    for (const auto& opt : outline_mode_options) {
        if (opt.first == cfg.outline_mode) {
            current_mode_label = opt.second;
            break;
        }
        MenuTooltip::show_item("Selects the outline rendering mode.");
    }

    ImGui::Text("Glow Style");
    ImGui::SetNextItemWidth(-1.0f);
    if (BeginPaddedCombo("##OutlineType", current_mode_label.c_str())) {
        for (const auto& opt : outline_mode_options) {
            bool is_selected = (opt.first == cfg.outline_mode);
            if (ImGui::Selectable(opt.second.c_str(), is_selected)) {
                cfg.outline_mode = opt.first;
            }
            if (is_selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        EndPaddedCombo();
    }

    bool show_glow_controls = cfg.outline_mode == "glow";
    bool show_stencil_controls = cfg.outline_mode == "stencil";

    if (show_glow_controls || show_stencil_controls) {
        ImGui::Spacing();

        if (show_glow_controls) {
            RenderSliderFloat("Glow Thickness", "##GlowThickness", &cfg.glow_thickness, 0.0f, 6.0f, "%.2f");
            MenuTooltip::show_item("Sets the thickness of the glow around players.");
        } else {
            RenderSliderFloat("Outline Thickness", "##GlowThickness", &cfg.glow_thickness, 0.0f, 6.0f, "%.2f");
            MenuTooltip::show_item("Sets the thickness of the stencil outline.");
        }

        if (cfg.glow_health_based) {
            RenderColorButton("Health Start", "##HealthStart_btn", "##HealthStart_pick", cfg.glow_health_start);
            MenuTooltip::show_item("Sets the glow color at full health.");
            RenderColorButton("Health End", "##HealthEnd_btn", "##HealthEnd_pick", cfg.glow_health_end);
            MenuTooltip::show_item("Sets the glow color at low health.");
        } else {
            if (show_glow_controls) {
                RenderColorButton("Glow Color", "##GlowCol_btn", "##GlowCol_pick", cfg.glow_color);
                MenuTooltip::show_item("Sets the glow color.");
            } else {
                RenderColorButton("Outline Color", "##GlowCol_btn", "##GlowCol_pick", cfg.glow_color);
                MenuTooltip::show_item("Sets the stencil outline color.");
            }
        }

        RenderToggleTip("HP-Based Color", "##HealthBased", &cfg.glow_health_based,
                        "Uses a health-based color gradient instead of one fixed color.");

        if (show_glow_controls) {
            RenderSliderFloat("Glow Strength", "##GlowIntensity", &cfg.glow_intensity, 0.0f, 5.0f, "%.2f");
            MenuTooltip::show_item("Sets the intensity of the glow.");

            ImGui::Spacing();
            RenderToggleTip("Pulse", "##GlowPulse", &cfg.glow_pulse,
                            "Animates the glow intensity over time.");
            if (cfg.glow_pulse) {
                RenderSliderFloat("Pulse Speed", "##GlowPulseSpeed", &cfg.glow_pulse_speed, 0.1f, 10.0f, "%.2f");
                MenuTooltip::show_item("Sets the speed of the glow pulse.");
            }
        }
    }
    ImGui::Spacing();
    RenderRevealControls("Reveal After Hit Through Smoke", "glow_smoke", cfg.glow_hit_reveal, cfg.glow_hit_reveal_seconds,
                         cfg.glow_hit_reveal_dynamic, cfg.glow_hit_reveal_fade, &cfg.glow_hit_reveal_color,
                         &cfg.glow_hit_reveal_thickness);
    RenderRevealControls("Reveal On Damage", "glow_damage", cfg.glow_damage_reveal, cfg.glow_damage_reveal_seconds,
                         cfg.glow_damage_reveal_dynamic, cfg.glow_damage_reveal_fade, &cfg.glow_damage_reveal_color,
                         &cfg.glow_damage_reveal_thickness);
    ImGui::Spacing();
    RenderToggleTip("Flash Check", "##glow_flash", &cfg.glow_hide_when_flashed,
                    "Hides glow while the local player is flashed.");
    RenderToggleTip("Hide While Shooting", "##glow_shoot", &cfg.glow_hide_while_shooting,
                    "Hides glow while the local player is shooting.");
    EndSection();
}

static const std::vector<std::pair<std::string, std::string>> kill_fx_mode_options = {
    {"disabled", "Disabled"},
    {"chams", "Chams Statue"},
    {"particles", "3D Particles"},
    {"pulse", "Screen Pulse Vignette"},
    {"lightning", "Lightning Strike"},
    {"shockwave", "Shockwave Ring"}
};

static void render_kill_fx_mode_combo(const char* label, const char* id, KillFxMode& current_mode)
{
    std::string current_label = "Disabled";
    for (const auto& opt : kill_fx_mode_options) {
        if (opt.first == (std::string)current_mode) {
            current_label = opt.second;
            break;
        }
    }

    ImGui::Text("%s", label);
    ImGui::SetNextItemWidth(-1.0f);
    if (BeginPaddedCombo(id, current_label.c_str())) {
        for (const auto& opt : kill_fx_mode_options) {
            bool is_selected = (opt.first == (std::string)current_mode);
            if (ImGui::Selectable(opt.second.c_str(), is_selected)) {
                current_mode = KillFxMode(opt.first);
            }
            if (is_selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        EndPaddedCombo();
    }
    MenuTooltip::show_item("Selects the visual effect displayed after a kill.");
}

static void RenderSettingHint(const char* text);

static void render_esp_distance_preview(const OverlayConfig& cfg)
{
    const float scale = ImGui::GetFontSize() / 13.0f;
    const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float height = 14.0f * scale;
    const float label_height = ImGui::GetTextLineHeight();
    const float spacing = ImGui::GetStyle().ItemSpacing.y;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 bar_min(std::round(origin.x), std::round(origin.y));
    const ImVec2 bar_max(std::round(origin.x + width), std::round(origin.y + height));
    ImGui::Dummy(ImVec2(width, height + spacing + label_height));

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(bar_min, bar_max, ImGui::GetColorU32(MenuTheme::bg_input), 4.0f * scale);

    constexpr int segment_count = 48;
    const float cutoff = std::max(cfg.esp_max_distance, 0.001f);
    const float fade_start = std::clamp(
        cfg.esp_distance_fade_start > 0.0f ? cfg.esp_distance_fade_start : cutoff * 0.8f,
        0.0f,
        cutoff);
    for (int i = 0; i < segment_count; ++i) {
        const float left = static_cast<float>(i) / static_cast<float>(segment_count);
        const float right = static_cast<float>(i + 1) / static_cast<float>(segment_count);
        const float distance_meters = cutoff * (left + right) * 0.5f;
        ImVec4 color = MenuTheme::accent_primary;
        color.w = esp_distance_alpha(distance_meters, cfg);
        const ImVec2 segment_min(std::round(bar_min.x + width * left), bar_min.y);
        const ImVec2 segment_max(std::round(bar_min.x + width * right), bar_max.y);
        draw_list->AddRectFilled(segment_min, segment_max, ImGui::GetColorU32(color));
    }

    const float fade_fraction = fade_start / cutoff;
    const float fade_x = std::round(bar_min.x + width * fade_fraction);
    const float cutoff_x = std::round(bar_max.x - 1.0f * scale);
    draw_list->AddLine(ImVec2(fade_x, bar_min.y - 2.0f * scale),
                       ImVec2(fade_x, bar_max.y + 2.0f * scale),
                       ImGui::GetColorU32(MenuTheme::text_primary), 1.0f * scale);
    draw_list->AddLine(ImVec2(cutoff_x, bar_min.y - 2.0f * scale),
                       ImVec2(cutoff_x, bar_max.y + 2.0f * scale),
                       ImGui::GetColorU32(MenuTheme::text_muted), 1.0f * scale);

    char fade_label[32];
    char cutoff_label[32];
    std::snprintf(fade_label, sizeof(fade_label), "Fade %.0f m", fade_start);
    std::snprintf(cutoff_label, sizeof(cutoff_label), "Cutoff %.0f m", cutoff);
    const float label_y = std::round(bar_max.y + spacing);
    draw_list->AddText(ImVec2(std::round(bar_min.x), label_y),
                       ImGui::GetColorU32(MenuTheme::text_muted), fade_label);
    const float cutoff_width = ImGui::CalcTextSize(cutoff_label).x;
    draw_list->AddText(ImVec2(std::round(bar_max.x - cutoff_width), label_y),
                       ImGui::GetColorU32(MenuTheme::text_muted), cutoff_label);
}

static void render_esp_distance_controls(OverlayConfig& cfg)
{
    constexpr float default_distance_meters = 1000.0f / distance::UNITS_PER_METER;
    constexpr float max_distance_meters = 150.0f;
    // This card only appears while the ESP "Limit Distance" toggle is on, so a
    // real limit always applies; the mode only chooses how ESP ends.
    const char* distance_modes[] = {"Hard cutoff", "Fade out"};
    int mode = cfg.esp_distance_fade_enabled ? 1 : 0;
    const int previous_mode = mode;

    // Legacy configs may still carry esp_max_distance == 0 (the old "Unlimited").
    if (cfg.esp_max_distance <= 0.0f) cfg.esp_max_distance = default_distance_meters;

    RenderCombo("Distance Mode", "##esp_distance_mode", &mode, distance_modes, 2);
    MenuTooltip::show_item("Selects whether player ESP is cut off or faded by distance.");
    RenderKeybindConfig("Show All ESP", "esp_distance_override", GLFW_KEY_SLASH);
    MenuTooltip::show_item("Hold this key to temporarily ignore the distance limit. Right-click the key to switch between Hold and Toggle modes.");
    if (cfg.esp_distance_override) {
        RenderSettingHint("Distance override active, all player ESP is visible.");
    }
    if (mode != previous_mode) {
        cfg.esp_distance_fade_enabled = mode == 1;
        if (mode == 1 && cfg.esp_distance_fade_start <= 0.0f) {
            cfg.esp_distance_fade_start = cfg.esp_max_distance * 0.8f;
        }
    }

    cfg.esp_max_distance = std::clamp(cfg.esp_max_distance, 1.0f, max_distance_meters);
    RenderSliderFloat("Disappear At", "##esp_max_distance", &cfg.esp_max_distance, 1.0f,
                      max_distance_meters, "%.0f m");
    MenuTooltip::show_item("Sets the distance where player ESP disappears.");

    char cutoff_hint[96];
    std::snprintf(cutoff_hint, sizeof(cutoff_hint), "%.0f m is about %.0f Source units.",
                  cfg.esp_max_distance, cfg.esp_max_distance * distance::UNITS_PER_METER);
    RenderSettingHint(cutoff_hint);
    RenderSettingHint("Example, straight-line map distance:");
    RenderSettingHint("Mirage: Top Mid to CT Mid, about 1,695 units (43 m).");

    if (mode != 1) return;

    if (cfg.esp_distance_fade_start <= 0.0f) cfg.esp_distance_fade_start = cfg.esp_max_distance * 0.8f;
    cfg.esp_distance_fade_start = std::clamp(cfg.esp_distance_fade_start, 0.0f, cfg.esp_max_distance);
    RenderSliderFloat("Fade Begins At", "##esp_distance_fade_start", &cfg.esp_distance_fade_start, 1.0f,
                      cfg.esp_max_distance, "%.0f m");
    MenuTooltip::show_item("Sets the distance where ESP opacity starts to decrease.");
    RenderSliderFloat("Opacity At Cutoff", "##esp_distance_fade_min_alpha", &cfg.esp_distance_fade_min_alpha,
                      0.0f, 1.0f, "%.2f");
    MenuTooltip::show_item("Sets the minimum opacity at the distance cutoff.");
    RenderSettingHint("Full opacity before the fade, then a smooth curve down to the cutoff opacity. 0.00 hides it; 1.00 keeps it fully visible.");
    render_esp_distance_preview(cfg);
}

// ── ESP sub-feature cards ──────────────────────────────────────────────────
// The ESP card only carries enable toggles. Each feature's options live in
// their own card, spawned when the matching toggle is on.

static void render_section_esp_distance(OverlayConfig& cfg)
{
    BeginSection("Distance", "##sec_esp_distance");
    render_esp_distance_controls(cfg);
    EndSection();
}

static void render_section_head_circle(OverlayConfig& cfg)
{
    BeginSection("Head Circle", "##sec_head_circle");
    RenderToggleTip("Filled", "##esp_head_circle_filled", &cfg.esp_head_circle_filled,
                    "Fills the head circle instead of drawing only its outline.");
    RenderToggleTip("Health Color", "##esp_head_circle_health_color",
                    &cfg.esp_head_circle_health_color,
                    "Colors the head circle from the player's health.");
    RenderToggleTip("Real Size", "##esp_head_circle_world_size",
                    &cfg.esp_head_circle_world_size,
                    "Scales the head circle from the player's world-space head size.");
    RenderToggleTip("Outline", "##esp_head_circle_outline", &cfg.esp_head_circle_outline,
                    "Draws an outline around the head circle.");
    RenderSliderFloat("Opacity", "##esp_head_circle_opacity", &cfg.esp_head_circle_opacity, 0.1f, 1.0f, "%.2f");
    MenuTooltip::show_item("Sets the opacity of the head circle.");
    RenderToggleTip("Wall Aware", "##esp_head_circle_wall_aware",
                    &cfg.esp_head_circle_wall_aware,
                    "Keeps a short-lived head marker when the player moves behind a wall.");
    if (cfg.esp_head_circle_wall_aware) {
        RenderSliderFloat("Last Seen Opacity", "##esp_head_circle_occluded_opacity",
                          &cfg.esp_head_circle_occluded_opacity, 0.05f, 1.0f, "%.2f");
        MenuTooltip::show_item("Sets the opacity of the last-seen head marker.");
        RenderSliderFloat("Last Seen Duration", "##esp_head_circle_ghost_seconds",
                          &cfg.esp_head_circle_ghost_seconds, 0.5f, 10.0f, "%.1f s");
        MenuTooltip::show_item("Sets how long the last-seen head marker remains.");
        RenderToggleTip("Peek Flash", "##esp_head_circle_peek_flash", &cfg.esp_head_circle_peek_flash,
                        "Flashes the head marker when a hidden player becomes visible.");
    }
    EndSection();
}

static void render_section_weapon_icons(OverlayConfig& cfg)
{
    BeginSection("Weapon Icons", "##sec_weapon_icons");
    RenderSliderFloat("Opacity", "##weapon_opacity", &cfg.weapon_icon_opacity, 0.0f, 1.0f, "%.2f");
    MenuTooltip::show_item("Sets the opacity of weapon icons.");
    RenderColorButton("Background", "##weapon_bg", "##weapon_bg_pick", cfg.weapon_icon_background);
    MenuTooltip::show_item("Sets the background color behind weapon icons.");
    EndSection();
}

static void render_section_offscreen_arrows(OverlayConfig& cfg)
{
    BeginSection("Off-screen Arrows", "##sec_offscreen_arrows");
    RenderColorButton("Arrow Color", "##osa_col_btn", "##osa_col_pick", cfg.esp_offscreen_arrows_color);
    MenuTooltip::show_item("Sets the default off-screen arrow color.");
    RenderColorButton("Exposed Color (Enemy Sees You)", "##osa_exp_col_btn", "##osa_exp_col_pick", cfg.esp_offscreen_arrows_exposed_color);
    MenuTooltip::show_item("Sets the arrow color when the enemy can see you.");
    RenderSliderFloat("Size", "##osa_size", &cfg.esp_offscreen_arrows_size, 6.0f, 32.0f, "%.0f px");
    MenuTooltip::show_item("Sets the off-screen arrow size.");
    RenderSliderFloat("Distance From Crosshair", "##osa_radius", &cfg.esp_offscreen_arrows_radius, 20.0f, 1000.0f, "%.0f px");
    MenuTooltip::show_item("Sets how far arrows are drawn from the crosshair.");
    RenderSliderFloat("Max Distance (0 = Unlimited)", "##osa_max_dist", &cfg.esp_offscreen_arrows_max_dist, 0.0f, 150.0f, "%.0f m");
    MenuTooltip::show_item("Limits off-screen arrows by player distance. Zero disables the limit.");
    RenderToggleTip("Show Distance", "##osa_distance", &cfg.esp_offscreen_arrows_distance,
                    "Shows the distance next to each off-screen arrow.");
    EndSection();
}

static void render_section_esp_reveals(OverlayConfig& cfg)
{
    BeginSection("Reveals", "##sec_esp_reveals");
    RenderToggleTip("Hit Reveal", "##esp_reveal_enabled", &cfg.esp_damage_reveal,
                    "Reveals a player you damage.");
    if (cfg.esp_damage_reveal) {
        RenderToggleTip("Smokes Only", "##esp_reveal_smokes_only", &cfg.esp_hit_reveal,
                        "Only reveals players you damage while they are in smoke.");
        cfg.esp_damage_reveal_seconds = std::clamp(cfg.esp_damage_reveal_seconds, 0.1f, 3.0f);
        RenderSliderFloat("Display Time", "##esp_reveal_seconds", &cfg.esp_damage_reveal_seconds, 0.1f, 3.0f, "%.1f s");
        MenuTooltip::show_item("How long the reveal remains visible.");
        RenderToggleTip("Track Position On Damage", "##esp_reveal_dynamic", &cfg.esp_damage_reveal_dynamic,
                        "Keeps the reveal at the target's last known position.");
        RenderToggleTip("Fade Out", "##esp_reveal_fade", &cfg.esp_damage_reveal_fade,
                        "Fades the reveal instead of removing it abruptly.");
    }
    EndSection();
}

static void render_section_esp_overlay(OverlayConfig& cfg)
{
    BeginSection("ESP", "##sec_esp_main");
    RenderToggleTip("Visible", "##esp_box_hp_vis", &cfg.esp_box_hp_visible,
                    "Shows ESP elements for players in line of sight.");
    RenderToggleTip("Occluded", "##esp_box_hp_occ", &cfg.esp_box_hp_occluded,
                    "Shows ESP elements for players behind obstacles.");
    RenderToggleTip("Box", "##esp_box", &cfg.esp_box,
                    "Shows a box around each player.");
    RenderToggleTip("HP Bar", "##esp_hp_bar", &cfg.esp_health_bar,
                    "Shows each player's health bar.");
    RenderToggleTip("Skeleton", "##esp_skeleton", &cfg.esp_skeleton,
                    "Shows each player's skeleton.");
    RenderToggleTip("Head Circle", "##esp_head_circle", &cfg.esp_head_circle,
                    "Shows a circle around each player's head.");
    RenderToggleTip("Noise ESP", "##esp_step_circles", &cfg.esp_step_circles,
                    "Shows recent movement and action events around players.");
    RenderToggleTip("Names", "##esp_names", &cfg.esp_names,
                    "Shows player names.");
    RenderToggleTip("Armor Bar", "##esp_armor_bar", &cfg.esp_armor_bar,
                    "Shows each player's armor bar.");
    RenderToggleTip("Ammo Bar", "##esp_ammo_bar", &cfg.esp_ammo_bar,
                    "Shows each player's remaining ammo.");
    RenderToggleTip("Flags", "##esp_flags", &cfg.esp_flags,
                    "Shows player status flags.");
    RenderToggleTip("Weapon Icons", "##esp_weapon_icons", &cfg.esp_weapon_icons,
                    "Shows the weapon held by each player as an icon.");
    RenderToggleTip("Hitmarkers", "##hitmarker", &cfg.hitmarker,
                    "Shows a marker when your shot damages a player.");
    RenderToggleTip("Off-screen Arrows", "##osa_enabled", &cfg.esp_offscreen_arrows,
                    "Shows arrows for players outside the screen.");
    RenderToggleTip("Limit Distance", "##esp_distance_limit", &cfg.esp_distance_limit_enabled,
                    "Limits player ESP by distance. When off, ESP stays visible at any range.");
    ImGui::Spacing();
    RenderToggleTip("Flash Check", "##esp_flash", &cfg.esp_hide_when_flashed,
                    "Hides ESP while the local player is flashed.");
    RenderToggleTip("Hide While Shooting", "##esp_shoot", &cfg.esp_hide_while_shooting,
                    "Hides ESP while the local player is shooting.");
    EndSection();
}

static void render_section_crosshair_distance(OverlayConfig& cfg)
{
    BeginSection("Crosshair Distance", "##sec_crosshair_distance");
    RenderColorButton("World Color", "##xhair_dist_col_btn", "##xhair_dist_col_pick", cfg.esp_crosshair_distance_color);
    MenuTooltip::show_item("Sets the distance color for world positions.");
    RenderColorButton("Player Color", "##xhair_dist_player_col_btn", "##xhair_dist_player_col_pick",
                      cfg.esp_crosshair_distance_player_color);
    MenuTooltip::show_item("Sets the distance color for players.");
    RenderSliderFloat("Vertical Offset", "##xhair_dist_offset", &cfg.esp_crosshair_distance_offset, -100.0f, 100.0f, "%.0f px");
    MenuTooltip::show_item("Moves the distance label vertically.");
    RenderColorButton("Pin Color", "##xhair_dist_pin_col_btn", "##xhair_dist_pin_col_pick", cfg.esp_crosshair_distance_pin_color);
    MenuTooltip::show_item("Sets the color of a pinned distance marker.");
    RenderKeybindConfig("Pin Key", "crosshair_distance_pin_key", 1002);
    RenderKeybindConfig("Clear Pin Key", "crosshair_distance_clear_pin_key", -1);
    EndSection();
}

static void render_section_spread_circle(OverlayConfig& cfg)
{
    BeginSection("Spread Circle", "##sec_spread_circle");
    RenderColorButton("Color", "##spread_circle_col_btn", "##spread_circle_col_pick", cfg.esp_spread_circle_color);
    MenuTooltip::show_item("Sets the spread circle color.");
    RenderSliderFloat("Fill Alpha", "##spread_circle_fill_alpha", &cfg.esp_spread_circle_fill_alpha, 0.0f, 1.0f, "%.2f");
    MenuTooltip::show_item("Sets the opacity of the filled spread circle.");
    RenderToggleTip("Draw Outline", "##spread_circle_outline", &cfg.esp_spread_circle_outline,
                    "Draws an outline around the spread circle.");
    if (cfg.esp_spread_circle_outline) {
        RenderSliderFloat("Outline Thickness", "##spread_circle_thick", &cfg.esp_spread_circle_thickness, 0.5f, 5.0f, "%.1f");
        MenuTooltip::show_item("Sets the spread circle outline thickness.");
    }
    RenderToggleTip("Show Base Spread", "##spread_circle_show_base", &cfg.esp_spread_circle_show_base,
                    "Includes the weapon's base spread in the circle.");
    RenderSliderFloat("Animation Speed", "##spread_circle_anim_speed", &cfg.esp_spread_circle_animation_speed, 1.0f, 30.0f, "%.1f");
    MenuTooltip::show_item("Sets how quickly the spread circle responds to changes.");
    EndSection();
}


static void render_section_damage_fx(OverlayConfig& cfg)
{
    BeginSection("DamageFX", "##sec_damage_fx");

    // ── Hit FX ──────────────────────────────────────────────────────────────
    render_kill_fx_mode_combo("Hit FX", "##hit_effect_mode", cfg.hit_effect_mode);
    if (cfg.hit_effect_mode != KillFxModeEnum::DISABLED) {
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        ImGui::Dummy(ImVec2(0.0f, 4.0f));

        if (cfg.hit_effect_mode == KillFxModeEnum::CHAMS) {
            render_style_combo("Cham Shader", "##HitChamsStyle", cfg.hit_chams_style);
            if (cfg.hit_chams_style != "disabled") {
                RenderColorButton("Color", "##hit_chams_col_btn", "##hit_chams_col_pick", cfg.hit_chams_color);
                if (chams_style_uses_secondary_color(cfg.hit_chams_style)) {
                    RenderColorButton("Color 2", "##hit_chams_col2_btn", "##hit_chams_col2_pick",
                                      cfg.hit_chams_color_sec);
                }
            }
            RenderSliderFloat("Fade Time", "##hit_chams_fade", &cfg.hit_chams_fade_time, 0.1f, 3.0f, "%.1fs");
        }
        else if (cfg.hit_effect_mode == KillFxModeEnum::PARTICLES) {
            RenderColorButton("Particle Color", "##hit_effect_col_btn", "##hit_effect_col_pick", cfg.hit_effect_color);
            MenuTooltip::show_item("Sets the hit particle color.");
            RenderSliderInt("Particle Count", "##hit_effect_count", &cfg.hit_effect_count, 1, 60, "%d");
            MenuTooltip::show_item("Sets the number of particles per hit.");
            RenderSliderFloat("Size", "##hit_effect_size", &cfg.hit_effect_size, 0.5f, 10.0f, "%.1f px");
            MenuTooltip::show_item("Sets the hit particle size.");
            RenderSliderFloat("Speed", "##hit_effect_speed", &cfg.hit_effect_speed, 0.1f, 5.0f, "%.1f");
            MenuTooltip::show_item("Sets the hit particle speed.");
            RenderSliderFloat("Lifetime", "##hit_effect_life", &cfg.hit_effect_lifetime, 0.1f, 2.0f, "%.2fs");
            MenuTooltip::show_item("Sets how long hit particles remain visible.");
            RenderSliderFloat("Gravity", "##hit_effect_grav", &cfg.hit_effect_gravity, -1000.0f, 0.0f, "%.0f");
            MenuTooltip::show_item("Sets the downward hit particle acceleration.");
        }
        else if (cfg.hit_effect_mode == KillFxModeEnum::PULSE) {
            RenderColorButton("Pulse Color", "##hit_pulse_col_btn", "##hit_pulse_col_pick", cfg.hit_pulse_color);
            RenderSliderFloat("Duration", "##hit_pulse_dur", &cfg.hit_pulse_duration, 0.1f, 2.0f, "%.1fs");
            RenderSliderFloat("Intensity", "##hit_pulse_int", &cfg.hit_pulse_intensity, 0.1f, 3.0f, "%.1f");
        }
        else if (cfg.hit_effect_mode == KillFxModeEnum::LIGHTNING) {
            RenderColorButton("Lightning Color", "##hit_light_col_btn", "##hit_light_col_pick", cfg.hit_lightning_color);
            RenderSliderFloat("Duration", "##hit_light_dur", &cfg.hit_lightning_duration, 0.1f, 2.0f, "%.1fs");
            RenderSliderFloat("Width/Thickness", "##hit_light_width", &cfg.hit_lightning_width, 1.0f, 10.0f, "%.1f px");
            RenderSliderInt("Strike Count", "##hit_light_count", &cfg.hit_lightning_count, 1, 10, "%d");
        }
        else if (cfg.hit_effect_mode == KillFxModeEnum::SHOCKWAVE) {
            RenderColorButton("Shockwave Color", "##hit_shock_col_btn", "##hit_shock_col_pick", cfg.hit_shockwave_color);
            RenderSliderFloat("Duration", "##hit_shock_dur", &cfg.hit_shockwave_duration, 0.1f, 2.0f, "%.1fs");
            RenderSliderFloat("Max Radius", "##hit_shock_radius", &cfg.hit_shockwave_max_radius, 10.0f, 300.0f, "%.0f");
            RenderSliderFloat("Width/Thickness", "##hit_shock_width", &cfg.hit_shockwave_width, 1.0f, 10.0f, "%.1f px");
        }

        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        RenderToggle("Only My Hits", "##hit_fx_local_only", &cfg.hit_fx_local_only);
    }

    ImGui::Dummy(ImVec2(0.0f, 6.0f));

    // ── Kill FX ─────────────────────────────────────────────────────────────
    render_kill_fx_mode_combo("Kill FX", "##kill_effect", cfg.kill_effect);
    if (cfg.kill_effect != KillFxModeEnum::DISABLED) {
        RenderToggleTip("Show HEADSHOT Text", "##headshot_text", &cfg.kill_headshot_text,
                        "Shows HEADSHOT text for headshot kills.");
        RenderToggleTip("Show KILL Text", "##kill_show_text", &cfg.kill_show_text,
                        "Shows KILL text when a kill effect is triggered.");
    }

    if (cfg.kill_effect != KillFxModeEnum::DISABLED) {
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        ImGui::Dummy(ImVec2(0.0f, 4.0f));

        if (cfg.kill_effect == KillFxModeEnum::CHAMS) {
            render_style_combo("Cham Shader", "##KillChamsStyle", cfg.kill_chams_style);
            if (cfg.kill_chams_style != "disabled") {
                RenderColorButton("Color", "##kill_chams_col_btn", "##kill_chams_col_pick", cfg.kill_chams_color);
                MenuTooltip::show_item("Sets the kill chams color.");
                if (chams_style_uses_secondary_color(cfg.kill_chams_style)) {
                    RenderColorButton("Color 2", "##kill_chams_col2_btn", "##kill_chams_col2_pick",
                                      cfg.kill_chams_color_sec);
                    MenuTooltip::show_item("Sets the secondary kill chams color.");
                }
            }
            RenderSliderFloat("Fade Time", "##kill_chams_fade", &cfg.kill_chams_fade_time, 0.1f, 5.0f, "%.1fs");
            MenuTooltip::show_item("Sets how long the kill chams remain visible.");
            RenderToggleTip("Heaven Mode (Float)", "##kill_chams_heaven", &cfg.kill_chams_heaven,
                            "Makes the kill chams float upward.");
            if (cfg.kill_chams_heaven) {
                RenderSliderFloat("Float Speed", "##kill_chams_heaven_speed", &cfg.kill_chams_heaven_speed, 10.0f, 200.0f, "%.0f");
                MenuTooltip::show_item("Sets the upward speed of the floating kill chams.");
            }
        }
        else if (cfg.kill_effect == KillFxModeEnum::PARTICLES) {
            RenderColorButton("Particle Color", "##kill_part_col_btn", "##kill_part_col_pick", cfg.kill_particles_color);
            MenuTooltip::show_item("Sets the kill particle color.");
            RenderSliderInt("Particle Count", "##kill_part_count", &cfg.kill_particles_count, 10, 100, "%d");
            MenuTooltip::show_item("Sets the number of particles emitted per kill.");
            RenderSliderFloat("Velocity/Speed", "##kill_part_speed", &cfg.kill_particles_speed, 0.5f, 5.0f, "%.1f");
            MenuTooltip::show_item("Sets the initial particle velocity.");
            RenderSliderFloat("Lifetime", "##kill_part_life", &cfg.kill_particles_lifetime, 0.2f, 3.0f, "%.1fs");
            MenuTooltip::show_item("Sets how long kill particles remain visible.");
            RenderSliderFloat("Gravity", "##kill_part_grav", &cfg.kill_particles_gravity, -1000.0f, 0.0f, "%.0f");
            MenuTooltip::show_item("Sets the downward particle acceleration.");
            RenderSliderFloat("Size", "##kill_part_size", &cfg.kill_particles_size, 1.0f, 15.0f, "%.1f px");
            MenuTooltip::show_item("Sets the size of kill particles.");
        }
        else if (cfg.kill_effect == KillFxModeEnum::PULSE) {
            RenderColorButton("Pulse Color", "##kill_pulse_col_btn", "##kill_pulse_col_pick", cfg.kill_pulse_color);
            MenuTooltip::show_item("Sets the screen pulse color.");
            RenderSliderFloat("Duration", "##kill_pulse_dur", &cfg.kill_pulse_duration, 0.1f, 2.0f, "%.1fs");
            MenuTooltip::show_item("Sets how long the screen pulse lasts.");
            RenderSliderFloat("Intensity", "##kill_pulse_int", &cfg.kill_pulse_intensity, 0.1f, 3.0f, "%.1f");
            MenuTooltip::show_item("Sets the screen pulse intensity.");
        }
        else if (cfg.kill_effect == KillFxModeEnum::LIGHTNING) {
            RenderColorButton("Lightning Color", "##kill_light_col_btn", "##kill_light_col_pick", cfg.kill_lightning_color);
            MenuTooltip::show_item("Sets the lightning strike color.");
            RenderSliderFloat("Duration", "##kill_light_dur", &cfg.kill_lightning_duration, 0.1f, 2.0f, "%.1fs");
            MenuTooltip::show_item("Sets how long the lightning strike lasts.");
            RenderSliderFloat("Width/Thickness", "##kill_light_width", &cfg.kill_lightning_width, 1.0f, 10.0f, "%.1f px");
            MenuTooltip::show_item("Sets the lightning line thickness.");
            RenderSliderInt("Strike Count", "##kill_light_count", &cfg.kill_lightning_count, 1, 10, "%d");
            MenuTooltip::show_item("Sets the number of lightning strikes.");
        }
        else if (cfg.kill_effect == KillFxModeEnum::SHOCKWAVE) {
            RenderColorButton("Shockwave Color", "##kill_shock_col_btn", "##kill_shock_col_pick", cfg.kill_shockwave_color);
            MenuTooltip::show_item("Sets the shockwave color.");
            RenderSliderFloat("Duration", "##kill_shock_dur", &cfg.kill_shockwave_duration, 0.1f, 2.0f, "%.1fs");
            MenuTooltip::show_item("Sets how long the shockwave lasts.");
            RenderSliderFloat("Max Radius", "##kill_shock_radius", &cfg.kill_shockwave_max_radius, 10.0f, 300.0f, "%.0f");
            MenuTooltip::show_item("Sets the maximum shockwave radius.");
            RenderSliderFloat("Width/Thickness", "##kill_shock_width", &cfg.kill_shockwave_width, 1.0f, 10.0f, "%.1f px");
            MenuTooltip::show_item("Sets the shockwave ring thickness.");
        }
    }
    EndSection();
}

static void render_section_step_circles(OverlayConfig& cfg)
{
    BeginSection("Noise ESP", "##sec_step_circles");
    RenderSliderFloat("Start Radius", "##step_circ_rad", &cfg.esp_step_circles_radius, 8.0f, 80.0f, "%.0f u");
    MenuTooltip::show_item("Sets the initial radius of a noise marker.");
    RenderSliderFloat("Max Radius", "##noise_max_rad", &cfg.esp_noise_max_radius, 20.0f, 200.0f, "%.0f u");
    MenuTooltip::show_item("Sets the maximum radius of a noise marker.");
    RenderSliderFloat("Duration", "##step_circ_age", &cfg.esp_step_circles_max_age, 0.5f, 6.0f, "%.1f s");
    MenuTooltip::show_item("Sets how long noise markers remain visible.");
    RenderToggleTip("Fade Over Age", "##step_circ_fade", &cfg.esp_step_circles_fade,
                    "Fades noise markers as they age.");
    RenderToggleTip("Filled", "##step_circ_fill", &cfg.esp_step_circles_filled,
                    "Fills noise markers instead of drawing only their outlines.");
    RenderToggleTip("Hide While Walking", "##step_circ_hide_walk", &cfg.esp_step_circles_hide_while_walking,
                    "Hides noise markers while the local player is walking.");
    RenderToggleTip("Plant/Defuse Text", "##noise_plant_defuse_text", &cfg.esp_noise_show_plant_defuse_text,
                    "Shows text for plant and defuse events.");

    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    RenderToggleTip("Movement", "##noise_movement", &cfg.esp_noise_movement,
                    "Shows movement noise markers.");
    if (cfg.esp_noise_movement) {
        RenderColorButton("Movement Color", "##noise_move_col_btn", "##noise_move_col_pick", cfg.esp_noise_movement_color);
        MenuTooltip::show_item("Sets the color of movement noise markers.");
    }
    RenderToggleTip("Shooting", "##noise_shooting", &cfg.esp_noise_shooting,
                    "Shows shooting noise markers.");
    if (cfg.esp_noise_shooting) {
        RenderColorButton("Shooting Color", "##noise_shoot_col_btn", "##noise_shoot_col_pick", cfg.esp_noise_shooting_color);
        MenuTooltip::show_item("Sets the color of shooting noise markers.");
    }
    RenderToggleTip("Reload", "##noise_reload", &cfg.esp_noise_reload,
                    "Shows reload noise markers.");
    if (cfg.esp_noise_reload) {
        RenderColorButton("Reload Color", "##noise_reload_col_btn", "##noise_reload_col_pick", cfg.esp_noise_reload_color);
        MenuTooltip::show_item("Sets the color of reload noise markers.");
    }
    RenderToggleTip("Landing", "##noise_landing", &cfg.esp_noise_landing,
                    "Shows landing noise markers.");
    if (cfg.esp_noise_landing) {
        RenderColorButton("Landing Color", "##noise_land_col_btn", "##noise_land_col_pick", cfg.esp_noise_landing_color);
        MenuTooltip::show_item("Sets the color of landing noise markers.");
    }
    RenderToggleTip("Scope", "##noise_scope", &cfg.esp_noise_scope,
                    "Shows scoping noise markers.");
    if (cfg.esp_noise_scope) {
        RenderColorButton("Scope Color", "##noise_scope_col_btn", "##noise_scope_col_pick", cfg.esp_noise_scope_color);
        MenuTooltip::show_item("Sets the color of scoping noise markers.");
    }
    RenderToggleTip("Plant", "##noise_plant", &cfg.esp_noise_plant,
                    "Shows bomb plant noise markers.");
    if (cfg.esp_noise_plant) {
        RenderColorButton("Plant Color", "##noise_plant_col_btn", "##noise_plant_col_pick", cfg.esp_noise_plant_color);
        MenuTooltip::show_item("Sets the color of plant noise markers.");
    }
    RenderToggleTip("Defuse", "##noise_defuse", &cfg.esp_noise_defuse,
                    "Shows bomb defuse noise markers.");
    if (cfg.esp_noise_defuse) {
        RenderColorButton("Defuse Color", "##noise_defuse_col_btn", "##noise_defuse_col_pick", cfg.esp_noise_defuse_color);
        MenuTooltip::show_item("Sets the color of defuse noise markers.");
    }
    EndSection();
}

static void render_section_skeleton(OverlayConfig& cfg)
{
    BeginSection("Skeleton", "##sec_skeleton");
    RenderToggleTip("Curved", "##skele_round", &cfg.esp_rounded_skeleton,
                    "Uses rounded joints and segments for the skeleton.");
    RenderSliderFloat("Thickness", "##skele_th", &cfg.esp_skeleton_thickness, 0.5f, 5.0f, "%.1f");
    MenuTooltip::show_item("Sets the skeleton line thickness.");
    RenderDualColor("Color", "##skele_vis_btn", "##skele_vis_pick", cfg.esp_skeleton_color_vis, "##skele_occ_btn",
                    "##skele_occ_pick", cfg.esp_skeleton_color_invis);
    MenuTooltip::show_item("Sets the visible and occluded skeleton colors.");
    // Delay only applies when both visible + occluded ESP layers are enabled
    if (cfg.esp_box_hp_visible && cfg.esp_box_hp_occluded) {
        RenderToggleTip("Vis Color Delay", "##skele_vis_delay_en", &cfg.esp_skeleton_vis_delay_enabled,
                        "Delays the change from the occluded to visible skeleton color.");
        if (cfg.esp_skeleton_vis_delay_enabled) {
            RenderSliderFloat("Invis→Vis Delay", "##skele_vis_delay_ms", &cfg.esp_skeleton_vis_delay_ms, 0.0f,
                              1000.0f, "%.0f ms");
            MenuTooltip::show_item("Sets the delay before the visible skeleton color is shown.");
        }
    }
    RenderToggleTip("Glow", "##skele_glow", &cfg.esp_skeleton_glow,
                    "Adds a glow around the skeleton.");
    RenderToggleTip("Glow Pulse", "##skele_glow_puls", &cfg.esp_skeleton_glow_pulse,
                    "Animates the skeleton glow intensity.");
    if (cfg.esp_skeleton_glow_pulse) {
        RenderSliderFloat("Glow Pulse Speed", "##skele_glow_puls_spd", &cfg.esp_skeleton_glow_pulse_speed, 0.1f, 10.0f,
                          "%.1f");
        MenuTooltip::show_item("Sets the speed of the skeleton glow pulse.");
    }
    RenderSliderFloat("Thickness", "##skele_glow_th", &cfg.esp_skeleton_glow_thickness, 0.1f, 10.0f, "%.1f");
    MenuTooltip::show_item("Sets the skeleton glow thickness.");
    RenderSliderFloat("Intensity", "##skele_glow_int", &cfg.esp_skeleton_glow_intensity, 0.1f, 5.0f, "%.1f");
    MenuTooltip::show_item("Sets the skeleton glow intensity.");
    RenderToggleTip("HP-Based Color", "##skele_glow_hb", &cfg.esp_skeleton_glow_health_based,
                    "Uses player health to choose the skeleton glow color.");
    if (cfg.esp_skeleton_glow_health_based) {
        RenderColorButton("Health Start", "##skele_glow_hs_btn", "##skele_glow_hs_pick",
                          cfg.esp_skeleton_glow_health_start);
        MenuTooltip::show_item("Sets the skeleton glow color at full health.");
        RenderColorButton("Health End", "##skele_glow_he_btn", "##skele_glow_he_pick",
                          cfg.esp_skeleton_glow_health_end);
        MenuTooltip::show_item("Sets the skeleton glow color at low health.");
    } else {
        RenderColorButton("Glow Color", "##skele_glow_col_btn", "##skele_glow_col_pick", cfg.esp_skeleton_glow_color);
        MenuTooltip::show_item("Sets the skeleton glow color.");
    }
    EndSection();
}

static void render_section_bounding_box(OverlayConfig& cfg)
{
    BeginSection("Box", "##sec_box");
    RenderSliderFloat("Thickness", "##box_th", &cfg.esp_box_thickness, 0.5f, 5.0f, "%.1f");
    MenuTooltip::show_item("Sets the player box line thickness.");
    RenderColorButton("Color", "##box_col_btn", "##box_col_pick", cfg.esp_box_color);
    MenuTooltip::show_item("Sets the player box color.");
    RenderToggleTip("Outline", "##box_outl", &cfg.esp_box_outline,
                    "Draws an outline around the player box.");

    const char* box_modes[] = {"Dynamic (Bones)", "Dynamic (Feet + Head)", "Static (Distance-Based)"};
    ImGui::Text("Mode");
    ImGui::SetNextItemWidth(-1.0f);
    RenderPaddedCombo("##box_mode", &cfg.esp_box_mode, box_modes, IM_ARRAYSIZE(box_modes));
    MenuTooltip::show_item("Selects how player box dimensions are calculated.");

    if (cfg.esp_box_mode == 2) {
        RenderSliderFloat("Static Width", "##box_stat_w", &cfg.esp_box_static_w, 5000.0f, 80000.0f, "%.0f");
        MenuTooltip::show_item("Sets the width of distance-based boxes.");
        RenderSliderFloat("Static Height", "##box_stat_h", &cfg.esp_box_static_h, 5000.0f, 120000.0f, "%.0f");
        MenuTooltip::show_item("Sets the height of distance-based boxes.");
    }
    EndSection();
}

static void render_section_health_bar(OverlayConfig& cfg)
{
    BeginSection("HP Bar", "##sec_hp");
    RenderSliderFloat("Thickness", "##hp_th", &cfg.esp_health_bar_thickness, 0.5f, 10.0f, "%.1f");
    MenuTooltip::show_item("Sets the health bar thickness.");
    RenderToggleTip("Gradient", "##hp_grad", &cfg.esp_health_bar_gradient,
                    "Uses a gradient for the health bar color.");
    if (cfg.esp_health_bar_gradient) {
        RenderColorButton("Health Start", "##hp_gh_btn", "##hp_gh_pick", cfg.esp_health_bar_gradient_start);
        MenuTooltip::show_item("Sets the health bar color at full health.");
        RenderColorButton("Health End", "##hp_gl_btn", "##hp_gl_pick", cfg.esp_health_bar_gradient_end);
        MenuTooltip::show_item("Sets the health bar color at low health.");
    } else {
        RenderColorButton("Color", "##hp_col_btn", "##hp_col_pick", cfg.esp_health_bar_color);
        MenuTooltip::show_item("Sets the health bar color.");
    }
    RenderToggleTip("Outline", "##hp_outl", &cfg.esp_health_bar_outline,
                    "Draws an outline around the health bar.");
    EndSection();
}

static void render_section_armor_bar(OverlayConfig& cfg)
{
    BeginSection("Armor Bar", "##sec_armor_bar");
    RenderSliderFloat("Thickness", "##armor_th", &cfg.esp_armor_bar_thickness, 0.5f, 10.0f, "%.1f");
    MenuTooltip::show_item("Sets the armor bar thickness.");
    RenderToggleTip("Gradient", "##armor_grad", &cfg.esp_armor_bar_gradient,
                    "Uses a gradient for the armor bar color.");
    if (cfg.esp_armor_bar_gradient) {
        RenderColorButton("Armor Start", "##armor_gs_btn", "##armor_gs_pick", cfg.esp_armor_bar_gradient_start);
        MenuTooltip::show_item("Sets the armor bar color at full armor.");
        RenderColorButton("Armor End", "##armor_ge_btn", "##armor_ge_pick", cfg.esp_armor_bar_gradient_end);
        MenuTooltip::show_item("Sets the armor bar color at low armor.");
    } else {
        RenderColorButton("Color", "##armor_col_btn", "##armor_col_pick", cfg.esp_armor_bar_color);
        MenuTooltip::show_item("Sets the armor bar color.");
    }
    RenderToggleTip("Outline", "##armor_outl", &cfg.esp_armor_bar_outline,
                    "Draws an outline around the armor bar.");
    EndSection();
}

static void render_section_ammo_bar(OverlayConfig& cfg)
{
    BeginSection("Ammo Bar", "##sec_ammo_bar");
    RenderSliderFloat("Thickness", "##ammo_th", &cfg.esp_ammo_bar_thickness, 0.5f, 10.0f, "%.1f");
    MenuTooltip::show_item("Sets the ammo bar thickness.");
    RenderToggleTip("Gradient", "##ammo_grad", &cfg.esp_ammo_bar_gradient,
                    "Uses a gradient for the ammo bar color.");
    if (cfg.esp_ammo_bar_gradient) {
        RenderColorButton("Ammo Start", "##ammo_gs_btn", "##ammo_gs_pick", cfg.esp_ammo_bar_gradient_start);
        MenuTooltip::show_item("Sets the ammo bar color at full ammo.");
        RenderColorButton("Ammo End", "##ammo_ge_btn", "##ammo_ge_pick", cfg.esp_ammo_bar_gradient_end);
        MenuTooltip::show_item("Sets the ammo bar color at low ammo.");
    } else {
        RenderColorButton("Color", "##ammo_col_btn", "##ammo_col_pick", cfg.esp_ammo_bar_color);
        MenuTooltip::show_item("Sets the ammo bar color.");
    }
    RenderToggleTip("Outline", "##ammo_outl", &cfg.esp_ammo_bar_outline,
                    "Draws an outline around the ammo bar.");
    EndSection();
}

static void render_section_flags(OverlayConfig& cfg)
{
    BeginSection("Flags", "##sec_flags");
    RenderToggleTip("Reloading", "##flag_reload", &cfg.esp_flags_reload,
                    "Shows a flag when a player is reloading.");
    RenderToggleTip("Scoped", "##flag_scoped", &cfg.esp_flags_scoped,
                    "Shows a flag when a player is scoped in.");
    RenderToggleTip("Defuser", "##flag_defuser", &cfg.esp_flags_defuser,
                    "Shows a flag when a player carries a defuse kit.");
    RenderToggleTip("Helmet", "##flag_helmet", &cfg.esp_flags_helmet,
                    "Shows a flag when a player has a helmet.");
    RenderToggleTip("Kevlar/Armor", "##flag_kevlar", &cfg.esp_flags_kevlar,
                    "Shows a flag when a player has armor.");
    RenderToggleTip("Distance", "##flag_distance", &cfg.esp_flags_distance,
                    "Shows the distance to each player.");
    RenderToggleTip("HP Numbers", "##flag_hp", &cfg.esp_flags_hp,
                    "Shows player health as a number.");
    RenderToggleTip("Bomb Carrier", "##flag_bomb", &cfg.esp_flags_bomb,
                    "Shows a flag when a player carries the bomb.");
    if (cfg.esp_flags_bomb) {
        RenderColorButton("Bomb Color", "##flag_bomb_col_btn", "##flag_bomb_col_pick", cfg.esp_flags_bomb_color);
        MenuTooltip::show_item("Sets the bomb carrier flag color.");
    }
    RenderToggleTip("Flashed", "##flag_flashed", &cfg.esp_flags_flashed,
                    "Shows a flag when a player is flashed.");
    if (cfg.esp_flags_flashed) {
        RenderColorButton("Flashed Color", "##flag_flashed_col_btn", "##flag_flashed_col_pick", cfg.esp_flags_flashed_color);
        MenuTooltip::show_item("Sets the flashed flag color.");
    }
    RenderColorButton("Flags Color", "##flags_col_btn", "##flags_col_pick", cfg.esp_flags_color);
    MenuTooltip::show_item("Sets the default color for player flags.");
    EndSection();
}

static void render_section_grenade_features(OverlayConfig& cfg)
{
    BeginSection("Grenade Features", "##sec_grenade_features");
    RenderToggleTip("Grenade Helper", "##GHelperEnabled", &cfg.grenade_helper_enabled,
                    "Enables lineup guidance and throw assistance.");
    RenderToggleTip("Grenade Solver", "##SolverEnabled", &cfg.grenade_solver_show_trajectory,
                    "Enables grenade trajectory solving and landing zones.");
    RenderToggleTip("Grenade Creator", "##GCreatorEnabled", &cfg.grenade_creator_enabled,
                    "Enables the editor for creating local grenade lineups.");
    EndSection();
}

static void render_section_trajectory(OverlayConfig& cfg)
{
    BeginSection("Trajectories", "##sec_traj_main");
    RenderToggleTip("Show Invisible", "##TrajWalls", &cfg.trajectory_show_through_walls,
                    "Shows grenade trajectories through walls.");

    ImGui::Spacing();
    ImGui::TextColored(MenuTheme::text_secondary, "Sizes & Timing");
    RenderSliderFloat("Line Thickness", "##traj_thick", &cfg.trajectory_thickness, 0.5f, 5.0f, "%.1f");
    MenuTooltip::show_item("Sets the grenade trajectory line thickness.");
    RenderSliderFloat("Bounce Box Size", "##traj_bounce", &cfg.trajectory_bounce_size, 0.5f, 10.0f, "%.1f");
    MenuTooltip::show_item("Sets the size of trajectory bounce markers.");
    RenderSliderFloat("Detonation Radius", "##traj_detonat", &cfg.trajectory_detonation_radius, 5.0f, 50.0f, "%.1f");
    MenuTooltip::show_item("Sets the radius shown around a grenade detonation.");
    RenderSliderFloat("Fade Duration", "##traj_fade", &cfg.trajectory_fade_time, 0.1f, 5.0f, "%.1fs");
    MenuTooltip::show_item("Sets how long old trajectory segments remain visible.");
    EndSection();
}

static void render_section_grenade_creator()
{
    BeginSection("Grenade Creator", "##sec_gcreator");

    MenuClient* menu = MenuClient::get_instance();
    if (menu) {
            ImGui::Spacing();
            ImGui::Text("Unsaved Preview Lineup Config:");
            ImGui::Spacing();

            ImGui::InputText("Title", menu->gcreator_title, sizeof(menu->gcreator_title));
            MenuTooltip::show_item("Sets the lineup title.");
            ImGui::InputText("Description", menu->gcreator_desc, sizeof(menu->gcreator_desc));
            MenuTooltip::show_item("Sets the lineup description.");
            ImGui::InputText("Map", menu->gcreator_map, sizeof(menu->gcreator_map));
            MenuTooltip::show_item("Sets the map name for the lineup.");

            const char* weapon_types[] = { "HE", "Flash", "Smoke", "Molotov", "Decoy" };
            RenderPaddedCombo("Grenade Type", &menu->gcreator_weapon_idx, weapon_types,
                              IM_ARRAYSIZE(weapon_types));
            MenuTooltip::show_item("Selects the grenade used by the lineup.");

            ImGui::InputFloat3("Throw Position", menu->gcreator_pos, "%.2f");
            MenuTooltip::show_item("Sets the world position where the throw starts.");
            ImGui::InputFloat2("View Angles (P/Y)", menu->gcreator_angles, "%.2f");
            MenuTooltip::show_item("Sets the pitch and yaw for the throw.");

            const char* throw_methods[] = { "left", "right", "middle", "none" };
            RenderPaddedCombo("Throw Type", &menu->gcreator_throw_type_idx, throw_methods,
                              IM_ARRAYSIZE(throw_methods));
            MenuTooltip::show_item("Selects the throw release type.");

            RenderToggleTip("Crouch", "##gcreator_crouch", &menu->gcreator_crouch,
                            "Adds crouching to the lineup movement.");
            RenderToggleTip("Sneak (Shift)", "##gcreator_shift", &menu->gcreator_shift,
                            "Adds the Shift movement modifier to the lineup.");
            RenderToggleTip("Jump", "##gcreator_jump", &menu->gcreator_jump,
                            "Adds a jump to the lineup movement.");

            ImGui::InputText("Movement Key", menu->gcreator_movement_key, sizeof(menu->gcreator_movement_key));
            MenuTooltip::show_item("Sets the key used during lineup movement.");
            ImGui::InputInt("Movement Ticks", &menu->gcreator_movement_ticks);
            MenuTooltip::show_item("Sets how many ticks the movement input is held.");
            if (menu->gcreator_movement_ticks < 0) menu->gcreator_movement_ticks = 0;

            ImGui::Spacing();
            if (ImGui::Button("Auto Fill Current Position/Angles", ImVec2(-1.0f, 0.0f))) {
                App* app = App::get_instance();
                if (app) {
                    const auto& last_in = app->get_last_input();
                    menu->gcreator_pos[0] = last_in.packet.local_origin.x;
                    menu->gcreator_pos[1] = last_in.packet.local_origin.y;
                    menu->gcreator_pos[2] = last_in.packet.local_origin.z;
                    menu->gcreator_angles[0] = last_in.packet.local_angles.x;
                    menu->gcreator_angles[1] = last_in.packet.local_angles.y;

                    // Map weapon type
                    int held = last_in.packet.held_weapon_id;
                    int w_idx = -1;
                    if (held == 44) w_idx = 0; // HE
                    else if (held == 43) w_idx = 1; // Flash
                    else if (held == 45) w_idx = 2; // Smoke
                    else if (held == 46 || held == 48) w_idx = 3; // Molotov
                    else if (held == 47) w_idx = 4; // Decoy

                    if (w_idx != -1) {
                        menu->gcreator_weapon_idx = w_idx;
                    }

                    // Auto-fill active map name
                    std::string map_name = app->get_current_map();
                    if (!map_name.empty()) {
                        std::string clean_map = map_name;
                        if (clean_map.find("maps/") == 0) clean_map = clean_map.substr(5);
                        if (clean_map.find(".vpk") != std::string::npos) clean_map = clean_map.substr(0, clean_map.find(".vpk"));
                        std::strncpy(menu->gcreator_map, clean_map.c_str(), sizeof(menu->gcreator_map));
                    }
                }
            }
            MenuTooltip::show_item("Copies the current map, position, view angles, and held grenade into the form.");

            if (ImGui::Button("Save Created Lineup", ImVec2(-1.0f, 0.0f))) {
                App* app = App::get_instance();
                if (app) {
                    std::string target_map = menu->gcreator_map;
                    if (target_map.empty()) {
                        target_map = app->get_current_map();
                        if (target_map.find("maps/") == 0) target_map = target_map.substr(5);
                        if (target_map.find(".vpk") != std::string::npos) target_map = target_map.substr(0, target_map.find(".vpk"));
                    }

                    if (!target_map.empty()) {
                        GrenadeLineup lineup;
                        lineup.title = menu->gcreator_title;
                        lineup.description = menu->gcreator_desc;
                        lineup.weapon = static_cast<uint8_t>(menu->gcreator_weapon_idx + 1);
                        lineup.position = Vec3(menu->gcreator_pos[0], menu->gcreator_pos[1], menu->gcreator_pos[2]);
                        lineup.viewangles = Vec3(menu->gcreator_angles[0], menu->gcreator_angles[1], 0.0f);
                        lineup.movement.throw_type = throw_methods[menu->gcreator_throw_type_idx];
                        if (menu->gcreator_crouch) lineup.movement.modifiers.push_back("crouch");
                        if (menu->gcreator_shift) lineup.movement.modifiers.push_back("shift");
                        lineup.movement.movement_key = menu->gcreator_movement_key;
                        lineup.movement.jump = menu->gcreator_jump;
                        lineup.movement.movement_ticks = menu->gcreator_movement_ticks;

                        std::vector<std::string> parts;
                        if (lineup.movement.throw_type != "none") parts.push_back(lineup.movement.throw_type);
                        for (const auto& mod : lineup.movement.modifiers) parts.push_back(mod);
                        if (!lineup.movement.movement_key.empty()) parts.push_back(lineup.movement.movement_key);
                        if (lineup.movement.jump) parts.push_back("jump");
                        if (parts.empty()) {
                            lineup.throw_method = "manual";
                        } else {
                            std::string res;
                            for (size_t i = 0; i < parts.size(); ++i) {
                                if (i > 0) res += " + ";
                                res += parts[i];
                            }
                            lineup.throw_method = res;
                        }

                        app->get_grenade_helper_data().add_local_lineup(target_map, lineup);

                        menu->gcreator_title[0] = '\0';
                        menu->gcreator_desc[0] = '\0';
                        menu->gcreator_pos[0] = menu->gcreator_pos[1] = menu->gcreator_pos[2] = 0.0f;
                        menu->gcreator_angles[0] = menu->gcreator_angles[1] = menu->gcreator_angles[2] = 0.0f;
                        menu->gcreator_throw_type_idx = 0;
                        menu->gcreator_crouch = false;
                        menu->gcreator_shift = false;
                        menu->gcreator_movement_key[0] = '\0';
                        menu->gcreator_jump = false;
                        menu->gcreator_movement_ticks = 0;
                    }
                }
            }
            MenuTooltip::show_item("Saves the entered lineup to the local grenade data.");
    }
    EndSection();
}

static void render_section_grenade_helper(OverlayConfig& cfg)
{
    BeginSection("Grenade Helper", "##sec_ghelper");

    ImGui::Spacing();
    RenderSliderFloat("Aim Activation Radius (Units)", "##gh_aim", &cfg.grenade_helper_aim_radius, 1.0f, 50.0f,
                      "%.1f");
    MenuTooltip::show_item("Sets how close the crosshair must be to a lineup aim point.");
    ImGui::Spacing();
    RenderSliderFloat("Render Distance (Units)", "##gh_distance", &cfg.grenade_helper_render_distance, 100.0f, 5000.0f,
                      "%.0f");
    MenuTooltip::show_item("Sets how far away lineup guidance is rendered.");
    ImGui::Spacing();
    RenderToggleTip("Show Trace Lines", "##gh_show_trace_lines", &cfg.grenade_helper_show_trace_lines,
                    "Shows the predicted grenade path from the lineup.");
    RenderToggleTip("Visibility Check", "##gh_vischeck", &cfg.grenade_helper_vischeck,
                    "Hides lineup guidance when the standing spot is blocked by map geometry.");
    RenderToggleTip("Auto Aim", "##gh_auto_aim", &cfg.grenade_helper_auto_aim,
                    "Automatically aims at the selected lineup.");
    if (cfg.grenade_helper_auto_aim) {
        RenderSliderFloat("Auto Aim FOV", "##gh_aim_fov", &cfg.grenade_helper_aim_fov, 1.0f, 180.0f, "%.1f deg");
        MenuTooltip::show_item("Sets the maximum angle for automatic lineup aiming.");
        RenderSliderFloat("Auto Aim Smooth", "##gh_aim_smooth", &cfg.grenade_helper_aim_speed, 1.0f, 200.0f, "%.0f%%");
        MenuTooltip::show_item("Sets the smoothing applied to automatic lineup aiming.");
    }
    RenderToggleTip("Auto Throw", "##gh_auto_throw", &cfg.grenade_helper_auto_throw,
                    "Automatically performs the selected throw when aligned.");

    ImGui::Spacing();
    RenderKeybindConfig("Activation Key", "grenade_helper_active", 1003);
    EndSection();
}
static void render_section_grenade_solver(OverlayConfig& cfg)
{
    BeginSection("Grenade Solver", "##sec_gsolver_settings");

    ImGui::Spacing();
    RenderToggleTip("Show Zones", "##SolverZones", &cfg.grenade_solver_show_zones,
                    "Shows grenade solver landing zones.");
    RenderSliderFloat("Solver Still Time (s)", "##SolverStill", &cfg.grenade_solver_still_time, 0.1f, 2.0f, "%.2f");
    MenuTooltip::show_item("Sets how long the solver waits for a stable landing result.");
        
    ImGui::Spacing();
    ImGui::Spacing();
        
    RenderToggleTip("Enable Location Creator", "##LocCreatorEnabled", &cfg.grenade_solver_location_creator_enabled,
                    "Enables manual creation of grenade landing zones.");
        
    if (cfg.grenade_solver_location_creator_enabled) {
            ImGui::Spacing();
            ImGui::Spacing();

            // Bind button
            ImGui::AlignTextToFramePadding();
            ImGui::Text("Creation Bind");
            ImGui::SameLine();
            
            // Draw the keybind button for grenade_solver_create_bind
            MenuClient* menu = MenuClient::get_instance();
            if (menu) {
                OverlayConfig& menu_cfg = menu->get_cfg();
                KeyBind* kb = nullptr;
                for (auto& bind : menu_cfg.keybinds) {
                    if (bind.setting_name == "grenade_solver_create_bind") {
                        kb = &bind;
                        break;
                    }
                }
                if (!kb) {
                    KeyBind new_kb;
                    new_kb.setting_name = "grenade_solver_create_bind";
                    new_kb.key = menu_cfg.grenade_solver_create_bind;
                    menu_cfg.keybinds.push_back(new_kb);
                    kb = &menu_cfg.keybinds.back();
                }
                
                std::string btn_label = GetKeybindLabel(menu, kb);
                float text_w = ImGui::CalcTextSize(btn_label.c_str()).x;
                float spacing = 4.0f;
                ImGui::SameLine();
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - text_w - spacing);
                DrawKeybindButton("grenade_solver_create_bind");
            }

            ImGui::Spacing();
            ImGui::Spacing();
            
            const char* gtypes[] = { "HE", "Flash", "Smoke", "Molotov", "Decoy" };
            
            ImGui::Text("Manual Location Creator:");
            ImGui::Spacing();
            
            if (menu) {
                ImGui::InputText("Location Name", menu->creator_name, sizeof(menu->creator_name));
                MenuTooltip::show_item("Sets the name of the landing zone.");
                ImGui::InputFloat3("Min Bound (Mins)", menu->creator_mins, "%.1f");
                MenuTooltip::show_item("Sets the minimum corner of the landing zone bounds.");
                ImGui::InputFloat3("Max Bound (Maxs)", menu->creator_maxs, "%.1f");
                MenuTooltip::show_item("Sets the maximum corner of the landing zone bounds.");
                RenderPaddedCombo("Grenade Type", &menu->creator_grenade_type, gtypes,
                                  IM_ARRAYSIZE(gtypes));
                MenuTooltip::show_item("Selects the grenade type for the landing zone.");
                
                ImGui::Spacing();
                if (ImGui::Button("Save Location", ImVec2(-1.0f, 0.0f))) {
                    App* app = App::get_instance();
                    if (app) {
                        std::string map_name = app->get_current_map();
                        if (!map_name.empty()) {
                            LandingZone zone;
                            zone.name = menu->creator_name;
                            zone.mins = glm::vec3(menu->creator_mins[0], menu->creator_mins[1], menu->creator_mins[2]);
                            zone.maxs = glm::vec3(menu->creator_maxs[0], menu->creator_maxs[1], menu->creator_maxs[2]);
                            zone.grenade_type = static_cast<uint8_t>(menu->creator_grenade_type + 1);
                            
                            // Add to landing zone manager and save to disk
                            app->get_landing_zone_manager().add_zone(map_name, zone);
                            app->get_landing_zone_manager().save();

                            // Update VisibilityWorker zones immediately
                            app->get_visibility_worker().set_landing_zones(app->get_landing_zone_manager().get_zones(map_name));
                            
                            // Reset fields
                            menu->creator_name[0] = '\0';
                            menu->creator_mins[0] = menu->creator_mins[1] = menu->creator_mins[2] = 0.0f;
                            menu->creator_maxs[0] = menu->creator_maxs[1] = menu->creator_maxs[2] = 0.0f;
                        }
                    }
                }
                MenuTooltip::show_item("Saves the landing zone for the current map.");
            }
    }
    
    EndSection();
}

static void render_section_vpk(OverlayConfig& cfg)
{
    BeginSection("VPK Parser", "##sec_vpk");
    RenderToggleTip("Enable Vis-Check", "##VisCheck", &cfg.use_depth_prepass,
                      "Uploads and renders full VPK map geometry on the GPU to occlude player chams realistically.");
    RenderToggleTip("Smoke Voxel Check", "##SmokeCheck", &cfg.esp_smoke_check,
                      "Checks if player visibility ray intersects dynamic smoke grenade voxels.");

    ImGui::Spacing();
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Active Mode:");
    ImGui::SameLine();
    if (cfg.use_depth_prepass) {
        ImGui::Text("GPU Depth Prepass");
    } else {
        ImGui::Text("None (Always Visible)");
    }
    EndSection();
}

static void render_section_map_geo(OverlayConfig& cfg)
{
    BeginSection("Map Geometry", "##sec_map_geo");
    RenderToggleTip("Show Map Geometry", "##MapVis", &cfg.map_visualizer_enabled,
                    "Shows the loaded map geometry as a wireframe.");
    if (cfg.map_visualizer_enabled) {
        RenderToggleTip("Only Visible", "##MapDepth", &cfg.map_visualizer_depth_tested,
                        "Hides map geometry that is behind other map surfaces.");
        RenderColorButton("Wireframe Color", "##map_wire_btn", "##map_wire_pick", cfg.map_visualizer_color);
        MenuTooltip::show_item("Sets the map geometry wireframe color.");
    }
    EndSection();
}

static void render_section_paths(OverlayConfig& cfg)
{
    BeginSection("Directories", "##sec_paths");
    char maps_dir_buf[256];
    std::strncpy(maps_dir_buf, cfg.maps_dir.c_str(), sizeof(maps_dir_buf));
    if (RenderInputText("Maps Path", "##MapsPath", maps_dir_buf, sizeof(maps_dir_buf))) {
        cfg.maps_dir = maps_dir_buf;
    }
    MenuTooltip::show_item("Sets the directory containing map files.");

    char vpk_path_buf[256];
    std::strncpy(vpk_path_buf, cfg.vpk_path.c_str(), sizeof(vpk_path_buf));
    if (RenderInputText("VPK Path", "##VPKPath", vpk_path_buf, sizeof(vpk_path_buf))) {
        cfg.vpk_path = vpk_path_buf;
    }
    MenuTooltip::show_item("Sets the directory containing VPK game archives.");
    EndSection();
}

static void render_section_performance(OverlayConfig& cfg, GLFWwindow* overlay_window, GLFWwindow* menu_window)
{
    BeginSection("Performance & Rendering", "##sec_perf");
    RenderSliderInt("Target FPS", "##TargetFPS", &cfg.fps, 0, 360, "%d FPS");
    MenuTooltip::show_item("0 means unlimited. The overlay uses a high-precision sleeper to match the rate.");

    RenderToggleTip("Show FPS Counter", "##ShowFPS", &cfg.show_fps,
                    "Shows the current overlay frame rate.");
    if (cfg.show_fps) {
        RenderSliderFloat("Size", "##fps_scale", &cfg.fps_scale, 0.75f, 2.0f, "%.2fx");
        MenuTooltip::show_item("Sets the FPS counter size.");
    }

    bool prev_vsync = cfg.vsync;
    RenderToggle("Enable VSync", "##VSync", &cfg.vsync);
    MenuTooltip::show_item(
        "Synchronizes the overlay's swap interval to the monitor refresh rate.\nReduces tearing but may cap "
        "FPS. Overrides Target FPS when enabled.");
    if (cfg.vsync != prev_vsync && overlay_window) {
        glfwMakeContextCurrent(overlay_window);
        glfwSwapInterval(cfg.vsync ? 1 : 0);
        glfwMakeContextCurrent(menu_window);
    }

    RenderToggleTip("Extrapolate View Matrix", "##Extrapolate", &cfg.extrapolate,
                      "Performs linear extrapolation of game view projection matrix to smooth chams tracking.");

    RenderToggleTip("Hyprland Compatibility Mode", "##Hyprland", &cfg.hyprland_support,
                      "Bypasses X11 override_redirect. Useful for wlroots-based WMs like Hyprland.\nNote: Requires "
                      "restarting the overlay application to take effect.");
    EndSection();
}

static void render_section_geoscale(OverlayConfig& cfg)
{
    BeginSection("Overlay Scaling", "##sec_geoscale");

    static const std::vector<std::pair<std::string, std::string>> scaling_options = {
        {"stretched", "Full Screen / Fullscreen Windowed"},
        {"centered", "Centered (Black Bars)"},
        {"custom", "Custom Position / Size"}};

    std::string current_scaling_label = "Stretched (Full Screen)";
    for (const auto& opt : scaling_options) {
        if (opt.first == cfg.scaling) {
            current_scaling_label = opt.second;
            break;
        }
    }

    ImGui::Text("Scaling Mode");
    ImGui::SetNextItemWidth(-1.0f);
    if (BeginPaddedCombo("##ScalingMode", current_scaling_label.c_str())) {
        for (const auto& opt : scaling_options) {
            bool is_selected = (opt.first == cfg.scaling);
            if (ImGui::Selectable(opt.second.c_str(), is_selected)) {
                cfg.scaling = opt.first;
            }
            if (is_selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        EndPaddedCombo();
    }
    MenuTooltip::show_item("Selects how the game area is positioned and scaled in the overlay.");

    RenderInputInt("Monitor Width", "##MonW", &cfg.monitor_w);
    MenuTooltip::show_item("Sets the target monitor width in pixels.");
    RenderInputInt("Monitor Height", "##MonH", &cfg.monitor_h);
    MenuTooltip::show_item("Sets the target monitor height in pixels.");
    RenderInputInt("Game Width", "##GameW", &cfg.game_w);
    MenuTooltip::show_item("Sets the game viewport width in pixels.");
    RenderInputInt("Game Height", "##GameH", &cfg.game_h);
    MenuTooltip::show_item("Sets the game viewport height in pixels.");

    if (cfg.scaling == "custom") {
        RenderInputInt("Offset X (Game Left)", "##OffX", &cfg.game_x);
        MenuTooltip::show_item("Sets the horizontal offset of the game area.");
        RenderInputInt("Offset Y (Game Top)", "##OffY", &cfg.game_y);
        MenuTooltip::show_item("Sets the vertical offset of the game area.");
    }
    EndSection();
}

static void render_section_hardware(OverlayConfig& cfg, OverlayClient* overlay)
{
    BeginSection("Hardware Selection", "##sec_hw");

    // Display current GPU info
    const char* gl_vendor = (const char*)glGetString(GL_VENDOR);
    const char* gl_renderer = (const char*)glGetString(GL_RENDERER);
    if (gl_vendor && gl_renderer) {
        ImGui::Text("Active GPU: %s (%s)", gl_renderer, gl_vendor);
    }

    // GPU Preference combo
    std::vector<GpuDevice> detected_gpus = detect_gpus();

    std::string current_gpu_label = "Default / System Decision";
    if (cfg.gpu_preference == "default") {
        current_gpu_label = "Default / System Decision";
    } else {
        for (const auto& gpu : detected_gpus) {
            if (gpu.name == cfg.gpu_preference) {
                current_gpu_label = gpu.display_name;
                break;
            }
        }
    }

    ImGui::Text("Preferred GPU");
    ImGui::SetNextItemWidth(-1.0f);
    if (BeginPaddedCombo("##PreferredGPU", current_gpu_label.c_str())) {
        bool is_default_selected = (cfg.gpu_preference == "default");
        if (ImGui::Selectable("Default / System Decision", is_default_selected)) {
            cfg.gpu_preference = "default";
        }
        if (is_default_selected) {
            ImGui::SetItemDefaultFocus();
        }

        for (const auto& gpu : detected_gpus) {
            bool is_selected = (gpu.name == cfg.gpu_preference);
            if (ImGui::Selectable(gpu.display_name.c_str(), is_selected)) {
                cfg.gpu_preference = gpu.name;
            }
            if (is_selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        EndPaddedCombo();
    }
    MenuTooltip::show_item(
        "Forces OpenGL driver loading via Prime render offload settings.\n(Requires restarting the application "
        "to apply).");

    // Monitor Selection combo
    int monitor_count = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&monitor_count);
    std::string current_monitor_label = "Monitor 0 (Primary)";

    if (monitors && monitor_count > 0) {
        int target_idx = cfg.monitor_index;
        if (target_idx < 0 || target_idx >= monitor_count) {
            target_idx = 0;
        }
        GLFWmonitor* m = monitors[target_idx];
        const GLFWvidmode* mode = glfwGetVideoMode(m);
        char label_buf[128];
        if (mode) {
            std::snprintf(label_buf, sizeof(label_buf), "Monitor %d (%s) - %dx%d", target_idx, glfwGetMonitorName(m),
                          mode->width, mode->height);
        } else {
            std::snprintf(label_buf, sizeof(label_buf), "Monitor %d (%s)", target_idx, glfwGetMonitorName(m));
        }
        current_monitor_label = label_buf;
    }

    ImGui::Text("Target Monitor");
    ImGui::SetNextItemWidth(-1.0f);
    if (BeginPaddedCombo("##TargetMonitor", current_monitor_label.c_str())) {
        for (int i = 0; i < monitor_count; ++i) {
            GLFWmonitor* m = monitors[i];
            const GLFWvidmode* mode = glfwGetVideoMode(m);
            char label_buf[128];
            if (mode) {
                std::snprintf(label_buf, sizeof(label_buf), "Monitor %d (%s) - %dx%d", i, glfwGetMonitorName(m),
                              mode->width, mode->height);
            } else {
                std::snprintf(label_buf, sizeof(label_buf), "Monitor %d (%s)", i, glfwGetMonitorName(m));
            }

            bool is_selected = (i == cfg.monitor_index);
            if (ImGui::Selectable(label_buf, is_selected)) {
                cfg.monitor_index = i;
                // Update monitor dimensions so ESP coordinate math stays correct
                if (mode) {
                    cfg.monitor_w = mode->width;
                    cfg.monitor_h = mode->height;
                }
                if (overlay) {
                    overlay->update_monitor_index(i);
                }
            }
            if (is_selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        EndPaddedCombo();
    }
    MenuTooltip::show_item(
        "Sets the monitor where the transparent game overlay will be placed.\n(Applies in real-time).");
    EndSection();
}

static void render_section_memreader(OverlayConfig& cfg, const GameReader& reader)
{
    // Frequency Tuning section — custom layout: title + right-justified stats, then input below
    BeginSection("Memory Reader Thread Tuning", "##sec_memreader");

    // 1. Local Player Frequency
    {
        float w = get_section_width();
        float est = 1000.0f / (cfg.gamereader_local_ms > 0.0f ? cfg.gamereader_local_ms : 1.0f);
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Local Player");
        ImGui::SameLine();
        char stats_buf[128];
        std::snprintf(stats_buf, sizeof(stats_buf), "EST: %.2f tps, ACT: %.2f tps", est, reader.get_local_tps());
        float stats_w = ImGui::CalcTextSize(stats_buf).x;
        ImGui::SetCursorPosX(w - stats_w);
        ImGui::TextDisabled("%s", stats_buf);
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputFloat("##LocalFreq", &cfg.gamereader_local_ms, 0.5f, 1.0f, "%.2f ms")) {
            if (cfg.gamereader_local_ms < 1.0f) cfg.gamereader_local_ms = 1.0f;
            if (cfg.gamereader_local_ms > 100.0f) cfg.gamereader_local_ms = 100.0f;
        }
        MenuTooltip::show_item("Sets the local player read interval in milliseconds.");
    }
    ImGui::Spacing();

    // 2. Players Frequency
    {
        float w = get_section_width();
        float est = 1000.0f / (cfg.gamereader_players_ms > 0.0f ? cfg.gamereader_players_ms : 1.0f);
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Other Players");
        ImGui::SameLine();
        char stats_buf[128];
        std::snprintf(stats_buf, sizeof(stats_buf), "EST: %.2f tps, ACT: %.2f tps", est, reader.get_players_tps());
        float stats_w = ImGui::CalcTextSize(stats_buf).x;
        ImGui::SetCursorPosX(w - stats_w);
        ImGui::TextDisabled("%s", stats_buf);
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputFloat("##PlayersFreq", &cfg.gamereader_players_ms, 0.5f, 1.0f, "%.2f ms")) {
            if (cfg.gamereader_players_ms < 1.0f) cfg.gamereader_players_ms = 1.0f;
            if (cfg.gamereader_players_ms > 100.0f) cfg.gamereader_players_ms = 100.0f;
        }
        MenuTooltip::show_item("Sets the other-player read interval in milliseconds.");
    }
    ImGui::Spacing();

    // 3. Entities Frequency
    {
        float w = get_section_width();
        float est = 1000.0f / (cfg.gamereader_entities_ms > 0.0f ? cfg.gamereader_entities_ms : 1.0f);
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Non-Player Entities");
        ImGui::SameLine();
        char stats_buf[128];
        std::snprintf(stats_buf, sizeof(stats_buf), "EST: %.2f tps, ACT: %.2f tps", est, reader.get_entities_tps());
        float stats_w = ImGui::CalcTextSize(stats_buf).x;
        ImGui::SetCursorPosX(w - stats_w);
        ImGui::TextDisabled("%s", stats_buf);
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputFloat("##EntitiesFreq", &cfg.gamereader_entities_ms, 0.5f, 1.0f, "%.2f ms")) {
            if (cfg.gamereader_entities_ms < 1.0f) cfg.gamereader_entities_ms = 1.0f;
            if (cfg.gamereader_entities_ms > 100.0f) cfg.gamereader_entities_ms = 100.0f;
        }
        MenuTooltip::show_item("Sets the non-player entity read interval in milliseconds.");
    }

    // Automatic live offset refresh status (runs once on startup)
    {
        const offset_refresher::RefreshStatus& refresh = offset_refresher::last_status();
        if (refresh.attempted) {
            ImGui::Spacing();
            const ImVec4 color = refresh.success ? ImVec4(0.45f, 0.9f, 0.45f, 1.0f)
                                                 : ImVec4(1.0f, 0.35f, 0.35f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, color);
            ImGui::TextWrapped("Offsets: %s", refresh.summary.c_str());
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered() && (!refresh.errors.empty() || !refresh.warnings.empty())) {
                ImGui::BeginTooltip();
                constexpr size_t kTooltipLimit = 12;
                size_t shown = 0;
                for (const std::string& error : refresh.errors) {
                    if (shown++ >= kTooltipLimit) break;
                    ImGui::TextUnformatted(("Error: " + error).c_str());
                }
                for (const std::string& warning : refresh.warnings) {
                    if (shown++ >= kTooltipLimit) {
                        ImGui::TextDisabled("...");
                        break;
                    }
                    ImGui::TextUnformatted(("Warning: " + warning).c_str());
                }
                ImGui::EndTooltip();
            }
        }
    }
    EndSection();
}

static void render_section_convar_tester(const GameReader& reader)
{
    BeginSection("ConVar Reader", "##sec_convar_reader");

    if (!reader.is_attached()) {
        ImGui::TextDisabled("Waiting for cs2 process attachment...");
        EndSection();
        return;
    }

    const uintptr_t cvar_iface = reader.get_cvar_interface_address();
    if (cvar_iface == 0) {
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.2f, 1.0f),
                           "ICvar interface not resolved (libtier0.so / CreateInterface).");
    } else {
        ImGui::Text("ICvar interface: 0x%llX", static_cast<unsigned long long>(cvar_iface));
    }

    static char convar_name_buf[128] = "sv_cheats";
    static ConvarLookupResult last_result{};
    static bool auto_refresh = false;

    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##convar_name", "convar name", convar_name_buf, sizeof(convar_name_buf));
    MenuTooltip::show_item("Enter the name of the game convar to inspect.");

    const float button_height = 30.0f * g_menu_preferences.menu_scale;
    if (ImGui::Button("Lookup", ImVec2(-1.0f, button_height))) {
        last_result = reader.lookup_convar(convar_name_buf);
    }
    MenuTooltip::show_item("Looks up the entered convar every frame while enabled.");
    RenderToggleTip("Auto refresh", "##convar_auto_refresh", &auto_refresh,
                    "Repeats the convar lookup while enabled.");

    if (auto_refresh && convar_name_buf[0] != '\0') {
        last_result = reader.lookup_convar(convar_name_buf);
    }

    ImGui::Spacing();
    if (last_result.found) {
        ImGui::Text("Name: %s", last_result.name.c_str());
        ImGui::Text("Address: 0x%llX", static_cast<unsigned long long>(last_result.address));
        ImGui::Text("Type: %s (%d)", convar_type_name(last_result.type), last_result.type);
        ImGui::Text("Flags: %s (0x%X)", convar_flags_summary(last_result.flags), last_result.flags);
        ImGui::Text("Value: %s", last_result.value.c_str());
        if (!last_result.default_value.empty()) {
            ImGui::Text("Default: %s", last_result.default_value.c_str());
        }
        if (!last_result.description.empty()) {
            ImGui::TextWrapped("Description: %s", last_result.description.c_str());
        }
    } else if (!last_result.error.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", last_result.error.c_str());
    } else {
        ImGui::TextDisabled("Enter a convar name and click Lookup.");
    }

    EndSection();
}

static void render_section_debug_diag(OverlayConfig& cfg)
{
    BeginSection("Debug Diagnostics", "##sec_debug_diag");
    RenderToggleTip("Debug Game Reader", "##DebugGR", &cfg.debug_game_reader,
                      "Toggles the logging of game reader packet and processing stats in the terminal.");
    RenderToggleTip("Timings", "##DebugGPU", &cfg.debug_gpu_profiling,
                    "Shows GPU and rendering timing diagnostics.");
    RenderToggleTip("Debug Visibility", "##DebugVis", &cfg.debug_visibility,
                      "Toggles detailed console logging for grenade/trajectory state changes.");
    RenderToggleTip("CS2 Focus Check", "##FocusCheck", &cfg.focus_check_enabled,
                    "Only renders the game overlay and permits synthetic input while CS2 is focused.");
    EndSection();
}

// ─────────────────────────────────────────────────────────────────────────────
// Composite tab render functions (called from menu_client)
// Each uses a single-row, two-cell table so columns are fully independent
// and sections stack top-to-bottom with no cross-column height sync gaps.
// ─────────────────────────────────────────────────────────────────────────────

static void render_section_world_features(OverlayConfig& cfg)
{
    BeginSection("World Features", "##sec_world_features");
    RenderToggleTip("Bomb Bar", "##esp_bomb_bar", &cfg.esp_bomb_bar,
                    "Shows the planted bomb timer as a bar.");
    RenderToggleTip("World C4 Timer", "##c4_render_in_world", &cfg.c4_render_in_world,
                    "Shows the C4 timer at the bomb's world position.");
    RenderToggleTip("Dropped Items ESP", "##esp_dropped_items", &cfg.esp_dropped_items,
                    "Shows ESP for dropped weapons and items.");
    RenderToggleTip("Trajectories", "##TrajEnabled", &cfg.draw_grenade_trajectory,
                    "Shows predicted grenade trajectories.");
    RenderToggleTip("Grenade Timers", "##GWarnsEnabled", &cfg.grenade_warnings_enabled,
                    "Shows timers and warnings for active grenades.");
    RenderToggleTip("Inferno Spread", "##InfernoSpreadEnabled", &cfg.inferno_spread_enabled,
                    "Shows the spread area of active fire grenades.");
    RenderToggleTip("Anti Flash", "##anti_flash", &cfg.anti_flash,
                    "While the local player is blinded, replaces the white flash with a black screen, the map "
                    "wireframe and a center crosshair dot.");
    RenderToggleTip("Smoke Wireframe", "##SmokeWireframe", &cfg.smoke_wireframe,
                    "Draws a smooth wireframe cage following the dense shape of active smoke grenades.");
    RenderToggleTip("Through Smokes", "##ThroughSmokes", &cfg.through_smokes,
                    "Renders the map geometry through smoke grenades, clipped to the smoke shape.");
    RenderToggleTip("Bullet Impacts", "##bullet_impacts_enabled", &cfg.bullet_impacts_enabled,
                    "Replays the game's own bullet impact positions, including recoil and spread.");
    RenderToggleTip("Spread Circle", "##esp_spread_circle", &cfg.esp_spread_circle,
                    "Shows the current weapon spread around the crosshair.");
    RenderToggleTip("Crosshair Distance", "##esp_crosshair_distance", &cfg.esp_crosshair_distance,
                    "Shows the distance from the crosshair to the selected point.");
    RenderToggleTip("Crosshair Overlay", "##crosshair_overlay_enabled",
                    &cfg.crosshair_overlay_enabled,
                    "Enables the configurable crosshair overlay.");
    EndSection();
}

static void render_section_smoke_wireframe(OverlayConfig& cfg)
{
    BeginSection("Smoke Wireframe", "##sec_smoke_wireframe");
    RenderColorButton("Wireframe Color", "##smoke_wireframe_btn", "##smoke_wireframe_pick",
                      cfg.smoke_wireframe_color);
    MenuTooltip::show_item("Sets the smoke wireframe color.");
    EndSection();
}

static void render_section_through_smokes(OverlayConfig& cfg)
{
    BeginSection("Through Smokes", "##sec_through_smokes");
    RenderColorButton("Through Color", "##through_smokes_btn", "##through_smokes_pick",
                      cfg.through_smokes_color);
    MenuTooltip::show_item("Sets the color of map geometry shown through smoke.");
    EndSection();
}

static void render_section_crosshair_overlay(OverlayConfig& cfg)
{
    BeginSection("Crosshair Overlay", "##sec_force_crosshair");
    RenderToggleTip("Force Always Display", "##crosshair_overlay_force",
                    &cfg.crosshair_overlay_force,
                    "Shows the overlay even when the game crosshair is hidden.");
    RenderToggleTip("Unscoped Snipers", "##crosshair_overlay_snipers",
                    &cfg.crosshair_overlay_snipers_enabled,
                    "Shows the overlay for sniper weapons while unscoped.");
    RenderToggleTip("Recoil Crosshair", "##crosshair_overlay_recoil",
                    &cfg.crosshair_overlay_recoil_enabled,
                    "Expands the crosshair based on weapon recoil.");
    if (cfg.crosshair_overlay_recoil_enabled) {
        RenderToggleTip("Only While Shooting", "##crosshair_overlay_only_shooting",
                        &cfg.crosshair_overlay_only_shooting,
                        "Shows recoil expansion only while firing.");
        if (cfg.crosshair_overlay_only_shooting) {
            RenderSliderInt("Shot Delay", "##crosshair_overlay_shot_delay",
                            &cfg.crosshair_overlay_shot_delay, 0, 10, "%d shots");
            MenuTooltip::show_item("Sets how many shots are required before recoil expansion appears.");
        }
    }
    RenderToggleTip("Crosshair ESP", "##crosshair_esp_enabled",
                    &cfg.crosshair_esp_enabled,
                    "Shows an indicator for the enemy under the crosshair.");
    if (cfg.crosshair_esp_enabled) {
        RenderColorButton("Enemy Target Color", "##crosshair_esp_color_btn",
                          "##crosshair_esp_color_pick", cfg.crosshair_esp_color);
        MenuTooltip::show_item("Sets the color of the crosshair ESP target.");
        RenderSliderFloat("Crosshair ESP Fov", "##crosshair_esp_fov",
                          &cfg.crosshair_esp_fov, 0.5f, 30.0f, "%.1f°");
        MenuTooltip::show_item("Sets the angle used to identify a crosshair ESP target.");
    }
    RenderColorButton("Crosshair Color", "##crosshair_overlay_color_btn",
                      "##crosshair_overlay_color_pick", cfg.crosshair_overlay_color);
    MenuTooltip::show_item("Sets the crosshair overlay color.");
    RenderSliderFloat("Size Offset", "##crosshair_overlay_size",
                      &cfg.crosshair_overlay_size, -20.0f, 20.0f, "%.1f");
    MenuTooltip::show_item("Adjusts the crosshair arm length.");
    RenderSliderFloat("Gap Offset", "##crosshair_overlay_gap",
                      &cfg.crosshair_overlay_gap, -20.0f, 20.0f, "%.1f");
    MenuTooltip::show_item("Adjusts the gap between the crosshair and its arms.");
    RenderSliderFloat("Thickness Offset", "##crosshair_overlay_thickness",
                      &cfg.crosshair_overlay_thickness, -10.0f, 10.0f, "%.1f");
    MenuTooltip::show_item("Adjusts the crosshair arm thickness.");
    RenderSliderFloat("Offset X", "##crosshair_overlay_offset_x",
                      &cfg.crosshair_overlay_offset_x, -200.0f, 200.0f, "%.0f px");
    MenuTooltip::show_item("Moves the crosshair horizontally.");
    RenderSliderFloat("Offset Y", "##crosshair_overlay_offset_y",
                      &cfg.crosshair_overlay_offset_y, -200.0f, 200.0f, "%.0f px");
    MenuTooltip::show_item("Moves the crosshair vertically.");

    EndSection();
}

static void render_section_timers(OverlayConfig& cfg)
{
    BeginSection("Timers", "##sec_world_timers");
    RenderToggleTip("Minimal Timer Bars", "##world_timer_minimal_style", &cfg.world_timer_minimal_style,
                    "Uses compact timer bars with less text.");
    RenderSliderFloat("Timer Scale", "##world_timer_scale", &cfg.world_timer_scale,
                      0.50f, 1.50f, "%.2f");
    MenuTooltip::show_item("Sets the scale of world timer displays.");

    if (cfg.grenade_warnings_enabled) {
        ImGui::Spacing();
        ImGui::TextColored(MenuTheme::text_secondary, "Grenade Timers");
        RenderSliderFloat("Warning Distance", "##gw_dist", &cfg.grenade_warning_distance, 100.0f, 5000.0f, "%.0f u");
        MenuTooltip::show_item("Sets the distance where grenade warnings begin.");
        RenderSliderFloat("Warning Time", "##gw_time", &cfg.grenade_warning_time, 0.5f, 5.0f, "%.1fs");
        MenuTooltip::show_item("Sets how early grenade detonation warnings appear.");
    }

    if (cfg.c4_render_in_world) {
        ImGui::Spacing();
        ImGui::TextColored(MenuTheme::text_secondary, "World C4 Timer");
        RenderToggleTip("Gradient Color", "##c4_timer_grad_toggle", &cfg.c4_timer_gradient_enabled,
                        "Interpolates the world C4 timer color over the remaining time.");
        if (cfg.c4_timer_gradient_enabled) {
            RenderColorButton("Start Color (Full Time)", "##c4_timer_grad_start_btn", "##c4_timer_grad_start_pick", cfg.c4_timer_gradient_start);
            MenuTooltip::show_item("Sets the C4 timer color with the full time remaining.");
            RenderColorButton("End Color (0s Left)", "##c4_timer_grad_end_btn", "##c4_timer_grad_end_pick", cfg.c4_timer_gradient_end);
            MenuTooltip::show_item("Sets the C4 timer color at zero seconds remaining.");
        } else {
            RenderColorButton("Timer Color", "##c4_timer_col_btn", "##c4_timer_col_pick", cfg.c4_timer_color);
            MenuTooltip::show_item("Sets the fixed world C4 timer color.");
        }
    }
    EndSection();
}

static void render_section_world_colors(OverlayConfig& cfg)
{
    BeginSection("Colors", "##sec_world_colors");

    if (cfg.draw_grenade_trajectory || cfg.grenade_warnings_enabled) {
        ImGui::TextColored(MenuTheme::text_secondary, "Trajectories & Grenade Timers");
        RenderColorButton("HE Grenade Color", "##he_btn", "##he_pick", cfg.grenade_color_he);
        MenuTooltip::show_item("Sets the color for HE grenade trajectories and timers.");
        RenderColorButton("Flashbang Color", "##flash_btn", "##flash_pick", cfg.grenade_color_flash);
        MenuTooltip::show_item("Sets the color for flashbang trajectories and timers.");
        RenderColorButton("Smoke Grenade Color", "##smoke_btn", "##smoke_pick", cfg.grenade_color_smoke);
        MenuTooltip::show_item("Sets the color for smoke grenade trajectories and timers.");
        RenderColorButton("Molotov Color", "##molotov_btn", "##molotov_pick", cfg.grenade_color_molotov);
        MenuTooltip::show_item("Sets the color for Molotov trajectories and timers.");
        RenderColorButton("Decoy Color", "##decoy_btn", "##decoy_pick", cfg.grenade_color_decoy);
        MenuTooltip::show_item("Sets the color for decoy trajectories and timers.");
    }

    if (cfg.inferno_spread_enabled) {
        if (cfg.draw_grenade_trajectory || cfg.grenade_warnings_enabled) {
            ImGui::Spacing();
        }
        ImGui::TextColored(MenuTheme::text_secondary, "Inferno Spread");
        RenderColorButton("Outline Color", "##inferno_out_btn", "##inferno_out_pick", cfg.inferno_outline_color);
        MenuTooltip::show_item("Sets the inferno spread outline color.");
        RenderColorButton("Fill Color", "##inferno_fill_btn", "##inferno_fill_pick", cfg.inferno_fill_color);
        MenuTooltip::show_item("Sets the inferno spread fill color.");
    }

    EndSection();
}

static void render_section_bomb_bar(OverlayConfig& cfg)
{
    BeginSection("Bomb Bar", "##sec_bomb_bar");
    RenderColorButton("Background Color", "##bomb_bar_bg_col_btn", "##bomb_bar_bg_col_pick", cfg.bomb_bar_bg_color);
    MenuTooltip::show_item("Sets the bomb bar background color.");
    RenderToggleTip("Gradient Color", "##bomb_bar_grad_toggle", &cfg.bomb_bar_gradient_enabled,
                    "Interpolates the bomb bar color over the remaining time.");
    if (cfg.bomb_bar_gradient_enabled) {
        RenderColorButton("Start Color (Full Time)", "##bomb_bar_grad_start_btn", "##bomb_bar_grad_start_pick", cfg.bomb_bar_gradient_start);
        MenuTooltip::show_item("Sets the bomb bar color with the full time remaining.");
        RenderColorButton("End Color (0s Left)", "##bomb_bar_grad_end_btn", "##bomb_bar_grad_end_pick", cfg.bomb_bar_gradient_end);
        MenuTooltip::show_item("Sets the bomb bar color at zero seconds remaining.");
    } else {
        RenderColorButton("Bar Color", "##bomb_bar_col_btn", "##bomb_bar_col_pick", cfg.bomb_bar_color);
        MenuTooltip::show_item("Sets the fixed bomb bar color.");
    }
    EndSection();
}

static void render_section_dropped_items(OverlayConfig& cfg)
{
    BeginSection("Dropped Items ESP", "##sec_dropped_items");
    RenderToggleTip("Draw Text", "##esp_dropped_items_text", &cfg.esp_dropped_items_text,
                    "Shows item names for dropped items.");
    RenderToggleTip("Draw Icons", "##esp_dropped_items_icons", &cfg.esp_dropped_items_icons,
                    "Shows icons for dropped items.");
    RenderColorButton("Color", "##esp_dropped_items_color_btn", "##esp_dropped_items_color_pick", cfg.esp_dropped_items_color);
    MenuTooltip::show_item("Sets the dropped item ESP color.");
    RenderSliderFloat("Max Distance", "##esp_dropped_items_max_dist", &cfg.esp_dropped_items_max_dist, 100.0f, 5000.0f, "%.0f u");
    MenuTooltip::show_item("Sets the maximum distance for dropped item ESP.");
    EndSection();
}

static void RenderSettingHint(const char* text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, MenuTheme::text_muted);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

static void RenderBeepConeDiagram(float range_m, float fov_deg)
{
    const float scale = ImGui::GetFontSize() / 13.0f;
    const float width = ImGui::GetContentRegionAvail().x;
    const float height = 150.0f * scale;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(width, height));

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const ImVec2 box_max(origin.x + width, origin.y + height);
    draw_list->AddRectFilled(origin, box_max, ImGui::GetColorU32(MenuTheme::bg_input), MenuTheme::panel_rounding);

    const float margin = 14.0f * scale;
    const ImVec2 player(origin.x + width * 0.5f, box_max.y - margin);
    const float limit_radius = std::min(player.y - origin.y - 26.0f * scale, width * 0.5f - margin);
    const float range_fraction = std::clamp(range_m / proximity_beep::kMaxRangeMeters, 0.05f, 1.0f);
    const float radius = limit_radius * range_fraction;
    const float half_fov = IM_PI * 0.5f *
                           std::clamp(fov_deg, proximity_beep::kMinFovDegrees, proximity_beep::kMaxFovDegrees) / 180.0f;
    const float forward = -IM_PI * 0.5f;

    const ImVec4 accent = MenuTheme::accent_primary;
    const ImVec4 muted = MenuTheme::text_muted;
    draw_list->PathArcTo(player, limit_radius, -IM_PI, 0.0f, 48);
    draw_list->PathStroke(ImGui::GetColorU32(ImVec4(muted.x, muted.y, muted.z, 0.35f)), ImDrawFlags_None, 1.0f);

    draw_list->PathLineTo(player);
    draw_list->PathArcTo(player, radius, forward - half_fov, forward + half_fov, 48);
    draw_list->PathFillConvex(ImGui::GetColorU32(ImVec4(accent.x, accent.y, accent.z, 0.22f)));
    draw_list->PathLineTo(player);
    draw_list->PathArcTo(player, radius, forward - half_fov, forward + half_fov, 48);
    draw_list->PathStroke(ImGui::GetColorU32(accent), ImDrawFlags_Closed, 1.5f * scale);

    const float marker = 5.0f * scale;
    draw_list->AddTriangleFilled(ImVec2(player.x, player.y - marker * 1.4f), ImVec2(player.x - marker, player.y + marker),
                                 ImVec2(player.x + marker, player.y + marker),
                                 ImGui::GetColorU32(MenuTheme::text_primary));

    char text[48];
    std::snprintf(text, sizeof(text), "%.0f m", range_m);
    const ImVec2 range_size = ImGui::CalcTextSize(text);
    draw_list->AddText(ImVec2(player.x - range_size.x * 0.5f, player.y - radius - range_size.y - 3.0f * scale),
                       ImGui::GetColorU32(MenuTheme::text_primary), text);

    std::snprintf(text, sizeof(text), "%.0f°", fov_deg);
    const ImVec2 fov_size = ImGui::CalcTextSize(text);
    draw_list->AddText(ImVec2(player.x - fov_size.x * 0.5f, player.y - radius * 0.5f - fov_size.y * 0.5f),
                       ImGui::GetColorU32(MenuTheme::text_primary), text);

    draw_list->AddText(ImVec2(origin.x + 10.0f * scale, origin.y + 8.0f * scale), ImGui::GetColorU32(accent),
                       "Beeps inside the cone");
    draw_list->AddText(ImVec2(origin.x + 10.0f * scale, origin.y + 8.0f * scale + ImGui::GetTextLineHeight()),
                       ImGui::GetColorU32(muted), "Silent outside it");
}

static void render_section_proximity_beep(OverlayConfig& cfg)
{
    BeginSection("Sound ESP", "##sec_proximity_beep");
    RenderBeepConeDiagram(cfg.proximity_beep_max_range, cfg.proximity_beep_fov);
    RenderSliderFloat("Beep Distance", "##proximity_beep_range", &cfg.proximity_beep_max_range, 5.0f,
                      proximity_beep::kMaxRangeMeters, "%.0f m");
    MenuTooltip::show_item("Sets the maximum enemy distance that produces a beep.");
    RenderSettingHint("Enemies farther away than this are silent.");
    RenderSliderFloat("Field of View", "##proximity_beep_fov", &cfg.proximity_beep_fov,
                      proximity_beep::kMinFovDegrees, proximity_beep::kMaxFovDegrees, "%.0f°");
    MenuTooltip::show_item("Sets the forward cone in which enemies can produce a beep.");
    RenderSettingHint("Width of the cone in front of you. Enemies outside it are silent, so there is no all-around beep.");
    RenderSliderFloat("Volume", "##proximity_beep_volume", &cfg.proximity_beep_volume, 0.05f, 1.0f, "%.2f");
    MenuTooltip::show_item("Sets the sound ESP volume.");
    RenderSliderFloat("Fastest Interval", "##proximity_beep_fast", &cfg.proximity_beep_fast_interval, 0.15f, 1.0f, "%.2f s");
    MenuTooltip::show_item("Sets the shortest interval between proximity beeps.");
    RenderSliderFloat("Slowest Interval", "##proximity_beep_slow", &cfg.proximity_beep_slow_interval, 0.3f, 3.0f, "%.2f s");
    MenuTooltip::show_item("Sets the longest interval between proximity beeps.");
    RenderSettingHint("Closer enemies beep faster, louder and at a higher pitch.");
    EndSection();
}

static const std::vector<std::pair<std::string, std::string>> bullet_impact_style_options = {
    {"flash", "Flash"},
    {"boxes", "3D Box"}
};

static void render_bullet_impact_style_combo(const char* label, const char* id, BulletImpactStyle& current_style)
{
    std::string current_label = "Flash";
    for (const auto& opt : bullet_impact_style_options) {
        if (opt.first == (std::string)current_style) {
            current_label = opt.second;
            break;
        }
    }

    ImGui::Text("%s", label);
    ImGui::SetNextItemWidth(-1.0f);
    if (BeginPaddedCombo(id, current_label.c_str())) {
        for (const auto& opt : bullet_impact_style_options) {
            bool is_selected = (opt.first == (std::string)current_style);
            if (ImGui::Selectable(opt.second.c_str(), is_selected)) {
                current_style = BulletImpactStyle(opt.first);
            }
            if (is_selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        EndPaddedCombo();
    }
}

static void render_section_bullet_impacts(OverlayConfig& cfg)
{
    BeginSection("Bullet Impacts", "##sec_bullet_impacts");
    render_bullet_impact_style_combo("Style", "##bullet_impacts_style", cfg.bullet_impacts_style);
    if (cfg.bullet_impacts_style == BulletImpactStyleEnum::BOXES) {
        RenderColorButton("Box Color", "##bullet_impacts_box_col_btn", "##bullet_impacts_box_col_pick",
                          cfg.bullet_impacts_box_color);
        RenderSliderFloat("Box Size", "##bullet_impacts_box_size", &cfg.bullet_impacts_box_size, 1.0f, 20.0f, "%.1f u");
    } else {
        RenderColorButton("Color", "##bullet_impacts_col_btn", "##bullet_impacts_col_pick", cfg.bullet_impacts_color);
        RenderSliderFloat("Size", "##bullet_impacts_size", &cfg.bullet_impacts_size, 1.0f, 20.0f, "%.1f px");
    }
    RenderSliderFloat(cfg.bullet_impacts_hold ? "Fade Time" : "Duration", "##bullet_impacts_dur",
                      &cfg.bullet_impacts_duration, 0.02f, 1.0f, "%.2fs");
    RenderToggle("Hold While Firing", "##bullet_impacts_hold", &cfg.bullet_impacts_hold);
    if (cfg.bullet_impacts_hold)
        RenderSliderFloat("Firing Gap", "##bullet_impacts_hold_gap", &cfg.bullet_impacts_hold_gap, 0.05f, 1.0f, "%.2fs");
    RenderToggle("Additional Impacts", "##bullet_impacts_spread", &cfg.bullet_impacts_spread);
    ImGui::TextWrapped("Uses the game's own impact positions, including recoil and spread. Additional Impacts shows up to 4 pellet or penetration impacts together. 3D Box draws the game's sv_showimpacts-style boxes (red = client). No impact when impact data is unavailable.");
    EndSection();
}

// Master toggles for the Players tab. Every feature's settings live in their
// own card below, revealed when the matching toggle here is on.
static void render_section_player_features(OverlayConfig& cfg)
{
    BeginSection("Player Features", "##sec_player_features");
    RenderToggle("Chams", "##chams_enabled", &cfg.chams_enabled);
    RenderToggleTip("Glow", "##GlowEnabled", &cfg.glow_enabled,
                    "Enables the player glow or stencil outline.");
    RenderToggleTip("ESP", "##EspEnabled", &cfg.esp_enabled,
                    "Enables the player ESP overlay.");
    RenderToggle("DamageFX", "##damage_fx_enabled", &cfg.damage_fx_enabled);
    RenderToggleTip("Sound ESP", "##proximity_beep_enabled", &cfg.proximity_beep_enabled,
                    "Enables directional sound cues for nearby enemies.");
    EndSection();
}

void render_visuals_tab(OverlayConfig& cfg)
{
    BeginTwoColumnLayout("##visuals_2col");
    // Left column
    render_section_player_features(cfg);
    if (BeginAnimatedCard("##anim_chams", cfg.chams_enabled, true)) {
        render_section_chams(cfg);
        EndAnimatedCard();
    }
    if (BeginAnimatedCard("##anim_esp", cfg.esp_enabled, true)) {
        render_section_esp_overlay(cfg);
        EndAnimatedCard();
    }
    if (BeginAnimatedCard("##anim_esp_distance", cfg.esp_enabled && cfg.esp_distance_limit_enabled)) {
        render_section_esp_distance(cfg);
        EndAnimatedCard();
    }
    if (BeginAnimatedCard("##anim_esp_head_circle", cfg.esp_enabled && cfg.esp_head_circle)) {
        render_section_head_circle(cfg);
        EndAnimatedCard();
    }
    if (BeginAnimatedCard("##anim_noise_esp", cfg.esp_enabled && cfg.esp_step_circles)) {
        render_section_step_circles(cfg);
        EndAnimatedCard();
    }
    if (BeginAnimatedCard("##anim_esp_box", cfg.esp_enabled && cfg.esp_box)) {
        render_section_bounding_box(cfg);
        EndAnimatedCard();
    }
    if (BeginAnimatedCard("##anim_esp_flags", cfg.esp_enabled && cfg.esp_flags)) {
        render_section_flags(cfg);
        EndAnimatedCard();
    }
    if (BeginAnimatedCard("##anim_glow", cfg.glow_enabled, true)) {
        render_section_outline(cfg);
        EndAnimatedCard();
    }
    if (BeginAnimatedCard("##anim_damage_fx", cfg.damage_fx_enabled, true)) {
        render_section_damage_fx(cfg);
        EndAnimatedCard();
    }

    // Right column
    SwitchColumn();
    if (BeginAnimatedCard("##anim_proximity_beep", cfg.proximity_beep_enabled)) {
        render_section_proximity_beep(cfg);
        EndAnimatedCard();
    }
    if (BeginAnimatedCard("##anim_esp_skeleton", cfg.esp_enabled && cfg.esp_skeleton)) {
        render_section_skeleton(cfg);
        EndAnimatedCard();
    }
    if (BeginAnimatedCard("##anim_esp_health", cfg.esp_enabled && cfg.esp_health_bar)) {
        render_section_health_bar(cfg);
        EndAnimatedCard();
    }
    if (BeginAnimatedCard("##anim_esp_armor", cfg.esp_enabled && cfg.esp_armor_bar)) {
        render_section_armor_bar(cfg);
        EndAnimatedCard();
    }
    if (BeginAnimatedCard("##anim_esp_ammo", cfg.esp_enabled && cfg.esp_ammo_bar)) {
        render_section_ammo_bar(cfg);
        EndAnimatedCard();
    }
    if (BeginAnimatedCard("##anim_esp_weapon_icons", cfg.esp_enabled && cfg.esp_weapon_icons)) {
        render_section_weapon_icons(cfg);
        EndAnimatedCard();
    }
    if (BeginAnimatedCard("##anim_offscreen_arrows", cfg.esp_enabled && cfg.esp_offscreen_arrows)) {
        render_section_offscreen_arrows(cfg);
        EndAnimatedCard();
    }
    if (BeginAnimatedCard("##anim_esp_reveals", cfg.esp_enabled)) {
        render_section_esp_reveals(cfg);
        EndAnimatedCard();
    }
    EndTwoColumnLayout();
}

static void render_section_anti_flash(OverlayConfig& cfg)
{
    BeginSection("Anti Flash", "##sec_anti_flash");
    RenderSliderFloat("Start Alpha", "##anti_flash_alpha_start", &cfg.anti_flash_alpha_start, 0.0f, 255.0f, "%.0f");
    MenuTooltip::show_item("Sets the initial brightness of the flash transition.");
    RenderSliderFloat("End Alpha", "##anti_flash_alpha_end", &cfg.anti_flash_alpha_end, 0.0f, 255.0f, "%.0f");
    MenuTooltip::show_item("Sets the final brightness of the flash transition.");
    RenderColorButton("Background Color", "##anti_flash_bg_col_btn", "##anti_flash_bg_col_pick",
                      cfg.anti_flash_background_color);
    MenuTooltip::show_item("Sets the anti-flash background color.");
    RenderSliderFloat("Background Opacity", "##anti_flash_background_opacity",
                      &cfg.anti_flash_background_opacity, 0.0f, 1.0f, "%.2f");
    MenuTooltip::show_item("Sets the anti-flash background opacity.");
    RenderToggleTip("Map Wireframe", "##anti_flash_wireframe", &cfg.anti_flash_wireframe,
                    "Draws the map collision wireframe on top of the black screen.");
    if (cfg.anti_flash_wireframe) {
        RenderToggleTip("Only Visible", "##anti_flash_wf_depth", &cfg.anti_flash_wireframe_depth_tested,
                        "Hides wireframe geometry occluded by the map.");
        RenderSliderFloat("Wireframe Thickness", "##anti_flash_wf_thickness",
                          &cfg.anti_flash_wireframe_thickness, 1.0f, 8.0f, "%.1f px");
        MenuTooltip::show_item("Sets the anti-flash wireframe thickness.");
        RenderColorButton("Wireframe Color", "##anti_flash_wf_col_btn", "##anti_flash_wf_col_pick",
                          cfg.anti_flash_wireframe_color);
        MenuTooltip::show_item("Sets the anti-flash wireframe color.");
    }
    RenderToggleTip("Crosshair Dot", "##anti_flash_crosshair", &cfg.anti_flash_crosshair,
                    "Draws a small dot in the center of the screen while blinded.");
    if (cfg.anti_flash_crosshair) {
        RenderColorButton("Dot Color", "##anti_flash_dot_col_btn", "##anti_flash_dot_col_pick",
                          cfg.anti_flash_crosshair_color);
        MenuTooltip::show_item("Sets the anti-flash center dot color.");
        RenderSliderFloat("Dot Radius", "##anti_flash_dot_radius", &cfg.anti_flash_crosshair_radius, 1.0f, 12.0f,
                          "%.1f");
        MenuTooltip::show_item("Sets the anti-flash center dot radius.");
    }
    RenderToggleTip("Force Chams", "##anti_flash_chams", &cfg.anti_flash_chams,
                    "Turns chams on for the blinded window when they are not already enabled, reusing the "
                    "Players -> Chams colours.");
    if (cfg.anti_flash_chams) {
        render_style_combo("Chams Material", "##anti_flash_chams_style", cfg.anti_flash_chams_style);
    }
    EndSection();
}

void render_world_tab(OverlayConfig& cfg)
{
    BeginTwoColumnLayout("##world_2col");
    // Left column
    render_section_world_features(cfg);
    if (BeginAnimatedCard("##anim_bomb_bar", cfg.esp_bomb_bar)) {
        render_section_bomb_bar(cfg);
        EndAnimatedCard();
    }
    if (BeginAnimatedCard("##anim_world_timers", cfg.grenade_warnings_enabled || cfg.c4_render_in_world)) {
        render_section_timers(cfg);
        EndAnimatedCard();
    }
    if (BeginAnimatedCard("##anim_dropped_items", cfg.esp_dropped_items)) {
        render_section_dropped_items(cfg);
        EndAnimatedCard();
    }
    if (BeginAnimatedCard("##anim_anti_flash", cfg.anti_flash, true)) {
        render_section_anti_flash(cfg);
        EndAnimatedCard();
    }
    if (BeginAnimatedCard("##anim_smoke_wireframe", cfg.smoke_wireframe)) {
        render_section_smoke_wireframe(cfg);
        EndAnimatedCard();
    }
    if (BeginAnimatedCard("##anim_through_smokes", cfg.through_smokes)) {
        render_section_through_smokes(cfg);
        EndAnimatedCard();
    }
    if (BeginAnimatedCard("##anim_bullet_impacts", cfg.bullet_impacts_enabled)) {
        render_section_bullet_impacts(cfg);
        EndAnimatedCard();
    }
    if (BeginAnimatedCard("##anim_spread_circle", cfg.esp_spread_circle)) {
        render_section_spread_circle(cfg);
        EndAnimatedCard();
    }
    if (BeginAnimatedCard("##anim_crosshair_distance", cfg.esp_crosshair_distance)) {
        render_section_crosshair_distance(cfg);
        EndAnimatedCard();
    }

    // Right column
    SwitchColumn();
    if (BeginAnimatedCard("##anim_crosshair_overlay", cfg.crosshair_overlay_enabled)) {
        render_section_crosshair_overlay(cfg);
        EndAnimatedCard();
    }
    if (BeginAnimatedCard("##anim_trajectory_card", cfg.draw_grenade_trajectory)) {
        render_section_trajectory(cfg);
        EndAnimatedCard();
    }
    if (BeginAnimatedCard("##anim_world_colors",
                          cfg.draw_grenade_trajectory || cfg.grenade_warnings_enabled || cfg.inferno_spread_enabled)) {
        render_section_world_colors(cfg);
        EndAnimatedCard();
    }
    EndTwoColumnLayout();
}

void render_grenades_tab(OverlayConfig& cfg)
{
    BeginTwoColumnLayout("##grenades_2col");

    render_section_grenade_features(cfg);
    if (BeginAnimatedCard("##anim_grenade_creator", cfg.grenade_creator_enabled)) {
        render_section_grenade_creator();
        EndAnimatedCard();
    }

    SwitchColumn();
    if (BeginAnimatedCard("##anim_grenade_helper", cfg.grenade_helper_enabled)) {
        render_section_grenade_helper(cfg);
        EndAnimatedCard();
    }
    if (BeginAnimatedCard("##anim_grenade_solver", cfg.grenade_solver_show_trajectory)) {
        render_section_grenade_solver(cfg);
        EndAnimatedCard();
    }

    EndTwoColumnLayout();
}

void render_debug_tab(OverlayConfig& cfg, OverlayClient* overlay, GLFWwindow* menu_window, const GameReader& reader)
{
    BeginTwoColumnLayout("##debug_2col");
    // Left column
    render_section_vpk(cfg);
    render_section_paths(cfg);
    render_section_performance(cfg, overlay ? overlay->get_window() : nullptr, menu_window);
    render_section_memreader(cfg, reader);
    render_section_convar_tester(reader);
    // Right column
    SwitchColumn();
    render_section_map_geo(cfg);
    render_section_geoscale(cfg);
    render_section_hardware(cfg, overlay);
    render_section_debug_diag(cfg);
    EndTwoColumnLayout();
}

// ─────────────────────────────────────────────────────────────────────────────
// Trigger Tab
// ─────────────────────────────────────────────────────────────────────────────

static void RenderHitboxGroups(const char* label, int* current_mask)
{
    ImGui::Text("%s", label);

    struct HitboxOption {
        const char* name;
        int flag;
    };
    static constexpr HitboxOption options[] = {
        { "Head", HITBOX_GROUP_HEAD },       { "Neck", HITBOX_GROUP_NECK },
        { "Chest", HITBOX_GROUP_CHEST },     { "Stomach", HITBOX_GROUP_STOMACH },
        { "Arms", HITBOX_GROUP_ARM },        { "Legs", HITBOX_GROUP_LEG },
    };

    constexpr int all_groups = HITBOX_GROUP_HEAD | HITBOX_GROUP_NECK | HITBOX_GROUP_CHEST |
                               HITBOX_GROUP_STOMACH | HITBOX_GROUP_ARM | HITBOX_GROUP_LEG;
    std::string preview;
    if (*current_mask == 0) {
        preview = "None";
    } else if ((*current_mask & all_groups) == all_groups) {
        preview = "All groups";
    } else {
        for (const HitboxOption& option : options) {
            if ((*current_mask & option.flag) == 0) continue;
            if (!preview.empty()) preview += ", ";
            preview += option.name;
        }
    }

    ImGui::PushID(label);
    ImGui::SetNextItemWidth(-1.0f);
    if (BeginPaddedCombo("##target_hitbox_groups", preview.c_str(), ImGuiComboFlags_HeightLarge)) {
        for (const HitboxOption& option : options) {
            const bool selected = (*current_mask & option.flag) != 0;
            if (ImGui::Selectable(option.name, selected, ImGuiSelectableFlags_DontClosePopups)) {
                *current_mask ^= option.flag;
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();

                const float check_size = ImGui::GetFontSize() * 0.70f;
                const ImVec2 item_min = ImGui::GetItemRectMin();
                const ImVec2 item_max = ImGui::GetItemRectMax();
                const ImVec2 check_pos(std::round(item_max.x - check_size - ImGui::GetStyle().FramePadding.x),
                                       std::round(item_min.y + (item_max.y - item_min.y - check_size) * 0.5f));
                ImGui::RenderCheckMark(ImGui::GetWindowDrawList(), check_pos,
                                       ImGui::GetColorU32(MenuTheme::accent_primary), check_size);
            }
        }

        if (ImGui::Button("Select all")) {
            *current_mask = all_groups;
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear")) {
            *current_mask = 0;
        }
        EndPaddedCombo();
    }
    ImGui::PopID();
}

static bool RenderWeaponCategoryCombo(const char* label, int* current_index, const char* const labels[],
                                      const char* const icons[], int item_count, const SvgCache& svg_cache)
{
    if (!current_index || item_count <= 0) return false;
    if (*current_index < 0 || *current_index >= item_count) *current_index = 0;

    const float scale = ImGui::GetFontSize() / 13.0f;
    ImGui::TextColored(MenuTheme::text_secondary, "%s", label);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 2.0f * scale);

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f * scale, 6.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 6.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 9.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, 9.0f * scale);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, MenuTheme::bg_input);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, MenuTheme::bg_card_hover);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));

    ImGui::SetNextItemWidth(-1.0f);
    const ImVec2 combo_pos = ImGui::GetCursorScreenPos();
    const float combo_height = ImGui::GetFrameHeight();
    bool value_changed = false;
    if (BeginPaddedCombo("##aim_weapon_category", labels[*current_index])) {
        for (int i = 0; i < item_count; ++i) {
            const bool selected = *current_index == i;
            if (ImGui::Selectable(labels[i], selected)) {
                *current_index = i;
                value_changed = true;
            }

            const ImVec2 item_min = ImGui::GetItemRectMin();
            const ImVec2 item_max = ImGui::GetItemRectMax();
            const SvgTexture texture = svg_cache.get_weapon_texture(icons[i]);
            if (texture.id != 0 && texture.height > 0.0f) {
                const float aspect = texture.width / texture.height;
                float icon_h = 14.0f * scale;
                float icon_w = icon_h * aspect;
                if (icon_w > 36.0f * scale) {
                    icon_w = 36.0f * scale;
                    icon_h = icon_w / aspect;
                }

                const float icon_x =
                    std::round(item_min.x + ImGui::CalcTextSize(labels[i]).x + 12.0f * scale);
                const float icon_y = std::round(item_min.y + (item_max.y - item_min.y - icon_h) * 0.5f);
                ImGui::GetWindowDrawList()->AddImage(
                    reinterpret_cast<ImTextureID>(static_cast<intptr_t>(texture.id)),
                    ImVec2(icon_x, icon_y), ImVec2(icon_x + icon_w, icon_y + icon_h),
                    ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), ImGui::GetColorU32(MenuTheme::text_primary));
            }

            if (selected) ImGui::SetItemDefaultFocus();
        }
        EndPaddedCombo();
    }

    const SvgTexture selected_texture = svg_cache.get_weapon_texture(icons[*current_index]);
    if (selected_texture.id != 0 && selected_texture.height > 0.0f) {
        const float aspect = selected_texture.width / selected_texture.height;
        float icon_h = 14.0f * scale;
        float icon_w = icon_h * aspect;
        if (icon_w > 36.0f * scale) {
            icon_w = 36.0f * scale;
            icon_h = icon_w / aspect;
        }

        const float icon_x =
            std::round(combo_pos.x + 10.0f * scale + ImGui::CalcTextSize(labels[*current_index]).x + 8.0f * scale);
        const float icon_y = std::round(combo_pos.y + (combo_height - icon_h) * 0.5f);
        ImGui::GetWindowDrawList()->AddImage(
            reinterpret_cast<ImTextureID>(static_cast<intptr_t>(selected_texture.id)),
            ImVec2(icon_x, icon_y), ImVec2(icon_x + icon_w, icon_y + icon_h),
            ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), ImGui::GetColorU32(MenuTheme::text_primary));
    }

    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(4);
    MenuTooltip::show_item("Selects the weapon category for aim and trigger settings.");
    return value_changed;
}

// Legit-aimbot cards.
void render_combat_tab(OverlayConfig& cfg, const SvgCache& svg_cache)
{
    static int current_cat = 0;
    if (current_cat > 8) current_cat = 0;

    const char* weapon_svgs[] = {
        "usp_silencer", "deagle", "ak47", "mp9", "ssg08",
        "awp", "scar20", "nova", "negev"
    };
    const char* weapon_labels[] = {
        "Light Pistols", "Heavy Pistols", "Rifles", "SMGs", "Scout",
        "AWP", "Auto Snipers", "Shotguns", "LMGs"
    };

    // The weapon category picker drives the per-weapon legitbot/triggerbot
    // settings (hitchance, hitboxes), so it only shows while one of those
    // pipelines is enabled.
    const bool show_weapon_category = cfg.aim_enabled || cfg.trigger_enabled;
    if (BeginAnimatedCard("##anim_weapon_category", show_weapon_category)) {
        RenderWeaponCategoryCombo("Select Weapon Category", &current_cat, weapon_labels, weapon_svgs, 9, svg_cache);
        ImGui::Spacing();
        EndAnimatedCard();
    }

    // Map active category variables
    float* active_hitchance = nullptr;
    float* active_noscope_hitchance = nullptr;
    float* active_hitbox_scale = nullptr;
    int* active_hitbox_groups = nullptr;
    float* active_aim_fov = nullptr;
    float* active_aim_smooth = nullptr;
    bool* active_aim_rcs = nullptr;
    float* active_aim_rcs_scale_x = nullptr;
    float* active_aim_rcs_scale_y = nullptr;
    float* active_aim_rcs_smooth = nullptr;

    switch (current_cat) {
        case 0:
            active_hitchance = &cfg.trigger_light_pistols_hitchance;
            active_hitbox_scale = &cfg.trigger_light_pistols_hitbox_scale;
            active_hitbox_groups = &cfg.trigger_light_pistols_hitbox_groups;
            active_aim_fov = &cfg.aim_light_pistols_fov;
            active_aim_smooth = &cfg.aim_light_pistols_smooth;
            active_aim_rcs = &cfg.aim_light_pistols_rcs;
            active_aim_rcs_scale_x = &cfg.aim_light_pistols_rcs_scale_x;
            active_aim_rcs_scale_y = &cfg.aim_light_pistols_rcs_scale_y;
            active_aim_rcs_smooth = &cfg.aim_light_pistols_rcs_smooth;
            break;
        case 1:
            active_hitchance = &cfg.trigger_heavy_pistols_hitchance;
            active_hitbox_scale = &cfg.trigger_heavy_pistols_hitbox_scale;
            active_hitbox_groups = &cfg.trigger_heavy_pistols_hitbox_groups;
            active_aim_fov = &cfg.aim_heavy_pistols_fov;
            active_aim_smooth = &cfg.aim_heavy_pistols_smooth;
            active_aim_rcs = &cfg.aim_heavy_pistols_rcs;
            active_aim_rcs_scale_x = &cfg.aim_heavy_pistols_rcs_scale_x;
            active_aim_rcs_scale_y = &cfg.aim_heavy_pistols_rcs_scale_y;
            active_aim_rcs_smooth = &cfg.aim_heavy_pistols_rcs_smooth;
            break;
        case 2:
            active_hitchance = &cfg.trigger_rifles_hitchance;
            active_hitbox_scale = &cfg.trigger_rifles_hitbox_scale;
            active_hitbox_groups = &cfg.trigger_rifles_hitbox_groups;
            active_aim_fov = &cfg.aim_rifles_fov;
            active_aim_smooth = &cfg.aim_rifles_smooth;
            active_aim_rcs = &cfg.aim_rifles_rcs;
            active_aim_rcs_scale_x = &cfg.aim_rifles_rcs_scale_x;
            active_aim_rcs_scale_y = &cfg.aim_rifles_rcs_scale_y;
            active_aim_rcs_smooth = &cfg.aim_rifles_rcs_smooth;
            break;
        case 3:
            active_hitchance = &cfg.trigger_smgs_hitchance;
            active_hitbox_scale = &cfg.trigger_smgs_hitbox_scale;
            active_hitbox_groups = &cfg.trigger_smgs_hitbox_groups;
            active_aim_fov = &cfg.aim_smgs_fov;
            active_aim_smooth = &cfg.aim_smgs_smooth;
            active_aim_rcs = &cfg.aim_smgs_rcs;
            active_aim_rcs_scale_x = &cfg.aim_smgs_rcs_scale_x;
            active_aim_rcs_scale_y = &cfg.aim_smgs_rcs_scale_y;
            active_aim_rcs_smooth = &cfg.aim_smgs_rcs_smooth;
            break;
        case 4:
            active_hitchance = &cfg.trigger_scout_hitchance;
            active_noscope_hitchance = &cfg.trigger_scout_noscope_hitchance;
            active_hitbox_scale = &cfg.trigger_scout_hitbox_scale;
            active_hitbox_groups = &cfg.trigger_scout_hitbox_groups;
            active_aim_fov = &cfg.aim_scout_fov;
            active_aim_smooth = &cfg.aim_scout_smooth;
            active_aim_rcs = &cfg.aim_scout_rcs;
            active_aim_rcs_scale_x = &cfg.aim_scout_rcs_scale_x;
            active_aim_rcs_scale_y = &cfg.aim_scout_rcs_scale_y;
            active_aim_rcs_smooth = &cfg.aim_scout_rcs_smooth;
            break;
        case 5:
            active_hitchance = &cfg.trigger_awp_hitchance;
            active_noscope_hitchance = &cfg.trigger_awp_noscope_hitchance;
            active_hitbox_scale = &cfg.trigger_awp_hitbox_scale;
            active_hitbox_groups = &cfg.trigger_awp_hitbox_groups;
            active_aim_fov = &cfg.aim_awp_fov;
            active_aim_smooth = &cfg.aim_awp_smooth;
            active_aim_rcs = &cfg.aim_awp_rcs;
            active_aim_rcs_scale_x = &cfg.aim_awp_rcs_scale_x;
            active_aim_rcs_scale_y = &cfg.aim_awp_rcs_scale_y;
            active_aim_rcs_smooth = &cfg.aim_awp_rcs_smooth;
            break;
        case 6:
            active_hitchance = &cfg.trigger_auto_snipers_hitchance;
            active_noscope_hitchance = &cfg.trigger_auto_snipers_noscope_hitchance;
            active_hitbox_scale = &cfg.trigger_auto_snipers_hitbox_scale;
            active_hitbox_groups = &cfg.trigger_auto_snipers_hitbox_groups;
            active_aim_fov = &cfg.aim_auto_snipers_fov;
            active_aim_smooth = &cfg.aim_auto_snipers_smooth;
            active_aim_rcs = &cfg.aim_auto_snipers_rcs;
            active_aim_rcs_scale_x = &cfg.aim_auto_snipers_rcs_scale_x;
            active_aim_rcs_scale_y = &cfg.aim_auto_snipers_rcs_scale_y;
            active_aim_rcs_smooth = &cfg.aim_auto_snipers_rcs_smooth;
            break;
        case 7:
            active_hitchance = &cfg.trigger_shotguns_hitchance;
            active_hitbox_scale = &cfg.trigger_shotguns_hitbox_scale;
            active_hitbox_groups = &cfg.trigger_shotguns_hitbox_groups;
            active_aim_fov = &cfg.aim_shotguns_fov;
            active_aim_smooth = &cfg.aim_shotguns_smooth;
            active_aim_rcs = &cfg.aim_shotguns_rcs;
            active_aim_rcs_scale_x = &cfg.aim_shotguns_rcs_scale_x;
            active_aim_rcs_scale_y = &cfg.aim_shotguns_rcs_scale_y;
            active_aim_rcs_smooth = &cfg.aim_shotguns_rcs_smooth;
            break;
        case 8:
            active_hitchance = &cfg.trigger_lmgs_hitchance;
            active_hitbox_scale = &cfg.trigger_lmgs_hitbox_scale;
            active_hitbox_groups = &cfg.trigger_lmgs_hitbox_groups;
            active_aim_fov = &cfg.aim_lmgs_fov;
            active_aim_smooth = &cfg.aim_lmgs_smooth;
            active_aim_rcs = &cfg.aim_lmgs_rcs;
            active_aim_rcs_scale_x = &cfg.aim_lmgs_rcs_scale_x;
            active_aim_rcs_scale_y = &cfg.aim_lmgs_rcs_scale_y;
            active_aim_rcs_smooth = &cfg.aim_lmgs_rcs_smooth;
            break;
        case 9:
            active_hitchance = &cfg.trigger_others_hitchance;
            active_hitbox_scale = &cfg.trigger_others_hitbox_scale;
            active_hitbox_groups = &cfg.trigger_others_hitbox_groups;
            active_aim_fov = &cfg.aim_others_fov;
            active_aim_smooth = &cfg.aim_others_smooth;
            active_aim_rcs = &cfg.aim_others_rcs;
            active_aim_rcs_scale_x = &cfg.aim_others_rcs_scale_x;
            active_aim_rcs_scale_y = &cfg.aim_others_rcs_scale_y;
            active_aim_rcs_smooth = &cfg.aim_others_rcs_smooth;
            break;
    }

    BeginTwoColumnLayout("##combat_2col");
    
    // COLUMN 1: General & Legitbot
    BeginSection("General", "##sec_general");
    RenderToggle("Enable Legitbot", "##aim_enabled", &cfg.aim_enabled);
    MenuTooltip::show_item("Enables the per-weapon legit aimbot.");

    RenderToggleTip("Enable Triggerbot", "##trigger_enabled", &cfg.trigger_enabled,
                    "Enables automatic firing when a target meets the trigger conditions.");
    
    if (cfg.trigger_enabled) {
        RenderToggleTip("Magnet Trigger", "##trigger_magnet", &cfg.trigger_magnet,
                        "Allows the triggerbot to assist aim toward a valid target.");
    }
    
    if (cfg.aim_enabled || (cfg.trigger_enabled && cfg.trigger_magnet)) {
        RenderKeybindConfig("Aim Key", "aim_active", 1001);
    }
    if (cfg.trigger_enabled) {
        RenderKeybindConfig("Trigger Key", "trigger_active", 1004);
    }

    if (cfg.aim_enabled || cfg.trigger_enabled) {
        RenderToggleTip("Smoke Check", "##smoke_check", &cfg.smoke_check,
                        "Ignores targets hidden by smoke when selecting targets.");
        RenderToggleTip("Visibility Check", "##vischeck", &cfg.vischeck,
                        "Checks line of sight before selecting targets.");

    }
    EndSection();

    const bool show_legit_aim = cfg.aim_enabled || (cfg.trigger_magnet && cfg.trigger_enabled);
    if (BeginAnimatedCard("##anim_legit_aim", show_legit_aim)) {
        BeginSection("Legit Aimbot", "##sec_legit_aim");
        if (active_aim_fov) {
            RenderSliderFloat("FOV", "##aim_fov", active_aim_fov, 0.1f, 25.0f, "%.1f deg");
            MenuTooltip::show_item("Sets the maximum aim angle for this weapon category.");
        }
        if (active_aim_smooth) {
            RenderSliderFloat("Smooth", "##aim_smooth", active_aim_smooth, 1.0f, 100.0f, "%.1f");
            MenuTooltip::show_item("Sets how gradually aim moves toward a target.");
        }
        const char* smooth_types[] = { "Constant", "Slow-End", "Fast-End", "Progressive (Hybrid)" };
        RenderCombo("Smooth Type", "##aim_smooth_type", &cfg.aim_smooth_type, smooth_types, IM_ARRAYSIZE(smooth_types));
        MenuTooltip::show_item("Selects the smoothing curve used for aim movement.");

        RenderToggleTip("Randomize Smooth", "##aim_random_smooth", &cfg.aim_random_smooth,
                        "Adds random variation to aim smoothing.");
        if (cfg.aim_random_smooth) {
            RenderSliderFloat("Randomize Smooth X", "##aim_randomize_smooth_x", &cfg.aim_randomize_smooth_x, 0.0f, 10.0f, "%.1f");
            MenuTooltip::show_item("Sets horizontal smoothing variation.");
            RenderSliderFloat("Randomize Smooth Y", "##aim_randomize_smooth_y", &cfg.aim_randomize_smooth_y, 0.0f, 10.0f, "%.1f");
            MenuTooltip::show_item("Sets vertical smoothing variation.");
        }
        RenderSliderFloat("Horizontal Bias", "##aim_horizontal_bias", &cfg.aim_horizontal_bias, 0.5f, 2.0f, "%.2f");
        MenuTooltip::show_item("Biases aim movement toward horizontal or vertical correction.");
        RenderToggleTip("Prediction", "##aim_prediction", &cfg.aim_prediction,
                        "Predicts target movement when aiming.");
        
        RenderToggleTip("Draw FOV Circle", "##aim_fov_circle", &cfg.aim_fov_circle,
                        "Draws the active aim FOV around the crosshair.");
        if (cfg.aim_fov_circle) {
            RenderColorButton("Circle Color", "##AimFovCol_btn", "##AimFovCol_pick", cfg.aim_fov_circle_color);
            MenuTooltip::show_item("Sets the aim FOV circle color.");
        }
        RenderToggleTip("Overshoot humanization", "##aim_overshoot", &cfg.aim_overshoot,
                        "Adds occasional overshoot to aim movement.");
        if (cfg.aim_overshoot) {
            RenderSliderFloat("Overshoot Chance", "##aim_overshoot_chance", &cfg.aim_overshoot_chance, 0.0f, 1.0f, "%.2f");
            MenuTooltip::show_item("Sets the chance that aim overshoots the target.");
            RenderSliderFloat("Overshoot Scale", "##aim_overshoot_scale", &cfg.aim_overshoot_scale, 1.0f, 1.20f, "%.2f");
            MenuTooltip::show_item("Sets the size of aim overshoot.");
        }
        if (active_aim_rcs) {
            RenderToggleTip("RCS (Recoil Control)", "##aim_rcs", active_aim_rcs,
                            "Compensates for weapon recoil while aiming.");
            if (*active_aim_rcs) {
                if (active_aim_rcs_scale_x) {
                    RenderSliderFloat("RCS Scale X (Yaw)", "##aim_rcs_scale_x", active_aim_rcs_scale_x, 0.0f, 1.0f, "%.2f");
                    MenuTooltip::show_item("Sets horizontal recoil compensation strength.");
                }
                if (active_aim_rcs_scale_y) {
                    RenderSliderFloat("RCS Scale Y (Pitch)", "##aim_rcs_scale_y", active_aim_rcs_scale_y, 0.0f, 1.0f, "%.2f");
                    MenuTooltip::show_item("Sets vertical recoil compensation strength.");
                }
                if (active_aim_rcs_smooth) {
                    RenderSliderFloat("RCS Smooth", "##aim_rcs_smooth", active_aim_rcs_smooth, 1.0f, 100.0f, "%.1f");
                    MenuTooltip::show_item("Sets how gradually recoil compensation is applied.");
                }
            }
        }
        EndSection();
        EndAnimatedCard();
    }

    // COLUMN 2: Hitboxes & Trigger
    SwitchColumn();

    // Hitboxes belong to the legitbot/triggerbot pair: visible whenever either
    // is enabled.
    const bool show_hitboxes = cfg.aim_enabled || cfg.trigger_enabled;
    if (BeginAnimatedCard("##anim_hitboxes", show_hitboxes)) {
        BeginSection("Hitboxes", "##sec_hitboxes");
        if (active_hitbox_groups) {
            RenderHitboxGroups("Target Hitbox Groups", active_hitbox_groups);
        }
        if (active_hitbox_scale) {
            RenderSliderFloat("Hitbox Scale", "##aim_hitbox_scale_cur", active_hitbox_scale, 0.5f, 1.0f, "%.2f");
            MenuTooltip::show_item("Scales the selected hitboxes for targeting.");
        }
        RenderToggleTip("Show Hitboxes", "##trigger_show_hitboxes", &cfg.trigger_show_hitboxes,
                        "Draws the selected target hitboxes.");
        EndSection();
        EndAnimatedCard();
    }

    if (BeginAnimatedCard("##anim_triggerbot", cfg.trigger_enabled)) {
        BeginSection("Triggerbot", "##sec_trigger_bot_merged");
        RenderToggleTip("Fire On Target Lost", "##trigger_shoot_lost", &cfg.trigger_shoot_on_target_lost,
                        "Allows a shot to continue when the target is briefly lost.");
        RenderToggleTip("Debug Logging", "##trigger_debug", &cfg.trigger_debug,
                        "Logs triggerbot decisions for debugging.");
        


        ImGui::Spacing();
        
        // Trigger Mode combo
        const char* trigger_modes[] = {"Simple", "Seeded", "Hitchance", "Both"};
        ImGui::Text("Trigger Mode");
        ImGui::SetNextItemWidth(-1.0f);
        RenderPaddedCombo("##trigger_mode", &cfg.trigger_mode, trigger_modes, IM_ARRAYSIZE(trigger_modes));
        MenuTooltip::show_item("Selects the triggerbot firing and hitchance mode.");

        // Validation Mode combo
        const char* validation_modes[] = {"Crosshair", "Crosshair + Hitbox", "Hitbox (Auto-fire)"};
        ImGui::Text("Validation Mode");
        ImGui::SetNextItemWidth(-1.0f);
        RenderPaddedCombo("##trigger_val_mode", &cfg.trigger_validation_mode, validation_modes,
                          IM_ARRAYSIZE(validation_modes));
        MenuTooltip::show_item("Selects how triggerbot targets are validated.");
        
        // Hitchance settings
        bool show_hitchance = (cfg.trigger_mode == 2 || cfg.trigger_mode == 3);
        if (show_hitchance && active_hitchance) {
            RenderSliderFloat("Hitchance %", "##trigger_hc_cur", active_hitchance, 0.0f, 100.0f, "%.1f %%");
            MenuTooltip::show_item("Sets the minimum seeded shot hitchance for this weapon category.");
            if (active_noscope_hitchance) {
                RenderSliderFloat("Noscope Hitchance %", "##trigger_ns_hc_cur", active_noscope_hitchance, 0.0f, 100.0f, "%.1f %%");
                MenuTooltip::show_item("Sets the minimum noscope hitchance for this weapon category.");
            }
        }
        
        ImGui::Spacing();
        
        // Delays
        RenderSliderInt("Delay Min (ms)", "##trigger_delay_min", &cfg.trigger_delay_min, 0, 500, "%d ms");
        MenuTooltip::show_item("Sets the minimum delay before triggerbot firing.");
        RenderSliderInt("Delay Max (ms)", "##trigger_delay_max", &cfg.trigger_delay_max, 0, 500, "%d ms");
        MenuTooltip::show_item("Sets the maximum delay before triggerbot firing.");
        RenderSliderInt("Delay Between Shots (ms)", "##trigger_delay_between_shots", &cfg.trigger_delay_between_shots, 0, 1000, "%d ms");
        MenuTooltip::show_item("Sets the minimum delay between triggerbot shots.");
        RenderSliderFloat("Bucket Guard (deg)", "##trigger_bucket_guard", &cfg.trigger_bucket_guard_degrees, 0.0f, 0.249f, "%.4f");
        MenuTooltip::show_item("Sets the angular guard used for trigger validation.");
        RenderToggleTip("Predict Origin (vel)", "##trigger_predict_origin", &cfg.trigger_predict_origin,
                        "Predicts target origin from velocity during validation.");
        EndSection();
        EndAnimatedCard();
    }

    EndTwoColumnLayout();
}

static void render_section_auto_accept(OverlayConfig& cfg)
{
    BeginSection("Auto Accept", "##sec_auto_accept");
    RenderToggleTip("Show Target", "##auto_accept_show_target", &cfg.auto_accept_show_target,
                    "Shows the auto-accept cursor target.");
    RenderSliderFloat("Target X", "##auto_accept_target_x", &cfg.auto_accept_target_x_percent, 0.0f, 100.0f, "%.1f%%");
    MenuTooltip::show_item("Sets the horizontal target position as a screen percentage.");
    RenderSliderFloat("Target Y", "##auto_accept_target_y", &cfg.auto_accept_target_y_percent, 0.0f, 100.0f, "%.1f%%");
    MenuTooltip::show_item("Sets the vertical target position as a screen percentage.");
    RenderSliderFloat("Accept Delay (ms)", "##auto_accept_delay", &cfg.auto_accept_delay_ms, 0.0f, 15000.0f,
                      "%.0f ms");
    MenuTooltip::show_item("Sets the delay before accepting a prompt.");
    RenderSliderFloat("Mouse Smooth", "##auto_accept_smooth", &cfg.auto_accept_smooth, 1.0f, 50.0f, "%.1f");
    MenuTooltip::show_item("Sets the smoothness of cursor movement to the target.");
    ImGui::Spacing();
    RenderToggleTip("Progressive Smooth", "##auto_accept_progressive_smooth", &cfg.auto_accept_progressive_smooth,
                    "Varies cursor smoothing during the movement.");
    RenderToggleTip("Random Smooth", "##auto_accept_random_smooth", &cfg.auto_accept_random_smooth,
                    "Adds random variation to cursor smoothing.");
    if (cfg.auto_accept_random_smooth) {
        RenderSliderFloat("Randomize X", "##auto_accept_rand_x", &cfg.auto_accept_randomize_smooth_x, 0.0f, 10.0f, "%.2f");
        MenuTooltip::show_item("Sets horizontal cursor smoothing variation.");
        RenderSliderFloat("Randomize Y", "##auto_accept_rand_y", &cfg.auto_accept_randomize_smooth_y, 0.0f, 10.0f, "%.2f");
        MenuTooltip::show_item("Sets vertical cursor smoothing variation.");
    }
    RenderToggleTip("Overshoot", "##auto_accept_overshoot", &cfg.auto_accept_overshoot,
                    "Adds occasional cursor overshoot before settling on the target.");
    if (cfg.auto_accept_overshoot) {
        RenderSliderFloat("Overshoot Chance", "##auto_accept_overshoot_chance", &cfg.auto_accept_overshoot_chance, 0.0f, 1.0f, "%.2f");
        MenuTooltip::show_item("Sets the chance of cursor overshoot.");
        RenderSliderFloat("Overshoot Scale", "##auto_accept_overshoot_scale", &cfg.auto_accept_overshoot_scale, 1.0f, 2.0f, "%.2f");
        MenuTooltip::show_item("Sets the amount of cursor overshoot.");
    }
    EndSection();
}

static void render_section_edge_jump(OverlayConfig& cfg)
{
    BeginSection("Edge Jump", "##sec_edge_jump");
    RenderKeybindConfig("Activation Key", "edgebug_activation_key", 1004);
    ImGui::Spacing();
    RenderToggleTip("Crouch on Jump", "##ej_crouch_on_jump", &cfg.ej_crouch_on_jump,
                    "Crouches when the edge jump is triggered.");
    RenderToggleTip("Knife Only", "##ej_knife_only", &cfg.ej_knife_only,
                    "Limits edge jump activation to knife-held movement.");
    RenderToggleTip("Strafe Assist", "##ej_strafe_assist", &cfg.ej_strafe_assist,
                    "Adds automatic strafing during an edge jump.");
    RenderToggleTip("Debug Logging", "##ej_debug", &cfg.ej_debug,
                    "Logs edge jump detection and activation.");
    RenderSliderFloat("Check Distance", "##ej_check_distance", &cfg.ej_check_distance, 5.0f, 50.0f, "%.1f units");
    MenuTooltip::show_item("Sets how far ahead edge detection checks.");
    RenderSliderFloat("Ground Check Height", "##ej_ground_check_height", &cfg.ej_ground_check_height, 10.0f, 150.0f, "%.1f units");
    MenuTooltip::show_item("Sets the height used for ground detection.");
    RenderSliderFloat("Ground Tolerance", "##ej_ground_tolerance", &cfg.ej_ground_tolerance, 5.0f, 100.0f, "%.1f units");
    MenuTooltip::show_item("Sets the vertical tolerance for ground detection.");
    RenderSliderInt("Jump Delay (ms)", "##ej_jump_delay_ms", &cfg.ej_jump_delay_ms, 0, 100, "%d ms");
    MenuTooltip::show_item("Sets the delay before the jump input is sent.");
    RenderSliderFloat("Min Movement Speed", "##ej_min_movement_speed", &cfg.ej_min_movement_speed, 50.0f, 250.0f, "%.1f units/s");
    MenuTooltip::show_item("Sets the minimum speed required for edge jump activation.");
    EndSection();
}

static void render_section_kb_viewer(OverlayConfig& cfg)
{
    BeginSection("Keybinds Viewer", "##sec_misc_kb_viewer");
    RenderSliderFloat("Position X", "##kb_viewer_x", &cfg.kb_viewer_x, 0.0f, 2560.0f, "%.0f px");
    MenuTooltip::show_item("Sets the keybind viewer horizontal position.");
    RenderSliderFloat("Position Y", "##kb_viewer_y", &cfg.kb_viewer_y, 0.0f, 1440.0f, "%.0f px");
    MenuTooltip::show_item("Sets the keybind viewer vertical position.");
    RenderSliderFloat("Size", "##kb_viewer_scale", &cfg.kb_viewer_scale, 0.75f, 2.0f, "%.2fx");
    MenuTooltip::show_item("Sets the keybind viewer size.");
    EndSection();
}

static void render_section_spec_list(OverlayConfig& cfg)
{
    BeginSection("Spectator List", "##sec_misc_spec_list");
    RenderSliderFloat("Position X", "##spec_list_x", &cfg.spec_list_x, 0.0f, 2560.0f, "%.0f px");
    MenuTooltip::show_item("Sets the spectator list horizontal position.");
    RenderSliderFloat("Position Y", "##spec_list_y", &cfg.spec_list_y, 0.0f, 1440.0f, "%.0f px");
    MenuTooltip::show_item("Sets the spectator list vertical position.");
    RenderSliderFloat("Size", "##spec_list_scale", &cfg.spec_list_scale, 0.75f, 2.0f, "%.2fx");
    MenuTooltip::show_item("Sets the spectator list size.");
    EndSection();
}

static void render_section_cstrafe(OverlayConfig& cfg)
{
    BeginSection("Counter-Strafe Coach", "##sec_misc_cstrafe");
    RenderToggleTip("Show Speed Graph", "##cstrafe_show_graph", &cfg.cstrafe_show_graph,
                    "Shows the speed graph in the counter-strafe coach.");
    RenderSliderFloat("Position X", "##cstrafe_x", &cfg.cstrafe_x, 0.0f, 2560.0f, "%.0f px");
    MenuTooltip::show_item("Sets the counter-strafe coach horizontal position.");
    RenderSliderFloat("Position Y", "##cstrafe_y", &cfg.cstrafe_y, 0.0f, 1440.0f, "%.0f px");
    MenuTooltip::show_item("Sets the counter-strafe coach vertical position.");
    RenderSliderFloat("Size", "##cstrafe_scale", &cfg.cstrafe_scale, 0.75f, 2.0f, "%.2fx");
    MenuTooltip::show_item("Sets the counter-strafe coach size.");
    EndSection();
}

static void render_section_keybinds()
{
    BeginSection("Keybinds", "##sec_misc_keybinds");
    RenderKeybindConfig("Left Mouse", "key_left_mouse", 1000);
    RenderKeybindConfig("Right Mouse", "key_right_mouse", 1001);
    RenderKeybindConfig("Middle Mouse", "key_middle_mouse", 1002);
    RenderKeybindConfig("Mouse XBUTTON1", "key_mouse_xbutton1", 1003);
    RenderKeybindConfig("Mouse XBUTTON2", "key_mouse_xbutton2", 1004);
    RenderKeybindConfig("W Key", "key_w", 87);
    RenderKeybindConfig("A Key", "key_a", 65);
    RenderKeybindConfig("S Key", "key_s", 83);
    RenderKeybindConfig("D Key", "key_d", 68);
    RenderKeybindConfig("Jump Key (Space)", "key_jump", 32);
    RenderKeybindConfig("Crouch Key (LCTRL)", "key_crouch", 341);
    RenderKeybindConfig("Shift Key (LSHIFT)", "key_shift", 340);
    EndSection();
}

// Master toggles for the Misc tab. Feature settings live in their own cards,
// revealed when the matching toggle is on. Team Check is a global setting with
// no settings of its own, so it simply sits here.
static void render_section_misc_features(OverlayConfig& cfg)
{
    BeginSection("Misc Features", "##sec_misc_features");
    RenderToggleTip("Auto Accept", "##auto_accept_enabled", &cfg.auto_accept_enabled,
                    "Automatically accepts match and ready prompts.");
    RenderToggleTip("Edge Jump", "##ej_enabled", &cfg.ej_enabled,
                    "Automatically jumps when moving off an edge.");
    RenderToggleTip("Keybinds Viewer", "##kb_viewer_enabled", &cfg.kb_viewer_enabled,
                    "Shows the configured keybinds on the overlay.");
    RenderToggleTip("Spectator List", "##spec_list_enabled", &cfg.spec_list_enabled,
                    "Shows the spectator list on the overlay.");
    RenderToggleTip("Counter-Strafe Coach", "##cstrafe_enabled", &cfg.cstrafe_enabled,
                    "Enables the counter-strafe coaching overlay.");
    RenderToggleTip("Team Check", "##team_check", &cfg.team_check,
                    "Prevents target selection and ESP actions against teammates.");
    EndSection();
}

void render_misc_tab(OverlayConfig& cfg)
{
    BeginTwoColumnLayout("##misc_2col");

    // Left column: master toggles, then the revealed settings cards
    render_section_misc_features(cfg);
    if (BeginAnimatedCard("##anim_auto_accept", cfg.auto_accept_enabled)) {
        render_section_auto_accept(cfg);
        EndAnimatedCard();
    }
    if (BeginAnimatedCard("##anim_edge_jump", cfg.ej_enabled)) {
        render_section_edge_jump(cfg);
        EndAnimatedCard();
    }
    if (BeginAnimatedCard("##anim_kb_viewer", cfg.kb_viewer_enabled)) {
        render_section_kb_viewer(cfg);
        EndAnimatedCard();
    }
    if (BeginAnimatedCard("##anim_spec_list", cfg.spec_list_enabled)) {
        render_section_spec_list(cfg);
        EndAnimatedCard();
    }
    if (BeginAnimatedCard("##anim_cstrafe", cfg.cstrafe_enabled)) {
        render_section_cstrafe(cfg);
        EndAnimatedCard();
    }

    // Right column: Keybinds stay visible at all times
    SwitchColumn();
    render_section_keybinds();

    EndTwoColumnLayout();
}

static std::vector<ConfigProfileInfo> g_configs;
static bool g_configs_loaded = false;
static std::string g_selected_config_name;
static std::string g_pending_load_name;
static std::string g_pending_delete_name;
static std::string g_config_status;
static char g_new_config_name[65] = "";

static void refresh_configs_list(const std::string& preferred = {})
{
    g_configs = list_config_profiles();
    g_configs_loaded = true;

    auto contains = [](const std::vector<ConfigProfileInfo>& profiles, const std::string& name) {
        return std::any_of(profiles.begin(), profiles.end(),
                           [&](const ConfigProfileInfo& profile) { return profile.name == name; });
    };

    if (!preferred.empty() && contains(g_configs, preferred)) {
        g_selected_config_name = preferred;
    } else if (contains(g_configs, g_selected_config_name)) {
        return;
    } else if (contains(g_configs, g_active_config_name)) {
        g_selected_config_name = g_active_config_name;
    } else if (contains(g_configs, get_default_config_name())) {
        g_selected_config_name = get_default_config_name();
    } else if (!g_configs.empty()) {
        g_selected_config_name = g_configs.front().name;
    } else {
        g_selected_config_name.clear();
    }
}

static bool load_selected_config(const std::string& name, OverlayConfig& cfg)
{
    OverlayConfig loaded;
    if (!try_load_config(name + ".json", loaded)) return false;

    cfg = std::move(loaded);
    cfg.mark_dirty();
    set_active_config_snapshot(name, cfg);
    if (MenuClient* menu = MenuClient::get_instance()) {
        if (OverlayClient* overlay = menu->get_overlay()) {
            overlay->update_monitor_index(cfg.monitor_index);
        }
    }
    return true;
}

static void render_config_profile_row(const ConfigProfileInfo& profile)
{
    const bool selected = profile.name == g_selected_config_name;
    const float scale = ImGui::GetFontSize() / 13.0f;
    const float row_height = 27.0f * scale;
    const ImVec2 position = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;

    ImGui::PushID(profile.name.c_str());
    if (ImGui::Selectable("##profile", false, ImGuiSelectableFlags_None, ImVec2(width, row_height))) {
        g_selected_config_name = profile.name;
        g_new_config_name[0] = '\0';
    }
    const bool hovered = ImGui::IsItemHovered();
    const ImGuiID animation_id = ImGui::GetID("##profile_animation");
    const float state =
        MenuAnimation::get().animate(animation_id, selected ? 1.0f : (hovered ? 0.4f : 0.0f), 24.0f);

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    if (state > 0.001f) {
        ImVec4 background = MenuTheme::bg_card_hover;
        background.w *= selected ? state : state / 0.4f;
        draw_list->AddRectFilled(position, ImVec2(position.x + width, position.y + row_height),
                                 ImGui::GetColorU32(background), 4.0f * scale);
    }
    if (state > 0.4f) {
        const float bar_state = std::clamp((state - 0.4f) / 0.6f, 0.0f, 1.0f);
        const float bar_height = (row_height - 5.0f * scale) * bar_state;
        const float center_y = position.y + row_height * 0.5f;
        draw_list->AddRectFilled(ImVec2(std::round(position.x), std::round(center_y - bar_height * 0.5f)),
                                 ImVec2(std::round(position.x + 3.5f * scale),
                                        std::round(center_y + bar_height * 0.5f)),
                                 ImGui::GetColorU32(MenuTheme::accent_primary), 2.0f * scale);
    }

    std::string display_name = profile.name;
    if (profile.is_default) display_name += " [default]";
    const ImVec4 text_color =
        LerpToggleColor(MenuTheme::text_secondary, MenuTheme::accent_primary, std::clamp(state, 0.0f, 1.0f));
    const float indent = 4.0f * std::clamp((state - 0.4f) / 0.6f, 0.0f, 1.0f) * scale;
    const ImVec2 text_size = ImGui::CalcTextSize(display_name.c_str());
    draw_list->AddText(ImVec2(std::round(position.x + 9.0f * scale + indent),
                              std::round(position.y + (row_height - text_size.y) * 0.5f)),
                       ImGui::GetColorU32(text_color), display_name.c_str());
    if (selected) ImGui::SetItemDefaultFocus();
    ImGui::PopID();
}

void render_config_tab(OverlayConfig& cfg)
{
    if (!g_configs_loaded) {
        refresh_configs_list();
    }

    BeginTwoColumnLayout("##config_2col");

    BeginSection("Profile Management", "##sec_configs");

    ImGui::TextColored(MenuTheme::text_secondary, "Available Configurations");
    ImGui::PushStyleColor(ImGuiCol_ChildBg, MenuTheme::bg_input);
    ImGui::PushStyleColor(ImGuiCol_Border, MenuTheme::border_subtle);
    ImGui::BeginChild("##configs_listbox", ImVec2(-1.0f, 180.0f * g_menu_preferences.menu_scale),
                      ImGuiChildFlags_Border);
    for (const ConfigProfileInfo& profile : g_configs) {
        render_config_profile_row(profile);
    }
    ImGui::EndChild();
    ImGui::PopStyleColor(2);

    ImGui::Spacing();
    ImGui::TextColored(MenuTheme::text_secondary, "Configuration Name");
    ImGui::SetNextItemWidth(-1.0f);
    const char* name_hint = g_selected_config_name.empty() ? "Enter config name..." : g_selected_config_name.c_str();
    ImGui::InputTextWithHint("##new_cfg_name", name_hint, g_new_config_name, sizeof(g_new_config_name));
    MenuTooltip::show_item("Enter a name when creating a new configuration profile.");
    ImGui::Spacing();

    const float button_height = 30.0f * g_menu_preferences.menu_scale;
    const bool has_selection = !g_selected_config_name.empty();
    if (!has_selection) ImGui::BeginDisabled();
    if (ImGui::Button("Load", ImVec2(-1.0f, button_height))) {
        if (active_config_has_unsaved_changes(cfg)) {
            g_pending_load_name = g_selected_config_name;
            ImGui::OpenPopup("Unsaved Changes");
        } else if (load_selected_config(g_selected_config_name, cfg)) {
            g_config_status = "Loaded '" + g_selected_config_name + "'";
        } else {
            g_config_status = "Failed to load '" + g_selected_config_name + "'";
        }
    }
    MenuTooltip::show_item("Loads the selected configuration profile.");
    if (!has_selection) ImGui::EndDisabled();

    ImGui::Spacing();
    if (ImGui::Button("Save", ImVec2(-1.0f, button_height))) {
        if (g_new_config_name[0] != '\0') {
            const std::string created = create_unique_config(g_new_config_name, cfg);
            if (!created.empty()) {
                set_active_config_snapshot(created, cfg);
                g_new_config_name[0] = '\0';
                refresh_configs_list(created);
                g_config_status = "Created '" + created + "'";
            } else {
                g_config_status = "Failed to create configuration; check the log for the save path";
            }
        } else if (has_selection && save_config_profile(g_selected_config_name, cfg)) {
            set_active_config_snapshot(g_selected_config_name, cfg);
            refresh_configs_list();
            g_config_status = "Saved '" + g_selected_config_name + "'";
        } else if (has_selection) {
            g_config_status = "Failed to save '" + g_selected_config_name + "'; check the log for the save path";
        } else {
            g_config_status = "Select a configuration or enter a new name";
        }
    }
    MenuTooltip::show_item("Saves the named or selected configuration profile.");

    ImGui::Spacing();
    if (!has_selection) ImGui::BeginDisabled();
    if (ImGui::Button("Set Default", ImVec2(-1.0f, button_height))) {
        if (set_default_config(g_selected_config_name)) {
            refresh_configs_list(g_selected_config_name);
            g_config_status = "'" + g_selected_config_name + "' will load on launch";
        } else {
            g_config_status = "Failed to set startup configuration";
        }
    }
    MenuTooltip::show_item("Sets the selected configuration as the startup profile.");
    if (!has_selection) ImGui::EndDisabled();

    ImGui::Spacing();
    if (!has_selection) ImGui::BeginDisabled();
    if (RenderDangerHoverButton("Delete##profile_delete", "##profile_delete",
                                ImVec2(-1.0f, button_height))) {
        g_pending_delete_name = g_selected_config_name;
        ImGui::OpenPopup("Delete Configuration");
    }
    MenuTooltip::show_item("Deletes the selected configuration profile.");
    if (!has_selection) ImGui::EndDisabled();

    if (!g_config_status.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(MenuTheme::text_secondary, "%s", g_config_status.c_str());
    }
    EndSection();

    SwitchColumn();

    BeginSection("Close Program", "##sec_close_program");
    MenuClient* menu_client = MenuClient::get_instance();
    const float unload_button_height = 30.0f * g_menu_preferences.menu_scale;
    if (RenderDangerHoverButton("Unload##close_program", "##close_program",
                                ImVec2(-1.0f, unload_button_height)) && menu_client) {
        glfwSetWindowShouldClose(menu_client->get_window(), GLFW_TRUE);
    }
    MenuTooltip::show_item("Closes the overlay application.");
    EndSection();

    BeginSection("Menu Appearance", "##sec_menu_appearance");

    const MenuPreferences previous_preferences = g_menu_preferences;
    RenderColorButton("Primary Accent Color", "##menu_accent_btn", "##menu_accent_popup",
                      g_menu_preferences.accent_color);
    MenuTooltip::show_item("Sets the menu accent color.");
    RenderColorButton("Background Color", "##menu_background_btn", "##menu_background_popup",
                      g_menu_preferences.background_color);
    MenuTooltip::show_item("Sets the menu background color.");
    RenderColorButton("Card Color", "##menu_card_btn", "##menu_card_popup", g_menu_preferences.card_color);
    MenuTooltip::show_item("Sets the menu card color.");
    RenderColorButton("Text Color", "##menu_text_btn", "##menu_text_popup", g_menu_preferences.text_color);
    MenuTooltip::show_item("Sets the menu text color.");
    RenderColorButton("Outline Color", "##menu_outline_btn", "##menu_outline_popup",
                      g_menu_preferences.outline_color);
    MenuTooltip::show_item("Sets the menu outline color.");
    RenderToggleTip("Smooth UI Animations", "##menu_animations", &g_menu_preferences.animations_enabled,
                    "Enables animated menu transitions.");
    if (g_menu_preferences.animations_enabled) {
        RenderSliderFloat("Animation Lerp Speed", "##menu_animation_speed", &g_menu_preferences.animation_speed,
                          4.0f, 30.0f, "%.1f");
        MenuTooltip::show_item("Sets the speed of menu animations.");
    }

    RenderToggleTip("Ctrl+Q Quits App", "##menu_quit_shortcut", &g_menu_preferences.quit_shortcut_enabled,
                    "Enables Ctrl+Q as an application quit shortcut.");

    const char* menu_scale_options[] = {"75%", "100%", "125%", "150%", "175%", "200%"};
    const int previous_scale_index =
        std::clamp(static_cast<int>(std::round((g_menu_preferences.menu_scale - 0.75f) / 0.25f)), 0, 5);
    int menu_scale_index = previous_scale_index;
    char menu_scale_label[64];
    std::snprintf(menu_scale_label, sizeof(menu_scale_label), "Menu Scale Factor (%d%%, Ctrl +/-, Ctrl 0 resets)",
                  static_cast<int>(std::round(g_menu_preferences.menu_scale * 100.0f)));
    RenderCombo(menu_scale_label, "##menu_scale", &menu_scale_index, menu_scale_options,
                IM_ARRAYSIZE(menu_scale_options));
    MenuTooltip::show_item("Scales the menu's fonts and control density. The window size itself is "
                           "managed by your desktop's window manager (drag the title bar or borders).");
    if (menu_scale_index != previous_scale_index) {
        g_menu_preferences.menu_scale = 0.75f + (static_cast<float>(menu_scale_index) * 0.25f);
    }

    if (ImGui::Button("Reset Window Size", ImVec2(-1.0f, 0.0f))) {
        if (MenuClient* menu = MenuClient::get_instance()) {
            menu->reset_window_size();
        }
    }
    MenuTooltip::show_item("Restores the menu window to its default size for the current scale.");

    ImGui::Spacing();
    if (ImGui::Button("Reset Menu Appearance", ImVec2(-1.0f, 0.0f))) {
        std::vector<std::string> collapsed_sections = std::move(g_menu_preferences.collapsed_sections);
        g_menu_preferences = MenuPreferences{};
        g_menu_preferences.collapsed_sections = std::move(collapsed_sections);
    }
    MenuTooltip::show_item("Restores the default menu colors, scale, and animation settings.");

    g_menu_preferences.accent_color.a = 1.0f;
    g_menu_preferences.background_color.a = 1.0f;
    g_menu_preferences.card_color.a = 1.0f;
    if (!(g_menu_preferences == previous_preferences)) {
        schedule_menu_preferences_save();
    }
    EndSection();

    EndTwoColumnLayout();

    if (ImGui::BeginPopupModal("Unsaved Changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Save changes to '%s' before loading '%s'?", g_active_config_name.c_str(),
                    g_pending_load_name.c_str());
        if (ImGui::Button("Save", ImVec2(110.0f, 0.0f))) {
            if (!save_active_config(cfg)) {
                g_config_status = "Failed to save '" + g_active_config_name + "'";
            } else if (load_selected_config(g_pending_load_name, cfg)) {
                g_config_status = "Loaded '" + g_pending_load_name + "'";
                ImGui::CloseCurrentPopup();
            } else {
                g_config_status = "Failed to load '" + g_pending_load_name + "'";
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard", ImVec2(110.0f, 0.0f))) {
            if (load_selected_config(g_pending_load_name, cfg)) {
                g_config_status = "Loaded '" + g_pending_load_name + "'";
            } else {
                g_config_status = "Failed to load '" + g_pending_load_name + "'";
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(110.0f, 0.0f))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopupModal("Delete Configuration", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Delete '%s'?", g_pending_delete_name.c_str());
        ImGui::TextDisabled("This cannot be undone.");
        if (ImGui::Button("Delete", ImVec2(120.0f, 0.0f))) {
            const bool deleting_active = g_pending_delete_name == g_active_config_name;
            if (delete_config(g_pending_delete_name)) {
                if (deleting_active) {
                    load_selected_config("default", cfg);
                    refresh_configs_list("default");
                } else {
                    refresh_configs_list(g_active_config_name);
                }
                g_config_status = "Deleted '" + g_pending_delete_name + "'";
            } else {
                g_config_status = "Failed to delete '" + g_pending_delete_name + "'";
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Radar tab — embedded FC2observ-style web radar
// ─────────────────────────────────────────────────────────────────────────────
void render_radar_tab(OverlayConfig& cfg)
{
    BeginTwoColumnLayout("##radar_2col");

    BeginSection("Web Radar", "##sec_radar");
    RenderToggleTip("Enable Radar", "##radar_enabled", &cfg.radar_enabled,
                    "Serve a live web radar (positions, bomb, smokes, projectiles) that can be opened in any "
                    "browser on the configured port.");

    if (!cfg.radar_enabled) {
        EndSection();
        EndTwoColumnLayout();
        return;
    }

    // Host text field, kept in sync with the std::string config value.
    static char host_buf[64] = "";
    static std::string host_last;
    if (host_last != cfg.radar_host) {
        std::snprintf(host_buf, sizeof(host_buf), "%s", cfg.radar_host.c_str());
        host_last = cfg.radar_host;
    }
    if (RenderInputText("Bind Host", "##radar_host", host_buf, sizeof(host_buf))) {
        cfg.radar_host = host_buf;
        host_last = cfg.radar_host;
    }
    RenderSettingHint("127.0.0.1 for local only, 0.0.0.0 to allow other devices on your network.");

    RenderInputInt("HTTP Port", "##radar_http_port", &cfg.radar_http_port);
    RenderInputInt("WebSocket Port", "##radar_ws_port", &cfg.radar_ws_port);
    cfg.radar_http_port = std::clamp(cfg.radar_http_port, 1, 65535);
    cfg.radar_ws_port = std::clamp(cfg.radar_ws_port, 1, 65535);

    RenderToggle("Start On Launch", "##radar_autostart", &cfg.radar_autostart);
    RenderToggle("Open Browser On Start", "##radar_open_browser", &cfg.radar_open_browser);
    RenderToggle("Transparent Background", "##radar_transparent", &cfg.radar_transparent);

    ImGui::Spacing();
    const float button_height = 30.0f * g_menu_preferences.menu_scale;
    radar::RadarServer* server = radar::active_server();
    const radar::ServerStatus status = server ? server->status() : radar::ServerStatus{};
    const bool running = status.running;

    if (running) {
        if (RenderDangerHoverButton("Stop Server##radar_stop", "##radar_stop",
                                    ImVec2(-1.0f, button_height))) {
            radar::request_stop();
        }
    } else {
        if (ImGui::Button("Start Server", ImVec2(-1.0f, button_height))) {
            radar::request_start();
        }
    }

    ImGui::Spacing();
    if (!running) ImGui::BeginDisabled();
    if (ImGui::Button("Open In Browser", ImVec2(-1.0f, button_height))) {
        radar::RadarServer::open_in_browser(status.http_port > 0 ? status.http_port : cfg.radar_http_port);
    }
    if (!running) ImGui::EndDisabled();

    ImGui::Spacing();
    if (running) {
        ImGui::TextColored(MenuTheme::text_primary, "Status: running");
        ImGui::TextColored(MenuTheme::text_secondary, "URL:  http://%s:%d/", cfg.radar_host.c_str(),
                           status.http_port);
        ImGui::TextColored(MenuTheme::text_secondary, "WS:   port %d", status.ws_port);
        ImGui::TextColored(MenuTheme::text_secondary, "Viewers: %d   Frames: %llu", status.clients,
                           static_cast<unsigned long long>(status.updates));
    } else {
        ImGui::TextColored(MenuTheme::text_secondary, "Status: stopped");
    }
    if (!status.last_error.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", status.last_error.c_str());
    }
    EndSection();

    SwitchColumn();

    BeginSection("Radar Appearance", "##sec_radar_appearance");
    RenderSliderFloat("Player Dot Scale", "##radar_player_dot_scale", &cfg.radar_player_dot_scale, 0.2f, 2.0f, "%.2f");
    RenderSliderFloat("Bomb Dot Scale", "##radar_bomb_dot_scale", &cfg.radar_bomb_dot_scale, 0.2f, 2.0f, "%.2f");
    RenderToggle("Show Player Names", "##radar_show_names", &cfg.radar_show_names);
    if (cfg.radar_show_names) {
        RenderSliderInt("Max Name Length", "##radar_max_name_length", &cfg.radar_max_name_length, 1, 24, "%d");
    }
    RenderToggle("Show Buy Zones", "##radar_show_buyzones", &cfg.radar_show_buyzones);
    RenderToggle("Show Projectiles", "##radar_show_projectiles", &cfg.radar_show_projectiles);
    RenderToggle("Show Smokes", "##radar_show_smokes", &cfg.radar_show_smokes);
    RenderToggle("Auto Zoom", "##radar_autozoom", &cfg.radar_autozoom);

    const char* vert_options[] = {"None", "Color", "Scale"};
    RenderCombo("Height Indicator", "##radar_vert_indicator", &cfg.radar_vert_indicator, vert_options,
                IM_ARRAYSIZE(vert_options));
    cfg.radar_vert_indicator = std::clamp(cfg.radar_vert_indicator, 0, 2);
    EndSection();

    EndTwoColumnLayout();
}
