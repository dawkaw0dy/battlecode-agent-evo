#pragma once
// Draft-time map reading. The draft bot measures the map and writes a Plan into draft memory;
// every champion reads the same Plan, so the whole team follows one strategy.
#include <cambc.h>
#include <cstdint>
#include <vector>

constexpr std::uint8_t plan_magic = 0xF7;
constexpr int max_groups = 200;

// Team-wide strategy chosen from the map's numbers.
enum plan_mode : std::uint8_t {
    mode_spread = 0,   // many reachable groups: each champion farms its own arc of the map
    mode_pack = 1,     // few or hard groups: champions converge on shared targets
    mode_skirmish = 2, // small map close to the enemy: farm together and fight on sight
};

// Per-group flag bits.
constexpr std::uint8_t group_reachable = 1;
constexpr std::uint8_t group_big = 2; // contains a golem or monolith

struct Plan {
    std::uint8_t magic = 0;
    std::uint8_t mode = mode_pack;
    std::uint8_t team_size = 0;
    std::uint8_t ngroups = 0;
    std::uint8_t bank_gems = 40;   // carry threshold before walking to an altar
    std::uint8_t chase = 0;        // 1: chase visible enemies when we have the numbers
    std::uint8_t golem_dps = 255;  // team dps (x10) needed before golem groups are hunted
    std::uint8_t altar_far = 0;    // spawn-to-altar distance class
    std::uint16_t checksum = 0;    // sum of group gem values, to detect a mismatched layout
    std::uint16_t spawn_gap = 0;   // walking distance between the two spawns
    std::uint8_t flags[max_groups] = {};
    std::uint8_t zone[max_groups] = {}; // angular rank around our spawn (0..255), for spread zones
};
static_assert(sizeof(Plan) <= 1024);

// Static per-group facts derived from the map layout.
struct GroupFacts {
    std::vector<cambc::vec2> tiles;
    int gems = 0, gold = 0;
    float hp = 0;        // total health of every member
    float monster_hp = 0;
    float str = 0;       // summed strength of attacking members
    int range = 0;       // largest attack range
    bool golem = false, monolith = false;
    cambc::vec2 center{};
};

GroupFacts group_facts(const cambc::map::camp_group& group);
// Walls for static pathing: terrain, altars and the enemy spawn.
cambc::bitboard static_walls();
// Fill `plan` from the map. Safe to call in the draft (full map visible).
void build_plan(Plan& plan);
// Read the plan from draft memory, or rebuild it locally if missing.
const Plan& the_plan();
