#pragma once

#include <linux/input.h>

#include <random>
#include <string>
#include <vector>

// Input injection through the kernel evdev stack.
//
// Instead of creating a separate uinput device (which is trivially
// distinguishable from real hardware, and is ignored by Proton/SDL when the
// game has exclusive raw-input grab on the physical mouse), this backend
// attaches to the actual mouse and keyboard event nodes and injects
// EV_REL / EV_KEY events with write(). The kernel input core re-delivers these
// events through the device that produced them, so the game sees them exactly
// like real hardware input.
//
// Requires write access to /dev/input/event* (root or membership in the
// "input" group).
class EvdevInput {
public:
    EvdevInput();
    ~EvdevInput();

    static void set_input_enabled(bool enabled);
    static bool is_input_enabled();

    // Lazy one-shot probe: true when this process can open a writable mouse
    // event device. Used for the "run with sudo" warning when injection is not
    // possible. Never emits input events.
    static bool is_input_available();

    bool is_open() const
    {
        return mouse_fd >= 0;
    }
    bool has_keyboard() const
    {
        return keyboard_fd >= 0;
    }

    const std::string& get_mouse_path() const
    {
        return mouse_path;
    }
    const std::string& get_mouse_name() const
    {
        return mouse_name;
    }
    const std::string& get_keyboard_path() const
    {
        return keyboard_path;
    }
    const std::string& get_keyboard_name() const
    {
        return keyboard_name;
    }

    // Release every key/button this instance pressed.
    void release_all();

    // Relative mouse movement (integer rounding with remainder accumulation).
    void move_rel(double dx, double dy);

    // Mouse buttons (1 = left, 2 = right, 3 = middle).
    void click(int button);
    void clickp(int button);
    void clickr(int button);

    // Keyboard keys (Linux input key codes, e.g. KEY_W).
    void key_press(int key);
    void keyp(int key);
    void keyr(int key);

private:
    int mouse_fd = -1;
    int keyboard_fd = -1;
    std::string mouse_path;
    std::string mouse_name;
    std::string keyboard_path;
    std::string keyboard_name;

    std::vector<bool> pressed_state;  // Indexed by input code (0..KEY_MAX-1)
    std::mt19937 rng;
    std::uniform_int_distribution<int> delay_dist;

    double remainder_x = 0.0;
    double remainder_y = 0.0;

    bool open_mouse();
    bool open_keyboard();

    static std::string find_by_id_suffix(const char* suffix);
    static bool device_supports_rel(int fd, int code);
    static bool device_supports_key(int fd, int code);
    static std::string device_name(int fd);

    void send_event(int fd, uint16_t type, uint16_t code, int32_t value);
    void sleep_random_delay();
    int get_mouse_button_code(int button) const;
    void release_all_unchecked();
};
