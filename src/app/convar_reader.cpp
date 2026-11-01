#include "convar_reader.hpp"

#include "logger.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <vector>

namespace {

constexpr uintptr_t kCvarListOffset = 0x40;

constexpr uintptr_t kConVarName = 0x00;
constexpr uintptr_t kConVarDefaultValue = 0x08;
constexpr uintptr_t kConVarDescription = 0x20;
constexpr uintptr_t kConVarType = 0x28;
constexpr uintptr_t kConVarFlags = 0x30;
constexpr uintptr_t kConVarValue = 0x58;


bool str_iequals(const char* a, const char* b, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
        if (a[i] == '\0') {
            return true;
        }
    }
    return b[len] == '\0';
}

template <typename T>
T read_value(const ProcessMemoryView& mem, uintptr_t address)
{
    T value{};
    if (mem.read_fn) {
        mem.read_fn(address, &value, sizeof(T));
    }
    return value;
}

std::string read_cstring(const ProcessMemoryView& mem, uintptr_t address, size_t max_len = 256)
{
    if (!address || !mem.read_str_fn) {
        return "";
    }
    return mem.read_str_fn(address, max_len);
}

std::string format_convar_value(const ProcessMemoryView& mem, uintptr_t value_addr, int16_t type)
{
    switch (type) {
        case 0: {
            const bool v = read_value<uint8_t>(mem, value_addr) != 0;
            return v ? "true" : "false";
        }
        case 1: return std::to_string(read_value<int16_t>(mem, value_addr));
        case 2: return std::to_string(read_value<uint16_t>(mem, value_addr));
        case 3: return std::to_string(read_value<int32_t>(mem, value_addr));
        case 4: return std::to_string(read_value<uint32_t>(mem, value_addr));
        case 5: return std::to_string(read_value<int64_t>(mem, value_addr));
        case 6: return std::to_string(read_value<uint64_t>(mem, value_addr));
        case 7: return std::to_string(read_value<float>(mem, value_addr));
        case 8: return std::to_string(read_value<double>(mem, value_addr));
        case 9: return read_cstring(mem, read_value<uintptr_t>(mem, value_addr));
        case 10: {
            const uint32_t rgba = read_value<uint32_t>(mem, value_addr);
            char buf[32];
            std::snprintf(buf, sizeof(buf), "#%08X", rgba);
            return buf;
        }
        case 11: {
            const float x = read_value<float>(mem, value_addr + 0);
            const float y = read_value<float>(mem, value_addr + 4);
            return std::to_string(x) + ", " + std::to_string(y);
        }
        case 12: {
            const float x = read_value<float>(mem, value_addr + 0);
            const float y = read_value<float>(mem, value_addr + 4);
            const float z = read_value<float>(mem, value_addr + 8);
            return std::to_string(x) + ", " + std::to_string(y) + ", " + std::to_string(z);
        }
        case 13: {
            const float x = read_value<float>(mem, value_addr + 0);
            const float y = read_value<float>(mem, value_addr + 4);
            const float z = read_value<float>(mem, value_addr + 8);
            const float w = read_value<float>(mem, value_addr + 12);
            return std::to_string(x) + ", " + std::to_string(y) + ", " + std::to_string(z) + ", " +
                   std::to_string(w);
        }
        case 14: {
            const float x = read_value<float>(mem, value_addr + 0);
            const float y = read_value<float>(mem, value_addr + 4);
            const float z = read_value<float>(mem, value_addr + 8);
            return std::to_string(x) + ", " + std::to_string(y) + ", " + std::to_string(z);
        }
        default:
            return "<unsupported type>";
    }
}

std::string format_default_value(const ProcessMemoryView& mem, uintptr_t entry, int16_t type)
{
    const uintptr_t default_ptr = read_value<uintptr_t>(mem, entry + kConVarDefaultValue);
    if (!default_ptr) {
        return "";
    }
    return format_convar_value(mem, default_ptr, type);
}

uintptr_t resolve_singleton_from_factory(const ProcessMemoryView& mem, uintptr_t fn_addr)
{
    if (!fn_addr || !mem.read_fn) {
        return 0;
    }

    uint8_t code[128]{};
    if (!mem.read_fn(fn_addr, code, sizeof(code))) {
        return 0;
    }

    for (size_t i = 0; i + 7 < sizeof(code); ++i) {
        if (code[i] == 0x48 && code[i + 1] == 0x8B && code[i + 2] == 0x05) {
            int32_t disp = 0;
            std::memcpy(&disp, code + i + 3, sizeof(disp));
            const uintptr_t global_ptr = fn_addr + i + 7 + static_cast<uintptr_t>(disp);
            const uintptr_t singleton = read_value<uintptr_t>(mem, global_ptr);
            if (singleton) {
                return singleton;
            }
        }
    }

    return 0;
}

uintptr_t resolve_via_create_interface(const ProcessMemoryView& mem)
{
    if (mem.tier0_path.empty() || mem.tier0_base == 0) {
        BUNNYWARE_LOG_WARN("ConvarReader: tier0 path is empty ({}) or base is zero ({:#x})", mem.tier0_path, mem.tier0_base);
        return 0;
    }

    const auto create_off = elf_export_offset(mem.tier0_path, "CreateInterface");
    if (!create_off) {
        BUNNYWARE_LOG_WARN("ConvarReader: CreateInterface export not found in {}", mem.tier0_path);
        return 0;
    }

    const uintptr_t fn_addr = mem.tier0_base + *create_off;
    uint8_t code[128]{};
    if (!mem.read_fn(fn_addr, code, sizeof(code))) {
        BUNNYWARE_LOG_WARN("ConvarReader: Failed to read memory at CreateInterface ({:#x})", fn_addr);
        return 0;
    }

    // Modern CreateInterface assembly in libtier0.so:
    // 48 8b 1d 79 5c 31 00    mov    0x315c79(%rip),%rbx
    // Let's resolve the RIP relative pointer directly.
    uintptr_t reg_head_ptr = 0;
    for (size_t i = 0; i + 7 < sizeof(code); ++i) {
        if (code[i] == 0x48 && code[i + 1] == 0x8B && (code[i + 2] == 0x1D || code[i + 2] == 0x05)) {
            int32_t disp = 0;
            std::memcpy(&disp, code + i + 3, sizeof(disp));
            reg_head_ptr = fn_addr + i + 7 + static_cast<uintptr_t>(disp);
            break;
        }
    }

    if (!reg_head_ptr) {
        BUNNYWARE_LOG_WARN("ConvarReader: Failed to locate interface registry from CreateInterface at {:#x}", fn_addr);
        return 0;
    }

    // InterfaceReg layout: m_CreateFn (0x00), m_pName (0x08), m_pNext (0x10)
    uintptr_t reg = read_value<uintptr_t>(mem, reg_head_ptr);
    while (reg) {
        const uintptr_t create_fn = read_value<uintptr_t>(mem, reg + 0);
        const uintptr_t name_ptr = read_value<uintptr_t>(mem, reg + 8);
        const std::string if_name = read_cstring(mem, name_ptr);
        if (if_name.find("VEngineCvar") != std::string::npos) {
            const uintptr_t iface = resolve_singleton_from_factory(mem, create_fn);
            if (iface) {
                BUNNYWARE_LOG_INFO("ConvarReader: Resolved {} -> {:#x}", if_name, iface);
                return iface;
            } else {
                BUNNYWARE_LOG_WARN("ConvarReader: Failed to resolve singleton for {}", if_name);
            }
        }
        reg = read_value<uintptr_t>(mem, reg + 16);
    }

    BUNNYWARE_LOG_WARN("ConvarReader: VEngineCvar interface not found in registry");
    return 0;
}

} // namespace

std::optional<uintptr_t> elf_export_offset(const std::string& path, const char* symbol)
{
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return std::nullopt;
    }

    file.seekg(0, std::ios::end);
    const std::streamsize file_size = file.tellg();
    if (file_size <= 0) {
        return std::nullopt;
    }
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> data(static_cast<size_t>(file_size));
    if (!file.read(reinterpret_cast<char*>(data.data()), file_size)) {
        return std::nullopt;
    }

    if (data.size() < sizeof(uint64_t) + 0x40) {
        return std::nullopt;
    }

    if (std::memcmp(data.data(), "\x7F""ELF", 4) != 0 || data[4] != 2) {
        return std::nullopt;
    }

    uint64_t shoff = 0;
    uint16_t shentsize = 0;
    uint16_t shnum = 0;
    uint16_t shstrndx = 0;
    std::memcpy(&shoff, data.data() + 0x28, sizeof(shoff));
    std::memcpy(&shentsize, data.data() + 0x3A, sizeof(shentsize));
    std::memcpy(&shnum, data.data() + 0x3C, sizeof(shnum));
    std::memcpy(&shstrndx, data.data() + 0x3E, sizeof(shstrndx));

    if (shoff == 0 || shentsize < 64 || shnum == 0 || shstrndx >= shnum) {
        return std::nullopt;
    }

    auto get_section = [&](uint16_t index, uint64_t& offset, uint64_t& size) -> bool {
        const size_t base = static_cast<size_t>(shoff + static_cast<uint64_t>(index) * shentsize);
        if (base + 64 > data.size()) {
            return false;
        }
        std::memcpy(&offset, data.data() + base + 0x18, sizeof(offset));
        std::memcpy(&size, data.data() + base + 0x20, sizeof(size));
        return true;
    };

    uint64_t shstr_off = 0;
    uint64_t shstr_size = 0;
    if (!get_section(shstrndx, shstr_off, shstr_size)) {
        return std::nullopt;
    }

    auto section_name = [&](uint32_t name_off) -> std::string {
        if (name_off >= shstr_size || shstr_off + name_off >= data.size()) {
            return "";
        }
        const char* start = reinterpret_cast<const char*>(data.data() + shstr_off + name_off);
        return std::string(start);
    };

    uint64_t dynsym_off = 0;
    uint64_t dynsym_size = 0;
    uint64_t dynstr_off = 0;
    uint64_t dynstr_size = 0;

    for (uint16_t i = 0; i < shnum; ++i) {
        const size_t base = static_cast<size_t>(shoff + static_cast<uint64_t>(i) * shentsize);
        if (base + 64 > data.size()) {
            continue;
        }
        uint32_t name_off = 0;
        std::memcpy(&name_off, data.data() + base, sizeof(name_off));
        const std::string name = section_name(name_off);
        uint64_t offset = 0;
        uint64_t size = 0;
        if (!get_section(i, offset, size)) {
            continue;
        }
        if (name == ".dynsym") {
            dynsym_off = offset;
            dynsym_size = size;
        } else if (name == ".dynstr") {
            dynstr_off = offset;
            dynstr_size = size;
        }
    }

    if (dynsym_off == 0 || dynstr_off == 0) {
        return std::nullopt;
    }

    const size_t sym_count = static_cast<size_t>(dynsym_size / 24);
    for (size_t i = 0; i < sym_count; ++i) {
        const size_t sym_base = static_cast<size_t>(dynsym_off + i * 24);
        if (sym_base + 24 > data.size()) {
            break;
        }

        uint32_t name_idx = 0;
        uint64_t value = 0;
        std::memcpy(&name_idx, data.data() + sym_base, sizeof(name_idx));
        std::memcpy(&value, data.data() + sym_base + 8, sizeof(value));

        if (name_idx >= dynstr_size || dynstr_off + name_idx >= data.size()) {
            continue;
        }

        const char* sym_name = reinterpret_cast<const char*>(data.data() + dynstr_off + name_idx);
        if (std::strcmp(sym_name, symbol) == 0) {
            return value;
        }
    }

    return std::nullopt;
}

bool is_valid_cvar_interface(const ProcessMemoryView& mem, uintptr_t cvar_interface) {
    if (cvar_interface == 0) return false;
    uint32_t count = read_value<uint32_t>(mem, cvar_interface + kCvarListOffset + 0);
    uint32_t allocated = read_value<uint32_t>(mem, cvar_interface + kCvarListOffset + 4);
    uintptr_t pElements = read_value<uintptr_t>(mem, cvar_interface + kCvarListOffset + 16);
    return pElements != 0 && allocated > 0 && allocated < 100000 && count <= allocated;
}

uintptr_t resolve_cvar_interface(const ProcessMemoryView& mem, uintptr_t optional_tier0_rva)
{
    uintptr_t dyn_iface = resolve_via_create_interface(mem);
    if (is_valid_cvar_interface(mem, dyn_iface)) {
        BUNNYWARE_LOG_INFO("ConvarReader: Dynamic resolution found valid cvar interface at {:#x}", dyn_iface);
        return dyn_iface;
    }

    if (optional_tier0_rva != 0 && mem.tier0_base != 0) {
        uintptr_t rva_iface = mem.tier0_base + optional_tier0_rva - 0x40;
        if (is_valid_cvar_interface(mem, rva_iface)) {
            BUNNYWARE_LOG_INFO("ConvarReader: RVA resolution found valid cvar interface at {:#x}", rva_iface);
            return rva_iface;
        }
    }

    // Return 0 if not found
    return 0;
}

ConvarLookupResult lookup_convar(const ProcessMemoryView& mem, uintptr_t cvar_interface, const char* name)
{
    ConvarLookupResult result{};
    result.cvar_interface = cvar_interface;

    if (!name || name[0] == '\0') {
        result.error = "Empty convar name";
        return result;
    }
    if (cvar_interface == 0 || !mem.read_fn) {
        result.error = "CVar interface unavailable";
        return result;
    }

    // On Linux, the active ConVar list is a CUtlLinkedList at cvar_interface + kCvarListOffset (0x40).
    // The CUtlLinkedList layout is:
    // Offset 0: nCount (m_Size)
    // Offset 4: nAllocated (m_nAllocationCount)
    // Offset 16: pElements (pointer to nodes array)
    const uint32_t count = read_value<uint32_t>(mem, cvar_interface + kCvarListOffset + 0);
    const uint32_t allocated = read_value<uint32_t>(mem, cvar_interface + kCvarListOffset + 4);
    const uintptr_t pElements = read_value<uintptr_t>(mem, cvar_interface + kCvarListOffset + 16);

    BUNNYWARE_LOG_INFO("ConvarReader: cvar_interface={:#x}, count={}, allocated={}, pElements={:#x}", 
                     cvar_interface, count, allocated, pElements);

    if (!pElements || allocated == 0) {
        result.error = "Convar list pointer is null or empty";
        return result;
    }

    const size_t name_len = std::strlen(name);

    for (uint32_t i = 0; i < allocated; ++i) {
        // CUtlLinkedList node:
        // Offset 0: ConVar* (8-byte pointer to ConVar)
        // Offset 8: prev (2 bytes)
        // Offset 10: next (2 bytes)
        const uintptr_t entry = read_value<uintptr_t>(mem, pElements + 16 * i);
        if (entry == 0) {
            continue;
        }

        const uintptr_t entry_name_ptr = read_value<uintptr_t>(mem, entry + kConVarName);
        if (!entry_name_ptr) {
            continue;
        }

        std::string entry_name;
        entry_name.resize(name_len + 1);
        if (!mem.read_fn(entry_name_ptr, entry_name.data(), name_len)) {
            continue;
        }
        entry_name[name_len] = '\0';

        if (!str_iequals(entry_name.c_str(), name, name_len)) {
            continue;
        }

        const int16_t type = read_value<int16_t>(mem, entry + kConVarType);
        result.found = true;
        result.address = entry;
        result.name = read_cstring(mem, entry_name_ptr);
        result.type = type;
        result.flags = read_value<uint32_t>(mem, entry + kConVarFlags);
        result.description = read_cstring(mem, read_value<uintptr_t>(mem, entry + kConVarDescription));
        result.value = format_convar_value(mem, entry + kConVarValue, type);
        result.default_value = format_default_value(mem, entry, type);
        return result;
    }

    result.error = "Convar not found";
    return result;
}

const char* convar_type_name(int16_t type)
{
    switch (type) {
        case 0: return "bool";
        case 1: return "int16";
        case 2: return "uint16";
        case 3: return "int32";
        case 4: return "uint32";
        case 5: return "int64";
        case 6: return "uint64";
        case 7: return "float32";
        case 8: return "float64";
        case 9: return "string";
        case 10: return "color";
        case 11: return "vector2";
        case 12: return "vector3";
        case 13: return "vector4";
        case 14: return "qangle";
        default: return "unknown";
    }
}

const char* convar_flags_summary(uint32_t flags)
{
    static thread_local std::string summary;
    summary.clear();
    auto append = [&](const char* label) {
        if (!summary.empty()) {
            summary += ", ";
        }
        summary += label;
    };

    if (flags == 0) {
        return "none";
    }
    if (flags & (1u << 1)) append("devonly");
    if (flags & (1u << 2)) append("sv");
    if (flags & (1u << 3)) append("cl");
    if (flags & (1u << 4)) append("hidden");
    if (flags & (1u << 5)) append("protected");
    if (flags & (1u << 7)) append("archive");
    if (flags & (1u << 8)) append("notify");
    if (flags & (1u << 14)) append("cheat");
    if (flags & (1u << 15)) append("per_user");
    if (flags & (1u << 19)) append("release");
    if (flags & (1u << 22)) append("notconnected");

    if (summary.empty()) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "0x%X", flags);
        summary = buf;
    }
    return summary.c_str();
}
