#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <type_traits>
#include <vector>

// Bullet Impacts: the local player's native client bullet impacts (ported from MI). The
// game's C_CSPlayerPawn bullet services keep a vector of client impact endpoints that
// already include recoil and spread, so nothing is rebuilt from view angles or seeds.
namespace impacts {

inline constexpr std::size_t tail_limit = 128;
inline constexpr std::int32_t allocation_limit = 1'000'000;
inline constexpr double acquisition_limit = 0.050;
inline constexpr double freshness_limit = 0.150;
inline constexpr double future_tolerance = 0.002;
inline constexpr double native_expiry_duration = 10.0;
inline constexpr std::uint32_t services_owner_offset = 0x38;
inline constexpr std::uint32_t impacts_offset_limit = 0x1000;

// Built only by layout_from_offsets. A zero layout is unavailable.
// expected_vtable is absolute; globals_slot is a client RVA.
struct Layout {
    std::uintptr_t client_base = 0;
    std::uint32_t services_offset = 0;
    std::uintptr_t expected_vtable = 0;
    std::uintptr_t globals_slot = 0;
    std::uint32_t impacts_offset = 0;
};

struct Record {
    float x = 0, y = 0, z = 0;
    float copied_clock = 0; // Native globals +0, not a shot tick or unique ID.
    float expires = 0;      // Native current time + 10 seconds.
};
static_assert(std::is_trivially_copyable_v<Record> && sizeof(Record) == 0x14);
static_assert(offsetof(Record, copied_clock) == 0x0c && offsetof(Record, expires) == 0x10);

struct VectorHeader {
    std::int32_t count = 0;
    std::uint32_t padding = 0;
    std::uintptr_t data = 0;
    std::int32_t capacity = 0;
    std::uint32_t flags = 0;
};
static_assert(sizeof(std::uintptr_t) == 8 && sizeof(VectorHeader) == 0x18);
static_assert(offsetof(VectorHeader, data) == 8 && offsetof(VectorHeader, capacity) == 0x10);

// The alive local pawn of one attachment. Any change re-primes the consumer.
struct Subject {
    int pid = 0;
    std::uintptr_t client_base = 0;
    std::uintptr_t pawn = 0;
    bool valid() const noexcept { return pid > 0 && client_base && pawn; }
    bool operator==(const Subject&) const = default;
};

struct Snapshot {
    Subject subject{};
    std::uintptr_t services = 0;
    VectorHeader header{};
    std::uint32_t first_index = 0, record_count = 0;
    std::array<Record, tail_limit> records{};
    double begin = 0, end = 0; // Host steady-clock acquisition interval.
    float realtime_begin = 0, realtime_end = 0;
    float current_time_begin = 0, current_time_end = 0;
};
using SnapshotPtr = std::shared_ptr<const Snapshot>;

inline bool span(std::uintptr_t base, std::size_t size) noexcept {
    return base && size && base <= std::numeric_limits<std::uintptr_t>::max() - (size - 1);
}
inline bool address(std::uintptr_t base, std::uintptr_t offset,
                    std::size_t size, std::uintptr_t& out) noexcept {
    if (!base || offset > std::numeric_limits<std::uintptr_t>::max() - base) return false;
    out = base + offset;
    return span(out, size);
}
inline std::size_t services_span(const Layout& layout) noexcept {
    return std::max<std::size_t>(services_owner_offset + sizeof(std::uintptr_t),
                                 layout.impacts_offset + sizeof(VectorHeader));
}

// The offset refresher resolves the three client values by signature
// (dwBulletServicesVTable, dwBulletImpactGlobals, dwBulletServices_impacts). The
// producer pattern pins the record layout, the globals clock fields and the vector
// members, so a changed producer fails to resolve. Any missing, zero or implausible
// value leaves impacts unavailable.
inline std::optional<Layout> layout_from_offsets(std::uintptr_t client_base, std::uint32_t services_offset,
                                                 std::uintptr_t vtable_rva, std::uintptr_t globals_rva,
                                                 std::uintptr_t impacts_offset) noexcept {
    std::uintptr_t vtable = 0, globals = 0;
    if (!client_base || !services_offset || !vtable_rva || !globals_rva || !impacts_offset ||
        services_offset % alignof(std::uintptr_t) || vtable_rva % alignof(std::uintptr_t) ||
        globals_rva % alignof(std::uintptr_t) || impacts_offset % alignof(std::uintptr_t) ||
        impacts_offset > impacts_offset_limit ||
        (impacts_offset < services_owner_offset + sizeof(std::uintptr_t) &&
         impacts_offset + sizeof(VectorHeader) > services_owner_offset) ||
        !address(client_base, vtable_rva, sizeof(std::uintptr_t), vtable) ||
        !address(client_base, globals_rva, sizeof(std::uintptr_t), globals)) return {};
    return Layout{client_base, services_offset, vtable, globals_rva,
                  static_cast<std::uint32_t>(impacts_offset)};
}
inline bool valid_header(const VectorHeader& h) noexcept {
    if (h.count < 0 || h.capacity < h.count || h.capacity < 0 ||
        h.count > allocation_limit || h.capacity > allocation_limit) return false;
    if (!h.capacity) return !h.data;
    return h.data && h.data % alignof(float) == 0 &&
        span(h.data, static_cast<std::size_t>(h.capacity) * sizeof(Record));
}
inline bool valid_record(const Record& r) noexcept {
    return std::isfinite(r.x) && std::isfinite(r.y) && std::isfinite(r.z) &&
        std::isfinite(r.copied_clock) && r.copied_clock >= 0 &&
        std::isfinite(r.expires) && r.expires >= native_expiry_duration;
}
inline bool same_record(const Record& a, const Record& b) noexcept {
    return std::memcmp(&a, &b, sizeof(Record)) == 0;
}
inline bool valid_clocks(const Snapshot& s) noexcept {
    return std::isfinite(s.realtime_begin) && std::isfinite(s.realtime_end) &&
        std::isfinite(s.current_time_begin) && std::isfinite(s.current_time_end) &&
        s.realtime_begin >= 0 && s.realtime_end >= s.realtime_begin &&
        s.current_time_begin >= 0 && s.current_time_end >= s.current_time_begin;
}
inline bool valid_interval(const Snapshot& s) noexcept {
    return std::isfinite(s.begin) && std::isfinite(s.end) && s.begin > 0 &&
        s.end >= s.begin && s.end - s.begin <= acquisition_limit;
}
inline bool valid_tail(const Snapshot& s) noexcept {
    if (!valid_header(s.header)) return false;
    const auto n = std::min<std::uint32_t>(static_cast<std::uint32_t>(s.header.count), tail_limit);
    return s.record_count == n && s.first_index == static_cast<std::uint32_t>(s.header.count) - n;
}

// ReadExact is bool(address, destination, bytes), Clock returns host steady seconds,
// and Verify confirms the attachment and local pawn are still the subject's.
// Only occupied tail entries are read; native allocation capacity is never data.
template<class ReadExact, class Clock, class Verify>
SnapshotPtr read_snapshot(const Subject& subject, const Layout& layout,
                          ReadExact& read, Clock& clock, Verify& verify) {
    if (!subject.valid() || !layout.client_base || !layout.services_offset ||
        !layout.expected_vtable || !layout.globals_slot || !layout.impacts_offset) return {};
    Snapshot s;
    s.subject = subject;
    s.begin = clock();
    if (!std::isfinite(s.begin) || s.begin <= 0 || !verify()) return {};
    std::uintptr_t service_slot = 0, globals_slot = 0;
    if (!address(subject.pawn, layout.services_offset, sizeof(std::uintptr_t), service_slot) ||
        !address(layout.client_base, layout.globals_slot, sizeof(std::uintptr_t), globals_slot)) return {};
    std::uintptr_t globals = 0, vtable = 0, owner = 0;
    if (!read(service_slot, &s.services, sizeof(s.services)) || !s.services ||
        s.services % alignof(std::uintptr_t) || !span(s.services, services_span(layout)) ||
        !read(s.services, &vtable, sizeof(vtable)) || vtable != layout.expected_vtable ||
        !read(s.services + services_owner_offset, &owner, sizeof(owner)) || owner != subject.pawn ||
        !read(globals_slot, &globals, sizeof(globals)) || !globals ||
        globals % alignof(float) || !span(globals, 0x34) ||
        !read(globals, &s.realtime_begin, sizeof(float)) ||
        !read(globals + 0x30, &s.current_time_begin, sizeof(float)) ||
        !read(s.services + layout.impacts_offset, &s.header, sizeof(s.header)) || !valid_header(s.header)) return {};
    s.record_count = std::min<std::uint32_t>(static_cast<std::uint32_t>(s.header.count), tail_limit);
    s.first_index = static_cast<std::uint32_t>(s.header.count) - s.record_count;
    if (s.record_count) {
        std::uintptr_t payload = 0;
        const auto bytes = s.record_count * sizeof(Record);
        if (!address(s.header.data, s.first_index * sizeof(Record), bytes, payload) ||
            !read(payload, s.records.data(), bytes)) return {};
        std::array<Record, tail_limit> closing_records{};
        if (!read(payload, closing_records.data(), bytes) ||
            std::memcmp(s.records.data(), closing_records.data(), bytes)) return {};
        for (std::size_t i = 0; i < s.record_count; ++i)
            if (!valid_record(s.records[i])) return {};
    }
    VectorHeader closing_header{};
    std::uintptr_t closing_service = 0, closing_vtable = 0, closing_owner = 0, closing_globals = 0;
    if (!read(s.services + layout.impacts_offset, &closing_header, sizeof(closing_header)) ||
        std::memcmp(&s.header, &closing_header, sizeof(s.header)) ||
        !read(s.services, &closing_vtable, sizeof(closing_vtable)) || closing_vtable != vtable ||
        !read(s.services + services_owner_offset, &closing_owner, sizeof(closing_owner)) || closing_owner != owner ||
        !read(service_slot, &closing_service, sizeof(closing_service)) || closing_service != s.services ||
        !read(globals_slot, &closing_globals, sizeof(closing_globals)) || closing_globals != globals ||
        !read(globals, &s.realtime_end, sizeof(float)) ||
        !read(globals + 0x30, &s.current_time_end, sizeof(float)) || !valid_clocks(s) || !verify()) return {};
    s.end = clock();
    if (!valid_interval(s)) return {};
    return std::make_shared<const Snapshot>(s);
}

struct Result {
    bool reset = false; // Clear any held flash before processing returned records.
    bool ready = false;
    std::vector<Record> records;
};

// Turns successive snapshots into newly added impacts. The first valid snapshot only
// primes the cursor (retained history is never replayed); a lost anchor, a shrinking
// vector, a stale record or a subject/services change re-primes and reports a reset.
class Consumer {
    Subject subject_{};
    std::uintptr_t services_ = 0;
    std::int32_t count_ = 0;
    Record anchor_{};
    bool primed_ = false;

    void prime(const Snapshot& s) noexcept {
        subject_ = s.subject;
        services_ = s.services;
        count_ = s.header.count;
        if (count_) anchor_ = s.records[s.record_count - 1];
        primed_ = true;
    }
    static bool fresh(const Record& r, const Snapshot& s, double elapsed) noexcept {
        const double age_real = static_cast<double>(s.realtime_end) + elapsed - r.copied_clock;
        const double age_game = static_cast<double>(s.current_time_end) + elapsed -
            (static_cast<double>(r.expires) - native_expiry_duration);
        // The producer lies between the two observed native clocks, so a record
        // cannot be ahead of the closing clock except for float quantization.
        const auto quantum = [](float value) {
            return static_cast<double>(std::nextafter(value, std::numeric_limits<float>::infinity())) - value;
        };
        const auto real_tolerance = std::min(future_tolerance, quantum(s.realtime_end) * 2);
        const auto game_tolerance = std::min(future_tolerance,
            std::max(quantum(s.current_time_end), quantum(r.expires)) * 2);
        return valid_record(r) && age_real >= -real_tolerance && age_game >= -game_tolerance &&
            age_real <= freshness_limit && age_game <= freshness_limit;
    }
public:
    void clear() noexcept {
        primed_ = false;
        subject_ = {};
        services_ = 0;
        count_ = 0;
        anchor_ = {};
    }

    Result consume(const SnapshotPtr& snapshot, double host_now, bool enabled) {
        Result out;
        if (!enabled || !snapshot || !snapshot->subject.valid() || !snapshot->services ||
            !valid_interval(*snapshot) || !valid_clocks(*snapshot) || !valid_tail(*snapshot) ||
            !std::isfinite(host_now) || host_now < snapshot->end ||
            host_now - snapshot->end > freshness_limit) {
            clear();
            out.reset = true;
            return out;
        }
        const auto& s = *snapshot;
        for (std::size_t i = 0; i < s.record_count; ++i) {
            if (!valid_record(s.records[i])) {
                clear();
                out.reset = true;
                return out;
            }
        }
        out.ready = true;
        const auto rebase = [&]() {
            prime(s);
            out.reset = true;
            out.records.clear();
            return out;
        };
        if (!primed_ || subject_ != s.subject || services_ != s.services ||
            s.header.count < count_) return rebase();
        if (count_) {
            const auto index = static_cast<std::uint32_t>(count_ - 1);
            if (index < s.first_index || index >= s.first_index + s.record_count ||
                !same_record(anchor_, s.records[index - s.first_index])) return rebase();
        }
        const auto added = static_cast<std::uint32_t>(s.header.count - count_);
        if (added > tail_limit || static_cast<std::uint32_t>(count_) < s.first_index) return rebase();
        const double elapsed = std::max(0.0, host_now - s.end);
        for (auto index = static_cast<std::uint32_t>(count_);
             index < static_cast<std::uint32_t>(s.header.count); ++index) {
            const auto& r = s.records[index - s.first_index];
            if (!fresh(r, s, elapsed)) return rebase();
            out.records.push_back(r); // Source indices preserve identical pellets.
        }
        prime(s);
        return out;
    }
};

} // namespace impacts
