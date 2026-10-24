#pragma once
// Map analysis shared by the draft and the champions: camp groups, squads and the sweep loop.
#include <cambc.h>
#include <cstdint>
#include <vector>

struct GroupInfo {
    std::vector<cambc::vec2> tiles;  // camp spawn tiles
    cambc::vec2 center{};
    int gems = 0, gold = 0, respawn = 0;
    float fight_hp = 0;      // health of monsters that hit back
    float soft_hp = 0;       // health of clusters
    float str = 0;           // summed monster strength
    int range = 0;
    bool boss = false;       // monolith or golem inside
    int d_ally = 9999, d_enemy = 9999, d_dash = 9999;  // walking distance from each spawn
};

// Draft memory layout.
namespace layout {
constexpr int magic = 0, team_size = 1, dash_map = 2, picks = 3, group_count = 8, owner = 9;
constexpr int max_groups = 1000;
}

// Shared memory layout (8 bytes).
namespace shm {
constexpr int target = 0;        // five bytes: target group + 1 per slot
constexpr int enemy_round = 5;   // round & 255, then position
constexpr int enemy_x = 6, enemy_y = 7;
}

const cambc::bitboard& walk_walls();  // terrain we cannot walk on, plus the enemy spawn
const cambc::bitboard& dash_walls();  // the same, with one-tile straight holes passable
std::vector<GroupInfo> analyse_groups();
bool is_dash_map(const std::vector<GroupInfo>& groups);
std::vector<cambc::champion_draft> preferred_picks(int team_size, bool dash_map);
// Owner slot per group (255 = nobody: boss, unreachable or deep in enemy land).
std::vector<std::uint8_t> split_groups(const std::vector<GroupInfo>& groups, int team_size, bool dash_map);
float group_value(const GroupInfo& g);
