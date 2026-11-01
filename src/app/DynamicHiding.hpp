#pragma once
#include <cmath>

#include "config.hpp"

inline void apply_flash_visibility(OverlayConfig& cfg, float alpha)
{
    if (!std::isfinite(alpha) || alpha <= 0.001f) return;
    if (cfg.chams_hide_when_flashed) {
        cfg.style_vis = ChamsStyleEnum::DISABLED;
        cfg.style_invis = ChamsStyleEnum::DISABLED;
        cfg.anti_flash_chams = false;
        if (cfg.kill_effect == KillFxModeEnum::CHAMS) cfg.kill_effect = KillFxModeEnum::DISABLED;
        if (cfg.hit_effect_mode == KillFxModeEnum::CHAMS) cfg.hit_effect_mode = KillFxModeEnum::DISABLED;
    }
    if (cfg.glow_hide_when_flashed) cfg.glow_enabled = false;
    if (cfg.esp_hide_when_flashed) {
        cfg.esp_enabled = false;
        cfg.esp_step_circles = false;
    }
}

inline void apply_shooting_visibility(OverlayConfig& cfg, bool shooting)
{
    if (!shooting) return;
    OverlayConfig mask = cfg;
    mask.chams_hide_when_flashed = cfg.chams_hide_while_shooting;
    mask.glow_hide_when_flashed = cfg.glow_hide_while_shooting;
    mask.esp_hide_when_flashed = cfg.esp_hide_while_shooting;
    apply_flash_visibility(mask, 1.0f);
    mask.chams_hide_when_flashed = cfg.chams_hide_when_flashed;
    mask.glow_hide_when_flashed = cfg.glow_hide_when_flashed;
    mask.esp_hide_when_flashed = cfg.esp_hide_when_flashed;
    cfg = mask;
}
