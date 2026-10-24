// Camp-sweep champion: each champion sweeps its sector of camp groups (gem value per unit of
// travel, wait and clear time), shares its target so light groups are not doubled up, and banks
// gems in small batches whenever an altar is close to its route.
#include <cambc.h>
#include "bot.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <variant>
#include <vector>
#include <iostream>

using cambc::vec2;
using cambc::team;

namespace {

constexpr std::array<vec2, 8> dirs{vec2{0, 1}, vec2{1, 1}, vec2{1, 0}, vec2{1, -1},
    vec2{0, -1}, vec2{-1, -1}, vec2{-1, 0}, vec2{-1, 1}};
constexpr int unreached = 65535;
constexpr bool DEBUG_LOG = false;
constexpr bool STATE_LOG = false;
constexpr bool STAGE = false;
char g_state = '?';

int dist(vec2 a, vec2 b) { return static_cast<int>(a.chebyshev_distance(b)); }
int map_w() { static const int w = cambc::map::width(); return w; }
int map_h() { static const int h = cambc::map::height(); return h; }
bool in_bounds(vec2 p) { return p.x >= 0 && p.y >= 0 && p.x < map_w() && p.y < map_h(); }
size_t tile_index(vec2 p) { return static_cast<size_t>(p.y * map_w() + p.x); }

cambc::bitboard empty_board() {
    cambc::bitboard b;
    b.clear();
    return b;
}

void add_box(cambc::bitboard& board, vec2 c, int r) {
    for (int dx = -r; dx <= r; ++dx)
        for (int dy = -r; dy <= r; ++dy) {
            const vec2 p{c.x + dx, c.y + dy};
            if (in_bounds(p)) board.set(p);
        }
}

// Chebyshev dilation with whole-board shifts.
cambc::bitboard dilate(const cambc::bitboard& board, int r) {
    cambc::bitboard out = board;
    for (int i = 0; i < r; ++i) out = out | out.shift_right(1) | out.shift_right(-1);
    for (int i = 0; i < r; ++i) out = out | out.shift_up(1) | out.shift_up(-1);
    return out;
}

bool fuel_ok(std::uint64_t need) { return cambc::fuel_remaining() > need; }

// A turn that ran out of fuel resumes next round on stale data: act only in the round we started.
std::uint32_t g_round = 0;
bool live() { return cambc::round_number() == g_round; }
int now() { return static_cast<int>(cambc::round_number()); }

void safe_recall() {
    if (!live()) return;
    try { cambc::self().recall(); } catch (const cambc::error&) {}
}

float camp_str(int kind) {
    switch (static_cast<cambc::camp_type>(kind)) {
    case cambc::camp_type::wolf: return 20;
    case cambc::camp_type::elk: return 60;
    case cambc::camp_type::monolith: return 200;
    case cambc::camp_type::golem: return 120;
    default: return 0;
    }
}
float camp_max_hp(int kind) {
    switch (static_cast<cambc::camp_type>(kind)) {
    case cambc::camp_type::wolf: return 1350;
    case cambc::camp_type::elk: return 2400;
    case cambc::camp_type::monolith: return 20000;
    case cambc::camp_type::golem: return 5000;
    default: return 120;
    }
}
int camp_range(int kind) {
    switch (static_cast<cambc::camp_type>(kind)) {
    case cambc::camp_type::wolf: return 3;
    case cambc::camp_type::elk: return 4;
    case cambc::camp_type::monolith: return 5;
    case cambc::camp_type::golem: return 4;
    default: return 0;
    }
}

int draft_index(cambc::champion_type t) {
    const int v = static_cast<int>(t);
    if (v <= 2) return v;
    if (v <= 4) return 3;
    return v - 1;
}

// Vision snapshot, refreshed after anything that changes what we see.
struct View {
    cambc::bitboard visb, camps, champs, teleports;
    Grid8 vis, lcamps, lchamps, ltele;
    bool valid = false;
};
View g_view;
std::vector<std::pair<vec2, float>> g_camp_hp;
const Grid8* g_walls_local = nullptr;

void invalidate_view() { g_view.valid = false; g_camp_hp.clear(); }
const View& view() {
    if (!g_view.valid) {
        g_view.visb = cambc::vision::visible_tiles();
        g_view.camps = cambc::vision::camp_tiles();
        g_view.champs = cambc::vision::champion_tiles();
        g_view.teleports = cambc::vision::teleport_tiles();
        g_view.vis.load(g_view.visb);
        g_view.lcamps.load(g_view.camps);
        g_view.lchamps.load(g_view.champs);
        g_view.ltele.load(g_view.teleports);
        g_view.valid = true;
    }
    return g_view;
}

const Grid8& hard_walls_local() {
    static const Grid8 g = [] { Grid8 x; x.load(cambc::map::walls() | cambc::map::altars()); return x; }();
    return g;
}

bool blocked_at(vec2 p) {
    if (!in_bounds(p)) return true;
    if (p == cambc::self().position()) return false;
    const auto& v = view();
    return g_walls_local->at(p) || v.lcamps.at(p) || v.lchamps.at(p) || v.ltele.at(p);
}

float camp_hp(vec2 p) {
    for (auto& [q, hp] : g_camp_hp)
        if (q == p) return hp;
    float hp = 0;
    try { hp = cambc::vision::camp_at(p).health(); } catch (const std::exception&) {}
    g_camp_hp.push_back({p, hp});
    return hp;
}

std::uint8_t shm_byte(int i) {
    std::array<std::uint8_t, 1> b{};
    cambc::shared_memory.read(static_cast<std::uint32_t>(i), std::span<std::uint8_t>{b.data(), 1});
    return b[0];
}
void shm_set(int i, std::uint8_t v) {
    std::array<std::uint8_t, 1> b{v};
    cambc::shared_memory.write(static_cast<std::uint32_t>(i), std::span<const std::uint8_t>{b.data(), 1});
}

struct Report { int round = -1000; vec2 where{}; int count = 0; };
Report read_report() {
    std::array<std::uint8_t, 8> b{};
    cambc::shared_memory.read(0, std::span<std::uint8_t>{b.data(), b.size()});
    Report r;
    if (!b[shm::enemy_round]) return r;
    // Round stored modulo 254 (plus one; zero means no report): recover the latest matching round.
    const int t = static_cast<int>(cambc::round_number());
    const int age = ((t % 254 + 1) - b[shm::enemy_round] + 254) % 254;
    r.round = t - age;
    r.where = vec2{b[shm::enemy_x] & 127, b[shm::enemy_y] & 127};
    r.count = 1 + ((b[shm::enemy_x] >> 7) & 1) + ((b[shm::enemy_y] >> 7) & 1);
    return r;
}

struct Sighting { vec2 pos; float hp, power; int range; };

std::vector<Sighting> visible_champions(team side) {
    std::vector<Sighting> out;
    const vec2 me = cambc::self().position();
    for (auto p : cambc::vision::champion_tiles(side).positions()) {
        if (p == me) continue;
        try {
            auto c = cambc::vision::champion_at(p);
            if (!c.is_alive()) continue;
            out.push_back({p, c.health(), c.strength() + 0.4f * c.focus(), c.attack_range()});
        } catch (const cambc::error&) {
        }
    }
    return out;
}

float ability_range(const cambc::ability& ab) {
    float range = 4;
    if (const auto& info = ab.info()) {
        if (auto* d = std::get_if<cambc::ability_damage>(&*info)) range = d->range;
        else if (auto* c = std::get_if<cambc::ability_cc>(&*info)) range = c->range;
        else if (auto* a = std::get_if<cambc::ability_aoe_damage>(&*info)) range = a->range;
        else if (auto* h = std::get_if<cambc::ability_heal>(&*info)) range = h->range;
    }
    return range;
}

} // namespace

void Grid8::load(const cambc::bitboard& b) {
    v.resize(cambc::bitboard::linear_buffer_size());
    b.export_linear_buffer(std::span<std::uint8_t>{v});
}
bool Grid8::at(vec2 p) const {
    if (!in_bounds(p) || v.empty()) return false;
    const size_t i = tile_index(p);
    return (v[i >> 3] >> (i & 7)) & 1;
}
void Grid16::load(const cambc::distance_field& f) {
    v.resize(static_cast<size_t>(map_w() * map_h()));
    f.export_linear_buffer(std::span<std::uint16_t>{v});
}
int Grid16::at(vec2 p) const {
    if (!in_bounds(p) || v.empty()) return unreached;
    return v[tile_index(p)];
}

void Bot::initialise() {
    ready_ = true;
    auto& me = cambc::self();
    groups_ = analyse_groups();
    handles_ = cambc::map::camp_groups();
    known_.assign(groups_.size(), Known{});
    group_field_.assign(groups_.size(), Grid16{});
    group_at_.assign(static_cast<size_t>(map_w() * map_h()), -1);
    kind_at_.assign(static_cast<size_t>(map_w() * map_h()), -1);
    for (size_t i = 0; i < handles_.size(); ++i) {
        for (const auto& c : handles_[i].camps()) {
            const int k = static_cast<int>(c.kind());
            known_[i].hp.push_back(camp_max_hp(k));
            group_at_[tile_index(c.spawn_position())] = static_cast<std::int16_t>(i);
            kind_at_[tile_index(c.spawn_position())] = static_cast<std::int8_t>(k);
        }
    }

    const auto* mem = cambc::draft_memory.data();
    const size_t size = cambc::draft_memory.size();
    const int mine = draft_index(me.champion_name());
    bool dash_map = false;
    std::vector<int> picks;
    if (size > layout::owner && static_cast<char>(mem[layout::magic]) == 'W') {
        team_size_ = static_cast<int>(mem[layout::team_size]);
        dash_map = static_cast<int>(mem[layout::dash_map]) != 0;
        for (int i = 0; i < team_size_ && i < 5; ++i) picks.push_back(static_cast<int>(mem[layout::picks + i]));
        const size_t count = static_cast<size_t>(mem[layout::group_count]);
        if (count == groups_.size() && layout::owner + count <= size)
            for (size_t i = 0; i < count; ++i) owner_.push_back(static_cast<std::uint8_t>(mem[layout::owner + i]));
    } else {
        dash_map = is_dash_map(groups_);
        team_size_ = static_cast<int>(cambc::champions(team::ally).size());
        for (auto p : preferred_picks(team_size_, dash_map)) picks.push_back(static_cast<int>(p));
    }
    // The real roster can differ from the plan (a missed pick): slots follow the plan where it matches.
    pick_slot_.assign(16, -1);
    for (size_t i = 0; i < picks.size(); ++i)
        if (picks[i] >= 0 && picks[i] < 16) pick_slot_[static_cast<size_t>(picks[i])] = static_cast<int>(i);
    if (mine >= 0 && mine < 16) slot_ = pick_slot_[static_cast<size_t>(mine)];
    if (slot_ < 0) slot_ = 4;
    if (owner_.size() != groups_.size()) owner_ = split_groups(groups_, team_size_, dash_map);

    for (const auto& ab : me.abilities())
        if (ab.is_dash()) dasher_ = true;
    walls_ = dasher_ ? &dash_walls() : &walk_walls();
    static Grid8 wl;
    wl.load(walk_walls());
    g_walls_local = &wl;

    cambc::distance_field f;
    cambc::bitboard seen;
    f.compute(*walls_, cambc::map::spawn_region(team::ally), seen);
    home_.load(f);
    // Tiles beside an altar; camp spawn tiles only where an altar has no other free side.
    auto beside = empty_board();
    for (auto a : cambc::map::altars().positions()) {
        auto box = empty_board();
        add_box(box, a, cambc::constants::submit_gems_range);
        box &= ~walk_walls();
        auto free = box;
        for (auto t : box.positions())
            if (group_at_[tile_index(t)] >= 0) free.set(t, false);
        beside |= free.is_empty() ? box : free;
    }
    f.compute(*walls_, beside, seen);
    altar_.load(f);
    altar_tiles_ = beside;

    for (size_t i = 0; i < groups_.size(); ++i) field_order_.push_back(static_cast<int>(i));
    std::stable_sort(field_order_.begin(), field_order_.end(), [&](int a, int b) {
        const bool ma = owner_[a] == slot_, mb = owner_[b] == slot_;
        if (ma != mb) return ma;
        return groups_[a].d_ally < groups_[b].d_ally;
    });
}

void Bot::fill_fields(std::uint64_t reserve) {
    for (int i : field_order_) {
        if (!group_field_[i].empty()) continue;
        if (!fuel_ok(reserve)) return;
        auto roots = empty_board();
        for (auto t : groups_[i].tiles) roots.set(t);
        // Roots inside walls are refused (a group can reach into the enemy spawn).
        roots &= ~*walls_;
        cambc::distance_field f;
        cambc::bitboard seen;
        if (!roots.is_empty()) f.compute(*walls_, roots, seen);
        group_field_[i].load(f);
        if (roots.is_empty()) std::fill(group_field_[i].v.begin(), group_field_[i].v.end(), static_cast<std::uint16_t>(unreached));
    }
}

cambc::bitboard Bot::tiles_of(int group) const {
    auto b = empty_board();
    for (auto t : groups_[group].tiles)
        if (in_bounds(t)) b.set(t);
    return b;
}

int Bot::eta(int group) const {
    const vec2 pos = cambc::self().position();
    if (!group_field_[group].empty()) {
        const int d = group_field_[group].at(pos);
        return d >= unreached ? 9999 : d;
    }
    if ((dasher_ ? groups_[group].d_dash : groups_[group].d_ally) >= 9999) return 9999;
    return dist(pos, groups_[group].center) * 5 / 4 + 2;
}

void Bot::read_allies() {
    allies_.clear();
    const vec2 me = cambc::self().position();
    for (auto& c : cambc::champions(team::ally)) {
        try {
            const int di = draft_index(c.champion_name());
            const int slot = (di >= 0 && di < 16) ? pick_slot_[static_cast<size_t>(di)] : -1;
            const bool alive = c.is_alive();
            vec2 p{-100, -100};
            float frac = 0;
            if (alive) {
                p = c.position();
                frac = c.health() / std::max(1.0f, c.max_health());
            }
            if (p == me) continue;
            allies_.push_back({slot, p, alive, frac});
        } catch (const std::exception&) {
        }
    }
}

void Bot::note_capture(int group) { (void)group; }

void Bot::update_knowledge() {
    const int t = now();
    for (auto& k : known_)
        if (k.state == Known::respawning && t >= k.ready) k.state = Known::unknown;
    const auto& v = view();
    const vec2 pos = cambc::self().position();
    for (size_t i = 0; i < groups_.size(); ++i) {
        const int d = dist(groups_[i].center, pos);
        if (d > 14) continue;
        auto& k = known_[i];
        const auto& camps = handles_[i].camps();
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
            if (d <= 9 && static_cast<int>(i) == target_)
                for (size_t c = 0; c < camps.size(); ++c) {
                    const vec2 p = camps[c].spawn_position();
                    if (!v.vis.at(p)) continue;
                    const int kind = static_cast<int>(camps[c].kind());
                    if (!v.lcamps.at(p)) k.hp[c] = 0;
                    else if (camp_str(kind) > 0) k.hp[c] = camp_hp(p);
                    else k.hp[c] = camp_max_hp(kind);
                }
            continue;
        }
        if (k.state == Known::respawning) continue;
        try {
            const auto st = handles_[i].state();
            if (std::holds_alternative<cambc::map::camp_group_alive>(st)) {
                k.state = Known::alive;
                for (size_t c = 0; c < camps.size(); ++c)
                    if (v.vis.at(camps[c].spawn_position())) k.hp[c] = 0;
            } else if (std::holds_alternative<cambc::map::camp_group_reward_available>(st)) {
                k.state = Known::reward;
            } else {
                const bool was_reward = k.state == Known::reward;
                k.state = Known::respawning;
                k.ready = t + std::max(1, std::get<cambc::map::camp_group_respawning>(st).timer);
                for (size_t c = 0; c < camps.size(); ++c) k.hp[c] = camp_max_hp(static_cast<int>(camps[c].kind()));
                if (was_reward) note_capture(static_cast<int>(i));
            }
        } catch (const std::exception&) {
        }
    }
}

void Bot::buy_items() {
    using it = cambc::item_type;
    auto& me = cambc::self();
    if (me.gold_count() < 30) return;
    const auto who = me.champion_name();
    const bool tank = who == cambc::champion_type::ryth || who == cambc::champion_type::marcella;
    // Emblem order: camp damage taken first, then camp damage dealt, then raw stats.
    std::vector<std::pair<it, it>> plan;
    if (tank) {
        plan = {{it::jungle_shard_of_health, it::jungle_emblem_of_health},
                {it::monster_shard_of_strength, it::monster_emblem_of_strength},
                {it::power_shard_of_health, it::power_emblem_of_health},
                {it::power_shard_of_strength, it::power_emblem_of_strength}};
    } else {
        plan = {{it::jungle_shard_of_strength, it::jungle_emblem_of_strength},
                {it::monster_shard_of_strength, it::monster_emblem_of_strength},
                {it::power_shard_of_strength, it::power_emblem_of_strength},
                {it::power_shard_of_health, it::power_emblem_of_health}};
    }
    for (int guard = 0; guard < 30; ++guard) {
        if (me.gold_count() < 30 || !fuel_ok(20000) || !live()) return;
        const auto counts = me.item_count();
        it shard = plan.back().first;
        bool found = false;
        for (const auto& [s, e] : plan)
            if (counts[e] == 0) { shard = s; found = true; break; }
        if (!found) {
            const auto ps = counts[it::power_shard_of_strength] + counts[it::power_emblem_of_strength] * 5;
            const auto ph = counts[it::power_shard_of_health] + counts[it::power_emblem_of_health] * 5;
            shard = ps <= ph ? it::power_shard_of_strength : it::power_shard_of_health;
        }
        if (!me.can_buy(shard, 1)) { cant_spend_ = true; return; }
        try { me.buy(shard, 1); } catch (const cambc::error&) { cant_spend_ = true; return; }
        cant_spend_ = false;
    }
}

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
        std::array<std::pair<int, vec2>, 16> cands;
        size_t n = 0;
        for (int dx = -2; dx <= 2; ++dx)
            for (int dy = -2; dy <= 2; ++dy) {
                if (std::max(std::abs(dx), std::abs(dy)) != 2) continue;
                const vec2 t{pos.x + dx, pos.y + dy};
                if (!in_bounds(t) || blocked_at(t)) continue;
                bool path = false;
                for (auto d : dirs) {
                    const vec2 m{pos.x + d.x, pos.y + d.y};
                    if (dist(m, t) != 1 || !in_bounds(m)) continue;
                    if (hard_walls_local().at(m) || view().lchamps.at(m) || view().lcamps.at(m)) continue;
                    path = true;
                    break;
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
            }
        }
        return false;
    }
    return false;
}

bool Bot::hold_for_dash() {
    const int t = now();
    if (t != last_hold_ + 1) hold_start_ = t;
    last_hold_ = t;
    return t - hold_start_ < 12;
}

bool Bot::walk_field(const Grid16& field, bool allow_dash) {
    auto& me = cambc::self();
    if (moved_ || move_failed_ || !me.can_move() || field.empty()) return false;
    if (now() < dynamic_until_) return false;
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
        if (dasher_)
            for (auto d : dirs) {
                const vec2 t{from.x + d.x, from.y + d.y};
                if (in_bounds(t) && cambc::map::holes().is_set(t) && field.at(t) < field.at(from)) return hold_for_dash();
            }
        dynamic_until_ = now() + 10;
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

bool Bot::step_to(const cambc::bitboard& goals_in, bool allow_dash) {
    auto& me = cambc::self();
    if (moved_ || move_failed_ || !me.can_move()) return false;
    if (!fuel_ok(30000)) return false;
    const vec2 pos = me.position();
    const auto& v = view();
    auto blocked = *walls_ | v.champs | v.camps | v.teleports;
    blocked.set(pos, false);
    // Allies hidden from our view still block tiles.
    for (const auto& a : allies_)
        if (a.alive && in_bounds(a.pos)) blocked.set(a.pos);
    blocked.set(pos, false);
    const auto goals = goals_in & ~blocked;
    if (goals.is_empty() || goals.is_set(pos)) return false;
    cambc::distance_field field;
    cambc::bitboard reached;
    auto here = empty_board();
    here.set(pos);
    field.compute_to(blocked, goals, here, reached);
    if (!reached.is_set(pos)) {
        // Units plug the way: walk the terrain-only route as far as free tiles allow.
        cambc::distance_field plain;
        cambc::bitboard plain_reached;
        plain.compute_to(*walls_, goals, here, plain_reached);
        if (!plain_reached.is_set(pos)) return false;
        field = plain;
        reached = plain_reached;
    }
    auto best_walk = [&](vec2 from) {
        vec2 best = from;
        int best_d = reached.is_set(from) ? field.get(from) : unreached;
        for (auto d : dirs) {
            const vec2 t{from.x + d.x, from.y + d.y};
            if (blocked_at(t) || !reached.is_set(t)) continue;
            bool ally_here = false;
            for (const auto& a : allies_)
                if (a.alive && a.pos == t) ally_here = true;
            if (ally_here) continue;
            const int fd = field.get(t);
            if (fd < best_d) { best_d = fd; best = t; }
        }
        return best;
    };
    vec2 best = best_walk(pos);
    if ((allow_dash && field.get(pos) >= 4) || (dasher_ && best == pos)) {
        if (try_dash(nullptr, &field, &reached)) {
            moved_ = true;
            if (move_failed_ || !me.can_move()) return true;
            if (goals.is_set(me.position())) return true;
            best = best_walk(me.position());
        }
    }
    const vec2 from = me.position();
    if (best == from) {
        if (moved_) return true;
        if (dasher_)
            for (auto d : dirs) {
                const vec2 t{from.x + d.x, from.y + d.y};
                if (in_bounds(t) && cambc::map::holes().is_set(t) && reached.is_set(t) && field.get(t) < field.get(from))
                    return hold_for_dash();
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
        if (ab.is_damage() || ab.is_aoe_damage()) {
            try { spell += ab.damage_amount(); } catch (const std::exception&) {}
        }
    }
    if (reset) dps *= 9.0f / 8.0f;
    if (me.champion_name() == cambc::champion_type::vendetta) dps *= 1.25f;
    if (me.champion_name() == cambc::champion_type::varg) dps *= 1.3f;
    return dps + spell / 8.0f;
}

// Living allies whose announced target is this group.
int Bot::helpers(int group) const {
    int n = 0;
    for (const auto& a : allies_)
        if (a.alive && a.slot >= 0 && a.slot < 5 && ally_target_[static_cast<size_t>(a.slot)] == group) ++n;
    return n;
}

float Bot::remaining_fight_hp(int group) const {
    float total = 0;
    const auto& camps = handles_[group].camps();
    for (size_t c = 0; c < camps.size(); ++c)
        if (camp_str(static_cast<int>(camps[c].kind())) > 0) total += known_[group].hp[c];
    return total;
}

// Damage the squad takes clearing the group, killing the hardest hitters first.
float Bot::estimate_damage(int group, float dps) const {
    std::array<std::pair<float, float>, 32> m;
    size_t n = 0;
    const auto& camps = handles_[group].camps();
    for (size_t c = 0; c < camps.size() && n < m.size(); ++c) {
        const float s = camp_str(static_cast<int>(camps[c].kind()));
        if (s <= 0 || known_[group].hp[c] <= 0) continue;
        m[n++] = {s, known_[group].hp[c]};
    }
    std::sort(m.begin(), m.begin() + n, [](auto a, auto b) { return a.first / a.second > b.first / b.second; });
    float t = 0, dmg = 0;
    for (size_t i = 0; i < n; ++i) {
        t += m[i].second / std::max(1.0f, dps);
        dmg += m[i].first * t;
    }
    return dmg * taken_mult_;
}

int Bot::squad_near(int group) const {
    int n = 0;
    const vec2 c = groups_[group].center;
    const int r = groups_[group].range + 4;
    for (const auto& a : allies_)
        if (a.alive && dist(a.pos, c) <= r && a.hp_frac > 0.3f) ++n;
    return n;
}

bool Bot::attack_group(int group) {
    auto& me = cambc::self();
    if (!me.can_attack() || !live()) return false;
    const vec2 pos = me.position();
    const int range = me.attack_range();
    const float str = me.strength();
    const bool splash = me.champion_name() == cambc::champion_type::varg;
    float best_score = -1e9f;
    vec2 best{};
    bool found = false;
    const auto& camps = handles_[group].camps();
    for (const auto& c : camps) {
        const vec2 p = c.spawn_position();
        if (dist(p, pos) > range || !view().lcamps.at(p) || !view().vis.at(p)) continue;
        const float hp = camp_hp(p);
        if (hp <= 0) continue;
        const float s = camp_str(static_cast<int>(c.kind()));
        // Hardest hitters per health first, finishing blows before that, clusters last.
        float score = s / std::max(1.0f, hp) * 100000.0f - hp / 1000.0f;
        if (s > 0 && hp <= str) score += 1e6f;
        if (splash)
            for (const auto& o : camps)
                if (o.spawn_position() != p && dist(o.spawn_position(), p) <= 1 && view().lcamps.at(o.spawn_position()))
                    score += 3000.0f;
        if (score > best_score) { best_score = score; best = p; found = true; }
    }
    // Monsters of another group can stand in range too (a blocker): hit them only if nothing of ours is.
    if (!found) return false;
    try {
        auto c = cambc::vision::camp_at(best);
        me.attack(c);
        attacked_ = true;
        invalidate_view();
        return true;
    } catch (const cambc::error&) {
        return false;
    }
}

bool Bot::reset_and_hit(int group) {
    auto& me = cambc::self();
    if (!attacked_ || cast_ || !fuel_ok(8000) || !live() || me.can_attack() || !me.can_cast()) return false;
    for (const auto& ab : me.abilities()) {
        if (!ab.is_attack_reset() || !ab.is_ready()) continue;
        cast_ = true;
        try { ab.use(); } catch (const cambc::error&) { return false; }
        if (me.can_attack() && group >= 0) return attack_group(group);
        return true;
    }
    return false;
}

bool Bot::heal_ally() {
    auto& me = cambc::self();
    if (cast_ || !live() || !me.can_cast()) return false;
    for (const auto& ab : me.abilities()) {
        if (!ab.is_heal() || !ab.is_ready()) continue;
        float amount = 300;
        try { amount = ab.heal_amount(); } catch (const std::exception&) {}
        const int r = static_cast<int>(ability_range(ab));
        vec2 best{};
        float best_missing = 0;
        for (auto p : cambc::vision::champion_tiles(team::ally).positions()) {
            if (p == me.position() || dist(p, me.position()) > r) continue;
            try {
                auto c = cambc::vision::champion_at(p);
                if (!c.is_alive()) continue;
                const float missing = c.max_health() - c.health();
                if (missing > best_missing) { best_missing = missing; best = p; }
            } catch (const cambc::error&) {}
        }
        if (best_missing < amount * 0.7f) return false;
        try {
            ab.use(cambc::vision::champion_at(best));
            cast_ = true;
            return true;
        } catch (const cambc::error&) {}
        return false;
    }
    return false;
}

void Bot::cast_spells(int group) {
    auto& me = cambc::self();
    if (cast_ || moved_ || !live() || !me.can_cast()) return;
    if (heal_ally()) return;
    for (const auto& ab : me.abilities()) {
        if (!ab.is_ready()) continue;
        if (ab.is_shield()) {
            if (incoming_ > 0 && me.health() < me.max_health() * 0.9f) {
                cast_ = true;
                try { ab.use(); } catch (const cambc::error&) {}
                return;
            }
            continue;
        }
        if (group < 0 || ab.is_dash() || !(ab.is_damage() || ab.is_cc() || ab.is_aoe_damage())) continue;
        if (!ab.can_target_camps() && !ab.can_target_ground()) continue;
        const int r = static_cast<int>(ability_range(ab));
        vec2 best{};
        float best_score = 0;
        const auto& camps = handles_[group].camps();
        for (const auto& c : camps) {
            const vec2 p = c.spawn_position();
            if (dist(p, me.position()) > r || !view().lcamps.at(p) || !view().vis.at(p)) continue;
            const float hp = camp_hp(p);
            if (hp <= 0) continue;
            float score = 1 + camp_str(static_cast<int>(c.kind()));
            if (ab.is_aoe_damage())
                for (const auto& o : camps)
                    if (o.spawn_position() != p && dist(o.spawn_position(), p) <= 1 && view().lcamps.at(o.spawn_position()))
                        score += 1 + camp_str(static_cast<int>(o.kind()));
            if (score > best_score) { best_score = score; best = p; }
        }
        if (best_score <= 0) continue;
        // Single-target spells are wasted on clusters: keep them for monsters.
        if (!ab.is_aoe_damage() && best_score < 2) continue;
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

bool Bot::disengage() {
    auto& me = cambc::self();
    std::array<cambc::bitboard, 6> by_range;
    for (auto& b : by_range) b.clear();
    bool any = false;
    for (auto p : view().camps.positions()) {
        if (dist(p, me.position()) > 6) continue;
        const int kind = kind_at_[tile_index(p)];
        if (kind >= 0 && camp_str(kind) <= 0) continue;
        try {
            auto c = cambc::vision::camp_at(p);
            if (!c.is_awake()) continue;
            by_range[static_cast<size_t>(std::clamp(camp_range(static_cast<int>(c.kind())), 0, 5))].set(p);
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
    safe &= ~walk_walls();
    return step_to(safe, true);
}

void Bot::report_enemy(vec2 where, int count) {
    const int r = now();
    shm_set(shm::enemy_round, static_cast<std::uint8_t>(r % 254 + 1));
    shm_set(shm::enemy_x, static_cast<std::uint8_t>((where.x & 127) | (count >= 2 ? 128 : 0)));
    shm_set(shm::enemy_y, static_cast<std::uint8_t>((where.y & 127) | (count >= 3 ? 128 : 0)));
}

// Fight an enemy force we outnumber, run from a stronger one. True when the turn is spent.
bool Bot::handle_enemies() {
    auto& me = cambc::self();
    const vec2 pos = me.position();
    if (cambc::vision::champion_tiles(team::enemy).is_empty()) {
        // Help a squad mate who reported an invader close by.
        const Report r = read_report();
        if (now() - r.round <= 2 && in_bounds(r.where) && dist(r.where, pos) <= 10 && r.count <= 2
            && me.health() > me.max_health() * 0.6f && incoming_ == 0 && me.gem_count() < 30) {
            auto goal = empty_board();
            add_box(goal, r.where, 2);
            goal &= ~walk_walls();
            return step_to(goal, true);
        }
        return false;
    }
    auto enemies = visible_champions(team::enemy);
    std::erase_if(enemies, [](const Sighting& e) { return cambc::map::spawn_region(team::enemy).is_set(e.pos); });
    if (enemies.empty()) return false;

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
    float theirs = 0;
    for (const auto& e : enemies) {
        const int d = dist(e.pos, pos);
        const int walk = reached.is_set(e.pos) ? field.get(e.pos) : 99;
        if (walk > 9 && d > 1) continue;
        dmin = std::min(dmin, d);
        if (walk <= 8) { ++near; theirs += e.hp * e.power; }
    }
    if (!near) return false;
    float ours = me.health() * (me.strength() + 0.4f * me.focus());
    for (const auto& a : visible_champions(team::ally))
        if (dist(a.pos, pos) <= 6) ours += a.hp * a.power;
    report_enemy(enemies.front().pos, near);

    const bool stronger = ours >= theirs * 1.1f;
    if (DEBUG_LOG && now() < 600)
        std::cout << "E r" << now() << " s" << slot_ << " p" << pos.x << "," << pos.y << " hp" << (int)me.health()
                  << " near" << near << " dmin" << dmin << " ours" << (int)(ours / 1000) << " theirs" << (int)(theirs / 1000)
                  << " inc" << incoming_ << "\n";
    const bool home = cambc::map::spawn_region(team::ally).is_set(pos);
    if (!stronger && !home && (me.health() < me.max_health() * 0.9f || near >= 2 || ours < theirs * 0.8f)) {
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
        if (dmin >= 6 && !moved_ && incoming_ == 0) {
            safe_recall();
            return true;
        }
        go_home();
        return true;
    }

    if (!live()) return true;
    if (!cast_ && !moved_ && me.can_cast()) {
        for (const auto& ab : me.abilities()) {
            if (cast_ || !ab.is_ready() || ab.is_dash()) continue;
            const bool unit_spell = (ab.is_damage() || ab.is_cc()) && ab.can_target_enemies();
            const bool area_spell = ab.is_aoe_damage() && ab.can_target_ground();
            if (!unit_spell && !area_spell) continue;
            const int r = static_cast<int>(ability_range(ab));
            const Sighting* aim = nullptr;
            for (const auto& e : enemies)
                if (dist(e.pos, pos) <= r && view().vis.at(e.pos) && (!aim || e.hp < aim->hp)) aim = &e;
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
                        ab.use();
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
    if (dmin <= 3 && me.health() > me.max_health() * 0.4f) {
        auto goal = empty_board();
        for (const auto& e : enemies) add_box(goal, e.pos, range);
        goal &= ~walk_walls();
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

// Best next group: reward per unit of travel, waiting and clear time. Groups too strong for one
// champion attract helpers; light groups another champion is heading for are left to it.
int Bot::choose_target() {
    auto& me = cambc::self();
    const int t = now();
    const float hp = me.health(), max_hp = me.max_health();
    const float margin = 100 + 0.1f * max_hp;
    const Report report = read_report();
    const bool pack = report.count >= 2 && t - report.round <= 25 && in_bounds(report.where);
    int best = -1;
    float best_score = 0;
    for (int i = 0; i < static_cast<int>(groups_.size()); ++i) {
        const auto& g = groups_[i];
        const auto& k = known_[i];
        if (k.blocked_until > t) continue;
        float value = group_value(g);
        if (value <= 0) continue;
        if (pack && dist(g.center, report.where) <= 8) value *= 0.3f;
        const int e = eta(i);
        if (e >= 9999) continue;
        const int avail = k.state == Known::respawning ? k.ready : t;
        const int wait = std::max(0, avail - (t + e));
        float factor = owner_[i] == slot_ ? 1.0f : (owner_[i] == 255 ? 0.6f : 0.45f);
        if (g.d_enemy + 3 < g.d_ally) factor *= 0.6f;
        if (k.state == Known::reward) factor *= 1.3f;
        if (i == target_) factor *= 1.3f;
        if (value * factor / (e + wait + 10.0f) <= best_score) continue;
        const int help = helpers(i);
        float clear = 0;
        if (k.state != Known::reward) {
            const float solo = estimate_damage(i, dps_);
            const bool heavy = solo > hp - margin;
            if (help > 0) factor *= heavy ? 1.0f : 0.5f;  // join heavy fights, leave light ones
            const float dps = dps_ * (1.0f + 0.85f * static_cast<float>(help));
            const float dmg = estimate_damage(i, dps);
            clear = (remaining_fight_hp(i) + g.soft_hp) / std::max(1.0f, dps) + 1;
            if (dmg > hp - margin) {
                // Camps never heal: wear the group down over several trips if each trip does real work.
                float hits = 0;
                const auto& camps = handles_[i].camps();
                for (size_t c = 0; c < camps.size(); ++c)
                    if (k.hp[c] > 0) hits += camp_str(static_cast<int>(camps[c].kind()));
                hits *= taken_mult_;
                const float cap = max_hp - margin;
                if (hits > 0 && cap / hits < 4) continue;
                const float trips = std::ceil(dmg / std::max(1.0f, cap));
                if (trips > 8) continue;
                clear += trips * (10 + std::min(60, g.d_ally));
            }
        } else if (help > 0) {
            factor *= 0.3f;
        }
        const float score = value * factor / (e + wait + clear + 10.0f);
        if (score > best_score) { best_score = score; best = i; }
    }
    return best;
}

// Re-plan every few rounds and when the target is gone; announce it to the team.
void Bot::pick_target() {
    const int t = now();
    for (int i = 0; i < 5; ++i) ally_target_[static_cast<size_t>(i)] = shm_byte(shm::target + i) - 1;
    const bool gone = target_ < 0 || known_[target_].blocked_until > t
        || (known_[target_].state == Known::respawning && known_[target_].ready - t - eta(target_) > 4);
    if (gone || t - last_choice_ >= 3) {
        const int pick = choose_target();
        last_choice_ = t;
        if (pick != target_) {
            stage_since_ = -1;
            if (STATE_LOG && pick >= 0)
                std::cout << "T r" << t << " s" << slot_ << " " << target_ << "->" << pick << " eta" << eta(pick)
                          << " st" << static_cast<int>(known_[pick].state) << " v" << group_value(groups_[pick])
                          << " own" << static_cast<int>(owner_[pick]) << " gone" << gone << "\n";
        }
        target_ = pick;
    }
    if (slot_ >= 0 && slot_ < 5) shm_set(shm::target + slot_, static_cast<std::uint8_t>(target_ + 1));
}

bool Bot::handle_tribute() {
    auto& me = cambc::self();
    const int gems = static_cast<int>(me.gem_count());
    if (gems <= 0) return false;
    const vec2 pos = me.position();
    const auto enemy_locks = cambc::vision::altar_lock_tiles(team::enemy);
    for (auto a : cambc::map::altars().positions()) {
        const float turns = std::ceil(gems / 10.0f) + 1;
        if (dist(a, pos) <= cambc::constants::submit_gems_range && !enemy_locks.is_set(a)
            && me.health() > incoming_ * taken_mult_ * turns + 40) {
            if (live()) {
                try { me.submit_gems(a); } catch (const cambc::error&) {}
            }
            return true;
        }
    }
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
    if (STATE_LOG && now() % 25 == 0) std::cout << "A r" << now() << " s" << slot_ << " gems" << gems << " d" << d_altar << " altars" << cambc::map::altars().count() << " tiles" << altar_tiles_.count() << "\n";
    if (d_altar >= unreached) return false;
    const int late = static_cast<int>(cambc::constants::max_rounds) - now();
    const Report report = read_report();
    const bool danger = now() - report.round <= 30 && in_bounds(report.where) && dist(report.where, pos) <= 16;
    // Is the squad busy at the target without us? Then a detour costs little.
    bool idle = target_ < 0;
    bool fighting = false;
    if (target_ >= 0) {
        const auto& k = known_[target_];
        if (k.state == Known::respawning && k.ready - now() > d_altar) idle = true;
        fighting = k.state == Known::alive && eta(target_) <= groups_[target_].range + 2 && incoming_ > 0;
    }
    // Detour to bank: altar trip versus going straight to the target.
    int detour = d_altar * 2;
    if (target_ >= 0 && !group_field_[target_].empty()) {
        int from_altar = unreached;
        for (auto t : groups_[target_].tiles) from_altar = std::min(from_altar, altar_.at(t));
        if (from_altar < unreached) detour = std::max(0, d_altar + from_altar - eta(target_));
    }
    bool go = gems >= 70 || (idle && gems >= 8) || late < d_altar + 25 || (danger && gems >= 10)
        || d_altar <= 2 || detour <= 1 + gems / 8 || (gems >= 40 && d_altar <= 12);
    if (fighting && gems < 60) go = false;
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
    const auto& k = known_[group];
    const int e = eta(group);
    const int t = now();
    g_state = 'w';
    if (e > groups_[group].range + 4 && walk_field(group_field_[group], true)) return;

    if (k.state == Known::reward) {
        g_state = 'C';
        const auto spawns = tiles_of(group);
        const vec2 here = me.position();
        if (std::find(groups_[group].tiles.begin(), groups_[group].tiles.end(), here) != groups_[group].tiles.end()) {
            if (capture_group_ != group) { capture_group_ = group; capture_since_ = t; }
            g_state = '0';
            if (t - capture_since_ > cambc::constants::capture_time + 6) {
                known_[group].blocked_until = t + 60;
                capture_group_ = -1;
            }
            return;
        }
        // One of us on a spawn tile is enough; others stand by.
        g_state = '1';
        step_to(spawns, false);
        g_state = '2';
        return;
    }
    if (k.state == Known::respawning) {
        g_state = 'r';
        const auto own = tiles_of(group);
        auto near = dilate(own, 2) & ~own;
        near &= ~walk_walls();
        if (!near.is_set(me.position())) step_to(near, true);
        return;
    }

    // Wait for the squad before waking monsters we cannot clear alone.
    const float solo = estimate_damage(group, dps_);
    bool awake = false;
    for (const auto& c : handles_[group].camps()) {
        const vec2 p = c.spawn_position();
        if (!view().lcamps.at(p) || dist(p, me.position()) > 8 || camp_str(static_cast<int>(c.kind())) <= 0) continue;
        try { if (cambc::vision::camp_at(p).is_awake()) { awake = true; break; } } catch (const std::exception&) {}
    }
    // A helper is on the way to a group we cannot clear alone: wait a little at the edge.
    int coming = 0;
    for (const auto& a : allies_)
        if (a.alive && a.slot >= 0 && a.slot < 5 && ally_target_[static_cast<size_t>(a.slot)] == group
            && dist(a.pos, groups_[group].center) <= 20 && a.hp_frac > 0.5f) ++coming;
    if (STAGE && !awake && solo > me.health() * 0.7f && coming > 0 && squad_near(group) == 0) {
        if (stage_group_ != group || stage_since_ < 0) { stage_group_ = group; stage_since_ = t; }
        if (t - stage_since_ < 12) {
            g_state = 's';
            const auto living = tiles_of(group) & ~(view().visb & ~view().camps);
            const int r = groups_[group].range;
            auto ring = dilate(living, r + 2) & ~dilate(living, r);
            ring &= ~walk_walls();
            if (!ring.is_set(me.position())) step_to(ring, false);
            return;
        }
    }

    g_state = 'f';
    cast_spells(group);
    if (attack_group(group)) {
        reset_and_hit(group);
        return;
    }
    const int r = std::max(1, me.attack_range());
    const auto living = tiles_of(group) & ~(view().visb & ~view().camps);
    auto goal = dilate(living, r) & ~walk_walls();
    step_to(goal, e > 3);
    if (attack_group(group)) reset_and_hit(group);
}

void Bot::play_turn() {
    g_state = '?';
    try {
        play_inner();
    } catch (const cambc::error& e) {
        std::cout << "dbg err state " << g_state << " r" << now() << " s" << slot_ << " " << e.what() << "\n";
    }
    if (STATE_LOG) std::cout << "S" << slot_ << g_state << (moved_ ? "m" : "") << (attacked_ ? "a" : "") << "\n";
}

void Bot::play_inner() {
    g_round = cambc::round_number();
    if (!ready_) initialise();
    auto& me = cambc::self();
    moved_ = attacked_ = cast_ = move_failed_ = false;
    invalidate_view();

    if (!me.is_alive()) {
        g_state = 'D';
        buy_items();
        fill_fields(40000);
        return;
    }
    me.set_priority(10 - std::min(slot_, 9));
    read_allies();
    (void)view();
    update_knowledge();
    const bool at_spawn = cambc::map::spawn_region(team::ally).is_set(me.position());
    if (at_spawn || me.champion_name() == cambc::champion_type::tavi) buy_items();
    if (me.is_recalling() || me.is_submitting_gems() || me.is_teleporting()) {
        g_state = me.is_recalling() ? 'R' : 'T';
        fill_fields(40000);
        return;
    }

    incoming_ = 0;
    exit_steps_ = 0;
    for (auto p : view().camps.positions()) {
        const int d = dist(p, me.position());
        if (d > 5) continue;
        const int kind = kind_at_[tile_index(p)];
        if (kind >= 0 && camp_str(kind) <= 0) continue;
        try {
            auto c = cambc::vision::camp_at(p);
            const int r = camp_range(static_cast<int>(c.kind()));
            if (c.is_awake() && d <= r) {
                incoming_ += camp_str(static_cast<int>(c.kind()));
                exit_steps_ = std::max(exit_steps_, r + 1 - d);
            }
        } catch (const std::exception&) {}
    }
    {
        using it = cambc::item_type;
        const auto counts = me.item_count();
        taken_mult_ = 1.0f;
        if (counts[it::jungle_emblem_of_strength] + counts[it::jungle_emblem_of_health] + counts[it::jungle_emblem_of_focus] > 0)
            taken_mult_ *= 0.7f;
        dps_ = my_dps();
        if (counts[it::monster_emblem_of_strength] + counts[it::monster_emblem_of_health] + counts[it::monster_emblem_of_focus] > 0)
            dps_ *= 1.1f;
    }
    if (handle_enemies()) { g_state = 'E'; return; }
    if (!fuel_ok(30000)) { g_state = 'F'; return; }

    const float hp = me.health(), max_hp = me.max_health();
    if (!at_spawn) spawn_wait_ = 0;
    else if (hp < max_hp * 0.92f && spawn_wait_ < 3) { ++spawn_wait_; g_state = 'H'; return; }
    // Badly hurt: get out of camp range, then recall.
    if (!at_spawn && (hp < max_hp * 0.3f || (incoming_ > 0 && hp < incoming_ * taken_mult_ * (exit_steps_ + 2) + 40))) {
        g_state = 'L';
        if (disengage()) return;
        if (incoming_ == 0 && !moved_) {
            if (me.gem_count() > 0 && altar_.at(me.position()) <= 3 && handle_tribute()) return;
            safe_recall();
            return;
        }
    }
    if (heal_ally()) {}

    pick_target();
    const int target = target_;
    if (handle_tribute()) { g_state = 'B'; return; }

    const int home_d = home_.at(me.position());
    if (target < 0) {
        g_state = 'N';
        if (!at_spawn && incoming_ == 0 && !moved_ && (hp < max_hp * 0.9f || me.gold_count() >= 60)) {
            if (home_d <= 5) go_home();
            else safe_recall();
        }
        fill_fields(40000);
        return;
    }
    const auto& k = known_[target];
    const bool engaged = k.state == Known::alive && eta(target) <= groups_[target].range + 2;
    // Spend a full purse at home between fights.
    if (!at_spawn && me.gold_count() >= 220 && me.gem_count() == 0 && incoming_ == 0 && !cant_spend_
        && me.champion_name() != cambc::champion_type::tavi && !engaged) {
        g_state = '$';
        if (home_d <= 8) go_home();
        else if (!moved_) safe_recall();
        return;
    }
    // Heal before a fight we cannot survive at this health.
    if (!at_spawn && hp < max_hp * 0.45f && incoming_ == 0 && k.state == Known::alive) {
        g_state = 'h';
        if (home_d <= 5) go_home();
        else if (!moved_) safe_recall();
        return;
    }
    if (DEBUG_LOG && now() % 10 == 0)
        std::cout << "r" << now() << " s" << slot_ << " p" << me.position().x << "," << me.position().y << " hp" << (int)hp
                  << " t" << target << " st" << (int)k.state << " eta" << eta(target) << " near" << squad_near(target)
                  << " gems" << me.gem_count() << " inc" << incoming_ << "\n";
    farm(target);
    if (g_state == '2') g_state = '3';
    fill_fields(50000);
}
