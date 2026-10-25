// The fuel subsystem (see fuel.h): vision snapshot, map analysis, camp knowledge, distance fields, movement.
#include <cambc.h>
#include "bot.h"
#include "fuel.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <utility>

std::uint32_t g_round = 0;

void warn(const char* where, const std::exception& e) {
    static int count = 0;
    if (++count <= 20) std::cout << "warn: " << where << ": " << e.what() << '\n';
}

namespace {
// Scratch boards are allocated once, on first use (map layout must be known).
struct Scratch {
    cambc::bitboard tmp, blocked, goals, goals2, here, seen, wseen;
    cambc::distance_field f, w;
};
Scratch* g_s = nullptr;
Scratch& scratch() {
    if (!g_s) g_s = new Scratch;
    return *g_s;
}

View* g_view = nullptr;
std::vector<cambc::champion> g_others;  // allied handles other than ours, read once
std::vector<cambc::champion_type> g_other_types;
bool g_others_read = false;

Bits g_walls_local, g_hard_local;
}  // namespace

void andnot_into(const cambc::bitboard& a, const cambc::bitboard& b, cambc::bitboard& out) {
    auto& t = scratch().tmp;  // a & b, taken before out (which may be a or b) is written
    t.clear();
    t |= a;
    t &= b;
    if (&out != &a) {
        out.clear();
        out |= a;
    }
    out ^= t;
}

void invalidate() {
    if (g_view) g_view->ok = false;
}

const View& view() {
    if (!g_view) {
        g_view = new View;
        g_view->lvis.bind(g_view->vis);
        g_view->lcamps.bind(g_view->camps);
        g_view->lchamps.bind(g_view->champs);
        g_view->ltele.bind(g_view->tele);
    }
    View& v = *g_view;
    if (v.ok) return v;
    if (!g_others_read) {
        g_others_read = true;
        const int me = static_cast<int>(static_cast<cambc::handle_champion_t>(cambc::self()));
        for (const auto& a : cambc::champions(team::ally))
            if (static_cast<int>(static_cast<cambc::handle_champion_t>(a)) != me) {
                g_others.push_back(a);
                cambc::champion_type t = cambc::champion_type::vesper_vale;
                try { t = a.champion_name(); } catch (const cambc::error&) {}
                g_other_types.push_back(t);
            }
    }
    cambc::vision::visible_tiles_into(v.vis);
    cambc::vision::camp_tiles_into(v.camps);
    cambc::vision::champion_tiles_into(team::enemy, v.foes);
    cambc::vision::champion_tiles_into(team::ally, v.champs);
    v.champs |= v.foes;
    cambc::vision::teleport_tiles_into(v.tele);
    v.self = cambc::self().position();
    v.allies.clear();
    v.ally_types.clear();
    for (size_t i = 0; i < g_others.size(); ++i) {
        cambc::detail::champion_info info{};
        if (!peek(g_others[i], info)) continue;
        const vec2 p{info.x, info.y};
        if (!inside(p)) continue;
        v.allies.push_back(p);
        v.ally_types.push_back(g_other_types[i]);
        raw_set(v.champs.ptr(), p);
    }
    v.ok = true;
    return v;
}

const Bits& walls_local() { return g_walls_local; }
const Bits& hard_local() { return g_hard_local; }

bool free_tile(vec2 p) {
    if (!inside(p)) return false;
    const auto& v = view();
    if (p == v.self) return true;
    return !g_walls_local.at(p) && !v.lcamps.at(p) && !v.lchamps.at(p) && !v.ltele.at(p);
}

// ---------------------------------------------------------------- setup

void Bot::initialise() {
    init_ = true;
    init_dims();
    g_walls_local.bind(walk_walls());
    {
        static const cambc::bitboard hard = cambc::map::walls() | cambc::map::altars();
        g_hard_local.bind(hard);
    }
    auto& me = cambc::self();
    size_ = static_cast<int>(cambc::champions(team::ally).size());
    // Groups and the plan from draft memory (no camp_groups() / analyse_map() unless it has none).
    read_plan(g_, size_, home_slot_, open_);
    const size_t ng = g_.size();
    k_.assign(ng, Known{});
    for (size_t i = 0; i < ng; ++i)
        for (auto kind : g_[i].kinds) k_[i].hp.push_back(camp_spec(kind).hp);
    gfield_.clear();
    gfield_.resize(ng);
    group_at_.assign(static_cast<size_t>(map_w() * map_h()), -1);
    camp_at_.assign(static_cast<size_t>(map_w() * map_h()), -1);
    gtiles_.reserve(ng);
    for (size_t i = 0; i < ng; ++i) {
        int r = 0;
        for (auto t : g_[i].tiles) r = std::max(r, cheb(t, g_[i].center));
        reach_.push_back(r);
        gtiles_.emplace_back();  // cleared board
        auto* w = gtiles_.back().ptr();
        for (size_t c = 0; c < g_[i].tiles.size(); ++c) {
            const vec2 t = g_[i].tiles[c];
            raw_set(w, t);
            group_at_[static_cast<size_t>(tidx(t))] = static_cast<std::int16_t>(i);
            camp_at_[static_cast<size_t>(tidx(t))] = static_cast<std::int16_t>(c);
        }
    }
    // Cassi and Issac count as one pick.
    slot_ = slot_of(me.champion_name(), size_);
    for (const auto& ab : me.abilities())
        if (ab.is_dash()) dasher_ = true;
    my_range_ = me.attack_range();
    build_threats();
    spawn_local_.bind(cambc::map::spawn_region(team::ally));
    {
        const int self_h = static_cast<int>(static_cast<cambc::handle_champion_t>(me));
        for (const auto& a : cambc::champions(team::ally))
            if (static_cast<int>(static_cast<cambc::handle_champion_t>(a)) != self_h) others_.push_back(a);
    }

    auto& s = scratch();
    home_.compute(walk_walls(), cambc::map::spawn_region(team::ally), s.seen);
    // Tiles from which an altar can take a tribute, written straight into the board.
    altar_spots_.clear();
    const int ar = cambc::constants::submit_gems_range;
    altars_ = cambc::map::altars().positions();
    enemy_lock_.assign(altars_.size(), -1);
    lock_seen_.assign(altars_.size(), -1000);
    for (auto a : altars_)
        for (int dy = -ar; dy <= ar; ++dy)
            for (int dx = -ar; dx <= ar; ++dx) {
                const vec2 t{a.x + dx, a.y + dy};
                if (inside(t)) raw_set(altar_spots_.ptr(), t);
            }
    andnot_into(altar_spots_, walk_walls(), s.goals);
    altar_spots_.clear();
    altar_spots_ |= s.goals;
    altar_.compute(walk_walls(), altar_spots_, s.seen);
    for (auto t : cambc::map::spawn_region(team::ally).positions()) home_altar_ = std::min(home_altar_, altar_.at(t));
    bank_d_.assign(ng, kFar);
    for (size_t i = 0; i < ng; ++i)
        for (auto t : g_[i].tiles) bank_d_[i] = std::min(bank_d_[i], altar_.at(t));

    for (size_t i = 0; i < ng; ++i) fill_order_.push_back(static_cast<int>(i));
    std::stable_sort(fill_order_.begin(), fill_order_.end(), [&](int a, int b) {
        const bool ma = home_slot_[a] == slot_, mb = home_slot_[b] == slot_;
        if (ma != mb) return ma;
        return g_[a].d_ally < g_[b].d_ally;
    });
}

// Walking fields to each group's tiles, one host BFS each (no export), while fuel lasts.
void Bot::fill_fields(std::uint64_t reserve) {
    for (int i : fill_order_) {
        if (!gfield_[i].empty()) continue;
        if (!fuel(reserve) || !live()) return;
        gfield_[i].compute(walk_walls(), gtiles_[i], scratch().seen);
    }
}

int Bot::eta(int group) const {
    if (!gfield_[group].empty()) {
        const int d = gfield_[group].at(pos_);
        return d >= kFar ? kFar : d;
    }
    if (g_[group].d_ally >= kFar) return kFar;
    return cheb(pos_, g_[group].center) * 5 / 4 + 2;
}

bool Bot::group_awake(int group) const {
    for (auto t : g_[group].tiles)
        if (bit(awake_, t)) return true;
    return false;
}

// ---------------------------------------------------------------- knowledge

void Bot::observe() {
    const int now = static_cast<int>(cambc::round_number());
    for (auto& k : k_)
        if (k.state == Known::respawning && now >= k.ready) { k.state = Known::unknown; block_dirty_ = true; }
    const auto& v = view();
    for (size_t i = 0; i < g_.size(); ++i) {
        if (cheb(g_[i].center, pos_) > 8 + reach_[i]) continue;
        auto& k = k_[i];
        bool seen = false, living = false;
        for (size_t c = 0; c < g_[i].tiles.size(); ++c) {
            const vec2 p = g_[i].tiles[c];
            if (!v.lvis.at(p)) continue;
            seen = true;
            if (v.lcamps.at(p)) {
                living = true;
                if (k.hp[c] <= 0) { k.hp[c] = camp_spec(g_[i].kinds[c]).hp; block_dirty_ = true; }
            } else if (k.state == Known::alive && k.hp[c] > 0) {
                k.hp[c] = 0;
                block_dirty_ = true;
            }
        }
        if (!seen) continue;
        if (living) {
            if (k.state != Known::alive) {
                // Back alive: unseen camps at full health, visible empty tiles dead.
                for (size_t c = 0; c < g_[i].tiles.size(); ++c) {
                    const vec2 p = g_[i].tiles[c];
                    if (!v.lvis.at(p)) k.hp[c] = camp_spec(g_[i].kinds[c]).hp;
                    else if (!v.lcamps.at(p)) k.hp[c] = 0;
                }
                k.state = Known::alive;
                block_dirty_ = true;
            }
            continue;
        }
        if (k.state == Known::respawning) continue;
        // A reward we are not after only needs an occasional look.
        if (k.state == Known::reward && static_cast<int>(i) != target_ && (now + static_cast<int>(i)) % 4 != 0) continue;
        // One host query: camp_group::state() would rebuild the whole vision board first (~4k fuel).
        const auto handle = g_[i].handle;
        std::uint32_t st = 0;
        if (cambc::detail::sdk_camp_group_state(handle, &st) != 0 || st == 0 || st > 3) continue;
        const auto before = k.state;
        if (st == 1) {
            k.state = Known::alive;
        } else if (st == 2) {
            k.state = Known::reward;
        } else {
            std::int32_t timer = 1;
            if (cambc::detail::sdk_camp_group_cooldown(handle, &timer) != 0) timer = 1;
            k.state = Known::respawning;
            k.ready = now + std::max(1, static_cast<int>(timer));
            for (size_t c = 0; c < g_[i].tiles.size(); ++c) k.hp[c] = camp_spec(g_[i].kinds[c]).hp;
        }
        if (k.state != before) block_dirty_ = true;
    }
    // Health of the camps of the group we fight.
    if (target_ >= 0 && cheb(g_[target_].center, pos_) <= 8 + reach_[target_]) {
        auto& k = k_[target_];
        for (size_t c = 0; c < g_[target_].tiles.size(); ++c) {
            const vec2 p = g_[target_].tiles[c];
            if (cheb(p, pos_) > 8 || !v.lvis.at(p) || !v.lcamps.at(p) || camp_spec(g_[target_].kinds[c]).str <= 0) continue;
            try { k.hp[c] = cambc::vision::camp_at(p).health(); } catch (const std::exception&) {}
        }
    }
}

// Tiles of camps we believe alive, written straight into the board.
void Bot::rebuild_camp_block() {
    if (!block_dirty_) return;
    block_dirty_ = false;
    camp_block_.clear();
    auto* w = camp_block_.ptr();
    for (size_t i = 0; i < g_.size(); ++i) {
        const auto& k = k_[i];
        if (k.state == Known::respawning || k.state == Known::reward) continue;
        for (size_t c = 0; c < g_[i].tiles.size(); ++c)
            if (k.hp[c] > 0) raw_set(w, g_[i].tiles[c]);
    }
}

// ---------------------------------------------------------------- movement

namespace {
int fdist(const cambc::distance_field& f, vec2 p) { return inside(p) ? raw_dist(f.ptr(), p) : kFar; }
}  // namespace

bool Bot::try_dash(const cambc::distance_field& f, const cambc::bitboard& seen) {
    auto& me = cambc::self();
    if (cast_ || !dasher_ || !me.can_cast() || !fuel(12000) || !dash_for_travel()) return false;
    const vec2 pos = me.position();
    if (!bit(seen, pos)) return false;
    const int here = fdist(f, pos);
    for (const auto& ab : me.abilities()) {
        if (!ab.is_dash() || !ab.is_ready()) continue;
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
                // Need a passable middle tile (holes may be jumped, walls not).
                bool path = false;
                for (auto s : kDirs) {
                    const vec2 m{pos.x + s.x, pos.y + s.y};
                    if (!inside(m) || cheb(m, t) != 1) continue;
                    if (hard_local().at(m) || v.lchamps.at(m) || v.lcamps.at(m)) continue;
                    path = true;
                    break;
                }
                if (path) cand[n++] = {d, t};
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

// One step (and a dash when it gains two) along the shortest route around known camps and
// visible units. When only living camps close the way, remember their group in plug_.
// One host BFS from the goals that stops at our tile; boards are refilled in place.
bool Bot::go(const cambc::bitboard& goals_in, bool dash) {
    auto& me = cambc::self();
    if (moved_ || move_failed_ || !me.can_move() || !live()) return false;
    if (!fuel(25000)) return false;
    const vec2 pos = me.position();
    if (!inside(pos)) return false;
    const auto& v = view();
    auto& s = scratch();
    s.blocked.clear();
    s.blocked |= walk_walls();
    s.blocked |= camp_block_;
    s.blocked |= v.camps;
    s.blocked |= v.champs;
    s.blocked |= v.tele;
    raw_clear(s.blocked.ptr(), pos);
    andnot_into(goals_in, s.blocked, s.goals);
    if (bit(s.goals, pos) || s.goals.is_empty()) return false;
    s.here.clear();
    raw_set(s.here.ptr(), pos);
    s.seen.clear();
    s.f.compute_to(s.blocked, s.goals, s.here, s.seen);
    if (bit(s.seen, pos)) return step_on(s.f, s.seen, s.goals, s.blocked, dash);
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
    if (dash && fdist(f, pos) >= 3 && try_dash(f, seen)) {
        moved_ = true;
        if (move_failed_ || !me.can_move()) return true;
    }
    const vec2 from = me.position();
    if (!inside(from)) return moved_;
    if (bit(goals, from)) return true;
    vec2 best = from;
    int best_d = bit(seen, from) ? fdist(f, from) : kFar;
    for (auto d : kDirs) {
        const vec2 t{from.x + d.x, from.y + d.y};
        if (!free_tile(t) || !bit(seen, t) || bit(blocked, t)) continue;
        const int fd = raw_dist(f.ptr(), t);
        if (fd < best_d) { best_d = fd; best = t; }
    }
    if (best == from || !live()) return moved_;
    moved_ = true;
    bool ok = false;
    try { ok = me.move_absolute(best); } catch (const cambc::error& e) { warn("move", e); }
    invalidate();
    if (!ok) move_failed_ = true;
    return ok;
}

void Bot::go_home() { go(cambc::map::spawn_region(team::ally), true); }

// Walk home when it is close, otherwise recall. True when the turn is used.
bool Bot::recall_or_walk(int walk_limit) {
    auto& me = cambc::self();
    if (spawn_local_.at(pos_)) return false;
    const int d = home_.at(pos_);
    if (d <= walk_limit || static_cast<int>(cambc::round_number()) < ambush_until_) {
        go_home();  // close by, or an unseen enemy would break the recall
        return true;
    }
    if (moved_ || incoming_ > 0 || !live()) return moved_;
    try { me.recall(); } catch (const cambc::error& e) { warn("recall", e); }
    return true;
}

// Far from the group: one step down its cached walking field instead of a fresh route search. The parent
// used it to save fuel (its route search cost ~10k); routing every step with go() is cheap now and also
// dashes and steps around units, so this is off (kept for reference and as a switch).
constexpr bool kWalkStatic = false;
bool Bot::walk_static(int group, int far) {
    auto& me = cambc::self();
    const int now = static_cast<int>(cambc::round_number());
    if (!kWalkStatic || far <= 6 || now < static_off_until_ || moved_ || move_failed_ || !live() || !me.can_move()) return false;
    const auto& f = gfield_[group];
    if (f.empty()) return false;
    if (dasher_ && !cast_ && me.can_cast() && dash_for_travel())
        for (const auto& ab : me.abilities())
            if (ab.is_dash() && ab.is_ready()) return false;
    const vec2 pos = me.position();
    vec2 best = pos;
    int best_d = f.at(pos);
    if (best_d >= kFar) return false;
    for (auto d : kDirs) {
        const vec2 t{pos.x + d.x, pos.y + d.y};
        const int fd = f.at(t);
        if (fd >= best_d || !free_tile(t) || bit(camp_block_, t)) continue;
        best_d = fd;
        best = t;
    }
    if (best == pos) { static_off_until_ = now + 12; return false; }
    moved_ = true;
    bool ok = false;
    try { ok = me.move_absolute(best); } catch (const cambc::error& e) { warn("move", e); }
    invalidate();
    if (!ok) { move_failed_ = true; static_off_until_ = now + 12; }
    return true;
}
