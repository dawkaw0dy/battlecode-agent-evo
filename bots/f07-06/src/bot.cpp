// Camp sweep: every champion runs its own farming loop over the camp groups, ordered by reward per turn
// of travel, respawn wait and clear time; gems are banked in small batches whenever an altar is close.
#include <cambc.h>
#include "bot.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <variant>
#include <vector>
#include <cstdio>
#include <string>
#define FP(tag) do { } while (0)

namespace {

struct CampSpec { float hp, str; int range; };
CampSpec camp_spec(cambc::camp_type t) {
    switch (t) {
    case cambc::camp_type::wolf: return {1350, 20, 3};
    case cambc::camp_type::elk: return {2400, 60, 4};
    case cambc::camp_type::gem_cluster: return {120, 0, 0};
    case cambc::camp_type::gold_cluster: return {120, 0, 0};
    case cambc::camp_type::monolith: return {20000, 200, 5};
    case cambc::camp_type::golem: return {5000, 120, 4};
    }
    return {1500, 50, 3};
}

bool is_monster(cambc::camp_type t) {
    return t != cambc::camp_type::gem_cluster && t != cambc::camp_type::gold_cluster;
}

float power(float hp, float str, float focus) { return hp * (str + 0.25f * focus); }

// Shared memory: bytes 0-4 claimed group + 1 per slot, 5-6 last enemy sighting, 7 its round (low byte).
std::array<std::uint8_t, 8> g_mem{};

}  // namespace

void Bot::init() {
    inited = true;
    init_layout();
    auto& me = cambc::self();
    const int W = lay::W, H = lay::H;

    swalls = std::make_unique<cambc::bitboard>();
    *swalls |= cambc::map::walls();
    *swalls |= cambc::map::holes();
    *swalls |= cambc::map::altars();
    *swalls |= cambc::map::spawn_region(team::enemy);
    myspawn = std::make_unique<cambc::bitboard>();
    *myspawn |= cambc::map::spawn_region(team::ally);
    blocked = std::make_unique<cambc::bitboard>();
    campwalls = std::make_unique<cambc::bitboard>();

    home = std::make_unique<cambc::distance_field>();
    home_v = std::make_unique<cambc::bitboard>();
    home->compute(*swalls, *myspawn, *home_v);
    FP("i-home");
    dme = std::make_unique<cambc::distance_field>();
    dme_v = std::make_unique<cambc::bitboard>();
    work = std::make_unique<cambc::distance_field>();
    work_v = std::make_unique<cambc::bitboard>();

    // Distances from the enemy spawn, through terrain only.
    cambc::bitboard terrain;
    terrain |= cambc::map::walls();
    terrain |= cambc::map::holes();
    terrain |= cambc::map::altars();
    work->compute(terrain, cambc::map::spawn_region(team::enemy), *work_v);

    FP("i-enemy");
    tile_group.assign(static_cast<size_t>(W) * H, -1);
    FP("i-assign");
    gobj = cambc::map::camp_groups();
    for (const auto& g : gobj) {
        Group G;
        G.handle = static_cast<std::uint32_t>(static_cast<cambc::handle_camp_group_t>(g));
        G.gems = static_cast<int>(g.gem_value());
        G.gold = static_cast<int>(g.gold_value());
        G.respawn = g.max_respawn_timer();
        for (const auto& c : g.camps()) {
            const auto t = c.kind();
            const auto s = camp_spec(t);
            G.tiles.push_back(c.spawn_position());
            if (is_monster(t)) {
                G.hp += s.hp;
                G.str += s.str;
                ++G.monsters;
                G.range = std::max(G.range, s.range);
            } else {
                G.hp += s.hp;
            }
            if (t == cambc::camp_type::monolith || t == cambc::camp_type::golem) G.boss = true;
        }
        for (auto t : G.tiles) {
            G.dspawn = std::min(G.dspawn, rdist(*home, *home_v, t));
            G.denemy = std::min(G.denemy, rdist(*work, *work_v, t));
        }
        G.hp_left = G.hp;
        G.str_left = G.str;
        const int idx = static_cast<int>(groups.size());
        for (auto t : G.tiles)
            for (auto d : kDirs) {
                const vec2 n{t.x + d.x, t.y + d.y};
                if (!inside(n) || rbit(*swalls, n)) continue;
                if (std::find(G.tiles.begin(), G.tiles.end(), n) != G.tiles.end()) continue;
                if (std::find(G.ring.begin(), G.ring.end(), n) == G.ring.end()) G.ring.push_back(n);
            }
        for (auto t : G.tiles)
            if (inside(t)) tile_group[static_cast<size_t>(t.y) * W + t.x] = static_cast<short>(idx);
        groups.push_back(std::move(G));
    }
    FP("i-groups");
    altars = cambc::map::altars().positions();

    const auto allies = cambc::champions(team::ally);
    nallies = static_cast<int>(allies.size());
    for (int i = 0; i < nallies; ++i)
        if (allies[i] == me) slot = i;
    if (slot > 4) slot = 4;
}

void Bot::snapshot() {
    auto& me = cambc::self();
    pos = me.position();
    hp = me.health();
    maxhp = std::max(1.0f, me.max_health());
    str = me.strength();
    focus = me.focus();
    gems = static_cast<int>(me.gem_count());
    gold = static_cast<int>(me.gold_count());
    range = me.attack_range();
    ranged = range > 1;

    const auto champs = cambc::vision::champion_tiles();
    const auto camp_tiles = cambc::vision::camp_tiles();
    campwalls->clear();
    for (const auto& G : groups)
        if (G.st == 1)
            for (auto t : G.tiles) rset(*campwalls, t);
    rclr(*campwalls, pos);
    blocked->clear();
    *blocked |= *swalls;
    *blocked |= *campwalls;
    *blocked |= champs;
    *blocked |= camp_tiles;
    rclr(*blocked, pos);

    enemies.clear();
    for (auto p : cambc::vision::champion_tiles(team::enemy).positions()) {
        try {
            const auto c = cambc::vision::champion_at(p);
            EnemyView e{p, c.health(), c.max_health(), c.strength(), c.focus(), c.attack_range(),
                c.is_recalling() || c.is_submitting_gems()};
            if (e.hp > 0) enemies.push_back(e);
        } catch (...) {
        }
    }
    camps.clear();
    for (auto p : camp_tiles.positions()) {
        try {
            const auto c = cambc::vision::camp_at(p);
            const auto st = c.state();
            if (!st.is_alive()) continue;
            const auto t = c.kind();
            const auto s = camp_spec(t);
            CampView v{p, c.health(), s.str, s.range,
                tile_group[static_cast<size_t>(p.y) * lay::W + p.x], st.is_awake(), is_monster(t)};
            if (v.hp > 0) camps.push_back(v);
        } catch (...) {
        }
    }

    // Walking distances from here through terrain (units ignored).
    cambc::bitboard root;
    rset(root, pos);
    cambc::bitboard walls;
    walls |= *swalls;
    walls |= *campwalls;
    walls |= camp_tiles;
    rclr(walls, pos);
    dme->compute(walls, root, *dme_v);
}

void Bot::update_groups() {
    for (auto& G : groups)
        if (G.st == 3 && round >= G.ready_at) {
            G.st = 1;
            G.hp_left = G.hp;
            G.str_left = G.str;
        }
    const auto visible = cambc::vision::visible_tiles();
    const auto ally_tiles = cambc::vision::champion_tiles(team::ally);
    for (int idx = 0; idx < static_cast<int>(groups.size()); ++idx) {
        bool seen = false;
        for (auto t : groups[idx].tiles)
            if (rbit(visible, t)) {
                seen = true;
                break;
            }
        if (!seen) continue;
        const auto& g = gobj[idx];
        auto& G = groups[idx];
        try {
            const auto st = g.state();
            if (std::holds_alternative<cambc::map::camp_group_alive>(st)) {
                if (G.st != 1) {
                    G.hp_left = G.hp;
                    G.str_left = G.str;
                }
                G.st = 1;
                // Health left: visible camps exactly, unseen tiles keep their share of the old estimate.
                float seen_hp = 0, seen_str = 0, unseen_full = 0, unseen_str = 0;
                for (auto t : G.tiles) {
                    if (rbit(visible, t)) continue;
                    unseen_full += 1;
                }
                for (const auto& c : camps)
                    if (c.gi == idx) {
                        seen_hp += c.hp;
                        if (c.monster) seen_str += c.str;
                    }
                if (unseen_full > 0) {
                    const float share = unseen_full / static_cast<float>(G.tiles.size());
                    unseen_str = G.str_left * share;
                    G.hp_left = seen_hp + G.hp_left * share;
                    G.str_left = seen_str + unseen_str;
                } else {
                    G.hp_left = seen_hp;
                    G.str_left = seen_str;
                }
            } else if (std::holds_alternative<cambc::map::camp_group_reward_available>(st)) {
                G.st = 2;
                G.hp_left = 0;
                G.str_left = 0;
                for (auto t : G.tiles)
                    if (t != pos && rbit(ally_tiles, t)) G.ally_cap = round;
            } else {
                const auto& r = std::get<cambc::map::camp_group_respawning>(st);
                G.st = 3;
                G.ready_at = round + std::max(1, r.timer);
            }
        } catch (...) {
        }
    }
}

void Bot::shop() {
    auto& me = cambc::self();
    using it = cambc::item_type;
    const auto t = me.champion_name();
    const bool tanky = t == cambc::champion_type::marcella || t == cambc::champion_type::ryth;
    for (int guard = 0; guard < 12 && bought < 80; ++guard) {
        it item = it::power_shard_of_strength;
        if (bought < 5) item = tanky ? it::jungle_shard_of_health : it::jungle_shard_of_strength;
        else if (bought < 10) item = it::monster_shard_of_strength;
        else if (bought < 15) item = it::durability_shard_of_health;
        else item = (bought % 2) ? it::power_shard_of_health : it::power_shard_of_strength;
        if (gold < 30) break;
        bool ok = false;
        try {
            ok = me.can_buy(item, 1);
        } catch (...) {
            ok = false;
        }
        if (!ok) break;
        me.buy(item, 1);
        gold -= 30;
        ++bought;
    }
}

float Bot::threat_at(vec2 p) const {
    float t = 0;
    for (const auto& e : enemies)
        if (!e.channel && cheb(p, e.pos) <= e.range + 1) t += e.str;
    for (const auto& c : camps)
        if (c.awake && c.monster && cheb(p, c.pos) <= c.range) t += c.str;
    return t;
}

// One step towards the nearest goal tile, routing around visible units.
bool Bot::step_to(cambc::bitboard& goals, bool avoid) {
    auto& me = cambc::self();
    if (moved || move_failed || !me.can_move()) return false;
    if (rbit(goals, pos)) return false;
    goals &= ~*blocked;
    if (goals.is_empty()) return false;
    cambc::bitboard dest;
    rset(dest, pos);
    work->compute_to(*blocked, goals, dest, *work_v);
    int here = rdist(*work, *work_v, pos);
    if (here >= kFar) {
        // Units block every route: plan through champions, step only onto free tiles.
        cambc::bitboard walls2;
        walls2 |= *swalls;
        walls2 |= *campwalls;
        walls2 |= cambc::vision::camp_tiles();
        rclr(walls2, pos);
        work->compute_to(walls2, goals, dest, *work_v);
        here = rdist(*work, *work_v, pos);
        if (here >= kFar) return false;
    }
    vec2 best = pos;
    float best_score = 1e9f;
    for (auto d : kDirs) {
        const vec2 n{pos.x + d.x, pos.y + d.y};
        if (!inside(n) || rbit(*blocked, n)) continue;
        const int dn = rdist(*work, *work_v, n);
        if (dn >= here) continue;
        float s = static_cast<float>(dn);
        if (avoid) s += threat_at(n) * 0.02f;
        if (s < best_score) {
            best_score = s;
            best = n;
        }
    }
    if (best == pos) return false;
    if (me.move_absolute(best)) {
        moved = true;
        pos = best;
        return true;
    }
    move_failed = true;
    return false;
}

int Bot::altar_distance(vec2& best_altar) {
    int best = kFar;
    for (auto a : altars) {
        try {
            if (cambc::vision::has_altar_lock(a) && cambc::vision::altar_lock_at(a).is_enemy()) continue;
        } catch (...) {
        }
        for (auto d : kDirs) {
            const vec2 n{a.x + d.x, a.y + d.y};
            if (!inside(n) || rbit(*swalls, n)) continue;
            const int dn = rdist(*dme, *dme_v, n);
            if (dn < best) {
                best = dn;
                best_altar = a;
            }
        }
    }
    return best;
}

bool Bot::bank() {
    auto& me = cambc::self();
    vec2 altar{-1, -1};
    if (altar_distance(altar) >= kFar) return false;
    if (cheb(pos, altar) <= cambc::constants::submit_gems_range) {
        me.submit_gems(altar);
        return true;
    }
    cambc::bitboard goals;
    rbox(goals, altar, cambc::constants::submit_gems_range);
    if (!step_to(goals, true)) {
        // Every side of the altar is occupied: wait beside it.
        return cheb(pos, altar) <= 3;
    }
    if (cheb(pos, altar) <= cambc::constants::submit_gems_range && !moved) {
        me.submit_gems(altar);
    }
    return true;
}

bool Bot::try_recall() {
    auto& me = cambc::self();
    if (rbit(*myspawn, pos)) return false;
    if (rdist(*home, *home_v, pos) <= 5) {
        cambc::bitboard goals;
        goals |= *myspawn;
        return step_to(goals, true);
    }
    if (moved) return false;
    me.recall();
    return true;
}

bool Bot::flee() {
    auto& me = cambc::self();
    if (rbit(*myspawn, pos)) return true;
    // Bank first when an altar is beside us and the channel is short.
    if (gems > 0 && gems <= 20) {
        for (auto a : altars)
            if (cheb(pos, a) <= cambc::constants::submit_gems_range) {
                try {
                    if (!(cambc::vision::has_altar_lock(a) && cambc::vision::altar_lock_at(a).is_enemy())) {
                        me.submit_gems(a);
                        return true;
                    }
                } catch (...) {
                }
            }
    }
    bool near_enemy = false;
    for (const auto& e : enemies)
        if (!e.channel && cheb(pos, e.pos) <= 5) near_enemy = true;
    if (!near_enemy && threat_at(pos) <= 0) return try_recall();
    if (!me.can_move()) return strike_enemy(true) || strike_camps(true);
    vec2 best = pos;
    float best_score = static_cast<float>(rdist(*home, *home_v, pos)) + threat_at(pos) * 0.05f;
    for (auto d : kDirs) {
        const vec2 n{pos.x + d.x, pos.y + d.y};
        if (!inside(n) || rbit(*blocked, n)) continue;
        const int hd = rdist(*home, *home_v, n);
        if (hd >= kFar) continue;
        float s = static_cast<float>(hd) + threat_at(n) * 0.05f;
        for (const auto& e : enemies)
            if (!e.channel) s -= 0.3f * static_cast<float>(std::min(cheb(n, e.pos), 6));
        if (s < best_score) {
            best_score = s;
            best = n;
        }
    }
    if (best != pos) {
        if (me.move_absolute(best)) {
            moved = true;
            pos = best;
        } else {
            move_failed = true;
        }
    }
    // Melee can still hit back; ranged only if it did not move.
    strike_enemy(false);
    return true;
}

bool Bot::use_spells(bool can_excl, bool vs_enemy) {
    auto& me = cambc::self();
    if (cast || !me.can_cast()) return false;
    const auto abs = me.abilities();
    bool has_reset = false;
    for (const auto& ab : abs)
        if (ab.is_attack_reset() && ab.is_ready()) has_reset = true;
    for (const auto& ab : abs) {
        try {
            if (!ab.is_ready()) continue;
            if (ab.is_damage() || ab.is_cc()) {
                float rng;
                bool excl;
                if (ab.is_damage()) {
                    const auto p = ab.get<cambc::ability_damage>();
                    rng = p.range;
                    excl = p.exclusive_with_movement;
                } else {
                    const auto p = ab.get<cambc::ability_cc>();
                    rng = p.range;
                    excl = p.exclusive_with_movement;
                }
                if (excl && (!can_excl || moved)) continue;
                const float dmg = ab.damage_amount();
                if (has_reset && me.can_attack() && dmg < str * 0.9f && !vs_enemy) continue;
                if (vs_enemy) {
                    int bi = -1;
                    for (int i = 0; i < static_cast<int>(enemies.size()); ++i)
                        if (cheb(pos, enemies[i].pos) <= static_cast<int>(rng)
                            && (bi < 0 || enemies[i].hp < enemies[bi].hp))
                            bi = i;
                    if (bi < 0) continue;
                    const auto c = cambc::vision::champion_at(enemies[bi].pos);
                    if (ab.use(c)) {
                        cast = true;
                        enemies[bi].hp -= dmg;
                        return true;
                    }
                } else {
                    if (ab.is_cc() && dmg < str) continue;  // stuns on camps only pay through damage
                    int bi = -1;
                    for (int i = 0; i < static_cast<int>(camps.size()); ++i) {
                        const auto& c = camps[i];
                        if (c.hp <= 0 || (c.gi != target && !c.awake)) continue;
                        if (cheb(pos, c.pos) > static_cast<int>(rng)) continue;
                        if (bi < 0 || (c.monster && !camps[bi].monster)
                            || (c.monster == camps[bi].monster && c.hp < camps[bi].hp))
                            bi = i;
                    }
                    if (bi < 0) continue;
                    const auto c = cambc::vision::camp_at(camps[bi].pos);
                    if (ab.use(c)) {
                        cast = true;
                        camps[bi].hp -= dmg;
                        return true;
                    }
                }
            } else if (ab.is_aoe_damage()) {
                const auto p = ab.get<cambc::ability_aoe_damage>();
                if (p.exclusive_with_movement && (!can_excl || moved)) continue;
                const int rng = static_cast<int>(p.range);
                const int rad = std::max(1, static_cast<int>(p.radius));
                vec2 best{-1, -1};
                int best_n = 0;
                auto consider = [&](vec2 t, int n) {
                    if (n > best_n) {
                        best_n = n;
                        best = t;
                    }
                };
                if (vs_enemy) {
                    for (const auto& e : enemies) {
                        if (cheb(pos, e.pos) > rng) continue;
                        int n = 0;
                        for (const auto& o : enemies)
                            if (cheb(e.pos, o.pos) <= rad) ++n;
                        consider(e.pos, n);
                    }
                } else {
                    for (const auto& c : camps) {
                        if (c.gi != target && !c.awake) continue;
                        if (cheb(pos, c.pos) > rng) continue;
                        int n = 0;
                        for (const auto& o : camps)
                            if ((o.gi == target || o.awake) && cheb(c.pos, o.pos) <= rad) ++n;
                        consider(c.pos, n);
                    }
                }
                if (best_n == 0) continue;
                if (ab.use(best)) {
                    cast = true;
                    return true;
                }
            } else if (ab.is_shield()) {
                if (threat_at(pos) > 0 && hp < maxhp * 0.9f) {
                    if (ab.use()) {
                        cast = true;
                        return true;
                    }
                }
            }
        } catch (...) {
        }
    }
    return false;
}

void Bot::basic_and_reset(bool vs_enemy) {
    auto& me = cambc::self();
    auto pick = [&]() -> int {
        int bi = -1;
        float bs = 1e9f;
        if (vs_enemy) {
            for (int i = 0; i < static_cast<int>(enemies.size()); ++i) {
                const auto& e = enemies[i];
                if (e.hp <= 0 || cheb(pos, e.pos) > range) continue;
                const float s = e.hp - (e.channel ? 300.0f : 0.0f);
                if (s < bs) {
                    bs = s;
                    bi = i;
                }
            }
        } else {
            for (int i = 0; i < static_cast<int>(camps.size()); ++i) {
                const auto& c = camps[i];
                if (c.hp <= 0 || (c.gi != target && !c.awake) || cheb(pos, c.pos) > range) continue;
                int nb = 0;
                for (const auto& o : camps)
                    if (o.hp > 0 && &o != &c && cheb(o.pos, c.pos) <= 1) ++nb;
                float s = c.hp - 300.0f * nb + (c.monster ? 0.0f : 5000.0f);
                if (s < bs) {
                    bs = s;
                    bi = i;
                }
            }
        }
        return bi;
    };
    auto hit = [&]() -> bool {
        if (!me.can_attack()) return false;
        const int i = pick();
        if (i < 0) return false;
        if (vs_enemy) {
            if (!cambc::vision::has_champion(enemies[i].pos)) {
                enemies[i].hp = 0;
                return false;
            }
            me.attack(cambc::vision::champion_at(enemies[i].pos));
            enemies[i].hp -= str;
        } else {
            if (!cambc::vision::has_camp(camps[i].pos)) {
                camps[i].hp = 0;
                return false;
            }
            me.attack(cambc::vision::camp_at(camps[i].pos));
            camps[i].hp -= str;
        }
        attacked = true;
        return true;
    };
    try {
        hit();
        // A reset is legal once our own attack is spent (after a hit, or after a ranged walk).
        if (!cast && me.can_cast() && !me.can_attack() && (attacked || (ranged && moved)) && pick() >= 0) {
            for (const auto& ab : me.abilities()) {
                if (!ab.is_attack_reset() || !ab.is_ready()) continue;
                if (ab.use()) {
                    cast = true;
                    hit();
                }
                break;
            }
        }
    } catch (...) {
    }
}

bool Bot::strike_camps(bool can_excl) {
    bool any = false;
    for (const auto& c : camps)
        if (c.hp > 0 && (c.gi == target || c.awake) && cheb(pos, c.pos) <= std::max(range, 4)) any = true;
    if (!any) return false;
    use_spells(can_excl, false);
    basic_and_reset(false);
    return attacked || cast;
}

bool Bot::strike_enemy(bool can_excl) {
    bool any = false;
    for (const auto& e : enemies)
        if (e.hp > 0 && cheb(pos, e.pos) <= std::max(range, 4)) any = true;
    if (!any) return false;
    use_spells(can_excl, true);
    basic_and_reset(true);
    return attacked || cast;
}

bool Bot::fight_enemies() {
    auto& me = cambc::self();
    const auto& espawn = cambc::map::spawn_region(team::enemy);
    std::vector<int> near;
    for (int i = 0; i < static_cast<int>(enemies.size()); ++i) {
        const auto& e = enemies[i];
        const int d = cheb(pos, e.pos);
        if (d > 8) continue;
        if (rbit(espawn, e.pos) && d > 3) continue;
        near.push_back(i);
    }
    if (near.empty()) return false;
    enemy_round = round;
    enemy_pos = enemies[near[0]].pos;
    try {
        g_mem[5] = static_cast<std::uint8_t>(enemy_pos.x);
        g_mem[6] = static_cast<std::uint8_t>(enemy_pos.y);
        g_mem[7] = static_cast<std::uint8_t>(round & 255);
        cambc::shared_memory.write(5, std::span<std::uint8_t>{g_mem.data() + 5, 3});
    } catch (...) {
    }

    float ours = power(hp, str, focus), theirs = 0;
    try {
        for (auto p : cambc::vision::champion_tiles(team::ally).positions()) {
            if (p == pos || cheb(p, pos) > 6) continue;
            const auto c = cambc::vision::champion_at(p);
            if (c.is_recalling()) continue;
            ours += power(c.health(), c.strength(), c.focus());
        }
    } catch (...) {
    }
    float weakest = 1e9f;
    for (int i : near) {
        const auto& e = enemies[i];
        theirs += power(e.hp, e.str, e.focus) * (e.channel ? 0.3f : 1.0f);
        if (cheb(pos, e.pos) <= range) weakest = std::min(weakest, e.hp);
    }
    const bool low = hp < maxhp * 0.3f;
    const bool kill = weakest <= str * 1.5f;
    const bool fight = !low && (ours >= theirs * 1.15f || kill);
    if (!fight) {
        retreat = retreat || hp < maxhp * 0.6f;
        return flee();
    }
    if (strike_enemy(true)) return true;
    // Chase the nearest enemy, a limited number of turns without a hit.
    if (round < no_chase_until) return false;
    int ni = near[0];
    for (int i : near)
        if (cheb(pos, enemies[i].pos) < cheb(pos, enemies[ni].pos)) ni = i;
    if (++chase > 8) {
        chase = 0;
        no_chase_until = round + 15;
        return false;
    }
    cambc::bitboard goals;
    rbox(goals, enemies[ni].pos, range);
    if (step_to(goals, false)) {
        if (strike_enemy(false)) chase = 0;
        return true;
    }
    (void)me;
    return false;
}

void Bot::choose_target(bool, int&) {
    const float dps = str * 1.12f + 0.15f * focus;
    const float gw = round < 1200 ? 0.35f : 0.12f;
    const float reduce = bought >= 5 ? 0.7f : 1.0f;
    int best = -1;
    float best_score = 0;
    for (int i = 0; i < static_cast<int>(groups.size()); ++i) {
        auto& G = groups[i];
        if (G.gems + G.gold <= 0) continue;
        int d = kFar;
        for (auto t : G.ring) d = std::min(d, rdist(*dme, *dme_v, t));
        if (G.st != 1)
            for (auto t : G.tiles) d = std::min(d, rdist(*dme, *dme_v, t));
        if (d >= kFar) continue;
        d = std::max(0, d + 1 - range);
        int helpers = 0;
        for (int s = 0; s < 5 && s < nallies; ++s)
            if (s != slot && g_mem[s] == i + 1) ++helpers;
        float wait = 0, clear = 0, extra = 0;
        if (G.st == 3) wait = static_cast<float>(std::max(0, G.ready_at - round - d));
        if (G.st == 2 && round - G.ally_cap < 6) continue;
        float hpL = G.st == 2 ? 0 : G.hp_left, strL = G.st == 2 ? 0 : G.str_left;
        if (G.st == 3) {
            hpL = G.hp;
            strL = G.str;
        }
        clear = hpL / (dps * (1.0f + 0.8f * helpers));
        const float dmg = strL * reduce * clear * 0.75f / (1.0f + helpers);
        const float budget = std::max(hp - 0.3f * maxhp, 1.0f);
        if (dmg > budget) {
            const float trips = std::ceil((dmg - budget) / (maxhp * 0.65f));
            if (trips > 4) continue;
            extra = trips * (static_cast<float>(G.dspawn) + 14.0f);
        }
        float fac = 1.0f;
        if (i == target) fac *= 1.3f;
        if (helpers > 0 && clear < 12) fac *= 0.3f;
        if (G.denemy + 4 < G.dspawn) fac *= 0.65f;
        if (round - enemy_round < 20 && cheb(enemy_pos, G.tiles[0]) < 10) fac *= 0.4f;
        const float value = static_cast<float>(G.gems) + gw * static_cast<float>(G.gold);
        const float score = value * fac / (static_cast<float>(d) + wait + clear + extra + 8.0f);
        if (score > best_score) {
            best_score = score;
            best = i;
        }
    }
    if (best != target) {
        stall = 0;
        shrink = 0;
    }
    target = best;
}

bool Bot::farm() {
    if (target < 0) return false;
    auto& G = groups[target];
    if (G.st == 2) {
        for (auto t : G.tiles)
            if (t == pos) {
                strike_enemy(true) || strike_camps(true);
                return true;  // hold the capture tile
            }
        cambc::bitboard goals;
        for (auto t : G.tiles) rset(goals, t);
        if (step_to(goals, true)) return true;
        return strike_camps(true);
    }
    // Hit what is in range first; otherwise walk into range of the group's living camps.
    if (strike_camps(true)) {
        stall = 0;
        return true;
    }
    const int r = std::max(1, range - shrink);
    cambc::bitboard goals;
    bool any = false;
    for (const auto& c : camps)
        if (c.gi == target && c.hp > 0) {
            rbox(goals, c.pos, r);
            any = true;
        }
    if (!any) {
        const int rr = G.st == 3 ? 2 : r;
        for (auto t : G.tiles) rbox(goals, t, rr);
    }
    if (any && rbit(goals, pos)) {
        // In range but nothing hit (line of sight): come closer.
        if (++stall >= 2 && shrink < range - 1) {
            ++shrink;
            stall = 0;
        }
    }
    if (G.st == 3 && rbit(goals, pos)) return false;
    if (step_to(goals, false)) {
        strike_camps(false);
        return true;
    }
    return false;
}

void Bot::play_turn() {
    auto& me = cambc::self();
    round = static_cast<int>(cambc::round_number());
    FP("start");
    if (!inited) { init(); FP("init");
        return; }
    if (!me.is_alive()) {
        gold = static_cast<int>(me.gold_count());
        shop();
        retreat = false;
        return;
    }
    if (me.is_recalling() || me.is_submitting_gems() || me.is_teleporting()) return;
    moved = attacked = cast = move_failed = false;

    snapshot();
    FP("snap");
    try {
        cambc::shared_memory.read(0, std::span<std::uint8_t>{g_mem.data(), 8});
    } catch (...) {
    }
    {
        const int r = g_mem[7];
        const int age = (round - r) & 255;
        if (age < 30 && round - enemy_round > age) {
            enemy_round = round - age;
            enemy_pos = vec2{g_mem[5], g_mem[6]};
        }
    }
    update_groups();
    FP("groups");
    if (static_cast<int>(cambc::round_number()) != round) return;  // resumed after running out of fuel

    const bool at_spawn = rbit(*myspawn, pos);
    if (at_spawn) {
        gold = static_cast<int>(me.gold_count());
        shop();
        retreat = false;
        if (hp < maxhp * 0.95f && spawn_wait < 2 && enemies.empty()) {
            ++spawn_wait;
            return;
        }
    } else {
        spawn_wait = 0;
    }

    if (hp < maxhp * 0.3f || hp < threat_at(pos) * 2.0f) retreat = true;
    if (fight_enemies()) return;
    if (retreat) {
        if (!at_spawn) {
            if (threat_at(pos) > 0) flee();
            else try_recall();
            return;
        }
        retreat = false;
    }

    // Shopping trip when rich and already worn down.
    if (!at_spawn && gold >= 300 && hp < maxhp * 0.7f && threat_at(pos) <= 0) {
        retreat = true;
        try_recall();
        return;
    }

    vec2 altar{-1, -1};
    const int ad = gems > 0 ? altar_distance(altar) : kFar;
    const int need = static_cast<int>(cambc::constants::winning_gems)
        - static_cast<int>(cambc::global_submitted_gems(team::ally));
    const bool enemy_recent = round - enemy_round < 25;
    bool want_bank = gems > 0 && ad < kFar
        && (gems >= 60 || (gems >= 10 && ad <= 4) || (gems >= 25 && ad <= 12) || gems >= need
            || round + ad + 6 >= 2000 || (enemy_recent && gems >= 15));

    if (round - last_choice >= 3 || target < 0 || groups[target].st == 3
        || (groups[target].st == 2 && round - groups[target].ally_cap < 6)) {
        int bd = 0;
        choose_target(want_bank, bd);
        last_choice = round;
        FP("choose");
        g_mem[slot] = static_cast<std::uint8_t>(target + 1);
        try {
            cambc::shared_memory.write(slot, std::span<std::uint8_t>{g_mem.data() + slot, 1});
        } catch (...) {
        }
    }
    // Nothing worth farming nearby: bank what we carry.
    if (!want_bank && gems > 0 && (target < 0 || groups[target].st == 3)) want_bank = true;
    // Do not leave a capture in progress.
    if (want_bank && target >= 0 && groups[target].st == 2 && gems < need) {
        for (auto t : groups[target].tiles)
            if (cheb(t, pos) <= 4) want_bank = false;
    }
    if (false) std::printf("T r%d s%d pos(%d,%d) hp%.0f/%.0f gems%d tgt%d st%d bank%d ad%d altar(%d,%d) retreat%d\n", round, slot, pos.x, pos.y, hp, maxhp, gems, target, target>=0?groups[target].st:-1, (int)want_bank, ad, altar.x, altar.y, (int)retreat);
    if (want_bank && strike_camps(true)) return;
    if (want_bank && bank()) return;
    if (farm()) return;
    // Idle: walk towards the target group.
    if (target >= 0) {
        cambc::bitboard goals;
        for (auto t : groups[target].tiles) rbox(goals, t, 2);
        step_to(goals, false);
    }
}
