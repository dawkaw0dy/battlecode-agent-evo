// Split-farm champion: each champion farms its own region of camp groups, tributes at the
// nearest altar and only groups up when an ally reports an invader in shared memory.
#include <cambc.h>
#include "bot.h"
#include "trace.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <variant>
#include <vector>

using cambc::vec2;
using cambc::team;

// Fight tuning switches.
constexpr bool kUseMemo = false;      // count enemies seen in the last rounds
constexpr bool kFleeHpGate = true;    // keep retreating out of sight only while hurt
constexpr bool kTradeAlways = false;  // trade blows whenever fleeing cannot avoid them
constexpr int kHelpRadius = 12, kHelpCount = 2;  // answer ally reports of this many enemies this close
constexpr bool kTradeWhenDoomed = false;  // trade when home is out of reach anyway
constexpr bool kFleeStep = true;      // threat-aware flee steps instead of the home path
constexpr bool kAvoidSpawnCampers = true;  // keep out of reach of enemies on their spawn
// Spell tuning switches.
constexpr bool kSpellFocusCamp = true;  // single-target camp spells on the basic-attack target
constexpr bool kShieldEarly = true;     // shield as soon as something can hit us
constexpr bool kStunFlee = false;        // ranged stunners stun the nearest chaser instead of stepping
constexpr bool kStunChannel = false;     // stun/damage spells prefer enemies channeling a tribute or recall
constexpr bool kShieldFlee = false;      // shield while running away


namespace {

constexpr std::array<vec2, 8> dirs{vec2{0, 1}, vec2{1, 1}, vec2{1, 0}, vec2{1, -1},
    vec2{0, -1}, vec2{-1, -1}, vec2{-1, 0}, vec2{-1, 1}};
constexpr int unreached = 65535;

int dist(vec2 a, vec2 b) { return static_cast<int>(a.chebyshev_distance(b)); }
int map_w() { static const int w = cambc::map::width(); return w; }
int map_h() { static const int h = cambc::map::height(); return h; }
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
        g_view.valid = true;
    }
    return g_view;
}

// Tiles in reach of enemies standing on their own spawn (they heal there and cannot be fought),
// rebuilt each turn; avoided unless we already stand in it.
cambc::bitboard g_hotb;
Grid8 g_hot;
bool g_hot_any = false;
bool hot_avoid() { return g_hot_any && !g_hot.at(cambc::self().position()); }

bool blocked_at(vec2 p) {
    if (!in_bounds(p)) return true;
    const auto& v = view();
    if (p == cambc::self().position()) return false;
    if (hot_avoid() && g_hot.at(p)) return true;
    return walls_local().at(p) || v.lcamps.at(p) || v.lchamps.at(p) || v.ltele.at(p);
}

cambc::bitboard blocked_board(const cambc::bitboard& walls) {
    const auto& v = view();
    auto blocked = walls | v.champs | v.camps | v.teleports;
    if (hot_avoid()) blocked |= g_hotb;
    blocked.set(cambc::self().position(), false);
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

std::vector<Sighting> visible_champions(team side) {
    std::vector<Sighting> out;
    const vec2 me = cambc::self().position();
    for (auto p : cambc::vision::champion_tiles(side).positions()) {
        if (p == me) continue;
        try {
            auto c = cambc::vision::champion_at(p);
            if (!c.is_alive()) continue;
            Sighting s{p, c.health(), c.max_health(), c.strength() + 0.3f * c.focus(), c.attack_range()};
            try { s.gems = static_cast<int>(c.gem_count()); } catch (const cambc::error&) {}
            try { s.type = static_cast<int>(c.champion_name()); } catch (const cambc::error&) {}
            try { s.stunned = c.buffs().has_stun(); } catch (const cambc::error&) {}
            if (kStunChannel) {
                try { s.channeling = c.is_submitting_gems() || c.is_recalling(); } catch (const cambc::error&) {}
            }
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
    if (dasher_) {
        f.compute(static_walls_ally(), cambc::map::spawn_region(team::ally), seen);
        home_walk_.load(f);
    } else {
        home_walk_ = home_;
    }
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

// Dash two tiles straight along a field when it saves two steps. Field values come from a
// local grid or from a host field with its reached mask.
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
        // Landing tiles two steps away that gain at least two steps, best first.
        std::array<std::pair<int, vec2>, 16> cands;
        size_t n = 0;
        for (int dx = -2; dx <= 2; ++dx)
            for (int dy = -2; dy <= 2; ++dy) {
                if (std::max(std::abs(dx), std::abs(dy)) != 2) continue;
                const vec2 t{pos.x + dx, pos.y + dy};
                if (!in_bounds(t) || blocked_at(t)) continue;
                // Some first step must be free of walls and units (holes may be jumped).
                bool path = false;
                for (int sx = -1; sx <= 1 && !path; ++sx)
                    for (int sy = -1; sy <= 1 && !path; ++sy) {
                        if (!sx && !sy) continue;
                        const vec2 m{pos.x + sx, pos.y + sy};
                        if (dist(m, t) != 1 || !in_bounds(m)) continue;
                        if (hard_walls_local().at(m) || view().lchamps.at(m) || view().lcamps.at(m)) continue;
                        path = true;
                    }
                if (!path) continue;
                const int d = value(t);
                if (d <= here - 2) cands[n++] = {d, t};
            }
        std::sort(cands.begin(), cands.begin() + n, [](auto a, auto b) { return a.first < b.first; });
        for (size_t i = 0; i < n && i < 4; ++i) {
            if (!live()) return false;
            try {
                const bool ok = ab.use(cands[i].second);
                cast_ = true;
                invalidate_view();
                if (!ok) move_failed_ = true;
                return ok;
            } catch (const cambc::error&) {
                // The engine's dash path rules are stricter than ours: try the next landing tile.
            }
        }
        return false;
    }
    return false;
}

// Walk one step down a static field (and dash when possible); false if no free improving step.
bool Bot::walk_field(const Grid16& field, bool allow_dash) {
    auto& me = cambc::self();
    if (moved_ || move_failed_ || !me.can_move()) return false;
    // After the static way was blocked, route around units for a while instead of flip-flopping.
    if (static_cast<int>(cambc::round_number()) < dynamic_until_) return false;
    const vec2 pos = me.position();
    const int here = field.at(pos);
    if (here >= unreached || here == 0) return false;
    auto best_walk = [&](vec2 from) {
        vec2 best = from;
        int best_d = field.at(from);
        for (auto d : dirs) {
            const vec2 t{from.x + d.x, from.y + d.y};
            if (blocked_at(t)) continue;
            const int fd = field.at(t);
            if (fd < best_d) { best_d = fd; best = t; }
        }
        return best;
    };
    vec2 best = best_walk(pos);
    if ((allow_dash && here >= 4) || (dasher_ && best == pos)) {
        if (try_dash(&field, nullptr, nullptr)) {
            moved_ = true;
            if (move_failed_ || !me.can_move()) return true;
            best = best_walk(me.position());
        }
    }
    const vec2 from = me.position();
    if (best == from) {
        if (moved_) return true;
        // The way on is a hole only a dash can cross: wait here for the dash to come back.
        if (dasher_)
            for (auto d : dirs) {
                const vec2 t{from.x + d.x, from.y + d.y};
                if (in_bounds(t) && cambc::map::holes().is_set(t) && field.at(t) < field.at(from)) return hold_for_dash();
            }
        dynamic_until_ = static_cast<int>(cambc::round_number()) + 12;
        return false;
    }
    if (!live()) return moved_;
    moved_ = true;
    bool ok = false;
    try { ok = me.move_absolute(best); } catch (const cambc::error&) {}
    invalidate_view();
    if (!ok) move_failed_ = true;
    return ok;
}

// Shortest walk to any free goal tile around the units we can see.
bool Bot::step_to(const cambc::bitboard& goals_in, bool allow_dash) {
    auto& me = cambc::self();
    if (moved_ || move_failed_ || !me.can_move()) return false;
    if (!fuel_ok(35000)) return false;
    const vec2 pos = me.position();
    auto blocked = blocked_board(*walls_);
    auto goals = goals_in & ~blocked;
    if (goals.is_empty() || goals.is_set(pos)) return false;
    cambc::distance_field field;
    cambc::bitboard reached;
    auto here = empty_board();
    here.set(pos);
    field.compute_to(blocked, goals, here, reached);
    const int dyn = reached.is_set(pos) ? field.get(pos) : unreached;
    if (dyn >= 6) {
        // Compare with the way ignoring units: a long detour means units plug a gap.
        cambc::distance_field plain;
        cambc::bitboard plain_reached;
        plain.compute_to(*walls_, goals, here, plain_reached);
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
                        blocker_since_ = static_cast<int>(cambc::round_number());
                    }
                    return false;
                }
                if (view().lchamps.at(t)) {
                    // A champion in the gap: wait a little for it to move, then go round.
                    const int now = static_cast<int>(cambc::round_number());
                    if (now != gap_last_ + 1) gap_start_ = now;
                    gap_last_ = now;
                    if (now - gap_start_ < 5 && !urgent_) return true;
                    break;
                }
            }
            if (dyn >= unreached) {
                field = plain;
                reached = plain_reached;
            }
        }
        if (dyn >= unreached && !plain_reached.is_set(pos)) return false;
    }
    if (!reached.is_set(pos)) return false;
    auto best_walk = [&](vec2 from, int& best_d) {
        vec2 best = from;
        best_d = reached.is_set(from) ? field.get(from) : unreached;
        for (auto d : dirs) {
            const vec2 t{from.x + d.x, from.y + d.y};
            if (blocked_at(t) || !reached.is_set(t)) continue;
            const int fd = field.get(t);
            if (fd < best_d) { best_d = fd; best = t; }
        }
        return best;
    };
    int walk_d = unreached;
    vec2 best = best_walk(pos, walk_d);
    if ((allow_dash && field.get(pos) >= 4) || (dasher_ && best == pos)) {
        if (try_dash(nullptr, &field, &reached)) {
            moved_ = true;
            if (move_failed_ || !me.can_move()) return true;
            if (goals.is_set(me.position())) return true;
            best = best_walk(me.position(), walk_d);
        }
    }
    const vec2 from = me.position();
    if (best == from) {
        if (moved_) return true;
        if (dasher_)
            for (auto d : dirs) {
                const vec2 t{from.x + d.x, from.y + d.y};
                if (in_bounds(t) && cambc::map::holes().is_set(t) && reached.is_set(t) && field.get(t) < field.get(from)) return hold_for_dash();
            }
        return false;
    }
    if (!live()) return moved_;
    moved_ = true;
    bool ok = false;
    try { ok = me.move_absolute(best); } catch (const cambc::error&) {}
    invalidate_view();
    if (!ok) move_failed_ = true;
    return ok;
}

// Waiting for the dash to cross a hole; give up after a while in case the jump is impossible.
bool Bot::hold_for_dash() {
    const int now = static_cast<int>(cambc::round_number());
    if (now != last_hold_ + 1) hold_start_ = now;
    last_hold_ = now;
    return now - hold_start_ < 12;
}

bool Bot::dash_ready() const {
    for (const auto& ab : cambc::self().abilities())
        if (ab.is_dash() && ab.is_ready()) return true;
    return false;
}

void Bot::go_home() {
    urgent_ = true;
    // Without a dash ready, never wait beside a hole: walk the long way round.
    const bool w = (dasher_ && !dash_ready()) ? walk_field(home_walk_, false) : walk_field(home_, true);
    bool st = false;
    if (!w) st = step_to(cambc::map::spawn_region(team::ally), true);
    TRACE("  home w" << w << " st" << st << " hd" << home_.at(cambc::self().position()) << " dyn" << dynamic_until_ << " mv" << moved_ << " mf" << move_failed_ << " cm" << cambc::self().can_move() << " fuel" << cambc::fuel_remaining());
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
            if (incoming_ > 0 && (kShieldEarly || me.health() < me.max_health() * 0.95f)) {
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
        float best_focus = -1e9f, spell_dmg = 0;
        if (kSpellFocusCamp && !ab.is_aoe_damage()) {
            try { spell_dmg = ab.damage_amount(); } catch (const std::exception&) {}
        }
        for (const auto& c : handles_[group].camps()) {
            const vec2 p = c.spawn_position();
            if (dist(p, me.position()) > r || !view().lcamps.at(p) || !view().vis.at(p)) continue;
            const float hp = camp_hp(p);
            if (hp <= 0) continue;
            if (kSpellFocusCamp && !ab.is_aoe_damage()) {
                // Same order as the basic attack: finish kills, then the hardest hitters, then the weakest.
                float score = camp_strength(c.kind()) * 10 - hp / 100;
                if (hp <= spell_dmg) score += 10000;
                if (score > best_focus) { best_focus = score; best_hp = hp; best = p; }
                continue;
            }
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

// Spell range from the ability's info (4 when unknown).
static int spell_range(const cambc::ability& ab) {
    float r = 4;
    if (const auto& info = ab.info()) {
        if (auto* d = std::get_if<cambc::ability_damage>(&*info)) r = d->range;
        else if (auto* c = std::get_if<cambc::ability_cc>(&*info)) r = c->range;
        else if (auto* g = std::get_if<cambc::ability_aoe_damage>(&*info)) r = g->range;
    }
    return static_cast<int>(r);
}

// Attack the visible enemy champion at p; true on success.
bool Bot::hit_champion(vec2 p) {
    auto& me = cambc::self();
    if (!live() || !me.can_attack()) return false;
    try {
        if (!cambc::vision::has_champion(p)) return false;
        auto c = cambc::vision::champion_at(p);
        if (!c.is_visible() || !c.is_alive() || !c.is_enemy()) return false;
        me.attack(c);
        attacked_ = true;
        invalidate_view();
        return true;
    } catch (const cambc::error&) {
        return false;
    }
}

// Best enemy to hit from here: one we can finish now, then gem carriers, then the lowest health.
int Bot::pick_victim(const std::vector<Sighting>& enemies, int range, float burst) const {
    const vec2 pos = cambc::self().position();
    int best = -1;
    float best_score = -1e9f;
    for (size_t i = 0; i < enemies.size(); ++i) {
        const auto& e = enemies[i];
        if (dist(e.pos, pos) > range || !view().vis.at(e.pos)) continue;
        float score = -e.hp + 3.0f * static_cast<float>(e.gems);
        if (e.hp <= burst) score += 5000;
        if (e.stunned) score += 150;
        if (e.channeling) score += 400;
        if (score > best_score) { best_score = score; best = static_cast<int>(i); }
    }
    return best;
}

// Damage and stun spells on enemy champions in range; true when one was cast.
bool Bot::fight_spells(const std::vector<Sighting>& enemies, float burst) {
    auto& me = cambc::self();
    if (cast_ || moved_ || !live() || !me.can_cast()) return false;
    for (const auto& ab : me.abilities()) {
        if (!ab.is_ready()) continue;
        if (ab.is_shield()) {
            if (me.health() < me.max_health() * 0.9f || (kShieldEarly && threatened_)) { cast_ = true; safe_use(ab); return true; }
            continue;
        }
        if (ab.is_heal()) continue;
        const bool unit_spell = (ab.is_damage() || ab.is_cc()) && ab.can_target_enemies();
        const bool area_spell = ab.is_aoe_damage() && ab.can_target_ground();
        if (!unit_spell && !area_spell) continue;
        const int r = spell_range(ab);
        int aim = -1;
        if (unit_spell) {
            aim = pick_victim(enemies, r, burst);
        } else {
            // Area spells where they hit the most champions.
            int best_hits = 0;
            for (size_t i = 0; i < enemies.size(); ++i) {
                if (dist(enemies[i].pos, me.position()) > r || !view().vis.at(enemies[i].pos)) continue;
                int hits = 0;
                for (const auto& o : enemies) hits += dist(o.pos, enemies[i].pos) <= 1 ? 1 : 0;
                if (hits > best_hits) { best_hits = hits; aim = static_cast<int>(i); }
            }
        }
        if (aim < 0) continue;
        try {
            const vec2 p = enemies[static_cast<size_t>(aim)].pos;
            if (unit_spell) {
                if (!cambc::vision::has_champion(p)) continue;
                auto c = cambc::vision::champion_at(p);
                if (!c.is_visible() || !c.is_alive()) continue;
                ab.use(c);
            } else {
                ab.use(p);
            }
            cast_ = true;
            invalidate_view();
            return true;
        } catch (const cambc::error&) {
        }
    }
    return false;
}

// Basic attack (with an attack reset for a second swing) on the best victim in range.
bool Bot::fight_attack(const std::vector<Sighting>& enemies, float burst) {
    auto& me = cambc::self();
    if (!me.can_attack()) return false;
    const int v = pick_victim(enemies, me.attack_range(), burst);
    if (v < 0) return false;
    const vec2 p = enemies[static_cast<size_t>(v)].pos;
    const bool hit = hit_champion(p);
    TRACE("   attack " << p.x << "," << p.y << " hp" << int(enemies[static_cast<size_t>(v)].hp) << " ok" << hit << " str" << me.strength());
    if (!hit) return false;
    if (!cast_ && live() && !me.can_attack() && me.can_cast() && fuel_ok(8000))
        for (const auto& ab : me.abilities())
            if (ab.is_attack_reset() && ab.is_ready()) {
                cast_ = true;
                safe_use(ab);
                if (me.can_attack()) hit_champion(p);
                break;
            }
    return true;
}

// One step (or a dash) that keeps out of the enemies' reach, homewards when that is equal.
bool Bot::flee_step(const std::vector<Sighting>& threats) {
    auto& me = cambc::self();
    if (moved_ || move_failed_ || !me.can_move() || !live()) return false;
    const vec2 pos = me.position();
    auto cost = [&](vec2 t) {
        int danger = 0;
        for (const auto& e : threats) {
            const int reach = e.range <= 1 ? 2 : e.range;  // melee walk and hit; ranged must stand still
            if (dist(t, e.pos) <= reach) danger += 1;
        }
        const int h = home_walk_.at(t);
        return danger * 1000 + (h >= unreached ? 500 : h);
    };
    vec2 best = pos;
    int best_cost = cost(pos);
    for (auto d : dirs) {
        const vec2 t{pos.x + d.x, pos.y + d.y};
        if (blocked_at(t)) continue;
        const int c = cost(t);
        if (c < best_cost) { best_cost = c; best = t; }
    }
    // Still in reach after the best step: a dash may get clear.
    if (best_cost >= 1000 && !cast_ && me.can_cast() && fuel_ok(15000))
        for (const auto& ab : me.abilities()) {
            if (!ab.is_dash() || !ab.is_ready()) continue;
            std::array<std::pair<int, vec2>, 16> cands;
            size_t n = 0;
            for (int dx = -2; dx <= 2; ++dx)
                for (int dy = -2; dy <= 2; ++dy) {
                    if (std::max(std::abs(dx), std::abs(dy)) != 2) continue;
                    const vec2 t{pos.x + dx, pos.y + dy};
                    if (!in_bounds(t) || blocked_at(t) || hard_walls_local().at(t) || cambc::map::holes().is_set(t)) continue;
                    bool path = false;
                    for (int sx = -1; sx <= 1 && !path; ++sx)
                        for (int sy = -1; sy <= 1 && !path; ++sy) {
                            const vec2 m{pos.x + sx, pos.y + sy};
                            if ((!sx && !sy) || !in_bounds(m) || dist(m, t) != 1) continue;
                            if (hard_walls_local().at(m) || view().lchamps.at(m) || view().lcamps.at(m)) continue;
                            path = true;
                        }
                    const int c = cost(t);
                    if (path && c < best_cost) cands[n++] = {c, t};
                }
            std::sort(cands.begin(), cands.begin() + n, [](auto a, auto b) { return a.first < b.first; });
            for (size_t i = 0; i < n && i < 4; ++i) {
                if (!live()) return false;
                try {
                    const bool ok = ab.use(cands[i].second);
                    cast_ = true;
                    moved_ = true;
                    invalidate_view();
                    if (!ok) move_failed_ = true;
                    if (ok) return true;
                    break;
                } catch (const cambc::error&) {
                }
            }
            break;
        }
    if (best == pos || !me.can_move() || move_failed_) return false;
    moved_ = true;
    bool ok = false;
    try { ok = me.move_absolute(best); } catch (const cambc::error&) {}
    invalidate_view();
    if (!ok) move_failed_ = true;
    return ok;
}

// Run for home; recall once nothing can interrupt it.
void Bot::retreat(int dmin, const std::vector<Sighting>& threats) {
    auto& me = cambc::self();
    const vec2 pos = me.position();
    const bool home = cambc::map::spawn_region(team::ally).is_set(pos);
    if (home) return;
    if (dmin >= 6 && !moved_ && incoming_ == 0 && home_.at(pos) > 4) {
        safe_recall();
        return;
    }
    if (kFleeStep && dmin <= 4 && flee_step(threats)) return;
    go_home();
}

// Fight a weaker or equal enemy force, run from a stronger one. True when the turn is spent.
bool Bot::handle_enemies() {
    auto& me = cambc::self();
    const vec2 pos = me.position();
    const int now = static_cast<int>(cambc::round_number());
    const bool home = cambc::map::spawn_region(team::ally).is_set(pos);
    const float hp_frac = me.health() / std::max(1.0f, me.max_health());
    std::vector<Sighting> enemies;
    if (!cambc::vision::champion_tiles(team::enemy).is_empty()) {
        enemies = visible_champions(team::enemy);
        std::vector<Sighting> parked;
        for (const auto& e : enemies)
            if (cambc::map::spawn_region(team::enemy).is_set(e.pos)) parked.push_back(e);
        // Enemies on their own spawn heal fully every round and cannot be reached: ignore them.
        std::erase_if(enemies, [](const Sighting& e) { return cambc::map::spawn_region(team::enemy).is_set(e.pos); });
        // Standing in their reach with nobody else to hit: step out of it.
        if (kAvoidSpawnCampers && g_hot_any && g_hot.at(pos) && !home) {
            bool target = false;
            for (const auto& e : enemies) target |= dist(e.pos, pos) <= me.attack_range();
            TRACE("r" << now << " " << static_cast<int>(me.champion_name()) << " p" << pos.x << "," << pos.y << " HOT hp" << int(me.health()) << " tgt" << target);
            if (!target && flee_step(parked)) return true;
        }
    }
    if (enemies.empty()) {
        // Keep running from a force we just fled: it is probably still behind us.
        if (now < flee_until_ && !home && (!kFleeHpGate || hp_frac < 0.9f)) {
            if (!moved_ && incoming_ == 0 && home_.at(pos) > 4) safe_recall();
            else go_home();
            return true;
        }
        // Answer an ally's report of an invader nearby.
        const Report r = read_report();
        if (r.round > -1000 && r.slot != slot_ && now - r.round <= 3 && in_bounds(r.where) && dist(r.where, pos) <= kHelpRadius
            && hp_frac > 0.6f && me.gem_count() < 40 && r.count <= kHelpCount && incoming_ == 0) {
            auto goal = empty_board();
            add_box(goal, r.where, 2);
            goal &= ~static_walls_ally();
            return step_to(goal, true);
        }
        return false;
    }

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
    int near = 0, dmin = 99;
    float their_hp = 0, their_pow = 0;
    bool hit_anyway = false;  // some enemy can hit us next turn even if we step away
    std::vector<Sighting> threats;
    for (const auto& e : enemies) {
        const int d = dist(e.pos, pos);
        const int walk = reached.is_set(e.pos) ? field.get(e.pos) : 99;
        if (walk > 9 && d > 1) continue;
        dmin = std::min(dmin, d);
        threats.push_back(e);
        if (walk <= 8 || d <= 1) {
            ++near;
            their_hp += e.hp;
            their_pow += std::max(20.0f, e.power);
        }
        if (e.range <= 1 ? d <= 1 : d <= e.range - 1) hit_anyway = true;
    }
    threatened_ = hit_anyway;
    for (const auto& e : enemies)
        if (e.type >= 0 && e.type < static_cast<int>(enemy_memo_.size()))
            enemy_memo_[static_cast<size_t>(e.type)] = Memo{e.pos, e.hp, e.power, now};
    if (!near) return false;
    // Enemies seen in the last few rounds and now out of sight are probably still close.
    for (size_t t = 0; t < enemy_memo_.size(); ++t) {
        const auto& m = enemy_memo_[t];
        if (!kUseMemo || now - m.round > 4 || now == m.round || dist(m.pos, pos) > 8) continue;
        bool seen = false;
        for (const auto& e : enemies) seen |= e.type == static_cast<int>(t);
        if (seen) continue;
        their_hp += m.hp;
        their_pow += std::max(20.0f, m.power);
    }
    float our_hp = me.health(), our_pow = std::max(20.0f, me.strength() + 0.3f * me.focus());
    for (const auto& a : visible_champions(team::ally))
        if (dist(a.pos, pos) <= 6) { our_hp += a.hp; our_pow += std::max(20.0f, a.power); }
    report_enemy(threats.front().pos, near);

    const float ratio = (our_hp * our_pow) / std::max(1.0f, their_hp * their_pow);
    // Damage we can deal this turn: two swings with a reset plus a ready spell.
    float burst = me.strength();
    for (const auto& ab : me.abilities()) {
        if (!ab.is_ready()) continue;
        if (ab.is_attack_reset()) burst += me.strength();
        else if (ab.is_damage() || ab.is_cc() || ab.is_aoe_damage()) {
            try { burst += ab.damage_amount(); } catch (const std::exception&) {}
        }
    }
    const Sighting* weakest = nullptr;
    for (const auto& e : threats)
        if (!weakest || e.hp < weakest->hp) weakest = &e;
    const bool finishable = weakest && weakest->hp <= burst * 1.2f && dist(weakest->pos, pos) <= 4;
    enum { engage, hold, flee } mode = hold;
    if (ratio >= 1.3f || (finishable && ratio >= 0.6f)) mode = engage;
    else if (ratio < 0.85f) mode = flee;
    if (hp_frac < 0.3f && !finishable) mode = flee;
    if (mode == hold && now < flee_until_) mode = flee;  // no turning back halfway home
    if (home) mode = mode == flee ? hold : mode;

    bool in_range = false;
    for (const auto& e : threats)
        if (dist(e.pos, pos) <= me.attack_range() && view().vis.at(e.pos)) in_range = true;
    TRACE("r" << now << " " << static_cast<int>(me.champion_name()) << " p" << pos.x << "," << pos.y << " hp" << int(me.health())
          << " g" << me.gem_count() << " near" << near << " dmin" << dmin << " ratio" << ratio << " mode" << int(mode)
          << " inr" << in_range << " hit" << hit_anyway);

    if (!live()) return true;
    if (mode == flee) {
        flee_until_ = now + 10;
        // Running from someone who hits us anyway only gives free damage: trade instead,
        // as long as we can take one of them down before we die.
        float incoming = 0;
        for (const auto& e : threats)
            if (dist(e.pos, pos) <= std::max(1, e.range) + 1) incoming += std::max(20.0f, e.power);
        const int v = pick_victim(threats, me.attack_range(), burst);
        const float victim_hp = v >= 0 ? threats[static_cast<size_t>(v)].hp : 1e9f;
        const float to_kill = 1 + std::max(0.0f, victim_hp - burst) / std::max(1.0f, me.strength());
        const float to_die = me.health() / std::max(1.0f, incoming);
        // Home too far to reach alive: running only wastes the damage we could still deal.
        const int escape = home_walk_.at(pos) >= unreached ? 99 : home_walk_.at(pos);
        const bool doomed = kTradeWhenDoomed && escape > to_die + 1;
        if (in_range && hit_anyway && (kTradeAlways ? hp_frac >= 0.12f : (to_kill <= to_die + 0.5f || doomed)) && home_.at(pos) > 2) {
            fight_spells(threats, burst);
            fight_attack(threats, burst);
            return true;
        }
        // A shield does not stop us walking: raise it before running.
        if (kShieldFlee && !cast_ && live() && me.can_cast() && (hit_anyway || hp_frac < 0.9f))
            for (const auto& ab : me.abilities())
                if (ab.is_shield() && ab.is_ready()) { cast_ = true; safe_use(ab); break; }
        // Ranged stunner: stun the nearest chaser and shoot instead of stepping; it loses its turn.
        if (kStunFlee && me.attack_range() > 1 && !moved_ && !cast_ && me.can_cast() && home_.at(pos) > 2) {
            for (const auto& ab : me.abilities()) {
                if (!ab.is_ready() || !ab.is_cc() || !ab.can_target_enemies()) continue;
                const Sighting* chaser = nullptr;
                for (const auto& e : threats)
                    if (!e.stunned && dist(e.pos, pos) <= spell_range(ab) && view().vis.at(e.pos)
                        && (!chaser || dist(e.pos, pos) < dist(chaser->pos, pos))) chaser = &e;
                if (!chaser) break;
                try {
                    if (!cambc::vision::has_champion(chaser->pos)) break;
                    auto c = cambc::vision::champion_at(chaser->pos);
                    if (!c.is_visible() || !c.is_alive() || !live()) break;
                    ab.use(c);
                    cast_ = true;
                    invalidate_view();
                    fight_attack(threats, burst);
                    return true;
                } catch (const cambc::error&) {
                }
                break;
            }
        }
        // Melee: a free swing before stepping away.
        if (me.attack_range() <= 1) fight_attack(threats, burst);
        retreat(dmin, threats);
        return true;
    }

    fight_spells(threats, burst);
    if (fight_attack(threats, burst)) { chase_streak_ = 0; return true; }
    if (cast_ && !me.can_move()) return true;
    const int range = me.attack_range();
    if (mode == engage && weakest && dmin <= 6 && now >= no_chase_until_) {
        auto goal = empty_board();
        add_box(goal, weakest->pos, range);
        goal &= ~static_walls_ally();
        if (step_to(goal, dmin > range + 1)) {
            if (++chase_streak_ > 12) { no_chase_until_ = now + 30; chase_streak_ = 0; }
            if (range <= 1) fight_attack(threats, burst);
            return true;
        }
    }
    if (mode == hold) {
        // Even fight: melee must close in to matter; ranged hold outside the enemy's reach.
        if (range <= 1 && weakest && dmin <= 4 && hp_frac > 0.4f) {
            auto goal = empty_board();
            add_box(goal, weakest->pos, 1);
            goal &= ~static_walls_ally();
            step_to(goal, false);
            fight_attack(threats, burst);
            return true;
        }
        if (range > 1 && hit_anyway && !in_range) {
            go_home();
            return true;
        }
        if (in_range || cast_) return true;
    }
    return cast_;
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
    if (!go) return false;
    if (enemy_locks.is_empty() && walk_field(altar_, true)) return true;
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
        auto near = dilate(group_tiles_[group], 2) & ~group_tiles_[group];
        near &= ~static_walls_ally();
        if (!near.is_set(me.position())) step_to(near, true);
        return;
    }

    cast_spells(group);
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


void Bot::play_turn() {
    g_turn_round = cambc::round_number();
    pos_ = cambc::self().position();
    if (!ready_) initialise();
    auto& me = cambc::self();
    moved_ = attacked_ = cast_ = move_failed_ = urgent_ = threatened_ = false;
    invalidate_view();

    if (!me.is_alive()) {
        buy_items();
        fill_fields(40000);
        return;
    }
    (void)view();
    // Enemies parked on their spawn shoot whoever walks past: keep out of their reach.
    g_hot_any = false;
    if (kAvoidSpawnCampers) {
        const auto parked = cambc::vision::champion_tiles(team::enemy) & cambc::map::spawn_region(team::enemy);
        if (!parked.is_empty()) {
            g_hotb = empty_board();
            for (auto p : parked.positions()) {
                int reach = 3;
                try { const int r = cambc::vision::champion_at(p).attack_range(); reach = r <= 1 ? 2 : r; } catch (const cambc::error&) {}
                add_box(g_hotb, p, reach);
            }
            g_hotb &= ~cambc::map::spawn_region(team::ally);
            g_hot.load(g_hotb);
            g_hot_any = true;
        }
    }
    update_knowledge();
    const bool at_spawn = cambc::map::spawn_region(team::ally).is_set(me.position());
    if (at_spawn || me.champion_name() == cambc::champion_type::tavi) buy_items();
    if (me.is_recalling() || me.is_submitting_gems() || me.is_teleporting()) {
        fill_fields(40000);
        return;
    }
    // Strength of awake camps that can reach us.
    incoming_ = 0;
    exit_steps_ = 0;
    for (auto p : view().camps.positions()) {
        const int d = dist(p, me.position());
        if (d > 5) continue;
        const int kind = camp_kind_at_[static_cast<size_t>(tile_index(p))];
        if (kind >= 0 && camp_strength(static_cast<cambc::camp_type>(kind)) <= 0) continue;  // clusters never hit
        try {
            auto c = cambc::vision::camp_at(p);
            const int r = camp_range(c.kind());
            if (c.is_awake() && d <= r) {
                incoming_ += camp_strength(c.kind());
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
    if (handle_enemies()) { return; }
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
    const int wait = target_wait_;

    if (blocker_group_ >= 0) {
        // Living camps stand between us and where we want to go: cut through them.
        if (known_[blocker_group_].state != Known::alive || now - blocker_since_ > 40) blocker_group_ = -1;
        else {
            farm(blocker_group_);
            return;
        }
    }
    if (handle_tribute(target, wait)) { return; }

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
    farm(target);
    fill_fields(50000);
}
