#pragma once
// The map plan: chosen in the draft from map measurements, read back by every champion.
#include "mapinfo.h"
#include <span>

struct Plan {
    std::vector<cambc::champion_draft> picks;
    std::uint8_t mode = kModeFarm;
    int bank = 40;          // carry this many gems before a dedicated bank trip
    int raid = -1;          // boss group to raid together (-1 = none)
    int raid_pow = 0;       // strength a champion needs before joining the raid
    int raid2 = -1, raid2_pow = 0;
    int raid_round = 2000;  // raids start by this round whatever the strength
    std::uint8_t flags = 0;
    std::array<std::uint8_t, 5> squad{0, 1, 2, 3, 4};   // squad id per pick slot
    std::vector<std::uint8_t> cls;  // GroupClass per group
    std::vector<std::int8_t> owner; // pick slot whose sector holds the group (-1 = shared)
};

namespace planflag {
constexpr std::uint8_t kAggro = 1;     // small map: punish enemies near our camps
constexpr std::uint8_t kLocks = 2;     // carry altar locks
}  // namespace planflag

Plan make_plan(const MapInfo& m, int team_size);
void write_plan(const Plan& p, const MapInfo& m, std::span<std::byte> mem);
// Read the plan back from draft memory (champions). Falls back to make_plan on a bad magic.
Plan read_plan(const MapInfo& m);
