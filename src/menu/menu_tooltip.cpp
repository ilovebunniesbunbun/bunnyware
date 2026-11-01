#include "menu_tooltip.hpp"

#include "external/imgui/imgui.h"

namespace MenuTooltip {
void show_item(const char* text)
{
    if (!ImGui::BeginItemTooltip()) return;

    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}
}
