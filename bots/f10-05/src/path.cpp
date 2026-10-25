// The pathing subsystem: go(), step_on(), try_dash(), go_home(), recall_or_walk() (walk_static() stays off).
//
// Rules it is built on (GameRules.md + patches 100-102): one step per turn to any of the 8 neighbours;
// walls, holes, altars and the enemy spawn are never walkable; an occupied tile is not a destination
// (a known one throws, an unseen champion in a bush makes the move return false, reveals the tile and keeps
// the move allowance, so another tile can be tried the same turn); dashes cover up to 2 steps along a legal
// path and may cross holes, not walls; camps never move, sleep until hit and then hit the nearest champion
// within their range (wolf 3, elk 4, golem 4, monolith 5) every round.
//
// Design:
// - Routes come from the host's distance field (compute_to from the goals, stopping at our tile), never a
//   local BFS. Walls are walk_walls(); living camps (seen or remembered) are walls.
// - Champions are walls only within kUnitRadius of us: far ones will have moved by the time we get there,
//   and routing around them sent the parent on long detours (or into its walk-only fallback). Goals that
//   any visible champion stands on are dropped.
// - Safety: awake camps near us mark their reach as danger. When the plain route enters it within
//   kTraceSteps, a second route that treats danger as walls is taken if it costs at most kSafeSlack more steps.
//   Goal tiles are never danger (an approach into a camp's reach is the point of farming it).
// - The step: the lowest field distance; ties go to tiles out of danger, then to tiles with more ways on
//   (a single blocked successor does not stop us). A move that returns false (hidden unit) is retried once
//   on the next candidate.
// - Dash: when the route still has >= kDashFrom steps, land on the reachable ring-2 tile with the lowest
//   distance (crossing holes counts), avoiding danger when a safe landing is as good.
#include <cambc.h>
#include "bot.h"
#include "fuel.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <utility>

namespace {

constexpr int kUnitRadius = 2;   // champions closer than this (Chebyshev) block the route
constexpr int kDangerScan = 10;  // awake camps this close to us shape the route
constexpr int kTraceSteps = 10;  // how far along the plain route we look for danger
constexpr int kSafeSlack = 4;    // extra steps accepted to stay out of awake camps' reach
constexpr int kDashFrom = 3;     // dash for travel only with at least this many steps left

struct PathScratch {
    cambc::bitboard blocked, sblocked, goals, goals2, here, seen, sseen, wseen, danger;
    cambc::distance_field f, sf, w;
    bool danger_on = false;
};
PathScratch* g_ps = nullptr;
PathScratch& ps() {
    if (!g_ps) g_ps = new PathScratch;
    return *g_ps;
}

int fdist(const cambc::distance_field& f, vec2 p) { return inside(p) ? raw_dist(f.ptr(), p) : kFar; }
bool in_danger(vec2 p) {
    const auto& s = ps();
    return s.danger_on && inside(p) && raw_bit(s.danger.ptr(), p);
}

}  // namespace

bool Bot::try_dash(const cambc::distance_field& f, const cambc::bitboard& seen) {
    auto& me = cambc::self();
    if (cast_ || !dasher_ || !me.can_cast() || !fuel(12000) || !dash_for_travel()) return false;
    const vec2 pos = me.position();
    if (!bit(seen, pos)) return false;
    const int here = fdist(f, pos);
    for (const auto& ab : me.abilities()) {
        if (!ab.is_dash() || !ab.is_ready()) continue;
        // (distance, danger, tile): lowest distance first, safe landings before unsafe ones.
        std::array<std::pair<int, vec2>, 16> cand;
        size_t n = 0;
        const auto& v = view();
        for (int dx = -2; dx <= 2; ++dx)
            for (int dy = -2; dy <= 2; ++dy) {
                if (std::max(std::abs(dx), std::abs(dy)) != 2) continue;
                const vec2 t{pos.x + dx, pos.y + dy};
                if (!free_tile(t) || !bit(seen, t)) continue;
                const int d = fdist(f, t);
                if (d > here - 2) continue;
                // A legal path needs a middle tile that is not a wall or altar (holes may be jumped) and
                // holds no unit.
                bool path = false;
                for (auto s : kDirs) {
                    const vec2 m{pos.x + s.x, pos.y + s.y};
                    if (!inside(m) || cheb(m, t) != 1) continue;
                    if (hard_local().at(m) || v.lchamps.at(m) || v.lcamps.at(m)) continue;
                    path = true;
                    break;
                }
                if (path) cand[n++] = {d * 2 + (in_danger(t) ? 1 : 0), t};
            }
        std::sort(cand.begin(), cand.begin() + n, [](auto a, auto b) { return a.first < b.first; });
        for (size_t i = 0; i < n && i < 3; ++i) {
            if (!live()) return false;
            try {
                const bool ok = ab.use(cand[i].second);
                cast_ = true;
                invalidate();
                if (!ok) move_failed_ = true;
                return ok;
            } catch (const cambc::error&) {
                // The engine's dash path rules can be stricter than ours: try the next landing.
            }
        }
        return false;
    }
    return false;
}

// One step (and a dash when it gains two) along the shortest safe-enough route around known camps and
// nearby units. When only living camps close the way, remember their group in plug_.
bool Bot::go(const cambc::bitboard& goals_in, bool dash) {
    auto& me = cambc::self();
    if (moved_ || move_failed_ || !me.can_move() || !live()) return false;
    if (!fuel(25000)) return false;
    const vec2 pos = me.position();
    if (!inside(pos)) return false;
    const auto& v = view();
    auto& s = ps();
    s.danger_on = false;
    s.blocked.clear();
    s.blocked |= walk_walls();
    s.blocked |= camp_block_;
    s.blocked |= v.camps;
    s.blocked |= v.tele;
    // Goals: walkable, not a camp, not a reserved teleport landing, not under any visible champion.
    andnot_into(goals_in, s.blocked, s.goals);
    andnot_into(s.goals, v.champs, s.goals);
    raw_clear(s.goals.ptr(), pos);  // our own tile is in champs: re-add it below if it was a goal
    if (bit(goals_in, pos) && !bit(s.blocked, pos)) return false;  // already there
    if (s.goals.is_empty()) return false;
    // Champions near us are walls; far ones will have moved.
    {
        auto* w = s.blocked.ptr();
        for (int dy = -kUnitRadius; dy <= kUnitRadius; ++dy)
            for (int dx = -kUnitRadius; dx <= kUnitRadius; ++dx) {
                const vec2 t{pos.x + dx, pos.y + dy};
                if ((dx | dy) == 0 || !inside(t) || !v.lchamps.at(t)) continue;
                raw_set(w, t);
            }
    }
    raw_clear(s.blocked.ptr(), pos);
    s.here.clear();
    raw_set(s.here.ptr(), pos);
    s.seen.clear();
    s.f.compute_to(s.blocked, s.goals, s.here, s.seen);
    if (bit(s.seen, pos)) {
        // Awake camps about: does the route walk into their reach?
        const int plain_d = fdist(s.f, pos);
        if (!awake_.is_empty() && plain_d > 2 && fuel(50000)) {
            bool any = false;
            s.danger.clear();
            auto* dw = s.danger.ptr();
            for (auto p : awake_.positions()) {
                if (cheb(p, pos) > kDangerScan) continue;
                const int gi = group_at_[static_cast<size_t>(tidx(p))];
                const int ci = camp_at_[static_cast<size_t>(tidx(p))];
                if (gi < 0 || ci < 0) continue;
                const auto spec = camp_spec(g_[gi].kinds[static_cast<size_t>(ci)]);
                if (spec.str <= 0) continue;
                const int r = spec.range;
                for (int y = std::max(0, p.y - r); y <= std::min(map_h() - 1, p.y + r); ++y)
                    for (int x = std::max(0, p.x - r); x <= std::min(map_w() - 1, p.x + r); ++x) raw_set(dw, vec2{x, y});
                any = true;
            }
            if (any && !raw_bit(dw, pos)) {
                andnot_into(s.danger, s.goals, s.danger);
                s.danger_on = true;
                // Trace the plain route.
                bool hits = false;
                vec2 at = pos;
                for (int step = 0; step < kTraceSteps && !hits; ++step) {
                    const int here = fdist(s.f, at);
                    if (here == 0) break;
                    vec2 next = at;
                    int nd = here;
                    for (auto d : kDirs) {
                        const vec2 t{at.x + d.x, at.y + d.y};
                        if (!bit(s.seen, t) || bit(s.blocked, t)) continue;
                        const int fd = raw_dist(s.f.ptr(), t);
                        if (fd < nd) { nd = fd; next = t; }
                    }
                    if (next == at) break;
                    if (raw_bit(dw, next)) hits = true;
                    at = next;
                }
                if (hits) {
                    s.sblocked.clear();
                    s.sblocked |= s.blocked;
                    s.sblocked |= s.danger;
                    raw_clear(s.sblocked.ptr(), pos);
                    s.sseen.clear();
                    s.sf.compute_to(s.sblocked, s.goals, s.here, s.sseen, static_cast<std::uint16_t>(plain_d + kSafeSlack));
                    if (bit(s.sseen, pos) && fdist(s.sf, pos) <= plain_d + kSafeSlack)
                        return step_on(s.sf, s.sseen, s.goals, s.sblocked, dash);
                }
            }
        }
        return step_on(s.f, s.seen, s.goals, s.blocked, dash);
    }
    // Camps (or units) close the way: find what plugs the walk-only route.
    andnot_into(goals_in, walk_walls(), s.goals2);
    s.wseen.clear();
    s.w.compute_to(walk_walls(), s.goals2, s.here, s.wseen);
    if (!bit(s.wseen, pos)) return false;
    // Follow that route until the first camp on it (field read in place).
    vec2 at = pos;
    for (int steps = 0; steps < 60; ++steps) {
        vec2 next = at;
        int nd = fdist(s.w, at);
        for (auto d : kDirs) {
            const vec2 t{at.x + d.x, at.y + d.y};
            if (!bit(s.wseen, t) || walls_local().at(t)) continue;
            const int fd = raw_dist(s.w.ptr(), t);
            if (fd < nd) { nd = fd; next = t; }
        }
        if (next == at) break;
        const int gi = group_at_[static_cast<size_t>(tidx(next))];
        if (gi >= 0 && bit(camp_block_, next)) {
            if (plug_ != gi) { plug_ = gi; plug_since_ = static_cast<int>(cambc::round_number()); plug_altar_ = false; }
            return false;
        }
        at = next;
    }
    // Units in the gap: walk the plain route and wait there for them to move.
    return step_on(s.w, s.wseen, s.goals, s.blocked, dash);
}

// Dash and/or walk one step down field f (reached tiles in seen) towards goals.
bool Bot::step_on(const cambc::distance_field& f, const cambc::bitboard& seen, const cambc::bitboard& goals,
                  const cambc::bitboard& blocked, bool dash) {
    auto& me = cambc::self();
    const vec2 pos = me.position();
    if (!live() || !inside(pos)) return false;
    if (dash && fdist(f, pos) >= kDashFrom && try_dash(f, seen)) {
        moved_ = true;
        if (move_failed_ || !me.can_move()) return true;
    }
    const vec2 from = me.position();
    if (!inside(from)) return moved_;
    if (bit(goals, from)) return true;
    const int here = bit(seen, from) ? fdist(f, from) : kFar;
    // Candidates that make progress, best first: distance, then out of danger, then more ways on.
    struct Cand { int key; vec2 t; };
    std::array<Cand, 8> cand;
    size_t n = 0;
    int best_d = kFar;
    for (auto d : kDirs) {
        const vec2 t{from.x + d.x, from.y + d.y};
        if (!free_tile(t) || !bit(seen, t) || bit(blocked, t)) continue;
        const int fd = raw_dist(f.ptr(), t);
        if (fd >= here) continue;
        cand[n++] = {fd * 64 + (in_danger(t) ? 16 : 0), t};
        best_d = std::min(best_d, fd);
    }
    if (n == 0 || !live()) return moved_;
    if (n > 1) {
        for (size_t i = 0; i < n; ++i) {
            if (cand[i].key / 64 != best_d || best_d == 0) continue;
            int ways = 0;
            for (auto d : kDirs) {
                const vec2 u{cand[i].t.x + d.x, cand[i].t.y + d.y};
                if (u == from || !bit(seen, u) || bit(blocked, u)) continue;
                if (raw_dist(f.ptr(), u) < best_d && free_tile(u)) ++ways;
            }
            cand[i].key += 8 - std::min(8, ways);
        }
        std::sort(cand.begin(), cand.begin() + n, [](const Cand& a, const Cand& b) { return a.key < b.key; });
    }
    vec2 failed{-1, -1};
    for (size_t i = 0; i < n && i < 2; ++i) {
        const vec2 t = cand[i].t;
        if (!live() || !me.can_move()) break;
        if (i > 0 && (cheb(t, failed) == 0 || !free_tile(t))) continue;
        moved_ = true;
        bool ok = false;
        try {
            ok = me.move_absolute(t);
        } catch (const cambc::error& e) {
            warn("move", e);
            invalidate();
            break;
        }
        invalidate();
        if (ok) return true;
        failed = t;  // a hidden champion stands there: it is in view now, try the next tile once
    }
    move_failed_ = true;
    return false;
}

void Bot::go_home() { go(cambc::map::spawn_region(team::ally), true); }

// Walk home when it is close, otherwise recall. True when the turn is used.
bool Bot::recall_or_walk(int walk_limit) {
    auto& me = cambc::self();
    if (spawn_local_.at(pos_)) return false;
    const int d = home_.at(pos_);
    if (d <= walk_limit) {
        go_home();
        return true;
    }
    if (moved_ || incoming_ > 0 || !live()) return moved_;
    try { me.recall(); } catch (const cambc::error& e) { warn("recall", e); }
    return true;
}

// The parent's step down a group's cached field instead of a route search: off (go() is cheap enough and
// sees units, danger and dashes). Kept as the same call so farm() needs no change.
bool Bot::walk_static(int, int) { return false; }
