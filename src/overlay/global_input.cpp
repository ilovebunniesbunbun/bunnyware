#include "global_input.hpp"
#include "logger.hpp"
#include "virtual_input.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <cstring>
#include <filesystem>
#include <GLFW/glfw3.h>

GlobalInputReader::GlobalInputReader()
{
    // Scan /dev/input/event* and open keyboard/mouse devices
    try {
        if (std::filesystem::exists("/dev/input")) {
            for (const auto& entry : std::filesystem::directory_iterator("/dev/input")) {
                std::string path = entry.path().string();
                if (path.find("/dev/input/event") == 0) {
                    int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
                    if (fd >= 0) {
                        // Skip our own uinput device so injected input is never
                        // mistaken for the user's.
                        char device_name[256] = {};
                        if (ioctl(fd, EVIOCGNAME(sizeof(device_name) - 1), device_name) >= 0 &&
                            std::strcmp(device_name, kVirtualDeviceName) == 0) {
                            close(fd);
                            continue;
                        }

                        // Query capabilities
                        unsigned long bits[EV_MAX / 8 + 1];
                        std::memset(bits, 0, sizeof(bits));
                        if (ioctl(fd, EVIOCGBIT(0, sizeof(bits)), bits) >= 0) {
                            // Verify EV_KEY capability
                            bool supports_keys = (bits[0] & (1 << EV_KEY)) != 0;
                            if (supports_keys) {
                                fds.push_back(fd);
                                continue;
                            }
                        }
                        close(fd);
                    }
                }
            }
        }
        BUNNYWARE_LOG_INFO("GlobalInputReader: Opened {} kernel input event devices.", fds.size());
    } catch (const std::exception& e) {
        BUNNYWARE_LOG_ERROR("GlobalInputReader: Failed to scan /dev/input: {}", e.what());
    }
}

GlobalInputReader::~GlobalInputReader()
{
    for (int fd : fds) {
        if (fd >= 0) {
            close(fd);
        }
    }
}

int GlobalInputReader::glfw_to_linux_key(int glfw_key) const
{
    // Mouse button mapping (1000 + button)
    if (glfw_key >= 1000 && glfw_key <= 1007) {
        switch (glfw_key) {
            case 1000: return BTN_LEFT;
            case 1001: return BTN_RIGHT;
            case 1002: return BTN_MIDDLE;
            case 1003: return BTN_SIDE;
            case 1004: return BTN_EXTRA;
            default: return -1;
        }
    }

    // Function keys F1-F12
    if (glfw_key >= GLFW_KEY_F1 && glfw_key <= GLFW_KEY_F12) {
        return KEY_F1 + (glfw_key - GLFW_KEY_F1);
    }

    // Numbers 0-9
    if (glfw_key >= GLFW_KEY_0 && glfw_key <= GLFW_KEY_9) {
        if (glfw_key == GLFW_KEY_0) return KEY_0;
        return KEY_1 + (glfw_key - GLFW_KEY_1);
    }

    // Alphabetic keys (GLFW maps to ASCII uppercase, Linux input codes map to physical positions)
    switch (glfw_key) {
        case GLFW_KEY_A: return KEY_A;
        case GLFW_KEY_B: return KEY_B;
        case GLFW_KEY_C: return KEY_C;
        case GLFW_KEY_D: return KEY_D;
        case GLFW_KEY_E: return KEY_E;
        case GLFW_KEY_F: return KEY_F;
        case GLFW_KEY_G: return KEY_G;
        case GLFW_KEY_H: return KEY_H;
        case GLFW_KEY_I: return KEY_I;
        case GLFW_KEY_J: return KEY_J;
        case GLFW_KEY_K: return KEY_K;
        case GLFW_KEY_L: return KEY_L;
        case GLFW_KEY_M: return KEY_M;
        case GLFW_KEY_N: return KEY_N;
        case GLFW_KEY_O: return KEY_O;
        case GLFW_KEY_P: return KEY_P;
        case GLFW_KEY_Q: return KEY_Q;
        case GLFW_KEY_R: return KEY_R;
        case GLFW_KEY_S: return KEY_S;
        case GLFW_KEY_T: return KEY_T;
        case GLFW_KEY_U: return KEY_U;
        case GLFW_KEY_V: return KEY_V;
        case GLFW_KEY_W: return KEY_W;
        case GLFW_KEY_X: return KEY_X;
        case GLFW_KEY_Y: return KEY_Y;
        case GLFW_KEY_Z: return KEY_Z;

        // Modifiers & special keys
        case GLFW_KEY_SPACE: return KEY_SPACE;
        case GLFW_KEY_ENTER: return KEY_ENTER;
        case GLFW_KEY_ESCAPE: return KEY_ESC;
        case GLFW_KEY_INSERT: return KEY_INSERT;
        case GLFW_KEY_DELETE: return KEY_DELETE;
        case GLFW_KEY_TAB: return KEY_TAB;
        case GLFW_KEY_BACKSPACE: return KEY_BACKSPACE;
        case GLFW_KEY_CAPS_LOCK: return KEY_CAPSLOCK;
        case GLFW_KEY_LEFT_SHIFT: return KEY_LEFTSHIFT;
        case GLFW_KEY_RIGHT_SHIFT: return KEY_RIGHTSHIFT;
        case GLFW_KEY_LEFT_CONTROL: return KEY_LEFTCTRL;
        case GLFW_KEY_RIGHT_CONTROL: return KEY_RIGHTCTRL;
        case GLFW_KEY_LEFT_ALT: return KEY_LEFTALT;
        case GLFW_KEY_RIGHT_ALT: return KEY_RIGHTALT;

        // Numpad / Keypad keys
        case GLFW_KEY_KP_0: return KEY_KP0;
        case GLFW_KEY_KP_1: return KEY_KP1;
        case GLFW_KEY_KP_2: return KEY_KP2;
        case GLFW_KEY_KP_3: return KEY_KP3;
        case GLFW_KEY_KP_4: return KEY_KP4;
        case GLFW_KEY_KP_5: return KEY_KP5;
        case GLFW_KEY_KP_6: return KEY_KP6;
        case GLFW_KEY_KP_7: return KEY_KP7;
        case GLFW_KEY_KP_8: return KEY_KP8;
        case GLFW_KEY_KP_9: return KEY_KP9;
        case GLFW_KEY_KP_DECIMAL: return KEY_KPDOT;
        case GLFW_KEY_KP_DIVIDE: return KEY_KPSLASH;
        case GLFW_KEY_KP_MULTIPLY: return KEY_KPASTERISK;
        case GLFW_KEY_KP_SUBTRACT: return KEY_KPMINUS;
        case GLFW_KEY_KP_ADD: return KEY_KPPLUS;
        case GLFW_KEY_KP_ENTER: return KEY_KPENTER;
        case GLFW_KEY_KP_EQUAL: return KEY_KPEQUAL;

        // Arrows & Navigation
        case GLFW_KEY_UP: return KEY_UP;
        case GLFW_KEY_DOWN: return KEY_DOWN;
        case GLFW_KEY_LEFT: return KEY_LEFT;
        case GLFW_KEY_RIGHT: return KEY_RIGHT;
        case GLFW_KEY_PAGE_UP: return KEY_PAGEUP;
        case GLFW_KEY_PAGE_DOWN: return KEY_PAGEDOWN;
        case GLFW_KEY_HOME: return KEY_HOME;
        case GLFW_KEY_END: return KEY_END;

        // Punctuation & Other Symbol keys
        case GLFW_KEY_GRAVE_ACCENT: return KEY_GRAVE;
        case GLFW_KEY_MINUS: return KEY_MINUS;
        case GLFW_KEY_EQUAL: return KEY_EQUAL;
        case GLFW_KEY_LEFT_BRACKET: return KEY_LEFTBRACE;
        case GLFW_KEY_RIGHT_BRACKET: return KEY_RIGHTBRACE;
        case GLFW_KEY_BACKSLASH: return KEY_BACKSLASH;
        case GLFW_KEY_SEMICOLON: return KEY_SEMICOLON;
        case GLFW_KEY_APOSTROPHE: return KEY_APOSTROPHE;
        case GLFW_KEY_COMMA: return KEY_COMMA;
        case GLFW_KEY_PERIOD: return KEY_DOT;
        case GLFW_KEY_SLASH: return KEY_SLASH;
        default: return -1;
    }
}

void GlobalInputReader::begin_frame()
{
    if (key_state_cache.size() != fds.size()) {
        key_state_cache.resize(fds.size());
    }
    for (size_t i = 0; i < fds.size(); ++i) {
        auto& state = key_state_cache[i];
        std::memset(state.data(), 0, state.size());
        // On ioctl failure the bitmap stays zeroed (key reported unpressed)
        ioctl(fds[i], EVIOCGKEY(state.size()), state.data());
    }
}

bool GlobalInputReader::is_key_pressed(int glfw_key) const
{
    if (glfw_key < 0) return false;

    // Resolve to Linux input codes (stack-local, no heap allocation)
    int linux_keys[2];
    int num_keys = 0;
    if (glfw_key == 1003) {
        linux_keys[0] = BTN_SIDE;
        linux_keys[1] = BTN_BACK;
        num_keys = 2;
    } else if (glfw_key == 1004) {
        linux_keys[0] = BTN_EXTRA;
        linux_keys[1] = BTN_FORWARD;
        num_keys = 2;
    } else {
        int lk = glfw_to_linux_key(glfw_key);
        if (lk >= 0) {
            linux_keys[0] = lk;
            num_keys = 1;
        }
    }

    if (num_keys == 0) return false;

    // Fast path: read from the per-frame snapshot (zero syscalls)
    if (!key_state_cache.empty()) {
        for (const auto& state : key_state_cache) {
            for (int k = 0; k < num_keys; ++k) {
                int lk = linux_keys[k];
                if (lk >= 0 && lk < KEY_MAX) {
                    if ((state[lk / 8] & (1 << (lk % 8))) != 0) {
                        return true;
                    }
                }
            }
        }
        return false;
    }

    // Fallback: live query if begin_frame() was never called
    unsigned char key_state[KEY_MAX / 8 + 1];
    for (int fd : fds) {
        std::memset(key_state, 0, sizeof(key_state));
        if (ioctl(fd, EVIOCGKEY(sizeof(key_state)), key_state) >= 0) {
            for (int k = 0; k < num_keys; ++k) {
                int lk = linux_keys[k];
                if (lk >= 0 && lk < KEY_MAX) {
                    if ((key_state[lk / 8] & (1 << (lk % 8))) != 0) {
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

