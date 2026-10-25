#pragma once
// Map analysis shared by the draft and the champions.
#include "common.h"

struct GroupInfo {
    std::vector<vec2> tiles;          // camp spawn tiles
    std::vector<cambc::camp_type> kinds;
    vec2 center{};
    int gems = 0, gold = 0, respawn = 0;
    float hp = 0, str = 0;            // total monster health and strength (clusters excluded from str)
    int range = 0;                    // largest monster attack range
    bool boss = false;                // monolith or golem inside
    int d_ally = kFar, d_enemy = kFar;  // walking distance from each spawn
};

// Static walls for our walking: terrain plus the enemy spawn.
const cambc::bitboard& walk_walls();
std::vector<GroupInfo> analyse_map();
std::vector<GroupInfo> analyse_map(const std::vector<cambc::map::camp_group>& groups);  // reuses a camp_groups() read

// Champion picks in slot order for a team of `size`.
std::vector<cambc::champion_draft> pick_order(int size);
// Slot of a champion type in the pick order (-1 if not there).
int slot_of(cambc::champion_type t, int size);

// Draft memory layout.
namespace dm {
constexpr int magic = 0;      // 'E'
constexpr int size = 1;       // team size the plan was made for
constexpr int ngroups = 2;    // number of groups (<= 250)
constexpr int home = 4;       // one byte per group: home slot, 255 = shared
constexpr int pmagic = 256;   // 'P' when the map plan below is present
constexpr int pflags = 257;   // MapPlan::flags
constexpr int pgroup = 260;   // one byte per group (<= 250): MapPlan::group bits
}
// Map plan made at draft time (draft memory) so every champion plays the same map the same way.
struct MapPlan {
    enum : std::uint8_t { contest = 1, boss = 2, b_spells = 4, runner = 8 };  // contest: most gem income in groups both sides reach alike;
                                                     // boss: much of it held by bosses (raided by the whole team);
                                                     // b_spells: a large map, played with parent B's spells;
                                                     // runner: few altars, so the last pick runs locks to them
    enum : std::uint8_t { g_contested = 1 };  // per group: both spawns about equally far
    std::uint8_t flags = 0;
    std::vector<std::uint8_t> group;
};
MapPlan make_plan(const std::vector<GroupInfo>& groups, int slots);
// Home slot per group for `slots` champions (255 = nobody's in particular).
std::vector<std::uint8_t> assign_homes(const std::vector<GroupInfo>& groups, int slots);
