#pragma once
// Map reading shared by the draft and the champions: camp groups, distances, the plan.
#include "common.h"

struct GroupInfo {
    std::vector<vec2> tiles;              // camp spawn tiles
    std::vector<cambc::camp_type> kinds;
    vec2 center{};                        // member tile closest to the centroid
    int gems = 0, gold = 0, respawn = 0;
    float hp = 0, str = 0;                // total health; total strength of attacking members
    int range = 0;                        // largest member attack range
    bool boss = false;                    // monolith or golem inside
    bool monolith = false;
    int d_ally = kFar, d_enemy = kFar;    // walking distance from each spawn
};

// Terrain we can never stand on: walls, holes, altars and the enemy spawn.
cambc::bitboard static_walls();
// static_walls plus every camp spawn tile.
cambc::bitboard static_walls_with_camps();
std::vector<GroupInfo> analyse_groups();
// Walking distances from our and the enemy spawn into d_ally / d_enemy (one field each).
void measure_spawn_distances(std::vector<GroupInfo>& g);

// Plan kinds written by the draft.
enum Plan : std::uint8_t { kFarm = 0, kBoss = 1, kRush = 2 };

// Draft memory layout.
namespace dm {
constexpr int magic = 0;      // 'M'
constexpr int plan = 1;       // Plan
constexpr int npicks = 2;     // number of picks
constexpr int picks = 3;      // picks[5] (champion_draft ids)
constexpr int ngroups = 8;    // number of groups (<= 250)
constexpr int boss = 9;       // boss group to raid (255 = none)
constexpr int raid_turns = 10;// boss: minimum team strength (tank turns) before raiding
constexpr int nsquads = 11;   // number of squads
constexpr int squad = 12;     // squad[6]: squad id of each body slot
constexpr int owner = 32;     // one byte per group: owner squad, 255 = shared
}

// Shared memory layout (8 bytes).
namespace sm {
constexpr int target = 0;     // bytes 0..3: target group + 1 of each squad (written by its leader)
constexpr int raid = 6;       // boss raid flag: group + 1
constexpr int alarm = 7;      // round/8 of the latest enemy sighting near our spawn
}

// Champion type of a draft pick (Cassi and Issac: Cassi).
inline cambc::champion_type type_of_pick(cambc::champion_draft d) {
    const int v = static_cast<int>(d);
    return static_cast<cambc::champion_type>(v >= 4 ? v + 1 : v);
}
// Body slot of a champion type given the pick order (-1 if absent).
inline int slot_of(cambc::champion_type t, const std::vector<cambc::champion_draft>& picks) {
    int s = 0;
    for (auto p : picks) {
        if (p == cambc::champion_draft::cassi_and_issac) {
            if (t == cambc::champion_type::cassi) return s;
            if (t == cambc::champion_type::issac) return s + 1;
            s += 2;
        } else {
            if (type_of_pick(p) == t) return s;
            s += 1;
        }
    }
    return -1;
}
