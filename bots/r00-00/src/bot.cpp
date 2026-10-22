// Champion decisions. main.cpp dispatches here after the draft.
#include <cambc.h>
#include "bot.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <variant>
#include <vector>

using cambc::vec2;
using cambc::team;

static int distance(vec2 a, vec2 b) {
    return static_cast<int>(a.chebyshev_distance(b));
}

static cambc::bitboard tiles_around(vec2 position, int radius) {
    cambc::bitboard tiles;
    tiles.clear();
    tiles.draw_chebyshev(position, radius);
    return tiles;
}

// Combine terrain and visible occupants using native bitboard operations.
static cambc::bitboard blocked_tiles() {
    auto blocked = cambc::map::walls() | cambc::map::holes() | cambc::map::altars()
        | cambc::map::spawn_region(team::enemy) | cambc::vision::champion_tiles()
        | cambc::vision::camp_tiles() | cambc::vision::teleport_tiles();
    blocked.set(cambc::self().position(), false);
    return blocked;
}

static bool moved_this_turn = false;

static bool move_raw(cambc::bitboard goals);

static bool move_towards_closest(cambc::bitboard goals) {
    const bool moved = move_raw(goals);
    if (moved) moved_this_turn = true;
    return moved;
}

static bool move_raw(cambc::bitboard goals) {
    auto& me = cambc::self();
    if (!me.can_move() || goals.is_set(me.position()))
        return false;

    const auto blocked = blocked_tiles();
    goals &= ~blocked | cambc::vision::camp_tiles();
    if (goals.is_empty())
        return false;
    if (goals.count() == 1 && !blocked.is_set(goals.positions()[0]))
        return me.move_towards(goals.positions()[0]);

    const auto path_walls = blocked & ~goals;
    cambc::distance_field field;
    cambc::bitboard visited;
    field.compute_to(path_walls, goals, tiles_around(me.position(), 0), visited);

    auto step = me.position();
    for (auto tile : blocked.reachable_tiles(step, me.base_stats().base_move_speed).positions())
        if (field.get(tile) < field.get(step)) step = tile;

    return step != me.position() && me.move_absolute(step);
}

// Static health and strength per camp kind (current patch values).
static float camp_hp(cambc::camp_type t) {
    switch (t) {
    case cambc::camp_type::wolf: return 1350.f;
    case cambc::camp_type::elk: return 2400.f;
    case cambc::camp_type::monolith: return 20000.f;
    case cambc::camp_type::golem: return 5000.f;
    default: return 120.f;
    }
}

static float camp_str(cambc::camp_type t) {
    switch (t) {
    case cambc::camp_type::wolf: return 20.f;
    case cambc::camp_type::elk: return 60.f;
    case cambc::camp_type::monolith: return 200.f;
    case cambc::camp_type::golem: return 120.f;
    default: return 0.f;
    }
}

void Bot::init() {
    plan = read_plan();
    for (const auto& group : cambc::map::camp_groups()) {
        GroupInfo info{group, group.spawn_tiles(), 0.f, 0.f, 0};
        for (const auto& c : group.camps()) {
            info.hp += camp_hp(c.kind());
            info.str += camp_str(c.kind());
        }
        groups.push_back(std::move(info));
    }
    const auto spawn = cambc::map::spawn_region(team::ally).positions();
    home = spawn.empty() ? cambc::self().position() : spawn[0];
    initialised = true;
}

void Bot::buy_items() {
    auto& me = cambc::self();
    // Melee/strength champions: a jungle emblem first (30% less camp damage), then power shards.
    const bool focus_user = me.champion_name() == cambc::champion_type::willow;
    for (int guard = 0; guard < 20; ++guard) {
        cambc::item_type item = cambc::item_type::jungle_shard_of_strength;
        if (jungle_bought >= 5) {
            item = (power_bought % 3 == 2) ? cambc::item_type::power_shard_of_health
                : (focus_user ? cambc::item_type::power_shard_of_focus : cambc::item_type::power_shard_of_strength);
        }
        if (!me.can_buy(item, 1)) break;
        me.buy(item, 1);
        if (jungle_bought < 5) ++jungle_bought;
        else ++power_bought;
    }
}

// Rough per-turn damage this champion deals to camps.
static float my_dps() {
    auto& me = cambc::self();
    float dps = me.strength();
    dps += me.strength() / 8.f; // attack resets
    dps += me.focus() * 2.f / 8.f;
    return dps;
}

// Use one ability against a camp or an enemy champion at `pos`.
template<class Target>
static bool cast_damage(const Target& target) {
    auto& me = cambc::self();
    if (!me.can_cast() || moved_this_turn) return false;
    for (const auto& ab : me.abilities()) {
        if (!(ab.is_damage() || ab.is_aoe_damage() || ab.is_cc()) || !ab.is_ready()) continue;
        const auto tiles = ab.valid_tiles();
        if (!tiles.is_set(target.position())) continue;
        if (ab.can_target_ground()) {
            if (ab.use(target.position())) return true;
        } else if constexpr (std::is_same_v<Target, cambc::camp>) {
            if (ab.can_target_camps() && ab.use(target)) return true;
        } else {
            if (ab.can_target_enemies() && ab.use(target)) return true;
        }
    }
    return false;
}

static bool use_attack_reset() {
    auto& me = cambc::self();
    if (!me.can_cast() || me.can_attack()) return false;
    for (const auto& ab : me.abilities())
        if (ab.is_attack_reset() && ab.is_ready() && ab.can_target_self()) return ab.use();
    return false;
}

// A unit may die (and leave vision) from our own cast; check its tile before touching it.
static bool still_there(const cambc::camp& c, vec2 pos) {
    return cambc::vision::camp_tiles().is_set(pos) && c.is_visible() && c.health() > 0;
}
static bool still_there(const cambc::champion& c, vec2 pos) {
    return cambc::vision::champion_tiles().is_set(pos) && c.is_visible() && c.is_alive() && c.health() > 0;
}

template<class Target>
static bool hit(const Target& target) {
    auto& me = cambc::self();
    const vec2 pos = target.position();
    bool acted = cast_damage(target);
    if (me.can_attack() && me.attack_tiles().is_set(pos) && still_there(target, pos)) {
        me.attack(target);
        acted = true;
        if (still_there(target, pos) && use_attack_reset() && me.can_attack() && still_there(target, pos))
            me.attack(target);
    }
    return acted;
}

static void support_abilities() {
    auto& me = cambc::self();
    if (!me.can_cast()) return;
    for (const auto& ab : me.abilities()) {
        if (!ab.is_ready()) continue;
        if (ab.is_shield() && ab.can_target_self() && me.health() < me.max_health() * 0.8f) {
            bool threat = false;
            for (const auto& c : cambc::vision::camps())
                if (c.is_awake() && distance(c.position(), me.position()) <= 5) threat = true;
            if (!cambc::vision::champions(team::enemy).empty()) threat = true;
            if (threat && ab.use()) return;
        }
        if (ab.is_heal() && ab.can_target_allies()) {
            const auto tiles = ab.valid_tiles();
            for (const auto& ally : cambc::vision::champions(team::ally)) {
                if (ally.position() == me.position() || !ally.is_alive()) continue;
                if (ally.health() < ally.max_health() * 0.7f && tiles.is_set(ally.position()) && ab.use(ally)) return;
            }
        }
    }
}

int Bot::choose_target() {
    auto& me = cambc::self();
    const int now = cambc::round_number();
    const auto visible = cambc::vision::visible_tiles();
    const float dps = my_dps();
    const auto visible_camps = cambc::vision::camps();
    int allies = 0;
    for (const auto& ally : cambc::vision::champions(team::ally))
        if (ally.position() != me.position() && distance(ally.position(), me.position()) <= 4) ++allies;
    const float team_dps = dps * (1.f + 0.8f * allies);
    const float damage_mult = jungle_bought >= 5 ? 0.7f : 1.f;

    int best = -1;
    float best_score = 0.f;
    for (int i = 0; i < static_cast<int>(groups.size()); ++i) {
        auto& g = groups[i];
        float hp = g.hp;
        float str = g.str;
        bool reward = false;
        if (!(g.spawns & visible).is_empty()) {
            const auto state = g.group.state();
            if (const auto* r = std::get_if<cambc::map::camp_group_respawning>(&state)) {
                g.available_round = now + r->timer;
            } else if (std::holds_alternative<cambc::map::camp_group_reward_available>(state)) {
                reward = true;
                g.available_round = 0;
            } else {
                g.available_round = 0;
                hp = 0.f;
                str = 0.f;
                for (const auto& c : g.group.camps())
                    if (!visible.is_set(c.spawn_position())) { hp += camp_hp(c.kind()); str += camp_str(c.kind()); }
                for (const auto& c : visible_camps)
                    if (g.spawns.is_set(c.spawn_position())) { hp += c.health(); str += camp_str(c.kind()); }
            }
        }
        int dist = 1000;
        for (const auto& c : g.group.camps()) dist = std::min(dist, distance(me.position(), c.spawn_position()));
        if (g.available_round > now + dist) continue;
        if (reward) { hp = 0.f; str = 0.f; }
        const float clear = hp / team_dps;
        const float damage = clear * str * damage_mult;
        if (damage > me.health() * plan.danger_pct / 100.f) continue;
        const float value = static_cast<float>(g.group.gem_value()) + 0.08f * static_cast<float>(g.group.gold_value());
        if (value <= 0.f) continue;
        float score = value / (static_cast<float>(dist) + clear + 6.f);
        if (i == target) score *= 1.3f; // hysteresis
        if (score > best_score) { best_score = score; best = i; }
    }
    return best;
}

bool Bot::go_bank() {
    auto& me = cambc::self();
    cambc::bitboard goals;
    goals.clear();
    for (auto altar : cambc::map::altars().positions()) {
        const bool locked = cambc::vision::has_altar_lock(altar);
        if (distance(me.position(), altar) <= cambc::constants::submit_gems_range && !locked) {
            me.submit_gems(altar);
            return true;
        }
        if (!locked) goals |= tiles_around(altar, cambc::constants::submit_gems_range);
    }
    move_towards_closest(goals);
    return true;
}

void Bot::play_turn() {
    if (!initialised) init();
    moved_this_turn = false;

    auto& me = cambc::self();
    if (!me.is_alive()) {
        buy_items();
        return;
    }
    if (me.is_recalling() || me.is_submitting_gems() || me.is_teleporting()) return;

    const bool at_spawn = cambc::map::spawn_region(team::ally).is_set(me.position());
    if (at_spawn) buy_items();
    if (!me.can_move() && !me.can_attack() && !me.can_cast()) return;

    support_abilities();

    // Fight enemy champions in range; chase weak ones nearby.
    const auto enemies = cambc::vision::champions(team::enemy);
    for (const auto& e : enemies) {
        if (!still_there(e, e.position())) continue;
        if (me.attack_tiles().is_set(e.position()) || distance(e.position(), me.position()) <= 4) {
            if (hit(e)) return;
        }
    }

    // Recall when badly hurt.
    if (!at_spawn && me.health() < me.max_health() * 0.33f) {
        me.recall();
        return;
    }
    // Stay to heal on spawn.
    if (at_spawn && me.health() < me.max_health() * 0.95f) return;

    for (const auto& e : enemies) {
        if (!still_there(e, e.position())) continue;
        if (e.health() < me.health() * 0.7f && me.health() > me.max_health() * 0.6f
            && distance(e.position(), me.position()) <= me.attack_range() + plan.chase_range
            && !cambc::map::spawn_region(team::enemy).is_set(e.position())) {
            move_towards_closest(tiles_around(e.position(), 1));
            if (still_there(e, e.position())) hit(e);
            return;
        }
    }

    // Stuck detection: standing still without fighting means the target cannot be worked.
    if (me.position() == last_pos && target >= 0) {
        if (++stuck_turns > 8) {
            groups[target].available_round = cambc::round_number() + 40;
            target = -1;
            stuck_turns = 0;
        }
    } else {
        stuck_turns = 0;
    }
    last_pos = me.position();

    target = choose_target();

    const unsigned gems = me.gem_count();
    if (gems > 0 && (gems >= plan.bank_threshold || target < 0 || me.health() < me.max_health() * 0.5f
                     || cambc::round_number() > 1960)) {
        // Finish an almost-dead camp fight first.
        go_bank();
        return;
    }

    // Attack camps in range: the target group's camps, or any awake camp already fighting us.
    auto try_attack = [&]() {
        const auto camps = cambc::vision::camps();
        const cambc::camp* best = nullptr;
        for (const auto& c : camps) {
            if (!still_there(c, c.position())) continue;
            const bool ours = target >= 0 && groups[target].spawns.is_set(c.spawn_position());
            if (!ours && !c.is_awake()) continue;
            const bool in_range = me.attack_tiles().is_set(c.position()) || distance(c.position(), me.position()) <= 4;
            if (!in_range) continue;
            if (!best || c.health() < best->health()) best = &c;
        }
        return best && hit(*best);
    };

    if (try_attack()) {
        stuck_turns = 0;
        if (me.can_move() && target >= 0 && !me.attack_tiles().is_empty()) {
            // Melee champions may still step closer after attacking.
        }
        return;
    }

    if (target < 0) {
        // Nothing worth doing: head home to shop.
        if (!at_spawn) move_towards_closest(cambc::map::spawn_region(team::ally));
        return;
    }

    auto goals = groups[target].spawns;
    const bool could_move = me.can_move();
    if (move_towards_closest(goals)) try_attack();
    else if (could_move && !goals.is_set(me.position())
             && distance(me.position(), groups[target].group.camps()[0].spawn_position()) > 2) {
        // Unreachable or blocked target: skip it for a while.
        groups[target].available_round = cambc::round_number() + 60;
        target = -1;
    }
    me.set_priority(static_cast<int>(me.health()));
}
