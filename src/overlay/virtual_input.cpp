#include "virtual_input.hpp"
#include "logger.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <sys/ioctl.h>
#include <chrono>
#include <cmath>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_set>

namespace {
std::atomic_bool g_input_enabled{false};
std::mutex g_instances_mutex;
std::unordered_set<VirtualInput*> g_instances;
}

VirtualInput::VirtualInput() : pressed_state(1024, false), delay_dist(75, 150)
{
    // Seed standard mt19937 RNG
    std::random_device rd;
    rng.seed(rd());

    fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        throw std::runtime_error("VirtualInput: Failed to open /dev/uinput. Ensure you are running as sudo / root.");
    }

    // Enable sync, mouse buttons, relative motion, and keyboard keys
    ioctl(fd, UI_SET_EVBIT, EV_SYN);
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_REL);

    // Relative mouse X and Y movement
    ioctl(fd, UI_SET_RELBIT, REL_X);
    ioctl(fd, UI_SET_RELBIT, REL_Y);

    // Mouse buttons 1 (Left), 2 (Right), 3 (Middle)
    ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);
    ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT);
    ioctl(fd, UI_SET_KEYBIT, BTN_MIDDLE);

    // Enable all keyboard event keys up to KEY_MAX
    for (int k = 1; k < KEY_MAX; ++k) {
        ioctl(fd, UI_SET_KEYBIT, k);
    }

    // Setup device structures spoofed as a TI-84 Plus Silver Calculator
    struct uinput_setup usetup;
    std::memset(&usetup, 0, sizeof(usetup));
    usetup.id.bustype = BUS_USB;
    usetup.id.vendor = 0x0451;      // Texas Instruments
    usetup.id.product = 0xe008;     // TI-84 Plus Silver Calculator
    usetup.id.version = 1;
    std::strncpy(usetup.name, kVirtualDeviceName, UINPUT_MAX_NAME_SIZE);

    ioctl(fd, UI_DEV_SETUP, &usetup);
    ioctl(fd, UI_DEV_CREATE);

    std::scoped_lock lock(g_instances_mutex);
    g_instances.insert(this);
}

VirtualInput::~VirtualInput()
{
    {
        std::scoped_lock lock(g_instances_mutex);
        g_instances.erase(this);
    }
    if (fd >= 0) {
        ioctl(fd, UI_DEV_DESTROY);
        close(fd);
        fd = -1;
    }
}

void VirtualInput::set_input_enabled(bool enabled)
{
    const bool previous = g_input_enabled.exchange(enabled, std::memory_order_acq_rel);
    if (previous && !enabled) {
        std::scoped_lock lock(g_instances_mutex);
        for (VirtualInput* input : g_instances) {
            input->release_all_unchecked();
        }
    }
}

bool VirtualInput::is_input_enabled()
{
    return g_input_enabled.load(std::memory_order_acquire);
}

void VirtualInput::release_all_unchecked()
{
    if (fd < 0) return;

    bool released_any = false;
    for (size_t code = 0; code < pressed_state.size(); ++code) {
        if (!pressed_state[code]) continue;
        pressed_state[code] = false;
        send_event(EV_KEY, static_cast<uint16_t>(code), 0);
        released_any = true;
    }
    if (released_any) {
        send_event(EV_SYN, SYN_REPORT, 0);
    }
    remainder_x = 0.0;
    remainder_y = 0.0;
}

void VirtualInput::release_all()
{
    release_all_unchecked();
}

void VirtualInput::send_event(uint16_t type, uint16_t code, int32_t value)
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
        // Log error if write fails
        BUNNYWARE_LOG_ERROR("VirtualInput: Failed to write input event");
    }
}

void VirtualInput::sleep_random_delay()
{
    int delay = delay_dist(rng);
    std::this_thread::sleep_for(std::chrono::milliseconds(delay));
}

int VirtualInput::get_mouse_button_code(int button) const
{
    switch (button) {
        case 1:  return BTN_LEFT;
        case 2:  return BTN_RIGHT;
        case 3:  return BTN_MIDDLE;
        default: return BTN_LEFT;
    }
}

void VirtualInput::move_rel(double dx, double dy)
{
    if (!is_input_enabled()) return;

    remainder_x += dx;
    remainder_y += dy;

    // Standard rounding to nearest whole integer
    int move_x = static_cast<int>(std::round(remainder_x));
    int move_y = static_cast<int>(std::round(remainder_y));

    remainder_x -= move_x;
    remainder_y -= move_y;

    if (move_x != 0) {
        send_event(EV_REL, REL_X, move_x);
    }
    if (move_y != 0) {
        send_event(EV_REL, REL_Y, move_y);
    }
    if (move_x != 0 || move_y != 0) {
        send_event(EV_SYN, SYN_REPORT, 0);
    }
}

void VirtualInput::click(int button)
{
    if (!is_input_enabled()) return;

    clickp(button);
    sleep_random_delay();
    clickr(button);
}

void VirtualInput::clickp(int button)
{
    if (!is_input_enabled()) return;

    int code = get_mouse_button_code(button);
    if (code >= 0 && code < 1024) {
        pressed_state[code] = true;
        send_event(EV_KEY, code, 1);
        send_event(EV_SYN, SYN_REPORT, 0);
    }
}

void VirtualInput::clickr(int button)
{
    int code = get_mouse_button_code(button);
    if (code >= 0 && code < 1024) {
        if (pressed_state[code]) {
            pressed_state[code] = false;
            send_event(EV_KEY, code, 0);
            send_event(EV_SYN, SYN_REPORT, 0);
        }
    }
}

void VirtualInput::key_press(int key)
{
    if (!is_input_enabled()) return;

    keyp(key);
    sleep_random_delay();
    keyr(key);
}

void VirtualInput::keyp(int key)
{
    if (!is_input_enabled()) return;

    if (key >= 0 && key < 1024) {
        pressed_state[key] = true;
        send_event(EV_KEY, key, 1);
        send_event(EV_SYN, SYN_REPORT, 0);
    }
}

void VirtualInput::keyr(int key)
{
    if (key >= 0 && key < 1024) {
        if (pressed_state[key]) {
            pressed_state[key] = false;
            send_event(EV_KEY, key, 0);
            send_event(EV_SYN, SYN_REPORT, 0);
        }
    }
}
