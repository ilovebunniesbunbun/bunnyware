#pragma once
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <string>
#include <unordered_map>

#include "FrameState.hpp"
#include "config.hpp"
#include "overlay/game_reader.hpp"

class HitReveal {
public:
    struct Hit {
        double at = 0.0;
        PlayerData player{};
        FrameState::RenderPalette palette;
        bool through_smoke = false;
        int frozen_slot = -1;
    };

    struct Choice {
        const Hit* hit = nullptr;
        float seconds = 1.0f;
        bool dynamic = true;
        bool fade = true;
        bool ignore_solid = false;
        bool damage = false;
        explicit operator bool() const { return hit != nullptr; }
    };

private:
    static constexpr double window = 0.75;
    struct Candidate {
        Hit hit;
        int loss;
        double first;
        bool recent_shot;
    };
    struct Evidence {
        double at;
        float damage;
    };
    std::unordered_map<int, PlayerData> previous;
    std::unordered_map<int, Hit> hits;
    std::unordered_map<int, Hit> any_hits;
    std::unordered_map<int, Candidate> pending;
    std::deque<Evidence> evidence;
    float damage = -1.0f;
    int shots = 0;
    double shot_at = -10.0;
    double ambiguous_until = -10.0;

    static bool same_name(const PlayerData& a, const PlayerData& b)
    {
        return std::strncmp(a.name, b.name, sizeof(a.name)) == 0;
    }

public:
    void clear()
    {
        previous.clear();
        hits.clear();
        any_hits.clear();
        pending.clear();
        evidence.clear();
        damage = -1.0f;
        shots = 0;
        shot_at = -10.0;
        ambiguous_until = -10.0;
    }

    bool empty() const { return hits.empty() && any_hits.empty(); }

    void update(const ShmPacket& p, const FrameState& state, double now, bool collect_any, bool collect_local)
    {
        if (!collect_any) any_hits.clear();
        if (!collect_local) {
            hits.clear();
            pending.clear();
            evidence.clear();
        }
        if (p.local_health <= 0 || (!collect_any && !collect_local)) {
            clear();
            return;
        }
        if (std::isfinite(p.local_total_damage) && p.local_total_damage < damage) clear();
        if (p.local_shots_fired > shots) shot_at = now;
        while (!evidence.empty() && now - evidence.front().at > window) evidence.pop_front();
        for (auto it = pending.begin(); it != pending.end();) {
            if (now - it->second.hit.at > window)
                it = pending.erase(it);
            else
                ++it;
        }

        const float dealt = damage >= 0.0f && std::isfinite(p.local_total_damage) ? p.local_total_damage - damage : 0.0f;
        if (collect_local && dealt > 0.0f) evidence.push_back({now, dealt});
        const int count = std::clamp(p.player_count, 0, static_cast<int>(shm::MAX_PLAYERS));
        for (int i = 0; i < count; ++i) {
            const auto& enemy = p.players[i];
            if (!enemy.active || enemy.team == p.local_team) continue;
            auto before = previous.find(enemy.entity_index);
            if (before == previous.end() || !same_name(before->second, enemy)) {
                pending.erase(enemy.entity_index);
                hits.erase(enemy.entity_index);
                any_hits.erase(enemy.entity_index);
                continue;
            }
            if (enemy.health < before->second.health && before->second.health > 0) {
                FrameState::RenderPalette palette;
                if (i < static_cast<int>(state.render_palettes.size())) palette = state.render_palettes[i];
                const bool smoked = palette.model && palette.is_occluded_by_smoke;
                Hit snapshot{now, enemy, palette, smoked, -1};
                if (collect_any && enemy.health > 0) any_hits.insert_or_assign(enemy.entity_index, snapshot);
                if (!collect_local) continue;
                auto old = pending.find(enemy.entity_index);
                const int prior_loss = old == pending.end() ? 0 : old->second.loss;
                const double first = old == pending.end() ? now : old->second.first;
                pending.insert_or_assign(enemy.entity_index,
                                         Candidate{snapshot, prior_loss + before->second.health - std::max(0, enemy.health),
                                                   first, now - shot_at <= window});
            }
        }
        if (pending.size() > 1) {
            ambiguous_until = now + window;
            pending.clear();
            evidence.clear();
        }
        if (now < ambiguous_until) {
            pending.clear();
            evidence.clear();
        }
        if (pending.size() == 1) {
            auto& candidate = pending.begin()->second;
            if (now - candidate.first >= 0.05 && candidate.hit.player.health > 0) {
                float dealt_in_window = 0.0f;
                for (const auto& e : evidence)
                    if (std::abs(e.at - candidate.hit.at) <= window) dealt_in_window += e.damage;
                if (candidate.recent_shot && dealt_in_window > 0.0f &&
                    std::abs(dealt_in_window - static_cast<float>(candidate.loss)) <= 1.0f) {
                    Hit confirmed = candidate.hit;
                    confirmed.at = now;
                    hits.insert_or_assign(confirmed.player.entity_index, std::move(confirmed));
                    pending.clear();
                    evidence.clear();
                }
            }
        }
        previous.clear();
        for (int i = 0; i < count; ++i)
            if (p.players[i].active) previous.emplace(p.players[i].entity_index, p.players[i]);
        auto prune = [&](auto& records) {
            for (auto it = records.begin(); it != records.end();) {
                auto player = previous.find(it->first);
                if (now - it->second.at > 3.0 || player == previous.end() || player->second.health <= 0 ||
                    !same_name(player->second, it->second.player))
                    it = records.erase(it);
                else
                    ++it;
            }
        };
        prune(hits);
        prune(any_hits);
        damage = std::isfinite(p.local_total_damage) ? p.local_total_damage : -1.0f;
        shots = p.local_shots_fired;
    }

    template <class F>
    void for_each_hit(F f)
    {
        for (auto& [entity, hit] : hits) f(hit);
        for (auto& [entity, hit] : any_hits) f(hit);
    }

    const Hit* get(const PlayerData& player, double now, float seconds, bool any_damage) const
    {
        const auto& records = any_damage ? any_hits : hits;
        auto it = records.find(player.entity_index);
        if (it == records.end() || !same_name(it->second.player, player)) return nullptr;
        if (!any_damage && !it->second.through_smoke) return nullptr;
        if (now - it->second.at >= std::clamp(seconds, 0.1f, 3.0f)) return nullptr;
        return &it->second;
    }

    Choice choose_kind(const PlayerData& player, const OverlayConfig& cfg, int category, double now, bool any_damage) const
    {
        Choice result;
        if (category == 2) {
            // ESP reveals: "Hit Reveal" (esp_damage_reveal) enables them and
            // "Smokes Only" (esp_hit_reveal) restricts them to smoke hits.
            if (!cfg.esp_damage_reveal) return result;
            const bool smokes_only = cfg.esp_hit_reveal;
            if (any_damage == smokes_only) return result;
            result.seconds = cfg.esp_damage_reveal_seconds;
            result.hit = get(player, now, result.seconds, !smokes_only);
            result.dynamic = cfg.esp_damage_reveal_dynamic;
            result.fade = cfg.esp_damage_reveal_fade;
            result.ignore_solid = !smokes_only;
            result.damage = !smokes_only;
            return result;
        }
        if (any_damage) {
            const bool enabled = category == 0 ? cfg.chams_damage_reveal
                               : category == 1 ? cfg.glow_damage_reveal
                                               : cfg.esp_damage_reveal;
            if (!enabled) return result;
            result.seconds = category == 0 ? cfg.chams_damage_reveal_seconds
                           : category == 1 ? cfg.glow_damage_reveal_seconds
                                           : cfg.esp_damage_reveal_seconds;
            result.hit = get(player, now, result.seconds, true);
            result.dynamic = category == 0 ? cfg.chams_damage_reveal_dynamic
                           : category == 1 ? cfg.glow_damage_reveal_dynamic
                                           : cfg.esp_damage_reveal_dynamic;
            result.fade = category == 0 ? cfg.chams_damage_reveal_fade
                        : category == 1 ? cfg.glow_damage_reveal_fade
                                        : cfg.esp_damage_reveal_fade;
            result.ignore_solid = true;
            result.damage = true;
            return result;
        }
        const bool enabled = category == 0 ? cfg.chams_hit_reveal : cfg.glow_hit_reveal;
        if (!enabled) return result;
        result.seconds = category == 0 ? cfg.chams_hit_reveal_seconds : cfg.glow_hit_reveal_seconds;
        result.hit = get(player, now, result.seconds, false);
        result.dynamic = category == 0 ? cfg.chams_hit_reveal_dynamic : cfg.glow_hit_reveal_dynamic;
        result.fade = category == 0 ? cfg.chams_hit_reveal_fade : cfg.glow_hit_reveal_fade;
        return result;
    }

    Choice choose(const PlayerData& player, const OverlayConfig& cfg, int category, double now) const
    {
        auto damage_choice = choose_kind(player, cfg, category, now, true);
        return damage_choice ? damage_choice : choose_kind(player, cfg, category, now, false);
    }

    static void outline_style(OverlayConfig& output, const OverlayConfig& source, bool damage_kind)
    {
        output.glow_enabled = true;
        output.outline_mode = "stencil";
        output.glow_color = damage_kind ? source.glow_damage_reveal_color : source.glow_hit_reveal_color;
        output.glow_thickness = damage_kind ? source.glow_damage_reveal_thickness : source.glow_hit_reveal_thickness;
        output.glow_health_based = false;
        output.glow_pulse = false;
        output.style_vis = ChamsStyleEnum::DISABLED;
        output.style_invis = ChamsStyleEnum::DISABLED;
    }

    static float opacity(const Hit* hit, double now, float seconds, bool fade)
    {
        if (!hit || !fade) return 1.0f;
        return std::clamp(1.0f - static_cast<float>((now - hit->at) / std::clamp(seconds, 0.1f, 3.0f)), 0.0f, 1.0f);
    }

    static bool any_enabled(const OverlayConfig& cfg, bool damage_kind)
    {
        return damage_kind ? (cfg.chams_damage_reveal || cfg.glow_damage_reveal || cfg.esp_damage_reveal)
                           : (cfg.chams_hit_reveal || cfg.glow_hit_reveal || cfg.esp_hit_reveal);
    }

    static bool any_frozen(const OverlayConfig& cfg)
    {
        return (cfg.chams_damage_reveal && !cfg.chams_damage_reveal_dynamic) ||
               (cfg.glow_damage_reveal && !cfg.glow_damage_reveal_dynamic) ||
               (cfg.chams_hit_reveal && !cfg.chams_hit_reveal_dynamic) ||
               (cfg.glow_hit_reveal && !cfg.glow_hit_reveal_dynamic);
    }
};
