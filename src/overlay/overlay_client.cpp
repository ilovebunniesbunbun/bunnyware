#include "overlay_client.hpp"
#include "renderer/gl_loader.hpp"
#include <X11/Xlib.h>
#include <X11/extensions/shape.h>
#include "logger.hpp"
#include "display_scaling.hpp"
#include "config.hpp"
#include <filesystem>
#include <limits>
#include <cstring>
#include <cstdlib>
#include <array>
#include <chrono>
#include <vector>
#include <algorithm>
#include <poll.h>

#include "external/imgui/imgui.h"
#include "external/imgui/imgui_impl_glfw.h"
#include "external/imgui/imgui_impl_opengl3.h"

namespace {
ImFont* load_overlay_pixel_font(ImFontAtlas* atlas, const ImFontConfig& font_config) {
    const std::array<std::string, 4> paths = {
        get_config_path("assets/smallest_pixel-7.ttf"),
        get_config_path("../assets/smallest_pixel-7.ttf"),
        "assets/smallest_pixel-7.ttf",
        "../assets/smallest_pixel-7.ttf"
    };
    for (const auto& path : paths) {
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error)) continue;
        if (ImFont* font = atlas->AddFontFromFileTTF(path.c_str(), 10.0f, &font_config)) return font;
    }
    return nullptr;
}
} // namespace

// --- Wayland Client Callbacks ---
void registry_handle_global(void* data, struct wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
    OverlayClient* client = static_cast<OverlayClient*>(data);
    if (std::strcmp(interface, "wl_compositor") == 0) {
        client->wl_compositor = static_cast<struct wl_compositor*>(wl_registry_bind(registry, name, &wl_compositor_interface, std::min(version, 4u)));
    } else if (std::strcmp(interface, "wl_subcompositor") == 0) {
        client->wl_subcompositor = static_cast<struct wl_subcompositor*>(wl_registry_bind(registry, name, &wl_subcompositor_interface, 1));
    } else if (std::strcmp(interface, "wl_shm") == 0) {
        client->wl_shm = static_cast<struct wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    } else if (std::strcmp(interface, "zwlr_layer_shell_v1") == 0) {
        client->wl_layer_shell = static_cast<struct zwlr_layer_shell_v1*>(wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, std::min(version, 4u)));
    } else if (std::strcmp(interface, "wp_tearing_control_manager_v1") == 0) {
        client->wl_tearing_control_manager = static_cast<struct wp_tearing_control_manager_v1*>(wl_registry_bind(registry, name, &wp_tearing_control_manager_v1_interface, 1));
    } else if (std::strcmp(interface, "wp_viewporter") == 0) {
        client->wl_viewporter = static_cast<struct wp_viewporter*>(wl_registry_bind(registry, name, &wp_viewporter_interface, 1));
    } else if (std::strcmp(interface, "wp_fractional_scale_manager_v1") == 0) {
        client->wl_fractional_scale_manager = static_cast<struct wp_fractional_scale_manager_v1*>(wl_registry_bind(registry, name, &wp_fractional_scale_manager_v1_interface, 1));
    } else if (std::strcmp(interface, "wl_output") == 0) {
        client->wl_outputs.push_back({name, version});
    }
}

void registry_handle_global_remove(void* data, struct wl_registry*, uint32_t name) {
    auto* client = static_cast<OverlayClient*>(data);
    std::erase_if(client->wl_outputs, [name](const auto& output) { return output.name == name; });
    if (client->selected_output_name == name) client->output_removed = true;
}

static const struct wl_registry_listener registry_listener = {
    registry_handle_global,
    registry_handle_global_remove
};

void layer_surface_handle_configure(void* data, struct zwlr_layer_surface_v1* surface, uint32_t serial, uint32_t width, uint32_t height) {
    OverlayClient* client = static_cast<OverlayClient*>(data);
    zwlr_layer_surface_v1_ack_configure(surface, serial);
    
    // Callbacks only update pending state: another GL context may be current.
    if (width > 0 && width <= static_cast<uint32_t>(std::numeric_limits<int>::max())) {
        client->logical_width = static_cast<int>(width);
    }
    if (height > 0 && height <= static_cast<uint32_t>(std::numeric_limits<int>::max())) {
        client->logical_height = static_cast<int>(height);
    }
    client->scaling_dirty = true;
    client->wayland_configured = true;
}

void layer_surface_handle_closed(void* data, struct zwlr_layer_surface_v1*) {
    static_cast<OverlayClient*>(data)->wayland_closed = true;
    BUNNYWARE_LOG_INFO("Wayland layer-surface closed by compositor.");
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    layer_surface_handle_configure,
    layer_surface_handle_closed
};

// --- OverlayClient Implementation ---

OverlayClient::OverlayClient(int w, int h, int x, int y, bool hyprland_support, int monitor_index, bool force_x11, const std::string& scaling, int game_width, int game_height, int game_x, int game_y)
    : width(w), height(h), hyprland_support(hyprland_support), monitor_index(monitor_index), scaling_mode(scaling), game_width(game_width), game_height(game_height), game_x(game_x), game_y(game_y),
      physical_monitor_width(w), physical_monitor_height(h), logical_width(w), logical_height(h) {
    
    // Detect if running under a Wayland session unless --x11 forces the X11 backend
    const char* xdg_session = std::getenv("XDG_SESSION_TYPE");
    const char* wayland_disp = std::getenv("WAYLAND_DISPLAY");
    if (!force_x11 && ((xdg_session && std::strcmp(xdg_session, "wayland") == 0) || wayland_disp)) {
        use_wayland = true;
    }

    if (force_x11) {
        BUNNYWARE_LOG_INFO("Launch option --x11: forcing the X11/GLFW overlay backend.");
        if (!std::getenv("DISPLAY")) {
            BUNNYWARE_LOG_WARN("--x11 requested but DISPLAY is not set; X11/XWayland may not be reachable.");
        }
    }

    try {
        if (use_wayland) {
            BUNNYWARE_LOG_INFO("Wayland session detected. Initializing Wayland Layer-Shell overlay.");
            init_wayland();
        } else {
            BUNNYWARE_LOG_INFO("X11 session detected. Initializing X11/GLFW overlay.");
            init_window(x, y, hyprland_support);
            init_opengl();
            set_click_through(true);
        }
    } catch (...) {
        cleanup();
        throw;
    }
}

OverlayClient::~OverlayClient() {
    cleanup();
}

bool OverlayClient::should_close() {
    if (use_wayland) {
        if (wayland_closed) return true;
        if (wl_display) {
            int err = wl_display_get_error(wl_display);
            if (err != 0) {
                static bool error_logged = false;
                if (!error_logged) {
                    const struct wl_interface* interface = nullptr;
                    uint32_t id = 0;
                    uint32_t protocol_err = wl_display_get_protocol_error(wl_display, &interface, &id);
                    if (protocol_err != 0) {
                        BUNNYWARE_LOG_ERROR("OverlayClient: Wayland protocol error: code={}, interface={}, id={}",
                            protocol_err, interface ? interface->name : "unknown", id);
                    } else {
                        BUNNYWARE_LOG_ERROR("OverlayClient: Wayland socket/system error: {}", err);
                    }
                    error_logged = true;
                }
                return true;
            }
        }
        return false;
    }
    return glfwWindowShouldClose(window);
}

void OverlayClient::poll_events() {
    if (use_wayland) {
        if (wl_display) {
            // 1. Dispatch any events already in the queue
            wl_display_dispatch_pending(wl_display);

            // 2. Prepare to read from the display socket.
            // If wl_display_prepare_read returns -1, it means there are events in the queue
            // that need to be dispatched first. We loop until we successfully prepare or there's an error.
            int retries = 0;
            while (wl_display_prepare_read(wl_display) != 0 && retries < 10) {
                if (wl_display_get_error(wl_display) != 0) {
                    return;
                }
                wl_display_dispatch_pending(wl_display);
                retries++;
            }

            if (retries < 10) {
                // 3. Flush outgoing requests
                wl_display_flush(wl_display);

                // 4. Poll the file descriptor with a timeout of 0 (non-blocking)
                struct pollfd fds;
                fds.fd = wl_display_get_fd(wl_display);
                fds.events = POLLIN;
                fds.revents = 0;

                int ret = poll(&fds, 1, 0); // 0 means return immediately
                if (ret > 0) {
                    if (fds.revents & (POLLIN | POLLERR | POLLHUP)) {
                        // Read events from the socket into the queue
                        if (wl_display_read_events(wl_display) == -1) {
                            return;
                        }
                        // Dispatch the newly read events
                        wl_display_dispatch_pending(wl_display);
                    } else {
                        wl_display_cancel_read(wl_display);
                    }
                } else {
                    // If poll returned 0 (timeout) or -1 (error), cancel the read intent
                    wl_display_cancel_read(wl_display);
                }
            }
        }
        if (output_removed) {
            output_removed = false;
            wayland_closed = false;
            // Recreate after dispatch, never destroy listener-owned objects in callbacks.
            monitor_index = -1;
            update_monitor_index(0);
        }
        apply_wayland_scaling();
        glfwPollEvents();
    } else {
        glfwPollEvents();
        glfwGetFramebufferSize(window, &width, &height);
    }
}

void OverlayClient::set_click_through(bool enabled) {
    if (use_wayland) return; // Click-through configured permanently at setup on Wayland
    
    Display* display = glfwGetX11Display();
    if (!display) {
        BUNNYWARE_LOG_ERROR("Failed to get X11 display for click-through.");
        return;
    }
    Window x_win = glfwGetX11Window(window);
    if (!x_win) {
        BUNNYWARE_LOG_ERROR("Failed to get X11 window.");
        return;
    }
    if (enabled) {
        XShapeCombineRectangles(display, x_win, ShapeInput, 0, 0, nullptr, 0, ShapeSet, YXBanded);
    } else {
        XShapeCombineMask(display, x_win, ShapeInput, 0, 0, None, ShapeSet);
    }
    XFlush(display);
}

void OverlayClient::make_current() {
    if (use_wayland) {
        if (wl_display && wl_display_get_error(wl_display) != 0) {
            return;
        }
        if (egl_display != EGL_NO_DISPLAY && egl_surface != EGL_NO_SURFACE && egl_context != EGL_NO_CONTEXT) {
            if (!eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context)) wayland_closed = true;
        }
    } else {
        if (window) {
            glfwMakeContextCurrent(window);
        }
    }
}

void OverlayClient::set_vsync(bool enabled) {
    if (use_wayland) {
        if (wl_display && wl_display_get_error(wl_display) != 0) {
            return;
        }
        if (egl_display != EGL_NO_DISPLAY) {
            eglSwapInterval(egl_display, enabled ? 1 : 0);
        }
    } else {
        glfwSwapInterval(enabled ? 1 : 0);
    }
}

void OverlayClient::init_window(int x, int y, bool hyprland_support) {
    glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);

    if (!glfwInit()) {
        throw std::runtime_error("OVERLAY_CLIENT: Failed to initialize GLFW");
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_COMPAT_PROFILE);
    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_TRUE);
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
    glfwWindowHint(GLFW_FLOATING, GLFW_TRUE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    glfwWindowHint(GLFW_STENCIL_BITS, 8);
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_FALSE);
#ifdef GLFW_SCALE_FRAMEBUFFER
    glfwWindowHint(GLFW_SCALE_FRAMEBUFFER, GLFW_FALSE);
#endif

    window = glfwCreateWindow(width, height, "bunnyware.overlay", nullptr, nullptr);
    if (!window) {
        throw std::runtime_error("OVERLAY_CLIENT: Failed to create overlay window");
    }

    glfwSetWindowPos(window, x, y);

    Display* display = glfwGetX11Display();
    Window x_win = glfwGetX11Window(window);
    if (hyprland_support) {
        BUNNYWARE_LOG_INFO("Hyprland compatibility mode enabled; skipping override_redirect "
                     "(relying on input region + windowrules for click-through).");
    } else if (display && x_win) {
        glfwHideWindow(window);
        XSetWindowAttributes attrs;
        attrs.override_redirect = True;
        XChangeWindowAttributes(display, x_win, CWOverrideRedirect, &attrs);
        glfwShowWindow(window);
        BUNNYWARE_LOG_INFO("X11 override_redirect applied successfully.");
    } else {
        BUNNYWARE_LOG_ERROR("Failed to apply override_redirect.");
    }
}

void OverlayClient::init_opengl() {
    glfwMakeContextCurrent(window);
    glfwSwapInterval(0);

    glfwGetFramebufferSize(window, &width, &height);
    glViewport(0, 0, width, height);
    
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    IMGUI_CHECKVERSION();
    imgui_context = ImGui::CreateContext();
    ImGui::SetCurrentContext(imgui_context);

    ImGuiIO& io = ImGui::GetIO();
    ImFontConfig font_cfg;
    font_cfg.OversampleH = 1;
    font_cfg.OversampleV = 1;
    font_cfg.PixelSnapH = true;

    ImFont* font = load_overlay_pixel_font(io.Fonts, font_cfg);
    if (!font) {
        BUNNYWARE_LOG_ERROR("OVERLAY_CLIENT: Failed to load smallest_pixel-7.ttf font, falling back to default.");
        io.Fonts->AddFontDefault(&font_cfg);
    }

    ImGui_ImplGlfw_InitForOpenGL(window, false);
    ImGui_ImplGlfw_SetPhysicalPixelCoordinates(true);
    ImGui_ImplOpenGL3_Init("#version 130");
}

void OverlayClient::init_wayland() {
    wl_display = wl_display_connect(nullptr);
    if (!wl_display) {
        throw std::runtime_error("OVERLAY_CLIENT: Failed to connect to Wayland display");
    }

    wl_registry = wl_display_get_registry(wl_display);
    wl_registry_add_listener(wl_registry, &registry_listener, this);
    wl_display_roundtrip(wl_display);

    if (!wl_compositor || !wl_layer_shell) {
        throw std::runtime_error("OVERLAY_CLIENT: Wayland compositor or layer-shell protocol not supported");
    }

    bind_wayland_output();

    wl_surface = wl_compositor_create_surface(wl_compositor);
    if (!wl_surface) {
        throw std::runtime_error("OVERLAY_CLIENT: Failed to create Wayland surface");
    }

    create_surface_scaling();

    wl_layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        wl_layer_shell, wl_surface, wl_output, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "bunnyware.overlay"
    );
    if (!wl_layer_surface) {
        throw std::runtime_error("OVERLAY_CLIENT: Failed to create layer surface");
    }

    zwlr_layer_surface_v1_add_listener(wl_layer_surface, &layer_surface_listener, this);

    uint32_t anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                      ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
    zwlr_layer_surface_v1_set_anchor(wl_layer_surface, anchor);
    request_wayland_geometry();
    zwlr_layer_surface_v1_set_exclusive_zone(wl_layer_surface, -1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(wl_layer_surface, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);

    // Empty input region = click-through
    struct wl_region* empty_region = wl_compositor_create_region(wl_compositor);
    wl_surface_set_input_region(wl_surface, empty_region);
    wl_region_destroy(empty_region);

    // Request tearing support if manager is present
    if (wl_tearing_control_manager) {
        wl_tearing_control = wp_tearing_control_manager_v1_get_tearing_control(wl_tearing_control_manager, wl_surface);
        if (wl_tearing_control) {
            wp_tearing_control_v1_set_presentation_hint(wl_tearing_control, WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ASYNC);
            BUNNYWARE_LOG_INFO("Wayland tearing control enabled (async presentation hint set).");
        }
    } else {
        BUNNYWARE_LOG_WARN("Wayland tearing control manager NOT supported by compositor.");
    }

    wl_surface_commit(wl_surface);
    while (!wayland_configured) {
        if (wayland_closed || wl_display_dispatch(wl_display) < 0) {
            throw std::runtime_error("OVERLAY_CLIENT: Wayland configure failed");
        }
    }

    egl_display = eglGetDisplay((EGLNativeDisplayType)wl_display);
    if (egl_display == EGL_NO_DISPLAY) {
        throw std::runtime_error("OVERLAY_CLIENT: Failed to get EGL display");
    }

    EGLint major, minor;
    if (!eglInitialize(egl_display, &major, &minor)) {
        throw std::runtime_error("OVERLAY_CLIENT: Failed to initialize EGL");
    }

    EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24,
        EGL_STENCIL_SIZE, 8,
        EGL_NONE
    };

    EGLint num_configs;
    if (!eglChooseConfig(egl_display, config_attribs, &egl_config, 1, &num_configs) || num_configs == 0) {
        throw std::runtime_error("OVERLAY_CLIENT: Failed to choose EGL config");
    }

    if (!eglBindAPI(EGL_OPENGL_API)) {
        throw std::runtime_error("OVERLAY_CLIENT: Failed to bind OpenGL API to EGL");
    }

    EGLint context_attribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT_KHR,
        EGL_NONE
    };

    egl_context = eglCreateContext(egl_display, egl_config, EGL_NO_CONTEXT, context_attribs);
    if (egl_context == EGL_NO_CONTEXT) {
        throw std::runtime_error("OVERLAY_CLIENT: Failed to create EGL context");
    }

    apply_wayland_scaling();
    if (wayland_closed) throw std::runtime_error("OVERLAY_CLIENT: Invalid Wayland rendering dimensions");
    wl_egl_window = wl_egl_window_create(wl_surface, width, height);
    if (!wl_egl_window) {
        throw std::runtime_error("OVERLAY_CLIENT: Failed to create wl_egl_window");
    }

    egl_surface = eglCreateWindowSurface(egl_display, egl_config, (EGLNativeWindowType)wl_egl_window, nullptr);
    if (egl_surface == EGL_NO_SURFACE) {
        throw std::runtime_error("OVERLAY_CLIENT: Failed to create EGL window surface");
    }

    make_current();
    if (wayland_closed) throw std::runtime_error("OVERLAY_CLIENT: Failed to make EGL context current");

    glewExperimental = GL_TRUE;
    // glewInit() returns an error code on Wayland EGL because it tries to locate
    // a GLX display which is not available, but it still successfully populates the GL entry points.
    glewInit();

    glViewport(0, 0, width, height);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    IMGUI_CHECKVERSION();
    imgui_context = ImGui::CreateContext();
    ImGui::SetCurrentContext(imgui_context);

    ImGuiIO& io = ImGui::GetIO();
    ImFontConfig font_cfg;
    font_cfg.OversampleH = 1;
    font_cfg.OversampleV = 1;
    font_cfg.PixelSnapH = true;

    ImFont* font = load_overlay_pixel_font(io.Fonts, font_cfg);
    if (!font) {
        BUNNYWARE_LOG_ERROR("OVERLAY_CLIENT: Wayland failed to load smallest_pixel-7 font");
        io.Fonts->AddFontDefault(&font_cfg);
    }

    ImGui_ImplOpenGL3_Init("#version 130");
}

void OverlayClient::begin_frame() {
    frame_active = false;
    if (use_wayland && wl_display && wl_display_get_error(wl_display) != 0) {
        return;
    }
    make_current();
    if (use_wayland) {
        if (wayland_closed || !wayland_configured || egl_surface == EGL_NO_SURFACE) return;
        apply_wayland_scaling();
    } else {
        glfwGetFramebufferSize(window, &width, &height);
        glViewport(0, 0, width, height);
    }

    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    ImGui::SetCurrentContext(imgui_context);

    if (use_wayland) {
        ImGui_ImplOpenGL3_NewFrame();
        
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2((float)width, (float)height);
        io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
        glViewport(0, 0, width, height);
        
        static auto last_time = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        io.DeltaTime = std::chrono::duration<float>(now - last_time).count();
        if (io.DeltaTime <= 0.0f) io.DeltaTime = 1.0f / 60.0f;
        last_time = now;

        ImGui::NewFrame();
    } else {
        static auto last_raise = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_raise).count() >= 100) {
            Display* display = glfwGetX11Display();
            Window x_win = glfwGetX11Window(window);
            if (display && x_win) {
                XRaiseWindow(display, x_win);
                XFlush(display);
            }
            last_raise = now;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));
        io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
        ImGui::NewFrame();
    }

    frame_active = true;
    static GLuint last_overlay_tex_id = 0;
    GLuint overlay_tex_id = (GLuint)(intptr_t)ImGui::GetIO().Fonts->TexID;
    if (overlay_tex_id && overlay_tex_id != last_overlay_tex_id) {
        glBindTexture(GL_TEXTURE_2D, overlay_tex_id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindTexture(GL_TEXTURE_2D, 0);
        last_overlay_tex_id = overlay_tex_id;
    }
}

void OverlayClient::draw_fps(int fps, float scale) {
    ImGui::SetCurrentContext(imgui_context);
    ImDrawList* draw_list = ImGui::GetBackgroundDrawList();
    std::string fps_text = "FPS: " + std::to_string(fps);

    const float clamped_scale = std::clamp(scale, 0.75f, 2.0f);
    ImFont* font = ImGui::GetFont();
    const float font_size = ImGui::GetFontSize() * clamped_scale;
    const float text_width = font->CalcTextSizeA(font_size, 9999.0f, 0.0f, fps_text.c_str()).x;
    const float x_pos = width - text_width - 20.0f * clamped_scale;
    const float y_pos = 20.0f * clamped_scale;

    draw_list->AddText(font, font_size, ImVec2(std::round(x_pos + clamped_scale), std::round(y_pos + clamped_scale)),
                       IM_COL32(0, 0, 0, 255), fps_text.c_str());
    draw_list->AddText(font, font_size, ImVec2(std::round(x_pos), std::round(y_pos)), IM_COL32(0, 255, 0, 255),
                       fps_text.c_str());
}

void OverlayClient::end_frame() {
    if (!frame_active) return;
    frame_active = false;
    if (use_wayland && wl_display && wl_display_get_error(wl_display) != 0) {
        return;
    }
    ImGui::SetCurrentContext(imgui_context);
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    if (use_wayland) {
        if (eglSwapBuffers(egl_display, egl_surface) != EGL_TRUE) {
            EGLint err = eglGetError();
            BUNNYWARE_LOG_ERROR("OverlayClient: eglSwapBuffers failed with error: {:#x}", err);
        }
        wl_display_flush(wl_display);
    } else {
        glfwSwapBuffers(window);
    }
}

void OverlayClient::cleanup() {
    make_current();
    if (use_wayland) {
        if (imgui_context) {
            ImGui::SetCurrentContext(imgui_context);
            ImGui_ImplOpenGL3_Shutdown();
            ImGui::DestroyContext(imgui_context);
            imgui_context = nullptr;
        }
        if (egl_display != EGL_NO_DISPLAY) {
            eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (egl_surface != EGL_NO_SURFACE) {
                eglDestroySurface(egl_display, egl_surface);
                egl_surface = EGL_NO_SURFACE;
            }
            if (egl_context != EGL_NO_CONTEXT) {
                eglDestroyContext(egl_display, egl_context);
                egl_context = EGL_NO_CONTEXT;
            }
            eglTerminate(egl_display);
            egl_display = EGL_NO_DISPLAY;
        }
        if (wl_tearing_control) {
            wp_tearing_control_v1_destroy(wl_tearing_control);
            wl_tearing_control = nullptr;
        }
        if (wl_tearing_control_manager) {
            wp_tearing_control_manager_v1_destroy(wl_tearing_control_manager);
            wl_tearing_control_manager = nullptr;
        }
        if (wl_egl_window) {
            wl_egl_window_destroy(wl_egl_window);
            wl_egl_window = nullptr;
        }
        if (wl_layer_surface) {
            zwlr_layer_surface_v1_destroy(wl_layer_surface);
            wl_layer_surface = nullptr;
        }
        destroy_surface_scaling();
        if (wl_surface) {
            wl_surface_destroy(wl_surface);
            wl_surface = nullptr;
        }
        if (wl_output) {
            wl_output_destroy(wl_output);
            wl_output = nullptr;
        }
        if (wl_shm) {
            wl_shm_destroy(wl_shm);
            wl_shm = nullptr;
        }
        if (wl_subcompositor) {
            wl_subcompositor_destroy(wl_subcompositor);
            wl_subcompositor = nullptr;
        }
        if (wl_compositor) {
            wl_compositor_destroy(wl_compositor);
            wl_compositor = nullptr;
        }
        if (wl_layer_shell) {
            zwlr_layer_shell_v1_destroy(wl_layer_shell);
            wl_layer_shell = nullptr;
        }
        if (wl_fractional_scale_manager) {
            wp_fractional_scale_manager_v1_destroy(wl_fractional_scale_manager);
            wl_fractional_scale_manager = nullptr;
        }
        if (wl_viewporter) {
            wp_viewporter_destroy(wl_viewporter);
            wl_viewporter = nullptr;
        }
        if (wl_registry) {
            wl_registry_destroy(wl_registry);
            wl_registry = nullptr;
        }
        if (wl_display) {
            wl_display_disconnect(wl_display);
            wl_display = nullptr;
        }
    } else {
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
        glfwTerminate();
    }
}

void OverlayClient::update_monitor_index(int new_index)
{
    if (monitor_index == new_index) return;
    BUNNYWARE_LOG_INFO("OverlayClient: switching monitor index from {} to {}", monitor_index, new_index);
    monitor_index = new_index;

    if (use_wayland) {
        // Tear down tearing control (tied to the old wl_surface)
        if (wl_tearing_control) {
            wp_tearing_control_v1_destroy(wl_tearing_control);
            wl_tearing_control = nullptr;
        }

        // Tear down EGL surface (tied to wl_egl_window)
        if (egl_surface != EGL_NO_SURFACE) {
            eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            eglDestroySurface(egl_display, egl_surface);
            egl_surface = EGL_NO_SURFACE;
        }

        // Tear down wl_egl_window (tied to wl_surface)
        if (wl_egl_window) {
            wl_egl_window_destroy(wl_egl_window);
            wl_egl_window = nullptr;
        }

        // Tear down layer surface (holds role on wl_surface)
        if (wl_layer_surface) {
            zwlr_layer_surface_v1_destroy(wl_layer_surface);
            wl_layer_surface = nullptr;
        }

        // Tear down wl_surface (role must be fully removed before reassignment)
        destroy_surface_scaling();
        if (wl_surface) {
            wl_surface_destroy(wl_surface);
            wl_surface = nullptr;
        }

        // Tear down old wl_output
        if (wl_output) {
            wl_output_destroy(wl_output);
            wl_output = nullptr;
        }

        bind_wayland_output();

        // Recreate wl_surface fresh (no role)
        wl_surface = wl_compositor_create_surface(wl_compositor);
        if (!wl_surface) {
            BUNNYWARE_LOG_ERROR("OverlayClient: Failed to recreate wl_surface on monitor switch");
            wayland_closed = true;
            return;
        }

        create_surface_scaling();

        // Recreate layer surface on new output
        wl_layer_surface = zwlr_layer_shell_v1_get_layer_surface(
            wl_layer_shell, wl_surface, wl_output, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "bunnyware.overlay"
        );
        if (!wl_layer_surface) {
            BUNNYWARE_LOG_ERROR("OverlayClient: Failed to recreate layer surface on monitor switch");
            wayland_closed = true;
            return;
        }

        zwlr_layer_surface_v1_add_listener(wl_layer_surface, &layer_surface_listener, this);

        uint32_t anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                          ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
        zwlr_layer_surface_v1_set_anchor(wl_layer_surface, anchor);
        request_wayland_geometry();
        zwlr_layer_surface_v1_set_exclusive_zone(wl_layer_surface, -1);
        zwlr_layer_surface_v1_set_keyboard_interactivity(wl_layer_surface, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);

        // Click-through input region
        struct wl_region* empty_region = wl_compositor_create_region(wl_compositor);
        wl_surface_set_input_region(wl_surface, empty_region);
        wl_region_destroy(empty_region);

        // Re-attach tearing control to new wl_surface
        if (wl_tearing_control_manager) {
            wl_tearing_control = wp_tearing_control_manager_v1_get_tearing_control(wl_tearing_control_manager, wl_surface);
            if (wl_tearing_control) {
                wp_tearing_control_v1_set_presentation_hint(wl_tearing_control, WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ASYNC);
            }
        }

        // Commit and wait for configure
        wayland_configured = false;
        wl_surface_commit(wl_surface);
        // Never attach a buffer before the initial configure is acknowledged.
        while (!wayland_configured) {
            if (wayland_closed || wl_display_dispatch(wl_display) < 0) {
                wayland_closed = true;
                return;
            }
        }
        apply_wayland_scaling();

        // Recreate wl_egl_window and EGL surface on the new wl_surface
        wl_egl_window = wl_egl_window_create(wl_surface, width, height);
        if (!wl_egl_window) {
            BUNNYWARE_LOG_ERROR("OverlayClient: Failed to recreate wl_egl_window on monitor switch");
            wayland_closed = true;
            return;
        }

        egl_surface = eglCreateWindowSurface(egl_display, egl_config, (EGLNativeWindowType)wl_egl_window, nullptr);
        if (egl_surface == EGL_NO_SURFACE) {
            BUNNYWARE_LOG_ERROR("OverlayClient: Failed to recreate EGL surface on monitor switch");
            wayland_closed = true;
            return;
        }

        // Make the new EGL surface current
        make_current();
        glViewport(0, 0, width, height);
        wl_display_flush(wl_display);
    } else {
        // X11 / GLFW repositioning
        if (window) {
            int monitor_count = 0;
            GLFWmonitor** monitors = glfwGetMonitors(&monitor_count);
            if (monitor_index >= 0 && monitor_index < monitor_count) {
                GLFWmonitor* monitor = monitors[monitor_index];
                int offset_x = 0, offset_y = 0;
                glfwGetMonitorPos(monitor, &offset_x, &offset_y);
                const GLFWvidmode* mode = glfwGetVideoMode(monitor);
                if (mode) {
                    const auto geometry = display_scaling::overlay_geometry(mode->width, mode->height, scaling_mode,
                                                                            game_width, game_height, game_x, game_y);
                    int requested_height = geometry.height;
                    if (geometry.width == mode->width && geometry.height == mode->height) --requested_height;
                    glfwSetWindowPos(window, offset_x + geometry.x, offset_y + geometry.y);
                    glfwSetWindowSize(window, geometry.width, requested_height);
                    glfwGetFramebufferSize(window, &width, &height);
                }
            }
        }
    }
}


void OverlayClient::bind_wayland_output() {
    output_scale = 1;
    output_transform = WL_OUTPUT_TRANSFORM_NORMAL;
    selected_output_name = 0;
    if (wl_outputs.empty()) return;
    const int target = monitor_index >= 0 && monitor_index < static_cast<int>(wl_outputs.size()) ? monitor_index : 0;
    const auto output = wl_outputs[target];
    selected_output_name = output.name;
    wl_output = static_cast<struct wl_output*>(wl_registry_bind(wl_registry, output.name, &wl_output_interface, std::min(output.version, 4u)));
    static const wl_output_listener listener = {
        [](void* data, struct wl_output*, int32_t, int32_t, int32_t, int32_t, int32_t, const char*, const char*, int32_t transform) {
            auto* client = static_cast<OverlayClient*>(data);
            if ((client->output_transform % 2) != (transform % 2)) {
                std::swap(client->physical_monitor_width, client->physical_monitor_height);
            }
            client->output_transform = transform;
            client->request_wayland_geometry();
        },
        [](void* data, struct wl_output*, uint32_t flags, int32_t width, int32_t height, int32_t refresh_mhz) {
            if (flags & WL_OUTPUT_MODE_CURRENT) {
                auto* client = static_cast<OverlayClient*>(data);
                if (refresh_mhz > 0) {
                    client->monitor_refresh_hz = (refresh_mhz + 500) / 1000;
                }
                if (width > 0 && height > 0) {
                    client->physical_monitor_width = client->output_transform % 2 ? height : width;
                    client->physical_monitor_height = client->output_transform % 2 ? width : height;
                    client->request_wayland_geometry();
                }
            }
        },
        [](void*, struct wl_output*) {},
        [](void* data, struct wl_output*, int32_t scale) {
            auto* client = static_cast<OverlayClient*>(data);
            if (scale > 0) {
                client->output_scale = scale;
                if (!client->preferred_scale_received) client->preferred_scale_120 = static_cast<uint32_t>(scale) * 120u;
                client->scaling_dirty = true;
                client->request_wayland_geometry();
            }
        },
        [](void*, struct wl_output*, const char*) {},
        [](void*, struct wl_output*, const char*) {}
    };
    if (!wl_output || wl_output_add_listener(wl_output, &listener, this) < 0 || wl_display_roundtrip(wl_display) < 0) {
        throw std::runtime_error("OVERLAY_CLIENT: Failed to bind Wayland output");
    }
}

void OverlayClient::create_surface_scaling() {
    preferred_scale_received = false;
    preferred_scale_120 = static_cast<uint32_t>(output_scale) * 120u;
    scaling_dirty = true;
    if (wl_viewporter && wl_fractional_scale_manager) {
        wl_viewport = wp_viewporter_get_viewport(wl_viewporter, wl_surface);
        wl_fractional_scale = wp_fractional_scale_manager_v1_get_fractional_scale(wl_fractional_scale_manager, wl_surface);
        if (!wl_viewport || !wl_fractional_scale) {
            throw std::runtime_error("OVERLAY_CLIENT: Failed to create Wayland scaling objects");
        }
        static const wp_fractional_scale_v1_listener listener = {
            [](void* data, wp_fractional_scale_v1*, uint32_t scale) {
                auto* client = static_cast<OverlayClient*>(data);
                if (scale > 0) {
                    client->preferred_scale_received = true;
                    client->preferred_scale_120 = scale;
                    client->scaling_dirty = true;
                    client->request_wayland_geometry();
                }
            }
        };
        wp_fractional_scale_v1_add_listener(wl_fractional_scale, &listener, this);
    } else {
        BUNNYWARE_LOG_WARN("Wayland fractional-scale/viewporter unavailable: using integer output scale; compositor fractional resampling may remain.");
    }
}

void OverlayClient::destroy_surface_scaling() {
    if (wl_fractional_scale) {
        wp_fractional_scale_v1_destroy(wl_fractional_scale);
        wl_fractional_scale = nullptr;
    }
    if (wl_viewport) {
        wp_viewport_destroy(wl_viewport);
        wl_viewport = nullptr;
    }
}

void OverlayClient::apply_wayland_scaling() {
    if (!scaling_dirty || !wayland_configured) return;
    if (!wl_fractional_scale && wl_proxy_get_version(reinterpret_cast<wl_proxy*>(wl_surface)) < WL_SURFACE_SET_BUFFER_SCALE_SINCE_VERSION) output_scale = 1;
    const uint32_t scale = wl_fractional_scale ? preferred_scale_120 : static_cast<uint32_t>(output_scale) * 120u;
    const auto extent = display_scaling::wayland_extent(logical_width, logical_height, scale);
    if (!extent.valid()) {
        wayland_closed = true;
        return;
    }
    // Render 1:1 into the integer buffer. The viewport crops the fractional
    // right/bottom padding rather than stretching all ceil(buffer) pixels.
    // Integer game projection may touch that cropped edge by less than 1px.
    width = extent.buffer_width;
    height = extent.buffer_height;
    if (wl_egl_window) wl_egl_window_resize(wl_egl_window, width, height, 0, 0);
    if (wl_viewport) {
        // Older wl_surface versions already use the required default scale1.
        if (wl_proxy_get_version(reinterpret_cast<wl_proxy*>(wl_surface)) >= WL_SURFACE_SET_BUFFER_SCALE_SINCE_VERSION) {
            wl_surface_set_buffer_scale(wl_surface, 1);
        }
        wp_viewport_set_source(wl_viewport, 0, 0, extent.source_width_fixed, extent.source_height_fixed);
        wp_viewport_set_destination(wl_viewport, logical_width, logical_height);
    } else if (wl_proxy_get_version(reinterpret_cast<wl_proxy*>(wl_surface)) >= WL_SURFACE_SET_BUFFER_SCALE_SINCE_VERSION) {
        wl_surface_set_buffer_scale(wl_surface, output_scale);
    }
    scaling_dirty = false;
}

void OverlayClient::request_wayland_geometry() {
    if (!wl_layer_surface) return;
    if (scaling_mode == "stretched") {
        zwlr_layer_surface_v1_set_anchor(wl_layer_surface, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
            ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
        zwlr_layer_surface_v1_set_size(wl_layer_surface, 0, 0);
        return;
    }
    const auto geometry = display_scaling::overlay_geometry(physical_monitor_width, physical_monitor_height,
                                                            scaling_mode, game_width, game_height, game_x, game_y);
    const bool supports_buffer_scale = wl_proxy_get_version(reinterpret_cast<wl_proxy*>(wl_surface)) >= WL_SURFACE_SET_BUFFER_SCALE_SINCE_VERSION;
    const double scale = wl_fractional_scale ? preferred_scale_120 / 120.0 : (supports_buffer_scale ? static_cast<double>(output_scale) : 1.0);
    const int requested_width = display_scaling::rounded_pixel_extent(display_scaling::physical_to_logical(geometry.width, scale));
    const int requested_height = display_scaling::rounded_pixel_extent(display_scaling::physical_to_logical(geometry.height, scale));
    if (requested_width == 0 || requested_height == 0) {
        wayland_closed = true;
        return;
    }
    if (scaling_mode == "custom") {
        zwlr_layer_surface_v1_set_anchor(wl_layer_surface, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
        zwlr_layer_surface_v1_set_margin(wl_layer_surface,
            static_cast<int>(std::round(display_scaling::physical_to_logical(game_y, scale))), 0, 0,
            static_cast<int>(std::round(display_scaling::physical_to_logical(game_x, scale))));
    } else {
        // Opposite anchors with explicit dimensions center the surface on output.
        zwlr_layer_surface_v1_set_anchor(wl_layer_surface, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
            ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    }
    zwlr_layer_surface_v1_set_size(wl_layer_surface, requested_width, requested_height);
}
