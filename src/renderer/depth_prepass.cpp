#include "depth_prepass.hpp"
#include "gl_loader.hpp"
#include "logger.hpp"
#include "meshopt/Meshoptimizer.h"

static const char* depth_vertex_shader = R"glsl(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in float aTypeID;
uniform mat4 uViewProj;
out float vTypeID;
void main() {
    vTypeID = aTypeID;
    gl_Position = uViewProj * vec4(aPos, 1.0);
}
)glsl";

static const char* depth_fragment_shader = R"glsl(
#version 330 core
in float vTypeID;
void main() {
    if (vTypeID < -0.001) {
        float alpha = -vTypeID;
        if (alpha < (15.0 / 255.0)) {
            discard;
        }
        // Shift depth relative to distance from far plane: prevents clipping/clamping at 1.0
        gl_FragDepth = gl_FragCoord.z + (1.0 - gl_FragCoord.z) * (1.0 - alpha) * 0.1;
    } else {
        gl_FragDepth = gl_FragCoord.z;
    }
}
)glsl";

static const char* wireframe_fragment_shader_src = R"glsl(
#version 330 core
out vec4 FragColor;
uniform vec4 uColor;
in float vTypeID;
void main() {
    vec4 finalColor = uColor;
    if (vTypeID < -0.001) {
        discard;
    } else if (vTypeID > 0.5 && vTypeID < 1.5) {
        finalColor = vec4(1.0, 0.0, 0.0, uColor.a); // Breakables (Red)
    } else if (vTypeID > 1.5 && vTypeID < 2.5) {
        finalColor = vec4(0.0, 1.0, 0.0, uColor.a); // Physics props (Green)
    } else if (vTypeID > 2.5 && vTypeID < 3.5) {
        finalColor = vec4(0.0, 0.0, 1.0, uColor.a); // Func Brush (Blue)
    } else if (vTypeID > 3.5 && vTypeID < 4.5) {
        finalColor = vec4(1.0, 0.0, 1.0, uColor.a); // Func Clip VPhysics (Magenta)
    } else if (vTypeID > 4.5 && vTypeID < 5.5) {
        finalColor = vec4(0.0, 1.0, 1.0, uColor.a); // Func Physbox (Cyan)
    } else if (vTypeID > 6.5 && vTypeID < 7.5) {
        finalColor = vec4(1.0, 0.5, 0.0, uColor.a); // Doors (Orange)
    }
    FragColor = finalColor;
}
)glsl";

DepthPrepassRenderer::~DepthPrepassRenderer() {
    cleanup();
}

void DepthPrepassRenderer::cleanup() {
    clear_geometry();
    if (dynamic_vbo) {
        glDeleteBuffers(1, &dynamic_vbo);
        dynamic_vbo = 0;
    }
    if (dynamic_vao) {
        glDeleteVertexArrays(1, &dynamic_vao);
        dynamic_vao = 0;
    }
    dynamic_vertex_count = 0;

    if (smoke_vbo) {
        glDeleteBuffers(1, &smoke_vbo);
        smoke_vbo = 0;
    }
    if (smoke_vao) {
        glDeleteVertexArrays(1, &smoke_vao);
        smoke_vao = 0;
    }
    smoke_vertex_count = 0;

    if (reset_vbo) {
        glDeleteBuffers(1, &reset_vbo);
        reset_vbo = 0;
    }
    if (reset_vao) {
        glDeleteVertexArrays(1, &reset_vao);
        reset_vao = 0;
    }

    if (program_id) {
        glDeleteProgram(program_id);
        program_id = 0;
    }
    if (wireframe_program_id) {
        glDeleteProgram(wireframe_program_id);
        wireframe_program_id = 0;
    }
    if (vertex_shader) {
        glDeleteShader(vertex_shader);
        vertex_shader = 0;
    }
    if (fragment_shader) {
        glDeleteShader(fragment_shader);
        fragment_shader = 0;
    }
    if (wireframe_fragment_shader) {
        glDeleteShader(wireframe_fragment_shader);
        wireframe_fragment_shader = 0;
    }
}

static Frustum extract_frustum(const float* vp) {
    Frustum frustum;

    // Left Plane: Row 3 + Row 0
    frustum.planes[0].normal.x = vp[3] + vp[0];
    frustum.planes[0].normal.y = vp[7] + vp[4];
    frustum.planes[0].normal.z = vp[11] + vp[8];
    frustum.planes[0].distance = vp[15] + vp[12];

    // Right Plane: Row 3 - Row 0
    frustum.planes[1].normal.x = vp[3] - vp[0];
    frustum.planes[1].normal.y = vp[7] - vp[4];
    frustum.planes[1].normal.z = vp[11] - vp[8];
    frustum.planes[1].distance = vp[15] - vp[12];

    // Bottom Plane: Row 3 + Row 1
    frustum.planes[2].normal.x = vp[3] + vp[1];
    frustum.planes[2].normal.y = vp[7] + vp[5];
    frustum.planes[2].normal.z = vp[11] + vp[9];
    frustum.planes[2].distance = vp[15] + vp[13];

    // Top Plane: Row 3 - Row 1
    frustum.planes[3].normal.x = vp[3] - vp[1];
    frustum.planes[3].normal.y = vp[7] - vp[5];
    frustum.planes[3].normal.z = vp[11] - vp[9];
    frustum.planes[3].distance = vp[15] - vp[13];

    // Near Plane: Row 3 + Row 2
    frustum.planes[4].normal.x = vp[3] + vp[2];
    frustum.planes[4].normal.y = vp[7] + vp[6];
    frustum.planes[4].normal.z = vp[11] + vp[10];
    frustum.planes[4].distance = vp[15] + vp[14];

    // Far Plane: Row 3 - Row 2
    frustum.planes[5].normal.x = vp[3] - vp[2];
    frustum.planes[5].normal.y = vp[7] - vp[6];
    frustum.planes[5].normal.z = vp[11] - vp[10];
    frustum.planes[5].distance = vp[15] - vp[14];

    // Normalize planes
    for (int i = 0; i < 6; ++i) {
        float length = glm::length(frustum.planes[i].normal);
        if (length > 0.0f) {
            frustum.planes[i].normal /= length;
            frustum.planes[i].distance /= length;
        }
    }

    return frustum;
}

void DepthPrepassRenderer::clear_geometry() {
    if (vbo) {
        glDeleteBuffers(1, &vbo);
        vbo = 0;
    }
    if (ebo) {
        glDeleteBuffers(1, &ebo);
        ebo = 0;
    }
    if (vao) {
        glDeleteVertexArrays(1, &vao);
        vao = 0;
    }
    vertex_count = 0;
    index_count = 0;
    chunks.clear();
}

void DepthPrepassRenderer::clear_dynamic_geometry() {
    dynamic_vertex_count = 0;
}

bool DepthPrepassRenderer::compile_shader(unsigned int shader, const char* source) {
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    int success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char info_log[512];
        glGetShaderInfoLog(shader, 512, nullptr, info_log);
        BUNNYWARE_LOG_ERROR("DEPTH_PREPASS: Shader compilation error: {}", info_log);
        return false;
    }
    return true;
}

bool DepthPrepassRenderer::link_program() {
    program_id = glCreateProgram();
    glAttachShader(program_id, vertex_shader);
    glAttachShader(program_id, fragment_shader);
    glLinkProgram(program_id);

    int success;
    glGetProgramiv(program_id, GL_LINK_STATUS, &success);
    if (!success) {
        char info_log[512];
        glGetProgramInfoLog(program_id, 512, nullptr, info_log);
        BUNNYWARE_LOG_ERROR("DEPTH_PREPASS: Shader linking error: {}", info_log);
        return false;
    }
    return true;
}

bool DepthPrepassRenderer::link_wireframe_program() {
    wireframe_program_id = glCreateProgram();
    glAttachShader(wireframe_program_id, vertex_shader);
    glAttachShader(wireframe_program_id, wireframe_fragment_shader);
    glLinkProgram(wireframe_program_id);

    int success;
    glGetProgramiv(wireframe_program_id, GL_LINK_STATUS, &success);
    if (!success) {
        char info_log[512];
        glGetProgramInfoLog(wireframe_program_id, 512, nullptr, info_log);
        BUNNYWARE_LOG_ERROR("MAP_VISUALIZER: Shader linking error: {}", info_log);
        return false;
    }
    return true;
}

bool DepthPrepassRenderer::init() {
    vertex_shader = glCreateShader(GL_VERTEX_SHADER);
    if (!compile_shader(vertex_shader, depth_vertex_shader)) return false;

    fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);
    if (!compile_shader(fragment_shader, depth_fragment_shader)) return false;

    if (!link_program()) return false;

    loc_view_proj = glGetUniformLocation(program_id, "uViewProj");

    // Initialize map visualizer shaders
    wireframe_fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);
    if (!compile_shader(wireframe_fragment_shader, wireframe_fragment_shader_src)) return false;

    if (!link_wireframe_program()) return false;

    loc_wf_view_proj = glGetUniformLocation(wireframe_program_id, "uViewProj");
    loc_wf_color = glGetUniformLocation(wireframe_program_id, "uColor");

    // Initialize dynamic geometry VAO & VBO once
    glGenVertexArrays(1, &dynamic_vao);
    glGenBuffers(1, &dynamic_vbo);

    glBindVertexArray(dynamic_vao);
    glBindBuffer(GL_ARRAY_BUFFER, dynamic_vbo);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(MapParser::Vertex), (void*)offsetof(MapParser::Vertex, pos));

    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, sizeof(MapParser::Vertex), (void*)offsetof(MapParser::Vertex, type_id));

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    // Initialize smoke voxel geometry VAO & VBO (through-smokes stencil marks)
    glGenVertexArrays(1, &smoke_vao);
    glGenBuffers(1, &smoke_vbo);

    glBindVertexArray(smoke_vao);
    glBindBuffer(GL_ARRAY_BUFFER, smoke_vbo);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(MapParser::Vertex), (void*)offsetof(MapParser::Vertex, pos));

    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, sizeof(MapParser::Vertex), (void*)offsetof(MapParser::Vertex, type_id));

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    // Fullscreen triangle used to reset depth inside stenciled regions. It rides
    // the depth program with an identity transform and z = 1 (far plane); the
    // fragment shader keeps it because its type id is 0.
    const float reset_triangle[] = {
        -1.0f, -1.0f, 1.0f, 0.0f,
        3.0f,  -1.0f, 1.0f, 0.0f,
        -1.0f, 3.0f,  1.0f, 0.0f,
    };
    glGenVertexArrays(1, &reset_vao);
    glGenBuffers(1, &reset_vbo);
    glBindVertexArray(reset_vao);
    glBindBuffer(GL_ARRAY_BUFFER, reset_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(reset_triangle), reset_triangle, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(3 * sizeof(float)));
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    return true;
}

void DepthPrepassRenderer::upload_geometry(const std::vector<MapParser::Triangle>& triangles) {
    clear_geometry();
    if (triangles.empty()) return;

    // 1. Calculate map bounds
    glm::vec3 map_mins{1e9f}, map_maxs{-1e9f};
    for (const auto& tri : triangles) {
        glm::vec3 v0(tri.v0.pos.x, tri.v0.pos.y, tri.v0.pos.z);
        glm::vec3 v1(tri.v1.pos.x, tri.v1.pos.y, tri.v1.pos.z);
        glm::vec3 v2(tri.v2.pos.x, tri.v2.pos.y, tri.v2.pos.z);
        map_mins = glm::min(map_mins, glm::min(v0, glm::min(v1, v2)));
        map_maxs = glm::max(map_maxs, glm::max(v0, glm::max(v1, v2)));
    }

    // 2. Define a 3D grid of chunks (4 x 4 x 2 = 32 chunks)
    const int Nx = 4;
    const int Ny = 4;
    const int Nz = 2;
    const int total_cells = Nx * Ny * Nz;

    glm::vec3 size = map_maxs - map_mins;
    if (size.x < 1.0f) size.x = 1.0f;
    if (size.y < 1.0f) size.y = 1.0f;
    if (size.z < 1.0f) size.z = 1.0f;

    std::vector<std::vector<MapParser::Triangle>> cell_triangles(total_cells);

    for (const auto& tri : triangles) {
        // Find centroid
        float cx = (tri.v0.pos.x + tri.v1.pos.x + tri.v2.pos.x) / 3.0f;
        float cy = (tri.v0.pos.y + tri.v1.pos.y + tri.v2.pos.y) / 3.0f;
        float cz = (tri.v0.pos.z + tri.v1.pos.z + tri.v2.pos.z) / 3.0f;

        int ix = std::clamp(static_cast<int>((cx - map_mins.x) / size.x * Nx), 0, Nx - 1);
        int iy = std::clamp(static_cast<int>((cy - map_mins.y) / size.y * Ny), 0, Ny - 1);
        int iz = std::clamp(static_cast<int>((cz - map_mins.z) / size.z * Nz), 0, Nz - 1);

        int cell_idx = ix + iy * Nx + iz * Nx * Ny;
        cell_triangles[cell_idx].push_back(tri);
    }

    // 3. Process and optimize each non-empty chunk
    std::vector<MapParser::Vertex> global_vertices;
    std::vector<unsigned int> global_indices;
    chunks.clear();

    for (int cell_idx = 0; cell_idx < total_cells; ++cell_idx) {
        const auto& cell_tris = cell_triangles[cell_idx];
        if (cell_tris.empty()) continue;

        size_t cell_src_vertex_count = cell_tris.size() * 3;
        const MapParser::Vertex* cell_src_vertices = reinterpret_cast<const MapParser::Vertex*>(cell_tris.data());

        // Generate remap table for vertex deduplication in this chunk
        std::vector<unsigned int> remap(cell_src_vertex_count);
        size_t unique_vertex_count = meshopt_generateVertexRemap(
            remap.data(),
            nullptr, // unindexed input
            cell_src_vertex_count,
            cell_src_vertices,
            cell_src_vertex_count,
            sizeof(MapParser::Vertex)
        );

        // Remap vertices and build initial index buffer
        std::vector<MapParser::Vertex> unique_vertices(unique_vertex_count);
        std::vector<unsigned int> unique_indices(cell_src_vertex_count);

        meshopt_remapVertexBuffer(
            unique_vertices.data(),
            cell_src_vertices,
            cell_src_vertex_count,
            sizeof(MapParser::Vertex),
            remap.data()
        );

        meshopt_remapIndexBuffer(
            unique_indices.data(),
            nullptr, // unindexed input
            cell_src_vertex_count,
            remap.data()
        );

        // Reorder indices to maximize post-transform vertex cache hit rate
        std::vector<unsigned int> optimized_indices(cell_src_vertex_count);
        meshopt_optimizeVertexCache(
            optimized_indices.data(),
            unique_indices.data(),
            cell_src_vertex_count,
            unique_vertex_count
        );

        // Calculate chunk bounds
        glm::vec3 chunk_mins{1e9f}, chunk_maxs{-1e9f};
        for (const auto& v : unique_vertices) {
            glm::vec3 p(v.pos.x, v.pos.y, v.pos.z);
            chunk_mins = glm::min(chunk_mins, p);
            chunk_maxs = glm::max(chunk_maxs, p);
        }

        // Record chunk info
        MapChunk chunk;
        chunk.mins = chunk_mins;
        chunk.maxs = chunk_maxs;
        chunk.index_offset = global_indices.size();
        chunk.index_count = cell_src_vertex_count;
        chunks.push_back(chunk);

        // Append to global geometry buffers
        size_t base_vertex = global_vertices.size();
        global_vertices.insert(global_vertices.end(), unique_vertices.begin(), unique_vertices.end());

        for (unsigned int idx : optimized_indices) {
            global_indices.push_back(idx + static_cast<unsigned int>(base_vertex));
        }
    }

    vertex_count = global_vertices.size();
    index_count = global_indices.size();

    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glGenBuffers(1, &ebo);

    glBindVertexArray(vao);

    // Upload deduplicated vertices
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, global_vertices.size() * sizeof(MapParser::Vertex), global_vertices.data(), GL_STATIC_DRAW);

    // Upload optimized indices
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, global_indices.size() * sizeof(unsigned int), global_indices.data(), GL_STATIC_DRAW);

    // Attributes configuration
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(MapParser::Vertex), (void*)offsetof(MapParser::Vertex, pos));

    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, sizeof(MapParser::Vertex), (void*)offsetof(MapParser::Vertex, type_id));

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

void DepthPrepassRenderer::upload_dynamic_geometry(const std::vector<MapParser::Triangle>& triangles) {
    dynamic_vertex_count = triangles.size() * 3;
    if (triangles.empty()) return;

    glBindBuffer(GL_ARRAY_BUFFER, dynamic_vbo);
    glBufferData(GL_ARRAY_BUFFER, triangles.size() * sizeof(MapParser::Triangle), triangles.data(), GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void DepthPrepassRenderer::upload_smoke_shell(const std::vector<MapParser::Triangle>& triangles) {
    smoke_vertex_count = triangles.size() * 3;
    if (triangles.empty()) return;

    glBindBuffer(GL_ARRAY_BUFFER, smoke_vbo);
    glBufferData(GL_ARRAY_BUFFER, triangles.size() * sizeof(MapParser::Triangle), triangles.data(), GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void DepthPrepassRenderer::clear_smoke_shell() {
    smoke_vertex_count = 0;
}

void DepthPrepassRenderer::stamp_smoke_silhouette(const float* view_proj) {
    if (!has_smoke_shell()) return;

    // The caller owns the stencil configuration; this pass only rasterizes the
    // smoke shape with color writes and depth writes disabled, so nothing but
    // the stencil buffer is affected. Both triangle sides are drawn so the
    // silhouette also fills when the camera sits inside the cloud, and a small
    // forward polygon offset keeps the stamp passing the depth test even where
    // the occlusion prepass stored this exact geometry.
    glUseProgram(program_id);
    glUniformMatrix4fv(loc_view_proj, 1, GL_FALSE, view_proj);

    GLboolean color_mask[4];
    glGetBooleanv(GL_COLOR_WRITEMASK, color_mask);
    GLboolean depth_mask = GL_TRUE;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_mask);
    GLint depth_func = GL_LEQUAL;
    glGetIntegerv(GL_DEPTH_FUNC, &depth_func);
    const GLboolean cull_was_enabled = glIsEnabled(GL_CULL_FACE);
    const GLboolean offset_was_enabled = glIsEnabled(GL_POLYGON_OFFSET_FILL);

    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(-1.0f, -1.0f);

    glBindVertexArray(smoke_vao);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)smoke_vertex_count);
    glBindVertexArray(0);

    if (!offset_was_enabled) glDisable(GL_POLYGON_OFFSET_FILL);
    glDepthFunc(depth_func);
    glColorMask(color_mask[0], color_mask[1], color_mask[2], color_mask[3]);
    glDepthMask(depth_mask);
    // Restore culling exactly. Leaving it enabled when the caller had it off
    // silently culls every screen-space quad drawn later (they are CW in NDC).
    if (cull_was_enabled) {
        glEnable(GL_CULL_FACE);
    } else {
        glDisable(GL_CULL_FACE);
    }
    glUseProgram(0);
}

void DepthPrepassRenderer::begin_frame() {
    rendered_this_frame_ = false;
}

void DepthPrepassRenderer::refresh_depth_in_stencil(const float* view_proj) {
    if (!has_geometry() || !reset_vao) return;

    GLboolean color_mask[4];
    glGetBooleanv(GL_COLOR_WRITEMASK, color_mask);
    GLboolean depth_mask = GL_TRUE;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_mask);
    GLint depth_func = GL_LESS;
    glGetIntegerv(GL_DEPTH_FUNC, &depth_func);
    const GLboolean depth_test_was_enabled = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean cull_was_enabled = glIsEnabled(GL_CULL_FACE);
    GLint cull_face_mode = GL_BACK;
    glGetIntegerv(GL_CULL_FACE_MODE, &cull_face_mode);

    glUseProgram(program_id);

    // 1. Push depth back to the far plane wherever the caller's stencil marks
    //    match, discarding whatever the smoke (or earlier passes) wrote there.
    const float identity[16] = {1.0f, 0.0f, 0.0f, 0.0f,
                                0.0f, 1.0f, 0.0f, 0.0f,
                                0.0f, 0.0f, 1.0f, 0.0f,
                                0.0f, 0.0f, 0.0f, 1.0f};
    glUniformMatrix4fv(loc_view_proj, 1, GL_FALSE, identity);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_ALWAYS);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glBindVertexArray(reset_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    // 2. Rasterize the static map depth so the nearest map surface is restored.
    //    The fill offset stores that depth slightly away from the viewer so the
    //    map wireframe drawn afterwards is not rejected by z-fighting.
    glUniformMatrix4fv(loc_view_proj, 1, GL_FALSE, view_proj);
    glDepthFunc(GL_LESS);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(1.0f, 1.0f);

    const Frustum frustum = extract_frustum(view_proj);
    glBindVertexArray(vao);
    for (const auto& chunk : chunks) {
        if (!frustum.intersects_aabb(chunk.mins, chunk.maxs)) continue;
        glDrawElements(GL_TRIANGLES, (GLsizei)chunk.index_count, GL_UNSIGNED_INT,
                       (void*)(chunk.index_offset * sizeof(unsigned int)));
    }
    glBindVertexArray(0);
    glDisable(GL_POLYGON_OFFSET_FILL);

    // Restore the caller's state.
    glColorMask(color_mask[0], color_mask[1], color_mask[2], color_mask[3]);
    glDepthMask(depth_mask);
    glDepthFunc(depth_func);
    if (!depth_test_was_enabled) glDisable(GL_DEPTH_TEST);
    // Restore culling exactly. Leaving it enabled when the caller had it off
    // silently culls every screen-space quad drawn later (they are CW in NDC).
    if (cull_was_enabled) {
        glEnable(GL_CULL_FACE);
    } else {
        glDisable(GL_CULL_FACE);
    }
    glCullFace(cull_face_mode);
    glUseProgram(0);
}

void DepthPrepassRenderer::render(const float* view_proj) {
    bool has_static = has_geometry();
    bool has_dynamic = has_dynamic_geometry();
    if (!has_static && !has_dynamic) return;

    rendered_this_frame_ = true;

    glUseProgram(program_id);
    glUniformMatrix4fv(loc_view_proj, 1, GL_FALSE, view_proj);

    // Save current face culling state
    GLboolean cull_was_enabled = glIsEnabled(GL_CULL_FACE);
    GLint prev_cull_face;
    glGetIntegerv(GL_CULL_FACE_MODE, &prev_cull_face);

    // Enable backface culling for the depth prepass
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);

    // Disable color rendering: write only to depth buffer
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LESS);

    // Render map geometry with view frustum culling
    if (has_static) {
        glBindVertexArray(vao);
        Frustum frustum = extract_frustum(view_proj);
        for (const auto& chunk : chunks) {
            if (frustum.intersects_aabb(chunk.mins, chunk.maxs)) {
                glDrawElements(GL_TRIANGLES, (GLsizei)chunk.index_count, GL_UNSIGNED_INT, (void*)(chunk.index_offset * sizeof(unsigned int)));
            }
        }
    }

    if (has_dynamic) {
        glBindVertexArray(dynamic_vao);
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)dynamic_vertex_count);
    }
    
    // Restore state
    glBindVertexArray(0);
    if (!cull_was_enabled) {
        glDisable(GL_CULL_FACE);
    }
    glCullFace(prev_cull_face);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glUseProgram(0);
}

namespace {
// The overlay runs a compatibility-profile context, so GL_LINE rasterisation
// honours widths above 1.0 on most drivers. Clamp the request to the driver's
// advertised range and report once if it cannot be satisfied.
float clamp_wireframe_line_width(float requested)
{
    if (!(requested > 1.0f)) return 1.0f;

    static float max_supported = 0.0f;
    static bool warned = false;
    if (max_supported <= 0.0f) {
        GLfloat range[2] = {1.0f, 1.0f};
        glGetFloatv(GL_ALIASED_LINE_WIDTH_RANGE, range);
        max_supported = range[1] > 1.0f ? range[1] : 1.0f;
    }

    const float clamped = requested < max_supported ? requested : max_supported;
    if (clamped < requested && !warned) {
        warned = true;
        BUNNYWARE_LOG_WARN("DepthPrepass: wireframe width {:.1f} capped to {:.1f} by the driver", requested,
                         clamped);
    }
    return clamped;
}
}  // namespace

void DepthPrepassRenderer::render_wireframe(const float* view_proj, const float* color, bool depth_tested,
                                            float line_width) {
    bool has_static = has_geometry();
    bool has_dynamic = has_dynamic_geometry();
    if (!has_static && !has_dynamic) return;

    glUseProgram(wireframe_program_id);
    glUniformMatrix4fv(loc_wf_view_proj, 1, GL_FALSE, view_proj);
    glUniform4fv(loc_wf_color, 1, color);

    // Render as line wireframe
    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    glLineWidth(clamp_wireframe_line_width(line_width));

    // Enable blending for transparency support
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    if (depth_tested) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    glDepthMask(GL_FALSE);

    // Render map geometry with view frustum culling
    if (has_static) {
        glBindVertexArray(vao);
        Frustum frustum = extract_frustum(view_proj);
        for (const auto& chunk : chunks) {
            if (frustum.intersects_aabb(chunk.mins, chunk.maxs)) {
                glDrawElements(GL_TRIANGLES, (GLsizei)chunk.index_count, GL_UNSIGNED_INT, (void*)(chunk.index_offset * sizeof(unsigned int)));
            }
        }
    }

    if (has_dynamic) {
        glBindVertexArray(dynamic_vao);
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)dynamic_vertex_count);
    }

    // Restore state
    glBindVertexArray(0);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glLineWidth(1.0f);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LEQUAL);
    glUseProgram(0);
}
