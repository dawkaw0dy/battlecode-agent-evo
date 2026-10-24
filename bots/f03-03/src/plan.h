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
    int d_dash = kFar;                // from our spawn when one-tile holes can be dashed over
};

// Static walls for our walking: terrain plus the enemy spawn.
const cambc::bitboard& walk_walls();
// walk_walls() without the holes a dash crosses (standable floor on both sides, straight across).
cambc::bitboard dash_walls();
std::vector<GroupInfo> analyse_map();

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
constexpr int has_groups = 3; // 'G' when the group table below is filled in
constexpr int groups = 256;   // group table (pack_groups), up to the end of the buffer
constexpr int end = 1024;
}
// The group table lets the champions skip the map scan on their first turn (it overran the fuel).
size_t pack_groups(const std::vector<GroupInfo>& groups, std::byte* out, size_t cap);  // 0 if it does not fit
bool unpack_groups(const std::byte* in, size_t len, size_t n, std::vector<GroupInfo>& out);
// Home slot per group for `slots` champions (255 = nobody's in particular).
std::vector<std::uint8_t> assign_homes(const std::vector<GroupInfo>& groups, int slots);
