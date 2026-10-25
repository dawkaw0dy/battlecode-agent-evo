// Champion decisions: fight or flee, heal, bank, farm with the pack.
#include "bot.h"
#include "nav.h"
#include "draft.h"
#include "fight.h"
#include <cambc.h>
#include <algorithm>
#include <cmath>
#include <variant>
#include <iostream>
#include <array>
#include <span>

using cambc::item_type;

namespace {

struct Stage { item_type item; int n; };

std::vector<Stage> plan_for(cambc::champion_type t) {
    std::vector<Stage> p{{item_type::jungle_shard_of_strength, 5}};
    if (t == cambc::champion_type::vesper_vale) p.push_back({item_type::treasure_shard_of_strength, 5});
    p.push_back({item_type::monster_shard_of_strength, 5});
    p.push_back({item_type::power_shard_of_health, 5});
    p.push_back({item_type::power_shard_of_strength, 5});
    p.push_back({item_type::durability_shard_of_health, 5});
    p.push_back({item_type::power_shard_of_health, 5});
    for (int i = 0; i < 20; ++i) p.push_back({i % 2 ? item_type::power_shard_of_health : item_type::power_shard_of_strength, 5});
    return p;
}

// Shared memory is read once per turn into a local copy; writes go to both.
std::array<std::uint8_t, 8> g_sm{};
void sm_load() { cambc::shared_memory.read(0, std::span<std::uint8_t>{g_sm.data(), g_sm.size()}); }
std::uint8_t sm_get(int i) { return g_sm[static_cast<std::size_t>(i)]; }
void sm_set(int i, std::uint8_t v) {
    g_sm[static_cast<std::size_t>(i)] = v;
    cambc::shared_memory[static_cast<std::size_t>(i)] = v;
}

bool has_lock() {
    try {
        return cambc::self().consumables()[cambc::consumable_type::altar_lock].count() > 0;
    } catch (const std::exception&) {
        return false;
    }
}

float unit_dps(const Unit& u) { return u.str + 0.25f * u.focus; }

constexpr bool kUseLocks = true;           // place altar locks at all (A/B switch: on, enemy tributes -7%)
constexpr int kLockRange = 2;              // altar locks are placed within this distance
constexpr int kLockGems = 15;
constexpr bool kPreLock = true;            // lock altars the enemy uses whenever we pass them
constexpr int kPreLockSlack = 10;
constexpr int kLockDetour = 0;             // walk this far to lock such an altar (6 measured worse: 8/20 vs 11/20)          // ... when their spawn is at most this much farther than ours
constexpr int kBreakCost = 12;             // turns an enemy lock adds to an altar (5 hits and the risk)
constexpr int kBankStick = 6;              // keep the chosen altar unless another is this much closer              // lock ahead of an enemy carrying at least this many gems
constexpr float kForeignValue = 0.5f;      // groups on the enemy side
constexpr float kOtherSectorValue = 0.85f; // groups in another champion's sector
constexpr float kCampDamageTaken = 1.0f;   // share of camp damage we expect to take (jungle emblem, stuns)
constexpr bool kUseSim = true;            // decide fights by the focus-fire simulation (else the hp*dps ratio)
constexpr int kSimTurns = 8;               // simulation horizon
constexpr float kEngageValue = 150.0f;     // simulated value needed to start a fight
constexpr float kFleeValue = 100.0f;       // run when the simulation says we lose this much
constexpr float kEngageRatio = 1.5f;       // fight when our hp*dps beats theirs by this much
constexpr float kFleeRatio = 0.85f;        // run when below this and they can reach us
constexpr int kDeepChase = 7;              // never chase targets this close to their spawn
constexpr int kChaseTurns = 4;             // approach turns without a hit before giving up
constexpr int kChaseReach = 2;             // close in only on targets this far beyond our range
constexpr int kRallyTurns = 12;            // wait this long for helpers at a strong group
constexpr bool kUseDanger = false;         // keep farming/banking steps out of enemy reach (measured worse)
constexpr float kGateValue = 400.0f;       // bonus for a group that blocks every altar
constexpr int kExtraHelpers = 2;           // teammates a strong group may call in
constexpr float kCallCost = 10.0f;         // turns lost waiting for called helpers
constexpr int kEscapeMargin = 2;           // extra turns of camp hits budgeted when leaving
constexpr float kRoomHp = 0.55f;           // make room for a teammate below this share of health
constexpr float kTankShare = 0.8f;         // max health needed: this share of a group's hits until out of range
constexpr float kGateTankShare = 0.4f;     // the same for a group that blocks every altar
constexpr int kFleeMemory = 8;             // after running, keep out of their reach this long
constexpr int kRecallSafe = 10;            // no recall with an enemy seen this close in the last 3 rounds
constexpr int kWaryRounds = 10;            // an enemy seen this recently ...
constexpr float kWaryHp = 0.5f;            // ... sends us home below this share of health
constexpr int kSightRounds = 24;           // an enemy sighting counts this long
constexpr float kSightValue1 = 1.0f;       // value of a group near one sighted enemy when alone
constexpr float kSightValue2 = 0.35f;      // ... near two or more

// Put our lock on an altar within reach. Returns true when it was placed.
bool place_lock(vec2 altar) {
    if (!kUseLocks || !has_lock() || cambc::vision::has_altar_lock(altar)) return false;
    if (cheb(cambc::self().position(), altar) > kLockRange) return false;
    try {
        return cambc::self().consumables()[cambc::consumable_type::altar_lock].use(altar);
    } catch (const std::exception&) {
        return false;
    }
}

Field g_from_altars;  // static distance to the nearest altar approach tile
std::vector<int> g_bad_until;
std::uint64_t g_choose_fuel = 0;

}  // namespace

// Which pack each draft slot farms with.
int pack_of(int slot, int n) {
    static const int kPacks[6][5] = {
        {0, 0, 0, 0, 0}, {0, 0, 0, 0, 0}, {0, 1, 0, 0, 0},
        {0, 1, 2, 0, 0},           // 3 champions
        {0, 1, 2, 3, 0},           // 4
        {0, 1, 2, 3, 4},           // 5
    };
    if (n < 1 || n > 5 || slot < 0 || slot > 4) return 0;
    return kPacks[n][slot];
}

float Bot::my_dps() const {
    auto& me = cambc::self();
    float d = me.strength();
    for (const auto& s : kit)
        if (s.valid && (s.kind == cambc::ability_kind::damage || s.kind == cambc::ability_kind::aoe_damage))
            d += me.focus() * s.modifier / 8.0f;
    return d;
}

void Bot::init() {
    inited = true;
    W.init();
    auto& me = cambc::self();
    ranged = me.attack_range() > 1;
    team_n = static_cast<int>(cambc::champions(team::ally).size());
    if (team_n < 1) team_n = 1;
    const auto abs = me.abilities();
    for (int i = 0; i < 2; ++i) {
        Slot s;
        try {
            const auto& inf = abs[static_cast<std::size_t>(i)].info();
            if (!inf) continue;
            std::visit([&](const auto& a) {
                s.valid = true;
                s.kind = a.kind;
                s.excl = a.exclusive_with_movement;
                using cambc::ability_target_mask;
                s.enemies = a.valid_targets.contains(ability_target_mask::enemies);
                s.camps = a.valid_targets.contains(ability_target_mask::camps);
                s.allies = a.valid_targets.contains(ability_target_mask::allies);
                s.ground = a.valid_targets.contains(ability_target_mask::ground);
                s.self = a.valid_targets.contains(ability_target_mask::self);
                using T = std::decay_t<decltype(a)>;
                if constexpr (std::is_same_v<T, cambc::ability_attack_reset>) {
                    s.enhanced = a.enhanced_attack;
                } else if constexpr (std::is_same_v<T, cambc::ability_dash>) {
                    s.range = a.range;
                } else if constexpr (std::is_same_v<T, cambc::ability_aoe_damage>) {
                    s.range = a.range; s.modifier = a.modifier; s.radius = a.radius;
                } else {
                    s.range = a.range; s.modifier = a.modifier;
                }
            }, *inf);
        } catch (const std::exception&) {
            s.valid = false;
        }
        kit[static_cast<std::size_t>(i)] = s;
    }
    const auto order = draft_order();
    const auto my = me.champion_name();
    for (std::size_t i = 0; i < order.size(); ++i)
        if (draft_type_matches(order[i], my)) slot_index = static_cast<int>(i);
    pack = pack_of(slot_index, team_n);
    pack_n = 0;
    packs = 1;
    for (int i = 0; i < team_n; ++i) {
        if (pack_of(i, team_n) == pack) ++pack_n;
        packs = std::max(packs, pack_of(i, team_n) + 1);
    }
    W.partition(packs);

    g_bad_until.assign(W.groups.size(), -1);
    debug_block({
        std::cout << "init " << static_cast<int>(my) << " slot " << slot_index << " pack " << pack << "/" << pack_n
                  << " ours " << W.our_center.x << "," << W.our_center.y << " mirror " << W.mirror(W.our_center).x << ","
                  << W.mirror(W.our_center).y << " theirs " << W.their_center.x << "," << W.their_center.y
                  << " groups " << W.groups.size() << " altars " << W.altars.size() << " fuel " << fuel_left() << "\n";
        if (slot_index == 0) std::cout << "  consts heal " << cambc::constants::spawn_healing << " capture " << cambc::constants::capture_time
            << " respawn " << cambc::constants::respawn_time << " recall " << cambc::constants::recall_time << " sleep " << cambc::constants::camp_sleep_time
            << " passive_gold " << cambc::constants::passive_gold << " lock_hits " << cambc::constants::altar_lock_hits << " ward_cd " << cambc::constants::ward_cooldown << "\n";
        if (slot_index == 0)
            for (const auto& G : W.groups) {
                std::cout << "  G" << G.id << " (" << G.center.x << "," << G.center.y << ") gems " << G.gems << " gold " << G.gold
                          << " resp " << G.respawn_max << " hp " << static_cast<int>(G.hp()) << " str " << static_cast<int>(G.str())
                          << " ours " << G.ours << " theirs " << G.theirs << " sec " << G.sector << (G.island ? " island" : "") << (G.gate ? " GATE" : "") << " :";
                for (const auto& m : G.m) std::cout << " " << static_cast<int>(m.kind);
                std::cout << "\n";
            }
    });
}

void Bot::shop() {
    auto& me = cambc::self();
    try {
        if (!has_lock() && me.gold_count() >= 10 && me.can_buy(cambc::consumable_type::altar_lock))
            me.buy(cambc::consumable_type::altar_lock);
    } catch (const std::exception&) {}
    static const auto plan = plan_for(me.champion_name());
    for (int guard = 0; guard < 40 && stage < static_cast<int>(plan.size()); ++guard) {
        const auto& st = plan[static_cast<std::size_t>(stage)];
        if (stage_bought >= st.n) { ++stage; stage_bought = 0; continue; }
        if (me.gold_count() < st.item.cost()) break;
        bool ok = false;
        try {
            if (me.can_buy(st.item, 1)) { me.buy(st.item, 1); ok = true; }
        } catch (const std::exception&) { ok = false; }
        if (!ok) break;
        ++stage_bought;
    }
}

// ---------------------------------------------------------------- actions

bool Bot::attack_unit_at(vec2 p) {
    auto& me = cambc::self();
    if (!me.can_attack() || cheb(me.position(), p) > me.attack_range() || !bit(W.visible, p)) return false;
    try {
        if (cambc::vision::has_champion(p)) {
            auto c = cambc::vision::champion_at(p);
            if (c.team() == team::ally) return false;
            me.attack(c);
        } else if (cambc::vision::has_camp(p)) {
            auto c = cambc::vision::camp_at(p);
            me.attack(c);
        } else {
            return false;
        }
    } catch (const std::exception&) {
        return false;
    }
    attacked = true;
    return true;
}

bool Bot::reset_and_hit(vec2 p) {
    auto& me = cambc::self();
    if (!attacked || me.can_attack() || casted) return false;
    if (!(cambc::vision::has_champion(p) || cambc::vision::has_camp(p))) return false;
    for (std::size_t i = 0; i < 2; ++i) {
        const auto& s = kit[i];
        if (!s.valid || s.kind != cambc::ability_kind::attack_reset) continue;
        try {
            const auto ab = me.abilities()[i];
            if (!me.can_cast() || !ab.is_ready()) continue;
            if (!ab.use()) continue;
            casted = true;
        } catch (const std::exception&) {
            return false;
        }
        return attack_unit_at(p);
    }
    return false;
}

bool Bot::cast_on_enemy(const Unit& e) {
    auto& me = cambc::self();
    if (casted) return false;
    try {
        if (!me.can_cast()) return false;
    } catch (const std::exception&) { return false; }
    const int d = cheb(me.position(), e.pos);
    // Stuns first on channels and on enemies that can reach us; then damage.
    int best = -1;
    float best_v = 0;
    for (int i = 0; i < 2; ++i) {
        const auto& s = kit[static_cast<std::size_t>(i)];
        if (!s.valid || (s.excl && walked)) continue;
        float v = 0;
        if (s.kind == cambc::ability_kind::cc && s.enemies) {
            if (d > s.range || e.stunned) continue;
            v = e.channel ? 500.0f : 200.0f + me.focus() * s.modifier;
        } else if (s.kind == cambc::ability_kind::damage && s.enemies) {
            if (d > s.range) continue;
            v = me.focus() * s.modifier;
            if (v >= e.hp) v += 400.0f;
        } else if (s.kind == cambc::ability_kind::aoe_damage) {
            if (d > s.range) continue;
            v = me.focus() * s.modifier;
        } else {
            continue;
        }
        if (v > best_v) { best_v = v; best = i; }
    }
    if (best < 0) return false;
    const auto& s = kit[static_cast<std::size_t>(best)];
    try {
        const auto ab = me.abilities()[static_cast<std::size_t>(best)];
        if (!ab.is_ready()) return false;
        bool ok;
        if (s.kind == cambc::ability_kind::aoe_damage) ok = ab.use(e.pos);
        else ok = ab.use(cambc::vision::champion_at(e.pos));
        if (ok) casted = true;
        return ok;
    } catch (const std::exception&) {
        return false;
    }
}

bool Bot::cast_on_camp(const CampSeen& c) {
    auto& me = cambc::self();
    if (casted) return false;
    try {
        if (!me.can_cast()) return false;
    } catch (const std::exception&) { return false; }
    const int d = cheb(me.position(), c.pos);
    for (int i = 0; i < 2; ++i) {
        const auto& s = kit[static_cast<std::size_t>(i)];
        if (!s.valid || (s.excl && walked)) continue;
        const bool dmg = s.kind == cambc::ability_kind::damage || s.kind == cambc::ability_kind::aoe_damage;
        const bool stun = s.kind == cambc::ability_kind::cc && c.str > 0 && c.awake;
        if (!(dmg || stun) || d > s.range) continue;
        if (!s.camps && !s.ground) continue;
        try {
            const auto ab = me.abilities()[static_cast<std::size_t>(i)];
            if (!ab.is_ready()) continue;
            bool ok;
            if (s.kind == cambc::ability_kind::aoe_damage || !s.camps) ok = ab.use(c.pos);
            else ok = ab.use(cambc::vision::camp_at(c.pos));
            if (ok) { casted = true; return true; }
        } catch (const std::exception&) {
            return false;
        }
    }
    return false;
}

bool Bot::cast_support() {
    auto& me = cambc::self();
    if (casted) return false;
    try {
        if (!me.can_cast()) return false;
    } catch (const std::exception&) { return false; }
    for (int i = 0; i < 2; ++i) {
        const auto& s = kit[static_cast<std::size_t>(i)];
        if (!s.valid || (s.excl && walked)) continue;
        try {
            const auto ab = me.abilities()[static_cast<std::size_t>(i)];
            if (s.kind == cambc::ability_kind::heal) {
                const Unit* best = nullptr;
                for (const auto& a : W.allies)
                    if (cheb(a.pos, me.position()) <= s.range && a.hp < a.max_hp - me.focus() * s.modifier * 0.8f
                        && (!best || a.hp / a.max_hp < best->hp / best->max_hp)) best = &a;
                if (best && ab.is_ready() && ab.use(cambc::vision::champion_at(best->pos))) { casted = true; return true; }
            } else if (s.kind == cambc::ability_kind::shield) {
                bool hot = me.health() < me.max_health() * 0.85f;
                bool threat = !W.enemies.empty();
                for (const auto& c : W.camps)
                    if (c.awake && c.str > 0 && cheb(c.pos, me.position()) <= c.range) threat = true;
                if (hot && threat && ab.is_ready() && !me.buffs().has_shield() && ab.use()) { casted = true; return true; }
            }
        } catch (const std::exception&) {
            return false;
        }
    }
    return false;
}

bool Bot::dash_towards(vec2 p, bool away) {
    auto& me = cambc::self();
    if (casted || N.blocked_this_turn) return false;
    for (int i = 0; i < 2; ++i) {
        const auto& s = kit[static_cast<std::size_t>(i)];
        if (!s.valid || s.kind != cambc::ability_kind::dash || (s.excl && walked)) continue;
        try {
            if (!me.can_cast()) return false;
            const auto ab = me.abilities()[static_cast<std::size_t>(i)];
            if (!ab.is_ready()) return false;
            const vec2 pos = me.position();
            const auto tiles = ab.valid_tiles();
            vec2 best = pos;
            int best_v = away ? cheb(pos, p) : -cheb(pos, p);
            const int r = static_cast<int>(s.range);
            for (int dy = -r; dy <= r; ++dy)
                for (int dx = -r; dx <= r; ++dx) {
                    const vec2 q{pos.x + dx, pos.y + dy};
                    if (q == pos || !W.free(q) || !bit(tiles, q)) continue;
                    const int v = away ? cheb(q, p) * 4 - W.from_spawn.at(q) / 8 : -cheb(q, p);
                    if (v > best_v) { best_v = v; best = q; }
                }
            if (best == pos) return false;
            if (ab.use(best)) { casted = true; walked = true; return true; }
            N.blocked_this_turn = true;
        } catch (const std::exception&) {
            return false;
        }
    }
    return false;
}

// ---------------------------------------------------------------- altar control

// Locks block the enemy's tributes at an altar (not ours) and placing one cancels their running channels.
bool Bot::handle_locks() {
    if (!has_lock()) return false;
    const vec2 pos = cambc::self().position();
    for (auto& a : W.altars) {
        if (cheb(a.pos, pos) > kLockRange || cambc::vision::has_altar_lock(a.pos)) continue;
        // An altar they can reach about as easily as we can: lock it on the way past (it never blocks us).
        bool want = kPreLock && a.theirs <= a.ours + kPreLockSlack;
        for (const auto& e : W.enemies) {
            if (e.tribute && cheb(e.pos, a.pos) <= cambc::constants::submit_gems_range) want = true;  // cancel it
            if (e.gems >= kLockGems && cheb(e.pos, a.pos) <= 4) want = true;                          // shut the door
        }
        if (want && place_lock(a.pos)) {
            a.ally_lock_seen = W.round;
            act = "lock";
            ++locks_placed;
            return true;
        }
    }
    return false;
}

// A short walk to lock an enemy-usable altar close by.
bool Bot::lock_detour() {
    if (!kLockDetour || !has_lock() || W.round < 20) return false;
    const vec2 pos = cambc::self().position();
    for (const auto& a : W.altars) {
        if (a.theirs > a.ours + kPreLockSlack || a.approach.empty()) continue;
        if (cheb(a.pos, pos) > kLockDetour || cheb(a.pos, pos) <= kLockRange) continue;
        if (bit(W.visible, a.pos) && cambc::vision::has_altar_lock(a.pos)) continue;
        if (W.round - a.ally_lock_seen < 60) continue;
        cambc::bitboard goal;
        add_square(goal, a.pos, kLockRange);
        goal &= ~W.blocked;
        if (N.step(goal)) {
            walked = true;
            act = "lock-detour";
            handle_locks();
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------- fights

bool Bot::handle_fight() {
    auto& me = cambc::self();
    const vec2 pos = me.position();
    // Remember enemies for a couple of rounds: vision flickers in bushes and around walls.
    for (const auto& e : W.enemies) {
        bool found = false;
        for (auto& r : seen_enemies)
            if (r.u.type == e.type) { r.u = e; r.round = W.round; found = true; }
        if (!found) seen_enemies.push_back({e, W.round});
    }
    if (W.enemies.empty()) {
        bool recent = false;
        for (const auto& r : seen_enemies)
            if (W.round - r.round <= 2 && cheb(r.u.pos, pos) <= 8) recent = true;
        if (!recent) return false;
    }
    last_enemy_round = W.round;
    const float hp = me.health();
    const float dps = my_dps();
    float our_hp = hp, our_dps = dps;
    for (const auto& a : W.allies)
        if (cheb(a.pos, pos) <= 6 && !a.channel) { our_hp += a.hp; our_dps += unit_dps(a); }
    float their_hp = 0, their_dps = 0;
    bool threatened = false;
    std::vector<vec2> threats;
    std::vector<int> reach;
    for (const auto& r : seen_enemies) {
        if (W.round - r.round > 2) continue;
        const auto& e = r.u;
        const int d = cheb(e.pos, pos);
        if (d > 9) continue;
        their_hp += e.hp;
        const float f = e.channel ? 0.3f : (e.stunned ? 0.6f : 1.0f);
        their_dps += unit_dps(e) * f;
        if (d <= e.range + 2) threatened = true;
        threats.push_back(e.pos);
        reach.push_back(e.range + 1);
    }
    if (threats.empty()) return false;
    // Awake camps hitting us fight on nobody's side but cost us.
    for (const auto& c : W.camps)
        if (c.awake && c.str > 0 && cheb(c.pos, pos) <= c.range) their_dps += c.str * 0.5f;
    const float ratio = (our_hp * our_dps) / std::max(1.0f, their_hp * their_dps);
    // Turn-by-turn focus fire with approach delays: who dies, and what it is worth.
    std::vector<SimUnit> sim_o, sim_t;
    sim_o.push_back({pos, hp, dps, me.attack_range(), static_cast<int>(me.gem_count()), ranged, false});
    for (const auto& a : W.allies)
        if (cheb(a.pos, pos) <= 7) sim_o.push_back({a.pos, a.hp, unit_dps(a), a.range, a.gems, a.range > 1, a.channel});
    for (const auto& rs : seen_enemies) {
        if (W.round - rs.round > 2 || cheb(rs.u.pos, pos) > 10) continue;
        const auto& e = rs.u;
        sim_t.push_back({e.pos, e.hp, unit_dps(e), e.range, e.gems, e.range > 1, e.channel || e.stunned});
    }
    for (const auto& c : W.camps)
        if (c.awake && c.str > 0 && cheb(c.pos, pos) <= c.range) sim_o[0].hp -= c.str * 2.0f;  // camps keep hitting us
    if (sim_o[0].hp < 1) sim_o[0].hp = 1;
    const SimResult sim = simulate(sim_o, sim_t, kSimTurns);

    // Best target among the visible: quick kills, carriers and channels.
    const Unit* tgt = nullptr;
    float tgt_v = -1e9f;
    for (const auto& e : W.enemies) {
        const int d = cheb(e.pos, pos);
        if (d > 8) continue;
        float v = -e.hp / std::max(1.0f, our_dps) * 10.0f + static_cast<float>(e.gems) * 0.5f - static_cast<float>(d) * 3.0f;
        if (e.channel) v += 40.0f;
        if (d <= me.attack_range()) v += 25.0f;
        if (e.hp <= me.strength()) v += 100.0f;
        if (v > tgt_v) { tgt_v = v; tgt = &e; }
    }
    const bool in_reach = tgt && cheb(tgt->pos, pos) <= me.attack_range();
    const bool kill_now = in_reach && tgt->hp <= dps;
    // Chasing towards their spawn (which heals them fully) or for too long loses.
    const bool deep = tgt && W.from_spawn.at(W.mirror(tgt->pos)) < kDeepChase;
    const int tgt_d = tgt ? cheb(tgt->pos, pos) : 99;
    const bool may_chase = tgt && !deep && W.round >= chase_block_until
        && (tgt_d <= me.attack_range() + kChaseReach || tgt->channel || tgt->hp <= dps * 2.0f);

    const bool sim_win = sim.value >= kEngageValue && (!sim.we_die || sim.value >= 3.0f * kEngageValue);
    bool engage = (kUseSim ? sim_win : ratio >= kEngageRatio) || (tgt && tgt->channel && (kUseSim ? sim.value >= 0 : ratio >= 0.7f)) || kill_now
        || (tgt && tgt->hp <= dps * 1.5f && hp > me.max_health() * 0.3f);
    if (!in_reach && !may_chase) engage = false;
    debug_block({
        std::cout << "  F r" << W.round << " c" << static_cast<int>(me.champion_name()) << " at " << pos.x << "," << pos.y << " hp " << static_cast<int>(hp)
                  << " ours " << static_cast<int>(our_hp) << "/" << static_cast<int>(our_dps) << " theirs " << static_cast<int>(their_hp) << "/"
                  << static_cast<int>(their_dps) << " ratio " << ratio << " sim " << sim.value << (sim.we_die ? " DIE" : "") << " eng " << engage << " thr " << threatened << " n" << threats.size()
                  << " deep " << deep << " chase " << chase_turns;
        if (tgt) std::cout << " tgt " << tgt->pos.x << "," << tgt->pos.y << " hp " << static_cast<int>(tgt->hp) << " d " << cheb(tgt->pos, pos);
        std::cout << "\n";
    });
    if (engage && tgt) {
        act = "engage";
        const Unit e = *tgt;
        const int d = cheb(e.pos, pos);
        if (d <= me.attack_range()) {
            cast_on_enemy(e);
            attack_unit_at(e.pos);
            reset_and_hit(e.pos);
            chase_turns = 0;
            return true;
        }
        if (++chase_turns > kChaseTurns) { chase_turns = 0; chase_block_until = W.round + 8; }
        // Spells that do not exclude walking, then close in.
        cast_on_enemy(e);
        if (!casted || !(kit[0].excl || kit[1].excl)) {
            if (d >= 4 && !ranged) dash_towards(e.pos, false);
            cambc::bitboard goal;
            add_square(goal, e.pos, ranged ? me.attack_range() : 1);
            if (N.step(goal)) walked = true;
        }
        if (cheb(me.position(), e.pos) <= me.attack_range()) {
            if (attack_unit_at(e.pos)) chase_turns = 0;
            reset_and_hit(e.pos);
        }
        return true;
    }

    const bool losing = kUseSim ? sim.value <= -kFleeValue || (sim.we_die && sim.value < kEngageValue) : ratio < kFleeRatio;
    if (losing && threatened) {
        act = "flee";
        flee_until = W.round + kFleeMemory;
        // Hit something first when that costs nothing (melee may still walk), then run.
        if (!ranged && tgt && cheb(tgt->pos, pos) <= 1) attack_unit_at(tgt->pos);
        for (const auto& e : W.enemies)
            if (!e.stunned && cheb(e.pos, pos) <= 3) {
                for (const auto& sl : kit)
                    if (sl.valid && sl.kind == cambc::ability_kind::cc && !sl.excl) { cast_on_enemy(e); break; }
                break;
            }
        if (!dash_towards(threats[0], true)) {
            if (N.flee(threats, reach)) walked = true;
            else if (!N.blocked_this_turn && tgt) {
                // Cornered: running is not possible, so make them pay.
                act = "cornered";
                cast_on_enemy(*tgt);
                if (cheb(tgt->pos, pos) <= me.attack_range()) {
                    attack_unit_at(tgt->pos);
                    reset_and_hit(tgt->pos);
                } else if (!casted || !(kit[0].excl || kit[1].excl)) {
                    cambc::bitboard goal;
                    add_square(goal, tgt->pos, ranged ? me.attack_range() : 1);
                    if (N.step(goal)) walked = true;
                    if (cheb(me.position(), tgt->pos) <= me.attack_range()) attack_unit_at(tgt->pos);
                }
            }
        }
        return true;
    }

    // Not engaging: keep out of their reach while we farm or bank.
    if (kUseDanger && !sim_win) {
        std::vector<int> keep;
        for (std::size_t i = 0; i < reach.size(); ++i) keep.push_back(reach[i] + 1);
        N.set_danger(threats, keep);
    }
    // Even fight: trade only what is already in reach, keep farming otherwise.
    if (in_reach) {
        act = "trade";
        cast_on_enemy(*tgt);
        attack_unit_at(tgt->pos);
        reset_and_hit(tgt->pos);
        return ranged;  // melee may still walk
    }
    return false;
}

// ---------------------------------------------------------------- health

bool Bot::handle_health() {
    auto& me = cambc::self();
    const vec2 pos = me.position();
    const float hp = me.health();
    const float r = hp / std::max(1.0f, me.max_health());
    const bool at_spawn = bit(W.our_spawn, pos);
    if (at_spawn) {
        if (r < 0.9f && (healing || r < 0.6f)) { healing = true; act = "heal"; return true; }
        healing = false;
        return false;
    }
    // Awake camps hit the nearest champion: count those we are nearest to, and how long leaving takes.
    float incoming = 0;
    int escape = 0;
    bool camp_hits = false;
    std::vector<vec2> threats;
    std::vector<int> reach;
    for (const auto& c : W.camps) {
        if (!c.awake || c.str <= 0) continue;
        const int d = cheb(c.pos, pos);
        if (d > c.range + 1) continue;
        threats.push_back(c.pos);
        reach.push_back(c.range);
        if (d > c.range) continue;
        camp_hits = true;
        bool nearest = true;
        for (const auto& a : W.allies) if (cheb(a.pos, c.pos) < d) { nearest = false; break; }
        if (!nearest) continue;
        incoming += c.str;
        escape = std::max(escape, c.range - d + 1);
    }
    bool enemy_near = false;
    for (const auto& e : W.enemies)
        if (cheb(e.pos, pos) <= e.range + 4) {
            enemy_near = true;
            threats.push_back(e.pos);
            reach.push_back(e.range + 1);
            if (cheb(e.pos, pos) <= e.range + 1) incoming += unit_dps(e);
        }

    // Leave while we still can: the hits until we are out of reach must not kill us.
    const bool must_leave = camp_hits && incoming * static_cast<float>(escape + kEscapeMargin) >= hp;
    debug_block({
        if (r < 0.5f) std::cout << "  H r" << W.round << " c" << static_cast<int>(me.champion_name()) << " hp " << static_cast<int>(hp) << " inc " << incoming
                                << " esc " << escape << " hits " << camp_hits << " leave " << must_leave << " camps " << W.camps.size() << "\n";
    });
    float low = 0.3f;
    if (enemy_near) low = 0.4f;
    // Hurt after a fight with enemies still about: go heal before they come back for us.
    for (const auto& rs : seen_enemies)
        if (W.round - rs.round <= kWaryRounds && cheb(rs.u.pos, pos) <= 12) low = std::max(low, kWaryHp);
    if (r >= low && !must_leave) return false;
    // A camp about to die is worth finishing when it cannot kill us first.
    if (camp_hits && !enemy_near && !must_leave) {
        for (const auto& c : W.camps)
            if (cheb(c.pos, pos) <= me.attack_range() && c.hp <= me.strength() * 2 && r > 0.15f) return false;
    }
    if (camp_hits || enemy_near) {
        act = camp_hits ? "low-camp-flee" : "low-enemy-flee";
        if (enemy_near) flee_until = W.round + kFleeMemory;
        if (!dash_towards(threats.back(), true))
            if (N.flee(threats, reach)) walked = true;
        return true;
    }
    if (r >= low) return false;
    // A recall is a 5-turn channel that any hit cancels: never start it with an enemy close by.
    std::vector<vec2> near_foes;
    std::vector<int> near_reach;
    for (const auto& rs : seen_enemies)
        if (W.round - rs.round <= 3 && cheb(rs.u.pos, pos) <= kRecallSafe) { near_foes.push_back(rs.u.pos); near_reach.push_back(rs.u.range + 2); }
    if (!near_foes.empty()) {
        act = "run-home";
        if (N.flee(near_foes, near_reach)) walked = true;
        return true;
    }
    act = "recall";
    try {
        if (!me.is_recalling()) me.recall();
    } catch (const std::exception& e) {
        debug_block({ std::cout << "  recall error " << e.what() << "\n"; });
    }
    return true;
}

// ---------------------------------------------------------------- banking

bool Bot::can_tribute_safely(int gems) const {
    auto& me = cambc::self();
    const int turns = std::max(1, (gems + 9) / 10);
    float incoming = 0;
    const vec2 pos = me.position();
    for (const auto& e : W.enemies)
        if (cheb(e.pos, pos) <= e.range + turns) incoming += unit_dps(e) * static_cast<float>(turns);
    for (const auto& c : W.camps)
        if (c.awake && cheb(c.pos, pos) <= c.range) incoming += c.str * static_cast<float>(turns);
    return incoming < me.health() * 0.8f;
}

bool Bot::handle_bank() {
    auto& me = cambc::self();
    const int g = static_cast<int>(me.gem_count());
    if (g == 0) { banking = false; return false; }
    const vec2 pos = me.position();
    const int score = static_cast<int>(cambc::global_submitted_gems(team::ally));
    int d = g_from_altars.at(pos);
    if (!g_from_altars.ready()) {
        d = kFar;
        for (const auto& a : W.altars) d = std::min(d, cheb(a.pos, pos) * 5 / 4);
    }
    if (W.all_gated) {
        bool open = false;
        for (const auto& G : W.groups) if (G.gate && G.state != GroupState::alive) open = true;
        if (!open) return false;
    }
    bool want = banking || g >= 45 || (g >= 15 && d <= 8) || W.round >= 1800 || score + g >= 1000;
    // Stay on a reward we are capturing.
    const int gi = W.group_at(pos);
    if (gi >= 0 && W.groups[gi].state == GroupState::reward) return false;
    if (!want || d >= kFar) return false;
    banking = true;
    act = "bank-walk";

    const int ar = cambc::constants::submit_gems_range;
    // Pick one altar: walking distance, plus the turns a known enemy lock costs to break.
    int best = -1, best_c = kFar;
    for (int i = 0; i < static_cast<int>(W.altars.size()); ++i) {
        const auto& a = W.altars[static_cast<std::size_t>(i)];
        if (a.approach.empty()) continue;
        const Field* f = W.altar_field(i, false);
        int c = f ? f->at(pos) : cheb(a.pos, pos) * 5 / 4;
        if (c >= kFar) continue;
        if (a.enemy_locked) c += kBreakCost;
        if (i == bank_altar) c -= kBankStick;  // keep the choice unless another is clearly better
        if (c < best_c) { best_c = c; best = i; }
    }
    if (best < 0) return false;
    bank_altar = best;
    auto& A = W.altars[static_cast<std::size_t>(best)];
    if (cheb(A.pos, pos) > ar) {
        cambc::bitboard goal;
        for (auto q : A.approach) goal.set(q);
        if (N.step(goal, W.altar_field(best))) walked = true;
    }
    if (cheb(A.pos, me.position()) > ar) return true;
    try {
        if (A.enemy_locked || cambc::vision::has_altar_lock(A.pos) && !bit(cambc::vision::altar_lock_tiles(team::ally), A.pos)) {
            // Tributing at a locked altar hits the lock instead.
            if (me.can_attack()) { me.submit_gems(A.pos); attacked = true; act = "break-lock"; }
            return true;
        }
        if (!can_tribute_safely(g)) return true;
        act = "tribute";
        // With enemies about, our own lock keeps theirs (which would cancel us) off this altar.
        if (!W.enemies.empty()) place_lock(A.pos);
        me.submit_gems(A.pos);
        banking = false;
        bank_altar = -1;
    } catch (const std::exception&) {}
    return true;
}

// ---------------------------------------------------------------- farming

int Bot::helpers_on(int gi) const {
    int n = 0;
    for (int k = 0; k < team_n && k < 5; ++k)
        if (k != pack && sm_get(k) == gi) ++n;
    return n;
}

int Bot::choose_group() {
    const std::uint64_t f0 = cambc::fuel_remaining();
    struct Done { std::uint64_t f0; ~Done() { g_choose_fuel += f0 - cambc::fuel_remaining(); } } done{f0};
    auto& me = cambc::self();
    const vec2 pos = me.position();
    const float dps1 = std::max(10.0f, my_dps());
    const float hp1 = me.max_health();
    const float gold_w = W.round < 600 ? 0.15f : (W.round < 1200 ? 0.08f : 0.03f);
    // The team's latest enemy sighting (shared memory bytes 5-7).
    const vec2 sight{sm_get(5), sm_get(6)};
    const int sb = sm_get(7);
    const int sight_count = sb >> 6;
    const int sight_age = sb == 0 ? 999 : (((W.round >> 2) - (sb & 0x3F)) & 0x3F) * 4;
    int best = -1;
    float best_s = -1;
    for (const auto& G : W.groups) {
        if (G.m.empty() || g_bad_until[G.id] > W.round || G.island) continue;
        // The nearest champion takes every hit: it must be able to walk out of range alive.
        if (hp1 < G.full_str * static_cast<float>(G.max_range + 2) * (G.gate ? kGateTankShare : kTankShare)) continue;
        // Terrain distance from the group's own field, else a guess.
        const Field* f = W.group_field(G.id, false);
        const int d = f ? f->at(pos) : cheb(G.center, pos) * 4 / 3 + 2;
        if (d >= kFar) continue;
        int wait = 0;
        float hpv = G.hp(), str = G.str();
        if (G.state == GroupState::respawning) {
            wait = std::max(0, G.respawn_at - W.round);
            hpv = 0; str = 0;
            for (const auto& m : G.m) { hpv += m.max_hp; str += m.str; }
        } else if (G.state == GroupState::reward) {
            hpv = 0; str = 0;
        }
        // Everyone on it shares the work, the hits and the reward. A strong group may be worth calling
        // more teammates to: score it at its best team size (they see our target and the rally waits).
        const int have = helpers_on(G.id) + 1;
        const int most = G.gate ? team_n : std::min(team_n, have + kExtraHelpers);
        float base = static_cast<float>(G.gems) + gold_w * static_cast<float>(G.gold);
        if (G.sector != pack) base *= G.sector < 0 && G.ours > G.theirs + 2 ? kForeignValue : kOtherSectorValue;
        // Enemies seen there lately: alone we are prey.
        if (sight_age <= kSightRounds && cheb(G.center, sight) <= G.radius + 7 && have < 2)
            base *= sight_count >= 2 ? kSightValue2 : kSightValue1;
        if (G.gate && G.state == GroupState::alive) base += kGateValue;
        float s = -1;
        for (int k = have; k <= std::max(have, most); ++k) {
            const float n = static_cast<float>(k);
            const float T = hpv / (dps1 * n * 0.9f);
            const float share_dmg = str * T / n * kCampDamageTaken;
            // Camps do not heal: a share too big for one trip takes several, each a recall and a walk back.
            const float trips = std::max(0.0f, std::ceil(share_dmg / (0.7f * hp1)) - 1.0f);
            const float time = static_cast<float>(std::max(d, wait)) + T + trips * (static_cast<float>(std::min(G.ours, 150)) + 8.0f) + 6.0f;
            // Waiting for helpers who are not there yet costs a little.
            const float sk = base / n / (time + (k > have ? kCallCost : 0.0f));
            s = std::max(s, sk);
        }
        if (s > best_s) { best_s = s; best = G.id; }
    }
    return best;
}

// A hurt teammate pinned by camps needs the tile behind it: step away from the camps if we block it.
bool Bot::make_room() {
    auto& me = cambc::self();
    const vec2 pos = me.position();
    if (me.health() < me.max_health() * 0.5f) return false;
    const Unit* stuck = nullptr;
    for (const auto& a : W.allies) {
        if (cheb(a.pos, pos) > 1 || a.hp >= a.max_hp * kRoomHp) continue;
        // Only when it stands between us and the camps hitting it.
        for (const auto& c : W.camps)
            if (c.awake && c.str > 0 && cheb(c.pos, a.pos) <= c.range && cheb(c.pos, a.pos) < cheb(c.pos, pos)) { stuck = &a; break; }
        if (stuck) break;
    }
    if (!stuck) return false;
    auto camp_dist = [&](vec2 q) {
        int best = 99;
        for (const auto& c : W.camps) if (c.awake && c.str > 0) best = std::min(best, cheb(c.pos, q));
        return best;
    };
    vec2 best = pos;
    int best_v = camp_dist(pos) * 2 + (cheb(pos, stuck->pos) >= 2 ? 3 : 0);
    for (auto d : kDirs) {
        const vec2 q = pos + d;
        if (!N.free(q)) continue;
        const int v = camp_dist(q) * 2 + (cheb(q, stuck->pos) >= 2 ? 3 : 0);
        if (v > best_v) { best_v = v; best = q; }
    }
    if (best == pos || !N.move_to(best)) return false;
    walked = true;
    act = "make-room";
    return true;
}

bool Bot::work_group(int gi) {
    auto& me = cambc::self();
    auto& G = W.groups[static_cast<std::size_t>(gi)];
    const vec2 pos = me.position();
    if (G.state == GroupState::respawning) {
        if (G.respawn_at - W.round > 12) return false;
        act = "wait-respawn";
        // Wait next to it.
        cambc::bitboard goal;
        for (auto t : G.tiles) add_square(goal, t, ranged ? 3 : 1);
        goal &= ~G.spawn;
        if (N.step(goal, W.group_field(gi))) walked = true;
        return true;
    }
    if (G.state == GroupState::reward || G.alive_count() == 0) {
        if (G.state != GroupState::reward && G.seen == W.round) return false;
        act = "capture";
        if (bit(G.spawn, pos)) return true;  // capturing
        act = "to-capture";
        if (N.step(G.spawn, W.group_field(gi))) walked = true;
        return true;
    }

    // Live camps of the group in sight.
    const CampSeen* tgt = nullptr;
    const CampSeen* in_range = nullptr;
    for (const auto& c : W.camps) {
        if (c.group != gi) continue;
        if (!tgt || c.hp < tgt->hp) tgt = &c;
        if (cheb(c.pos, pos) <= me.attack_range() && (!in_range || c.hp < in_range->hp)) in_range = &c;
    }
    if (in_range) tgt = in_range;
    if (tgt && !tgt->awake && G.str() > 0) {
        // Wait for the helpers before waking a group too strong for us alone.
        int present = 1;
        for (const auto& a : W.allies) if (cheb(a.pos, G.center) <= G.radius + 6) ++present;
        const int assigned = helpers_on(gi) + 1;
        const float solo_dmg = G.str() * G.hp() / (std::max(10.0f, my_dps()) * static_cast<float>(present));
        if (assigned > present && solo_dmg / static_cast<float>(present) > me.health() * 0.8f && rally_turns < kRallyTurns) {
            ++rally_turns;
            act = "rally";
            return true;
        }
    }
    if (tgt && make_room()) {
        if (!ranged && cheb(tgt->pos, me.position()) <= me.attack_range()) attack_unit_at(tgt->pos);
        return true;
    }
    if (tgt) {
        rally_turns = 0;
        const CampSeen c = *tgt;
        if (approach_target != gi) { approach_target = gi; approach_r = me.attack_range(); }
        cast_on_camp(c);
        if (cheb(c.pos, pos) <= me.attack_range()) {
            if (attack_unit_at(c.pos)) { reset_and_hit(c.pos); act = "hit-camp"; return true; }
            // In range but unseen: come closer.
            if (approach_r > 1) --approach_r;
        }
        if (casted && (kit[0].excl || kit[1].excl)) { act = "cast-camp"; return true; }
        act = "approach-camp";
        cambc::bitboard goal;
        const int r = std::min(approach_r, me.attack_range());
        add_square(goal, c.pos, r);
        if (ranged && r >= 2) {
            // Keep a step away from every live camp so the melee pick up the hits.
            cambc::bitboard close;
            for (const auto& o : W.camps) if (o.group == gi) add_square(close, o.pos, 1);
            cambc::bitboard far = goal;
            far &= ~close;
            far &= ~W.blocked;
            if (!far.is_empty()) goal = far;
        }
        if (N.step(goal)) {
            walked = true;
            stuck_turns = 0;
        } else if (!bit(goal, me.position())) {
            if (++stuck_turns > 8) { g_bad_until[gi] = W.round + 60; stuck_turns = 0; return false; }
        }
        if (cheb(c.pos, me.position()) <= me.attack_range() && attack_unit_at(c.pos)) { reset_and_hit(c.pos); act = "step-hit-camp"; }
        return true;
    }

    // Walk to the group: arrival by walking distance, not by a square that may reach through walls.
    const Field* gf = W.group_field(gi);
    cambc::bitboard goal;
    for (auto t : G.tiles) add_square(goal, t, 1);
    goal &= ~W.blocked;
    const int wd = gf ? gf->at(pos) : (bit(goal, pos) ? 0 : kFar);
    if (wd == 0) {
        // Beside the group but no camp in sight: they are hidden or gone. Stand on a spawn tile.
        act = "search";
        if (N.step(G.spawn)) walked = true;
        if (++stuck_turns > 6) { stuck_turns = 0; for (auto& m : G.m) m.hp = 0; }
        return true;
    }
    act = "travel";
    if (N.step(goal, gf)) { walked = true; stuck_turns = 0; }
    else if (++stuck_turns > 10) { g_bad_until[gi] = W.round + 60; stuck_turns = 0; return false; }
    return true;
}

void Bot::handle_farm() {
    int gi = sm_get(pack);
    const int n = static_cast<int>(W.groups.size());
    bool valid = gi < n && g_bad_until[static_cast<std::size_t>(gi)] <= W.round;
    if (valid) {
        const auto& G = W.groups[static_cast<std::size_t>(gi)];
        // Only a fresh sighting overrides the team's choice.
        if (G.seen == W.round && G.state == GroupState::respawning && G.respawn_at - W.round > 12) valid = false;
    }
    bool chose = false;
    if (!valid) {
        gi = choose_group();
        chose = true;
        if (gi < 0) return;
        sm_set(pack, static_cast<std::uint8_t>(gi));
    }
    target = gi;
    if (!work_group(gi)) {
        g_bad_until[static_cast<std::size_t>(gi)] = std::max(g_bad_until[static_cast<std::size_t>(gi)], W.round + 1);
        // One choice per turn: it is the most expensive thing we do.
        if (chose || fuel_left() < 50000) { sm_set(pack, 0xFF); return; }
        const int next = choose_group();
        if (next < 0) return;
        sm_set(pack, static_cast<std::uint8_t>(next));
        target = next;
        work_group(next);
    }
}

// ---------------------------------------------------------------- turn

void Bot::play_turn() {
    auto& me = cambc::self();
    if (!inited) {
        init();
        if (W.round <= 2) { for (int k = 0; k < 5; ++k) sm_set(k, 0xFF); sm_set(7, 0); }
    }
    sm_load();
    casted = walked = attacked = false;
    act = "idle";
    g_prof_n = 0;
    prof_mark();
    if (!me.is_alive()) {
        debug_block({ std::cout << "A " << cambc::round_number() << " " << static_cast<int>(me.champion_name()) << " dead\n"; });
        shop();
        if (W.ready && fuel_left() > 80000) { W.round = static_cast<int>(cambc::round_number()); W.precompute_some(); }
        return;
    }
    W.refresh();
    prof_mark();
    N.new_turn();
    if (!g_from_altars.ready() && W.round > 3 && fuel_left() > 85000) {
        cambc::bitboard roots;
        for (const auto& a : W.altars)
            for (auto q : a.approach) roots.set(q);
        if (!roots.is_empty()) g_from_altars.compute(W.blocked, roots);
        debug_block({
            int bad = 0, n = 0;
            for (int y = 0; y < g_h; ++y)
                for (int x = 0; x < g_w; ++x) {
                    const vec2 q{x, y};
                    ++n;
                    if (g_from_altars.at(q) != g_from_altars.f->get(q)) { if (bad < 3) std::cout << "  mismatch " << x << "," << y << " raw " << g_from_altars.at(q) << " get " << g_from_altars.f->get(q) << "\n"; ++bad; }
                }
            std::cout << "field check " << bad << "/" << n << " w " << g_w << " h " << g_h << " fcw " << g_fcw << " bstride " << g_bstride << "\n";
        });
    }
    if (me.is_recalling() || me.is_submitting_gems() || me.is_teleporting()) {
        debug_block({ std::cout << "A " << W.round << " " << static_cast<int>(me.champion_name()) << (me.is_recalling() ? " recalling" : " channel") << "\n"; });
        if (fuel_left() > 80000) W.precompute_some();
        return;
    }
    if (bit(W.our_spawn, me.position())) shop();
    if (!W.enemies.empty()) {
        // Report the enemy nearest to our spawn, with how many we see.
        const Unit* e0 = nullptr;
        for (const auto& e : W.enemies)
            if (!e0 || W.from_spawn.at(e.pos) < W.from_spawn.at(e0->pos)) e0 = &e;
        const int cnt = std::min(3, static_cast<int>(W.enemies.size()));
        sm_set(5, static_cast<std::uint8_t>(e0->pos.x));
        sm_set(6, static_cast<std::uint8_t>(e0->pos.y));
        sm_set(7, static_cast<std::uint8_t>((cnt << 6) | ((W.round >> 2) & 0x3F) | (cnt == 0 ? 1 : 0)));
    }
    cast_support();

    const char* mode = "farm";
    if (W.round < flee_until) {
        // We ran from them lately: do not walk back into their reach while farming or banking.
        std::vector<vec2> at;
        std::vector<int> reach;
        for (const auto& rs : seen_enemies)
            if (W.round - rs.round <= kFleeMemory) { at.push_back(rs.u.pos); reach.push_back(rs.u.range + 2); }
        N.set_danger(at, reach);
    }
    handle_locks();
    if (handle_fight()) mode = "fight";
    else if (handle_health()) mode = "health";
    else if (handle_bank()) mode = "bank";
    else if (lock_detour()) mode = "lock";
    else handle_farm();
    prof_mark();
    debug_block({
        std::cout << "A " << W.round << " " << static_cast<int>(me.champion_name()) << " " << act << (walked ? " w" : "") << (attacked ? " a" : "") << "\n";
        std::cout << "P " << W.round << " " << static_cast<int>(me.champion_name()) << " " << me.position().x << "," << me.position().y << " ld " << N.last_dist << " f " << N.fail << " g " << me.gem_count() << "\n";
        if (W.round % 10 == 0) {
            const vec2 p = me.position();
            std::cout << "r" << W.round << " c" << static_cast<int>(me.champion_name()) << " " << mode << " pos " << p.x << "," << p.y
                      << " hp " << static_cast<int>(me.health()) << " gems " << me.gem_count() << " gold " << me.gold_count()
                      << " tgt " << target << " sm " << static_cast<int>(sm_get(pack));
            if (target >= 0) {
                const auto& G = W.groups[static_cast<std::size_t>(target)];
                std::cout << " G(" << G.center.x << "," << G.center.y << " st" << static_cast<int>(G.state) << " hp" << static_cast<int>(G.hp())
                          << " alive" << G.alive_count() << ")";
            }
            std::cout << " walked " << walked << " att " << attacked << " ld " << N.last_dist << "\n";
        }
    });

    // Free hits on enemies already in reach.
    if (me.can_attack() && !(ranged && walked)) {
        for (const auto& e : W.enemies)
            if (cheb(e.pos, me.position()) <= me.attack_range() && attack_unit_at(e.pos)) break;
    }
    if (fuel_left() > 80000) W.precompute_some();
    prof_mark();
    debug_block({
        const std::uint64_t used = g_prof[0] - g_prof[g_prof_n - 1];
        if (used > 50000) {
            std::cout << "fuel r" << W.round << " " << static_cast<int>(me.champion_name()) << " used " << used << ":";
            for (int i = 1; i < g_prof_n; ++i) std::cout << " " << (g_prof[i - 1] - g_prof[i]);
            std::cout << " choose=" << g_choose_fuel << "\n";
        }
    });
    g_choose_fuel = 0;
}
