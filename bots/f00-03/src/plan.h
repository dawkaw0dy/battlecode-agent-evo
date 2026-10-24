#pragma once
// Map analysis shared by the draft and the champions: camp groups, distances and the region split.
#include <cambc.h>
#include <cstdint>
#include <vector>

struct GroupData {
    std::vector<cambc::vec2> tiles;  // camp spawn tiles
    cambc::vec2 center{};
    int gems = 0, gold = 0, respawn = 0;
    float hp = 0, dps = 0;            // total monster health and strength
    int range = 0;                    // largest monster attack range
    bool has_monolith = false;
    int dist_ally = 9999, dist_enemy = 9999;
    int dist_ally_dash = 9999;            // with one-tile holes jumpable
};

// Draft memory layout.
namespace layout {
constexpr int magic = 0;      // 'S'
constexpr int team_size = 1;
constexpr int picks = 2;      // up to 5 champion_draft values, slot order
constexpr int group_count = 7;
constexpr int regions = 8;    // one byte per group: owning slot, 255 = unassigned
constexpr int max_groups = 200;
constexpr int dash_map = 8 + max_groups; // 1 when the draft chose dash champions
}

const cambc::bitboard& static_walls_ally();
// Static walls with the one-tile holes a two-step dash can jump removed.
const cambc::bitboard& dash_walls_ally();
std::vector<GroupData> analyse_groups();
// Region owner per group (255 = none), for `slots` champions ordered strongest first.
std::vector<std::uint8_t> split_regions(const std::vector<GroupData>& groups, int slots, bool dash_map);
// True when most of the camp value is only reachable by dashing over holes.
bool is_dash_map(const std::vector<GroupData>& groups);
std::vector<cambc::champion_draft> preferred_picks(int team_size, bool dash_map = false);
