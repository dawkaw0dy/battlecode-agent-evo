// Walking with the host's distance search.
#include "nav.h"
#include <iostream>

Nav N;

void add_square(cambc::bitboard& b, vec2 c, int r) {
    for (int dy = -r; dy <= r; ++dy)
        for (int dx = -r; dx <= r; ++dx) {
            const vec2 q{c.x + dx, c.y + dy};
            if (inside(q)) b.set(q);
        }
}

void Nav::set_danger(const std::vector<vec2>& at, const std::vector<int>& reach) {
    danger_.clear();
    danger_on = false;
    for (std::size_t i = 0; i < at.size(); ++i) { add_square(danger_, at[i], reach[i]); danger_on = true; }
    // Already inside: let the flee logic get us out instead of freezing.
    if (danger_on && bit(danger_, cambc::self().position())) danger_on = false;
}

bool Nav::free(vec2 p) const {
    if (!W.free(p)) return false;
    if (danger_on && raw_bit(danger_.ptr(), p)) return false;
    return !(p == bad_tile_ && W.round - bad_round_ <= 3);
}

bool Nav::move_to(vec2 p) {
    auto& me = cambc::self();
    if (blocked_this_turn || !me.can_move() || !W.free(p)) return false;
    if (cheb(p, me.position()) != 1) return false;
    bool ok = false;
    try {
        ok = me.move_absolute(p);
    } catch (const std::exception& e) {
        debug_block({ std::cout << "  move error " << e.what() << "\n"; });
        ok = false;
    }
    if (!ok) {
        // A hidden unit stands there (often an ally in a bush): avoid the tile for a few rounds.
        blocked_this_turn = true;
        bad_tile_ = p;
        bad_round_ = W.round;
        return false;
    }
    W.occupied.set(p);
    return true;
}

bool Nav::descend(const Field& f, int here) {
    const vec2 pos = cambc::self().position();
    vec2 best = pos;
    int best_d = here;
    for (auto d : kDirs) {
        const vec2 q = pos + d;
        if (!free(q)) continue;
        const int v = f.at(q);
        if (v < best_d) { best_d = v; best = q; }
    }
    if (best == pos) return false;
    return move_to(best);
}

bool Nav::dynamic_step(const cambc::bitboard& goals) {
    const vec2 pos = cambc::self().position();
    fail = 0;
    cambc::bitboard walls = W.alive_camps;
    walls &= ~W.visible;
    walls |= W.blocked;
    walls |= W.occupied;
    if (W.round - bad_round_ <= 3 && inside(bad_tile_)) walls.set(bad_tile_);
    if (inside(pos)) walls.set(pos, false);
    cambc::bitboard roots = goals;
    roots &= ~walls;
    const bool dynamic = !roots.is_empty();
    if (!dynamic) {
        roots = goals;
        roots &= ~W.blocked;
        if (roots.is_empty()) { fail = 1; return false; }
    }
    if (fuel_left() < 30000) { fail = 2; return false; }
    cambc::bitboard dest;
    dest.set(pos);
    cambc::bitboard seen;
    if (!dyn_.f) dyn_.f = std::make_unique<cambc::distance_field>();
    dyn_.f->compute_to(dynamic ? walls : W.blocked, roots, dest, seen);
    int here = dyn_.at(pos);
    if (here >= kFar && dynamic && fuel_left() > 40000) {
        // Units close the way: route on terrain alone and side-step blockers.
        roots = goals;
        roots &= ~W.blocked;
        if (roots.is_empty()) { fail = 1; return false; }
        dyn_.f->compute_to(W.blocked, roots, dest, seen);
        here = dyn_.at(pos);
    }
    if (here >= kFar) { fail = 3; return false; }
    last_dist = here;
    if (descend(dyn_, here)) return true;
    if (blocked_this_turn) return false;
    // All better tiles are taken: a sideways step that keeps the distance.
    for (auto d : kDirs) {
        const vec2 q = pos + d;
        if (free(q) && dyn_.at(q) == here) return move_to(q);
    }
    fail = 4;
    return false;
}

bool Nav::step(const cambc::bitboard& goals, const Field* f) {
    auto& me = cambc::self();
    if (blocked_this_turn || !me.can_move()) return false;
    const vec2 pos = me.position();
    if (bit(goals, pos)) { last_dist = 0; return false; }
    if (f && W.round > dynamic_until_) {
        const int here = f->at(pos);
        if (here < kFar && here > 3) {
            last_dist = here;
            if (descend(*f, here)) return true;
            if (blocked_this_turn) return false;
        }
    }
    // Once the search with units takes over, stay with it: the terrain field may point through camps.
    dynamic_until_ = W.round + 8;
    return dynamic_step(goals);
}

bool Nav::flee(const std::vector<vec2>& threats, const std::vector<int>& reach) {
    auto& me = cambc::self();
    if (blocked_this_turn || !me.can_move()) return false;
    const vec2 pos = me.position();
    auto score = [&](vec2 q) {
        // Head home unless that walks into someone's reach.
        float s = -2.0f * static_cast<float>(std::min(W.from_spawn.at(q), 300));
        // Dead ends trap us: count the ways out of the tile.
        int exits = 0;
        for (auto e : kDirs) if (W.passable(q + e)) ++exits;
        if (exits <= 3) s -= 25.0f * static_cast<float>(4 - exits);
        for (std::size_t i = 0; i < threats.size(); ++i) {
            const int d = cheb(q, threats[i]);
            const int r = reach[i];
            if (d <= r) s -= 60.0f + 15.0f * static_cast<float>(r - d);
            s += static_cast<float>(std::min(d, 8));
        }
        return s;
    };
    vec2 best = pos;
    float best_s = score(pos);
    for (auto d : kDirs) {
        const vec2 q = pos + d;
        if (!W.free(q) || (q == bad_tile_ && W.round - bad_round_ <= 3)) continue;
        const float s = score(q);
        if (s > best_s) { best_s = s; best = q; }
    }
    debug_block({
        if (best == pos) {
            std::cout << "  FLEE-STUCK r" << W.round << " at " << pos.x << "," << pos.y << " s " << best_s << ":";
            for (auto d : kDirs) {
                const vec2 q = pos + d;
                std::cout << " [" << q.x << "," << q.y << " free" << W.free(q) << " s" << (W.free(q) ? score(q) : 0.0f) << "]";
            }
            std::cout << " threats";
            for (std::size_t i = 0; i < threats.size(); ++i) std::cout << " " << threats[i].x << "," << threats[i].y << "/" << reach[i];
            std::cout << "\n";
        }
    });
    if (best == pos) return false;
    return move_to(best);
}
