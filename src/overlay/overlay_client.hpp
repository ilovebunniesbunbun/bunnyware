#pragma once
#include "renderer/gl_loader.hpp"
#define GLFW_EXPOSE_NATIVE_X11
#include <GLFW/glfw3native.h>
#include <string>
#include <vector>

// Wayland & EGL headers
#include <wayland-client.h>
#include <wayland-egl.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "tearing-control-v1-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "viewporter-client-protocol.h"

class OverlayClient {
    friend void registry_handle_global(void* data, struct wl_registry* registry, uint32_t name, const char* interface, uint32_t version);
    friend void registry_handle_global_remove(void*, struct wl_registry*, uint32_t);
    friend void layer_surface_handle_closed(void*, struct zwlr_layer_surface_v1*);
    friend void layer_surface_handle_configure(void* data, struct zwlr_layer_surface_v1* surface, uint32_t serial, uint32_t width, uint32_t height);
public:
    OverlayClient(int width, int height, int x, int y, bool hyprland_support = false, int monitor_index = 0,
                  bool force_x11 = false, const std::string& scaling = "stretched",
                  int game_width = 1920, int game_height = 1080, int game_x = 0, int game_y = 0);
    ~OverlayClient();

    bool should_close();
    void poll_events();
    void begin_frame();
    void end_frame();
    void draw_fps(int fps, float scale);
    void set_click_through(bool enabled);
    void make_current();
    void set_vsync(bool enabled);
    void update_monitor_index(int new_index);

    GLFWwindow* get_window() { return window; }
    int get_width() const { return width; }
    int get_height() const { return height; }
    int get_refresh_hz() const { return monitor_refresh_hz; }
    struct ImGuiContext* get_imgui_context() { return imgui_context; }
    bool is_wayland() const { return use_wayland; }

private:
    int width;
    int height;
    bool hyprland_support = false;
    bool use_wayland = false;
    int monitor_index = 0;
    std::string scaling_mode = "stretched";
    int game_width = 1920;
    int game_height = 1080;
    int game_x = 0;
    int game_y = 0;
    int physical_monitor_width = 0;
    int physical_monitor_height = 0;
    int monitor_refresh_hz = 0;
    int logical_width = 0;
    int logical_height = 0;
    uint32_t preferred_scale_120 = 120;
    int output_scale = 1;
    int output_transform = WL_OUTPUT_TRANSFORM_NORMAL;
    bool preferred_scale_received = false;
    uint32_t selected_output_name = 0;
    bool scaling_dirty = true;
    bool output_removed = false;
    bool wayland_closed = false;
    bool frame_active = false;
    struct WlOutputInfo {
        uint32_t name;
        uint32_t version;
    };
    std::vector<WlOutputInfo> wl_outputs;

    // X11 / GLFW Window Backend
    GLFWwindow* window = nullptr;

    // Wayland Client Backend
    struct wl_display* wl_display = nullptr;
    struct wl_registry* wl_registry = nullptr;
    struct wl_compositor* wl_compositor = nullptr;
    struct wl_subcompositor* wl_subcompositor = nullptr;
    struct wl_output* wl_output = nullptr;
    struct wl_shm* wl_shm = nullptr;
    struct zwlr_layer_shell_v1* wl_layer_shell = nullptr;
    struct wl_surface* wl_surface = nullptr;
    struct zwlr_layer_surface_v1* wl_layer_surface = nullptr;
    struct wl_egl_window* wl_egl_window = nullptr;
    struct wp_tearing_control_manager_v1* wl_tearing_control_manager = nullptr;
    struct wp_tearing_control_v1* wl_tearing_control = nullptr;

    struct wp_viewporter* wl_viewporter = nullptr;
    struct wp_viewport* wl_viewport = nullptr;
    struct wp_fractional_scale_manager_v1* wl_fractional_scale_manager = nullptr;
    struct wp_fractional_scale_v1* wl_fractional_scale = nullptr;

    // EGL State
    EGLDisplay egl_display = EGL_NO_DISPLAY;
    EGLConfig egl_config = nullptr;
    EGLContext egl_context = EGL_NO_CONTEXT;
    EGLSurface egl_surface = EGL_NO_SURFACE;
    
    bool wayland_configured = false;

    // Modern text rendering resources via ImGui context
    struct ImGuiContext* imgui_context = nullptr;

    void init_window(int x, int y, bool hyprland_support);
    void init_opengl();
    
    void init_wayland();
    void bind_wayland_output();
    void create_surface_scaling();
    void destroy_surface_scaling();
    void apply_wayland_scaling();
    void request_wayland_geometry();
    void cleanup();
};
