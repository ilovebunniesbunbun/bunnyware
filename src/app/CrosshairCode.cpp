#include "CrosshairCode.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace crosshair {
namespace {

constexpr std::string_view kDictionary =
    "ABCDEFGHJKLMNOPQRSTUVWXYZabcdefhijkmnopqrstuvwxyz23456789";

float decode_signed_tenth(uint8_t value)
{
    const int signed_value = value >= 128 ? static_cast<int>(value) - 256
                                          : static_cast<int>(value);
    return static_cast<float>(signed_value) / 10.0f;
}

float custom_round(float value)
{
    float integral = 0.0f;
    const float fractional = std::modf(value, &integral);

    if (fractional >= 0.9f) return integral + 1.0f;
    if (fractional <= 0.1f) return integral;
    if (fractional >= 0.6f && fractional <= 0.8f) return integral + 0.8f;
    if (fractional >= 0.2f && fractional <= 0.5f) return integral + 0.5f;
    return value;
}

} // namespace

std::optional<Settings> decode(std::string_view code)
{
    if (!code.starts_with("CSGO") || code.size() < 5) {
        return std::nullopt;
    }

    std::string characters;
    characters.reserve(code.size() - 5);
    for (char character : code.substr(5)) {
        if (character != '-') {
            characters.push_back(character);
        }
    }
    if (characters.empty()) {
        return std::nullopt;
    }

    std::vector<uint8_t> bytes{0};
    for (auto it = characters.rbegin(); it != characters.rend(); ++it) {
        const size_t dictionary_index = kDictionary.find(*it);
        if (dictionary_index == std::string_view::npos) {
            return std::nullopt;
        }

        unsigned int carry = static_cast<unsigned int>(dictionary_index);
        for (uint8_t& byte : bytes) {
            const unsigned int result = static_cast<unsigned int>(byte) * 57U + carry;
            byte = static_cast<uint8_t>(result % 256U);
            carry = result / 256U;
        }
        while (carry > 0) {
            bytes.push_back(static_cast<uint8_t>(carry % 256U));
            carry /= 256U;
        }
    }

    if (bytes.size() > 18) {
        return std::nullopt;
    }
    bytes.resize(18, 0);

    std::array<uint8_t, 18> decoded{};
    for (size_t i = 0; i < decoded.size(); ++i) {
        decoded[i] = bytes[decoded.size() - 1 - i];
    }

    unsigned int checksum = 0;
    for (size_t i = 1; i < decoded.size(); ++i) {
        checksum += decoded[i];
    }
    if (decoded[0] != static_cast<uint8_t>(checksum % 256U)) {
        return std::nullopt;
    }

    Settings settings;
    settings.gap = decode_signed_tenth(decoded[2]);
    settings.outline_thickness = static_cast<float>(decoded[3]) / 2.0f;
    settings.red = decoded[4];
    settings.green = decoded[5];
    settings.blue = decoded[6];
    settings.alpha = decoded[7];
    settings.dynamic_split_distance = decoded[8] & 0x7fU;
    settings.recoil = ((decoded[8] >> 4U) & 8U) == 8U;
    settings.fixed_gap = decode_signed_tenth(decoded[9]);
    settings.color_index = decoded[10] & 7U;
    settings.draw_outline = (decoded[10] & 8U) == 8U;
    settings.dynamic_split_alpha_inner =
        static_cast<float>(decoded[10] >> 4U) / 10.0f;
    settings.dynamic_split_alpha_outer =
        static_cast<float>(decoded[11] & 0x0fU) / 10.0f;
    settings.dynamic_max_distance_split_ratio =
        static_cast<float>(decoded[11] >> 4U) / 10.0f;
    settings.thickness = static_cast<float>(decoded[12]) / 10.0f;
    settings.style = (decoded[13] & 0x0fU) >> 1U;
    settings.dot = ((decoded[13] >> 4U) & 1U) == 1U;
    settings.gap_uses_weapon_value = ((decoded[13] >> 4U) & 2U) == 2U;
    settings.use_alpha = ((decoded[13] >> 4U) & 4U) == 4U;
    settings.t_style = ((decoded[13] >> 4U) & 8U) == 8U;

    const unsigned int size_high = decoded[15] & 0x1fU;
    settings.size =
        static_cast<float>((size_high << 8U) + decoded[14]) / 10.0f;
    return settings;
}

Geometry build_geometry(const Settings& settings, int width, int height)
{
    Geometry result;
    result.color = {settings.red, settings.green, settings.blue, settings.alpha};

    if (width <= 0 || height <= 0) {
        return result;
    }

    const float scale = static_cast<float>(height) / 720.0f;
    const float rounded_size = custom_round(settings.size) * 2.0f;
    const float rounded_thickness = custom_round(settings.thickness) * 2.0f;

    int length = static_cast<int>(std::round(rounded_size * scale));
    int thickness = static_cast<int>(std::round(rounded_thickness * scale));

    if (settings.size > 0.0f && length < 1) length = 1;
    // CS2 treats a configured thickness of zero as a one-pixel raster line.
    if (thickness < 1) thickness = 1;

    const int gap = static_cast<int>(std::round(settings.gap * 2.0f * scale));
    const int center_x = static_cast<int>(std::floor(static_cast<float>(width) / 2.0f));
    const int center_y = static_cast<int>(std::floor(static_cast<float>(height) / 2.0f));
    const int axis_start = -static_cast<int>(
        std::floor(static_cast<float>(thickness) / 2.0f));
    const int axis_end = axis_start + thickness;

    auto add_rect = [&](int x_offset, int y_offset, int rect_width, int rect_height) {
        const int x1 = center_x + x_offset;
        const int y1 = center_y + y_offset;
        result.rectangles.push_back({x1, y1, x1 + rect_width, y1 + rect_height});
    };

    // Place each arm outside the complete center axis. This matters for odd
    // raster thicknesses: with a one-pixel axis, the right/bottom edge is at
    // +1 rather than 0. Starting at 0 overlaps those two arms with the center
    // pixel and makes the crosshair visibly lopsided after a stretched scale.
    add_rect(axis_end + gap, axis_start, length, thickness);
    add_rect(axis_start - gap - length, axis_start, length, thickness);
    add_rect(axis_start, axis_end + gap, thickness, length);
    if (!settings.t_style) {
        add_rect(axis_start, axis_start - gap - length, thickness, length);
    }
    if (settings.dot) {
        add_rect(axis_start, axis_start, thickness, thickness);
    }

    return result;
}

} // namespace crosshair
