#pragma once
#include "app/FrameInput.hpp"
#include "app/FrameState.hpp"
#include "config.hpp"
#include "model_cache.hpp"
#include "renderer/gpu_chams.hpp"
#include "renderer/depth_prepass.hpp"
#include "renderer/esp_renderer.hpp"
#include "renderer/gpu_profiler.hpp"
#include "vpk/source2.hpp"

struct DeadPlayerCham {
    unsigned int vao = 0;
    unsigned int ibo = 0;
    size_t index_count = 0;
    std::vector<source2::Mat3x4> skinning_palette;
    Vec3 position{};
    double kill_time = 0.0;
    float color[4];
    float secondary_color[4];
    int style_id = 0;
    float fade_time = 1.0f;
    mutable int assigned_ubo_slot = -1;
};

struct ChamsPassInputs {
    const FrameInput& input;
    const FrameState& state;
    const OverlayConfig& cfg;
    GpuChamsRenderer& chams_renderer;
    DepthPrepassRenderer& depth_prepass;
    EspRenderer& esp_renderer;
    GpuProfiler* profiler;
    ModelCache& model_cache;
    int window_width;
    int window_height;
    const std::vector<DeadPlayerCham>& active_kill_chams;
    const std::vector<DeadPlayerCham>& active_hit_chams;
};

class ChamsPass {
public:
    static void run(const ChamsPassInputs& inputs);
};
