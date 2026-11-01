# Bunnyware CS2

When Constelia.ai/Fantasy.cat closed its doors completely back in the earlier half of this year, I saw the writing on the wall, and that it was close to being shutdown. I decided to start working on my own CS2 External Cheat to replace it and this is my version of it.

**Contributors:**
Credits to these contributors, which helped work on bunnyware
- [@remcostoeten](https://github.com/remcostoeten)
- [@anxiolytique](https://github.com/anxiolytique)
- [@lizzie731](https://github.com/lizzie731)
- [@Chopper1337](https://github.com/Chopper1337)
- [@thenameskayla](https://github.com/thenameskayla)

Credits to [@Moyomo](https://github.com/Moyomo) and [@vacmeharder](https://github.com/vacmeharder) for their work on the radar (FC2Observ) thats implemented into this project

![alt text](https://i.imgur.com/wlBHMOl.png)

## Prerequisites & Dependencies

- **Operating system**: Linux (developed on Arch-based distributions; X11 or Wayland with XWayland).
- **Privileges**: Root/`sudo` — required to open `/dev/uinput` for input simulation and to read process memory via `process_vm_readv` when `ptrace_scope` restrictions are active.
- **Compiler / build**: A C++20 compiler (GCC 10+ or Clang 11+), CMake 3.10+, and `pkgconf`.
- **Graphics**: A driver exposing an OpenGL 3.3 compatibility profile.

Install the toolchain and libraries on Arch Linux:

```bash
sudo pacman -S base-devel cmake pkgconf glfw glew glm spdlog dbus \
    wayland wayland-protocols mesa libx11 libxext libxrandr nlohmann-json
```

> The build also needs the `fractional-scale-v1` and `viewporter` Wayland protocol XMLs, which ship with an up-to-date `wayland-protocols` package.

---

## Build

From the repository root:

```bash
cmake -B build -S .
make -j$(nproc) -C build
```

The binary is written to **`build/bunnyware`**. The build automatically copies `assets/` and `offsets.json` into the build directory next to the executable.

A convenience wrapper is also provided:

```bash
./build.sh            # configure + build (tests enabled)
./build.sh -s         # build, then start the overlay via sudo
./build.sh -s -- --x11
./build.sh -c -d -s   # clean debug build, then start
```

Run `./build.sh --help` for all options.

### Tests

The C++ unit tests are opt-in:

```bash
cmake -B build -S . -DBUNNYWARE_BUILD_TESTS=ON
make -j$(nproc) -C build
ctest --test-dir build --output-on-failure
```

---

## Usage

### Live mode

1. Launch **Counter-Strike 2**.
2. Run the utility as root (needed for `/dev/uinput` and process memory reads):

   ```bash
   sudo ./build/bunnyware
   ```

   Force the X11/XWayland overlay backend instead of Wayland layer-shell (useful when the compositor lacks layer-shell support, or for troubleshooting):

   ```bash
   sudo ./build/bunnyware --x11
   ```

   Refresh `offsets.json` (and the synced `build/offsets.json`) from the live game and exit, without starting the overlay:

   ```bash
   sudo ./build/bunnyware --update-offsets
   # alias: --refresh-offsets
   ```

   Validate a config file and exit:

   ```bash
   sudo ./build/bunnyware --validate-config [path/to/config.json]
   ```

3. Configure everything from the Dear ImGui settings window — visuals, chams, aimbot, triggerbot, colors, and more. Use the **Config** tab to create, load, save, set-default, or delete profiles stored in the `configs/` folder beside the binary.

On every normal launch the utility first refreshes the offsets it consumes (signature-scanned globals, the `ICvar` interface fallback, and the schema fields used by the reader) straight from the live game. If a signature breaks, the failure is logged and the existing `offsets.json` is kept.

---

## Configuration

- Config profiles are JSON files in a **`configs/`** directory **beside the executable** (for a build tree this is `build/configs/`). If that location is not writable, the utility falls back to `$XDG_CONFIG_HOME/bunnyware/configs` or `~/.config/bunnyware/configs`.
- The startup default and menu appearance live in **`configs/_settings.json`**.
- A read-only baseline of every setting is kept at **`assets/config/default.json`**.
- Override the config file path with the **`BUNNYWARE_CONFIG`** environment variable, and the configs directory with **`BUNNYWARE_CONFIG_DIR`**.

---

## Hyprland Configuration

When running under **Hyprland**, enable **Hyprland Compatibility Mode** in the overlay settings. Because wlroots does not forward input coordinates through override-redirect windows the way X11 does, add the following window rule to keep the overlay floating on top, click-through, and correctly aligned.

For Hyprland 0.55 and above, add this to `~/.config/hypr/hyprland.lua`:

```lua
-- Hyprland 0.55 and above
hl.window_rule({
    match = { class = "^(bunnyware.overlay)$" },
    float = true,
    pin = true,
    size = "100% 100%",
    move = "0 0",
    no_focus = true,
    no_blur = true,
    no_shadow = true,
    animation = "none",
    border_size = 0,
    suppress_event = "maximize",
})
```

<details>
<summary><strong>Legacy Hyprlang configuration (Hyprland 0.54 or lower)</strong></summary>

> [!CAUTION]
> The configuration below is outdated as of Hyprland 0.55, where Lua replaced Hyprlang. On up-to-date systems, use the Lua rule above instead.

Add the following to your `hyprland.conf` (choose the format matching your Hyprland version):

**Modern syntax (`windowrulev2` — recommended)**

```hyprland
# Align and float the overlay
windowrulev2 = float, class:^(bunnyware.overlay)$
windowrulev2 = pin, class:^(bunnyware.overlay)$
windowrulev2 = size 100% 100%, class:^(bunnyware.overlay)$
windowrulev2 = move 0 0, class:^(bunnyware.overlay)$

# Make it non-interactive and cosmetic
windowrulev2 = nofocus, class:^(bunnyware.overlay)$
windowrulev2 = noblur, class:^(bunnyware.overlay)$
windowrulev2 = noshadow, class:^(bunnyware.overlay)$
windowrulev2 = noanim, class:^(bunnyware.overlay)$
windowrulev2 = borderwidth 0, class:^(bunnyware.overlay)$
windowrulev2 = suppressevent maximize, class:^(bunnyware.overlay)$
```

**Classic syntax (`windowrule`)**

```hyprland
windowrule {
    name = bunnywareoverlay
    match:class = ^(bunnyware.overlay)$
    float = on           # REQUIRED — keep it out of the tiling layout
    pin = on             # REQUIRED — stay on top / visible across workspaces
    no_focus = on        # REQUIRED — don't steal focus from CS2 when it maps
    no_blur = true       # cosmetic — no blur on the transparent surface
    no_shadow = true     # cosmetic — no drop shadow
    border_size = 0      # REQUIRED-ish — no border drawn over the game
    no_anim = true       # cosmetic — no open/close animation
    suppress_event = maximize  # safety — ignore maximize requests
}
```

</details>

---

## Radar

Bunnyware CS2 includes an **embedded web radar**: the binary runs its own HTTP and WebSocket servers and serves a browser-based radar frontend showing player positions, the bomb, smokes, and projectiles. Enable it in the **Radar** tab, set the bind host (`127.0.0.1` for local-only or `0.0.0.0` to allow other devices), and open the configured HTTP port in any browser. Because it is a web page, it can also be used as a transparent browser source in OBS.

The radar frontend is a C++ re-implementation inspired by **[FC2observ](https://gitlab.com/vacmeharder/fc2observ)**, and its bundled web assets are derived from that project. Those assets are licensed under **GPL-3.0** and carry their attribution in `assets/radar/` (see `LICENSE.FC2observ` and `README.FC2observ.md`); the radar map images retain their own MIT licenses.

---

## Troubleshooting

- **"Permission denied" / no input or no ESP data** — run as root (`sudo`). `/dev/uinput` and `process_vm_readv` both require elevated privileges.
- **Overlay does not appear on Wayland** — the compositor may not support `wlr-layer-shell`. Try `sudo ./build/bunnyware --x11` to force the X11/XWayland backend.
- **Broken signatures or stale data after a game update** — run `sudo ./build/bunnyware --update-offsets` to re-scan `offsets.json` from the live game.
- **Settings not persisting** — ensure the `configs/` directory beside the binary is writable, or point `BUNNYWARE_CONFIG_DIR` at a writable location.

---

## License

Bunnyware CS2 is licensed under the **GNU General Public License v3.0** — see [LICENSE](LICENSE). It bundles or links against third-party components under their own licenses; the full list of attributions is in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

---

## Credits & Attribution

Bunnyware CS2 builds on and bundles work from several open-source projects:

- **Dear ImGui**, **meshoptimizer**, **NanoSVG**, and **zstd** are vendored under `src/external/`.
- **Lucide** icons are used for the UI (ISC).
- The **web radar** frontend and assets are derived from **FC2observ** (GPL-3.0), with map images under MIT.
- Radar assets and tooling: thanks to **[@typedef-FC](https://github.com/typedef-FC)**.
- The build links against system libraries including **GLFW**, **GLEW**, **GLM**, **spdlog**, **nlohmann/json**, **OpenGL**, **X11/Xext**, **Wayland/Wayland-EGL**, and **D-Bus**.

See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for the complete attribution and license details.
