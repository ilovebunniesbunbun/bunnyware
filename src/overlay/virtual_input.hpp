#pragma once

#include <random>
#include <vector>
#include <linux/input.h>
#include <linux/uinput.h>

// Spoofed device name; GlobalInputReader skips this device so the overlay never
// mistakes its own injected input for the user's.
inline constexpr char kVirtualDeviceName[] = "TI-84 Plus Silver Calculator";

class VirtualInput {
private:
    int fd = -1;
    std::vector<bool> pressed_state; // Index 0 to 1023 to track keys and mouse button states
    std::mt19937 rng;
    std::uniform_int_distribution<int> delay_dist;

    double remainder_x = 0.0;
    double remainder_y = 0.0;

    void send_event(uint16_t type, uint16_t code, int32_t value);
    void sleep_random_delay();
    int get_mouse_button_code(int button) const;
    void release_all_unchecked();

public:
    VirtualInput();
    ~VirtualInput();

    static void set_input_enabled(bool enabled);
    static bool is_input_enabled();

    void release_all();

    // Relative mouse movement (with integer rounding and remainder accumulation)
    void move_rel(double dx, double dy);

    // Mouse button click / press / release (buttons 1-3)
    void click(int button);
    void clickp(int button);
    void clickr(int button);

    // Keyboard key press / press and hold / release
    void key_press(int key);
    void keyp(int key);
    void keyr(int key);
};
