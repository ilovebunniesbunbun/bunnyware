#include "CrosshairDistance.hpp"

#include "FrameInput.hpp"
#include "VisibilityWorker.hpp"
#include "config.hpp"
#include "external/imgui/imgui.h"
#include "math/Distance.hpp"
#include "overlay/esp_drawing.hpp"

#include <cmath>
#include <cstdio>

namespace {

constexpr float MAX_TRACE_UNITS = 8192.0f;
constexpr float PLAYER_RADIUS = 16.0f;
constexpr float PLAYER_FEET_OFFSET = 8.0f;
constexpr float PLAYER_HEAD_OFFSET = 64.0f;
constexpr float PIN_RADIUS = 5.0f;

struct AimHit {
    float units;
    bool is_player;
    glm::vec3 point;
};

std::optional<AimHit> measure(const FrameInput& input, const VisibilityWorker& vis_worker)
{
    const ShmPacket& packet = input.packet;
    const glm::vec3 eye = packet.local_eye;
    const glm::vec3 fwd = distance::view_forward(packet.local_angles);

    const TraceResult world = vis_worker.trace_ray_bvh(eye, eye + fwd * MAX_TRACE_UNITS);
    float best_units = world.hit ? world.distance : distance::MISS;
    bool best_is_player = false;

    const int count = std::min(packet.player_count, static_cast<int>(shm::MAX_PLAYERS));
    for (int i = 0; i < count; ++i) {
        const PlayerData& player = packet.players[i];
        if (!player.active || player.health <= 0) continue;
        if (glm::distance(player.origin, packet.local_origin) < 1.0f) continue;

        const glm::vec3 feet = player.origin + glm::vec3(0.0f, 0.0f, PLAYER_FEET_OFFSET);
        const glm::vec3 head = player.origin + glm::vec3(0.0f, 0.0f, PLAYER_HEAD_OFFSET);
        const float t = distance::ray_capsule_distance(eye, fwd, feet, head, PLAYER_RADIUS);
        if (t == distance::MISS) continue;
        if (best_units == distance::MISS || t < best_units) {
            best_units = t;
            best_is_player = true;
        }
    }

    if (best_units == distance::MISS) return std::nullopt;
    return AimHit{best_units, best_is_player, eye + fwd * best_units};
}

ImU32 to_imcolor(const Color& c)
{
    return ImColor(c.r, c.g, c.b, c.a);
}

void draw_outlined_text(ImDrawList* draw_list, const char* text, float x, float y, ImU32 col, float alpha)
{
    const ImU32 outline = IM_COL32(0, 0, 0, static_cast<int>(220 * alpha));
    x = std::round(x);
    y = std::round(y);
    draw_list->AddText(ImVec2(x + 1.0f, y + 1.0f), outline, text);
    draw_list->AddText(ImVec2(x - 1.0f, y + 1.0f), outline, text);
    draw_list->AddText(ImVec2(x + 1.0f, y - 1.0f), outline, text);
    draw_list->AddText(ImVec2(x - 1.0f, y - 1.0f), outline, text);
    draw_list->AddText(ImVec2(x, y), col, text);
}

void format_meters(char* buf, size_t size, float units)
{
    std::snprintf(buf, size, "%.1f m", distance::units_to_meters(units));
}

}

void CrosshairDistance::handle_keys(const FrameInput& input, const VisibilityWorker& vis_worker, bool pin_down,
                                    bool clear_down)
{
    if (pin_down && !pin_was_down_ && input.packet.local_health > 0) {
        if (const auto hit = measure(input, vis_worker)) pin_ = hit->point;
    }
    if (clear_down && !clear_was_down_) pin_.reset();

    pin_was_down_ = pin_down;
    clear_was_down_ = clear_down;
}

void CrosshairDistance::render(ImDrawList* draw_list, const FrameInput& input, const OverlayConfig& cfg,
                               const VisibilityWorker& vis_worker, int width, int height)
{
    if (input.packet.local_health <= 0) return;

    char text[32];
    if (const auto hit = measure(input, vis_worker)) {
        format_meters(text, sizeof(text), hit->units);
        const Color& c = hit->is_player ? cfg.esp_crosshair_distance_player_color : cfg.esp_crosshair_distance_color;
        const ImVec2 size = ImGui::CalcTextSize(text);
        draw_outlined_text(draw_list, text, width * 0.5f - size.x * 0.5f,
                           height * 0.5f + cfg.esp_crosshair_distance_offset, to_imcolor(c), c.a);
    }

    if (!pin_) return;

    float sx = 0.0f;
    float sy = 0.0f;
    if (!world_to_screen(*pin_, &sx, &sy, input.render_view_matrix, width, height)) return;

    const Color& c = cfg.esp_crosshair_distance_pin_color;
    const ImU32 col = to_imcolor(c);
    const ImVec2 center(std::round(sx), std::round(sy));
    draw_list->AddCircleFilled(center, PIN_RADIUS + 1.5f, IM_COL32(0, 0, 0, static_cast<int>(200 * c.a)), 16);
    draw_list->AddCircleFilled(center, PIN_RADIUS, col, 16);

    format_meters(text, sizeof(text), glm::distance(glm::vec3(input.packet.local_eye), *pin_));
    const ImVec2 size = ImGui::CalcTextSize(text);
    draw_outlined_text(draw_list, text, sx - size.x * 0.5f, sy - PIN_RADIUS - size.y - 3.0f, col, c.a);
}
