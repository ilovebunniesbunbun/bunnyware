#include "esp_renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "gl_loader.hpp"
#include "logger.hpp"
#include "HighlightTint.hpp"

static const char* esp_vertex_shader = R"glsl(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aColor;
layout(location = 2) in float aAlphaMultiplier;
out vec4 vColor;
out float vAlphaMultiplier;
uniform mat4 uProj;
void main() {
    gl_Position = uProj * vec4(aPos, 1.0);
    vColor = aColor;
    vAlphaMultiplier = aAlphaMultiplier;
}
)glsl";

static const char* esp_fragment_shader = R"glsl(
#version 330 core
in vec4 vColor;
in float vAlphaMultiplier;
out vec4 fragColor;
uniform vec4 uColorOverride;
uniform int uUseOverride;
void main() {
    if (uUseOverride != 0) {
        fragColor = vec4(uColorOverride.rgb, uColorOverride.a * vAlphaMultiplier);
    } else {
        fragColor = vec4(vColor.rgb, vColor.a * vAlphaMultiplier);
    }
}
)glsl";

static const char* trajectory_vertex_shader = R"glsl(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aColor;
layout(location = 2) in float aProgress;
out vec4 vColor;
out float vProgress;
uniform mat4 uProj;
uniform float uFadeAlpha;
void main() {
    gl_Position = uProj * vec4(aPos, 1.0);
    vColor = vec4(aColor.rgb, aColor.a * uFadeAlpha);
    vProgress = aProgress;
}
)glsl";

static const char* trajectory_fragment_shader = R"glsl(
#version 330 core
in vec4 vColor;
in float vProgress;
out vec4 fragColor;
uniform float uEraseProgress;
void main() {
    if (vProgress < uEraseProgress) {
        discard;
    }
    fragColor = vColor;
}
)glsl";

static const char* instanced_vertex_shader = R"glsl(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aOffset;
layout(location = 2) in float aSize;
layout(location = 3) in vec4 aColor;
out vec4 vColor;
uniform mat4 uProj;
void main() {
    vec3 worldPos = aOffset + aPos * aSize;
    gl_Position = uProj * vec4(worldPos, 1.0);
    vColor = aColor;
}
)glsl";

static const char* instanced_fragment_shader = R"glsl(
#version 330 core
in vec4 vColor;
out vec4 fragColor;
void main() {
    fragColor = vColor;
}
)glsl";

static Vec3 catmull_rom_3d(const Vec3& p0, const Vec3& p1, const Vec3& p2, const Vec3& p3, float t)
{
    float t2 = t * t;
    float t3 = t2 * t;
    return {0.5f * ((2.0f * p1.x) + (-p0.x + p2.x) * t + (2.0f * p0.x - 5.0f * p1.x + 4.0f * p2.x - p3.x) * t2 +
                    (-p0.x + 3.0f * p1.x - 3.0f * p2.x + p3.x) * t3),
            0.5f * ((2.0f * p1.y) + (-p0.y + p2.y) * t + (2.0f * p0.y - 5.0f * p1.y + 4.0f * p2.y - p3.y) * t2 +
                    (-p0.y + 3.0f * p1.y - 3.0f * p2.y + p3.y) * t3),
            0.5f * ((2.0f * p1.z) + (-p0.z + p2.z) * t + (2.0f * p0.z - 5.0f * p1.z + 4.0f * p2.z - p3.z) * t2 +
                    (-p0.z + 3.0f * p1.z - 3.0f * p2.z + p3.z) * t3)};
}

static float catmull_rom_1d(float p0, float p1, float p2, float p3, float t)
{
    float t2 = t * t;
    float t3 = t2 * t;
    return 0.5f * ((2.0f * p1) + (-p0 + p2) * t + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
                   (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
}

EspRenderer::~EspRenderer()
{
    cleanup();
}

bool EspRenderer::compile_shader(unsigned int shader, const char* source)
{
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    int success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char log[512];
        glGetShaderInfoLog(shader, 512, nullptr, log);
        BUNNYWARE_LOG_ERROR("ESP_RENDERER: Shader compilation error: {}", log);
        return false;
    }
    return true;
}

bool EspRenderer::link_program()
{
    program_id = glCreateProgram();
    glAttachShader(program_id, vertex_shader);
    glAttachShader(program_id, fragment_shader);
    glLinkProgram(program_id);
    int success;
    glGetProgramiv(program_id, GL_LINK_STATUS, &success);
    if (!success) {
        char log[512];
        glGetProgramInfoLog(program_id, 512, nullptr, log);
        BUNNYWARE_LOG_ERROR("ESP_RENDERER: Shader linking error: {}", log);
        return false;
    }
    return true;
}

bool EspRenderer::link_custom_program(unsigned int& prog_id, const char* vs_src, const char* fs_src)
{
    unsigned int vs = glCreateShader(GL_VERTEX_SHADER);
    if (!compile_shader(vs, vs_src)) return false;

    unsigned int fs = glCreateShader(GL_FRAGMENT_SHADER);
    if (!compile_shader(fs, fs_src)) return false;

    prog_id = glCreateProgram();
    glAttachShader(prog_id, vs);
    glAttachShader(prog_id, fs);
    glLinkProgram(prog_id);

    int success;
    glGetProgramiv(prog_id, GL_LINK_STATUS, &success);
    glDeleteShader(vs);
    glDeleteShader(fs);

    if (!success) {
        char log[512];
        glGetProgramInfoLog(prog_id, 512, nullptr, log);
        BUNNYWARE_LOG_ERROR("ESP_RENDERER: Custom program linking error: {}", log);
        return false;
    }
    return true;
}

bool EspRenderer::init()
{
    vertex_shader = glCreateShader(GL_VERTEX_SHADER);
    if (!compile_shader(vertex_shader, esp_vertex_shader)) return false;

    fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);
    if (!compile_shader(fragment_shader, esp_fragment_shader)) return false;

    if (!link_program()) return false;

    loc_proj = glGetUniformLocation(program_id, "uProj");
    loc_color_override = glGetUniformLocation(program_id, "uColorOverride");
    loc_use_override = glGetUniformLocation(program_id, "uUseOverride");

    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);

    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);

    // Preallocate VBO to 64KB
    vbo_capacity = 64 * 1024;
    glBufferData(GL_ARRAY_BUFFER, vbo_capacity, nullptr, GL_STREAM_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(EspVertex), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(EspVertex), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, sizeof(EspVertex), (void*)(7 * sizeof(float)));

    // Link trajectory program
    if (!link_custom_program(trajectory_program_id, trajectory_vertex_shader, trajectory_fragment_shader)) return false;
    loc_traj_proj = glGetUniformLocation(trajectory_program_id, "uProj");
    loc_traj_erase = glGetUniformLocation(trajectory_program_id, "uEraseProgress");
    loc_traj_fade = glGetUniformLocation(trajectory_program_id, "uFadeAlpha");

    // Link instanced program
    if (!link_custom_program(instanced_program_id, instanced_vertex_shader, instanced_fragment_shader)) return false;
    loc_inst_proj = glGetUniformLocation(instanced_program_id, "uProj");

    // Create static Unit Box VAO/VBO
    const float unit_box[] = {-1.0f, -1.0f, -1.0f, 1.0f,  -1.0f, -1.0f, 1.0f,  -1.0f, -1.0f, 1.0f,  1.0f,  -1.0f,
                              1.0f,  1.0f,  -1.0f, -1.0f, 1.0f,  -1.0f, -1.0f, 1.0f,  -1.0f, -1.0f, -1.0f, -1.0f,
                              -1.0f, -1.0f, 1.0f,  1.0f,  -1.0f, 1.0f,  1.0f,  -1.0f, 1.0f,  1.0f,  1.0f,  1.0f,
                              1.0f,  1.0f,  1.0f,  -1.0f, 1.0f,  1.0f,  -1.0f, 1.0f,  1.0f,  -1.0f, -1.0f, 1.0f,
                              -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, 1.0f,  1.0f,  -1.0f, -1.0f, 1.0f,  -1.0f, 1.0f,
                              1.0f,  1.0f,  -1.0f, 1.0f,  1.0f,  1.0f,  -1.0f, 1.0f,  -1.0f, -1.0f, 1.0f,  1.0f};
    glGenVertexArrays(1, &unit_box_vao);
    glGenBuffers(1, &unit_box_vbo);
    glBindVertexArray(unit_box_vao);
    glBindBuffer(GL_ARRAY_BUFFER, unit_box_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(unit_box), unit_box, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);

    // Create static Unit Circle VAO/VBO
    float unit_circle[48 * 3];
    constexpr int SEGMENTS = 24;
    for (int i = 0; i < SEGMENTS; ++i) {
        float a1 = i * (2.0f * 3.14159265f / SEGMENTS);
        float a2 = (i + 1) * (2.0f * 3.14159265f / SEGMENTS);
        unit_circle[i * 6 + 0] = std::cos(a1);
        unit_circle[i * 6 + 1] = std::sin(a1);
        unit_circle[i * 6 + 2] = 0.0f;
        unit_circle[i * 6 + 3] = std::cos(a2);
        unit_circle[i * 6 + 4] = std::sin(a2);
        unit_circle[i * 6 + 5] = 0.0f;
    }
    glGenVertexArrays(1, &unit_circle_vao);
    glGenBuffers(1, &unit_circle_vbo);
    glBindVertexArray(unit_circle_vao);
    glBindBuffer(GL_ARRAY_BUFFER, unit_circle_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(unit_circle), unit_circle, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);

    // Create dynamic Instance VBO with pre-allocated max capacity
    glGenBuffers(1, &instance_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, instance_vbo);
    glBufferData(GL_ARRAY_BUFFER, 1024 * sizeof(BoxInstance), nullptr, GL_DYNAMIC_DRAW);

    box_instances.reserve(1024);
    circle_instances.reserve(1024);

    // Bind instance attributes to unit box VAO
    glBindVertexArray(unit_box_vao);
    glBindBuffer(GL_ARRAY_BUFFER, instance_vbo);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)0);
    glVertexAttribDivisor(1, 1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(3 * sizeof(float)));
    glVertexAttribDivisor(2, 1);
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(4 * sizeof(float)));
    glVertexAttribDivisor(3, 1);

    // Bind instance attributes to unit circle VAO
    glBindVertexArray(unit_circle_vao);
    glBindBuffer(GL_ARRAY_BUFFER, instance_vbo);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)0);
    glVertexAttribDivisor(1, 1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(3 * sizeof(float)));
    glVertexAttribDivisor(2, 1);
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(4 * sizeof(float)));
    glVertexAttribDivisor(3, 1);

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    return true;
}

void EspRenderer::cleanup()
{
    clear();
    line_batches.clear();
    if (vao) {
        glDeleteVertexArrays(1, &vao);
        vao = 0;
    }
    if (vbo) {
        glDeleteBuffers(1, &vbo);
        vbo = 0;
    }
    if (vertex_shader) {
        glDeleteShader(vertex_shader);
        vertex_shader = 0;
    }
    if (fragment_shader) {
        glDeleteShader(fragment_shader);
        fragment_shader = 0;
    }
    if (program_id) {
        glDeleteProgram(program_id);
        program_id = 0;
    }

    if (unit_box_vao) {
        glDeleteVertexArrays(1, &unit_box_vao);
        unit_box_vao = 0;
    }
    if (unit_box_vbo) {
        glDeleteBuffers(1, &unit_box_vbo);
        unit_box_vbo = 0;
    }
    if (unit_circle_vao) {
        glDeleteVertexArrays(1, &unit_circle_vao);
        unit_circle_vao = 0;
    }
    if (unit_circle_vbo) {
        glDeleteBuffers(1, &unit_circle_vbo);
        unit_circle_vbo = 0;
    }
    if (instance_vbo) {
        glDeleteBuffers(1, &instance_vbo);
        instance_vbo = 0;
    }
    if (trajectory_program_id) {
        glDeleteProgram(trajectory_program_id);
        trajectory_program_id = 0;
    }
    if (instanced_program_id) {
        glDeleteProgram(instanced_program_id);
        instanced_program_id = 0;
    }

    for (auto& t : gpu_trajectories) {
        if (t.vao) glDeleteVertexArrays(1, &t.vao);
        if (t.vbo) glDeleteBuffers(1, &t.vbo);
    }
    gpu_trajectories.clear();
}

void EspRenderer::clear()
{
    for (auto& p : line_batches) {
        p.second.clear();
    }
    triangle_vertices.clear();
    box_instances.clear();
    circle_instances.clear();
}

EspRenderer::VertexMark EspRenderer::mark() const
{
    VertexMark m;
    m.lines.reserve(line_batches.size());
    for (const auto& p : line_batches) m.lines.push_back(p.second.size());
    m.triangles = triangle_vertices.size();
    return m;
}

void EspRenderer::tint_since(const VertexMark& m, float alpha, const float* tint)
{
    auto apply = [&](EspVertex& v) {
        float color[4] = {v.r, v.g, v.b, v.a};
        tint_highlight(color, alpha, tint);
        v.r = color[0];
        v.g = color[1];
        v.b = color[2];
        v.a = color[3];
    };
    for (size_t b = 0; b < line_batches.size(); ++b) {
        auto& verts = line_batches[b].second;
        for (size_t i = b < m.lines.size() ? m.lines[b] : 0; i < verts.size(); ++i) apply(verts[i]);
    }
    for (size_t i = m.triangles; i < triangle_vertices.size(); ++i) apply(triangle_vertices[i]);
}

void EspRenderer::multiply_alpha_since(const VertexMark& m, float alpha)
{
    const float multiplier = std::clamp(alpha, 0.0f, 1.0f);
    for (size_t b = 0; b < line_batches.size(); ++b) {
        auto& verts = line_batches[b].second;
        for (size_t i = b < m.lines.size() ? m.lines[b] : 0; i < verts.size(); ++i)
            verts[i].alpha_multiplier *= multiplier;
    }
    for (size_t i = m.triangles; i < triangle_vertices.size(); ++i)
        triangle_vertices[i].alpha_multiplier *= multiplier;
}

void EspRenderer::set_projection(const float* proj_matrix)
{
    std::memcpy(current_proj, proj_matrix, sizeof(float) * 16);
}

void EspRenderer::set_ortho(float left, float right, float bottom, float top)
{
    float ortho[16] = {2.0f / (right - left),
                       0.0f,
                       0.0f,
                       0.0f,
                       0.0f,
                       2.0f / (top - bottom),
                       0.0f,
                       0.0f,
                       0.0f,
                       0.0f,
                       1.0f,
                       0.0f,
                       -(right + left) / (right - left),
                       -(top + bottom) / (top - bottom),
                       0.0f,
                       1.0f};
    set_projection(ortho);
}

void EspRenderer::add_line_2d(float x1, float y1, float x2, float y2, const float* color, float thickness, float z)
{
    std::vector<EspVertex>* batch = nullptr;
    for (auto& p : line_batches) {
        if (p.first == thickness) {
            batch = &p.second;
            break;
        }
    }
    if (!batch) {
        line_batches.emplace_back(thickness, std::vector<EspVertex>{});
        batch = &line_batches.back().second;
    }
    batch->push_back({x1, y1, z, color[0], color[1], color[2], color[3]});
    batch->push_back({x2, y2, z, color[0], color[1], color[2], color[3]});
}

void EspRenderer::add_quad_2d(float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4,
                              const float* color, float z)
{
    triangle_vertices.push_back({x1, y1, z, color[0], color[1], color[2], color[3]});
    triangle_vertices.push_back({x2, y2, z, color[0], color[1], color[2], color[3]});
    triangle_vertices.push_back({x3, y3, z, color[0], color[1], color[2], color[3]});

    triangle_vertices.push_back({x1, y1, z, color[0], color[1], color[2], color[3]});
    triangle_vertices.push_back({x3, y3, z, color[0], color[1], color[2], color[3]});
    triangle_vertices.push_back({x4, y4, z, color[0], color[1], color[2], color[3]});
}

void EspRenderer::add_rect_2d(float x, float y, float w, float h, const float* color, float z)
{
    add_quad_2d(x, y, x + w, y, x + w, y + h, x, y + h, color, z);
}

void EspRenderer::add_outlined_rect_2d(float x, float y, float w, float h, const float* color, float thickness,
                                       bool draw_outline, float z)
{
    float ix = std::round(x);
    float iy = std::round(y);
    float iw = std::round(w);
    float ih = std::round(h);
    float t = std::round(thickness);
    if (t < 1.0f) t = 1.0f;

    float half_t = t * 0.5f;

    if (draw_outline) {
        float black[4] = {0.0f, 0.0f, 0.0f, color[3]};

        // Outer outline quads (thickness 1.0f)
        // Top outer:
        add_quad_2d(ix - half_t - 1.0f, iy - half_t - 1.0f, ix + iw + half_t + 1.0f, iy - half_t - 1.0f,
                    ix + iw + half_t + 1.0f, iy - half_t, ix - half_t - 1.0f, iy - half_t, black, z);

        // Bottom outer:
        add_quad_2d(ix - half_t - 1.0f, iy + ih + half_t, ix + iw + half_t + 1.0f, iy + ih + half_t,
                    ix + iw + half_t + 1.0f, iy + ih + half_t + 1.0f, ix - half_t - 1.0f, iy + ih + half_t + 1.0f,
                    black, z);

        // Left outer:
        add_quad_2d(ix - half_t - 1.0f, iy - half_t, ix - half_t, iy - half_t, ix - half_t, iy + ih + half_t,
                    ix - half_t - 1.0f, iy + ih + half_t, black, z);

        // Right outer:
        add_quad_2d(ix + iw + half_t, iy - half_t, ix + iw + half_t + 1.0f, iy - half_t, ix + iw + half_t + 1.0f,
                    iy + ih + half_t, ix + iw + half_t, iy + ih + half_t, black, z);

        // Inner outline quads (thickness 1.0f)
        if (iw > t + 2.0f && ih > t + 2.0f) {
            // Top inner:
            add_quad_2d(ix + half_t, iy + half_t, ix + iw - half_t, iy + half_t, ix + iw - half_t, iy + half_t + 1.0f,
                        ix + half_t, iy + half_t + 1.0f, black, z);

            // Bottom inner:
            add_quad_2d(ix + half_t, iy + ih - half_t - 1.0f, ix + iw - half_t, iy + ih - half_t - 1.0f,
                        ix + iw - half_t, iy + ih - half_t, ix + half_t, iy + ih - half_t, black, z);

            // Left inner:
            add_quad_2d(ix + half_t, iy + half_t + 1.0f, ix + half_t + 1.0f, iy + half_t + 1.0f, ix + half_t + 1.0f,
                        iy + ih - half_t - 1.0f, ix + half_t, iy + ih - half_t - 1.0f, black, z);

            // Right inner:
            add_quad_2d(ix + iw - half_t - 1.0f, iy + half_t + 1.0f, ix + iw - half_t, iy + half_t + 1.0f,
                        ix + iw - half_t, iy + ih - half_t - 1.0f, ix + iw - half_t - 1.0f, iy + ih - half_t - 1.0f,
                        black, z);
        }
    }

    // Main box quads (thickness t)
    // Top main:
    add_quad_2d(ix - half_t, iy - half_t, ix + iw + half_t, iy - half_t, ix + iw + half_t, iy + half_t, ix - half_t,
                iy + half_t, color, z);

    // Bottom main:
    add_quad_2d(ix - half_t, iy + ih - half_t, ix + iw + half_t, iy + ih - half_t, ix + iw + half_t, iy + ih + half_t,
                ix - half_t, iy + ih + half_t, color, z);

    // Left main:
    add_quad_2d(ix - half_t, iy + half_t, ix + half_t, iy + half_t, ix + half_t, iy + ih - half_t, ix - half_t,
                iy + ih - half_t, color, z);

    // Right main:
    add_quad_2d(ix + iw - half_t, iy + half_t, ix + iw + half_t, iy + half_t, ix + iw + half_t, iy + ih - half_t,
                ix + iw - half_t, iy + ih - half_t, color, z);
}

void EspRenderer::add_health_bar_2d(float x, float y, float w, float h, float health, const OverlayConfig& cfg, float z)
{
    (void)w;
    if (health < 0.0f) health = 0.0f;
    if (health > 100.0f) health = 100.0f;

    float ix = std::round(x);
    float iy = std::round(y);
    float ih = std::round(h);
    float bar_w = std::round(cfg.esp_health_bar_thickness);
    if (bar_w < 1.0f) bar_w = 1.0f;

    float bar_x = ix - bar_w - 4.0f;
    float bar_y = iy;
    float bar_h = ih;

    float hp_pct = health / 100.0f;
    float fill_h = std::round(bar_h * hp_pct);
    float fill_y = bar_y + (bar_h - fill_h);

    if (cfg.esp_health_bar_outline) {
        float black[4] = {0.0f, 0.0f, 0.0f, cfg.esp_health_bar_color[3]};
        add_rect_2d(bar_x - 1.0f, bar_y - 1.0f, bar_w + 2.0f, bar_h + 2.0f, black, z);
    }

    float r = cfg.esp_health_bar_color[0];
    float g = cfg.esp_health_bar_color[1];
    float b = cfg.esp_health_bar_color[2];
    float a = cfg.esp_health_bar_color[3];
    if (cfg.esp_health_bar_gradient) {
        r = cfg.esp_health_bar_gradient_start[0] * hp_pct + cfg.esp_health_bar_gradient_end[0] * (1.0f - hp_pct);
        g = cfg.esp_health_bar_gradient_start[1] * hp_pct + cfg.esp_health_bar_gradient_end[1] * (1.0f - hp_pct);
        b = cfg.esp_health_bar_gradient_start[2] * hp_pct + cfg.esp_health_bar_gradient_end[2] * (1.0f - hp_pct);
        a = cfg.esp_health_bar_gradient_start[3] * hp_pct + cfg.esp_health_bar_gradient_end[3] * (1.0f - hp_pct);
    }

    float color[4] = {r, g, b, a};
    add_rect_2d(bar_x, fill_y, bar_w, fill_h, color, z);
}

void EspRenderer::add_circle_2d(float cx, float cy, float radius, const float* color, float thickness, int segments, float z)
{
    float step = 2.0f * 3.1415926535f / segments;
    float prev_x = cx + radius;
    float prev_y = cy;
    for (int i = 1; i <= segments; ++i) {
        float angle = i * step;
        float curr_x = cx + radius * std::cos(angle);
        float curr_y = cy + radius * std::sin(angle);
        add_line_2d(prev_x, prev_y, curr_x, curr_y, color, thickness, z);
        prev_x = curr_x;
        prev_y = curr_y;
    }
}

void EspRenderer::add_filled_circle_2d(float cx, float cy, float radius, const float* color, int segments, float z)
{
    float step = 2.0f * 3.1415926535f / segments;
    float prev_x = cx + radius;
    float prev_y = cy;
    for (int i = 1; i <= segments; ++i) {
        float angle = i * step;
        float curr_x = cx + radius * std::cos(angle);
        float curr_y = cy + radius * std::sin(angle);
        triangle_vertices.push_back({cx, cy, z, color[0], color[1], color[2], color[3]});
        triangle_vertices.push_back({prev_x, prev_y, z, color[0], color[1], color[2], color[3]});
        triangle_vertices.push_back({curr_x, curr_y, z, color[0], color[1], color[2], color[3]});
        prev_x = curr_x;
        prev_y = curr_y;
    }
}

void EspRenderer::add_line_3d(const Vec3& p1, const Vec3& p2, const float* color, float thickness)
{
    constexpr float EPSILON = 0.05f;  // Near plane clip threshold in homogeneous coordinates

    // Use column-major indexing since current_proj (gl_vp) is column-major
    float w1 = current_proj[3] * p1.x + current_proj[7] * p1.y + current_proj[11] * p1.z + current_proj[15];
    float w2 = current_proj[3] * p2.x + current_proj[7] * p2.y + current_proj[11] * p2.z + current_proj[15];

    // 1. Cull if both are behind near plane
    if (w1 < EPSILON && w2 < EPSILON) return;

    Vec3 clip_p1 = p1;
    Vec3 clip_p2 = p2;

    // 2. Near-plane clipping
    if (w1 < EPSILON) {
        float t = (EPSILON - w1) / (w2 - w1);
        clip_p1 = {p1.x + t * (p2.x - p1.x), p1.y + t * (p2.y - p1.y), p1.z + t * (p2.z - p1.z)};
        w1 = EPSILON;
    } else if (w2 < EPSILON) {
        float t = (EPSILON - w2) / (w1 - w2);
        clip_p2 = {p2.x + t * (p1.x - p2.x), p2.y + t * (p1.y - p2.y), p2.z + t * (p1.z - p2.z)};
        w2 = EPSILON;
    }

    // 3. Frustum culling (only check if both are in front of the camera, which is guaranteed now)
    float x1 =
        current_proj[0] * clip_p1.x + current_proj[4] * clip_p1.y + current_proj[8] * clip_p1.z + current_proj[12];
    float x2 =
        current_proj[0] * clip_p2.x + current_proj[4] * clip_p2.y + current_proj[8] * clip_p2.z + current_proj[12];

    if (x1 < -w1 && x2 < -w2) return;
    if (x1 > w1 && x2 > w2) return;

    float y1 =
        current_proj[1] * clip_p1.x + current_proj[5] * clip_p1.y + current_proj[9] * clip_p1.z + current_proj[13];
    float y2 =
        current_proj[1] * clip_p2.x + current_proj[5] * clip_p2.y + current_proj[9] * clip_p2.z + current_proj[13];

    if (y1 < -w1 && y2 < -w2) return;
    if (y1 > w1 && y2 > w2) return;

    std::vector<EspVertex>* batch = nullptr;
    for (auto& p : line_batches) {
        if (p.first == thickness) {
            batch = &p.second;
            break;
        }
    }
    if (!batch) {
        line_batches.emplace_back(thickness, std::vector<EspVertex>{});
        batch = &line_batches.back().second;
    }
    batch->push_back({clip_p1.x, clip_p1.y, clip_p1.z, color[0], color[1], color[2], color[3]});
    batch->push_back({clip_p2.x, clip_p2.y, clip_p2.z, color[0], color[1], color[2], color[3]});
}

void EspRenderer::add_capsule_3d(const Vec3& start, const Vec3& end, float radius, const float* color, float thickness)
{
    // Draw the main line between start and end
    add_line_3d(start, end, color, thickness);

    // Helper to draw 3D orthogonal wireframe circles around a point
    auto draw_wireframe_sphere = [&](const Vec3& center, float r) {
        constexpr int segments = 8;
        for (int step = 0; step < segments; ++step) {
            float a1 = (float)step * 2.0f * (float)M_PI / segments;
            float a2 = (float)(step + 1) * 2.0f * (float)M_PI / segments;
            
            float c1 = std::cos(a1) * r;
            float s1 = std::sin(a1) * r;
            float c2 = std::cos(a2) * r;
            float s2 = std::sin(a2) * r;

            // XY circle
            add_line_3d(Vec3{center.x + c1, center.y + s1, center.z}, Vec3{center.x + c2, center.y + s2, center.z}, color, thickness);
            // XZ circle
            add_line_3d(Vec3{center.x + c1, center.y, center.z + s1}, Vec3{center.x + c2, center.y, center.z + s2}, color, thickness);
            // YZ circle
            add_line_3d(Vec3{center.x, center.y + c1, center.z + s1}, Vec3{center.x, center.y + c2, center.z + s2}, color, thickness);
        }
    };

    draw_wireframe_sphere(start, radius);

    float dx = end.x - start.x;
    float dy = end.y - start.y;
    float dz = end.z - start.z;
    float dist = std::sqrt(dx*dx + dy*dy + dz*dz);

    if (dist > 0.01f) {
        draw_wireframe_sphere(end, radius);

        // Draw 4 longitudinal lines connecting the outer spheres
        float nx = dx / dist;
        float ny = dy / dist;
        float nz = dz / dist;

        // Perpendicular vector 1
        float ux = 0.0f, uy = 0.0f, uz = 0.0f;
        if (std::abs(nz) < 0.9f) {
            // cross product with {0,0,1}
            float len = std::sqrt(nx*nx + ny*ny);
            ux = -ny / len;
            uy = nx / len;
            uz = 0.0f;
        } else {
            // cross product with {0,1,0}
            float len = std::sqrt(nx*nx + nz*nz);
            ux = nz / len;
            uy = 0.0f;
            uz = -nx / len;
        }

        // Perpendicular vector 2 (cross product of direction and vector 1)
        float vx = ny * uz - nz * uy;
        float vy = nz * ux - nx * uz;
        float vz = nx * uy - ny * ux;

        for (int i = 0; i < 4; ++i) {
            float angle = (float)i * (float)M_PI / 2.0f;
            float ca = std::cos(angle);
            float sa = std::sin(angle);

            float ox = (ux * ca + vx * sa) * radius;
            float oy = (uy * ca + vy * sa) * radius;
            float oz = (uz * ca + vz * sa) * radius;

            add_line_3d(
                Vec3{start.x + ox, start.y + oy, start.z + oz},
                Vec3{end.x + ox, end.y + oy, end.z + oz},
                color,
                thickness
            );
        }
    }
}


void EspRenderer::add_skeleton_chain_3d(const std::vector<Vec3>& sanitized_positions, const std::vector<int>& chain,
                                        const std::vector<float>& vis, int player_idx, const OverlayConfig& cfg,
                                        bool for_glow, float pulse_factor, const float* override_glow_color)
{
    Vec3 points[8];
    float bone_vis[8];
    int point_count = 0;

    for (int b : chain) {
        if (b >= static_cast<int>(sanitized_positions.size())) continue;
        if (point_count >= 8) break;
        points[point_count] = sanitized_positions[b];

        int vis_idx = player_idx * 128 + b;
        if (vis_idx < static_cast<int>(vis.size())) {
            bone_vis[point_count] = vis[vis_idx];
        } else {
            bone_vis[point_count] = 1.0f;
        }
        point_count++;
    }

    if (point_count < 2) return;

    float thickness = cfg.esp_skeleton_thickness;

    if (cfg.esp_rounded_skeleton) {
        Vec3 ctrl_pts[10];  // max 8 + 2 padding
        float ctrl_vis[10];
        ctrl_pts[0] = points[0];
        ctrl_vis[0] = bone_vis[0];
        for (int i = 0; i < point_count; ++i) {
            ctrl_pts[i + 1] = points[i];
            ctrl_vis[i + 1] = bone_vis[i];
        }
        ctrl_pts[point_count + 1] = points[point_count - 1];
        ctrl_vis[point_count + 1] = bone_vis[point_count - 1];
        int ctrl_count = point_count + 2;

        Vec3 last_pt;
        float last_v = 0.0f;
        bool first = true;

        for (int i = 0; i + 3 < ctrl_count; ++i) {
            const auto& p0 = ctrl_pts[i];
            const auto& p1 = ctrl_pts[i + 1];
            const auto& p2 = ctrl_pts[i + 2];
            const auto& p3 = ctrl_pts[i + 3];

            float v0 = ctrl_vis[i];
            float v1 = ctrl_vis[i + 1];
            float v2 = ctrl_vis[i + 2];
            float v3 = ctrl_vis[i + 3];

            for (int step = 0; step <= 20; ++step) {
                float t = static_cast<float>(step) / 20.0f;
                Vec3 pt = catmull_rom_3d(p0, p1, p2, p3, t);
                float v = catmull_rom_1d(v0, v1, v2, v3, t);

                if (first) {
                    last_pt = pt;
                    last_v = v;
                    first = false;
                    continue;
                }

                if (for_glow) {
                    float color[4];
                    if (override_glow_color) {
                        std::memcpy(color, override_glow_color, sizeof(float) * 4);
                    } else {
                        std::memcpy(color, cfg.esp_skeleton_glow_color, sizeof(float) * 4);
                    }
                    color[3] *= pulse_factor;
                    add_line_3d(last_pt, pt, color, thickness);
                } else {
                    float mid_v = (last_v + v) * 0.5f;
                    const float* color = (mid_v > 0.5f) ? cfg.esp_skeleton_color_vis : cfg.esp_skeleton_color_invis;
                    add_line_3d(last_pt, pt, color, thickness);
                }

                last_pt = pt;
                last_v = v;
            }
        }
    } else {
        for (int i = 0; i < point_count - 1; ++i) {
            const auto& pt1 = points[i];
            const auto& pt2 = points[i + 1];
            float v1 = bone_vis[i];
            float v2 = bone_vis[i + 1];

            if (for_glow) {
                float color[4];
                if (override_glow_color) {
                    std::memcpy(color, override_glow_color, sizeof(float) * 4);
                } else {
                    std::memcpy(color, cfg.esp_skeleton_glow_color, sizeof(float) * 4);
                }
                color[3] *= pulse_factor;
                add_line_3d(pt1, pt2, color, thickness);
            } else {
                const int steps = 5;
                Vec3 prev_step = pt1;
                float prev_v = v1;

                for (int s = 1; s <= steps; ++s) {
                    float t = static_cast<float>(s) / steps;
                    Vec3 curr_step = {pt1.x + t * (pt2.x - pt1.x), pt1.y + t * (pt2.y - pt1.y),
                                      pt1.z + t * (pt2.z - pt1.z)};
                    float curr_v = v1 + t * (v2 - v1);
                    float mid_v = (prev_v + curr_v) * 0.5f;

                    const float* color = (mid_v > 0.5f) ? cfg.esp_skeleton_color_vis : cfg.esp_skeleton_color_invis;
                    add_line_3d(prev_step, curr_step, color, thickness);

                    prev_step = curr_step;
                    prev_v = curr_v;
                }
            }
        }
    }
}

void EspRenderer::add_skeleton_3d(const std::vector<Vec3>& sanitized_positions, int player_idx,
                                  const std::vector<float>& vis, const OverlayConfig& cfg, bool for_glow,
                                  float pulse_factor, const float* override_glow_color)
{
    static const std::vector<std::vector<int>> bone_chains = {
        {7, 6, 23, 1}, {23, 10, 11}, {23, 14, 15}, {1, 18, 19}, {1, 21, 22}};

    for (const auto& chain : bone_chains) {
        add_skeleton_chain_3d(sanitized_positions, chain, vis, player_idx, cfg, for_glow, pulse_factor,
                              override_glow_color);
    }
}

void EspRenderer::upload_lines()
{
    if (line_batches.empty()) return;

    size_t total_vertices = 0;
    for (const auto& pair : line_batches) {
        total_vertices += pair.second.size();
    }
    if (total_vertices == 0) return;

    size_t required_size = total_vertices * sizeof(EspVertex);

    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);

    if (required_size > vbo_capacity) {
        vbo_capacity = required_size * 2;
        glBufferData(GL_ARRAY_BUFFER, vbo_capacity, nullptr, GL_STREAM_DRAW);
    }

    size_t offset = 0;
    for (const auto& pair : line_batches) {
        const auto& vertices = pair.second;
        if (vertices.empty()) continue;

        size_t bytes = vertices.size() * sizeof(EspVertex);
        glBufferSubData(GL_ARRAY_BUFFER, offset, bytes, vertices.data());
        offset += bytes;
    }

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void EspRenderer::draw_lines_override(const float* override_color)
{
    if (line_batches.empty()) return;

    glUseProgram(program_id);
    glUniformMatrix4fv(loc_proj, 1, GL_FALSE, current_proj);
    glUniform1i(loc_use_override, override_color ? 1 : 0);
    if (override_color) {
        glUniform4fv(loc_color_override, 1, override_color);
    }

    glBindVertexArray(vao);
    
    size_t offset = 0;
    for (const auto& pair : line_batches) {
        float thickness = pair.first;
        const auto& vertices = pair.second;
        if (vertices.empty()) continue;

        glLineWidth(thickness);
        glDrawArrays(GL_LINES, (GLint)(offset / sizeof(EspVertex)), (GLsizei)vertices.size());
        offset += vertices.size() * sizeof(EspVertex);
    }

    glBindVertexArray(0);
    glUseProgram(0);
}

void EspRenderer::flush_lines()
{
    upload_lines();
    draw_lines_override(nullptr);
}

void EspRenderer::flush_lines_override(const float* override_color)
{
    upload_lines();
    draw_lines_override(override_color);
}

void EspRenderer::flush_triangles()
{
    if (triangle_vertices.empty()) return;

    size_t required_size = triangle_vertices.size() * sizeof(EspVertex);

    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);

    if (required_size > vbo_capacity) {
        vbo_capacity = required_size * 2;
        glBufferData(GL_ARRAY_BUFFER, vbo_capacity, nullptr, GL_STREAM_DRAW);
    }
    glBufferSubData(GL_ARRAY_BUFFER, 0, required_size, triangle_vertices.data());

    glUseProgram(program_id);
    glUniformMatrix4fv(loc_proj, 1, GL_FALSE, current_proj);
    glUniform1i(loc_use_override, 0);

    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)triangle_vertices.size());

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glUseProgram(0);

    triangle_vertices.clear();
}

void EspRenderer::upload_trajectory(uint32_t handle, float spawn_time, const std::vector<Vec3>& points,
                                    const float* color)
{
    if (points.empty()) return;

    std::vector<TrajectoryVertex> vertices;
    vertices.reserve(points.size());

    for (size_t i = 0; i < points.size(); ++i) {
        float progress = points.size() > 1 ? static_cast<float>(i) / (points.size() - 1) : 1.0f;
        vertices.push_back({points[i].x, points[i].y, points[i].z, color[0], color[1], color[2], color[3], progress});
    }

    GpuTrajectory traj;
    traj.entity_handle = handle;
    traj.spawn_time = spawn_time;
    traj.vertex_count = points.size();

    glGenVertexArrays(1, &traj.vao);
    glGenBuffers(1, &traj.vbo);

    glBindVertexArray(traj.vao);
    glBindBuffer(GL_ARRAY_BUFFER, traj.vbo);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(TrajectoryVertex), vertices.data(), GL_STATIC_DRAW);

    // Attribute 0: Position
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(TrajectoryVertex), (void*)0);

    // Attribute 1: Color
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(TrajectoryVertex), (void*)(3 * sizeof(float)));

    // Attribute 2: Progress
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, sizeof(TrajectoryVertex), (void*)(7 * sizeof(float)));

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    gpu_trajectories.push_back(traj);
}

void EspRenderer::draw_gpu_trajectory(uint32_t handle, float spawn_time, float erase_progress, float fade_alpha, float thickness)
{
    for (const auto& traj : gpu_trajectories) {
        if (traj.entity_handle == handle && traj.spawn_time == spawn_time) {
            glUseProgram(trajectory_program_id);
            glUniformMatrix4fv(loc_traj_proj, 1, GL_FALSE, current_proj);
            glUniform1f(loc_traj_erase, erase_progress);
            glUniform1f(loc_traj_fade, fade_alpha);

            glLineWidth(thickness);

            glBindVertexArray(traj.vao);
            glDrawArrays(GL_LINE_STRIP, 0, (GLsizei)traj.vertex_count);

            glBindVertexArray(0);
            glUseProgram(0);
            break;
        }
    }
}

void EspRenderer::prune_gpu_trajectories(const std::vector<uint32_t>& active_handles)
{
    std::vector<GpuTrajectory> next_gpu_trajectories;
    for (auto& traj : gpu_trajectories) {
        bool still_needed = false;
        for (uint32_t h : active_handles) {
            if (h == traj.entity_handle) {
                still_needed = true;
                break;
            }
        }
        if (still_needed) {
            next_gpu_trajectories.push_back(traj);
        } else {
            if (traj.vao) glDeleteVertexArrays(1, &traj.vao);
            if (traj.vbo) glDeleteBuffers(1, &traj.vbo);
        }
    }
    gpu_trajectories = std::move(next_gpu_trajectories);
}

void EspRenderer::add_box_instance(const Vec3& center, float size, const float* color)
{
    box_instances.push_back({center, size, {color[0], color[1], color[2], color[3]}});
}

void EspRenderer::add_circle_instance(const Vec3& center, float radius, const float* color)
{
    circle_instances.push_back({center, radius, {color[0], color[1], color[2], color[3]}});
}

void EspRenderer::add_filled_circle_3d(const Vec3& center, float radius, const float* color, int segments)
{
    if (radius <= 0.0f || segments < 3) return;
    const float step = 2.0f * 3.1415926535f / static_cast<float>(segments);
    for (int i = 0; i < segments; ++i) {
        const float a0 = static_cast<float>(i) * step;
        const float a1 = static_cast<float>(i + 1) * step;
        const float x0 = center.x + radius * std::cos(a0);
        const float y0 = center.y + radius * std::sin(a0);
        const float x1 = center.x + radius * std::cos(a1);
        const float y1 = center.y + radius * std::sin(a1);
        // Triangle: center, edge_i, edge_i+1 (XY plane, Z-up)
        triangle_vertices.push_back({center.x, center.y, center.z, color[0], color[1], color[2], color[3]});
        triangle_vertices.push_back({x0, y0, center.z, color[0], color[1], color[2], color[3]});
        triangle_vertices.push_back({x1, y1, center.z, color[0], color[1], color[2], color[3]});
    }
}

void EspRenderer::flush_instances()
{
    if (box_instances.empty() && circle_instances.empty()) return;

    // The instance VBO is pre-allocated for 1024 instances, so larger batches
    // (e.g. smoke wireframe cages) are uploaded and drawn in chunks.
    constexpr size_t kMaxInstancesPerBatch = 1024;

    glUseProgram(instanced_program_id);
    glUniformMatrix4fv(loc_inst_proj, 1, GL_FALSE, current_proj);

    // 1. Draw Instanced Boxes
    if (!box_instances.empty()) {
        glBindBuffer(GL_ARRAY_BUFFER, instance_vbo);
        glBindVertexArray(unit_box_vao);
        for (size_t offset = 0; offset < box_instances.size(); offset += kMaxInstancesPerBatch) {
            const size_t count = std::min(kMaxInstancesPerBatch, box_instances.size() - offset);
            glBufferSubData(GL_ARRAY_BUFFER, 0, count * sizeof(BoxInstance), box_instances.data() + offset);
            glDrawArraysInstanced(GL_LINES, 0, 24, (GLsizei)count);
        }
    }

    // 2. Draw Instanced Circles
    if (!circle_instances.empty()) {
        glBindBuffer(GL_ARRAY_BUFFER, instance_vbo);
        glBindVertexArray(unit_circle_vao);
        for (size_t offset = 0; offset < circle_instances.size(); offset += kMaxInstancesPerBatch) {
            const size_t count = std::min(kMaxInstancesPerBatch, circle_instances.size() - offset);
            glBufferSubData(GL_ARRAY_BUFFER, 0, count * sizeof(CircleInstance), circle_instances.data() + offset);
            glDrawArraysInstanced(GL_LINES, 0, 48, (GLsizei)count);
        }
    }

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glUseProgram(0);

    box_instances.clear();
    circle_instances.clear();
}
