#pragma once

// Pure, host-independent helpers for the in-process offset refresher:
// byte-pattern parsing/scanning and the cs2-linux operation-chain semantics.
// Kept header-only so unit tests can exercise them without a live game process.

#include <cstdint>
#include <cstring>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace offset_scanner {

struct Pattern {
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> wildcards; // 1 = wildcard byte
    size_t anchor_index = 0;
    uint8_t anchor_value = 0;

    bool parse(const std::string& text, std::string* error = nullptr)
    {
        bytes.clear();
        wildcards.clear();
        anchor_index = 0;
        anchor_value = 0;

        std::istringstream stream(text);
        std::string token;
        while (stream >> token) {
            if (token == "?" || token == "??") {
                bytes.push_back(0);
                wildcards.push_back(1);
                continue;
            }
            if (token.size() > 2) {
                if (error) *error = "invalid pattern token '" + token + "'";
                return false;
            }
            uint8_t value = 0;
            for (const char c : token) {
                int digit = -1;
                if (c >= '0' && c <= '9') digit = c - '0';
                else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
                if (digit < 0) {
                    if (error) *error = "invalid hex digit in token '" + token + "'";
                    return false;
                }
                value = static_cast<uint8_t>((value << 4) | digit);
            }
            bytes.push_back(value);
            wildcards.push_back(0);
        }

        if (bytes.empty()) {
            if (error) *error = "empty pattern";
            return false;
        }

        bool anchored = false;
        for (size_t i = 0; i < wildcards.size(); ++i) {
            if (!wildcards[i]) {
                anchor_index = i;
                anchor_value = bytes[i];
                anchored = true;
                break;
            }
        }
        (void)anchored; // an all-wildcard pattern simply anchors at index 0
        return true;
    }

    bool empty() const { return bytes.empty(); }

    const uint8_t* find(const uint8_t* data, size_t size) const
    {
        if (bytes.empty() || data == nullptr || size < bytes.size()) {
            return nullptr;
        }

        const size_t pattern_size = bytes.size();
        const size_t last_start = size - pattern_size;
        size_t search = 0;
        while (search <= last_start) {
            const void* hit = std::memchr(data + search + anchor_index, anchor_value,
                                          last_start - search + 1);
            if (hit == nullptr) {
                return nullptr;
            }
            const size_t start = static_cast<size_t>(static_cast<const uint8_t*>(hit) - data) -
                                 anchor_index;
            bool match = true;
            for (size_t i = 0; i < pattern_size; ++i) {
                if (!wildcards[i] && data[start + i] != bytes[i]) {
                    match = false;
                    break;
                }
            }
            if (match) {
                return data + start;
            }
            search = start + 1;
        }
        return nullptr;
    }
};

enum class OperationKind { add, sub, rip, read, slice };

// Mirrors the cs2-linux operation chain:
//   add/sub: adjust the working address.
//   rip:     disp32 at `result + offset`; result = result + disp + length.
//   read:    result = *(uint64_t*)result.
//   slice:   result = little-endian integer from bytes [result+value, result+length).
// The final value is converted to uint32_t RVA against module_base (or kept as a
// small constant when it is below the module base, as cs2-linux does).
struct Operation {
    OperationKind kind = OperationKind::rip;
    uint64_t value = 0;   // add/sub operand, or slice start
    uint64_t offset = 3;  // rip displacement field offset
    uint64_t length = 7;  // rip instruction length, or slice end
};

template <typename ReadFn>
bool apply_operations(uintptr_t result, const std::vector<Operation>& operations, ReadFn&& read,
                      uintptr_t module_base, uint32_t* out_rva)
{
    if (out_rva == nullptr) {
        return false;
    }

    for (const Operation& operation : operations) {
        switch (operation.kind) {
            case OperationKind::add:
                result += operation.value;
                break;
            case OperationKind::sub:
                result -= operation.value;
                break;
            case OperationKind::rip: {
                int32_t displacement = 0;
                if (!read(result + operation.offset, &displacement, sizeof(displacement))) {
                    return false;
                }
                result = result + static_cast<uint64_t>(static_cast<int64_t>(displacement)) +
                         operation.length;
                break;
            }
            case OperationKind::read: {
                uint64_t value = 0;
                if (!read(result, &value, sizeof(value))) {
                    return false;
                }
                result = value;
                break;
            }
            case OperationKind::slice: {
                if (operation.length <= operation.value || operation.length - operation.value > 8) {
                    return false;
                }
                uint8_t buffer[8] = {0};
                const size_t slice_size = static_cast<size_t>(operation.length - operation.value);
                if (!read(result + operation.value, buffer, slice_size)) {
                    return false;
                }
                uint64_t value = 0;
                std::memcpy(&value, buffer, slice_size);
                result = value;
                break;
            }
        }
    }

    *out_rva = (result >= module_base) ? static_cast<uint32_t>(result - module_base)
                                       : static_cast<uint32_t>(result);
    return true;
}

} // namespace offset_scanner
