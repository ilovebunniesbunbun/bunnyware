#include "config.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <system_error>
#include <type_traits>

#include <unistd.h>

#include "logger.hpp"

using json = nlohmann::json;

namespace {
constexpr const char* kDefaultProfile = "default";
constexpr const char* kSettingsFile = "_settings.json";

std::string g_default_config_name = kDefaultProfile;
std::optional<OverlayConfig> g_active_config_snapshot;
std::optional<std::chrono::steady_clock::time_point> g_preferences_save_deadline;

std::filesystem::path executable_directory()
{
    try {
        return std::filesystem::read_symlink("/proc/self/exe").parent_path();
    } catch (...) {
        return std::filesystem::current_path();
    }
}

std::filesystem::path user_config_directory()
{
    const char* xdg_config_home = std::getenv("XDG_CONFIG_HOME");
    if (xdg_config_home && xdg_config_home[0] && std::filesystem::path(xdg_config_home).is_absolute()) {
        return std::filesystem::path(xdg_config_home) / "bunnyware" / "configs";
    }

    const char* sudo_user = std::getenv("SUDO_USER");
    if (sudo_user && sudo_user[0]) {
        return std::filesystem::path("/home") / sudo_user / ".config" / "bunnyware" / "configs";
    }

    const char* home = std::getenv("HOME");
    if (home && home[0]) {
        return std::filesystem::path(home) / ".config" / "bunnyware" / "configs";
    }

    return {};
}

bool directory_can_be_created(const std::filesystem::path& directory)
{
    std::error_code error;
    if (std::filesystem::exists(directory, error)) {
        return std::filesystem::is_directory(directory, error) && access(directory.c_str(), W_OK) == 0;
    }

    std::filesystem::path parent = directory.parent_path();
    while (!parent.empty()) {
        if (std::filesystem::exists(parent, error)) {
            return std::filesystem::is_directory(parent, error) && access(parent.c_str(), W_OK) == 0;
        }
        const std::filesystem::path next = parent.parent_path();
        if (next == parent) break;
        parent = next;
    }
    return false;
}

std::string lowercase(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::filesystem::path profile_path(const std::string& name)
{
    return std::filesystem::path(get_configs_directory()) / (name + ".json");
}

bool write_json_atomic(const std::filesystem::path& path, const json& value)
{
    try {
        if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
        const std::filesystem::path temporary = path.string() + ".tmp";
        {
            std::ofstream file(temporary, std::ios::trunc);
            if (!file.is_open()) return false;
            file << value.dump(4) << '\n';
            file.flush();
            if (!file.good()) return false;
        }
        std::error_code error;
        std::filesystem::rename(temporary, path, error);
        if (error) {
            std::filesystem::remove(temporary);
            BUNNYWARE_LOG_ERROR("Failed to atomically replace config {}: {}", path.string(), error.message());
            return false;
        }
        return true;
    } catch (const std::exception& error) {
        BUNNYWARE_LOG_ERROR("Failed to write config {}: {}", path.string(), error.what());
        return false;
    }
}

json serialize_config(const OverlayConfig& cfg)
{
    json j;
#define X(type, name, json_key, default_val) j[json_key] = cfg.name;
    CONFIG_FIELDS
#undef X
    j["keybinds"] = cfg.keybinds;
    return j;
}

OverlayConfig without_dirty(const OverlayConfig& cfg)
{
    OverlayConfig copy = cfg;
    copy.clear_dirty();
    return copy;
}
}  // namespace

std::string sanitize_config_name(const std::string& name)
{
    std::string source = name;
    if (source.size() > 5 && lowercase(source.substr(source.size() - 5)) == ".json") {
        source.resize(source.size() - 5);
    }
    std::string out;
    out.reserve(source.size());
    bool pending_space = false;
    for (unsigned char c : source) {
        if (std::isalnum(c) || c == '_' || c == '-') {
            if (pending_space && !out.empty()) out.push_back(' ');
            out.push_back(static_cast<char>(c));
            pending_space = false;
        } else if (std::isspace(c)) {
            pending_space = !out.empty();
        } else if (c == '/' || c == '\\' || std::iscntrl(c)) {
            if (!out.empty() && out.back() != '-') out.push_back('-');
        }
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '-')) out.pop_back();
    if (out.size() > 64) out.resize(64);
    return out;
}

bool is_reserved_config_stem(const std::string& stem)
{
    const std::string normalized = lowercase(stem);
    return normalized == "_settings" || normalized == "offsets" || normalized == "overlay.schema" ||
           normalized == "compile_commands" || normalized == "cmakecache" || normalized.empty();
}

const std::string& config_file_path()
{
    static const std::string path = [] {
        const char* env = std::getenv("BUNNYWARE_CONFIG");
        return (env && env[0]) ? std::string(env)
                               : (std::filesystem::path(get_configs_directory()) / "default.json").string();
    }();
    return path;
}

// JSON serialization implementations
void to_json(nlohmann::json& j, const Color& color)
{
    j = nlohmann::json::array({
        static_cast<int>(color.r * 255.0f),
        static_cast<int>(color.g * 255.0f),
        static_cast<int>(color.b * 255.0f),
        static_cast<int>(color.a * 255.0f)
    });
}

void from_json(const nlohmann::json& j, Color& color)
{
    if (j.is_array() && j.size() == 4) {
        color.r = j[0].get<float>() / 255.0f;
        color.g = j[1].get<float>() / 255.0f;
        color.b = j[2].get<float>() / 255.0f;
        color.a = j[3].get<float>() / 255.0f;
    }
}

void to_json(nlohmann::json& j, const ChamsStyle& style)
{
    j = (std::string)style;
}

void from_json(const nlohmann::json& j, ChamsStyle& style)
{
    style = ChamsStyle(j.get<std::string>());
}

void to_json(nlohmann::json& j, const KillFxMode& mode)
{
    j = (std::string)mode;
}

void from_json(const nlohmann::json& j, KillFxMode& mode)
{
    if (j.is_boolean()) {
        mode = j.get<bool>() ? KillFxMode(KillFxModeEnum::CHAMS) : KillFxMode(KillFxModeEnum::DISABLED);
    } else if (j.is_string()) {
        mode = KillFxMode(j.get<std::string>());
    } else {
        mode = KillFxMode(KillFxModeEnum::DISABLED);
    }
}

void to_json(nlohmann::json& j, const BulletImpactStyle& style)
{
    j = (std::string)style;
}

void from_json(const nlohmann::json& j, BulletImpactStyle& style)
{
    if (j.is_string()) {
        style = BulletImpactStyle(j.get<std::string>());
    } else if (j.is_boolean()) {
        style = j.get<bool>() ? BulletImpactStyle(BulletImpactStyleEnum::BOXES)
                              : BulletImpactStyle(BulletImpactStyleEnum::FLASH);
    } else {
        style = BulletImpactStyle(BulletImpactStyleEnum::FLASH);
    }
}

void to_json(nlohmann::json& j, const KeyBind& kb)
{
    j = nlohmann::json{
        {"setting_name", kb.setting_name},
        {"key", kb.key},
        {"mode", kb.mode == BindMode::HOLD ? "hold" : "toggle"},
        {"is_slider", kb.is_slider},
        {"slider_value", kb.slider_value},
        {"slider_value_int", kb.slider_value_int}
    };
}

void from_json(const nlohmann::json& j, KeyBind& kb)
{
    j.at("setting_name").get_to(kb.setting_name);
    j.at("key").get_to(kb.key);
    std::string mode_str = j.value("mode", "toggle");
    kb.mode = (mode_str == "hold") ? BindMode::HOLD : BindMode::TOGGLE;
    kb.is_slider = j.value("is_slider", false);
    kb.slider_value = j.value("slider_value", 0.0f);
    kb.slider_value_int = j.value("slider_value_int", 0);

    kb.was_pressed = false;
    kb.original_val_bool = false;
    kb.original_val_float = 0.0f;
    kb.original_val_int = 0;
    kb.is_active_hold = false;
}

std::string g_active_config_name = "";
MenuPreferences g_menu_preferences;

std::string get_config_path(const std::string& filename)
{
    // If BUNNYWARE_CONFIG is set and this filename is that overridden path, do not resolve relative to exec dir
    const char* env = std::getenv("BUNNYWARE_CONFIG");
    if (env && env[0] && filename == env) {
        return filename;
    }
    if (std::filesystem::path(filename).is_absolute()) {
        return filename;
    }
    try {
        return (executable_directory() / filename).string();
    } catch (...) {
        return filename;
    }
}

std::string get_configs_directory()
{
    const char* override_directory = std::getenv("BUNNYWARE_CONFIG_DIR");
    if (override_directory && override_directory[0]) return override_directory;

    const std::filesystem::path portable_directory = executable_directory() / "configs";
    if (directory_can_be_created(portable_directory)) return portable_directory.string();

    const std::filesystem::path fallback_directory = user_config_directory();
    if (!fallback_directory.empty() && directory_can_be_created(fallback_directory)) {
        return fallback_directory.string();
    }

    return portable_directory.string();
}

static void collect_configs_from_dir(const std::filesystem::path& dir, std::vector<std::string>& configs)
{
    if (!std::filesystem::exists(dir) || !std::filesystem::is_directory(dir)) {
        return;
    }
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".json") {
            continue;
        }
        std::string stem = entry.path().stem().string();
        if (is_reserved_config_stem(stem)) {
            continue;
        }
        // Only show names that survive normalization, blocking unexpected paths.
        if (sanitize_config_name(stem) != stem) {
            continue;
        }
        configs.push_back(stem);
    }
}

std::vector<std::string> list_configs()
{
    std::vector<std::string> configs;
    try {
        collect_configs_from_dir(get_configs_directory(), configs);
    } catch (...) {
    }
    std::sort(configs.begin(), configs.end(), [](const std::string& lhs, const std::string& rhs) {
        return lowercase(lhs) < lowercase(rhs);
    });
    configs.erase(std::unique(configs.begin(), configs.end()), configs.end());
    return configs;
}

std::vector<ConfigProfileInfo> list_config_profiles()
{
    std::vector<ConfigProfileInfo> profiles;
    for (const std::string& name : list_configs()) {
        profiles.push_back({name, lowercase(name) == lowercase(g_default_config_name)});
    }
    return profiles;
}

bool delete_config(const std::string& name)
{
    const std::string stem = sanitize_config_name(name);
    if (stem.empty() || is_reserved_config_stem(stem)) {
        BUNNYWARE_LOG_ERROR("Refusing to delete invalid config profile: {}", name);
        return false;
    }
    try {
        const bool was_default = lowercase(stem) == lowercase(g_default_config_name);
        const bool removed = std::filesystem::remove(profile_path(stem));
        if (!removed) return false;

        if (lowercase(stem) == lowercase(kDefaultProfile)) {
            if (!reset_default_config()) return false;
        }
        if (was_default) {
            g_default_config_name = kDefaultProfile;
            if (!std::filesystem::exists(profile_path(kDefaultProfile)) && !reset_default_config()) return false;
            return save_menu_preferences();
        }
        return true;
    } catch (...) {
        return false;
    }
}

static bool parse_config_json(const json& j, OverlayConfig& cfg)
{
    try {
#define X(type, name, json_key, default_val) \
        if (j.contains(json_key)) cfg.name = j[json_key].get<type>();
        CONFIG_FIELDS
#undef X

        // DamageFX: the old hit particle toggle became the hit effect mode.
        if (j.contains("hit_effect_enabled") && !j.contains("hit_effect_mode")) {
            const bool enabled = j["hit_effect_enabled"].is_boolean() && j["hit_effect_enabled"].get<bool>();
            if (enabled) cfg.hit_effect_mode = KillFxMode(KillFxModeEnum::PARTICLES);
        }

        // "Hit Flash" was renamed to "Bullet Impacts".
        const auto migrate_renamed = [&j](const char* old_key, const char* new_key, auto& field) {
            if (!j.contains(new_key) && j.contains(old_key)) {
                field = j[old_key].get<std::decay_t<decltype(field)>>();
            }
        };
        migrate_renamed("hit_flash_enabled", "bullet_impacts_enabled", cfg.bullet_impacts_enabled);
        migrate_renamed("hit_flash_color", "bullet_impacts_color", cfg.bullet_impacts_color);
        migrate_renamed("hit_flash_size", "bullet_impacts_size", cfg.bullet_impacts_size);
        migrate_renamed("hit_flash_duration", "bullet_impacts_duration", cfg.bullet_impacts_duration);
        migrate_renamed("hit_flash_hold", "bullet_impacts_hold", cfg.bullet_impacts_hold);
        migrate_renamed("hit_flash_hold_gap", "bullet_impacts_hold_gap", cfg.bullet_impacts_hold_gap);
        migrate_renamed("hit_flash_spread", "bullet_impacts_spread", cfg.bullet_impacts_spread);

        // Players tab restructure: Chams and DamageFX gained master toggles that
        // gate their (already existing) style/mode selections. Profiles created
        // before the toggles existed keep whatever they had configured enabled.
        if (!j.contains("chams_enabled")) {
            cfg.chams_enabled = cfg.style_vis.value != ChamsStyleEnum::DISABLED ||
                                cfg.style_invis.value != ChamsStyleEnum::DISABLED;
        }
        if (!j.contains("damage_fx_enabled")) {
            cfg.damage_fx_enabled = cfg.kill_effect.value != KillFxModeEnum::DISABLED ||
                                    cfg.hit_effect_mode.value != KillFxModeEnum::DISABLED;
        }

        if (j.contains("keybinds")) {
            cfg.keybinds = j["keybinds"].get<std::vector<KeyBind>>();
        }
        if (cfg.scaling.empty()) {
            cfg.scaling = "stretched";
        }
        return true;
    } catch (const std::exception& e) {
        BUNNYWARE_LOG_ERROR("Config parse exception: {}", e.what());
        return false;
    } catch (...) {
        BUNNYWARE_LOG_ERROR("Config parse unknown exception");
        return false;
    }
}

bool try_load_config(const std::string& filename, OverlayConfig& out)
{
    const std::filesystem::path requested(filename);
    std::string path;
    if (requested.is_absolute() || filename.find('/') != std::string::npos) {
        path = get_config_path(filename);
    } else {
        const std::string stem = sanitize_config_name(requested.stem().string());
        path = profile_path(stem).string();
    }
    std::ifstream file(path);
    if (!file.is_open()) {
        BUNNYWARE_LOG_ERROR("Config file not found: {}", path);
        return false;
    }

    try {
        json j;
        file >> j;
        file.close();

        OverlayConfig parsed;
        if (!parse_config_json(j, parsed)) {
            return false;
        }
        out = std::move(parsed);
        return true;
    } catch (const std::exception& e) {
        BUNNYWARE_LOG_ERROR("Config load exception ({}): {}", path, e.what());
        return false;
    } catch (...) {
        BUNNYWARE_LOG_ERROR("Config load unknown exception: {}", path);
        return false;
    }
}

OverlayConfig load_config(const std::string& filename)
{
    OverlayConfig cfg;
    if (!try_load_config(filename, cfg)) {
        // Startup / missing file: keep compile-time defaults.
        if (cfg.scaling.empty()) {
            cfg.scaling = "stretched";
        }
    }
    return cfg;
}

int get_style_id(const ChamsStyle& style)
{
    switch (style.value) {
        case ChamsStyleEnum::SOLID: return 1;
        case ChamsStyleEnum::FLAT: return 2;
        case ChamsStyleEnum::METALLIC: return 3;
        case ChamsStyleEnum::GLASS: return 4;
        default: return 0;
    }
}

bool chams_style_uses_secondary_color(const ChamsStyle& style)
{
    switch (style.value) {
        case ChamsStyleEnum::DISABLED:
        case ChamsStyleEnum::SOLID:
        case ChamsStyleEnum::FLAT:
        case ChamsStyleEnum::METALLIC:
        case ChamsStyleEnum::GLASS:
            return false;
        default:
            return true;
    }
}

void save_config(const std::string& filename, const OverlayConfig& cfg)
{
    std::filesystem::path path;
    const std::filesystem::path as_path(filename);
    if (as_path.is_absolute() || filename.find('/') != std::string::npos) {
        path = get_config_path(filename);
    } else {
        std::string stem = as_path.stem().string();
        stem = sanitize_config_name(stem);
        if (stem.empty() || is_reserved_config_stem(stem)) {
            BUNNYWARE_LOG_ERROR("Refusing to save invalid/reserved config name: {}", filename);
            return;
        }
        path = profile_path(stem);
    }

    if (write_json_atomic(path, serialize_config(cfg))) {
        BUNNYWARE_LOG_INFO("Saved config successfully to {}", path.string());
    } else {
        BUNNYWARE_LOG_ERROR("Failed to save config to path: {}", path.string());
    }
}

namespace {
OverlayConfig make_baseline_config()
{
    OverlayConfig cfg;
    cfg.show_fps = false;
    cfg.style_vis = ChamsStyleEnum::DISABLED;
    cfg.style_invis = ChamsStyleEnum::DISABLED;
    cfg.glow_enabled = false;
    cfg.esp_enabled = false;
    cfg.esp_spread_circle = false;
    cfg.esp_bomb_bar = false;
    cfg.bomb_bar_gradient_enabled = true;
    cfg.c4_render_in_world = false;
    cfg.c4_timer_gradient_enabled = true;
    cfg.kill_effect = KillFxModeEnum::DISABLED;
    cfg.kill_chams_enabled = false;
    cfg.esp_dropped_items = false;
    cfg.map_visualizer_enabled = false;
    cfg.draw_grenade_trajectory = false;
    cfg.grenade_warnings_enabled = false;
    cfg.inferno_spread_enabled = false;
    cfg.grenade_helper_enabled = false;
    cfg.grenade_creator_enabled = false;
    cfg.grenade_solver_enabled = false;
    cfg.grenade_solver_show_trajectory = false;
    cfg.grenade_solver_location_creator_enabled = false;
    cfg.trigger_enabled = false;
    cfg.trigger_active = false;
    cfg.aim_enabled = false;
    cfg.aim_active = false;
    cfg.auto_accept_enabled = false;
    cfg.ej_enabled = false;
    cfg.kb_viewer_enabled = false;
    cfg.spec_list_enabled = false;
    cfg.cstrafe_enabled = false;
    cfg.debug_game_reader = false;
    cfg.debug_gpu_profiling = false;
    cfg.debug_visibility = false;
    cfg.clear_dirty();
    return cfg;
}

bool profile_exists_case_insensitive(const std::string& name, std::string* canonical = nullptr)
{
    const std::string wanted = lowercase(name);
    for (const std::string& existing : list_configs()) {
        if (lowercase(existing) == wanted) {
            if (canonical) *canonical = existing;
            return true;
        }
    }
    return false;
}

bool load_settings_file()
{
    std::ifstream file(std::filesystem::path(get_configs_directory()) / kSettingsFile);
    if (!file.is_open()) return false;
    try {
        json settings;
        file >> settings;
        g_default_config_name = sanitize_config_name(settings.value("default_profile", std::string(kDefaultProfile)));
        if (g_default_config_name.empty()) g_default_config_name = kDefaultProfile;

        if (settings.contains("menu") && settings["menu"].is_object()) {
            const json& menu = settings["menu"];
            if (menu.contains("accent_color")) g_menu_preferences.accent_color = menu["accent_color"].get<Color>();
            if (menu.contains("background_color")) {
                g_menu_preferences.background_color = menu["background_color"].get<Color>();
            }
            if (menu.contains("card_color")) g_menu_preferences.card_color = menu["card_color"].get<Color>();
            if (menu.contains("text_color")) g_menu_preferences.text_color = menu["text_color"].get<Color>();
            if (menu.contains("outline_color")) g_menu_preferences.outline_color = menu["outline_color"].get<Color>();
            g_menu_preferences.animations_enabled = menu.value("animations_enabled", true);
            g_menu_preferences.animation_speed =
                std::clamp(menu.value("animation_speed", 16.0f), 4.0f, 30.0f);
            g_menu_preferences.menu_scale = std::clamp(menu.value("menu_scale", 1.0f), 0.75f, 2.0f);
            g_menu_preferences.collapsed_sections.clear();
            if (menu.contains("collapsed_sections") && menu["collapsed_sections"].is_array()) {
                for (const json& entry : menu["collapsed_sections"]) {
                    if (entry.is_string()) set_section_collapsed(entry.get<std::string>(), true);
                }
            }
            g_menu_preferences.quit_shortcut_enabled = menu.value("quit_shortcut_enabled", true);
        }
        g_menu_preferences.accent_color.a = 1.0f;
        g_menu_preferences.background_color.a = 1.0f;
        g_menu_preferences.card_color.a = 1.0f;
        return true;
    } catch (const std::exception& error) {
        BUNNYWARE_LOG_ERROR("Failed to parse config settings: {}", error.what());
        return false;
    }
}
}  // namespace

bool save_menu_preferences()
{
    json settings = {
        {"version", 1},
        {"default_profile", g_default_config_name},
        {"menu",
         {
             {"accent_color", g_menu_preferences.accent_color},
             {"background_color", g_menu_preferences.background_color},
             {"card_color", g_menu_preferences.card_color},
             {"text_color", g_menu_preferences.text_color},
             {"outline_color", g_menu_preferences.outline_color},
             {"animations_enabled", g_menu_preferences.animations_enabled},
             {"animation_speed", g_menu_preferences.animation_speed},
             {"menu_scale", g_menu_preferences.menu_scale},
             {"collapsed_sections", g_menu_preferences.collapsed_sections},
             {"quit_shortcut_enabled", g_menu_preferences.quit_shortcut_enabled},
         }},
    };
    return write_json_atomic(std::filesystem::path(get_configs_directory()) / kSettingsFile, settings);
}

bool is_section_collapsed(const std::string& section_id)
{
    const std::vector<std::string>& sections = g_menu_preferences.collapsed_sections;
    return std::binary_search(sections.begin(), sections.end(), section_id);
}

void set_section_collapsed(const std::string& section_id, bool collapsed)
{
    std::vector<std::string>& sections = g_menu_preferences.collapsed_sections;
    const auto position = std::lower_bound(sections.begin(), sections.end(), section_id);
    const bool present = position != sections.end() && *position == section_id;
    if (collapsed && !present) sections.insert(position, section_id);
    if (!collapsed && present) sections.erase(position);
}

void schedule_menu_preferences_save()
{
    g_preferences_save_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
}

void flush_menu_preferences_save()
{
    if (!g_preferences_save_deadline || std::chrono::steady_clock::now() < *g_preferences_save_deadline) return;
    save_menu_preferences();
    g_preferences_save_deadline.reset();
}

bool reset_default_config()
{
    OverlayConfig baseline = make_baseline_config();
    const std::string asset_path = get_config_path("assets/config/default.json");
    try {
        std::ifstream asset(asset_path);
        if (asset.is_open()) {
            json overrides;
            asset >> overrides;
            if (!parse_config_json(overrides, baseline)) {
                BUNNYWARE_LOG_WARN("Ignoring invalid baseline overrides at {}", asset_path);
                baseline = make_baseline_config();
            }
        } else {
            BUNNYWARE_LOG_WARN("Baseline asset not found at {}; using compiled baseline", asset_path);
        }
    } catch (const std::exception& error) {
        BUNNYWARE_LOG_WARN("Unable to read baseline asset {}: {}", asset_path, error.what());
        baseline = make_baseline_config();
    }
    if (!write_json_atomic(profile_path(kDefaultProfile), serialize_config(baseline))) return false;
    BUNNYWARE_LOG_INFO("Reset baseline profile at {}", profile_path(kDefaultProfile).string());
    return true;
}

bool initialize_config_system()
{
    try {
        std::filesystem::create_directories(get_configs_directory());
    } catch (const std::exception& error) {
        BUNNYWARE_LOG_ERROR("Unable to create config directory: {}", error.what());
        return false;
    }

    if (!std::filesystem::exists(profile_path(kDefaultProfile)) && !reset_default_config()) return false;

    g_default_config_name = kDefaultProfile;
    g_menu_preferences = MenuPreferences{};
    const bool loaded_settings = load_settings_file();
    std::string canonical_default;
    if (!profile_exists_case_insensitive(g_default_config_name, &canonical_default)) {
        g_default_config_name = kDefaultProfile;
    } else {
        g_default_config_name = canonical_default;
    }
    if (!loaded_settings || canonical_default.empty()) return save_menu_preferences();
    return true;
}

const std::string& get_default_config_name()
{
    return g_default_config_name;
}

bool set_default_config(const std::string& name)
{
    std::string canonical;
    if (!profile_exists_case_insensitive(sanitize_config_name(name), &canonical)) return false;
    const std::string previous = g_default_config_name;
    g_default_config_name = canonical;
    if (save_menu_preferences()) return true;
    g_default_config_name = previous;
    return false;
}

bool save_config_profile(const std::string& name, const OverlayConfig& cfg)
{
    std::string canonical;
    if (!profile_exists_case_insensitive(sanitize_config_name(name), &canonical)) return false;
    return write_json_atomic(profile_path(canonical), serialize_config(cfg));
}

std::string create_unique_config(const std::string& requested_name, const OverlayConfig& cfg)
{
    const std::string base = sanitize_config_name(requested_name);
    if (base.empty() || is_reserved_config_stem(base)) return {};

    std::string candidate = base;
    int suffix = 2;
    while (profile_exists_case_insensitive(candidate)) {
        candidate = base + " - " + std::to_string(suffix++);
    }
    if (!write_json_atomic(profile_path(candidate), serialize_config(cfg))) return {};
    return candidate;
}

void set_active_config_snapshot(const std::string& name, const OverlayConfig& cfg)
{
    g_active_config_name = name;
    g_active_config_snapshot = without_dirty(cfg);
}

bool active_config_has_unsaved_changes(const OverlayConfig& cfg)
{
    return !g_active_config_snapshot.has_value() || without_dirty(cfg) != *g_active_config_snapshot;
}

OverlayConfig load_startup_config()
{
    if (!initialize_config_system()) {
        BUNNYWARE_LOG_ERROR("Configuration system initialization failed; config changes may not be saved.");
    }

    const char* env = std::getenv("BUNNYWARE_CONFIG");
    if (env && env[0]) {
        OverlayConfig overridden;
        if (try_load_config(env, overridden)) {
            set_active_config_snapshot(std::filesystem::path(env).stem().string(), overridden);
            return overridden;
        }
    }

    OverlayConfig loaded;
    if (!try_load_config(g_default_config_name + ".json", loaded)) {
        g_default_config_name = kDefaultProfile;
        reset_default_config();
        save_menu_preferences();
        if (!try_load_config(std::string(kDefaultProfile) + ".json", loaded)) loaded = make_baseline_config();
    }
    set_active_config_snapshot(g_default_config_name, loaded);
    return loaded;
}

bool save_active_config(const OverlayConfig& cfg)
{
    const char* env = std::getenv("BUNNYWARE_CONFIG");
    if (env && env[0]) {
        if (!write_json_atomic(get_config_path(env), serialize_config(cfg))) return false;
        set_active_config_snapshot(std::filesystem::path(env).stem().string(), cfg);
    } else if (g_active_config_name.empty()) {
        if (!save_config_profile(kDefaultProfile, cfg)) return false;
        set_active_config_snapshot(kDefaultProfile, cfg);
    } else {
        const std::string stem = sanitize_config_name(g_active_config_name);
        if (stem.empty()) {
            if (!save_config_profile(kDefaultProfile, cfg)) return false;
            set_active_config_snapshot(kDefaultProfile, cfg);
        } else {
            if (!save_config_profile(stem, cfg)) return false;
            set_active_config_snapshot(stem, cfg);
        }
    }
    return true;
}

std::vector<GpuDevice> detect_gpus()
{
    std::vector<GpuDevice> gpus;
    try {
        if (std::filesystem::exists("/sys/class/drm")) {
            for (const auto& entry : std::filesystem::directory_iterator("/sys/class/drm")) {
                std::string filename = entry.path().filename().string();
                if (filename.rfind("card", 0) == 0 && filename.find('-') == std::string::npos) {
                    std::string vendor_path = entry.path().string() + "/device/vendor";
                    std::string device_path = entry.path().string() + "/device/device";
                    std::string uevent_path = entry.path().string() + "/device/uevent";

                    std::string vendor = "";
                    std::string device = "";
                    std::string driver = "";

                    std::ifstream vf(vendor_path);
                    if (vf >> vendor) {
                        // strip any newline or spaces if read from stream
                    }
                    std::ifstream df(device_path);
                    if (df >> device) {
                        //
                    }

                    std::ifstream uf(uevent_path);
                    std::string line;
                    while (std::getline(uf, line)) {
                        if (line.rfind("DRIVER=", 0) == 0) {
                            driver = line.substr(7);
                        }
                    }

                    std::string display_name = "";
                    if (vendor == "0x10de" || vendor == "10de") {
                        display_name = "NVIDIA Dedicated GPU (" + filename + ")";
                    } else if (vendor == "0x1002" || vendor == "1002") {
                        display_name = "AMD Radeon GPU (" + filename + ")";
                    } else if (vendor == "0x8086" || vendor == "8086") {
                        display_name = "Intel Graphics GPU (" + filename + ")";
                    } else {
                        display_name = "Generic GPU (" + filename + ")";
                    }

                    gpus.push_back({filename, vendor, device, driver, display_name});
                }
            }
        }
    } catch (...) {
    }

    std::sort(gpus.begin(), gpus.end(), [](const GpuDevice& a, const GpuDevice& b) { return a.name < b.name; });
    return gpus;
}

bool validate_config(const std::string& filename, std::string& error_msg)
{
    std::string path = get_config_path(filename);
    std::ifstream file(path);
    if (!file.is_open()) {
        error_msg = "Cannot open config file: " + path;
        return false;
    }
    try {
        json j;
        file >> j;
        file.close();

#define X(type, name, json_key, default_val) \
        if (j.contains(json_key)) { \
            if (std::is_same<type, int>::value && !j[json_key].is_number_integer()) { \
                error_msg = "Field '" json_key "' must be an integer"; \
                return false; \
            } \
            if (std::is_same<type, bool>::value && !j[json_key].is_boolean()) { \
                error_msg = "Field '" json_key "' must be a boolean"; \
                return false; \
            } \
            if (std::is_same<type, float>::value && !j[json_key].is_number()) { \
                error_msg = "Field '" json_key "' must be a number"; \
                return false; \
            } \
            if (std::is_same<type, std::string>::value && !j[json_key].is_string()) { \
                error_msg = "Field '" json_key "' must be a string"; \
                return false; \
            } \
            if (std::is_same<type, Color>::value && (!j[json_key].is_array() || j[json_key].size() != 4)) { \
                error_msg = "Field '" json_key "' must be an array of 4 integers/floats"; \
                return false; \
            } \
            if (std::is_same<type, ChamsStyle>::value && !j[json_key].is_string()) { \
                error_msg = "Field '" json_key "' must be a string"; \
                return false; \
            } \
        }
        CONFIG_FIELDS
#undef X
        return true;
    } catch (const std::exception& e) {
        error_msg = e.what();
        return false;
    }
}

template <typename T>
T get_config_value(const OverlayConfig& cfg, const std::string& name) {
#define X(type, name_var, json_key, default_val) \
    if constexpr (std::is_same_v<type, T>) { \
        if (name == json_key || name == #name_var) return cfg.name_var; \
    }
    CONFIG_FIELDS
#undef X
    return T{};
}

template <typename T>
void set_config_value(const OverlayConfig& cfg, const std::string& name, T value) {
#define X(type, name_var, json_key, default_val) \
    if constexpr (std::is_same_v<type, T>) { \
        if (name == json_key || name == #name_var) { \
            if (cfg.name_var != value) { \
                cfg.name_var = value; \
                cfg.mark_dirty(); \
            } \
            return; \
        } \
    }
    CONFIG_FIELDS
#undef X
}

bool get_config_bool(const OverlayConfig& cfg, const std::string& name) {
    return get_config_value<bool>(cfg, name);
}

void set_config_bool(const OverlayConfig& cfg, const std::string& name, bool value) {
    set_config_value<bool>(cfg, name, value);
}

float get_config_float(const OverlayConfig& cfg, const std::string& name) {
    return get_config_value<float>(cfg, name);
}

void set_config_float(const OverlayConfig& cfg, const std::string& name, float value) {
    set_config_value<float>(cfg, name, value);
}

int get_config_int(const OverlayConfig& cfg, const std::string& name) {
    return get_config_value<int>(cfg, name);
}

void set_config_int(const OverlayConfig& cfg, const std::string& name, int value) {
    set_config_value<int>(cfg, name, value);
}

bool is_setting_bool(const std::string& name) {
#define X(type, name_var, json_key, default_val) \
    if (name == json_key || name == #name_var) return std::is_same_v<type, bool>;
    CONFIG_FIELDS
#undef X
    return false;
}

bool is_setting_float(const std::string& name) {
#define X(type, name_var, json_key, default_val) \
    if (name == json_key || name == #name_var) return std::is_same_v<type, float>;
    CONFIG_FIELDS
#undef X
    return false;
}

bool is_setting_int(const std::string& name) {
#define X(type, name_var, json_key, default_val) \
    if (name == json_key || name == #name_var) return std::is_same_v<type, int>;
    CONFIG_FIELDS
#undef X
    return false;
}

const char* get_setting_name_by_ptr(const OverlayConfig& cfg, const void* ptr) {
#define X(type, name_var, json_key, default_val) \
    if ((const void*)&cfg.name_var == ptr) return json_key;
    CONFIG_FIELDS
#undef X
    return nullptr;
}
