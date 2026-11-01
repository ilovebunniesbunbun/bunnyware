#include "game_reader.hpp"
#include "../app/convar_reader.hpp"
#include "logger.hpp"
#include "offset_refresher.hpp"
#include "precision_sleep.hpp"
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <fstream>
#include <iostream>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <iomanip>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include "config.hpp"
#include "../math/ViewMatrix.hpp"

// ---------------------------------------------------------------------------
// C_SmokeGrenadeProjectile voxel-frame layout.
//
// Reversed statically from libclient.so (build 2000927, ClientVersion
// 2000927 / Oct 5 2026) in the voxel decode path:
//   - m_VoxelFrameData (schema field, resolved from offsets.json) is a
//     C_NetworkUtlVectorBase (0x18 bytes; element count at +0x00, data
//     pointer at +0x08). On this build it sits at C_SmokeGrenadeProjectile
//     +0x1310.
//   - The frame-decoder state follows the vector. The decoder (driven from
//     the class's per-frame update while m_bSmokeVolumeDataReceived is set)
//     keeps:
//       * voxel +0x100: frame index counter, incremented once per decoded
//         frame (class +0x1410 on this build);
//       * voxel +0x110: RGBA buffer pointer built for the 32^3 voxel grid,
//         lazily allocated to 32*32*32*4 = 131072 bytes on first decode
//         (class +0x1420 on this build).
//   - Class size is 0x1488 (operator new/delete both use it), so both
//     members are in bounds.
// ---------------------------------------------------------------------------
constexpr uint32_t kSmokeVoxelFrameIndexDelta = 0x100;
constexpr uint32_t kSmokeVoxelRgbaPointerDelta = 0x110;
constexpr uint32_t kSmokeVoxelRgbaBytes = 32u * 32u * 32u * 4u; // 131072

static bool is_w_pressed_x11() {
    static Display* d = nullptr;
    static bool init_attempted = false;
    if (!init_attempted) {
        d = XOpenDisplay(NULL);
        init_attempted = true;
    }
    if (d) {
        char keys_return[32];
        XQueryKeymap(d, keys_return);
        KeyCode kc = XKeysymToKeycode(d, XK_w);
        if (kc != 0) {
            return !!(keys_return[kc >> 3] & (1 << (kc & 7)));
        }
    }
    return false;
}


GameReader::GameReader() {
    latest_packet = {};
}

GameReader::~GameReader() {
    shutdown();
}

bool GameReader::read_raw(uintptr_t address, void* dest, size_t size) const {
    if (target_pid <= 0 || address == 0) return false;
    struct iovec local_iov = { dest, size };
    struct iovec remote_iov = { reinterpret_cast<void*>(address), size };
    ssize_t bytes = process_vm_readv(target_pid, &local_iov, 1, &remote_iov, 1, 0);
    return bytes == static_cast<ssize_t>(size);
}

std::string GameReader::read_str(uintptr_t address, size_t max_len) const {
    if (target_pid <= 0 || address == 0) return "";
    std::string s;
    s.resize(max_len);
    struct iovec local_iov = { s.data(), max_len };
    struct iovec remote_iov = { reinterpret_cast<void*>(address), max_len };
    ssize_t bytes = process_vm_readv(target_pid, &local_iov, 1, &remote_iov, 1, 0);
    if (bytes <= 0) return "";
    size_t len = strnlen(s.c_str(), max_len);
    s.resize(len);
    return s;
}

uintptr_t GameReader::get_entity(int index) const {
    if (index < 0 || index >= 32768) return 0;
    uintptr_t entity_list_base = read_mem<uintptr_t>(client_base + dwEntityList);
    if (!entity_list_base) return 0;
    uintptr_t chunk = read_mem<uintptr_t>(entity_list_base + 8 * ((index & 0x7FFF) >> 9));
    if (!chunk) return 0;
    uintptr_t identity = chunk + 112 * (index & 0x1FF);
    return read_mem<uintptr_t>(identity);
}

uintptr_t GameReader::get_entity_from_handle(uint32_t handle) const {
    if (handle == 0 || handle == 0xFFFFFFFF) return 0;
    return get_entity(handle & 0x7FFF);
}

uintptr_t GameReader::get_entity_cached(uintptr_t entity_list_base, int index) const {
    if (index < 0 || index >= 32768 || !entity_list_base) return 0;
    uintptr_t chunk = read_mem<uintptr_t>(entity_list_base + 8 * ((index & 0x7FFF) >> 9));
    if (!chunk) return 0;
    uintptr_t identity = chunk + 112 * (index & 0x1FF);
    return read_mem<uintptr_t>(identity);
}

uintptr_t GameReader::get_entity_from_handle_cached(uintptr_t entity_list_base, uint32_t handle) const {
    if (handle == 0 || handle == 0xFFFFFFFF) return 0;
    return get_entity_cached(entity_list_base, handle & 0x7FFF);
}

bool GameReader::attach_to_process() {
    target_pid = -1;
    client_base = 0;
    engine_base = 0;
    schemasystem_base = 0;

    // 1. Find PID of "cs2"
    DIR* dir = opendir("/proc");
    if (!dir) return false;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_type != DT_DIR) continue;

        char* end;
        long pid = strtol(entry->d_name, &end, 10);
        if (*end != '\0') continue;

        char comm_path[128];
        snprintf(comm_path, sizeof(comm_path), "/proc/%ld/comm", pid);
        std::ifstream comm_file(comm_path);
        if (comm_file.is_open()) {
            std::string comm;
            std::getline(comm_file, comm);
            if (comm == "cs2") {
                target_pid = pid;
                break;
            }
        }
    }
    closedir(dir);

    int current_pid = target_pid.load();
    if (current_pid <= 0) return false;

    BUNNYWARE_LOG_INFO("GameReader: Found cs2 process with PID: {}", current_pid);

    // 2. Parse /proc/PID/maps for bases
    char maps_path[128];
    snprintf(maps_path, sizeof(maps_path), "/proc/%d/maps", current_pid);
    std::ifstream maps_file(maps_path);
    if (!maps_file.is_open()) {
        BUNNYWARE_LOG_ERROR("GameReader: Failed to open maps file for PID {}", current_pid);
        target_pid = -1;
        return false;
    }

    std::string line;
    uintptr_t local_client_base = 0;
    uintptr_t local_engine_base = 0;
    uintptr_t local_schema_base = 0;
    uintptr_t local_tier0_base = 0;
    std::string local_tier0_path;
    while (std::getline(maps_file, line)) {
        if (line.find("libclient.so") != std::string::npos && local_client_base == 0) {
            local_client_base = std::stoul(line.substr(0, line.find('-')), nullptr, 16);
        } else if (line.find("libengine2.so") != std::string::npos && local_engine_base == 0) {
            local_engine_base = std::stoul(line.substr(0, line.find('-')), nullptr, 16);
        } else if (line.find("libschemasystem.so") != std::string::npos && local_schema_base == 0) {
            local_schema_base = std::stoul(line.substr(0, line.find('-')), nullptr, 16);
        } else if (line.find("libtier0.so") != std::string::npos && local_tier0_base == 0) {
            local_tier0_base = std::stoul(line.substr(0, line.find('-')), nullptr, 16);
            const auto path_pos = line.find('/');
            if (path_pos != std::string::npos) {
                local_tier0_path = line.substr(path_pos);
                if (!local_tier0_path.empty() && local_tier0_path.back() == '\r') {
                    local_tier0_path.pop_back();
                }
            }
        }
    }

    if (local_client_base == 0 || local_schema_base == 0) {
        BUNNYWARE_LOG_ERROR("GameReader: Failed to resolve critical bases (client_base: {:#x}, schemasystem_base: {:#x})", local_client_base, local_schema_base);
        target_pid = -1;
        return false;
    }

    client_base = local_client_base;
    engine_base = local_engine_base;
    schemasystem_base = local_schema_base;
    tier0_base = local_tier0_base;
    tier0_path = local_tier0_path;

    if (local_tier0_base != 0) {
        cvar_interface = resolve_cvar_interface_locked();
    } else {
        cvar_interface = 0;
    }

    BUNNYWARE_LOG_INFO("GameReader: Successfully attached to cs2 process. Bases resolved - client: {:#x}, schemasystem: {:#x}, engine: {:#x}, tier0: {:#x}, cvar: {:#x}",
                     local_client_base, local_schema_base, local_engine_base, local_tier0_base, cvar_interface.load());
    return true;
}

bool GameReader::load_offsets_and_schemas() {
    // 1. Resolve JSON paths
    std::string offsets_path = "offsets.json";
    std::ifstream f_offsets(offsets_path);
    if (!f_offsets.is_open()) {
        offsets_path = "../offsets.json";
        f_offsets.open(offsets_path);
    }
    if (!f_offsets.is_open()) {
        BUNNYWARE_LOG_ERROR("GameReader: Failed to open offsets.json in any of the checked paths.");
        return false;
    }

    BUNNYWARE_LOG_INFO("GameReader: Loaded consolidated offsets from {}", offsets_path);

    nlohmann::json j_data;
    try {
        f_offsets >> j_data;
        
        // 1. Parse base offsets
        if (j_data.contains("offsets")) {
            auto o = j_data["offsets"];
            auto get_offset = [&](const std::string& name) -> uintptr_t {
                if (o.contains(name)) return o[name].get<uintptr_t>();
                return 0;
            };
            dwEntityList = get_offset("dwEntityList");
            dwViewMatrix = get_offset("dwViewMatrix");
            dwLocalPlayerPawn = get_offset("dwLocalPlayerPawn");
            dwLocalPlayerController = get_offset("dwLocalPlayerController");
            dwGlobalVars = get_offset("dwGlobalVars");
            dwViewAngles = get_offset("dwViewAngles");
            dwCSGOInput = get_offset("dwCSGOInput");
            dwWindowWidth = get_offset("dwWindowWidth");
            dwWindowHeight = get_offset("dwWindowHeight");
            dwGameRules = get_offset("dwGameRules");
            dwSensitivity = get_offset("dwSensitivity");
            dwSensitivity_sensitivity = get_offset("dwSensitivity_sensitivity");
            dwBulletServicesVTable = get_offset("dwBulletServicesVTable");
            dwBulletImpactGlobals = get_offset("dwBulletImpactGlobals");
            dwBulletServices_impacts = get_offset("dwBulletServices_impacts");
            if (!dwBulletServicesVTable || !dwBulletImpactGlobals || !dwBulletServices_impacts)
                BUNNYWARE_LOG_WARN("GameReader: Bullet Impacts impact signatures missing; native impacts unavailable");
            // Prefer new names; fall back to older keys if present
            dwMatchSessionPointer = get_offset("dwMatchSessionPointer");
            if (dwMatchSessionPointer == 0) {
                dwMatchSessionPointer = get_offset("dwMatchmakingReservation");
            }
            if (o.contains("dwMatchSessionPointer_state")) {
                dwMatchSessionPointer_state = static_cast<uint32_t>(o["dwMatchSessionPointer_state"].get<uintptr_t>());
            } else if (o.contains("dwMatchmakingReservation_state")) {
                dwMatchSessionPointer_state = static_cast<uint32_t>(o["dwMatchmakingReservation_state"].get<uintptr_t>());
            }
            
            BUNNYWARE_LOG_DEBUG("GameReader: Parsed base offsets: dwEntityList: {:#x}, dwViewMatrix: {:#x}, dwLocalPlayerPawn: {:#x}, dwLocalPlayerController: {:#x}, dwGlobalVars: {:#x}, dwViewAngles: {:#x}, dwCSGOInput: {:#x}, dwMatchSessionPointer: {:#x} (state+{:#x})",
                          dwEntityList, dwViewMatrix, dwLocalPlayerPawn, dwLocalPlayerController, dwGlobalVars, dwViewAngles, dwCSGOInput,
                          dwMatchSessionPointer, dwMatchSessionPointer_state);
        } else {
            BUNNYWARE_LOG_ERROR("GameReader: consolidated offsets.json does not contain 'offsets' key");
            return false;
        }

        if (j_data.contains("interfaces")) {
            auto interfaces = j_data["interfaces"];
            if (interfaces.contains("tier0_VEngineCvar")) {
                tier0_VEngineCvar_rva = interfaces["tier0_VEngineCvar"].get<uintptr_t>();
                BUNNYWARE_LOG_INFO("GameReader: Loaded tier0_VEngineCvar interface RVA: {:#x}", tier0_VEngineCvar_rva);
            }
        }

        // 2. Parse schemas
        if (j_data.contains("schemas")) {
            auto s = j_data["schemas"];
            auto get_schema = [&](const std::string& class_name, const std::string& field_name) -> uint32_t {
                std::string key = class_name + "." + field_name;
                if (s.contains(key)) {
                    uint32_t offset = s[key].get<uint32_t>();
                    BUNNYWARE_LOG_DEBUG("GameReader: Resolved schema offset {} -> {:#x}", key, offset);
                    return offset;
                }
                BUNNYWARE_LOG_WARN("GameReader: Failed to resolve schema offset for {}", key);
                return 0;
            };

            m_pGameSceneNode = get_schema("C_BaseEntity", "m_pGameSceneNode");
            m_modelState = get_schema("CSkeletonInstance", "m_modelState");
            m_ModelName = get_schema("CModelState", "m_ModelName");
            if (m_ModelName == 0) {
                m_ModelName = 0xA8;
                BUNNYWARE_LOG_INFO("GameReader: Using default fallback for m_ModelName: {:#x}", m_ModelName);
            }

            m_lifeState = get_schema("C_BaseEntity", "m_lifeState");
            m_iTeamNum = get_schema("C_BaseEntity", "m_iTeamNum");
            m_iHealth = get_schema("C_BaseEntity", "m_iHealth");
            m_pWeaponServices = get_schema("C_BasePlayerPawn", "m_pWeaponServices");
            m_hActiveWeapon = get_schema("CPlayer_WeaponServices", "m_hActiveWeapon");
            m_hMyWeapons = get_schema("CPlayer_WeaponServices", "m_hMyWeapons");
            if (m_hMyWeapons == 0) {
                m_hMyWeapons = 72; // 0x48 fallback
                BUNNYWARE_LOG_INFO("GameReader: Using default fallback for m_hMyWeapons: {}", m_hMyWeapons);
            }
            m_flFlashOverlayAlpha = get_schema("C_CSPlayerPawnBase", "m_flFlashOverlayAlpha");
            if (m_flFlashOverlayAlpha == 0) {
                m_flFlashOverlayAlpha = 0x13A4;
                BUNNYWARE_LOG_INFO("GameReader: Using default fallback for m_flFlashOverlayAlpha: {:#x}", m_flFlashOverlayAlpha);
            }
            BUNNYWARE_LOG_INFO("GameReader: Local flash alpha offset (m_flFlashOverlayAlpha) = {:#x}",
                             m_flFlashOverlayAlpha);
            m_bPinPulled = get_schema("C_BaseCSGrenade", "m_bPinPulled");
            m_flThrowStrength = get_schema("C_BaseCSGrenade", "m_flThrowStrength");
            
            m_vecAbsVelocity = get_schema("C_BaseEntity", "m_vecAbsVelocity");
            if (m_vecAbsVelocity == 0) {
                m_vecAbsVelocity = get_schema("C_BaseEntity", "m_vecVelocity");
                BUNNYWARE_LOG_INFO("GameReader: Resolved m_vecVelocity instead of m_vecAbsVelocity: {:#x}", m_vecAbsVelocity);
            }

            m_vInitialPosition = get_schema("C_BaseCSGrenadeProjectile", "m_vInitialPosition");
            m_vInitialVelocity = get_schema("C_BaseCSGrenadeProjectile", "m_vInitialVelocity");
            m_flSimulationTime = get_schema("C_BaseEntity", "m_flSimulationTime");
            m_vSmokeDetonationPos = get_schema("C_SmokeGrenadeProjectile", "m_vSmokeDetonationPos");
            m_vecAbsOrigin = get_schema("CGameSceneNode", "m_vecAbsOrigin");
            m_angAbsRotation = get_schema("CGameSceneNode", "m_angAbsRotation");
            if (m_angAbsRotation == 0) m_angAbsRotation = 0xDC;
            m_flAbsScale = get_schema("CGameSceneNode", "m_flAbsScale");
            if (m_flAbsScale == 0) m_flAbsScale = 0xE8;
            m_pEntity = get_schema("CEntityInstance", "m_pEntity");

            m_bFireIsBurning = get_schema("C_Inferno", "m_bFireIsBurning");
            m_fireCount = get_schema("C_Inferno", "m_fireCount");
            m_firePositions = get_schema("C_Inferno", "m_firePositions");
            m_hOwnerEntity = get_schema("C_BaseEntity", "m_hOwnerEntity");
            m_hThrower = get_schema("C_BaseGrenade", "m_hThrower");
            m_bIsIncGrenade = get_schema("C_MolotovProjectile", "m_bIsIncGrenade");
            m_nExplodeEffectTickBegin = get_schema("C_BaseCSGrenadeProjectile", "m_nExplodeEffectTickBegin");
            m_bDidSmokeEffect = get_schema("C_SmokeGrenadeProjectile", "m_bDidSmokeEffect");
            m_nSmokeEffectTickBegin = get_schema("C_SmokeGrenadeProjectile", "m_nSmokeEffectTickBegin");
            m_VoxelFrameData = get_schema("C_SmokeGrenadeProjectile", "m_VoxelFrameData");
            m_nVoxelFrameDataSize = get_schema("C_SmokeGrenadeProjectile", "m_nVoxelFrameDataSize");
            m_bSmokeVolumeDataReceived = get_schema("C_SmokeGrenadeProjectile", "m_bSmokeVolumeDataReceived");
            m_bSmokeEffectSpawned = get_schema("C_SmokeGrenadeProjectile", "m_bSmokeEffectSpawned");
            m_nDecoyShotTick = get_schema("C_DecoyProjectile", "m_nDecoyShotTick");

            m_AttributeManager = get_schema("C_EconEntity", "m_AttributeManager");
            m_Item = get_schema("C_AttributeContainer", "m_Item");
            m_iItemDefinitionIndex = get_schema("C_EconItemView", "m_iItemDefinitionIndex");
            m_vOldOrigin = get_schema("C_BasePlayerPawn", "m_vOldOrigin");
            m_pItemServices = get_schema("C_BasePlayerPawn", "m_pItemServices");
            m_bHasDefuser = get_schema("CCSPlayer_ItemServices", "m_bHasDefuser");
            m_bPawnHasDefuser = get_schema("CCSPlayerController", "m_bPawnHasDefuser");
            if (m_bPawnHasDefuser == 0) {
                m_bPawnHasDefuser = 2720;
                BUNNYWARE_LOG_INFO("GameReader: Using default fallback for m_bPawnHasDefuser: {}", m_bPawnHasDefuser);
            }
            m_szCrosshairCodes = get_schema("CCSPlayerController", "m_szCrosshairCodes");
            if (m_szCrosshairCodes == 0) {
                m_szCrosshairCodes = 0x9C0;
                BUNNYWARE_LOG_INFO("GameReader: Using default fallback for m_szCrosshairCodes: {:#x}",
                                 m_szCrosshairCodes);
            }
            m_ArmorValue = get_schema("C_CSPlayerPawn", "m_ArmorValue");
            if (m_ArmorValue == 0) {
                m_ArmorValue = 11020;
                BUNNYWARE_LOG_INFO("GameReader: Using default fallback for m_ArmorValue: {}", m_ArmorValue);
            }
            m_angEyeAngles = get_schema("C_CSPlayerPawn", "m_angEyeAngles");
            
            // The field moved to CBasePlayerController; keep the older class as a fallback.
            m_iszPlayerName = get_schema("CBasePlayerController", "m_iszPlayerName");
            if (m_iszPlayerName == 0) m_iszPlayerName = get_schema("CCSPlayerController", "m_iszPlayerName");
            if (m_iszPlayerName == 0) m_iszPlayerName = 2172;
            m_bIsScoped = get_schema("C_CSPlayerPawn", "m_bIsScoped");
            if (m_bIsScoped == 0) m_bIsScoped = 10976;
            m_flFOVSensitivityAdjust = get_schema("C_BasePlayerPawn", "m_flFOVSensitivityAdjust");
            if (m_flFOVSensitivityAdjust == 0) m_flFOVSensitivityAdjust = 4880;
            m_bInReload = get_schema("C_CSWeaponBase", "m_bInReload");
            if (m_bInReload == 0) m_bInReload = 9860;
            m_bPrevHelmet = get_schema("C_CSPlayerPawn", "m_bPrevHelmet");
            if (m_bPrevHelmet == 0) m_bPrevHelmet = 5199;
            m_nSubclassID = get_schema("C_BaseEntity", "m_nSubclassID");
            if (m_nSubclassID == 0) m_nSubclassID = 1264;
            m_szName = get_schema("CCSWeaponBaseVData", "m_szName");
            if (m_szName == 0) m_szName = 1824;
            m_nDamage = get_schema("CCSWeaponBaseVData", "m_nDamage");
            if (m_nDamage == 0) m_nDamage = 2088;
            m_flRangeModifier = get_schema("CCSWeaponBaseVData", "m_flRangeModifier");
            if (m_flRangeModifier == 0) m_flRangeModifier = 2108;
            m_flPenetration = get_schema("CCSWeaponBaseVData", "m_flPenetration");
            if (m_flPenetration == 0) m_flPenetration = 2100;
            m_flArmorRatio = get_schema("CCSWeaponBaseVData", "m_flArmorRatio");
            if (m_flArmorRatio == 0) m_flArmorRatio = 2096;
            m_flHeadshotMultiplier = get_schema("CCSWeaponBaseVData", "m_flHeadshotMultiplier");
            if (m_flHeadshotMultiplier == 0) m_flHeadshotMultiplier = 2092;
            m_iClip1 = get_schema("C_BasePlayerWeapon", "m_iClip1");
            if (m_iClip1 == 0) m_iClip1 = 9576;
            m_iMaxClip1 = get_schema("CBasePlayerWeaponVData", "m_iMaxClip1");
            if (m_iMaxClip1 == 0) m_iMaxClip1 = 1232;
            
            m_pActionTrackingServices = get_schema("CCSPlayerController", "m_pActionTrackingServices");
            if (m_pActionTrackingServices == 0) m_pActionTrackingServices = 2456;
            
            m_flTotalRoundDamageDealt = get_schema("CCSPlayerController_ActionTrackingServices", "m_flTotalRoundDamageDealt");
            if (m_flTotalRoundDamageDealt == 0) m_flTotalRoundDamageDealt = 296;
            
            m_iNumRoundKills = get_schema("CCSPlayerController_ActionTrackingServices", "m_iNumRoundKills");
            if (m_iNumRoundKills == 0) m_iNumRoundKills = 288;

            m_iNumRoundKillsHeadshots = get_schema("CCSPlayerController_ActionTrackingServices", "m_iNumRoundKillsHeadshots");
            if (m_iNumRoundKillsHeadshots == 0) m_iNumRoundKillsHeadshots = 292;
            
            m_hController = get_schema("C_BasePlayerPawn", "m_hController");
            if (m_hController == 0) {
                m_hController = 4912;
                BUNNYWARE_LOG_INFO("GameReader: Using default fallback for m_hController: {}", m_hController);
            }
            m_hPawn = get_schema("CBasePlayerController", "m_hPawn");
            if (m_hPawn == 0) {
                m_hPawn = 2108;
                BUNNYWARE_LOG_INFO("GameReader: Using default fallback for m_hPawn: {}", m_hPawn);
            }
            m_vecViewOffset = get_schema("C_BaseModelEntity", "m_vecViewOffset");
            if (m_vecViewOffset == 0) {
                m_vecViewOffset = 3568;
                BUNNYWARE_LOG_INFO("GameReader: Using default fallback for m_vecViewOffset: {}", m_vecViewOffset);
            }
            m_bBeingDefused = get_schema("C_PlantedC4", "m_bBeingDefused");
            if (m_bBeingDefused == 0) {
                m_bBeingDefused = 4388;
                BUNNYWARE_LOG_INFO("GameReader: Using default fallback for m_bBeingDefused: {}", m_bBeingDefused);
            }
            m_bBombDefused = get_schema("C_PlantedC4", "m_bBombDefused");
            if (m_bBombDefused == 0) {
                m_bBombDefused = 4412;
                BUNNYWARE_LOG_INFO("GameReader: Using default fallback for m_bBombDefused: {}", m_bBombDefused);
            }
            m_bHasExploded = get_schema("C_PlantedC4", "m_bHasExploded");
            if (m_bHasExploded == 0) {
                m_bHasExploded = 4381;
                BUNNYWARE_LOG_INFO("GameReader: Using default fallback for m_bHasExploded: {}", m_bHasExploded);
            }
            m_flC4Blow = get_schema("C_PlantedC4", "m_flC4Blow");
            if (m_flC4Blow == 0) {
                m_flC4Blow = 4376;
                BUNNYWARE_LOG_INFO("GameReader: Using default fallback for m_flC4Blow: {}", m_flC4Blow);
            }
            m_flDefuseCountDown = get_schema("C_PlantedC4", "m_flDefuseCountDown");
            if (m_flDefuseCountDown == 0) {
                m_flDefuseCountDown = 4408;
                BUNNYWARE_LOG_INFO("GameReader: Using default fallback for m_flDefuseCountDown: {}", m_flDefuseCountDown);
            }

            m_iIDEntIndex = get_schema("C_CSPlayerPawn", "m_iIDEntIndex");
            if (m_iIDEntIndex == 0) m_iIDEntIndex = 0x4294;

            m_pAimPunchServices = get_schema("C_CSPlayerPawn", "m_pAimPunchServices");
            if (m_pAimPunchServices == 0) m_pAimPunchServices = 0x1418;
            m_pBulletServices = get_schema("C_CSPlayerPawn", "m_pBulletServices");

            m_predictableBaseAngle = get_schema("CCSPlayer_AimPunchServices", "m_predictableBaseAngle");
            if (m_predictableBaseAngle == 0) m_predictableBaseAngle = 0x50;

            m_bIsWalking = get_schema("C_CSPlayerPawn", "m_bIsWalking");
            if (m_bIsWalking == 0) m_bIsWalking = 0x2AE0;
            m_flEmitSoundTime = get_schema("C_CSPlayerPawn", "m_flEmitSoundTime");
            if (m_flEmitSoundTime == 0) m_flEmitSoundTime = 0x2B08;
            m_iShotsFired = get_schema("C_CSPlayerPawn", "m_iShotsFired");
            if (m_iShotsFired == 0) m_iShotsFired = 0x2B14;
            m_bStartedArming = get_schema("C_C4", "m_bStartedArming");
            if (m_bStartedArming == 0) m_bStartedArming = 0x2B70;

            // Web radar: round/match state and team scores.
            m_iTeamScore = get_schema("C_Team", "m_iScore");
            m_bFreezePeriod = get_schema("C_CSGameRules", "m_bFreezePeriod");
            m_bWarmupPeriod = get_schema("C_CSGameRules", "m_bWarmupPeriod");
            m_iRoundTime = get_schema("C_CSGameRules", "m_iRoundTime");
            m_totalRoundsPlayed = get_schema("C_CSGameRules", "m_totalRoundsPlayed");
            m_gamePhase = get_schema("C_CSGameRules", "m_gamePhase");
            m_fRoundStartTime = get_schema("C_CSGameRules", "m_fRoundStartTime");
            if (m_iTeamScore == 0) m_iTeamScore = 0x7B0;   // C_Team.m_iScore fallback
            if (m_bFreezePeriod == 0) m_bFreezePeriod = 0x39;
            if (m_bWarmupPeriod == 0) m_bWarmupPeriod = 0x3A;
            if (m_iRoundTime == 0) m_iRoundTime = 0x60;
            if (m_totalRoundsPlayed == 0) m_totalRoundsPlayed = 0x80;
            if (m_gamePhase == 0) m_gamePhase = 0x7C;
            if (m_fRoundStartTime == 0) m_fRoundStartTime = 0x68;

            m_bPawnIsAlive = get_schema("CCSPlayerController", "m_bPawnIsAlive");
            if (m_bPawnIsAlive == 0) m_bPawnIsAlive = 0xa9c;

            m_hObserverPawn = get_schema("CCSPlayerController", "m_hObserverPawn");
            if (m_hObserverPawn == 0) m_hObserverPawn = 0xa98;

            m_hPlayerPawn = get_schema("CCSPlayerController", "m_hPlayerPawn");
            if (m_hPlayerPawn == 0) m_hPlayerPawn = 0xa94;

            m_pObserverServices = get_schema("C_BasePlayerPawn", "m_pObserverServices");
            if (m_pObserverServices == 0) m_pObserverServices = 0x11a8;

            m_hObserverTarget = get_schema("CPlayer_ObserverServices", "m_hObserverTarget");
            if (m_hObserverTarget == 0) m_hObserverTarget = 0x4c;

            m_iObserverMode = get_schema("CPlayer_ObserverServices", "m_iObserverMode");
            if (m_iObserverMode == 0) m_iObserverMode = 0x48;

            m_weaponMode = get_schema("C_CSWeaponBase", "m_weaponMode");
            if (m_weaponMode == 0) m_weaponMode = 0x2648;

            m_fAccuracyPenalty = get_schema("C_CSWeaponBase", "m_fAccuracyPenalty");
            if (m_fAccuracyPenalty == 0) m_fAccuracyPenalty = 0x2660;

            m_flSpread = get_schema("CCSWeaponBaseVData", "m_flSpread");
            if (m_flSpread == 0) m_flSpread = 0x758;

            m_flMaxSpeed = get_schema("CCSWeaponBaseVData", "m_flMaxSpeed");
            if (m_flMaxSpeed == 0) m_flMaxSpeed = 0x750;

            m_flInaccuracyMove = get_schema("CCSWeaponBaseVData", "m_flInaccuracyMove");
            if (m_flInaccuracyMove == 0) m_flInaccuracyMove = 0x790;

            m_flInaccuracyJumpInitial = get_schema("CCSWeaponBaseVData", "m_flInaccuracyJumpInitial");
            if (m_flInaccuracyJumpInitial == 0) m_flInaccuracyJumpInitial = 0x7C0;

            m_fFlags = get_schema("C_BaseEntity", "m_fFlags");
            if (m_fFlags == 0) m_fFlags = 0x568;

            m_MoveType = get_schema("C_BaseEntity", "m_MoveType");
            if (m_MoveType == 0) m_MoveType = 0x69D;

            m_vecVelocity = get_schema("C_BaseEntity", "m_vecVelocity");
            if (m_vecVelocity == 0) m_vecVelocity = 0x5A0;

            m_nNumBullets = get_schema("CCSWeaponBaseVData", "m_nNumBullets");
            if (m_nNumBullets == 0) m_nNumBullets = 1848;

            m_nSpreadSeed = get_schema("CCSWeaponBaseVData", "m_nSpreadSeed");
            if (m_nSpreadSeed == 0) m_nSpreadSeed = 2016;

            m_nRecoilSeed = get_schema("CCSWeaponBaseVData", "m_nRecoilSeed");
            if (m_nRecoilSeed == 0) m_nRecoilSeed = 2012;

            m_flRecoilIndex = get_schema("C_CSWeaponBase", "m_flRecoilIndex");
            if (m_flRecoilIndex == 0) m_flRecoilIndex = 9840;

            m_nTickBase = get_schema("CBasePlayerController", "m_nTickBase");
            if (m_nTickBase == 0) m_nTickBase = 2104;

            m_nSimulationTick = get_schema("C_BaseEntity", "m_nSimulationTick");
            if (m_nSimulationTick == 0) m_nSimulationTick = 1280;

            m_iPing = get_schema("CCSPlayerController", "m_iPing");
            if (m_iPing == 0) m_iPing = 2480;
        } else {
            BUNNYWARE_LOG_ERROR("GameReader: consolidated offsets.json does not contain 'schemas' key");
            return false;
        }

    } catch (const std::exception& e) {
        BUNNYWARE_LOG_ERROR("GameReader: Exception parsing offsets.json: {}", e.what());
        return false;
    } catch (...) {
        BUNNYWARE_LOG_ERROR("GameReader: Unknown exception parsing offsets.json");
        return false;
    }

    if (dwEntityList == 0 || dwViewMatrix == 0) {
        BUNNYWARE_LOG_ERROR("GameReader: Critical offset missing (dwEntityList: {:#x}, dwViewMatrix: {:#x})", dwEntityList, dwViewMatrix);
        return false;
    }
    return true;
}

std::string GameReader::resolve_model_name(uintptr_t pawn_addr, uintptr_t scene_node) {
    if (!scene_node) return "";
    
    // Check local cache first (if key is valid)
    if (pawn_addr != 0) {
        auto it = model_name_cache.find(pawn_addr);
        if (it != model_name_cache.end()) {
            return it->second;
        }
    }

    std::string model_name = "";
    uintptr_t model_handle = read_mem<uintptr_t>(scene_node + 0x210);
    if (model_handle) {
        uintptr_t cmodel = read_mem<uintptr_t>(model_handle + 0);
        if (cmodel) {
            uintptr_t ptrs[48];
            if (read_raw(cmodel + 8, ptrs, sizeof(ptrs))) {
                for (int idx = 0; idx < 48; ++idx) {
                    uintptr_t p = ptrs[idx];
                    if (p) {
                        std::string s = read_str(p, 128);
                        if (s.length() >= 12 && s.length() <= 320) {
                            // Check if it fits typical agent model patterns
                            if (s.find("ctm_") != std::string::npos || s.find("tm_") != std::string::npos ||
                                s.find("characters/") != std::string::npos || s.find("models/") != std::string::npos ||
                                s.find(".vmdl") != std::string::npos) {
                                model_name = s;
                                break;
                            }
                        }
                    }
                }
            }
        }
    }

    if (model_name.empty()) {
        uintptr_t name_symbol = read_mem<uintptr_t>(scene_node + m_modelState + m_ModelName);
        if (name_symbol) {
            std::string s = read_str(name_symbol, 128);
            if (s.length() >= 12 && s.length() <= 320) {
                if (s.find("ctm_") != std::string::npos || s.find("tm_") != std::string::npos ||
                    s.find("characters/") != std::string::npos || s.find("models/") != std::string::npos ||
                    s.find(".vmdl") != std::string::npos) {
                    model_name = s;
                }
            }
        }
    }

    if (pawn_addr != 0 && !model_name.empty()) {
        model_name_cache[pawn_addr] = model_name;
    }
    return model_name;
}

// Full skinning needs the entire bone cache. Skeleton/vis/hitbox ESP only need
// low indices (chains + key vis bones + capsules) — max used id is 23.
static bool cfg_needs_full_bone_cache(const OverlayConfig* cfg)
{
    if (!cfg) return true;
    if (cfg->style_vis.value != ChamsStyleEnum::DISABLED) return true;
    if (cfg->style_invis.value != ChamsStyleEnum::DISABLED) return true;
    if (cfg->glow_enabled) return true;
    if (cfg->esp_enabled && cfg->esp_skeleton && cfg->esp_skeleton_glow) return true;
    if (cfg->kill_effect.value == KillFxModeEnum::CHAMS) return true;
    if (cfg->hit_effect_mode.value == KillFxModeEnum::CHAMS) return true;
    return false;
}

void GameReader::update_bone_cache(PlayerData& player_pkt, uintptr_t scene_node) {
    player_pkt.bone_count = 0;
    if (!scene_node) return;

    uintptr_t bone_cache_ptr = read_mem<uintptr_t>(scene_node + m_modelState + 0x80);
    if (!bone_cache_ptr) return;

    // Define the raw bone structure for direct process_vm_readv copy
    struct RawBone {
        float pos[3];
        float scale;
        float rot[4];
    };

    const bool full_bones = cfg_needs_full_bone_cache(cfg);
    // Sparse path: bones 0..23 covers skeleton chains, BVH key bones, and hitbox capsules
    constexpr int kSparseBoneCount = 24;
    const int bone_limit = full_bones ? 128 : kSparseBoneCount;

    RawBone raw_bones[128];
    struct iovec local_iov[1];
    struct iovec remote_iov[1];

    const size_t bytes_wanted = static_cast<size_t>(bone_limit) * sizeof(RawBone);
    local_iov[0].iov_base = raw_bones;
    local_iov[0].iov_len = bytes_wanted;
    remote_iov[0].iov_base = reinterpret_cast<void*>(bone_cache_ptr);
    remote_iov[0].iov_len = bytes_wanted;

    ssize_t bytes_read = process_vm_readv(target_pid, local_iov, 1, remote_iov, 1, 0);
    if (bytes_read > 0) {
        int bones_read = static_cast<int>(bytes_read / static_cast<ssize_t>(sizeof(RawBone)));
        player_pkt.bone_count = bones_read;
        for (int b = 0; b < bones_read; ++b) {
            player_pkt.bones[b].position.x = raw_bones[b].pos[0];
            player_pkt.bones[b].position.y = raw_bones[b].pos[1];
            player_pkt.bones[b].position.z = raw_bones[b].pos[2];
            player_pkt.bones[b].rotation.x = raw_bones[b].rot[0];
            player_pkt.bones[b].rotation.y = raw_bones[b].rot[1];
            player_pkt.bones[b].rotation.z = raw_bones[b].rot[2];
            player_pkt.bones[b].rotation.w = raw_bones[b].rot[3];
        }
    } else {
        // Fallback to sequential bone reads (reading 32 bytes per bone in a single syscall)
        int bones_read = 0;
        for (int b = 0; b < bone_limit; ++b) {
            RawBone bone;
            if (!read_raw(bone_cache_ptr + b * sizeof(RawBone), &bone, sizeof(RawBone))) {
                // If a read fails, we've hit the end of the valid bone cache. Stop immediately to prevent useless syscalls.
                break;
            }
            player_pkt.bones[b].position = {bone.pos[0], bone.pos[1], bone.pos[2]};
            player_pkt.bones[b].rotation = {bone.rot[0], bone.rot[1], bone.rot[2], bone.rot[3]};
            bones_read++;
        }
        player_pkt.bone_count = bones_read;
    }
}

void GameReader::reader_loop() {
    const float TICK_INTERVAL = 1.0f / 64.0f;
    std::unordered_map<uint32_t, bool> thrower_to_inc; // owner_handle -> is_incendiary

    // Logging variables for read performance
    auto last_local_tick = std::chrono::high_resolution_clock::now();
    auto last_players_tick = std::chrono::high_resolution_clock::now();
    auto last_entities_tick = std::chrono::high_resolution_clock::now();

    int local_ticks = 0, players_ticks = 0, entities_ticks = 0;
    double local_total_time = 0.0, players_total_time = 0.0, entities_total_time = 0.0;
    double local_max_time = 0.0, players_max_time = 0.0, entities_max_time = 0.0;

    auto last_stats_print = std::chrono::steady_clock::now();

    ShmPacket local_packet = {};
    bool last_pin_pulled = false;
    Vec3 last_recorded_velocity = {0.0f, 0.0f, 0.0f};
    float last_recorded_strength = 0.0f;
    uint8_t last_recorded_grenade_type = 0;
    bool has_logged_crosshair_read = false;
    uintptr_t last_logged_crosshair_pointer = 0;
    std::string last_logged_crosshair_code;
    
    // Throw state tracking
    bool player_just_threw = false;
    std::chrono::steady_clock::time_point throw_time;

    struct TrackedThrow {
        uint32_t entity_handle = 0;
        std::string aim_spot_name;
        uint8_t grenade_type = 0;
        Vec3 zone_mins{0.0f};
        Vec3 zone_maxs{0.0f};
        Vec3 predicted_det_pos{0.0f};
        
        Vec3 player_pos{0.0f};
        Vec3 player_angles{0.0f};
        Vec3 player_vel{0.0f};
        float throw_strength = 0.0f;
        
        Vec3 init_pos{0.0f};
        Vec3 init_vel{0.0f};
        float init_speed = 0.0f;
        Vec3 last_known_pos{0.0f};
        
        bool has_detonated = false;
        bool has_smoked = false;
        bool is_tracked = false;
    };
    
    std::vector<TrackedThrow> tracked_throws;
    TrackedThrow pending_tracked_throw;

    while (is_running) {
        // 1. Verify target process connection
        if (target_pid <= 0) {
            if (!has_logged_waiting) {
                BUNNYWARE_LOG_INFO("GameReader: Waiting for cs2 process...");
                has_logged_waiting = true;
            }
            if (!attach_to_process()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
            has_logged_waiting = false;
        }

        // Check if process memory is still readable by reading client base
        uint32_t check_val = 0;
        if (!read_raw(client_base, &check_val, sizeof(check_val))) {
            BUNNYWARE_LOG_WARN("GameReader: cs2 process exited or memory is no longer readable.");
            target_pid = -1;
            model_name_cache.clear();
            thrower_to_inc.clear();
            continue;
        }

        auto now = std::chrono::high_resolution_clock::now();
        bool packet_changed = false;

        // Update stats every 5 seconds
        auto now_steady = std::chrono::steady_clock::now();
        double elapsed_stats = std::chrono::duration<double>(now_steady - last_stats_print).count();
        if (elapsed_stats >= 5.0) {
            float local_fps = local_ticks / (float)elapsed_stats;
            float players_fps = players_ticks / (float)elapsed_stats;
            float entities_fps = entities_ticks / (float)elapsed_stats;

            m_local_tps.store(local_fps);
            m_players_tps.store(players_fps);
            m_entities_tps.store(entities_fps);

            float local_avg = local_ticks > 0 ? (float)(local_total_time / local_ticks) : 0.0f;
            float players_avg = players_ticks > 0 ? (float)(players_total_time / players_ticks) : 0.0f;
            float entities_avg = entities_ticks > 0 ? (float)(entities_total_time / entities_ticks) : 0.0f;

            m_local_avg_time.store(local_avg);
            m_players_avg_time.store(players_avg);
            m_entities_avg_time.store(entities_avg);

            if (atomic_debug_reader.load(std::memory_order_relaxed)) {
                BUNNYWARE_LOG_INFO("[GameReader Loop Stats] "
                             "Local: {:.1f} tps ({:.3f}ms), "
                             "Players: {:.1f} tps ({:.3f}ms), "
                             "Entities: {:.1f} tps ({:.3f}ms)",
                             local_fps, local_avg,
                             players_fps, players_avg,
                             entities_fps, entities_avg);
            }

            local_ticks = 0; local_total_time = 0.0; local_max_time = 0.0;
            players_ticks = 0; players_total_time = 0.0; players_max_time = 0.0;
            entities_ticks = 0; entities_total_time = 0.0; entities_max_time = 0.0;
            last_stats_print = now_steady;
        }

        // Get local player pawn address (needed for other loops)
        uintptr_t local_pawn_addr = 0;
        float raw_local_flash_alpha = 0.0f;  // un-normalized m_flFlashOverlayAlpha, diagnostics only
        if (dwLocalPlayerPawn) {
            local_pawn_addr = read_mem<uintptr_t>(client_base + dwLocalPlayerPawn);
        }
        if (!local_pawn_addr && dwLocalPlayerController) {
            uintptr_t local_controller = read_mem<uintptr_t>(client_base + dwLocalPlayerController);
            if (local_controller) {
                uint32_t pawn_handle = read_mem<uint32_t>(local_controller + m_hPawn);
                if (pawn_handle != 0 && pawn_handle != 0xFFFFFFFF) {
                    local_pawn_addr = get_entity_from_handle(pawn_handle);
                }
            }
        }

        // 1. Local Player Loop
        float local_freq = atomic_local_ms.load(std::memory_order_relaxed);
        double local_elapsed = std::chrono::duration<double, std::milli>(now - last_local_tick).count();
        if (local_elapsed >= local_freq) {
            auto start = std::chrono::high_resolution_clock::now();

            // Keep the last complete matrix available so a torn read in the
            // pre/post read below can fall back to it instead of publishing a
            // half-updated snapshot.
            float previous_view_matrix[16];
            std::memcpy(previous_view_matrix, local_packet.view_matrix, sizeof(previous_view_matrix));
            float sampled_view_matrix[16] = {};
            if (read_raw(client_base + dwViewMatrix, sampled_view_matrix, sizeof(sampled_view_matrix))) {
                std::memcpy(local_packet.view_matrix, sampled_view_matrix, sizeof(local_packet.view_matrix));
            }

            if (engine_base && dwWindowWidth && dwWindowHeight) {
                const int game_window_width =
                    read_mem<int>(engine_base + dwWindowWidth);
                const int game_window_height =
                    read_mem<int>(engine_base + dwWindowHeight);
                if (game_window_width > 0 && game_window_width <= 16384 &&
                    game_window_height > 0 && game_window_height <= 16384) {
                    local_packet.game_window_width = game_window_width;
                    local_packet.game_window_height = game_window_height;
                }
            }

            // Read map directory name + curtime from GlobalVars
            local_packet.curtime = 0.0f;
            uintptr_t global_vars = read_mem<uintptr_t>(client_base + dwGlobalVars);
            if (global_vars) {
                local_packet.curtime = read_mem<float>(global_vars + 0x30);
                uintptr_t map_name_ptr = read_mem<uintptr_t>(global_vars + 0x198);
                if (map_name_ptr) {
                    std::string map_name = read_str(map_name_ptr, 64);
                    if (map_name != last_seen_map_name) {
                        // A new map can recycle model addresses for different models.
                        last_seen_map_name = map_name;
                        clear_hitbox_cache();
                    }
                    std::strncpy(local_packet.map_name, map_name.c_str(), sizeof(local_packet.map_name) - 1);
                }
            }

            if (!local_pawn_addr) {
                // No local pawn (main menu / disconnected): clear the local fields
                // so the web radar does not draw a stale local-player dot.
                local_packet.local_team = 0;
                local_packet.local_health = 0;
                local_packet.local_name[0] = '\0';
            }

            if (local_pawn_addr) {
                Vec3 eye_pos = read_mem<Vec3>(local_pawn_addr + m_vOldOrigin);                Vec3 view_offset = read_mem<Vec3>(local_pawn_addr + m_vecViewOffset);
                local_packet.local_eye = {eye_pos.x + view_offset.x, eye_pos.y + view_offset.y, eye_pos.z + view_offset.z};

                local_packet.local_origin = eye_pos;
                local_packet.local_team = read_mem<uint8_t>(local_pawn_addr + m_iTeamNum);
                local_packet.local_health = read_mem<int>(local_pawn_addr + m_iHealth);
                local_packet.local_armor = read_mem<int>(local_pawn_addr + m_ArmorValue);
                raw_local_flash_alpha = m_flFlashOverlayAlpha
                    ? read_mem<float>(local_pawn_addr + m_flFlashOverlayAlpha)
                    : 0.0f;
                local_packet.local_flash_alpha = normalize_flash_alpha(raw_local_flash_alpha);
                local_packet.local_velocity = read_mem<Vec3>(local_pawn_addr + m_vecVelocity);

                uintptr_t local_controller = read_mem<uintptr_t>(client_base + dwLocalPlayerController);
                if (local_controller) {
                    local_packet.local_has_defuser = read_mem<bool>(local_controller + m_bPawnHasDefuser) ? 1 : 0;
                    {
                        const std::string local_name = read_str(local_controller + m_iszPlayerName, 32);
                        std::strncpy(local_packet.local_name, local_name.c_str(),
                                     sizeof(local_packet.local_name) - 1);
                        local_packet.local_name[sizeof(local_packet.local_name) - 1] = '\0';
                    }
                    local_packet.local_tick_base = read_mem<int>(local_controller + m_nTickBase);
                    local_packet.local_seed_tick = 0;

                    local_packet.local_crosshair_code[0] = '\0';
                    const uintptr_t crosshair_code_address =
                        read_mem<uintptr_t>(local_controller + m_szCrosshairCodes);                    std::string crosshair_code;
                    if (crosshair_code_address) {
                        crosshair_code =
                            read_str(crosshair_code_address,
                                     sizeof(local_packet.local_crosshair_code) - 1);
                        std::strncpy(local_packet.local_crosshair_code,
                                     crosshair_code.c_str(),
                                     sizeof(local_packet.local_crosshair_code) - 1);
                        local_packet.local_crosshair_code[
                            sizeof(local_packet.local_crosshair_code) - 1] = '\0';
                    }

                    const bool debug_crosshair =
                        atomic_debug_reader.load(std::memory_order_relaxed);
                    if (debug_crosshair &&
                        (!has_logged_crosshair_read ||
                         crosshair_code_address != last_logged_crosshair_pointer ||
                         crosshair_code != last_logged_crosshair_code)) {
                        BUNNYWARE_LOG_INFO(
                            "[CrosshairOverlay] memory read controller={:#x} "
                            "schema_offset={:#x} code_pointer={:#x} code='{}' length={}",
                            local_controller, m_szCrosshairCodes,
                            crosshair_code_address, crosshair_code,
                            crosshair_code.size());
                        last_logged_crosshair_pointer = crosshair_code_address;
                        last_logged_crosshair_code = crosshair_code;
                    }
                    has_logged_crosshair_read = debug_crosshair;
                    
                    uintptr_t action_tracking = read_mem<uintptr_t>(local_controller + m_pActionTrackingServices);
                    if (action_tracking) {
                        local_packet.local_total_damage = read_mem<float>(action_tracking + m_flTotalRoundDamageDealt);
                        local_packet.local_total_kills = read_mem<int>(action_tracking + m_iNumRoundKills);
                        local_packet.local_total_kills_headshots = read_mem<int>(action_tracking + m_iNumRoundKillsHeadshots);
                        local_packet.local_kills_valid = 1;
                    } else {
                        local_packet.local_total_damage = 0.0f;
                        local_packet.local_total_kills = 0;
                        local_packet.local_total_kills_headshots = 0;
                        local_packet.local_kills_valid = 0;
                    }
                } else {
                    local_packet.local_has_defuser = 0;
                    local_packet.local_name[0] = '\0';
                    local_packet.local_crosshair_code[0] = '\0';
                    local_packet.local_total_damage = 0.0f;
                    local_packet.local_total_kills = 0;
                    local_packet.local_total_kills_headshots = 0;
                    local_packet.local_kills_valid = 0;
                }

                // Held weapon and grenade parsing
                uintptr_t weapon_services = read_mem<uintptr_t>(local_pawn_addr + m_pWeaponServices);
                bool weapon_resolved = false;
                if (weapon_services) {
                    uint32_t active_weapon_handle = read_mem<uint32_t>(weapon_services + m_hActiveWeapon);
                    uintptr_t active_weapon_addr = get_entity_from_handle(active_weapon_handle);
                    if (active_weapon_addr) {
                        uint16_t item_id = read_mem<uint16_t>(active_weapon_addr + m_AttributeManager + m_Item + m_iItemDefinitionIndex);
                        local_packet.held_weapon_id = item_id;

                        uintptr_t weapon_vdata = read_mem<uintptr_t>(active_weapon_addr + m_nSubclassID + 8);
                        bool loaded_vdata = false;
                        if (weapon_vdata) {
                            local_packet.local_weapon_damage = (float)read_mem<int32_t>(weapon_vdata + m_nDamage);
                            local_packet.local_weapon_range_modifier = read_mem<float>(weapon_vdata + m_flRangeModifier);
                            local_packet.local_weapon_penetration = read_mem<float>(weapon_vdata + m_flPenetration);
                            local_packet.local_weapon_armor_ratio = read_mem<float>(weapon_vdata + m_flArmorRatio);
                            local_packet.local_weapon_headshot_multiplier = read_mem<float>(weapon_vdata + m_flHeadshotMultiplier);
                            if (local_packet.local_weapon_damage > 0.0f && local_packet.local_weapon_range_modifier > 0.0f) {
                                loaded_vdata = true;
                            }
                        }
                        if (!loaded_vdata) {
                            float fallback_dmg = 35.0f, fallback_pen = 1.0f, fallback_armor = 1.0f, fallback_hs = 4.0f, fallback_range = 0.99f;
                            get_weapon_autowall_fallback_stats(item_id, fallback_dmg, fallback_pen, fallback_armor, fallback_hs, fallback_range);
                            local_packet.local_weapon_damage = fallback_dmg;
                            local_packet.local_weapon_range_modifier = fallback_range;
                            local_packet.local_weapon_penetration = fallback_pen;
                            local_packet.local_weapon_armor_ratio = fallback_armor;
                            local_packet.local_weapon_headshot_multiplier = fallback_hs;
                        }
                        weapon_resolved = true;

                        uint8_t grenade_type = 0;
                        if (item_id == 43) grenade_type = 2;      // Flashbang
                        else if (item_id == 44) grenade_type = 1; // HE
                        else if (item_id == 45) grenade_type = 3; // Smoke
                        else if (item_id == 46) grenade_type = 4; // Molotov
                        else if (item_id == 47) grenade_type = 5; // Decoy
                        else if (item_id == 48) grenade_type = 4; // Incendiary

                        if (grenade_type > 0) {
                            bool pin_pulled = read_mem<bool>(active_weapon_addr + m_bPinPulled);
                            if (pin_pulled) {
                                local_packet.held_grenade_type = grenade_type;
                                local_packet.pin_pulled = 1;
                                local_packet.throw_strength = read_mem<float>(active_weapon_addr + m_flThrowStrength);
                                local_packet.local_velocity = read_mem<Vec3>(local_pawn_addr + m_vecVelocity);
                                last_recorded_velocity = local_packet.local_velocity;
                                last_recorded_strength = local_packet.throw_strength;
                                last_recorded_grenade_type = grenade_type;
                                // angles are now read universally every frame
                            } else {
                                local_packet.held_grenade_type = GRENADE_NONE;
                                local_packet.pin_pulled = 0;
                            }
                            
                            if (last_pin_pulled && !pin_pulled) {
                                pending_tracked_throw = {};
                                pending_tracked_throw.player_pos = local_packet.local_origin;
                                pending_tracked_throw.player_angles = local_packet.local_angles;
                                pending_tracked_throw.player_vel = last_recorded_velocity;
                                pending_tracked_throw.throw_strength = last_recorded_strength;
                                pending_tracked_throw.grenade_type = last_recorded_grenade_type;
                                pending_tracked_throw.is_tracked = true;

                                float p_pitch = local_packet.local_angles.x;
                                float p_yaw = local_packet.local_angles.y;

                                {
                                    std::lock_guard<std::mutex> lock(aim_spots_mutex_);
                                    bool matched = false;
                                    for (const auto& spot : active_aim_spots_) {
                                        float pitch_diff = std::abs(p_pitch - spot.pitch);
                                        float yaw_diff = std::abs(p_yaw - spot.yaw);
                                        if (yaw_diff > 180.0f) yaw_diff = 360.0f - yaw_diff;

                                        float fov_dist = std::sqrt(pitch_diff*pitch_diff + yaw_diff*yaw_diff);
                                        if (fov_dist < 5.0f) {
                                            pending_tracked_throw.aim_spot_name = spot.zone_name;
                                            pending_tracked_throw.zone_mins = spot.zone_mins;
                                            pending_tracked_throw.zone_maxs = spot.zone_maxs;
                                            pending_tracked_throw.predicted_det_pos = {spot.predicted_det_pos.x, spot.predicted_det_pos.y, spot.predicted_det_pos.z};
                                            matched = true;
                                            break;
                                        }
                                    }

                                    if (!matched) {
                                        pending_tracked_throw.aim_spot_name = "untracked";
                                        pending_tracked_throw.zone_mins = {0.0f, 0.0f, 0.0f};
                                        pending_tracked_throw.zone_maxs = {0.0f, 0.0f, 0.0f};
                                        pending_tracked_throw.predicted_det_pos = {0.0f, 0.0f, 0.0f};
                                    }
                                }

                                player_just_threw = true;
                                throw_time = std::chrono::steady_clock::now();
                            }
                            last_pin_pulled = pin_pulled;
                        } else {
                            local_packet.held_grenade_type = GRENADE_NONE;
                            local_packet.pin_pulled = 0;
                            last_pin_pulled = false;
                        }
                    }
                }
                if (!weapon_resolved) {
                    local_packet.held_weapon_id = 0;
                    local_packet.local_weapon_damage = 0.0f;
                    local_packet.local_weapon_range_modifier = 0.0f;
                    local_packet.local_weapon_penetration = 0.0f;
                    local_packet.local_weapon_armor_ratio = 0.0f;
                    local_packet.local_weapon_headshot_multiplier = 0.0f;
                }

                // --- Read Triggerbot specific local player info ---
                // m_aimPunchAngle was removed from C_CSPlayerPawn. The current view
                // punch lives on the AimPunchServices object; skip it while the
                // service cache is empty/full (length 0 or 129) to avoid bogus vectors.
                local_packet.local_aim_punch = {0.0f, 0.0f, 0.0f};
                if (m_pAimPunchServices && m_predictableBaseAngle) {
                    const uintptr_t aim_punch_services =
                        read_mem<uintptr_t>(local_pawn_addr + m_pAimPunchServices);
                    if (aim_punch_services) {
                        const int32_t cache_length = read_mem<int32_t>(aim_punch_services + 0x88);
                        if (cache_length != 0 && cache_length != 129) {
                            local_packet.local_aim_punch =
                                read_mem<Vec3>(aim_punch_services + m_predictableBaseAngle);
                        }
                    }
                }
                local_packet.local_shots_fired =
                    m_iShotsFired ? read_mem<int>(local_pawn_addr + m_iShotsFired) : 0;

                local_packet.local_is_scoped = read_mem<bool>(local_pawn_addr + m_bIsScoped) ? 1 : 0;
                local_packet.local_flags = read_mem<uint32_t>(local_pawn_addr + m_fFlags);
                 
                float sensitivity_val = 1.25f;
                if (dwSensitivity && dwSensitivity_sensitivity) {
                    uintptr_t sens_ptr = read_mem<uintptr_t>(client_base + dwSensitivity);
                    if (sens_ptr) {
                        sensitivity_val = read_mem<float>(sens_ptr + dwSensitivity_sensitivity);
                    }
                }
                local_packet.local_sensitivity = sensitivity_val;
                local_packet.local_fov_sensitivity_adjust = read_mem<float>(local_pawn_addr + m_flFOVSensitivityAdjust);

                int raw_crosshair_id = read_mem<int>(local_pawn_addr + m_iIDEntIndex);
                local_packet.local_in_crosshair_id = (raw_crosshair_id > 0) ? (raw_crosshair_id & 0x7FFF) : -1;

                // Read local player view angles
                if (dwCSGOInput) {
                    uintptr_t csgo_input = read_mem<uintptr_t>(client_base + dwCSGOInput);
                    if (csgo_input) {
                        uintptr_t view_angles_offset = dwViewAngles - dwCSGOInput;
                        local_packet.local_angles = read_mem<Vec3>(csgo_input + view_angles_offset);
                    }
                } else {
                    local_packet.local_angles = read_mem<Vec3>(client_base + dwViewAngles);
                }

                // --- Calculate local weapon spread ---
                local_packet.local_spread = 0.0f;
                local_packet.local_base_spread = 0.0f;
                local_packet.local_recoil_index = 0.0f;
                local_packet.local_num_bullets = 1;
                local_packet.local_spread_seed = 0;
                local_packet.local_recoil_seed = 0;
                local_packet.target_hitboxes.count = 0;
                local_packet.local_seed_tick = 0;
                local_packet.local_weapon_accuracy_nospread = 0.0f;
                local_packet.local_weapon_accuracy_forcespread = 0.0f;
                local_packet.local_weapon_air_spread_scale = 1.0f;
                local_packet.local_sv_jump_impulse = 301.993377f;

                for (int i = 0; i < static_cast<int>(shm::MAX_PLAYERS); ++i) {
                    local_packet.player_hitboxes[i].count = 0;
                }

                // Match the reference seed-trigger tick selection.  A local
                // hosted session uses GlobalVars::tickcount, while an online
                // client predicts the next command from the pawn simulation
                // tick.  m_nTickBase + 1 is only the final fallback.
                int global_tick = global_vars ? read_mem<int>(global_vars + 0x44) : -1;
                int simulation_tick = m_nSimulationTick
                    ? read_mem<int>(local_pawn_addr + m_nSimulationTick) : -1;
                int ping = m_iPing && local_controller
                    ? read_mem<int>(local_controller + m_iPing) : 0;
                if (ping <= 0 && global_tick > 0) {
                    local_packet.local_seed_tick = global_tick;
                } else if (simulation_tick >= 0) {
                    local_packet.local_seed_tick = simulation_tick + 1;
                } else if (local_packet.local_tick_base >= 0) {
                    local_packet.local_seed_tick = local_packet.local_tick_base + 1;
                }

                if (local_packet.local_in_crosshair_id > 0) {
                    uintptr_t target_pawn_addr = get_entity(local_packet.local_in_crosshair_id);
                    if (target_pawn_addr) {
                        local_packet.target_hitboxes = query_hitboxes(target_pawn_addr);
                    }
                }

                if (weapon_services) {
                    uint32_t active_weapon_handle = read_mem<uint32_t>(weapon_services + m_hActiveWeapon);
                    uintptr_t active_weapon_addr = get_entity_from_handle(active_weapon_handle);
                    if (active_weapon_addr) {
                        uintptr_t weapon_vdata = read_mem<uintptr_t>(active_weapon_addr + m_nSubclassID + 8);
                        if (weapon_vdata) {
                            int w_mode = read_mem<int>(active_weapon_addr + m_weaponMode);
                            uint32_t w_offset = (w_mode == 1) ? 0x4 : 0x0;

                            float spread_value = read_mem<float>(weapon_vdata + m_flSpread + w_offset);
                            float accuracy = read_mem<float>(active_weapon_addr + m_fAccuracyPenalty);
                            float max_speed = read_mem<float>(weapon_vdata + m_flMaxSpeed + w_offset);

                            Vec3 velocity = read_mem<Vec3>(local_pawn_addr + m_vecVelocity);
                            float horizontal_speed = std::sqrt(velocity.x * velocity.x + velocity.y * velocity.y);
                            float max_duck_speed = max_speed * 0.34f;
                            float movement_inaccuracy_scale = 0.0f;
                            if (max_speed > 0.0f && ((max_speed * 0.95f) - max_duck_speed) != 0.0f) {
                                movement_inaccuracy_scale = (horizontal_speed - max_duck_speed) / ((max_speed * 0.95f) - max_duck_speed);
                            }

                            if (movement_inaccuracy_scale > 0.0f) {
                                if (movement_inaccuracy_scale > 1.0f) movement_inaccuracy_scale = 1.0f;
                                bool is_walking = read_mem<bool>(local_pawn_addr + m_bIsWalking);
                                if (!is_walking) {
                                    movement_inaccuracy_scale = std::pow(movement_inaccuracy_scale, 0.25f);
                                }
                                float inaccuracy_move = read_mem<float>(weapon_vdata + m_flInaccuracyMove + w_offset);
                                accuracy += movement_inaccuracy_scale * inaccuracy_move;
                            }

                            bool on_ground = (local_packet.local_flags & (1 << 0)) != 0;
                            uint8_t move_type = read_mem<uint8_t>(local_pawn_addr + m_MoveType);
                            bool on_ladder = (move_type == 9);

                            if (!on_ground || on_ladder) {
                                float inaccuracy_jump_initial = read_mem<float>(weapon_vdata + m_flInaccuracyJumpInitial);
                                float sqrt_max_jump_speed = 17.377956669f;
                                float sqrt_vertical_speed = std::sqrt(std::abs(velocity.z));
                                float min_accuracy_penalty_speed = sqrt_max_jump_speed * 0.25f;
                                float air_speed_inaccuracy = 0.0f;
                                if ((sqrt_max_jump_speed - min_accuracy_penalty_speed) != 0.0f) {
                                    air_speed_inaccuracy = inaccuracy_jump_initial * (sqrt_vertical_speed - min_accuracy_penalty_speed) / (sqrt_max_jump_speed - min_accuracy_penalty_speed);
                                }

                                if (air_speed_inaccuracy < 0.0f) air_speed_inaccuracy = 0.0f;
                                if (air_speed_inaccuracy > 2.0f * inaccuracy_jump_initial) air_speed_inaccuracy = 2.0f * inaccuracy_jump_initial;

                                accuracy += air_speed_inaccuracy;
                            }

                            if (accuracy > 1.0f) accuracy = 1.0f;

                            // Apply weapon accuracy convars (SeedTrigger-style)
                            float acc_nospread = local_packet.local_weapon_accuracy_nospread;
                            float acc_forcespread = local_packet.local_weapon_accuracy_forcespread;

                            if (acc_nospread > 0.0f) {
                                local_packet.local_base_spread = 0.0f;
                            }
                            if (acc_forcespread > 0.0f) {
                                spread_value = acc_forcespread;
                            }

                            local_packet.local_spread = spread_value + accuracy;
                            local_packet.local_base_spread = (acc_nospread > 0.0f) ? 0.0f : spread_value;
                            local_packet.local_recoil_index = read_mem<float>(active_weapon_addr + m_flRecoilIndex);
                            local_packet.local_num_bullets = read_mem<int>(weapon_vdata + m_nNumBullets);
                            local_packet.local_spread_seed = read_mem<int>(weapon_vdata + m_nSpreadSeed);
                            local_packet.local_recoil_seed = read_mem<int>(weapon_vdata + m_nRecoilSeed);
                        }
                    }
                }
                // --- View matrix for this snapshot ---
                // Two reads straddling the packet assembly are reduced to a
                // temporal midpoint so the packet matrix is representative of
                // the instant this snapshot describes; a torn read keeps the
                // previous complete matrix instead.
                float pre_view_matrix[16] = {};
                float post_view_matrix[16] = {};

                struct iovec local_iov[2];
                struct iovec remote_iov[2];

                local_iov[0].iov_base = pre_view_matrix;
                local_iov[0].iov_len = sizeof(pre_view_matrix);
                remote_iov[0].iov_base = reinterpret_cast<void*>(client_base + dwViewMatrix);
                remote_iov[0].iov_len = sizeof(pre_view_matrix);

                local_iov[1].iov_base = post_view_matrix;
                local_iov[1].iov_len = sizeof(post_view_matrix);
                remote_iov[1].iov_base = reinterpret_cast<void*>(client_base + dwViewMatrix);
                remote_iov[1].iov_len = sizeof(post_view_matrix);

                const ssize_t bytes_read = process_vm_readv(target_pid, local_iov, 2, remote_iov, 2, 0);
                const ssize_t expected_bytes =
                    static_cast<ssize_t>(sizeof(pre_view_matrix) + sizeof(post_view_matrix));

                if (bytes_read == expected_bytes) {
                    DecomposedVP pre_dec = decompose_vp(pre_view_matrix);
                    DecomposedVP post_dec = decompose_vp(post_view_matrix);
                    DecomposedVP midpoint_dec = pre_dec;
                    midpoint_dec.pos = glm::mix(pre_dec.pos, post_dec.pos, 0.5f);
                    midpoint_dec.rot = glm::normalize(glm::slerp(pre_dec.rot, post_dec.rot, 0.5f));
                    midpoint_dec.g_x = std::lerp(pre_dec.g_x, post_dec.g_x, 0.5f);
                    midpoint_dec.g_y = std::lerp(pre_dec.g_y, post_dec.g_y, 0.5f);
                    midpoint_dec.A = std::lerp(pre_dec.A, post_dec.A, 0.5f);
                    midpoint_dec.B = std::lerp(pre_dec.B, post_dec.B, 0.5f);
                    reconstruct_vp(midpoint_dec, local_packet.view_matrix);
                } else {
                    // On a partial read, keep the last complete snapshot.
                    std::memcpy(local_packet.view_matrix, previous_view_matrix, sizeof(local_packet.view_matrix));
                    if (is_w_pressed_x11()) {
                        BUNNYWARE_LOG_INFO("[VM_TORN] View matrix read failed: bytes_read={}, expected={}",
                                         bytes_read, expected_bytes);
                    }
                }
            } else {
                local_packet.local_origin = {};
                local_packet.local_team = 0;
                local_packet.local_health = 0;
                local_packet.local_armor = 0;
                local_packet.local_flash_alpha = 0.0f;
                raw_local_flash_alpha = 0.0f;
                local_packet.local_has_defuser = 0;
                local_packet.local_total_damage = 0.0f;
                local_packet.local_total_kills = 0;
                local_packet.local_total_kills_headshots = 0;
                local_packet.local_kills_valid = 0;
                local_packet.local_aim_punch = {0.0f, 0.0f, 0.0f};
                local_packet.local_shots_fired = 0;
                local_packet.local_crosshair_code[0] = '\0';
                local_packet.local_spread = 0.0f;
                local_packet.local_in_crosshair_id = -1;
                local_packet.local_flags = 0;
                local_packet.local_is_scoped = 0;
                local_packet.local_sensitivity = 1.25f;
                local_packet.local_fov_sensitivity_adjust = 1.0f;
            }

            // Anti Flash diagnostic: mirror the overlay's engage/release latch so
            // a log reader can tell a reader-side miss apart from an overlay-side
            // render miss. The raw value is printed too, so a future domain change
            // (normalized 0..1 vs 0..255) is immediately visible.
            if (cfg && cfg->anti_flash) {
                static bool local_flash_latched = false;
                static bool local_flash_latched_last = false;
                if (local_packet.local_flash_alpha >= cfg->anti_flash_alpha_start) {
                    local_flash_latched = true;
                } else if (local_packet.local_flash_alpha <= cfg->anti_flash_alpha_end) {
                    local_flash_latched = false;
                }
                if (local_flash_latched != local_flash_latched_last) {
                    local_flash_latched_last = local_flash_latched;
                    BUNNYWARE_LOG_INFO(
                        "GameReader: local flash raw {:.4f} -> {:.1f}/255 ({}, engage >= {:.1f}, release <= {:.1f}); "
                        "offset {:#x}, pawn {:#x}",
                        raw_local_flash_alpha, local_packet.local_flash_alpha,
                        local_flash_latched ? "ENGAGED" : "RELEASED", cfg->anti_flash_alpha_start,
                        cfg->anti_flash_alpha_end, m_flFlashOverlayAlpha, local_pawn_addr);
                }
            }

            // Bullet Impacts: native client impacts of the alive local pawn. Independent of
            // view/recoil/spread estimates; any failed or racing read publishes nothing.
            pending_impacts.reset();
            if (atomic_client_impacts.load(std::memory_order_relaxed) && local_pawn_addr &&
                local_packet.local_health > 0) {
                const uintptr_t base = client_base.load();
                if (impact_layout_client_base != base) {
                    impact_layout_client_base = base;
                    impact_layout = impacts::layout_from_offsets(base, m_pBulletServices, dwBulletServicesVTable,
                                                                 dwBulletImpactGlobals, dwBulletServices_impacts);
                    if (impact_layout)
                        BUNNYWARE_LOG_INFO("GameReader: Bullet Impacts native client-impact layout resolved");
                    else
                        BUNNYWARE_LOG_WARN("GameReader: Bullet Impacts native client impacts unavailable for this game build");
                }
                if (impact_layout) {
                    const impacts::Subject subject{target_pid.load(), base, local_pawn_addr};
                    auto impact_read = [&](uintptr_t address, void* dest, size_t size) {
                        return read_raw(address, dest, size);
                    };
                    auto impact_clock = [] {
                        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
                    };
                    auto verify_impact_source = [&] {
                        return target_pid.load() == subject.pid && client_base.load() == subject.client_base &&
                            (!dwLocalPlayerPawn || read_mem<uintptr_t>(base + dwLocalPlayerPawn) == subject.pawn);
                    };
                    pending_impacts = impacts::read_snapshot(subject, *impact_layout, impact_read, impact_clock,
                                                             verify_impact_source);
                }
            }

            auto end = std::chrono::high_resolution_clock::now();
            double elapsed = std::chrono::duration<double, std::milli>(end - start).count();
            local_total_time += elapsed;
            local_max_time = std::max(local_max_time, elapsed);
            local_ticks++;
            last_local_tick = now;
            packet_changed = true;
        }

        // 2. Other Players Loop
        float players_freq = atomic_players_ms.load(std::memory_order_relaxed);
        double players_elapsed = std::chrono::duration<double, std::milli>(now - last_players_tick).count();
        if (players_elapsed >= players_freq) {
            auto start = std::chrono::high_resolution_clock::now();


            int player_count = 0;
            local_packet.spectator_count = 0;
            std::vector<uintptr_t> active_pawns;
            active_pawns.reserve(shm::MAX_PLAYERS);
            uintptr_t entity_list_base = read_mem<uintptr_t>(client_base + dwEntityList);
            if (entity_list_base) {
                // Determine the pawn being viewed by the local player (themselves if alive, or their spectate target if dead)
                uintptr_t local_view_pawn = 0;
                uintptr_t local_controller = read_mem<uintptr_t>(client_base + dwLocalPlayerController);
                if (local_controller) {
                    bool local_alive = read_mem<bool>(local_controller + m_bPawnIsAlive);
                    if (local_alive) {
                        uint32_t local_pawn_handle = read_mem<uint32_t>(local_controller + m_hPlayerPawn);
                        if (local_pawn_handle == 0 || local_pawn_handle == 0xFFFFFFFF) {
                            local_pawn_handle = read_mem<uint32_t>(local_controller + m_hPawn);
                        }
                        if (local_pawn_handle != 0 && local_pawn_handle != 0xFFFFFFFF) {
                            local_view_pawn = get_entity_from_handle_cached(entity_list_base, local_pawn_handle);
                        }
                    } else {
                        uint32_t local_obs_handle = read_mem<uint32_t>(local_controller + m_hObserverPawn);
                        if (local_obs_handle != 0 && local_obs_handle != 0xFFFFFFFF) {
                            uintptr_t local_obs_pawn = get_entity_from_handle_cached(entity_list_base, local_obs_handle);
                            if (local_obs_pawn) {
                                uintptr_t observer_services = read_mem<uintptr_t>(local_obs_pawn + m_pObserverServices);
                                if (observer_services) {
                                    uint32_t observer_target_handle = read_mem<uint32_t>(observer_services + m_hObserverTarget);
                                    if (observer_target_handle != 0 && observer_target_handle != 0xFFFFFFFF) {
                                        local_view_pawn = get_entity_from_handle_cached(entity_list_base, observer_target_handle);
                                    }
                                }
                            }
                        }
                    }
                }

                if (!local_view_pawn && local_pawn_addr) {
                    uintptr_t observer_services = read_mem<uintptr_t>(local_pawn_addr + m_pObserverServices);
                    if (observer_services) {
                        uint32_t observer_target_handle = read_mem<uint32_t>(observer_services + m_hObserverTarget);
                        if (observer_target_handle != 0 && observer_target_handle != 0xFFFFFFFF) {
                            local_view_pawn = get_entity_from_handle_cached(entity_list_base, observer_target_handle);
                        }
                    }
                }

                if (!local_view_pawn) {
                    local_view_pawn = local_pawn_addr;
                }

                static auto last_diagnostics_print = std::chrono::steady_clock::now();
                auto now_diagnostics = std::chrono::steady_clock::now();
                double elapsed_diagnostics = std::chrono::duration<double>(now_diagnostics - last_diagnostics_print).count();
                bool should_print_diagnostics = false;
                if (atomic_debug_reader.load(std::memory_order_relaxed) && elapsed_diagnostics >= 5.0) {
                    should_print_diagnostics = true;
                    last_diagnostics_print = now_diagnostics;
                }

                if (should_print_diagnostics) {
                    BUNNYWARE_LOG_INFO("GameReader Diagnostics: local_pawn_addr: {:#x}, local_view_pawn: {:#x}", local_pawn_addr, local_view_pawn);
                }

                // Entity indices 1..64 share a single entity-list chunk, so the chunk
                // pointer is memoised instead of being re-read 64 times per iteration.
                uintptr_t chunk = 0;
                int chunk_index_cached = -1;
                for (int i = 1; i <= 64; ++i) {
                    const int chunk_index = (i & 0x7FFF) >> 9;
                    if (chunk_index != chunk_index_cached) {
                        chunk = read_mem<uintptr_t>(entity_list_base + 8 * chunk_index);
                        chunk_index_cached = chunk_index;
                    }
                    if (!chunk) continue;
                    uintptr_t identity = chunk + 112 * (i & 0x1FF);
                    uintptr_t controller_addr = read_mem<uintptr_t>(identity);
                    if (!controller_addr) continue;

                    // Spectator scan
                    if (controller_addr != local_controller) {
                        // Check if they are spectating (try observer pawn, then player pawn, then base pawn handle)
                        uint32_t scan_pawn_handle = read_mem<uint32_t>(controller_addr + m_hObserverPawn);
                        if (scan_pawn_handle == 0 || scan_pawn_handle == 0xFFFFFFFF) {
                            scan_pawn_handle = read_mem<uint32_t>(controller_addr + m_hPlayerPawn);
                        }
                        if (scan_pawn_handle == 0 || scan_pawn_handle == 0xFFFFFFFF) {
                            scan_pawn_handle = read_mem<uint32_t>(controller_addr + m_hPawn);
                        }

                        if (scan_pawn_handle != 0 && scan_pawn_handle != 0xFFFFFFFF) {
                            uintptr_t scan_pawn_addr = get_entity_from_handle_cached(entity_list_base, scan_pawn_handle);
                            if (scan_pawn_addr) {
                                uintptr_t observer_services = read_mem<uintptr_t>(scan_pawn_addr + m_pObserverServices);
                                if (should_print_diagnostics) {
                                    std::string sname = read_str(controller_addr + m_iszPlayerName, 32);
                                    BUNNYWARE_LOG_INFO("  Player '{}': pawn_handle: {:#x}, pawn_addr: {:#x}, obs_services: {:#x}", 
                                                     sname, scan_pawn_handle, scan_pawn_addr, observer_services);
                                }
                                if (observer_services) {
                                    uint32_t observer_target_handle = read_mem<uint32_t>(observer_services + m_hObserverTarget);
                                    if (observer_target_handle != 0 && observer_target_handle != 0xFFFFFFFF) {
                                        uintptr_t observer_target_pawn = get_entity_from_handle_cached(entity_list_base, observer_target_handle);
                                        uint8_t mode = read_mem<uint8_t>(observer_services + m_iObserverMode);
                                        if (should_print_diagnostics) {
                                            BUNNYWARE_LOG_INFO("    -> target_handle: {:#x}, target_pawn: {:#x}, mode: {}", 
                                                             observer_target_handle, observer_target_pawn, (int)mode);
                                        }
                                        if (observer_target_pawn && observer_target_pawn == local_view_pawn) {
                                            if (mode == 2 || mode == 5 || mode == 6) {
                                                if (local_packet.spectator_count < 64) {
                                                    SpectatorData& spec = local_packet.spectators[local_packet.spectator_count];
                                                    std::string sname = read_str(controller_addr + m_iszPlayerName, 32);
                                                    std::strncpy(spec.name, sname.c_str(), sizeof(spec.name) - 1);
                                                    spec.name[sizeof(spec.name) - 1] = '\0';
                                                    spec.is_first_person = (mode == 5) ? 1 : 0;
                                                    local_packet.spectator_count++;
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }

                    uint32_t pawn_handle = read_mem<uint32_t>(controller_addr + m_hPawn);
                    if (pawn_handle == 0 || pawn_handle == 0xFFFFFFFF) continue;

                    uintptr_t entity_addr = get_entity_from_handle_cached(entity_list_base, pawn_handle);
                    if (!entity_addr) continue;

                    int health = read_mem<int>(entity_addr + m_iHealth);
                    uint8_t life_state = read_mem<uint8_t>(entity_addr + m_lifeState);
                    int team = read_mem<uint8_t>(entity_addr + m_iTeamNum);

                    if (life_state == 0 && health > 0) {
                        if (entity_addr != local_pawn_addr && player_count < 64) {
                            PlayerData& player = local_packet.players[player_count];
                            player.team = team;
                            player.health = health;
                            player.active = 1;
                            player.entity_index = pawn_handle & 0x7FFF;

                            // Defuser check
                            uintptr_t item_services = read_mem<uintptr_t>(entity_addr + m_pItemServices);
                            player.has_defuser = read_mem<bool>(item_services + m_bHasDefuser) ? 1 : 0;

                            // Origin coordinates
                            player.origin = read_mem<Vec3>(entity_addr + m_vOldOrigin);

                            // Resolve name from controller
                            std::string pname = read_str(controller_addr + m_iszPlayerName, 32);
                            std::strncpy(player.name, pname.c_str(), sizeof(player.name) - 1);
                            player.name[sizeof(player.name) - 1] = '\0';

                            // Read armor & helmet
                            player.armor = read_mem<int>(entity_addr + m_ArmorValue);
                            player.has_helmet = read_mem<bool>(entity_addr + m_bPrevHelmet) ? 1 : 0;
                            player.is_scoped = read_mem<bool>(entity_addr + m_bIsScoped) ? 1 : 0;
                            player.is_walking = (m_bIsWalking && read_mem<bool>(entity_addr + m_bIsWalking)) ? 1 : 0;
                            player.emit_sound_time = m_flEmitSoundTime
                                ? read_mem<float>(entity_addr + m_flEmitSoundTime)
                                : 0.0f;
                            player.flash_alpha = m_flFlashOverlayAlpha
                                ? normalize_flash_alpha(read_mem<float>(entity_addr + m_flFlashOverlayAlpha))
                                : 0.0f;
                            player.has_bomb = 0;
                            player.velocity = m_vecAbsVelocity
                                ? read_mem<Vec3>(entity_addr + m_vecAbsVelocity)
                                : (m_vecVelocity ? read_mem<Vec3>(entity_addr + m_vecVelocity) : Vec3{});
                            player.flags = m_fFlags ? read_mem<uint32_t>(entity_addr + m_fFlags) : 0;
                            player.shots_fired = m_iShotsFired ? read_mem<int>(entity_addr + m_iShotsFired) : 0;
                            player.eye_angles = m_angEyeAngles ? read_mem<Vec3>(entity_addr + m_angEyeAngles) : Vec3{};

                            // Active weapon & reloading check
                            std::memset(player.active_weapon_name, 0, sizeof(player.active_weapon_name));
                            player.is_reloading = 0;
                            player.ammo = 0;
                            player.max_ammo = 0;
                            uintptr_t weapon_services = read_mem<uintptr_t>(entity_addr + m_pWeaponServices);
                            if (weapon_services) {
                                uint32_t active_weapon_handle = read_mem<uint32_t>(weapon_services + m_hActiveWeapon);
                                uintptr_t active_weapon_addr = get_entity_from_handle_cached(entity_list_base, active_weapon_handle);
                                if (active_weapon_addr) {
                                    player.is_reloading = read_mem<bool>(active_weapon_addr + m_bInReload) ? 1 : 0;
                                    player.ammo = read_mem<int>(active_weapon_addr + m_iClip1);
                                    uintptr_t weapon_vdata = read_mem<uintptr_t>(active_weapon_addr + m_nSubclassID + 8);
                                    if (weapon_vdata) {
                                        player.max_ammo = read_mem<int>(weapon_vdata + m_iMaxClip1);
                                        uintptr_t name_ptr = read_mem<uintptr_t>(weapon_vdata + m_szName);
                                        if (name_ptr) {
                                            std::string wname = read_str(name_ptr, 64);
                                            if (wname.rfind("weapon_", 0) == 0) {
                                                wname = wname.substr(7);
                                            }
                                            std::strncpy(player.active_weapon_name, wname.c_str(), sizeof(player.active_weapon_name) - 1);
                                        }
                                    }
                                }

                                // Bomb carrier check: scan m_hMyWeapons inventory for weapon_c4
                                // C_NetworkUtlVectorBase layout: int32 count @ +0x0, T* elements @ +0x8 (CHandle = uint32)
                                if (m_hMyWeapons) {
                                    int weapon_count = read_mem<int>(weapon_services + m_hMyWeapons);
                                    uintptr_t weapon_list = read_mem<uintptr_t>(weapon_services + m_hMyWeapons + 0x8);
                                    if (weapon_list && weapon_count > 0 && weapon_count <= 64) {
                                        for (int wi = 0; wi < weapon_count; ++wi) {
                                            uint32_t inv_weapon_handle = read_mem<uint32_t>(weapon_list + wi * sizeof(uint32_t));
                                            if (inv_weapon_handle == 0 || inv_weapon_handle == 0xFFFFFFFF) continue;
                                            uintptr_t inv_weapon_addr = get_entity_from_handle_cached(entity_list_base, inv_weapon_handle);
                                            if (!inv_weapon_addr) continue;
                                            uint16_t def_index = read_mem<uint16_t>(inv_weapon_addr + m_AttributeManager + m_Item + m_iItemDefinitionIndex);
                                            if (def_index == weapon_c4) {
                                                player.has_bomb = 1;
                                                break;
                                            }
                                        }
                                    }
                                }
                            }

                            // Resolve model name
                            uintptr_t scene_node = read_mem<uintptr_t>(entity_addr + m_pGameSceneNode);
                            std::string model = resolve_model_name(entity_addr, scene_node);
                            std::strncpy(player.model_name, model.c_str(), sizeof(player.model_name) - 1);

                            // Update skeletal bones
                            update_bone_cache(player, scene_node);
                            active_pawns.push_back(entity_addr);
                            local_packet.player_hitboxes[player_count] = query_hitboxes(entity_addr);
                            player_count++;
                        }
                    }
                }
            }
            static auto last_spectator_print = std::chrono::steady_clock::now();
            auto now_spectator = std::chrono::steady_clock::now();
            double elapsed_spectator = std::chrono::duration<double>(now_spectator - last_spectator_print).count();
            if (local_packet.spectator_count > 0 && atomic_debug_reader.load(std::memory_order_relaxed) && elapsed_spectator >= 5.0) {
                last_spectator_print = now_spectator;
                BUNNYWARE_LOG_INFO("GameReader: Found {} spectators!", local_packet.spectator_count);
                for (int s = 0; s < local_packet.spectator_count; ++s) {
                    BUNNYWARE_LOG_INFO("  Spectator {}: {} (1st person: {})", s, local_packet.spectators[s].name, local_packet.spectators[s].is_first_person);
                }
            }

            local_packet.player_count = player_count;

            // Clean model name cache for stale players
            for (auto it = model_name_cache.begin(); it != model_name_cache.end();) {
                if (std::find(active_pawns.begin(), active_pawns.end(), it->first) == active_pawns.end()) {
                    it = model_name_cache.erase(it);
                } else {
                    ++it;
                }
            }

            auto end = std::chrono::high_resolution_clock::now();
            double elapsed = std::chrono::duration<double, std::milli>(end - start).count();
            players_total_time += elapsed;
            players_max_time = std::max(players_max_time, elapsed);
            players_ticks++;
            last_players_tick = now;
            packet_changed = true;
        }

        // 3. Other Entities Loop
        float entities_freq = atomic_entities_ms.load(std::memory_order_relaxed);
        double entities_elapsed = std::chrono::duration<double, std::milli>(now - last_entities_tick).count();
        if (entities_elapsed >= entities_freq) {
            auto start = std::chrono::high_resolution_clock::now();

            int projectile_count = 0;
            int inferno_count = 0;
            int door_count = 0;
            int dropped_item_count = 0;
            int smoke_count = 0;
            uintptr_t planted_c4_entity_addr = 0;
            local_packet.c4_arming = {};
            // Round / match state (best-effort; skipped when unavailable).
            local_packet.game_rules = {};
            if (dwGameRules) {
                const uintptr_t game_rules = read_mem<uintptr_t>(client_base + dwGameRules);
                if (game_rules) {
                    local_packet.game_rules.active = 1;
                    local_packet.game_rules.is_freeze_period =
                        read_mem<bool>(game_rules + m_bFreezePeriod) ? 1 : 0;
                    local_packet.game_rules.is_warmup = read_mem<bool>(game_rules + m_bWarmupPeriod) ? 1 : 0;
                    local_packet.game_rules.round_time = read_mem<int>(game_rules + m_iRoundTime);
                    local_packet.game_rules.total_rounds_played =
                        read_mem<int>(game_rules + m_totalRoundsPlayed);
                    local_packet.game_rules.game_phase = read_mem<int>(game_rules + m_gamePhase);
                }
            }

            uintptr_t entity_list_base = read_mem<uintptr_t>(client_base + dwEntityList);
            if (entity_list_base) {
                // Team scores live on CCSTeam entities in the low entity indices.
                if (m_iTeamScore) {
                    uintptr_t team_chunk = 0;
                    int team_chunk_idx = -1;
                    for (int ti = 1; ti <= 64; ++ti) {
                        const int ci = (ti & 0x7FFF) >> 9;
                        if (ci != team_chunk_idx) {
                            team_chunk = read_mem<uintptr_t>(entity_list_base + 8 * ci);
                            team_chunk_idx = ci;
                        }
                        if (!team_chunk) continue;
                        const uintptr_t identity = team_chunk + 112 * (ti & 0x1FF);
                        const uintptr_t team_ent = read_mem<uintptr_t>(identity);
                        if (!team_ent) continue;
                        const uintptr_t designer_name_ptr = read_mem<uintptr_t>(identity + 0x20);
                        const std::string cname = read_str(designer_name_ptr, 64);
                        if (cname.find("team") == std::string::npos &&
                            cname.find("Team") == std::string::npos) {
                            continue;
                        }
                        const int team = read_mem<uint8_t>(team_ent + m_iTeamNum);
                        const int score = read_mem<int>(team_ent + m_iTeamScore);
                        if (team == 2) local_packet.game_rules.t_score = score;
                        else if (team == 3) local_packet.game_rules.ct_score = score;
                    }
                }
                uintptr_t active_chunk = 0;
                int last_chunk_idx = -1;
                alignas(16) uint8_t chunk_buffer[512 * 112];
                for (int i = 65; i < 32768; ++i) {
                    int chunk_idx = (i & 0x7FFF) >> 9;
                    if (chunk_idx != last_chunk_idx) {
                        active_chunk = read_mem<uintptr_t>(entity_list_base + chunk_idx * 8);
                        last_chunk_idx = chunk_idx;
                        if (active_chunk) {
                            if (!read_raw(active_chunk, chunk_buffer, sizeof(chunk_buffer))) {
                                active_chunk = 0;
                            }
                        }
                    }
                    if (!active_chunk) continue;

                    int entry_idx = i & 0x1FF;
                    uint8_t* entry_ptr = chunk_buffer + 112 * entry_idx;
                    uintptr_t entity_addr = *reinterpret_cast<uintptr_t*>(entry_ptr);
                    if (!entity_addr) continue;

                    uintptr_t designer_name_ptr = *reinterpret_cast<uintptr_t*>(entry_ptr + 0x20);
                    std::string class_name = read_str(designer_name_ptr, 64);
                    if (class_name.empty()) {
                        uintptr_t vtable = read_mem<uintptr_t>(entity_addr);
                        if (vtable) {
                            uintptr_t typeinfo = read_mem<uintptr_t>(vtable - 8);
                            if (typeinfo) {
                                uintptr_t name_ptr = read_mem<uintptr_t>(typeinfo + 8);
                                if (name_ptr) {
                                    std::string raw_name = read_str(name_ptr, 64);
                                    if (!raw_name.empty()) {
                                        if (raw_name.find("C_Inferno") != std::string::npos || raw_name.find("inferno") != std::string::npos) {
                                            class_name = "C_Inferno";
                                        } else {
                                            size_t idx = 0;
                                            while (idx < raw_name.size() && std::isdigit(static_cast<unsigned char>(raw_name[idx]))) {
                                                idx++;
                                            }
                                            if (idx < raw_name.size()) {
                                                class_name = raw_name.substr(idx);
                                            } else {
                                                class_name = raw_name;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                    if (class_name.empty()) continue;

                    // Process active smoke voxel data (vischeck)
                    if (class_name == "C_SmokeGrenadeProjectile" || class_name == "smokegrenade_projectile") {
                        uint32_t handle = *reinterpret_cast<uint32_t*>(entry_ptr + 0x10);
                        // Voxel frame-decoder state; see the kSmokeVoxel* deltas
                        // documented at the top of this file.
                        uintptr_t wrapper = entity_addr + m_VoxelFrameData; // 0x1310
                        uintptr_t rgba_ptr = read_mem<uintptr_t>(wrapper + kSmokeVoxelRgbaPointerDelta); // 0x1420
                        Vec3 center = read_mem<Vec3>(entity_addr + m_vSmokeDetonationPos);
                        int32_t frame_idx = read_mem<int32_t>(wrapper + kSmokeVoxelFrameIndexDelta); // 0x1410
                        int smoke_tick = read_mem<int>(entity_addr + m_nSmokeEffectTickBegin);
                        bool did_smoke = read_mem<bool>(entity_addr + m_bDidSmokeEffect);

                        const bool smoke_data_ready = did_smoke && smoke_tick > 0 && rgba_ptr > 0x10000 &&
                                                      center.x != 0.0f && frame_idx >= 0 && frame_idx <= 255 &&
                                                      smoke_count < 4;
                        bool rgba_read_ok = false;
                        if (smoke_data_ready) {
                            ActiveSmokeData& smoke = local_packet.smokes[smoke_count];
                            smoke.active = 1;
                            smoke.entity_handle = handle;
                            smoke.center = center;
                            smoke.team = 0;
                            if (m_hThrower) {
                                const uint32_t thrower_handle = read_mem<uint32_t>(entity_addr + m_hThrower);
                                if (thrower_handle != 0 && thrower_handle != 0xFFFFFFFF) {
                                    const uintptr_t thrower_addr =
                                        get_entity_from_handle_cached(entity_list_base, thrower_handle);
                                    if (thrower_addr) {
                                        smoke.team = read_mem<uint8_t>(thrower_addr + m_iTeamNum);
                                    }
                                }
                            }
                            
                            std::vector<uint8_t> rgba(kSmokeVoxelRgbaBytes);
                            if (read_raw(rgba_ptr, rgba.data(), rgba.size())) {
                                rgba_read_ok = true;
                                memset(smoke.alphas, 0, sizeof(smoke.alphas));
                                memset(smoke.bitset, 0, sizeof(smoke.bitset));
                                
                                auto expandBits = [](uint32_t v) -> uint32_t {
                                    v = (v | (v << 16)) & 0x030000FF;
                                    v = (v | (v <<  8)) & 0x0300F00F;
                                    v = (v | (v <<  4)) & 0x030C30C3;
                                    v = (v | (v <<  2)) & 0x09249249;
                                    return v;
                                };

                                for (size_t lin = 0; lin < 32768; ++lin) {
                                    uint8_t alpha = rgba[lin * 4 + 3];
                                    if (alpha > 4) {
                                        smoke.alphas[lin] = alpha;
                                        
                                        uint32_t x = lin % 32;
                                        uint32_t y = (lin / 32) % 32;
                                        uint32_t z = lin / 1024;
                                        
                                        uint32_t morton = expandBits(x) | (expandBits(y) << 1) | (expandBits(z) << 2);
                                        uint32_t w = morton >> 6;
                                        uint32_t tz = morton & 0x3F;
                                        
                                        smoke.bitset[w] |= (1ULL << tz);
                                    }
                                }
                                smoke_count++;
                            }
                        }

                        // Debug Visibility surfaces why a smoke is (or is not)
                        // captured, so the debug view can be trusted.
                        if (atomic_debug_visibility.load(std::memory_order_relaxed)) {
                            static double last_smoke_log = 0.0;
                            const double now = std::chrono::duration<double>(
                                std::chrono::steady_clock::now().time_since_epoch()).count();
                            if (now - last_smoke_log > 1.0) {
                                last_smoke_log = now;
                                BUNNYWARE_LOG_INFO(
                                    "[SMOKE-DEBUG] handle={} wrapper=0x{:X} did_smoke={} tick={} "
                                    "rgba_ptr=0x{:X} frame_idx={} center=({:.1f}, {:.1f}, {:.1f}) "
                                    "data_ready={} rgba_read_ok={} captured={}",
                                    handle, wrapper, did_smoke, smoke_tick, rgba_ptr, frame_idx,
                                    center.x, center.y, center.z, smoke_data_ready, rgba_read_ok,
                                    smoke_count);
                            }
                        }
                    }

                    // Process active projectiles
                    if (projectile_count < 8 && 
                        (class_name == "C_HEGrenadeProjectile" || class_name == "hegrenade_projectile" ||
                         class_name == "C_FlashbangProjectile" || class_name == "flashbang_projectile" ||
                         class_name == "C_SmokeGrenadeProjectile" || class_name == "smokegrenade_projectile" ||
                         class_name == "C_MolotovProjectile" || class_name == "molotov_projectile" ||
                         class_name == "C_DecoyProjectile" || class_name == "decoy_projectile")) {

                        uint8_t type = 0;
                        if (class_name == "C_HEGrenadeProjectile" || class_name == "hegrenade_projectile") type = 1;
                        else if (class_name == "C_FlashbangProjectile" || class_name == "flashbang_projectile") type = 2;
                        else if (class_name == "C_SmokeGrenadeProjectile" || class_name == "smokegrenade_projectile") type = 3;
                        else if (class_name == "C_MolotovProjectile" || class_name == "molotov_projectile") type = 4;
                        else if (class_name == "C_DecoyProjectile" || class_name == "decoy_projectile") type = 5;

                        InFlightProjectile& proj = local_packet.projectiles[projectile_count];
                        proj.active = 1;
                        proj.type = type;
                        uint32_t handle = *reinterpret_cast<uint32_t*>(entry_ptr + 0x10);
                        proj.entity_handle = handle;
                        proj.team = 0;
                        if (m_hThrower) {
                            const uint32_t thrower_handle = read_mem<uint32_t>(entity_addr + m_hThrower);
                            if (thrower_handle != 0 && thrower_handle != 0xFFFFFFFF) {
                                const uintptr_t thrower_addr =
                                    get_entity_from_handle_cached(entity_list_base, thrower_handle);
                                if (thrower_addr) {
                                    proj.team = read_mem<uint8_t>(thrower_addr + m_iTeamNum);
                                }
                            }
                        }

                        proj.initial_position = read_mem<Vec3>(entity_addr + m_vInitialPosition);
                        proj.initial_velocity = read_mem<Vec3>(entity_addr + m_vInitialVelocity);

                        static std::vector<uint32_t> seen_projectile_handles;
                        if (std::find(seen_projectile_handles.begin(), seen_projectile_handles.end(), handle) == seen_projectile_handles.end()) {
                            seen_projectile_handles.push_back(handle);
                            if (seen_projectile_handles.size() > 64) {
                                seen_projectile_handles.erase(seen_projectile_handles.begin());
                            }
                            
                            auto time_diff = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - throw_time).count();
                            if (player_just_threw && time_diff < 1000) {
                                float speed = std::sqrt(proj.initial_velocity.x * proj.initial_velocity.x + 
                                                        proj.initial_velocity.y * proj.initial_velocity.y + 
                                                        proj.initial_velocity.z * proj.initial_velocity.z);
                                
                                if (pending_tracked_throw.is_tracked) {
                                    pending_tracked_throw.entity_handle = handle;
                                    pending_tracked_throw.init_pos = proj.initial_position;
                                    pending_tracked_throw.init_vel = proj.initial_velocity;
                                    pending_tracked_throw.init_speed = speed;
                                    pending_tracked_throw.last_known_pos = proj.initial_position;
                                    pending_tracked_throw.has_detonated = false;
                                    pending_tracked_throw.has_smoked = false;
                                    
                                    tracked_throws.push_back(pending_tracked_throw);
                                }
                                player_just_threw = false;
                            }
                        }

                        uintptr_t scene_node = read_mem<uintptr_t>(entity_addr + m_pGameSceneNode);
                        if (scene_node) {
                            proj.current_position = read_mem<Vec3>(scene_node + m_vecAbsOrigin);
                        } else if (class_name == "C_SmokeGrenadeProjectile" || class_name == "smokegrenade_projectile") {
                            proj.current_position = read_mem<Vec3>(entity_addr + m_vSmokeDetonationPos);
                        } else {
                            proj.current_position = proj.initial_position;
                        }

                        float sim_time = read_mem<float>(entity_addr + m_flSimulationTime);
                        proj.spawn_time = sim_time > 0.0f ? sim_time : (float)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() / 1000.0f;

                        proj.timer_start_time = 0.0f;
                        proj.duration = 0.0f;

                        if (class_name == "C_SmokeGrenadeProjectile" || class_name == "smokegrenade_projectile") {
                            bool did_smoke = read_mem<bool>(entity_addr + m_bDidSmokeEffect);
                            if (did_smoke) {
                                int smoke_tick = read_mem<int>(entity_addr + m_nSmokeEffectTickBegin);
                                proj.timer_start_time = smoke_tick * TICK_INTERVAL;
                                proj.duration = 18.0f;
                            }
                        } else if (class_name == "C_DecoyProjectile" || class_name == "decoy_projectile") {
                            int decoy_tick = read_mem<int>(entity_addr + m_nDecoyShotTick);
                            if (decoy_tick > 0) {
                                proj.timer_start_time = decoy_tick * TICK_INTERVAL;
                                proj.duration = 15.0f;
                            }
                        } else if (class_name == "C_MolotovProjectile" || class_name == "molotov_projectile") {
                            uint32_t thrower_handle = read_mem<uint32_t>(entity_addr + m_hThrower);
                            if (thrower_handle > 0) {
                                bool is_inc = read_mem<bool>(entity_addr + m_bIsIncGrenade);
                                thrower_to_inc[thrower_handle] = is_inc;
                            }
                        }
                        projectile_count++;
                    }
                    // Process ground infernos
                    else if ((class_name == "C_Inferno" || class_name == "inferno") && inferno_count < 4) {
                        bool is_burning = read_mem<bool>(entity_addr + m_bFireIsBurning);
                        if (!is_burning) continue;
                        int raw_fire_count = read_mem<int>(entity_addr + m_fireCount);

                        uintptr_t scene_node = read_mem<uintptr_t>(entity_addr + m_pGameSceneNode);
                        Vec3 origin = {};
                        if (scene_node) {
                            origin = read_mem<Vec3>(scene_node + m_vecAbsOrigin);
                        }

                        InfernoData& inf = local_packet.infernos[inferno_count];
                        inf.active = 1;
                        inf.entity_handle = *reinterpret_cast<uint32_t*>(entry_ptr + 0x10);

                        uint32_t owner_handle = read_mem<uint32_t>(entity_addr + m_hOwnerEntity);
                        bool is_inc = false;
                        auto it = thrower_to_inc.find(owner_handle);
                        if (it != thrower_to_inc.end()) {
                            is_inc = it->second;
                        }
                        inf.duration = is_inc ? 5.5f : 7.0f;
                        inf.start_time = (float)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() / 1000.0f;

                        if (raw_fire_count > 0) {
                            uintptr_t fire_positions_addr = entity_addr + m_firePositions;
                            inf.fire_count = std::min(raw_fire_count, 64);
                            for (int f = 0; f < inf.fire_count; ++f) {
                                inf.fire_positions[f] = read_mem<Vec3>(fire_positions_addr + (f * 12));
                            }
                        } else {
                            inf.fire_count = 1;
                            inf.fire_positions[0] = origin;
                        }
                        inferno_count++;
                    }
                    // Process dynamic doors
                    else if ((class_name == "C_PropDoorRotating" || class_name == "C_BasePropDoor" || class_name == "C_BaseDoor") && door_count < 32) {
                        uintptr_t scene_node = read_mem<uintptr_t>(entity_addr + m_pGameSceneNode);
                        if (scene_node) {
                            Vec3 origin = read_mem<Vec3>(scene_node + m_vecAbsOrigin);
                            Vec3 angles = read_mem<Vec3>(scene_node + m_angAbsRotation);
                            float scale = read_mem<float>(scene_node + m_flAbsScale);

                            std::string model = resolve_model_name(entity_addr, scene_node);
                            if (!model.empty()) {
                                DoorData& door = local_packet.doors[door_count];
                                door.active = 1;
                                door.origin = origin;
                                door.yaw = angles.y;
                                door.scale = Vec3(scale, scale, scale);
                                std::strncpy(door.model_name, model.c_str(), sizeof(door.model_name) - 1);
                                door.model_name[sizeof(door.model_name) - 1] = '\0';
                                door_count++;
                            }
                        }
                    }
                    else if (class_name == "C_PlantedC4" || class_name == "planted_c4" || class_name == "PlantedC4") {
                        planted_c4_entity_addr = entity_addr;
                    }
                    else if (class_name == "C_C4" || class_name == "c4") {
                        // Carried bomb being armed
                        bool arming = m_bStartedArming && read_mem<bool>(entity_addr + m_bStartedArming);
                        if (arming) {
                            uintptr_t scene_node = read_mem<uintptr_t>(entity_addr + m_pGameSceneNode);
                            if (scene_node) {
                                local_packet.c4_arming.active = 1;
                                local_packet.c4_arming.position = read_mem<Vec3>(scene_node + m_vecAbsOrigin);
                            }
                        }
                    }
                    else if (dropped_item_count < static_cast<int>(shm::MAX_DROPPED_ITEMS)) {
                        uintptr_t weapon_vdata = read_mem<uintptr_t>(entity_addr + m_nSubclassID + 8);
                        if (weapon_vdata) {
                            uintptr_t name_ptr = read_mem<uintptr_t>(weapon_vdata + m_szName);
                            if (name_ptr) {
                                std::string weapon_name = read_str(name_ptr, 64);
                                if (weapon_name.find("weapon_") != std::string::npos) {
                                    int32_t owner_handle = read_mem<int32_t>(entity_addr + m_hOwnerEntity);
                                    if (owner_handle == -1) {
                                        uintptr_t scene_node = read_mem<uintptr_t>(entity_addr + m_pGameSceneNode);
                                        if (scene_node) {
                                            Vec3 origin = read_mem<Vec3>(scene_node + m_vecAbsOrigin);
                                            
                                            // Clean name (strip "weapon_")
                                            std::string clean_name = weapon_name;
                                            if (clean_name.rfind("weapon_", 0) == 0) {
                                                clean_name = clean_name.substr(7);
                                            }

                                            DroppedItem& item = local_packet.dropped_items[dropped_item_count];
                                            item.active = 1;
                                            item.origin = origin;
                                            std::strncpy(item.name, clean_name.c_str(), sizeof(item.name) - 1);
                                            item.name[sizeof(item.name) - 1] = '\0';
                                            dropped_item_count++;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
            local_packet.projectile_count = projectile_count;
            local_packet.inferno_count = inferno_count;
            local_packet.door_count = door_count;
            local_packet.dropped_item_count = dropped_item_count;
            local_packet.smoke_count = smoke_count;
            for (int i = door_count; i < 32; ++i) {
                local_packet.doors[i].active = 0;
            }
            for (int i = dropped_item_count; i < static_cast<int>(shm::MAX_DROPPED_ITEMS); ++i) {
                local_packet.dropped_items[i].active = 0;
            }
            for (int i = smoke_count; i < 4; ++i) {
                local_packet.smokes[i].active = 0;
            }

            if (planted_c4_entity_addr) {
                uintptr_t scene_node = read_mem<uintptr_t>(planted_c4_entity_addr + m_pGameSceneNode);
                Vec3 origin = {};
                if (scene_node) {
                    origin = read_mem<Vec3>(scene_node + m_vecAbsOrigin);
                }

                local_packet.planted_c4.active = 1;
                local_packet.planted_c4.position = origin;
                
                float blow_time = read_mem<float>(planted_c4_entity_addr + m_flC4Blow);
                float defuse_countdown = read_mem<float>(planted_c4_entity_addr + m_flDefuseCountDown);
                
                float curtime = 0.0f;
                uintptr_t global_vars = read_mem<uintptr_t>(client_base + dwGlobalVars);
                if (global_vars) {
                    curtime = read_mem<float>(global_vars + 0x30);
                }
                
                local_packet.planted_c4.time_left = std::max(0.0f, blow_time - curtime);
                local_packet.planted_c4.defuse_time_left = defuse_countdown > 0.0f ? std::max(0.0f, defuse_countdown - curtime) : 0.0f;
                local_packet.planted_c4.is_being_defused = read_mem<uint8_t>(planted_c4_entity_addr + m_bBeingDefused);
                local_packet.planted_c4.is_defused = read_mem<uint8_t>(planted_c4_entity_addr + m_bBombDefused);
                local_packet.planted_c4.has_exploded = read_mem<uint8_t>(planted_c4_entity_addr + m_bHasExploded);
            } else {
                local_packet.planted_c4.active = 0;
            }

            auto end = std::chrono::high_resolution_clock::now();
            double elapsed = std::chrono::duration<double, std::milli>(end - start).count();
            entities_total_time += elapsed;
            entities_max_time = std::max(entities_max_time, elapsed);
            entities_ticks++;
            last_entities_tick = now;
            packet_changed = true;
        }

        // Mutex Copy & Publish
        if (packet_changed) {
            std::lock_guard<std::mutex> lock(packet_mutex);
            // Copy data to local latest packet
            uint32_t final_frame = latest_packet.frame_index + 1;
            
            // Backup the index, overwrite struct, restore incremented index
            local_packet.frame_index = final_frame;
            latest_packet = local_packet;
            latest_impacts = pending_impacts;
            has_new_data = true;
        }

        // Update and check tracked throws
        for (auto it = tracked_throws.begin(); it != tracked_throws.end(); ) {
            bool found_in_active = false;
            Vec3 current_pos = it->init_pos;
            
            for (int p = 0; p < local_packet.projectile_count; ++p) {
                if (local_packet.projectiles[p].entity_handle == it->entity_handle) {
                    found_in_active = true;
                    current_pos = local_packet.projectiles[p].current_position;
                    break;
                }
            }
            
            if (found_in_active) {
                it->last_known_pos = current_pos;
                
                // Read did_smoke status
                uintptr_t entity_addr = get_entity_from_handle(it->entity_handle);
                if (entity_addr) {
                    bool did_smoke = read_mem<bool>(entity_addr + m_bDidSmokeEffect);
                    if (did_smoke && !it->has_smoked) {
                        it->has_smoked = true;
                        it->has_detonated = true;
                    }
                }
            } else {
                // Projectile vanished
                if (!it->has_detonated) {
                    it->has_detonated = true;
                }
            }
            
            if (it->has_detonated) {
                it = tracked_throws.erase(it);
            } else {
                ++it;
            }
        }

        // Calculate when the next tick is due across all loops
        auto next_local = last_local_tick + std::chrono::duration_cast<std::chrono::high_resolution_clock::duration>(std::chrono::duration<double, std::milli>(atomic_local_ms.load(std::memory_order_relaxed)));
        auto next_players = last_players_tick + std::chrono::duration_cast<std::chrono::high_resolution_clock::duration>(std::chrono::duration<double, std::milli>(atomic_players_ms.load(std::memory_order_relaxed)));
        auto next_entities = last_entities_tick + std::chrono::duration_cast<std::chrono::high_resolution_clock::duration>(std::chrono::duration<double, std::milli>(atomic_entities_ms.load(std::memory_order_relaxed)));

        auto next_tick = std::min({next_local, next_players, next_entities});
        auto tick_now = std::chrono::high_resolution_clock::now();
        if (next_tick > tick_now) {
            // Sleep on an absolute monotonic deadline until the next tick.
            // (Previously: coarse sleep + yield-spin, which burned ~40% of a core.)
            precision_sleep_until(std::chrono::steady_clock::now() + (next_tick - tick_now));
        } else {
            std::this_thread::yield();
        }
    }
}

bool GameReader::initialize(const struct OverlayConfig* config) {
    cfg = config;
    if (cfg) {
        atomic_local_ms.store(cfg->gamereader_local_ms, std::memory_order_relaxed);
        atomic_players_ms.store(cfg->gamereader_players_ms, std::memory_order_relaxed);
        atomic_entities_ms.store(cfg->gamereader_entities_ms, std::memory_order_relaxed);
        atomic_debug_reader.store(cfg->debug_game_reader, std::memory_order_relaxed);
        atomic_debug_visibility.store(cfg->debug_visibility, std::memory_order_relaxed);
    }
    last_debug_print_time = (double)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() / 1000.0;
    


    BUNNYWARE_LOG_INFO("GameReader: Initializing game reader...");

    // Refresh the offsets BUNNYWARE consumes straight from the live cs2 process
    // (process_vm_readv only).
    const offset_refresher::RefreshStatus refresh = offset_refresher::refresh_from_live_process();
    if (refresh.attempted && !refresh.success) {
        if (refresh.target_found) {
            BUNNYWARE_LOG_ERROR("GameReader: Live offset refresh failed ({} error(s)); using the existing offsets.json values.",
                              refresh.errors.size());
        } else {
            BUNNYWARE_LOG_INFO("GameReader: cs2 is not running yet; keeping the existing offsets.json values. Start cs2 first to auto-refresh.");
        }
    }

    if (!load_offsets_and_schemas()) {
        BUNNYWARE_LOG_ERROR("GameReader: Failed to load offsets.json or libclient.so.json.");
        return false;
    }

    is_running = true;
    reader_thread = std::thread(&GameReader::reader_loop, this);
    BUNNYWARE_LOG_INFO("GameReader: Game reader thread spawned successfully.");
    return true;
}

void GameReader::shutdown() {
    is_running = false;
    if (reader_thread.joinable()) {
        reader_thread.join();
    }
    reset_process_state();
}

bool GameReader::fetch_latest(ShmPacket& out_packet) {
    if (cfg) {
        atomic_local_ms.store(cfg->gamereader_local_ms, std::memory_order_relaxed);
        atomic_players_ms.store(cfg->gamereader_players_ms, std::memory_order_relaxed);
        atomic_entities_ms.store(cfg->gamereader_entities_ms, std::memory_order_relaxed);
        atomic_debug_reader.store(cfg->debug_game_reader, std::memory_order_relaxed);
        atomic_debug_visibility.store(cfg->debug_visibility, std::memory_order_relaxed);
        atomic_client_impacts.store(cfg->bullet_impacts_enabled, std::memory_order_relaxed);
    }
    std::lock_guard<std::mutex> lock(packet_mutex);
    if (!has_new_data) return false;

    out_packet = latest_packet;
    has_new_data = false;
    return true;
}

impacts::SnapshotPtr GameReader::fetch_client_impacts() {
    std::lock_guard<std::mutex> lock(packet_mutex);
    return latest_impacts;
}

bool GameReader::read_view_matrix(float* out_matrix) {
    if (target_pid > 0 && client_base != 0 && dwViewMatrix != 0) {
        return read_raw(client_base + dwViewMatrix, out_matrix, sizeof(float) * 16);
    }
    std::lock_guard<std::mutex> lock(packet_mutex);
    std::memcpy(out_matrix, latest_packet.view_matrix, sizeof(latest_packet.view_matrix));
    return true;
}

TargetHitboxData GameReader::query_hitboxes(uintptr_t target_pawn_addr) const {
    TargetHitboxData result{};
    result.count = 0;
    if (!target_pawn_addr) return result;

    uintptr_t game_scene_node = read_mem<uintptr_t>(target_pawn_addr + m_pGameSceneNode);
    if (!game_scene_node) return result;

    uintptr_t model_handle = read_mem<uintptr_t>(game_scene_node + 0x210);
    if (!model_handle) return result;

    uintptr_t cmodel = read_mem<uintptr_t>(model_handle);
    if (!cmodel) return result;

    uintptr_t render_meshes_ptr = read_mem<uintptr_t>(cmodel + 0x78);
    if (!render_meshes_ptr) return result;

    uintptr_t render_meshes = read_mem<uintptr_t>(render_meshes_ptr);
    if (!render_meshes) return result;

    uintptr_t hitbox_data = read_mem<uintptr_t>(render_meshes + 0x150);
    if (!hitbox_data) return result;

    const int count = read_mem<int>(hitbox_data + 0x28);
    if (count <= 0 || count > 20) return result;

    const int remap_count = read_mem<int>(cmodel + 0x220);

    // Serve repeat queries of the same model straight from the cache. The pointer
    // chain above (cmodel/count/remap_count) is re-validated on every call.
    for (size_t i = 0; i < kHitboxCacheSize; ++i) {
        const HitboxCacheEntry& cached = hitbox_cache_[i];
        if (cached.cmodel == cmodel && cached.count == count && cached.remap_count == remap_count) {
            return cached.data;
        }
    }

    uintptr_t array_ptr = read_mem<uintptr_t>(hitbox_data + 0x30);
    if (!array_ptr) return result;

    uintptr_t remap_table = read_mem<uintptr_t>(cmodel + 0x228);
    uintptr_t mesh_a = read_mem<uintptr_t>(cmodel + 0x240);
    uintptr_t mesh_b = read_mem<uintptr_t>(cmodel + 0x2F0);
    const bool has_remap = remap_table && mesh_a && mesh_b && remap_count > 0;

    // Without a bone remap table the entries cannot be mapped to bones, so the
    // caller gets an empty result (unchanged behaviour).
    if (!has_remap) return result;

    // One bulk read for the remap table instead of one syscall per entry.
    constexpr int kMaxRemapEntries = 512;
    int16_t remap_buf[kMaxRemapEntries] = {};
    const int remap_to_read = std::min(remap_count, kMaxRemapEntries);
    if (!read_raw(remap_table, remap_buf, static_cast<size_t>(remap_to_read) * sizeof(int16_t))) {
        return result;
    }

    const uint16_t ofs_a = read_mem<uint16_t>(mesh_a);
    const uint16_t ofs_b = read_mem<uint16_t>(mesh_b);

    // ... and one bulk read for the whole hitbox array instead of up to five reads
    // per hitbox entry.
    constexpr size_t kHitboxStride = 0x70;
    uint8_t hitbox_raw[20 * kHitboxStride];
    if (!read_raw(array_ptr, hitbox_raw, static_cast<size_t>(count) * kHitboxStride)) {
        return result;
    }

    parse_hitbox_entries(hitbox_raw, count, remap_buf, remap_to_read, ofs_a, ofs_b, result);

    // Store the resolved set so later ticks for the same model cost ~8 reads.
    HitboxCacheEntry& cache_slot = hitbox_cache_[hitbox_cache_cursor_];
    cache_slot.cmodel = cmodel;
    cache_slot.count = count;
    cache_slot.remap_count = remap_count;
    cache_slot.data = result;
    hitbox_cache_cursor_ = (hitbox_cache_cursor_ + 1) % kHitboxCacheSize;

    return result;
}

void GameReader::reset_process_state() {
    target_pid = -1;
    client_base = 0;
    engine_base = 0;
    schemasystem_base = 0;
    tier0_base = 0;
    cvar_interface = 0;
    tier0_path.clear();
    model_name_cache.clear();
    clear_hitbox_cache();
    last_seen_map_name.clear();
    impact_layout.reset();
    impact_layout_client_base = 0;
    pending_impacts.reset();
}

ProcessMemoryView GameReader::make_process_memory_view() const {
    ProcessMemoryView mem{};
    mem.pid = target_pid.load();
    mem.tier0_base = tier0_base.load();
    mem.tier0_path = tier0_path;
    mem.read_fn = [this](uintptr_t address, void* dest, size_t size) {
        return read_raw(address, dest, size);
    };
    mem.read_str_fn = [this](uintptr_t address, size_t max_len) {
        return read_str(address, max_len);
    };
    return mem;
}

uintptr_t GameReader::resolve_cvar_interface_locked() {
    if (tier0_base.load() == 0) {
        return 0;
    }

    ProcessMemoryView mem = make_process_memory_view();
    return resolve_cvar_interface(mem, tier0_VEngineCvar_rva);
}

ConvarLookupResult GameReader::lookup_convar(const char* name) const {
    ProcessMemoryView mem = make_process_memory_view();
    uintptr_t iface = cvar_interface.load();
    if (iface == 0 && mem.tier0_base != 0) {
        iface = resolve_cvar_interface(mem, tier0_VEngineCvar_rva);
    }
    return ::lookup_convar(mem, iface, name);
}
