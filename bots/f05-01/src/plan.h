#pragma once
// Map analysis shared by the draft and the champions.
#include "common.h"
#include <array>

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
constexpr int open_magic = 256;  // 'O' when the opening plan below is written
constexpr int open_g = 257;   // per slot: first group, 255 = none
constexpr int open_n = 262;   // per slot: champions that group's plan counts on
}
// Draft-time opening: every slot's first group and the team it clears it with, chosen jointly (the
// champions' own choice is greedy, so two slots each planned half a raid and waited for help that never came).
void plan_opening(const std::vector<GroupInfo>& groups, const std::vector<cambc::champion_draft>& picks,
                  std::array<std::uint8_t, 5>& first, std::array<std::uint8_t, 5>& team);
// Home slot per group for `slots` champions (255 = nobody's in particular).
std::vector<std::uint8_t> assign_homes(const std::vector<GroupInfo>& groups, int slots);
