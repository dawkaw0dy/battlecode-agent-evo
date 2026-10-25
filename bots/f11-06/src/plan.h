#pragma once
// The map plan: chosen in the draft from map measurements, read back by every champion.
#include "mapinfo.h"
#include <span>

struct Plan {
    std::vector<cambc::champion_draft> picks;
    std::uint8_t mode = kModeFarm;
    int bank = 40;          // carry this many gems before a dedicated bank trip
    int raid = -1;          // boss group to raid together (-1 = none)
    int raid_pow = 0;       // mean strength per champion needed before the raid
    std::uint8_t flags = 0;
    std::vector<std::uint8_t> cls;  // GroupClass per group
};

namespace planflag {
constexpr std::uint8_t kAggro = 1;     // small map: punish enemies near our camps
constexpr std::uint8_t kLocks = 2;     // carry altar locks
}  // namespace planflag

Plan make_plan(const MapInfo& m, int team_size);
void write_plan(const Plan& p, const MapInfo& m, std::span<std::byte> mem);
// Read the plan back from draft memory (champions). Falls back to make_plan on a bad magic.
Plan read_plan(const MapInfo& m);
