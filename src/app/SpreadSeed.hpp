#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

namespace spread {

inline float normalize_angle(float a) { return a - std::floor(a * 0.0027777778f + 0.5f) * 360.0f; }
inline float quantize_angle(float a) { return std::floor(normalize_angle(a) * 2.0f) * 0.5f; }

inline float bucket_margin(float a)
{
    const float n = normalize_angle(a);
    const float inside = n - std::floor(n * 2.0f) * 0.5f;
    return std::fmin(inside, 0.5f - inside);
}

inline std::uint32_t sha1_seed(float pitch, float yaw, std::int32_t tick)
{
    std::uint32_t parts[3];
    std::memcpy(&parts[0], &pitch, 4);
    std::memcpy(&parts[1], &yaw, 4);
    std::memcpy(&parts[2], &tick, 4);
    std::uint8_t block[64] = {};
    for (int i = 0; i < 3; ++i)
        for (int b = 0; b < 4; ++b) block[i * 4 + b] = std::uint8_t(parts[i] >> (8 * b));
    block[12] = 0x80;
    block[63] = 96;
    std::uint32_t w[80];
    for (int i = 0; i < 16; ++i)
        w[i] = std::uint32_t(block[i * 4]) << 24 | std::uint32_t(block[i * 4 + 1]) << 16 |
            std::uint32_t(block[i * 4 + 2]) << 8 | std::uint32_t(block[i * 4 + 3]);
    const auto rotl = [](std::uint32_t v, unsigned s) { return (v << s) | (v >> (32u - s)); };
    for (int i = 16; i < 80; ++i) w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    std::uint32_t a = 0x67452301u, b = 0xEFCDAB89u, c = 0x98BADCFEu, d = 0x10325476u, e = 0xC3D2E1F0u;
    for (int i = 0; i < 80; ++i) {
        std::uint32_t f, k;
        if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999u;
        else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1u;
        else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDCu;
        else f = b ^ c ^ d, k = 0xCA62C1D6u;
        const std::uint32_t t = rotl(a, 5) + f + e + k + w[i];
        e = d, d = c, c = rotl(b, 30), b = a, a = t;
    }
    const std::uint32_t h0 = 0x67452301u + a;
    return (h0 >> 24) | ((h0 >> 8) & 0xff00u) | ((h0 << 8) & 0xff0000u) | (h0 << 24);
}

inline std::uint32_t shot_seed(float hash_pitch, float hash_yaw, std::int32_t tick)
{
    return sha1_seed(quantize_angle(hash_pitch), quantize_angle(hash_yaw), tick);
}

class ValveRandomStream {
public:
    void set_seed(std::int32_t seed)
    {
        idum_ = seed >= 0 ? -seed : seed;
        if (idum_ == 0) idum_ = -1;
        iy_ = 0;
        std::memset(iv_, 0, sizeof(iv_));
    }
    std::int32_t generate()
    {
        if (idum_ <= 0 || iy_ == 0) {
            std::int64_t v = -std::int64_t(idum_);
            if (v < 1 || v > 2147483647) v = 1;
            std::int32_t s = std::int32_t(v);
            for (int j = 39; j >= 0; --j) {
                s = lcg(s);
                if (j < 32) iv_[j] = s;
            }
            idum_ = s;
            iy_ = iv_[0];
        }
        idum_ = lcg(idum_);
        const std::int32_t idx = iy_ / 0x4000000;
        iy_ = iv_[idx];
        iv_[idx] = idum_;
        return iy_;
    }
    float random_float(float lo = 0.0f, float hi = 1.0f)
    {
        constexpr float am = 4.6566129e-10f, rnmx = 0.99999988f;
        float fl = am * float(generate());
        if (fl > rnmx) fl = rnmx;
        return lo + fl * (hi - lo);
    }

private:
    static std::int32_t lcg(std::int32_t s)
    {
        const std::int32_t k = s / 127773;
        std::int32_t r = 16807 * (s - k * 127773) - 2836 * k;
        if (r < 0) r += 2147483647;
        return r;
    }
    std::int32_t idum_ = 0, iy_ = 0, iv_[32] = {};
};

struct WeaponSpread {
    float total = 0;
    float base = 0;
    float recoil_index = 0;
    int weapon_id = 0;
    int num_bullets = 1;
    int spread_seed = 0;
};

inline std::uint32_t pellet_seed(std::uint32_t seed, int pellet, int num_bullets, int spread_seed)
{
    const std::uint32_t s = seed + 1u;
    if (num_bullets > 1 && spread_seed > 0) return (s & 255u) + std::uint32_t(pellet) * std::uint32_t(spread_seed);
    return s;
}

struct Offset {
    float x = 0, y = 0;
};
inline Offset spread_offset(std::uint32_t seed, int pellet, const WeaponSpread& w)
{
    constexpr float two_pi = 6.28318530717958647692f;
    ValveRandomStream prng;
    prng.set_seed(std::int32_t(pellet_seed(seed, pellet, w.num_bullets, w.spread_seed)));
    float r1 = prng.random_float(0.0f, 1.0f);
    const float r2 = prng.random_float(0.0f, two_pi);
    float r3 = prng.random_float(0.0f, 1.0f);
    const float r4 = prng.random_float(0.0f, two_pi);
    if (w.weapon_id == 28 && w.recoil_index < 3.0f) {
        float v1 = r1, v3 = r3;
        int c = 3;
        do {
            --c;
            v1 *= v1;
            v3 *= v3;
        } while (float(c) > w.recoil_index);
        r1 = 1.0f - v1;
        r3 = 1.0f - v3;
    }
    if (w.weapon_id == 64 && w.num_bullets == 1) {
        r1 = 1.0f - r1 * r1;
        r3 = 1.0f - r3 * r3;
    }
    const float inaccuracy = std::fmax(0.0f, w.total - w.base);
    Offset o;
    o.x = std::cos(r2) * (r1 * inaccuracy) + std::cos(r4) * (r3 * w.base);
    o.y = std::sin(r2) * (r1 * inaccuracy) + std::sin(r4) * (r3 * w.base);
    return o;
}

struct Basis {
    float forward[3], right[3], up[3];
};
inline Basis view_basis(float pitch_deg, float yaw_deg)
{
    constexpr float k = 3.14159265358979f / 180.0f;
    const float sp = std::sin(pitch_deg * k), cp = std::cos(pitch_deg * k);
    const float sy = std::sin(yaw_deg * k), cy = std::cos(yaw_deg * k);
    return {{cp * cy, cp * sy, -sp}, {-sy, cy, 0.0f}, {sp * cy, sp * sy, cp}};
}
inline void spread_direction(const Basis& b, Offset o, float out[3])
{
    float l2 = 0;
    for (int i = 0; i < 3; ++i) {
        out[i] = b.forward[i] - b.right[i] * o.x + b.up[i] * o.y;
        l2 += out[i] * out[i];
    }
    const float l = std::sqrt(l2);
    if (l > 1e-6f && std::isfinite(l))
        for (int i = 0; i < 3; ++i) out[i] /= l;
}

}
