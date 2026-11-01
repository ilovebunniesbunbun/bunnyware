# Third-Party Notices

Bunnyware CS2 is licensed under the GNU General Public License v3.0 (see
[LICENSE](LICENSE)). It bundles or links against the following third-party
components. Each remains under its own license; the notices below are provided
for attribution and license-compliance purposes.

## Vendored source (`src/external/`)

| Component | License | Upstream |
| --- | --- | --- |
| Dear ImGui | MIT | https://github.com/ocornut/imgui |
| meshoptimizer | MIT | https://github.com/zeux/meshoptimizer |
| NanoSVG | zlib | https://github.com/memononen/nanosvg |
| zstd | BSD-3-Clause OR GPL-2.0 (dual) | https://github.com/facebook/zstd |

## Bundled assets

| Asset | License | Notes |
| --- | --- | --- |
| Lucide icons (`assets/icons/ui/lucide/`) | ISC | See `assets/icons/ui/lucide/LICENSE` |
| FC2observ radar assets (`assets/radar/`) | GPL-3.0 | Derived from [FC2observ](https://gitlab.com/vacmeharder/fc2observ); see `assets/radar/LICENSE.FC2observ` and `assets/radar/README.FC2observ.md` |
| Radar map images (`assets/radar/maps/*/`) | MIT | See the `LICENSE` file in each map directory |

## System dependencies

The build links against system libraries that are **not** vendored in this
repository and keep their own licenses, including: GLFW, GLEW, GLM, spdlog,
nlohmann/json, OpenGL, X11/Xext, Wayland/Wayland-EGL, and D-Bus.
