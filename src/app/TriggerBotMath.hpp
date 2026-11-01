#pragma once
#include <cstdint>
#include <cmath>
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
// Robust angle normalization and quantization
// ─────────────────────────────────────────────────────────────────────────────
inline float normalize_angle(float a) {
    return a - std::floor(a * 0.0027777778f + 0.5f) * 360.0f;
}

inline float quantize_angle(float a) {
    return std::floor(normalize_angle(a) * 2.0f) * 0.5f;
}

// ─────────────────────────────────────────────────────────────────────────────
// One-shot SHA-1 transform for exactly 12 bytes
// ─────────────────────────────────────────────────────────────────────────────
inline uint32_t sha1_transform_12bytes(float pitch, float yaw, int tick) {
    uint8_t buffer[64] = {0};
    std::memcpy(&buffer[0], &pitch, 4);
    std::memcpy(&buffer[4], &yaw, 4);
    std::memcpy(&buffer[8], &tick, 4);

    uint32_t w[80] = {0};
    for (int i = 0; i < 3; ++i) {
        w[i] = ((uint32_t)buffer[i * 4 + 0] << 24) |
               ((uint32_t)buffer[i * 4 + 1] << 16) |
               ((uint32_t)buffer[i * 4 + 2] << 8)  |
               ((uint32_t)buffer[i * 4 + 3]);
    }
    w[3] = 0x80000000;
    w[15] = 96; // 12 bytes = 96 bits

    auto rotl = [](uint32_t value, unsigned int shift) -> uint32_t {
        return (value << shift) | (value >> (32 - shift));
    };

    for (int i = 16; i < 80; ++i) {
        w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    uint32_t h0 = 0x67452301;
    uint32_t h1 = 0xEFCDAB89;
    uint32_t h2 = 0x98BADCFE;
    uint32_t h3 = 0x10325476;
    uint32_t h4 = 0xC3D2E1F0;

    uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;

    for (int i = 0; i < 80; ++i) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }

        uint32_t temp = rotl(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rotl(b, 30);
        b = a;
        a = temp;
    }

    h0 += a;
    h1 += b;
    h2 += c;
    h3 += d;
    h4 += e;

    uint32_t seed = ((h0 & 0xff000000) >> 24) |
                    ((h0 & 0x00ff0000) >> 8)  |
                    ((h0 & 0x0000ff00) << 8)  |
                    ((h0 & 0x000000ff) << 24);
    return seed;
}

// ─────────────────────────────────────────────────────────────────────────────
// Park-Miller LCG with Bays-Durham shuffle table
// ─────────────────────────────────────────────────────────────────────────────
class ValveRandomStream {
    int32_t m_idum = 0;
    int32_t m_iy = 0;
    int32_t m_iv[32] = {0};
    
    static int32_t lcg(int32_t state) {
        const int32_t k = state / 127773;
        int32_t result = 16807 * (state - k * 127773) - 2836 * k;
        if (result < 0) result += 2147483647;
        return result;
    }
    
public:
    void set_seed(int32_t seed) {
        m_idum = seed >= 0 ? -seed : seed;
        if (m_idum == 0) m_idum = -1;
        m_iy = 0;
        std::memset(m_iv, 0, sizeof(m_iv));
    }
    
    int32_t generate() {
        if (m_idum <= 0 || m_iy == 0) {
            int32_t v = -m_idum;
            if (v < 1) v = 1;
            for (int j = 39; j >= 0; --j) {
                v = lcg(v);
                if (j < 32) m_iv[j] = v;
            }
            m_idum = v;
            m_iy = m_iv[0];
        }
        m_idum = lcg(m_idum);
        const int32_t idx = m_iy / 0x4000000;
        m_iy = m_iv[idx];
        m_iv[idx] = m_idum;
        return m_iy;
    }
    
    float random_float(float min_val = 0.0f, float max_val = 1.0f) {
        constexpr float AM = 4.6566129e-10f;
        constexpr float RNMX = 0.99999988f;
        float fl = AM * generate();
        if (fl > RNMX) fl = RNMX;
        return min_val + fl * (max_val - min_val);
    }
};
