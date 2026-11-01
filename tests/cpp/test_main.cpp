#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

#include "bvh_parser.hpp"
#include "config.hpp"
#include "math/Distance.hpp"
#include "overlay/esp_drawing.hpp"
#include "game_reader.hpp"
#include "focus_monitor.hpp"
#include "offset_scanner.hpp"
#include "app/smoke_wireframe.hpp"
#include "app/DynamicHiding.hpp"
#include "app/HitEffects.hpp"
#include "renderer/HighlightTint.hpp"
#include "app/TriggerBotMath.hpp"
#include "app/ProximityBeep.hpp"
#include "menu/color_clipboard.hpp"
#include "menu/menu_search.hpp"

void test_distance_helpers()
{
    assert(std::abs(distance::units_to_meters(393.7f) - 10.0f) < 1e-4f);
    assert(std::abs(distance::meters_between({0, 0, 0}, {0, 0, 39.37f}) - 1.0f) < 1e-4f);

    const glm::vec3 fwd = distance::view_forward({0.0f, 90.0f, 0.0f});
    assert(std::abs(fwd.y - 1.0f) < 1e-5f);

    const glm::vec3 eye{0.0f, 0.0f, 40.0f};
    const glm::vec3 x_axis{1.0f, 0.0f, 0.0f};
    const glm::vec3 feet{500.0f, 0.0f, 8.0f};
    const glm::vec3 head{500.0f, 0.0f, 64.0f};
    assert(std::abs(distance::ray_capsule_distance(eye, x_axis, feet, head, 16.0f) - 500.0f) < 1e-3f);
    assert(distance::ray_capsule_distance(eye, -x_axis, feet, head, 16.0f) == distance::MISS);
    assert(distance::ray_capsule_distance(eye, {0.0f, 1.0f, 0.0f}, feet, head, 16.0f) == distance::MISS);
    std::cout << "test_distance_helpers passed" << std::endl;
}

void test_esp_distance()
{
    OverlayConfig cfg{};
    PlayerData player{};
    ShmPacket packet{};
    packet.local_eye = {0.0f, 0.0f, 0.0f};
    player.origin = {1000.0f, 0.0f, 0.0f};
    cfg.esp_max_distance = distance::units_to_meters(1000.0f);

    // "Limit Distance" off => always unlimited, regardless of esp_max_distance.
    player.origin.x = 100000.0f;
    assert(!is_beyond_esp_distance(player, packet, cfg));
    assert(std::abs(esp_distance_alpha(500.0f, cfg) - 1.0f) < 1e-5f);

    // Enabling the limit makes esp_max_distance apply.
    cfg.esp_distance_limit_enabled = true;
    player.origin.x = 1000.0f;
    assert(!is_beyond_esp_distance(player, packet, cfg));
    player.origin.x = 1001.0f;
    assert(is_beyond_esp_distance(player, packet, cfg));

    cfg.esp_max_distance = 100.0f;
    cfg.esp_distance_fade_enabled = true;
    cfg.esp_distance_fade_start = 80.0f;
    cfg.esp_distance_fade_min_alpha = 0.2f;
    assert(std::abs(esp_distance_alpha(80.0f, cfg) - 1.0f) < 1e-5f);
    assert(std::abs(esp_distance_alpha(90.0f, cfg) - 0.6f) < 1e-5f);
    assert(std::abs(esp_distance_alpha(100.0f, cfg) - 0.2f) < 1e-5f);

    cfg.esp_max_distance = 0.0f;
    assert(!is_beyond_esp_distance(player, packet, cfg));
    assert(std::abs(esp_distance_alpha(500.0f, cfg) - 1.0f) < 1e-5f);

    cfg.esp_max_distance = distance::units_to_meters(1000.0f);
    cfg.esp_distance_fade_enabled = true;
    cfg.esp_distance_override = true;
    player.origin.x = 100000.0f;
    assert(!is_beyond_esp_distance(player, packet, cfg));
    assert(std::abs(esp_distance_alpha(500.0f, cfg) - 1.0f) < 1e-5f);
    std::cout << "test_esp_distance passed\n";
}

void test_fuzzy_search()
{
    assert(fuzzy_match_score("glow", "Glow Color") > 0);
    assert(fuzzy_match_score("gclr", "Glow Color") > 0);
    assert(fuzzy_match_score("missing", "Glow Color") < 0);

    const std::vector<MenuSearchMatch> trigger_matches = find_menu_search_matches("trig");
    assert(!trigger_matches.empty());
    // The top hit for "trig" must be a trigger setting. Don't pin an exact
    // label: the specific top entry changes as trigger options are added.
    assert(trigger_matches.front().entry->label.rfind("Trigger", 0) == 0);
    bool found_triggerbot = false;
    for (const MenuSearchMatch& match : trigger_matches) {
        if (match.entry->label == "Triggerbot") found_triggerbot = true;
    }
    assert(found_triggerbot);

    const std::vector<MenuSearchMatch> wireframe_matches = find_menu_search_matches("wireframe");
    assert(!wireframe_matches.empty());
    bool found_anti_flash_wireframe = false;
    for (const MenuSearchMatch& match : wireframe_matches) {
        if (match.entry->label == "Anti Flash Wireframe") found_anti_flash_wireframe = true;
    }
    assert(found_anti_flash_wireframe);

    const std::vector<MenuSearchMatch> hitbox_matches = find_menu_search_matches("hitbox scale");
    assert(!hitbox_matches.empty());

    const std::vector<MenuSearchMatch> empty_matches = find_menu_search_matches("");
    assert(empty_matches.size() == get_menu_search_entries().size());
    std::cout << "test_fuzzy_search passed!\n";
}

void test_shm_bounds()
{
    assert(shm::MAX_PLAYERS == 64);
    assert(shm::MAX_BONES == 128);
    assert(shm::MAX_PROJECTILES == 8);
    assert(sizeof(ShmPacket) == 451529);
    assert(sizeof(PlayerData) == 3872);
    assert(sizeof(C4ArmingState) == 16);
    assert(sizeof(InFlightProjectile) == 55);
    assert(sizeof(GameRulesData) == 24);
    std::cout << "test_shm_bounds passed!\n";
}

// m_flFlashOverlayAlpha is normalized to 0..1 on current CS2 builds and 0..255
// on older ones. Anti Flash and the ESP "Flashed" flag both compare against a
// 0..255 threshold, so the reader folds the small domain up before publishing.
void test_flash_alpha()
{
    assert(normalize_flash_alpha(0.0f) == 0.0f);
    assert(std::abs(normalize_flash_alpha(1.0f) - 255.0f) < 0.001f);
    assert(std::abs(normalize_flash_alpha(0.5f) - 127.5f) < 0.001f);

    // A 0..255 build must pass through untouched.
    assert(std::abs(normalize_flash_alpha(255.0f) - 255.0f) < 0.001f);
    assert(std::abs(normalize_flash_alpha(225.0f) - 225.0f) < 0.001f);
    assert(std::abs(normalize_flash_alpha(200.0f) - 200.0f) < 0.001f);

    // The 225 default must engage on a near-full flash and stay idle on a weak
    // one, whichever scale the game happens to expose.
    constexpr float threshold = 225.0f;
    assert(normalize_flash_alpha(0.95f) > threshold);
    assert(normalize_flash_alpha(0.50f) < threshold);
    assert(normalize_flash_alpha(250.0f) > threshold);
    assert(normalize_flash_alpha(120.0f) < threshold);

    // The shipped defaults must live on the same 0..255 scale the reader
    // publishes, otherwise the trigger can never fire (regression guard).
    const OverlayConfig defaults{};
    assert(std::abs(defaults.anti_flash_alpha_start - 200.0f) < 0.001f);
    assert(std::abs(defaults.anti_flash_alpha_end - 125.0f) < 0.001f);
    assert(defaults.anti_flash_alpha_end < defaults.anti_flash_alpha_start);
    assert(std::abs(defaults.anti_flash_wireframe_thickness - 3.0f) < 0.001f);
    assert(defaults.anti_flash_chams_style == ChamsStyleEnum::SOLID);
    assert(defaults.anti_flash_chams);  // forced chams on by default

    // Background must default to fully opaque black.
    const Color bg = defaults.anti_flash_background_color;
    assert(std::abs(bg.r) < 0.001f && std::abs(bg.g) < 0.001f && std::abs(bg.b) < 0.001f);
    assert(std::abs(bg.a - 1.0f) < 0.001f);
    assert(std::abs(defaults.anti_flash_background_opacity - 1.0f) < 0.001f);

    // Wireframe colour must serialize back to the requested 00C8C5FF.
    const Color wire = defaults.anti_flash_wireframe_color;
    assert(std::abs(wire.r * 255.0f) < 0.01f);
    assert(std::abs(wire.g * 255.0f - 200.0f) < 0.01f);
    assert(std::abs(wire.b * 255.0f - 197.0f) < 0.01f);
    assert(std::abs(wire.a * 255.0f - 255.0f) < 0.01f);

    std::cout << "test_flash_alpha passed!\n";
}

// Verifies the scatter-gathered hitbox decode (GameReader::query_hitboxes) against a
// synthetic CModel hitbox array so the batched read path is covered without a live
// game process.
void test_hitbox_parse()
{
    constexpr int kStride = 0x70;
    constexpr int kEntryCount = 25;
    alignas(16) uint8_t raw[kEntryCount * kStride] = {};
    const int16_t remap[8] = {-1, 7, 23, 6, 4, 10, 11, 1};

    auto set_u16 = [](uint8_t* base, size_t offset, uint16_t value) {
        std::memcpy(base + offset, &value, sizeof(value));
    };
    auto set_f32 = [](uint8_t* base, size_t offset, float value) { std::memcpy(base + offset, &value, sizeof(value)); };

    // The caller's mesh offsets are folded into the raw hitbox index before the
    // bone remap lookup: slot = hb_idx + ofs_a + ofs_b (here 1 + 2 = 3).
    // Entry 0: hb_idx 1 -> slot 4 -> remap[4] = bone 4, valid radius, known mins/maxs.
    set_u16(raw + 0 * kStride, 0x48, 1);
    set_f32(raw + 0 * kStride, 0x30, 4.5f);
    set_f32(raw + 0 * kStride, 0x18, -1.0f);
    set_f32(raw + 0 * kStride, 0x1C, -2.0f);
    set_f32(raw + 0 * kStride, 0x20, -3.0f);
    set_f32(raw + 0 * kStride, 0x24, 1.0f);
    set_f32(raw + 0 * kStride, 0x28, 2.0f);
    set_f32(raw + 0 * kStride, 0x2C, 3.0f);

    // Entry 1: hb_idx 0 -> slot 3 -> remap[3] = bone 6.
    set_u16(raw + 1 * kStride, 0x48, 0);
    set_f32(raw + 1 * kStride, 0x30, 5.0f);

    // Entry 2: hb_idx 2 -> slot 5 -> remap[5] = bone 10, but radius 500 is out of bounds -> skipped.
    set_u16(raw + 2 * kStride, 0x48, 2);
    set_f32(raw + 2 * kStride, 0x30, 500.0f);

    // Entry 3: hb_idx 4 -> slot 7 -> remap[7] = bone 1.
    set_u16(raw + 3 * kStride, 0x48, 4);
    set_f32(raw + 3 * kStride, 0x30, 10.0f);

    // Entries 4..24: valid duplicates used to exercise the 20-entry cap.
    for (int i = 4; i < kEntryCount; ++i) {
        set_u16(raw + i * kStride, 0x48, 1);
        set_f32(raw + i * kStride, 0x30, 1.0f);
    }

    TargetHitboxData out{};
    parse_hitbox_entries(raw, kEntryCount, remap, 8, 1, 2, out);
    assert(out.count == 20);
    assert(out.entries[0].bone == 4);
    assert(std::abs(out.entries[0].radius - 4.5f) < 1e-6f);
    assert(std::abs(out.entries[0].mins.x + 1.0f) < 1e-6f);
    assert(std::abs(out.entries[0].mins.z + 3.0f) < 1e-6f);
    assert(std::abs(out.entries[0].maxs.y - 2.0f) < 1e-6f);
    assert(out.entries[1].bone == 6);
    assert(out.entries[1].radius == 5.0f);
    assert(out.entries[2].bone == 1);
    assert(out.entries[2].radius == 10.0f);
    assert(out.entries[19].bone == 4);

    // A remap slot beyond the table is skipped as well.
    TargetHitboxData truncated{};
    parse_hitbox_entries(raw, 1, remap, 1, 0, 0, truncated);
    assert(truncated.count == 0);

    // Degenerate inputs are ignored.
    TargetHitboxData empty{};
    parse_hitbox_entries(nullptr, 4, remap, 8, 0, 0, empty);
    parse_hitbox_entries(raw, 0, remap, 8, 0, 0, empty);
    parse_hitbox_entries(raw, 4, nullptr, 8, 0, 0, empty);
    assert(empty.count == 0);

    std::cout << "test_hitbox_parse passed!\n";
}

void test_bvh_raycast()
{
    LocalMapBVH bvh;
    LocalMapBVH::Triangle tri;
    tri.v0 = glm::vec3(-10.0f, -10.0f, 0.0f);
    tri.v1 = glm::vec3(10.0f, -10.0f, 0.0f);
    tri.v2 = glm::vec3(0.0f, 10.0f, 0.0f);
    bvh.triangles.push_back(tri);
    bvh.build();

    TraceResult res = bvh.trace_ray(glm::vec3(0.0f, 0.0f, 10.0f), glm::vec3(0.0f, 0.0f, -10.0f));
    assert(res.hit == true);
    assert(std::abs(res.end_pos.z) < 1e-4f);
    std::cout << "test_bvh_raycast passed!\n";
}

void test_color_conv()
{
    Color c(1.0f, 0.5f, 0.25f, 1.0f);
    assert(c.r == 1.0f);
    assert(c.g == 0.5f);
    assert(c.b == 0.25f);
    assert(c.a == 1.0f);
    assert(c[0] == 1.0f);
    assert(c[1] == 0.5f);

    Color c2(1.0f, 0.5f, 0.25f, 1.0f);
    assert(c == c2);

    // Test ColorClipboard formatting
    float col_arr[4] = {1.0f, 0.0f, 0.5f, 1.0f};
    char buf[16] = {0};
    ColorClipboard::FormatHexColor(col_arr, buf, sizeof(buf));
    assert(std::string(buf) == "FF0080FF");

    // Test ColorClipboard ApplyHexColor
    float parsed_col[4] = {0};
    assert(ColorClipboard::ApplyHexColor("FF0080FF", parsed_col));
    assert(std::abs(parsed_col[0] - 1.0f) < 0.01f);
    assert(std::abs(parsed_col[1] - 0.0f) < 0.01f);
    assert(std::abs(parsed_col[2] - 0.5019f) < 0.01f);
    assert(std::abs(parsed_col[3] - 1.0f) < 0.01f);

    // Test 6-char hex
    float parsed_col6[4] = {0};
    assert(ColorClipboard::ApplyHexColor("FF0080", parsed_col6));
    assert(std::abs(parsed_col6[0] - 1.0f) < 0.01f);
    assert(std::abs(parsed_col6[2] - 0.5019f) < 0.01f);

    // Test ParseColorString formats
    float parsed_flex[4] = {0};
    assert(ColorClipboard::ParseColorString("#FF0080FF", parsed_flex));
    assert(std::abs(parsed_flex[0] - 1.0f) < 0.01f);

    assert(ColorClipboard::ParseColorString("   #FF0080", parsed_flex));
    assert(std::abs(parsed_flex[2] - 0.5019f) < 0.01f);

    // Invalid format returns false
    assert(!ColorClipboard::ParseColorString("invalid_color_xyz", parsed_flex));

    std::cout << "test_color_conv passed!\n";
}

void test_config_serialize()
{
    Color c(0.1f, 0.2f, 0.3f, 0.4f);
    nlohmann::json j = {static_cast<int>(c.r * 255.0f), static_cast<int>(c.g * 255.0f), static_cast<int>(c.b * 255.0f),
                        static_cast<int>(c.a * 255.0f)};
    assert(j[0] == 25);
    assert(j[1] == 51);
    assert(j[2] == 76);
    assert(j[3] == 102);

    const std::vector<std::pair<std::string, int>> chams_styles = {
        {"solid", 1},
        {"flat", 2},
        {"metallic", 3},
        {"glass", 4},
    };
    for (const auto& [name, style_id] : chams_styles) {
        ChamsStyle style(name);
        assert(static_cast<std::string>(style) == name);
        assert(get_style_id(style) == style_id);
        assert(!chams_style_uses_secondary_color(style));

        nlohmann::json style_json = style;
        assert(style_json == name);
        ChamsStyle round_trip = style_json.get<ChamsStyle>();
        assert(round_trip == style);
    }

    assert(ChamsStyle("textured").value == ChamsStyleEnum::SOLID);
    assert(ChamsStyle("fresnel").value == ChamsStyleEnum::METALLIC);
    const std::vector<std::string> removed_styles = {
        "glow_blend", "glow", "cs2_glow", "wireframe", "hologram", "thermal",
        "cel_shaded", "matrix", "rainbow", "trans", "circuit", "pride",
    };
    for (const std::string& name : removed_styles) {
        assert(ChamsStyle(name).value == ChamsStyleEnum::SOLID);
    }
    // Materials added by the chams expansion were removed again; old configs
    // should degrade to Disabled instead of failing to load.
    const std::vector<std::string> removed_materials = {
        "brushed_alloy", "carbon_fiber", "ceramic",  "duo_tone",  "edge_lit",
        "hex_shield",    "holo_foil",    "liquid_metal", "neon_scan", "pearl",
        "plasma",        "reactive_energy", "signal_pulse", "topographic", "velvet",
    };
    for (const std::string& name : removed_materials) {
        assert(ChamsStyle(name).value == ChamsStyleEnum::DISABLED);
        assert(get_style_id(ChamsStyle(name)) == 0);
    }
    assert(ChamsStyle("not_a_material").value == ChamsStyleEnum::DISABLED);
    assert(get_style_id(ChamsStyle("disabled")) == 0);

    // Test validate_config on valid and invalid files
    std::string valid_filename = "test_valid_config.json";
    std::string invalid_filename = "test_invalid_config.json";
    std::string valid_filepath = get_config_path(valid_filename);
    std::string invalid_filepath = get_config_path(invalid_filename);

    std::ofstream vf(valid_filepath);
    vf << "{\n  \"fps\": 120,\n  \"vsync\": false,\n  \"show_fps\": true,\n  \"extrapolate\": false,\n  \"scaling\": "
          "\"stretched\"\n}\n";
    vf.close();

    std::ofstream ivf(invalid_filepath);
    ivf << "{\n  \"fps\": \"not-an-int\",\n  \"vsync\": false\n}\n";
    ivf.close();

    std::string err;
    assert(validate_config(valid_filename, err) == true);
    assert(validate_config(invalid_filename, err) == false);

    // Clean up
    std::remove(valid_filepath.c_str());
    std::remove(invalid_filepath.c_str());

    // Test KeyBind serialization
    KeyBind kb;
    kb.setting_name = "glow_enabled";
    kb.key = 70;
    kb.mode = BindMode::HOLD;
    kb.is_slider = false;
    kb.slider_value = 1.5f;

    nlohmann::json j_kb = kb;
    assert(j_kb["setting_name"] == "glow_enabled");
    assert(j_kb["key"] == 70);
    assert(j_kb["mode"] == "hold");
    assert(j_kb["slider_value"] == 1.5f);

    KeyBind kb_load = j_kb.get<KeyBind>();
    assert(kb_load.setting_name == "glow_enabled");
    assert(kb_load.key == 70);
    assert(kb_load.mode == BindMode::HOLD);
    assert(kb_load.slider_value == 1.5f);

    std::cout << "test_config_serialize passed!\n";
}

void test_config_profiles()
{
    const std::filesystem::path temp_root =
        std::filesystem::temp_directory_path() /
        ("bunnyware-config-test-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(temp_root);
    setenv("BUNNYWARE_CONFIG_DIR", temp_root.c_str(), 1);

    assert(initialize_config_system());
    std::vector<ConfigProfileInfo> profiles = list_config_profiles();
    assert(profiles.size() == 1);
    assert(profiles.front().name == "default");
    assert(profiles.front().is_default);

    OverlayConfig baseline;
    assert(try_load_config("default.json", baseline));
    assert(!baseline.aim_enabled);
    assert(!baseline.trigger_enabled);
    assert(baseline.esp_enabled);
    assert(baseline.fps_scale == 1.0f);
    assert(baseline.kb_viewer_scale == 1.0f);
    assert(baseline.grenade_helper_enabled);
    assert(!baseline.spec_list_enabled);
    assert(baseline.spec_list_scale == 1.0f);
    assert(baseline.focus_check_enabled);
    assert(baseline.team_check);
    assert(baseline.aim_rifles_rcs);
    assert(baseline.cstrafe_scale == 1.0f);

    OverlayConfig custom = baseline;
    custom.aim_enabled = true;
    const std::string first = create_unique_config("Rifle", custom);
    const std::string second = create_unique_config("Rifle", custom);
    assert(first == "Rifle");
    assert(second == "Rifle - 2");
    assert(set_default_config(second));
    assert(get_default_config_name() == second);

    set_active_config_snapshot(first, custom);
    assert(!active_config_has_unsaved_changes(custom));
    custom.trigger_enabled = true;
    assert(active_config_has_unsaved_changes(custom));
    assert(save_config_profile(first, custom));
    set_active_config_snapshot(first, custom);
    assert(!active_config_has_unsaved_changes(custom));

    // Colors serialize to 4 ints in 0..255, so use values that survive that
    // quantization exactly (multiples of 1/255).
    g_menu_preferences.accent_color = Color(51.0f / 255.0f, 102.0f / 255.0f, 153.0f / 255.0f, 1.0f);
    g_menu_preferences.text_color = Color(204.0f / 255.0f, 178.0f / 255.0f, 153.0f / 255.0f, 1.0f);
    g_menu_preferences.outline_color = Color(76.0f / 255.0f, 51.0f / 255.0f, 26.0f / 255.0f, 128.0f / 255.0f);
    g_menu_preferences.menu_scale = 1.25f;
    set_section_collapsed("sec_chams", true);
    set_section_collapsed("sec_box", true);
    set_section_collapsed("sec_box", false);
    set_section_collapsed("sec_armor_bar", true);
    assert(is_section_collapsed("sec_chams"));
    assert(!is_section_collapsed("sec_box"));
    assert(save_menu_preferences());
    g_menu_preferences = MenuPreferences{};
    assert(!is_section_collapsed("sec_chams"));
    assert(initialize_config_system());
    assert(is_section_collapsed("sec_chams"));
    assert(is_section_collapsed("sec_armor_bar"));
    assert(!is_section_collapsed("sec_box"));
    assert(g_menu_preferences.accent_color == Color(51.0f / 255.0f, 102.0f / 255.0f, 153.0f / 255.0f, 1.0f));
    assert(g_menu_preferences.text_color == Color(204.0f / 255.0f, 178.0f / 255.0f, 153.0f / 255.0f, 1.0f));
    assert(g_menu_preferences.outline_color == Color(76.0f / 255.0f, 51.0f / 255.0f, 26.0f / 255.0f, 128.0f / 255.0f));
    assert(g_menu_preferences.menu_scale == 1.25f);

    std::ifstream profile_file(temp_root / "Rifle.json");
    nlohmann::json profile_json;
    profile_file >> profile_json;
    assert(!profile_json.contains("menu_scale"));

    assert(delete_config(second));
    assert(get_default_config_name() == "default");
    assert(std::filesystem::exists(temp_root / "default.json"));

    {
        std::ofstream corrupt_settings(temp_root / "_settings.json", std::ios::trunc);
        corrupt_settings << "{not valid json";
    }
    assert(initialize_config_system());
    assert(get_default_config_name() == "default");
    assert(g_menu_preferences == MenuPreferences{});

    {
        std::ofstream corrupt_default(temp_root / "default.json", std::ios::trunc);
        corrupt_default << "{\"aim_enabled\":\"wrong type\"}";
    }
    OverlayConfig recovered = load_startup_config();
    assert(!recovered.aim_enabled);
    assert(recovered.esp_enabled);

    unsetenv("BUNNYWARE_CONFIG_DIR");
    std::filesystem::remove_all(temp_root);
    std::cout << "test_config_profiles passed!\n";
}

void test_focus_monitor()
{
    assert(FocusMonitor::matches_cs2_identity("cs2", "cs2", "Counter-Strike 2"));
    assert(FocusMonitor::matches_cs2_identity("steam_app_730", "steam_app_730", ""));
    assert(FocusMonitor::matches_cs2_identity("", "", "", "/games/cs2/bin/linuxsteamrt64/cs2"));
    assert(FocusMonitor::matches_cs2_identity("", "", "Counter-Strike 2 - Vulkan"));

    assert(!FocusMonitor::matches_cs2_identity("org.kde.konsole", "konsole", "Terminal"));
    assert(!FocusMonitor::matches_cs2_identity("firefox", "firefox", "Counter-Strike 2 guide"));
    assert(!FocusMonitor::matches_cs2_identity("firefox", "firefox", "Counter-Strike 2"));
    assert(!FocusMonitor::matches_cs2_identity("", "", "", ""));

    if (std::getenv("BUNNYWARE_TEST_LIVE_FOCUS")) {
        FocusMonitor monitor(true);
        assert(monitor.backend_available());
        for (int i = 0; i < 10; ++i) {
            monitor.is_cs2_focused();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        assert(monitor.has_native_focus_state());
    }
    std::cout << "test_focus_monitor passed!\n";
}

class sha1
{
public:
	void reset( )
	{
		this->m_state[ 0 ] = 0x67452301;
		this->m_state[ 1 ] = 0xEFCDAB89;
		this->m_state[ 2 ] = 0x98BADCFE;
		this->m_state[ 3 ] = 0x10325476;
		this->m_state[ 4 ] = 0xC3D2E1F0;
		this->m_count = 0;
	}

	void update( const void* data, std::size_t len )
	{
		const auto* bytes = static_cast< const std::uint8_t* >( data );
		auto index = static_cast< std::size_t >( this->m_count & 63 );

		this->m_count += len;

		std::size_t i { 0 };

		if ( index )
		{
			auto part_len = 64 - index;

			if ( len >= part_len )
			{
				std::memcpy( this->m_buffer + index, bytes, part_len );
				this->transform( this->m_buffer );
				i = part_len;
			}
			else
			{
				std::memcpy( this->m_buffer + index, bytes, len );
				return;
			}
		}

		for ( ; i + 64 <= len; i += 64 )
		{
			this->transform( bytes + i );
		}

		if ( i < len )
		{
			std::memcpy( this->m_buffer, bytes + i, len - i );
		}
	}

	void final( )
	{
		std::uint8_t padding[ 64 ] {};
		padding[ 0 ] = 0x80;

		auto index = static_cast< std::size_t >( this->m_count & 63 );
		auto pad_len = ( index < 56 ) ? ( 56 - index ) : ( 120 - index );
		auto bit_count = this->m_count * 8;

		this->update( padding, pad_len );

		std::uint8_t bits[ 8 ] { };

		for ( int i = 0; i < 8; ++i )
		{
			bits[ 7 - i ] = static_cast< std::uint8_t >( bit_count >> ( i * 8 ) );
		}

		this->update( bits, 8 );

		for ( int i = 0; i < 5; ++i )
		{
			this->m_digest[ i * 4 + 0 ] = static_cast< std::uint8_t >( this->m_state[ i ] >> 24 );
			this->m_digest[ i * 4 + 1 ] = static_cast< std::uint8_t >( this->m_state[ i ] >> 16 );
			this->m_digest[ i * 4 + 2 ] = static_cast< std::uint8_t >( this->m_state[ i ] >> 8 );
			this->m_digest[ i * 4 + 3 ] = static_cast< std::uint8_t >( this->m_state[ i ] );
		}
	}

	std::uint32_t get_first_uint32( ) const
	{
		std::uint32_t result;
		std::memcpy( &result, this->m_digest, sizeof( result ) );
		return result;
	}

private:
	static std::uint32_t rotl( std::uint32_t v, int n )
	{
		return ( v << n ) | ( v >> ( 32 - n ) );
	}

	void transform( const std::uint8_t* block )
	{
		std::uint32_t w[ 80 ] { };

		for ( int i = 0; i < 16; ++i )
		{
			w[ i ] = ( static_cast< std::uint32_t >( block[ i * 4 + 0 ] ) << 24 ) |
				( static_cast< std::uint32_t >( block[ i * 4 + 1 ] ) << 16 ) |
				( static_cast< std::uint32_t >( block[ i * 4 + 2 ] ) << 8 ) |
				( static_cast< std::uint32_t >( block[ i * 4 + 3 ] ) << 0 );
		}

		for ( int i = 16; i < 80; ++i )
		{
			w[ i ] = rotl( w[ i - 3 ] ^ w[ i - 8 ] ^ w[ i - 14] ^ w[ i - 16 ], 1 );
		}

		std::uint32_t a = this->m_state[ 0 ];
		std::uint32_t b = this->m_state[ 1 ];
		std::uint32_t c = this->m_state[ 2 ];
		std::uint32_t d = this->m_state[ 3 ];
		std::uint32_t e = this->m_state[ 4 ];

		for ( int i = 0; i < 80; ++i )
		{
			std::uint32_t f { 0 };
			std::uint32_t k { 0 };

			if ( i < 20 )
			{
				f = ( b & c ) | ( ~b & d );
				k = 0x5A827999;
			}
			else if ( i < 40 )
			{
				f = b ^ c ^ d;
				k = 0x6ED9EBA1;
			}
			else if ( i < 60 )
			{
				f = ( b & c ) | ( b & d ) | ( c & d );
				k = 0x8F1BBCDC;
			}
			else
			{
				f = b ^ c ^ d;
				k = 0xCA62C1D6;
			}

			std::uint32_t temp = rotl( a, 5 ) + f + e + k + w[ i ];
			e = d;
			d = c;
			c = rotl(b, 30);
			b = a;
			a = temp;
		}

		this->m_state[ 0 ] += a;
		this->m_state[ 1 ] += b;
		this->m_state[ 2 ] += c;
		this->m_state[ 3 ] += d;
		this->m_state[ 4 ] += e;
	}

	std::uint32_t m_state[ 5 ];
	std::uint8_t m_buffer[ 64 ];
	std::uint8_t m_digest[ 20 ];
	std::size_t m_count;
};

void test_triggerbot_math()
{
    // Test 1: normalize_angle
    assert(std::abs(normalize_angle(0.0f)) < 1e-4f);
    assert(std::abs(normalize_angle(360.0f)) < 1e-4f);
    assert(std::abs(normalize_angle(-360.0f)) < 1e-4f);
    assert(std::abs(normalize_angle(720.0f)) < 1e-4f);
    assert(std::abs(normalize_angle(361.0f) - 1.0f) < 1e-4f);
    assert(std::abs(normalize_angle(-359.0f) - 1.0f) < 1e-4f);
    assert(std::abs(normalize_angle(540.0f) - 180.0f) < 1e-4f || std::abs(normalize_angle(540.0f) + 180.0f) < 1e-4f);
    assert(std::abs(normalize_angle(-900.0f) - 180.0f) < 1e-4f || std::abs(normalize_angle(-900.0f) + 180.0f) < 1e-4f);
    std::cout << "test_triggerbot_math: normalize_angle tests passed!\n";

    // Test 2: quantize_angle
    assert(std::abs(quantize_angle(45.2f) - 45.0f) < 1e-4f);
    assert(std::abs(quantize_angle(45.4f) - 45.0f) < 1e-4f);
    assert(std::abs(quantize_angle(45.5f) - 45.5f) < 1e-4f);
    assert(std::abs(quantize_angle(45.7f) - 45.5f) < 1e-4f);
    std::cout << "test_triggerbot_math: quantize_angle tests passed!\n";

    // Test 3: sha1_transform_12bytes matches reference SHA-1 class from itami-v3
    struct TestBuffer {
        float pitch;
        float yaw;
        int tick;
    };

    auto run_ref_sha1 = [](float p, float y, int t) -> uint32_t {
        TestBuffer buf;
        buf.pitch = p;
        buf.yaw = y;
        buf.tick = t;

        sha1 hash;
        hash.reset();
        hash.update(&buf, 12);
        hash.final();
        return hash.get_first_uint32();
    };

    float pitches[] = {0.0f, 45.0f, -45.0f, 89.0f, -89.0f};
    float yaws[] = {0.0f, 90.0f, -90.0f, 180.0f, -180.0f};
    int ticks[] = {0, 1, 1000, 99999, -5};

    for (float p : pitches) {
        for (float y : yaws) {
            for (int t : ticks) {
                uint32_t ref = run_ref_sha1(p, y, t);
                uint32_t opt = sha1_transform_12bytes(p, y, t);
                assert(ref == opt);
            }
        }
    }
    std::cout << "test_triggerbot_math: sha1_transform_12bytes matches reference SHA-1 passed!\n";

    // Test 4: ValveRandomStream LCG logic
    ValveRandomStream prng;
    prng.set_seed(1337);
    float first_r = prng.random_float(0.0f, 1.0f);
    assert(first_r >= 0.0f && first_r <= 1.0f);

    std::cout << "test_triggerbot_math: ValveRandomStream LCG tests passed!\n";
    std::cout << "test_triggerbot_math passed!\n";
}

void test_offset_scanner()
{
    using namespace offset_scanner;

    // Pattern parsing and wildcard scanning.
    Pattern pattern;
    std::string error;
    assert(pattern.parse("48 8B 1D ? ? ? ? 48 85 DB 74 ? 49 89 FC", &error));
    const uint8_t buffer[] = {
        0x90, 0x48, 0x8B, 0x1D, 0x11, 0x22, 0x33, 0x44, 0x48,
        0x85, 0xDB, 0x74, 0x7F, 0x49, 0x89, 0xFC,
    };
    assert(pattern.find(buffer, sizeof(buffer)) == buffer + 1);

    Pattern too_short;
    assert(too_short.parse("FF FF", &error));
    const uint8_t tiny[] = {0xFF};
    assert(too_short.find(tiny, sizeof(tiny)) == nullptr);

    Pattern invalid;
    assert(!invalid.parse("48 ZZ", &error));

    // Operation-chain semantics against a synthetic memory image.
    std::map<uintptr_t, std::vector<uint8_t>> memory;
    auto write_bytes = [&](uintptr_t address, const void* data, size_t size) {
        std::vector<uint8_t>& block = memory[address];
        block.resize(size);
        std::memcpy(block.data(), data, size);
    };
    auto write_value = [&](uintptr_t address, auto value) {
        write_bytes(address, &value, sizeof(value));
    };
    auto read_memory = [&](uintptr_t address, void* out, size_t size) {
        const auto it = memory.find(address);
        if (it == memory.end() || it->second.size() < size) return false;
        std::memcpy(out, it->second.data(), size);
        return true;
    };

    // rip: disp32 at +3 == 0x40, instruction at 0x1000, length 7.
    write_value(static_cast<uintptr_t>(0x1003), static_cast<int32_t>(0x40));
    uint32_t rva = 0;
    assert(apply_operations(0x1000, { { OperationKind::rip, 0, 3, 7 } }, read_memory, 0x1000, &rva));
    assert(rva == 0x47);

    // read + add: pointer dereference then relative adjustment.
    write_value(static_cast<uintptr_t>(0x2000), static_cast<uint64_t>(0x3000));
    assert(apply_operations(0x2000, { { OperationKind::read }, { OperationKind::add, 0x10 } },
                            read_memory, 0x1000, &rva));
    assert(rva == 0x2010);

    // slice yields the small constant value (as dwSensitivity_sensitivity does).
    const uint8_t small[] = {0x58, 0x00};
    write_bytes(static_cast<uintptr_t>(0x4000), small, sizeof(small));
    assert(apply_operations(0x4000, { { OperationKind::slice, 0, 0, 2 } }, read_memory, 0x1000, &rva));
    assert(rva == 0x58);

    // sub + rip combined (dwEntityList shape: match - 62, then disp32 at +3).
    write_value(static_cast<uintptr_t>(0x1FC5), static_cast<int32_t>(0x40));
    assert(apply_operations(0x2000, { { OperationKind::sub, 62 }, { OperationKind::rip, 0, 3, 7 } },
                            read_memory, 0x1000, &rva));
    assert(rva == 0x1009);

    // Reads of unmapped addresses fail instead of fabricating values.
    assert(!apply_operations(0x9999, { { OperationKind::rip, 0, 3, 7 } }, read_memory, 0x1000, &rva));

    std::cout << "test_offset_scanner passed!\n";
}


void test_hit_features()
{
    float color[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    const float tint[4] = {0.0f, 1.0f, 0.0f, 0.5f};
    tint_highlight(color, 0.5f, tint);
    assert(color[0] == 0.0f && color[1] == 1.0f && color[2] == 0.0f);
    assert(std::abs(color[3] - 0.25f) < 1e-6f);
    float shadow[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    tint_highlight(shadow, 1.0f, tint);
    assert(shadow[1] == 0.0f && std::abs(shadow[3] - 0.5f) < 1e-6f);

    OverlayConfig cfg;
    cfg.chams_hide_when_flashed = true;
    cfg.glow_hide_when_flashed = false;
    cfg.esp_hide_when_flashed = false;
    cfg.style_vis = ChamsStyleEnum::FLAT;
    cfg.glow_enabled = true;
    OverlayConfig untouched = cfg;
    apply_flash_visibility(untouched, 0.0f);
    assert(untouched.style_vis == ChamsStyleEnum::FLAT);
    OverlayConfig flashed = cfg;
    apply_flash_visibility(flashed, 0.6f);
    assert(flashed.style_vis == ChamsStyleEnum::DISABLED && flashed.glow_enabled);

    cfg.esp_enabled = true;
    cfg.esp_hide_while_shooting = true;
    cfg.chams_hide_while_shooting = false;
    cfg.glow_hide_while_shooting = false;
    OverlayConfig shooting = cfg;
    apply_shooting_visibility(shooting, true);
    assert(!shooting.esp_enabled && shooting.style_vis == ChamsStyleEnum::FLAT);
    assert(!shooting.esp_hide_when_flashed && shooting.chams_hide_when_flashed);

    hitfx::HitDetector detector;
    detector.set_use_shot_sample(true);
    const auto sample = hitfx::aim_sample({0.0f, 0.0f, 64.0f}, {0.0f, 90.0f, 0.0f}, {});
    assert(sample.ray.valid);
    std::vector<hitfx::EnemySample> enemies = {{5, 42u, 100}};
    assert(detector.update(0.0, true, 0, sample, enemies, true).empty());
    enemies[0].health = 100;
    assert(detector.update(0.1, true, 0, sample, enemies, true).empty());
    enemies[0].health = 73;
    auto events = detector.update(0.2, true, 1, sample, enemies, true);
    assert(events.size() == 1 && events[0].entity == 5 && events[0].damage == 27 && !events[0].kill);
    assert(detector.new_shots() == 1);
    enemies[0].identity = 7u;
    enemies[0].health = 10;
    assert(detector.update(0.25, true, 1, sample, enemies, true).empty());
    enemies[0].health = 0;
    assert(detector.update(2.0, true, 1, sample, enemies, true).empty());

    const auto rays = hitfx::shot_rays(sample, 4);
    assert(rays.rays.size() == 1 && rays.rays[0].valid);

    hitfx::HitFx fx;
    hitfx::EffectParams params;
    params.count = 6;
    fx.spawn_effect({0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, params);
    assert(fx.particles().size() == 6);
    for (int i = 0; i < 20; ++i) fx.step(0.1f, -300.0f);
    assert(fx.empty());

    std::cout << "test_hit_features passed!\n";
}

void test_smoke_wireframe()
{
    auto expand_bits = [](uint32_t v) -> uint32_t {
        v = (v | (v << 16)) & 0x030000FF;
        v = (v | (v << 8)) & 0x0300F00F;
        v = (v | (v << 4)) & 0x030C30C3;
        v = (v | (v << 2)) & 0x09249249;
        return v;
    };
    auto set_voxel = [&](ActiveSmokeData& smoke, int x, int y, int z, uint8_t alpha) {
        smoke.alphas[x + 32 * (y + 32 * z)] = alpha;
        const uint32_t morton = expand_bits(static_cast<uint32_t>(x)) |
                                (expand_bits(static_cast<uint32_t>(y)) << 1) |
                                (expand_bits(static_cast<uint32_t>(z)) << 2);
        smoke.bitset[morton >> 6] |= (1ULL << (morton & 0x3F));
    };

    // --- Dense 3x3x3 cube at voxel [10, 12]^3 ------------------------------
    // Three occupied layers; each becomes one full contour ring plus one strut
    // per ring segment in every layer gap: 3*K + 2*K = 5*K lines. The outermost
    // rings are extended to the outer voxel faces (z -80 and -20 here).
    ActiveSmokeData smoke{};
    smoke.active = 1;
    smoke.center = Vec3(100.0f, -200.0f, 50.0f);
    for (int x = 10; x <= 12; ++x)
        for (int y = 10; y <= 12; ++y)
            for (int z = 10; z <= 12; ++z) set_voxel(smoke, x, y, z, 255);

    std::vector<smoke_wireframe::CageLine> lines;
    smoke_wireframe::collect_cage_lines(smoke, lines);
    assert(lines.size() == 5 * static_cast<std::size_t>(smoke_wireframe::kCageRingSegments));

    auto same_point = [](const Vec3& p, float x, float y, float z) {
        return std::abs(p.x - x) < 1e-3f && std::abs(p.y - y) < 1e-3f && std::abs(p.z - z) < 1e-3f;
    };
    auto has_point = [&](const std::vector<smoke_wireframe::CageLine>& cage, const Vec3& p) {
        for (const smoke_wireframe::CageLine& line : cage) {
            if (same_point(line.a, p.x, p.y, p.z) || same_point(line.b, p.x, p.y, p.z)) return true;
        }
        return false;
    };
    auto has_line = [&](const std::vector<smoke_wireframe::CageLine>& cage, const Vec3& a, const Vec3& b) {
        for (const smoke_wireframe::CageLine& line : cage) {
            if ((same_point(line.a, a.x, a.y, a.z) && same_point(line.b, b.x, b.y, b.z)) ||
                (same_point(line.a, b.x, b.y, b.z) && same_point(line.b, a.x, a.y, a.z)))
                return true;
        }
        return false;
    };
    auto has_vertex = [&](const std::vector<Vec3>& shell, const Vec3& p) {
        for (const Vec3& v : shell) {
            if (same_point(v, p.x, p.y, p.z)) return true;
        }
        return false;
    };

    // Each layer's contour passes through the outer voxel faces: the middle
    // layer's centroid is (0, -300) in world space and its +X sample reaches
    // 30 units out (20 to the cell center + the 10-unit cell half), and the
    // rings are tied together by vertical struts.
    assert(has_point(lines, Vec3(30.0f, -300.0f, -50.0f)));
    assert(has_line(lines, Vec3(30.0f, -300.0f, -80.0f), Vec3(30.0f, -300.0f, -50.0f)));
    assert(has_line(lines, Vec3(30.0f, -300.0f, -20.0f), Vec3(30.0f, -300.0f, -50.0f)));

    // The filled shell used to stamp the through-smokes silhouette follows the
    // same rings: two triangles per ring gap segment, three vertices each.
    std::vector<Vec3> cube_shell;
    smoke_wireframe::collect_cage_triangles(smoke, cube_shell);
    assert(cube_shell.size() == 288);  // 2 gaps * 24 segments * 2 triangles * 3
    assert(has_vertex(cube_shell, Vec3(30.0f, -300.0f, -20.0f)));
    assert(has_vertex(cube_shell, Vec3(30.0f, -300.0f, -80.0f)));

    // The ring-level API lets renderers build the rings once and feed both the
    // line and shell collectors.
    smoke_wireframe::CageRing shared_rings[smoke_wireframe::kMaxCageRings];
    const int shared_count = smoke_wireframe::build_cage_rings(smoke, shared_rings, smoke_wireframe::kMaxCageRings);
    assert(shared_count == 3);
    std::vector<smoke_wireframe::CageLine> ring_lines;
    std::vector<Vec3> ring_shell;
    smoke_wireframe::collect_cage_lines_from_rings(shared_rings, shared_count, ring_lines,
                                                   smoke_wireframe::kMaxCageLines);
    smoke_wireframe::collect_cage_triangles_from_rings(shared_rings, shared_count, ring_shell,
                                                       smoke_wireframe::kMaxCageTriangles);
    assert(ring_lines.size() == lines.size());
    assert(ring_shell.size() == cube_shell.size());

    // Zero rings emit nothing.
    ring_lines.clear();
    ring_shell.clear();
    smoke_wireframe::collect_cage_lines_from_rings(shared_rings, 0, ring_lines, smoke_wireframe::kMaxCageLines);
    smoke_wireframe::collect_cage_triangles_from_rings(shared_rings, 0, ring_shell,
                                                       smoke_wireframe::kMaxCageTriangles);
    assert(ring_lines.empty());
    assert(ring_shell.empty());

    // --- Sphere-like blob: all cells within a radius-6 voxel ball ----------
    // 13 occupied layers produce a lathe of contour rings (13 rings + 12 strut
    // sets), widest at the equator (6*20 + 10 = 130 from the axis) and
    // tapering to 10 at both caps, so the cage follows the smoke shape instead
    // of the voxel grid.
    ActiveSmokeData ball{};
    ball.active = 1;
    ball.center = Vec3(0.0f, 0.0f, 0.0f);
    for (int x = 0; x < 32; ++x)
        for (int y = 0; y < 32; ++y)
            for (int z = 0; z < 32; ++z) {
                const int dx = x - 16, dy = y - 16, dz = z - 16;
                if (dx * dx + dy * dy + dz * dz <= 36) set_voxel(ball, x, y, z, 255);
            }

    std::vector<smoke_wireframe::CageLine> ball_lines;
    smoke_wireframe::collect_cage_lines(ball, ball_lines);
    // 13 rings + 12 strut sets; the caps are extended to z +/- 130.
    assert(ball_lines.size() == 25 * static_cast<std::size_t>(smoke_wireframe::kCageRingSegments));
    assert(has_point(ball_lines, Vec3(130.0f, 0.0f, 0.0f)));
    assert(has_point(ball_lines, Vec3(10.0f, 0.0f, 130.0f)));
    assert(has_point(ball_lines, Vec3(10.0f, 0.0f, -130.0f)));
    assert(has_line(ball_lines, Vec3(110.0f, 0.0f, -20.0f), Vec3(130.0f, 0.0f, 0.0f)));

    std::vector<Vec3> ball_shell;
    smoke_wireframe::collect_cage_triangles(ball, ball_shell);
    assert(ball_shell.size() == 1728);  // 12 gaps * 24 segments * 2 triangles * 3

    // --- Threshold, cap and inactive cases ---------------------------------
    // A single dense layer becomes a short two-ring cylinder spanning the voxel
    // cell's full 20-unit height: 2 rings + 1 strut set = 3*K lines and a shell
    // of 2*K triangles.
    ActiveSmokeData single{};
    single.active = 1;
    single.center = Vec3(0.0f, 0.0f, 0.0f);
    set_voxel(single, 15, 15, 15, smoke_wireframe::kSmokeAlphaThreshold);
    std::vector<smoke_wireframe::CageLine> single_lines;
    smoke_wireframe::collect_cage_lines(single, single_lines);
    assert(single_lines.size() == 3 * static_cast<std::size_t>(smoke_wireframe::kCageRingSegments));
    std::vector<Vec3> single_shell;
    smoke_wireframe::collect_cage_triangles(single, single_shell);
    assert(single_shell.size() == 2 * static_cast<std::size_t>(smoke_wireframe::kCageRingSegments) * 3);

    // The per-voxel cutoff matches the vischeck: "alpha < 15" is ignored there.
    single.alphas[15 + 32 * (15 + 32 * 15)] = smoke_wireframe::kSmokeAlphaThreshold - 1;
    single_lines.clear();
    smoke_wireframe::collect_cage_lines(single, single_lines);
    assert(single_lines.empty());
    single_shell.clear();
    smoke_wireframe::collect_cage_triangles(single, single_shell);
    assert(single_shell.empty());

    // Two stacked dense voxels: two rings plus their strut set.
    ActiveSmokeData stacked{};
    stacked.active = 1;
    stacked.center = Vec3(0.0f, 0.0f, 0.0f);
    set_voxel(stacked, 15, 15, 12, 255);
    set_voxel(stacked, 15, 15, 13, 255);
    std::vector<smoke_wireframe::CageLine> stacked_lines;
    smoke_wireframe::collect_cage_lines(stacked, stacked_lines);
    assert(stacked_lines.size() == 3 * static_cast<std::size_t>(smoke_wireframe::kCageRingSegments));

    // The caps bound the emitted cage without dropping the whole smoke.
    std::vector<smoke_wireframe::CageLine> capped;
    smoke_wireframe::collect_cage_lines(smoke, capped, 10);
    assert(capped.size() == 10);
    std::vector<Vec3> capped_shell;
    smoke_wireframe::collect_cage_triangles(smoke, capped_shell, 10);
    assert(capped_shell.size() == 30);

    // Inactive smokes contribute nothing.
    ActiveSmokeData inactive = smoke;
    inactive.active = 0;
    std::vector<smoke_wireframe::CageLine> none;
    smoke_wireframe::collect_cage_lines(inactive, none);
    assert(none.empty());
    std::vector<Vec3> no_shell;
    smoke_wireframe::collect_cage_triangles(inactive, no_shell);
    assert(no_shell.empty());

    std::cout << "test_smoke_wireframe passed!\n";
}

void test_proximity_beep()
{
    constexpr float kDegToRad = 3.14159265f / 180.0f;
    auto packet = std::make_unique<ShmPacket>();
    std::memset(packet.get(), 0, sizeof(ShmPacket));
    packet->local_team = 3;
    packet->local_health = 100;
    packet->local_angles = Vec3(0.0f, 0.0f, 0.0f);

    OverlayConfig cfg;
    cfg.proximity_beep_enabled = true;
    cfg.proximity_beep_max_range = 30.0f;
    cfg.proximity_beep_fov = 110.0f;
    cfg.proximity_beep_fast_interval = 0.15f;
    cfg.proximity_beep_slow_interval = 1.0f;
    cfg.proximity_beep_volume = 0.8f;
    cfg.team_check = true;

    auto place = [&](int slot, int team, int health, float meters, float bearing_deg = 0.0f) {
        PlayerData& player = packet->players[slot];
        const float radians = bearing_deg * kDegToRad;
        player.active = 1;
        player.team = team;
        player.health = health;
        player.origin = Vec3(meters * distance::UNITS_PER_METER * std::cos(radians),
                             meters * distance::UNITS_PER_METER * std::sin(radians), 0.0f);
        packet->player_count = std::max(packet->player_count, slot + 1);
    };
    auto set_enemy = [&](float meters, float bearing_deg = 0.0f) {
        std::memset(packet->players, 0, sizeof(packet->players));
        packet->player_count = 0;
        place(0, 2, 100, meters, bearing_deg);
    };
    auto heard = [&]() { return proximity_beep::nearest_enemy_in_cone_meters(*packet, cfg).has_value(); };

    assert(!proximity_beep::nearest_enemy_in_cone_meters(*packet, cfg));

    place(0, 2, 100, 10.0f);
    place(1, 3, 100, 2.0f);
    place(2, 2, 0, 1.0f);
    place(3, 2, 100, 25.0f);
    packet->players[4].active = 0;
    packet->players[4].team = 2;
    packet->players[4].health = 100;
    packet->player_count = 5;
    const auto nearest = proximity_beep::nearest_enemy_in_cone_meters(*packet, cfg);
    assert(nearest && std::fabs(*nearest - 10.0f) < 1e-3f);
    cfg.team_check = false;
    const auto with_team = proximity_beep::nearest_enemy_in_cone_meters(*packet, cfg);
    assert(with_team && std::fabs(*with_team - 2.0f) < 1e-3f);
    cfg.team_check = true;

    set_enemy(10.0f, 50.0f);
    assert(heard());
    set_enemy(10.0f, -50.0f);
    assert(heard());
    set_enemy(10.0f, 60.0f);
    assert(!heard());
    set_enemy(10.0f, 180.0f);
    assert(!heard());
    set_enemy(0.0f);
    assert(heard());

    cfg.proximity_beep_fov = 30.0f;
    set_enemy(10.0f, 14.0f);
    assert(heard());
    set_enemy(10.0f, 16.0f);
    assert(!heard());

    cfg.proximity_beep_fov = 360.0f;
    set_enemy(10.0f, 180.0f);
    assert(!heard());
    set_enemy(10.0f, 95.0f);
    assert(!heard());
    set_enemy(10.0f, 85.0f);
    assert(heard());
    cfg.proximity_beep_fov = 110.0f;

    packet->local_angles.y = 90.0f;
    set_enemy(10.0f, 0.0f);
    assert(!heard());
    set_enemy(10.0f, 90.0f);
    assert(heard());
    packet->local_angles.y = 0.0f;

    assert(!proximity_beep::make_plan(31.0f, cfg));
    assert(!proximity_beep::make_plan(-1.0f, cfg));
    const int top_step = proximity_beep::kPitchSteps - 1;
    const auto point_blank = proximity_beep::make_plan(0.0f, cfg);
    assert(point_blank && std::fabs(point_blank->interval_sec - 0.15f) < 1e-5f);
    assert(std::fabs(point_blank->volume - 0.8f) < 1e-5f);
    assert(point_blank->pitch_step == top_step);
    const auto at_full = proximity_beep::make_plan(proximity_beep::kFullIntensityMeters, cfg);
    assert(at_full && std::fabs(at_full->interval_sec - 0.15f) < 1e-5f);
    assert(std::fabs(at_full->volume - 0.8f) < 1e-5f);
    assert(at_full->pitch_step == top_step);
    const auto at_edge = proximity_beep::make_plan(30.0f, cfg);
    assert(at_edge && std::fabs(at_edge->interval_sec - 1.0f) < 1e-5f);
    assert(std::fabs(at_edge->volume - 0.8f * proximity_beep::kQuietestFraction) < 1e-5f);
    assert(at_edge->pitch_step == 0);

    const auto halfway = proximity_beep::make_plan(16.5f, cfg);
    const float strength = std::pow(0.5f, proximity_beep::kFalloffCurve);
    assert(halfway);
    assert(std::fabs(halfway->interval_sec - (1.0f + (0.15f - 1.0f) * strength)) < 1e-4f);
    assert(std::fabs(halfway->volume - 0.8f * (0.25f + 0.75f * strength)) < 1e-4f);
    assert(halfway->pitch_step == static_cast<int>(std::lround(strength * static_cast<float>(top_step))));

    float last_interval = 1e9f;
    float last_volume = -1.0f;
    int last_pitch = -1;
    for (const float meters : {28.0f, 20.0f, 10.0f, 2.0f}) {
        const auto plan = proximity_beep::make_plan(meters, cfg);
        assert(plan);
        assert(plan->interval_sec < last_interval);
        assert(plan->volume > last_volume);
        assert(plan->pitch_step >= last_pitch);
        last_interval = plan->interval_sec;
        last_volume = plan->volume;
        last_pitch = plan->pitch_step;
    }

    assert(std::fabs(proximity_beep::pitch_step_hz(0) - proximity_beep::kPitchFarHz) < 1e-3f);
    assert(std::fabs(proximity_beep::pitch_step_hz(top_step) - proximity_beep::kPitchNearHz) < 1e-2f);
    assert(proximity_beep::pitch_step_hz(7) < proximity_beep::pitch_step_hz(8));
    assert(std::fabs(proximity_beep::pitch_step_hz(99) - proximity_beep::kPitchNearHz) < 1e-2f);

    OverlayConfig short_range = cfg;
    short_range.proximity_beep_max_range = 10.0f;
    assert(!proximity_beep::make_plan(10.5f, short_range));
    const auto short_edge = proximity_beep::make_plan(10.0f, short_range);
    assert(short_edge && std::fabs(short_edge->interval_sec - 1.0f) < 1e-5f);
    short_range.proximity_beep_max_range = 4.0f;
    const auto tiny_range = proximity_beep::make_plan(2.0f, short_range);
    assert(tiny_range && std::fabs(tiny_range->interval_sec - 0.15f) < 1e-5f);

    OverlayConfig inverted = cfg;
    inverted.proximity_beep_fast_interval = 0.5f;
    inverted.proximity_beep_slow_interval = 0.2f;
    inverted.proximity_beep_volume = 4.0f;
    const auto clamped = proximity_beep::make_plan(0.0f, inverted);
    assert(clamped && clamped->interval_sec >= 0.5f - 1e-5f);
    assert(clamped->volume <= 1.0f + 1e-5f);

    ProximityBeep scheduler;
    set_enemy(20.0f);
    assert(scheduler.update(*packet, cfg, 0.0));
    assert(!scheduler.update(*packet, cfg, 0.05));
    assert(!scheduler.update(*packet, cfg, 0.70));
    assert(scheduler.update(*packet, cfg, 0.80));

    set_enemy(3.0f);
    assert(!scheduler.update(*packet, cfg, 0.85));
    const auto closer = scheduler.update(*packet, cfg, 1.0);
    assert(closer);
    assert(closer->volume > proximity_beep::make_plan(20.0f, cfg)->volume);
    assert(closer->pitch_step > proximity_beep::make_plan(20.0f, cfg)->pitch_step);

    set_enemy(50.0f);
    assert(!scheduler.update(*packet, cfg, 1.1));
    set_enemy(20.0f);
    assert(scheduler.update(*packet, cfg, 1.11));

    set_enemy(20.0f, 180.0f);
    assert(!scheduler.update(*packet, cfg, 1.2));
    set_enemy(20.0f);
    assert(scheduler.update(*packet, cfg, 1.21));

    packet->local_health = 0;
    assert(!scheduler.update(*packet, cfg, 5.0));
    packet->local_health = 100;
    assert(scheduler.update(*packet, cfg, 5.1));
    cfg.proximity_beep_enabled = false;
    assert(!scheduler.update(*packet, cfg, 9.0));

    std::cout << "test_proximity_beep passed!\n";
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <test-flag>\n";
        return 1;
    }

    std::string arg = argv[1];
    if (arg == "--distance") {
        test_distance_helpers();
    } else if (arg == "--fuzzy-search") {
        test_fuzzy_search();
    } else if (arg == "--esp-distance") {
        test_esp_distance();
    } else if (arg == "--shm-bounds") {
        test_shm_bounds();
    } else if (arg == "--flash-alpha") {
        test_flash_alpha();
    } else if (arg == "--hitbox-parse") {
        test_hitbox_parse();
    } else if (arg == "--bvh-raycast") {
        test_bvh_raycast();
    } else if (arg == "--color-conv") {
        test_color_conv();
    } else if (arg == "--config-serialize") {
        test_config_serialize();
    } else if (arg == "--config-profiles") {
        test_config_profiles();
    } else if (arg == "--focus-monitor") {
        test_focus_monitor();
    } else if (arg == "--offset-scanner") {
        test_offset_scanner();
    } else if (arg == "--proximity-beep") {
        test_proximity_beep();
    } else if (arg == "--triggerbot-math") {
        test_triggerbot_math();
    } else if (arg == "--smoke-wireframe") {
        test_smoke_wireframe();
    } else if (arg == "--hit-features") {
        test_hit_features();
    } else {
        std::cerr << "Unknown test flag: " << arg << "\n";
        return 1;
    }

    return 0;
}
