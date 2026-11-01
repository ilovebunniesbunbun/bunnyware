#include <cstdlib>
#include <vector>
#include <unistd.h>
#include <sys/types.h>

#include "app/App.hpp"
#include "config.hpp"
#include "logger.hpp"
#include "overlay/offset_refresher.hpp"

int main(int argc, char** argv)
{
    // Parse launch options
    bool force_x11 = false;
    bool validate_config_only = false;
    bool update_offsets_only = false;
    std::string validate_config_path;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--x11") {
            force_x11 = true;
        } else if (arg == "--update-offsets" || arg == "--refresh-offsets") {
            update_offsets_only = true;
        } else if (arg == "--validate-config") {
            validate_config_only = true;
            if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) {
                validate_config_path = argv[++i];
            }
        }
    }

    // Automatically set up X11 and Wayland display authorization when run under sudo
    if (getenv("SUDO_USER") != nullptr) {
        if (getenv("XAUTHORITY") == nullptr) {
            std::string xauth_path = "/home/";
            xauth_path += getenv("SUDO_USER");
            xauth_path += "/.Xauthority";
            setenv("XAUTHORITY", xauth_path.c_str(), 1);
        }
        if (getenv("DISPLAY") == nullptr) {
            setenv("DISPLAY", ":0", 0);
        }
    }
    if (getenv("SUDO_UID") != nullptr) {
        std::string uid_str = getenv("SUDO_UID");
        std::string runtime_dir = "/run/user/" + uid_str;
        std::string wayland_socket = runtime_dir + "/wayland-0";
        if (access(wayland_socket.c_str(), F_OK) == 0) {
            if (getenv("XDG_RUNTIME_DIR") == nullptr) {
                setenv("XDG_RUNTIME_DIR", runtime_dir.c_str(), 1);
            }
            if (getenv("WAYLAND_DISPLAY") == nullptr) {
                setenv("WAYLAND_DISPLAY", "wayland-0", 1);
            }
            if (getenv("XDG_SESSION_TYPE") == nullptr) {
                setenv("XDG_SESSION_TYPE", "wayland", 1);
            }
            BUNNYWARE_LOG_INFO("Running under sudo: redirected Wayland display to {}/wayland-0", runtime_dir);
        }
    }

    if (validate_config_only) {
        std::string filename = validate_config_path.empty() ? config_file_path() : validate_config_path;
        std::string error_msg;
        if (validate_config(filename, error_msg)) {
            BUNNYWARE_LOG_INFO("Config file '{}' is valid.", filename);
            return 0;
        } else {
            BUNNYWARE_LOG_ERROR("Config validation failed: {}", error_msg);
            return 1;
        }
    }

    if (update_offsets_only) {
        const offset_refresher::RefreshStatus status = offset_refresher::refresh_from_live_process();
        return status.success ? 0 : 1;
    }

    BUNNYWARE_LOG_INFO("Launching GPU-Skinned Linux Overlay...");

    OverlayConfig cfg = load_startup_config();
    if (force_x11) {
        cfg.force_x11 = true;
        BUNNYWARE_LOG_INFO("Launch option --x11 detected; forcing the X11/XWayland overlay backend.");
    }
    spdlog::set_level(spdlog::level::debug);

    // Apply GPU offload settings before GLFW/OpenGL driver initialization
    if (cfg.gpu_preference != "default" && !cfg.gpu_preference.empty()) {
        std::vector<GpuDevice> gpus = detect_gpus();
        const GpuDevice* selected_gpu = nullptr;
        for (const auto& gpu : gpus) {
            if (gpu.name == cfg.gpu_preference) {
                selected_gpu = &gpu;
                break;
            }
        }
        if (selected_gpu) {
            BUNNYWARE_LOG_INFO("Applying GPU preference: {}", selected_gpu->display_name);
            if (selected_gpu->vendor_id == "0x10de" || selected_gpu->vendor_id == "10de" ||
                selected_gpu->driver == "nvidia") {
                setenv("__NV_PRIME_RENDER_OFFLOAD", "1", 1);
                setenv("__GLX_VENDOR_LIBRARY_NAME", "nvidia", 1);
                setenv("__VK_LAYER_NV_optimus", "NVIDIA_only", 1);
                unsetenv("DRI_PRIME");
            } else {
                // AMD / Intel offload via DRI_PRIME
                setenv("DRI_PRIME", selected_gpu->name.c_str(), 1);
                unsetenv("__NV_PRIME_RENDER_OFFLOAD");
                unsetenv("__GLX_VENDOR_LIBRARY_NAME");
                unsetenv("__VK_LAYER_NV_optimus");
            }
        }
    } else {
        // Clear offload environment variables for default setting
        unsetenv("__NV_PRIME_RENDER_OFFLOAD");
        unsetenv("__GLX_VENDOR_LIBRARY_NAME");
        unsetenv("__VK_LAYER_NV_optimus");
        unsetenv("DRI_PRIME");
    }

    App app(cfg);
    int ret = app.run();

    // Fix ownership of config and settings files if run via sudo
    const char* sudo_uid_str = getenv("SUDO_UID");
    const char* sudo_gid_str = getenv("SUDO_GID");
    if (sudo_uid_str && sudo_gid_str) {
        uid_t uid = std::stoul(sudo_uid_str);
        gid_t gid = std::stoul(sudo_gid_str);
        chown(get_config_path("imgui.ini").c_str(), uid, gid);
        chown("imgui.ini", uid, gid);
        chown(get_configs_directory().c_str(), uid, gid);
        for (const auto& config_name : list_configs()) {
            std::string path = get_configs_directory() + "/" + config_name + ".json";
            chown(path.c_str(), uid, gid);
        }
        chown((get_configs_directory() + "/_settings.json").c_str(), uid, gid);
    }
    return ret;
}
