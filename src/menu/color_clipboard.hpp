#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ColorClipboard {

inline float s_copied_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
inline bool s_has_copied_color = false;

inline void FormatHexColor(const float* color, char* buffer, size_t buffer_size)
{
    const auto channel = [](float value) {
        return static_cast<unsigned int>(std::round(std::clamp(value, 0.0f, 1.0f) * 255.0f));
    };
    std::snprintf(buffer, buffer_size, "%02X%02X%02X%02X", channel(color[0]), channel(color[1]),
                  channel(color[2]), channel(color[3]));
}

inline bool ApplyHexColor(const char* text, float* color)
{
    const size_t length = std::strlen(text);
    if (length != 6 && length != 8) return false;

    char* end = nullptr;
    const unsigned long value = std::strtoul(text, &end, 16);
    if (!end || *end != '\0') return false;

    if (length == 6) {
        color[0] = static_cast<float>((value >> 16) & 0xffUL) / 255.0f;
        color[1] = static_cast<float>((value >> 8) & 0xffUL) / 255.0f;
        color[2] = static_cast<float>(value & 0xffUL) / 255.0f;
    } else {
        color[0] = static_cast<float>((value >> 24) & 0xffUL) / 255.0f;
        color[1] = static_cast<float>((value >> 16) & 0xffUL) / 255.0f;
        color[2] = static_cast<float>((value >> 8) & 0xffUL) / 255.0f;
        color[3] = static_cast<float>(value & 0xffUL) / 255.0f;
    }
    return true;
}

inline bool ParseColorString(const char* str, float* out_color)
{
    if (!str) return false;
    while (*str == ' ' || *str == '\t' || *str == '\r' || *str == '\n') str++;
    if (*str == '#') str++;
    return ApplyHexColor(str, out_color);
}

} // namespace ColorClipboard

#if __has_include("external/imgui/imgui.h") || defined(IMGUI_VERSION)
#include "external/imgui/imgui.h"

namespace ColorClipboard {

inline void CopyColor(const float* color)
{
    s_copied_color[0] = color[0];
    s_copied_color[1] = color[1];
    s_copied_color[2] = color[2];
    s_copied_color[3] = color[3];
    s_has_copied_color = true;

    char hex_buf[12];
    FormatHexColor(color, hex_buf, sizeof(hex_buf));
    ImGui::SetClipboardText(hex_buf);
}

inline bool PasteColor(float* out_color)
{
    const char* clip = ImGui::GetClipboardText();
    if (clip && *clip && ParseColorString(clip, out_color)) {
        return true;
    }
    if (s_has_copied_color) {
        out_color[0] = s_copied_color[0];
        out_color[1] = s_copied_color[1];
        out_color[2] = s_copied_color[2];
        out_color[3] = s_copied_color[3];
        return true;
    }
    return false;
}

inline void RenderColorContextMenu(const char* popup_id, float* color)
{
    if (ImGui::BeginPopupContextItem(popup_id, ImGuiPopupFlags_MouseButtonRight)) {
        if (ImGui::MenuItem("Copy Color")) CopyColor(color);
        if (ImGui::MenuItem("Paste Color")) PasteColor(color);
        ImGui::EndPopup();
    }
}

} // namespace ColorClipboard
#endif
