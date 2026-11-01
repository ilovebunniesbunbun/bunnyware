#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/quaternion.hpp>
#include <cmath>
#include <chrono>
#include <cstring>
#include <algorithm>

inline glm::vec3 get_camera_position(const float* view_proj) {
    // view_proj is row-major. glm::make_mat4 treats it as column-major, transposing it.
    // Therefore, Column 2 of the true inverse matrix corresponds to Row 2 of the transposed inverse.
    glm::mat4 vp = glm::make_mat4(view_proj);
    glm::mat4 inv = glm::inverse(vp);
    glm::vec4 col2(inv[0][2], inv[1][2], inv[2][2], inv[3][2]);
    if (std::abs(col2.w) < 1e-5f) {
        return glm::vec3(0.0f, 0.0f, 0.0f);
    }
    return glm::vec3(col2) / col2.w;
}

struct DecomposedVP {
    glm::vec3 pos;
    glm::quat rot;
    float g_x;
    float g_y;
    float A;
    float B;
};

inline DecomposedVP decompose_vp(const float* vp) {
    // vp is Row-Major:
    // Row 0: vp[0..3]
    // Row 1: vp[4..7]
    // Row 2: vp[8..11]
    // Row 3: vp[12..15]
    glm::vec4 r0(vp[0], vp[1], vp[2], vp[3]);
    glm::vec4 r1(vp[4], vp[5], vp[6], vp[7]);
    glm::vec4 r2(vp[8], vp[9], vp[10], vp[11]);
    glm::vec4 r3(vp[12], vp[13], vp[14], vp[15]);

    float g_x = glm::length(glm::vec3(r0));
    float g_y = glm::length(glm::vec3(r1));

    glm::vec3 right = glm::normalize(glm::vec3(r0));
    glm::vec3 up = glm::normalize(glm::vec3(r1));
    glm::vec3 forward = glm::normalize(glm::vec3(r3));

    float T_x = r0.w / (g_x > 0.0001f ? g_x : 1.0f);
    float T_y = r1.w / (g_y > 0.0001f ? g_y : 1.0f);
    float T_z = r3.w;

    // pos = -(T_x * right + T_y * up + T_z * forward)
    glm::vec3 pos = -(T_x * right + T_y * up + T_z * forward);

    // Camera orientation matrix (orthogonal columns: Right, Up, -Forward)
    glm::mat3 R;
    R[0] = right;
    R[1] = up;
    R[2] = -forward;

    glm::quat rot = glm::quat_cast(R);

    // Extract A and B from r2 (r2 = -A * forward + B * (0,0,0,1))
    float A = -glm::dot(glm::vec3(r2), forward);
    float B = r2.w + A * r3.w;

    return {pos, rot, g_x, g_y, A, B};
}

inline void reconstruct_vp(const DecomposedVP& dec, float* out_vp) {
    glm::mat3 R = glm::mat3_cast(dec.rot);
    glm::vec3 right = R[0];
    glm::vec3 up = R[1];
    glm::vec3 forward = -R[2];

    glm::vec4 r0(right * dec.g_x, -glm::dot(right, dec.pos) * dec.g_x);
    glm::vec4 r1(up * dec.g_y, -glm::dot(up, dec.pos) * dec.g_y);
    
    glm::vec3 v2 = -forward;
    glm::vec4 r2(v2 * dec.A, glm::dot(forward, dec.pos) * dec.A + dec.B);
    glm::vec4 r3(forward, -glm::dot(forward, dec.pos));

    // Write out_vp as row-major
    out_vp[0] = r0.x; out_vp[1] = r0.y; out_vp[2] = r0.z; out_vp[3] = r0.w;
    out_vp[4] = r1.x; out_vp[5] = r1.y; out_vp[6] = r1.z; out_vp[7] = r1.w;
    out_vp[8] = r2.x; out_vp[9] = r2.y; out_vp[10] = r2.z; out_vp[11] = r2.w;
    out_vp[12] = r3.x; out_vp[13] = r3.y; out_vp[14] = r3.z; out_vp[15] = r3.w;
}

class ViewMatrixExtrapolator {
private:
    DecomposedVP prev_dec{};
    DecomposedVP curr_dec{};
    float curr_raw_matrix[16]{};
    std::chrono::high_resolution_clock::time_point prev_time;
    std::chrono::high_resolution_clock::time_point curr_time;
    bool has_prev = false;
    bool has_curr = false;

    bool matrix_equals(const float* a, const float* b) {
        for (int i = 0; i < 16; ++i) {
            if (std::abs(a[i] - b[i]) > 1e-7f) return false;
        }
        return true;
    }

public:
    void reset() {
        has_prev = false;
        has_curr = false;
    }

    void update(const float* matrix) {
        auto now = std::chrono::high_resolution_clock::now();
        if (has_curr) {
            if (matrix_equals(matrix, curr_raw_matrix)) {
                return;
            }
            prev_dec = curr_dec;
            prev_time = curr_time;
            has_prev = true;
        }
        std::memcpy(curr_raw_matrix, matrix, sizeof(float) * 16);
        curr_dec = decompose_vp(matrix);
        curr_time = now;
        has_curr = true;
    }

    void get_extrapolated_matrix(float* out_matrix) {
        auto now = std::chrono::high_resolution_clock::now();
        if (!has_curr) {
            std::memset(out_matrix, 0, sizeof(float) * 16);
            return;
        }
        if (!has_prev) {
            std::memcpy(out_matrix, curr_raw_matrix, sizeof(float) * 16);
            return;
        }

        auto dt_update = std::chrono::duration<double, std::milli>(curr_time - prev_time).count();
        auto dt_render = std::chrono::duration<double, std::milli>(now - curr_time).count();

        if (dt_update < 1.0 || dt_update > 200.0) {
            std::memcpy(out_matrix, curr_raw_matrix, sizeof(float) * 16);
            return;
        }

        double t = dt_render / dt_update;
        if (t > 2.0) t = 2.0;
        if (t < 0.0) t = 0.0;

        DecomposedVP extra_dec;
        // Extrapolate position linearly
        extra_dec.pos = curr_dec.pos + static_cast<float>(t) * (curr_dec.pos - prev_dec.pos);

        // Extrapolate rotation via SLERP
        glm::quat delta_rot = curr_dec.rot * glm::inverse(prev_dec.rot);
        extra_dec.rot = glm::slerp(curr_dec.rot, delta_rot * curr_dec.rot, static_cast<float>(t));

        // Keep current projection settings static
        extra_dec.g_x = curr_dec.g_x;
        extra_dec.g_y = curr_dec.g_y;
        extra_dec.A = curr_dec.A;
        extra_dec.B = curr_dec.B;

        reconstruct_vp(extra_dec, out_matrix);
    }
};
