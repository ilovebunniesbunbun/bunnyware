#include "menu_client.hpp"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cctype>
#include <filesystem>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

#include "external/imgui/imgui.h"
#include "external/imgui/imgui_impl_glfw.h"
#include "external/imgui/imgui_impl_opengl3.h"
#include "external/imgui/imgui_internal.h"
#include "logger.hpp"
#include "menu_animation.hpp"
#include "menu_tabs.hpp"
#include "menu_theme.hpp"
#include "overlay/game_reader.hpp"
#include "renderer/gl_loader.hpp"
#include "app/SvgCache.hpp"

MenuClient* MenuClient::g_instance = nullptr;

MenuClient::MenuClient(OverlayConfig& config, const GameReader& reader, const SvgCache& svg_cache,
                       GLFWwindow* share_window)
    : cfg(config), game_reader(reader), svg_cache(svg_cache)
{
    g_instance = this;
    init_window(share_window);
    init_imgui();
}

MenuClient::~MenuClient()
{
    if (g_instance == this) {
        g_instance = nullptr;
    }
    glfwMakeContextCurrent(window);
    if (imgui_context) {
        ImGui::SetCurrentContext(imgui_context);
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext(imgui_context);
        imgui_context = nullptr;
    }
    if (window) {
        glfwDestroyWindow(window);
        window = nullptr;
    }
}

bool MenuClient::should_close() const
{
    return glfwWindowShouldClose(window);
}

static constexpr float kMenuScaleMin = 0.75f;
static constexpr float kMenuScaleMax = 2.0f;
static constexpr float kMenuScaleStep = 0.05f;
static constexpr float kMenuScaleDefault = 1.0f;
// Preferred logical window size at 100% menu scale. The window manager owns the
// actual size once the window exists; this is only the opening/scale-change size.
static constexpr float kMenuPreferredWidth = 820.0f;
static constexpr float kMenuPreferredHeight = 620.0f;
static constexpr int kMenuMinWidth = 560;
static constexpr int kMenuMinHeight = 420;
static constexpr double kZoomToastDuration = 1.2;
static constexpr double kZoomToastFade = 0.3;
static double g_zoom_toast_until = 0.0;

// GLFW window sizes are in screen coordinates, while framebuffer scaling makes
// the framebuffer `content_scale` times larger. Divide by the content scale so
// the *framebuffer* keeps the preferred extent on HiDPI displays.
static float menu_content_scale(GLFWwindow* window)
{
    float scale_x = 1.0f;
    float scale_y = 1.0f;
    if (window) {
        glfwGetWindowContentScale(window, &scale_x, &scale_y);
    } else if (GLFWmonitor* monitor = glfwGetPrimaryMonitor()) {
        glfwGetMonitorContentScale(monitor, &scale_x, &scale_y);
    }
    return scale_x > 0.0f ? scale_x : 1.0f;
}

static int menu_preferred_extent(float base, GLFWwindow* window)
{
    return display_scaling::rounded_pixel_extent(
        base * g_menu_preferences.menu_scale / menu_content_scale(window));
}

static void set_menu_scale(float scale)
{
    const float stepped = std::round(scale / kMenuScaleStep) * kMenuScaleStep;
    g_menu_preferences.menu_scale = std::clamp(stepped, kMenuScaleMin, kMenuScaleMax);
    g_zoom_toast_until = glfwGetTime() + kZoomToastDuration;
    schedule_menu_preferences_save();
}

static bool handle_zoom_shortcut(int key, int mods)
{
    if ((mods & GLFW_MOD_CONTROL) == 0) return false;
    if (key == GLFW_KEY_EQUAL || key == GLFW_KEY_KP_ADD) {
        set_menu_scale(g_menu_preferences.menu_scale + kMenuScaleStep);
        return true;
    }
    if (key == GLFW_KEY_MINUS || key == GLFW_KEY_KP_SUBTRACT) {
        set_menu_scale(g_menu_preferences.menu_scale - kMenuScaleStep);
        return true;
    }
    if (key == GLFW_KEY_0 || key == GLFW_KEY_KP_0) {
        set_menu_scale(kMenuScaleDefault);
        return true;
    }
    return false;
}

static int menu_tab_nav_slot(int tab_index)
{
    switch (tab_index) {
        case 2: return 0;
        case 0: return 1;
        case 6: return 2;
        case 1: return 3;
        case 3: return 4;
        case 7: return 5;
        case 4: return 6;
        case 5: return 7;
        default: return 1;
    }
}

static void render_zoom_toast()
{
    const double remaining = g_zoom_toast_until - glfwGetTime();
    if (remaining <= 0.0) return;
    const float alpha = static_cast<float>(std::clamp(remaining / kZoomToastFade, 0.0, 1.0));

    char text[32];
    std::snprintf(text, sizeof(text), "Menu scale %d%%",
                  static_cast<int>(std::round(g_menu_preferences.menu_scale * 100.0f)));

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float scale = ImGui::GetFontSize() / 13.0f;
    const ImVec2 padding(14.0f * scale, 8.0f * scale);
    const ImVec2 text_size = ImGui::CalcTextSize(text);
    const ImVec2 box_size(text_size.x + padding.x * 2.0f, text_size.y + padding.y * 2.0f);
    const ImVec2 box_min(viewport->WorkPos.x + (viewport->WorkSize.x - box_size.x) * 0.5f,
                         viewport->WorkPos.y + viewport->WorkSize.y - box_size.y - 24.0f * scale);
    const ImVec2 box_max(box_min.x + box_size.x, box_min.y + box_size.y);

    ImVec4 bg = MenuTheme::bg_card;
    ImVec4 border = MenuTheme::border_subtle;
    ImVec4 fg = MenuTheme::text_primary;
    bg.w *= alpha;
    border.w *= alpha;
    fg.w *= alpha;

    ImDrawList* draw_list = ImGui::GetForegroundDrawList();
    draw_list->AddRectFilled(box_min, box_max, ImGui::GetColorU32(bg), 6.0f * scale);
    draw_list->AddRect(box_min, box_max, ImGui::GetColorU32(border), 6.0f * scale);
    draw_list->AddText(ImVec2(box_min.x + padding.x, box_min.y + padding.y), ImGui::GetColorU32(fg), text);
}

static void menu_key_callback(GLFWwindow* window, int key, int scancode, int action, int mods)
{
    (void)scancode;
    if (action != GLFW_PRESS && action != GLFW_REPEAT) return;
    MenuClient* client = static_cast<MenuClient*>(glfwGetWindowUserPointer(window));
    if (!client) return;
    if (client->is_listening_keybind()) {
        if (action == GLFW_PRESS) client->set_listened_key(key);
        return;
    }
    if (action == GLFW_PRESS && (key == GLFW_KEY_F || key == GLFW_KEY_K) &&
        (mods & GLFW_MOD_CONTROL) != 0) {
        client->toggle_search();
        return;
    }
    handle_zoom_shortcut(key, mods);
}

static void mouse_button_callback(GLFWwindow* window, int button, int action, int mods)
{
    (void)mods;
    MenuClient* client = static_cast<MenuClient*>(glfwGetWindowUserPointer(window));
    if (client && action == GLFW_PRESS && client->is_listening_keybind()) {
        client->set_listened_key(1000 + button);
    }
}

void MenuClient::init_window(GLFWwindow* share_window)
{
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_COMPAT_PROFILE);
    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_FALSE);
    // Hand the window to the desktop's window manager: native title bar,
    // resize borders, snapping, tiling and maximise all come from the WM.
    glfwWindowHint(GLFW_DECORATED, GLFW_TRUE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    glfwWindowHint(GLFW_FLOATING, GLFW_FALSE);
#ifdef GLFW_SCALE_TO_MONITOR
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_FALSE);
#endif
#ifdef GLFW_SCALE_FRAMEBUFFER
    // Keep the menu framebuffer at native resolution and compensate its logical window size separately.
    glfwWindowHint(GLFW_SCALE_FRAMEBUFFER, GLFW_TRUE);
#endif

    width = menu_preferred_extent(kMenuPreferredWidth, nullptr);
    height = menu_preferred_extent(kMenuPreferredHeight, nullptr);
    BUNNYWARE_LOG_INFO("MenuClient: Creating settings window: {}x{}...", width, height);
    window = glfwCreateWindow(width, height, "Bunnyware", nullptr, share_window);
    if (!window) {
        throw std::runtime_error("MENU_CLIENT: Failed to create setting window");
    }
    BUNNYWARE_LOG_INFO("MenuClient: Settings window created successfully: {:p}", (void*)window);

    // Prevent the WM from shrinking the window below a usable layout.
    glfwSetWindowSizeLimits(window, kMenuMinWidth, kMenuMinHeight, GLFW_DONT_CARE, GLFW_DONT_CARE);

    // Position setting window near the center-left of the screen (supported on X11)
    if (glfwGetPlatform() == GLFW_PLATFORM_X11) {
        glfwSetWindowPos(window, 100, 100);
    }

    // Retain callbacks used by keybind capture UI.
    glfwSetWindowUserPointer(window, this);
    glfwSetMouseButtonCallback(window, mouse_button_callback);
    glfwSetKeyCallback(window, menu_key_callback);
}

void MenuClient::init_imgui()
{
    glfwMakeContextCurrent(window);
    glfwSwapInterval(0);  // Disable V-Sync to prevent blocking the shared overlay main loop

    IMGUI_CHECKVERSION();
    imgui_context = ImGui::CreateContext();
    ImGui::SetCurrentContext(imgui_context);
    ImGuiIO& io = ImGui::GetIO();
    (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplGlfw_SetPhysicalPixelCoordinates(true);
    glfwSetCursor(window, nullptr);
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    ImGui_ImplOpenGL3_Init("#version 130");

    apply_menu_preferences(g_menu_preferences);
    UpdateMenuScale(g_menu_preferences.menu_scale);
    BUNNYWARE_LOG_INFO("MenuClient: ImGui initialization completed.");
}

void MenuClient::UpdateMenuScale(float new_scale)
{
    applied_menu_scale = new_scale;
    ImGuiIO& io = ImGui::GetIO();
    io.FontGlobalScale = 1.0f; // Keep global scale modifier at baseline
    const bool font_texture_created = io.Fonts->TexID != 0;
    io.Fonts->Clear();
    bold_font = nullptr;

    ImFontConfig font_cfg;
    font_cfg.OversampleH = 1;
    font_cfg.OversampleV = 1;
    font_cfg.PixelSnapH = true;

    // Missing files must be filtered before ImGui's debug file-load assertion.
    const auto load_font = [&](const std::string& path) -> ImFont* {
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error)) return nullptr;
        return io.Fonts->AddFontFromFileTTF(path.c_str(), 13.0f * new_scale, &font_cfg);
    };
    ImFont* font = load_font("/usr/share/fonts/TTF/FiraSans-Medium.ttf");
    if (!font) font = load_font("/usr/share/fonts/noto/NotoSans-Medium.ttf");
    if (!font) font = load_font(get_config_path("assets/tahoma.ttf"));
    if (!font) font = load_font(get_config_path("../assets/tahoma.ttf"));
    if (!font) font = load_font("assets/tahoma.ttf");
    if (!font) font = load_font("../assets/tahoma.ttf");
    if (!font) {
        font_cfg.SizePixels = 13.0f * new_scale;
        font = io.Fonts->AddFontDefault(&font_cfg);
    }

    bold_font = load_font("/usr/share/fonts/TTF/FiraSans-Bold.ttf");
    if (!bold_font) bold_font = load_font("/usr/share/fonts/noto/NotoSans-Bold.ttf");
    if (!bold_font) bold_font = font;

    // Rebuild only the atlas texture after user scale changes.
    ImGui_ImplOpenGL3_DestroyFontsTexture();
    if (font_texture_created)
        ImGui_ImplOpenGL3_CreateFontsTexture();

    // Reset and scale system dimensions cleanly
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle(); // Reset to default metrics
    apply_menu_preferences(g_menu_preferences);
    apply_dark_theme();   // Re-apply base custom colors/paddings
    style.ScaleAllSizes(new_scale); // Let ImGui handle structural dimensions
}

void MenuClient::reset_window_size()
{
    if (!window) return;
    width = menu_preferred_extent(kMenuPreferredWidth, window);
    height = menu_preferred_extent(kMenuPreferredHeight, window);
    glfwSetWindowSize(window, width, height);
}

void MenuClient::render()
{
    glfwMakeContextCurrent(window);

    // Ensure the window is visible and mapped under Wayland/X11
    static bool window_shown = false;
    if (!window_shown) {
        BUNNYWARE_LOG_INFO("MenuClient: Mapping and showing settings window (glfwShowWindow).");
        glfwShowWindow(window);
        glfwFocusWindow(window);
        window_shown = true;
    }

    static int visible_check_count = 0;
    if (visible_check_count < 10) {
        BUNNYWARE_LOG_INFO("MenuClient: Frame {} - Visible: {}, Focused: {}, Iconified: {}", 
            visible_check_count,
            glfwGetWindowAttrib(window, GLFW_VISIBLE),
            glfwGetWindowAttrib(window, GLFW_FOCUSED),
            glfwGetWindowAttrib(window, GLFW_ICONIFIED));
        visible_check_count++;
    }

    ImGui::SetCurrentContext(imgui_context);

    // menu_scale only changes UI density (fonts + style). The window manager
    // owns the window size, so we merely *request* a matching size when the
    // user changes the scale; afterwards the WM is free to move/resize.
    const bool requested_resize = g_menu_preferences.menu_scale != applied_menu_scale;
    if (requested_resize) {
        UpdateMenuScale(g_menu_preferences.menu_scale);
        width = menu_preferred_extent(kMenuPreferredWidth, window);
        height = menu_preferred_extent(kMenuPreferredHeight, window);
        glfwSetWindowSize(window, width, height);
    }

    static MenuPreferences last_applied_preferences = g_menu_preferences;
    if (g_menu_preferences.accent_color != last_applied_preferences.accent_color ||
        g_menu_preferences.background_color != last_applied_preferences.background_color ||
        g_menu_preferences.card_color != last_applied_preferences.card_color ||
        g_menu_preferences.text_color != last_applied_preferences.text_color ||
        g_menu_preferences.outline_color != last_applied_preferences.outline_color) {
        apply_menu_preferences(g_menu_preferences);
        ImGuiStyle& style = ImGui::GetStyle();
        style = ImGuiStyle();
        apply_dark_theme();
        style.ScaleAllSizes(g_menu_preferences.menu_scale);
    }
    last_applied_preferences = g_menu_preferences;
    MenuAnimation::get().configure(g_menu_preferences.animations_enabled, g_menu_preferences.animation_speed);
    flush_menu_preferences_save();

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    MenuAnimation::get().update(ImGui::GetIO().DeltaTime);

    // Font uses default linear/bilinear filtering for clean rendering

    render_ui();
    render_zoom_toast();

    ImGui::Render();

    int display_w, display_h;
    glfwGetFramebufferSize(window, &display_w, &display_h);
    glViewport(0, 0, display_w, display_h);

    // Smooth grey-black background
    glClearColor(MenuTheme::bg_main.x, MenuTheme::bg_main.y, MenuTheme::bg_main.z, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(window);
}

namespace {
ImVec4 lerp_color(const ImVec4& from, const ImVec4& to, float amount)
{
    return ImVec4(from.x + (to.x - from.x) * amount, from.y + (to.y - from.y) * amount,
                  from.z + (to.z - from.z) * amount, from.w + (to.w - from.w) * amount);
}

float smoothstep01(float value)
{
    value = std::clamp(value, 0.0f, 1.0f);
    return value * value * (3.0f - 2.0f * value);
}

float measure_search_text_range(std::string_view text, std::size_t begin, std::size_t end)
{
    return ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), std::numeric_limits<float>::max(), 0.0f,
                                           text.data() + begin, text.data() + end)
        .x;
}

float draw_search_text_range(ImDrawList* draw_list, std::string_view text, std::size_t begin, std::size_t end,
                            float x, float y, ImU32 color)
{
    if (begin >= end) return x;
    draw_list->AddText(ImVec2(x, y), color, text.data() + begin, text.data() + end);
    return x + measure_search_text_range(text, begin, end);
}

void draw_search_result_label(ImDrawList* draw_list, std::string_view query, std::string_view label,
                             const ImVec2& position)
{
    const float scale = ImGui::GetFontSize() / 13.0f;
    const float font_size = ImGui::GetFontSize();
    const ImU32 text_color = ImGui::GetColorU32(MenuTheme::text_primary);
    const ImU32 match_color = ImGui::GetColorU32(MenuTheme::accent_hover);
    ImVec4 match_background = MenuTheme::accent_primary;
    match_background.w = 0.16f;
    const ImU32 match_background_color = ImGui::GetColorU32(match_background);

    float x = position.x;
    std::size_t query_index = 0;
    std::size_t label_index = 0;
    while (label_index < label.size()) {
        while (query_index < query.size() &&
               std::isalnum(static_cast<unsigned char>(query[query_index])) == 0) {
            ++query_index;
        }
        if (query_index == query.size()) {
            draw_search_text_range(draw_list, label, label_index, label.size(), x, position.y, text_color);
            return;
        }

        const char query_character =
            static_cast<char>(std::tolower(static_cast<unsigned char>(query[query_index])));
        std::size_t match_index = label_index;
        while (match_index < label.size()) {
            const unsigned char label_character = static_cast<unsigned char>(label[match_index]);
            if (std::isalnum(label_character) != 0 &&
                static_cast<char>(std::tolower(label_character)) == query_character) {
                break;
            }
            ++match_index;
        }
        if (match_index == label.size()) {
            draw_search_text_range(draw_list, label, label_index, label.size(), x, position.y, text_color);
            return;
        }

        x = draw_search_text_range(draw_list, label, label_index, match_index, x, position.y, text_color);
        const float character_width = measure_search_text_range(label, match_index, match_index + 1);
        draw_list->AddRectFilled(ImVec2(x - 1.5f * scale, position.y),
                                 ImVec2(x + character_width + 1.5f * scale, position.y + font_size),
                                 match_background_color, 2.0f * scale);
        x = draw_search_text_range(draw_list, label, match_index, match_index + 1, x, position.y, match_color);
        label_index = match_index + 1;
        ++query_index;
    }
}

bool render_animated_vertical_tab(const char* label, const char* id, bool selected, const SvgTexture& icon)
{
    const float scale = ImGui::GetFontSize() / 13.0f;
    const float height = 30.0f * scale;
    const float rounding = 6.0f * scale;

    const ImGuiID item_id = ImGui::GetID(id);
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(ImGui::GetContentRegionAvail().x, height));
    const bool hovered = ImGui::IsItemHovered();
    const float state_target = hovered ? 0.4f : 0.0f;
    const float state = MenuAnimation::get().animate(item_id, state_target, 26.0f);

    ImVec2 rect_min = ImGui::GetItemRectMin();
    ImVec2 rect_max = ImGui::GetItemRectMax();
    rect_min.x = std::round(rect_min.x);
    rect_min.y = std::round(rect_min.y);
    rect_max.x = std::round(rect_max.x);
    rect_max.y = std::round(rect_max.y);

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    if (!selected && state > 0.01f) {
        ImVec4 background = MenuTheme::bg_card;
        background.w *= state * 0.9f;
        draw_list->AddRectFilled(rect_min, rect_max, ImGui::GetColorU32(background), rounding);
    }

    const float content_state = selected ? 1.0f : state;
    const ImVec4 content_color = selected
                                     ? MenuTheme::bg_main
                                     : lerp_color(MenuTheme::text_secondary, MenuTheme::text_primary, content_state);
    const float content_offset = 4.0f * state * scale;
    if (icon.id != 0) {
        const float icon_size = 15.0f * scale;
        const float icon_x = std::round(rect_min.x + 18.0f * scale + content_offset);
        const float icon_y = std::round(rect_min.y + (height - icon_size) * 0.5f);
        draw_list->AddImage(reinterpret_cast<ImTextureID>(static_cast<intptr_t>(icon.id)),
                            ImVec2(icon_x, icon_y), ImVec2(icon_x + icon_size, icon_y + icon_size),
                            ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), ImGui::GetColorU32(content_color));
    }

    const ImVec2 text_size = ImGui::CalcTextSize(label);
    const float text_x = std::round(rect_min.x + (icon.id != 0 ? 42.0f : 24.0f) * scale + content_offset);
    const float text_y = std::round(rect_min.y + (height - text_size.y) * 0.5f);
    draw_list->AddText(ImVec2(text_x, text_y), ImGui::GetColorU32(content_color), label);

    return pressed;
}

void apply_smooth_scrolling(int tab_index)
{
    struct ScrollState {
        float current = 0.0f;
        float target = 0.0f;
        bool initialized = false;
    };
    static ScrollState states[8];

    ScrollState& state = states[std::clamp(tab_index, 0, 7)];
    const float scroll_max = ImGui::GetScrollMaxY();
    if (!state.initialized) {
        state.current = ImGui::GetScrollY();
        state.target = state.current;
        state.initialized = true;
    }

    state.current = std::clamp(state.current, 0.0f, scroll_max);
    state.target = std::clamp(state.target, 0.0f, scroll_max);

    // Popups receive the wheel exclusively while open. Without this guard the
    // parent settings panel also consumes the same wheel delta.
    if (ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) {
        state.current = ImGui::GetScrollY();
        state.target = state.current;
        return;
    }

    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const ImGuiID scrollbar_id = ImGui::GetWindowScrollbarID(window, ImGuiAxis_Y);
    if (ImGui::GetActiveID() == scrollbar_id) {
        state.current = ImGui::GetScrollY();
        state.target = state.current;
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && io.MouseWheel != 0.0f) {
        const float wheel_step = 55.0f * (ImGui::GetFontSize() / 13.0f);
        state.target = std::clamp(state.target - io.MouseWheel * wheel_step, 0.0f, scroll_max);
    }

    if (!MenuAnimation::get().enabled()) {
        state.current = state.target;
        ImGui::SetScrollY(std::round(state.current));
        return;
    }
    const float speed = 16.0f * MenuAnimation::get().speed_scale();
    const float amount = 1.0f - std::exp(-speed * std::min(io.DeltaTime, 0.05f));
    state.current += (state.target - state.current) * amount;
    if (std::abs(state.current - state.target) < 0.05f) state.current = state.target;
    ImGui::SetScrollY(std::round(state.current));
}
}  // namespace

void MenuClient::render_ui()
{
    OverlayConfig old_cfg = cfg;

    static int active_tab = 0;
    struct TabTransitionState {
        int displayed_tab = 0;
        int from_tab = 0;
        int to_tab = 0;
        int direction = 1;
        int active_nav_slot = 1;
        float elapsed = 1.0f;
        bool running = false;
    };
    static TabTransitionState tab_transition;
    static bool tab_has_scrollbar[8] = {};
    constexpr float tab_transition_duration = 0.200f;
    constexpr float exit_fraction = 0.34f;

    // Get current viewport to dynamically size the borderless window
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);

    // Force zero rounding, border size, and window padding for a seamless borderless layout
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

    // Page text from theme (not forced pure white)
    ImGui::PushStyleColor(ImGuiCol_Text, MenuTheme::text_primary);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, MenuTheme::bg_main);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, MenuTheme::bg_main);

    // Mask standard ImGui window decorations and manipulation flags
    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                    ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoSavedSettings;

    ImGui::Begin("Bunnyware", nullptr, window_flags);

    render_search_palette();
    if (pending_search_tab >= 0) {
        const int next_tab = pending_search_tab;
        pending_search_tab = -1;
        const int previous_slot = tab_transition.active_nav_slot;
        active_tab = next_tab;
        tab_transition.active_nav_slot = menu_tab_nav_slot(next_tab);
        if (next_tab != tab_transition.displayed_tab) {
            tab_transition.from_tab = tab_transition.displayed_tab;
            tab_transition.to_tab = next_tab;
            tab_transition.direction = tab_transition.active_nav_slot >= previous_slot ? 1 : -1;
            tab_transition.elapsed = 0.0f;
            tab_transition.running = MenuAnimation::get().enabled();
            if (!tab_transition.running) tab_transition.displayed_tab = next_tab;
        }
    }

    const float window_w = ImGui::GetWindowWidth();
    const float content_pad = ImGui::GetFontSize() * (6.0f / 13.0f);

    // Align the settings content table inside a small margin. Window chrome
    // (title bar, borders) is provided by the desktop window manager.
    ImGui::SetCursorPos(ImVec2(content_pad, content_pad));
    if (ImGui::BeginTable("VerticalTabsTable", 2, ImGuiTableFlags_None,
                          ImVec2(window_w - content_pad * 2.0f,
                                 ImGui::GetContentRegionAvail().y - content_pad))) {
        // Column 1 (Left): Fixed width navigation bar
        ImGui::TableSetupColumn("NavBar", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * (160.0f / 13.0f));
        // Column 2 (Right): Stretching content area
        ImGui::TableSetupColumn("Content", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextRow();

        // --- Column 1: Navigation ---
        ImGui::TableNextColumn();

        // Compact spacing between the custom-drawn animated tab rows.
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, ImGui::GetFontSize() * (2.0f / 13.0f)));

        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ImGui::GetFontSize() * (4.0f / 13.0f));
        ImGui::BeginChild("NavBarChild", ImVec2(0.0f, 0.0f), false, ImGuiWindowFlags_NoScrollbar);

        const float tab_scale = ImGui::GetFontSize() / 13.0f;
        const float tab_height = 30.0f * tab_scale;
        const float tab_spacing = 2.0f * tab_scale;
        const ImVec2 nav_start = ImGui::GetCursorScreenPos();
        const float nav_width = ImGui::GetContentRegionAvail().x;
        const ImGuiID highlight_id = ImGui::GetID("##vertical_tab_highlight");
        float raw_slot = MenuAnimation::get().animate(
            highlight_id, static_cast<float>(tab_transition.active_nav_slot), 24.0f);
        const float target_slot = static_cast<float>(tab_transition.active_nav_slot);
        const float slot_travel = tab_height + tab_spacing;
        if (std::abs(target_slot - raw_slot) * slot_travel < 0.5f) raw_slot = target_slot;
        const float animated_slot = raw_slot;
        const float highlight_y =
            std::round(nav_start.y + animated_slot * (tab_height + tab_spacing));
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImVec2(std::round(nav_start.x), highlight_y),
            ImVec2(std::round(nav_start.x + nav_width), std::round(highlight_y + tab_height)),
            ImGui::GetColorU32(MenuTheme::accent_primary), 0.0f);

        static constexpr const int tab_indices[] = { 2, 0, 6, 1, 3, 7, 4, 5 };
        static constexpr const char* tabs[] = {
            "Combat", "Players", "World", "Grenades", "Misc", "Radar", "Config", "Debug"
        };
        static constexpr const char* tab_icons[] = {
            "icons/ui/lucide/crosshair", "icons/ui/lucide/eye", "icons/ui/lucide/globe",
            "icons/ui/lucide/bomb", "icons/ui/lucide/shapes",
            "icons/ui/lucide/route", "icons/ui/lucide/file-cog", "icons/ui/lucide/bug"
        };
        for (int i = 0; i < 8; ++i) {
            ImGui::PushID(i);
            const SvgTexture icon = svg_cache.get_weapon_texture(tab_icons[i]);
            if (render_animated_vertical_tab(tabs[i], "##vertical_tab", active_tab == tab_indices[i], icon)) {
                const int next_tab = tab_indices[i];
                if (next_tab != active_tab) {
                    const int previous_slot = tab_transition.active_nav_slot;
                    active_tab = next_tab;
                    tab_transition.active_nav_slot = i;

                    if (MenuAnimation::get().enabled() && next_tab != tab_transition.displayed_tab) {
                        tab_transition.from_tab = tab_transition.displayed_tab;
                        tab_transition.to_tab = next_tab;
                        tab_transition.direction = i >= previous_slot ? 1 : -1;
                        tab_transition.elapsed = 0.0f;
                        tab_transition.running = true;
                    } else {
                        tab_transition.displayed_tab = next_tab;
                        tab_transition.running = false;
                    }
                }
            }
            ImGui::PopID();
        }

        const float search_y = ImGui::GetCursorPosY() +
                               std::max(0.0f, ImGui::GetContentRegionAvail().y - tab_height);
        ImGui::SetCursorPosY(search_y);
        ImGui::PushID("search_action");
        const SvgTexture search_icon = svg_cache.get_weapon_texture("icons/ui/lucide/search");
        if (render_animated_vertical_tab("Search", "##search_action", false, search_icon) &&
            !is_listening_keybind()) {
            toggle_search();
        }
        ImGui::PopID();

        ImGui::EndChild();  // NavBarChild

        ImGui::PopStyleVar();

        // --- Column 2: Content ---
        ImGui::TableNextColumn();
        ImGui::BeginChild("ContentChild", ImVec2(0.0f, 0.0f), false);

        float transition_alpha = 1.0f;
        float transition_offset = 0.0f;
        float card_phase = 1.0f;
        bool entering = true;
        if (tab_transition.running && MenuAnimation::get().enabled()) {
            tab_transition.elapsed += std::min(ImGui::GetIO().DeltaTime, 0.05f);
            const float progress =
                std::clamp(tab_transition.elapsed / tab_transition_duration, 0.0f, 1.0f);
            const float slide_distance = 12.0f * (ImGui::GetFontSize() / 13.0f);

            if (progress < exit_fraction) {
                card_phase = progress / exit_fraction;
                const float eased = smoothstep01(card_phase);
                tab_transition.displayed_tab = tab_transition.from_tab;
                transition_alpha = 1.0f - eased;
                const float offset =
                    -static_cast<float>(tab_transition.direction) * slide_distance * eased;
                transition_offset = std::abs(offset) < 0.5f ? 0.0f : std::round(offset);
                entering = false;
            } else {
                card_phase = (progress - exit_fraction) / (1.0f - exit_fraction);
                const float eased = smoothstep01(card_phase);
                tab_transition.displayed_tab = tab_transition.to_tab;
                transition_alpha = eased;
                const float offset =
                    static_cast<float>(tab_transition.direction) * slide_distance * (1.0f - eased);
                transition_offset = std::abs(offset) < 0.5f ? 0.0f : std::round(offset);
                entering = true;
            }

            if (progress >= 1.0f) {
                tab_transition.displayed_tab = tab_transition.to_tab;
                tab_transition.running = false;
                transition_alpha = 1.0f;
                transition_offset = 0.0f;
                card_phase = 1.0f;
            }
        } else {
            if (!MenuAnimation::get().enabled()) tab_transition.running = false;
            if (tab_transition.displayed_tab != active_tab)
                tab_transition.displayed_tab = active_tab;
        }

        // Fill the content column. Scrollbar space is always reserved below so
        // cards keep the same width when a tab becomes scrollable.
        const float panel_w = 0.0f;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + transition_offset);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * transition_alpha);
        ImGui::PushID(tab_transition.displayed_tab);
        const int displayed_tab_index = std::clamp(tab_transition.displayed_tab, 0, 7);
        const bool hide_unused_scrollbar = !tab_has_scrollbar[displayed_tab_index];
        if (hide_unused_scrollbar) {
            const ImVec4 transparent(0.0f, 0.0f, 0.0f, 0.0f);
            ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, transparent);
            ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, transparent);
            ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, transparent);
        }
        ImGuiWindowFlags panel_flags =
            ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_AlwaysVerticalScrollbar;
        if (tab_transition.running) panel_flags |= ImGuiWindowFlags_NoInputs;
        ImGui::BeginChild("LeftSettingsPanel", ImVec2(panel_w, 0.0f), false,
                          panel_flags);
        apply_smooth_scrolling(tab_transition.displayed_tab);
        set_tab_content_transition(card_phase, entering, tab_transition.running);

        switch (tab_transition.displayed_tab) {
            case 0:
                render_visuals_tab(cfg);
                break;
            case 1:
                render_grenades_tab(cfg);
                break;
            case 2:
                render_combat_tab(cfg, svg_cache);
                break;
            case 3:
                render_misc_tab(cfg);
                break;
            case 4:
                render_config_tab(cfg);
                break;
            case 5:
                render_debug_tab(cfg, overlay, window, game_reader);
                break;
            case 6:
                render_world_tab(cfg);
                break;
            case 7:
                render_radar_tab(cfg);
                break;
        }

        tab_has_scrollbar[displayed_tab_index] = ImGui::GetScrollMaxY() > 0.5f;
        set_tab_content_transition(1.0f, true, false);
        ImGui::EndChild();  // LeftSettingsPanel
        if (hide_unused_scrollbar) ImGui::PopStyleColor(3);
        ImGui::PopID();
        ImGui::PopStyleVar();

        ImGui::EndChild();  // ContentChild

        ImGui::EndTable();
    }

    ImGui::End();

    ImGui::PopStyleColor(3); // Text, WindowBg, ChildBg
    ImGui::PopStyleVar(3);

    // Compare config with the saved copy to mark dirty if any setting changed
    OverlayConfig temp_current = cfg;
    OverlayConfig temp_old = old_cfg;
    temp_current.clear_dirty();
    temp_old.clear_dirty();
    if (!(temp_current == temp_old)) {
        cfg.mark_dirty();
    }
}

void MenuClient::set_listened_key(int key)
{
    if (listening_keybind) {
        if (key == GLFW_KEY_ESCAPE || key == GLFW_KEY_DELETE) {
            listening_keybind->key = -1;
        } else {
            listening_keybind->key = key;
        }
        stop_listening();
    }
}

void MenuClient::toggle_search()
{
    if (search_open) {
        close_search();
        return;
    }
    search_open = true;
    search_focus_requested = true;
    search_selection = 0;
    search_results_dirty = true;
    search_query[0] = '\0';
}

bool MenuClient::is_search_section_target(const char* section_id) const
{
    return section_id && !pending_search_section.empty() && pending_search_section == section_id;
}

bool MenuClient::is_search_section_highlighted(const char* section_id) const
{
    return section_id && search_highlight_section == section_id && glfwGetTime() < search_highlight_until;
}

bool MenuClient::is_search_setting_highlighted(const char* setting_name) const
{
    return setting_name && search_highlight_setting == setting_name && glfwGetTime() < search_highlight_until;
}

void MenuClient::clear_search_section_target()
{
    pending_search_section.clear();
}

void MenuClient::close_search()
{
    search_open = false;
    search_focus_requested = false;
    search_selection = 0;
    search_query[0] = '\0';
}

void MenuClient::select_search_result(const MenuSearchEntry& entry)
{
    pending_search_tab = entry.tab_index;
    pending_search_section = entry.section_id;
    search_highlight_section = entry.section_id;
    search_highlight_setting = entry.setting_name;
    search_highlight_until = glfwGetTime() + 2.0;
    close_search();
}

void MenuClient::render_search_palette()
{
    if (!search_open) return;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float scale = ImGui::GetFontSize() / 13.0f;
    const float palette_width = std::min(560.0f * scale, viewport->WorkSize.x - 32.0f * scale);
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + 48.0f * scale),
                            ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(palette_width, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints(ImVec2(260.0f * scale, 0.0f),
                                        ImVec2(palette_width, viewport->WorkSize.y * 0.75f));

    ImVec4 palette_border = MenuTheme::border_subtle;
    palette_border.w = std::max(palette_border.w, 0.14f);
    ImVec4 selected_bg = MenuTheme::accent_primary;
    selected_bg.w = 0.12f;
    ImVec4 selected_hover_bg = MenuTheme::accent_primary;
    selected_hover_bg.w = 0.18f;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f * scale, 10.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f * scale, 7.0f * scale));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, MenuTheme::bg_card);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, MenuTheme::bg_main);
    ImGui::PushStyleColor(ImGuiCol_Border, palette_border);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, MenuTheme::bg_input);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, MenuTheme::bg_card_hover);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, MenuTheme::bg_input);
    ImGui::PushStyleColor(ImGuiCol_Header, selected_bg);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, selected_hover_bg);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, selected_hover_bg);

    bool palette_visible = true;
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize;
    if (ImGui::Begin("##menu_search_palette", &palette_visible, flags)) {
        if (bold_font) ImGui::PushFont(bold_font);
        ImGui::TextUnformatted("Search settings");
        if (bold_font) ImGui::PopFont();
        ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x -
                        ImGui::CalcTextSize("Ctrl+F / Ctrl+K").x);
        ImGui::TextDisabled("Ctrl+F / Ctrl+K");

        if (search_focus_requested) {
            ImGui::SetKeyboardFocusHere();
            search_focus_requested = false;
        }
        ImGui::SetNextItemWidth(-1.0f);
        const bool query_changed = ImGui::InputTextWithHint(
            "##menu_search_input", "Find a setting...", search_query, sizeof(search_query));
        if (query_changed) search_selection = 0;
        if (query_changed) search_results_dirty = true;

        if (search_results_dirty) {
            search_matches = find_menu_search_matches(search_query);
            search_results_dirty = false;
        }
        if (search_selection >= static_cast<int>(search_matches.size())) {
            search_selection = std::max(0, static_cast<int>(search_matches.size()) - 1);
        }

        const int visible_results = std::min(6, static_cast<int>(search_matches.size()));
        const float line_height = ImGui::GetTextLineHeight();
        const float row_height = line_height * 2.0f + 8.0f * scale;
        const float results_height = row_height * static_cast<float>(visible_results);
        if (visible_results > 0) {
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
            const bool results_visible = ImGui::BeginChild(
                "##menu_search_results", ImVec2(0.0f, results_height), false,
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            if (results_visible) {
                for (int index = 0; index < visible_results; ++index) {
                    const MenuSearchEntry& entry = *search_matches[index].entry;
                    const bool selected = index == search_selection;
                    const ImVec2 row_min = ImGui::GetCursorScreenPos();
                    const float row_width = ImGui::GetContentRegionAvail().x;
                    const bool clicked = ImGui::Selectable("##search_result", selected,
                                                          ImGuiSelectableFlags_SpanAllColumns,
                                                          ImVec2(row_width, row_height));
                    if (clicked) {
                        select_search_result(entry);
                    }
                    const ImVec2 label_pos(row_min.x + 12.0f * scale, row_min.y + 4.0f * scale);
                    const ImVec2 location_pos(row_min.x + 12.0f * scale,
                                              row_min.y + 4.0f * scale + line_height);
                    draw_search_result_label(ImGui::GetWindowDrawList(), search_query, entry.label, label_pos);
                    ImGui::GetWindowDrawList()->AddText(location_pos,
                                                        ImGui::GetColorU32(MenuTheme::text_secondary),
                                                        entry.location.c_str());
                }
            }
            ImGui::EndChild();
            ImGui::PopStyleVar();
        } else {
            ImGui::BeginChild("##menu_search_empty", ImVec2(0.0f, line_height * 2.0f), false,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            ImGui::TextDisabled("No settings match \"%s\"", search_query);
            ImGui::EndChild();
        }

        ImGui::Separator();
        const char* result_summary = search_query[0] == '\0' ? "Browse all settings" : "Matching settings";
        char result_summary_text[96];
        std::snprintf(result_summary_text, sizeof(result_summary_text), "%s | %zu found", result_summary,
                      search_matches.size());
        ImGui::TextDisabled("%s", result_summary_text);
        ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x -
                        ImGui::CalcTextSize("Up/Down navigate   Enter open   Esc close").x);
        ImGui::TextDisabled("Up/Down navigate   Enter open   Esc close");
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, false)) {
            if (!search_matches.empty()) {
                search_selection = std::min(search_selection + 1, visible_results - 1);
                search_focus_requested = true;
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, false)) {
            search_selection = std::max(search_selection - 1, 0);
            search_focus_requested = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) && !search_matches.empty()) {
            select_search_result(*search_matches[search_selection].entry);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) close_search();
    }
    ImGui::End();
    ImGui::PopStyleColor(9);
    ImGui::PopStyleVar(6);
    if (!palette_visible) close_search();
}
