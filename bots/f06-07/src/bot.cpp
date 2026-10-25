// Scaling-economy champion: farm the richest reachable camp groups (gold weighted early), turn every
// spawn visit (or death) into shards, bank gems at altars, and fight champions only with the numbers.
#include <cambc.h>
#include "bot.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <span>
#include <variant>

namespace {

using it = cambc::item_type;

cambc::bitboard g_walls;  // walk_walls(), built once

int now() { return static_cast<int>(cambc::round_number()); }

// Debug: turns spent per activity, printed every 250 rounds.
constexpr bool kStats = true;
int g_mode = 0;
std::array<int, 16> g_modes{};
const char* kModeNames[16] = {"dead", "chan", "noact", "heal", "fight", "escape", "hurt", "bank", "shoptrip",
    "notarget", "stuck", "healfirst", "capture", "wait", "hit", "approach"};
struct ModeCount {
    ~ModeCount() {
        if (!kStats) return;
        ++g_modes[static_cast<size_t>(g_mode)];
        const int r = static_cast<int>(cambc::round_number());
        if (r % 250 == 0) {
            std::cout << "stat r" << r << ' ' << cambc::to_string(cambc::self().champion_name());
            for (int i = 0; i < 16; ++i) if (g_modes[static_cast<size_t>(i)]) std::cout << ' ' << kModeNames[i] << '=' << g_modes[static_cast<size_t>(i)];
            std::cout << " gold=" << cambc::self().gold_count() << '\n';
        }
    }
};

bool has_any(const cambc::enum_map<it>& c, it a, it b, it d) { return c[a] + c[b] + c[d] > 0; }

// Properties shared by every ability payload.
const cambc::ability_properties* props(const cambc::ability& ab) {
    const auto& info = ab.info();
    if (!info) return nullptr;
    return std::visit([](const auto& v) -> const cambc::ability_properties* { return &v; }, *info);
}
bool excludes_walking(const cambc::ability& ab) {
    const auto* p = props(ab);
    return p && p->exclusive_with_movement;
}
float aoe_radius(const cambc::ability& ab) {
    const auto& info = ab.info();
    if (info)
        if (auto* a = std::get_if<cambc::ability_aoe_damage>(&*info)) return a->radius;
    return 1;
}

struct Step { it shard, emblem; };
std::vector<Step> build_for(cambc::champion_type t) {
    using ct = cambc::champion_type;
    const Step jungle_s{it::jungle_shard_of_strength, it::jungle_emblem_of_strength};
    const Step jungle_h{it::jungle_shard_of_health, it::jungle_emblem_of_health};
    const Step treasure_s{it::treasure_shard_of_strength, it::treasure_emblem_of_strength};
    const Step monster_s{it::monster_shard_of_strength, it::monster_emblem_of_strength};
    const Step durab_h{it::durability_shard_of_health, it::durability_emblem_of_health};
    const Step power_s{it::power_shard_of_strength, it::power_emblem_of_strength};
    const Step power_h{it::power_shard_of_health, it::power_emblem_of_health};
    switch (t) {
    case ct::vesper_vale: return {treasure_s, jungle_s, monster_s, power_s, durab_h, power_s};
    case ct::marcella: return {jungle_h, treasure_s, monster_s, power_s, durab_h, power_s};
    default: return {jungle_s, treasure_s, monster_s, power_s, durab_h, power_s};
    }
    (void)power_h;
}

// Shared memory: bytes 0-4 hold each slot's target group + 1; bytes 5-7 the latest enemy sighting.
void write_sighting(vec2 p) {
    std::array<std::uint8_t, 3> b{static_cast<std::uint8_t>(p.x + 1), static_cast<std::uint8_t>(p.y),
        static_cast<std::uint8_t>((cambc::round_number() >> 2) & 0xff)};
    cambc::shared_memory.write(5, std::span<const std::uint8_t>{b.data(), b.size()});
}
struct Sighting { bool ok = false; vec2 where{}; int age = 999; };
Sighting read_sighting() {
    std::array<std::uint8_t, 3> b{};
    cambc::shared_memory.read(5, std::span<std::uint8_t>{b.data(), b.size()});
    Sighting s;
    if (b[0] == 0) return s;
    s.where = {b[0] - 1, b[1]};
    const int r = static_cast<int>(cambc::round_number() >> 2) & 0xff;
    s.age = ((r - b[2]) & 0xff) * 4;
    s.ok = inside(s.where);
    return s;
}

int slot_of(cambc::champion_type t) {
    const int v = static_cast<int>(t);
    const int as_draft = v <= 2 ? v : (v <= 4 ? 3 : v - 1);
    const auto order = pick_order();
    for (size_t i = 0; i < order.size(); ++i)
        if (static_cast<int>(order[i]) == as_draft) return static_cast<int>(i);
    return 4;
}

}  // namespace

// ---------------------------------------------------------------- basics

bool Bot::live() const { return cambc::round_number() == round_; }
bool Bot::fuel(std::uint64_t need) const { return cambc::fuel_remaining() > need; }

void Bot::initialise() {
    init_ = true;
    init_dims();
    g_walls = walk_walls();
    walls_l_.load(g_walls);
    hard_l_.load(cambc::map::walls() | cambc::map::altars());
    auto& me = cambc::self();
    type_ = me.champion_name();
    slot_ = slot_of(type_);
    ranged_ = me.attack_range() > 1;
    for (const auto& ab : me.abilities())
        if (ab.is_dash()) dasher_ = true;

    h_ = cambc::map::camp_groups();
    group_at_.assign(static_cast<size_t>(g_w * g_h), -1);
    camp_at_.assign(static_cast<size_t>(g_w * g_h), -1);
    cambc::distance_field fa, fe;
    cambc::bitboard sa, se;
    fa.compute(g_walls, cambc::map::spawn_region(team::ally), sa);
    const auto walls_e = cambc::map::walls() | cambc::map::holes() | cambc::map::altars()
        | cambc::map::spawn_region(team::ally);
    fe.compute(walls_e, cambc::map::spawn_region(team::enemy), se);
    home_.load(fa);
    ehome_.load(fe);
    for (size_t i = 0; i < h_.size(); ++i) {
        Group d;
        long sx = 0, sy = 0;
        for (const auto& c : h_[i].camps()) {
            const vec2 p = c.spawn_position();
            d.tiles.push_back(p);
            d.kinds.push_back(c.kind());
            sx += p.x;
            sy += p.y;
            const auto s = camp_spec(c.kind());
            d.hp += s.hp;
            d.str += s.str;
            d.range = std::max(d.range, s.range);
            if (c.kind() == cambc::camp_type::monolith) d.monolith = d.boss = true;
            if (c.kind() == cambc::camp_type::golem) d.boss = true;
            group_at_[static_cast<size_t>(tidx(p))] = static_cast<std::int16_t>(i);
            camp_at_[static_cast<size_t>(tidx(p))] = static_cast<std::int16_t>(d.tiles.size() - 1);
        }
        if (!d.tiles.empty()) {
            const long n = static_cast<long>(d.tiles.size());
            d.center = {static_cast<int>(sx / n), static_cast<int>(sy / n)};
        }
        for (auto t : d.tiles) {
            d.reach = std::max(d.reach, cheb(t, d.center));
            // Camp tiles are occupied: measure to the tile itself (the BFS walks through it as a root-free tile).
            for (auto dir : kDirs) {
                const vec2 n{t.x + dir.x, t.y + dir.y};
                if (!inside(n)) continue;
                if (sa.is_set(n)) d.d_home = std::min<int>(d.d_home, fa.get(n) + 1);
                if (se.is_set(n)) d.d_enemy = std::min<int>(d.d_enemy, fe.get(n) + 1);
            }
        }
        d.gems = static_cast<int>(h_[i].gem_value());
        d.gold = static_cast<int>(h_[i].gold_value());
        d.respawn = h_[i].max_respawn_timer();
        g_.push_back(std::move(d));
        gtiles_.push_back(h_[i].spawn_tiles());
    }
    k_.assign(g_.size(), Known{});
    for (size_t i = 0; i < g_.size(); ++i)
        for (auto kind : g_[i].kinds) k_[i].hp.push_back(camp_spec(kind).hp);

    auto spots = empty_board();
    for (auto a : cambc::map::altars().positions()) {
        auto one = empty_board();
        one.set(a);
        spots |= grow(one, cambc::constants::submit_gems_range);
    }
    spots &= ~g_walls;
    cambc::distance_field f;
    cambc::bitboard seen;
    f.compute(g_walls, spots, seen);
    altar_.load(f);
    altar_spots_ = spots;
}

void Bot::refresh_view() {
    vis_ = cambc::vision::visible_tiles();
    camps_ = cambc::vision::camp_tiles();
    champs_ = cambc::vision::champion_tiles();
    lvis_.load(vis_);
    lcamps_.load(camps_);
    lchamps_.load(champs_);
}

bool Bot::free_tile(vec2 p) const {
    if (!inside(p)) return false;
    return !walls_l_.at(p) && !lcamps_.at(p) && !lchamps_.at(p);
}

// ---------------------------------------------------------------- knowledge

void Bot::observe() {
    const int t = now();
    for (auto& k : k_)
        if (k.state == Known::respawning && t >= k.ready) {
            k.state = Known::unknown;
        }
    for (size_t i = 0; i < g_.size(); ++i) {
        if (cheb(g_[i].center, pos_) > 8 + g_[i].reach) continue;
        auto& k = k_[i];
        bool seen = false, living = false;
        for (size_t c = 0; c < g_[i].tiles.size(); ++c) {
            const vec2 p = g_[i].tiles[c];
            if (!lvis_.at(p)) continue;
            seen = true;
            if (lcamps_.at(p)) living = true;
        }
        if (!seen) continue;
        if (living) {
            const bool fresh = k.state != Known::alive;
            for (size_t c = 0; c < g_[i].tiles.size(); ++c) {
                const vec2 p = g_[i].tiles[c];
                const float full = camp_spec(g_[i].kinds[c]).hp;
                if (!lvis_.at(p)) {
                    if (fresh) k.hp[c] = full;
                    continue;
                }
                if (!lcamps_.at(p)) { k.hp[c] = 0; continue; }
                if (cheb(p, pos_) <= 8 && fuel(20000)) {
                    try { k.hp[c] = cambc::vision::camp_at(p).health(); } catch (const cambc::error&) { k.hp[c] = full; }
                } else if (k.hp[c] <= 0) {
                    k.hp[c] = full;
                }
            }
            k.state = Known::alive;
            continue;
        }
        if (k.state == Known::respawning) continue;
        try {
            const auto st = h_[i].state();
            if (std::holds_alternative<cambc::map::camp_group_alive>(st)) {
                // Living camps we cannot see (bushes); keep the remembered health.
                k.state = Known::alive;
            } else if (std::holds_alternative<cambc::map::camp_group_reward_available>(st)) {
                k.state = Known::reward;
                for (auto& hp : k.hp) hp = 0;
            } else {
                k.state = Known::respawning;
                k.ready = t + std::max(1, std::get<cambc::map::camp_group_respawning>(st).timer);
                for (size_t c = 0; c < g_[i].tiles.size(); ++c) k.hp[c] = camp_spec(g_[i].kinds[c]).hp;
            }
        } catch (const std::exception&) {}
    }
}

void Bot::measure() {
    auto& me = cambc::self();
    incoming_ = 0;
    awake_ = cambc::vision::awake_camp_tiles();
    if (!awake_.is_empty())
        for (auto p : awake_.positions()) {
            const int d = cheb(p, pos_);
            if (d > 5) continue;
            const int gi = group_at_[static_cast<size_t>(tidx(p))];
            const int ci = camp_at_[static_cast<size_t>(tidx(p))];
            if (gi < 0 || ci < 0) continue;
            const auto s = camp_spec(g_[gi].kinds[static_cast<size_t>(ci)]);
            if (s.str <= 0 || d > s.range) continue;
            // A camp hits the nearest champion: an ally closer to it takes the blows.
            bool covered = false;
            for (const auto& a : allies_)
                if (cheb(a.pos, p) < d) { covered = true; break; }
            if (!covered) incoming_ += s.str;
        }
    const auto counts = me.item_count();
    taken_ = 1.0f;
    if (has_any(counts, it::jungle_emblem_of_strength, it::jungle_emblem_of_health, it::jungle_emblem_of_focus)) taken_ *= 0.7f;
    if (has_any(counts, it::durability_emblem_of_strength, it::durability_emblem_of_health, it::durability_emblem_of_focus)) taken_ *= 0.9f;
    incoming_ *= taken_;
    float dps = me.strength();
    float spell = 0;
    bool reset = false;
    for (const auto& ab : me.abilities()) {
        if (ab.is_attack_reset()) reset = true;
        if (ab.is_damage() || ab.is_aoe_damage()) {
            try { spell += ab.damage_amount(); } catch (const std::exception&) {}
        }
    }
    if (reset) dps *= 9.0f / 8.0f;
    if (type_ == cambc::champion_type::varg) dps *= 1.3f;
    if (type_ == cambc::champion_type::vendetta) dps *= 1.2f;
    dps += spell / 8.0f;
    if (has_any(counts, it::monster_emblem_of_strength, it::monster_emblem_of_health, it::monster_emblem_of_focus)) dps *= 1.1f;
    dps_ = std::max(10.0f, dps);
}

void Bot::write_claim(int group) {
    if (slot_ < 0 || slot_ > 4) return;
    std::uint8_t b = group >= 0 && group < 250 ? static_cast<std::uint8_t>(group + 1) : 0;
    cambc::shared_memory.write(static_cast<size_t>(slot_), std::span<const std::uint8_t>{&b, 1});
}

int Bot::claims_on(int group) const {
    std::array<std::uint8_t, 5> b{};
    cambc::shared_memory.read(0, std::span<std::uint8_t>{b.data(), b.size()});
    int n = 0;
    for (int s = 0; s < 5; ++s)
        if (s != slot_ && b[static_cast<size_t>(s)] == group + 1) ++n;
    return n;
}

// ---------------------------------------------------------------- shopping

void Bot::shop() {
    auto& me = cambc::self();
    if (me.gold_count() < 30) return;
    const auto plan = build_for(type_);
    for (int guard = 0; guard < 40; ++guard) {
        if (me.gold_count() < 30 || !fuel(12000) || !live()) return;
        const auto counts = me.item_count();
        const Step* next = nullptr;
        std::array<int, 64> need{};
        for (const auto& s : plan) {
            const auto e = static_cast<size_t>(static_cast<it::enum_type>(s.emblem)) & 63;
            ++need[e];
            if (static_cast<int>(counts[s.emblem]) < need[e]) { next = &s; break; }
        }
        // Past the plan: power shards, two strength for each health.
        const int str = static_cast<int>(counts[it::power_shard_of_strength] + 6 * counts[it::power_emblem_of_strength]);
        const int hp = static_cast<int>(counts[it::power_shard_of_health] + 6 * counts[it::power_emblem_of_health]);
        const it shard = next ? next->shard : (str <= 2 * hp ? it::power_shard_of_strength : it::power_shard_of_health);
        if (!me.can_buy(shard, 1)) return;
        try { me.buy(shard, 1); } catch (const cambc::error&) { return; }
    }
}

// ---------------------------------------------------------------- movement

bool Bot::try_dash(const cambc::distance_field& f, const cambc::bitboard& seen) {
    auto& me = cambc::self();
    if (cast_ || !dasher_ || !me.can_cast() || !fuel(15000) || !live()) return false;
    const vec2 pos = me.position();
    if (!seen.is_set(pos)) return false;
    const int here = f.get(pos);
    for (const auto& ab : me.abilities()) {
        if (!ab.is_dash() || !ab.is_ready()) continue;
        cambc::bitboard land;
        try { land = ab.valid_tiles(); } catch (const cambc::error&) { return false; }
        land &= seen;
        if (land.is_empty()) return false;
        vec2 best = pos;
        int best_d = here - 2;
        for (auto t : land.positions()) {
            if (!free_tile(t)) continue;
            const int d = f.get(t);
            if (d < best_d || (d == best_d && best == pos)) { best_d = d; best = t; }
        }
        if (best == pos || best_d > here - 2) return false;
        try {
            const bool ok = ab.use(best);
            cast_ = true;
            if (!ok) move_failed_ = true;
            refresh_view();
            return ok;
        } catch (const cambc::error&) {
            return false;
        }
    }
    return false;
}

// One step (and a dash when it gains two) along the shortest route around visible units.
bool Bot::go(const cambc::bitboard& goals_in, bool dash) {
    auto& me = cambc::self();
    if (moved_ || move_failed_ || !me.can_move() || !live() || !fuel(25000)) return false;
    const vec2 pos = me.position();
    if (!inside(pos)) return false;
    auto blocked = g_walls | camps_ | champs_;
    blocked.set(pos, false);
    auto goals = goals_in & ~blocked;
    if (goals.is_empty() || goals.is_set(pos)) return false;
    cambc::distance_field f;
    cambc::bitboard seen;
    auto here = empty_board();
    here.set(pos);
    f.compute_to(blocked, goals, here, seen);
    if (!seen.is_set(pos)) {
        // Units close the way: walk the terrain route and wait for them to move.
        auto g2 = goals_in & ~g_walls;
        if (g2.is_empty() || !fuel(25000)) return false;
        f.compute_to(g_walls, g2, here, seen);
        if (!seen.is_set(pos)) return false;
    }
    if (dash && f.get(pos) >= 3 && try_dash(f, seen)) {
        moved_ = true;
        if (move_failed_ || !me.can_move()) return true;
    }
    const vec2 from = me.position();
    if (!inside(from) || goals.is_set(from) || !seen.is_set(from)) return moved_;
    vec2 best = from;
    int best_d = f.get(from);
    for (auto d : kDirs) {
        const vec2 t{from.x + d.x, from.y + d.y};
        if (!free_tile(t) || !seen.is_set(t)) continue;
        const int fd = f.get(t);
        if (fd < best_d) { best_d = fd; best = t; }
    }
    if (best == from || !live()) return moved_;
    bool ok = false;
    try { ok = me.move_absolute(best); } catch (const cambc::error&) {}
    moved_ = true;
    refresh_view();
    if (!ok) move_failed_ = true;
    return ok;
}

// Walk home when it is close, otherwise recall once nothing hits us. True when the turn is used.
bool Bot::go_home_or_recall(int walk_limit) {
    auto& me = cambc::self();
    if (cambc::map::spawn_region(team::ally).is_set(pos_)) return false;
    const int d = home_.at(pos_);
    if (d <= walk_limit) {
        go(cambc::map::spawn_region(team::ally), true);
        return true;
    }
    if (moved_ || incoming_ > 0 || !live()) return moved_;
    for (const auto& f : foes_)
        if (cheb(f.pos, pos_) <= f.range + 3) {
            go(cambc::map::spawn_region(team::ally), true);
            return true;
        }
    try { me.recall(); } catch (const cambc::error&) {}
    return true;
}

// Step out of reach of awake camps.
bool Bot::escape_camps() {
    if (awake_.is_empty()) return false;
    auto danger = empty_board();
    bool any = false;
    std::array<cambc::bitboard, 6> by_range;
    for (auto& b : by_range) b.clear();
    for (auto p : awake_.positions()) {
        if (cheb(p, pos_) > 7) continue;
        const int gi = group_at_[static_cast<size_t>(tidx(p))];
        const int ci = camp_at_[static_cast<size_t>(tidx(p))];
        if (gi < 0 || ci < 0) continue;
        const auto s = camp_spec(g_[gi].kinds[static_cast<size_t>(ci)]);
        if (s.str <= 0) continue;
        by_range[static_cast<size_t>(std::clamp(s.range, 0, 5))].set(p);
        any = true;
    }
    if (!any) return false;
    for (int r = 1; r <= 5; ++r)
        if (!by_range[static_cast<size_t>(r)].is_empty()) danger |= grow(by_range[static_cast<size_t>(r)], r);
    if (!danger.is_set(pos_)) return false;
    auto safe = ~danger & ~g_walls;
    return go(safe, true);
}

// ---------------------------------------------------------------- enemies

bool Bot::spell_enemy(const std::vector<Unit>& list) {
    auto& me = cambc::self();
    if (cast_ || !me.can_cast() || !live() || list.empty() || !fuel(15000)) return false;
    for (const auto& ab : me.abilities()) {
        if (!ab.is_ready()) continue;
        if (ab.is_shield()) {
            if (me.health() < me.max_health() * 0.85f) {
                try { ab.use(); cast_ = true; return true; } catch (const cambc::error&) {}
            }
            continue;
        }
        if (!(ab.is_damage() || ab.is_cc() || ab.is_aoe_damage())) continue;
        if (moved_ && excludes_walking(ab)) continue;
        cambc::bitboard tiles;
        try { tiles = ab.valid_tiles(); } catch (const cambc::error&) { continue; }
        // Lowest health target in reach.
        const Unit* best = nullptr;
        for (const auto& u : list)
            if (tiles.is_set(u.pos) && (!best || u.hp < best->hp)) best = &u;
        if (!best) continue;
        try {
            if (ab.is_aoe_damage() || !ab.can_target_enemies()) {
                if (!ab.can_target_ground()) continue;
                ab.use(best->pos);
            } else {
                ab.use(cambc::vision::champion_at(best->pos));
            }
            cast_ = true;
            if (excludes_walking(ab)) moved_ = true;
            refresh_view();
            return true;
        } catch (const cambc::error&) {}
    }
    return false;
}

bool Bot::attack_enemy(const std::vector<Unit>& list) {
    auto& me = cambc::self();
    if (!me.can_attack() || !live() || list.empty()) return false;
    const auto tiles = me.attack_tiles();
    const Unit* best = nullptr;
    for (const auto& u : list)
        if (tiles.is_set(u.pos) && lchamps_.at(u.pos) && (!best || u.hp < best->hp)) best = &u;
    if (!best) return false;
    try {
        me.attack(cambc::vision::champion_at(best->pos));
        attacked_ = true;
        chase_turns_ = 0;
        refresh_view();
        return true;
    } catch (const cambc::error&) {}
    return false;
}

void Bot::reset_attack() {
    auto& me = cambc::self();
    if (!attacked_ || cast_ || !live() || me.can_attack() || !me.can_cast() || !fuel(8000)) return;
    for (const auto& ab : me.abilities()) {
        if (!ab.is_attack_reset() || !ab.is_ready()) continue;
        try { ab.use(); } catch (const cambc::error&) { return; }
        cast_ = true;
        return;
    }
}

// Fight when the square law (total health times total damage) favours us, run otherwise.
// True when the turn is spent.
bool Bot::fight() {
    auto& me = cambc::self();
    if (foes_.empty()) return false;
    last_enemy_ = now();
    write_sighting(foes_[0].pos);
    int dmin = 99;
    for (const auto& f : foes_) dmin = std::min(dmin, cheb(f.pos, pos_));
    float ahp = me.health(), adps = me.strength() + 0.3f * me.focus();
    for (const auto& a : allies_)
        if (cheb(a.pos, pos_) <= 7) { ahp += a.hp; adps += a.str + 0.3f * a.focus; }
    float ehp = 0, edps = 0;
    for (const auto& f : foes_)
        if (cheb(f.pos, pos_) <= 9) { ehp += f.hp; edps += f.str + 0.3f * f.focus; }
    if (ehp <= 0) return false;
    const float ratio = (ahp * adps) / std::max(1.0f, ehp * edps);
    float need = 1.3f + static_cast<float>(me.gem_count()) / 150.0f;
    if (home_.at(pos_) <= 6) need = 0.85f;
    if (incoming_ > 0) need += 0.2f;
    // Already in contact: running only gives free hits, so trade back unless clearly outmatched.
    bool contact = false;
    for (const auto& f : foes_)
        if (cheb(f.pos, pos_) <= std::max(f.range, me.attack_range()) + 1) contact = true;
    const bool engage = ratio >= need || (contact && ratio >= 0.75f && me.health() > me.max_health() * 0.3f);

    // A kill in reach is always worth the hit.
    {
        std::vector<Unit> weak;
        for (const auto& f : foes_)
            if (f.hp <= me.strength() * 1.05f) weak.push_back(f);
        if (!weak.empty() && attack_enemy(weak)) {
            reset_attack();
            if (me.can_attack()) attack_enemy(foes_);
            return true;
        }
    }
    if (engage) {
        if (now() < flee_until_ && !contact && ratio < need * 1.3f) {
            // Recently ran: only re-engage when clearly ahead.
        } else {
            spell_enemy(foes_);
            if (!attack_enemy(foes_)) {
                // Approach the weakest enemy, but never chase far or into their half.
                const Unit* t = nullptr;
                for (const auto& f : foes_)
                    if (!t || f.hp < t->hp) t = &f;
                const bool weak = t && t->hp < t->max_hp * 0.35f;
                const bool chase = t && cheb(t->pos, pos_) <= me.attack_range() + (weak ? 4 : 2)
                    && (ehome_.at(t->pos) + 4 >= home_.at(t->pos) || weak) && chase_turns_ < 5;
                if (!chase) {
                    if (!cast_ && !moved_) spell_enemy(foes_);
                    return cast_;
                }
                ++chase_turns_;
                {
                    auto goal = empty_board();
                    goal.set(t->pos);
                    goal = grow(goal, me.attack_range()) & ~g_walls;
                    go(goal, cheb(t->pos, pos_) > me.attack_range() + 1);
                    if (!ranged_ || !moved_) attack_enemy(foes_);
                    if (!cast_) spell_enemy(foes_);
                }
            }
            reset_attack();
            if (attacked_ && me.can_attack()) attack_enemy(foes_);
            return true;
        }
    }
    // Outnumbered: run from enemies that can reach us soon.
    bool threat = false;
    for (const auto& f : foes_)
        if (cheb(f.pos, pos_) <= f.range + 5) threat = true;
    if (!threat) return false;
    flee_until_ = now() + 10;
    if (dmin >= 6 && incoming_ == 0 && !moved_ && home_.at(pos_) > 12) {
        // Far enough to channel a recall before they close in.
        bool close = false;
        for (const auto& f : foes_)
            if (cheb(f.pos, pos_) <= f.range + 3) close = true;
        if (!close) {
            try { me.recall(); return true; } catch (const cambc::error&) {}
        }
    }
    // Hit back on the way when we can (melee walk and attack; ranged attack only if we stay).
    if (!ranged_) {
        go(cambc::map::spawn_region(team::ally), true);
        attack_enemy(foes_);
    } else {
        if (!go(cambc::map::spawn_region(team::ally), true)) attack_enemy(foes_);
    }
    // Stun a chaser when we can.
    if (!cast_) spell_enemy(foes_);
    return true;
}

// ---------------------------------------------------------------- gems

bool Bot::tribute() {
    auto& me = cambc::self();
    const int gems = static_cast<int>(me.gem_count());
    if (gems <= 0) return false;
    const int d = altar_.at(pos_);
    if (d >= kFar) return false;
    const int t = now();
    const bool late = t + d + gems / 10 + 4 >= static_cast<int>(cambc::constants::max_rounds);
    const bool want = gems >= 45 || (gems >= 20 && d <= 8) || (gems >= 8 && d <= 3) || late
        || (gems >= 20 && t - last_enemy_ < 20) || (shop_trip_ && d <= 10)
        || (static_cast<int>(cambc::global_submitted_gems(team::ally)) + gems >= static_cast<int>(cambc::constants::winning_gems));
    if (!want) return false;
    if (incoming_ > 0 && me.health() < incoming_ * (gems / 10 + 2)) return false;
    for (auto a : cambc::map::altars().positions()) {
        if (cheb(a, pos_) > cambc::constants::submit_gems_range) continue;
        if (!live()) return true;
        try { me.submit_gems(a); return true; } catch (const cambc::error&) {}
    }
    return go(altar_spots_, true);
}

// ---------------------------------------------------------------- camps

float Bot::remaining(int group) const {
    float t = 0;
    for (float hp : k_[group].hp) t += hp;
    return t;
}

// Damage taken clearing the group alone, killing the most dangerous camp per health first.
float Bot::estimate_damage(int group) const {
    std::array<std::pair<float, float>, 64> m;
    size_t n = 0;
    float other = 0;  // non-attacking camps still take time
    for (size_t c = 0; c < g_[group].kinds.size() && n < m.size(); ++c) {
        const float s = camp_spec(g_[group].kinds[c]).str;
        const float hp = k_[group].hp[c];
        if (hp <= 0) continue;
        if (s <= 0) { other += hp; continue; }
        m[n++] = {s, hp};
    }
    (void)other;
    std::sort(m.begin(), m.begin() + n, [](auto a, auto b) { return a.first / a.second > b.first / b.second; });
    float t = 0, dmg = 0;
    for (size_t i = 0; i < n; ++i) {
        t += m[i].second / dps_;
        dmg += m[i].first * t;
    }
    return dmg * taken_;
}

int Bot::choose() {
    auto& me = cambc::self();
    const int t = now();
    cambc::distance_field f;
    cambc::bitboard seen;
    auto here = empty_board();
    here.set(pos_);
    f.compute(g_walls, here, seen);
    const float gold_w = (type_ == cambc::champion_type::vesper_vale ? 0.6f : 0.4f)
        * std::max(0.5f, 1.0f - static_cast<float>(t) / 1600.0f);
    const float hp_cap = me.max_health();
    int best = -1;
    float best_score = 0;
    std::array<std::uint8_t, 5> claims{};
    cambc::shared_memory.read(0, std::span<std::uint8_t>{claims.data(), claims.size()});
    const auto sight = read_sighting();
    const bool fresh_sight = sight.ok && sight.age <= 24;
    for (size_t gi = 0; gi < g_.size(); ++gi) {
        const auto& g = g_[gi];
        const auto& k = k_[gi];
        if (k.skip_until > t) continue;
        int eta = kFar;
        for (auto p : g.tiles)
            for (auto dir : kDirs) {
                const vec2 n{p.x + dir.x, p.y + dir.y};
                if (inside(n) && seen.is_set(n)) eta = std::min<int>(eta, f.get(n));
            }
        if (eta >= kFar) continue;
        float wait = 0, clear = 0, extra = 0;
        if (k.state == Known::respawning) wait = static_cast<float>(std::max(0, k.ready - t - eta));
        if (k.state != Known::reward) {
            clear = remaining(static_cast<int>(gi)) / dps_;
            const float dmg = k.state == Known::respawning ? 0 : estimate_damage(static_cast<int>(gi));
            float full = dmg;
            if (k.state == Known::respawning) {
                // Fresh group after the wait.
                for (size_t c = 0; c < g.kinds.size(); ++c) (void)c;
                full = g.str * (g.hp / dps_) * 0.5f * taken_;
            }
            int others = 0;
            for (int s = 0; s < 5; ++s)
                if (s != slot_ && claims[static_cast<size_t>(s)] == gi + 1) ++others;
            if (g.hp > 3000) {
                // Big groups are shared: allies split the blows and the clear time.
                full /= static_cast<float>(1 + others);
                clear /= static_cast<float>(1 + others);
            }
            const float per_trip = hp_cap * 0.65f;
            const int trips = static_cast<int>(std::ceil(full / std::max(1.0f, per_trip)));
            if (g.monolith && trips > 2) continue;
            if (trips > 8) continue;
            if (trips > 1) extra += static_cast<float>((trips - 1) * (std::min(g.d_home, 60) + 12));
        }
        if (t + eta + static_cast<int>(wait + clear) + 5 > static_cast<int>(cambc::constants::max_rounds)) continue;
        float value = static_cast<float>(g.gems) + gold_w * static_cast<float>(g.gold);
        if (value <= 0) continue;
        if (g.d_enemy + 2 < g.d_home) value *= 0.7f;
        if (static_cast<int>(gi) == target_) value *= 1.5f;
        if (fresh_sight && cheb(sight.where, g.center) <= 8 + g.reach) value *= 0.4f;
        if (k.state == Known::reward) value *= 1.3f;
        int others = 0;
        for (int s = 0; s < 5; ++s)
            if (s != slot_ && claims[static_cast<size_t>(s)] == gi + 1) ++others;
        if (others > 0) value *= g.hp > 3000 ? 0.9f : 0.35f;
        const float score = value / (static_cast<float>(eta) + wait + clear + 8.0f + extra);
        if (score > best_score) { best_score = score; best = static_cast<int>(gi); }
    }
    return best;
}

bool Bot::hit_group(int group) {
    auto& me = cambc::self();
    if (!me.can_attack() || !live()) return false;
    const auto tiles = me.attack_tiles();
    const float str = me.strength();
    float best_score = -1e9f;
    vec2 best{};
    bool found = false;
    for (size_t c = 0; c < g_[group].tiles.size(); ++c) {
        const vec2 p = g_[group].tiles[c];
        if (!lcamps_.at(p) || !tiles.is_set(p)) continue;
        const float hp = std::max(1.0f, k_[group].hp[c]);
        const float s = camp_spec(g_[group].kinds[c]).str;
        float score = (s + 1) / hp * 1e4f - hp * 1e-3f;
        if (hp <= str) score += 1e6f;
        if (score > best_score) { best_score = score; best = p; found = true; }
    }
    if (!found) return false;
    try {
        me.attack(cambc::vision::camp_at(best));
        attacked_ = true;
        const int ci = camp_at_[static_cast<size_t>(tidx(best))];
        if (ci >= 0) {
            auto& hp = k_[group].hp[static_cast<size_t>(ci)];
            hp = std::max(1.0f, hp - str);
        }
        refresh_view();
        return true;
    } catch (const cambc::error&) {
        return false;
    }
}

void Bot::spells_on_group(int group) {
    auto& me = cambc::self();
    if (cast_ || !live() || !me.can_cast() || !fuel(15000)) return;
    for (const auto& ab : me.abilities()) {
        if (!ab.is_ready()) continue;
        if (ab.is_shield()) {
            if (incoming_ > 0 && me.health() < me.max_health() * 0.9f) {
                try { ab.use(); cast_ = true; } catch (const cambc::error&) {}
                return;
            }
            continue;
        }
        if (!(ab.is_damage() || ab.is_cc() || ab.is_aoe_damage())) continue;
        if (moved_ && excludes_walking(ab)) continue;
        if (!ab.can_target_camps() && !ab.can_target_ground()) continue;
        cambc::bitboard tiles;
        try { tiles = ab.valid_tiles(); } catch (const cambc::error&) { continue; }
        const float radius = ab.is_aoe_damage() ? aoe_radius(ab) : 0;
        float dmg = 0;
        try { dmg = ab.damage_amount(); } catch (const cambc::error&) {}
        vec2 best{};
        float best_score = -1;
        for (size_t c = 0; c < g_[group].tiles.size(); ++c) {
            const vec2 p = g_[group].tiles[c];
            if (!lcamps_.at(p) || !tiles.is_set(p)) continue;
            const float hp = k_[group].hp[c];
            if (hp <= 0) continue;
            float score = std::min(hp, dmg) + camp_spec(g_[group].kinds[c]).str;
            if (ab.is_aoe_damage())
                for (size_t o = 0; o < g_[group].tiles.size(); ++o)
                    if (o != c && cheb(g_[group].tiles[o], p) <= radius && lcamps_.at(g_[group].tiles[o]))
                        score += std::min(k_[group].hp[o], dmg);
            if (score > best_score) { best_score = score; best = p; }
        }
        if (best_score < 25) continue;
        try {
            if (ab.is_aoe_damage() || !ab.can_target_camps()) ab.use(best);
            else ab.use(cambc::vision::camp_at(best));
            cast_ = true;
            if (excludes_walking(ab)) moved_ = true;
            refresh_view();
        } catch (const cambc::error&) {}
        return;
    }
}

void Bot::farm(int group) {
    auto& me = cambc::self();
    auto& k = k_[group];
    const auto& g = g_[group];
    if (k.state == Known::reward) {
        // Capture: stand on one of the group's spawn tiles.
        g_mode = 12;
        if (gtiles_[group].is_set(pos_)) return;
        go(gtiles_[group], true);
        return;
    }
    // Living camps of the group (remembered).
    auto living = empty_board();
    bool any = false;
    for (size_t c = 0; c < g.tiles.size(); ++c)
        if (k.hp[c] > 0) { living.set(g.tiles[c]); any = true; }
    if (!any || k.state == Known::respawning) {
        // Wait on (or walk to) the group's tiles.
        g_mode = 13;
        if (gtiles_[group].is_set(pos_)) return;
        auto near = grow(gtiles_[group], 1) & ~g_walls;
        if (near.is_set(pos_) && k.state == Known::respawning) return;
        go(k.state == Known::respawning ? near : gtiles_[group], true);
        return;
    }
    spells_on_group(group);
    if (hit_group(group)) {
        g_mode = 14;
        chase_turns_ = 0;
        no_hit_turns_ = 0;
        reset_attack();
        if (attacked_ && me.can_attack()) hit_group(group);
        if (!cast_) spells_on_group(group);
        return;
    }
    // Approach to attack range; shrink when walls hide the camps from that range.
    if (shrink_g_ != group) { shrink_g_ = group; shrink_ = 0; }
    const int r = std::max(1, me.attack_range() - shrink_);
    auto goal = grow(living, r) & ~g_walls;
    if (goal.is_set(pos_)) {
        if (++no_hit_turns_ >= 2 && shrink_ < me.attack_range() - 1) { ++shrink_; no_hit_turns_ = 0; }
        goal = grow(living, std::max(1, me.attack_range() - shrink_)) & ~g_walls;
        if (goal.is_set(pos_)) {
            // In place but nothing to hit: our camp memory is stale (unseen or dead camps).
            if (no_hit_turns_ >= 3) {
                for (size_t c = 0; c < g.tiles.size(); ++c)
                    if (lvis_.at(g.tiles[c]) && !lcamps_.at(g.tiles[c])) k.hp[c] = 0;
            }
            go(living, false);
        } else {
            go(goal, false);
        }
    } else {
        go(goal, cheb(pos_, g.center) > g.range + 3);
    }
    if (!ranged_ || !moved_) {
        if (hit_group(group)) { reset_attack(); if (attacked_ && me.can_attack()) hit_group(group); }
    }
    if (!cast_ && !moved_) spells_on_group(group);
}

// ---------------------------------------------------------------- turn

void Bot::play_turn() {
    round_ = cambc::round_number();
    auto& me = cambc::self();
    if (!init_) {
        initialise();
        if (!fuel(60000)) return;
    }
    moved_ = attacked_ = cast_ = move_failed_ = false;
    ModeCount mc;
    g_mode = 0;

    if (!me.is_alive()) {
        shop();
        shop_trip_ = false;
        return;
    }
    pos_ = me.position();
    if (!inside(pos_)) return;
    const bool at_spawn = cambc::map::spawn_region(team::ally).is_set(pos_);
    if (at_spawn || type_ == cambc::champion_type::tavi) shop();
    if (at_spawn) shop_trip_ = false;

    g_mode = 1;
    if (me.is_recalling() || me.is_submitting_gems() || me.is_teleporting()) {
        // Cancel a recall when an enemy is about to catch us; it would be interrupted anyway.
        return;
    }
    g_mode = 2;
    if (!me.can_move() && !me.can_attack() && !me.can_cast()) return;

    refresh_view();
    foes_.clear();
    allies_.clear();
    const auto& enemy_spawn = cambc::map::spawn_region(team::enemy);
    for (auto p : champs_.positions()) {
        if (p == pos_) continue;
        try {
            auto c = cambc::vision::champion_at(p);
            if (!c.is_alive()) continue;
            Unit u{p, c.health(), c.max_health(), c.strength(), c.focus(), c.attack_range(), c.attack_range() <= 1};
            if (c.is_ally()) allies_.push_back(u);
            else if (!enemy_spawn.is_set(p)) foes_.push_back(u);
        } catch (const cambc::error&) {}
    }
    observe();
    measure();

    // Heal on spawn before heading out again (spawn heals at turn end).
    g_mode = 3;
    if (at_spawn && me.health() < me.max_health() * 0.9f && foes_.empty()) return;

    g_mode = 4;
    if (fight()) return;
    g_mode = 5;
    // Just escaped a fight hurt: heal before farming again.
    if (now() < flee_until_ && foes_.empty() && incoming_ == 0 && me.health() < me.max_health() * 0.6f)
        if (go_home_or_recall(8)) return;

    // Badly hurt: leave camp range, then go home.
    const float hp = me.health();
    if (incoming_ > 0 && (hp < me.max_health() * 0.2f || hp < incoming_ * 3.0f + 30.0f)) {
        if (escape_camps()) return;
    }
    g_mode = 6;
    if (hp < me.max_health() * 0.3f && incoming_ == 0) {
        if (tribute()) return;
        if (go_home_or_recall(8)) return;
    }
    g_mode = 7;
    if (tribute()) return;
    g_mode = 8;

    // Shop trip with enough gold, when no camp is attacking us.
    const int gold = static_cast<int>(me.gold_count());
    if (type_ != cambc::champion_type::tavi && !at_spawn && incoming_ == 0) {
        const int d = home_.at(pos_);
        const int need = d <= 8 ? 120 : 270;
        if (gold >= need && (target_ < 0 || k_[target_].state != Known::alive || remaining(target_) > 3 * dps_
                || d <= 8)) {
            shop_trip_ = true;
        }
        if (shop_trip_) {
            if (tribute()) return;
            if (go_home_or_recall(10)) return;
        }
    }

    // Choose a target every few rounds.
    const int t = now();
    const bool engaged = target_ >= 0 && k_[target_].skip_until <= t
        && (k_[target_].state == Known::alive || k_[target_].state == Known::reward)
        && cheb(pos_, g_[target_].center) <= g_[target_].reach + 6;
    if (target_ < 0 || (!engaged && t - chosen_at_ >= 4) || k_[target_].skip_until > t) {
        if (fuel(45000)) {
            const int prev = target_;
            target_ = choose();
            chosen_at_ = t;
            if (target_ != prev) { target_since_ = t; best_eta_ = kFar; stuck_since_ = t; }
            write_claim(target_);
        }
    }
    g_mode = 9;
    if (target_ < 0) {
        if (!at_spawn && me.health() < me.max_health() * 0.7f) go_home_or_recall(10);
        return;
    }
    // Give up on a target we cannot get closer to.
    g_mode = 10;
    {
        const int d = cheb(pos_, g_[target_].center);
        if (d < best_eta_) { best_eta_ = d; stuck_since_ = t; }
        else if (t - stuck_since_ > 25 && !attacked_ && k_[target_].state == Known::alive) {
            k_[target_].skip_until = t + 60;
            target_ = -1;
            return;
        }
    }
    // Hurt and the target still needs a long fight: heal first.
    g_mode = 11;
    if (k_[target_].state == Known::alive && hp < me.max_health() * 0.5f && incoming_ == 0
        && estimate_damage(target_) > hp * 0.8f) {
        if (go_home_or_recall(8)) return;
    }
    g_mode = 15;
    if (kStats && t % 10 == 0 && t <= 300 && type_ == cambc::champion_type::marcella)
        std::cout << "trace r" << t << " pos " << pos_.x << ',' << pos_.y << " tgt " << target_ << " c " << g_[target_].center.x << ',' << g_[target_].center.y
                  << " st " << int(k_[target_].state) << " rem " << remaining(target_) << " hp " << hp << " foes " << foes_.size() << '\n';
    farm(target_);
    if (attacked_ || cast_) best_eta_ = 0, stuck_since_ = t;
}
