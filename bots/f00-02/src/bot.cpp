// Safe banker: farm camps the team can clear without losing anyone, tribute gems as soon as
// an altar is close, recall early, and step away from fights we do not clearly win.
#include <cambc.h>
#include "bot.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <variant>
#include <vector>

using cambc::bitboard;
using cambc::distance_field;
using cambc::team;
using cambc::vec2;

namespace {

int W = 0, H = 0;
constexpr std::uint16_t UNR = distance_field::unreachable;
constexpr std::array<vec2, 8> DIRS{vec2{0, 1}, vec2{1, 1}, vec2{1, 0}, vec2{1, -1},
                                   vec2{0, -1}, vec2{-1, -1}, vec2{-1, 0}, vec2{-1, 1}};

int cheb(vec2 a, vec2 b) { return static_cast<int>(a.chebyshev_distance(b)); }
bool inb(vec2 p) { return p.x >= 0 && p.y >= 0 && p.x < W && p.y < H; }

// Chebyshev square with manual bounds checks (draw_chebyshev can fail near edges).
bitboard square(vec2 c, int r) {
    bitboard b;
    b.clear();
    for (int dy = -r; dy <= r; ++dy)
        for (int dx = -r; dx <= r; ++dx) {
            const vec2 p{c.x + dx, c.y + dy};
            if (inb(p)) b.set(p);
        }
    return b;
}

struct CampStat { float hp, str; int range; };
CampStat camp_stat(int kind) {
    switch (static_cast<cambc::camp_type>(kind)) {
    case cambc::camp_type::wolf: return {1350, 20, 3};
    case cambc::camp_type::elk: return {2400, 60, 4};
    case cambc::camp_type::gem_cluster: return {120, 0, 0};
    case cambc::camp_type::gold_cluster: return {120, 0, 0};
    case cambc::camp_type::monolith: return {20000, 200, 5};
    case cambc::camp_type::golem: return {5000, 120, 4};
    }
    return {1000, 50, 3};
}

std::vector<cambc::map::camp_group>& group_handles() {
    static std::vector<cambc::map::camp_group> handles = cambc::map::camp_groups();
    return handles;
}

bitboard occupied() {
    return cambc::vision::champion_tiles() | cambc::vision::camp_tiles() | cambc::vision::teleport_tiles();
}

struct Seen { vec2 pos; float hp, str; };

std::vector<Seen> visible_champions(team side) {
    std::vector<Seen> out;
    for (auto p : cambc::vision::champion_tiles(side).positions()) {
        auto c = cambc::vision::champion_at(p);
        if (!c.is_visible()) continue;
        out.push_back({p, c.health(), c.strength()});
    }
    return out;
}

// Awake camps that can currently hit `tile`.
bool camp_threat_at(vec2 tile) {
    for (auto p : cambc::vision::camp_tiles().positions()) {
        if (cheb(p, tile) > 5) continue;
        auto c = cambc::vision::camp_at(p);
        if (!c.is_visible() || !c.is_awake()) continue;
        if (c.strength() > 0 && cheb(p, tile) <= c.attack_range()) return true;
    }
    return false;
}

} // namespace

void Bot::init() {
    initialised = true;
    W = cambc::map::width();
    H = cambc::map::height();
    static_walls = cambc::map::walls() | cambc::map::holes() | cambc::map::altars()
        | cambc::map::spawn_region(team::enemy);
    own_spawn = cambc::map::spawn_region(team::ally);
    approach.clear();
    for (auto a : cambc::map::altars().positions()) approach |= square(a, cambc::constants::submit_gems_range);
    approach &= ~static_walls;
    bitboard visited;
    spawn_field.compute(static_walls, own_spawn, visited);
    altar_field.compute(static_walls, approach, visited);

    for (const auto& g : group_handles()) {
        GroupInfo info;
        info.spawns = g.spawn_tiles();
        info.gems = static_cast<int>(g.gem_value());
        info.gold = static_cast<int>(g.gold_value());
        info.respawn = g.max_respawn_timer();
        int adist = UNR;
        for (const auto& c : g.camps()) {
            const vec2 t = c.spawn_position();
            const int kind = static_cast<int>(c.kind());
            info.tiles.push_back(t);
            info.kinds.push_back(kind);
            const auto st = camp_stat(kind);
            if (st.str > 0) {
                info.monster_hp += st.hp;
                info.strength += st.str;
                info.monsters++;
                info.max_range = std::max(info.max_range, st.range);
            } else {
                info.cluster_hp += st.hp;
            }
            adist = std::min<int>(adist, altar_field.get(t));
        }
        info.altar_dist = adist;
        groups.push_back(std::move(info));
    }
    ready_round.assign(groups.size(), 0);
    banned_until.assign(groups.size(), 0);

    const auto allies = cambc::champions(team::ally);
    const auto& me = cambc::self();
    for (std::size_t i = 0; i < allies.size(); ++i)
        if (allies[i] == me) my_index = static_cast<int>(i);
    if (my_index > 7) my_index = 7;
}

int Bot::read_claim(int index) {
    std::uint8_t b = 0;
    cambc::shared_memory.read(static_cast<std::size_t>(index), std::span{&b, 1});
    return static_cast<int>(b) - 1;
}

void Bot::write_claim(int group) {
    std::uint8_t b = static_cast<std::uint8_t>(group + 1);
    cambc::shared_memory.write(static_cast<std::size_t>(my_index), std::span{&b, 1});
}

void Bot::buy_items() {
    auto& me = cambc::self();
    const auto counts = me.item_count();
    using cambc::item_type;
    int dur = static_cast<int>(counts[item_type::durability_shard_of_health])
        + 5 * static_cast<int>(counts[item_type::durability_emblem_of_health]);
    const bool healer = me.champion_name() == cambc::champion_type::willow;
    for (int guard = 0; guard < 20; ++guard) {
        const item_type item = dur < 5 ? item_type::durability_shard_of_health
            : healer ? item_type::power_shard_of_health : item_type::power_shard_of_strength;
        if (me.gold_count() < item.cost() || !me.can_buy(item, 1)) break;
        me.buy(item, 1);
        if (dur < 5) ++dur;
    }
}

void Bot::update_groups() {
    const auto vis = cambc::vision::visible_tiles();
    const int now = static_cast<int>(cambc::round_number());
    auto& handles = group_handles();
    for (std::size_t i = 0; i < groups.size(); ++i) {
        bool seen = false;
        for (auto t : groups[i].tiles)
            if (vis.is_set(t)) { seen = true; break; }
        if (!seen) continue;
        const auto state = handles[i].state();
        if (const auto* r = std::get_if<cambc::map::camp_group_respawning>(&state))
            ready_round[i] = now + std::max(1, r->timer);
        else
            ready_round[i] = 0;
    }
}

bool Bot::step_to(const bitboard& goals) {
    auto& me = cambc::self();
    if (moved || cast_lock || !me.can_move()) return false;
    const vec2 pos = me.position();
    auto walls = static_walls | occupied();
    walls.set(pos, false);
    auto roots = goals & ~walls;
    if (roots.is_empty() || roots.is_set(pos)) return false;
    distance_field field;
    bitboard visited;
    field.compute_to(walls, roots, square(pos, 0), visited);
    if (!visited.is_set(pos)) {
        // Allies may block a corridor: plan through champions, then take any free improving step.
        walls = static_walls | cambc::vision::camp_tiles();
        walls.set(pos, false);
        roots = goals & ~walls;
        if (roots.is_empty()) return false;
        field.compute_to(walls, roots, square(pos, 0), visited);
        if (!visited.is_set(pos)) return false;
        walls = static_walls | occupied();
    }
    const auto cur = field.get(pos);
    vec2 best = pos;
    std::uint16_t bestd = cur;
    for (auto d : DIRS) {
        const vec2 t{pos.x + d.x, pos.y + d.y};
        if (!inb(t) || walls.is_set(t) || !visited.is_set(t)) continue;
        const auto fd = field.get(t);
        if (fd < bestd) { bestd = fd; best = t; }
    }
    if (best == pos) return false;
    moved = true;
    return me.move_absolute(best);
}

bool Bot::flee(const std::vector<vec2>& threats) {
    auto& me = cambc::self();
    if (moved || cast_lock || !me.can_move() || threats.empty()) return false;
    const vec2 pos = me.position();
    auto walls = static_walls | occupied();
    walls.set(pos, false);
    auto score = [&](vec2 t) {
        int mind = 99;
        for (auto e : threats) mind = std::min(mind, cheb(t, e));
        const int sd = spawn_field.get(t);
        return mind * 10.0f - (sd >= UNR ? 500.0f : sd * 0.6f);
    };
    vec2 best = pos;
    float bests = score(pos);
    for (auto d : DIRS) {
        const vec2 t{pos.x + d.x, pos.y + d.y};
        if (!inb(t) || walls.is_set(t)) continue;
        const float s = score(t);
        if (s > bests) { bests = s; best = t; }
    }
    if (best == pos) return false;
    moved = true;
    return me.move_absolute(best);
}

bool Bot::go_bank() {
    auto& me = cambc::self();
    if (me.gem_count() == 0) return false;
    const vec2 pos = me.position();
    const int range = cambc::constants::submit_gems_range;
    bitboard goals = approach;
    for (auto a : cambc::map::altars().positions()) {
        const bool locked = cambc::vision::has_altar_lock(a);
        const bool enemy_lock = locked && cambc::vision::altar_lock_at(a).is_enemy();
        if (cheb(pos, a) <= range) {
            if (!enemy_lock) {
                me.submit_gems(a);
                return true;
            }
            if (me.can_attack()) {
                me.attack(cambc::vision::altar_lock_at(a));
                return true;
            }
        }
    }
    // Prefer altars without a visible enemy lock.
    bitboard avoid;
    avoid.clear();
    for (auto a : cambc::vision::altar_lock_tiles(team::enemy).positions()) avoid |= square(a, range);
    auto free_goals = goals & ~avoid;
    if (!free_goals.is_empty() && step_to(free_goals)) return true;
    step_to(goals);
    return true;
}

bool Bot::cast_on_camp(const vec2& tile) {
    auto& me = cambc::self();
    if (moved || cast_lock || !me.can_cast()) return false;
    for (const auto& ab : me.abilities()) {
        if (!(ab.is_damage() || ab.is_aoe_damage() || ab.is_cc())) continue;
        if (!ab.is_ready()) continue;
        if (!(ab.can_target_camps() || ab.can_target_ground())) continue;
        if (!ab.valid_tiles().is_set(tile)) continue;
        if (!cambc::vision::has_camp(tile)) return false;
        auto c = cambc::vision::camp_at(tile);
        if (!c.is_visible()) return false;
        cast_lock = true;
        return ab.can_target_camps() ? ab.use(c) : ab.use(tile);
    }
    return false;
}

void Bot::use_support_spells() {
    auto& me = cambc::self();
    if (!me.can_cast()) return;
    for (const auto& ab : me.abilities()) {
        if (!ab.is_ready()) continue;
        if (ab.is_heal() && ab.can_target_allies()) {
            const auto mask = ab.valid_tiles();
            const float amount = ab.heal_amount();
            for (auto p : cambc::vision::champion_tiles(team::ally).positions()) {
                if (p == me.position() || !mask.is_set(p)) continue;
                auto c = cambc::vision::champion_at(p);
                if (!c.is_visible() || c.health() <= 0) continue;
                const float missing = c.max_health() - c.health();
                if (missing >= amount * 0.7f || c.health() < 0.45f * c.max_health()) {
                    ab.use(c);
                    return;
                }
            }
        } else if (ab.is_shield() && ab.can_target_self()) {
            bool danger = camp_threat_at(me.position());
            if (!danger)
                for (auto p : cambc::vision::champion_tiles(team::enemy).positions())
                    if (cheb(p, me.position()) <= 4) danger = true;
            if (danger && me.health() < 0.97f * me.max_health()) {
                ab.use();
                return;
            }
        }
    }
}

void Bot::choose_target(const distance_field& from_me, const bitboard& reach) {
    auto& me = cambc::self();
    const int now = static_cast<int>(cambc::round_number());
    const float my_str = std::max(1.0f, me.strength());
    const float my_hp = me.health();
    const int allies = static_cast<int>(cambc::champions(team::ally).size());
    std::array<int, 8> claims{};
    for (int j = 0; j < 8; ++j) claims[j] = j == my_index ? -1 : read_claim(j);
    const float gold_w = me.gold_count() < 150 ? 0.25f : 0.12f;

    int best = -1;
    float best_score = 0;
    for (std::size_t i = 0; i < groups.size(); ++i) {
        const auto& g = groups[i];
        if (banned_until[i] > now) continue;
        if (g.monster_hp >= 20000) continue;          // monoliths are not worth the risk
        if (g.gems == 0 && g.gold == 0) continue;
        int dist = UNR;
        for (auto t : g.tiles)
            if (reach.is_set(t)) dist = std::min<int>(dist, from_me.get(t));
        if (dist >= UNR) continue;
        const int wait = std::max(0, ready_round[i] - now);
        const int arrival = std::max(dist, wait);
        int claimants = 0;
        for (int j = 0; j < 8; ++j) if (claims[j] == static_cast<int>(i)) ++claimants;
        float factor = 1.0f;
        float dps = my_str + 50.0f * claimants;
        if (g.monsters > 0) {
            const float team_hp = my_hp + 750.0f * claimants;
            const float ttk = g.monster_hp / dps;
            const float dmg = g.strength * ttk * (g.monsters > 1 ? 0.7f : 1.0f);
            if (dmg > 0.5f * team_hp) {
                // Needs a coalition: worth proposing only if the whole team could do it.
                const float all_dps = my_str + 50.0f * (allies - 1);
                const float all_hp = my_hp + 750.0f * (allies - 1);
                const float all_dmg = g.strength * (g.monster_hp / all_dps) * (g.monsters > 1 ? 0.7f : 1.0f);
                if (allies < 2 || all_dmg > 0.5f * all_hp) continue;
                factor = 0.45f;
                dps = all_dps;
            }
        } else if (claimants > 0) {
            factor = 0.1f;                             // someone else is already on this harmless group
        }
        const float clear = (g.monster_hp + g.cluster_hp) / dps;
        const float value = g.gems + gold_w * g.gold;
        const float cost = arrival + clear + 6.0f + 0.4f * std::min(g.altar_dist, 60);
        float score = factor * value / cost;
        if (static_cast<int>(i) == target) score *= 1.25f;
        if (score > best_score) { best_score = score; best = static_cast<int>(i); }
    }
    if (best != target) wait_turns = 0;
    target = best;
}

bool Bot::fight_group() {
    if (target < 0) return false;
    auto& me = cambc::self();
    const int now = static_cast<int>(cambc::round_number());
    const auto& g = groups[target];
    const vec2 pos = me.position();
    const auto vis = cambc::vision::visible_tiles();
    bool seen = false;
    for (auto t : g.tiles)
        if (vis.is_set(t)) { seen = true; break; }
    const int range = me.attack_range();
    if (!seen) {
        bitboard goals;
        goals.clear();
        for (auto t : g.tiles) goals |= square(t, range);
        step_to(goals & ~static_walls);
        return true;
    }
    const auto state = group_handles()[target].state();
    if (const auto* r = std::get_if<cambc::map::camp_group_respawning>(&state)) {
        ready_round[target] = now + std::max(1, r->timer);
        target = -1;
        return false;
    }
    if (std::holds_alternative<cambc::map::camp_group_reward_available>(state)) {
        if (g.spawns.is_set(pos)) return true;          // capturing
        for (auto p : cambc::vision::champion_tiles(team::ally).positions())
            if (p != pos && g.spawns.is_set(p)) {
                target = -1;                            // an ally is capturing it
                return false;
            }
        if (!(cambc::vision::champion_tiles(team::enemy) & square(g.tiles[0], 4)).is_empty()) {
            banned_until[target] = now + 40;
            target = -1;
            return false;
        }
        if (!step_to(g.spawns)) {
            if (moved) return true;
            target = -1;
            return false;
        }
        return true;
    }

    // Alive: list living camps of this group.
    struct Live { vec2 pos; float hp, str; bool awake; };
    std::vector<Live> live;
    float monster_hp = 0, strength = 0;
    bool any_awake = false;
    for (auto t : g.tiles) {
        if (!cambc::vision::has_camp(t)) continue;
        auto c = cambc::vision::camp_at(t);
        if (!c.is_visible() || !c.is_alive()) continue;
        const Live l{t, c.health(), c.strength(), c.is_awake()};
        live.push_back(l);
        if (l.str > 0) { monster_hp += l.hp; strength += l.str; }
        any_awake = any_awake || l.awake;
    }
    if (live.empty()) {
        bitboard goals;
        goals.clear();
        for (auto t : g.tiles) goals |= square(t, 1);
        step_to(goals & ~static_walls);
        return true;
    }

    // Enough allies here to clear it safely?
    if (strength > 0 && !any_awake) {
        float dps = 0, hp = 0;
        int monsters = 0;
        for (const auto& l : live) if (l.str > 0) ++monsters;
        for (auto p : cambc::vision::champion_tiles(team::ally).positions()) {
            if (cheb(p, live[0].pos) > 7) continue;
            auto c = cambc::vision::champion_at(p);
            if (!c.is_visible()) continue;
            dps += c.strength();
            hp += c.health();
        }
        dps = std::max(dps, 1.0f);
        const float dmg = strength * (monster_hp / dps) * (monsters > 1 ? 0.7f : 1.0f);
        if (dmg > 0.5f * hp) {
            if (++wait_turns > 30) {
                banned_until[target] = now + 120;
                target = -1;
                wait_turns = 0;
                return false;
            }
            // Wait next to the group without waking it.
            bitboard goals;
            goals.clear();
            for (const auto& l : live) goals |= square(l.pos, range + 1);
            for (const auto& l : live) goals &= ~square(l.pos, 1);
            goals &= ~static_walls;
            if (!goals.is_set(pos)) step_to(goals);
            return true;
        }
    }
    wait_turns = 0;

    // Choose a camp: killing blows first, then monsters (lowest health), then clusters.
    const auto atk = me.attack_tiles();
    const float my_str = me.strength();
    auto rank = [&](const Live& l) {
        float s = 0;
        if (l.hp <= my_str) s += 100000;
        if (l.str > 0) s += 50000;
        return s - l.hp;
    };
    const Live* in_range = nullptr;
    const Live* primary = nullptr;
    for (const auto& l : live) {
        if (!primary || rank(l) > rank(*primary)) primary = &l;
        if (atk.is_set(l.pos) && (!in_range || rank(l) > rank(*in_range))) in_range = &l;
    }
    // Prefer the in-range camp unless the primary target is a monster and the in-range one is not.
    const Live* tgt = in_range;
    if (in_range && primary->str > 0 && in_range->str <= 0 && in_range->hp > my_str) tgt = nullptr;

    if (tgt) {
        cast_on_camp(tgt->pos);
        if (me.can_attack() && cambc::vision::has_camp(tgt->pos)) {
            auto c = cambc::vision::camp_at(tgt->pos);
            if (c.is_visible() && c.is_alive()) me.attack(c);
        }
        return true;
    }
    cast_on_camp(primary->pos);
    bitboard goals = square(primary->pos, range) & ~static_walls;
    step_to(goals);
    if (me.can_attack()) {
        const auto atk2 = me.attack_tiles();
        for (const auto& l : live) {
            if (!atk2.is_set(l.pos) || !cambc::vision::has_camp(l.pos)) continue;
            auto c = cambc::vision::camp_at(l.pos);
            if (c.is_visible() && c.is_alive()) { me.attack(c); break; }
        }
    }
    return true;
}

void Bot::play_turn() {
    if (!initialised) init();
    moved = false;
    cast_lock = false;
    auto& me = cambc::self();
    if (!me.is_alive()) {
        buy_items();
        return;
    }
    if (me.is_recalling() || me.is_submitting_gems() || me.is_teleporting()) return;
    update_groups();

    const int now = static_cast<int>(cambc::round_number());
    const vec2 pos = me.position();
    const bool at_spawn = own_spawn.is_set(pos);
    if (at_spawn) buy_items();

    const auto enemies = visible_champions(team::enemy);
    const auto allies = visible_champions(team::ally);
    if (!enemies.empty()) last_enemy_round = now;
    use_support_spells();

    const float hp = me.health(), max_hp = me.max_health();
    const float my_str = me.strength();
    const unsigned gems = me.gem_count();

    // Enemy champions: fight only clear wins, otherwise step away.
    if (!enemies.empty()) {
        float ours = 0, theirs = 0;
        for (const auto& a : allies)
            if (cheb(a.pos, pos) <= 6) ours += a.hp * std::max(a.str, 1.0f);
        std::vector<vec2> threats;
        for (const auto& e : enemies)
            if (cheb(e.pos, pos) <= 7) { theirs += e.hp * std::max(e.str, 1.0f); threats.push_back(e.pos); }
        if (!threats.empty()) {
            const auto atk = me.attack_tiles();
            const Seen* weakest = nullptr;
            for (const auto& e : enemies)
                if (atk.is_set(e.pos) && (!weakest || e.hp < weakest->hp)) weakest = &e;
            const bool kill_shot = weakest && weakest->hp <= my_str * 1.05f;
            const bool outmatched = theirs > 0.75f * ours || (gems > 0 && theirs > 0.4f * ours)
                || hp < 0.3f * max_hp;
            if (kill_shot && me.can_attack()) {
                auto c = cambc::vision::champion_at(weakest->pos);
                if (c.is_visible()) { me.attack(c); return; }
            }
            if (outmatched) {
                if (flee(threats)) return;
                if (weakest && me.can_attack()) {
                    auto c = cambc::vision::champion_at(weakest->pos);
                    if (c.is_visible()) me.attack(c);
                }
                return;
            }
            if (weakest && me.can_attack()) {
                auto c = cambc::vision::champion_at(weakest->pos);
                if (c.is_visible()) { me.attack(c); return; }
            }
        }
    }

    // Low health: get out of camp range, then recall.
    if (!at_spawn && hp < 0.35f * max_hp) {
        if (camp_threat_at(pos)) {
            std::vector<vec2> threats;
            for (auto p : cambc::vision::camp_tiles().positions())
                if (cheb(p, pos) <= 5) threats.push_back(p);
            if (flee(threats)) return;
        }
        if (gems > 0 && altar_field.get(pos) <= 1 && go_bank()) return;
        if (!camp_threat_at(pos)) { me.recall(); return; }
    }
    if (at_spawn && hp < 0.9f * max_hp && enemies.empty()) return;

    // Recall early to shop and heal.
    if (!at_spawn && gems == 0 && hp < 0.6f * max_hp && me.gold_count() >= 120
        && spawn_field.get(pos) > 10 && !camp_threat_at(pos) && enemies.empty()) {
        me.recall();
        return;
    }

    // Banking: tribute when an altar is close, the bag is big, or enemies were seen recently.
    bool capturing = false;
    if (target >= 0 && groups[target].spawns.is_set(pos)) capturing = true;
    const bool engaged = camp_threat_at(pos);
    if (gems > 0 && !capturing && !engaged) {
        const int adist = altar_field.get(pos);
        if (adist <= 6 + static_cast<int>(gems) / 2 || gems >= 20 || now - last_enemy_round < 12
            || now > 1900) {
            go_bank();
            return;
        }
    }

    // Farming.
    distance_field from_me;
    bitboard reach;
    from_me.compute(static_walls, square(pos, 0), reach);
    if (target < 0 || (now % 5 == 0 && !engaged && !capturing)) choose_target(from_me, reach);
    if (target >= 0 && !fight_group()) {
        choose_target(from_me, reach);
        if (target >= 0) fight_group();
    }
    write_claim(target);
    if (target < 0 && gems > 0) go_bank();
}
