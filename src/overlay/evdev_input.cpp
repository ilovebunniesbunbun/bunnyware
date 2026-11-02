#include "evdev_input.hpp"
#include "logger.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_set>

namespace {

// Input injection is gated by the game-focus check (App::set_game_activity_allowed):
// it starts disabled so no synthetic input can fire before the first focus poll.
std::atomic_bool g_input_enabled{false};
std::atomic_bool g_input_available{false};
std::atomic_bool g_quiet_probe{false};
std::once_flag g_input_probe_once;
std::mutex g_instances_mutex;
std::unordered_set<EvdevInput*> g_instances;

constexpr int kMouseButtonMin = BTN_MOUSE;  // 0x110
constexpr int kMouseButtonMax = BTN_TASK;   // 0x117
}  // namespace

EvdevInput::EvdevInput() : pressed_state(KEY_MAX, false), delay_dist(75, 150)
{
    std::random_device rd;
    rng.seed(rd());

    if (!open_mouse()) {
        throw std::runtime_error(
            "EvdevInput: no writable mouse event device found. "
            "Run as root or add your user to the 'input' group.");
    }
    if (!open_keyboard()) {
        BUNNYWARE_LOG_WARN("EvdevInput: no writable keyboard event device found; keyboard actions are disabled.");
    }

    std::scoped_lock lock(g_instances_mutex);
    g_instances.insert(this);
}

EvdevInput::~EvdevInput()
{
    release_all_unchecked();
    {
        std::scoped_lock lock(g_instances_mutex);
        g_instances.erase(this);
    }
    if (mouse_fd >= 0) {
        close(mouse_fd);
        mouse_fd = -1;
    }
    if (keyboard_fd >= 0) {
        close(keyboard_fd);
        keyboard_fd = -1;
    }
}

void EvdevInput::set_input_enabled(bool enabled)
{
    const bool previous = g_input_enabled.exchange(enabled, std::memory_order_acq_rel);
    if (previous && !enabled) {
        std::scoped_lock lock(g_instances_mutex);
        for (EvdevInput* input : g_instances) {
            input->release_all_unchecked();
        }
    }
}

bool EvdevInput::is_input_enabled()
{
    return g_input_enabled.load(std::memory_order_acquire);
}

bool EvdevInput::is_input_available()
{
    std::call_once(g_input_probe_once, [] {
        // Suppress the success info lines: this probe runs at startup and would
        // otherwise duplicate the device log when a module opens it for real.
        g_quiet_probe.store(true, std::memory_order_release);
        try {
            EvdevInput probe;
            g_input_available.store(true, std::memory_order_release);
        } catch (const std::exception& e) {
            BUNNYWARE_LOG_WARN("EvdevInput: input injection unavailable: {}", e.what());
            g_input_available.store(false, std::memory_order_release);
        }
        g_quiet_probe.store(false, std::memory_order_release);
    });
    return g_input_available.load(std::memory_order_acquire);
}

std::string EvdevInput::find_by_id_suffix(const char* suffix)
{
    DIR* dir = opendir("/dev/input/by-id");
    if (!dir) return {};

    std::string result;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        const std::string name = entry->d_name;
        if (name.size() >= std::strlen(suffix) &&
            name.compare(name.size() - std::strlen(suffix), std::strlen(suffix), suffix) == 0) {
            result = std::string("/dev/input/by-id/") + name;
            break;
        }
    }
    closedir(dir);
    return result;
}

bool EvdevInput::device_supports_rel(int fd, int code)
{
    unsigned long bits[REL_MAX / (8 * sizeof(unsigned long)) + 1] = {0};
    if (ioctl(fd, EVIOCGBIT(EV_REL, sizeof(bits)), bits) < 0) return false;
    return (bits[code / (8 * sizeof(unsigned long))] >> (code % (8 * sizeof(unsigned long)))) & 1UL;
}

bool EvdevInput::device_supports_key(int fd, int code)
{
    unsigned long bits[KEY_MAX / (8 * sizeof(unsigned long)) + 1] = {0};
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) < 0) return false;
    return (bits[code / (8 * sizeof(unsigned long))] >> (code % (8 * sizeof(unsigned long)))) & 1UL;
}

std::string EvdevInput::device_name(int fd)
{
    char name[256] = {};
    if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) >= 0) {
        return name;
    }
    return "Unknown Input Device";
}

bool EvdevInput::open_mouse()
{
    std::string path = find_by_id_suffix("-event-mouse");

    // Fallback: capability probe over every event node.
    if (path.empty()) {
        DIR* dir = opendir("/dev/input");
        if (!dir) return false;
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (std::strncmp(entry->d_name, "event", 5) != 0) continue;
            const std::string candidate = std::string("/dev/input/") + entry->d_name;
            int probe = open(candidate.c_str(), O_RDONLY | O_NONBLOCK);
            if (probe < 0) continue;
            const bool is_mouse = device_supports_rel(probe, REL_X) && device_supports_rel(probe, REL_Y) &&
                                  device_supports_key(probe, BTN_LEFT);
            close(probe);
            if (is_mouse) {
                path = candidate;
                break;
            }
        }
        closedir(dir);
    }

    if (path.empty()) return false;

    mouse_fd = open(path.c_str(), O_RDWR | O_NONBLOCK);
    if (mouse_fd < 0) {
        BUNNYWARE_LOG_ERROR(
            "EvdevInput: cannot open {} for writing ({}). Run as root or add your user to the "
            "'input' group.",
            path, std::strerror(errno));
        return false;
    }

    mouse_path = path;
    mouse_name = device_name(mouse_fd);
    if (!g_quiet_probe.load(std::memory_order_acquire)) {
        BUNNYWARE_LOG_INFO("EvdevInput: mouse injection via '{}' ({})", mouse_name, mouse_path);
    }
    return true;
}

bool EvdevInput::open_keyboard()
{
    std::string path = find_by_id_suffix("-event-kbd");

    if (path.empty()) {
        DIR* dir = opendir("/dev/input");
        if (!dir) return false;
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (std::strncmp(entry->d_name, "event", 5) != 0) continue;
            const std::string candidate = std::string("/dev/input/") + entry->d_name;
            int probe = open(candidate.c_str(), O_RDONLY | O_NONBLOCK);
            if (probe < 0) continue;
            const bool is_keyboard = device_supports_key(probe, KEY_A) && device_supports_key(probe, KEY_Z) &&
                                     device_supports_key(probe, KEY_SPACE);
            close(probe);
            if (is_keyboard) {
                path = candidate;
                break;
            }
        }
        closedir(dir);
    }

    if (path.empty()) return false;

    keyboard_fd = open(path.c_str(), O_RDWR | O_NONBLOCK);
    if (keyboard_fd < 0) {
        BUNNYWARE_LOG_WARN("EvdevInput: cannot open keyboard {} for writing ({}).", path, std::strerror(errno));
        return false;
    }

    keyboard_path = path;
    keyboard_name = device_name(keyboard_fd);
    if (!g_quiet_probe.load(std::memory_order_acquire)) {
        BUNNYWARE_LOG_INFO("EvdevInput: keyboard injection via '{}' ({})", keyboard_name, keyboard_path);
    }
    return true;
}

void EvdevInput::release_all_unchecked()
{
    bool released_any = false;
    for (size_t code = 0; code < pressed_state.size(); ++code) {
        if (!pressed_state[code]) continue;
        pressed_state[code] = false;

        const bool is_mouse_button =
            (static_cast<int>(code) >= kMouseButtonMin && static_cast<int>(code) <= kMouseButtonMax);
        const int fd = is_mouse_button ? mouse_fd : keyboard_fd;
        if (fd >= 0) {
            send_event(fd, EV_KEY, static_cast<uint16_t>(code), 0);
            released_any = true;
        }
    }
    if (released_any) {
        if (keyboard_fd >= 0) send_event(keyboard_fd, EV_SYN, SYN_REPORT, 0);
        if (mouse_fd >= 0) send_event(mouse_fd, EV_SYN, SYN_REPORT, 0);
    }
    remainder_x = 0.0;
    remainder_y = 0.0;
}

void EvdevInput::release_all()
{
    release_all_unchecked();
}

void EvdevInput::send_event(int fd, uint16_t type, uint16_t code, int32_t value)
{
    if (fd < 0) return;

    struct input_event ev;
    std::memset(&ev, 0, sizeof(ev));

    auto now = std::chrono::system_clock::now().time_since_epoch();
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(now);
    auto microsecs = std::chrono::duration_cast<std::chrono::microseconds>(now - secs);
    ev.time.tv_sec = secs.count();
    ev.time.tv_usec = microsecs.count();

    ev.type = type;
    ev.code = code;
    ev.value = value;

    if (write(fd, &ev, sizeof(ev)) < 0) {
        BUNNYWARE_LOG_ERROR("EvdevInput: failed to write input event (type={}, code={}, errno={})", type, code, errno);
    }
}

void EvdevInput::sleep_random_delay()
{
    int delay = delay_dist(rng);
    std::this_thread::sleep_for(std::chrono::milliseconds(delay));
}

int EvdevInput::get_mouse_button_code(int button) const
{
    switch (button) {
        case 1:
            return BTN_LEFT;
        case 2:
            return BTN_RIGHT;
        case 3:
            return BTN_MIDDLE;
        default:
            return BTN_LEFT;
    }
}

void EvdevInput::move_rel(double dx, double dy)
{
    if (!is_input_enabled() || mouse_fd < 0) return;

    remainder_x += dx;
    remainder_y += dy;

    // Standard rounding to nearest whole integer.
    int move_x = static_cast<int>(std::round(remainder_x));
    int move_y = static_cast<int>(std::round(remainder_y));

    remainder_x -= move_x;
    remainder_y -= move_y;

    if (move_x != 0) {
        send_event(mouse_fd, EV_REL, REL_X, move_x);
    }
    if (move_y != 0) {
        send_event(mouse_fd, EV_REL, REL_Y, move_y);
    }
    if (move_x != 0 || move_y != 0) {
        send_event(mouse_fd, EV_SYN, SYN_REPORT, 0);
    }
}

void EvdevInput::click(int button)
{
    if (!is_input_enabled()) return;

    clickp(button);
    sleep_random_delay();
    clickr(button);
}

void EvdevInput::clickp(int button)
{
    if (!is_input_enabled() || mouse_fd < 0) return;

    int code = get_mouse_button_code(button);
    if (code >= 0 && code < KEY_MAX) {
        pressed_state[code] = true;
        send_event(mouse_fd, EV_KEY, static_cast<uint16_t>(code), 1);
        send_event(mouse_fd, EV_SYN, SYN_REPORT, 0);
    }
}

void EvdevInput::clickr(int button)
{
    if (mouse_fd < 0) return;

    int code = get_mouse_button_code(button);
    if (code >= 0 && code < KEY_MAX && pressed_state[code]) {
        pressed_state[code] = false;
        send_event(mouse_fd, EV_KEY, static_cast<uint16_t>(code), 0);
        send_event(mouse_fd, EV_SYN, SYN_REPORT, 0);
    }
}

void EvdevInput::key_press(int key)
{
    if (!is_input_enabled()) return;

    keyp(key);
    sleep_random_delay();
    keyr(key);
}

void EvdevInput::keyp(int key)
{
    if (!is_input_enabled() || keyboard_fd < 0) return;

    if (key >= 0 && key < KEY_MAX) {
        pressed_state[key] = true;
        send_event(keyboard_fd, EV_KEY, static_cast<uint16_t>(key), 1);
        send_event(keyboard_fd, EV_SYN, SYN_REPORT, 0);
    }
}

void EvdevInput::keyr(int key)
{
    if (keyboard_fd < 0) return;

    if (key >= 0 && key < KEY_MAX && pressed_state[key]) {
        pressed_state[key] = false;
        send_event(keyboard_fd, EV_KEY, static_cast<uint16_t>(key), 0);
        send_event(keyboard_fd, EV_SYN, SYN_REPORT, 0);
    }
}
