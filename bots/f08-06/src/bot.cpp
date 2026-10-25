// Champion decisions: follow the draft's map plan, farm the best camp group, bank gems, fight when ahead.
#include <cambc.h>
#include "bot.h"
#include "plan.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <variant>
#include <vector>
#include <cstdio>

using cambc::vec2;
using cambc::team;

constexpr bool kDebug = false;
constexpr float kMaxTrips = 6;
constexpr float kGoldEarly = 0.5f, kGoldLate = 0.2f;
int dbg_engage = 0, dbg_flee = 0, dbg_ignore = 0;
namespace {

constexpr std::uint16_t kUnreached = cambc::distance_field::unreachable;
constexpr std::array<vec2, 8> kDirs{vec2{0, 1}, vec2{1, 1}, vec2{1, 0}, vec2{1, -1}, vec2{0, -1},
    vec2{-1, -1}, vec2{-1, 0}, vec2{-1, 1}};

int cheb(vec2 a, vec2 b) { return std::max(std::abs(a.x - b.x), std::abs(a.y - b.y)); }

vec2 add(vec2 a, vec2 b) { return vec2{a.x + b.x, a.y + b.y}; }

struct SpellInfo {
    float range = 0;
    bool exclusive = false;
    bool valid = false;
};

SpellInfo spell_info(const cambc::ability& ab) {
    SpellInfo s;
    const auto& info = ab.info();
    if (!info) return s;
    std::visit([&](const auto& v) {
        s.exclusive = v.exclusive_with_movement;
        if constexpr (requires { v.range; }) s.range = v.range;
    }, *info);
    s.valid = true;
    return s;
}

// Shop order: camp damage first, then camp toughness, then raw stats.
cambc::item_type shop_item(int n) {
    if (n < 5) return cambc::item_type::monster_shard_of_strength;
    if (n < 10) return cambc::item_type::jungle_shard_of_health;
    if (n < 15) return cambc::item_type::power_shard_of_strength;
    if (n < 20) return cambc::item_type::durability_shard_of_health;
    return (n % 2) ? cambc::item_type::power_shard_of_health : cambc::item_type::power_shard_of_strength;
}

} // namespace

void Bot::init() {
    auto& me = cambc::self();
    width = cambc::map::width();
    height = cambc::map::height();
    const auto& mem = cambc::draft_memory;
    auto byte = [&](int i) { return std::to_integer<int>(mem[i]); };
    const bool have_plan = byte(plan::kMagic) == 'M';
    plan_kind = have_plan ? byte(plan::kKind) : plan::farm;
    bank_at = have_plan ? std::max(10, byte(plan::kBank)) : 30;
    team_size = have_plan ? std::max(1, byte(plan::kTeam)) : 5;
    const int type = static_cast<int>(me.champion_name());
    slot = (have_plan && type < 12) ? byte(plan::kSlots + type) : 255;
    if (slot >= 8) slot = type % 8;

    terrain = cambc::map::walls() | cambc::map::holes() | cambc::map::altars() | cambc::map::spawn_region(team::enemy);
    owner.assign(static_cast<std::size_t>(width * height), 0);

    const auto list = cambc::map::camp_groups();
    const int ng = std::min<int>(plan::kMaxGroups, static_cast<int>(list.size()));
    groups.reserve(ng);
    for (int g = 0; g < ng; ++g) {
        Group grp{list[g]};
        grp.spawns = list[g].spawn_tiles();
        int sx = 0, sy = 0, n = 0;
        const auto tiles = grp.spawns.positions();
        for (auto p : tiles) { sx += p.x; sy += p.y; ++n; owner[p.y * width + p.x] = static_cast<std::uint8_t>(g + 1); }
        if (n == 0) n = 1;
        grp.center = vec2{sx / n, sy / n};
        for (auto p : tiles) grp.radius = std::max(grp.radius, cheb(p, grp.center));
        for (const auto& c : list[g].camps()) {
            grp.hp += plan::camp_hp(c.kind());
            grp.dps += plan::camp_dps(c.kind());
        }
        grp.hp_left = grp.hp;
        grp.gems = static_cast<float>(list[g].gem_value());
        grp.gold = static_cast<float>(list[g].gold_value());
        if (have_plan && g < byte(plan::kGroups)) {
            const int base = plan::kGroupBase + 4 * g;
            grp.flags = static_cast<std::uint8_t>(byte(base + plan::gFlags));
            grp.dist_us = 2 * byte(base + plan::gDistUs);
            grp.dist_them = 2 * byte(base + plan::gDistThem);
        }
        all_spawns |= grp.spawns;
        groups.push_back(grp);
    }

    cambc::bitboard visited;
    const auto altars = cambc::map::altars();
    altar_field.compute(terrain & ~altars, altars, visited);
    altar_roots = altars;
    spawn_field.compute(terrain, cambc::map::spawn_region(team::ally), visited);
    for (auto p : cambc::map::spawn_region(team::ally).positions())
        altar_from_spawn = std::min<int>(altar_from_spawn, altar_field.get(p));
    ready = true;
}

void Bot::claim(int group) {
    if (slot < 0 || slot >= 8) return;
    cambc::shared_memory[slot] = static_cast<std::uint8_t>(group + 1);
}

void Bot::shop() {
    auto& me = cambc::self();
    for (int i = 0; i < 40 && me.gold_count() >= 30; ++i) {
        const auto item = shop_item(shards);
        if (!me.can_buy(item, 1)) break;
        me.buy(item, 1);
        ++shards;
    }
}

// Update group knowledge from what this champion sees now.
void Bot::observe() {
    auto& me = cambc::self();
    const vec2 pos = me.position();
    const int round = static_cast<int>(cambc::round_number());
    const auto visible = cambc::vision::visible_tiles();
    // Damage already dealt to the visible camps of each group.
    std::vector<float> seen_hp(groups.size(), -1.f);
    for (auto p : cambc::vision::camp_tiles().positions()) {
        const int g = owner[p.y * width + p.x] - 1;
        if (g < 0) continue;
        const auto c = cambc::vision::camp_at(p);
        if (seen_hp[g] < 0) seen_hp[g] = groups[g].hp;
        seen_hp[g] -= std::max(0.f, c.max_health() - c.health());
    }
    for (std::size_t g = 0; g < groups.size(); ++g) {
        auto& grp = groups[g];
        if (cheb(pos, grp.center) > 9 + grp.radius) continue;
        if ((grp.spawns & visible).is_empty()) continue;
        const auto state = grp.handle.state();
        if (std::holds_alternative<cambc::map::camp_group_alive>(state)) {
            grp.ready_round = 0;
            grp.reward = false;
            grp.hp_left = seen_hp[g] >= 0 ? seen_hp[g] : grp.hp;
        } else if (std::holds_alternative<cambc::map::camp_group_reward_available>(state)) {
            grp.ready_round = 0;
            grp.reward = true;
            grp.hp_left = 0;
        } else {
            grp.ready_round = round + std::get<cambc::map::camp_group_respawning>(state).timer;
            grp.reward = false;
            grp.hp_left = grp.hp;
        }
    }
}

// Damage per turn of awake camps that can reach p (with a pad for stepping).
float Bot::camp_threat_near(vec2 p, int range_pad) {
    float total = 0;
    for (auto t : cambc::vision::camp_tiles().positions()) {
        if (cheb(t, p) > 5 + range_pad) continue;
        const auto c = cambc::vision::camp_at(t);
        if (!c.is_awake()) continue;
        if (cheb(t, p) <= c.attack_range() + range_pad) total += c.strength();
    }
    return total;
}

int Bot::choose_target() {
    auto& me = cambc::self();
    const vec2 pos = me.position();
    const int round = static_cast<int>(cambc::round_number());
    float my_dps = std::max(10.f, me.strength());
    const float my_hp = me.max_health();

    std::array<std::uint8_t, 8> claims{};
    cambc::shared_memory.read(0, std::span{claims});

    const float gold_w = round < 600 ? kGoldEarly : kGoldLate;
    const float camp_taken = shards >= 10 ? 0.7f : 1.f; // jungle emblem
    int best = -1;
    float best_score = 0;
    for (std::size_t gi = 0; gi < groups.size(); ++gi) {
        const auto& grp = groups[gi];
        if ((grp.flags & plan::f_skip) || grp.blocked_until > round) continue;
        // Same estimate for every group, so the current target is not judged by a different measure.
        const int eta = std::max(0, cheb(pos, grp.center) - grp.radius) * 5 / 4 + 2;
        if (grp.ready_round > round + eta + 8) continue;
        int others = 0;
        for (int s = 0; s < 8; ++s)
            if (s != slot && claims[s] == gi + 1) ++others;
        const int party = 1 + others;
        const float hp = grp.reward ? 0.f : std::max(1.f, grp.hp_left);
        const float clear = hp / (my_dps * party);
        const float wait = std::max(0, grp.ready_round - round - eta);
        float value = grp.gems + gold_w * grp.gold;
        // Camps never heal: a group too strong for one trip is worn down over several,
        // each extra trip costing a walk home and back.
        float trips = 0;
        if (grp.dps > 0 && !grp.reward) {
            const float incoming = grp.dps * clear * camp_taken;
            const float per_trip = my_hp * 0.65f * party;
            trips = std::ceil(incoming / per_trip) - 1.f;
            if (trips > kMaxTrips) continue;
        }
        const float trip_cost = trips * (12.f + 2.f * grp.dist_us);
        float score = value / (eta + clear + wait + trip_cost + 6.f);
        if (grp.flags & plan::f_boss) score *= plan_kind == plan::boss ? 1.2f : 0.6f;
        // Stay on our half: groups nearer the enemy spawn are where we die with our gems.
        if (grp.dist_them < grp.dist_us) score *= plan_kind == plan::aggro ? 0.5f : 0.3f;
        else if (grp.dist_them < grp.dist_us + 8) score *= 0.75f;
        if (others > 0 && (grp.dps == 0 || grp.hp_left < 1500)) score *= 0.35f;
        if (static_cast<int>(gi) == target) score *= 1.3f;
        if (score > best_score) { best_score = score; best = static_cast<int>(gi); }
    }
    return best;
}

// Step to a free neighbour with a smaller field value; stop once the value is at most stop_at.
// When units block every such step, route around them with a fresh search from the same roots.
bool Bot::step_field(const cambc::distance_field& base, int stop_at, const cambc::bitboard& roots) {
    auto& me = cambc::self();
    if (moved || !me.can_move()) return false;
    const vec2 pos = me.position();
    const int round = static_cast<int>(cambc::round_number());
    // A detour around units stays in use for a while, so we do not step back into the jam.
    const bool use_detour = detour_of == &base && round <= detour_until;
    const cambc::distance_field& field = use_detour ? detour : base;
    const int here = field.get(pos);
    if (here <= stop_at || base.get(pos) <= stop_at) return false;
    const auto occupied = cambc::vision::champion_tiles() | cambc::vision::camp_tiles() | cambc::vision::teleport_tiles();
    vec2 best = pos;
    int best_d = here;
    for (auto d : kDirs) {
        const vec2 n = add(pos, d);
        if (n.x < 0 || n.y < 0 || n.x >= width || n.y >= height) continue;
        if (terrain.is_set(n) || occupied.is_set(n)) continue;
        const int v = field.get(n);
        if (v < best_d) { best_d = v; best = n; }
    }
    if (best == pos) {
        if (++stuck < 2 || cambc::fuel_remaining() < 45000) return false;
        auto walls = terrain | occupied;
        walls &= ~roots;
        walls.set(pos, false);
        cambc::bitboard dest, visited;
        dest.set(pos, true);
        detour.compute(walls, roots, visited);
        detour_of = &base;
        detour_until = round + 12;
        const int dv = detour.get(pos);
        if (dv == kUnreached || dv <= stop_at) { detour_until = 0; return false; }
        const auto step = detour.next_move_from(pos);
        if (!step) return false;
        const vec2 n = add(pos, *step);
        if (n.x < 0 || n.y < 0 || n.x >= width || n.y >= height) return false;
        if (terrain.is_set(n) || occupied.is_set(n)) return false;
        best = n;
    }
    stuck = 0;
    moved = true;
    return me.move_absolute(best);
}

bool Bot::cast_on(vec2 p, bool champion) {
    auto& me = cambc::self();
    if (casted || !me.can_cast()) return false;
    const auto abilities = me.abilities();
    const int d = cheb(me.position(), p);
    for (const auto& ab : abilities) {
        if (!ab.is_ready()) continue;
        const bool offense = ab.is_damage() || ab.is_cc();
        const bool aoe = ab.is_aoe_damage();
        if (!offense && !aoe) continue;
        const auto info = spell_info(ab);
        if (!info.valid || (info.exclusive && moved) || d > info.range) continue;
        bool ok = false;
        if (offense) {
            if (champion) {
                if (!ab.can_target_enemies() || !cambc::vision::has_champion(p)) continue;
                ok = ab.use(cambc::vision::champion_at(p));
            } else {
                if (!ab.can_target_camps() || !cambc::vision::has_camp(p)) continue;
                ok = ab.use(cambc::vision::camp_at(p));
            }
        } else {
            if (!ab.can_target_ground()) continue;
            ok = ab.use(p);
        }
        if (ok) { casted = true; return true; }
    }
    return false;
}

void Bot::after_attack(vec2 p, bool champion) {
    auto& me = cambc::self();
    if (casted || !me.can_cast() || me.can_attack()) return;
    for (const auto& ab : me.abilities()) {
        if (!ab.is_attack_reset() || !ab.is_ready()) continue;
        const auto info = spell_info(ab);
        if (!info.valid || (info.exclusive && moved)) continue;
        if (!ab.use()) return;
        casted = true;
        if (!me.can_attack()) return;
        if (!me.attack_tiles().is_set(p)) return;
        if (champion) {
            if (!cambc::vision::has_champion(p)) return;
            const auto c = cambc::vision::champion_at(p);
            if (c.is_enemy() && c.health() > 0) me.attack(c);
        } else {
            if (!cambc::vision::has_camp(p)) return;
            const auto c = cambc::vision::camp_at(p);
            if (c.health() > 0) me.attack(c);
        }
        return;
    }
}

bool Bot::attack_unit_at(vec2 p, bool champion) {
    auto& me = cambc::self();
    if (!me.can_attack() || !me.attack_tiles().is_set(p)) return false;
    if (champion) {
        if (!cambc::vision::has_champion(p)) return false;
        const auto c = cambc::vision::champion_at(p);
        if (!c.is_enemy() || c.health() <= 0) return false;
        me.attack(c);
    } else {
        if (!cambc::vision::has_camp(p)) return false;
        const auto c = cambc::vision::camp_at(p);
        if (c.health() <= 0) return false;
        me.attack(c);
    }
    attacked = true;
    after_attack(p, champion);
    return true;
}

bool Bot::flee(const std::vector<vec2>& threats) {
    auto& me = cambc::self();
    if (moved || !me.can_move()) return false;
    const vec2 pos = me.position();
    const auto occupied = cambc::vision::champion_tiles() | cambc::vision::camp_tiles() | cambc::vision::teleport_tiles();
    auto score = [&](vec2 t) {
        int md = 99;
        for (auto e : threats) md = std::min(md, cheb(e, t));
        const int home = spawn_field.get(t);
        return md * 4 - (home == kUnreached ? 500 : home);
    };
    vec2 best = pos;
    int best_s = score(pos);
    for (auto d : kDirs) {
        const vec2 n = add(pos, d);
        if (n.x < 0 || n.y < 0 || n.x >= width || n.y >= height) continue;
        if (terrain.is_set(n) || occupied.is_set(n)) continue;
        const int s = score(n);
        if (s > best_s) { best_s = s; best = n; }
    }
    if (best == pos) return false;
    moved = true;
    return me.move_absolute(best);
}

// Enemy champions in sight: fight when the simple exchange favours us, otherwise get away.
bool Bot::fight() {
    auto& me = cambc::self();
    const vec2 pos = me.position();
    const auto enemy_tiles = cambc::vision::champion_tiles(team::enemy);
    if (enemy_tiles.is_empty()) return false;

    struct E { vec2 p; float hp, dps; int range; std::uint32_t gems; };
    std::vector<E> enemies;
    for (auto p : enemy_tiles.positions()) {
        if (cheb(p, pos) > 7) continue;
        const auto c = cambc::vision::champion_at(p);
        enemies.push_back({p, c.health(), c.strength() + 0.3f * c.focus(), c.attack_range(), c.gem_count()});
    }
    if (enemies.empty()) return false;

    float our_hp = 0, our_dps = 0;
    for (auto p : cambc::vision::champion_tiles(team::ally).positions()) {
        if (cheb(p, pos) > 5) continue;
        const auto c = cambc::vision::champion_at(p);
        our_hp += c.health();
        our_dps += c.strength() + 0.3f * c.focus();
    }
    float their_hp = 0, their_dps = 0;
    for (const auto& e : enemies) { their_hp += e.hp; their_dps += e.dps; }
    our_dps = std::max(our_dps, 1.f);
    their_dps = std::max(their_dps, 1.f);
    our_dps += 0.f;
    float bias = plan_kind == plan::aggro ? 0.9f : 1.15f;
    if (me.gem_count() >= 30) bias += 0.25f;
    const float camp_dps = camp_threat_near(pos, 1);
    const float t_kill_them = their_hp / our_dps;
    const float t_kill_us = our_hp / (their_dps + camp_dps);
    const bool engage = t_kill_them * bias < t_kill_us && me.health() > 0.25f * me.max_health();

    if (engage) ++dbg_engage;
    if (engage) {
        // Focus the enemy that dies soonest; prefer ones already in reach.
        const E* best = nullptr;
        float best_v = 1e9f;
        for (const auto& e : enemies) {
            float v = e.hp - (cheb(e.p, pos) <= me.attack_range() ? 300.f : 0.f) - 2.f * e.gems;
            if (v < best_v) { best_v = v; best = &e; }
        }
        const vec2 tp = best->p;
        cast_on(tp, true);
        if (attack_unit_at(tp, true)) return true;
        if (!moved && me.can_move() && cheb(pos, tp) > me.attack_range()) {
            moved = true;
            if (me.move_towards(tp)) {
                if (!casted) cast_on(tp, true);
                attack_unit_at(tp, true);
            }
        }
        return true;
    }

    // Not worth it: leave if anyone can reach us soon.
    std::vector<vec2> threats;
    bool close = false;
    for (const auto& e : enemies) {
        threats.push_back(e.p);
        if (cheb(e.p, pos) <= e.range + 2) close = true;
    }
    if (!close) { ++dbg_ignore; return false; }
    ++dbg_flee;
    // Tributes survive damage: finish one at the altar if we live through the channel.
    const std::uint32_t gems = me.gem_count();
    if (gems > 0) {
        const int turns = static_cast<int>((gems + 9) / 10) + 1;
        for (auto a : cambc::map::altars().positions()) {
            if (cheb(a, pos) > cambc::constants::submit_gems_range) continue;
            if (me.health() > (their_dps + camp_dps) * turns * 1.3f) {
                me.submit_gems(a);
                return true;
            }
        }
        avoid_altar_near(pos);
    }
    if (flee(threats)) return true;
    // Cornered: hit back.
    for (const auto& e : enemies) {
        cast_on(e.p, true);
        if (attack_unit_at(e.p, true)) return true;
    }
    return true;
}

// Enemies guard the altar we were heading for: route to the others for a while.
void Bot::avoid_altar_near(vec2 pos) {
    const int round = static_cast<int>(cambc::round_number());
    const auto altars = cambc::map::altars().positions();
    if (altars.size() < 2 || altar_field.get(pos) > 12) return;
    int best = -1, bd = 1 << 20;
    for (std::size_t i = 0; i < altars.size(); ++i)
        if (cheb(altars[i], pos) < bd) { bd = cheb(altars[i], pos); best = static_cast<int>(i); }
    if (best < 0 || bd > 10) return;
    if (altar_avoid.size() != altars.size()) altar_avoid.assign(altars.size(), 0);
    altar_avoid[best] = round + 60;
    rebuild_altar_field();
}

void Bot::rebuild_altar_field() {
    const int round = static_cast<int>(cambc::round_number());
    const auto altars = cambc::map::altars().positions();
    cambc::bitboard roots;
    for (std::size_t i = 0; i < altars.size(); ++i)
        if (i >= altar_avoid.size() || altar_avoid[i] <= round) roots.set(altars[i], true);
    if (roots.is_empty()) roots = cambc::map::altars();
    altar_roots = roots;
    if (detour_of == &altar_field) detour_until = 0;
    cambc::bitboard visited;
    altar_field.compute(terrain & ~cambc::map::altars(), roots, visited);
    altar_field_until = round + 60;
}

bool Bot::bank(bool force) {
    auto& me = cambc::self();
    const std::uint32_t gems = me.gem_count();
    if (gems == 0) return false;
    const vec2 pos = me.position();
    const int d = altar_field.get(pos);
    if (d == kUnreached) return false;
    const auto ours = cambc::global_submitted_gems(team::ally);
    const int round = static_cast<int>(cambc::round_number());
    const bool endgame = round >= 2000 - d - 30 || ours + gems >= 1000;
    if (!force && !endgame && static_cast<int>(gems) < bank_at && !(gems >= 8 && d <= 4)) return false;
    if (kDebug && cambc::round_number() % 20 == 0) std::printf("BANK r%u c%d pos %d,%d d %d gems %u afs %d stuck %d\n", cambc::round_number(), (int)me.champion_name(), pos.x, pos.y, d, gems, altar_from_spawn, stuck);
    // Far from every altar but the spawn is near one: recall there instead of walking.
    if (d > altar_from_spawn + 10 && camp_threat_near(pos, 1) == 0
        && cambc::vision::champion_tiles(team::enemy).is_empty() && !me.buffs().has_stun()) {
        me.recall();
        return true;
    }
    for (auto a : cambc::map::altars().positions()) {
        if (cheb(a, pos) <= cambc::constants::submit_gems_range) {
            me.submit_gems(a);
            return true;
        }
    }
    step_field(altar_field, 1, altar_roots);
    return true;
}

void Bot::farm() {
    auto& me = cambc::self();
    const vec2 pos = me.position();
    const int t = choose_target();
    if (t != target) idle_hits = 0;
    target = t;
    claim(t);
    if (t < 0) {
        // Nothing worth doing: bank what we carry, else wait at home.
        if (!bank(true)) step_field(spawn_field, 0, cambc::map::spawn_region(team::ally));
        return;
    }
    auto& grp = groups[t];
    if (field_group != t) {
        if (detour_of == &target_field) detour_until = 0;
        // Other groups' camps block the way; route around them.
        cambc::bitboard visited;
        auto walls = terrain | all_spawns;
        const int round = static_cast<int>(cambc::round_number());
        for (const auto& other : groups)
            if (other.reward || other.ready_round > round) walls &= ~other.spawns;
        walls &= ~grp.spawns;
        walls.set(pos, false);
        target_field.compute(walls, grp.spawns, visited);
        field_group = t;
        if (target_field.get(pos) == kUnreached) target_field.compute(terrain & ~grp.spawns, grp.spawns, visited);
        if (target_field.get(pos) == kUnreached) {
            grp.blocked_until = static_cast<int>(cambc::round_number()) + 100;
            target = -1;
            return;
        }
    }

    if (grp.reward) {
        // Capture: stand on a spawn tile of the group.
        if (grp.spawns.is_set(pos)) return;
        step_field(target_field, 0, grp.spawns);
        return;
    }

    // Attack the weakest-dangerous camp of this group within reach (monsters before clusters).
    auto pick_camp = [&]() -> vec2 {
        vec2 best{-1, -1};
        float best_v = 1e9f;
        const auto reach = me.attack_tiles();
        for (auto p : (cambc::vision::camp_tiles() & grp.spawns).positions()) {
            if (!reach.is_set(p)) continue;
            const auto c = cambc::vision::camp_at(p);
            const float hp = c.health();
            if (hp <= 0) continue;
            const float v = hp - (c.strength() > 0 ? 3000.f : 0.f);
            if (v < best_v) { best_v = v; best = p; }
        }
        return best;
    };
    vec2 cp = pick_camp();
    if (cp.x >= 0) {
        idle_hits = 0;
        cast_on(cp, false);
        attack_unit_at(cp, false);
        return;
    }
    // Spells reach further than basic attacks: use one on the nearest camp of the group.
    for (auto p : (cambc::vision::camp_tiles() & grp.spawns).positions())
        if (cheb(p, pos) <= 4 && cast_on(p, false)) break;

    const int ranged = me.attack_range() > 1;
    int stop = ranged ? (idle_hits > 3 ? 1 : 2) : 1;
    if (target_field.get(pos) <= stop) {
        ++idle_hits;
        // Near the group but out of reach: close in on its nearest living camp.
        vec2 near{-1, -1};
        int nd = 99;
        for (auto p : (cambc::vision::camp_tiles() & grp.spawns).positions())
            if (cheb(p, pos) < nd) { nd = cheb(p, pos); near = p; }
        if (near.x >= 0 && !moved && me.can_move()) {
            const auto occupied = cambc::vision::champion_tiles() | cambc::vision::camp_tiles() | cambc::vision::teleport_tiles();
            vec2 best = pos;
            int bd = nd;
            for (auto d : kDirs) {
                const vec2 n = add(pos, d);
                if (n.x < 0 || n.y < 0 || n.x >= width || n.y >= height) continue;
                if (terrain.is_set(n) || occupied.is_set(n)) continue;
                if (cheb(n, near) < bd) { bd = cheb(n, near); best = n; }
            }
            if (best != pos) {
                moved = true;
                if (me.move_absolute(best)) {
                    cp = pick_camp();
                    if (cp.x >= 0 && !ranged) attack_unit_at(cp, false);
                }
                return;
            }
        }
    }
    if (step_field(target_field, stop, grp.spawns) || !moved) {
        cp = pick_camp();
        if (cp.x >= 0 && !ranged) attack_unit_at(cp, false);
    }
}

void Bot::play_turn() {
    static int hist[12] = {};
    int st = turn_inner();
    hist[st]++;
    if (kDebug && (cambc::round_number() == 1000 || cambc::round_number() == 600)) {
        std::printf("HIST c%d", (int)cambc::self().champion_name());
        for (int k = 0; k < 12; ++k) std::printf(" %d", hist[k]);
        std::printf("\n");
    }
}

// Returns which branch handled the turn (debug histogram).
int Bot::turn_inner() {
    auto& me = cambc::self();
    if (!ready) init();
    moved = attacked = casted = false;

    if (!me.is_alive()) { shop(); return 0; }
    if (me.is_recalling() || me.is_submitting_gems() || me.is_teleporting()) return 1;
    if (me.buffs().has_stun() || (!me.can_move() && !me.can_attack() && !me.can_cast())) return 2;

    const vec2 pos = me.position();
    const bool at_spawn = cambc::map::spawn_region(team::ally).is_set(pos);
    if (at_spawn) shop();
    observe();
    if (altar_field_until > 0 && static_cast<int>(cambc::round_number()) >= altar_field_until && cambc::fuel_remaining() > 60000) {
        altar_field_until = 0;
        rebuild_altar_field();
        altar_field_until = 0;
    }

    // Shields whenever something is hitting us.
    const float threat = camp_threat_near(pos, 0);
    const bool enemy_near = !cambc::vision::champion_tiles(team::enemy).is_empty();
    if ((threat > 0 || enemy_near) && me.health() < 0.8f * me.max_health() && me.can_cast()) {
        for (const auto& ab : me.abilities())
            if (ab.is_shield() && ab.is_ready() && ab.use()) { casted = true; break; }
    }

    if (at_spawn && me.health() < 0.9f * me.max_health() && !enemy_near) return 3; // heals at turn end

    if (fight()) return 4;

    // Hurt: get out of camp reach first, then recall.
    const float hp = me.health();
    if (!at_spawn && (hp < 0.3f * me.max_health() || hp < threat * 4.f)) {
        if (threat > 0 || camp_threat_near(pos, 1) > 0) {
            if (!step_field(spawn_field, 0, cambc::map::spawn_region(team::ally))) me.recall();
        } else {
            me.recall();
        }
        return 5;
    }

    if (bank(false)) return 6;

    // Gold to spend: bank what we carry, then go shopping.
    if (!at_spawn && me.gold_count() >= 210 && threat == 0) {
        if (me.gem_count() > 0 && bank(true)) return 7;
        const auto home = spawn_field.get(pos);
        if (home != kUnreached && home > 8) { me.recall(); return 7; }
    }

    farm();
    return attacked ? 8 : moved ? 9 : target < 0 ? 10 : 11;
}
