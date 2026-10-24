// Altar-control champion: farm and bank gems like everyone else, but carry an altar lock, keep one
// champion watching the altar the enemy relies on, lock altars under enemy tributes and punish the
// champions who stop to clear the lock.
#include <cambc.h>
#include "bot.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <span>
#include <utility>
#include <variant>

namespace {

using it = cambc::item_type;
constexpr bool kLog = true;
constexpr int kLogFrom = 1, kLogTo = 0;
constexpr float kFightRatio = 1.0f;
constexpr float kHuntRatio = 1.6f;
constexpr float kStuckBonus = 1.3f;
constexpr bool kHelp = false;
constexpr int kHelpRange = 14;
constexpr int kThreat = 4;
constexpr int kGuardFrom = 250;
constexpr float kSightPenalty = 0.35f;

// A turn that ran out of fuel resumes next round on stale data: act only in the round we started.
std::uint32_t g_round = 0;
bool live() { return cambc::round_number() == g_round; }
bool fuel(std::uint64_t need) { return cambc::fuel_remaining() > need; }
int now() { return static_cast<int>(cambc::round_number()); }
const char* g_mode = "";

void warn(const char* where, const std::exception& e) {
    static int count = 0;
    if (++count <= 20) std::cout << "warn: " << where << ": " << e.what() << '\n';
}

// Vision snapshot, refreshed after anything that changes what we see.
struct View {
    cambc::bitboard vis, camps, champs, tele;
    Bits lvis, lcamps, lchamps, ltele;
    std::vector<vec2> allies;
    bool ok = false;
};
View g_view;
void invalidate() { g_view.ok = false; }
std::vector<cambc::champion> g_others;  // allied handles other than ours

const View& view() {
    if (!g_view.ok) {
        g_view.vis = cambc::vision::visible_tiles();
        g_view.camps = cambc::vision::camp_tiles();
        g_view.champs = cambc::vision::champion_tiles();
        g_view.tele = cambc::vision::teleport_tiles();
        g_view.allies.clear();
        for (const auto& a : g_others) {
            try {
                if (!a.is_alive()) continue;
                const vec2 p = a.position();
                if (inside(p)) {
                    g_view.allies.push_back(p);
                    g_view.champs.set(p);
                }
            } catch (const cambc::error&) {}
        }
        g_view.lvis.load(g_view.vis);
        g_view.lcamps.load(g_view.camps);
        g_view.lchamps.load(g_view.champs);
        g_view.ltele.load(g_view.tele);
        g_view.ok = true;
    }
    return g_view;
}

Bits g_walls, g_hard;
cambc::bitboard g_ww;  // walk_walls(), cached

bool free_tile(vec2 p) {
    if (!inside(p)) return false;
    const auto& v = view();
    return !g_walls.at(p) && !v.lcamps.at(p) && !v.lchamps.at(p) && !v.ltele.at(p);
}

struct Foe {
    cambc::champion c;
    vec2 pos;
    float hp, power;
    int range, gems;
    bool channel;
};

std::vector<Foe> foes() {
    std::vector<Foe> out;
    const auto tiles = cambc::vision::champion_tiles(team::enemy);
    if (tiles.is_empty()) return out;
    for (auto p : tiles.positions()) {
        try {
            auto c = cambc::vision::champion_at(p);
            if (!c.is_alive()) continue;
            int gems = 0;
            try { gems = static_cast<int>(c.gem_count()); } catch (const cambc::error&) {}
            out.push_back({c, p, c.health(), c.strength() + 0.25f * c.focus(), c.attack_range(), gems,
                c.is_submitting_gems()});
        } catch (const cambc::error&) {}
    }
    return out;
}

struct Step { it shard, emblem; };
const std::vector<Step>& build_for(cambc::champion_type t) {
    static const std::vector<Step> plain{
        {it::jungle_shard_of_strength, it::jungle_emblem_of_strength},
        {it::monster_shard_of_strength, it::monster_emblem_of_strength},
        {it::power_shard_of_health, it::power_emblem_of_health},
        {it::power_shard_of_strength, it::power_emblem_of_strength},
        {it::durability_shard_of_health, it::durability_emblem_of_health},
    };
    static const std::vector<Step> vesper{
        {it::jungle_shard_of_strength, it::jungle_emblem_of_strength},
        {it::treasure_shard_of_strength, it::treasure_emblem_of_strength},
        {it::monster_shard_of_strength, it::monster_emblem_of_strength},
        {it::power_shard_of_health, it::power_emblem_of_health},
        {it::durability_shard_of_health, it::durability_emblem_of_health},
    };
    return t == cambc::champion_type::vesper_vale ? vesper : plain;
}

bool has_any(const cambc::enum_map<it>& c, it a, it b, it d) { return c[a] + c[b] + c[d] > 0; }

float ability_range(const cambc::ability& ab) {
    float r = 4;
    try {
        const auto& info = ab.info();
        if (info) {
            if (auto* d = std::get_if<cambc::ability_damage>(&*info)) r = d->range;
            else if (auto* c = std::get_if<cambc::ability_cc>(&*info)) r = c->range;
            else if (auto* a = std::get_if<cambc::ability_aoe_damage>(&*info)) r = a->range;
            else if (auto* h = std::get_if<cambc::ability_heal>(&*info)) r = h->range;
        }
    } catch (const std::exception&) {}
    return r;
}

bool excludes_walk(const cambc::ability& ab) {
    try {
        const auto& info = ab.info();
        if (info) return std::visit([](const auto& x) { return x.exclusive_with_movement; }, *info);
    } catch (const std::exception&) {}
    return true;
}

bool has_lock() {
    try { return cambc::self().consumables()[cambc::consumable_type::altar_lock].count() > 0; }
    catch (const cambc::error&) { return false; }
}

// Shared memory: bytes 0-4 each slot's target group + 1; bytes 5-7 the latest enemy sighting
// (x + 1, y, round / 4).
struct Sighting { bool ok = false; vec2 where{}; int age = 999; };
Sighting read_sighting() {
    std::array<std::uint8_t, 3> b{};
    cambc::shared_memory.read(5, std::span<std::uint8_t>{b.data(), b.size()});
    Sighting s;
    if (b[0] == 0) return s;
    s.where = {b[0] - 1, b[1]};
    s.age = ((static_cast<int>(cambc::round_number() >> 2) - b[2]) & 0xff) * 4;
    s.ok = inside(s.where);
    return s;
}
void write_sighting(vec2 p) {
    std::array<std::uint8_t, 3> b{static_cast<std::uint8_t>(p.x + 1), static_cast<std::uint8_t>(p.y),
        static_cast<std::uint8_t>((cambc::round_number() >> 2) & 0xff)};
    cambc::shared_memory.write(5, std::span<const std::uint8_t>{b.data(), b.size()});
}

}  // namespace

// ---------------------------------------------------------------- setup

void Bot::initialise() {
    init_ = true;
    init_dims();
    auto& me = cambc::self();
    g_ww = walk_walls();
    g_walls.load(g_ww);
    g_hard.load(cambc::map::walls() | cambc::map::altars());
    const int me_h = static_cast<int>(static_cast<cambc::handle_champion_t>(me));
    std::vector<int> types;
    for (const auto& a : cambc::champions(team::ally)) {
        types.push_back(static_cast<int>(a.champion_name()));
        if (static_cast<int>(static_cast<cambc::handle_champion_t>(a)) != me_h) g_others.push_back(a);
    }
    std::sort(types.begin(), types.end());
    size_ = static_cast<int>(types.size());
    slot_ = static_cast<int>(std::find(types.begin(), types.end(), static_cast<int>(me.champion_name())) - types.begin());
    const bool have_tavi = std::find(types.begin(), types.end(), static_cast<int>(cambc::champion_type::tavi)) != types.end();
    guard_ = size_ >= 3 && (have_tavi ? me.champion_name() == cambc::champion_type::tavi : slot_ == size_ - 1);
    for (const auto& ab : me.abilities())
        if (ab.is_dash()) dasher_ = true;
    ranged_ = me.attack_range() > 1;

    cambc::distance_field f;
    cambc::bitboard seen;
    f.compute(g_ww, cambc::map::spawn_region(team::ally), seen);
    home_.load(f);
    Dist enemy;
    f.compute(cambc::map::walls() | cambc::map::holes() | cambc::map::altars(), cambc::map::spawn_region(team::enemy), seen);
    enemy.load(f);

    h_ = cambc::map::camp_groups();
    group_at_.assign(static_cast<size_t>(g_map_w * g_map_h), -1);
    camp_at_.assign(static_cast<size_t>(g_map_w * g_map_h), -1);
    for (size_t i = 0; i < h_.size(); ++i) {
        Group g;
        int sx = 0, sy = 0;
        for (const auto& c : h_[i].camps()) {
            g.tiles.push_back(c.spawn_position());
            g.kinds.push_back(c.kind());
            g.hp.push_back(camp_spec(c.kind()).hp);
            sx += c.spawn_position().x;
            sy += c.spawn_position().y;
        }
        if (g.tiles.empty()) continue;
        const int n = static_cast<int>(g.tiles.size());
        vec2 c{sx / n, sy / n};
        vec2 best = g.tiles[0];
        for (auto t : g.tiles) if (cheb(t, c) < cheb(best, c)) best = t;
        g.center = best;
        g.gems = static_cast<int>(h_[i].gem_value());
        g.gold = static_cast<int>(h_[i].gold_value());
        for (auto t : g.tiles) {
            g.reach = std::max(g.reach, cheb(t, g.center));
            g.d_ally = std::min(g.d_ally, home_.at(t));
            g.d_enemy = std::min(g.d_enemy, enemy.at(t));
        }
        g.spawn = h_[i].spawn_tiles();
        for (size_t c2 = 0; c2 < g.tiles.size(); ++c2) {
            group_at_[static_cast<size_t>(tidx(g.tiles[c2]))] = static_cast<std::int16_t>(g_.size());
            camp_at_[static_cast<size_t>(tidx(g.tiles[c2]))] = static_cast<std::int16_t>(c2);
        }
        g_.push_back(std::move(g));
    }
    // h_ must stay index-aligned with g_.
    if (g_.size() != h_.size()) {
        std::vector<cambc::map::camp_group> keep;
        for (auto& h : h_) if (!h.camps().empty()) keep.push_back(h);
        h_ = keep;
    }

    auto spots = empty_board();
    for (auto a : cambc::map::altars().positions()) {
        Altar al;
        al.pos = a;
        for (auto d : kDirs) {
            const vec2 t{a.x + d.x, a.y + d.y};
            al.d_ally = std::min(al.d_ally, home_.at(t));
            al.d_enemy = std::min(al.d_enemy, enemy.at(t));
        }
        altars_.push_back(al);
        auto one = empty_board();
        one.set(a);
        spots |= grow(one, cambc::constants::submit_gems_range);
    }
    spots &= ~g_ww;
    spots_ = spots;
    f.compute(g_ww, spots, seen);
    altar_.load(f);

    // The altar the enemy is most likely to bank at, among those we can reach in reasonable time.
    int best = kFar;
    for (size_t a = 0; a < altars_.size(); ++a) {
        const auto& al = altars_[a];
        if (al.d_ally >= kFar || al.d_enemy >= kFar) continue;
        if (al.d_ally > al.d_enemy) continue;
        const int score = al.d_enemy;
        if (score < best) { best = score; guard_altar_ = static_cast<int>(a); }
    }
    if (guard_altar_ < 0) guard_ = false;

    for (size_t i = 0; i < g_.size(); ++i) fill_order_.push_back(static_cast<int>(i));
    std::stable_sort(fill_order_.begin(), fill_order_.end(), [&](int a, int b) {
        return g_[a].d_ally < g_[b].d_ally;
    });
    camp_block_ = empty_board();
}

void Bot::fill_fields(std::uint64_t reserve) {
    for (int i : fill_order_) {
        if (!g_[i].field.empty()) continue;
        if (!fuel(reserve)) return;
        cambc::distance_field f;
        cambc::bitboard seen;
        f.compute(g_ww, g_[i].spawn, seen);
        g_[i].field.load(f);
    }
}

int Bot::eta(int gi) const {
    const auto& g = g_[gi];
    if (!g.field.empty()) return std::min(kFar, g.field.at(pos_));
    if (g.d_ally >= kFar) return kFar;
    return cheb(pos_, g.center) * 5 / 4 + 2;
}

// ---------------------------------------------------------------- knowledge

void Bot::observe() {
    const int t = now();
    for (auto& g : g_)
        if (g.state == Group::respawning && t >= g.ready) { g.state = Group::unknown; block_dirty_ = true; }
    const auto& v = view();
    for (size_t i = 0; i < g_.size(); ++i) {
        auto& g = g_[i];
        if (cheb(g.center, pos_) > 8 + g.reach) continue;
        bool seen = false, living = false;
        for (size_t c = 0; c < g.tiles.size(); ++c) {
            const vec2 p = g.tiles[c];
            if (!v.lvis.at(p)) continue;
            seen = true;
            if (v.lcamps.at(p)) {
                living = true;
                if (g.hp[c] <= 0) { g.hp[c] = camp_spec(g.kinds[c]).hp; block_dirty_ = true; }
            } else if (g.state == Group::alive && g.hp[c] > 0) {
                g.hp[c] = 0;
                block_dirty_ = true;
            }
        }
        if (!seen) continue;
        if (living) {
            if (g.state != Group::alive) {
                for (size_t c = 0; c < g.tiles.size(); ++c) {
                    const vec2 p = g.tiles[c];
                    if (!v.lvis.at(p)) g.hp[c] = camp_spec(g.kinds[c]).hp;
                    else if (!v.lcamps.at(p)) g.hp[c] = 0;
                }
                g.state = Group::alive;
                block_dirty_ = true;
            }
            continue;
        }
        if (g.state == Group::respawning) continue;
        if (g.state == Group::reward && static_cast<int>(i) != target_ && (t + static_cast<int>(i)) % 4 != 0) continue;
        try {
            const auto st = h_[i].state();
            const auto before = g.state;
            if (std::holds_alternative<cambc::map::camp_group_alive>(st)) {
                g.state = Group::alive;
            } else if (std::holds_alternative<cambc::map::camp_group_reward_available>(st)) {
                g.state = Group::reward;
            } else {
                g.state = Group::respawning;
                g.ready = t + std::max(1, std::get<cambc::map::camp_group_respawning>(st).timer);
                for (size_t c = 0; c < g.tiles.size(); ++c) g.hp[c] = camp_spec(g.kinds[c]).hp;
            }
            if (g.state != before) block_dirty_ = true;
        } catch (const std::exception&) {}
    }
    if (target_ >= 0 && cheb(g_[target_].center, pos_) <= 8 + g_[target_].reach) {
        auto& g = g_[target_];
        for (size_t c = 0; c < g.tiles.size(); ++c) {
            const vec2 p = g.tiles[c];
            if (cheb(p, pos_) > 8 || !v.lvis.at(p) || !v.lcamps.at(p)) continue;
            try { g.hp[c] = cambc::vision::camp_at(p).health(); } catch (const std::exception&) {}
        }
    }
}

void Bot::rebuild_block() {
    if (!block_dirty_) return;
    block_dirty_ = false;
    std::vector<std::uint8_t> buf(cambc::bitboard::linear_buffer_size(), 0);
    for (const auto& g : g_) {
        if (g.state == Group::respawning || g.state == Group::reward) continue;
        for (size_t c = 0; c < g.tiles.size(); ++c)
            if (g.hp[c] > 0) {
                const int t = tidx(g.tiles[c]);
                buf[static_cast<size_t>(t >> 3)] |= static_cast<std::uint8_t>(1u << (t & 7));
            }
    }
    camp_block_.import_linear_buffer(std::span<const std::uint8_t>{buf});
}

void Bot::measure() {
    auto& me = cambc::self();
    incoming_ = 0;
    const auto& v = view();
    const auto awake = cambc::vision::awake_camp_tiles();
    if (!awake.is_empty())
        for (auto p : awake.positions()) {
            const int d = cheb(p, pos_);
            if (d > 5) continue;
            const int gi = group_at_[static_cast<size_t>(tidx(p))];
            const int ci = camp_at_[static_cast<size_t>(tidx(p))];
            if (gi < 0 || ci < 0) continue;
            const auto s = camp_spec(g_[gi].kinds[static_cast<size_t>(ci)]);
            if (s.str <= 0 || d > s.range) continue;
            bool shielded = false;
            for (auto a : v.allies)
                if (cheb(a, p) < d) { shielded = true; break; }
            if (!shielded) incoming_ += s.str;
        }
    const auto counts = me.item_count();
    taken_ = 1.0f;
    if (has_any(counts, it::jungle_emblem_of_strength, it::jungle_emblem_of_health, it::jungle_emblem_of_focus)) taken_ *= 0.7f;
    if (has_any(counts, it::durability_emblem_of_strength, it::durability_emblem_of_health, it::durability_emblem_of_focus)) taken_ *= 0.9f;
    float dps = me.strength();
    float spell = 0;
    bool reset = false;
    for (const auto& ab : me.abilities()) {
        if (ab.is_attack_reset()) reset = true;
        if (ab.is_damage() || ab.is_aoe_damage() || ab.is_cc()) {
            try { spell += ab.damage_amount(); } catch (const std::exception&) {}
        }
    }
    if (reset) dps *= 9.0f / 8.0f;
    if (me.champion_name() == cambc::champion_type::vendetta) dps *= 1.25f;
    dps += spell / 8.0f;
    if (has_any(counts, it::monster_emblem_of_strength, it::monster_emblem_of_health, it::monster_emblem_of_focus)) dps *= 1.1f;
    dps_ = std::max(10.0f, dps);
}

void Bot::write_claim(int gi) {
    if (slot_ < 0 || slot_ > 4) return;
    std::uint8_t b = gi >= 0 && gi < 250 ? static_cast<std::uint8_t>(gi + 1) : 0;
    cambc::shared_memory.write(static_cast<size_t>(slot_), std::span<const std::uint8_t>{&b, 1});
}

int Bot::claimed_by_other(int gi) const {
    std::array<std::uint8_t, 5> b{};
    cambc::shared_memory.read(0, std::span<std::uint8_t>{b.data(), b.size()});
    int n = 0;
    for (int s = 0; s < 5; ++s)
        if (s != slot_ && b[static_cast<size_t>(s)] == gi + 1) ++n;
    return n;
}

// ---------------------------------------------------------------- shopping

void Bot::shop() {
    auto& me = cambc::self();
    // An altar lock first: it is cheap and the point of this team.
    try {
        if (!has_lock() && me.can_buy(cambc::consumable_type::altar_lock)) me.buy(cambc::consumable_type::altar_lock);
    } catch (const cambc::error& e) { warn("buy lock", e); }
    if (me.gold_count() < 30) return;
    const auto& plan = build_for(me.champion_name());
    for (int guard = 0; guard < 30; ++guard) {
        if (me.gold_count() < 30 || !fuel(15000) || !live()) return;
        const auto counts = me.item_count();
        const Step* next = nullptr;
        std::array<int, static_cast<size_t>(it::enum_type::count)> need{};
        for (const auto& s : plan) {
            const auto e = static_cast<size_t>(static_cast<it::enum_type>(s.emblem));
            ++need[e];
            if (static_cast<int>(counts[s.emblem]) < need[e]) { next = &s; break; }
        }
        const int str = static_cast<int>(counts[it::power_shard_of_strength] + 6 * counts[it::power_emblem_of_strength]);
        const int hp = static_cast<int>(counts[it::power_shard_of_health] + 6 * counts[it::power_emblem_of_health]);
        const it shard = next ? next->shard : (str <= hp ? it::power_shard_of_strength : it::power_shard_of_health);
        if (!me.can_buy(shard, 1)) return;
        try { me.buy(shard, 1); } catch (const cambc::error& e) { warn("buy", e); return; }
    }
}

// ---------------------------------------------------------------- movement

bool Bot::try_dash(const cambc::distance_field& f, const cambc::bitboard& seen) {
    auto& me = cambc::self();
    if (cast_ || !dasher_ || !me.can_cast() || !fuel(12000)) return false;
    const vec2 pos = me.position();
    if (!seen.is_set(pos)) return false;
    const int here = f.get(pos);
    for (const auto& ab : me.abilities()) {
        if (!ab.is_dash() || !ab.is_ready()) continue;
        std::array<std::pair<int, vec2>, 16> cand;
        size_t n = 0;
        for (int dx = -2; dx <= 2; ++dx)
            for (int dy = -2; dy <= 2; ++dy) {
                if (std::max(std::abs(dx), std::abs(dy)) != 2) continue;
                const vec2 t{pos.x + dx, pos.y + dy};
                if (!free_tile(t) || !seen.is_set(t)) continue;
                const int d = f.get(t);
                if (d > here - 2) continue;
                bool path = false;
                for (auto s : kDirs) {
                    const vec2 m{pos.x + s.x, pos.y + s.y};
                    if (!inside(m) || cheb(m, t) != 1) continue;
                    if (g_hard.at(m) || view().lchamps.at(m) || view().lcamps.at(m)) continue;
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
            } catch (const cambc::error&) {}
        }
        return false;
    }
    return false;
}

// One step (and a dash when it gains two) along the shortest route around believed-alive camps and
// visible units; falls back to the plain terrain route when those close the way.
bool Bot::go(const cambc::bitboard& goals_in, bool dash) {
    auto& me = cambc::self();
    if (moved_ || move_failed_ || !me.can_move() || !live()) return false;
    if (!fuel(25000)) return false;
    const vec2 pos = me.position();
    const auto& v = view();
    auto blocked = g_ww | camp_block_ | v.camps | v.champs | v.tele;
    blocked.set(pos, false);
    auto goals = goals_in & ~blocked;
    if (goals.is_set(pos)) return false;
    auto here = empty_board();
    here.set(pos);
    cambc::distance_field f;
    cambc::bitboard seen;
    if (!goals.is_empty()) f.compute_to(blocked, goals, here, seen);
    if (goals.is_empty() || !seen.is_set(pos)) {
        // Camps or units close the way: follow the terrain route and let the farm logic clear plugs.
        auto soft = g_ww | v.camps | v.champs | v.tele;
        soft.set(pos, false);
        auto g2 = goals_in & ~soft;
        if (g2.is_empty() || g2.is_set(pos)) return false;
        f.compute_to(soft, g2, here, seen);
        if (!seen.is_set(pos)) {
            g2 = goals_in & ~g_ww;
            if (g2.is_empty()) return false;
            f.compute_to(g_ww, g2, here, seen);
            if (!seen.is_set(pos)) return false;
        }
        blocked = soft;
    }
    if (dash && f.get(pos) >= 3 && try_dash(f, seen)) {
        moved_ = true;
        if (move_failed_ || !me.can_move()) return true;
    }
    if (!live()) return moved_;
    const vec2 from = me.position();
    if (goals_in.is_set(from)) return true;
    vec2 best = from;
    int best_d = seen.is_set(from) ? f.get(from) : kFar;
    for (auto d : kDirs) {
        const vec2 t{from.x + d.x, from.y + d.y};
        if (!free_tile(t) || !seen.is_set(t)) continue;
        const int fd = f.get(t);
        if (fd < best_d) { best_d = fd; best = t; }
    }
    if (best == from) {
        // A living camp on the only way forward: remember its group so we clear it.
        for (auto d : kDirs) {
            const vec2 t{from.x + d.x, from.y + d.y};
            if (!inside(t) || !seen.is_set(t) || !view().lcamps.at(t)) continue;
            if (f.get(t) >= best_d) continue;
            const int gi = group_at_[static_cast<size_t>(tidx(t))];
            if (gi >= 0) { plug_ = gi; plug_at_ = now(); }
            break;
        }
        return moved_;
    }
    moved_ = true;
    bool ok = false;
    try { ok = me.move_absolute(best); } catch (const cambc::error& e) { warn("move", e); }
    invalidate();
    if (!ok) move_failed_ = true;
    else pos_ = best;
    return ok;
}

// Walk home when it is close, otherwise recall. True when the turn is used.
bool Bot::go_home_or_recall(int walk_limit) {
    auto& me = cambc::self();
    if (cambc::map::spawn_region(team::ally).is_set(pos_)) return false;
    if (home_.at(pos_) <= walk_limit || incoming_ > 0 || now() - hit_at_ <= 2) {
        go(cambc::map::spawn_region(team::ally), true);
        return true;
    }
    if (moved_ || !live()) return moved_;
    try { me.recall(); } catch (const cambc::error& e) { warn("recall", e); }
    return true;
}

// ---------------------------------------------------------------- altar locks

// Lock an altar where an enemy is banking (cancels the channel) or is about to bank.
bool Bot::use_lock() {
    auto& me = cambc::self();
    if (altars_.empty() || !has_lock() || !live()) return false;
    const auto fs = foes();
    if (fs.empty()) return false;
    for (size_t a = 0; a < altars_.size(); ++a) {
        const vec2 ap = altars_[a].pos;
        if (cheb(ap, pos_) > 2) continue;
        bool locked = false;
        try { locked = cambc::vision::has_altar_lock(ap); } catch (const cambc::error&) {}
        if (locked) continue;
        int worth = 0;
        bool channel = false;
        for (const auto& f : fs) {
            if (cheb(f.pos, ap) > 1) continue;
            if (f.channel) { channel = true; worth += std::max(f.gems, 1); }
            else if (f.gems >= 12) worth += f.gems;
        }
        if (!channel && worth < 12) continue;
        try {
            if (me.consumables()[cambc::consumable_type::altar_lock].use(ap)) {
                ++locks_placed_;
                if (channel) ++locks_cancel_;
                lock_gems_ += worth;
                if (kLog) std::cout << "lock r" << now() << " altar " << ap.x << ',' << ap.y << " channel=" << channel
                                    << " gems=" << worth << '\n';
                return true;
            }
        } catch (const cambc::error& e) { warn("lock", e); }
    }
    return false;
}

// ---------------------------------------------------------------- enemies

bool Bot::enemies() {
    auto& me = cambc::self();
    auto fs = foes();
    const auto& enemy_spawn = cambc::map::spawn_region(team::enemy);
    std::vector<Foe> near;
    for (auto& f : fs)
        if (!enemy_spawn.is_set(f.pos) && cheb(f.pos, pos_) <= 9) near.push_back(f);
    if (near.empty()) return false;
    last_enemy_seen_ = now();
    write_sighting(near[0].pos);
    int dmin = 99;
    for (const auto& f : near) dmin = std::min(dmin, cheb(f.pos, pos_));

    // Square law: (sum hp)·(sum power) on each side.
    float our_hp = me.health(), our_pow = 1.1f * me.strength() + 0.25f * me.focus();
    for (const auto& a : g_others) {
        try {
            if (!a.is_alive() || cheb(a.position(), pos_) > 7) continue;
            our_hp += a.health();
            our_pow += 1.1f * a.strength() + 0.25f * a.focus();
        } catch (const cambc::error&) {}
    }
    float their_hp = 0, their_pow = 0;
    bool stuck_foe = false;
    for (const auto& f : near) {
        their_hp += f.hp;
        their_pow += 1.1f * f.power;
        if (f.channel) stuck_foe = true;
        for (const auto& al : altars_)
            if (cheb(al.pos, f.pos) <= 1) {
                bool lk = false;
                try { lk = cambc::vision::has_altar_lock(al.pos) && cambc::vision::altar_lock_at(al.pos).is_ally(); }
                catch (const cambc::error&) {}
                if (lk) stuck_foe = true;  // clearing our lock: punish
            }
    }
    float ratio = (our_hp * our_pow) / std::max(1.0f, their_hp * their_pow);
    if (home_.at(pos_) <= 3) ratio *= 1.5f;
    if (stuck_foe) ratio *= kStuckBonus;
    const bool home = cambc::map::spawn_region(team::ally).is_set(pos_);
    const float need = kFightRatio + std::min(0.5f, static_cast<float>(me.gem_count()) / 100.0f);

    if (ratio < need && !home) {
        // Far enemies that are not on us: keep farming (tribute checks sightings).
        if (dmin >= 6 && me.health() > 0.5f * me.max_health()) return false;
        g_mode = "flee";
        if (me.gem_count() > 0 && altar_.at(pos_) == 0 && !moved_ && tribute(true)) return true;
        if (!ranged_ && me.can_attack() && !attacked_ && live())
            for (const auto& f : near)
                if (cheb(f.pos, pos_) <= 1) {
                    try { me.attack(f.c); attacked_ = true; } catch (const cambc::error& e) { warn("swing", e); }
                    break;
                }
        if (dmin >= 6 && !moved_ && incoming_ <= 0 && now() - hit_at_ > 1 && live()) {
            try { me.recall(); } catch (const cambc::error& e) { warn("flee recall", e); }
            return true;
        }
        go(cambc::map::spawn_region(team::ally), true);
        return true;
    }
    // Strong enough but nobody close: keep farming unless the odds invite a hunt (or a tribute to break).
    if (dmin > 5 && ratio < kHuntRatio && !stuck_foe) return false;
    if (now() < no_chase_until_ && dmin > me.attack_range() + 1) return false;
    g_mode = "fight";

    auto weakest_in = [&](int r) -> const Foe* {
        const Foe* aim = nullptr;
        float best = 1e18f;
        for (const auto& f : near) {
            if (cheb(f.pos, cambc::self().position()) > r || !view().lvis.at(f.pos)) continue;
            const float sc = f.hp - (f.channel ? 400.0f : 0.0f);
            if (sc < best) { best = sc; aim = &f; }
        }
        return aim;
    };
    if (!cast_ && !moved_ && me.can_cast() && live()) {
        for (const auto& ab : me.abilities()) {
            if (cast_ || !ab.is_ready()) continue;
            const bool unit_spell = (ab.is_damage() || ab.is_cc()) && ab.can_target_enemies();
            const bool area_spell = ab.is_aoe_damage() && ab.can_target_ground();
            if (!unit_spell && !area_spell) continue;
            const Foe* aim = weakest_in(static_cast<int>(ability_range(ab)));
            if (!aim) continue;
            try {
                if (unit_spell) ab.use(cambc::vision::champion_at(aim->pos));
                else ab.use(aim->pos);
                cast_ = true;
                invalidate();
            } catch (const cambc::error& e) { warn("enemy spell", e); }
        }
    }
    auto strike = [&]() -> bool {
        const Foe* aim = weakest_in(me.attack_range());
        if (!aim || !me.can_attack() || attacked_ || !live()) return false;
        try {
            if (!cambc::vision::has_champion(aim->pos)) return false;
            auto c = cambc::vision::champion_at(aim->pos);
            if (!c.is_visible() || !c.is_alive()) return false;
            me.attack(c);
            attacked_ = true;
            invalidate();
            after_hit();
            if (!attacked_ && me.can_attack() && cambc::vision::has_champion(aim->pos)) {
                auto again = cambc::vision::champion_at(aim->pos);
                if (again.is_visible() && again.is_alive()) { me.attack(again); attacked_ = true; }
            }
        } catch (const cambc::error& e) { warn("strike", e); }
        return true;
    };
    if (strike()) { chase_turns_ = 0; return true; }
    if (cast_ && ranged_) return true;
    if (me.health() > 0.3f * me.max_health()) {
        if (++chase_turns_ > 5) {
            chase_turns_ = 0;
            no_chase_until_ = now() + 15;
            return false;
        }
        auto goal = empty_board();
        for (const auto& f : near) goal.set(f.pos);
        goal = grow(goal, me.attack_range()) & ~g_ww;
        go(goal, dmin > 2);
        if (!ranged_ && strike()) chase_turns_ = 0;
        return true;
    }
    go(cambc::map::spawn_region(team::ally), true);
    return true;
}

// Attack reset right after our own basic attack.
void Bot::after_hit() {
    auto& me = cambc::self();
    if (cast_ || !attacked_ || !live() || !me.can_cast()) return;
    for (const auto& ab : me.abilities()) {
        if (!ab.is_attack_reset() || !ab.is_ready()) continue;
        try {
            if (ab.use()) { cast_ = true; attacked_ = false; }
        } catch (const cambc::error&) {}
        return;
    }
}

// ---------------------------------------------------------------- tribute

bool Bot::tribute(bool force) {
    auto& me = cambc::self();
    const int gems = static_cast<int>(me.gem_count());
    if (gems <= 0 || altars_.empty()) return false;
    const int d = altar_.at(pos_);
    if (d >= kFar) return false;
    const bool endgame = now() > static_cast<int>(cambc::constants::max_rounds) - 60 - d;
    const bool want = force || endgame || gems >= 45 || (gems >= 20 && d <= 10) || (gems >= 10 && d <= 3)
        || (gems >= 10 && now() - last_enemy_seen_ < 40);
    if (!want) return false;
    if (d == 0) {
        // Pick an adjacent altar without an enemy lock; otherwise break the lock.
        vec2 locked{-1, -1};
        for (const auto& al : altars_) {
            if (cheb(al.pos, pos_) > cambc::constants::submit_gems_range) continue;
            bool enemy_lock = false;
            try {
                if (cambc::vision::has_altar_lock(al.pos)) enemy_lock = cambc::vision::altar_lock_at(al.pos).is_enemy();
            } catch (const cambc::error&) {}
            if (enemy_lock) { locked = al.pos; continue; }
            if (moved_ || !live()) return true;
            try { me.submit_gems(al.pos); return true; } catch (const cambc::error& e) { warn("submit", e); }
        }
        if (locked.x >= 0 && me.can_attack() && !attacked_ && live()) {
            try {
                me.attack(cambc::vision::altar_lock_at(locked));
                attacked_ = true;
                after_hit();
                return true;
            } catch (const cambc::error& e) { warn("unlock", e); }
        }
        return false;
    }
    auto goals = spots_;
    bool any_lock = false;
    try { any_lock = !cambc::vision::altar_lock_tiles(team::enemy).is_empty(); } catch (const cambc::error&) {}
    if (any_lock && altars_.size() > 1) goals = empty_board();
    for (const auto& al : altars_) {
        if (!any_lock || altars_.size() <= 1) break;
        bool enemy_lock = false;
        try {
            if (cambc::vision::has_altar_lock(al.pos)) enemy_lock = cambc::vision::altar_lock_at(al.pos).is_enemy();
        } catch (const cambc::error&) {}
        if (enemy_lock && altars_.size() > 1) continue;
        auto one = empty_board();
        one.set(al.pos);
        goals |= grow(one, cambc::constants::submit_gems_range);
    }
    goals &= ~g_ww;
    go(goals, true);
    return true;
}

// ---------------------------------------------------------------- camps

float Bot::remaining(int gi) const {
    float s = 0;
    for (float h : g_[gi].hp) s += std::max(0.0f, h);
    return s;
}

// Damage we take clearing the group alone, killing the weakest camps first.
float Bot::damage_taken(int gi) const {
    const auto& g = g_[gi];
    std::vector<std::pair<float, float>> c;  // hp, str
    for (size_t i = 0; i < g.tiles.size(); ++i)
        if (g.hp[i] > 0) c.push_back({g.hp[i], camp_spec(g.kinds[i]).str});
    std::sort(c.begin(), c.end());
    float t = 0, taken = 0, str_alive = 0;
    for (auto& x : c) str_alive += x.second;
    for (auto& x : c) {
        const float dt = x.first / dps_;
        taken += str_alive * dt;
        t += dt;
        str_alive -= x.second;
    }
    return taken * taken_;
}

int Bot::choose(int& wait_out) {
    auto& me = cambc::self();
    const int t = now();
    int best = -1;
    float best_s = 0;
    wait_out = 0;
    const float gold_w = 0.35f * std::max(0.4f, 1.0f - static_cast<float>(t) / 1500.0f);
    const float hpmax = me.max_health();
    const bool guarding = guard_ && now() > kGuardFrom;
    const vec2 ga = guarding && guard_altar_ >= 0 ? altars_[guard_altar_].pos : vec2{-100, -100};
    const auto sight = read_sighting();
    std::array<std::uint8_t, 5> claims{};
    cambc::shared_memory.read(0, std::span<std::uint8_t>{claims.data(), claims.size()});
    for (size_t i = 0; i < g_.size(); ++i) {
        const auto& g = g_[i];
        if (g.blocked_until > t) continue;
        const int e = eta(static_cast<int>(i));
        if (e >= kFar) continue;
        int wait = 0;
        if (g.state == Group::respawning) wait = std::max(0, g.ready - t - e);
        if (wait > 60) continue;
        const float value = static_cast<float>(g.gems) + gold_w * static_cast<float>(g.gold);
        if (value <= 0) continue;
        float f = 1.0f;
        if (g.d_enemy < g.d_ally) f *= 0.7f;
        if (g.state == Group::reward) f *= 1.3f;
        if (static_cast<int>(i) == target_ && g.state != Group::respawning) f *= 1.25f;
        if (sight.ok && sight.age < 30 && cheb(sight.where, g.center) <= 8) f *= kSightPenalty;
        if (guarding) {
            const int da = cheb(g.center, ga);
            f *= da <= 10 ? 1.6f : (da <= 16 ? 1.0f : 0.5f);
        }
        // Upper bound before the costly estimates.
        if (value * f / (static_cast<float>(e + wait) + 10.0f) <= best_s) continue;
        float clear = 0, trips = 1;
        if (g.state != Group::reward) {
            clear = remaining(static_cast<int>(i)) / dps_;
            const float taken = damage_taken(static_cast<int>(i));
            trips = std::max(1.0f, std::ceil(taken / std::max(1.0f, 0.7f * hpmax)));
            if (trips > 5) continue;
        }
        const float trip_cost = (trips - 1) * (static_cast<float>(g.d_ally) * 2 + 15);
        int nclaim = 0;
        for (int s = 0; s < 5; ++s)
            if (s != slot_ && claims[static_cast<size_t>(s)] == i + 1) ++nclaim;
        if (nclaim > 0 && clear < 15) f *= 0.3f;
        const float sc = value * f / (static_cast<float>(e + wait) + clear + trip_cost + 10.0f);
        if (sc > best_s) { best_s = sc; best = static_cast<int>(i); wait_out = wait; }
    }
    return best;
}

// Attack a camp in range: the target group first, then any awake camp hitting us.
bool Bot::hit_camps(int gi) {
    auto& me = cambc::self();
    if (!live()) return false;
    const auto& v = view();
    const int range = me.attack_range();
    vec2 tgt{-1, -1};
    float low = 1e18f;
    const auto& g = g_[gi];
    for (size_t c = 0; c < g.tiles.size(); ++c) {
        const vec2 p = g.tiles[c];
        if (!v.lcamps.at(p) || cheb(p, pos_) > range) continue;
        float h = g.hp[c];
        if (h < low) { low = h; tgt = p; }
    }
    if (tgt.x < 0) return false;
    // Damage spells before basic attacks (cast first: some exclude walking).
    if (!cast_ && !moved_ && me.can_cast()) {
        for (const auto& ab : me.abilities()) {
            if (cast_ || !ab.is_ready()) continue;
            if (!(ab.is_damage() || ab.is_aoe_damage())) continue;
            if (static_cast<float>(cheb(tgt, pos_)) > ability_range(ab)) continue;
            try {
                if (ab.is_aoe_damage()) ab.use(tgt);
                else ab.use(cambc::vision::camp_at(tgt));
                cast_ = true;
                invalidate();
            } catch (const cambc::error& e) { warn("camp spell", e); }
        }
    }
    if (!live()) return true;
    if (me.can_attack() && !attacked_) {
        try {
            if (cambc::vision::has_camp(tgt)) {
                me.attack(cambc::vision::camp_at(tgt));
                attacked_ = true;
                invalidate();
                after_hit();
                if (!attacked_ && me.can_attack() && cambc::vision::has_camp(tgt)) {
                    me.attack(cambc::vision::camp_at(tgt));
                    attacked_ = true;
                    invalidate();
                }
            }
        } catch (const cambc::error& e) { warn("camp hit", e); }
    }
    return true;
}

void Bot::farm(int gi) {
    auto& me = cambc::self();
    auto& g = g_[gi];
    if (g.state == Group::reward) {
        // Capture: stand on a spawn tile.
        g_mode = "capture";
        if (g.spawn.is_set(pos_)) return;
        go(g.spawn, true);
        return;
    }
    if (g.state == Group::respawning) {
        // Wait beside the group, never on its spawn tiles (that blocks the respawn).
        g_mode = "wait";
        if (cheb(pos_, g.center) <= g.reach + 2 && !g.spawn.is_set(pos_)) return;
        auto goal = grow(g.spawn, 2) & ~g_ww & ~g.spawn;
        go(goal, true);
        return;
    }
    g_mode = "hit";
    if (hit_camps(gi) && ranged_) return;
    if (!attacked_) g_mode = "approach";
    // Approach living camps (goal cached while the living set stays the same).
    int sig = 1;
    for (size_t c = 0; c < g.tiles.size(); ++c)
        if (g.hp[c] > 0) sig = sig * 31 + static_cast<int>(c) + 1;
    const int r = std::max(1, me.attack_range() - (now() - stuck_since_ > 6 && stuck_g_ == gi ? 1 : 0));
    if (goal_g_ != gi || goal_r_ != r || goal_sig_ != sig) {
        auto living = empty_board();
        for (size_t c = 0; c < g.tiles.size(); ++c)
            if (g.hp[c] > 0) living.set(g.tiles[c]);
        if (living.is_empty()) living = g.spawn;
        goal_ = grow(living, r) & ~g_ww;
        close_ = r > 1 ? grow(living, 1) & ~g_ww : goal_;
        goal_g_ = gi;
        goal_r_ = r;
        goal_sig_ = sig;
    }
    if (goal_.is_set(pos_)) {
        // In range but nothing hit (no line of sight): close in.
        if (!attacked_ && !moved_ && r > 1) go(close_, false);
    } else {
        go(goal_, true);
    }
    if (!ranged_ && !attacked_) hit_camps(gi);
}

// The guard waits next to its altar when it has nothing better to do.
void Bot::guard_post() {
    if (guard_altar_ < 0) return;
    const vec2 a = altars_[guard_altar_].pos;
    if (cheb(a, pos_) <= 2) return;
    if (post_.is_empty()) {
        auto one = empty_board();
        one.set(a);
        post_ = grow(one, 2) & ~g_ww;
    }
    go(post_, true);
}

// ---------------------------------------------------------------- turn

void Bot::play_turn() {
    g_round = cambc::round_number();
    moved_ = attacked_ = cast_ = move_failed_ = false;
    invalidate();
    g_mode = "start";
    turn();
    if (kLog) {
        static std::array<std::pair<const char*, int>, 24> counts{};
        for (auto& c : counts) {
            if (c.first == g_mode) { ++c.second; break; }
            if (!c.first) { c = {g_mode, 1}; break; }
        }
        if (now() % 500 == 0) {
            std::cout << "modes r" << now() << ' ' << cambc::to_string(cambc::self().champion_name());
            for (auto& c : counts) if (c.first) std::cout << ' ' << c.first << '=' << c.second;
            std::cout << '\n';
        }
    }
    if (kLog && (now() % 50 == 0 || (now() >= kLogFrom && now() <= kLogTo))) {
        auto& me = cambc::self();
        std::cout << "r" << now() << ' ' << cambc::to_string(me.champion_name()) << ' ' << g_mode << " at " << pos_.x << ',' << pos_.y
                  << " tgt " << target_ << " gems " << me.gem_count() << " hp " << me.health() << " alt " << altar_.at(pos_)
                  << " g " << (target_ >= 0 ? g_[target_].center.x : -1) << ',' << (target_ >= 0 ? g_[target_].center.y : -1)
                  << " st " << (target_ >= 0 ? int(g_[target_].state) : -1) << " eta " << (target_ >= 0 ? eta(target_) : -1)
                  << " stuck " << stuck_since_ << '/' << best_eta_ << " plug " << plug_
                  << " locks " << locks_placed_ << '/' << locks_cancel_ << '/' << lock_gems_ << " fuel " << cambc::fuel_remaining() << " live " << live() << '\n';
    }
}

void Bot::turn() {
    auto& me = cambc::self();
    if (!init_) initialise();
    if (!me.is_alive()) {
        g_mode = "dead";
        try { shop(); } catch (const cambc::error& e) { warn("dead shop", e); }
        return;
    }
    pos_ = me.position();
    {
        const float h = me.health();
        if (h < last_hp_ - 1.0f) hit_at_ = now();
        last_hp_ = h;
    }
    if (!inside(pos_)) return;
    if (me.is_recalling() || me.is_teleporting()) { g_mode = "recalling"; return; }
    if (me.is_submitting_gems()) { g_mode = "submitting"; return; }
    fill_fields(80000);
    observe();
    rebuild_block();
    measure();
    if (!live()) return;

    const bool on_spawn = cambc::map::spawn_region(team::ally).is_set(pos_);
    if (on_spawn || me.champion_name() == cambc::champion_type::tavi) shop();
    if (on_spawn) {
        shop_trip_ = false;
        if (me.health() < 0.9f * me.max_health() && spawn_wait_ < 3) {
            ++spawn_wait_;
            g_mode = "spawnheal";
            return;
        }
    } else {
        spawn_wait_ = 0;
    }

    // Altar lock: the cheapest and most decisive action we have.
    try { use_lock(); } catch (const std::exception& e) { warn("use_lock", e); }
    if (!live()) return;

    g_mode = "enemies";
    try { if (enemies()) return; } catch (const std::exception& e) { warn("enemies", e); }
    if (!live()) return;

    const float hp = me.health(), mh = me.max_health();
    // An ally reported enemies close by: join the fight while healthy.
    if (kHelp && !on_spawn && hp > 0.6f * mh && me.gem_count() < 30) {
        const auto sight = read_sighting();
        if (sight.ok && sight.age <= 4 && cheb(sight.where, pos_) <= kHelpRange && cheb(sight.where, pos_) > 3
            && home_.at(sight.where) < kFar) {
            g_mode = "help";
            auto goal = empty_board();
            goal.set(sight.where);
            goal = grow(goal, 2) & ~g_ww;
            if (go(goal, true)) return;
        }
    }
    // Damage from nothing we can see (an enemy in a bush): leave.
    if (hit_at_ == now() && incoming_ <= 0 && !on_spawn && hp < 0.9f * mh) {
        g_mode = "ambush";
        go(cambc::map::spawn_region(team::ally), true);
        return;
    }
    // Hurt: go heal.
    if (hp < 0.25f * mh || (incoming_ > 0 && hp < incoming_ * taken_ * 3 + 40)) {
        g_mode = "hurt";
        if (me.gem_count() >= 10 && altar_.at(pos_) <= 2 && tribute(true)) return;
        go_home_or_recall(6);
        return;
    }

    g_mode = "tribute";
    if (tribute(false)) return;
    g_mode = "shoptrip";

    if (!on_spawn && me.gold_count() >= 250 && incoming_ <= 0 && !shop_trip_ && home_.at(pos_) > 3) {
        shop_trip_ = true;
    }
    if (shop_trip_ && incoming_ <= 0 && !on_spawn) {
        go_home_or_recall(6);
        return;
    }

    g_mode = "choose";
    int wait = 0;
    if (target_ < 0 || now() - chosen_at_ >= 3 || g_[target_].state == Group::respawning) {
        const int prev = target_;
        target_ = choose(wait);
        chosen_at_ = now();
        if (target_ != prev) { stuck_g_ = target_; stuck_since_ = now(); best_eta_ = kFar; write_claim(target_); }
    }
    if (plug_ >= 0 && now() - plug_at_ < 25 && g_[plug_].state != Group::respawning && g_[plug_].state != Group::reward) {
        target_ = plug_;
    }
    if (target_ < 0) {
        if (me.gem_count() > 0 && tribute(true)) return;
        if (guard_ && now() > kGuardFrom) guard_post();
        else if (hp < 0.8f * mh) go_home_or_recall(0);
        return;
    }
    // Hurt and the next fight would not finish: heal first.
    if (hp < 0.5f * mh && g_[target_].state == Group::alive && damage_taken(target_) > hp * 0.8f && incoming_ <= 0) {
        g_mode = "heal";
        go_home_or_recall(4);
        return;
    }
    // Waiting for a respawn: bank gems or shop in the meantime.
    if (g_[target_].state == Group::respawning) {
        const int left = g_[target_].ready - now();
        if (me.gem_count() > 0 && left > 2 * altar_.at(pos_) + 4) {
            g_mode = "bankwait";
            if (tribute(true)) return;
        }
        if (me.gold_count() >= 90 && left > 12 + std::min(home_.at(pos_), 10) && incoming_ <= 0) {
            g_mode = "shopwait";
            go_home_or_recall(6);
            return;
        }
    }
    g_mode = "farm";
    // Stuck detection: no progress towards the target for a long time.
    const int e = eta(target_);
    if (e < best_eta_) { best_eta_ = e; stuck_since_ = now(); stuck_g_ = target_; }
    else if (now() - stuck_since_ > 40 && (g_[target_].state == Group::alive || g_[target_].state == Group::unknown) && cheb(pos_, g_[target_].center) > g_[target_].reach + me.attack_range() + 1) {
        g_[target_].blocked_until = now() + 60;
        target_ = -1;
        return;
    }
    farm(target_);
    if (guard_ && now() > kGuardFrom && !moved_ && !attacked_ && g_[target_].state == Group::respawning && wait > 0) guard_post();
}
