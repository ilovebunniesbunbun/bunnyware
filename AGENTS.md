# Bunnyware CS2

High-performance GPU-skinned player chams overlay for Counter-Strike 2 on Linux, rendering via Dear ImGui over a transparent X11/Wayland overlay.

## Quick Reference

- **Package Manager:** pacman (Arch Linux)
- **Build:** `cmake -B build -S . && make -j$(nproc) -C build`
- **Test (C++ unit tests):** `cmake -B build -S . -DBUNNYWARE_BUILD_TESTS=ON && make -j$(nproc) -C build && ctest --test-dir build --output-on-failure`
- **Config files:** `configs/*.json` beside the executable (`configs/_settings.json` stores the startup default and menu appearance; override the path with the `BUNNYWARE_CONFIG` environment variable)

## Feature Flow

Every feature lands through the same short loop so the codebase stays consistent:

1. **Scope** the change against the existing feature cards and config macros.
2. **Implement** it in the smallest surface that fits the architecture (see the rules below).
3. **Wire the UI** — new settings get a card, a tooltip, a search entry and a default in `assets/config/default.json`.
4. **Guard it** with a unit test only when it protects real, non-obvious behavior (see *Tests Are Opt-In*).
5. **Build and test** with `-DBUNNYWARE_BUILD_TESTS=ON` before opening a pull request.

Keep commits focused and describe *why* a change was made.

## Development Guidelines & Rules

### 1. Memory and IPC Alignment
- **Rule**: Whenever you modify shared memory structures (`ShmPacket`, `PlayerData`, etc.) in [game_reader.hpp](src/overlay/game_reader.hpp), keep the packed layout in sync with the size assertions in C++ [test_main.cpp](tests/cpp/test_main.cpp) (`#pragma pack(push, 1)`).

### 2. ImGui Text & Coordinate Rendering
- **Rule**: To prevent text and lines from appearing blurry in the transparent overlay:
  - Render the overlay pixel font at its native rasterized size (e.g. `10.0f`).
  - Pass `ImFontConfig` settings with `OversampleH=1`, `OversampleV=1`, and `PixelSnapH=true` when loading the font.
  - Round all drawing coordinates (`x`, `y` positions) using `std::round` before passing them to ImGui draw list calls to prevent subpixel interpolation.

### 3. Settings Menu UX & Layout
- **Rule**: Group all related features under dedicated title boxes.
- When a parent visual feature is disabled (e.g. box, chams, flags), all its configuration settings, sliders, and color pickers must collapse or hide to keep the settings menu clean.
- Only the master "Enable" checkboxes should remain visible when disabled.

### 4. SVG Silhouette & Drawing
- **Rule**: Weapon or item SVG silhouettes rendered on the ESP overlay must:
  - Be aspect-ratio scaled dynamically based on distance/bounding box dimensions (clamp between minimum and maximum bounds like 24px and 64px).
  - Include a 4-way offset black shadow/outline to ensure readability against dynamic 3D environments.

### 5. Running and Privileges
- **Rule**: Direct memory reading (`process_vm_readv`) and virtual input simulation (`/dev/input/event*` via evdev) require root/sudo access. When writing tests, automation scripts, or running the binary, make sure the user/process has adequate permissions.

### 6. Tests Are Opt-In
- **Rule**: Don't add tests by default. Add one only when it guards real, non-obvious behavior, and extend the existing single-file C++ harness (`tests/cpp/test_main.cpp` + an `add_test` entry in `CMakeLists.txt`) rather than introducing anything new.
- **Justified**: pure logic, parsers, and contracts that are hard to verify by hand (offset scanning, packed struct layout, config migration), or a regression test tied to a specific bug fix.
- **Not justified — do not add**: end-to-end tests that launch the built binary; tests that assert on log/stdout strings; tests that compile source at runtime; anything requiring a display, root, CS2, or the network; trivial getter/setter or "smoke" tests; and new test frameworks, harnesses, or non-C++ test suites.
- If you do add a test, state *why* in the commit message.
