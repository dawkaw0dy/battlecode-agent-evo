#pragma once
// Shared helpers and constants for the map-reader bot.
#include <cambc.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <vector>

using cambc::bitboard;
using cambc::team;
using cambc::vec2;

constexpr int kFar = 9999;
#ifndef EVO_TRACE
#define EVO_TRACE 0
#endif
constexpr bool kTrace = EVO_TRACE;
constexpr int kTraceSlot = 0, kTraceFrom = 1, kTraceTo = 260;
constexpr std::uint16_t kUnreached = 65535;

inline int cheb(vec2 a, vec2 b) {
    return std::max(std::abs(a.x - b.x), std::abs(a.y - b.y));
}

inline bool in_bounds(vec2 p) {
    return p.x >= 0 && p.y >= 0 && p.x < cambc::map::width() && p.y < cambc::map::height();
}

// Set every in-bounds tile within Chebyshev radius r of c (no engine edge cases).
inline void mark_square(bitboard& b, vec2 c, int r) {
    const int w = cambc::map::width(), h = cambc::map::height();
    if (c.x - r >= 0 && c.y - r >= 0 && c.x + r < w && c.y + r < h) {
        b.draw_chebyshev(c, static_cast<float>(r));
        return;
    }
    for (int y = std::max(0, c.y - r); y <= std::min(h - 1, c.y + r); ++y)
        for (int x = std::max(0, c.x - r); x <= std::min(w - 1, c.x + r); ++x) b.set({x, y});
}

// Static camp numbers (rules + patch notes).
struct CampSpec {
    float hp, str;
    int range, gems, gold;
};
inline CampSpec camp_spec(cambc::camp_type t) {
    switch (t) {
        case cambc::camp_type::wolf: return {1350, 20, 3, 4, 40};
        case cambc::camp_type::elk: return {2400, 60, 4, 15, 90};
        case cambc::camp_type::gem_cluster: return {120, 0, 0, 7, 0};
        case cambc::camp_type::gold_cluster: return {120, 0, 0, 0, 20};
        case cambc::camp_type::monolith: return {20000, 200, 5, 200, 300};
        case cambc::camp_type::golem: return {5000, 120, 4, 36, 160};
    }
    return {1000, 50, 3, 0, 0};
}

// Draft memory layout (1024 bytes, read-only for champions).
namespace dm {
constexpr int magic = 0;     // 'M'
constexpr int npicks = 1;    // number of draft picks
constexpr int picks = 2;     // 5 bytes: champion_draft values in pick order
constexpr int mode = 8;      // plan mode
constexpr int bank = 9;      // gem bank threshold
constexpr int raid = 10;     // boss raid group index + 1 (0 = none)
constexpr int raid_pow = 11; // strength (per champion, /4) required before raiding
constexpr int flags = 12;    // misc plan flags
constexpr int squads = 20;   // 5 bytes: squad id per pick slot
constexpr int ngroups = 16;  // number of groups recorded
constexpr int group = 32;    // one byte per group: plan class
constexpr int kMaxGroups = 240;
}  // namespace dm

// Plan modes chosen at draft time.
enum PlanMode : std::uint8_t { kModeFarm = 0, kModeCluster = 1, kModeBoss = 2, kModeSmall = 3 };
// Group plan classes.
enum GroupClass : std::uint8_t { kGHome = 0, kGContested = 1, kGEnemy = 2, kGBoss = 3, kGUnreachable = 4 };

// Shared memory layout (8 bytes).
namespace sm {
constexpr int claims = 0;       // bytes 0..4: claimed group + 1 per body slot (0 = none)
constexpr int alert = 5;        // bytes 5..6: last enemy sighting (x, y)
constexpr int alert_round = 7;  // byte 7: round of that sighting (mod 256)
constexpr int kSlots = 5;
constexpr std::uint8_t kDead = 0xFE;
}  // namespace sm
