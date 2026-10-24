// Big-game hunter: farm cheap camps for gold and shards, then raid the golem and monolith camps
// together, rotating the tank out before it dies. Gems are banked before every boss fight.
#include <cambc.h>
#include "bot.h"
#include "common.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <span>
#include <type_traits>
#include <variant>
#include <vector>

namespace {

using cambc::bitboard;
using it = cambc::item_type;

constexpr bool kDebug = true;
std::uint32_t g_round = 0;
const char* g_branch = "";
bool live() { return cambc::round_number() == g_round; }

void warn(const char* where, const std::exception& e) {
    static int count = 0;
    if (++count <= 10) std::cout << "warn: " << where << ": " << e.what() << '\n';
}

struct Group {
    cambc::map::camp_group g;
    std::vector<vec2> tiles;
    std::vector<float> hp, str;  // per camp: last known health, strength
    std::vector<float> full;
    std::vector<int> range;
    bitboard bb;
    vec2 center{};
    int gems = 0, gold = 0, respawn = 0;
    bool big = false, mono = false;
    int state = 0;  // 0 alive (assumed), 1 reward waiting, 2 respawning
    int respawn_at = 0, seen = 0;
    Dist field;
    bool field_ok = false;
    bitboard ring;
    bool ring_ok = false;
    int min_dist(vec2 p) const { int d = 1 << 20; for (vec2 t : tiles) d = std::min(d, cheb(t, p)); return d; }
    float hp_left() const { float s = 0; for (float h : hp) s += h; return s; }
    float str_left() const { float s = 0; for (size_t i = 0; i < hp.size(); ++i) if (hp[i] > 0) s += str[i]; return s; }
    int range_max() const { int r = 0; for (size_t i = 0; i < hp.size(); ++i) if (hp[i] > 0) r = std::max(r, range[i]); return r; }
};

std::vector<Group> G;
std::vector<std::int16_t> g_tile_group;  // tile -> group index or -1
bitboard g_static;                       // walls, holes, altars, enemy spawn
Bits L_static, L_enemy_spawn, L_ally_spawn;
std::vector<vec2> g_altars;
Dist g_home, g_altar_dist;
bool g_inited = false;
int g_team = 5;
int g_fields_this_turn = 0;
int g_my_index = 0;
vec2 g_spawn_pos{}, g_enemy_spawn_pos{};

// Per-turn view.
bitboard V_vis, V_camps, V_champs, V_awake, V_tele;
Bits LV_camps, LV_champs, LV_awake, LV_vis;
std::vector<vec2> g_allies;  // living allied positions other than ours
bool moved = false, attacked = false, cast_done = false, no_move = false;

// Per-champion memory.
int target = -1, target_since = 0;
int last_hp = 0;
int spawn_wait = 0;
int bought = 0;
bool retreating = false;
int stall = 0;
int stage_wait = 0;

bool is_melee() { return cambc::self().attack_range() <= 1; }

// Initialisation is spread over turns: loading camp groups costs a few thousand fuel each.
int g_init_phase = 0;
size_t g_load_next = 0;
std::vector<cambc::map::camp_group> g_all_groups;
bitboard g_camp_spawns, g_altar_goals;

bool init() {
    if (g_init_phase == 0) {
        init_dims();
        g_static = cambc::map::walls() | cambc::map::holes() | cambc::map::altars() | cambc::map::spawn_region(team::enemy);
        L_static.load(g_static);
        L_enemy_spawn.load(cambc::map::spawn_region(team::enemy));
        L_ally_spawn.load(cambc::map::spawn_region(team::ally));
        g_altars = cambc::map::altars().positions();
        g_spawn_pos = cambc::map::spawn_region(team::ally).positions()[0];
        g_enemy_spawn_pos = cambc::map::spawn_region(team::enemy).positions()[0];
        g_tile_group.assign(static_cast<size_t>(g_w * g_h), -1);
        g_team = static_cast<int>(cambc::champions(team::ally).size());
        g_all_groups = cambc::map::camp_groups();
        G.reserve(g_all_groups.size());
        g_camp_spawns = empty_board();
        bitboard visited;
        cambc::distance_field f;
        f.compute(g_static, cambc::map::spawn_region(team::ally), visited);
        g_home.load(f);
        g_init_phase = 1;
    }
    if (g_init_phase == 1) {
        while (g_load_next < g_all_groups.size()) {
            if (cambc::fuel_remaining() < 30000) return false;
            const auto& grp = g_all_groups[g_load_next++];
            Group x{grp, {}, {}, {}, {}, {}, empty_board()};
            x.gems = static_cast<int>(grp.gem_value());
            x.gold = static_cast<int>(grp.gold_value());
            x.respawn = grp.max_respawn_timer();
            long sx = 0, sy = 0;
            const size_t n = grp.camps().size();
            x.tiles.reserve(n); x.hp.reserve(n); x.full.reserve(n); x.str.reserve(n); x.range.reserve(n);
            for (const auto& c : grp.camps()) {
                const vec2 p = c.spawn_position();
                const auto spec = camp_spec(c.kind());
                x.tiles.push_back(p);
                x.hp.push_back(spec.hp);
                x.full.push_back(spec.hp);
                x.str.push_back(spec.str);
                x.range.push_back(spec.range);
                if (c.kind() == cambc::camp_type::golem || c.kind() == cambc::camp_type::monolith) x.big = true;
                if (c.kind() == cambc::camp_type::monolith) x.mono = true;
                sx += p.x; sy += p.y;
                if (inside(p)) g_tile_group[static_cast<size_t>(tidx(p))] = static_cast<std::int16_t>(G.size());
            }
            if (x.tiles.empty()) continue;
            x.center = {static_cast<int>(sx / static_cast<long>(n)), static_cast<int>(sy / static_cast<long>(n))};
            x.bb = grp.spawn_tiles();
            g_camp_spawns |= x.bb;
            G.push_back(std::move(x));
        }
        g_all_groups.clear();
        g_init_phase = 2;
    }
    if (g_init_phase == 2) {
        if (cambc::fuel_remaining() < 30000) return false;
        // Altar approach tiles, never on camp spawns.
        bitboard approach = grow(cambc::map::altars(), 1) & ~g_static & ~g_camp_spawns;
        g_altar_goals = approach;
        if (!approach.is_empty()) {
            bitboard visited;
            cambc::distance_field f;
            f.compute(g_static, approach, visited);
            g_altar_dist.load(f);
        }
        g_init_phase = 3;
        g_inited = true;
    }
    return g_inited;
}

int group_dist(int gi, vec2 p) {
    Group& x = G[static_cast<size_t>(gi)];
    if (!x.field_ok && g_fields_this_turn < 3 && cambc::fuel_remaining() > 40000) {
        ++g_fields_this_turn;
        bitboard visited;
        cambc::distance_field f;
        f.compute(g_static & ~x.bb, x.bb, visited);
        x.field.load(f);
        x.field_ok = true;
    }
    if (x.field_ok) return x.field.at(p);
    int best = kFar;
    for (vec2 t : x.tiles) best = std::min(best, cheb(t, p) * 13 / 10 + 2);
    return best;
}

void refresh_view() {
    V_vis = cambc::vision::visible_tiles();
    V_camps = cambc::vision::camp_tiles();
    V_champs = cambc::vision::champion_tiles();
    V_awake = cambc::vision::awake_camp_tiles();
    V_tele = cambc::vision::teleport_tiles();
    LV_vis.load(V_vis);
    LV_camps.load(V_camps);
    LV_champs.load(V_champs);
    LV_awake.load(V_awake);
}

void refresh_allies() {
    g_allies.clear();
    static std::vector<cambc::champion> others;
    static bool built = false;
    if (!built) {
        built = true;
        const auto mine = static_cast<cambc::handle_champion_t>(cambc::self());
        std::vector<std::uint32_t> hs;
        for (const auto& a : cambc::champions(team::ally)) {
            hs.push_back(static_cast<std::uint32_t>(static_cast<cambc::handle_champion_t>(a)));
            if (static_cast<cambc::handle_champion_t>(a) != mine) others.push_back(a);
        }
        std::sort(hs.begin(), hs.end());
        for (size_t i = 0; i < hs.size(); ++i)
            if (hs[i] == static_cast<std::uint32_t>(mine)) g_my_index = static_cast<int>(i);
    }
    for (const auto& a : others) {
        try {
            if (!a.is_alive()) continue;
            const vec2 p = a.position();
            if (inside(p)) g_allies.push_back(p);
        } catch (const cambc::error&) {}
    }
}

// Update what we know about camp groups from current vision.
void update_groups(vec2 me) {
    const int now = static_cast<int>(g_round);
    for (size_t gi = 0; gi < G.size(); ++gi) {
        Group& x = G[gi];
        bool any_vis = false;
        int alive = 0;
        for (size_t i = 0; i < x.tiles.size(); ++i) {
            if (!LV_vis.at(x.tiles[i])) continue;
            any_vis = true;
            if (LV_camps.at(x.tiles[i])) ++alive;
        }
        if (!any_vis) {
            if (x.state == 2 && now >= x.respawn_at) {
                x.state = 0;
                x.hp = x.full;
            } else if (x.state == 1 && now - x.seen > 30) {
                x.state = 2;
                x.respawn_at = x.seen + x.respawn;
            }
            continue;
        }
        x.seen = now;
        if (alive > 0) {
            if (x.state != 0) { x.state = 0; x.hp = x.full; }
            const bool near = cheb(x.center, me) <= 10 || static_cast<int>(gi) == target;
            for (size_t i = 0; i < x.tiles.size(); ++i) {
                if (!LV_vis.at(x.tiles[i])) continue;
                if (!LV_camps.at(x.tiles[i])) { x.hp[i] = 0; continue; }
                if (!near || cambc::fuel_remaining() < 30000) continue;
                try {
                    const auto c = cambc::vision::camp_at(x.tiles[i]);
                    x.hp[i] = c.health();
                } catch (const cambc::error&) {}
            }
        } else {
            try {
                const auto st = x.g.state();
                if (std::holds_alternative<cambc::map::camp_group_reward_available>(st)) {
                    x.state = 1;
                    std::fill(x.hp.begin(), x.hp.end(), 0.0f);
                } else if (const auto* r = std::get_if<cambc::map::camp_group_respawning>(&st)) {
                    x.state = 2;
                    x.respawn_at = now + r->timer;
                    std::fill(x.hp.begin(), x.hp.end(), 0.0f);
                } else {
                    // Alive but none of the visible tiles hold a camp: the rest are hidden.
                    if (x.state != 0) { x.state = 0; x.hp = x.full; }
                    for (size_t i = 0; i < x.tiles.size(); ++i) if (LV_vis.at(x.tiles[i])) x.hp[i] = 0;
                }
            } catch (const cambc::error&) {}
        }
    }
}

bitboard blocked_now() {
    bitboard b = g_static | V_camps | V_champs | V_tele;
    b.set(cambc::self().position(), false);
    return b;
}

bool attack_plug(vec2 p);

// One step towards the nearest free goal tile, using the units we can see. When camps close
// every route, walk the route that ignores camps and clear the camp that plugs it.
bool step_to(const bitboard& goals_in) {
    auto& me = cambc::self();
    if (moved || no_move || !live() || !me.can_move()) return false;
    const vec2 pos = me.position();
    if (goals_in.is_set(pos)) return false;
    bitboard blocked = blocked_now();
    bitboard goals = goals_in & ~blocked;
    if (goals.is_empty()) return false;
    bitboard dest = empty_board();
    dest.set(pos, true);
    bitboard visited;
    cambc::distance_field f;
    f.compute_to(blocked, goals, dest, visited);
    int d0 = f.get(pos);
    bool through_camps = false;
    if (d0 >= kFar) {
        blocked = g_static | V_champs | V_tele;
        blocked.set(pos, false);
        goals = goals_in & ~blocked;
        if (goals.is_empty()) return false;
        f.compute_to(blocked, goals, dest, visited);
        d0 = f.get(pos);
        if (d0 >= kFar) return false;
        through_camps = true;
    }
    vec2 best = pos, plug{-1, -1};
    int bd = d0, pd = d0;
    for (vec2 d : kDirs) {
        const vec2 n = pos + d;
        if (!inside(n) || L_static.at(n) || LV_champs.at(n)) continue;
        const int v = f.get(n);
        if (LV_camps.at(n)) {
            if (through_camps && v < pd) { pd = v; plug = n; }
            continue;
        }
        if (v < bd) { bd = v; best = n; }
    }
    if (best == pos) {
        if (plug.x >= 0) attack_plug(plug);
        return false;
    }
    if (!live()) return false;
    moved = true;
    const bool ok = me.move_absolute(best);
    if (ok && live()) refresh_view();
    return ok;
}

// Step that keeps away from the given threat tiles, preferring the way home.
bool step_away(const std::vector<vec2>& threats) {
    auto& me = cambc::self();
    if (moved || no_move || !live() || !me.can_move()) return false;
    const vec2 pos = me.position();
    auto score = [&](vec2 p) {
        int dmin = 99;
        for (vec2 t : threats) dmin = std::min(dmin, cheb(t, p));
        return std::min(dmin, 7) * 100 - g_home.at(p);
    };
    vec2 best = pos;
    int bs = score(pos);
    for (vec2 d : kDirs) {
        const vec2 n = pos + d;
        if (!inside(n) || L_static.at(n) || LV_camps.at(n) || LV_champs.at(n)) continue;
        const int s = score(n);
        if (s > bs) { bs = s; best = n; }
    }
    if (best == pos || !live()) return false;
    moved = true;
    const bool ok = me.move_absolute(best);
    if (ok && live()) refresh_view();
    return ok;
}

struct AbInfo { cambc::ability_kind kind{}; float range = 0; bool excl = false; bool any = false; };
AbInfo ab_info(const cambc::ability& a) {
    AbInfo r;
    const auto& inf = a.info();
    if (!inf) return r;
    r.any = true;
    std::visit([&](const auto& p) {
        using T = std::decay_t<decltype(p)>;
        r.kind = T::kind;
        r.excl = p.exclusive_with_movement;
        if constexpr (requires { p.range; }) r.range = p.range;
    }, *inf);
    return r;
}

// An area spell must not wake a group we do not mean to fight.
int g_fight_group = -1;
bool aoe_safe(vec2 c) {
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            const vec2 p{c.x + dx, c.y + dy};
            if (!LV_camps.at(p) || LV_awake.at(p)) continue;
            if (g_tile_group[static_cast<size_t>(tidx(p))] != g_fight_group) return false;
        }
    return true;
}

bool can_cast_now() {
    auto& me = cambc::self();
    return !cast_done && live() && me.can_cast();
}

// Cast an offensive spell at a camp or champion position. Returns true if cast.
template <class Target>
bool cast_offense(const Target& t, vec2 tpos, bool is_camp) {
    auto& me = cambc::self();
    if (!can_cast_now()) return false;
    const auto abs = me.abilities();
    for (const auto& a : abs) {
        const AbInfo inf = ab_info(a);
        if (!inf.any) continue;
        const bool dmg = inf.kind == cambc::ability_kind::damage || inf.kind == cambc::ability_kind::cc;
        const bool aoe = inf.kind == cambc::ability_kind::aoe_damage;
        if (!dmg && !aoe) continue;
        if (aoe && !aoe_safe(tpos)) continue;
        if (inf.excl && moved) continue;
        if (cheb(me.position(), tpos) > static_cast<int>(inf.range)) continue;
        try {
            if (!a.is_ready()) continue;
            if (dmg) {
                if (is_camp && !a.can_target_camps()) continue;
                if (!is_camp && !a.can_target_enemies()) continue;
                if (!live()) return false;
                if (a.use(t)) { cast_done = true; if (inf.excl) no_move = true; return true; }
            } else {
                if (!live()) return false;
                if (a.use(tpos)) { cast_done = true; if (inf.excl) no_move = true; return true; }
            }
        } catch (const cambc::error& e) { warn("cast", e); }
    }
    return false;
}

// Self utility spells: shield when under fire, heal a hurt ally.
void cast_support(bool under_fire) {
    auto& me = cambc::self();
    if (!can_cast_now()) return;
    const auto abs = me.abilities();
    for (const auto& a : abs) {
        const AbInfo inf = ab_info(a);
        if (!inf.any) continue;
        try {
            if (inf.kind == cambc::ability_kind::shield && under_fire && me.health() < me.max_health() * 0.97f) {
                if (!a.is_ready() || !live()) continue;
                if (a.use()) { cast_done = true; return; }
            } else if (inf.kind == cambc::ability_kind::heal) {
                if (!a.is_ready()) continue;
                for (const auto& ally : cambc::vision::champions(team::ally)) {
                    if (ally == me) continue;
                    if (cheb(ally.position(), me.position()) > static_cast<int>(inf.range)) continue;
                    if (ally.max_health() - ally.health() < 250) continue;
                    if (!live()) return;
                    if (a.use(ally)) { cast_done = true; return; }
                }
            }
        } catch (const cambc::error& e) { warn("support", e); }
    }
}

// Use an attack reset right after our own basic attack.
bool try_reset() {
    auto& me = cambc::self();
    if (!attacked || !can_cast_now() || me.can_attack()) return false;
    const auto abs = me.abilities();
    for (const auto& a : abs) {
        const AbInfo inf = ab_info(a);
        if (!inf.any || inf.kind != cambc::ability_kind::attack_reset) continue;
        try {
            if (!a.is_ready() || !live()) continue;
            if (a.use()) { cast_done = true; return true; }
        } catch (const cambc::error& e) { warn("reset", e); }
    }
    return false;
}

// Best camp target in attack range: target group or awake camps; most dangerous per health first.
bool attack_camp(int tgt, bool allow_big) {
    auto& me = cambc::self();
    g_fight_group = tgt;
    if (!live() || !me.can_attack()) return false;
    if (!is_melee() && moved) return false;
    const vec2 pos = me.position();
    const int r = me.attack_range();
    vec2 best{-1, -1};
    float bscore = -1;
    for (int dy = -r; dy <= r; ++dy)
        for (int dx = -r; dx <= r; ++dx) {
            const vec2 p{pos.x + dx, pos.y + dy};
            if (!LV_camps.at(p) || !LV_vis.at(p)) continue;
            const int gi = inside(p) ? g_tile_group[static_cast<size_t>(tidx(p))] : -1;
            const bool awake = LV_awake.at(p);
            if (gi != tgt && !awake) continue;
            if (gi >= 0 && G[static_cast<size_t>(gi)].big && !allow_big && !awake) continue;
            float hp = 1000, st = 0;
            if (gi >= 0) {
                const Group& x = G[static_cast<size_t>(gi)];
                for (size_t i = 0; i < x.tiles.size(); ++i)
                    if (x.tiles[i] == p) { hp = std::max(1.0f, x.hp[i]); st = x.str[i]; }
            }
            const float s = (st + 5.0f) / hp;
            if (s > bscore) { bscore = s; best = p; }
        }
    if (best.x < 0) return false;
    try {
        const auto c = cambc::vision::camp_at(best);
        if (c.health() <= 0) return false;
        cast_offense(c, best, true);
        if (!live() || !me.can_attack()) return false;
        me.attack(c);
        attacked = true;
        if (live()) refresh_view();
        return true;
    } catch (const cambc::error& e) { warn("attack camp", e); }
    return false;
}

// Attack a camp that blocks our only route, unless it belongs to a boss we are not raiding.
bool attack_plug(vec2 p) {
    auto& me = cambc::self();
    if (!live() || !me.can_attack() || attacked) return false;
    if (!is_melee() && moved) return false;
    const int gi = g_tile_group[static_cast<size_t>(tidx(p))];
    if (gi >= 0 && G[static_cast<size_t>(gi)].big && gi != g_fight_group) return false;
    if (cheb(p, me.position()) > me.attack_range()) return false;
    try {
        const auto c = cambc::vision::camp_at(p);
        if (c.health() <= 0) return false;
        me.attack(c);
        attacked = true;
        if (live()) refresh_view();
        return true;
    } catch (const cambc::error& e) { warn("attack plug", e); }
    return false;
}

// ---------------------------------------------------------------- shopping
void buy_items() {
    auto& me = cambc::self();
    const auto name = me.champion_name();
    const bool focus_main = name == cambc::champion_type::stellar || name == cambc::champion_type::prism
        || name == cambc::champion_type::cassi || name == cambc::champion_type::willow;
    const bool vesper = name == cambc::champion_type::vesper_vale;
    auto S = [&](it fs, it ff) { return focus_main ? ff : fs; };
    std::vector<it> plan;
    if (vesper) plan = {it::treasure_shard_of_strength, it::monster_shard_of_strength, it::power_shard_of_strength,
        it::power_shard_of_health, it::power_shard_of_strength};
    else plan = {S(it::jungle_shard_of_strength, it::jungle_shard_of_focus), S(it::monster_shard_of_strength, it::monster_shard_of_focus),
        it::power_shard_of_health, S(it::power_shard_of_strength, it::power_shard_of_focus), it::durability_shard_of_health};
    for (int guard = 0; guard < 20; ++guard) {
        if (!live()) return;
        const size_t stage = static_cast<size_t>(bought / 5);
        it item = stage < plan.size() ? plan[stage]
            : ((stage % 2) ? it::power_shard_of_health : S(it::power_shard_of_strength, it::power_shard_of_focus));
        if (me.gold_count() < item.cost()) return;
        try {
            if (!me.can_buy(item, 1)) return;
            me.buy(item, 1);
            ++bought;
        } catch (const cambc::error& e) { warn("buy", e); return; }
    }
}

// ---------------------------------------------------------------- shared memory
// byte 0: raid group + 1, byte 1: raid start round / 8.
int raid_group() {
    std::array<std::uint8_t, 2> b{};
    cambc::shared_memory.read(0, std::span<std::uint8_t>{b});
    return static_cast<int>(b[0]) - 1;
}
int raid_age() {
    std::array<std::uint8_t, 2> b{};
    cambc::shared_memory.read(0, std::span<std::uint8_t>{b});
    const int start = b[1] * 8;
    const int now = static_cast<int>(g_round) & 2047;
    return (now - start + 2048) & 2047;
}
void set_raid(int gi) {
    std::array<std::uint8_t, 2> b{static_cast<std::uint8_t>(gi + 1), static_cast<std::uint8_t>((g_round & 2047) / 8)};
    cambc::shared_memory.write(0, std::span<std::uint8_t>{b});
}

// bytes 2..7: per champion, round & 127 with the top bit set when last staged at the raid.
void mark_staged() {
    if (g_my_index > 5) return;
    std::array<std::uint8_t, 1> b{static_cast<std::uint8_t>((g_round & 127) | 128)};
    cambc::shared_memory.write(2 + static_cast<size_t>(g_my_index), std::span<std::uint8_t>{b});
}
int staged_count() {
    std::array<std::uint8_t, 6> b{};
    cambc::shared_memory.read(2, std::span<std::uint8_t>{b});
    int n = 0;
    for (auto v : b)
        if ((v & 128) && ((static_cast<int>(g_round & 127) - (v & 127)) & 127) <= 3) ++n;
    return n;
}

bool has_passive_raw(cambc::item_passive p);
bool has_passive(cambc::item_passive p) {
    static int cached_at = -1;
    static bool jungle = false, monster = false;
    if (cached_at != bought) {
        cached_at = bought;
        jungle = has_passive_raw(cambc::item_passive::jungle);
        monster = has_passive_raw(cambc::item_passive::monster);
    }
    return p == cambc::item_passive::jungle ? jungle : monster;
}

void safe_recall() {
    try {
        if (live() && !cambc::self().restrictions().contains(cambc::buff_restrictions::move)) cambc::self().recall();
    } catch (const cambc::error& e) { warn("recall", e); }
}

// Outside raids the same byte holds our claimed target group (top bit clear).
void write_claim(int gi) {
    if (g_my_index > 5) return;
    std::array<std::uint8_t, 1> b{static_cast<std::uint8_t>(gi >= 0 && gi < 127 ? gi + 1 : 0)};
    cambc::shared_memory.write(2 + static_cast<size_t>(g_my_index), std::span<std::uint8_t>{b});
}
std::array<std::uint8_t, 6> g_claims{};
void read_claims() { cambc::shared_memory.read(2, std::span<std::uint8_t>{g_claims}); }
bool claimed_by_other(int gi) {
    for (int i = 0; i < 6; ++i) {
        if (i == g_my_index) continue;
        const auto v = g_claims[static_cast<size_t>(i)];
        if (!(v & 128) && v == gi + 1) return true;
    }
    return false;
}

float my_dps() {
    auto& me = cambc::self();
    return me.strength() * 1.1f + me.focus() * 0.3f;
}

bool has_passive_raw(cambc::item_passive p) {
    const auto counts = cambc::self().item_count();
    for (int i = 0; i < static_cast<int>(it::count); ++i) {
        const it item{static_cast<std::uint32_t>(i)};
        if (counts[item] == 0) continue;
        try { if (item.passive() == p) return true; } catch (const cambc::error&) {}
    }
    return false;
}

// ---------------------------------------------------------------- enemies
struct Foe { vec2 p; float hp, str; };

// Returns true when the turn was spent on enemies.
bool handle_enemies(const std::vector<Foe>& foes) {
    auto& me = cambc::self();
    const vec2 pos = me.position();
    if (foes.empty()) return false;
    float ours = me.health() * (me.strength() + 0.3f * me.focus());
    int near_allies = 0;
    for (vec2 a : g_allies) if (cheb(a, pos) <= 6) ++near_allies;
    ours += static_cast<float>(near_allies) * me.max_health() * 0.7f * (me.strength() + 0.3f * me.focus());
    float theirs = 0;
    int closest = 99;
    for (const auto& f : foes) { theirs += f.hp * (f.str + 30.0f); closest = std::min(closest, cheb(f.p, pos)); }
    const float need = 1.2f + static_cast<float>(me.gem_count()) / 100.0f;
    if (ours >= theirs * need && me.health() > me.max_health() * 0.45f && near_allies + 1 >= static_cast<int>(foes.size())) {
        // Fight: spells and basic attack on the weakest enemy in range.
        const Foe* best = nullptr;
        for (const auto& f : foes)
            if (cheb(f.p, pos) <= 4 && (!best || f.hp < best->hp)) best = &f;
        if (best) {
            try {
                const auto c = cambc::vision::champion_at(best->p);
                cast_offense(c, best->p, false);
                if (live() && me.can_attack() && cheb(best->p, me.position()) <= me.attack_range() && (is_melee() || !moved)) {
                    me.attack(c);
                    attacked = true;
                    if (try_reset() && live() && me.can_attack()) {
                        const auto c2 = cambc::vision::champion_at(best->p);
                        if (c2.health() > 0) me.attack(c2);
                    }
                    return true;
                }
            } catch (const cambc::error& e) { warn("fight", e); }
        }
        // Chase only nearby enemies.
        if (closest <= 4 && !attacked) {
            bitboard goals = empty_board();
            for (const auto& f : foes) goals.set(f.p, true);
            goals = grow(goals, me.attack_range());
            step_to(goals);
            if (is_melee() && best && live() && me.can_attack()) {
                try {
                    if (cheb(best->p, me.position()) <= 1) {
                        const auto c = cambc::vision::champion_at(best->p);
                        me.attack(c);
                        attacked = true;
                    }
                } catch (const cambc::error& e) { warn("fight2", e); }
            }
            return true;
        }
        return false;
    }
    if (closest <= 6) {
        std::vector<vec2> threats;
        for (const auto& f : foes) threats.push_back(f.p);
        step_away(threats);
        retreating = true;
        return true;
    }
    return false;
}

std::vector<Foe> visible_foes() {
    std::vector<Foe> out;
    const auto tiles = (V_champs & cambc::vision::champion_tiles(team::enemy)).positions();
    for (vec2 p : tiles) {
        if (L_enemy_spawn.at(p)) continue;
        try {
            const auto c = cambc::vision::champion_at(p);
            out.push_back({p, c.health(), c.strength()});
        } catch (const cambc::error&) {}
        if (out.size() >= 6) break;
    }
    return out;
}

// Camps currently able to hit us.
std::vector<vec2> camp_threats(vec2 pos) {
    std::vector<vec2> t;
    for (int dy = -5; dy <= 5; ++dy)
        for (int dx = -5; dx <= 5; ++dx) {
            const vec2 p{pos.x + dx, pos.y + dy};
            if (!LV_awake.at(p)) continue;
            const int gi = g_tile_group[static_cast<size_t>(tidx(p))];
            int r = 3;
            if (gi >= 0) {
                const Group& x = G[static_cast<size_t>(gi)];
                for (size_t i = 0; i < x.tiles.size(); ++i) if (x.tiles[i] == p) r = x.range[i];
            }
            if (cheb(p, pos) <= r) t.push_back(p);
        }
    return t;
}

// ---------------------------------------------------------------- targets
float target_score(int gi, vec2 pos, float dps, float hp_now, float maxhp, float dmg_taken_mult) {
    Group& x = G[static_cast<size_t>(gi)];
    const int now = static_cast<int>(g_round);
    const int d = group_dist(gi, pos);
    if (d >= kFar) return 0;
    const float gold_w = now < 700 ? 0.5f : 0.3f;
    const float value = static_cast<float>(x.gems) + gold_w * static_cast<float>(x.gold);
    if (value <= 0) return 0;
    float wait = 0;
    if (x.state == 2) wait = static_cast<float>(std::max(0, x.respawn_at - now - d));
    const float hp_left = x.state == 0 ? x.hp_left() : (x.state == 2 ? [&] { float s = 0; for (float f : x.full) s += f; return s; }() : 0.0f);
    const float clear = hp_left / std::max(10.0f, dps);
    const float incoming = (x.state == 0 ? x.str_left() : [&] { float s = 0; for (float f : x.str) s += f; return s; }()) * dmg_taken_mult;
    float trips = 1;
    if (incoming > 0) {
        const float need = incoming * clear;
        trips = std::ceil(need / std::max(1.0f, 0.7f * maxhp));
        if (trips < 1) trips = 1;
        if (need > hp_now * 0.8f && trips <= 1) trips = 1.5f;
    }
    if (x.big) return 0;  // bosses only as a team raid
    if (trips > 6) return 0;
    const float home = static_cast<float>(std::min(g_home.at(x.center), 200));
    const float time = static_cast<float>(d) + wait + clear + (trips - 1) * (2 * home + 12) + 6;
    float s = value / (time + 10);
    if (gi == target) s *= 1.3f;
    if (x.state == 1) s *= 1.4f;
    // Allies already on a small group: leave it to them.
    if (claimed_by_other(gi)) s *= hp_left > 3000 ? 0.7f : 0.15f;
    // Enemy half: closer to their spawn than to ours.
    if (cheb(x.center, g_enemy_spawn_pos) + 4 < cheb(x.center, g_spawn_pos)) s *= 0.75f;
    return s;
}

// Should the team raid this boss group now?
bool raid_feasible(int gi, float dps, float maxhp, float mult) {
    const Group& x = G[static_cast<size_t>(gi)];
    if (x.state == 2 && x.respawn_at - static_cast<int>(g_round) > 40) return false;
    const int members = std::min(g_team, x.mono ? g_team : 3);
    if (members < 2) return false;
    const float team_dps = dps * static_cast<float>(members);
    float hp = 0, st = 0;
    for (size_t i = 0; i < x.full.size(); ++i) { hp += x.state == 0 ? x.hp[i] : x.full[i]; st += x.str[i]; }
    const float clear = hp / std::max(10.0f, team_dps);
    const float incoming = st * mult * clear;
    const float budget = maxhp * static_cast<float>(members) * (x.mono ? 1.6f : 1.4f);
    return incoming <= budget && clear <= (x.mono ? 70.0f : 40.0f);
}

}  // namespace

struct FuelReport {
    ~FuelReport() {
        if (!kDebug) return;
        const auto f = cambc::fuel_remaining();
        if (f < 25000 && g_round > 2) std::cout << "lowfuel r" << g_round << ' ' << f << '\n';
        if (g_round >= 150 && g_round < 154)
            std::cout << "br r" << g_round << ' ' << cambc::to_string(cambc::self().champion_name()) << ' ' << g_branch
                      << " moved " << moved << " att " << attacked << " stall " << stall << " tgt " << target << '\n';
    }
};

void play_turn_inner();

void Bot::play_turn() {
    try {
        play_turn_inner();
    } catch (const std::exception& e) { warn("turn", e); }
}

void play_turn_inner() {
    FuelReport fuel_report;
    g_round = cambc::round_number();
    moved = attacked = cast_done = no_move = false;
    g_branch = "start";
    g_fight_group = -1;
    g_fields_this_turn = 0;
    auto& me = cambc::self();
    if (!g_inited) {
        if (!me.is_alive()) return;
        if (!init()) return;
        if (!live() || cambc::fuel_remaining() < 30000) return;
    }

    if (!me.is_alive()) {
        buy_items();
        target = -1;
        return;
    }
    if (me.is_recalling() || me.is_submitting_gems() || me.is_teleporting()) return;

    refresh_view();
    refresh_allies();
    const vec2 pos = me.position();
    if (!inside(pos)) return;
    update_groups(pos);
    if (!live()) return;

    const float hp = me.health(), maxhp = me.max_health();
    if (kDebug && g_round % 50 == 0)
        std::cout << "dbg r" << g_round << ' ' << cambc::to_string(me.champion_name()) << " pos " << pos.x << ',' << pos.y
                  << " hp " << hp << '/' << maxhp << " tgt " << target << " raid " << raid_group() << " gems " << me.gem_count()
                  << " gold " << me.gold_count() << " fuel " << cambc::fuel_remaining() << '\n';
    const int hp_drop = std::max(0, last_hp - static_cast<int>(hp));
    last_hp = static_cast<int>(hp);
    const bool at_spawn = L_ally_spawn.at(pos);
    const float mult = has_passive(cambc::item_passive::jungle) ? 0.7f : 1.0f;
    const float dps = my_dps() * (has_passive(cambc::item_passive::monster) ? 1.1f : 1.0f);

    // Raid bookkeeping.
    int raid = raid_group();
    if (raid >= static_cast<int>(G.size())) raid = -1;
    if (raid >= 0) {
        const Group& rg = G[static_cast<size_t>(raid)];
        if (rg.state == 2 && rg.respawn_at - static_cast<int>(g_round) > 40) { set_raid(-1); raid = -1; }
        else if (raid_age() > 450) { set_raid(-1); raid = -1; }
        else if (raid_age() > 160 && rg.state == 0 && rg.hp_left() >= [&] { float t = 0; for (float f : rg.full) t += f; return t; }() - 1) {
            set_raid(-1);  // nobody got to it: give up for a while
            raid = -1;
        }
    }

    if (at_spawn) {
        buy_items();
        retreating = false;
        if (hp < maxhp * 0.9f && spawn_wait < 8) { ++spawn_wait; return; }
    }
    spawn_wait = 0;

    const auto threats = camp_threats(pos);
    cast_support(!threats.empty() || hp_drop > 0);

    // Enemies.
    const auto foes = visible_foes();
    g_branch = "enemy";
    if (handle_enemies(foes)) return;
    if (!live()) return;

    // Retreat when hurt: leave camp range, then recall.
    float incoming = 0;
    for (vec2 t : threats) {
        const int gi = g_tile_group[static_cast<size_t>(tidx(t))];
        if (gi < 0) continue;
        const Group& x = G[static_cast<size_t>(gi)];
        for (size_t i = 0; i < x.tiles.size(); ++i) if (x.tiles[i] == t) incoming += x.str[i];
    }
    incoming *= mult;
    const float low = std::max({maxhp * 0.35f, static_cast<float>(hp_drop) * 3.0f, incoming * 4.0f});
    if (!at_spawn && (hp < low || (retreating && hp < maxhp * 0.6f))) {
        retreating = true;
        g_branch = "retreat";
        if (threats.empty() && foes.empty()) {
            safe_recall();
            return;
        }
        // Keep hitting a nearly dead target rather than walking away.
        step_away(threats);
        attack_camp(target, raid == target);
        return;
    }
    retreating = false;

    // Shopping trip when rich and idle.
    if (!at_spawn && me.gold_count() >= 300 && threats.empty() && g_home.at(pos) > 8 && me.gem_count() < 10) {
        safe_recall();
        return;
    }

    // Tribute.
    const int gems = static_cast<int>(me.gem_count());
    const int altar_d = g_altar_dist.at(pos);
    const bool bank = gems > 0 && altar_d < kFar
        && (gems >= 22 || (gems >= 10 && altar_d <= 14) || (raid >= 0 && gems >= 8) || (g_round > 1880) || (gems >= 4 && altar_d <= 3));
    if (bank && (threats.empty() || gems >= 18)) {
        g_branch = "bank";
        for (vec2 a : g_altars) {
            if (cheb(a, pos) > 1) continue;
            try {
                if (cambc::vision::has_altar_lock(a)) continue;
                if (!live()) return;
                me.submit_gems(a);
                return;
            } catch (const cambc::error& e) { warn("tribute", e); }
        }
        if (step_to(g_altar_goals)) return;
    }

    // Raid declaration: not before the team has some shards, and not right after a failed raid.
    const int boss_round = 120;
    if (raid < 0 && static_cast<int>(g_round) >= boss_round && raid_age() >= 120) {
        for (size_t gi = 0; gi < G.size(); ++gi) {
            if (!G[gi].big) continue;
            if (G[gi].mono && (g_round < 350 || bought < 8 || g_team < 4)) continue;
            if (!G[gi].mono && bought < 3) continue;
            if (group_dist(static_cast<int>(gi), pos) >= kFar) continue;
            if (raid_feasible(static_cast<int>(gi), dps, maxhp, mult)) {
                set_raid(static_cast<int>(gi));
                raid = static_cast<int>(gi);
                break;
            }
        }
    }

    // Raid membership: monolith takes everyone, a golem the three nearest.
    bool in_raid = false;
    if (raid >= 0) {
        const Group& rg = G[static_cast<size_t>(raid)];
        // Golem raids take a fixed three (by handle order) so every champion agrees.
        in_raid = rg.mono || g_team <= 3 || g_my_index < 3;
    }

    if (in_raid) {
        g_branch = "raid";
        Group& rg = G[static_cast<size_t>(raid)];
        target = raid;
        g_fight_group = raid;
        if (rg.state == 1) {
            step_to(rg.bb);
            return;
        }
        const int rr = std::max(3, rg.range_max());
        // Count allies staged near the boss (shared memory: allies out of vision are unknown).
        if (rg.min_dist(pos) <= rr + 3) { mark_staged(); ++stage_wait; } else stage_wait = 0;
        const int gathered = staged_count();
        int need = std::max(2, std::min(g_team, rg.mono ? std::max(3, g_team - 1) : 3));
        if (stage_wait > 30) need = std::max(2, need - 1);
        bool fighting = false;
        for (vec2 t : rg.tiles) if (LV_awake.at(t)) fighting = true;
        if (kDebug && g_round % 50 == 0)
            std::cout << "raid r" << g_round << " idx " << g_my_index << " md " << rg.min_dist(pos) << " rr " << rr << " gathered " << gathered
                      << " need " << need << " fighting " << fighting << " state " << rg.state << " hp " << rg.hp_left() << '\n';
        if (gathered >= need || fighting) {
            if (attack_camp(raid, true)) { stall = 0; if (try_reset()) attack_camp(raid, true); return; }
            bitboard alive = empty_board();
            for (size_t i = 0; i < rg.tiles.size(); ++i) if (rg.hp[i] > 0) alive.set(rg.tiles[i], true);
            if (alive.is_empty()) alive = rg.bb;
            bitboard goal = grow(alive, me.attack_range());
            if (goal.is_set(pos)) {
                if (++stall > 2) {
                    const bitboard bush = grow(alive, 1) & cambc::map::bushes();
                    if (!bush.is_empty() && !bush.is_set(pos)) goal = bush;
                }
            }
            step_to(goal);
            if (attack_camp(raid, true)) try_reset();
            return;
        }
        // Stage just outside the boss range.
        const int md = rg.min_dist(pos);
        if (md > rr && md <= rr + 2) return;
        if (!rg.ring_ok) {
            rg.ring = grow(rg.bb, rr + 2) & ~grow(rg.bb, rr);
            rg.ring_ok = true;
        }
        step_to(rg.ring);
        return;
    }

    // Farm: choose the best group.
    read_claims();
    {
        float best = 0;
        int bi = -1;
        for (size_t gi = 0; gi < G.size(); ++gi) {
            if (cambc::fuel_remaining() < 25000) break;
            const float s = target_score(static_cast<int>(gi), pos, dps, hp, maxhp, mult);
            if (s > best) { best = s; bi = static_cast<int>(gi); }
        }
        if (bi != target) { target = bi; target_since = static_cast<int>(g_round); stall = 0; }
        write_claim(target);
    }
    if (target < 0) {
        // Nothing worth doing: bank gems or go home.
        if (gems > 0) step_to(g_altar_goals);
        else if (!at_spawn) step_to(cambc::map::spawn_region(team::ally));
        return;
    }
    Group& tg = G[static_cast<size_t>(target)];
    g_fight_group = target;
    g_branch = "farm";
    if (tg.state == 1) {
        g_branch = "capture";
        if (!tg.bb.is_set(pos)) step_to(tg.bb);
        return;
    }
    if (tg.state == 2) {
        g_branch = "respawnwait";
        // Wait near the group for its respawn, never on its spawn tiles (that delays it).
        const int md = tg.min_dist(pos);
        if (md >= 1 && md <= 2) return;
        step_to(grow(tg.bb, 2) & ~tg.bb);
        return;
    }
    if (attack_camp(target, false)) {
        if (try_reset()) attack_camp(target, false);
        stall = 0;
        return;
    }
    bitboard alive = empty_board();
    for (size_t i = 0; i < tg.tiles.size(); ++i) if (tg.hp[i] > 0) alive.set(tg.tiles[i], true);
    if (alive.is_empty()) alive = tg.bb;
    int r = me.attack_range();
    if (r > 1 && stall > 3) r = 1;
    bitboard goal = grow(alive, r);
    if (goal.is_set(pos)) ++stall;
    if (stall > 2) {
        // Camps hidden in a bush: step into the bush to see them.
        const bitboard bush = grow(alive, 1) & cambc::map::bushes();
        if (!bush.is_empty() && !bush.is_set(pos)) goal = bush;
    }
    step_to(goal);
    if (attack_camp(target, false)) { stall = 0; try_reset(); }
}
