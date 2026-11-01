#pragma once

#include <array>
#include <vector>
#include <linux/input.h>

class GlobalInputReader {
private:
    std::vector<int> fds;
    // Per-device key-state bitmaps captured by begin_frame() (one ioctl per
    // device per frame instead of one per device per key query).
    std::vector<std::array<unsigned char, KEY_MAX / 8 + 1>> key_state_cache;

public:
    GlobalInputReader();
    ~GlobalInputReader();

    int glfw_to_linux_key(int glfw_key) const;

    // Captures the key-state bitmap of every input device. Call once per frame
    // before issuing is_key_pressed() queries so they all read one consistent
    // snapshot and cost zero syscalls.
    void begin_frame();

    // Queries state for the given GLFW key/mouse button from the latest
    // begin_frame() snapshot. Falls back to a live device query if
    // begin_frame() was never called.
    bool is_key_pressed(int glfw_key) const;
};
