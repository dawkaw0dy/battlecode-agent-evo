// Split-farm champion: each champion farms its own region of camp groups, tributes at the
// nearest altar and only groups up when an ally reports an invader in shared memory.
#include <cambc.h>
#include "bot.h"
#include "dbg.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <variant>
#include <vector>

using cambc::vec2;
using cambc::team;

namespace {

constexpr std::array<vec2, 8> dirs{vec2{0, 1}, vec2{1, 1}, vec2{1, 0}, vec2{1, -1},
    vec2{0, -1}, vec2{-1, -1}, vec2{-1, 0}, vec2{-1, 1}};
constexpr int unreached = 65535;

int dist(vec2 a, vec2 b) { return static_cast<int>(a.chebyshev_distance(b)); }
int g_w = 0, g_h = 0;  // plain globals: function statics cost a guard check on every call
int map_w() { if (!g_w) g_w = cambc::map::width(); return g_w; }
int map_h() { if (!g_h) g_h = cambc::map::height(); return g_h; }
bool in_bounds(vec2 p) { return p.x >= 0 && p.y >= 0 && p.x < map_w() && p.y < map_h(); }
int tile_index(vec2 p) { return p.y * map_w() + p.x; }

// Chebyshev box with explicit bounds checks (draw_chebyshev can throw at the edge).
void add_box(cambc::bitboard& board, vec2 c, int r) {
    for (int dx = -r; dx <= r; ++dx)
        for (int dy = -r; dy <= r; ++dy) {
            vec2 p{c.x + dx, c.y + dy};
            if (in_bounds(p)) board.set(p);
        }
}

// Chebyshev dilation with whole-board shifts: a few host calls instead of one per tile.
cambc::bitboard dilate(const cambc::bitboard& board, int r) {
    cambc::bitboard out = board;
    for (int i = 0; i < r; ++i) out = out | out.shift_right(1) | out.shift_right(-1);
    for (int i = 0; i < r; ++i) out = out | out.shift_up(1) | out.shift_up(-1);
    return out;
}

cambc::bitboard empty_board() {
    cambc::bitboard b;
    b.clear();
    return b;
}

bool fuel_ok(std::uint64_t need) { return cambc::fuel_remaining() > need; }

// A turn that runs out of fuel resumes next round on stale data: act only in the round we started.
std::uint32_t g_turn_round = 0;
bool live() { return cambc::round_number() == g_turn_round; }

// Channel and self-cast actions can still be refused (a stun, a lock we cannot see): never let that end the turn.
void safe_recall() {
    if (!live()) return;
    try { cambc::self().recall(); } catch (const cambc::error&) {}
}
void safe_use(const cambc::ability& ab) {
    if (!live()) return;
    try { ab.use(); } catch (const cambc::error&) {}
}

const Grid8& walls_local() {
    static const Grid8 g = [] { Grid8 x; x.load(static_walls_ally()); return x; }();
    return g;
}
const Grid8& hard_walls_local() {  // walls and altars: what a dash cannot pass
    static const Grid8 g = [] { Grid8 x; x.load(cambc::map::walls() | cambc::map::altars()); return x; }();
    return g;
}

// Vision snapshot, refreshed after anything that changes what we see.
struct View {
    cambc::bitboard visb, camps, champs, teleports;
    Grid8 vis, lcamps, lchamps, ltele;
    vec2 me{};                   // our position when the snapshot was taken
    bool valid = false;
};
View g_view;
std::vector<std::pair<vec2, float>> g_camp_hp;

void invalidate_view() { g_view.valid = false; g_camp_hp.clear(); }
const View& view() {
    if (!g_view.valid) {
        g_view.visb = cambc::vision::visible_tiles();
        g_view.vis.load(g_view.visb);
        g_view.camps = cambc::vision::camp_tiles();
        g_view.champs = cambc::vision::champion_tiles();
        g_view.teleports = cambc::vision::teleport_tiles();
        g_view.lcamps.load(g_view.camps);
        g_view.lchamps.load(g_view.champs);
        g_view.ltele.load(g_view.teleports);
        g_view.me = cambc::self().position();
        g_view.valid = true;
    }
    return g_view;
}

// Tiles known to be occupied although the current vision may not show them: allies hidden in
// bushes, a hidden unit a failed move revealed, an enemy seen a moment ago. Kept short: units move.
struct Occupied { vec2 p; int until; };
std::vector<vec2> g_allies, g_enemies;  // champion positions this round (allies hidden in bushes included)
std::vector<Occupied> g_occupied;
int g_occupied_round = -1;

void remember_occupied(vec2 p, int until) {
    if (!in_bounds(p)) return;
    for (auto& o : g_occupied)
        if (o.p == p) { o.until = std::max(o.until, until); return; }
    g_occupied.push_back({p, until});
}

// Once per round: drop stale entries, add every ally (vision misses those in bushes) and the enemies in sight.
void refresh_occupied() {
    const int now = static_cast<int>(g_turn_round);
    if (g_occupied_round == now) return;
    g_occupied_round = now;
    std::erase_if(g_occupied, [now](const Occupied& o) { return o.until < now; });
    const vec2 me = cambc::self().position();
    g_allies.clear();
    g_enemies.clear();
    try {
        for (const auto& c : cambc::champions(team::ally)) {
            if (!c.is_alive()) continue;
            const vec2 p = c.position();
            if (p == me) continue;
            remember_occupied(p, now);
            g_allies.push_back(p);
        }
    } catch (const cambc::error&) {}
    for (auto p : cambc::vision::champion_tiles(team::enemy).positions())
        if (p != me) {
            remember_occupied(p, now + 1);
            g_enemies.push_back(p);
        }
}

bool remembered_at(vec2 p) {
    refresh_occupied();
    const int now = static_cast<int>(g_turn_round);
    for (const auto& o : g_occupied)
        if (o.p == p && o.until >= now) return true;
    return false;
}

// An allied champion stands on p (allies are refreshed every round, hidden ones included).
bool ally_at(vec2 p) {
    refresh_occupied();
    return std::find(g_allies.begin(), g_allies.end(), p) != g_allies.end();
}

bool blocked_at(vec2 p) {
    if (!in_bounds(p)) return true;
    const auto& v = view();
    if (p == v.me) return false;
    return walls_local().at(p) || v.lcamps.at(p) || v.lchamps.at(p) || v.ltele.at(p) || remembered_at(p);
}

cambc::bitboard blocked_board(const cambc::bitboard& walls) {
    const auto& v = view();
    auto blocked = walls | v.champs | v.camps | v.teleports;
    refresh_occupied();
    const int now = static_cast<int>(g_turn_round);
    for (const auto& o : g_occupied)
        if (o.until >= now) blocked.set(o.p);
    blocked.set(v.me, false);
    return blocked;
}

float camp_hp(vec2 p) {
    for (auto& [q, hp] : g_camp_hp)
        if (q == p) return hp;
    float hp = 0;
    try { hp = cambc::vision::camp_at(p).health(); } catch (const std::exception&) {}
    g_camp_hp.push_back({p, hp});
    return hp;
}

int draft_index(cambc::champion_type t) {
    const int v = static_cast<int>(t);
    if (v <= 2) return v;
    if (v <= 4) return 3;
    return v - 1;
}

float camp_strength(cambc::camp_type t) {
    switch (t) {
    case cambc::camp_type::wolf: return 20;
    case cambc::camp_type::elk: return 60;
    case cambc::camp_type::monolith: return 200;
    case cambc::camp_type::golem: return 120;
    default: return 0;
    }
}

float camp_max_hp(cambc::camp_type t) {
    switch (t) {
    case cambc::camp_type::wolf: return 1350;
    case cambc::camp_type::elk: return 2400;
    case cambc::camp_type::monolith: return 20000;
    case cambc::camp_type::golem: return 5000;
    default: return 120;
    }
}

int camp_range(cambc::camp_type t) {
    switch (t) {
    case cambc::camp_type::wolf: return 3;
    case cambc::camp_type::elk: return 4;
    case cambc::camp_type::monolith: return 5;
    case cambc::camp_type::golem: return 4;
    default: return 0;
    }
}

struct Sighting { vec2 pos; float hp, max_hp, power; int range; };

std::vector<Sighting> visible_champions(team side) {
    std::vector<Sighting> out;
    const vec2 me = cambc::self().position();
    for (auto p : cambc::vision::champion_tiles(side).positions()) {
        if (p == me) continue;
        try {
            auto c = cambc::vision::champion_at(p);
            if (!c.is_alive()) continue;
            Sighting s{p, c.health(), c.max_health(), c.strength() + 0.3f * c.focus(), c.attack_range()};
            out.push_back(s);
        } catch (const cambc::error&) {
        }
    }
    return out;
}

// The latest invader report an ally wrote to shared memory.
struct Report { int round = -1000; vec2 where{}; int count = 0; int slot = 0; };
Report read_report() {
    std::array<std::uint8_t, 6> buf{};
    cambc::shared_memory.read(0, std::span<std::uint8_t>{buf.data(), buf.size()});
    Report r;
    if (!buf[5]) return r;
    r.round = buf[0] | (buf[1] << 8);
    r.where = vec2{buf[2], buf[3]};
    r.count = buf[4];
    r.slot = buf[5] - 1;
    return r;
}

// Shard plan: emblem families to complete in order (a repeated emblem means another copy).
struct Step { cambc::item_type shard, emblem; };

} // namespace

void Grid8::load(const cambc::bitboard& b) {
    v.resize(cambc::bitboard::linear_buffer_size());
    b.export_linear_buffer(std::span<std::uint8_t>{v});
}
bool Grid8::at(vec2 p) const {
    if (!in_bounds(p) || v.empty()) return false;
    const int i = tile_index(p);
    return (v[static_cast<size_t>(i >> 3)] >> (i & 7)) & 1;
}
void Grid16::load(const cambc::distance_field& f) {
    v.resize(static_cast<size_t>(map_w() * map_h()));
    f.export_linear_buffer(std::span<std::uint16_t>{v});
}
int Grid16::at(vec2 p) const {
    if (!in_bounds(p) || v.empty()) return unreached;
    return v[static_cast<size_t>(tile_index(p))];
}

void Bot::initialise() {
    ready_ = true;
    auto& me = cambc::self();
    groups_ = analyse_groups();
    handles_ = cambc::map::camp_groups();
    known_.assign(groups_.size(), Known{});
    for (size_t i = 0; i < groups_.size(); ++i)
        for (const auto& c : handles_[i].camps()) known_[i].hp.push_back(camp_max_hp(c.kind()));
    group_field_.assign(groups_.size(), Grid16{});
    camp_group_at_.assign(static_cast<size_t>(map_w() * map_h()), -1);
    for (size_t i = 0; i < groups_.size(); ++i) group_tiles_.push_back(handles_[i].spawn_tiles());
    all_camp_tiles_ = empty_board();
    for (const auto& t : group_tiles_) all_camp_tiles_ |= t;
    camp_kind_at_.assign(static_cast<size_t>(map_w() * map_h()), -1);
    for (const auto& g : handles_)
        for (const auto& c : g.camps())
            camp_kind_at_[static_cast<size_t>(tile_index(c.spawn_position()))] = static_cast<std::int8_t>(static_cast<int>(c.kind()));
    for (size_t i = 0; i < groups_.size(); ++i)
        for (auto t : groups_[i].tiles) camp_group_at_[static_cast<size_t>(tile_index(t))] = static_cast<std::int16_t>(i);

    const auto* mem = cambc::draft_memory.data();
    const size_t size = cambc::draft_memory.size();
    const int mine = draft_index(me.champion_name());
    if (size > layout::regions && static_cast<char>(mem[layout::magic]) == 'S') {
        const int team_size = static_cast<int>(mem[layout::team_size]);
        for (int i = 0; i < team_size && i < 5; ++i)
            if (static_cast<int>(mem[layout::picks + i]) == mine) slot_ = i;
        const size_t count = static_cast<size_t>(mem[layout::group_count]);
        if (count == groups_.size() && layout::regions + count <= size)
            for (size_t i = 0; i < count; ++i) owner_.push_back(static_cast<std::uint8_t>(mem[layout::regions + i]));
    }
    for (const auto& ab : me.abilities())
        if (ab.is_dash()) dasher_ = true;
    walls_ = dasher_ ? &dash_walls_ally() : &static_walls_ally();
    if (owner_.size() != groups_.size()) {
        const bool dash_map = is_dash_map(groups_);
        const auto picks = preferred_picks(static_cast<int>(cambc::champions(team::ally).size()), dash_map);
        owner_ = split_regions(groups_, static_cast<int>(picks.size()), dash_map);
        for (size_t i = 0; i < picks.size(); ++i)
            if (static_cast<int>(picks[i]) == mine) slot_ = static_cast<int>(i);
    }

    // Distances to home and to the tiles beside any altar.
    cambc::distance_field f;
    cambc::bitboard seen;
    f.compute(*walls_, cambc::map::spawn_region(team::ally), seen);
    home_.load(f);
    auto beside = empty_board();
    for (auto a : cambc::map::altars().positions()) add_box(beside, a, cambc::constants::submit_gems_range);
    beside &= ~static_walls_ally();
    // Camp spawn tiles are usually occupied (a boss can sit beside an altar): never plan to stand there.
    for (const auto& g : groups_)
        for (auto t : g.tiles) beside.set(t, false);
    f.compute(*walls_, beside, seen);
    altar_.load(f);
    altar_tiles_ = beside;

    // Fill group fields for our own region first, then nearest to home.
    for (size_t i = 0; i < groups_.size(); ++i) field_order_.push_back(static_cast<int>(i));
    std::stable_sort(field_order_.begin(), field_order_.end(), [&](int a, int b) {
        const bool ma = owner_[a] == slot_, mb = owner_[b] == slot_;
        if (ma != mb) return ma;
        return groups_[a].dist_ally < groups_[b].dist_ally;
    });
}

void Bot::fill_fields(std::uint64_t reserve) {
    for (int i : field_order_) {
        if (!group_field_[i].empty()) continue;
        if (!fuel_ok(reserve)) return;
        auto roots = empty_board();
        for (auto t : groups_[i].tiles) roots.set(t);
        cambc::distance_field f;
        cambc::bitboard seen;
        f.compute(*walls_, roots, seen);
        group_field_[i].load(f);
    }
}

int Bot::eta(int group) const {
    const vec2 pos = pos_;
    if (!group_field_[group].empty()) {
        const int d = group_field_[group].at(pos);
        return d >= unreached ? 9999 : d;
    }
    if ((dasher_ ? groups_[group].dist_ally_dash : groups_[group].dist_ally) >= 9999) return 9999;
    return dist(pos, groups_[group].center) * 5 / 4 + 2;
}

void Bot::update_knowledge() {
    const int now = static_cast<int>(cambc::round_number());
    for (auto& k : known_)
        if (k.state == Known::respawning && now >= k.ready) k.state = Known::unknown;
    const auto& v = view();
    const vec2 pos = pos_;
    for (size_t i = 0; i < groups_.size(); ++i) {
        const int d = dist(groups_[i].center, pos);
        if (d > 16) continue;
        auto& k = known_[i];
        const auto& camps = handles_[i].camps();
        // A visible living camp means the group is alive: no need to ask the engine.
        bool seen = false, living = false;
        for (size_t c = 0; c < camps.size(); ++c) {
            const vec2 p = camps[c].spawn_position();
            if (!v.vis.at(p)) continue;
            seen = true;
            if (v.lcamps.at(p)) living = true;
        }
        if (!seen) continue;
        if (living) {
            k.state = Known::alive;
            k.seen = now;
            if (d <= 9 && (static_cast<int>(i) == target_ || static_cast<int>(i) == blocker_group_))
                for (size_t c = 0; c < camps.size(); ++c) {
                    const vec2 p = camps[c].spawn_position();
                    if (!v.vis.at(p)) continue;
                    if (!v.lcamps.at(p)) k.hp[c] = 0;
                    else if (camp_strength(camps[c].kind()) > 0) k.hp[c] = camp_hp(p);
                    else k.hp[c] = camp_max_hp(camps[c].kind());
                }
            continue;
        }
        if (k.state == Known::respawning) continue;  // timer already known
        try {
            const auto st = handles_[i].state();
            k.seen = now;
            if (std::holds_alternative<cambc::map::camp_group_alive>(st)) {
                k.state = Known::alive;
                for (size_t c = 0; c < camps.size(); ++c)
                    if (v.vis.at(camps[c].spawn_position())) k.hp[c] = 0;
            } else if (std::holds_alternative<cambc::map::camp_group_reward_available>(st)) {
                k.state = Known::reward;
            } else {
                k.state = Known::respawning;
                k.ready = now + std::max(1, std::get<cambc::map::camp_group_respawning>(st).timer);
                for (size_t c = 0; c < camps.size(); ++c) k.hp[c] = camp_max_hp(camps[c].kind());
            }
        } catch (const std::exception&) {
        }
    }
}

void Bot::buy_items() {
    using it = cambc::item_type;
    auto& me = cambc::self();
    static const std::vector<Step> plan{
        {it::jungle_shard_of_strength, it::jungle_emblem_of_strength},
        {it::power_shard_of_health, it::power_emblem_of_health},
        {it::monster_shard_of_strength, it::monster_emblem_of_strength},
        {it::power_shard_of_strength, it::power_emblem_of_strength},
        {it::durability_shard_of_health, it::durability_emblem_of_health},
        {it::power_shard_of_health, it::power_emblem_of_health},
        {it::power_shard_of_strength, it::power_emblem_of_strength},
        {it::power_shard_of_health, it::power_emblem_of_health},
        {it::power_shard_of_strength, it::power_emblem_of_strength},
        {it::power_shard_of_health, it::power_emblem_of_health},
        {it::power_shard_of_strength, it::power_emblem_of_strength},
    };
    if (me.gold_count() < 30) return;
    for (int guard = 0; guard < 40; ++guard) {
        if (me.gold_count() < 30 || !fuel_ok(20000)) return;
        const auto counts = me.item_count();
        const Step* next = nullptr;
        std::array<int, static_cast<size_t>(it::enum_type::count)> needed{};
        for (const auto& s : plan) {
            const auto e = static_cast<size_t>(static_cast<it::enum_type>(s.emblem));
            ++needed[e];
            if (static_cast<int>(counts[s.emblem]) < needed[e]) { next = &s; break; }
        }
        // Past the plan: alternate strength and health shards.
        const it extra = (counts[it::power_shard_of_strength] + counts[it::power_emblem_of_strength] * 5
                          <= counts[it::power_shard_of_health] + counts[it::power_emblem_of_health] * 5)
            ? it::power_shard_of_strength : it::power_shard_of_health;
        const it shard = next ? next->shard : extra;
        if (!live()) return;
        if (!me.can_buy(shard, 1)) {
            cant_spend_ = true;
            return;
        }
        try { me.buy(shard, 1); } catch (const cambc::error&) { cant_spend_ = true; return; }
        cant_spend_ = false;
    }
}

// ---- Pathing ----
// One rule for every route: walk the shortest path around the units we know of. Far from units the
// cached static field is exact, so we follow it greedily; with units nearby we search a local field to
// the static field's tiles K steps ahead, so the detour always agrees with the long route. If we keep
// circling between two tiles, both entry points switch to whole-map unit-aware fields for a while.

namespace {
constexpr int detour_radius = 5;   // K: how far ahead the local search aims
constexpr int detour_slack = 6;    // extra steps a local detour may take
constexpr int stuck_window = 8;    // step attempts looked at by the circling check
}

bool Bot::danger_at(vec2 p) const {
    for (const auto& [t, r] : threats_)
        if (dist(t, p) <= r) return true;
    for (const auto& e : g_enemies)
        if (dist(e, p) <= 2) return true;
    return false;
}

// Remember where each step attempt left us; circling on two tiles means the planners disagree or a
// blocker comes and goes with our vision: plan with whole-map fields that remember occupants.
void Bot::note_trail(vec2 p) {
    const int now = static_cast<int>(cambc::round_number());
    trail_[static_cast<size_t>(trail_n_ % stuck_window)] = p;
    trail_round_[static_cast<size_t>(trail_n_ % stuck_window)] = now;
    ++trail_n_;
    if (trail_n_ < stuck_window || now < dynamic_until_) return;
    int oldest = now;
    for (int r : trail_round_) oldest = std::min(oldest, r);
    if (now - oldest > 3 * stuck_window) return;
    std::array<vec2, stuck_window> distinct{};
    int n = 0;
    for (const auto& q : trail_) {
        bool seen = false;
        for (int i = 0; i < n; ++i) seen |= distinct[static_cast<size_t>(i)] == q;
        if (!seen) distinct[static_cast<size_t>(n++)] = q;
    }
    if (n <= 2) {
        dynamic_until_ = now + 15;
        trail_n_ = 0;
        DBG(slot_, "stuck at " << p.x << "," << p.y << " -> dynamic");
    }
}

// Move to the first candidate that works; a hidden unit refuses the step without cost, so try one more.
bool Bot::take_step(const std::array<vec2, 8>& cands, int n) {
    auto& me = cambc::self();
    const int now = static_cast<int>(cambc::round_number());
    for (int i = 0; i < n && i < 2; ++i) {
        if (!live() || !me.can_move()) break;
        const vec2 t = cands[static_cast<size_t>(i)];
        if (blocked_at(t)) continue;
        bool ok = false, threw = false;
        try { ok = me.move_absolute(t); } catch (const cambc::error&) { threw = true; }
        invalidate_view();
        DBG(slot_, "step " << me.position().x << "," << me.position().y << " try " << t.x << "," << t.y << " ok " << ok << (threw ? " threw" : ""));
        if (ok) {
            moved_ = true;
            note_trail(t);
            return true;
        }
        remember_occupied(t, now + 2);
        if (threw) break;
    }
    moved_ = true;
    move_failed_ = true;
    note_trail(me.position());
    return false;
}

// Dash two tiles along a field when it saves two steps. Field values come from a local grid or from
// a host field with its reached mask.
bool Bot::try_dash(const Grid16* local, const cambc::distance_field* field, const cambc::bitboard* reached) {
    auto& me = cambc::self();
    if (cast_ || !me.can_cast() || !fuel_ok(15000)) return false;
    auto value = [&](vec2 t) -> int {
        if (local) return local->at(t);
        if (!reached->is_set(t)) return unreached;
        return field->get(t);
    };
    for (const auto& ab : me.abilities()) {
        if (!ab.is_dash() || !ab.is_ready()) continue;
        const vec2 pos = me.position();
        const int here = value(pos);
        if (here >= unreached) return false;
        // Landing tiles two steps away that gain at least two steps, best first, safe ones before others.
        std::array<std::pair<int, vec2>, 16> cands;
        size_t n = 0;
        for (int dx = -2; dx <= 2; ++dx)
            for (int dy = -2; dy <= 2; ++dy) {
                if (std::max(std::abs(dx), std::abs(dy)) != 2) continue;
                const vec2 t{pos.x + dx, pos.y + dy};
                if (!in_bounds(t)) continue;
                const int d = value(t);
                if (d > here - 2 || blocked_at(t)) continue;
                // Some first step must be free of walls and units (holes may be jumped).
                bool path = false;
                for (int sx = -1; sx <= 1 && !path; ++sx)
                    for (int sy = -1; sy <= 1 && !path; ++sy) {
                        if (!sx && !sy) continue;
                        const vec2 m{pos.x + sx, pos.y + sy};
                        if (dist(m, t) != 1 || !in_bounds(m)) continue;
                        if (hard_walls_local().at(m) || view().lchamps.at(m) || view().lcamps.at(m) || remembered_at(m)) continue;
                        path = true;
                    }
                if (!path) continue;
                cands[n++] = {d * 4 + (danger_at(t) ? 3 : 0) + (dx && dy ? 1 : 0), t};
            }
        std::sort(cands.begin(), cands.begin() + n, [](auto a, auto b) { return a.first < b.first; });
        for (size_t i = 0; i < n && i < 4; ++i) {
            if (!live()) return false;
            try {
                const bool ok = ab.use(cands[i].second);
                DBG(slot_, "dash " << pos.x << "," << pos.y << " -> " << cands[i].second.x << "," << cands[i].second.y << " ok " << ok);
                cast_ = true;
                invalidate_view();
                if (!ok) {
                    remember_occupied(cands[i].second, static_cast<int>(cambc::round_number()) + 2);
                    move_failed_ = true;
                }
                return ok;
            } catch (const cambc::error&) {
                // The engine's dash path rules are stricter than ours: try the next landing tile.
            }
        }
        return false;
    }
    return false;
}

// Free neighbours one step down a host field, best first: safe before dangerous, straight before diagonal.
int Bot::field_steps(const cambc::distance_field& field, const cambc::bitboard& reached, vec2 from,
                     const Grid16* hint, std::array<vec2, 8>& out) {
    if (!reached.is_set(from)) return 0;
    const int here = field.get(from);
    std::array<std::pair<int, vec2>, 8> c;
    int n = 0;
    for (auto d : dirs) {
        const vec2 t{from.x + d.x, from.y + d.y};
        if (blocked_at(t) || !reached.is_set(t)) continue;
        const int v = field.get(t);
        if (v >= here) continue;
        int score = v * 64 + (danger_at(t) ? 16 : 0) + (d.x && d.y ? 1 : 0);
        if (hint) score += std::min(15, hint->at(t) - hint->at(from) + 2);
        c[static_cast<size_t>(n++)] = {score, t};
    }
    std::sort(c.begin(), c.begin() + n, [](auto a, auto b) { return a.first < b.first; });
    for (int i = 0; i < n; ++i) out[static_cast<size_t>(i)] = c[static_cast<size_t>(i)].second;
    return n;
}

// Units near us: search a small unit-aware field towards the static field's tiles K steps ahead.
int Bot::detour_steps(const Grid16& field, vec2 from, std::array<vec2, 8>& out) {
    if (!fuel_ok(25000)) return 0;
    const int here = field.at(from);
    const int aim = std::max(0, here - detour_radius);
    auto ring = empty_board();
    bool any = false;
    const int w = map_w(), h = map_h();
    const int x0 = std::max(0, from.x - detour_radius), x1 = std::min(w - 1, from.x + detour_radius);
    const int y0 = std::max(0, from.y - detour_radius), y1 = std::min(h - 1, from.y + detour_radius);
    for (int y = y0; y <= y1; ++y) {
        const std::uint16_t* row = field.v.data() + static_cast<size_t>(y * w);
        for (int x = x0; x <= x1; ++x) {
            if (row[x] != aim) continue;
            const vec2 t{x, y};
            if (blocked_at(t)) continue;
            ring.set(t);
            any = true;
        }
    }
    if (!any) return 0;
    const auto blocked = blocked_board(*walls_);
    auto start = empty_board();
    start.set(from);
    cambc::distance_field local;
    cambc::bitboard reached;
    fuel_mark("ring_done");
    local.compute_to(blocked, ring, start, reached, static_cast<std::uint16_t>(here - aim + detour_slack));
    fuel_mark("compute_done");
    if (!reached.is_set(from)) return 0;
    return field_steps(local, reached, from, &field, out);
}

// Follow the static field greedily for `steps` tiles: true when nothing we know of stands on that way;
// otherwise `blocker` is the first occupied tile on it.
static bool way_clear(const Grid16& field, vec2 from, int steps, vec2& blocker) {
    vec2 p = from;
    int d = field.at(p);
    for (int i = 0; i < steps && d > 0; ++i) {
        vec2 next = p;
        bool free_found = false, any = false;
        for (auto dir : dirs) {
            const vec2 t{p.x + dir.x, p.y + dir.y};
            if (field.at(t) != d - 1) continue;
            if (!blocked_at(t)) { next = t; free_found = true; break; }
            if (!any) { blocker = t; any = true; }
        }
        if (!free_found) return !any;
        p = next;
        --d;
    }
    return true;
}

// Walk one step down a cached static field (and dash when possible); false if no step was taken.
bool Bot::walk_field(const Grid16& field, bool allow_dash) {
    auto& me = cambc::self();
    if (moved_ || move_failed_ || !me.can_move()) return false;
    if (static_cast<int>(cambc::round_number()) < dynamic_until_) return false;
    const int here = field.at(me.position());
    if (here >= unreached || here == 0) return false;
    fuel_mark("wf_start");
    if (allow_dash && here >= 4 && try_dash(&field, nullptr, nullptr)) {
        moved_ = true;
        if (move_failed_ || !me.can_move() || field.at(me.position()) == 0) return true;
    }
    fuel_mark("wf_dashed");
    const vec2 from = me.position();
    const int h = field.at(from);
    std::array<vec2, 8> cands{};
    int n = 0;
    fuel_mark("pre_units");
    vec2 blocker{-1, -1};
    const bool crowded = !way_clear(field, from, detour_radius, blocker);
    fuel_mark("units_near");
    if (!crowded) {
        // Nothing in the way: the static field is exact.
        std::array<std::pair<int, vec2>, 8> c;
        for (auto d : dirs) {
            const vec2 t{from.x + d.x, from.y + d.y};
            if (blocked_at(t) || field.at(t) != h - 1) continue;
            c[static_cast<size_t>(n++)] = {(danger_at(t) ? 16 : 0) + (d.x && d.y ? 1 : 0), t};
        }
        std::sort(c.begin(), c.begin() + n, [](auto a, auto b) { return a.first < b.first; });
        for (int i = 0; i < n; ++i) cands[static_cast<size_t>(i)] = c[static_cast<size_t>(i)].second;
    } else {
        n = detour_steps(field, from, cands);
    }
    fuel_mark("wf_planned");
    if (n == 0) {
        if (moved_) return true;
        // The way on is a hole only a dash can cross: dash now or wait here for it.
        if (dasher_)
            for (auto d : dirs) {
                const vec2 t{from.x + d.x, from.y + d.y};
                if (in_bounds(t) && cambc::map::holes().is_set(t) && field.at(t) < h) {
                    if (try_dash(&field, nullptr, nullptr)) { moved_ = true; return true; }
                    return hold_for_dash();
                }
            }
        const int now = static_cast<int>(cambc::round_number());
        if (in_bounds(blocker) && view().lcamps.at(blocker)) {
            // Living camps plug the way and there is no short way round: clear them.
            const int g = camp_group_at_[static_cast<size_t>(tile_index(blocker))];
            if (g >= 0 && blocker_group_ < 0) {
                blocker_group_ = g;
                blocker_since_ = now;
            }
            return false;
        }
        if (in_bounds(blocker) && view().lchamps.at(blocker) && ally_at(blocker) && threats_.empty()) {
            // An ally in the way: wait a little for it to move on, then take the long way for a while.
            if (now != gap_last_ + 1) gap_start_ = now;
            gap_last_ = now;
            if (now - gap_start_ < 4) return true;
        }
        dynamic_until_ = now + 12;
        note_trail(from);
        return false;
    }
    const bool stepped = take_step(cands, n);
    fuel_mark("wf_stepped");
    return stepped || moved_;
}

// Shortest walk to any free goal tile around the units we know of.
bool Bot::step_to(const cambc::bitboard& goals_in, bool allow_dash) {
    auto& me = cambc::self();
    if (moved_ || move_failed_ || !me.can_move()) return false;
    if (!fuel_ok(35000)) return false;
    const vec2 pos = me.position();
    const int now = static_cast<int>(cambc::round_number());
    auto blocked = blocked_board(*walls_);
    auto goals = goals_in & ~blocked;
    if (goals.is_empty() || goals.is_set(pos)) return false;
    cambc::distance_field field;
    cambc::bitboard reached;
    auto here = empty_board();
    here.set(pos);
    fuel_mark("st_start");
    field.compute_to(blocked, goals, here, reached);
    const int dyn = reached.is_set(pos) ? field.get(pos) : unreached;
    fuel_mark("st_dyn");
    if (dyn >= 6 && now >= dynamic_until_) {
        // Compare with the way ignoring units: a long detour means units plug a gap.
        cambc::distance_field plain;
        cambc::bitboard plain_reached;
        plain.compute_to(*walls_, goals_in & ~*walls_, here, plain_reached);
        if (plain_reached.is_set(pos) && plain.get(pos) + 6 < dyn) {
            const int d0 = plain.get(pos);
            for (auto d : dirs) {
                const vec2 t{pos.x + d.x, pos.y + d.y};
                if (!in_bounds(t) || !plain_reached.is_set(t) || plain.get(t) >= d0) continue;
                if (view().lcamps.at(t)) {
                    // A living camp plugs the way: clear it rather than walk around.
                    const int g = camp_group_at_[static_cast<size_t>(tile_index(t))];
                    if (g >= 0 && blocker_group_ < 0) {
                        blocker_group_ = g;
                        blocker_since_ = now;
                    }
                    return false;
                }
                if (view().lchamps.at(t) || remembered_at(t)) {
                    // A champion in the gap: wait a little for it to move, then go round.
                    if (now != gap_last_ + 1) gap_start_ = now;
                    gap_last_ = now;
                    if (now - gap_start_ < 5) return true;
                    break;
                }
            }
        }
    }
    fuel_mark("st_plain");
    if (!reached.is_set(pos)) { note_trail(pos); return false; }
    if (allow_dash && dyn >= 4 && try_dash(nullptr, &field, &reached)) {
        moved_ = true;
        if (move_failed_ || !me.can_move() || goals.is_set(me.position())) return true;
    }
    const vec2 from = me.position();
    std::array<vec2, 8> cands{};
    const int n = field_steps(field, reached, from, nullptr, cands);
    if (n == 0) {
        if (moved_) return true;
        if (dasher_)
            for (auto d : dirs) {
                const vec2 t{from.x + d.x, from.y + d.y};
                if (in_bounds(t) && cambc::map::holes().is_set(t) && reached.is_set(t) && field.get(t) < field.get(from)) {
                    if (try_dash(nullptr, &field, &reached)) { moved_ = true; return true; }
                    return hold_for_dash();
                }
            }
        note_trail(from);
        return false;
    }
    return take_step(cands, n) || moved_;
}

// Waiting for the dash to cross a hole; give up after a while in case the jump is impossible.
bool Bot::hold_for_dash() {
    const int now = static_cast<int>(cambc::round_number());
    if (now != last_hold_ + 1) hold_start_ = now;
    last_hold_ = now;
    return now - hold_start_ < 12;
}

void Bot::go_home() {
    if (!walk_field(home_, true)) step_to(cambc::map::spawn_region(team::ally), true);
}

float Bot::my_dps() const {
    auto& me = cambc::self();
    float dps = me.strength();
    bool reset = false;
    float spell = 0;
    for (const auto& ab : me.abilities()) {
        if (ab.is_attack_reset()) reset = true;
        if (ab.is_damage() || ab.is_cc() || ab.is_aoe_damage()) {
            try { spell += ab.damage_amount(); } catch (const std::exception&) {}
        }
    }
    if (reset) dps *= 9.0f / 8.0f;
    if (me.champion_name() == cambc::champion_type::vendetta) dps *= 1.3f;  // kill resets
    return dps + spell / 8.0f;
}

// Damage we expect to take clearing a group, killing the strongest monsters first.
float Bot::estimate_damage(int group, float dps) const {
    auto& cache = known_[group];
    if (cache.est_dps == dps && cache.est_mult == taken_mult_ && cache.est_hp_sum == remaining_hp(group)) return cache.est_dmg;
    std::array<std::pair<float, float>, 32> monsters;  // strength, health
    size_t n = 0;
    const auto& camps = handles_[group].camps();
    for (size_t c = 0; c < camps.size(); ++c) {
        if (n == monsters.size()) break;
        const float s = camp_strength(camps[c].kind());
        if (s <= 0) continue;  // clusters do not hit back
        const float hp = known_[group].hp[c];
        if (hp <= 0) continue;
        monsters[n++] = {s, hp};
    }
    std::sort(monsters.begin(), monsters.begin() + n, [](auto a, auto b) { return a.first > b.first; });
    float t = 0, dmg = 0;
    for (size_t i = 0; i < n; ++i) {
        t += monsters[i].second / std::max(1.0f, dps);
        dmg += monsters[i].first * t;
    }
    cache.est_dps = dps;
    cache.est_mult = taken_mult_;
    cache.est_hp_sum = remaining_hp(group);
    cache.est_dmg = dmg * taken_mult_;
    return cache.est_dmg;
}

float Bot::remaining_hp(int group) const {
    float total = 0;
    for (float hp : known_[group].hp) total += hp;
    return total;
}

bool Bot::attack_group(int group) {
    auto& me = cambc::self();
    if (!me.can_attack()) return false;
    const vec2 pos = me.position();
    const int range = me.attack_range();
    const float str = me.strength();
    float best_score = -1e9f;
    vec2 best_pos{};
    bool found = false;
    for (const auto& c : handles_[group].camps()) {
        const vec2 p = c.spawn_position();
        if (dist(p, pos) > range || !view().lcamps.at(p) || !view().vis.at(p)) continue;
        const float hp = camp_hp(p);
        if (hp <= 0) continue;
        // Finish kills first, then the hardest hitters, then the weakest.
        float score = camp_strength(c.kind()) * 10 - hp / 100;
        if (hp <= str) score += 10000;
        if (score > best_score) { best_score = score; best_pos = p; found = true; }
    }
    if (!found || !live()) return false;
    try {
        auto c = cambc::vision::camp_at(best_pos);
        me.attack(c);
        attacked_ = true;
        invalidate_view();
        return true;
    } catch (const cambc::error&) {
        return false;
    }
}

// After our own basic attack, an attack reset lets us swing again.
bool Bot::reset_and_hit(int group) {
    auto& me = cambc::self();
    if (!attacked_ || cast_ || !fuel_ok(8000) || !live() || me.can_attack() || !me.can_cast()) return false;
    for (const auto& ab : me.abilities()) {
        if (!ab.is_attack_reset() || !ab.is_ready()) continue;
        cast_ = true;
        safe_use(ab);
        if (me.can_attack() && group >= 0) return attack_group(group);
        return true;
    }
    return false;
}

// Damage spells on the group we are clearing; shields while under fire. Cast before walking.
void Bot::cast_spells(int group) {
    auto& me = cambc::self();
    if (cast_ || moved_ || !live() || !me.can_cast()) return;
    for (const auto& ab : me.abilities()) {
        if (!ab.is_ready()) continue;
        if (ab.is_shield()) {
            if (incoming_ > 0 && me.health() < me.max_health() * 0.95f) {
                cast_ = true;
                safe_use(ab);
                return;
            }
            continue;
        }
        if (group < 0 || !(ab.is_damage() || ab.is_cc() || ab.is_aoe_damage())) continue;
        if (!ab.can_target_camps() && !ab.can_target_ground()) continue;
        float range = 4;
        const auto& info = ab.info();
        if (info) {
            if (auto* d = std::get_if<cambc::ability_damage>(&*info)) range = d->range;
            else if (auto* c = std::get_if<cambc::ability_cc>(&*info)) range = c->range;
            else if (auto* a = std::get_if<cambc::ability_aoe_damage>(&*info)) range = a->range;
        }
        const int r = static_cast<int>(range);
        // Area spells where they hit the most camps, single-target spells on the healthiest camp.
        vec2 best{};
        float best_hp = 0;
        int best_neighbours = -1;
        for (const auto& c : handles_[group].camps()) {
            const vec2 p = c.spawn_position();
            if (dist(p, me.position()) > r || !view().lcamps.at(p) || !view().vis.at(p)) continue;
            const float hp = camp_hp(p);
            if (hp <= 0) continue;
            int neighbours = 0;
            if (ab.is_aoe_damage())
                for (const auto& o : handles_[group].camps())
                    if (dist(o.spawn_position(), p) <= 1 && view().lcamps.at(o.spawn_position())) ++neighbours;
            if (neighbours > best_neighbours || (neighbours == best_neighbours && hp > best_hp)) {
                best_neighbours = neighbours;
                best_hp = hp;
                best = p;
            }
        }
        if (best_hp <= 0) continue;
        try {
            cast_ = true;
            if (ab.is_aoe_damage() || !ab.can_target_camps()) ab.use(best);
            else ab.use(cambc::vision::camp_at(best));
            invalidate_view();
        } catch (const cambc::error&) {
        }
        return;
    }
}

bool Bot::disengage_from_camps() {
    auto& me = cambc::self();
    std::array<cambc::bitboard, 6> by_range;
    for (auto& b : by_range) b.clear();
    bool any = false;
    for (auto p : view().camps.positions()) {
        if (dist(p, me.position()) > 6) continue;
        const int kind = camp_kind_at_[static_cast<size_t>(tile_index(p))];
        if (kind >= 0 && camp_strength(static_cast<cambc::camp_type>(kind)) <= 0) continue;
        try {
            auto c = cambc::vision::camp_at(p);
            if (!c.is_awake()) continue;
            by_range[static_cast<size_t>(std::clamp(camp_range(c.kind()), 0, 5))].set(p);
            any = true;
        } catch (const std::exception&) {}
    }
    if (!any) return false;
    auto danger = empty_board();
    for (int r = 1; r <= 5; ++r)
        if (!by_range[static_cast<size_t>(r)].is_empty()) danger |= dilate(by_range[static_cast<size_t>(r)], r);
    if (!danger.is_set(me.position())) return false;
    auto safe = empty_board();
    safe.fill();
    safe &= ~danger;
    safe &= ~static_walls_ally();
    return step_to(safe, true);
}

void Bot::report_enemy(vec2 where, int count) {
    std::array<std::uint8_t, 6> buf{};
    const auto r = static_cast<std::uint16_t>(cambc::round_number());
    buf[0] = static_cast<std::uint8_t>(r & 0xff);
    buf[1] = static_cast<std::uint8_t>(r >> 8);
    buf[2] = static_cast<std::uint8_t>(where.x);
    buf[3] = static_cast<std::uint8_t>(where.y);
    buf[4] = static_cast<std::uint8_t>(count);
    buf[5] = static_cast<std::uint8_t>(slot_ + 1);
    cambc::shared_memory.write(0, std::span<const std::uint8_t>{buf.data(), buf.size()});
}

// Fight a weaker or equal enemy force, run from a stronger one. True when the turn is spent.
bool Bot::handle_enemies() {
    auto& me = cambc::self();
    const vec2 pos = me.position();
    const int now = static_cast<int>(cambc::round_number());
    if (cambc::vision::champion_tiles(team::enemy).is_empty()) {
        // Answer an ally's report of an invader nearby.
        std::array<std::uint8_t, 6> buf{};
        cambc::shared_memory.read(0, std::span<std::uint8_t>{buf.data(), buf.size()});
        const int r = buf[0] | (buf[1] << 8);
        const vec2 where{buf[2], buf[3]};
        if (buf[5] && buf[5] != slot_ + 1 && now - r <= 3 && in_bounds(where) && dist(where, pos) <= 12
            && me.health() > me.max_health() * 0.6f && me.gem_count() < 40 && buf[4] <= 2 && incoming_ == 0) {
            auto goal = empty_board();
            add_box(goal, where, 2);
            goal &= ~static_walls_ally();
            return step_to(goal, true);
        }
        return false;
    }
    // Enemies on their own spawn heal fully every round and cannot be reached: ignore them.
    auto enemies = visible_champions(team::enemy);
    std::erase_if(enemies, [](const Sighting& e) { return cambc::map::spawn_region(team::enemy).is_set(e.pos); });
    if (enemies.empty()) return false;

    // Walking distance to each enemy: one across a hole or wall is no immediate threat.
    static const cambc::bitboard terrain = cambc::map::walls() | cambc::map::holes() | cambc::map::altars();
    cambc::distance_field field;
    cambc::bitboard reached;
    {
        auto targets = empty_board();
        for (const auto& e : enemies) targets.set(e.pos);
        auto root = empty_board();
        root.set(pos);
        field.compute_to(terrain, root, targets, reached, 12);
    }
    int near = 0;
    float their = 0;
    int dmin = 99;
    for (const auto& e : enemies) {
        const int d = dist(e.pos, pos);
        const int walk = reached.is_set(e.pos) ? field.get(e.pos) : 99;
        if (walk > 9 && d > 1) continue;
        dmin = std::min(dmin, d);
        if (walk <= 8) { ++near; their += e.hp * e.power; }
    }
    if (!near) return false;
    float ours = me.health() * (me.strength() + 0.3f * me.focus());
    for (const auto& a : visible_champions(team::ally))
        if (dist(a.pos, pos) <= 6) ours += a.hp * a.power;
    report_enemy(enemies.front().pos, near);

    const bool stronger = ours >= their * 1.15f;
    const bool home = cambc::map::spawn_region(team::ally).is_set(pos);
    if (!stronger && !home && (me.health() < me.max_health() * 0.9f || near >= 2 || ours < their * 0.8f)) {
        // Run home, hitting back first if a melee swing is free; recall once out of reach.
        if (me.attack_range() <= 1 && me.can_attack() && live())
            for (const auto& e : enemies)
                if (dist(e.pos, pos) <= 1 && view().vis.at(e.pos)) {
                    try {
                        me.attack(cambc::vision::champion_at(e.pos));
                        attacked_ = true;
                        invalidate_view();
                    } catch (const cambc::error&) {}
                    break;
                }
        if (dmin >= 6 && !moved_ && incoming_ == 0 && me.gem_count() > 0) {
            safe_recall();
            return true;
        }
        go_home();
        return true;
    }

    // Fight: open with a damage or stun spell on the weakest enemy in its range, then basic attacks.
    if (!live()) return true;
    if (!cast_ && !moved_ && me.can_cast()) {
        for (const auto& ab : me.abilities()) {
            if (cast_ || !ab.is_ready()) continue;
            const bool unit_spell = (ab.is_damage() || ab.is_cc()) && ab.can_target_enemies();
            const bool area_spell = ab.is_aoe_damage() && ab.can_target_ground();
            if (!unit_spell && !area_spell) continue;
            float spell_range = 4;
            if (const auto& info = ab.info()) {
                if (auto* d = std::get_if<cambc::ability_damage>(&*info)) spell_range = d->range;
                else if (auto* c = std::get_if<cambc::ability_cc>(&*info)) spell_range = c->range;
                else if (auto* g = std::get_if<cambc::ability_aoe_damage>(&*info)) spell_range = g->range;
            }
            const Sighting* aim = nullptr;
            for (const auto& e : enemies)
                if (dist(e.pos, pos) <= static_cast<int>(spell_range) && view().vis.at(e.pos) && (!aim || e.hp < aim->hp)) aim = &e;
            if (!aim) continue;
            try {
                if (unit_spell) ab.use(cambc::vision::champion_at(aim->pos));
                else ab.use(aim->pos);
                cast_ = true;
                invalidate_view();
            } catch (const cambc::error&) {
            }
        }
    }
    const int range = me.attack_range();
    const Sighting* best = nullptr;
    for (const auto& e : enemies)
        if (dist(e.pos, pos) <= range && view().vis.at(e.pos) && (!best || e.hp < best->hp)) best = &e;
    if (best && me.can_attack() && live()) {
        try {
            if (!cambc::vision::has_champion(best->pos)) return true;
            auto c = cambc::vision::champion_at(best->pos);
            if (!c.is_visible() || !c.is_alive()) return true;
            me.attack(c);
            attacked_ = true;
            invalidate_view();
            if (!cast_ && !me.can_attack() && me.can_cast() && fuel_ok(8000))
                for (const auto& ab : me.abilities())
                    if (ab.is_attack_reset() && ab.is_ready()) {
                        cast_ = true;
                        safe_use(ab);
                        if (me.can_attack() && cambc::vision::has_champion(best->pos)) {
                            auto again = cambc::vision::champion_at(best->pos);
                            if (again.is_visible() && again.is_alive()) me.attack(again);
                        }
                        break;
                    }
        } catch (const cambc::error&) {
        }
        return true;
    }
    if (cast_) return true;
    if (dmin <= 4 && me.health() > me.max_health() * 0.4f) {
        auto goal = empty_board();
        for (const auto& e : enemies) add_box(goal, e.pos, range);
        goal &= ~static_walls_ally();
        step_to(goal, false);
        if (range <= 1 && !attacked_ && live() && me.can_attack()) {
            for (const auto& e : enemies)
                if (dist(e.pos, me.position()) <= 1 && cambc::vision::has_champion(e.pos)) {
                    try {
                        auto c = cambc::vision::champion_at(e.pos);
                        if (c.is_visible() && c.is_alive()) { me.attack(c); attacked_ = true; }
                    } catch (const cambc::error&) {}
                    break;
                }
        }
        return true;
    }
    return false;
}

int Bot::choose_target(int& wait_out) {
    auto& me = cambc::self();
    const int now = static_cast<int>(cambc::round_number());
    const float dps = dps_;
    const float hp = me.health(), max_hp = me.max_health();
    const float margin = 120 + 0.1f * max_hp;
    int best = -1;
    float best_score = 0;
    wait_out = 0;
    // Steer clear of where allies recently saw a pack of enemies.
    const Report report = read_report();
    const bool pack = report.count >= 2 && now - report.round <= 25 && in_bounds(report.where);
    for (int i = 0; i < static_cast<int>(groups_.size()); ++i) {
        const auto& g = groups_[i];
        const auto& k = known_[i];
        float value = g.gems + 0.35f * g.gold;
        if (pack && dist(g.center, report.where) <= 8) value *= 0.3f;
        if (value <= 0) continue;
        if (k.blocked_until > now) continue;
        const int e = eta(i);
        if (e >= 9999) continue;
        const int avail = k.state == Known::respawning ? k.ready : now;
        const int wait = std::max(0, avail - (now + e));
        float factor = 1.0f;
        if (owner_[i] == 255) factor = 0.7f;
        else if (owner_[i] != slot_) factor = 0.45f;
        if (g.dist_enemy + 3 < g.dist_ally) factor *= 0.7f;
        if (k.state == Known::reward) factor *= 1.3f;
        if (i == target_) factor *= 1.3f;
        const float bound = value * factor / (e + wait + 12.0f);
        if (bound <= best_score) continue;
        float clear = 0;
        if (k.state != Known::reward) {
            // Camps never heal, so a group too strong for one trip can be worn down over several.
            const float dmg = estimate_damage(i, dps);
            clear = remaining_hp(i) / std::max(1.0f, dps);
            if (dmg > hp - margin) {
                float hits = 0;
                const auto& camps = handles_[i].camps();
                for (size_t c = 0; c < camps.size(); ++c)
                    if (k.hp[c] > 0) hits += camp_strength(camps[c].kind());
                hits *= taken_mult_;
                const float cap = max_hp - margin;
                if (hits > 0 && cap / hits < 6) continue;  // too little fighting per trip
                const float trips = std::ceil(dmg / std::max(1.0f, cap));
                if (trips > 6) continue;
                clear += trips * (10 + std::min(60, g.dist_ally));
            }
        }
        const float score = value * factor / (e + wait + clear + 12.0f);
        if (score > best_score) { best_score = score; best = i; wait_out = wait; }
    }
    return best;
}

bool Bot::handle_tribute(int next_target, int next_wait) {
    auto& me = cambc::self();
    const int gems = static_cast<int>(me.gem_count());
    if (gems <= 0) return false;
    const vec2 pos = me.position();
    const auto enemy_locks = cambc::vision::altar_lock_tiles(team::enemy);
    // Tribute when standing beside an altar.
    for (auto a : cambc::map::altars().positions()) {
        // Damage does not interrupt a tribute; just make sure we live through it.
        const float turns = std::ceil(gems / 10.0f) + 1;
        if (dist(a, pos) <= cambc::constants::submit_gems_range && !enemy_locks.is_set(a)
            && me.health() > incoming_ * taken_mult_ * turns + 50) {
            if (live()) {
                try { me.submit_gems(a); } catch (const cambc::error&) {}
            }
            return true;
        }
    }
    // Beside an enemy-locked altar: break the lock with basic attacks.
    if (!enemy_locks.is_empty() && me.can_attack() && live())
        for (auto a : enemy_locks.positions())
            if (dist(a, pos) <= std::min(cambc::constants::submit_gems_range, me.attack_range())) {
                try {
                    me.attack(cambc::vision::altar_lock_at(a));
                    attacked_ = true;
                    return true;
                } catch (const cambc::error&) {
                }
                break;
            }
    const int d_altar = altar_.at(pos);
    if (d_altar >= unreached) return false;
    const int late = static_cast<int>(cambc::constants::max_rounds) - static_cast<int>(cambc::round_number());
    // Bank sooner when enemies are about: carried gems go to whoever kills us.
    const Report report = read_report();
    const bool danger = static_cast<int>(cambc::round_number()) - report.round <= 30 && in_bounds(report.where)
        && dist(report.where, pos) <= 16;
    const bool go = gems >= 40 || next_target < 0 || next_wait > d_altar || d_altar <= 4
        || (gems >= 15 && d_altar <= 10) || (danger && gems >= 10) || late < d_altar + 30;
    DBG(slot_, "tribute? go " << go << " d_altar " << d_altar << " gems " << gems);
    if (!go) return false;
    if (enemy_locks.is_empty() && walk_field(altar_, true)) return true;
    DBG(slot_, "tribute walk_field failed; moved " << moved_ << " mf " << move_failed_ << " dyn_until " << dynamic_until_);
    auto goal = altar_tiles_;
    for (auto a : enemy_locks.positions()) {
        auto locked = empty_board();
        add_box(locked, a, cambc::constants::submit_gems_range);
        goal &= ~locked;
    }
    return step_to(goal, true);
}

void Bot::farm(int group) {
    auto& me = cambc::self();
    pos_ = me.position();
    const auto& k = known_[group];
    const int e = eta(group);
    // Far away: follow the cached field.
    if (e > 7 && walk_field(group_field_[group], true)) return;

    if (k.state == Known::reward) {
        // Stand on a spawn tile until the capture completes.
        const auto& spawns = group_tiles_[group];
        const int now = static_cast<int>(cambc::round_number());
        if (spawns.is_set(me.position())) {
            if (capture_group_ != group) { capture_group_ = group; capture_since_ = now; }
            if (now - capture_since_ > cambc::constants::capture_time + 6) {
                known_[group].blocked_until = now + 60;  // contested: try elsewhere
                capture_group_ = -1;
            }
            return;
        }
        step_to(spawns, false);
        return;
    }
    if (k.state == Known::respawning) {
        // Wait nearby without standing on the spawn tiles (that would delay the respawn).
        // Any camp spawn tile we park on delays that group's respawn too: wait off all of them.
        auto near = dilate(group_tiles_[group], 2) & ~group_tiles_[group];
        near &= ~static_walls_ally();
        if (!(near & ~all_camp_tiles_).is_empty()) near &= ~all_camp_tiles_;
        if (!near.is_set(me.position())) step_to(near, true);
        return;
    }

    fuel_mark("farm_pre-cast");
    cast_spells(group);
    fuel_mark("farm_post-cast");
    if (attack_group(group)) {
        reset_and_hit(group);
        return;
    }
    // Approach: tiles within attack range of any living camp in the group.
    const int r = std::max(1, me.attack_range() - approach_shrink_);
    // Living (or unseen) camp tiles of the group, grown by our range.
    const auto living = group_tiles_[group] & ~(view().visb & ~view().camps);
    auto goal = dilate(living, r) & ~static_walls_ally();
    if (goal.is_set(me.position()) && approach_shrink_ < me.attack_range() - 1) ++approach_shrink_;
    step_to(goal, e > 3);
    if (me.attack_range() <= 1 && attack_group(group)) reset_and_hit(group);
}


int g_turns = 0, g_overflows = 0;
struct FuelTrace {
    int slot;
    ~FuelTrace() {
        if (kDebug && cambc::round_number() != g_turn_round) ++g_overflows;
        fuel_mark("end");
        fuel_flush(slot);
    }
};

void Bot::play_turn() {
    fuel_mark("start");
    if (kDebug) {
        ++g_turns;
        if (cambc::round_number() % 500 == 499) std::cout << "OVF s" << slot_ << " " << g_overflows << "/" << g_turns << std::endl;
    }
    FuelTrace trace{slot_};
    g_turn_round = cambc::round_number();
    pos_ = cambc::self().position();
    if (!ready_) initialise();
    auto& me = cambc::self();
    moved_ = attacked_ = cast_ = move_failed_ = false;
    invalidate_view();
    if (kDebug && g_turn_round % 20 == 0)
        std::cout << "T r" << g_turn_round << " s" << slot_ << " (" << pos_.x << "," << pos_.y << ") tgt " << target_
                  << " gems " << me.gem_count() << " hp " << static_cast<int>(me.health()) << (me.is_alive() ? "" : " DEAD")
                  << (me.is_recalling() ? " REC" : "") << (me.is_submitting_gems() ? " SUB" : "") << std::endl;

    if (!me.is_alive()) {
        buy_items();
        fill_fields(40000);
        return;
    }
    (void)view();
    fuel_mark("view");
    update_knowledge();
    fuel_mark("knowledge");
    const bool at_spawn = cambc::map::spawn_region(team::ally).is_set(me.position());
    if (at_spawn || me.champion_name() == cambc::champion_type::tavi) buy_items();
    if (me.is_recalling() || me.is_submitting_gems() || me.is_teleporting()) {
        fill_fields(40000);
        return;
    }
    // Strength of awake camps that can reach us.
    incoming_ = 0;
    exit_steps_ = 0;
    threats_.clear();
    fuel_mark("occupied");
    for (auto p : view().camps.positions()) {
        const int d = dist(p, me.position());
        if (d > 6) continue;
        const int kind = camp_kind_at_[static_cast<size_t>(tile_index(p))];
        if (kind >= 0 && camp_strength(static_cast<cambc::camp_type>(kind)) <= 0) continue;  // clusters never hit
        if (kind < 0) continue;
        const auto type = static_cast<cambc::camp_type>(kind);
        const int r = camp_range(type);
        if (d > r + 1) continue;
        try {
            if (!cambc::vision::camp_at(p).is_awake()) continue;
            threats_.push_back({p, r});
            if (d <= r) {
                incoming_ += camp_strength(type);
                exit_steps_ = std::max(exit_steps_, r + 1 - d);
            }
        } catch (const std::exception&) {}
    }
    {
        const auto counts = me.item_count();
        taken_mult_ = 1.0f;
        if (counts[cambc::item_type::jungle_emblem_of_strength] + counts[cambc::item_type::jungle_emblem_of_health]
            + counts[cambc::item_type::jungle_emblem_of_focus] > 0) taken_mult_ *= 0.7f;
        if (counts[cambc::item_type::durability_emblem_of_health] + counts[cambc::item_type::durability_emblem_of_strength]
            + counts[cambc::item_type::durability_emblem_of_focus] > 0) taken_mult_ *= 0.9f;
        dps_ = my_dps();
        if (counts[cambc::item_type::monster_emblem_of_strength] + counts[cambc::item_type::monster_emblem_of_health]
            + counts[cambc::item_type::monster_emblem_of_focus] > 0) dps_ *= 1.1f;
    }
    fuel_mark("threats");
    if (handle_enemies()) { DBG(slot_, "enemies " << me.position().x << "," << me.position().y); return; }
    if (!fuel_ok(30000)) { return; }

    const float hp = me.health(), max_hp = me.max_health();
    // Just arrived home hurt: stay this turn, spawn heals at turn end.
    if (!at_spawn) spawn_wait_ = 0;
    else if (hp < max_hp * 0.95f && spawn_wait_ < 2) { ++spawn_wait_; return; }
    // Badly hurt: leave camp range, then recall.
    if (!at_spawn && (hp < max_hp * 0.2f || (incoming_ > 0 && hp < incoming_ * taken_mult_ * (exit_steps_ + 2) + 40))) {
        if (disengage_from_camps()) { return; }
        if (incoming_ == 0 && !moved_) {
            safe_recall();
            return;
        }
    }

    const int now = static_cast<int>(cambc::round_number());
    if (target_ < 0 || now - last_choice_ >= 3 || known_[target_].state == Known::respawning) {
        int wait = 0;
        pos_ = me.position();
        const int target = choose_target(wait);
        if (target != target_) approach_shrink_ = 0;
        target_ = target;
        target_wait_ = wait;
        last_choice_ = now;
    }
    const int target = target_;
    fuel_mark("chosen");
    const int wait = target_wait_;

    if (blocker_group_ >= 0) {
        // Living camps stand between us and where we want to go: cut through them.
        if (known_[blocker_group_].state != Known::alive || now - blocker_since_ > 40) blocker_group_ = -1;
        else {
            farm(blocker_group_);
            return;
        }
    }
    if (handle_tribute(target, wait)) { DBG(slot_, "tribute " << me.position().x << "," << me.position().y << " d_altar " << altar_.at(me.position()) << " moved " << moved_ << " mf " << move_failed_); return; }

    const int home_d = home_.at(me.position());
    if (target < 0 || wait > 12) {
        // Nothing worth doing soon: go shop and heal.
        if (!at_spawn && (me.gold_count() >= 60 || hp < max_hp * 0.8f) && incoming_ == 0 && me.gem_count() == 0) {
            if (home_d <= 5) go_home();
            else if (!moved_) safe_recall();
            return;
        }
        if (target < 0) {
            if (!at_spawn && hp < max_hp * 0.9f && incoming_ == 0 && !moved_) safe_recall();
            fill_fields(40000);
            return;
        }
    }
    // A full purse is wasted in the field: go home and spend it (Tavi shops anywhere).
    if (!at_spawn && me.gold_count() >= 250 && me.gem_count() == 0 && incoming_ == 0 && !cant_spend_
        && me.champion_name() != cambc::champion_type::tavi
        && (known_[target].state != Known::alive || eta(target) > 6) && remaining_hp(target) >= groups_[target].hp * 0.99f) {
        if (home_d <= 8) go_home();
        else if (!moved_) safe_recall();
        return;
    }
    // Heal before a fight we cannot finish at this health.
    if (!at_spawn && hp < max_hp * 0.5f && incoming_ == 0 && known_[target].state != Known::reward) {
        const float dmg = estimate_damage(target, dps_);
        if (dmg > hp - 100) {
            if (home_d <= 5) go_home();
            else if (!moved_) safe_recall();
            return;
        }
    }
    DBG(slot_, "farm " << target << " at " << me.position().x << "," << me.position().y);
    farm(target);
    DBG(slot_, " -> " << me.position().x << "," << me.position().y << " moved " << moved_ << " mf " << move_failed_);
    fill_fields(50000);
}
