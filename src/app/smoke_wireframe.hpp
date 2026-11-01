#pragma once
// Smoke grenade voxel wireframe ("cage") extraction.
//
// A smoke grenade's volume is a 32^3 voxel grid stored as a morton-order bitset
// plus a dense alpha (density) buffer. Instead of tracing the voxel grid itself,
// the cage builds one contour ring per occupied voxel layer (Z slice): the ring
// follows the centroid of the dense cells and the support distance in a fixed
// set of directions, inflated to the outer voxel faces. Consecutive layers are
// connected with vertical struts, producing a smooth, smoke-shaped net - a
// sphere-like blob yields circle rings, irregular smoke bends them accordingly.
// The bottom is never capped.
//
// Only voxels at or above kSmokeAlphaThreshold contribute, matching the
// per-voxel density cutoff the smoke vischeck uses.
//
// This header is GL-free so tests/cpp/test_main.cpp can validate the extraction.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "overlay/game_reader.hpp"

namespace smoke_wireframe {

// World-space edge of one voxel cell. Matches the depth prepass smoke cubes and
// the debug-visibility view (20.0f per voxel).
constexpr float kVoxelSpacing = 20.0f;
// Voxel index [0, 31] maps to (index - 16) * 20.0f around the detonation center.
constexpr float kGridHalfExtent = 16.0f;
// Density threshold for treating a voxel as smoke. Mirrors the per-voxel cutoff
// in VisibilityWorker::line_goes_through_smoke ("alpha < 15" is ignored there)
// and the depth prepass fragment shader (15.0 / 255.0).
constexpr uint8_t kSmokeAlphaThreshold = 15;
// Angular resolution of each contour ring.
constexpr int kCageRingSegments = 24;
// Upper bound on cage line segments emitted per frame across all smokes. The
// contour cage needs at most kCageRingSegments * (2 * 32 - 1) ~ 1500 lines per
// smoke; this only guards against pathological input.
constexpr std::size_t kMaxCageLines = 8192;
// Upper bound on cage shell triangles emitted per frame across all smokes.
constexpr std::size_t kMaxCageTriangles = 4096;

struct CageLine {
    Vec3 a;
    Vec3 b;
};

struct CageRing {
    Vec3 points[kCageRingSegments];
};

// 32 voxel layers plus one extra slot for the single-layer cylinder case.
constexpr int kMaxCageRings = 33;

inline uint32_t compact1(uint32_t v)
{
    v &= 0x09249249U;
    v = (v ^ (v >> 2)) & 0x030C30C3U;
    v = (v ^ (v >> 4)) & 0x0300F00FU;
    v = (v ^ (v >> 8)) & 0x00000FFFU;
    return v;
}

inline void morton3_decode(uint32_t m, int& x, int& y, int& z)
{
    x = static_cast<int>(compact1(m));
    y = static_cast<int>(compact1(m >> 1));
    z = static_cast<int>(compact1(m >> 2));
}

// True when the voxel exists in the 32^3 grid and its density is at or above the
// smoke threshold. Out-of-grid neighbors are empty.
inline bool voxel_is_visible(const ActiveSmokeData& smoke, int x, int y, int z)
{
    if (x < 0 || x > 31 || y < 0 || y > 31 || z < 0 || z > 31) return false;
    return smoke.alphas[x + 32 * (y + 32 * z)] >= kSmokeAlphaThreshold;
}

// Builds the ordered, bottom-to-top contour rings of the smoke shape. The first
// and last rings are extended to the outer voxel faces so the cage spans the
// smoke's full height; a smoke with a single dense layer becomes a two-ring
// cylinder. Returns the number of rings written (0 when no dense voxels exist).
inline int build_cage_rings(const ActiveSmokeData& smoke, CageRing* rings_out, int max_rings)
{
    // Pass 1: per-layer dense cell counts and coordinate sums (for centroids),
    // so the cells can be bucketed into a single flat allocation afterwards.
    uint32_t counts[32] = {};
    float sum_x[32] = {};
    float sum_y[32] = {};

    for (uint32_t w = 0; w < 512; ++w) {
        uint64_t bits = smoke.bitset[w];
        while (bits) {
            const uint32_t bit_idx = static_cast<uint32_t>(__builtin_ctzll(bits));
            bits &= (bits - 1);

            const uint32_t morton = (w << 6) | bit_idx;
            int x = 0, y = 0, z = 0;
            morton3_decode(morton, x, y, z);
            if (!voxel_is_visible(smoke, x, y, z)) continue;

            counts[z] += 1;
            sum_x[z] += smoke.center.x + (static_cast<float>(x) - kGridHalfExtent) * kVoxelSpacing;
            sum_y[z] += smoke.center.y + (static_cast<float>(y) - kGridHalfExtent) * kVoxelSpacing;
        }
    }

    uint32_t layer_offset[32];
    uint32_t total_cells = 0;
    for (int z = 0; z < 32; ++z) {
        layer_offset[z] = total_cells;
        total_cells += counts[z];
    }
    if (total_cells == 0) return 0;

    // Pass 2: place the dense cell centers into their layer's slice.
    struct Cell {
        float x;
        float y;
    };
    std::vector<Cell> cells(total_cells);
    uint32_t cursor[32];
    for (int z = 0; z < 32; ++z) {
        cursor[z] = layer_offset[z];
    }

    for (uint32_t w = 0; w < 512; ++w) {
        uint64_t bits = smoke.bitset[w];
        while (bits) {
            const uint32_t bit_idx = static_cast<uint32_t>(__builtin_ctzll(bits));
            bits &= (bits - 1);

            const uint32_t morton = (w << 6) | bit_idx;
            int x = 0, y = 0, z = 0;
            morton3_decode(morton, x, y, z);
            if (!voxel_is_visible(smoke, x, y, z)) continue;

            cells[cursor[z]++] = Cell{
                smoke.center.x + (static_cast<float>(x) - kGridHalfExtent) * kVoxelSpacing,
                smoke.center.y + (static_cast<float>(y) - kGridHalfExtent) * kVoxelSpacing,
            };
        }
    }

    // Contour per layer: centroid plus the support distance in fixed directions,
    // inflated by the cell square so rings hug the outer faces.
    constexpr float kTwoPi = 6.28318530718f;
    const float half_spacing = kVoxelSpacing * 0.5f;

    int ring_count = 0;
    for (int z = 0; z < 32 && ring_count < max_rings; ++z) {
        if (counts[z] == 0) continue;

        const float inv_count = 1.0f / static_cast<float>(counts[z]);
        const float cx = sum_x[z] * inv_count;
        const float cy = sum_y[z] * inv_count;
        const float world_z = smoke.center.z + (static_cast<float>(z) - kGridHalfExtent) * kVoxelSpacing;
        const Cell* layer = cells.data() + layer_offset[z];

        CageRing& ring = rings_out[ring_count++];
        for (int i = 0; i < kCageRingSegments; ++i) {
            const float angle = kTwoPi * static_cast<float>(i) / static_cast<float>(kCageRingSegments);
            const float dir_x = std::cos(angle);
            const float dir_y = std::sin(angle);

            float support = -1e30f;
            for (uint32_t c = 0; c < counts[z]; ++c) {
                support = std::max(support, (layer[c].x - cx) * dir_x + (layer[c].y - cy) * dir_y);
            }
            const float radius = support + half_spacing * (std::abs(dir_x) + std::abs(dir_y));
            ring.points[i] = Vec3{cx + dir_x * radius, cy + dir_y * radius, world_z};
        }
    }
    if (ring_count == 0) return 0;

    // Extend the outermost rings to the true top and bottom voxel faces.
    if (ring_count == 1) {
        if (max_rings < 2) return 1;
        rings_out[1] = rings_out[0];
        for (int i = 0; i < kCageRingSegments; ++i) {
            rings_out[0].points[i].z -= half_spacing;
            rings_out[1].points[i].z += half_spacing;
        }
        return 2;
    }
    for (int i = 0; i < kCageRingSegments; ++i) {
        rings_out[0].points[i].z -= half_spacing;
        rings_out[ring_count - 1].points[i].z += half_spacing;
    }
    return ring_count;
}

// Appends the wireframe segments for the given rings: one closed contour per
// ring plus vertical struts between consecutive rings. The bottom is never
// capped.
inline void collect_cage_lines_from_rings(const CageRing* rings, int ring_count, std::vector<CageLine>& out,
                                         std::size_t max_lines)
{
    for (int r = 0; r < ring_count; ++r) {
        const CageRing& ring = rings[r];
        for (int i = 0; i < kCageRingSegments; ++i) {
            if (out.size() >= max_lines) return;
            const int j = (i + 1) % kCageRingSegments;
            out.push_back(CageLine{ring.points[i], ring.points[j]});
            if (r > 0) {
                if (out.size() >= max_lines) return;
                out.push_back(CageLine{rings[r - 1].points[i], ring.points[i]});
            }
        }
    }
}

// Appends the filled contour shell for the given rings as a flat list of
// vertices, three per triangle (two triangles per ring gap segment). The
// through-smokes pass stamps this shell into the stencil buffer so its scope
// matches the wireframe cage exactly. The shell is open at the bottom.
inline void collect_cage_triangles_from_rings(const CageRing* rings, int ring_count, std::vector<Vec3>& out,
                                              std::size_t max_triangles)
{
    if (ring_count < 2) return;

    const std::size_t max_vertices = max_triangles * 3;
    for (int r = 1; r < ring_count; ++r) {
        const CageRing& low = rings[r - 1];
        const CageRing& high = rings[r];
        for (int i = 0; i < kCageRingSegments; ++i) {
            if (out.size() + 6 > max_vertices) return;
            const int j = (i + 1) % kCageRingSegments;
            out.push_back(low.points[i]);
            out.push_back(low.points[j]);
            out.push_back(high.points[j]);
            out.push_back(low.points[i]);
            out.push_back(high.points[j]);
            out.push_back(high.points[i]);
        }
    }
}

// Convenience wrappers that build the rings themselves. Renderers that need
// both the lines and the shell should build the rings once and call the
// *_from_rings variants instead.
inline void collect_cage_lines(const ActiveSmokeData& smoke, std::vector<CageLine>& out,
                               std::size_t max_lines = kMaxCageLines)
{
    if (!smoke.active) return;

    CageRing rings[kMaxCageRings];
    const int ring_count = build_cage_rings(smoke, rings, kMaxCageRings);
    collect_cage_lines_from_rings(rings, ring_count, out, max_lines);
}

inline void collect_cage_triangles(const ActiveSmokeData& smoke, std::vector<Vec3>& out,
                                   std::size_t max_triangles = kMaxCageTriangles)
{
    if (!smoke.active) return;

    CageRing rings[kMaxCageRings];
    const int ring_count = build_cage_rings(smoke, rings, kMaxCageRings);
    collect_cage_triangles_from_rings(rings, ring_count, out, max_triangles);
}

}  // namespace smoke_wireframe
