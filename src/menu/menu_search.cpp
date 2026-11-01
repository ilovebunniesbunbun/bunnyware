#include "menu_search.hpp"
#include "config.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <string>

namespace {
struct SearchCharacter {
    char value;
    bool word_start;
};

std::vector<SearchCharacter> normalize_search_text(std::string_view text)
{
    std::vector<SearchCharacter> normalized;
    bool word_start = true;
    for (const unsigned char character : text) {
        if (std::isalnum(character) == 0) {
            word_start = true;
            continue;
        }
        normalized.push_back({static_cast<char>(std::tolower(character)), word_start});
        word_start = false;
    }
    return normalized;
}

bool starts_with(std::string_view value, std::string_view prefix)
{
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

std::string humanize_setting_name(std::string_view name)
{
    std::string label;
    bool capitalize_next = true;
    for (const char character : name) {
        if (character == '_') {
            label.push_back(' ');
            capitalize_next = true;
            continue;
        }
        label.push_back(capitalize_next ? static_cast<char>(std::toupper(static_cast<unsigned char>(character))) : character);
        capitalize_next = false;
    }
    return label;
}

struct SearchLocation {
    int tab_index;
    const char* location;
    const char* section_id;
};

SearchLocation get_setting_location(std::string_view name)
{
    if (starts_with(name, "radar_")) {
        return {7, "Radar / Web Radar", "sec_radar"};
    }
    // Players tab master toggles live in the "Player Features" card.
    if (name == "chams_enabled" || name == "glow_enabled" || name == "esp_enabled" ||
        name == "damage_fx_enabled" || name == "proximity_beep_enabled") {
        return {0, "Players / Player Features", "sec_player_features"};
    }
    if (name == "style_vis" || name == "style_invis" || name == "color_vis" || name == "color_vis_sec" ||
        name == "color_invis" || name == "color_invis_sec" || name == "flat_chams_no_overlap" ||
        starts_with(name, "chams_")) {
        return {0, "Players / Chams", "sec_chams"};
    }
    if (starts_with(name, "glow_") || name == "outline_mode") {
        return {0, "Players / Glow", "sec_outline"};
    }
    if (starts_with(name, "esp_spread_circle")) {
        return {6, "World / Spread Circle", "sec_spread_circle"};
    }
    if (starts_with(name, "esp_crosshair_distance")) {
        return {6, "World / Crosshair Distance", "sec_crosshair_distance"};
    }
    if (starts_with(name, "esp_skeleton")) {
        return {0, "Players / Skeleton", "sec_skeleton"};
    }
    if (starts_with(name, "esp_box")) {
        return {0, "Players / Box", "sec_box"};
    }
    if (starts_with(name, "esp_health_bar")) {
        return {0, "Players / Health Bar", "sec_hp"};
    }
    if (starts_with(name, "esp_armor_bar")) {
        return {0, "Players / Armor Bar", "sec_armor_bar"};
    }
    if (starts_with(name, "esp_ammo_bar")) {
        return {0, "Players / Ammo Bar", "sec_ammo_bar"};
    }
    if (starts_with(name, "esp_flags")) {
        return {0, "Players / Flags", "sec_flags"};
    }
    if (starts_with(name, "esp_offscreen_arrows")) {
        return {0, "Players / Off-screen Arrows", "sec_offscreen_arrows"};
    }
    if (starts_with(name, "esp_head_circle")) {
        return {0, "Players / Head Circle", "sec_head_circle"};
    }
    if (starts_with(name, "esp_weapon_icons") || starts_with(name, "weapon_icon_")) {
        return {0, "Players / Weapon Icons", "sec_weapon_icons"};
    }
    if (starts_with(name, "esp_hit_reveal") || starts_with(name, "esp_damage_reveal")) {
        return {0, "Players / Reveals", "sec_esp_reveals"};
    }
    if (starts_with(name, "esp_distance_limit")) {
        return {0, "Players / ESP", "sec_esp_main"};
    }
    if (starts_with(name, "esp_max_distance") || starts_with(name, "esp_distance_")) {
        return {0, "Players / Distance", "sec_esp_distance"};
    }
    if (starts_with(name, "esp_step_circles") || starts_with(name, "esp_noise")) {
        return {0, "Players / Noise ESP", "sec_step_circles"};
    }
    if (starts_with(name, "esp_bomb_bar")) {
        return {6, "World / Bomb Bar", "sec_bomb_bar"};
    }
    if (starts_with(name, "esp_dropped_items")) {
        return {6, "World / Dropped Items", "sec_dropped_items"};
    }
    if (starts_with(name, "esp_") || starts_with(name, "weapon_icon_")) {
        return {0, "Players / ESP", "sec_esp_main"};
    }
    if (starts_with(name, "proximity_beep")) {
        return {0, "Players / Sound ESP", "sec_proximity_beep"};
    }
    if (starts_with(name, "kill_")) {
        return {0, "Players / DamageFX", "sec_damage_fx"};
    }
    if (starts_with(name, "bullet_impacts")) {
        return {6, "World / Bullet Impacts", "sec_bullet_impacts"};
    }
    if (starts_with(name, "hit_effect") || starts_with(name, "hit_chams") || starts_with(name, "hit_pulse") ||
        starts_with(name, "hit_lightning") || starts_with(name, "hit_shockwave") || starts_with(name, "hit_fx")) {
        return {0, "Players / DamageFX", "sec_damage_fx"};
    }
    if (name == "hitmarker") {
        return {0, "Players / ESP", "sec_esp_main"};
    }
    if (starts_with(name, "crosshair_overlay")) {
        return {6, "World / Crosshair Overlay", "sec_force_crosshair"};
    }
    if (starts_with(name, "crosshair_esp")) {
        return {6, "World / Crosshair Distance", "sec_crosshair_distance"};
    }
    if (starts_with(name, "smoke_wireframe")) {
        return {6, "World / Smoke Wireframe", "sec_smoke_wireframe"};
    }
    if (starts_with(name, "through_smokes")) {
        return {6, "World / Through Smokes", "sec_through_smokes"};
    }
    if (starts_with(name, "anti_flash")) {
        return {6, "World / Anti Flash", "sec_anti_flash"};
    }
    if (starts_with(name, "c4_") || starts_with(name, "world_timer") || starts_with(name, "grenade_warning")) {
        return {6, "World / Timers", "sec_world_timers"};
    }
    if (starts_with(name, "bomb_bar")) {
        return {6, "World / Bomb Bar", "sec_bomb_bar"};
    }
    if (starts_with(name, "grenade_color") || starts_with(name, "inferno_") ||
        starts_with(name, "trajectory_")) {
        return {6, "World / Trajectory Colors", "sec_world_colors"};
    }
    if (starts_with(name, "draw_grenade_trajectory")) {
        return {6, "World / Trajectories", "sec_traj_main"};
    }
    if (starts_with(name, "grenade_helper")) {
        return {1, "Grenades / Grenade Helper", "sec_ghelper"};
    }
    if (starts_with(name, "grenade_creator")) {
        return {1, "Grenades / Grenade Creator", "sec_gcreator"};
    }
    if (starts_with(name, "grenade_solver")) {
        return {1, "Grenades / Grenade Solver", "sec_gsolver_settings"};
    }
    if (starts_with(name, "grenade_")) {
        return {1, "Grenades / Grenade Features", "sec_grenade_features"};
    }
    if (name == "trigger_enabled" || name == "trigger_active" || name == "trigger_magnet") {
        return {2, "Combat / General", "sec_general"};
    }
    if (starts_with(name, "trigger_delay") || starts_with(name, "trigger_hitchance") ||
        starts_with(name, "trigger_")) {
        return {2, "Combat / Triggerbot", "sec_trigger_bot_merged"};
    }
    if (starts_with(name, "aim_") || name == "smoke_check" || name == "vischeck") {
        return {2, "Combat / General", "sec_general"};
    }
    // Misc tab master toggles live in the "Misc Features" card.
    if (name == "auto_accept_enabled" || name == "ej_enabled" || name == "kb_viewer_enabled" ||
        name == "spec_list_enabled" || name == "cstrafe_enabled" || name == "team_check") {
        return {3, "Misc / Misc Features", "sec_misc_features"};
    }
    if (starts_with(name, "kb_viewer")) {
        return {3, "Misc / Keybinds Viewer", "sec_misc_kb_viewer"};
    }
    if (starts_with(name, "auto_accept")) {
        return {3, "Misc / Auto Accept", "sec_auto_accept"};
    }
    if (starts_with(name, "ej_") || starts_with(name, "edgebug_")) {
        return {3, "Misc / Edge Jump", "sec_edge_jump"};
    }
    if (starts_with(name, "kb_") || starts_with(name, "key_") || starts_with(name, "trigger_attack_key") ||
        starts_with(name, "trigger_key")) {
        return {3, "Misc / Keybinds", "sec_misc_keybinds"};
    }
    if (starts_with(name, "spec_list")) {
        return {3, "Misc / Spectator List", "sec_misc_spec_list"};
    }
    if (starts_with(name, "cstrafe")) {
        return {3, "Misc / Counter-Strafe Coach", "sec_misc_cstrafe"};
    }
    if (name == "menu_w" || name == "menu_h") {
        return {4, "Config / Menu Appearance", "sec_menu_appearance"};
    }
    if (name == "maps_dir" || name == "vpk_path" || name == "monitor_w" || name == "monitor_h" ||
        name == "game_w" || name == "game_h" || name == "game_x" || name == "game_y" || name == "scaling" ||
        name == "fps" || name == "show_fps" || name == "vsync" || name == "extrapolate" ||
        starts_with(name, "gpu_") || starts_with(name, "monitor_index") || starts_with(name, "hyprland_")) {
        return {5, "Debug / Overlay Scaling", "sec_geoscale"};
    }
    if (starts_with(name, "gamereader_") || name == "debug_game_reader" || name == "focus_check_enabled") {
        return {5, "Debug / Memory Reader", "sec_memreader"};
    }
    if (starts_with(name, "map_visualizer")) {
        return {5, "Debug / Map Geometry", "sec_map_geo"};
    }
    return {5, "Debug / Diagnostics", "sec_debug_diag"};
}

MenuSearchEntry make_setting_entry(const char* name, const char* json_key)
{
    const SearchLocation location = get_setting_location(name);
    const std::string label = humanize_setting_name(name);
    return {label, location.location, label + " " + name + " " + json_key, location.tab_index, location.section_id, name};
}

const std::vector<MenuSearchEntry>& build_menu_search_entries()
{
    static const std::vector<MenuSearchEntry> entries = [] {
        std::vector<MenuSearchEntry> entries = {
        {"Chams", "Players / Chams", "chams material visible hidden", 0, "sec_chams"},
        {"Visible Material", "Players / Chams", "visible material chams", 0, "sec_chams"},
        {"Hidden Material", "Players / Chams", "hidden material occluded chams", 0, "sec_chams"},
        {"Glow", "Players / Glow", "glow outline stencil", 0, "sec_outline"},
        {"Glow Thickness", "Players / Glow", "glow thickness", 0, "sec_outline"},
        {"Glow Color", "Players / Glow", "glow color", 0, "sec_outline"},
        {"Glow Pulse Speed", "Players / Glow", "glow pulse speed", 0, "sec_outline"},
        {"ESP", "Players / ESP", "esp player overlay", 0, "sec_esp_main"},
        {"Limit Distance", "Players / ESP", "limit distance range cutoff", 0, "sec_esp_main"},
        {"ESP Max Distance", "Players / Distance", "esp max distance cutoff fade", 0, "sec_esp_distance"},
        {"Skeleton", "Players / Skeleton", "skeleton bones", 0, "sec_skeleton"},
        {"Box", "Players / Box", "box bounding box", 0, "sec_box"},
        {"Health Bar", "Players / Health Bar", "health hp bar", 0, "sec_hp"},
        {"Armor Bar", "Players / Armor Bar", "armor bar", 0, "sec_armor_bar"},
        {"Ammo Bar", "Players / Ammo Bar", "ammo bar", 0, "sec_ammo_bar"},
        {"Flags", "Players / Flags", "flags player status", 0, "sec_flags"},
        {"Head Circle", "Players / Head Circle", "head circle", 0, "sec_head_circle"},
        {"Weapon Icons", "Players / Weapon Icons", "weapon icons", 0, "sec_weapon_icons"},
        {"Off-screen Arrows", "Players / Off-screen Arrows", "offscreen off screen arrows", 0, "sec_offscreen_arrows"},
        {"Reveals", "Players / Reveals", "reveal smoke damage", 0, "sec_esp_reveals"},
        {"Noise ESP", "Players / Noise ESP", "noise esp footsteps movement", 0, "sec_step_circles"},
        {"DamageFX", "Players / DamageFX", "damage fx kill hit effects impacts particles lightning shockwave pulse", 0,
         "sec_damage_fx"},
        {"Sound ESP", "Players / Sound ESP", "sound esp proximity beep audio cue", 0, "sec_proximity_beep"},
        {"Grenade Features", "Grenades / Grenade Features", "grenade features", 1, "sec_grenade_features"},
        {"Grenade Creator", "Grenades / Grenade Creator", "grenade creator lineup", 1, "sec_gcreator"},
        {"Grenade Helper", "Grenades / Grenade Helper", "grenade helper lineup", 1, "sec_ghelper"},
        {"Grenade Solver", "Grenades / Grenade Solver", "grenade solver trajectory", 1, "sec_gsolver_settings"},
        {"Trajectories", "World / Trajectories", "grenade trajectories throw", 6, "sec_traj_main"},
        {"General", "Combat / General", "combat general", 2, "sec_general"},
        {"Legit Aimbot", "Combat / Legit Aimbot", "legit aim aimbot", 2, "sec_legit_aim"},
        {"FOV", "Combat / Legit Aimbot", "fov field of view", 2, "sec_legit_aim"},
        {"Smoothing", "Combat / Legit Aimbot", "smooth smoothing aim", 2, "sec_legit_aim"},
        {"Hitboxes", "Combat / Hitboxes", "hitbox hitboxes target", 2, "sec_hitboxes"},
        {"Triggerbot", "Combat / Triggerbot", "trigger triggerbot", 2, "sec_trigger_bot_merged"},
        {"Hitchance", "Combat / Triggerbot", "hitchance seed chance", 2, "sec_trigger_bot_merged"},
        {"Trigger Delays", "Combat / Triggerbot", "trigger delay shots", 2, "sec_trigger_bot_merged"},
        {"Misc Features", "Misc / Misc Features", "misc features toggles team check", 3, "sec_misc_features"},
        {"Auto Accept", "Misc / Auto Accept", "auto accept match", 3, "sec_auto_accept"},
        {"Edge Jump", "Misc / Edge Jump", "edge jump edgebug", 3, "sec_edge_jump"},
        {"Keybinds", "Misc / Keybinds", "keybind key binds shortcuts", 3, "sec_misc_keybinds"},
        {"Keybinds Viewer", "Misc / Keybinds Viewer", "keybinds viewer overlay display", 3, "sec_misc_kb_viewer"},
        {"Spectator List", "Misc / Spectator List", "spectator list", 3, "sec_misc_spec_list"},
        {"Counter-Strafe Coach", "Misc / Counter-Strafe Coach", "counter strafe coach movement", 3, "sec_misc_cstrafe"},
        {"Profile Management", "Config / Profiles", "profile config configuration", 4, "sec_configs"},
        {"Menu Appearance", "Config / Menu Appearance", "menu appearance theme colors", 4, "sec_menu_appearance"},
        {"Menu Scale", "Config / Menu Appearance", "menu scale zoom size", 4, "sec_menu_appearance"},
        {"VPK Parser", "Debug / VPK Parser", "vpk parser models", 5, "sec_vpk"},
        {"Map Geometry", "Debug / Map Geometry", "map geometry bvh", 5, "sec_map_geo"},
        {"Overlay Scaling", "Debug / Overlay Scaling", "overlay scaling display", 5, "sec_geoscale"},
        {"Hardware Selection", "Debug / Hardware Selection", "hardware gpu graphics", 5, "sec_hw"},
        {"Memory Reader", "Debug / Memory Reader", "memory reader game reader", 5, "sec_memreader"},
        {"ConVar Reader", "Debug / ConVar Reader", "convar cvar reader", 5, "sec_convar_reader"},
        {"Debug Diagnostics", "Debug / Diagnostics", "debug diagnostics", 5, "sec_debug_diag"},
        {"World Features", "World / World Features", "world features", 6, "sec_world_features"},
        {"Crosshair Overlay", "World / Crosshair Overlay", "crosshair overlay", 6, "sec_force_crosshair"},
        {"Crosshair Distance", "World / Crosshair Distance", "crosshair distance", 6, "sec_crosshair_distance"},
        {"Spread Circle", "World / Spread Circle", "spread circle recoil", 6, "sec_spread_circle"},
        {"Grenade Timers", "World / Timers", "grenade timers c4 bomb timer", 6, "sec_world_timers"},
        {"Bomb Bar", "World / Bomb Bar", "bomb c4 bar", 6, "sec_bomb_bar"},
        {"Dropped Items ESP", "World / Dropped Items", "dropped items esp weapons", 6, "sec_dropped_items"},
        {"Anti Flash", "World / Anti Flash", "anti flash blind", 6, "sec_anti_flash"},
        {"Smoke Wireframe", "World / Smoke Wireframe", "smoke wireframe cage", 6, "sec_smoke_wireframe"},
        {"Through Smokes", "World / Through Smokes", "through smokes wallbang", 6, "sec_through_smokes"},
        {"Bullet Impacts", "World / Bullet Impacts", "bullet impacts hit flash sparks recoil spread 3d box", 6,
         "sec_bullet_impacts"},
        {"Web Radar", "Radar / Web Radar", "web radar browser server port", 7, "sec_radar"},
        {"Radar Server Port", "Radar / Web Radar", "radar port http websocket", 7, "sec_radar"},
        {"Radar Appearance", "Radar / Appearance", "radar appearance dots names zoom", 7, "sec_radar_appearance"},
        };
#define X(type, name, json_key, default_val) entries.push_back(make_setting_entry(#name, json_key));
        CONFIG_FIELDS
#undef X
        return entries;
    }();
    return entries;
}
}

int fuzzy_match_score(std::string_view query, std::string_view candidate)
{
    const std::vector<SearchCharacter> normalized_query = normalize_search_text(query);
    const std::vector<SearchCharacter> normalized_candidate = normalize_search_text(candidate);
    if (normalized_query.empty()) return 0;
    if (normalized_candidate.empty()) return -1;

    int score = 1000 - static_cast<int>(normalized_candidate.size()) * 2;
    std::size_t candidate_index = 0;
    std::size_t previous_match = std::numeric_limits<std::size_t>::max();
    for (const SearchCharacter& query_character : normalized_query) {
        std::size_t match_index = candidate_index;
        while (match_index < normalized_candidate.size() &&
               normalized_candidate[match_index].value != query_character.value) {
            ++match_index;
        }
        if (match_index == normalized_candidate.size()) return -1;

        if (previous_match == std::numeric_limits<std::size_t>::max()) {
            score += match_index == 0 ? 60 : std::max(0, 30 - static_cast<int>(match_index));
        } else if (match_index == previous_match + 1) {
            score += 45;
        } else {
            score -= static_cast<int>(match_index - previous_match - 1) * 4;
        }
        if (normalized_candidate[match_index].word_start) score += 25;

        previous_match = match_index;
        candidate_index = match_index + 1;
    }
    return score;
}

const std::vector<MenuSearchEntry>& get_menu_search_entries()
{
    return build_menu_search_entries();
}

std::vector<MenuSearchMatch> find_menu_search_matches(std::string_view query)
{
    std::vector<MenuSearchMatch> matches;
    for (const MenuSearchEntry& entry : get_menu_search_entries()) {
        const int score = std::max({fuzzy_match_score(query, entry.label),
                                    fuzzy_match_score(query, entry.location),
                                    fuzzy_match_score(query, entry.search_text)});
        if (score >= 0) matches.push_back({&entry, score});
    }

    std::sort(matches.begin(), matches.end(), [](const MenuSearchMatch& left, const MenuSearchMatch& right) {
        if (left.score != right.score) return left.score > right.score;
        return left.entry->label < right.entry->label;
    });
    return matches;
}
