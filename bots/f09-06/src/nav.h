#pragma once
// Walking: precomputed terrain fields for long trips, a fresh host search with units near the goal.
#include "world.h"

struct Nav {
    bool blocked_this_turn = false;  // a move returned false: no more moves this turn
    int last_dist = kFar;            // distance to the goal before the last step
    int fail = 0;                    // why the last dynamic step failed (debug)

    // Step towards the goals. f, when given, is a terrain field towards (roughly) the same goals.
    bool step(const cambc::bitboard& goals, const Field* f = nullptr);
    // Step to a neighbour that keeps out of the threats' reach, preferring the way home.
    bool flee(const std::vector<vec2>& threats, const std::vector<int>& reach);
    bool move_to(vec2 p);            // single step to an adjacent free tile
    void new_turn() { blocked_this_turn = false; }
    bool free(vec2 p) const;         // free and not a tile where a hidden unit blocked us lately

private:
    Field dyn_;
    vec2 bad_tile_{-1, -1};
    int bad_round_ = -1000;
    int dynamic_until_ = -1;          // after a failed descent, search with units for a while
    bool dynamic_step(const cambc::bitboard& goals);
    bool descend(const Field& f, int here);
};

extern Nav N;

void add_square(cambc::bitboard& b, vec2 c, int r);  // bounds-checked Chebyshev square
inline std::uint64_t fuel_left() { return cambc::fuel_remaining(); }
