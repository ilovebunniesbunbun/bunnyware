#include "offset_refresher.hpp"

#include "logger.hpp"
#include "offset_scanner.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

namespace offset_refresher {
namespace {

using offset_scanner::Operation;
using offset_scanner::OperationKind;
using offset_scanner::Pattern;
using ordered_json = nlohmann::ordered_json;

constexpr const char* kClientModule = "libclient.so";
constexpr const char* kEngineModule = "libengine2.so";
constexpr const char* kTier0Module = "libtier0.so";
constexpr const char* kSchemaModule = "libschemasystem.so";

constexpr const char* kBulletImpactProducerPattern =
    "48 8D 05 ? ? ? ? F3 0F 10 05 ? ? ? ? 48 8B 00 F3 0F 10 48 30 F3 0F 58 C8 F3 0F 10 85 ? ? ? ? 0F 28 D0 66 0F 3A 21 10 10 48 8B 85 ? ? ? ? F3 0F 10 85 ? ? ? ? 66 0F 3A 21 85 ? ? ? ? 10 0F 16 C2 4C 63 60 50 8B 78 60 41 39 FC 0F 84 ? ? ? ? 48 8B 50 58 44 89 E0 48 8B B5 ? ? ? ? 83 C0 01 89 46 50 4B 8D 04 A4 48 8D 04 82 0F 11 00 F3 0F 11 48 10";

constexpr const char* kInterfaceListPattern = "48 8B 1D ? ? ? ? 48 85 DB 74 ? 49 89 FC";

constexpr size_t kReadChunk = 4u * 1024u * 1024u;
constexpr size_t kPageSize = 4096;
constexpr size_t kMaxModuleImage = 512u * 1024u * 1024u;
constexpr int kMaxInterfaceEntries = 4096;
constexpr int kMaxUtlTsHashElements = 65536;
constexpr int kMaxUtlTsHashChain = 200000;
constexpr int kMaxLogEntries = 16;

// ---------------------------------------------------------------------------
// Process / module access (process_vm_readv only)
// ---------------------------------------------------------------------------

pid_t find_cs2_pid()
{
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator("/proc", error)) {
        const std::string name = entry.path().filename().string();
        if (name.empty() || !std::all_of(name.begin(), name.end(),
                                         [](unsigned char c) { return std::isdigit(c) != 0; })) {
            continue;
        }
        std::ifstream comm(entry.path() / "comm");
        std::string value;
        if (!comm.is_open() || !std::getline(comm, value)) {
            continue;
        }
        while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) {
            value.pop_back();
        }
        if (value == "cs2") {
            return static_cast<pid_t>(std::stol(name));
        }
    }
    return -1;
}

class ProcessMemory {
public:
    explicit ProcessMemory(pid_t pid) : pid_(pid) {}

    pid_t pid() const { return pid_; }

    bool read(uintptr_t address, void* destination, size_t size) const
    {
        if (pid_ <= 0 || destination == nullptr || address == 0 || size == 0) {
            return false;
        }
        auto* out = static_cast<uint8_t*>(destination);
        size_t done = 0;
        while (done < size) {
            const size_t chunk = std::min(kReadChunk, size - done);
            iovec local{ out + done, chunk };
            iovec remote{ reinterpret_cast<void*>(address + done), chunk };
            const ssize_t bytes = process_vm_readv(pid_, &local, 1, &remote, 1, 0);
            if (bytes <= 0) {
                return false;
            }
            done += static_cast<size_t>(bytes);
        }
        return true;
    }

    template <typename T>
    bool read_value(uintptr_t address, T* out) const
    {
        return read(address, out, sizeof(T));
    }

    std::string read_cstring(uintptr_t address, size_t max_length = 256) const
    {
        std::string result;
        if (address == 0) {
            return result;
        }
        result.reserve(std::min<size_t>(max_length, 32));
        uint8_t buffer[64];
        while (result.size() < max_length) {
            const size_t chunk = std::min(sizeof(buffer), max_length - result.size());
            if (!read(address + result.size(), buffer, chunk)) {
                break;
            }
            const void* terminator = std::memchr(buffer, '\0', chunk);
            if (terminator != nullptr) {
                result.append(reinterpret_cast<const char*>(buffer),
                              static_cast<const char*>(terminator) - reinterpret_cast<const char*>(buffer));
                break;
            }
            result.append(reinterpret_cast<const char*>(buffer), chunk);
        }
        return result;
    }

private:
    pid_t pid_ = -1;
};

struct ModuleMap {
    std::string path;
    uintptr_t base = 0;
    uintptr_t end = 0;
};

std::map<std::string, ModuleMap> enumerate_modules(pid_t pid)
{
    std::map<std::string, ModuleMap> modules;
    std::ifstream maps("/proc/" + std::to_string(pid) + "/maps");
    std::string line;
    while (std::getline(maps, line)) {
        std::istringstream stream(line);
        std::string range;
        std::string permissions;
        std::string offset;
        std::string device;
        std::string inode;
        if (!(stream >> range >> permissions >> offset >> device >> inode)) {
            continue;
        }
        std::string path;
        std::getline(stream, path);
        const size_t first = path.find_first_not_of(" \t");
        if (first == std::string::npos) {
            continue;
        }
        path.erase(0, first);
        if (path.empty() || path[0] != '/') {
            continue;
        }

        uintptr_t start = 0;
        uintptr_t end = 0;
        if (std::sscanf(range.c_str(), "%lx-%lx", &start, &end) != 2 || start >= end) {
            continue;
        }

        const std::string name = std::filesystem::path(path).filename().string();
        auto [it, inserted] = modules.try_emplace(name);
        ModuleMap& module = it->second;
        if (inserted) {
            module.path = path;
            module.base = start;
            module.end = end;
        } else {
            module.base = std::min(module.base, start);
            module.end = std::max(module.end, end);
        }
    }
    return modules;
}

struct ModuleImage {
    bool loaded = false;
    uintptr_t base = 0;
    std::vector<uint8_t> data;

    bool load(const ProcessMemory& memory, const ModuleMap& module)
    {
        base = module.base;
        const size_t size = static_cast<size_t>(module.end - module.base);
        if (size == 0 || size > kMaxModuleImage) {
            return false;
        }
        data.assign(size, 0);

        size_t position = 0;
        while (position < size) {
            const size_t chunk = std::min(kReadChunk, size - position);
            if (!memory.read(base + position, data.data() + position, chunk)) {
                // Zero-fill unreadable pages so a mapping hole does not sink the
                // whole module (patterns never match all-zero regions).
                for (size_t page = 0; page < chunk; page += kPageSize) {
                    const size_t page_size = std::min(kPageSize, chunk - page);
                    memory.read(base + position + page, data.data() + position + page, page_size);
                }
            }
            position += chunk;
        }
        loaded = true;
        return true;
    }

    const uint8_t* find_match(const Pattern& pattern) const
    {
        if (!loaded || data.empty()) {
            return nullptr;
        }
        return pattern.find(data.data(), data.size());
    }
};

// ---------------------------------------------------------------------------
// Signature definitions (ported from the cs2-linux config, limited to the
// offsets BUNNYWARE consumes)
// ---------------------------------------------------------------------------

struct SignatureDef {
    const char* name;
    const char* module;
    const char* pattern;
    std::vector<Operation> operations;
    bool optional = false; // A miss is a warning and removes the stale value.
};

const std::vector<SignatureDef>& signature_definitions()
{
    static const std::vector<SignatureDef> definitions = {
        { "dwEntityList", kClientModule, "3B 72 10 75 0E 48 83 3A 00 0F 95 C0 C3",
          { { OperationKind::sub, 62 }, { OperationKind::rip } } },
        { "dwGlobalVars", kClientModule, "48 8D 05 ? ? ? ? 48 8B 00 8B 40 44 F3",
          { { OperationKind::rip, 0, 3, 7 } } },
        { "dwLocalPlayerController", kClientModule, "48 83 3D ? ? ? ? 00 0F 95 C0 C3",
          { { OperationKind::rip, 0, 3, 8 } } },
        { "dwLocalPlayerPawn", kClientModule,
          "48 8D 15 ? ? ? ? 48 8D 35 ? ? ? ? 48 8D 3D ? ? ? ? E8 ? ? ? ? 31 D2 31 F6 41 B8 ? ? ? ? 48 8D 3D ? ? ? ? B9 02 00 00 00",
          { { OperationKind::rip, 0, 17, 21 }, { OperationKind::add, 296 } } },
        { "dwSensitivity", kClientModule,
          "95 17 FF FF FF 66 44 89 BD F4 FE FF FF 4C 8D 3D ? ? ? ? 66 89",
          { { OperationKind::rip, 0, 16, 20 }, { OperationKind::add, 8 } } },
        { "dwSensitivity_sensitivity", kClientModule, "48 8B 5F 08 4C 8D 73 ? F6 43 31 80",
          { { OperationKind::slice, 7, 0, 8 } } },
        { "dwCSGOInput", kClientModule, "48 8D 05 ? ? ? ? 0F 58 C1 F3 41 0F 58 5E 08",
          { { OperationKind::rip }, { OperationKind::add, 16 } } },
        { "dwViewAngles", kClientModule, "48 8D 05 ? ? ? ? 0F 58 C1 F3 41 0F 58 5E 08",
          { { OperationKind::rip }, { OperationKind::add, 16 }, { OperationKind::add, 1352 } } },
        { "dwViewMatrix", kClientModule, "C6 83 ? ? 00 00 01 4C 8D 05",
          { { OperationKind::rip, 0, 10, 14 } } },
        { "dwGameRules", kClientModule, "48 8D 05 ? ? ? ? 48 8B 00 48 89 83 80 07 00 00",
          { { OperationKind::rip } } },
        { "dwWindowHeight", kEngineModule, "8B 50 08 5D 89 15 ? ? ? ? 8B 40 0C 89 05 ? ? ? ? C3",
          { { OperationKind::rip, 0, 15, 19 } } },
        { "dwWindowWidth", kEngineModule, "8B 50 08 5D 89 15 ? ? ? ? 8B 40 0C 89 05 ? ? ? ? C3",
          { { OperationKind::rip, 0, 6, 10 } } },

        // Bullet Impacts native client impacts (optional). The producer pattern spans the
        // whole impact producer, pinning the record layout, clock fields and vector members.
        { "dwBulletImpactGlobals", kClientModule, kBulletImpactProducerPattern,
          { { OperationKind::rip, 0, 3, 7 } }, true },
        { "dwBulletServices_impacts", kClientModule, kBulletImpactProducerPattern,
          { { OperationKind::slice, 75, 0, 76 } }, true },
        { "dwBulletServicesVTable", kClientModule,
          "48 8D 05 ? ? ? ? 66 0F EF C0 C7 43 48 00 00 00 00 48 89 03 48 8D 3D ? ? ? ? 0F 11 83 88 00 00 00",
          { { OperationKind::rip, 0, 3, 7 } }, true },
    };
    return definitions;
}

// ---------------------------------------------------------------------------
// Schema keys consumed by BUNNYWARE (keep in sync with get_schema() calls in
// game_reader.cpp; only these fields are refreshed automatically)
// ---------------------------------------------------------------------------

struct SchemaKey {
    const char* class_name;
    const char* field_name;
};

constexpr SchemaKey kSchemaKeys[] = {
    // Controllers
    { "CBasePlayerController", "m_hPawn" },
    { "CBasePlayerController", "m_nTickBase" },
    { "CBasePlayerController", "m_iszPlayerName" },

    // Weapon data (shared)
    { "CBasePlayerWeaponVData", "m_iMaxClip1" },

    // Controllers
    { "CCSPlayerController", "m_bPawnHasDefuser" },
    { "CCSPlayerController", "m_bPawnIsAlive" },
    { "CCSPlayerController", "m_hObserverPawn" },
    { "CCSPlayerController", "m_hPlayerPawn" },
    { "CCSPlayerController", "m_iPing" },
    { "CCSPlayerController", "m_pActionTrackingServices" },
    { "CCSPlayerController", "m_szCrosshairCodes" },

    // Controller action tracking
    { "CCSPlayerController_ActionTrackingServices", "m_flTotalRoundDamageDealt" },
    { "CCSPlayerController_ActionTrackingServices", "m_iNumRoundKills" },
    { "CCSPlayerController_ActionTrackingServices", "m_iNumRoundKillsHeadshots" },

    // Player services
    { "CCSPlayer_AimPunchServices", "m_predictableBaseAngle" },
    { "CCSPlayer_ItemServices", "m_bHasDefuser" },

    // Weapon data (shared)
    { "CCSWeaponBaseVData", "m_flArmorRatio" },
    { "CCSWeaponBaseVData", "m_flHeadshotMultiplier" },
    { "CCSWeaponBaseVData", "m_flInaccuracyJumpInitial" },
    { "CCSWeaponBaseVData", "m_flInaccuracyMove" },
    { "CCSWeaponBaseVData", "m_flMaxSpeed" },
    { "CCSWeaponBaseVData", "m_flPenetration" },
    { "CCSWeaponBaseVData", "m_flRangeModifier" },
    { "CCSWeaponBaseVData", "m_flSpread" },
    { "CCSWeaponBaseVData", "m_nDamage" },
    { "CCSWeaponBaseVData", "m_nNumBullets" },
    { "CCSWeaponBaseVData", "m_nRecoilSeed" },
    { "CCSWeaponBaseVData", "m_nSpreadSeed" },
    { "CCSWeaponBaseVData", "m_szName" },

    // Projectiles
    { "C_DecoyProjectile", "m_nDecoyShotTick" },
    { "C_MolotovProjectile", "m_bIsIncGrenade" },
    { "C_BaseCSGrenadeProjectile", "m_nExplodeEffectTickBegin" },
    { "C_BaseCSGrenadeProjectile", "m_vInitialPosition" },
    { "C_BaseCSGrenadeProjectile", "m_vInitialVelocity" },
    { "C_SmokeGrenadeProjectile", "m_bDidSmokeEffect" },
    { "C_SmokeGrenadeProjectile", "m_bSmokeEffectSpawned" },
    { "C_SmokeGrenadeProjectile", "m_bSmokeVolumeDataReceived" },
    { "C_SmokeGrenadeProjectile", "m_nSmokeEffectTickBegin" },
    { "C_SmokeGrenadeProjectile", "m_nVoxelFrameDataSize" },
    { "C_SmokeGrenadeProjectile", "m_VoxelFrameData" },
    { "C_SmokeGrenadeProjectile", "m_vSmokeDetonationPos" },

    // Grenades
    { "C_BaseCSGrenade", "m_bPinPulled" },
    { "C_BaseCSGrenade", "m_flThrowStrength" },
    { "C_BaseGrenade", "m_hThrower" },

    // Inferno
    { "C_Inferno", "m_bFireIsBurning" },
    { "C_Inferno", "m_fireCount" },
    { "C_Inferno", "m_firePositions" },

    // Econ / item definition
    { "C_AttributeContainer", "m_Item" },
    { "C_EconEntity", "m_AttributeManager" },
    { "C_EconItemView", "m_iItemDefinitionIndex" },

    // Base entity
    { "C_BaseEntity", "m_fFlags" },
    { "C_BaseEntity", "m_flSimulationTime" },
    { "C_BaseEntity", "m_hOwnerEntity" },
    { "C_BaseEntity", "m_iHealth" },
    { "C_BaseEntity", "m_iTeamNum" },
    { "C_BaseEntity", "m_lifeState" },
    { "C_BaseEntity", "m_MoveType" },
    { "C_BaseEntity", "m_nSimulationTick" },
    { "C_BaseEntity", "m_nSubclassID" },
    { "C_BaseEntity", "m_pGameSceneNode" },
    { "C_BaseEntity", "m_vecAbsVelocity" },
    { "C_BaseEntity", "m_vecVelocity" },

    // Model entity / model state
    { "C_BaseModelEntity", "m_vecViewOffset" },
    { "CGameSceneNode", "m_angAbsRotation" },
    { "CGameSceneNode", "m_flAbsScale" },
    { "CGameSceneNode", "m_vecAbsOrigin" },
    { "CModelState", "m_ModelName" },
    { "CSkeletonInstance", "m_modelState" },

    // Player pawns
    { "C_BasePlayerPawn", "m_flFOVSensitivityAdjust" },
    { "C_BasePlayerPawn", "m_hController" },
    { "C_BasePlayerPawn", "m_pItemServices" },
    { "C_BasePlayerPawn", "m_pObserverServices" },
    { "C_BasePlayerPawn", "m_pWeaponServices" },
    { "C_BasePlayerPawn", "m_vOldOrigin" },
    { "C_CSPlayerPawnBase", "m_flFlashOverlayAlpha" },
    { "C_CSPlayerPawn", "m_angEyeAngles" },
    { "C_CSPlayerPawn", "m_ArmorValue" },
    { "C_CSPlayerPawn", "m_bIsScoped" },
    { "C_CSPlayerPawn", "m_bIsWalking" },
    { "C_CSPlayerPawn", "m_bPrevHelmet" },
    { "C_CSPlayerPawn", "m_flEmitSoundTime" },
    { "C_CSPlayerPawn", "m_iIDEntIndex" },
    { "C_CSPlayerPawn", "m_iShotsFired" },
    { "C_CSPlayerPawn", "m_pAimPunchServices" },
    { "C_CSPlayerPawn", "m_pBulletServices" },

    // Player services
    { "CPlayer_ObserverServices", "m_hObserverTarget" },
    { "CPlayer_ObserverServices", "m_iObserverMode" },
    { "CPlayer_WeaponServices", "m_hActiveWeapon" },
    { "CPlayer_WeaponServices", "m_hMyWeapons" },

    // Weapons
    { "C_BasePlayerWeapon", "m_iClip1" },
    { "C_CSWeaponBase", "m_bInReload" },
    { "C_CSWeaponBase", "m_fAccuracyPenalty" },
    { "C_CSWeaponBase", "m_flRecoilIndex" },
    { "C_CSWeaponBase", "m_weaponMode" },

    // Planted C4
    { "C_C4", "m_bStartedArming" },
    { "C_PlantedC4", "m_bBeingDefused" },
    { "C_PlantedC4", "m_bBombDefused" },
    { "C_PlantedC4", "m_bHasExploded" },
    { "C_PlantedC4", "m_flC4Blow" },
    { "C_PlantedC4", "m_flDefuseCountDown" },

    // Web radar / match state
    { "C_Team", "m_iScore" },
    { "C_CSGameRules", "m_bFreezePeriod" },
    { "C_CSGameRules", "m_bWarmupPeriod" },
    { "C_CSGameRules", "m_iRoundTime" },
    { "C_CSGameRules", "m_totalRoundsPlayed" },
    { "C_CSGameRules", "m_gamePhase" },
    { "C_CSGameRules", "m_fRoundStartTime" },

    // Entity instance
    { "CEntityInstance", "m_pEntity" },
};
constexpr size_t kSchemaKeyCount = std::size(kSchemaKeys);

// ---------------------------------------------------------------------------
// Interface registry walking (CreateInterface linked list)
// ---------------------------------------------------------------------------

struct InterfaceEntry {
    std::string name;
    uintptr_t create_fn = 0;
};

// Scans `image` for the ICreateInterface registry pattern, resolves the global
// head pointer, and walks the InterfaceReg list.
std::vector<InterfaceEntry> read_module_interfaces(const ProcessMemory& memory,
                                                   const ModuleImage& image,
                                                   const char* pattern_text)
{
    std::vector<InterfaceEntry> entries;
    Pattern pattern;
    if (!pattern.parse(pattern_text)) {
        return entries;
    }
    const uint8_t* match = image.find_match(pattern);
    if (match == nullptr) {
        return entries;
    }

    const uintptr_t instruction = image.base + static_cast<uintptr_t>(match - image.data.data());
    int32_t displacement = 0;
    if (!memory.read_value(instruction + 3, &displacement)) {
        return entries;
    }
    const uintptr_t list_global = instruction + 7 + static_cast<uint64_t>(static_cast<int64_t>(displacement));

    uint64_t reg = 0;
    if (!memory.read_value(list_global, &reg)) {
        return entries;
    }

    std::unordered_set<uintptr_t> seen;
    while (reg != 0 && entries.size() < kMaxInterfaceEntries) {
        if (!seen.insert(reg).second) {
            break;
        }
        uint8_t record[0x18];
        if (!memory.read(reg, record, sizeof(record))) {
            break;
        }
        uint64_t create_fn = 0;
        uint64_t name_ptr = 0;
        uint64_t next = 0;
        std::memcpy(&create_fn, record + 0x00, sizeof(create_fn));
        std::memcpy(&name_ptr, record + 0x08, sizeof(name_ptr));
        std::memcpy(&next, record + 0x10, sizeof(next));

        if (name_ptr != 0) {
            entries.push_back({ memory.read_cstring(name_ptr), static_cast<uintptr_t>(create_fn) });
        }
        reg = next;
    }
    return entries;
}

// Looks at the first bytes of an interface factory and returns candidate
// singleton addresses for both `mov rax, [rip+x]` and `lea rax, [rip+x]` forms.
std::vector<uintptr_t> interface_factory_candidates(const ProcessMemory& memory, uintptr_t create_fn)
{
    std::vector<uintptr_t> candidates;
    if (create_fn == 0) {
        return candidates;
    }
    uint8_t code[128];
    if (!memory.read(create_fn, code, sizeof(code))) {
        return candidates;
    }
    for (size_t i = 0; i + 7 <= sizeof(code); ++i) {
        if (code[i] != 0x48 || code[i + 2] != 0x05) {
            continue;
        }
        if (code[i + 1] != 0x8B && code[i + 1] != 0x8D) {
            continue;
        }
        int32_t displacement = 0;
        std::memcpy(&displacement, code + i + 3, sizeof(displacement));
        const uintptr_t global = create_fn + i + 7 + static_cast<uint64_t>(static_cast<int64_t>(displacement));
        if (code[i + 1] == 0x8D) {
            candidates.push_back(global);
        } else {
            uint64_t pointer = 0;
            if (memory.read_value(global, &pointer) && pointer != 0) {
                candidates.push_back(static_cast<uintptr_t>(pointer));
            }
        }
    }
    std::unordered_set<uintptr_t> seen;
    candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                                    [&](uintptr_t candidate) { return !seen.insert(candidate).second; }),
                     candidates.end());
    return candidates;
}

// ---------------------------------------------------------------------------
// Schema system walking
// ---------------------------------------------------------------------------

struct TypeScope {
    std::string name;
    uintptr_t address = 0;
};

struct SchemaDumpResult {
    std::map<std::string, uint32_t> fields; // "Class.field" -> offset
    int classes_seen = 0;
    int needed_classes_seen = 0;
    std::string error;
};

bool cvar_interface_is_valid(const ProcessMemory& memory, uintptr_t interface)
{
    int32_t count = 0;
    int32_t allocated = 0;
    uint64_t elements = 0;
    if (!memory.read_value(interface + 0x40, &count)) return false;
    if (!memory.read_value(interface + 0x44, &allocated)) return false;
    if (!memory.read_value(interface + 0x50, &elements)) return false;
    return elements != 0 && allocated > 0 && allocated < 100000 && count <= allocated;
}

// Resolves ICvar from libtier0's CreateInterface registry. The consolidated
// offsets.json stores this as interface_base + 0x40 (see convar_reader.cpp).
std::optional<uint32_t> resolve_cvar_interface_rva(const ProcessMemory& memory,
                                                   const ModuleImage& tier0)
{
    const std::vector<InterfaceEntry> entries =
        read_module_interfaces(memory, tier0, kInterfaceListPattern);
    for (const InterfaceEntry& entry : entries) {
        if (entry.name.find("VEngineCvar") == std::string::npos) {
            continue;
        }
        for (uintptr_t candidate : interface_factory_candidates(memory, entry.create_fn)) {
            if (candidate < tier0.base || !cvar_interface_is_valid(memory, candidate)) {
                continue;
            }
            return static_cast<uint32_t>(candidate - tier0.base) + 0x40;
        }
    }
    return std::nullopt;
}

std::optional<uintptr_t> resolve_schema_system(const ProcessMemory& memory,
                                               const ModuleImage& schemasystem)
{
    const std::vector<InterfaceEntry> entries =
        read_module_interfaces(memory, schemasystem, kInterfaceListPattern);
    for (const InterfaceEntry& entry : entries) {
        if (entry.name != "SchemaSystem_001") {
            continue;
        }
        for (uintptr_t candidate : interface_factory_candidates(memory, entry.create_fn)) {
            int32_t registration_count = 0;
            int32_t type_scope_count = 0;
            if (memory.read_value(candidate + 0x280, &registration_count) &&
                registration_count > 0 &&
                memory.read_value(candidate + 0x1F0, &type_scope_count) &&
                type_scope_count > 0 && type_scope_count < 1024) {
                return candidate;
            }
        }
    }
    return std::nullopt;
}

std::vector<TypeScope> read_type_scopes(const ProcessMemory& memory, uintptr_t schema_system)
{
    std::vector<TypeScope> scopes;
    int32_t count = 0;
    uint64_t vector = 0;
    if (!memory.read_value(schema_system + 0x1F0, &count) || count <= 0) {
        return scopes;
    }
    if (!memory.read_value(schema_system + 0x1F8, &vector) || vector == 0) {
        return scopes;
    }
    count = std::min(count, 1024);
    for (int i = 0; i < count; ++i) {
        uint64_t scope = 0;
        if (!memory.read_value(vector + 8ull * static_cast<uint64_t>(i), &scope) || scope == 0) {
            continue;
        }
        char name[257] = {0};
        if (!memory.read(scope + 0x8, name, 256)) {
            continue;
        }
        name[256] = '\0';
        scopes.push_back({ name, static_cast<uintptr_t>(scope) });
    }
    return scopes;
}

// Port of cs2-linux's UtlTsHash traversal: bucket chains plus the free-blob
// chain, deduplicated by element address.
std::vector<uintptr_t> read_ts_hash_elements(const ProcessMemory& memory, uintptr_t hash_base)
{
    std::vector<uintptr_t> elements;
    int32_t blocks_allocated = 0;
    int32_t peak_allocated = 0;
    uint64_t free_head = 0;
    if (!memory.read_value(hash_base + 0x0C, &blocks_allocated) ||
        !memory.read_value(hash_base + 0x10, &peak_allocated) ||
        !memory.read_value(hash_base + 0x20, &free_head)) {
        return elements;
    }
    blocks_allocated = std::clamp(blocks_allocated, 0, kMaxUtlTsHashElements);
    peak_allocated = std::clamp(peak_allocated, 0, kMaxUtlTsHashElements);

    std::vector<uint8_t> buckets(0x18 * 768);
    if (!memory.read(hash_base + 0x60, buckets.data(), buckets.size())) {
        return elements;
    }

    for (size_t bucket = 0; bucket < 768; ++bucket) {
        uint64_t node = 0;
        std::memcpy(&node, buckets.data() + bucket * 0x18 + 0x10, sizeof(node));
        int guard = 0;
        while (node != 0 && guard++ < kMaxUtlTsHashChain) {
            uint8_t raw[0x18];
            if (!memory.read(node, raw, sizeof(raw))) {
                break;
            }
            uint64_t next = 0;
            uint64_t data = 0;
            std::memcpy(&next, raw + 0x08, sizeof(next));
            std::memcpy(&data, raw + 0x10, sizeof(data));
            if (data != 0) {
                elements.push_back(static_cast<uintptr_t>(data));
            }
            if (static_cast<int>(elements.size()) >= blocks_allocated) {
                break;
            }
            node = next;
        }
    }

    uint64_t blob = free_head;
    int guard = 0;
    while (blob != 0 && guard++ < kMaxUtlTsHashChain) {
        uint8_t raw[0x18];
        if (!memory.read(blob, raw, sizeof(raw))) {
            break;
        }
        uint64_t next = 0;
        uint64_t data = 0;
        std::memcpy(&next, raw + 0x00, sizeof(next));
        std::memcpy(&data, raw + 0x10, sizeof(data));
        if (data != 0) {
            elements.push_back(static_cast<uintptr_t>(data));
        }
        if (static_cast<int>(elements.size()) >= peak_allocated) {
            break;
        }
        blob = next;
    }

    std::unordered_set<uintptr_t> seen;
    std::vector<uintptr_t> unique;
    unique.reserve(elements.size());
    for (uintptr_t element : elements) {
        if (seen.insert(element).second) {
            unique.push_back(element);
        }
    }
    return unique;
}

bool read_class_binding(const ProcessMemory& memory, uintptr_t binding, std::string* name,
                        int* field_count, uintptr_t* fields)
{
    uint8_t header[0x38];
    if (!memory.read(binding, header, sizeof(header))) {
        return false;
    }
    uint64_t name_ptr = 0;
    int16_t count = 0;
    uint64_t fields_ptr = 0;
    std::memcpy(&name_ptr, header + 0x08, sizeof(name_ptr));
    std::memcpy(&count, header + 0x24, sizeof(count));
    std::memcpy(&fields_ptr, header + 0x30, sizeof(fields_ptr));
    if (name_ptr == 0) {
        return false;
    }
    *name = memory.read_cstring(name_ptr);
    if (name->empty()) {
        return false;
    }
    *field_count = std::clamp<int>(count, 0, 4096);
    *fields = static_cast<uintptr_t>(fields_ptr);
    return true;
}

int collect_class_fields(const ProcessMemory& memory, const std::string& class_name,
                         int field_count, uintptr_t fields,
                         const std::set<std::string>& wanted_fields,
                         std::map<std::string, uint32_t>* out)
{
    if (fields == 0 || field_count <= 0 || wanted_fields.empty()) {
        return 0;
    }
    std::vector<uint8_t> raw(static_cast<size_t>(field_count) * 0x20);
    if (!memory.read(fields, raw.data(), raw.size())) {
        return 0;
    }
    int found = 0;
    for (int i = 0; i < field_count; ++i) {
        const uint8_t* field = raw.data() + static_cast<size_t>(i) * 0x20;
        uint64_t name_ptr = 0;
        uint64_t type_ptr = 0;
        int32_t offset = 0;
        std::memcpy(&name_ptr, field + 0x00, sizeof(name_ptr));
        std::memcpy(&type_ptr, field + 0x08, sizeof(type_ptr));
        std::memcpy(&offset, field + 0x10, sizeof(offset));
        if (type_ptr == 0) {
            // cs2-linux stops at the first field without a type.
            break;
        }
        if (name_ptr == 0) {
            continue;
        }
        const std::string field_name = memory.read_cstring(name_ptr);
        if (wanted_fields.count(field_name) == 0) {
            continue;
        }
        // First binding wins: scopes are ordered client-first, so shared classes
        // keep the client layout (matches the consolidated offsets.json).
        const auto [it, inserted] =
            out->try_emplace(class_name + "." + field_name, static_cast<uint32_t>(offset));
        if (inserted) {
            ++found;
        }
    }
    return found;
}

bool dump_needed_schema_fields(const ProcessMemory& memory, const ModuleImage& schemasystem,
                               SchemaDumpResult* result)
{
    result->fields.clear();
    result->classes_seen = 0;
    result->needed_classes_seen = 0;
    result->error.clear();

    const std::optional<uintptr_t> schema_system = resolve_schema_system(memory, schemasystem);
    if (!schema_system.has_value()) {
        result->error = "SchemaSystem_001 not found";
        return false;
    }

    std::map<std::string, std::set<std::string>> needed_by_class;
    for (const SchemaKey& key : kSchemaKeys) {
        needed_by_class[key.class_name].insert(key.field_name);
    }

    std::vector<TypeScope> scopes = read_type_scopes(memory, *schema_system);
    if (scopes.empty()) {
        result->error = "no schema type scopes";
        return false;
    }

    // Prefer the client type scope; fall back to every scope when it is missing.
    std::stable_sort(scopes.begin(), scopes.end(), [](const TypeScope& a, const TypeScope& b) {
        const bool a_client = a.name.find("libclient") != std::string::npos;
        const bool b_client = b.name.find("libclient") != std::string::npos;
        return a_client && !b_client;
    });

    for (const TypeScope& scope : scopes) {
        const std::vector<uintptr_t> bindings = read_ts_hash_elements(memory, scope.address + 0x560);
        for (uintptr_t binding : bindings) {
            std::string class_name;
            int field_count = 0;
            uintptr_t fields = 0;
            if (!read_class_binding(memory, binding, &class_name, &field_count, &fields)) {
                continue;
            }
            ++result->classes_seen;
            const auto needed = needed_by_class.find(class_name);
            if (needed == needed_by_class.end()) {
                continue;
            }
            ++result->needed_classes_seen;
            collect_class_fields(memory, class_name, field_count, fields, needed->second,
                                 &result->fields);
        }
        if (result->fields.size() >= kSchemaKeyCount) {
            break;
        }
    }

    if (result->fields.empty()) {
        result->error = "no requested schema fields found";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// JSON merge / file output
// ---------------------------------------------------------------------------

std::filesystem::path resolve_primary_offsets_path()
{
    const std::filesystem::path local = "offsets.json";
    std::error_code error;
    if (std::filesystem::exists(local, error)) {
        return local;
    }
    const std::filesystem::path parent = "../offsets.json";
    if (std::filesystem::exists(parent, error)) {
        return parent;
    }
    return local;
}

bool read_file_bytes(const std::filesystem::path& path, std::string* out)
{
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    *out = buffer.str();
    return true;
}

bool write_file_atomic(const std::filesystem::path& path, const std::string& bytes)
{
    std::error_code error;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), error);
    }
    // Preserve the existing file mode (the tracked offsets.json is 0755).
    std::filesystem::perms permissions = std::filesystem::perms::unknown;
    if (std::filesystem::exists(path, error)) {
        permissions = std::filesystem::status(path, error).permissions();
        error.clear();
    }
    const std::filesystem::path temporary = path.string() + ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) {
            return false;
        }
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        file.flush();
        if (!file.good()) {
            return false;
        }
    }
    if (permissions != std::filesystem::perms::unknown) {
        std::filesystem::permissions(temporary, permissions, std::filesystem::perm_options::replace,
                                     error);
        error.clear();
    }
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary);
        return false;
    }
    return true;
}

void apply_sudo_ownership(const std::filesystem::path& path)
{
    const char* uid_text = std::getenv("SUDO_UID");
    const char* gid_text = std::getenv("SUDO_GID");
    if (uid_text == nullptr || gid_text == nullptr) {
        return;
    }
    const uid_t uid = static_cast<uid_t>(std::stoul(uid_text));
    const gid_t gid = static_cast<gid_t>(std::stoul(gid_text));
    chown(path.c_str(), uid, gid);
}

bool same_file(const std::filesystem::path& a, const std::filesystem::path& b)
{
    std::error_code error;
    return std::filesystem::equivalent(a, b, error);
}

// ---------------------------------------------------------------------------
// Refresh orchestration
// ---------------------------------------------------------------------------

struct BaseResolution {
    std::map<std::string, uint32_t> offsets;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    std::vector<std::string> missing_optional;
};

BaseResolution resolve_base_offsets(const ProcessMemory& memory,
                                    const std::map<std::string, ModuleMap>& modules,
                                    std::map<std::string, ModuleImage>* image_cache)
{
    BaseResolution resolution;
    auto load_image = [&](const std::string& module_name) -> const ModuleImage* {
        auto cached = image_cache->find(module_name);
        if (cached != image_cache->end()) {
            return cached->second.loaded ? &cached->second : nullptr;
        }
        const auto module = modules.find(module_name);
        ModuleImage image;
        if (module != modules.end()) {
            image.load(memory, module->second);
        }
        auto [it, inserted] = image_cache->emplace(module_name, std::move(image));
        (void)inserted;
        return it->second.loaded ? &it->second : nullptr;
    };

    for (const SignatureDef& definition : signature_definitions()) {
        auto& errors = definition.optional ? resolution.warnings : resolution.errors;
        auto fail = [&](std::string message) {
            errors.push_back(std::move(message));
            if (definition.optional) resolution.missing_optional.push_back(definition.name);
        };
        const ModuleImage* image = load_image(definition.module);
        if (image == nullptr) {
            fail(std::string("module not loaded: ") + definition.module +
                                        " (needed for " + definition.name + ")");
            continue;
        }
        Pattern pattern;
        std::string parse_error;
        if (!pattern.parse(definition.pattern, &parse_error)) {
            fail(std::string("bad pattern for ") + definition.name + ": " +
                                        parse_error);
            continue;
        }
        const uint8_t* match = image->find_match(pattern);
        if (match == nullptr) {
            fail(std::string("signature not found: ") + definition.name +
                                        " in " + definition.module);
            continue;
        }
        const uintptr_t instruction =
            image->base + static_cast<uintptr_t>(match - image->data.data());
        uint32_t rva = 0;
        auto reader = [&](uintptr_t address, void* out, size_t size) {
            return memory.read(address, out, size);
        };
        if (!offset_scanner::apply_operations(instruction, definition.operations, reader,
                                              image->base, &rva)) {
            fail(std::string("failed to evaluate signature: ") +
                                        definition.name);
            continue;
        }
        resolution.offsets[definition.name] = rva;
    }
    return resolution;
}

void validate_live_reads(const ProcessMemory& memory,
                         const std::map<std::string, ModuleMap>& modules,
                         const std::map<std::string, uint32_t>& offsets,
                         std::vector<std::string>* warnings)
{
    const auto client = modules.find(kClientModule);
    if (client == modules.end()) {
        return;
    }
    const uintptr_t client_base = client->second.base;

    auto read_at = [&](const char* name, uint64_t address, void* out, size_t size) {
        if (!memory.read(address, out, size)) {
            warnings->push_back(std::string("live read failed: ") + name);
        }
    };

    if (const auto it = offsets.find("dwViewMatrix"); it != offsets.end()) {
        float matrix[16] = {0};
        read_at("dwViewMatrix", client_base + it->second, matrix, sizeof(matrix));
    }
    if (const auto it = offsets.find("dwEntityList"); it != offsets.end()) {
        uint64_t entity_list = 0;
        read_at("dwEntityList", client_base + it->second, &entity_list, sizeof(entity_list));
    }
    if (const auto it = offsets.find("dwLocalPlayerController"); it != offsets.end()) {
        uint64_t controller = 0;
        read_at("dwLocalPlayerController", client_base + it->second, &controller, sizeof(controller));
    }
    if (const auto it = offsets.find("dwGlobalVars"); it != offsets.end()) {
        uint64_t global_vars = 0;
        read_at("dwGlobalVars", client_base + it->second, &global_vars, sizeof(global_vars));
    }

    const auto engine = modules.find(kEngineModule);
    if (engine != modules.end()) {
        if (const auto it = offsets.find("dwWindowWidth"); it != offsets.end()) {
            uint32_t width = 0;
            if (!memory.read_value(engine->second.base + it->second, &width) || width == 0) {
                warnings->push_back("dwWindowWidth did not read a sane value");
            }
        }
        if (const auto it = offsets.find("dwWindowHeight"); it != offsets.end()) {
            uint32_t height = 0;
            if (!memory.read_value(engine->second.base + it->second, &height) || height == 0) {
                warnings->push_back("dwWindowHeight did not read a sane value");
            }
        }
    }
}

RefreshStatus run_refresh()
{
    RefreshStatus status;
    status.attempted = true;

    const pid_t pid = find_cs2_pid();
    if (pid <= 0) {
        status.summary = "cs2 process not found (existing offsets.json kept)";
        return status;
    }
    status.target_found = true;

    ProcessMemory memory(pid);
    const std::map<std::string, ModuleMap> modules = enumerate_modules(pid);
    if (modules.empty()) {
        status.errors.push_back("could not read /proc/" + std::to_string(pid) + "/maps");
        status.summary = "module map unavailable";
        return status;
    }

    // 1. Required signature offsets. Any failure is fatal: keep the old file.
    std::map<std::string, ModuleImage> image_cache;
    const BaseResolution base = resolve_base_offsets(memory, modules, &image_cache);
    status.base_offsets_resolved = static_cast<int>(base.offsets.size());
    status.warnings = base.warnings;
    if (!base.errors.empty()) {
        status.errors = base.errors;
        status.summary = "signature validation failed (" + std::to_string(base.errors.size()) +
                         " error(s), existing offsets.json kept)";
        return status;
    }

    // 2. Needed schema fields (best effort; a game field rename must not block
    //    the base offset refresh).
    SchemaDumpResult schema;
    const auto schema_module = modules.find(kSchemaModule);
    if (schema_module != modules.end()) {
        ModuleImage schemasystem;
        if (schemasystem.load(memory, schema_module->second)) {
            if (dump_needed_schema_fields(memory, schemasystem, &schema)) {
                status.schema_fields_resolved = static_cast<int>(schema.fields.size());
                status.schema_fields_missing =
                    static_cast<int>(kSchemaKeyCount) - status.schema_fields_resolved;
                if (status.schema_fields_missing > 0) {
                    for (const SchemaKey& key : kSchemaKeys) {
                        const std::string composite =
                            std::string(key.class_name) + "." + key.field_name;
                        if (schema.fields.count(composite) == 0) {
                            status.warnings.push_back("schema field missing in this build: " +
                                                      composite);
                        }
                    }
                }
            } else {
                status.warnings.push_back("schema walk failed: " + schema.error);
            }
        } else {
            status.warnings.push_back("could not map libschemasystem.so");
        }
    } else {
        status.warnings.push_back("libschemasystem.so not loaded");
    }

    // 3. ICvar fallback interface (convar reader also resolves it dynamically).
    std::optional<uint32_t> cvar_rva;
    const auto tier0_module = modules.find(kTier0Module);
    if (tier0_module != modules.end()) {
        ModuleImage tier0;
        if (tier0.load(memory, tier0_module->second)) {
            cvar_rva = resolve_cvar_interface_rva(memory, tier0);
            status.cvar_interface_resolved = cvar_rva.has_value();
            if (!cvar_rva.has_value()) {
                status.warnings.push_back("VEngineCvar interface not resolved in libtier0.so");
            }
        }
    } else {
        status.warnings.push_back("libtier0.so not loaded");
    }

    validate_live_reads(memory, modules, base.offsets, &status.warnings);

    // 4. Merge into the consolidated offsets.json without touching values that
    //    are not refreshed here (full tables, match-session offsets, ...).
    const std::filesystem::path primary = resolve_primary_offsets_path();
    std::string original;
    const bool existed = std::filesystem::exists(primary);
    if (existed && !read_file_bytes(primary, &original)) {
        status.errors.push_back("could not read " + primary.string());
        status.summary = "offsets.json unreadable";
        return status;
    }

    ordered_json root;
    if (existed) {
        try {
            root = ordered_json::parse(original);
        } catch (const std::exception& parse_error) {
            status.errors.push_back(std::string("offsets.json is not valid JSON: ") + parse_error.what());
            status.summary = "offsets.json invalid (left untouched)";
            return status;
        }
    } else {
        root = ordered_json::object();
    }
    if (!root.is_object()) {
        status.errors.push_back("offsets.json root is not an object");
        status.summary = "offsets.json invalid (left untouched)";
        return status;
    }
    if (!root.contains("offsets") || !root["offsets"].is_object()) root["offsets"] = ordered_json::object();
    if (!root.contains("interfaces") || !root["interfaces"].is_object()) root["interfaces"] = ordered_json::object();
    if (!root.contains("schemas") || !root["schemas"].is_object()) root["schemas"] = ordered_json::object();

    auto number_or_sentinel = [](const ordered_json& value) -> uint64_t {
        return value.is_number() ? value.get<uint64_t>() : UINT64_MAX;
    };

    bool changed = false;
    for (const auto& [name, value] : base.offsets) {
        const uint64_t previous = number_or_sentinel(root["offsets"].value(name, ordered_json()));
        if (previous != value) {
            ++status.base_offsets_changed;
            changed = true;
        }
        root["offsets"][name] = value;
    }
    for (const std::string& name : base.missing_optional) {
        if (root["offsets"].erase(name) > 0) {
            ++status.base_offsets_changed;
            changed = true;
        }
    }
    if (cvar_rva.has_value()) {
        const uint64_t previous = number_or_sentinel(root["interfaces"].value("tier0_VEngineCvar", ordered_json()));
        if (previous != *cvar_rva) {
            changed = true;
        }
        root["interfaces"]["tier0_VEngineCvar"] = *cvar_rva;
    }
    for (const auto& [key, value] : schema.fields) {
        const uint64_t previous = number_or_sentinel(root["schemas"].value(key, ordered_json()));
        if (previous != value) {
            ++status.schema_fields_changed;
            changed = true;
        }
        root["schemas"][key] = value;
    }

    status.success = true;
    if (!existed || changed) {
        // ensure_ascii keeps the historical escaping of the few non-UTF-8
        // interface keys, so game-update diffs stay minimal.
        const std::string merged = root.dump(4, ' ', true) + "\n";
        std::vector<std::filesystem::path> targets{ primary };
        auto add_synced_copy = [&](const std::filesystem::path& candidate) {
            std::error_code error;
            if (std::filesystem::exists(candidate, error) && !same_file(candidate, primary)) {
                targets.push_back(candidate);
            }
        };
        add_synced_copy(primary.parent_path() / "build" / "offsets.json");
        add_synced_copy(primary.parent_path() / ".." / "offsets.json");

        for (const std::filesystem::path& target : targets) {
            std::string current;
            if (read_file_bytes(target, &current) && current == merged) {
                continue;
            }
            if (!write_file_atomic(target, merged)) {
                status.errors.push_back("failed to write " + target.string());
                continue;
            }
            apply_sudo_ownership(target);
            status.wrote_file = true;
        }
        status.success = status.errors.empty();
    }

    std::ostringstream summary;
    summary << "base " << status.base_offsets_resolved << "/" << signature_definitions().size()
            << " (" << status.base_offsets_changed << " changed)";
    summary << "; schema " << status.schema_fields_resolved << "/" << kSchemaKeyCount << " ("
            << status.schema_fields_changed << " changed)";
    summary << "; cvar " << (status.cvar_interface_resolved ? "ok" : "unresolved");
    summary << (existed ? "; " : "; created ") << primary.string();
    if (status.wrote_file) {
        summary << " written";
    }
    status.summary = summary.str();
    return status;
}

RefreshStatus& mutable_last_status()
{
    static RefreshStatus status;
    return status;
}

void log_status(const RefreshStatus& status)
{
    if (status.errors.empty() && status.warnings.empty()) {
        BUNNYWARE_LOG_INFO("OffsetRefresher: {}", status.summary);
    } else {
        BUNNYWARE_LOG_INFO("OffsetRefresher: {}", status.summary);
        int logged = 0;
        for (const std::string& error : status.errors) {
            if (logged++ >= kMaxLogEntries) {
                BUNNYWARE_LOG_ERROR("OffsetRefresher: ... and {} more error(s)", status.errors.size() - kMaxLogEntries);
                break;
            }
            BUNNYWARE_LOG_ERROR("OffsetRefresher: {}", error);
        }
        logged = 0;
        for (const std::string& warning : status.warnings) {
            if (logged++ >= kMaxLogEntries) {
                BUNNYWARE_LOG_WARN("OffsetRefresher: ... and {} more warning(s)", status.warnings.size() - kMaxLogEntries);
                break;
            }
            BUNNYWARE_LOG_WARN("OffsetRefresher: {}", warning);
        }
    }
}

} // namespace

RefreshStatus refresh_from_live_process()
{
    RefreshStatus status = run_refresh();
    log_status(status);
    mutable_last_status() = status;
    return status;
}

const RefreshStatus& last_status()
{
    return mutable_last_status();
}

} // namespace offset_refresher
