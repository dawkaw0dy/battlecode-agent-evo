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
    int bank_d = kFar;                // walk from the group to the nearest altar (draft table only)
};

// Static walls for our walking: terrain plus the enemy spawn.
const cambc::bitboard& walk_walls();
// With home_altar: also the walk from each group (and from our spawn) to the nearest altar.
std::vector<GroupInfo> analyse_map(int* home_altar = nullptr);

// Champion picks in slot order for a team of `size`.
std::vector<cambc::champion_draft> pick_order(int size);
// Slot of a champion type in the pick order (-1 if not there).
int slot_of(cambc::champion_type t, int size);

// Draft memory layout.
namespace dm {
constexpr int magic = 0;      // 'E'
constexpr int size = 1;       // team size the plan was made for
constexpr int ngroups = 2;    // number of groups (<= 250)
constexpr int table = 3;      // 'T' when the group table follows the home slots
constexpr int home = 4;       // one byte per group: home slot, 255 = shared
// Table: home_altar (2), then per group n (+128: gems, gold, respawn follow; otherwise they are the sums
// and maximum of the camp kinds' values), d_ally, d_enemy, bank_d; per camp x, y, kind.
constexpr int kGroupBytes = 7, kExplicitBytes = 5, kCampBytes = 3, kTableHead = 2;
}
// The draft's map analysis as a compact table (the champions' own scan costs several rounds of fuel on
// big maps). write_table returns false when it does not fit; read_table rebuilds the GroupInfo list.
bool write_table(const std::vector<GroupInfo>& groups, int home_altar, std::byte* mem, size_t off, size_t cap);
bool read_table(const std::byte* mem, size_t off, size_t cap, size_t n, std::vector<GroupInfo>& out, int& home_altar);
// Tiles in submit range of an altar and the walking field to them.
void altar_field(cambc::bitboard& spots, cambc::distance_field& f, cambc::bitboard& seen);
// Home region per group for `slots` regions (255 = nobody's in particular); weight[k] is the
// share of the value region k should hold (its champion count), empty for equal shares.
std::vector<std::uint8_t> assign_homes(const std::vector<GroupInfo>& groups, int slots,
                                       const std::vector<double>& weight = {});
// Opening squads: champions share home regions in pairs, so first contacts are not one on one.
constexpr bool kPairs = false;
inline int region_count(int size) { return kPairs && size >= 3 ? (size + 1) / 2 : size; }
inline int region_of(int slot, int size) { return kPairs && size >= 3 && slot >= 0 ? slot / 2 : slot; }
std::vector<double> region_weights(int size);
