#pragma once
// Map plan: made once at draft time from the whole map (size, team size, camp groups, altars, spawns),
// written to draft memory and read back by every champion, so the whole team plays the map the same way.
// The rest of the bot calls the same functions as before (analyse_map, pick_order, slot_of, assign_homes);
// map_plan() adds the map-level switches.
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

// Champion picks in slot order for a team of `size` (the plan's picks once a plan exists).
std::vector<cambc::champion_draft> pick_order(int size);
// Slot of a champion type in the pick order (-1 if not there).
int slot_of(cambc::champion_type t, int size);

// Draft memory layout. Bytes 0-253 keep the parent's layout (read by fuel.cpp); the plan lives at 512+.
namespace dm {
constexpr int magic = 0;      // 'E'
constexpr int size = 1;       // team size the plan was made for
constexpr int ngroups = 2;    // number of groups (<= 250)
constexpr int home = 4;       // one byte per group: home slot, 255 = shared
constexpr int pmagic = 512;   // 'Q' when the map plan is present
constexpr int pflags = 513;   // MapPlan::flags
constexpr int pdetour = 514;  // MapPlan::lock_detour
constexpr int pclass = 515;   // MapPlan::cls
constexpr int ppicks = 516;   // 5 bytes: champion_draft of each slot (255 = none)
constexpr int prunner = 521;  // MapPlan::runner_slot
}

// Map-level switches every champion follows.
struct MapPlan {
    enum : std::uint8_t { runner = 1 };  // the last pick runs altar locks to far altars
    enum Class : std::uint8_t { normal = 0, small = 1, large = 2 };
    std::uint8_t flags = 0;
    int lock_detour = 8;               // Chebyshev detour to drop a lock on an altar we pass
    std::uint8_t cls = normal;
    int runner_slot = -1;              // who runs the locks (with the runner flag)
    std::vector<cambc::champion_draft> picks;  // slot order
    bool has(std::uint8_t f) const { return (flags & f) != 0; }
};
// Make the plan from the map (draft and fallback); `slots` = picks this map allows.
MapPlan make_plan(const std::vector<GroupInfo>& groups, int slots);
// The plan in force: draft memory when present, else made from the map on first use.
const MapPlan& map_plan();
void set_plan(const MapPlan& p);    // the draft's own copy
void write_plan(std::span<std::byte> mem, const MapPlan& p);

// Home slot per group for `slots` champions (255 = nobody's in particular).
std::vector<std::uint8_t> assign_homes(const std::vector<GroupInfo>& groups, int slots);
