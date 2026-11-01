#include "focus_monitor.hpp"

#include "logger.hpp"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <dbus/dbus.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <vector>

namespace {
std::string lowercase(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string executable_for_pid(unsigned long pid)
{
    if (pid == 0) return {};

    std::error_code error;
    const std::filesystem::path executable =
        std::filesystem::read_symlink("/proc/" + std::to_string(pid) + "/exe", error);
    return error ? std::string{} : executable.string();
}

// dbus-broker drops session-bus connections from root (the SASL Hello never
// gets a reply), so the KWin D-Bus connection must be established with the
// desktop user's credentials. Everything else in the process stays root.
class ScopedDesktopCredentials {
public:
    ScopedDesktopCredentials()
    {
        const char* uid_text = std::getenv("SUDO_UID");
        const char* gid_text = std::getenv("SUDO_GID");
        if (uid_text == nullptr || gid_text == nullptr) return;
        try {
            const uid_t uid = static_cast<uid_t>(std::stoul(uid_text));
            const gid_t gid = static_cast<gid_t>(std::stoul(gid_text));
            saved_uid_ = geteuid();
            saved_gid_ = getegid();
            if (setegid(gid) != 0) return;
            if (seteuid(uid) != 0) {
                setegid(saved_gid_);
                return;
            }
            active_ = true;
        } catch (const std::exception&) {
            active_ = false;
        }
    }

    ~ScopedDesktopCredentials() { restore(); }

    ScopedDesktopCredentials(const ScopedDesktopCredentials&) = delete;
    ScopedDesktopCredentials& operator=(const ScopedDesktopCredentials&) = delete;

    void restore()
    {
        if (!active_) return;
        active_ = false;
        seteuid(saved_uid_);
        setegid(saved_gid_);
    }

private:
    bool active_ = false;
    uid_t saved_uid_ = 0;
    gid_t saved_gid_ = 0;
};

std::string window_utf8_property(Display* display, Window window, Atom property)
{
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long item_count = 0;
    unsigned long bytes_after = 0;
    unsigned char* data = nullptr;
    if (XGetWindowProperty(display, window, property, 0, 1024, False, AnyPropertyType,
                           &actual_type, &actual_format, &item_count, &bytes_after, &data) != Success ||
        !data) {
        return {};
    }

    std::string value(reinterpret_cast<char*>(data), item_count);
    XFree(data);
    return value;
}

// KWin's loadScript() accepts unreadable files but start() then discards them,
// so confirm the script really is loaded instead of trusting the call.
bool kwin_script_loaded(DBusConnection* connection, const char* plugin_name)
{
    if (connection == nullptr || plugin_name == nullptr) return false;

    DBusMessage* message = dbus_message_new_method_call(
        "org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting", "isScriptLoaded");
    if (message == nullptr) return false;

    const char* name = plugin_name;
    if (!dbus_message_append_args(message, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID)) {
        dbus_message_unref(message);
        return false;
    }

    DBusError error;
    dbus_error_init(&error);
    DBusMessage* reply =
        dbus_connection_send_with_reply_and_block(connection, message, 1500, &error);
    dbus_message_unref(message);
    if (reply == nullptr) {
        dbus_error_free(&error);
        return false;
    }

    dbus_bool_t loaded = FALSE;
    const bool parsed =
        dbus_message_get_args(reply, &error, DBUS_TYPE_BOOLEAN, &loaded, DBUS_TYPE_INVALID);
    dbus_message_unref(reply);
    if (!parsed) {
        dbus_error_free(&error);
        return false;
    }
    dbus_error_free(&error);
    return loaded == TRUE;
}
}  // namespace

struct FocusMonitor::Impl {
    explicit Impl(bool wayland_session)
    {
        initialize_x11();
        if (wayland_session) {
            initialize_kwin();
        }

        if (!kwin_available && !x11_display) {
            BUNNYWARE_LOG_ERROR(
                "FocusMonitor: no supported active-window backend; focus checks will fail closed.");
        }
    }

    ~Impl()
    {
        cleanup_kwin();
        if (x11_display) {
            XCloseDisplay(x11_display);
            x11_display = nullptr;
        }
    }

    void initialize_x11()
    {
        x11_display = XOpenDisplay(nullptr);
        if (!x11_display) return;

        x11_root = DefaultRootWindow(x11_display);
        net_active_window = XInternAtom(x11_display, "_NET_ACTIVE_WINDOW", True);
        net_wm_name = XInternAtom(x11_display, "_NET_WM_NAME", True);
        net_wm_pid = XInternAtom(x11_display, "_NET_WM_PID", True);
        if (net_active_window == None) {
            XCloseDisplay(x11_display);
            x11_display = nullptr;
            return;
        }
        BUNNYWARE_LOG_INFO("FocusMonitor: X11/XWayland EWMH fallback available.");
    }

    static DBusHandlerResult handle_focus_message(DBusConnection* connection, DBusMessage* message,
                                                   void* user_data)
    {
        if (!dbus_message_is_method_call(message, "org.bunnyware.Focus", "SetActiveWindow")) {
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        }

        const char* resource_name = "";
        const char* resource_class = "";
        const char* title = "";
        DBusError error;
        dbus_error_init(&error);
        if (!dbus_message_get_args(message, &error,
                                   DBUS_TYPE_STRING, &resource_name,
                                   DBUS_TYPE_STRING, &resource_class,
                                   DBUS_TYPE_STRING, &title,
                                   DBUS_TYPE_INVALID)) {
            BUNNYWARE_LOG_WARN("FocusMonitor: invalid KWin focus message: {}",
                             error.message ? error.message : "unknown error");
            dbus_error_free(&error);
            return DBUS_HANDLER_RESULT_HANDLED;
        }

        auto* self = static_cast<Impl*>(user_data);
        self->kwin_focused = FocusMonitor::matches_cs2_identity(
            resource_name ? resource_name : "", resource_class ? resource_class : "",
            title ? title : "");
        if (!self->kwin_received_state) {
            BUNNYWARE_LOG_INFO("FocusMonitor: received initial native Wayland focus state.");
        }
        self->kwin_received_state = true;

        DBusMessage* reply = dbus_message_new_method_return(message);
        if (reply) {
            dbus_connection_send(connection, reply, nullptr);
            dbus_message_unref(reply);
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    bool call_kwin_no_reply(const char* method, const char* string_arg = nullptr)
    {
        DBusMessage* message =
            dbus_message_new_method_call("org.kde.KWin", "/Scripting",
                                         "org.kde.kwin.Scripting", method);
        if (!message) return false;
        if (string_arg &&
            !dbus_message_append_args(message, DBUS_TYPE_STRING, &string_arg, DBUS_TYPE_INVALID)) {
            dbus_message_unref(message);
            return false;
        }

        DBusError error;
        dbus_error_init(&error);
        DBusMessage* reply =
            dbus_connection_send_with_reply_and_block(dbus_connection, message, 1500, &error);
        dbus_message_unref(message);
        if (!reply) {
            BUNNYWARE_LOG_WARN("FocusMonitor: KWin {} call failed: {}", method,
                             error.message ? error.message : "unknown error");
            dbus_error_free(&error);
            return false;
        }
        dbus_message_unref(reply);
        return true;
    }

    bool load_kwin_script()
    {
        const char* unique_name = dbus_bus_get_unique_name(dbus_connection);
        if (!unique_name) return false;

        const char* session_uid = std::getenv("SUDO_UID");
        if (!session_uid) session_uid = std::getenv("UID");
        kwin_plugin_name =
            "bunnyware_focus_" + std::string(session_uid ? session_uid : "session");
        call_kwin_no_reply("unloadScript", kwin_plugin_name.c_str());

        std::ostringstream script;
        script
            << "const bunnywareService = \"" << unique_name << "\";\n"
            << "function bunnywareString(value) {\n"
            << "    return value === undefined || value === null ? \"\" : String(value);\n"
            << "}\n"
            << "function bunnywareReport(window) {\n"
            << "    callDBus(bunnywareService, \"/Focus\", \"org.bunnyware.Focus\", "
               "\"SetActiveWindow\",\n"
            << "        bunnywareString(window ? window.resourceName : \"\"),\n"
            << "        bunnywareString(window ? window.resourceClass : \"\"),\n"
            << "        bunnywareString(window ? window.caption : \"\"));\n"
            << "}\n"
            << "workspace.windowActivated.connect(bunnywareReport);\n"
            << "bunnywareReport(workspace.activeWindow);\n";

        std::vector<char> path_template{
            '/', 't', 'm', 'p', '/', 'c', 'h', 'u', 'd', 'c', 's', '2', '-',
            'f', 'o', 'c', 'u', 's', '-', 'X', 'X', 'X', 'X', 'X', 'X', '.',
            'j', 's', '\0'};
        const int script_fd = mkstemps(path_template.data(), 3);
        if (script_fd < 0) return false;

        // KWin runs as the desktop user and silently drops scripts it cannot
        // read. mkstemps creates the file 0600, so make it readable and hand it
        // to the desktop user when running under sudo.
        fchmod(script_fd, 0644);
        const char* sudo_uid_text = std::getenv("SUDO_UID");
        const char* sudo_gid_text = std::getenv("SUDO_GID");
        if (sudo_uid_text != nullptr && sudo_gid_text != nullptr) {
            const auto parse_id = [](const char* text) -> unsigned long {
                return text != nullptr ? std::strtoul(text, nullptr, 10) : 0;
            };
            fchown(script_fd, static_cast<uid_t>(parse_id(sudo_uid_text)),
                   static_cast<gid_t>(parse_id(sudo_gid_text)));
        }

        kwin_script_path = path_template.data();
        const std::string script_source = script.str();
        size_t bytes_written = 0;
        while (bytes_written < script_source.size()) {
            const ssize_t result =
                write(script_fd, script_source.data() + bytes_written,
                      script_source.size() - bytes_written);
            if (result <= 0) {
                close(script_fd);
                return false;
            }
            bytes_written += static_cast<size_t>(result);
        }
        close(script_fd);

        DBusMessage* message =
            dbus_message_new_method_call("org.kde.KWin", "/Scripting",
                                         "org.kde.kwin.Scripting", "loadScript");
        if (!message) return false;
        const char* path = kwin_script_path.c_str();
        const char* plugin = kwin_plugin_name.c_str();
        if (!dbus_message_append_args(message,
                                      DBUS_TYPE_STRING, &path,
                                      DBUS_TYPE_STRING, &plugin,
                                      DBUS_TYPE_INVALID)) {
            dbus_message_unref(message);
            return false;
        }

        DBusError error;
        dbus_error_init(&error);
        DBusMessage* reply =
            dbus_connection_send_with_reply_and_block(dbus_connection, message, 1500, &error);
        dbus_message_unref(message);
        if (!reply) {
            BUNNYWARE_LOG_WARN("FocusMonitor: failed to load KWin focus script: {}",
                             error.message ? error.message : "unknown error");
            dbus_error_free(&error);
            return false;
        }

        dbus_int32_t script_id = -1;
        bool parsed = dbus_message_get_args(reply, &error,
                                            DBUS_TYPE_INT32, &script_id,
                                            DBUS_TYPE_INVALID);
        dbus_message_unref(reply);
        if (!parsed || script_id < 0) {
            BUNNYWARE_LOG_WARN("FocusMonitor: KWin returned an invalid script id.");
            dbus_error_free(&error);
            return false;
        }

        if (!call_kwin_no_reply("start")) {
            return false;
        }
        if (!kwin_script_loaded(dbus_connection, kwin_plugin_name.c_str())) {
            BUNNYWARE_LOG_WARN(
                "FocusMonitor: KWin did not start the focus script (is the script file readable?).");
            return false;
        }
        return true;
    }

    void initialize_kwin()
    {
        DBusError error;
        dbus_error_init(&error);

        const char* sudo_uid = std::getenv("SUDO_UID");
        std::string address;
        if (sudo_uid != nullptr) {
            address = "unix:path=/run/user/" + std::string(sudo_uid) + "/bus";
        }

        // Connect while temporarily acting as the desktop user; dbus-broker
        // rejects root peers on the user's session bus. The authenticated
        // connection keeps working after root is restored.
        {
            ScopedDesktopCredentials credentials;
            if (!address.empty()) {
                dbus_connection = dbus_connection_open_private(address.c_str(), &error);
                if (dbus_connection && !dbus_bus_register(dbus_connection, &error)) {
                    dbus_connection_close(dbus_connection);
                    dbus_connection_unref(dbus_connection);
                    dbus_connection = nullptr;
                }
            } else {
                dbus_connection = dbus_bus_get_private(DBUS_BUS_SESSION, &error);
            }
        }

        if (!dbus_connection) {
            BUNNYWARE_LOG_WARN("FocusMonitor: session D-Bus unavailable: {}",
                             error.message ? error.message : "unknown error");
            dbus_error_free(&error);
            return;
        }
        dbus_connection_set_exit_on_disconnect(dbus_connection, false);

        if (!dbus_bus_name_has_owner(dbus_connection, "org.kde.KWin", &error)) {
            if (dbus_error_is_set(&error)) {
                BUNNYWARE_LOG_WARN("FocusMonitor: failed to query KWin on D-Bus: {}",
                                 error.message ? error.message : "unknown error");
                dbus_error_free(&error);
            }
            cleanup_kwin();
            return;
        }

        static const DBusObjectPathVTable vtable = {
            nullptr,
            &Impl::handle_focus_message,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
        };
        if (!dbus_connection_register_object_path(dbus_connection, "/Focus", &vtable, this)) {
            BUNNYWARE_LOG_WARN("FocusMonitor: failed to register its D-Bus focus endpoint.");
            cleanup_kwin();
            return;
        }
        dbus_path_registered = true;

        if (!load_kwin_script()) {
            cleanup_kwin();
            return;
        }

        kwin_available = true;
        BUNNYWARE_LOG_INFO("FocusMonitor: native KDE Wayland focus tracking active.");
    }

    void cleanup_kwin()
    {
        if (dbus_connection) {
            if (!kwin_plugin_name.empty()) {
                call_kwin_no_reply("unloadScript", kwin_plugin_name.c_str());
            }
            if (dbus_path_registered) {
                dbus_connection_unregister_object_path(dbus_connection, "/Focus");
                dbus_path_registered = false;
            }
            dbus_connection_close(dbus_connection);
            dbus_connection_unref(dbus_connection);
            dbus_connection = nullptr;
        }
        if (!kwin_script_path.empty()) {
            std::error_code error;
            std::filesystem::remove(kwin_script_path, error);
        }
        kwin_available = false;
    }

    bool poll_x11()
    {
        if (!x11_display) return false;

        Atom actual_type = None;
        int actual_format = 0;
        unsigned long item_count = 0;
        unsigned long bytes_after = 0;
        unsigned char* data = nullptr;
        if (XGetWindowProperty(x11_display, x11_root, net_active_window, 0, 1, False, XA_WINDOW,
                               &actual_type, &actual_format, &item_count, &bytes_after, &data) !=
                Success ||
            !data || item_count != 1) {
            if (data) XFree(data);
            return false;
        }

        Window active_window = *reinterpret_cast<Window*>(data);
        XFree(data);
        if (active_window == None) return false;

        std::string resource_name;
        std::string resource_class;
        XClassHint class_hint{};
        if (XGetClassHint(x11_display, active_window, &class_hint)) {
            if (class_hint.res_name) {
                resource_name = class_hint.res_name;
                XFree(class_hint.res_name);
            }
            if (class_hint.res_class) {
                resource_class = class_hint.res_class;
                XFree(class_hint.res_class);
            }
        }

        std::string title;
        if (net_wm_name != None) {
            title = window_utf8_property(x11_display, active_window, net_wm_name);
        }
        if (title.empty()) {
            char* legacy_title = nullptr;
            if (XFetchName(x11_display, active_window, &legacy_title) && legacy_title) {
                title = legacy_title;
                XFree(legacy_title);
            }
        }

        unsigned long pid = 0;
        if (net_wm_pid != None) {
            Atom pid_type = None;
            unsigned char* pid_data = nullptr;
            if (XGetWindowProperty(x11_display, active_window, net_wm_pid, 0, 1, False, XA_CARDINAL,
                                   &pid_type, &actual_format, &item_count, &bytes_after, &pid_data) ==
                    Success &&
                pid_data && item_count == 1) {
                pid = *reinterpret_cast<unsigned long*>(pid_data);
            }
            if (pid_data) XFree(pid_data);
        }

        return FocusMonitor::matches_cs2_identity(
            resource_name, resource_class, title, executable_for_pid(pid));
    }

    bool poll()
    {
        if (dbus_connection) {
            if (!dbus_connection_read_write_dispatch(dbus_connection, 0)) {
                kwin_available = false;
                kwin_received_state = false;
            }
        }
        if (kwin_available && kwin_received_state) return kwin_focused;

        const auto now = std::chrono::steady_clock::now();
        if (now - last_x11_poll >= std::chrono::milliseconds(50)) {
            x11_focused = poll_x11();
            last_x11_poll = now;
        }
        return x11_focused;
    }

    Display* x11_display = nullptr;
    Window x11_root = None;
    Atom net_active_window = None;
    Atom net_wm_name = None;
    Atom net_wm_pid = None;
    bool x11_focused = false;
    std::chrono::steady_clock::time_point last_x11_poll{};

    DBusConnection* dbus_connection = nullptr;
    bool dbus_path_registered = false;
    bool kwin_available = false;
    bool kwin_received_state = false;
    bool kwin_focused = false;
    std::string kwin_plugin_name;
    std::string kwin_script_path;
};

FocusMonitor::FocusMonitor(bool wayland_session)
    : impl_(std::make_unique<Impl>(wayland_session))
{
}

FocusMonitor::~FocusMonitor() = default;

bool FocusMonitor::is_cs2_focused()
{
    return impl_->poll();
}

bool FocusMonitor::backend_available() const
{
    return impl_->kwin_available || impl_->x11_display;
}

bool FocusMonitor::has_native_focus_state() const
{
    return impl_->kwin_received_state;
}

bool FocusMonitor::matches_cs2_identity(const std::string& resource_name,
                                        const std::string& resource_class,
                                        const std::string& title,
                                        const std::string& executable)
{
    const auto is_cs2_id = [](const std::string& raw) {
        const std::string value = lowercase(raw);
        return value == "cs2" || value == "steam_app_730" ||
               value.find("steam_app_730") != std::string::npos;
    };

    if (is_cs2_id(resource_name) || is_cs2_id(resource_class)) return true;

    const std::string executable_name =
        lowercase(std::filesystem::path(executable).filename().string());
    if (executable_name == "cs2" || executable_name == "cs2.sh") return true;

    if (resource_name.empty() && resource_class.empty()) {
        const std::string normalized_title = lowercase(title);
        return normalized_title == "counter-strike 2" ||
               normalized_title == "counter-strike 2 - vulkan";
    }
    return false;
}
