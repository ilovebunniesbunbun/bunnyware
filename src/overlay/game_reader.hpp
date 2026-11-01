#pragma once
#include "../app/convar_reader.hpp"
#include "client_impacts.hpp"
#include <optional>
#include <thread>
#include <mutex>
#include <atomic>
#include <string>
#include <unordered_map>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <cstdint>
#include <cstring>
#include <algorithm>

namespace shm {
    constexpr std::size_t MAX_PLAYERS = 64;
    constexpr std::size_t MAX_BONES = 128;
    constexpr std::size_t MAX_PROJECTILES = 8;
    constexpr std::size_t MAX_MODEL_NAME = 128;
    constexpr std::size_t MAX_MAP_NAME = 64;
    constexpr std::size_t VIEW_MATRIX_SIZE = 16;
    constexpr std::size_t MAX_DOORS = 32;
    constexpr std::size_t MAX_DROPPED_ITEMS = 64;
}

using Vec3 = glm::vec3;
using Vec4 = glm::vec4;

/**
 * @name normalize_flash_alpha
 * @description Folds C_CSPlayerPawnBase::m_flFlashOverlayAlpha onto a common
 * 0..255 scale. Current CS2 builds expose the field normalized to 0..1 while
 * older ones used 0..255, so values inside (0, 1] are scaled up. Consumers (the
 * ESP "Flashed" flag and Anti Flash) can then share a single threshold scale.
 */
inline float normalize_flash_alpha(float raw)
{
    return (raw > 0.0f && raw <= 1.0f) ? raw * 255.0f : raw;
}

#pragma pack(push, 1)

struct DroppedItem {
    Vec3 origin;
    char name[32];
    uint8_t active;
    uint8_t pad[3];
};

#pragma pack(pop)

// Full bone transform: position + quaternion rotation
struct BoneTransform {
    Vec3 position;
    Vec4 rotation;  // quaternion (x, y, z, w)
};

enum GrenadeType : uint8_t {
    GRENADE_NONE = 0,
    GRENADE_HE,
    GRENADE_FLASH,
    GRENADE_SMOKE,
    GRENADE_MOLOTOV,
    GRENADE_DECOY,
    GRENADE_C4
};

enum WeaponId : uint16_t {
    weapon_none = 0,
    weapon_deagle = 1,
    weapon_elite = 2,
    weapon_fiveseven = 3,
    weapon_glock = 4,
    weapon_ak47 = 7,
    weapon_aug = 8,
    weapon_awp = 9,
    weapon_famas = 10,
    weapon_g3sg1 = 11,
    weapon_galilar = 13,
    weapon_m249 = 14,
    weapon_m4a1 = 16,
    weapon_mac10 = 17,
    weapon_p90 = 19,
    weapon_ump = 24,
    weapon_xm1014 = 25,
    weapon_bizon = 26,
    weapon_mag7 = 27,
    weapon_negev = 28,
    weapon_sawedoff = 29,
    weapon_tec9 = 30,
    weapon_taser = 31,
    weapon_hkp2000 = 32,
    weapon_mp7 = 33,
    weapon_mp9 = 34,
    weapon_nova = 35,
    weapon_p250 = 36,
    weapon_scar20 = 38,
    weapon_sg556 = 39,
    weapon_ssg08 = 40,
    weapon_knife = 42,
    weapon_flashbang = 43,
    weapon_hegrenade = 44,
    weapon_smokegrenade = 45,
    weapon_molotov = 46,
    weapon_decoy = 47,
    weapon_incgrenade = 48,
    weapon_c4 = 49,
    weapon_knife_t = 59,
    weapon_m4a1_silencer = 60,
    weapon_usp_silencer = 61,
    weapon_cz75a = 63,
    weapon_revolver = 64,
    weapon_bayonet = 500,
    weapon_knife_flip = 505,
    weapon_knife_gut = 506,
    weapon_knife_karambit = 507,
    weapon_knife_m9_bayonet = 508,
    weapon_knife_tactical = 509,
    weapon_knife_falchion = 512,
    weapon_knife_survival_bowie = 514,
    weapon_knife_butterfly = 515,
    weapon_knife_push = 516,
    weapon_knife_kukri = 526
};

enum HitboxGroupEnum {
    HITBOX_GROUP_HEAD     = 1 << 0,
    HITBOX_GROUP_NECK     = 1 << 1,
    HITBOX_GROUP_CHEST    = 1 << 2,
    HITBOX_GROUP_STOMACH  = 1 << 3,
    HITBOX_GROUP_ARM      = 1 << 4,
    HITBOX_GROUP_LEG      = 1 << 5
};

inline HitboxGroupEnum get_hitbox_group_from_bone(int bone) {
    if (bone == 7 || bone == 8) {
        return HITBOX_GROUP_HEAD;
    }
    if (bone == 6) {
        return HITBOX_GROUP_NECK;
    }
    if (bone == 23 || bone == 5 || bone == 4) {
        return HITBOX_GROUP_CHEST;
    }
    if (bone == 1 || bone == 0 || bone == 2 || bone == 3) {
        return HITBOX_GROUP_STOMACH;
    }
    if (bone >= 9 && bone <= 16) {
        return HITBOX_GROUP_ARM;
    }
    if ((bone >= 17 && bone <= 22) || bone >= 24) {
        return HITBOX_GROUP_LEG;
    }
    return HITBOX_GROUP_CHEST;
}

inline bool is_hitbox_group_enabled(int bone, int enabled_mask) {
    HitboxGroupEnum grp = get_hitbox_group_from_bone(bone);
    return (enabled_mask & grp) != 0;
}

enum WeaponGroup {
    WEAPON_GROUP_LIGHT_PISTOL,
    WEAPON_GROUP_HEAVY_PISTOL,
    WEAPON_GROUP_RIFLE,
    WEAPON_GROUP_AWP,
    WEAPON_GROUP_SCOUT,
    WEAPON_GROUP_SNIPER,
    WEAPON_GROUP_SMG,
    WEAPON_GROUP_MG,
    WEAPON_GROUP_SHOTGUN,
    WEAPON_GROUP_OTHERS
};

inline WeaponGroup get_weapon_group(uint16_t id)
{
    switch (id) {
        // Light pistols
        case 2:  // Dualies
        case 3:  // Five-Seven
        case 4:  // Glock
        case 30: // Tec-9
        case 32: // P2000
        case 36: // P250
        case 61: // USP-S
        case 63: // CZ75-Auto
            return WEAPON_GROUP_LIGHT_PISTOL;
        // Heavy pistols
        case 1:  // Deagle
        case 64: // Revolver
            return WEAPON_GROUP_HEAVY_PISTOL;
        // Rifles
        case 7:  // AK-47
        case 8:  // AUG
        case 10: // Famas
        case 13: // Galil
        case 16: // M4A4
        case 39: // SG556
        case 60: // M4A1-S
            return WEAPON_GROUP_RIFLE;
        // AWP
        case 9:
            return WEAPON_GROUP_AWP;
        // Scout
        case 40:
            return WEAPON_GROUP_SCOUT;
        // Auto Snipers
        case 11: // G3SG1
        case 38: // SCAR-20
            return WEAPON_GROUP_SNIPER;
        // SMGs
        case 17: // Mac-10
        case 19: // P90
        case 24: // Ump-45
        case 26: // Bizon
        case 33: // MP7
        case 34: // MP9
            return WEAPON_GROUP_SMG;
        // Shotguns
        case 25: // XM1014
        case 27: // Mag-7
        case 29: // Sawed-off
        case 35: // Nova
            return WEAPON_GROUP_SHOTGUN;
        // MGs
        case 14: // M249
        case 28: // Negev
            return WEAPON_GROUP_MG;
        default:
            return WEAPON_GROUP_OTHERS;
    }
}

inline bool is_shootable_weapon(uint16_t id)
{
    bool is_knife = (id == 42 || id == 59 || id == 500 || id == 503 || (id >= 505 && id <= 526));
    bool is_grenade = (id >= 43 && id <= 48);
    bool is_c4 = (id == 49);
    bool is_other_non_shootable = (id == 0);
    return !is_knife && !is_grenade && !is_c4 && !is_other_non_shootable;
}

inline void get_weapon_autowall_fallback_stats(uint16_t id, float& base_damage, float& penetration, float& armor_ratio, float& headshot_mult, float& range_modifier)
{
    WeaponGroup w_group = get_weapon_group(id);
    range_modifier = 0.99f;
    headshot_mult = 4.0f;
    switch (w_group) {
        case WEAPON_GROUP_AWP:
            base_damage = 115.0f;
            penetration = 2.5f;
            armor_ratio = 1.95f;
            break;
        case WEAPON_GROUP_SCOUT:
            base_damage = 88.0f;
            penetration = 2.0f;
            armor_ratio = 1.70f;
            break;
        case WEAPON_GROUP_SNIPER:
            base_damage = 80.0f;
            penetration = 2.0f;
            armor_ratio = 1.65f;
            break;
        case WEAPON_GROUP_RIFLE:
            base_damage = 35.0f;
            penetration = 2.0f;
            armor_ratio = 1.48f;
            break;
        case WEAPON_GROUP_HEAVY_PISTOL:
            base_damage = 53.0f;
            penetration = 2.0f;
            armor_ratio = 1.86f;
            break;
        case WEAPON_GROUP_MG:
            base_damage = 35.0f;
            penetration = 2.0f;
            armor_ratio = 1.60f;
            break;
        case WEAPON_GROUP_SMG:
            base_damage = 29.0f;
            penetration = 1.0f;
            armor_ratio = 1.15f;
            break;
        case WEAPON_GROUP_LIGHT_PISTOL:
            base_damage = 28.0f;
            penetration = 1.0f;
            armor_ratio = 0.94f;
            break;
        case WEAPON_GROUP_SHOTGUN:
            base_damage = 20.0f;
            penetration = 0.0f;
            armor_ratio = 1.00f;
            break;
        default:
            base_damage = 35.0f;
            penetration = 1.0f;
            armor_ratio = 1.00f;
            break;
    }
}

inline float scale_damage_by_hitgroup(int bone_id, int target_armor, bool target_has_helmet, float initial_damage, float armor_ratio, float headshot_mult) {
    HitboxGroupEnum group = get_hitbox_group_from_bone(bone_id);
    
    float multiplier = 1.0f;
    switch (group) {
        case HITBOX_GROUP_HEAD:    multiplier = headshot_mult; break;
        case HITBOX_GROUP_STOMACH: multiplier = 1.25f; break;
        case HITBOX_GROUP_LEG:     multiplier = 0.75f; break;
        default:                   multiplier = 1.0f;  break; // Chest, Neck, Arms
    }
    
    float damage = initial_damage * multiplier;
    
    // In CS2, legs and arms bypass armor entirely
    if (target_armor > 0 && group != HITBOX_GROUP_LEG && group != HITBOX_GROUP_ARM) {
        bool armor_protects = (group != HITBOX_GROUP_HEAD) || target_has_helmet;
        if (armor_protects) {
            damage *= armor_ratio * 0.5f;
        }
    }
    return std::max(damage, 0.0f);
}


#pragma pack(push, 1)

struct HitboxEntry {
    int bone;
    Vec3 mins;
    Vec3 maxs;
    float radius;
};

struct TargetHitboxData {
    int count;
    HitboxEntry entries[20];
};

struct InFlightProjectile {
    uint32_t entity_handle;     // CS2 entity index (EntIndex)
    uint8_t type;               // GrenadeType enum
    Vec3 initial_position;      // Position when thrown
    Vec3 initial_velocity;      // Velocity when thrown
    Vec3 current_position;      // Real-time world origin / detonation origin (via SceneNode)
    float spawn_time;           // Game time (curtime) when thrown
    float timer_start_time;     // Effect start time (curtime)
    float duration;             // Total duration (e.g. 18s, 15s)
    uint8_t active;             // 1 if active, 0 if empty slot
    uint8_t team;               // Thrower team (2 = T, 3 = CT, 0 = unknown)
};

struct InfernoData {
    uint32_t entity_handle;
    float start_time;
    float duration;
    int fire_count;
    Vec3 fire_positions[64];
    uint8_t active;
};

struct DoorData {
    Vec3 origin;
    float yaw;
    Vec3 scale;
    char model_name[128];
    uint8_t active;
};

struct ActiveSmokeData {
    uint32_t entity_handle;
    Vec3 center;
    uint64_t bitset[512];
    uint8_t alphas[32768];
    uint8_t active;
    uint8_t team;       // Thrower team (2 = T, 3 = CT, 0 = unknown)
    uint8_t pad[6];
};

struct SpectatorData {
    char name[32];
    uint8_t is_first_person;
};

// Round / match state pulled from C_CSGameRules (+ team entities for scores).
struct GameRulesData {
    uint8_t active;             // 1 when the game rules object was readable
    uint8_t is_freeze_period;
    uint8_t is_warmup;
    uint8_t pad;
    int round_time;             // C_CSGameRules::m_iRoundTime (seconds remaining)
    int total_rounds_played;    // C_CSGameRules::m_totalRoundsPlayed
    int game_phase;             // C_CSGameRules::m_gamePhase
    int t_score;                // C_Team::m_iScore for team 2
    int ct_score;               // C_Team::m_iScore for team 3
};

struct PlayerData {
    int team;
    int health;
    int active;
    int entity_index;
    int has_defuser;
    Vec3 origin;
    char model_name[shm::MAX_MODEL_NAME];        // e.g. "ctm_sas", "tm_leet" for mesh lookup
    int bone_count;             // actual bone count for this player
    BoneTransform bones[shm::MAX_BONES];   // full skeletal transforms
    char name[32];
    char active_weapon_name[32];
    int armor;
    uint8_t has_helmet;
    uint8_t is_scoped;
    uint8_t is_reloading;
    uint8_t is_walking;
    int ammo;
    int max_ammo;
    float emit_sound_time; // C_CSPlayerPawn::m_flEmitSoundTime (GameTime_t)
    float flash_alpha; // C_CSPlayerPawnBase::m_flFlashOverlayAlpha (0-255 after normalize_flash_alpha)
    uint8_t has_bomb;      // 1 if the player is carrying weapon_c4
    uint8_t pad_noise[3];
    Vec3 velocity;
    uint32_t flags;  // m_fFlags (FL_ONGROUND = 1)
    int shots_fired;
    Vec3 eye_angles;  // C_CSPlayerPawn::m_angEyeAngles (pitch, yaw, roll)
};

struct PlantedC4 {
    uint8_t active;
    Vec3 position;
    float time_left;
    float defuse_time_left;
    uint8_t is_being_defused;
    uint8_t is_defused;
    uint8_t has_exploded;
};

// Carried C4 currently being armed (plant noise)
struct C4ArmingState {
    uint8_t active;
    uint8_t pad[3];
    Vec3 position;
};

struct ShmPacket {
    uint32_t frame_index;
    float view_matrix[shm::VIEW_MATRIX_SIZE];
    Vec3 local_eye;
    char map_name[shm::MAX_MAP_NAME];
    float curtime; // GlobalVars curtime for emit-sound age checks
    int game_window_width;
    int game_window_height;

    // --- Local Player Throw State ---
    uint16_t held_weapon_id;
    uint8_t held_grenade_type;  // GrenadeType (0 if none held/pin not pulled)
    uint8_t pin_pulled;         // 1 = True, 0 = False
    float throw_strength;       // 0.0 to 1.0
    Vec3 local_velocity;        // Local player velocity vector
    Vec3 local_angles;          // Local player view angles (pitch/yaw/roll)
    
    // --- Planted C4 ---
    PlantedC4 planted_c4;
    C4ArmingState c4_arming;
    
    // --- Local Player Info ---
    Vec3 local_origin;
    int local_team;
    int local_health;
    int local_armor;
    float local_flash_alpha;   // C_CSPlayerPawnBase::m_flFlashOverlayAlpha (0-255 blind amount)
    uint8_t local_has_defuser;
    float local_total_damage;
    int local_total_kills;
    int local_total_kills_headshots;
    Vec3 local_aim_punch;
    int local_shots_fired;
    char local_crosshair_code[64];
    float local_spread;
    int local_in_crosshair_id;
    uint32_t local_flags;
    uint8_t local_is_scoped;
    uint8_t local_kills_valid;  // Round kill counters were read (zeroed otherwise)
    uint8_t local_pad[2];
    float local_sensitivity;
    float local_fov_sensitivity_adjust;
    
    // --- Triggerbot Advanced Fields ---
    float local_base_spread;
    float local_recoil_index;
    int local_num_bullets;
    int local_spread_seed;
    int local_recoil_seed;
    int local_tick_base;
    int local_seed_tick;

    // --- Triggerbot Convar Accuracy Overrides ---
    float local_weapon_accuracy_nospread;     // 0.0 or 1.0 from convar
    float local_weapon_accuracy_forcespread;  // forced spread override from convar
    float local_weapon_air_spread_scale;       // air spread scaling factor from convar
    float local_sv_jump_impulse;              // sv_jump_impulse value for air inaccuracy

    // --- Per-player hitboxes for triggerbot auto-fire mode ---
    TargetHitboxData player_hitboxes[shm::MAX_PLAYERS];
    
    // --- Local Player Active Weapon Characteristics ---
    float local_weapon_damage;
    float local_weapon_range_modifier;
    float local_weapon_penetration;
    float local_weapon_armor_ratio;
    float local_weapon_headshot_multiplier;
    
    TargetHitboxData target_hitboxes;
    
    // --- In-Flight Projectiles ---
    int projectile_count;
    InFlightProjectile projectiles[shm::MAX_PROJECTILES]; // Max 8 active projectiles tracked concurrently
    
    // --- Ground Inferno Zones ---
    int inferno_count;
    InfernoData infernos[4];

    // --- Dynamic Doors ---
    int door_count;
    DoorData doors[shm::MAX_DOORS];

    // --- Dropped Items ---
    int dropped_item_count;
    DroppedItem dropped_items[shm::MAX_DROPPED_ITEMS];

    // --- Active Smokes ---
    int smoke_count;
    ActiveSmokeData smokes[4];

    int spectator_count;
    SpectatorData spectators[shm::MAX_PLAYERS];

    int player_count;
    PlayerData players[shm::MAX_PLAYERS];

    GameRulesData game_rules;

    // Local player's display name (from the local controller). The local pawn is
    // intentionally excluded from players[] for the ESP, so the web radar reads
    // the name from here when synthesizing the local dot.
    char local_name[32];
};
#pragma pack(pop)

static_assert(sizeof(DroppedItem) == 48, "DroppedItem packing mismatch");
static_assert(sizeof(InFlightProjectile) == 55, "InFlightProjectile packing mismatch");
static_assert(sizeof(ActiveSmokeData) == 36888, "ActiveSmokeData packing mismatch");
static_assert(sizeof(InfernoData) == 785, "InfernoData packing mismatch");
static_assert(sizeof(DoorData) == 157, "DoorData packing mismatch");
static_assert(sizeof(PlayerData) == 3872, "PlayerData packing mismatch");
static_assert(sizeof(GameRulesData) == 24, "GameRulesData packing mismatch");
static_assert(sizeof(SpectatorData) == 33, "SpectatorData packing mismatch");
static_assert(sizeof(C4ArmingState) == 16, "C4ArmingState packing mismatch");
static_assert(sizeof(ShmPacket) == 451529, "ShmPacket packing mismatch");

/**
 * @name parse_hitbox_entries
 * @description Decodes the raw CModel hitbox array (stride 0x70, see
 * GameReader::query_hitboxes) into the packet-facing representation, applying the
 * model's bone remap table and the caller's mesh offsets. Entries whose remap slot
 * is out of range, whose bone is negative, or whose radius is out of bounds are
 * skipped; at most 20 entries are appended to out.count.
 *
 * Kept free-standing so the scatter-gathered read path can be unit tested without a
 * live game process.
 *
 * @example
 * parse_hitbox_entries(raw, count, remap_buf, remap_entries, ofs_a, ofs_b, result);
 */
inline void parse_hitbox_entries(const uint8_t* raw, int count, const int16_t* remap, int remap_entries, uint16_t ofs_a,
                                 uint16_t ofs_b, TargetHitboxData& out)
{
    if (!raw || !remap || count <= 0) return;

    for (int i = 0; i < count; ++i) {
        const uint8_t* const entry = raw + static_cast<std::size_t>(i) * 0x70;

        uint16_t hb_idx = 0;
        std::memcpy(&hb_idx, entry + 0x48, sizeof(hb_idx));
        const std::size_t slot = static_cast<std::size_t>(hb_idx + ofs_a + ofs_b);
        if (slot >= static_cast<std::size_t>(remap_entries)) continue;

        const int bone = remap[slot];
        if (bone < 0) continue;

        float radius = 0.0f;
        std::memcpy(&radius, entry + 0x30, sizeof(radius));
        if (radius < 0.0f || radius > 100.0f) continue;

        auto& hb = out.entries[out.count++];
        hb.bone = bone;
        hb.radius = radius;
        std::memcpy(&hb.mins, entry + 0x18, sizeof(Vec3));
        std::memcpy(&hb.maxs, entry + 0x24, sizeof(Vec3));
        if (out.count >= 20) break;
    }
}

struct ReaderAimSpot {
    float pitch = 0.0f;
    float yaw = 0.0f;
    std::string zone_name;
    uint8_t grenade_type = 0;
    glm::vec3 zone_mins{0.0f};
    glm::vec3 zone_maxs{0.0f};
    glm::vec3 predicted_det_pos{0.0f};
};

class GameReader {
private:
    uint32_t m_bPawnIsAlive = 0;
    uint32_t m_hObserverPawn = 0;
    uint32_t m_hPlayerPawn = 0;
    uint32_t m_pObserverServices = 0;
    uint32_t m_hObserverTarget = 0;
    uint32_t m_iObserverMode = 0;

    // New schemas for advanced triggerbot
    uint32_t m_nNumBullets = 0;
    uint32_t m_nSpreadSeed = 0;
    uint32_t m_nRecoilSeed = 0;
    uint32_t m_flRecoilIndex = 0;
    uint32_t m_nTickBase = 0;
    uint32_t m_nSimulationTick = 0;
    uint32_t m_iPing = 0;
    std::thread reader_thread;
    std::mutex packet_mutex;
    std::atomic<bool> is_running{false};
    
    // Double buffer for thread-safe access
    ShmPacket latest_packet;
    bool has_new_data = false;
    // Bullet Impacts native client impacts: host-only sidecar published with the packet.
    impacts::SnapshotPtr latest_impacts;
    impacts::SnapshotPtr pending_impacts;
    std::optional<impacts::Layout> impact_layout;
    uintptr_t impact_layout_client_base = 0;
    
    // Process attachment state
    std::atomic<int> target_pid{-1};
    std::atomic<uintptr_t> client_base{0};
    std::atomic<uintptr_t> engine_base{0};
    std::atomic<uintptr_t> schemasystem_base{0};
    std::atomic<uintptr_t> tier0_base{0};
    std::atomic<uintptr_t> cvar_interface{0};
    std::string tier0_path;
    uintptr_t tier0_VEngineCvar_rva = 0;
    bool has_logged_waiting = false;
    
    // Offsets resolved from offsets.json
    uintptr_t dwEntityList = 0;
    uintptr_t dwViewMatrix = 0;
    uintptr_t dwLocalPlayerPawn = 0;
    uintptr_t dwLocalPlayerController = 0;
    uintptr_t dwGlobalVars = 0;
    uintptr_t dwViewAngles = 0;
    uintptr_t dwCSGOInput = 0;
    uintptr_t dwWindowWidth = 0;
    uintptr_t dwWindowHeight = 0;
    uintptr_t dwSensitivity = 0;
    uintptr_t dwSensitivity_sensitivity = 0;
    // Match session (SetPlayerReady lea r12 target). Accept state at +0x90:
    // 0 NotReady, 1 Pending (match found), 2 Accepted, 3 DoneAccepted.
    uintptr_t dwMatchSessionPointer = 0;
    uint32_t dwMatchSessionPointer_state = 0x90;
    // Optional Bullet Impacts native-impact signatures; any zero leaves impacts unavailable.
    uintptr_t dwBulletServicesVTable = 0;
    uintptr_t dwBulletImpactGlobals = 0;
    uintptr_t dwBulletServices_impacts = 0;

    // Schema field offsets resolved from output json files
    uint32_t m_pGameSceneNode = 0;
    uint32_t m_modelState = 0;
    uint32_t m_ModelName = 0;
    uint32_t m_lifeState = 0;
    uint32_t m_iTeamNum = 0;
    uint32_t m_iHealth = 0;
    uint32_t m_pWeaponServices = 0;
    uint32_t m_hActiveWeapon = 0;
    uint32_t m_hMyWeapons = 0;
    uint32_t m_bPinPulled = 0;
    uint32_t m_flThrowStrength = 0;
    uint32_t m_vecAbsVelocity = 0;
    uint32_t m_vInitialPosition = 0;
    uint32_t m_vInitialVelocity = 0;
    uint32_t m_flSimulationTime = 0;
    uint32_t m_vSmokeDetonationPos = 0;
    uint32_t m_vecAbsOrigin = 0;
    uint32_t m_angAbsRotation = 0;
    uint32_t m_flAbsScale = 0;
    uint32_t m_pEntity = 0;
    uint32_t m_bFireIsBurning = 0;
    uint32_t m_fireCount = 0;
    uint32_t m_firePositions = 0;
    uint32_t m_hOwnerEntity = 0;
    uint32_t m_hThrower = 0;
    uint32_t m_bIsIncGrenade = 0;
    uint32_t m_nExplodeEffectTickBegin = 0;
    uint32_t m_bDidSmokeEffect = 0;
    uint32_t m_nSmokeEffectTickBegin = 0;
    // C_NetworkUtlVectorBase (0x18 bytes). The frame decoder state follows it:
    // frame index at +0x100, RGBA buffer pointer at +0x110 (0x1310 on build
    // 2000927; m_nVoxelFrameDataSize sits at +0x18, 0x1328 on that build).
    uint32_t m_VoxelFrameData = 0;
    uint32_t m_nVoxelFrameDataSize = 0;
    uint32_t m_bSmokeVolumeDataReceived = 0;
    uint32_t m_bSmokeEffectSpawned = 0;
    uint32_t m_nDecoyShotTick = 0;
    
    // Econ & Player specific schemas
    uint32_t m_AttributeManager = 0;
    uint32_t m_Item = 0;
    uint32_t m_iItemDefinitionIndex = 0;
    uint32_t m_vOldOrigin = 0;
    uint32_t m_pItemServices = 0;
    uint32_t m_bHasDefuser = 0;
    uint32_t m_angEyeAngles = 0;
    uint32_t m_hController = 0;
    uint32_t m_vecViewOffset = 0;
    uint32_t m_hPawn = 0;

    // Planted C4 schemas
    uint32_t m_bHasExploded = 0;
    uint32_t m_bBombDefused = 0;
    uint32_t m_flC4Blow = 0;
    uint32_t m_bBeingDefused = 0;
    uint32_t m_flDefuseCountDown = 0;
    
    uint32_t m_bPawnHasDefuser = 0;
    uint32_t m_szCrosshairCodes = 0;
    uint32_t m_ArmorValue = 0;
    uint32_t m_iszPlayerName = 0;
    uint32_t m_bIsScoped = 0;
    uint32_t m_flFOVSensitivityAdjust = 0;
    uint32_t m_bInReload = 0;
    uint32_t m_bPrevHelmet = 0;
    uint32_t m_nSubclassID = 0;
    uint32_t m_szName = 0;
    uint32_t m_nDamage = 0;
    uint32_t m_flRangeModifier = 0;
    uint32_t m_flPenetration = 0;
    uint32_t m_flArmorRatio = 0;
    uint32_t m_flHeadshotMultiplier = 0;
    uint32_t m_iClip1 = 0;
    uint32_t m_iMaxClip1 = 0;
    uint32_t m_pActionTrackingServices = 0;
    uint32_t m_flTotalRoundDamageDealt = 0;
    uint32_t m_iNumRoundKills = 0;
    uint32_t m_iNumRoundKillsHeadshots = 0;
    uint32_t m_iIDEntIndex = 0;
    uint32_t m_pAimPunchServices = 0;
    uint32_t m_pBulletServices = 0;
    uint32_t m_predictableBaseAngle = 0;
    uint32_t m_bIsWalking = 0;
    uint32_t m_flEmitSoundTime = 0;
    uint32_t m_iShotsFired = 0;

    // Web radar / match-state schemas
    uintptr_t dwGameRules = 0;
    uint32_t m_iTeamScore = 0;          // C_Team::m_iScore
    uint32_t m_bFreezePeriod = 0;       // C_CSGameRules::m_bFreezePeriod
    uint32_t m_bWarmupPeriod = 0;       // C_CSGameRules::m_bWarmupPeriod
    uint32_t m_iRoundTime = 0;          // C_CSGameRules::m_iRoundTime
    uint32_t m_totalRoundsPlayed = 0;   // C_CSGameRules::m_totalRoundsPlayed
    uint32_t m_gamePhase = 0;           // C_CSGameRules::m_gamePhase
    uint32_t m_fRoundStartTime = 0;     // C_CSGameRules::m_fRoundStartTime
    uint32_t m_bStartedArming = 0; // C_C4
    uint32_t m_flFlashOverlayAlpha = 0;
    uint32_t m_weaponMode = 0;
    uint32_t m_fAccuracyPenalty = 0;
    uint32_t m_flSpread = 0;
    uint32_t m_flMaxSpeed = 0;
    uint32_t m_flInaccuracyMove = 0;
    uint32_t m_flInaccuracyJumpInitial = 0;
    uint32_t m_fFlags = 0;
    uint32_t m_MoveType = 0;
    uint32_t m_vecVelocity = 0;

    // Thread-safe map for model names to optimize memory accesses
    std::unordered_map<uintptr_t, std::string> model_name_cache;
    const struct OverlayConfig* cfg = nullptr;
    double last_debug_print_time = 0.0;

    // Hitbox geometry (bone remap, radii, bounds) is a property of the model, not of
    // the tick, so it is read once per CModel and served from here afterwards. The
    // (cmodel, count, remap_count) triple is re-validated on every call to keep
    // recycled model addresses from serving a stale set.
    struct HitboxCacheEntry {
        uintptr_t cmodel = 0;
        int count = 0;
        int remap_count = 0;
        TargetHitboxData data{};
    };
    static constexpr size_t kHitboxCacheSize = 16;
    mutable HitboxCacheEntry hitbox_cache_[kHitboxCacheSize];
    mutable size_t hitbox_cache_cursor_ = 0;
    std::string last_seen_map_name;

    void clear_hitbox_cache() const
    {
        for (size_t i = 0; i < kHitboxCacheSize; ++i) {
            hitbox_cache_[i] = HitboxCacheEntry{};
        }
        hitbox_cache_cursor_ = 0;
    }

    // Atomic configuration shadows to prevent data races
    std::atomic<float> atomic_local_ms{2.0f};
    std::atomic<float> atomic_players_ms{2.0f};
    std::atomic<float> atomic_entities_ms{16.67f};
    std::atomic<bool> atomic_debug_reader{false};
    std::atomic<bool> atomic_debug_visibility{false};
    std::atomic<bool> atomic_client_impacts{false};

    // Loop performance statistics tracking (thread-safe)
    std::atomic<float> m_local_tps{0.0f};
    std::atomic<float> m_players_tps{0.0f};
    std::atomic<float> m_entities_tps{0.0f};
    std::atomic<float> m_local_avg_time{0.0f};
    std::atomic<float> m_players_avg_time{0.0f};
    std::atomic<float> m_entities_avg_time{0.0f};
    
    // Internal helper methods
    bool attach_to_process();
    bool load_offsets_and_schemas();
    void reader_loop();
    void reset_process_state();
    ProcessMemoryView make_process_memory_view() const;
    uintptr_t resolve_cvar_interface_locked();
    
    // Low-level memory reading utilities
    bool read_raw(uintptr_t address, void* dest, size_t size) const;
    std::string read_str(uintptr_t address, size_t max_len = 128) const;
    
    template <typename T>
    T read_mem(uintptr_t address) const {
        T value{};
        read_raw(address, &value, sizeof(T));
        return value;
    }
 
    std::string resolve_model_name(uintptr_t pawn_addr, uintptr_t scene_node);
    void update_bone_cache(PlayerData& player_pkt, uintptr_t scene_node);
    uintptr_t get_entity(int index) const;
    uintptr_t get_entity_from_handle(uint32_t handle) const;
    uintptr_t get_entity_cached(uintptr_t entity_list_base, int index) const;
    uintptr_t get_entity_from_handle_cached(uintptr_t entity_list_base, uint32_t handle) const;
    TargetHitboxData query_hitboxes(uintptr_t target_pawn_addr) const;
 
public:
    GameReader();
    ~GameReader();
 
    bool initialize(const struct OverlayConfig* config = nullptr);
    void shutdown();
    
    bool fetch_latest(ShmPacket& out_packet);
    bool read_view_matrix(float* out_matrix);
    // The newest native client-impact snapshot, or null while unavailable.
    impacts::SnapshotPtr fetch_client_impacts();

    // Getters for loop performance metrics
    float get_local_tps() const { return m_local_tps.load(); }
    float get_players_tps() const { return m_players_tps.load(); }
    float get_entities_tps() const { return m_entities_tps.load(); }
    float get_local_avg_time_ms() const { return m_local_avg_time.load(); }
    float get_players_avg_time_ms() const { return m_players_avg_time.load(); }
    float get_entities_avg_time_ms() const { return m_entities_avg_time.load(); }

    bool is_attached() const { return target_pid.load() > 0; }
    uintptr_t get_client_base() const { return client_base.load(); }
    uintptr_t get_dw_match_session_pointer() const { return dwMatchSessionPointer; }
    uint32_t get_dw_match_session_state_offset() const { return dwMatchSessionPointer_state; }
    bool try_read(uintptr_t address, void* dest, size_t size) const { return read_raw(address, dest, size); }
    uintptr_t get_cvar_interface_address() const { return cvar_interface.load(); }
    ConvarLookupResult lookup_convar(const char* name) const;

    void set_active_aim_spots(const std::vector<ReaderAimSpot>& spots) {
        std::lock_guard<std::mutex> lock(aim_spots_mutex_);
        active_aim_spots_ = spots;
    }

private:
    std::mutex aim_spots_mutex_;
    std::vector<ReaderAimSpot> active_aim_spots_;
};
