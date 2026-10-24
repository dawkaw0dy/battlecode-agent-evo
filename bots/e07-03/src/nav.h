#pragma once
#include <cambc.h>
#include <cstdint>

// Fuel-aware path fields. Terrain never changes, so a whole-map field towards a goal set (a camp group's
// spawn tiles, an altar's approach tiles, our spawn) is computed once and kept for the rest of the game;
// later turns read it (~10 fuel per tile read) instead of recomputing (~3.3 fuel per map tile).
// Measured: a field costs ~14k fuel on 61x71; copying it to a local array cost more than the reads it saves.
namespace nav {

struct Field {
    cambc::bitboard goals;       // the goal set (cache key)
    std::uint32_t count = 0;     // goal tiles  } cheap key prefilter: comparing whole boards is a host
    cambc::vec2 first{-1, -1};   // first goal tile } call per entry, so only matching keys are compared
    cambc::distance_field field; // terrain-only distances to the goals
    cambc::bitboard visited;     // tiles the search reached
    int used = 0;                // last round it was read (eviction)
    bool pinned = false;         // indexed by a slot (camp group / altar): never evicted

    // Path length from tile to the goals, or -1 when unreachable.
    int dist(cambc::vec2 tile) const;
};

// Impassable terrain: walls, holes, altars and the enemy spawn region.
const cambc::bitboard& terrain();
// Fuel for one whole-map field: measured on our own computes (~14k on 61x71), plus margin.
std::uint64_t field_cost();
// The cached field towards goals, or nullptr.
const Field* cached(const cambc::bitboard& goals);
// The cached field, computed now if fuel_remaining() keeps `reserve` after a whole-map field; else nullptr.
const Field* ensure(const cambc::bitboard& goals, std::uint64_t reserve);
// Field for a fixed goal set known by index (camp group i, altar i): O(1) after the first lookup.
const Field* ensure_slot(int slot, const cambc::bitboard& goals, std::uint64_t reserve);
// Like ensure, but a goal set seen for the first time is only remembered: one-off goals (a chase, a
// shrinking set of living camps) keep using the caller's cheaper early-stopping search.
const Field* ensure_repeated(const cambc::bitboard& goals, std::uint64_t reserve);

} // namespace nav
