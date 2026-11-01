#include "App.hpp"

#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "external/imgui/imgui.h"
#include "overlay/esp_drawing.hpp"
#include "renderer/HighlightTint.hpp"
#include "renderer/Passes.hpp"

namespace {

constexpr int kMaxBoneSlots = 64;

void pack_skinning_slot(float* slot_base, const std::vector<source2::Mat3x4>& palette)
{
    for (size_t j = 0; j < palette.size() && j < shm::MAX_BONES; ++j) {
        const auto& b = palette[j];
        float* dest = slot_base + j * 16;
        dest[0] = b[0][0];
        dest[1] = b[0][1];
        dest[2] = b[0][2];
        dest[3] = 0.0f;
        dest[4] = b[1][0];
        dest[5] = b[1][1];
        dest[6] = b[1][2];
        dest[7] = 0.0f;
        dest[8] = b[2][0];
        dest[9] = b[2][1];
        dest[10] = b[2][2];
        dest[11] = 0.0f;
        dest[12] = b[0][3];
        dest[13] = b[1][3];
        dest[14] = b[2][3];
        dest[15] = 1.0f;
    }
    for (size_t j = palette.size(); j < shm::MAX_BONES; ++j) {
        float* dest = slot_base + j * 16;
        std::fill_n(dest, 16, 0.0f);
        dest[0] = dest[5] = dest[10] = dest[15] = 1.0f;
    }
}

bool line_of_sight(const FrameState::RenderPalette& palette)
{
    return palette.is_visible || palette.is_occluded_by_smoke;
}

// Draws an axis-aligned 3D box at `center` (world units), matching the look of
// CS2's sv_showimpacts markers: flat-shaded faces with a brighter outline. The
// three faces facing the camera are drawn; the rest are culled.
void draw_impact_box(ImDrawList* draw_list, const float* view_matrix, int width, int height,
                     const glm::vec3& center, const glm::vec3& camera, float edge, const float rgb[3], float alpha)
{
    const float half = std::max(0.1f, edge) * 0.5f;
    const glm::vec3 corners[8] = {
        center + glm::vec3(-half, -half, -half), center + glm::vec3(-half, -half, half),
        center + glm::vec3(-half, half, -half),  center + glm::vec3(-half, half, half),
        center + glm::vec3(half, -half, -half),  center + glm::vec3(half, -half, half),
        center + glm::vec3(half, half, -half),   center + glm::vec3(half, half, half),
    };

    ImVec2 screen[8];
    bool valid[8];
    for (int i = 0; i < 8; ++i) {
        float sx = 0.0f, sy = 0.0f;
        valid[i] = world_to_screen(corners[i], &sx, &sy, view_matrix, width, height);
        screen[i] = ImVec2(sx, sy);
    }

    struct Face {
        int a, b, c, d;
        glm::vec3 normal;
    };
    static const Face faces[6] = {
        {0, 2, 3, 1, {-1.0f, 0.0f, 0.0f}}, {4, 5, 7, 6, {1.0f, 0.0f, 0.0f}},
        {0, 1, 5, 4, {0.0f, -1.0f, 0.0f}}, {2, 6, 7, 3, {0.0f, 1.0f, 0.0f}},
        {0, 4, 6, 2, {0.0f, 0.0f, -1.0f}}, {1, 3, 7, 5, {0.0f, 0.0f, 1.0f}},
    };
    const glm::vec3 to_camera = camera - center;
    const glm::vec3 light = glm::normalize(glm::vec3(0.35f, -0.45f, 0.82f));
    const auto to_u32 = [](const float r, const float g, const float b, const float a) {
        return ImGui::ColorConvertFloat4ToU32(
            ImVec4(std::clamp(r, 0.0f, 1.0f), std::clamp(g, 0.0f, 1.0f), std::clamp(b, 0.0f, 1.0f),
                   std::clamp(a, 0.0f, 1.0f)));
    };

    for (const Face& face : faces) {
        if (glm::dot(face.normal, to_camera) <= 0.0f) continue;  // back-facing
        if (!valid[face.a] || !valid[face.b] || !valid[face.c] || !valid[face.d]) continue;
        const float shade = 0.6f + 0.4f * std::max(0.0f, glm::dot(face.normal, light));
        const ImU32 fill = to_u32(rgb[0] * shade, rgb[1] * shade, rgb[2] * shade, alpha);
        draw_list->AddQuadFilled(screen[face.a], screen[face.b], screen[face.c], screen[face.d], fill);
    }

    static const int edges[12][2] = {
        {0, 1}, {0, 2}, {0, 4}, {1, 3}, {1, 5}, {2, 3}, {2, 6}, {3, 7}, {4, 5}, {4, 6}, {5, 7}, {6, 7},
    };
    const ImU32 edge_col = to_u32(rgb[0] + 0.35f, rgb[1] + 0.35f, rgb[2] + 0.35f, alpha);
    for (const auto& edge : edges) {
        if (!valid[edge[0]] || !valid[edge[1]]) continue;
        draw_list->AddLine(screen[edge[0]], screen[edge[1]], edge_col, 1.5f);
    }
}

}

void App::assign_frozen_reveal_slots(std::vector<float>& batch_bones, int& current_slot)
{
    const bool frozen = HitReveal::any_frozen(cfg);
    hit_reveal.for_each_hit([&](HitReveal::Hit& hit) {
        hit.frozen_slot = -1;
        if (!frozen || !hit.palette.model || current_slot >= kMaxBoneSlots) return;
        const size_t needed = static_cast<size_t>(current_slot + 1) * shm::MAX_BONES * 16;
        if (batch_bones.size() < needed) batch_bones.resize(needed, 0.0f);
        pack_skinning_slot(batch_bones.data() + static_cast<size_t>(current_slot) * shm::MAX_BONES * 16,
                           hit.palette.skinning_palette);
        hit.frozen_slot = current_slot++;
    });
}

void App::draw_reveal_passes(const FrameInput& input, const FrameState& state, const OverlayConfig& frame_cfg)
{
    if (hit_reveal.empty()) return;
    static const std::vector<DeadPlayerCham> no_kill_chams;
    static const std::vector<DeadPlayerCham> no_hit_chams;
    const double now = glfwGetTime();
    const int count = std::min<int>(input.packet.player_count, static_cast<int>(state.render_palettes.size()));
    bool depth_cleared = false;

    for (int category = 0; category < 3; ++category) {
        for (bool damage_kind : {false, true}) {
            OverlayConfig reveal_cfg = frame_cfg;
            reveal_cfg.esp_smoke_check = false;
            reveal_cfg.kill_effect = KillFxModeEnum::DISABLED;
            reveal_cfg.hit_effect_mode = KillFxModeEnum::DISABLED;
            reveal_cfg.map_visualizer_enabled = false;
            reveal_cfg.use_depth_prepass = false;
            reveal_cfg.trigger_show_hitboxes = false;
            reveal_cfg.esp_skeleton_vis_delay_enabled = false;
            if (category == 0) {
                reveal_cfg.glow_enabled = false;
                reveal_cfg.esp_enabled = false;
                if (reveal_cfg.style_vis == ChamsStyleEnum::DISABLED) reveal_cfg.style_vis = reveal_cfg.style_invis;
                if (reveal_cfg.style_vis == ChamsStyleEnum::DISABLED) continue;
                reveal_cfg.style_invis = ChamsStyleEnum::DISABLED;
            } else if (category == 1) {
                HitReveal::outline_style(reveal_cfg, frame_cfg, damage_kind);
                reveal_cfg.esp_enabled = false;
            } else {
                if (!reveal_cfg.esp_enabled || !reveal_cfg.esp_skeleton) continue;
                reveal_cfg.style_vis = ChamsStyleEnum::DISABLED;
                reveal_cfg.style_invis = ChamsStyleEnum::DISABLED;
                reveal_cfg.glow_enabled = false;
                reveal_cfg.esp_box_hp_visible = true;
            }

            FrameState only;
            bool any = false;
            for (int i = 0; i < count; ++i) {
                const auto& player = input.packet.players[i];
                if (!player.active || player.health <= 0) continue;
                if (frame_cfg.team_check && player.team == input.packet.local_team) continue;
                if (is_beyond_esp_distance(player, input.packet, frame_cfg)) continue;
                const auto choice = hit_reveal.choose_kind(player, frame_cfg, category, now, damage_kind);
                if (!choice) continue;
                if (category != 1 && !damage_kind && hit_reveal.choose_kind(player, frame_cfg, category, now, true)) continue;
                const auto& live = state.render_palettes[i];
                if (live.model && live.hidden(category)) continue;

                FrameState::RenderPalette palette = choice.dynamic ? live : choice.hit->palette;
                palette.distance_alpha = live.distance_alpha;
                const int slot = choice.dynamic ? state.player_ubo_slots[i] : choice.hit->frozen_slot;
                if (!palette.model) continue;
                if (category < 2 && slot < 0) continue;
                if (!choice.ignore_solid && !line_of_sight(palette)) continue;

                palette.is_visible = true;
                palette.is_occluded_by_smoke = false;
                const float opacity = HitReveal::opacity(choice.hit, now, choice.seconds, choice.fade);
                for (int c = 0; c < 3; ++c) {
                    palette.highlight_alpha[c] = c == category ? live.highlight_alpha[c] * opacity : 0.0f;
                    palette.highlight_tint[c] = c == category && live.highlight_tint[c];
                }
                std::memcpy(palette.highlight_color, live.highlight_color, sizeof(palette.highlight_color));
                if (palette.highlight_alpha[category] <= 0.0f) continue;

                if (!any) {
                    only.render_palettes.assign(count, FrameState::RenderPalette{});
                    only.player_ubo_slots.assign(count, -1);
                    only.vischeck_result = state.vischeck_result;
                    only.vischeck_result.player_visible.resize(count, false);
                    only.vischeck_result.player_occluded_by_smoke.resize(count, false);
                    only.dummy_vis = state.dummy_vis;
                    any = true;
                }
                only.render_palettes[i] = std::move(palette);
                only.player_ubo_slots[i] = slot;
                only.vischeck_result.player_visible[i] = true;
                only.vischeck_result.player_occluded_by_smoke[i] = false;
                if (category < 2) only.sorted_player_indices.push_back(i);
            }
            if (!any) continue;

            std::sort(only.sorted_player_indices.begin(), only.sorted_player_indices.end(), [&](int a, int b) {
                return only.render_palettes[a].model->vao < only.render_palettes[b].model->vao;
            });

            glDepthMask(GL_TRUE);
            glStencilMask(0xFF);
            glClearDepth(1.0);
            glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
            depth_cleared = true;

            ChamsPassInputs reveal_inputs{input,         only,          reveal_cfg,   chams_renderer,
                                          depth_prepass, esp_renderer,  nullptr,      model_cache,
                                          window_width,  window_height, no_kill_chams, no_hit_chams};
            ChamsPass::run(reveal_inputs);
        }
    }

    if (depth_cleared) {
        glDepthMask(GL_TRUE);
        glClear(GL_DEPTH_BUFFER_BIT);
        if (depth_prepass.has_rendered_this_frame()) depth_prepass.render(input.gl_vp);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
    }
}

void App::build_esp_reveal(const FrameInput& input, const FrameState& state, const OverlayConfig& frame_cfg,
                           const FrameInput*& esp_input, FrameState*& esp_state)
{
    if (hit_reveal.empty() || !frame_cfg.esp_enabled || !frame_cfg.esp_damage_reveal) return;
    const double now = glfwGetTime();
    const int count = std::min<int>(input.packet.player_count, static_cast<int>(state.render_palettes.size()));
    bool copied = false;
    bool frozen = false;

    for (int i = 0; i < count; ++i) {
        const auto& player = input.packet.players[i];
        if (!player.active || player.health <= 0) continue;
        if (frame_cfg.team_check && player.team == input.packet.local_team) continue;
        if (is_beyond_esp_distance(player, input.packet, frame_cfg)) continue;
        const auto choice = hit_reveal.choose(player, frame_cfg, 2, now);
        if (!choice) continue;
        const auto& live = state.render_palettes[i];
        if (live.model && live.hidden(2)) continue;

        const bool live_visible =
            i < static_cast<int>(state.vischeck_result.player_visible.size()) ? state.vischeck_result.player_visible[i] : true;
        if (live.model && live_visible) continue;
        const bool live_draws = live.model && frame_cfg.esp_box_hp_occluded;
        const bool use_frozen = !choice.dynamic && !live_draws && choice.hit->palette.model;
        const auto& source = use_frozen ? choice.hit->palette : live;
        if (!source.model) continue;
        if (!choice.ignore_solid && !line_of_sight(source)) continue;

        if (!copied) {
            esp_reveal_state = state;
            esp_reveal_state.vischeck_result.player_visible.resize(count, true);
            esp_reveal_state.vischeck_result.player_occluded_by_smoke.resize(count, false);
            copied = true;
        }
        auto& rp = esp_reveal_state.render_palettes[i];
        if (use_frozen) {
            float alpha[3];
            bool tint[3];
            float color[4];
            std::memcpy(alpha, rp.highlight_alpha, sizeof(alpha));
            std::memcpy(tint, rp.highlight_tint, sizeof(tint));
            std::memcpy(color, rp.highlight_color, sizeof(color));
            rp = choice.hit->palette;
            rp.distance_alpha = live.distance_alpha;
            std::memcpy(rp.highlight_alpha, alpha, sizeof(alpha));
            std::memcpy(rp.highlight_tint, tint, sizeof(tint));
            std::memcpy(rp.highlight_color, color, sizeof(color));
            if (!frozen) {
                if (!esp_reveal_frame) esp_reveal_frame = std::make_unique<FrameInput>();
                *esp_reveal_frame = input;
                frozen = true;
            }
            esp_reveal_frame->packet.players[i] = choice.hit->player;
        }
        rp.is_visible = true;
        rp.is_occluded_by_smoke = false;
        if (!live_draws) rp.highlight_alpha[2] *= HitReveal::opacity(choice.hit, now, choice.seconds, choice.fade);
        esp_reveal_state.vischeck_result.player_visible[i] = true;
        esp_reveal_state.vischeck_result.player_occluded_by_smoke[i] = false;
    }

    if (copied) esp_state = &esp_reveal_state;
    if (frozen) esp_input = esp_reveal_frame.get();
}

void App::update_hit_effects(const FrameInput& input, const FrameState& state, const OverlayConfig& frame_cfg,
                             ImDrawList* draw_list)
{
    namespace hfx = hitfx;
    const bool effects_on = frame_cfg.hit_effect_mode != KillFxModeEnum::DISABLED;
    if (!effects_on && !frame_cfg.bullet_impacts_enabled) {
        hit_fx_detector.reset();
        hit_fx.clear();
        hit_fx_effects.clear();
        active_hit_chams.clear();
        bullet_impacts_consumer_.clear();
        bullet_impacts_selection_.clear();
        return;
    }
    const auto& packet = input.packet;
    const auto to_p3 = [](const Vec3& v) { return hfx::P3{v.x, v.y, v.z}; };
    hfx::AimSample aim_sample = hfx::aim_sample(to_p3(packet.local_eye), to_p3(packet.local_angles),
                                                to_p3(packet.local_aim_punch));
    aim_sample.seeded = true;
    aim_sample.tick = packet.local_seed_tick > 0 ? packet.local_seed_tick : packet.local_tick_base;
    aim_sample.weapon.total = packet.local_spread;
    aim_sample.weapon.base = packet.local_base_spread;
    aim_sample.weapon.recoil_index = packet.local_recoil_index;
    aim_sample.weapon.weapon_id = packet.held_weapon_id;
    aim_sample.weapon.num_bullets = packet.local_num_bullets;
    aim_sample.weapon.spread_seed = packet.local_spread_seed;

    const int count = std::clamp(packet.player_count, 0, static_cast<int>(shm::MAX_PLAYERS));

    struct SlotJoints {
        std::array<hfx::P3, 32> positions{};
        std::array<bool, 32> available{};
        hfx::Joints view() const { return {positions.data(), available.data(), 32}; }
    };
    const auto slot_joints = [&](int slot) {
        SlotJoints out;
        if (slot < 0 || static_cast<size_t>(slot) >= state.render_palettes.size()) return out;
        const auto& palette = state.render_palettes[static_cast<size_t>(slot)];
        if (!palette.model) return out;
        const int limit = std::min<int>(32, static_cast<int>(palette.sanitized_bones.size()));
        for (int j = 0; j < limit; ++j) {
            const auto& bone = palette.sanitized_bones[static_cast<size_t>(j)];
            out.positions[static_cast<size_t>(j)] = {bone.x, bone.y, bone.z};
            out.available[static_cast<size_t>(j)] = true;
        }
        return out;
    };

    std::vector<hfx::EnemySample> enemies;
    enemies.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        const auto& player = packet.players[i];
        if (!player.active || player.team == packet.local_team || player.name[0] == '\0') continue;
        const std::string_view name(player.name, strnlen(player.name, sizeof(player.name)));
        enemies.push_back({player.entity_index, std::hash<std::string_view>{}(name), player.health});
    }

    const double now = glfwGetTime();
    hit_fx_detector.set_use_shot_sample(true);
    const auto events = hit_fx_detector.update(now, packet.local_health > 0, packet.local_shots_fired, aim_sample, enemies,
                                               frame_cfg.hit_fx_local_only);

    if (effects_on) {
        for (const auto& event : events) {
            int slot = -1;
            for (int i = 0; i < count; ++i) {
                if (packet.players[i].active && packet.players[i].entity_index == event.entity) {
                    slot = i;
                    break;
                }
            }
            if (slot < 0) continue;
            const auto& live = state.render_palettes[static_cast<size_t>(slot)];
            const auto joints = slot_joints(slot);
            const auto impact =
                hfx::estimate_impact(event.ray, joints.view(), to_p3(packet.players[slot].origin + Vec3(0.0f, 0.0f, 45.0f)));
            const glm::vec3 impact_pos(impact.point.x, impact.point.y, impact.point.z);

            switch (frame_cfg.hit_effect_mode.value) {
                case KillFxModeEnum::CHAMS: {
                    // Ghost afterimage: a frozen copy of the live pose that fades out.
                    if (!live.model) break;
                    DeadPlayerCham ghost;
                    ghost.vao = live.model->vao;
                    ghost.ibo = live.model->ibo;
                    ghost.index_count = live.model->index_count;
                    ghost.skinning_palette = live.skinning_palette;
                    ghost.kill_time = now;
                    ghost.fade_time = frame_cfg.hit_chams_fade_time;
                    ghost.style_id = get_style_id(frame_cfg.hit_chams_style);
                    std::memcpy(ghost.color, frame_cfg.hit_chams_color, sizeof(float) * 4);
                    std::memcpy(ghost.secondary_color, frame_cfg.hit_chams_color_sec, sizeof(float) * 4);
                    // Hits are far more frequent than kills, so keep a hard cap on ghosts.
                    if (active_hit_chams.size() >= 64) active_hit_chams.erase(active_hit_chams.begin());
                    active_hit_chams.push_back(std::move(ghost));
                    break;
                }
                case KillFxModeEnum::PARTICLES: {
                    damagefx::ParticleSettings settings;
                    settings.color = frame_cfg.hit_effect_color;
                    settings.count = frame_cfg.hit_effect_count;
                    settings.speed = frame_cfg.hit_effect_speed;
                    settings.lifetime = frame_cfg.hit_effect_lifetime;
                    settings.size = frame_cfg.hit_effect_size;
                    hit_fx_effects.spawn_particles(impact_pos, settings);
                    break;
                }
                case KillFxModeEnum::PULSE: {
                    damagefx::PulseSettings settings;
                    settings.color = frame_cfg.hit_pulse_color;
                    settings.duration = frame_cfg.hit_pulse_duration;
                    settings.intensity = frame_cfg.hit_pulse_intensity;
                    hit_fx_effects.spawn_pulse(settings);
                    break;
                }
                case KillFxModeEnum::LIGHTNING: {
                    damagefx::LightningSettings settings;
                    settings.color = frame_cfg.hit_lightning_color;
                    settings.duration = frame_cfg.hit_lightning_duration;
                    settings.width = frame_cfg.hit_lightning_width;
                    settings.count = frame_cfg.hit_lightning_count;
                    hit_fx_effects.spawn_lightning(impact_pos, settings);
                    break;
                }
                case KillFxModeEnum::SHOCKWAVE: {
                    damagefx::ShockwaveSettings settings;
                    settings.color = frame_cfg.hit_shockwave_color;
                    settings.duration = frame_cfg.hit_shockwave_duration;
                    settings.max_radius = frame_cfg.hit_shockwave_max_radius;
                    settings.width = frame_cfg.hit_shockwave_width;
                    hit_fx_effects.spawn_shockwave(impact_pos, settings);
                    break;
                }
                default:
                    break;
            }
        }
    }

    // Native client endpoints already include the game's recoil and spread. Never rebuild a ray
    // or guess a position when the impact source is unavailable, stale or changed.
    const auto native_impacts = bullet_impacts_consumer_.consume(
        game_reader.fetch_client_impacts(),
        std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(),
        frame_cfg.bullet_impacts_enabled);
    if (native_impacts.reset) {
        hit_fx.clear_flashes();
        bullet_impacts_selection_.clear();
    }
    if (native_impacts.ready) {
        hfx::FlashParams flash;
        if (frame_cfg.bullet_impacts_style == BulletImpactStyleEnum::BOXES) {
            flash.color = {frame_cfg.bullet_impacts_box_color.r, frame_cfg.bullet_impacts_box_color.g,
                           frame_cfg.bullet_impacts_box_color.b, frame_cfg.bullet_impacts_box_color.a};
            flash.size = frame_cfg.bullet_impacts_box_size;
        } else {
            flash.color = {frame_cfg.bullet_impacts_color.r, frame_cfg.bullet_impacts_color.g,
                           frame_cfg.bullet_impacts_color.b, frame_cfg.bullet_impacts_color.a};
            flash.size = frame_cfg.bullet_impacts_size;
        }
        flash.duration = frame_cfg.bullet_impacts_duration;
        flash.hold_gap = frame_cfg.bullet_impacts_hold ? frame_cfg.bullet_impacts_hold_gap : 0.0f;
        for (const auto& impact : native_impacts.records) {
            const auto choice = bullet_impacts_selection_.select(impact.copied_clock, impact.expires, frame_cfg.bullet_impacts_spread);
            if (!choice.show) continue;
            hit_fx.spawn_flash({impact.x, impact.y, impact.z}, flash, choice.replace_hold);
        }
    }

    // Retire expired ghost afterimages.
    for (auto it = active_hit_chams.begin(); it != active_hit_chams.end();) {
        if (now - it->kill_time >= it->fade_time) {
            it = active_hit_chams.erase(it);
        } else {
            ++it;
        }
    }

    hit_fx_effects.step(input.dt, frame_cfg.hit_effect_gravity);
    hit_fx.step(input.dt, frame_cfg.hit_effect_gravity);
    if (!draw_list) return;

    hit_fx_effects.draw(draw_list, input.render_view_matrix, window_width, window_height);
    if (hit_fx.flashes().empty()) return;

    const auto project = [&](const hfx::P3& p, ImVec2& out) {
        float sx = 0.0f, sy = 0.0f;
        if (!world_to_screen(Vec3(p.x, p.y, p.z), &sx, &sy, input.render_view_matrix, window_width, window_height))
            return false;
        out = ImVec2(sx, sy);
        return true;
    };
    const auto colour = [](const hfx::Rgba& c, float alpha) {
        return ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, std::clamp(c.a * alpha, 0.0f, 1.0f)));
    };

    const bool box_style = frame_cfg.bullet_impacts_style == BulletImpactStyleEnum::BOXES;
    for (const auto& flash : hit_fx.flashes()) {
        const float a = hfx::HitFx::flash_alpha(flash);
        if (a <= 0.0f) continue;
        if (box_style) {
            const float rgb[3] = {flash.color.r, flash.color.g, flash.color.b};
            draw_impact_box(draw_list, input.render_view_matrix, window_width, window_height,
                            glm::vec3(flash.pos.x, flash.pos.y, flash.pos.z), input.cam_pos, flash.size, rgb,
                            a * flash.color.a);
        } else {
            ImVec2 c;
            if (!project(flash.pos, c)) continue;
            const float r = std::max(0.5f, flash.size * 0.5f);
            draw_list->AddCircleFilled(c, r + 1.0f, colour(flash.color, a * 0.35f), 12);
            draw_list->AddCircleFilled(c, r, colour(flash.color, a), 12);
        }
    }
}
