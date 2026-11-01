#pragma once

#include <algorithm>

#include "config.hpp"
#include "external/imgui/imgui.h"

// Palette ported from menu_standalone (Material dark + warm cards).
// bg_main  = page / window / content chrome  (#141211)
// bg_card  = group-box / elevated panels      (#221f1d)
namespace MenuTheme {
inline ImVec4 bg_main       = ImVec4(0.0784f, 0.0706f, 0.0667f, 1.00f); // #141211
inline ImVec4 bg_sidebar    = ImVec4(0.0784f, 0.0706f, 0.0667f, 1.00f); // same as main
inline ImVec4 bg_card       = ImVec4(0.1333f, 0.1216f, 0.1137f, 1.00f); // #221f1d
inline ImVec4 bg_card_hover = ImVec4(0.1647f, 0.1529f, 0.1451f, 1.00f); // #2a2725
inline ImVec4 bg_input      = ImVec4(0.1647f, 0.1529f, 0.1451f, 1.00f); // #2a2725

inline ImVec4 text_primary   = ImVec4(0.925f, 0.918f, 0.902f, 1.00f); // #eceae6
inline ImVec4 text_secondary = ImVec4(0.620f, 0.604f, 0.580f, 1.00f); // #9e9a94
inline ImVec4 text_muted     = ImVec4(0.459f, 0.443f, 0.420f, 1.00f); // #75716b

// Soft off-white accent (active pills / section headers)
inline ImVec4 accent_primary = ImVec4(0.902f, 0.894f, 0.878f, 1.00f); // #e6e4e0
inline ImVec4 accent_hover   = ImVec4(0.960f, 0.953f, 0.937f, 1.00f);

// Subtle warm white border at ~8% alpha
inline ImVec4 border_subtle = ImVec4(1.000f, 0.980f, 0.950f, 0.08f);

inline float panel_rounding = 6.0f;
}

inline void apply_menu_preferences(const MenuPreferences& preferences)
{
    auto lighten = [](const Color& color, float amount) {
        return ImVec4(color.r + (1.0f - color.r) * amount, color.g + (1.0f - color.g) * amount,
                      color.b + (1.0f - color.b) * amount, 1.0f);
    };

    MenuTheme::accent_primary =
        ImVec4(preferences.accent_color.r, preferences.accent_color.g, preferences.accent_color.b, 1.0f);
    MenuTheme::accent_hover = lighten(preferences.accent_color, 0.10f);
    MenuTheme::bg_main = ImVec4(preferences.background_color.r, preferences.background_color.g,
                                preferences.background_color.b, 1.0f);
    MenuTheme::bg_sidebar = MenuTheme::bg_main;
    MenuTheme::bg_card =
        ImVec4(preferences.card_color.r, preferences.card_color.g, preferences.card_color.b, 1.0f);
    MenuTheme::bg_card_hover = lighten(preferences.card_color, 0.08f);
    MenuTheme::bg_input = lighten(preferences.card_color, 0.08f);
    MenuTheme::text_primary =
        ImVec4(preferences.text_color.r, preferences.text_color.g, preferences.text_color.b, preferences.text_color.a);
    MenuTheme::border_subtle = ImVec4(preferences.outline_color.r, preferences.outline_color.g,
                                      preferences.outline_color.b, preferences.outline_color.a);
}

inline void apply_dark_theme()
{
    using namespace MenuTheme;

    ImGuiStyle& style = ImGui::GetStyle();

    style.Alpha = 1.0f;
    style.DisabledAlpha = 0.6f;

    style.WindowRounding = 0.0f;
    style.ChildRounding = panel_rounding;
    style.FrameRounding = 5.0f;
    style.PopupRounding = panel_rounding;
    style.ScrollbarRounding = 12.0f;
    style.GrabRounding = 4.0f;
    style.TabRounding = panel_rounding;

    style.WindowPadding = ImVec2(8.0f, 8.0f);
    style.FramePadding = ImVec2(8.0f, 5.0f);
    style.ItemSpacing = ImVec2(8.0f, 6.0f);
    style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
    style.CellPadding = ImVec2(4.0f, 2.0f);
    style.IndentSpacing = 21.0f;
    style.ColumnsMinSpacing = 6.0f;
    style.ScrollbarSize = 9.0f;
    style.GrabMinSize = 10.0f;

    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.TabBorderSize = 0.0f;

    style.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    style.WindowMenuButtonPosition = ImGuiDir_Left;
    style.ColorButtonPosition = ImGuiDir_Right;
    style.ButtonTextAlign = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign = ImVec2(0.0f, 0.0f);

    ImVec4* c = style.Colors;
    c[ImGuiCol_Text] = text_primary;
    c[ImGuiCol_TextDisabled] = text_muted;
    // Page chrome stays bg_main; cards push bg_card explicitly in BeginSection.
    c[ImGuiCol_WindowBg] = bg_main;
    c[ImGuiCol_ChildBg] = bg_main;
    c[ImGuiCol_PopupBg] = bg_card;
    c[ImGuiCol_Border] = border_subtle;
    c[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_FrameBg] = bg_input;
    c[ImGuiCol_FrameBgHovered] = bg_card_hover;
    c[ImGuiCol_FrameBgActive] = bg_card_hover;
    c[ImGuiCol_TitleBg] = bg_sidebar;
    c[ImGuiCol_TitleBgActive] = bg_sidebar;
    c[ImGuiCol_TitleBgCollapsed] = bg_sidebar;
    c[ImGuiCol_MenuBarBg] = bg_sidebar;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.350f, 0.330f, 0.310f, 0.85f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.902f, 0.894f, 0.878f, 0.90f);
    c[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.960f, 0.953f, 0.937f, 1.00f);
    c[ImGuiCol_CheckMark] = accent_primary;
    c[ImGuiCol_SliderGrab] = accent_primary;
    c[ImGuiCol_SliderGrabActive] = accent_hover;
    c[ImGuiCol_Button] = bg_card;
    c[ImGuiCol_ButtonHovered] = bg_card_hover;
    c[ImGuiCol_ButtonActive] = bg_input;
    c[ImGuiCol_Header] = bg_card_hover;
    c[ImGuiCol_HeaderHovered] = bg_card_hover;
    c[ImGuiCol_HeaderActive] = bg_input;
    c[ImGuiCol_Separator] = border_subtle;
    c[ImGuiCol_SeparatorHovered] = accent_primary;
    c[ImGuiCol_SeparatorActive] = accent_hover;
    c[ImGuiCol_ResizeGrip] = bg_input;
    c[ImGuiCol_ResizeGripHovered] = accent_primary;
    c[ImGuiCol_ResizeGripActive] = accent_hover;
    c[ImGuiCol_Tab] = bg_sidebar;
    c[ImGuiCol_TabHovered] = bg_card_hover;
    c[ImGuiCol_TabActive] = bg_card;
    c[ImGuiCol_TabUnfocused] = bg_sidebar;
    c[ImGuiCol_TabUnfocusedActive] = bg_card;
    c[ImGuiCol_PlotLines] = text_secondary;
    c[ImGuiCol_PlotLinesHovered] = accent_primary;
    c[ImGuiCol_PlotHistogram] = accent_primary;
    c[ImGuiCol_PlotHistogramHovered] = accent_hover;
    c[ImGuiCol_TableHeaderBg] = bg_card;
    c[ImGuiCol_TableBorderStrong] = border_subtle;
    c[ImGuiCol_TableBorderLight] = border_subtle;
    c[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.02f);
    c[ImGuiCol_TextSelectedBg] = ImVec4(accent_primary.x, accent_primary.y, accent_primary.z, 0.35f);
    c[ImGuiCol_DragDropTarget] = accent_primary;
    c[ImGuiCol_NavHighlight] = accent_primary;
    c[ImGuiCol_NavWindowingHighlight] = ImVec4(1.0f, 1.0f, 1.0f, 0.7f);
    c[ImGuiCol_NavWindowingDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.4f);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.45f);
}
