#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

struct ConvarLookupResult {
    bool found = false;
    std::string error;
    uintptr_t address = 0;
    uintptr_t cvar_interface = 0;
    std::string name;
    std::string value;
    std::string default_value;
    std::string description;
    int16_t type = -1;
    uint32_t flags = 0;
};

struct ProcessMemoryView {
    int pid = -1;
    uintptr_t tier0_base = 0;
    std::string tier0_path;
    std::function<bool(uintptr_t address, void* dest, size_t size)> read_fn;
    std::function<std::string(uintptr_t address, size_t max_len)> read_str_fn;
};

std::optional<uintptr_t> elf_export_offset(const std::string& path, const char* symbol);

uintptr_t resolve_cvar_interface(const ProcessMemoryView& mem, uintptr_t optional_tier0_rva = 0);

ConvarLookupResult lookup_convar(const ProcessMemoryView& mem, uintptr_t cvar_interface, const char* name);

const char* convar_type_name(int16_t type);
const char* convar_flags_summary(uint32_t flags);
