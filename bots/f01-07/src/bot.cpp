// Scaling-economy champion: farm gold and gems on our side, turn gold into stats at every
// spawn visit, bank gems at the nearest altar, and fight only when the numbers favour us.
#include <cambc.h>
#include "bot.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <span>
#include <variant>

constexpr bool kDebugMap = false;
constexpr float kBossTurns = 8.0f;  // turns a champion must be able to tank a boss

namespace {
// Debug: turns spent per activity, printed every 200 rounds.
int g_mode = 0;
std::array<int, 20> g_modes{};
const char* kModeNames[20] = {"dead", "chan", "enemy", "fuel", "spwait", "hurt", "bank", "idle", "shop", "heal",
    "stuck", "travel", "fight", "capture", "respawn", "approach", "", "", "", ""};
struct ModeCount {
    ~ModeCount() {
        ++g_modes[static_cast<size_t>(g_mode)];
        const int r = static_cast<int>(cambc::round_number());
        if (r % 200 == 0) {
            std::cout << "stat r" << r << ' ' << cambc::to_string(cambc::self().champion_name());
            for (int i = 0; i < 16; ++i) if (g_modes[static_cast<size_t>(i)]) std::cout << ' ' << kModeNames[i] << '=' << g_modes[static_cast<size_t>(i)];
            std::cout << '\n';
        }
    }
};
}  // namespace

namespace {

using it = cambc::item_type;

// A turn that ran out of fuel resumes next round on stale data: act only in the round we started.
std::uint32_t g_round = 0;
bool live() { return cambc::round_number() == g_round; }
bool fuel(std::uint64_t need) { return cambc::fuel_remaining() > need; }

void warn(const char* where, const std::exception& e) {
    static int count = 0;
    if (++count <= 20) std::cout << "warn: " << where << ": " << e.what() << '\n';
}

// Vision snapshot, refreshed after anything that changes what we see.
struct View {
    cambc::bitboard vis, camps, champs, tele;
    Bits lvis, lcamps, lchamps, ltele;
    std::vector<vec2> allies;  // allied positions, including ones hidden in bushes
    bool ok = false;
};
View g_view;
void invalidate() { g_view.ok = false; }
const View& view() {
    if (!g_view.ok) {
        g_view.vis = cambc::vision::visible_tiles();
        g_view.camps = cambc::vision::camp_tiles();
        g_view.champs = cambc::vision::champion_tiles();
        g_view.tele = cambc::vision::teleport_tiles();
        g_view.allies.clear();
        const int me = static_cast<int>(static_cast<cambc::handle_champion_t>(cambc::self()));
        for (const auto& a : cambc::champions(team::ally)) {
            if (static_cast<int>(static_cast<cambc::handle_champion_t>(a)) == me) continue;
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

const Bits& walls_local() {
    static const Bits b = [] { Bits x; x.load(walk_walls()); return x; }();
    return b;
}
const Bits& hard_local() {  // what a dash cannot cross
    static const Bits b = [] { Bits x; x.load(cambc::map::walls() | cambc::map::altars()); return x; }();
    return b;
}

bool free_tile(vec2 p) {
    if (!inside(p)) return false;
    if (p == cambc::self().position()) return true;
    const auto& v = view();
    return !walls_local().at(p) && !v.lcamps.at(p) && !v.lchamps.at(p) && !v.ltele.at(p);
}

cambc::bitboard blocked_now(const cambc::bitboard& camps) {
    const auto& v = view();
    auto b = walk_walls() | camps | v.camps | v.champs | v.tele;
    b.set(cambc::self().position(), false);
    return b;
}

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

struct Foe { vec2 pos; float hp, max_hp, power; int range; };
std::vector<Foe> foes(team side) {
    std::vector<Foe> out;
    const vec2 me = cambc::self().position();
    for (auto p : cambc::vision::champion_tiles(side).positions()) {
        if (p == me) continue;
        try {
            auto c = cambc::vision::champion_at(p);
            if (!c.is_alive()) continue;
            out.push_back({p, c.health(), c.max_health(), c.strength() + 0.35f * c.focus(), c.attack_range()});
        } catch (const cambc::error&) {}
    }
    return out;
}

// Shared memory: bytes 0-4 hold each slot's target group + 1; bytes 5-7 the latest enemy sighting.
struct Sighting { bool ok = false; vec2 where{}; int age = 999; };
Sighting read_sighting() {
    std::array<std::uint8_t, 3> b{};
    cambc::shared_memory.read(5, std::span<std::uint8_t>{b.data(), b.size()});
    Sighting s;
    if (b[0] == 0) return s;
    s.where = {b[0] - 1, b[1]};
    const int now = static_cast<int>(cambc::round_number() >> 2) & 0xff;
    s.age = ((now - b[2]) & 0xff) * 4;
    s.ok = inside(s.where);
    return s;
}
void write_sighting(vec2 p) {
    std::array<std::uint8_t, 3> b{static_cast<std::uint8_t>(p.x + 1), static_cast<std::uint8_t>(p.y),
        static_cast<std::uint8_t>((cambc::round_number() >> 2) & 0xff)};
    cambc::shared_memory.write(5, std::span<const std::uint8_t>{b.data(), b.size()});
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

}  // namespace

// ---------------------------------------------------------------- setup

void Bot::initialise() {
    init_ = true;
    auto& me = cambc::self();
    g_ = analyse_map();
    h_ = cambc::map::camp_groups();
    k_.assign(g_.size(), Known{});
    for (size_t i = 0; i < g_.size(); ++i)
        for (auto kind : g_[i].kinds) k_[i].hp.push_back(camp_spec(kind).hp);
    gfield_.assign(g_.size(), Dist{});
    group_at_.assign(static_cast<size_t>(map_w() * map_h()), -1);
    camp_at_.assign(static_cast<size_t>(map_w() * map_h()), -1);
    for (size_t i = 0; i < g_.size(); ++i) {
        gtiles_.push_back(h_[i].spawn_tiles());
        for (size_t c = 0; c < g_[i].tiles.size(); ++c) {
            group_at_[static_cast<size_t>(tidx(g_[i].tiles[c]))] = static_cast<std::int16_t>(i);
            camp_at_[static_cast<size_t>(tidx(g_[i].tiles[c]))] = static_cast<std::int16_t>(c);
        }
    }
    size_ = static_cast<int>(cambc::champions(team::ally).size());
    // Cassi and Issac count as one pick.
    slot_ = slot_of(me.champion_name(), size_);
    const auto* mem = cambc::draft_memory.data();
    if (static_cast<char>(mem[dm::magic]) == 'E' && static_cast<int>(mem[dm::ngroups]) == static_cast<int>(std::min<size_t>(g_.size(), 250))) {
        size_ = static_cast<int>(mem[dm::size]);
        slot_ = slot_of(me.champion_name(), size_);
        for (size_t i = 0; i < g_.size(); ++i)
            home_slot_.push_back(i < 250 ? static_cast<std::uint8_t>(mem[dm::home + i]) : 255);
    } else {
        home_slot_ = assign_homes(g_, std::max(1, size_));
    }
    for (const auto& ab : me.abilities())
        if (ab.is_dash()) dasher_ = true;

    cambc::distance_field f;
    cambc::bitboard seen;
    f.compute(walk_walls(), cambc::map::spawn_region(team::ally), seen);
    home_.load(f);
    auto spots = empty_board();
    for (auto a : cambc::map::altars().positions()) {
        auto one = empty_board();
        one.set(a);
        spots |= grow(one, cambc::constants::submit_gems_range);
    }
    spots &= ~walk_walls();
    f.compute(walk_walls(), spots, seen);
    altar_.load(f);
    altar_spots_ = spots;

    for (size_t i = 0; i < g_.size(); ++i) fill_order_.push_back(static_cast<int>(i));
    std::stable_sort(fill_order_.begin(), fill_order_.end(), [&](int a, int b) {
        const bool ma = home_slot_[a] == slot_, mb = home_slot_[b] == slot_;
        if (ma != mb) return ma;
        return g_[a].d_ally < g_[b].d_ally;
    });
}

void Bot::fill_fields(std::uint64_t reserve) {
    for (int i : fill_order_) {
        if (!gfield_[i].empty()) continue;
        if (!fuel(reserve)) return;
        cambc::distance_field f;
        cambc::bitboard seen;
        f.compute(walk_walls(), gtiles_[i], seen);
        gfield_[i].load(f);
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

// ---------------------------------------------------------------- knowledge

void Bot::observe() {
    const int now = static_cast<int>(cambc::round_number());
    for (auto& k : k_)
        if (k.state == Known::respawning && now >= k.ready) k.state = Known::unknown;
    const auto& v = view();
    for (size_t i = 0; i < g_.size(); ++i) {
        if (cheb(g_[i].center, pos_) > 14) continue;
        auto& k = k_[i];
        bool seen = false, living = false;
        for (size_t c = 0; c < g_[i].tiles.size(); ++c) {
            const vec2 p = g_[i].tiles[c];
            if (!v.lvis.at(p)) continue;
            seen = true;
            if (v.lcamps.at(p)) living = true;
            else if (k.state == Known::alive) k.hp[c] = 0;
        }
        if (!seen) continue;
        if (living) {
            // A camp seen alive again after we counted it dead has respawned at full health.
            for (size_t c = 0; c < g_[i].tiles.size(); ++c) {
                const vec2 p = g_[i].tiles[c];
                if (v.lvis.at(p) && v.lcamps.at(p) && k.hp[c] <= 0) k.hp[c] = camp_spec(g_[i].kinds[c]).hp;
            }
            k.state = Known::alive;
            continue;
        }
        if (k.state == Known::respawning) continue;
        try {
            const auto st = h_[i].state();
            if (std::holds_alternative<cambc::map::camp_group_alive>(st)) {
                k.state = Known::alive;
            } else if (std::holds_alternative<cambc::map::camp_group_reward_available>(st)) {
                k.state = Known::reward;
            } else {
                k.state = Known::respawning;
                k.ready = now + std::max(1, std::get<cambc::map::camp_group_respawning>(st).timer);
                for (size_t c = 0; c < g_[i].tiles.size(); ++c) k.hp[c] = camp_spec(g_[i].kinds[c]).hp;
            }
        } catch (const std::exception&) {}
    }
    // Health of the camps of the group we fight.
    if (target_ >= 0 && cheb(g_[target_].center, pos_) <= 10) {
        auto& k = k_[target_];
        for (size_t c = 0; c < g_[target_].tiles.size(); ++c) {
            const vec2 p = g_[target_].tiles[c];
            if (!v.lvis.at(p) || !v.lcamps.at(p) || camp_spec(g_[target_].kinds[c]).str <= 0) continue;
            if (cheb(p, pos_) > 8) continue;
            try { k.hp[c] = cambc::vision::camp_at(p).health(); } catch (const std::exception&) {}
        }
    }
}

void Bot::rebuild_camp_block() {
    auto b = empty_board();
    for (size_t i = 0; i < g_.size(); ++i) {
        const auto& k = k_[i];
        if (k.state == Known::respawning || k.state == Known::reward) continue;
        bool all = true;
        for (float hp : k.hp) if (hp <= 0) { all = false; break; }
        if (all) { b |= gtiles_[i]; continue; }
        for (size_t c = 0; c < g_[i].tiles.size(); ++c)
            if (k.hp[c] > 0) b.set(g_[i].tiles[c]);
    }
    // Visible empty camp tiles are passable whatever we remembered.
    const auto& v = view();
    b &= ~(v.vis & ~v.camps);
    camp_block_ = b;
}

void Bot::measure() {
    auto& me = cambc::self();
    incoming_ = 0;
    exit_steps_ = 0;
    const auto& v = view();
    for (auto p : v.camps.positions()) {
        const int d = cheb(p, pos_);
        if (d > 5) continue;
        const int gi = group_at_[static_cast<size_t>(tidx(p))];
        const int ci = camp_at_[static_cast<size_t>(tidx(p))];
        if (gi >= 0 && ci >= 0 && camp_spec(g_[gi].kinds[ci]).str <= 0) continue;
        try {
            auto c = cambc::vision::camp_at(p);
            const auto s = camp_spec(c.kind());
            // A camp hits the nearest champion: an ally closer to it takes the blows.
            bool shielded = false;
            for (auto a : v.allies)
                if (cheb(a, p) < d) { shielded = true; break; }
            if (c.is_awake() && d <= s.range && !shielded) {
                incoming_ += s.str;
                exit_steps_ = std::max(exit_steps_, s.range + 1 - d);
            }
        } catch (const std::exception&) {}
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

void Bot::write_claim(int group) {
    if (slot_ < 0 || slot_ > 4) return;
    std::uint8_t b = group >= 0 && group < 250 ? static_cast<std::uint8_t>(group + 1) : 0;
    cambc::shared_memory.write(static_cast<size_t>(slot_), std::span<const std::uint8_t>{&b, 1});
}

int Bot::claimed_by_other(int group) const {
    std::array<std::uint8_t, 5> b{};
    cambc::shared_memory.read(0, std::span<std::uint8_t>{b.data(), b.size()});
    for (int s = 0; s < 5; ++s)
        if (s != slot_ && b[static_cast<size_t>(s)] == group + 1) return s;
    return -1;
}

// ---------------------------------------------------------------- shopping

void Bot::shop() {
    auto& me = cambc::self();
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
        // Past the plan: power shards, alternating strength and health.
        const int str = static_cast<int>(counts[it::power_shard_of_strength] + 6 * counts[it::power_emblem_of_strength]);
        const int hp = static_cast<int>(counts[it::power_shard_of_health] + 6 * counts[it::power_emblem_of_health]);
        const it shard = next ? next->shard : (str <= hp ? it::power_shard_of_strength : it::power_shard_of_health);
        if (!me.can_buy(shard, 1)) { cant_buy_ = true; return; }
        try { me.buy(shard, 1); } catch (const cambc::error& e) { warn("buy", e); cant_buy_ = true; return; }
        cant_buy_ = false;
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
                // Need a passable middle tile (holes may be jumped, walls not).
                bool path = false;
                for (auto s : kDirs) {
                    const vec2 m{pos.x + s.x, pos.y + s.y};
                    if (!inside(m) || cheb(m, t) != 1) continue;
                    if (hard_local().at(m) || view().lchamps.at(m) || view().lcamps.at(m)) continue;
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
bool Bot::go(const cambc::bitboard& goals_in, bool dash) {
    auto& me = cambc::self();
    if (moved_ || move_failed_ || !me.can_move() || !live()) return false;
    if (!fuel(25000)) return false;
    const vec2 pos = me.position();
    auto blocked = blocked_now(camp_block_);
    auto goals = goals_in & ~blocked;
    if (goals.is_empty() || goals.is_set(pos)) return false;
    cambc::distance_field f;
    cambc::bitboard seen;
    auto here = empty_board();
    here.set(pos);
    f.compute_to(blocked, goals, here, seen);
    if (!seen.is_set(pos)) {
        // Camps (or units) close the way: find what plugs the walk-only route.
        cambc::distance_field w;
        cambc::bitboard wseen;
        w.compute_to(walk_walls(), goals_in & ~walk_walls(), here, wseen);
        if (!wseen.is_set(pos)) return false;
        // Follow that route through tiles until the first camp on it.
        vec2 at = pos;
        for (int steps = 0; steps < 60; ++steps) {
            vec2 next = at;
            int nd = w.get(at);
            for (auto d : kDirs) {
                const vec2 t{at.x + d.x, at.y + d.y};
                if (!inside(t) || !wseen.is_set(t) || walls_local().at(t)) continue;
                const int fd = w.get(t);
                if (fd < nd) { nd = fd; next = t; }
            }
            if (next == at) break;
            const int gi = group_at_[static_cast<size_t>(tidx(next))];
            if (gi >= 0 && camp_block_.is_set(next)) {
                if (plug_ != gi) { plug_ = gi; plug_since_ = static_cast<int>(cambc::round_number()); }
                return false;
            }
            at = next;
        }
        // Units in the gap: walk the plain route and wait there for them to move.
        f = w;
        seen = wseen;
    }
    if (dash && f.get(pos) >= 3 && try_dash(f, seen)) {
        moved_ = true;
        if (move_failed_ || !me.can_move()) return true;
    }
    const vec2 from = me.position();
    if (goals.is_set(from)) return true;
    vec2 best = from;
    int best_d = seen.is_set(from) ? f.get(from) : kFar;
    for (auto d : kDirs) {
        const vec2 t{from.x + d.x, from.y + d.y};
        if (!free_tile(t) || !seen.is_set(t) || blocked.is_set(t)) continue;
        const int fd = f.get(t);
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
    if (cambc::map::spawn_region(team::ally).is_set(pos_)) return false;
    const int d = home_.at(pos_);
    if (d <= walk_limit) {
        go_home();
        return true;
    }
    if (moved_ || incoming_ > 0 || !live()) return moved_;
    try { me.recall(); } catch (const cambc::error& e) { warn("recall", e); }
    return true;
}

// ---------------------------------------------------------------- camps

float Bot::remaining(int group) const {
    float t = 0;
    for (float hp : k_[group].hp) t += hp;
    return t;
}

// Damage taken clearing the group alone, killing the most dangerous camp per health first.
float Bot::estimate_damage(int group) const {
    std::array<std::pair<float, float>, 48> m;
    size_t n = 0;
    for (size_t c = 0; c < g_[group].kinds.size() && n < m.size(); ++c) {
        const float s = camp_spec(g_[group].kinds[c]).str;
        const float hp = k_[group].hp[c];
        if (s <= 0 || hp <= 0) continue;
        m[n++] = {s, hp};
    }
    std::sort(m.begin(), m.begin() + n, [](auto a, auto b) { return a.first / a.second > b.first / b.second; });
    float t = 0, dmg = 0;
    for (size_t i = 0; i < n; ++i) {
        t += m[i].second / dps_;
        dmg += m[i].first * t;
    }
    return dmg * taken_;
}

bool Bot::hit_group(int group) {
    auto& me = cambc::self();
    if (!me.can_attack() || !live()) return false;
    const vec2 pos = me.position();
    const int range = me.attack_range();
    const float str = me.strength();
    float best_score = -1e9f;
    vec2 best{};
    bool found = false;
    const auto& v = view();
    for (size_t c = 0; c < g_[group].tiles.size(); ++c) {
        const vec2 p = g_[group].tiles[c];
        if (cheb(p, pos) > range || !v.lvis.at(p) || !v.lcamps.at(p)) continue;
        const float hp = std::max(1.0f, k_[group].hp[c]);
        const float s = camp_spec(g_[group].kinds[c]).str;
        float score = s / hp * 1e4f - hp * 1e-3f;
        if (hp <= str) score += 1e6f;  // a kill this hit
        if (score > best_score) { best_score = score; best = p; found = true; }
    }
    if (!found) return false;
    try {
        auto c = cambc::vision::camp_at(best);
        me.attack(c);
        attacked_ = true;
        invalidate();
        const int ci = camp_at_[static_cast<size_t>(tidx(best))];
        if (ci >= 0) k_[group].hp[static_cast<size_t>(ci)] = std::max(0.0f, k_[group].hp[static_cast<size_t>(ci)] - str);
        return true;
    } catch (const cambc::error& e) {
        warn("hit", e);
        return false;
    }
}

void Bot::reset_hit(int group) {
    auto& me = cambc::self();
    if (!attacked_ || cast_ || !fuel(8000) || !live() || me.can_attack() || !me.can_cast()) return;
    for (const auto& ab : me.abilities()) {
        if (!ab.is_attack_reset() || !ab.is_ready()) continue;
        cast_ = true;
        try { ab.use(); } catch (const cambc::error& e) { warn("reset", e); return; }
        if (me.can_attack()) hit_group(group);
        return;
    }
}

// Damage spells on camps; shield while under fire. Cast before walking.
void Bot::spells_on_group(int group) {
    auto& me = cambc::self();
    if (cast_ || moved_ || !live() || !me.can_cast()) return;
    const auto& v = view();
    for (const auto& ab : me.abilities()) {
        if (!ab.is_ready()) continue;
        if (ab.is_shield()) {
            if (incoming_ > 0 && me.health() < me.max_health() * 0.9f) {
                cast_ = true;
                try { ab.use(); } catch (const cambc::error& e) { warn("shield", e); }
                return;
            }
            continue;
        }
        if (!(ab.is_damage() || ab.is_cc() || ab.is_aoe_damage())) continue;
        if (!ab.can_target_camps() && !ab.can_target_ground()) continue;
        const int r = static_cast<int>(ability_range(ab));
        vec2 best{};
        float best_score = -1;
        for (size_t c = 0; c < g_[group].tiles.size(); ++c) {
            const vec2 p = g_[group].tiles[c];
            if (cheb(p, me.position()) > r || !v.lvis.at(p) || !v.lcamps.at(p)) continue;
            const float hp = k_[group].hp[c];
            if (hp <= 0) continue;
            float score = std::min(hp, 400.0f) + camp_spec(g_[group].kinds[c]).str;
            if (ab.is_aoe_damage())
                for (const auto& o : g_[group].tiles)
                    if (o != p && cheb(o, p) <= 1 && v.lcamps.at(o)) score += 300;
            if (score > best_score) { best_score = score; best = p; }
        }
        if (best_score < 0) continue;
        try {
            cast_ = true;
            if (ab.is_aoe_damage() || !ab.can_target_camps()) ab.use(best);
            else ab.use(cambc::vision::camp_at(best));
            invalidate();
        } catch (const cambc::error& e) { warn("spell", e); }
        return;
    }
}

// Step out of reach of awake camps.
bool Bot::escape_camps() {
    std::array<cambc::bitboard, 6> by_range;
    for (auto& b : by_range) b.clear();
    bool any = false;
    for (auto p : view().camps.positions()) {
        if (cheb(p, pos_) > 7) continue;
        try {
            auto c = cambc::vision::camp_at(p);
            const auto s = camp_spec(c.kind());
            if (!c.is_awake() || s.str <= 0) continue;
            by_range[static_cast<size_t>(std::clamp(s.range, 0, 5))].set(p);
            any = true;
        } catch (const std::exception&) {}
    }
    if (!any) return false;
    auto danger = empty_board();
    for (int r = 1; r <= 5; ++r)
        if (!by_range[static_cast<size_t>(r)].is_empty()) danger |= grow(by_range[static_cast<size_t>(r)], r);
    if (!danger.is_set(pos_)) return false;
    auto safe = empty_board();
    safe.fill();
    safe &= ~danger;
    safe &= ~walk_walls();
    return go(safe, true);
}

// ---------------------------------------------------------------- enemies

// Fight a force we outmatch, run from a stronger one. True when the turn is spent.
bool Bot::enemies() {
    auto& me = cambc::self();
    const int now = static_cast<int>(cambc::round_number());
    auto list = foes(team::enemy);
    const auto& enemy_spawn = cambc::map::spawn_region(team::enemy);
    std::erase_if(list, [&](const Foe& e) { return enemy_spawn.is_set(e.pos) || cheb(e.pos, pos_) > 8; });
    if (list.empty()) {
        // Help an ally who reported a lone invader close by.
        const auto s = read_sighting();
        if (s.ok && s.age <= 4 && cheb(s.where, pos_) <= 10 && me.health() > me.max_health() * 0.6f
            && me.gem_count() < 30 && incoming_ == 0) {
            auto goal = empty_board();
            goal.set(s.where);
            goal = grow(goal, 2) & ~walk_walls();
            return go(goal, true);
        }
        return false;
    }
    last_enemy_seen_ = now;
    float theirs = 0;
    int dmin = 99;
    for (const auto& e : list) {
        theirs += e.hp * e.power;
        dmin = std::min(dmin, cheb(e.pos, pos_));
    }
    float ours = me.health() * (me.strength() + 0.35f * me.focus());
    for (const auto& a : foes(team::ally))
        if (cheb(a.pos, pos_) <= 6) ours += a.hp * a.power;
    write_sighting(list.front().pos);

    const bool home = cambc::map::spawn_region(team::ally).is_set(pos_);
    const bool strong = ours >= theirs * 1.2f;
    if (!strong && !home && (me.health() < me.max_health() * 0.85f || list.size() >= 2 || ours < theirs * 0.85f)) {
        // Run: a melee swing first if one is free, recall once nobody is close.
        if (me.attack_range() <= 1 && me.can_attack() && live())
            for (const auto& e : list)
                if (cheb(e.pos, pos_) <= 1 && view().lvis.at(e.pos)) {
                    try {
                        me.attack(cambc::vision::champion_at(e.pos));
                        attacked_ = true;
                        invalidate();
                    } catch (const cambc::error& ex) { warn("swing", ex); }
                    break;
                }
        if (dmin >= 6 && !moved_ && incoming_ == 0) {
            if (live()) try { me.recall(); } catch (const cambc::error& ex) { warn("flee recall", ex); }
            return true;
        }
        go_home();
        return true;
    }

    // Fight: spells on the weakest enemy in range, then basic attacks with a reset.
    if (!live()) return true;
    if (!cast_ && !moved_ && me.can_cast()) {
        for (const auto& ab : me.abilities()) {
            if (cast_ || !ab.is_ready()) continue;
            const bool unit_spell = (ab.is_damage() || ab.is_cc()) && ab.can_target_enemies();
            const bool area_spell = ab.is_aoe_damage() && ab.can_target_ground();
            if (!unit_spell && !area_spell) continue;
            const int r = static_cast<int>(ability_range(ab));
            const Foe* aim = nullptr;
            for (const auto& e : list)
                if (cheb(e.pos, pos_) <= r && view().lvis.at(e.pos) && (!aim || e.hp < aim->hp)) aim = &e;
            if (!aim) continue;
            try {
                if (unit_spell) ab.use(cambc::vision::champion_at(aim->pos));
                else ab.use(aim->pos);
                cast_ = true;
                invalidate();
            } catch (const cambc::error& ex) { warn("enemy spell", ex); }
        }
    }
    auto strike = [&]() -> bool {
        const int range = me.attack_range();
        const vec2 at = me.position();
        const Foe* best = nullptr;
        for (const auto& e : list)
            if (cheb(e.pos, at) <= range && (!best || e.hp < best->hp)) best = &e;
        if (!best || !me.can_attack() || !live()) return false;
        try {
            if (!cambc::vision::has_champion(best->pos)) return false;
            auto c = cambc::vision::champion_at(best->pos);
            if (!c.is_visible() || !c.is_alive()) return false;
            me.attack(c);
            attacked_ = true;
            invalidate();
            if (!cast_ && !me.can_attack() && me.can_cast() && fuel(8000))
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
        } catch (const cambc::error& ex) { warn("strike", ex); }
        return true;
    };
    if (strike()) return true;
    if (cast_) return true;
    if (dmin <= 4 && me.health() > me.max_health() * 0.35f) {
        auto goal = empty_board();
        for (const auto& e : list) goal.set(e.pos);
        goal = grow(goal, me.attack_range()) & ~walk_walls();
        go(goal, false);
        if (me.attack_range() <= 1) strike();
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- gems

bool Bot::tribute() {
    auto& me = cambc::self();
    const int gems = static_cast<int>(me.gem_count());
    if (gems <= 0) return false;
    const auto locks = cambc::vision::altar_lock_tiles(team::enemy);
    for (auto a : cambc::map::altars().positions()) {
        if (cheb(a, pos_) > cambc::constants::submit_gems_range || locks.is_set(a)) continue;
        const float turns = std::ceil(gems / 10.0f) + 1;
        if (me.health() > incoming_ * taken_ * turns + 40) {
            if (live()) try { me.submit_gems(a); } catch (const cambc::error& e) { warn("submit", e); }
            return true;
        }
    }
    if (!locks.is_empty() && me.can_attack() && live())
        for (auto a : locks.positions())
            if (cheb(a, pos_) <= std::min(cambc::constants::submit_gems_range, me.attack_range())) {
                try {
                    me.attack(cambc::vision::altar_lock_at(a));
                    attacked_ = true;
                    return true;
                } catch (const cambc::error& e) { warn("lock", e); }
                break;
            }
    const int d = altar_.at(pos_);
    if (d >= kFar) return false;
    const int now = static_cast<int>(cambc::round_number());
    const int left = static_cast<int>(cambc::constants::max_rounds) - now;
    const auto s = read_sighting();
    const bool danger = (s.ok && s.age <= 30 && cheb(s.where, pos_) <= 16) || now - last_enemy_seen_ <= 20;
    const bool go_now = gems >= 45 || d <= 3 || (gems >= 20 && d <= 10) || (danger && gems >= 10)
        || target_ < 0 || target_wait_ > d || left < d + 25
        || cambc::global_submitted_gems(team::ally) + static_cast<unsigned>(gems) >= cambc::constants::winning_gems;
    if (!go_now) return false;
    auto goal = altar_spots_;
    for (auto a : locks.positions()) {
        auto one = empty_board();
        one.set(a);
        goal &= ~grow(one, cambc::constants::submit_gems_range);
    }
    if ((goal & ~camp_block_).is_empty()) {
        // Every spot is under a living camp (a boss beside the only altar): clear it first.
        for (auto t : (goal & camp_block_).positions()) {
            const int gi = group_at_[static_cast<size_t>(tidx(t))];
            if (gi >= 0) {
                if (plug_ != gi) { plug_ = gi; plug_since_ = now; }
                break;
            }
        }
        return false;
    }
    return go(goal & ~camp_block_, true);
}

// ---------------------------------------------------------------- targets

int Bot::choose(int& wait_out) {
    auto& me = cambc::self();
    const int now = static_cast<int>(cambc::round_number());
    const float hp = me.health(), max_hp = me.max_health();
    const float margin = 100 + 0.1f * max_hp;
    const auto s = read_sighting();
    const float gold_w = (me.champion_name() == cambc::champion_type::vesper_vale ? 0.5f : 0.35f)
        * std::max(0.4f, 1.0f - now / 1500.0f);
    int best = -1;
    float best_score = 0;
    wait_out = 0;
    std::array<std::uint8_t, 5> claims{};
    cambc::shared_memory.read(0, std::span<std::uint8_t>{claims.data(), claims.size()});
    for (int i = 0; i < static_cast<int>(g_.size()); ++i) {
        const auto& g = g_[i];
        const auto& k = k_[i];
        if (k.blocked_until > now) continue;
        float value = g.gems + gold_w * g.gold;
        if (value <= 0) continue;
        const int e = eta(i);
        if (e >= kFar) continue;
        const int avail = k.state == Known::respawning ? k.ready : now;
        const int wait = std::max(0, avail - (now + e));
        float f = 1.0f;
        if (home_slot_.size() == g_.size()) {
            if (home_slot_[i] == 255) f = 0.8f;
            else if (home_slot_[i] != slot_) f = 0.6f;
        }
        if (g.d_enemy + 3 < g.d_ally) f *= 0.7f;
        if (k.state == Known::reward) f *= 1.3f;
        if (i == target_) f *= 1.25f;
        if (s.ok && s.age <= 24 && cheb(s.where, g.center) <= 7) f *= 0.5f;
        int claimers = 0;
        for (int o = 0; o < 5; ++o)
            if (o != slot_ && claims[static_cast<size_t>(o)] == i + 1) ++claimers;
        // Bosses are raided together; anything else is better left to whoever claimed it.
        if (g.boss) f *= 1.0f + 0.3f * claimers;
        else for (int c = 0; c < claimers; ++c) f *= 0.3f;
        const float bound = value * f / (e + wait + 10.0f);
        if (bound <= best_score) continue;
        float clear = 0;
        if (g.boss && k.state != Known::reward) {
            // Allies who join share the work, and a camp hits only the nearest of us.
            const float n = size_ <= 1 ? 1.0f : std::min<float>(static_cast<float>(size_), 2.0f + claimers);
            const float t = remaining(i) / (dps_ * n);
            float hits = 0;
            for (size_t c = 0; c < g.kinds.size(); ++c)
                if (k.hp[c] > 0) hits += camp_spec(g.kinds[c]).str;
            hits *= taken_;
            const float cap = max_hp - margin;
            if (hits > 0 && cap / hits < kBossTurns) continue;  // too frail to tank it yet
            const float trips = std::ceil(hits * t / n / std::max(1.0f, cap));
            if (trips > 10) continue;
            clear = t + (trips - 1) * (12 + std::min(60, g.d_ally));
        } else if (k.state != Known::reward) {
            const float dmg = estimate_damage(i);
            clear = remaining(i) / dps_;
            if (dmg > hp - margin) {
                float hits = 0;
                for (size_t c = 0; c < g.kinds.size(); ++c)
                    if (k.hp[c] > 0) hits += camp_spec(g.kinds[c]).str;
                hits *= taken_;
                const float cap = max_hp - margin;
                if (hits > 0 && cap / hits < 5) continue;  // dies before doing real work
                const float trips = std::ceil(dmg / std::max(1.0f, cap));
                if (trips > 5) continue;
                clear += trips * (12 + std::min(60, g.d_ally));
            }
        }
        const float score = value * f / (e + wait + clear + 10.0f);
        if (score > best_score) { best_score = score; best = i; wait_out = wait; }
    }
    return best;
}

void Bot::farm(int group) {
    auto& me = cambc::self();
    const auto& k = k_[group];
    const int now = static_cast<int>(cambc::round_number());
    const int e = eta(group);

    g_mode = e > 3 ? 11 : 15;
    if (k.state == Known::reward) {
        g_mode = 13;
        const auto& tiles = gtiles_[group];
        if (tiles.is_set(pos_)) {
            if (capture_g_ != group) { capture_g_ = group; capture_since_ = now; }
            if (now - capture_since_ > cambc::constants::capture_time + 6) {
                k_[group].blocked_until = now + 60;
                capture_g_ = -1;
            }
            return;
        }
        go(tiles, e > 3);
        return;
    }
    capture_g_ = -1;
    if (k.state == Known::respawning) {
        g_mode = 14;
        // Wait close by but off the spawn tiles: standing on one delays the respawn.
        auto near = grow(gtiles_[group], 2) & ~gtiles_[group] & ~walk_walls();
        if (!near.is_set(pos_)) go(near, true);
        return;
    }
    spells_on_group(group);
    if (hit_group(group)) {
        g_mode = 12;
        reset_hit(group);
        return;
    }
    if (cast_) g_mode = 12;
    // Approach tiles in attack range of living (or unseen) camps of the group.
    const auto& v = view();
    const auto living = gtiles_[group] & ~(v.vis & ~v.camps);
    if (shrink_g_ != group) { shrink_g_ = group; shrink_ = 0; }
    const int r = std::max(1, me.attack_range() - shrink_);
    auto goal = grow(living, r) & ~walk_walls();
    if (goal.is_empty()) goal = grow(gtiles_[group], 1) & ~walk_walls();
    // In range but nothing to hit: walls hide the camps, come closer.
    if (goal.is_set(pos_) && !attacked_ && r > 1) {
        ++shrink_;
        goal = grow(living, r - 1) & ~walk_walls();
    }
    go(goal, e > 3);
    if (hit_group(group)) reset_hit(group);
}

// ---------------------------------------------------------------- turn


void Bot::play_turn() {
    ModeCount counter;
    g_mode = 0;
    g_round = cambc::round_number();
    auto& me = cambc::self();
    pos_ = me.position();
    if (!init_) {
        initialise();
        if (slot_ == 0 && kDebugMap) {
            Bits wl, ho, al, sa, se, bu;
            wl.load(cambc::map::walls()); ho.load(cambc::map::holes()); al.load(cambc::map::altars());
            sa.load(cambc::map::spawn_region(team::ally)); se.load(cambc::map::spawn_region(team::enemy)); bu.load(cambc::map::bushes());
            for (int y = map_h() - 1; y >= 0; --y) {
                std::string row = "map ";
                for (int x = 0; x < map_w(); ++x) {
                    const vec2 p{x, y};
                    char c = '.';
                    if (wl.at(p)) c = '#';
                    else if (ho.at(p)) c = 'o';
                    else if (al.at(p)) c = 'A';
                    else if (sa.at(p)) c = 'S';
                    else if (se.at(p)) c = 'E';
                    else if (bu.at(p)) c = ',';
                    const int gi = group_at_[static_cast<size_t>(tidx(p))];
                    if (gi >= 0) c = "wegGMK"[static_cast<int>(g_[gi].kinds[static_cast<size_t>(camp_at_[static_cast<size_t>(tidx(p))])])];
                    row += c;
                }
                std::cout << row << '\n';
            }
        }
    }
    moved_ = attacked_ = cast_ = move_failed_ = false;
    invalidate();

    if (!me.is_alive()) {
        g_mode = 0;
        write_claim(-1);
        shop();
        fill_fields(30000);
        return;
    }
    const bool at_spawn = cambc::map::spawn_region(team::ally).is_set(pos_);
    if (at_spawn || me.champion_name() == cambc::champion_type::tavi) shop();
    observe();
    if (me.is_recalling() || me.is_submitting_gems() || me.is_teleporting()) {
        g_mode = 1;
        fill_fields(30000);
        return;
    }
    measure();
    rebuild_camp_block();
    g_mode = 2;
    try {
        if (enemies()) return;
    } catch (const cambc::error& e) { warn("enemies", e); }
    g_mode = 3;
    if (!fuel(25000)) return;
    g_mode = 4;

    const float hp = me.health(), max_hp = me.max_health();
    if (!at_spawn) spawn_wait_ = 0;
    else if (hp < max_hp * 0.95f && spawn_wait_ < 2) { ++spawn_wait_; return; }

    // Badly hurt: leave camp range, then recall.
    g_mode = 5;
    if (!at_spawn && (hp < max_hp * 0.2f || (incoming_ > 0 && hp < incoming_ * taken_ * (exit_steps_ + 2) + 40))) {
        if (escape_camps()) return;
        if (incoming_ == 0 && recall_or_walk(4)) return;
    }

    const int now = static_cast<int>(cambc::round_number());
    if (target_ < 0 || now - chosen_at_ >= 3 || k_[target_].state == Known::respawning) {
        int wait = 0;
        const int t = choose(wait);
        if (t != target_) { stuck_g_ = t; stuck_since_ = now; best_eta_ = kFar; }
        target_ = t;
        target_wait_ = wait;
        chosen_at_ = now;
        write_claim(t);
    }
    if (plug_ >= 0) {
        float hits = 0;
        for (size_t c = 0; c < g_[plug_].kinds.size(); ++c)
            if (k_[plug_].hp[c] > 0) hits += camp_spec(g_[plug_].kinds[c]).str;
        if (g_[plug_].boss && target_ >= 0 && (max_hp - 100 - 0.1f * max_hp) / std::max(1.0f, hits * taken_) < kBossTurns) {
            k_[target_].blocked_until = now + 60;
            target_ = -1;
            plug_ = -1;
            return;
        }
        if (k_[plug_].state != Known::alive || now - plug_since_ > 40 || remaining(plug_) <= 0) plug_ = -1;
        else if (hp > max_hp * 0.4f) {
            farm(plug_);
            return;
        }
    }
    g_mode = 6;
    if (tribute()) return;
    g_mode = 7;

    const int t = target_;
    if (t < 0 || target_wait_ > 15) {
        if (!at_spawn && (me.gold_count() >= 60 || hp < max_hp * 0.8f) && incoming_ == 0 && me.gem_count() == 0) {
            recall_or_walk(5);
            return;
        }
        if (t < 0) {
            fill_fields(30000);
            return;
        }
    }
    // Gold buys nothing in the field: go and spend a full purse between fights.
    g_mode = 8;
    if (!at_spawn && me.gold_count() >= 240 && me.gem_count() == 0 && incoming_ == 0 && !cant_buy_
        && me.champion_name() != cambc::champion_type::tavi
        && (k_[t].state != Known::alive || eta(t) > 6) && remaining(t) >= g_[t].hp * 0.99f) {
        if (recall_or_walk(8)) return;
    }
    // Heal first when the next fight would not finish at this health.
    g_mode = 9;
    if (!at_spawn && hp < max_hp * 0.5f && incoming_ == 0 && k_[t].state != Known::reward
        && estimate_damage(t) > hp - 100) {
        if (recall_or_walk(5)) return;
    }
    // No progress towards the target for a long time: give it up for a while.
    g_mode = 10;
    {
        const int e = eta(t);
        if (e < best_eta_ || e <= 3 || attacked_) stuck_since_ = now;
        best_eta_ = e;
        if (now - stuck_since_ > 20) {
            k_[t].blocked_until = now + 40;
            target_ = -1;
            return;
        }
    }
    if (now % 100 == 0)
        std::cout << "dbg r" << now << ' ' << cambc::to_string(me.champion_name()) << " pos " << pos_.x << ',' << pos_.y
                  << " t " << t << " eta " << eta(t) << " st " << int(k_[t].state) << " plug " << plug_
                  << " ctr " << g_[t].center.x << ',' << g_[t].center.y << " hp " << hp << '\n';
    farm(t);
    fill_fields(40000);
}
