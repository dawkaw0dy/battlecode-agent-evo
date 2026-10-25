#pragma once
// Draft memory layout: the map plan the draft writes and every champion reads.
#include <cambc.h>
#include <cstdint>

namespace plan {

enum kind : std::uint8_t { farm = 0, boss = 1, aggro = 2 };

constexpr int kMagic = 0;      // 'M'
constexpr int kKind = 1;       // plan kind
constexpr int kBank = 2;       // gems carried before walking to an altar
constexpr int kTeam = 3;       // champions per team
constexpr int kGroups = 4;     // number of camp groups
constexpr int kSlots = 8;      // 12 bytes: champion_type -> slot (255 = not ours)
constexpr int kGroupBase = 32; // 4 bytes per group
constexpr int kMaxGroups = 240;

// Per-group bytes.
constexpr int gDistUs = 0;   // walking distance from our spawn / 2 (255 = unreachable)
constexpr int gDistThem = 1; // walking distance from the enemy spawn / 2
constexpr int gFlags = 2;
constexpr int gParty = 3;    // champions needed to clear it without dying

enum flag : std::uint8_t { f_boss = 1, f_monster = 2, f_contested = 4, f_skip = 8 };

// Base stats per camp_type (patched values).
inline float camp_hp(cambc::camp_type t) {
    switch (t) {
    case cambc::camp_type::wolf: return 1350;
    case cambc::camp_type::elk: return 2400;
    case cambc::camp_type::gem_cluster: return 120;
    case cambc::camp_type::gold_cluster: return 120;
    case cambc::camp_type::monolith: return 20000;
    case cambc::camp_type::golem: return 5000;
    }
    return 1000;
}
inline float camp_dps(cambc::camp_type t) {
    switch (t) {
    case cambc::camp_type::wolf: return 20;
    case cambc::camp_type::elk: return 60;
    case cambc::camp_type::monolith: return 200;
    case cambc::camp_type::golem: return 120;
    default: return 0;
    }
}
inline bool is_boss(cambc::camp_type t) {
    return t == cambc::camp_type::monolith || t == cambc::camp_type::golem;
}

} // namespace plan
