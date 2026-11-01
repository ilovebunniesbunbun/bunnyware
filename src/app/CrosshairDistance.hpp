#pragma once

#include <glm/glm.hpp>
#include <optional>

struct ImDrawList;
struct OverlayConfig;
struct FrameInput;
class VisibilityWorker;

class CrosshairDistance {
public:
    void handle_keys(const FrameInput& input, const VisibilityWorker& vis_worker, bool pin_down, bool clear_down);
    void render(ImDrawList* draw_list, const FrameInput& input, const OverlayConfig& cfg,
                const VisibilityWorker& vis_worker, int width, int height);
    void clear_pin() { pin_.reset(); }

private:
    std::optional<glm::vec3> pin_;
    bool pin_was_down_ = false;
    bool clear_was_down_ = false;
};
