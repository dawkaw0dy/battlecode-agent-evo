#pragma once
// Map analysis shared by the draft and the champions: camp groups, distances, the region split and
// the per-map plan (who raids which boss, which group blocks the altars).
#include <cambc.h>
#include <array>
#include <cstdint>
#include <vector>

struct GroupData {
    std::vector<cambc::vec2> tiles;  // camp spawn tiles
    cambc::vec2 center{};
    int gems = 0, gold = 0, respawn = 0;
    float hp = 0, dps = 0;            // total monster health and strength
    float max_str = 0;                // strongest single monster
    int range = 0;                    // largest monster attack range
    bool has_monolith = false, has_golem = false;
    int dist_ally = 9999, dist_enemy = 9999;
    int dist_ally_dash = 9999;            // with one-tile holes jumpable
    bool boss() const { return has_monolith || has_golem; }
};

// Draft memory layout.
namespace layout {
constexpr int magic = 0;      // 'R'
constexpr int team_size = 1;
constexpr int picks = 2;      // up to 5 champion_draft values, slot order
constexpr int group_count = 7;
constexpr int regions = 8;    // one byte per group: owning slot, 255 = unassigned
constexpr int max_groups = 200;
constexpr int dash_map = 8 + max_groups; // 1 when the draft chose dash champions
constexpr int mode = dash_map + 1;       // Plan::Mode
constexpr int raiders = mode + 1;        // bit per slot
constexpr int raid_count = raiders + 1;
constexpr int raid_list = raid_count + 1;  // up to 8 group indices, best first
constexpr int blocker = raid_list + 8;     // group that blocks every altar tile, 255 = none
constexpr int need = blocker + 1;          // raiders that must gather before a boss is engaged
}

// What the whole team does on this map, decided once in the draft.
struct Plan {
    enum Mode : std::uint8_t { split = 0, mixed = 1, raid = 2 } mode = split;
    std::uint8_t raiders = 0;           // bit per slot
    std::vector<int> raid_list;         // boss groups, best first
    int blocker = -1;                   // group that must die before any tribute
    int need = 2;
    bool dash_map = false;
    std::vector<cambc::champion_draft> picks;
    std::vector<std::uint8_t> owner;    // region per group
};

const cambc::bitboard& static_walls_ally();
// Static walls with the one-tile holes a two-step dash can jump removed.
const cambc::bitboard& dash_walls_ally();
std::vector<GroupData> analyse_groups();
// Region owner per group (255 = none), for `slots` champions ordered strongest first.
std::vector<std::uint8_t> split_regions(const std::vector<GroupData>& groups, int slots, bool dash_map);
// True when most of the camp value is only reachable by dashing over holes.
bool is_dash_map(const std::vector<GroupData>& groups);
std::vector<cambc::champion_draft> preferred_picks(int team_size, bool dash_map = false, bool raid = false);
Plan make_plan(const std::vector<GroupData>& groups, int team_size);
void write_plan(const Plan& plan, int group_count);
// False when the draft memory holds no plan for this map.
bool read_plan(Plan& plan, int group_count);
