// Champion decisions for the big-game hunter. main.cpp dispatches here after the draft.
#include <cambc.h>
#include "bot.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <optional>
#include <variant>
#include <vector>

using cambc::vec2;
using cambc::team;
using cambc::bitboard;

namespace {

int W = 0, H = 0;

int dist(vec2 a, vec2 b) { return static_cast<int>(a.chebyshev_distance(b)); }

bool in_bounds(vec2 p) { return p.x >= 0 && p.y >= 0 && p.x < W && p.y < H; }

// Chebyshev square with our own bounds checks (draw_chebyshev can fail near the map edge).
bitboard square(vec2 c, int r) {
    bitboard b;
    b.clear();
    for (int dy = -r; dy <= r; ++dy)
        for (int dx = -r; dx <= r; ++dx) {
            vec2 p{c.x + dx, c.y + dy};
            if (in_bounds(p)) b.set(p);
        }
    return b;
}

bitboard single(vec2 p) {
    bitboard b;
    b.clear();
    b.set(p);
    return b;
}

struct CampInfo { int gems, gold, hp, str, range; };
CampInfo camp_info(cambc::camp_type t) {
    switch (t) {
    case cambc::camp_type::wolf: return {4, 40, 1350, 20, 3};
    case cambc::camp_type::elk: return {15, 90, 2400, 60, 4};
    case cambc::camp_type::gem_cluster: return {7, 0, 120, 0, 0};
    case cambc::camp_type::gold_cluster: return {0, 20, 120, 0, 0};
    case cambc::camp_type::monolith: return {200, 300, 20000, 200, 5};
    case cambc::camp_type::golem: return {36, 160, 5000, 120, 4};
    }
    return {0, 0, 1000, 50, 3};
}

struct Group {
    bitboard spawns;
    std::vector<vec2> tiles;
    int gems = 0, gold = 0, hp = 0, str = 0, range = 0, big = 0;
    int avail = 0;      // first round we expect the group to be alive again
    int bad_until = 0;  // unreachable or stuck: skip until this round
};

std::vector<cambc::map::camp_group> handles;
std::vector<Group> groups;
std::vector<vec2> altars;
bitboard terrain;      // walls, holes, altars and the enemy spawn
bitboard ally_spawn;
int team_n = 5;
cambc::champion_type my_type;
bool is_melee = true;
bool is_tank = false;
int build_step = 0;

// Per-turn state.
bool moved = false;        // walked or dashed this turn
bool move_failed = false;  // a move returned false (hidden unit): no more walking this turn
int now = 0;

// Stuck detection while travelling to the pack target.
vec2 last_pos{-1, -1};
int still_turns = 0;

// Shared memory: [0] target group + 1, [1] spare, [2..7] three (group + 1, avail / 16) notes.
int read_target() {
    std::uint8_t b = cambc::shared_memory[0];
    return static_cast<int>(b) - 1;
}
void write_target(int g) { cambc::shared_memory[0] = static_cast<std::uint8_t>(g + 1); }

void read_notes() {
    for (int i = 0; i < 3; ++i) {
        int g = static_cast<int>(static_cast<std::uint8_t>(cambc::shared_memory[2 + 2 * i])) - 1;
        int a = static_cast<std::uint8_t>(cambc::shared_memory[3 + 2 * i]) * 16;
        if (g >= 0 && g < static_cast<int>(groups.size())) groups[g].avail = std::max(groups[g].avail, a);
    }
}
void write_note(int g, int avail) {
    int slot = -1, oldest = 1 << 30;
    for (int i = 0; i < 3; ++i) {
        int gi = static_cast<int>(static_cast<std::uint8_t>(cambc::shared_memory[2 + 2 * i])) - 1;
        int a = static_cast<std::uint8_t>(cambc::shared_memory[3 + 2 * i]) * 16;
        if (gi == g) { slot = i; break; }
        if (gi < 0) a = -1;
        if (a < oldest) { oldest = a; slot = i; }
    }
    if (slot < 0) return;
    cambc::shared_memory[2 + 2 * slot] = static_cast<std::uint8_t>(g + 1);
    cambc::shared_memory[3 + 2 * slot] = static_cast<std::uint8_t>(std::min(255, (avail + 15) / 16));
}

bitboard blocked_now() {
    auto b = terrain | cambc::vision::champion_tiles() | cambc::vision::camp_tiles()
        | cambc::vision::teleport_tiles();
    b.set(cambc::self().position(), false);
    return b;
}

bool try_move(vec2 to) {
    auto& me = cambc::self();
    if (moved || move_failed || !me.can_move()) return false;
    try {
        if (me.move_absolute(to)) {
            moved = true;
            return true;
        }
    } catch (const std::exception& e) {
        std::cout << "warn: move r" << now << " " << me.position().x << "," << me.position().y << " -> " << to.x
                  << "," << to.y << " ch=" << cambc::vision::has_champion(to) << " camp=" << cambc::vision::has_camp(to)
                  << " ct=" << cambc::vision::champion_tiles().is_set(to) << cambc::vision::champion_tiles(team::ally).is_set(to) << cambc::vision::champion_tiles(team::enemy).is_set(to) << " bush=" << cambc::map::bushes().is_set(to) << " mebush=" << cambc::map::bushes().is_set(me.position()) << " tp=" << cambc::vision::has_teleport(to)
                  << " vis=" << cambc::vision::visible_tiles().is_set(to) << " " << e.what() << '\n';
    }
    move_failed = true;
    return false;
}

// Step towards the nearest goal tile. Goals are filtered to free tiles. Returns true if we moved.
bool step_to(bitboard goals) {
    auto& me = cambc::self();
    if (moved || move_failed || !me.can_move()) return false;
    const vec2 pos = me.position();
    if (goals.is_set(pos)) return false;
    const auto blocked = blocked_now();
    goals &= ~blocked;
    if (goals.is_empty()) return false;

    cambc::distance_field field;
    bitboard visited;
    visited.clear();
    field.compute_to(blocked, goals, single(pos), visited);
    if (!visited.is_set(pos)) {
        // Units block every path: route on terrain alone and take any free improving step.
        field.compute_to(terrain, goals, single(pos), visited);
        if (!visited.is_set(pos)) return false;
    }
    const int here = field.get(pos);
    vec2 best = pos;
    int best_d = here;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            vec2 t{pos.x + dx, pos.y + dy};
            if ((dx == 0 && dy == 0) || !in_bounds(t) || blocked.is_set(t) || !visited.is_set(t)) continue;
            int d = field.get(t);
            if (d < best_d) { best_d = d; best = t; }
        }
    if (best == pos) return false;
    return try_move(best);
}

// Step to the free neighbour that is farthest from all threat positions (ties: closer to spawn).
bool step_away(const std::vector<vec2>& threats) {
    auto& me = cambc::self();
    if (moved || move_failed || !me.can_move() || threats.empty()) return false;
    const vec2 pos = me.position();
    const auto blocked = blocked_now();
    auto score = [&](vec2 t) {
        int m = 1000;
        for (auto th : threats) m = std::min(m, dist(t, th));
        return m;
    };
    vec2 best = pos;
    int best_s = score(pos);
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            vec2 t{pos.x + dx, pos.y + dy};
            if ((dx == 0 && dy == 0) || !in_bounds(t) || blocked.is_set(t)) continue;
            int s = score(t);
            if (s > best_s) { best_s = s; best = t; }
        }
    if (best == pos) return false;
    return try_move(best);
}

// ---------------------------------------------------------------- visible units

struct Foe {
    vec2 pos;
    float hp, max_hp;
    bool champ;
    bool awake;
    int str, range;
    int group;  // camp group index, -1 for champions
};

int group_of_tile(vec2 p) {
    for (int i = 0; i < static_cast<int>(groups.size()); ++i)
        if (groups[i].spawns.is_set(p)) return i;
    return -1;
}

std::vector<Foe> visible_camps() {
    std::vector<Foe> out;
    for (auto p : cambc::vision::camp_tiles().positions()) {
        try {
            auto c = cambc::vision::camp_at(p);
            if (!c.is_visible()) continue;
            auto st = c.state();
            if (!st.is_alive()) continue;
            auto info = camp_info(c.kind());
            out.push_back({p, c.health(), c.max_health(), false, st.is_awake(), info.str, info.range, group_of_tile(p)});
        } catch (const std::exception&) {
        }
    }
    return out;
}

std::vector<Foe> visible_enemies() {
    std::vector<Foe> out;
    for (auto p : cambc::vision::champion_tiles(team::enemy).positions()) {
        try {
            auto c = cambc::vision::champion_at(p);
            if (!c.is_visible() || !c.is_alive()) continue;
            out.push_back({p, c.health(), c.max_health(), true, true, static_cast<int>(c.strength()),
                c.attack_range(), -1});
        } catch (const std::exception&) {
        }
    }
    return out;
}

struct Ally { vec2 pos; float hp, max_hp; };
std::vector<Ally> visible_allies() {
    std::vector<Ally> out;
    const vec2 me = cambc::self().position();
    for (auto p : cambc::vision::champion_tiles(team::ally).positions()) {
        if (p == me) continue;
        try {
            auto c = cambc::vision::champion_at(p);
            if (!c.is_visible() || !c.is_alive()) continue;
            out.push_back({p, c.health(), c.max_health()});
        } catch (const std::exception&) {
        }
    }
    return out;
}

// ---------------------------------------------------------------- abilities

bool exclusive(const cambc::ability& ab) {
    const auto& inf = ab.info();
    if (!inf) return true;
    return std::visit([](const auto& p) { return p.exclusive_with_movement; }, *inf);
}

bool ready(const cambc::ability& ab) {
    auto& me = cambc::self();
    if (!me.can_cast() || !ab.info() || !ab.is_ready()) return false;
    if (moved && exclusive(ab)) return false;
    return true;
}

bool cast_at_foe(const cambc::ability& ab, const Foe& f) {
    try {
        if (f.champ) {
            auto c = cambc::vision::champion_at(f.pos);
            if (!c.is_visible()) return false;
            return ab.use(c);
        }
        auto c = cambc::vision::camp_at(f.pos);
        if (!c.is_visible()) return false;
        return ab.use(c);
    } catch (const std::exception& e) {
        std::cout << "warn: cast " << e.what() << '\n';
        return false;
    }
}

// Damage/stun/heal/shield spells. `foes` are the hostile units we are fighting (camps of the target
// group or enemy champions). Returns true if something was cast.
bool cast_spells(const std::vector<Foe>& foes, bool fighting_champs) {
    auto& me = cambc::self();
    if (!me.can_cast()) return false;
    const vec2 pos = me.position();
    auto abilities = me.abilities();
    const float my_hp = me.health(), my_max = me.max_health();

    for (const auto& ab : abilities) {
        if (!ready(ab)) continue;
        try {
            if (ab.is_heal()) {
                bitboard range = ab.valid_tiles();
                float best = 0;
                std::optional<vec2> target;
                for (auto& a : visible_allies()) {
                    if (!range.is_set(a.pos)) continue;
                    float missing = a.max_hp - a.hp;
                    float need = missing / a.max_hp;
                    if (missing >= 0.6f * ab.heal_amount() || need > 0.5f) {
                        if (missing > best) { best = missing; target = a.pos; }
                    }
                }
                if (target) {
                    auto c = cambc::vision::champion_at(*target);
                    if (c.is_visible() && ab.use(c)) return true;
                }
                continue;
            }
            if (ab.is_shield()) {
                bool threatened = false;
                for (auto& f : foes)
                    if ((f.awake || f.champ) && f.str > 0 && dist(f.pos, pos) <= f.range + 1) threatened = true;
                if (threatened && my_hp < my_max * 0.9f) {
                    if (ab.use()) return true;
                }
                continue;
            }
            if (ab.is_damage() || ab.is_cc() || ab.is_aoe_damage()) {
                if (foes.empty()) continue;
                bitboard range = ab.valid_tiles();
                if (ab.can_target_ground() && !ab.can_target_camps() && !ab.can_target_enemies()) {
                    // Area spell: best centre among foe tiles and their neighbours.
                    float radius = 1.0f;
                    const auto& inf = *ab.info();
                    if (auto p = std::get_if<cambc::ability_aoe_damage>(&inf)) radius = p->radius;
                    int r = static_cast<int>(radius + 0.01f);
                    int best_n = 0;
                    vec2 best_c{-1, -1};
                    const auto allies_b = cambc::vision::champion_tiles(team::ally);
                    for (auto& f : foes) {
                        for (int dy = -r; dy <= r; ++dy)
                            for (int dx = -r; dx <= r; ++dx) {
                                vec2 c{f.pos.x + dx, f.pos.y + dy};
                                if (!in_bounds(c) || !range.is_set(c)) continue;
                                int n = 0;
                                for (auto& g : foes)
                                    if (dist(g.pos, c) <= r) n += g.champ ? 3 : ((g.awake || g.str == 0) ? 2 : 1);
                                if (n > best_n) { best_n = n; best_c = c; }
                            }
                    }
                    if (best_n >= 2 && best_c.x >= 0) {
                        if (ab.use(best_c)) return true;
                    }
                    continue;
                }
                // Unit-targeted: champions first, then the lowest-health foe in range.
                const Foe* best = nullptr;
                float best_s = -1e9f;
                for (auto& f : foes) {
                    if (!range.is_set(f.pos)) continue;
                    if (f.champ && !ab.can_target_enemies()) continue;
                    if (!f.champ && !ab.can_target_camps()) continue;
                    float s = f.champ ? 10000.0f - f.hp : 1000.0f - f.hp / 10.0f;
                    if (ab.is_cc() && !f.champ) s += f.str * 10.0f * (f.awake ? 1.0f : 0.0f);
                    if (ab.is_cc() && !ab.is_damage() && !f.champ && f.str == 0) continue;
                    if (s > best_s) { best_s = s; best = &f; }
                }
                if (best && cast_at_foe(ab, *best)) return true;
                continue;
            }
        } catch (const std::exception& e) {
            std::cout << "warn: spell " << e.what() << '\n';
        }
    }
    (void)fighting_champs;
    return false;
}

bool attack_foe(const Foe& f) {
    auto& me = cambc::self();
    if (!me.can_attack()) return false;
    if (dist(me.position(), f.pos) > me.attack_range()) return false;
    try {
        if (f.champ) {
            auto c = cambc::vision::champion_at(f.pos);
            if (!c.is_visible()) return false;
            me.attack(c);
        } else {
            auto c = cambc::vision::camp_at(f.pos);
            if (!c.is_visible()) return false;
            me.attack(c);
        }
        if (!is_melee) moved = true;  // ranged: attacking uses the move allowance
        return true;
    } catch (const std::exception& e) {
        std::cout << "warn: attack " << e.what() << '\n';
        return false;
    }
}

// After our own basic attack this turn: reset and hit again if the target still stands.
void attack_reset_followup(const Foe& f) {
    auto& me = cambc::self();
    if (!me.can_cast() || me.can_attack()) return;
    for (const auto& ab : me.abilities()) {
        if (!ab.info() || !ab.is_attack_reset() || !ab.is_ready()) continue;
        try {
            bool alive = f.champ ? cambc::vision::has_champion(f.pos) : cambc::vision::has_camp(f.pos);
            if (!alive) return;
            if (ab.use()) attack_foe(f);
        } catch (const std::exception& e) {
            std::cout << "warn: reset " << e.what() << '\n';
        }
        return;
    }
}

// Pick the foe to hit: in range first, prefer champions, then the lowest health.
const Foe* pick_in_range(const std::vector<Foe>& foes) {
    auto& me = cambc::self();
    const Foe* best = nullptr;
    float best_s = -1e9f;
    for (auto& f : foes) {
        if (dist(me.position(), f.pos) > me.attack_range()) continue;
        float s = (f.champ ? 100000.0f : 0.0f) - f.hp;
        if (s > best_s) { best_s = s; best = &f; }
    }
    return best;
}

// Full combat turn against `foes`: spells, attack, reset, then approach `approach` if not in range.
void fight(const std::vector<Foe>& foes, bool champs) {
    auto& me = cambc::self();
    cast_spells(foes, champs);
    if (const Foe* f = pick_in_range(foes)) {
        if (attack_foe(*f)) attack_reset_followup(*f);
        return;
    }
    // Not in range: move toward the closest foe's attack positions.
    const Foe* near = nullptr;
    int nd = 1000;
    for (auto& f : foes) {
        int d = dist(me.position(), f.pos);
        if (d < nd) { nd = d; near = &f; }
    }
    if (!near) return;
    int r = me.attack_range();
    bitboard goals;
    goals.clear();
    for (auto& f : foes)
        if (dist(f.pos, near->pos) <= 3) goals |= square(f.pos, r);
    if (step_to(goals)) {
        if (const Foe* f = pick_in_range(foes)) {
            if (is_melee && attack_foe(*f)) attack_reset_followup(*f);
        }
    }
}

// ---------------------------------------------------------------- shopping

std::vector<cambc::item_type> build_order() {
    using I = cambc::item_type;
    std::vector<cambc::item_type> v;
    auto add = [&](I t, int n) { for (int i = 0; i < n; ++i) v.push_back(t); };
    switch (my_type) {
    case cambc::champion_type::ryth:
    case cambc::champion_type::marcella:
    case cambc::champion_type::varg:
    case cambc::champion_type::heart_of_ruin:
        add(I::jungle_shard_of_health, 5);
        add(I::monster_shard_of_strength, 5);
        add(I::power_shard_of_health, 5);
        add(I::power_shard_of_strength, 30);
        break;
    case cambc::champion_type::willow:
        add(I::jungle_shard_of_health, 5);
        add(I::power_shard_of_focus, 5);
        add(I::power_shard_of_health, 30);
        break;
    case cambc::champion_type::stellar:
    case cambc::champion_type::cassi:
    case cambc::champion_type::prism:
        add(I::monster_shard_of_focus, 5);
        add(I::jungle_shard_of_health, 5);
        add(I::power_shard_of_focus, 30);
        break;
    default:
        add(I::monster_shard_of_strength, 5);
        add(I::jungle_shard_of_health, 5);
        add(I::power_shard_of_strength, 30);
        break;
    }
    return v;
}

void buy_items() {
    auto& me = cambc::self();
    static const auto order = build_order();
    for (int guard = 0; guard < 40 && build_step < static_cast<int>(order.size()); ++guard) {
        auto it = order[build_step];
        if (me.gold_count() < it.cost()) return;
        try {
            if (!me.can_buy(it, 1)) return;
            me.buy(it, 1);
            ++build_step;
        } catch (const std::exception& e) {
            std::cout << "warn: buy " << e.what() << '\n';
            return;
        }
    }
}

// ---------------------------------------------------------------- knowledge and targets

void observe_groups() {
    std::vector<cambc::map::camp_group> vis;
    try {
        vis = cambc::vision::camp_groups();
    } catch (const std::exception&) {
        return;
    }
    const auto camps = cambc::vision::camp_tiles();
    for (auto& vg : vis) {
        int gi = -1;
        for (int i = 0; i < static_cast<int>(handles.size()); ++i)
            if (handles[i] == vg) { gi = i; break; }
        if (gi < 0) continue;
        auto& g = groups[gi];
        if (!(camps & g.spawns).is_empty()) { g.avail = std::min(g.avail, now); continue; }
        try {
            auto st = vg.state();
            if (auto r = std::get_if<cambc::map::camp_group_respawning>(&st)) {
                int a = now + std::max(1, static_cast<int>(r->timer));
                if (a > g.avail + 2) write_note(gi, a);
                g.avail = a;
            } else {
                g.avail = std::min(g.avail, now);
            }
        } catch (const std::exception&) {
        }
    }
}

// Rough team power, from our own stats as a proxy for every champion.
float team_dps() {
    auto& me = cambc::self();
    float s = std::max(40.0f, me.strength());
    return team_n * s * 1.25f;
}
float team_ehp() {
    auto& me = cambc::self();
    return team_n * std::max(800.0f, me.max_health());
}

int choose_target() {
    auto& me = cambc::self();
    const vec2 pos = me.position();
    cambc::distance_field field;
    bitboard visited;
    visited.clear();
    field.compute(terrain, single(pos), visited);
    const float dps = team_dps(), ehp = team_ehp();
    int best = -1;
    float best_s = -1;
    for (int i = 0; i < static_cast<int>(groups.size()); ++i) {
        auto& g = groups[i];
        if (g.bad_until > now || g.gems + g.gold == 0) continue;
        int eta = 10000;
        for (auto t : g.tiles) {
            // Camp tiles are free terrain; the field reads distance to them directly.
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    vec2 q{t.x + dx, t.y + dy};
                    if (in_bounds(q) && visited.is_set(q)) eta = std::min(eta, static_cast<int>(field.get(q)));
                }
        }
        if (eta >= 10000) continue;
        int wait = std::max(0, g.avail - now - eta);
        float kill = g.hp / dps;
        float taken = g.str * kill;
        if (taken > 0.55f * ehp) continue;
        float gem_w = now < 300 ? 1.5f : 3.0f;
        float gold_w = now < 1200 ? 0.5f : 0.2f;
        float value = g.gems * gem_w + g.gold * gold_w;
        float risk = 1.0f + taken / ehp;
        float score = value / ((eta + wait + kill + 6.0f) * risk);
        if (score > best_s) { best_s = score; best = i; }
    }
    return best;
}

bool target_valid(int t) {
    if (t < 0 || t >= static_cast<int>(groups.size())) return false;
    auto& g = groups[t];
    if (g.bad_until > now) return false;
    int d = dist(cambc::self().position(), g.tiles[0]);
    return g.avail <= now + d + 15;
}

// ---------------------------------------------------------------- tribute

int nearest_altar(vec2 pos, bool avoid_enemy_lock) {
    int best = -1, bd = 1 << 20;
    for (int i = 0; i < static_cast<int>(altars.size()); ++i) {
        if (avoid_enemy_lock && cambc::vision::has_altar_lock(altars[i])) {
            try {
                if (cambc::vision::altar_lock_at(altars[i]).is_enemy()) continue;
            } catch (const std::exception&) {
            }
        }
        int d = dist(pos, altars[i]);
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

bool do_tribute() {
    auto& me = cambc::self();
    const vec2 pos = me.position();
    if (me.gem_count() == 0) return false;
    for (auto a : altars) {
        if (dist(pos, a) > cambc::constants::submit_gems_range) continue;
        if (cambc::vision::has_altar_lock(a)) {
            try {
                auto lock = cambc::vision::altar_lock_at(a);
                if (lock.is_enemy()) {
                    if (me.can_attack() && dist(pos, a) <= me.attack_range()) {
                        me.attack(lock);
                        if (!is_melee) moved = true;
                        return true;
                    }
                    continue;
                }
            } catch (const std::exception& e) {
                std::cout << "warn: lock " << e.what() << '\n';
                continue;
            }
        }
        try {
            me.submit_gems(a);
            return true;
        } catch (const std::exception& e) {
            std::cout << "warn: submit " << e.what() << '\n';
        }
    }
    int ai = nearest_altar(pos, true);
    if (ai < 0) ai = nearest_altar(pos, false);
    if (ai < 0) return false;
    step_to(square(altars[ai], cambc::constants::submit_gems_range));
    return true;
}

// ---------------------------------------------------------------- main decisions

std::vector<vec2> threats_near(const std::vector<Foe>& camps, const std::vector<Foe>& enemies, vec2 pos) {
    std::vector<vec2> t;
    for (auto& c : camps)
        if (c.awake && c.str > 0 && dist(c.pos, pos) <= c.range + 1) t.push_back(c.pos);
    for (auto& e : enemies)
        if (dist(e.pos, pos) <= 5) t.push_back(e.pos);
    return t;
}

void hunt(const std::vector<Foe>& camps, const std::vector<Ally>& allies) {
    auto& me = cambc::self();
    const vec2 pos = me.position();

    int t = read_target();
    if (!target_valid(t)) {
        t = choose_target();
        if (t >= 0) write_target(t);
        still_turns = 0;
    }
    if (t < 0) {
        // Nothing worth doing: bank gems or wait at spawn.
        if (me.gem_count() > 0) do_tribute();
        return;
    }
    auto& g = groups[t];

    // Living camps of the target group.
    std::vector<Foe> foes;
    bool any_awake = false;
    for (auto& c : camps)
        if (c.group == t) {
            foes.push_back(c);
            any_awake |= c.awake;
        }
    // Awake camps of other groups that are hitting us are fought too.
    for (auto& c : camps)
        if (c.group != t && c.awake && c.str > 0 && dist(c.pos, pos) <= c.range) foes.push_back(c);

    if (!foes.empty()) {
        // Big camps: gather first, bank gems first (a camp kill destroys them).
        if (g.big && !any_awake) {
            if (me.gem_count() >= 5) { do_tribute(); return; }
            int near = 0;
            for (auto& a : allies)
                if (dist(a.pos, pos) <= 6) ++near;
            int need = std::min(team_n - 1, g.big == 2 ? 4 : 2);
            bool healthy = me.health() >= me.max_health() * 0.8f;
            int nd = 1000;
            for (auto& f : foes) nd = std::min(nd, dist(f.pos, pos));
            if ((near < need || !healthy) && nd <= g.range + 3) {
                if (nd <= g.range) {
                    std::vector<vec2> th;
                    for (auto& f : foes) th.push_back(f.pos);
                    step_away(th);
                }
                if (!healthy && near >= need) me.recall();
                return;
            }
        }
        fight(foes, false);
        still_turns = 0;
        return;
    }

    // No living camp in sight. Is the target in vision?
    bool seen = !(cambc::vision::visible_tiles() & g.spawns).is_empty();
    if (seen) {
        if (g.avail > now + 1) {
            // Respawning: pick another target next turn.
            write_target(-1);
            return;
        }
        // Reward available (or about to be): stand on a free spawn tile of the group.
        if (g.spawns.is_set(pos)) return;
        step_to(g.spawns);
        return;
    }

    // Travel.
    if (pos == last_pos) ++still_turns; else still_turns = 0;
    last_pos = pos;
    if (still_turns > 12) {
        g.bad_until = now + 100;
        write_target(-1);
        still_turns = 0;
        return;
    }
    bitboard goals;
    goals.clear();
    for (auto tile : g.tiles) goals |= square(tile, 1);
    if (!step_to(goals)) step_to(g.spawns);
}

void init() {
    W = cambc::map::width();
    H = cambc::map::height();
    terrain = cambc::map::walls() | cambc::map::holes() | cambc::map::altars()
        | cambc::map::spawn_region(team::enemy);
    ally_spawn = cambc::map::spawn_region(team::ally);
    altars = cambc::map::altars().positions();
    handles = cambc::map::camp_groups();
    for (auto& h : handles) {
        Group g;
        g.spawns = h.spawn_tiles();
        for (const auto& c : h.camps()) {
            auto info = camp_info(c.kind());
            g.tiles.push_back(c.spawn_position());
            g.hp += info.hp;
            g.str += info.str;
            g.range = std::max(g.range, info.range);
            if (c.kind() == cambc::camp_type::golem) g.big = std::max(g.big, 1);
            if (c.kind() == cambc::camp_type::monolith) g.big = 2;
        }
        g.gems = static_cast<int>(h.gem_value());
        g.gold = static_cast<int>(h.gold_value());
        if (g.tiles.empty()) g.tiles = g.spawns.positions();
        groups.push_back(g);
    }
    cambc::draft_memory_buffer dm;
    if (!dm.is_empty() && dm.size() > 0) {
        int n = static_cast<int>(static_cast<std::uint8_t>(dm[0]));
        if (n >= 1 && n <= 6) team_n = n;
    }
    auto& me = cambc::self();
    my_type = me.champion_name();
    is_melee = me.attack_range() <= 1;
    using C = cambc::champion_type;
    is_tank = my_type == C::ryth || my_type == C::marcella || my_type == C::varg || my_type == C::heart_of_ruin;
    me.set_priority(is_tank ? 3 : (is_melee ? 2 : 1));
}

} // namespace

void Bot::play_turn() {
    const auto fuel0 = cambc::fuel_remaining();
    struct FuelLog {
        std::uint64_t f0;
        ~FuelLog() {
            auto used = f0 - cambc::fuel_remaining();
            if (used > 60000) std::cout << "fuel r" << cambc::round_number() << " used " << used << '\n';
        }
    } fuel_log{fuel0};
    if (!initialised) {
        init();
        initialised = true;
    }
    moved = false;
    move_failed = false;
    now = static_cast<int>(cambc::round_number());
    auto& me = cambc::self();

    if (!me.is_alive()) {
        buy_items();
        return;
    }
    if (me.is_recalling() || me.is_submitting_gems() || me.is_teleporting()) return;

    const vec2 pos = me.position();
    const bool at_spawn = ally_spawn.is_set(pos);
    if (at_spawn) buy_items();

    read_notes();
    observe_groups();

    const auto camps = visible_camps();
    const auto enemies = visible_enemies();
    const auto allies = visible_allies();
    const float hp = me.health(), max_hp = me.max_health();

    // Heal up on spawn before leaving.
    if (at_spawn && hp < max_hp * 0.95f && enemies.empty()) return;

    // Low health: get out of range, then recall.
    if (!at_spawn && hp < max_hp * 0.35f) {
        auto th = threats_near(camps, enemies, pos);
        if (!th.empty()) {
            // Still allowed to cast a heal/shield or finish an adjacent target while backing off.
            cast_spells(camps, false);
            if (!step_away(th)) {
                if (const Foe* f = pick_in_range(enemies)) attack_foe(*f);
            }
            return;
        }
        me.recall();
        return;
    }

    // Enemy champions nearby: fight when we are not outnumbered.
    if (!enemies.empty()) {
        int foes_near = 0, friends_near = 1;
        for (auto& e : enemies) if (dist(e.pos, pos) <= 6) ++foes_near;
        for (auto& a : allies) if (dist(a.pos, pos) <= 6) ++friends_near;
        bool in_range = pick_in_range(enemies) != nullptr;
        if (foes_near > 0) {
            if (friends_near >= foes_near || in_range) {
                bool close = false;
                for (auto& e : enemies) if (dist(e.pos, pos) <= 4) close = true;
                if (close || in_range) {
                    std::vector<Foe> targets;
                    for (auto& e : enemies) if (dist(e.pos, pos) <= 6) targets.push_back(e);
                    fight(targets, true);
                    return;
                }
            } else if (hp < max_hp * 0.7f) {
                std::vector<vec2> th;
                for (auto& e : enemies) th.push_back(e.pos);
                if (step_away(th)) return;
            }
        }
    }

    // Bank gems when carrying a lot, late in the game, or when an altar is close.
    const int gems = static_cast<int>(me.gem_count());
    if (gems > 0) {
        int ai = nearest_altar(pos, true);
        int ad = ai >= 0 ? dist(pos, altars[ai]) : 1000;
        int ours = static_cast<int>(cambc::global_submitted_gems(team::ally));
        bool win_now = ours + gems >= static_cast<int>(cambc::constants::winning_gems);
        bool late = now >= 1850;
        if (gems >= 40 || win_now || late || (ad <= 8 && gems >= 8) || (ad <= 3)) {
            bool fighting_awake = false;
            for (auto& c : camps) if (c.awake && dist(c.pos, pos) <= c.range + 1) fighting_awake = true;
            if (!fighting_awake || win_now) {
                if (do_tribute()) return;
            }
        }
    }

    // Shopping trip: plenty of gold, nothing carried, not in a fight.
    if (!at_spawn && me.gold_count() >= 240 && gems == 0 && threats_near(camps, enemies, pos).empty()) {
        me.recall();
        return;
    }

    hunt(camps, allies);
}
