#pragma once
#include <vector>
#include <glm/glm.hpp>
#include "../vpk/vmdl/maps/map_parser.hpp"

struct Plane {
    glm::vec3 normal;
    float distance;

    bool is_aabb_in_front(const glm::vec3& mins, const glm::vec3& maxs) const {
        glm::vec3 positive_vertex = mins;
        if (normal.x >= 0) positive_vertex.x = maxs.x;
        if (normal.y >= 0) positive_vertex.y = maxs.y;
        if (normal.z >= 0) positive_vertex.z = maxs.z;

        return glm::dot(normal, positive_vertex) + distance >= 0.0f;
    }
};

struct Frustum {
    Plane planes[6];

    bool intersects_aabb(const glm::vec3& mins, const glm::vec3& maxs) const {
        for (int i = 0; i < 6; ++i) {
            if (!planes[i].is_aabb_in_front(mins, maxs)) {
                return false;
            }
        }
        return true;
    }
};

struct MapChunk {
    glm::vec3 mins;
    glm::vec3 maxs;
    size_t index_offset;
    size_t index_count;
};

class DepthPrepassRenderer {
public:
    DepthPrepassRenderer() = default;
    ~DepthPrepassRenderer();

    bool init();
    void cleanup();

    void upload_geometry(const std::vector<MapParser::Triangle>& triangles);
    void upload_dynamic_geometry(const std::vector<MapParser::Triangle>& triangles);
    // Smoke silhouette shell for the current frame: the wireframe cage shape
    // triangulated. Stamped into the stencil buffer so the smoke's own depth can
    // be refreshed inside the silhouette and the map wireframe drawn through the
    // smoke is clipped to it.
    void upload_smoke_shell(const std::vector<MapParser::Triangle>& triangles);
    void render(const float* view_proj);
    void render_wireframe(const float* view_proj, const float* color, bool depth_tested = true,
                          float line_width = 1.0f);
    void stamp_smoke_silhouette(const float* view_proj);
    // Rebuilds the map-only depth inside the stenciled region (used by the smoke
    // wireframe and through-smokes passes): resets depth to the far plane where
    // the stencil matches, then rasterizes the static map depth with a small fill
    // offset so depth-tested draws reveal the first map surface behind the smoke
    // instead of every surface along the view ray. Expects the caller's stencil
    // test to be active and does not modify stencil state.
    void refresh_depth_in_stencil(const float* view_proj);

    // Frame bookkeeping so callers can request the map depth buffer without
    // duplicating an already-rendered prepass.
    void begin_frame();
    bool has_rendered_this_frame() const { return rendered_this_frame_; }

    bool has_geometry() const { return vertex_count > 0; }
    void clear_geometry();

    bool has_dynamic_geometry() const { return dynamic_vertex_count > 0; }
    void clear_dynamic_geometry();

    bool has_smoke_shell() const { return smoke_vertex_count > 0; }
    void clear_smoke_shell();

private:
    unsigned int program_id = 0;
    unsigned int vertex_shader = 0;
    unsigned int fragment_shader = 0;

    unsigned int wireframe_program_id = 0;
    unsigned int wireframe_fragment_shader = 0;

    unsigned int vao = 0;
    unsigned int vbo = 0;
    unsigned int ebo = 0;
    size_t vertex_count = 0;
    size_t index_count = 0;
    std::vector<MapChunk> chunks;

    unsigned int dynamic_vao = 0;
    unsigned int dynamic_vbo = 0;
    size_t dynamic_vertex_count = 0;

    unsigned int smoke_vao = 0;
    unsigned int smoke_vbo = 0;
    size_t smoke_vertex_count = 0;

    unsigned int reset_vao = 0;
    unsigned int reset_vbo = 0;
    bool rendered_this_frame_ = false;

    int loc_view_proj = -1;
    int loc_wf_view_proj = -1;
    int loc_wf_color = -1;

    bool compile_shader(unsigned int shader, const char* source);
    bool link_program();
    bool link_wireframe_program();
};
