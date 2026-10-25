#pragma once
// The fuel subsystem: everything that turns engine state into cheap local reads (vision snapshot, map
// analysis, camp knowledge, the camp-block board) and every distance field and route search.
//
// Costs measured with fuel_remaining() (release build): a host call ~35-80 fuel, a new bitboard ~340-530
// (an allocation; an in-place |= is ~70-190), camp_group::state() ~4,000 (it rebuilds the vision board),
// map::camp_groups() ~30,000, a host BFS ~3 fuel per tile, exporting a field ~2 per tile and memcpy ~1 per
// byte, while a local loop costs ~10-50 per element. So: read engine buffers in place (raw_bit / raw_dist),
// keep fields in engine storage, refill scratch boards with *_into calls, and never BFS locally.
#include <cambc.h>
#include <cstdint>
#include <exception>
#include <vector>
#include "common.h"

// A turn that ran out of fuel resumes next round on stale data: act only in the round we started.
extern std::uint32_t g_round;
inline bool live() { return cambc::round_number() == g_round; }
inline bool fuel(std::uint64_t need) { return cambc::fuel_remaining() > need; }
void warn(const char* where, const std::exception& e);

// A champion's live state without exceptions: false when it is not in our vision (a thrown cambc::error
// costs ~3,000 fuel; position() of an ally out of sight throws).
inline bool peek(const cambc::champion& c, cambc::detail::champion_info& out) {
    const auto h = static_cast<std::uint32_t>(static_cast<cambc::handle_champion_t>(c));
    std::int32_t alive = 0;
    if (cambc::detail::sdk_champion_is_alive(h, &alive) != 0 || alive == 0) return false;
    return cambc::detail::sdk_champion(h, &out) == 0;
}

// Vision snapshot, refreshed lazily after anything that changes what we see.
struct View {
    cambc::bitboard vis, camps, champs, tele, foes;
    Bits lvis, lcamps, lchamps, ltele;
    std::vector<vec2> allies;                       // allied positions, including ones hidden in bushes
    std::vector<cambc::champion_type> ally_types;   // ... and who stands there
    vec2 self{};
    bool ok = false;
};
const View& view();
void invalidate();

const Bits& walls_local();  // terrain we cannot walk on, enemy spawn included
const Bits& hard_local();   // what a dash cannot cross
bool free_tile(vec2 p);     // walkable and not occupied as far as we can see (our own tile counts as free)

// out = a & ~b without temporaries; out may be a or b.
void andnot_into(const cambc::bitboard& a, const cambc::bitboard& b, cambc::bitboard& out);
