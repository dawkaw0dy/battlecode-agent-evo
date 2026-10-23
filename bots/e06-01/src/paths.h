#pragma once
#include <cambc.h>
#include <cstdint>

// Fuel-aware path distances. Terrain never changes, so a distance field towards a goal set (a camp
// group's spawn tiles, the tiles next to an altar, our spawn) is computed once and cached; afterwards
// a lookup or a walking step costs a handful of host calls instead of a whole-map search per turn.
namespace paths {

// Fuel one whole-map field costs (~11 per tile) plus margin.
std::uint64_t field_cost();

// Cached static distance from `from` to the nearest goal tile: -1 when not cached yet, a large value
// when unreachable. Never computes.
int cached_distance(const cambc::bitboard& goals, cambc::vec2 from);

// Static distance, computing and caching the field when fuel allows, else a Chebyshev estimate.
int distance(const cambc::bitboard& goals, cambc::vec2 from);

// Make sure the field for `goals` is cached, computing it only when `reserve` fuel would be left.
// True when it is cached afterwards.
bool ensure(const cambc::bitboard& goals, std::uint64_t reserve);

// Best free step (among `free_steps`) towards `goals` along the cached field: strictly closer than
// `from` (or as close, with `allow_equal`, to side-step a blocker). False when the field is not cached or
// no free step qualifies.
bool step_towards(const cambc::bitboard& goals, cambc::vec2 from, const cambc::bitboard& free_steps,
    cambc::vec2& step, bool allow_equal = false);

} // namespace paths
