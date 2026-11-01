#pragma once
#include <cstring>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <vector>

struct GpuDevice {
    std::string name;
    std::string vendor_id;
    std::string device_id;
    std::string driver;
    std::string display_name;
};

std::vector<GpuDevice> detect_gpus();

struct Color {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 0.0f;

    Color() = default;
    Color(float red, float green, float blue, float alpha) : r(red), g(green), b(blue), a(alpha)
    {
    }

    bool operator==(const Color& other) const
    {
        return r == other.r && g == other.g && b == other.b && a == other.a;
    }
    bool operator!=(const Color& other) const
    {
        return !(*this == other);
    }

    float& operator[](size_t index)
    {
        return (&r)[index];
    }
    const float& operator[](size_t index) const
    {
        return (&r)[index];
    }
    operator float*()
    {
        return &r;
    }
    operator const float*() const
    {
        return &r;
    }
    float* data()
    {
        return &r;
    }
    const float* data() const
    {
        return &r;
    }
};

enum class ChamsStyleEnum {
    DISABLED,
    SOLID,
    FLAT,
    METALLIC,
    GLASS,
};

class ChamsStyle {
public:
    ChamsStyleEnum value = ChamsStyleEnum::FLAT;

    ChamsStyle() = default;
    ChamsStyle(ChamsStyleEnum val) : value(val)
    {
    }
    ChamsStyle(const std::string& str)
    {
        if (str == "solid" || str == "textured")
            value = ChamsStyleEnum::SOLID;
        else if (str == "flat")
            value = ChamsStyleEnum::FLAT;
        else if (str == "metallic" || str == "fresnel")
            value = ChamsStyleEnum::METALLIC;
        else if (str == "glass")
            value = ChamsStyleEnum::GLASS;
        else if (str == "glow_blend" || str == "glow" || str == "cs2_glow" ||
                 str == "wireframe" || str == "hologram" || str == "thermal" ||
                 str == "cel_shaded" || str == "matrix" || str == "rainbow" ||
                 str == "trans" || str == "circuit" || str == "pride")
            value = ChamsStyleEnum::SOLID;
        else
            value = ChamsStyleEnum::DISABLED;
    }
    ChamsStyle(const char* str) : ChamsStyle(std::string(str))
    {
    }

    operator std::string() const
    {
        switch (value) {
            case ChamsStyleEnum::SOLID:
                return "solid";
            case ChamsStyleEnum::FLAT:
                return "flat";
            case ChamsStyleEnum::METALLIC:
                return "metallic";
            case ChamsStyleEnum::GLASS:
                return "glass";
            default:
                return "disabled";
        }
    }

    bool operator==(const ChamsStyle& other) const
    {
        return value == other.value;
    }
    bool operator!=(const ChamsStyle& other) const
    {
        return value != other.value;
    }
    bool operator==(const std::string& str) const
    {
        return (std::string) * this == str;
    }
    bool operator!=(const std::string& str) const
    {
        return (std::string) * this != str;
    }
    bool operator==(const char* str) const
    {
        return (std::string) * this == str;
    }
};

enum class KillFxModeEnum { DISABLED, CHAMS, PARTICLES, PULSE, LIGHTNING, SHOCKWAVE };

class KillFxMode {
public:
    KillFxModeEnum value = KillFxModeEnum::CHAMS;

    KillFxMode() = default;
    KillFxMode(KillFxModeEnum val) : value(val)
    {
    }
    KillFxMode(const std::string& str)
    {
        if (str == "chams")
            value = KillFxModeEnum::CHAMS;
        else if (str == "particles")
            value = KillFxModeEnum::PARTICLES;
        else if (str == "pulse")
            value = KillFxModeEnum::PULSE;
        else if (str == "lightning")
            value = KillFxModeEnum::LIGHTNING;
        else if (str == "shockwave")
            value = KillFxModeEnum::SHOCKWAVE;
        else
            value = KillFxModeEnum::DISABLED;
    }
    KillFxMode(const char* str) : KillFxMode(std::string(str))
    {
    }

    operator std::string() const
    {
        switch (value) {
            case KillFxModeEnum::CHAMS:
                return "chams";
            case KillFxModeEnum::PARTICLES:
                return "particles";
            case KillFxModeEnum::PULSE:
                return "pulse";
            case KillFxModeEnum::LIGHTNING:
                return "lightning";
            case KillFxModeEnum::SHOCKWAVE:
                return "shockwave";
            default:
                return "disabled";
        }
    }

    bool operator==(const KillFxMode& other) const
    {
        return value == other.value;
    }
    bool operator!=(const KillFxMode& other) const
    {
        return value != other.value;
    }
    bool operator==(const std::string& str) const
    {
        return (std::string) * this == str;
    }
    bool operator!=(const std::string& str) const
    {
        return (std::string) * this != str;
    }
    bool operator==(const char* str) const
    {
        return (std::string) * this == str;
    }
    bool operator!=(const char* str) const
    {
        return (std::string) * this != str;
    }
};

// Bullet Impacts rendering style: the classic circle flash or CS2-style 3D
// impact boxes (the sv_showimpacts look).
enum class BulletImpactStyleEnum { FLASH, BOXES };

class BulletImpactStyle {
public:
    BulletImpactStyleEnum value = BulletImpactStyleEnum::FLASH;

    BulletImpactStyle() = default;
    BulletImpactStyle(BulletImpactStyleEnum val) : value(val)
    {
    }
    BulletImpactStyle(const std::string& str)
    {
        if (str == "boxes" || str == "cs2" || str == "cs2_boxes")
            value = BulletImpactStyleEnum::BOXES;
        else
            value = BulletImpactStyleEnum::FLASH;
    }
    BulletImpactStyle(const char* str) : BulletImpactStyle(std::string(str))
    {
    }

    operator std::string() const
    {
        switch (value) {
            case BulletImpactStyleEnum::BOXES:
                return "boxes";
            default:
                return "flash";
        }
    }

    bool operator==(const BulletImpactStyle& other) const
    {
        return value == other.value;
    }
    bool operator!=(const BulletImpactStyle& other) const
    {
        return value != other.value;
    }
    bool operator==(const std::string& str) const
    {
        return (std::string) * this == str;
    }
    bool operator!=(const std::string& str) const
    {
        return (std::string) * this != str;
    }
    bool operator==(const char* str) const
    {
        return (std::string) * this == str;
    }
    bool operator!=(const char* str) const
    {
        return (std::string) * this != str;
    }
};

enum class BindMode { TOGGLE, HOLD };

struct KeyBind {
    std::string setting_name;
    int key = -1;
    BindMode mode = BindMode::TOGGLE;
    bool is_slider = false;
    float slider_value = 0.0f;
    int slider_value_int = 0;

    // Transient execution state (not serialized)
    bool was_pressed = false;
    bool original_val_bool = false;
    float original_val_float = 0.0f;
    int original_val_int = 0;
    bool is_active_hold = false;

    bool operator==(const KeyBind& other) const = default;
};

// JSON serialization forward declarations
void to_json(nlohmann::json& j, const Color& color);
void from_json(const nlohmann::json& j, Color& color);
void to_json(nlohmann::json& j, const ChamsStyle& style);
void from_json(const nlohmann::json& j, ChamsStyle& style);
void to_json(nlohmann::json& j, const KeyBind& kb);
void from_json(const nlohmann::json& j, KeyBind& kb);
void to_json(nlohmann::json& j, const KillFxMode& mode);
void from_json(const nlohmann::json& j, KillFxMode& mode);
void to_json(nlohmann::json& j, const BulletImpactStyle& style);
void from_json(const nlohmann::json& j, BulletImpactStyle& style);

#define CONFIG_FIELDS                                                                                         \
    X(int, monitor_w, "monitor_w", 2560)                                                                      \
    X(int, monitor_h, "monitor_h", 1440)                                                                      \
    X(int, game_w, "game_w", 1280)                                                                            \
    X(int, game_h, "game_h", 960)                                                                             \
    X(std::string, scaling, "scaling", "")                                                                    \
    X(int, game_x, "game_x", 0)                                                                               \
    X(int, game_y, "game_y", 0)                                                                               \
    X(int, fps, "fps", 0)                                                                                     \
    X(bool, show_fps, "show_fps", true)                                                                       \
    X(float, fps_scale, "fps_scale", 1.0f)                                                                    \
    X(bool, vsync, "vsync", false)                                                                            \
    X(bool, extrapolate, "extrapolate", false)                                                                \
    X(Color, color_vis, "color_visible", Color(0.0f, 1.0f, 0.0f, 200.0f / 255.0f))                            \
    X(Color, color_vis_sec, "color_visible_sec", Color(1.0f, 1.0f, 0.0f, 0.8f))                               \
    X(Color, color_invis, "color_invisible", Color(1.0f, 0.0f, 0.0f, 200.0f / 255.0f))                        \
    X(Color, color_invis_sec, "color_invisible_sec", Color(1.0f, 0.5f, 0.0f, 0.8f))                           \
    X(std::string, maps_dir, "maps_dir", "./maps")                                                            \
    X(bool, hyprland_support, "hyprland_support", false)                                                      \
    X(int, monitor_index, "monitor_index", 0)                                                                 \
    X(std::string, gpu_preference, "gpu_preference", "default")                                               \
    X(std::string, vpk_path, "vpk_path", "auto")                                                              \
    X(bool, chams_enabled, "chams_enabled", false)                                                            \
    X(ChamsStyle, style_vis, "chams_style_visible", ChamsStyle(ChamsStyleEnum::DISABLED))                         \
    X(ChamsStyle, style_invis, "chams_style_hidden", ChamsStyle(ChamsStyleEnum::DISABLED))                        \
    X(bool, use_depth_prepass, "use_depth_prepass", true)                                                     \
    X(bool, use_bvh_fallback, "use_bvh_fallback", true)                                                       \
    X(bool, flat_chams_no_overlap, "flat_chams_no_overlap", true)                                             \
    X(bool, glow_enabled, "glow_enabled", false)                                                              \
    X(std::string, outline_mode, "outline_mode", "glow")                                                      \
    X(bool, glow_health_based, "glow_health_based", false)                                                    \
    X(Color, glow_color, "glow_color", Color(1.0f, 0.0f, 1.0f, 0.8f))                                         \
    X(Color, glow_health_start, "glow_health_start", Color(0.0f, 1.0f, 0.0f, 0.8f))                           \
    X(Color, glow_health_end, "glow_health_end", Color(1.0f, 0.0f, 0.0f, 0.8f))                               \
    X(float, glow_thickness, "glow_thickness", 0.0f)                                                          \
    X(float, glow_intensity, "glow_intensity", 1.0f)                                                          \
    X(bool, glow_pulse, "glow_pulse", false)                                                                  \
    X(float, glow_pulse_speed, "glow_pulse_speed", 2.0f)                                                      \
    X(bool, esp_smoke_check, "esp_smoke_check", true)                                                         \
    X(bool, esp_enabled, "esp_enabled", true)                                                                 \
    X(bool, esp_distance_limit_enabled, "esp_distance_limit_enabled", false)                                  \
    X(float, esp_max_distance, "esp_max_distance", 25.0f)                                                     \
    X(bool, esp_distance_fade_enabled, "esp_distance_fade_enabled", false)                                    \
    X(float, esp_distance_fade_start, "esp_distance_fade_start", 0.0f)                                        \
    X(float, esp_distance_fade_min_alpha, "esp_distance_fade_min_alpha", 0.0f)                                \
    X(bool, esp_distance_override, "esp_distance_override", false)                                            \
    X(bool, esp_skeleton, "esp_skeleton", true)                                                               \
    X(bool, esp_rounded_skeleton, "esp_rounded_skeleton", false)                                              \
    X(float, esp_skeleton_thickness, "esp_skeleton_thickness", 1.5f)                                          \
    X(bool, esp_skeleton_vis_delay_enabled, "esp_skeleton_vis_delay_enabled", false)                          \
    X(float, esp_skeleton_vis_delay_ms, "esp_skeleton_vis_delay_ms", 150.0f)                                  \
    X(Color, esp_skeleton_color_vis, "esp_skeleton_color_vis", Color(1.0f, 1.0f, 1.0f, 0.8f))                 \
    X(Color, esp_skeleton_color_invis, "esp_skeleton_color_invis", Color(1.0f, 0.0f, 0.0f, 0.8f))             \
    X(bool, esp_skeleton_glow, "esp_skeleton_glow", false)                                                    \
    X(Color, esp_skeleton_glow_color, "esp_skeleton_glow_color", Color(0.0f, 1.0f, 1.0f, 0.8f))               \
    X(bool, esp_skeleton_glow_pulse, "esp_skeleton_glow_pulse", false)                                        \
    X(float, esp_skeleton_glow_pulse_speed, "esp_skeleton_glow_pulse_speed", 2.0f)                            \
    X(float, esp_skeleton_glow_intensity, "esp_skeleton_glow_intensity", 1.0f)                                \
    X(float, esp_skeleton_glow_thickness, "esp_skeleton_glow_thickness", 1.5f)                                \
    X(bool, esp_skeleton_glow_health_based, "esp_skeleton_glow_health_based", false)                          \
    X(Color, esp_skeleton_glow_health_start, "esp_skeleton_glow_health_start", Color(0.0f, 1.0f, 0.0f, 0.8f)) \
    X(Color, esp_skeleton_glow_health_end, "esp_skeleton_glow_health_end", Color(1.0f, 0.0f, 0.0f, 0.8f))     \
    X(bool, esp_box, "esp_box", false)                                                                        \
    X(float, esp_box_thickness, "esp_box_thickness", 1.5f)                                                    \
    X(Color, esp_box_color, "esp_box_color", Color(1.0f, 1.0f, 1.0f, 0.8f))                                   \
    X(bool, esp_box_outline, "esp_box_outline", true)                                                         \
    X(int, esp_box_mode, "esp_box_mode", 1)                                                                   \
    X(float, esp_box_static_w, "esp_box_static_w", 41500.0f)                                                  \
    X(float, esp_box_static_h, "esp_box_static_h", 76200.0f)                                                  \
    X(bool, esp_box_hp_visible, "esp_box_hp_visible", true)                                                   \
    X(bool, esp_box_hp_occluded, "esp_box_hp_occluded", true)                                                 \
    X(bool, esp_health_bar, "esp_health_bar", true)                                                          \
    X(bool, esp_health_bar_gradient, "esp_health_bar_gradient", true)                                         \
    X(Color, esp_health_bar_color, "esp_health_bar_color", Color(0.0f, 1.0f, 0.0f, 0.8f))                     \
    X(Color, esp_health_bar_gradient_start, "esp_health_bar_gradient_start", Color(0.0f, 1.0f, 0.0f, 0.8f))   \
    X(Color, esp_health_bar_gradient_end, "esp_health_bar_gradient_end", Color(1.0f, 0.0f, 0.0f, 0.8f))       \
    X(bool, esp_health_bar_outline, "esp_health_bar_outline", true)                                           \
    X(float, esp_health_bar_thickness, "esp_health_bar_thickness", 2.0f)                                      \
    X(bool, esp_bomb_bar, "esp_bomb_bar", true)                                                               \
    X(bool, esp_names, "esp_names", true)                                                                     \
    X(bool, esp_armor_bar, "esp_armor_bar", false)                                                             \
    X(float, esp_armor_bar_thickness, "esp_armor_bar_thickness", 2.0f)                                        \
    X(bool, esp_armor_bar_gradient, "esp_armor_bar_gradient", false)                                          \
    X(Color, esp_armor_bar_color, "esp_armor_bar_color", Color(0.0f, 162.0f / 255.0f, 232.0f / 255.0f, 1.0f)) \
    X(Color, esp_armor_bar_gradient_start, "esp_armor_bar_gradient_start", Color(0.0f, 162.0f / 255.0f, 232.0f / 255.0f, 1.0f)) \
    X(Color, esp_armor_bar_gradient_end, "esp_armor_bar_gradient_end", Color(0.0f, 80.0f / 255.0f, 120.0f / 255.0f, 1.0f)) \
    X(bool, esp_armor_bar_outline, "esp_armor_bar_outline", true)                                             \
    X(bool, esp_ammo_bar, "esp_ammo_bar", false)                                                              \
    X(float, esp_ammo_bar_thickness, "esp_ammo_bar_thickness", 2.0f)                                          \
    X(bool, esp_ammo_bar_gradient, "esp_ammo_bar_gradient", false)                                            \
    X(Color, esp_ammo_bar_color, "esp_ammo_bar_color", Color(235.0f / 255.0f, 180.0f / 255.0f, 0.0f, 1.0f))   \
    X(Color, esp_ammo_bar_gradient_start, "esp_ammo_bar_gradient_start", Color(235.0f / 255.0f, 180.0f / 255.0f, 0.0f, 1.0f)) \
    X(Color, esp_ammo_bar_gradient_end, "esp_ammo_bar_gradient_end", Color(150.0f / 255.0f, 110.0f / 255.0f, 0.0f, 1.0f)) \
    X(bool, esp_ammo_bar_outline, "esp_ammo_bar_outline", true)                                               \
    X(bool, esp_flags, "esp_flags", false)                                                                    \
    X(Color, esp_flags_color, "esp_flags_color", Color(255.0f / 255.0f, 255.0f / 255.0f, 255.0f / 255.0f, 1.0f)) \
    X(bool, esp_flags_reload, "esp_flags_reload", true)                                                       \
    X(bool, esp_flags_scoped, "esp_flags_scoped", true)                                                       \
    X(bool, esp_flags_defuser, "esp_flags_defuser", true)                                                     \
    X(bool, esp_flags_helmet, "esp_flags_helmet", true)                                                       \
    X(bool, esp_flags_kevlar, "esp_flags_kevlar", true)                                                       \
    X(bool, esp_flags_distance, "esp_flags_distance", true)                                                   \
    X(bool, esp_flags_hp, "esp_flags_hp", true)                                                               \
    X(bool, esp_flags_bomb, "esp_flags_bomb", true)                                                           \
    X(Color, esp_flags_bomb_color, "esp_flags_bomb_color", Color(1.0f, 64.0f / 255.0f, 64.0f / 255.0f, 1.0f)) \
    X(bool, esp_flags_flashed, "esp_flags_flashed", true)                                                     \
    X(Color, esp_flags_flashed_color, "esp_flags_flashed_color", Color(1.0f, 1.0f, 0.0f, 1.0f))               \
    X(bool, esp_head_circle, "esp_head_circle", false)                                                        \
    X(bool, esp_head_circle_filled, "esp_head_circle_filled", false)                                          \
    X(bool, esp_head_circle_health_color, "esp_head_circle_health_color", false)                              \
    X(bool, esp_head_circle_world_size, "esp_head_circle_world_size", false)                                  \
    X(bool, esp_head_circle_outline, "esp_head_circle_outline", false)                                        \
    X(float, esp_head_circle_opacity, "esp_head_circle_opacity", 1.0f)                                        \
    X(bool, esp_head_circle_wall_aware, "esp_head_circle_wall_aware", false)                                  \
    X(float, esp_head_circle_occluded_opacity, "esp_head_circle_occluded_opacity", 0.35f)                     \
    X(bool, esp_head_circle_peek_flash, "esp_head_circle_peek_flash", false)                                  \
    X(float, esp_head_circle_ghost_seconds, "esp_head_circle_ghost_seconds", 3.0f)                            \
    X(bool, esp_step_circles, "esp_step_circles", false)                                                      \
    X(Color, esp_step_circles_color, "esp_step_circles_color", Color(64.0f / 255.0f, 160.0f / 255.0f, 1.0f, 0.85f)) \
    X(float, esp_step_circles_radius, "esp_step_circles_radius", 28.0f)                                      \
    X(float, esp_step_circles_max_age, "esp_step_circles_max_age", 3.0f)                                      \
    X(bool, esp_step_circles_fade, "esp_step_circles_fade", true)                                             \
    X(bool, esp_step_circles_filled, "esp_step_circles_filled", false)                                        \
    X(bool, esp_step_circles_hide_while_walking, "esp_step_circles_hide_while_walking", true)                 \
    X(bool, esp_step_circles_scale_walk, "esp_step_circles_scale_walk", true)                                 \
    X(float, esp_noise_max_radius, "esp_noise_max_radius", 90.0f)                                              \
    X(bool, esp_noise_movement, "esp_noise_movement", false)                                                  \
    X(bool, esp_noise_shooting, "esp_noise_shooting", true)                                                   \
    X(bool, esp_noise_reload, "esp_noise_reload", true)                                                       \
    X(bool, esp_noise_landing, "esp_noise_landing", true)                                                     \
    X(bool, esp_noise_scope, "esp_noise_scope", true)                                                         \
    X(bool, esp_noise_plant, "esp_noise_plant", true)                                                         \
    X(bool, esp_noise_defuse, "esp_noise_defuse", true)                                                       \
    X(bool, esp_noise_show_plant_defuse_text, "esp_noise_show_plant_defuse_text", true)                       \
    X(Color, esp_noise_movement_color, "esp_noise_movement_color", Color(64.0f / 255.0f, 160.0f / 255.0f, 1.0f, 0.85f)) \
    X(Color, esp_noise_shooting_color, "esp_noise_shooting_color", Color(64.0f / 255.0f, 160.0f / 255.0f, 1.0f, 0.85f)) \
    X(Color, esp_noise_reload_color, "esp_noise_reload_color", Color(64.0f / 255.0f, 160.0f / 255.0f, 1.0f, 0.85f)) \
    X(Color, esp_noise_landing_color, "esp_noise_landing_color", Color(64.0f / 255.0f, 160.0f / 255.0f, 1.0f, 0.85f)) \
    X(Color, esp_noise_scope_color, "esp_noise_scope_color", Color(64.0f / 255.0f, 160.0f / 255.0f, 1.0f, 0.85f)) \
    X(Color, esp_noise_plant_color, "esp_noise_plant_color", Color(64.0f / 255.0f, 160.0f / 255.0f, 1.0f, 0.85f)) \
    X(Color, esp_noise_defuse_color, "esp_noise_defuse_color", Color(64.0f / 255.0f, 160.0f / 255.0f, 1.0f, 0.85f)) \
    X(bool, esp_weapon_icons, "esp_weapon_icons", true)                                                       \
    X(float, weapon_icon_opacity, "weapon_icon_opacity", 1.0f) \
    X(Color, weapon_icon_background, "weapon_icon_background", Color(0.0f, 0.0f, 0.0f, 0.0f)) \
    X(bool, esp_offscreen_arrows, "esp_offscreen_arrows", false)                                              \
    X(Color, esp_offscreen_arrows_color, "esp_offscreen_arrows_color", Color(1.0f, 1.0f, 1.0f, 0.8f))         \
    X(Color, esp_offscreen_arrows_exposed_color, "esp_offscreen_arrows_exposed_color", Color(1.0f, 26.0f / 255.0f, 26.0f / 255.0f, 0.9f)) \
    X(float, esp_offscreen_arrows_size, "esp_offscreen_arrows_size", 14.0f)                                   \
    X(float, esp_offscreen_arrows_radius, "esp_offscreen_arrows_radius", 150.0f)                              \
    X(float, esp_offscreen_arrows_max_dist, "esp_offscreen_arrows_max_dist", 0.0f)                            \
    X(bool, esp_offscreen_arrows_distance, "esp_offscreen_arrows_distance", true)                             \
    X(bool, crosshair_overlay_enabled, "crosshair_overlay_enabled", false)                                    \
    X(bool, crosshair_overlay_force, "crosshair_overlay_force", false)                                        \
    X(bool, crosshair_overlay_snipers_enabled, "crosshair_overlay_snipers_enabled", true)                     \
    X(bool, crosshair_overlay_recoil_enabled, "crosshair_overlay_recoil_enabled", false)                      \
    X(bool, crosshair_overlay_only_shooting, "crosshair_overlay_only_shooting", true)                         \
    X(int, crosshair_overlay_shot_delay, "crosshair_overlay_shot_delay", 0)                                   \
    X(float, crosshair_overlay_offset_x, "crosshair_overlay_offset_x", 0.0f)                                  \
    X(float, crosshair_overlay_offset_y, "crosshair_overlay_offset_y", 0.0f)                                  \
    X(float, crosshair_overlay_gap, "crosshair_overlay_gap", 0.0f)                                            \
    X(float, crosshair_overlay_size, "crosshair_overlay_size", 0.0f)                                          \
    X(float, crosshair_overlay_thickness, "crosshair_overlay_thickness", 0.0f)                                \
    X(Color, crosshair_overlay_color, "crosshair_overlay_color", Color(0.0f, 1.0f, 0.0f, 1.0f))               \
    X(bool, crosshair_esp_enabled, "crosshair_esp_enabled", false)                                             \
    X(Color, crosshair_esp_color, "crosshair_esp_color", Color(1.0f, 0.0f, 0.0f, 1.0f))                       \
    X(float, crosshair_esp_fov, "crosshair_esp_fov", 5.0f)                                                    \
    X(bool, proximity_beep_enabled, "proximity_beep_enabled", false)                                          \
    X(float, proximity_beep_max_range, "proximity_beep_max_range", 30.0f)                                     \
    X(float, proximity_beep_fov, "proximity_beep_fov", 110.0f)                                                \
    X(float, proximity_beep_fast_interval, "proximity_beep_fast_interval", 0.15f)                              \
    X(float, proximity_beep_slow_interval, "proximity_beep_slow_interval", 1.0f)                              \
    X(float, proximity_beep_volume, "proximity_beep_volume", 0.8f)                                            \
    X(bool, c4_render_in_world, "c4_render_in_world", true)                                                   \
    X(bool, c4_timer_gradient_enabled, "c4_timer_gradient_enabled", true)                                     \
    X(Color, c4_timer_gradient_start, "c4_timer_gradient_start", Color(0.0f, 1.0f, 0.0f, 1.0f))               \
    X(Color, c4_timer_gradient_end, "c4_timer_gradient_end", Color(1.0f, 0.0f, 0.0f, 1.0f))                   \
    X(Color, c4_timer_color, "c4_timer_color", Color(0.0f, 1.0f, 0.0f, 1.0f))                                 \
    X(Color, bomb_bar_bg_color, "bomb_bar_bg_color", Color(0.094f, 0.094f, 0.106f, 0.902f))                    \
    X(Color, bomb_bar_color, "bomb_bar_color", Color(0.0f, 1.0f, 0.0f, 1.0f))                                 \
    X(bool, bomb_bar_gradient_enabled, "bomb_bar_gradient_enabled", true)                                     \
    X(Color, bomb_bar_gradient_start, "bomb_bar_gradient_start", Color(0.0f, 1.0f, 0.0f, 1.0f))               \
    X(Color, bomb_bar_gradient_end, "bomb_bar_gradient_end", Color(1.0f, 0.0f, 0.0f, 1.0f))                   \
    X(bool, debug_game_reader, "debug_game_reader", true)                                                     \
    X(bool, focus_check_enabled, "focus_check_enabled", true)                                                 \
    X(bool, hitmarker, "hitmarker", true)                                                                     \
    X(bool, kill_show_text, "kill_show_text", true) \
    X(bool, kill_headshot_text, "kill_headshot_text", false) \
    X(bool, damage_fx_enabled, "damage_fx_enabled", false)                                                    \
    X(KillFxMode, kill_effect, "kill_effect", KillFxMode(KillFxModeEnum::CHAMS))                               \
    X(bool, kill_chams_enabled, "kill_chams_enabled", true)                                                   \
    X(ChamsStyle, kill_chams_style, "kill_chams_style", ChamsStyle(ChamsStyleEnum::GLASS))                 \
    X(Color, kill_chams_color, "kill_chams_color", Color(1.0f, 1.0f, 1.0f, 1.0f))                             \
    X(Color, kill_chams_color_sec, "kill_chams_color_sec", Color(0.25f, 0.8f, 1.0f, 1.0f))                   \
    X(float, kill_chams_fade_time, "kill_chams_fade_time", 1.0f)                                              \
    X(bool, kill_chams_heaven, "kill_chams_heaven", true)                                                     \
    X(float, kill_chams_heaven_speed, "kill_chams_heaven_speed", 50.0f)                                       \
    X(Color, kill_particles_color, "kill_particles_color", Color(1.0f, 0.15f, 0.15f, 1.0f))                   \
    X(int, kill_particles_count, "kill_particles_count", 30)                                                  \
    X(float, kill_particles_speed, "kill_particles_speed", 2.0f)                                              \
    X(float, kill_particles_lifetime, "kill_particles_lifetime", 1.2f)                                        \
    X(float, kill_particles_size, "kill_particles_size", 3.0f)                                                \
    X(float, kill_particles_gravity, "kill_particles_gravity", -300.0f)                                       \
    X(Color, kill_pulse_color, "kill_pulse_color", Color(1.0f, 0.0f, 0.0f, 0.4f))                             \
    X(float, kill_pulse_duration, "kill_pulse_duration", 0.6f)                                                \
    X(float, kill_pulse_intensity, "kill_pulse_intensity", 1.0f)                                              \
    X(Color, kill_lightning_color, "kill_lightning_color", Color(0.3f, 0.8f, 1.0f, 1.0f))                     \
    X(float, kill_lightning_duration, "kill_lightning_duration", 0.4f)                                         \
    X(float, kill_lightning_width, "kill_lightning_width", 4.0f)                                              \
    X(int, kill_lightning_count, "kill_lightning_count", 3)                                                   \
    X(Color, kill_shockwave_color, "kill_shockwave_color", Color(0.0f, 1.0f, 0.7f, 0.8f))                     \
    X(float, kill_shockwave_duration, "kill_shockwave_duration", 0.5f)                                        \
    X(float, kill_shockwave_max_radius, "kill_shockwave_max_radius", 120.0f)                                  \
    X(float, kill_shockwave_width, "kill_shockwave_width", 3.0f)                                              \
    X(bool, esp_dropped_items, "esp_dropped_items", true)                                                     \
    X(bool, esp_dropped_items_text, "esp_dropped_items_text", true)                                           \
    X(bool, esp_dropped_items_icons, "esp_dropped_items_icons", false)                                         \
    X(Color, esp_dropped_items_color, "esp_dropped_items_color", Color(1.0f, 1.0f, 1.0f, 0.8f))               \
    X(float, esp_dropped_items_max_dist, "esp_dropped_items_max_dist", 1000.0f)                               \
    X(bool, debug_gpu_profiling, "debug_gpu_profiling", false)                                                \
    X(bool, debug_visibility, "debug_visibility", false)                                                      \
    X(float, gamereader_local_ms, "gamereader_local_ms", 2.0f)                                                \
    X(float, gamereader_players_ms, "gamereader_players_ms", 2.0f)                                            \
    X(float, gamereader_entities_ms, "gamereader_entities_ms", 16.67f)                                        \
    X(bool, map_visualizer_enabled, "map_visualizer_enabled", false)                                          \
    X(bool, map_visualizer_depth_tested, "map_visualizer_depth_tested", true)                                 \
    X(Color, map_visualizer_color, "map_visualizer_color", Color(0.0f, 0.784f, 0.392f, 0.47f))                \
    X(bool, anti_flash, "anti_flash", false)                                                                  \
    X(float, anti_flash_alpha_start, "anti_flash_alpha_start", 200.0f)                                        \
    X(float, anti_flash_alpha_end, "anti_flash_alpha_end", 125.0f)                                            \
    X(Color, anti_flash_background_color, "anti_flash_background_color", Color(0.0f, 0.0f, 0.0f, 1.0f))       \
    X(float, anti_flash_background_opacity, "anti_flash_background_opacity", 1.0f)                            \
    X(bool, anti_flash_wireframe, "anti_flash_wireframe", true)                                               \
    X(bool, anti_flash_wireframe_depth_tested, "anti_flash_wireframe_depth_tested", true)                     \
    X(float, anti_flash_wireframe_thickness, "anti_flash_wireframe_thickness", 3.0f)                          \
    X(Color, anti_flash_wireframe_color, "anti_flash_wireframe_color", Color(0.0f, 200.0f / 255.0f, 197.0f / 255.0f, 1.0f)) \
    X(bool, anti_flash_crosshair, "anti_flash_crosshair", true)                                               \
    X(Color, anti_flash_crosshair_color, "anti_flash_crosshair_color", Color(1.0f, 1.0f, 1.0f, 1.0f))         \
    X(float, anti_flash_crosshair_radius, "anti_flash_crosshair_radius", 3.0f)                                \
    X(bool, anti_flash_chams, "anti_flash_chams", true)                                                       \
    X(ChamsStyle, anti_flash_chams_style, "anti_flash_chams_style", ChamsStyle(ChamsStyleEnum::SOLID))        \
    X(bool, chams_hide_when_flashed, "chams_hide_when_flashed", false) \
    X(bool, glow_hide_when_flashed, "glow_hide_when_flashed", false) \
    X(bool, esp_hide_when_flashed, "esp_hide_when_flashed", false) \
    X(bool, chams_hide_while_shooting, "chams_hide_while_shooting", false) \
    X(bool, glow_hide_while_shooting, "glow_hide_while_shooting", false) \
    X(bool, esp_hide_while_shooting, "esp_hide_while_shooting", false) \
    X(bool, esp_damage_reveal, "esp_damage_reveal", false) \
    X(float, esp_damage_reveal_seconds, "esp_damage_reveal_seconds", 1.0f) \
    X(bool, esp_damage_reveal_dynamic, "esp_damage_reveal_dynamic", true) \
    X(bool, esp_damage_reveal_fade, "esp_damage_reveal_fade", true) \
    X(bool, esp_hit_reveal, "esp_hit_reveal", false) \
    X(bool, chams_damage_reveal, "chams_damage_reveal", false) \
    X(float, chams_damage_reveal_seconds, "chams_damage_reveal_seconds", 1.0f) \
    X(bool, chams_damage_reveal_dynamic, "chams_damage_reveal_dynamic", true) \
    X(bool, chams_damage_reveal_fade, "chams_damage_reveal_fade", true) \
    X(bool, chams_hit_reveal, "chams_hit_reveal", false) \
    X(float, chams_hit_reveal_seconds, "chams_hit_reveal_seconds", 1.0f) \
    X(bool, chams_hit_reveal_dynamic, "chams_hit_reveal_dynamic", true) \
    X(bool, chams_hit_reveal_fade, "chams_hit_reveal_fade", true) \
    X(bool, glow_damage_reveal, "glow_damage_reveal", false) \
    X(float, glow_damage_reveal_seconds, "glow_damage_reveal_seconds", 1.0f) \
    X(bool, glow_damage_reveal_dynamic, "glow_damage_reveal_dynamic", true) \
    X(bool, glow_damage_reveal_fade, "glow_damage_reveal_fade", true) \
    X(Color, glow_damage_reveal_color, "glow_damage_reveal_color", Color(1.0f, 0.25f, 0.1f, 1.0f)) \
    X(float, glow_damage_reveal_thickness, "glow_damage_reveal_thickness", 1.5f) \
    X(bool, glow_hit_reveal, "glow_hit_reveal", false) \
    X(float, glow_hit_reveal_seconds, "glow_hit_reveal_seconds", 1.0f) \
    X(bool, glow_hit_reveal_dynamic, "glow_hit_reveal_dynamic", true) \
    X(bool, glow_hit_reveal_fade, "glow_hit_reveal_fade", true) \
    X(Color, glow_hit_reveal_color, "glow_hit_reveal_color", Color(0.2f, 0.8f, 1.0f, 1.0f)) \
    X(float, glow_hit_reveal_thickness, "glow_hit_reveal_thickness", 1.5f) \
    X(KillFxMode, hit_effect_mode, "hit_effect_mode", KillFxMode(KillFxModeEnum::DISABLED)) \
    X(Color, hit_effect_color, "hit_effect_color", Color(1.0f, 0.15f, 0.15f, 1.0f)) \
    X(int, hit_effect_count, "hit_effect_count", 14) \
    X(float, hit_effect_size, "hit_effect_size", 2.5f) \
    X(float, hit_effect_speed, "hit_effect_speed", 1.0f) \
    X(float, hit_effect_lifetime, "hit_effect_lifetime", 0.45f) \
    X(float, hit_effect_gravity, "hit_effect_gravity", -300.0f) \
    X(ChamsStyle, hit_chams_style, "hit_chams_style", ChamsStyle(ChamsStyleEnum::GLASS)) \
    X(Color, hit_chams_color, "hit_chams_color", Color(1.0f, 1.0f, 1.0f, 1.0f)) \
    X(Color, hit_chams_color_sec, "hit_chams_color_sec", Color(0.25f, 0.8f, 1.0f, 1.0f)) \
    X(float, hit_chams_fade_time, "hit_chams_fade_time", 0.6f) \
    X(Color, hit_pulse_color, "hit_pulse_color", Color(1.0f, 0.0f, 0.0f, 0.4f)) \
    X(float, hit_pulse_duration, "hit_pulse_duration", 0.35f) \
    X(float, hit_pulse_intensity, "hit_pulse_intensity", 1.0f) \
    X(Color, hit_lightning_color, "hit_lightning_color", Color(0.3f, 0.8f, 1.0f, 1.0f)) \
    X(float, hit_lightning_duration, "hit_lightning_duration", 0.25f) \
    X(float, hit_lightning_width, "hit_lightning_width", 3.0f) \
    X(int, hit_lightning_count, "hit_lightning_count", 1) \
    X(Color, hit_shockwave_color, "hit_shockwave_color", Color(0.0f, 1.0f, 0.7f, 0.8f)) \
    X(float, hit_shockwave_duration, "hit_shockwave_duration", 0.3f) \
    X(float, hit_shockwave_max_radius, "hit_shockwave_max_radius", 50.0f) \
    X(float, hit_shockwave_width, "hit_shockwave_width", 2.0f) \
    X(bool, bullet_impacts_enabled, "bullet_impacts_enabled", false) \
    X(Color, bullet_impacts_color, "bullet_impacts_color", Color(1.0f, 1.0f, 1.0f, 1.0f)) \
    X(float, bullet_impacts_size, "bullet_impacts_size", 3.0f) \
    X(float, bullet_impacts_duration, "bullet_impacts_duration", 0.06f) \
    X(bool, bullet_impacts_hold, "bullet_impacts_hold", false) \
    X(float, bullet_impacts_hold_gap, "bullet_impacts_hold_gap", 0.2f) \
    X(bool, bullet_impacts_spread, "bullet_impacts_spread", false) \
    X(BulletImpactStyle, bullet_impacts_style, "bullet_impacts_style", BulletImpactStyle(BulletImpactStyleEnum::FLASH)) \
    X(Color, bullet_impacts_box_color, "bullet_impacts_box_color", Color(1.0f, 0.0f, 0.0f, 1.0f)) \
    X(float, bullet_impacts_box_size, "bullet_impacts_box_size", 4.0f) \
    X(bool, hit_fx_local_only, "hit_fx_local_only", true) \
    X(bool, smoke_wireframe, "smoke_wireframe", false)                                                        \
    X(Color, smoke_wireframe_color, "smoke_wireframe_color", Color(0.45f, 0.65f, 1.0f, 0.8f))                 \
    X(bool, through_smokes, "through_smokes", false)                                                          \
    X(Color, through_smokes_color, "through_smokes_color", Color(0.0f, 0.784f, 0.392f, 0.6f))                 \
    X(Color, grenade_color_he, "grenade_color_he", Color(1.0f, 0.267f, 0.267f, 0.8f))                         \
    X(Color, grenade_color_flash, "grenade_color_flash", Color(1.0f, 0.839f, 0.2f, 0.8f))                     \
    X(Color, grenade_color_smoke, "grenade_color_smoke", Color(0.2f, 0.8f, 0.467f, 0.8f))                     \
    X(Color, grenade_color_molotov, "grenade_color_molotov", Color(1.0f, 0.549f, 0.102f, 0.8f))               \
    X(Color, grenade_color_decoy, "grenade_color_decoy", Color(0.4f, 0.6f, 0.8f, 0.8f))                       \
    X(Color, inferno_outline_color, "inferno_outline_color", Color(0.824f, 0.314f, 0.0f, 0.7f))               \
    X(Color, inferno_fill_color, "inferno_fill_color", Color(1.0f, 0.588f, 0.0f, 0.235f))                     \
    X(bool, inferno_spread_enabled, "inferno_spread_enabled", true)                                           \
    X(bool, draw_grenade_trajectory, "draw_grenade_trajectory", true)                                         \
    X(float, trajectory_thickness, "trajectory_thickness", 2.0f)                                              \
    X(float, trajectory_bounce_size, "trajectory_bounce_size", 2.0f)                                          \
    X(float, trajectory_detonation_radius, "trajectory_detonation_radius", 15.0f)                             \
    X(float, trajectory_fade_time, "trajectory_fade_time", 1.5f)                                              \
    X(bool, trajectory_show_through_walls, "trajectory_show_through_walls", false)                            \
    X(bool, grenade_warnings_enabled, "grenade_warnings_enabled", true)                                       \
    X(float, world_timer_scale, "world_timer_scale", 1.0f)                                                   \
    X(bool, world_timer_minimal_style, "world_timer_minimal_style", false)                                   \
    X(float, grenade_warning_distance, "grenade_warning_distance", 500.0f)                                    \
    X(float, grenade_warning_time, "grenade_warning_time", 2.5f)                                              \
    X(bool, grenade_helper_enabled, "grenade_helper_enabled", true)                                           \
    X(bool, grenade_creator_enabled, "grenade_creator_enabled", false)                                         \
    X(bool, grenade_solver_enabled, "grenade_solver_enabled", true)                                           \
    X(float, grenade_solver_max_range, "grenade_solver_max_range", 2500.0f)                                   \
    X(float, grenade_solver_still_time, "grenade_solver_still_time", 0.5f)                                    \
    X(bool, grenade_solver_show_zones, "grenade_solver_show_zones", true)                                     \
    X(bool, grenade_solver_show_trajectory, "grenade_solver_show_trajectory", false)                          \
    X(bool, grenade_solver_location_creator_enabled, "grenade_solver_location_creator_enabled", true)         \
    X(bool, grenade_helper_show_trace_lines, "grenade_helper_show_trace_lines", true)                         \
    X(bool, grenade_helper_vischeck, "grenade_helper_vischeck", true)                                         \
    X(int,  grenade_solver_create_bind, "grenade_solver_create_bind", 1001) /* GLFW mouse button 2 (Right Click) */  \
    X(float, grenade_helper_aim_radius, "grenade_helper_aim_radius", 4.1f)                                     \
    X(float, grenade_helper_aim_fov, "grenade_helper_aim_fov", 30.0f)                                         \
    X(float, grenade_helper_render_distance, "grenade_helper_render_distance", 2500.0f)                       \
    X(bool, grenade_helper_auto_aim, "grenade_helper_auto_aim", true)                                         \
    X(bool, grenade_helper_auto_throw, "grenade_helper_auto_throw", true)                                     \
    X(bool, grenade_helper_active, "grenade_helper_active", false)                                            \
    X(float, grenade_helper_aim_speed, "grenade_helper_aim_speed", 75.0f)                                     \
    X(bool, trigger_enabled, "trigger_enabled", false)                                                        \
    X(bool, trigger_active, "trigger_active", false)                                                          \
    X(bool, trigger_simple_mode, "trigger_simple_mode", false)                                                \
    X(int, trigger_mode, "trigger_mode", 0)                                                                   \
    X(float, trigger_hitchance, "trigger_hitchance", 75.0f)                                                   \
    X(float, trigger_light_pistols_hitchance, "trigger_light_pistols_hitchance", 70.0f)                       \
    X(float, trigger_heavy_pistols_hitchance, "trigger_heavy_pistols_hitchance", 70.0f)                       \
    X(float, trigger_rifles_hitchance, "trigger_rifles_hitchance", 60.0f)                                     \
    X(float, trigger_smgs_hitchance, "trigger_smgs_hitchance", 55.0f)                                         \
    X(float, trigger_scout_hitchance, "trigger_scout_hitchance", 85.0f)                                       \
    X(float, trigger_scout_noscope_hitchance, "trigger_scout_noscope_hitchance", 30.0f)                       \
    X(float, trigger_awp_hitchance, "trigger_awp_hitchance", 85.0f)                                           \
    X(float, trigger_awp_noscope_hitchance, "trigger_awp_noscope_hitchance", 30.0f)                           \
    X(float, trigger_auto_snipers_hitchance, "trigger_auto_snipers_hitchance", 85.0f)                         \
    X(float, trigger_auto_snipers_noscope_hitchance, "trigger_auto_snipers_noscope_hitchance", 30.0f)         \
    X(float, trigger_shotguns_hitchance, "trigger_shotguns_hitchance", 65.0f)                                 \
    X(float, trigger_lmgs_hitchance, "trigger_lmgs_hitchance", 50.0f)                                         \
    X(float, trigger_others_hitchance, "trigger_others_hitchance", 50.0f)                                     \
    X(int, trigger_validation_mode, "trigger_validation_mode", 0)                                             \
    X(bool, trigger_show_hitboxes, "trigger_show_hitboxes", false)                                            \
    X(bool, smoke_check, "smoke_check", true)                                                                 \
    X(bool, vischeck, "vischeck", true)                                                                       \
    X(bool, aim_enabled, "aim_enabled", false)                                                                \
    X(bool, aim_active, "aim_active", false)                                                                  \
    X(bool, trigger_magnet, "trigger_magnet", false)                                                          \
    X(float, aim_fov, "aim_fov", 2.5f)                                                                        \
    X(float, aim_smooth, "aim_smooth", 35.0f)                                                                 \
    X(int, aim_smooth_type, "aim_smooth_type", 1)                                                             \
    X(bool, aim_random_smooth, "aim_random_smooth", true)                                                     \
    X(float, aim_randomize_smooth_x, "aim_randomize_smooth_x", 2.0f)                                          \
    X(float, aim_randomize_smooth_y, "aim_randomize_smooth_y", 2.0f)                                          \
    X(float, aim_light_pistols_fov, "aim_light_pistols_fov", 2.5f)                                            \
    X(float, aim_light_pistols_smooth, "aim_light_pistols_smooth", 35.0f)                                     \
    X(bool, aim_light_pistols_rcs, "aim_light_pistols_rcs", true)                                             \
    X(float, aim_light_pistols_rcs_scale_x, "aim_light_pistols_rcs_scale_x", 0.8f)                            \
    X(float, aim_light_pistols_rcs_scale_y, "aim_light_pistols_rcs_scale_y", 0.8f)                            \
    X(float, aim_light_pistols_rcs_smooth, "aim_light_pistols_rcs_smooth", 30.0f)                             \
    X(float, aim_heavy_pistols_fov, "aim_heavy_pistols_fov", 2.5f)                                            \
    X(float, aim_heavy_pistols_smooth, "aim_heavy_pistols_smooth", 35.0f)                                     \
    X(bool, aim_heavy_pistols_rcs, "aim_heavy_pistols_rcs", true)                                             \
    X(float, aim_heavy_pistols_rcs_scale_x, "aim_heavy_pistols_rcs_scale_x", 0.8f)                            \
    X(float, aim_heavy_pistols_rcs_scale_y, "aim_heavy_pistols_rcs_scale_y", 0.8f)                            \
    X(float, aim_heavy_pistols_rcs_smooth, "aim_heavy_pistols_rcs_smooth", 30.0f)                             \
    X(float, aim_rifles_fov, "aim_rifles_fov", 2.5f)                                                          \
    X(float, aim_rifles_smooth, "aim_rifles_smooth", 35.0f)                                                   \
    X(bool, aim_rifles_rcs, "aim_rifles_rcs", true)                                                           \
    X(float, aim_rifles_rcs_scale_x, "aim_rifles_rcs_scale_x", 0.8f)                                          \
    X(float, aim_rifles_rcs_scale_y, "aim_rifles_rcs_scale_y", 0.8f)                                          \
    X(float, aim_rifles_rcs_smooth, "aim_rifles_rcs_smooth", 30.0f)                                           \
    X(float, aim_smgs_fov, "aim_smgs_fov", 2.5f)                                                              \
    X(float, aim_smgs_smooth, "aim_smgs_smooth", 35.0f)                                                       \
    X(bool, aim_smgs_rcs, "aim_smgs_rcs", true)                                                               \
    X(float, aim_smgs_rcs_scale_x, "aim_smgs_rcs_scale_x", 0.8f)                                              \
    X(float, aim_smgs_rcs_scale_y, "aim_smgs_rcs_scale_y", 0.8f)                                              \
    X(float, aim_smgs_rcs_smooth, "aim_smgs_rcs_smooth", 30.0f)                                               \
    X(float, aim_scout_fov, "aim_scout_fov", 2.5f)                                                            \
    X(float, aim_scout_smooth, "aim_scout_smooth", 35.0f)                                                     \
    X(bool, aim_scout_rcs, "aim_scout_rcs", true)                                                             \
    X(float, aim_scout_rcs_scale_x, "aim_scout_rcs_scale_x", 0.8f)                                            \
    X(float, aim_scout_rcs_scale_y, "aim_scout_rcs_scale_y", 0.8f)                                            \
    X(float, aim_scout_rcs_smooth, "aim_scout_rcs_smooth", 30.0f)                                             \
    X(float, aim_awp_fov, "aim_awp_fov", 2.5f)                                                                \
    X(float, aim_awp_smooth, "aim_awp_smooth", 35.0f)                                                         \
    X(bool, aim_awp_rcs, "aim_awp_rcs", true)                                                                 \
    X(float, aim_awp_rcs_scale_x, "aim_awp_rcs_scale_x", 0.8f)                                                \
    X(float, aim_awp_rcs_scale_y, "aim_awp_rcs_scale_y", 0.8f)                                                \
    X(float, aim_awp_rcs_smooth, "aim_awp_rcs_smooth", 30.0f)                                                 \
    X(float, aim_auto_snipers_fov, "aim_auto_snipers_fov", 2.5f)                                              \
    X(float, aim_auto_snipers_smooth, "aim_auto_snipers_smooth", 35.0f)                                       \
    X(bool, aim_auto_snipers_rcs, "aim_auto_snipers_rcs", true)                                               \
    X(float, aim_auto_snipers_rcs_scale_x, "aim_auto_snipers_rcs_scale_x", 0.8f)                              \
    X(float, aim_auto_snipers_rcs_scale_y, "aim_auto_snipers_rcs_scale_y", 0.8f)                              \
    X(float, aim_auto_snipers_rcs_smooth, "aim_auto_snipers_rcs_smooth", 30.0f)                               \
    X(float, aim_shotguns_fov, "aim_shotguns_fov", 2.5f)                                                      \
    X(float, aim_shotguns_smooth, "aim_shotguns_smooth", 35.0f)                                               \
    X(bool, aim_shotguns_rcs, "aim_shotguns_rcs", true)                                                       \
    X(float, aim_shotguns_rcs_scale_x, "aim_shotguns_rcs_scale_x", 0.8f)                                      \
    X(float, aim_shotguns_rcs_scale_y, "aim_shotguns_rcs_scale_y", 0.8f)                                      \
    X(float, aim_shotguns_rcs_smooth, "aim_shotguns_rcs_smooth", 30.0f)                                       \
    X(float, aim_lmgs_fov, "aim_lmgs_fov", 2.5f)                                                              \
    X(float, aim_lmgs_smooth, "aim_lmgs_smooth", 35.0f)                                                       \
    X(bool, aim_lmgs_rcs, "aim_lmgs_rcs", true)                                                               \
    X(float, aim_lmgs_rcs_scale_x, "aim_lmgs_rcs_scale_x", 0.8f)                                              \
    X(float, aim_lmgs_rcs_scale_y, "aim_lmgs_rcs_scale_y", 0.8f)                                              \
    X(float, aim_lmgs_rcs_smooth, "aim_lmgs_rcs_smooth", 30.0f)                                               \
    X(float, aim_others_fov, "aim_others_fov", 2.5f)                                                          \
    X(float, aim_others_smooth, "aim_others_smooth", 35.0f)                                                   \
    X(bool, aim_others_rcs, "aim_others_rcs", true)                                                           \
    X(float, aim_others_rcs_scale_x, "aim_others_scale_x", 0.8f)                                              \
    X(float, aim_others_rcs_scale_y, "aim_others_scale_y", 0.8f)                                              \
    X(float, aim_others_rcs_smooth, "aim_others_rcs_smooth", 30.0f)                                           \
    X(float, aim_horizontal_bias, "aim_horizontal_bias", 1.05f)                                               \
    X(bool, aim_prediction, "aim_prediction", true)                                                           \
    X(bool, aim_fov_circle, "aim_fov_circle", true)                                                           \
    X(Color, aim_fov_circle_color, "aim_fov_circle_color", Color(1.0f, 1.0f, 1.0f, 0.4f))                      \
    X(bool, aim_overshoot, "aim_overshoot", true)                                                             \
    X(float, aim_overshoot_chance, "aim_overshoot_chance", 0.35f)                                             \
    X(float, aim_overshoot_scale, "aim_overshoot_scale", 1.06f)                                               \
    X(bool, aim_rcs, "aim_rcs", true)                                                                         \
    X(float, aim_rcs_scale_x, "aim_rcs_scale_x", 0.8f)                                                        \
    X(float, aim_rcs_scale_y, "aim_rcs_scale_y", 0.8f)                                                        \
    X(float, aim_rcs_smooth, "aim_rcs_smooth", 30.0f)                                                         \
    X(float, trigger_light_pistols_hitbox_scale, "trigger_light_pistols_hitbox_scale", 1.0f)                  \
    X(float, trigger_heavy_pistols_hitbox_scale, "trigger_heavy_pistols_hitbox_scale", 1.0f)                  \
    X(float, trigger_rifles_hitbox_scale, "trigger_rifles_hitbox_scale", 1.0f)                                \
    X(float, trigger_smgs_hitbox_scale, "trigger_smgs_hitbox_scale", 1.0f)                                    \
    X(float, trigger_scout_hitbox_scale, "trigger_scout_hitbox_scale", 1.0f)                                  \
    X(float, trigger_awp_hitbox_scale, "trigger_awp_hitbox_scale", 1.0f)                                      \
    X(float, trigger_auto_snipers_hitbox_scale, "trigger_auto_snipers_hitbox_scale", 1.0f)                    \
    X(float, trigger_shotguns_hitbox_scale, "trigger_shotguns_hitbox_scale", 1.0f)                            \
    X(float, trigger_lmgs_hitbox_scale, "trigger_lmgs_hitbox_scale", 1.0f)                                    \
    X(float, trigger_others_hitbox_scale, "trigger_others_hitbox_scale", 1.0f)                                \
    X(int, trigger_light_pistols_hitbox_groups, "trigger_light_pistols_hitbox_groups", 63)                     \
    X(int, trigger_heavy_pistols_hitbox_groups, "trigger_heavy_pistols_hitbox_groups", 63)                     \
    X(int, trigger_rifles_hitbox_groups, "trigger_rifles_hitbox_groups", 63)                                   \
    X(int, trigger_smgs_hitbox_groups, "trigger_smgs_hitbox_groups", 63)                                       \
    X(int, trigger_scout_hitbox_groups, "trigger_scout_hitbox_groups", 63)                                     \
    X(int, trigger_awp_hitbox_groups, "trigger_awp_hitbox_groups", 63)                                         \
    X(int, trigger_auto_snipers_hitbox_groups, "trigger_auto_snipers_hitbox_groups", 63)                       \
    X(int, trigger_shotguns_hitbox_groups, "trigger_shotguns_hitbox_groups", 63)                               \
    X(int, trigger_lmgs_hitbox_groups, "trigger_lmgs_hitbox_groups", 63)                                       \
    X(int, trigger_others_hitbox_groups, "trigger_others_hitbox_groups", 63)                                   \
    X(int, trigger_delay_min, "trigger_delay_min", 350)                                                       \
    X(int, trigger_delay_max, "trigger_delay_max", 350)                                                       \
    X(int, trigger_delay_between_shots, "trigger_delay_between_shots", 350)                                   \
    X(float, trigger_bucket_guard_degrees, "trigger_bucket_guard_degrees", 0.01f)                              \
    X(bool, trigger_predict_origin, "trigger_predict_origin", true)                                           \
    X(bool, trigger_sounds, "trigger_sounds", true)                                                           \
    X(bool, trigger_debug, "trigger_debug", false)                                                            \
    X(bool, team_check, "team_check", true)                                                   \
    X(bool, trigger_shoot_on_target_lost, "trigger_shoot_on_target_lost", false)                              \
    X(bool, trigger_use_bones, "trigger_use_bones", false)                                                    \
    X(int, trigger_attack_key, "trigger_attack_key", 1000)                                                    \
    X(int, trigger_key, "trigger_key", 1001)                                    \
    X(bool, auto_accept_enabled, "auto_accept_enabled", false)                                                \
    X(bool, auto_accept_show_target, "auto_accept_show_target", true)                                         \
    X(float, auto_accept_target_x_percent, "auto_accept_target_x_percent", 50.0f)                             \
    X(float, auto_accept_target_y_percent, "auto_accept_target_y_percent", 40.0f)                             \
    X(float, auto_accept_delay_ms, "auto_accept_delay_ms", 2500.0f)                                           \
    X(float, auto_accept_smooth, "auto_accept_smooth", 40.0f)                                                 \
    X(bool, auto_accept_progressive_smooth, "auto_accept_progressive_smooth", true)                           \
    X(bool, auto_accept_random_smooth, "auto_accept_random_smooth", false)                                    \
    X(float, auto_accept_randomize_smooth_x, "auto_accept_randomize_smooth_x", 0.0f)                          \
    X(float, auto_accept_randomize_smooth_y, "auto_accept_randomize_smooth_y", 0.0f)                          \
    X(bool, auto_accept_overshoot, "auto_accept_overshoot", false)                                            \
    X(float, auto_accept_overshoot_chance, "auto_accept_overshoot_chance", 0.0f)                              \
    X(float, auto_accept_overshoot_scale, "auto_accept_overshoot_scale", 1.0f)                                 \
    X(bool, ej_enabled, "ej_enabled", false)                                                                  \
    X(bool, ej_debug, "ej_debug", false)                                                                      \
    X(bool, ej_render_debug, "ej_render_debug", false)                                                        \
    X(bool, ej_crouch_on_jump, "ej_crouch_on_jump", false)                                                     \
    X(bool, ej_knife_only, "ej_knife_only", true)                                                             \
    X(bool, ej_strafe_assist, "ej_strafe_assist", false)                                                      \
    X(float, ej_check_distance, "ej_check_distance", 15.0f)                                                   \
    X(float, ej_ground_check_height, "ej_ground_check_height", 80.0f)                                         \
    X(float, ej_ground_tolerance, "ej_ground_tolerance", 30.0f)                                               \
    X(int, ej_jump_delay_ms, "ej_jump_delay_ms", 20)                                                          \
    X(float, ej_min_movement_speed, "ej_min_movement_speed", 150.0f)                                          \
    X(int, edgebug_activation_key, "edgebug_activation_key", 1004)                                            \
    X(bool, kb_viewer_enabled, "kb_viewer_enabled", false)                                                    \
    X(float, kb_viewer_x, "kb_viewer_x", 20.0f)                                                                \
    X(float, kb_viewer_y, "kb_viewer_y", 650.0f)                                                               \
    X(float, kb_viewer_scale, "kb_viewer_scale", 1.0f)                                                         \
    X(bool, spec_list_enabled, "spec_list_enabled", true)                                                      \
    X(float, spec_list_x, "spec_list_x", 10.0f)                                                                \
    X(float, spec_list_y, "spec_list_y", 300.0f)                                                               \
    X(float, spec_list_scale, "spec_list_scale", 1.0f)                                                         \
    X(int, key_left_mouse, "key_left_mouse", 1000)                                                            \
    X(int, key_right_mouse, "key_right_mouse", 1001)                                                          \
    X(int, key_middle_mouse, "key_middle_mouse", 1002)                                                        \
    X(int, key_mouse_xbutton1, "key_mouse_xbutton1", 1003)                                                    \
    X(int, key_mouse_xbutton2, "key_mouse_xbutton2", 1004)                                                    \
    X(int, key_w, "key_w", 87)                                                                                \
    X(int, key_a, "key_a", 65)                                                                                \
    X(int, key_s, "key_s", 83)                                                                                \
    X(int, key_d, "key_d", 68)                                                                                \
    X(int, key_jump, "key_jump", 32)                                                                          \
    X(int, key_crouch, "key_crouch", 341)                                                                     \
    X(int, key_shift, "key_shift", 340)                                                                       \
    X(bool, cstrafe_enabled, "cstrafe_enabled", false)                                                        \
    X(float, cstrafe_x, "cstrafe_x", 20.0f)                                                                   \
    X(float, cstrafe_y, "cstrafe_y", 450.0f)                                                                  \
    X(float, cstrafe_scale, "cstrafe_scale", 1.0f)                                                             \
    X(bool, cstrafe_show_graph, "cstrafe_show_graph", true)                                                   \
    X(bool, esp_spread_circle, "esp_spread_circle", false)                                                    \
    X(Color, esp_spread_circle_color, "esp_spread_circle_color", Color(0.0f, 1.0f, 1.0f, 0.5f))                \
    X(float, esp_spread_circle_fill_alpha, "esp_spread_circle_fill_alpha", 0.1f)                              \
    X(bool, esp_spread_circle_outline, "esp_spread_circle_outline", true)                                     \
    X(float, esp_spread_circle_thickness, "esp_spread_circle_thickness", 1.0f)                                \
    X(bool, esp_spread_circle_show_base, "esp_spread_circle_show_base", true)                                 \
    X(float, esp_spread_circle_animation_speed, "esp_spread_circle_animation_speed", 15.0f)                   \
    X(bool, esp_crosshair_distance, "esp_crosshair_distance", false)                                          \
    X(Color, esp_crosshair_distance_color, "esp_crosshair_distance_color", Color(1.0f, 1.0f, 1.0f, 0.9f))     \
    X(Color, esp_crosshair_distance_player_color, "esp_crosshair_distance_player_color", Color(1.0f, 0.35f, 0.35f, 1.0f)) \
    X(float, esp_crosshair_distance_offset, "esp_crosshair_distance_offset", 14.0f)                           \
    X(Color, esp_crosshair_distance_pin_color, "esp_crosshair_distance_pin_color", Color(1.0f, 0.8f, 0.2f, 1.0f)) \
    X(bool, radar_enabled, "radar_enabled", false)                                                            \
    X(bool, radar_autostart, "radar_autostart", false)                                                        \
    X(std::string, radar_host, "radar_host", "127.0.0.1")                                                     \
    X(int, radar_http_port, "radar_http_port", 36364)                                                         \
    X(int, radar_ws_port, "radar_ws_port", 36365)                                                             \
    X(bool, radar_open_browser, "radar_open_browser", false)                                                  \
    X(bool, radar_transparent, "radar_transparent", false)                                                    \
    X(float, radar_player_dot_scale, "radar_player_dot_scale", 0.6f)                                          \
    X(float, radar_bomb_dot_scale, "radar_bomb_dot_scale", 0.7f)                                              \
    X(bool, radar_show_names, "radar_show_names", true)                                                       \
    X(int, radar_max_name_length, "radar_max_name_length", 8)                                                 \
    X(bool, radar_show_buyzones, "radar_show_buyzones", true)                                                 \
    X(bool, radar_show_projectiles, "radar_show_projectiles", true)                                           \
    X(bool, radar_show_smokes, "radar_show_smokes", true)                                                     \
    X(bool, radar_autozoom, "radar_autozoom", false)                                                          \
    X(int, radar_vert_indicator, "radar_vert_indicator", 2)                                                   \
    X(int, menu_w, "menu_w", 960)                                                                             \
    X(int, menu_h, "menu_h", 620)

struct OverlayConfig {
#define X(type, name, json_key, default_val) mutable type name = default_val;
    CONFIG_FIELDS
#undef X

    // Default equality operator (C++20)
    bool operator==(const OverlayConfig& other) const = default;

    // Dirty flag state
    mutable bool dirty_ = false;
    void mark_dirty() const
    {
        dirty_ = true;
    }
    bool is_dirty() const
    {
        return dirty_;
    }
    void clear_dirty() const
    {
        dirty_ = false;
    }

    // Setters kept inline for menu window sizing compatibility
    void set_menu_w(int val) const
    {
        if (menu_w != val) {
            menu_w = val;
            mark_dirty();
        }
    }
    void set_menu_h(int val) const
    {
        if (menu_h != val) {
            menu_h = val;
            mark_dirty();
        }
    }

    mutable std::vector<KeyBind> keybinds;

    // Runtime-only launch option (--x11). Forces the X11/XWayland overlay
    // backend even when a Wayland session is detected. Never serialized.
    bool force_x11 = false;
};

extern std::string g_active_config_name;

struct MenuPreferences {
    Color accent_color = Color(0.902f, 0.894f, 0.878f, 1.0f);
    Color background_color = Color(0.0784f, 0.0706f, 0.0667f, 1.0f);
    Color card_color = Color(0.1333f, 0.1216f, 0.1137f, 1.0f);
    Color text_color = Color(0.925f, 0.918f, 0.902f, 1.0f);
    Color outline_color = Color(1.0f, 0.980f, 0.950f, 0.08f);
    bool animations_enabled = true;
    float animation_speed = 16.0f;
    float menu_scale = 1.0f;
    std::vector<std::string> collapsed_sections;
    bool quit_shortcut_enabled = true;

    bool operator==(const MenuPreferences& other) const = default;
};

struct ConfigProfileInfo {
    std::string name;
    bool is_default = false;
};

extern MenuPreferences g_menu_preferences;

bool is_section_collapsed(const std::string& section_id);
void set_section_collapsed(const std::string& section_id, bool collapsed);

// Friendly profile-name normalization. Empty result means invalid.
std::string sanitize_config_name(const std::string& name);
// Stems that must never appear as user profiles (offsets.json, etc.).
bool is_reserved_config_stem(const std::string& stem);

std::string get_config_path(const std::string& filename);
std::string get_configs_directory();
std::vector<std::string> list_configs();
std::vector<ConfigProfileInfo> list_config_profiles();
bool delete_config(const std::string& name);
bool set_default_config(const std::string& name);
const std::string& get_default_config_name();
bool reset_default_config();
bool save_config_profile(const std::string& name, const OverlayConfig& cfg);
std::string create_unique_config(const std::string& requested_name, const OverlayConfig& cfg);
bool initialize_config_system();
OverlayConfig load_startup_config();
bool save_menu_preferences();
void schedule_menu_preferences_save();
void flush_menu_preferences_save();
void set_active_config_snapshot(const std::string& name, const OverlayConfig& cfg);
bool active_config_has_unsaved_changes(const OverlayConfig& cfg);
bool save_active_config(const OverlayConfig& cfg);

// Loads config; on missing/invalid file returns compile-time defaults (startup path).
OverlayConfig load_config(const std::string& filename);
// Loads only on successful open+parse. Leaves `out` untouched on failure.
bool try_load_config(const std::string& filename, OverlayConfig& out);
int get_style_id(const ChamsStyle& style);
bool chams_style_uses_secondary_color(const ChamsStyle& style);
void save_config(const std::string& filename, const OverlayConfig& cfg);

// Resolves the active config file path. Defaults to "configs/default.json" but can be
// overridden via the BUNNYWARE_CONFIG environment variable (used by the E2E test
// harness so tests never touch the real user config).
const std::string& config_file_path();
bool validate_config(const std::string& filename, std::string& error_msg);

bool get_config_bool(const OverlayConfig& cfg, const std::string& name);
void set_config_bool(const OverlayConfig& cfg, const std::string& name, bool value);
float get_config_float(const OverlayConfig& cfg, const std::string& name);
void set_config_float(const OverlayConfig& cfg, const std::string& name, float value);
int get_config_int(const OverlayConfig& cfg, const std::string& name);
void set_config_int(const OverlayConfig& cfg, const std::string& name, int value);

bool is_setting_bool(const std::string& name);
bool is_setting_float(const std::string& name);
bool is_setting_int(const std::string& name);

const char* get_setting_name_by_ptr(const OverlayConfig& cfg, const void* ptr);
