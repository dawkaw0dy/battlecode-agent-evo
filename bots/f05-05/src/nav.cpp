// Fuel and movement: the turn's fuel guards, the vision snapshot, the walking fields we keep, and the
// route steps built on them.
//
// Where the fuel went before (measured on 100x100 maps, ~54k a turn and 5-9% of turns cut short): every
// vision refresh exported four boards (~8.5k, several refreshes a turn), every kept field was exported
// (~23k on top of a ~30k search), and every bitboard expression allocated. Here tiles are read straight
// from the engine's storage (common.h), boards are built in place into storage that lives as long as the
// bot, the allies' positions are read once a round, and enemy lists once per vision refresh. Field
// searches only start when the fuel left covers what this map's searches cost.
#include <cambc.h>
#include "bot.h"
#include <algorithm>
#include <array>
#include <span>

// A turn that ran out of fuel resumes next round on stale data: act only in the round we started.
std::uint32_t g_round = 0;
bool live() { return cambc::round_number() == g_round; }
bool fuel(std::uint64_t need) { return cambc::fuel_remaining() > need; }

namespace {
View* g_view = nullptr;              // allocated on first use: boards need the map layout
std::uint32_t g_gen = 1;             // vision generation, bumped by invalidate()
vec2 g_self{};
std::uint32_t g_self_gen = 0;
// Allied handles and types never change; their positions only change between our turns.
std::vector<cambc::champion> g_others;
std::vector<cambc::champion_type> g_other_types;
bool g_others_built = false;
std::uint32_t g_allies_round = 0xffffffffu;
std::vector<AllySeen> g_allies;
std::array<std::vector<Foe>, 2> g_foes;
std::array<std::uint32_t, 2> g_foes_gen{0, 0};
Bits g_walls_local, g_hard_local;
// Boards and fields reused by every route search.
struct RouteScratch {
    cambc::bitboard blocked, open, notb, goals, goals2, here, seen, wseen;
    cambc::distance_field route, walk;
};
RouteScratch* g_rs = nullptr;
RouteScratch& rs() {
    if (!g_rs) g_rs = new RouteScratch;
    return *g_rs;
}

inline int fget(const cambc::distance_field& f, vec2 p) {
    if (!inside(p)) return kFar;
    return g_raw ? raw_dist(f.ptr(), p) : f.get(p);
}
inline bool bset(const cambc::bitboard& b, vec2 p) {
    if (!inside(p)) return false;
    return g_raw ? raw_bit(b.ptr(), p) : b.is_set(p);
}

// One host call for all a champion shows; false (no exception: a throw costs thousands) when out of sight.
bool champ_info(std::uint32_t h, cambc::detail::champion_info& out) { return cambc::detail::sdk_champion(h, &out) == 0; }
bool champ_alive(std::uint32_t h) {
    std::int32_t a = 0;
    return cambc::detail::sdk_champion_is_alive(h, &a) == 0 && a != 0;
}

vec2 self_pos() {
    if (g_self_gen != g_gen) {
        g_self = cambc::self().position();
        g_self_gen = g_gen;
    }
    return g_self;
}

void refresh_allies() {
    const std::uint32_t now = cambc::round_number();
    if (g_allies_round == now) return;
    g_allies_round = now;
    if (!g_others_built) {
        g_others_built = true;
        const int me = static_cast<int>(static_cast<cambc::handle_champion_t>(cambc::self()));
        for (const auto& a : cambc::champions(team::ally))
            if (static_cast<int>(static_cast<cambc::handle_champion_t>(a)) != me) {
                g_others.push_back(a);
                cambc::champion_type t{};
                try { t = a.champion_name(); } catch (const cambc::error&) {}
                g_other_types.push_back(t);
            }
    }
    g_allies.clear();
    for (size_t i = 0; i < g_others.size(); ++i) {
        const auto h = static_cast<std::uint32_t>(static_cast<cambc::handle_champion_t>(g_others[i]));
        cambc::detail::champion_info ci{};
        if (!champ_alive(h) || !champ_info(h, ci)) continue;
        const vec2 p{ci.x, ci.y};
        if (inside(p)) g_allies.push_back({p, g_other_types[i], ci.health});
    }
}
}  // namespace

void nav_setup() {
    g_walls_local.ref(walk_walls());  // a static board: its storage never moves
    auto hard = cambc::map::walls() | cambc::map::altars();
    g_hard_local.load(hard);
    auto& r = rs();
    r.open.fill();
    r.open ^= walk_walls();
}

const cambc::bitboard& open_tiles() { return rs().open; }

void invalidate() {
    if (g_view) g_view->ok = false;
    ++g_gen;
}

const View& view() {
    if (!g_view) {
        g_view = new View;
        g_view->lvis.ref(g_view->vis);
        g_view->lcamps.ref(g_view->camps);
        g_view->lchamps.ref(g_view->champs);
        g_view->ltele.ref(g_view->tele);
    }
    View& v = *g_view;
    if (!v.ok) {
        // Filled in place: the Bits above keep pointing at these boards.
        cambc::vision::visible_tiles_into(v.vis);
        cambc::vision::camp_tiles_into(v.camps);
        cambc::vision::champion_tiles_into(team::ally, v.champs);
        cambc::vision::champion_tiles_into(team::enemy, scratch());
        v.champs |= scratch();
        cambc::vision::teleport_tiles_into(v.tele);
        refresh_allies();
        v.allies.clear();
        v.ally_types.clear();
        for (const auto& a : g_allies) {
            v.allies.push_back(a.pos);
            v.ally_types.push_back(a.type);
        }
        for (auto p : v.allies) v.champs.set(p);  // allies hidden in bushes too
        v.ok = true;
    }
    return v;
}

const std::vector<AllySeen>& allies_seen() {
    refresh_allies();
    return g_allies;
}

const Bits& walls_local() { return g_walls_local; }
const Bits& hard_local() { return g_hard_local; }  // what a dash cannot cross

bool free_tile(vec2 p) {
    if (!inside(p)) return false;
    if (p == self_pos()) return true;
    const auto& v = view();
    return !g_walls_local.at(p) && !v.lcamps.at(p) && !v.lchamps.at(p) && !v.ltele.at(p);
}

// Walls, the enemy spawn, the camps given and every visible unit (our own tile excluded), in place.
const cambc::bitboard& blocked_now(const cambc::bitboard& camps) {
    const auto& v = view();
    auto& b = rs().blocked;
    b.clear();
    b |= walk_walls();
    b |= camps;
    b |= v.camps;
    b |= v.champs;
    b |= v.tele;
    b.set(self_pos(), false);
    return b;
}

std::vector<Foe> foes(team side) {
    const size_t s = side == team::ally ? 0 : 1;
    if (g_foes_gen[s] == g_gen) return g_foes[s];
    std::vector<Foe> out;
    const vec2 me = self_pos();
    std::array<vec2, 16> tiles;
    cambc::vision::champion_tiles_into(side, scratch());
    const std::uint32_t n = std::min<std::uint32_t>(scratch().positions_into(std::span<vec2>{tiles}), 16);
    for (std::uint32_t i = 0; i < n; ++i) {
        const vec2 p = tiles[i];
        if (p == me) continue;
        std::uint32_t h = 0;
        cambc::detail::champion_info ci{};
        if (cambc::detail::sdk_champion_at(p.x, p.y, &h) != 0 || !champ_alive(h) || !champ_info(h, ci)) continue;
        Foe f{p, ci.health, ci.max_health, 1.1f * ci.strength + 0.25f * ci.focus, ci.attack_range};
        if (side == team::enemy) {
            const auto st = cambc::champion_state::from_bits_retain(static_cast<std::uint32_t>(ci.state));
            f.chan = st.contains(cambc::champion_state::submitting_gems);
            f.recall = st.contains(cambc::champion_state::recalling);
            std::uint32_t g = 0;
            if (cambc::detail::sdk_champion_gems(h, &g) == 0) f.gems = static_cast<int>(g);
        }
        out.push_back(f);
    }
    g_foes[s] = out;
    g_foes_gen[s] = g_gen;
    return out;
}

// ---------------------------------------------------------------- kept fields

// Walking fields to the groups, best ones first, while the fuel left covers a whole search.
void Bot::fill_fields(std::uint64_t reserve) {
    for (int i : fill_order_) {
        if (!gfield_[i].empty()) continue;
        if (!fuel(reserve + field_cost())) return;
        gfield_[i].compute(walk_walls(), gtiles_[i]);
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

// ---------------------------------------------------------------- routes

// One step (and a dash when it gains two) along the shortest route around known camps and
// visible units. When only living camps close the way, remember their group in plug_.
bool Bot::go(const cambc::bitboard& goals_in, bool dash) {
    auto& me = cambc::self();
    if (moved_ || move_failed_ || !me.can_move() || !live()) return false;
    if (!fuel(25000)) return false;
    auto& r = rs();
    const vec2 pos = self_pos();
    const auto& blocked = blocked_now(camp_block_);
    r.notb.fill();
    r.notb ^= blocked;
    r.goals.clear();
    r.goals |= goals_in;
    r.goals &= r.notb;
    if (r.goals.is_empty() || bset(r.goals, pos)) return false;
    r.here.clear();
    r.here.set(pos);
    r.route.compute_to(blocked, r.goals, r.here, r.seen);
    if (bset(r.seen, pos)) return step_on(r.route, r.seen, r.goals, blocked, dash);
    // Camps (or units) close the way: find what plugs the walk-only route.
    if (!fuel(15000 + field_cost())) return false;
    r.goals2.clear();
    r.goals2 |= goals_in;
    r.goals2 &= r.open;
    r.walk.compute_to(walk_walls(), r.goals2, r.here, r.wseen);
    if (!bset(r.wseen, pos)) return false;
    // Follow that route through tiles until the first camp on it.
    vec2 at = pos;
    for (int steps = 0; steps < 60; ++steps) {
        vec2 next = at;
        int nd = fget(r.walk, at);
        for (auto d : kDirs) {
            const vec2 t{at.x + d.x, at.y + d.y};
            const int fd = fget(r.walk, t);
            if (fd < nd && !walls_local().at(t)) { nd = fd; next = t; }
        }
        if (next == at) break;
        const int gi = group_at_[static_cast<size_t>(tidx(next))];
        if (gi >= 0 && bset(camp_block_, next)) {
            if (plug_ != gi) { plug_ = gi; plug_since_ = static_cast<int>(cambc::round_number()); plug_altar_ = false; }
            return false;
        }
        at = next;
    }
    // Units in the gap: walk the plain route and wait there for them to move.
    return step_on(r.walk, r.wseen, r.goals, blocked, dash);
}

// Dash and/or walk one step down field f (reached tiles in seen) towards goals.
bool Bot::step_on(const cambc::distance_field& f, const cambc::bitboard& seen, const cambc::bitboard& goals,
                  const cambc::bitboard& blocked, bool dash) {
    auto& me = cambc::self();
    const vec2 pos = self_pos();
    if (!live() || !inside(pos)) return false;
    if (dash && fget(f, pos) >= 3 && try_dash(f, seen)) {
        moved_ = true;
        if (move_failed_ || !me.can_move()) return true;
    }
    const vec2 from = self_pos();
    if (bset(goals, from)) return true;
    vec2 best = from;
    int best_d = bset(seen, from) ? fget(f, from) : kFar;
    for (auto d : kDirs) {
        const vec2 t{from.x + d.x, from.y + d.y};
        if (!free_tile(t) || !bset(seen, t) || bset(blocked, t)) continue;
        const int fd = fget(f, t);
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

// Far from the group: one step down its kept walking field instead of a fresh route search. Not while a
// dash is ready (the routed walk uses it), and not for a while after the kept step was blocked.
bool Bot::walk_static(int group, int far) {
    auto& me = cambc::self();
    const int now = static_cast<int>(cambc::round_number());
    if (far <= 6 || now < static_off_until_ || moved_ || move_failed_ || !live() || !me.can_move()) return false;
    const auto& f = gfield_[group];
    if (f.empty()) return false;
    if (dasher_ && !cast_ && me.can_cast())
        for (const auto& ab : me.abilities())
            if (ab.is_dash() && ab.is_ready()) return false;
    const vec2 pos = self_pos();
    vec2 best = pos;
    int best_d = f.at(pos);
    if (best_d >= kFar) return false;
    for (auto d : kDirs) {
        const vec2 t{pos.x + d.x, pos.y + d.y};
        const int fd = f.at(t);
        if (fd >= best_d || !free_tile(t) || bset(camp_block_, t)) continue;
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

// Raw reads for code outside this file (spells.cpp's dash).
int field_at(const cambc::distance_field& f, vec2 p) { return fget(f, p); }
bool board_at(const cambc::bitboard& b, vec2 p) { return bset(b, p); }
