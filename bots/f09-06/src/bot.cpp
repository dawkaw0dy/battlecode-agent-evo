// Champion decisions: fight or flee, heal, bank, farm with the pack.
#include "bot.h"
#include "nav.h"
#include "draft.h"
#include <cambc.h>
#include <algorithm>
#include <cmath>
#include <variant>
#include <iostream>

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

std::uint8_t sm_get(int i) { return static_cast<std::uint8_t>(cambc::shared_memory[static_cast<std::size_t>(i)]); }
void sm_set(int i, std::uint8_t v) { cambc::shared_memory[static_cast<std::size_t>(i)] = v; }

bool has_lock() {
    try {
        return cambc::self().consumables()[cambc::consumable_type::altar_lock].count() > 0;
    } catch (const std::exception&) {
        return false;
    }
}

float unit_dps(const Unit& u) { return u.str + 0.25f * u.focus; }

constexpr float kCampDamageTaken = 1.0f;  // share of camp damage we expect to take (jungle emblem, stuns)

Field g_from_altars;  // static distance to the nearest altar approach tile
std::vector<int> g_bad_until;
std::uint64_t g_choose_fuel = 0;

}  // namespace

// Which pack each draft slot farms with.
int pack_of(int slot, int n) {
    static const int kPacks[6][5] = {
        {0, 0, 0, 0, 0}, {0, 0, 0, 0, 0}, {0, 0, 0, 0, 0},
        {0, 0, 1, 0, 0},           // 3 champions
        {0, 0, 1, 1, 0},           // 4
        {0, 0, 1, 1, 2},           // 5
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
    for (int i = 0; i < team_n; ++i) if (pack_of(i, team_n) == pack) ++pack_n;

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
                          << " ours " << G.ours << " theirs " << G.theirs << (G.island ? " island" : "") << " :";
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

// ---------------------------------------------------------------- fights

bool Bot::handle_fight() {
    auto& me = cambc::self();
    if (W.enemies.empty()) return false;
    last_enemy_round = W.round;
    const vec2 pos = me.position();
    const float hp = me.health();
    const float dps = my_dps();
    float our_hp = hp, our_dps = dps;
    for (const auto& a : W.allies)
        if (cheb(a.pos, pos) <= 6 && !a.channel) { our_hp += a.hp; our_dps += unit_dps(a); }
    float their_hp = 0, their_dps = 0;
    bool threatened = false;
    std::vector<vec2> threats;
    std::vector<int> reach;
    for (const auto& e : W.enemies) {
        const int d = cheb(e.pos, pos);
        if (d > 9) continue;
        their_hp += e.hp;
        float f = e.channel ? 0.3f : (e.stunned ? 0.6f : 1.0f);
        their_dps += unit_dps(e) * f;
        if (d <= e.range + 2) threatened = true;
        threats.push_back(e.pos);
        reach.push_back(e.range + 1);
    }
    if (threats.empty()) return false;
    const float ratio = (our_hp * our_dps) / std::max(1.0f, their_hp * their_dps);

    // Best target: quick kills, carriers and channels.
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

    const bool engage = ratio >= 1.3f || (tgt && tgt->channel && ratio >= 0.7f)
        || (tgt && tgt->hp <= dps * 1.5f && hp > me.max_health() * 0.3f);
    if (engage && tgt) {
        const Unit e = *tgt;
        const int d = cheb(e.pos, pos);
        if (d <= me.attack_range()) {
            cast_on_enemy(e);
            attack_unit_at(e.pos);
            reset_and_hit(e.pos);
            return true;
        }
        // Spells that do not exclude walking, then close in.
        cast_on_enemy(e);
        if (!casted || !kit[0].excl) {
            if (d >= 4 && !ranged) dash_towards(e.pos, false);
            cambc::bitboard goal;
            add_square(goal, e.pos, ranged ? me.attack_range() : 1);
            if (N.step(goal)) walked = true;
        }
        if (cheb(me.position(), e.pos) <= me.attack_range()) {
            attack_unit_at(e.pos);
            reset_and_hit(e.pos);
        }
        return true;
    }

    if (ratio < 0.8f && threatened) {
        // Hit something first when that costs nothing (melee may still walk), then run.
        if (!ranged && tgt && cheb(tgt->pos, pos) <= 1) attack_unit_at(tgt->pos);
        for (const auto& e : W.enemies)
            if (!e.stunned && cheb(e.pos, pos) <= 3) {
                for (const auto& s : kit)
                    if (s.valid && s.kind == cambc::ability_kind::cc && !s.excl) { cast_on_enemy(e); break; }
                break;
            }
        if (!dash_towards(threats[0], true))
            if (N.flee(threats, reach)) walked = true;
        return true;
    }

    // Even fight: trade only what is already in reach, keep farming otherwise.
    if (tgt && cheb(tgt->pos, pos) <= me.attack_range()) {
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
    const float r = me.health() / std::max(1.0f, me.max_health());
    const bool at_spawn = bit(W.our_spawn, pos);
    if (at_spawn) {
        if (r < 0.9f && (healing || r < 0.6f)) { healing = true; return true; }
        healing = false;
        return false;
    }
    bool camp_hits = false;
    std::vector<vec2> threats;
    std::vector<int> reach;
    for (const auto& c : W.camps)
        if (c.awake && c.str > 0 && cheb(c.pos, pos) <= c.range + 1) {
            if (cheb(c.pos, pos) <= c.range) camp_hits = true;
            threats.push_back(c.pos);
            reach.push_back(c.range);
        }
    bool enemy_near = false;
    for (const auto& e : W.enemies)
        if (cheb(e.pos, pos) <= e.range + 4) { enemy_near = true; threats.push_back(e.pos); reach.push_back(e.range + 1); }

    float low = 0.3f;
    if (enemy_near) low = 0.4f;
    if (r >= low) return false;
    // A camp about to die is worth finishing.
    if (camp_hits && !enemy_near) {
        for (const auto& c : W.camps)
            if (cheb(c.pos, pos) <= me.attack_range() && c.hp <= me.strength() * 2 && r > 0.15f) return false;
    }
    if (camp_hits || enemy_near) {
        if (enemy_near) dash_towards(threats.back(), true);
        if (N.flee(threats, reach)) walked = true;
        return true;
    }
    try {
        if (!me.is_recalling()) me.recall();
    } catch (const std::exception&) {}
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
    bool want = banking || g >= 45 || (g >= 15 && d <= 8) || W.round >= 1800 || score + g >= 1000;
    // Stay on a reward we are capturing.
    const int gi = W.group_at(pos);
    if (gi >= 0 && W.groups[gi].state == GroupState::reward) return false;
    if (!want || d >= kFar) return false;
    banking = true;

    const int ar = cambc::constants::submit_gems_range;
    const auto enemy_locks = cambc::vision::altar_lock_tiles(team::enemy);
    for (auto& a : W.altars) {
        if (cheb(a.pos, pos) > ar) continue;
        const bool locked = bit(enemy_locks, a.pos);
        if (locked) a.enemy_lock_seen = W.round;
        try {
            if (locked) {
                if (me.can_attack()) { me.submit_gems(a.pos); attacked = true; return true; }
                continue;
            }
            if (!can_tribute_safely(g)) return false;
            me.submit_gems(a.pos);
            banking = false;
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }
    cambc::bitboard goal;
    bool any = false;
    for (const auto& a : W.altars) {
        if (bit(enemy_locks, a.pos) || W.round - a.enemy_lock_seen < 30) continue;
        for (auto q : a.approach) goal.set(q);
        any = true;
    }
    if (!any)
        for (const auto& a : W.altars)
            for (auto q : a.approach) goal.set(q);
    if (N.step(goal, any && g_from_altars.ready() ? &g_from_altars : nullptr)) walked = true;
    // Arrived this turn: start at once.
    for (const auto& a : W.altars) {
        if (cheb(a.pos, me.position()) > ar || bit(enemy_locks, a.pos)) continue;
        if (!can_tribute_safely(g)) break;
        try {
            me.submit_gems(a.pos);
            banking = false;
        } catch (const std::exception&) {}
        break;
    }
    return true;
}

// ---------------------------------------------------------------- farming

int Bot::choose_group() {
    const std::uint64_t f0 = cambc::fuel_remaining();
    struct Done { std::uint64_t f0; ~Done() { g_choose_fuel += f0 - cambc::fuel_remaining(); } } done{f0};
    auto& me = cambc::self();
    const vec2 pos = me.position();
    const float pack_dps = std::max(10.0f, my_dps() * static_cast<float>(pack_n) * 0.85f);
    const float pack_hp = me.max_health() * static_cast<float>(pack_n);
    const float gold_w = W.round < 600 ? 0.15f : (W.round < 1200 ? 0.08f : 0.03f);
    int taken[3] = {-1, -1, -1};
    for (int k = 0; k < 3; ++k) if (k != pack) taken[k] = sm_get(k);
    int best = -1;
    float best_s = -1;
    for (const auto& G : W.groups) {
        if (G.m.empty() || g_bad_until[G.id] > W.round) continue;
        if (G.id == taken[0] || G.id == taken[1] || G.id == taken[2]) continue;
        if (G.island) continue;
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
        const float T = hpv / pack_dps;
        // Camps do not heal: a group too strong for one trip takes several, each a recall and a walk back.
        const float trips = std::max(0.0f, std::ceil(str * T * kCampDamageTaken / (0.7f * pack_hp)) - 1.0f);
        float value = static_cast<float>(G.gems) + gold_w * static_cast<float>(G.gold);
        if (G.theirs + 3 < G.ours) value *= 0.75f;
        const float time = static_cast<float>(std::max(d, wait)) + T + trips * (static_cast<float>(std::min(G.ours, 150)) + 8.0f) + 6.0f;
        const float s = value / time;
        if (s > best_s) { best_s = s; best = G.id; }
    }
    return best;
}

bool Bot::work_group(int gi) {
    auto& me = cambc::self();
    auto& G = W.groups[static_cast<std::size_t>(gi)];
    const vec2 pos = me.position();
    if (G.state == GroupState::respawning) {
        if (G.respawn_at - W.round > 12) return false;
        // Wait next to it.
        cambc::bitboard goal;
        for (auto t : G.tiles) add_square(goal, t, ranged ? 3 : 1);
        goal &= ~G.spawn;
        if (N.step(goal, W.group_field(gi))) walked = true;
        return true;
    }
    if (G.state == GroupState::reward || G.alive_count() == 0) {
        if (G.state != GroupState::reward && G.seen == W.round) return false;
        if (bit(G.spawn, pos)) return true;  // capturing
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
    if (tgt) {
        const CampSeen c = *tgt;
        if (approach_target != gi) { approach_target = gi; approach_r = me.attack_range(); }
        cast_on_camp(c);
        if (cheb(c.pos, pos) <= me.attack_range()) {
            if (attack_unit_at(c.pos)) { reset_and_hit(c.pos); return true; }
            // In range but unseen: come closer.
            if (approach_r > 1) --approach_r;
        }
        if (casted && (kit[0].excl || kit[1].excl)) return true;
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
        if (cheb(c.pos, me.position()) <= me.attack_range() && attack_unit_at(c.pos)) reset_and_hit(c.pos);
        return true;
    }

    // Walk to the group.
    cambc::bitboard goal;
    for (auto t : G.tiles) add_square(goal, t, 2);
    if (bit(goal, pos)) {
        // Close but nothing in sight: the camps are gone or hidden.
        add_square(goal, pos, 0);
        if (N.step(G.spawn)) walked = true;
        if (++stuck_turns > 6) { stuck_turns = 0; for (auto& m : G.m) if (bit(W.visible, m.pos)) m.hp = 0; }
        return true;
    }
    if (N.step(goal, W.group_field(gi))) { walked = true; stuck_turns = 0; }
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
    if (!valid) {
        gi = choose_group();
        if (gi < 0) return;
        sm_set(pack, static_cast<std::uint8_t>(gi));
    }
    target = gi;
    if (!work_group(gi)) {
        g_bad_until[static_cast<std::size_t>(gi)] = std::max(g_bad_until[static_cast<std::size_t>(gi)], W.round + 1);
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
        if (W.round <= 2) for (int k = 0; k < 3; ++k) sm_set(k, 0xFF);
    }
    casted = walked = attacked = false;
    g_prof_n = 0;
    prof_mark();
    if (!me.is_alive()) {
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
    }
    if (me.is_recalling() || me.is_submitting_gems() || me.is_teleporting()) {
        if (fuel_left() > 80000) W.precompute_some();
        return;
    }
    if (bit(W.our_spawn, me.position())) shop();
    cast_support();

    const char* mode = "farm";
    if (handle_fight()) mode = "fight";
    else if (handle_health()) mode = "health";
    else if (handle_bank()) mode = "bank";
    else handle_farm();
    prof_mark();
    debug_block({
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
