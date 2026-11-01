#pragma once
#include <vector>
#include <glm/glm.hpp>
#include "model_cache.hpp"
#include "vpk/source2.hpp"
#include "VisibilityWorker.hpp"

struct FrameState {
    struct RenderPalette {
        const CachedModel* model = nullptr;
        std::vector<source2::Mat3x4> skinning_palette;
        bool is_visible = true;
        bool is_occluded_by_smoke = false;
        std::vector<glm::vec3> sanitized_bones;
        float distance_alpha = 1.0f;
        float highlight_alpha[3] = {1.0f, 1.0f, 1.0f};
        bool highlight_tint[3] = {false, false, false};
        float highlight_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        bool hidden(int category) const { return highlight_alpha[category] <= 0.0f; }
        bool highlighted(int category) const { return highlight_tint[category] || highlight_alpha[category] < 1.0f; }
        const float* tint(int category) const { return highlight_tint[category] ? highlight_color : nullptr; }
    };

    std::vector<RenderPalette> render_palettes;
    std::vector<int> player_ubo_slots;
    std::vector<int> sorted_player_indices;
    std::vector<float> dummy_vis;
    VischeckResult vischeck_result;
};
