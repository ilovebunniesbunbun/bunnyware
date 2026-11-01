# Bunnyware CS2

**Bunnyware CS2** is a high-performance external utility for **Counter-Strike 2 on Linux**. It renders a transparent, GPU-skinned player chams and ESP overlay with OpenGL 3.3 and Dear ImGui, driven by a multi-threaded visibility engine over direct `process_vm_readv` memory reads. Alongside the visuals it ships a per-weapon **Legit Aimbot**, a seed-aware hitchance **Triggerbot**, movement helpers, a grenade helper/solver, an embedded web radar, and a `/dev/uinput`-based input simulation layer.

> [!NOTE]
> This project is intended for educational and research purposes only. Using it on Valve matchmaking servers violates the Steam Subscriber Agreement and can result in a permanent ban. You are responsible for how you use it.

---

## Features

### Rendering & Visuals
- **Skinned 3D player chams** — separate visible and hidden/occluded materials with flat/solid, metallic, Fresnel, glow-blend and pulsing shaders, plus optional secondary colors.
- **Glow / stencil outline** — selectable style and thickness, health-based coloring, and pulsing.
- **Player ESP** — bounding boxes, HP/armor/ammo bars, skeletons, head circles, names, status flags, weapon icons, hitmarkers, and off-screen arrows, with independent visible/occluded filtering, distance limiting, and hit reveals.
- **Occlusion engine** — a GPU depth prepass for pixel-perfect wall occlusion with a multi-threaded CPU BVH raycast fallback.
- **DamageFX** — configurable hit effects rendered as chams, particles, or a pulse.
- **Sound ESP** — directional proximity cues for nearby enemies.
- **Crosshair tools** — a configurable crosshair overlay with recoil expansion and unscoped-sniper support, crosshair ESP, spread circle, and crosshair-to-point distance readout.
- **Bullet impacts** — replays the game's own impact positions, including recoil and spread.
- **Noise ESP** — visualizes recent movement and action events around players.

### Combat
- **Legit Aimbot** — per weapon-category FOV, smoothing curves (constant / slow-end / fast-end / progressive), randomized smoothing, horizontal bias, movement prediction, overshoot humanization, a drawable FOV circle, and recoil control (RCS) with per-axis scale and smoothing.
- **Triggerbot** — `Simple`, `Seeded`, `Hitchance`, and `Both` trigger modes; crosshair / crosshair+hitbox / hitbox auto-fire validation modes; seeded and noscope hitchance thresholds; configurable fire and between-shot delays; magnet trigger; selectable hitbox groups with scaling; smoke and visibility checks; and fire-on-target-lost.

### Movement
- **Edge Jump** — automatic jump on ledge detection with crouch-on-jump, knife-only, strafe assist, and tunable detection distances, tolerances, and delays.
- **Counter-Strafe Coach** — an on-screen speed graph and feedback overlay for counter-strafing practice.

### Grenades
- **Grenade Helper** — lineup guidance with trace lines, visibility checks, automatic aim, and automatic throw.
- **Grenade Solver** — solves throws and displays landing zones.
- **Grenade Creator** — records and edits local lineups.
- **Trajectories** — predicted grenade paths with bounce markers, detonation radius, and fade timing.
- **Grenade timers/warnings** and **inferno spread** visualization.

### World
- **Bomb bar** and **world-space C4 timer**, plus **dropped item ESP**.
- **Anti Flash** — replaces the flash effect with a black screen, an optional map collision wireframe, a center crosshair dot, and optional forced chams.
- **Smoke wireframe** and **through-smokes** rendering of map geometry clipped to active smoke shapes.

### Radar
- **Embedded web radar** served over HTTP + WebSocket and viewable in any browser — see [Radar](#radar).

### Menu & Configuration
- Dear ImGui settings window with vertical tabs (**Combat, Players, World, Grenades, Misc, Radar, Config, Debug**) and a fuzzy settings search.
- Named configuration profiles (save / load / set-default / delete) and a themeable menu appearance.
- On-screen keybinds viewer, spectator list, and fully rebindable input keybinds.

### Environment Integrations
- Wayland **layer-shell** backend with an X11/XWayland fallback (`--x11`).
- Hyprland window-rule support through the `bunnyware.overlay` class.
- Display scaling / aspect-ratio mapping, GPU selection, and `/dev/uinput` virtual input simulation.
- Auto-accept for match and ready prompts.
- VPK parser and map-geometry extraction.

---

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
- The build links against system libraries including **GLFW**, **GLEW**, **GLM**, **spdlog**, **nlohmann/json**, **OpenGL**, **X11/Xext**, **Wayland/Wayland-EGL**, and **D-Bus**.

See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for the complete attribution and license details.
