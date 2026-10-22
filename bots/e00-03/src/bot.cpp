// Champion decisions for the camp sweep. main.cpp dispatches here after the draft.
#include <cambc.h>
#include "bot.h"
#include <algorithm>
#include <cstdint>
#include <span>
#include <type_traits>
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

// Run one engine query/action; a rejected action (e.g. a hidden blocker or a unit that just died)
// returns false instead of aborting the whole turn.
template<class F>
static bool guarded(F&& f) {
    try {
        return static_cast<bool>(f());
    } catch (const std::exception&) {
        return false;
    }
}

// "Excludes walking" spells throw after a move, so remember whether we walked this turn.
static bool moved_this_turn = false;

static bool move_towards_closest(cambc::bitboard goals) {
    auto& me = cambc::self();
    if (!me.can_move() || goals.is_set(me.position()))
        return false;

    // Only free goals or occupied camp tiles can seed the field.
    const auto blocked = blocked_tiles();
    goals &= ~blocked | cambc::vision::camp_tiles();
    if (goals.is_empty())
        return false;

    const auto path_walls = blocked & ~goals;
    cambc::distance_field field;
    cambc::bitboard visited;
    field.compute_to(path_walls, goals, tiles_around(me.position(), 0), visited);

    auto step = me.position();
    for (auto tile : blocked.reachable_tiles(step, me.base_stats().base_move_speed).positions())
        if (field.get(tile) < field.get(step)) step = tile;

    if (step == me.position() || !guarded([&] { return me.move_absolute(step); })) return false;
    moved_this_turn = true;
    return true;
}

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

// Shared memory: one byte per champion (by type) holding its target group + 1.
static unsigned my_slot() {
    return static_cast<unsigned>(cambc::self().champion_name()) % 8u;
}

static void publish_target(int target) {
    std::uint8_t v = static_cast<std::uint8_t>(target < 0 || target > 254 ? 0 : target + 1);
    cambc::shared_memory.write(my_slot(), std::span{&v, 1});
}

static std::uint8_t read_slot(unsigned slot) {
    std::uint8_t v = 0;
    cambc::shared_memory.read(slot, std::span{&v, 1});
    return v;
}

void Bot::init() {
    const auto* plan = reinterpret_cast<const std::uint8_t*>(cambc::draft_memory.data());
    const bool has_plan = plan[0] == 'S';
    const int planned = has_plan ? plan[1] : 0;
    int i = 0;
    for (const auto& group : cambc::map::camp_groups()) {
        Group g{group, group.spawn_tiles()};
        for (const auto& c : group.camps()) {
            g.hp += camp_hp(c.kind());
            g.str += camp_str(c.kind());
            if (c.kind() == cambc::camp_type::golem || c.kind() == cambc::camp_type::monolith) g.big = true;
        }
        if (i < planned && i < 1000) g.plan_eff = plan[2 + i] / 20.f;
        groups.push_back(std::move(g));
        ++i;
    }
    initialised = true;
}

void Bot::buy_items() {
    auto& me = cambc::self();
    // Jungle emblem first (30% less camp damage: fewer recalls), then power shards (6-for-5).
    const auto type = me.champion_name();
    const bool caster = type == cambc::champion_type::stellar || type == cambc::champion_type::prism
        || type == cambc::champion_type::cassi;
    for (int guard = 0; guard < 20; ++guard) {
        cambc::item_type item = caster ? cambc::item_type::jungle_shard_of_focus : cambc::item_type::jungle_shard_of_strength;
        if (jungle_bought >= 5)
            item = caster ? cambc::item_type::power_shard_of_focus : cambc::item_type::power_shard_of_strength;
        if (!me.can_buy(item, 1) || !guarded([&] { me.buy(item, 1); return true; })) break;
        if (jungle_bought < 5) ++jungle_bought;
    }
}

static float my_dps() {
    auto& me = cambc::self();
    return me.strength() * 1.125f + me.focus() * 2.f / 8.f;
}

// A unit may die (and leave vision) from our own cast; check its tile before touching it.
static bool still_there(const cambc::camp& c, vec2 pos) {
    return cambc::vision::camp_tiles().is_set(pos) && c.is_visible() && c.health() > 0;
}
static bool still_there(const cambc::champion& c, vec2 pos) {
    return cambc::vision::champion_tiles().is_set(pos) && c.is_visible() && c.is_alive() && c.health() > 0;
}

// Pick the ground tile in `tiles` that covers most visible camps (radius one).
static bool best_aoe_tile(const cambc::bitboard& tiles, vec2 fallback, vec2& out) {
    const auto camp_tiles = cambc::vision::camp_tiles();
    int best = 0;
    for (const auto& c : cambc::vision::camps()) {
        if (!c.is_visible()) continue;
        const vec2 p = c.position();
        if (!tiles.is_set(p)) continue;
        const int n = static_cast<int>((tiles_around(p, 1) & camp_tiles).count());
        if (n > best) { best = n; out = p; }
    }
    if (best == 0 && tiles.is_set(fallback)) { out = fallback; best = 1; }
    return best > 0;
}

template<class Target>
static bool cast_damage(const Target& target) {
    auto& me = cambc::self();
    if (!me.can_cast() || moved_this_turn) return false;
    for (const auto& ab : me.abilities()) {
        if (!(ab.is_damage() || ab.is_aoe_damage() || ab.is_cc()) || !ab.is_ready()) continue;
        const auto tiles = ab.valid_tiles();
        if (!tiles.is_set(target.position())) continue;
        if (ab.can_target_ground()) {
            vec2 where = target.position();
            if constexpr (std::is_same_v<Target, cambc::camp>) best_aoe_tile(tiles, target.position(), where);
            if (guarded([&] { return ab.use(where); })) return true;
        } else if constexpr (std::is_same_v<Target, cambc::camp>) {
            if (ab.can_target_camps() && guarded([&] { return ab.use(target); })) return true;
        } else {
            if (ab.can_target_enemies() && guarded([&] { return ab.use(target); })) return true;
        }
    }
    return false;
}

static bool use_attack_reset() {
    auto& me = cambc::self();
    if (!me.can_cast() || me.can_attack()) return false;
    for (const auto& ab : me.abilities())
        if (ab.is_attack_reset() && ab.is_ready() && ab.can_target_self()) return guarded([&] { return ab.use(); });
    return false;
}

template<class Target>
static bool hit(const Target& target) {
    auto& me = cambc::self();
    const vec2 pos = target.position();
    bool acted = false;
    if (me.can_attack() && me.attack_tiles().is_set(pos) && still_there(target, pos)) {
        acted = guarded([&] { me.attack(target); return true; });
        if (acted && still_there(target, pos) && use_attack_reset() && me.can_attack() && still_there(target, pos))
            guarded([&] { me.attack(target); return true; });
    }
    if (still_there(target, pos) && cast_damage(target)) acted = true;
    return acted;
}

static bool camps_threaten(vec2 at, int radius) {
    for (const auto& c : cambc::vision::camps())
        if (c.is_visible() && c.is_awake() && distance(c.position(), at) <= radius) return true;
    return false;
}

static void support_abilities() {
    auto& me = cambc::self();
    if (!me.can_cast()) return;
    for (const auto& ab : me.abilities()) {
        if (!ab.is_ready()) continue;
        if (ab.is_shield() && ab.can_target_self() && me.health() < me.max_health() * 0.85f
            && (camps_threaten(me.position(), 5) || !cambc::vision::champions(team::enemy).empty())) {
            if (guarded([&] { return ab.use(); })) return;
        }
        if (ab.is_heal() && ab.can_target_allies()) {
            const auto tiles = ab.valid_tiles();
            for (const auto& ally : cambc::vision::champions(team::ally)) {
                if (!ally.is_visible() || ally.position() == me.position() || !ally.is_alive()) continue;
                if (ally.health() < ally.max_health() * 0.7f && tiles.is_set(ally.position()) && guarded([&] { return ab.use(ally); })) return;
            }
        }
    }
}

// Score groups by reward per round of travel + clear time; respawning groups are skipped, not waited on.
int Bot::choose_target() {
    auto& me = cambc::self();
    const int now = cambc::round_number();
    const auto visible = cambc::vision::visible_tiles();
    const auto visible_camps = cambc::vision::camps();
    int allies = 0;
    for (const auto& ally : cambc::vision::champions(team::ally))
        if (ally.is_visible() && ally.position() != me.position() && distance(ally.position(), me.position()) <= 5) ++allies;
    const float team_dps = my_dps() * (1.f + 0.8f * allies);
    const float damage_mult = jungle_bought >= 5 ? 0.7f : 1.f;

    // Groups other allies have claimed.
    std::vector<int> claimed;
    const unsigned mine = my_slot();
    for (unsigned s = 0; s < 8; ++s)
        if (s != mine) if (const auto v = read_slot(s)) claimed.push_back(v - 1);

    int best = -1;
    float best_score = 0.f;
    for (int i = 0; i < static_cast<int>(groups.size()); ++i) {
        auto& g = groups[i];
        float hp = g.hp, str = g.str;
        cambc::map::camp_group_state state = cambc::map::camp_group_alive{};
        if (!(g.spawns & visible).is_empty() && guarded([&] { state = g.group.state(); return true; })) {
            if (const auto* r = std::get_if<cambc::map::camp_group_respawning>(&state)) {
                g.ready_round = now + std::max(r->timer, 1);
            } else if (std::holds_alternative<cambc::map::camp_group_reward_available>(state)) {
                g.ready_round = 0;
                hp = 0.f;
                str = 0.f;
            } else {
                hp = 0.f;
                str = 0.f;
                for (const auto& c : g.group.camps())
                    if (!visible.is_set(c.spawn_position())) { hp += camp_hp(c.kind()); str += camp_str(c.kind()); }
                for (const auto& c : visible_camps)
                    if (c.is_visible() && g.spawns.is_set(c.spawn_position())) { hp += c.health(); str += camp_str(c.kind()); }
            }
        }
        int dist = 1000;
        for (const auto& c : g.group.camps()) dist = std::min(dist, distance(me.position(), c.spawn_position()));
        if (g.ready_round > now + dist) continue;
        if (hp > 15000.f) continue; // monoliths are not part of the loop
        const float clear = hp / team_dps;
        const float damage = clear * str * damage_mult;
        if (damage > me.health() * 0.5f) continue;
        const float value = static_cast<float>(g.group.gem_value()) + 0.05f * static_cast<float>(g.group.gold_value());
        if (value <= 0.f) continue;
        float score = value / (static_cast<float>(dist) + clear + 5.f);
        score *= 0.85f + 0.15f * std::min(g.plan_eff, 3.f) / 3.f;
        // Small groups split their reward: leave them to the ally that claimed them.
        if (std::find(claimed.begin(), claimed.end(), i) != claimed.end())
            score *= (g.big || hp > 2000.f) ? 1.1f : 0.4f;
        if (i == target) score *= 1.3f; // hysteresis
        if (score > best_score) { best_score = score; best = i; }
    }
    return best;
}

void Bot::go_bank() {
    auto& me = cambc::self();
    cambc::bitboard goals;
    goals.clear();
    for (auto altar : cambc::map::altars().positions()) {
        const bool locked = cambc::vision::has_altar_lock(altar);
        if (distance(me.position(), altar) <= cambc::constants::submit_gems_range && !locked) {
            if (guarded([&] { me.submit_gems(altar); return true; })) return;
        }
        if (!locked) goals |= tiles_around(altar, cambc::constants::submit_gems_range);
    }
    move_towards_closest(goals);
}

// Bank in small batches: one countdown tick per 10 gems, so frequent trips near altars cost little.
bool Bot::should_bank(int nearest_altar) {
    auto& me = cambc::self();
    const unsigned gems = me.gem_count();
    if (gems == 0) { banking = false; return false; }
    if (banking) return true;
    const int now = cambc::round_number();
    bool go = gems >= 30
        || (gems >= 8 && nearest_altar <= 7)
        || nearest_altar <= 2
        || target < 0
        || me.health() < me.max_health() * 0.5f
        || (gems >= 10 && !cambc::vision::champions(team::enemy).empty())
        || now > 1940
        || (now > 1800 && gems >= 5);
    banking = go;
    return go;
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

    guarded([&] { support_abilities(); return true; });

    // Fight enemy champions already in reach.
    const auto enemies = cambc::vision::champions(team::enemy);
    for (const auto& e : enemies) {
        const bool acted = guarded([&] {
            if (!e.is_visible() || !still_there(e, e.position())) return false;
            const bool reach = me.attack_tiles().is_set(e.position()) || distance(e.position(), me.position()) <= 4;
            return reach && hit(e);
        });
        if (acted) { me.set_priority(static_cast<int>(me.health())); return; }
    }

    // Recall when low; stay on spawn until healed.
    if (!at_spawn && me.health() < me.max_health() * 0.33f) {
        if (guarded([&] { me.recall(); return true; })) return;
    }
    if (at_spawn && me.health() < me.max_health() * 0.9f) return;

    // Stuck detection: standing still with a target and not capturing means it cannot be worked.
    const bool on_target = target >= 0 && groups[target].spawns.is_set(me.position());
    if (me.position() == last_pos && target >= 0 && !on_target) {
        if (++stuck_turns > 8) {
            groups[target].ready_round = cambc::round_number() + 40;
            target = -1;
            stuck_turns = 0;
        }
    } else {
        stuck_turns = 0;
    }
    last_pos = me.position();

    int chosen = target;
    if (!guarded([&] { chosen = choose_target(); return true; })) chosen = -1;
    target = chosen;
    publish_target(target);

    int nearest_altar = 1000;
    for (auto altar : cambc::map::altars().positions())
        if (!cambc::vision::has_altar_lock(altar))
            nearest_altar = std::min(nearest_altar, distance(me.position(), altar));

    // Attack camps in range: the target group's members, or any awake camp hitting us.
    auto try_attack = [&]() { return guarded([&] {
        const cambc::camp* best = nullptr;
        const auto camps = cambc::vision::camps();
        for (const auto& c : camps) {
            if (!c.is_visible() || !still_there(c, c.position())) continue;
            const bool ours = target >= 0 && groups[target].spawns.is_set(c.spawn_position());
            if (!ours && !c.is_awake()) continue;
            const bool in_range = me.attack_tiles().is_set(c.position()) || distance(c.position(), me.position()) <= 4;
            if (!in_range) continue;
            if (!best || c.health() < best->health()) best = &c;
        }
        return best && hit(*best);
    }); };

    // Finish a camp fight before walking off to bank; awake camps would chip us anyway.
    const bool fighting = guarded([&] { return camps_threaten(me.position(), 4); });
    if (!fighting && should_bank(nearest_altar)) {
        go_bank();
        me.set_priority(static_cast<int>(me.health()));
        return;
    }

    if (try_attack()) {
        me.set_priority(static_cast<int>(me.health()));
        return;
    }

    if (target < 0) {
        if (me.gem_count() > 0) go_bank();
        else if (!at_spawn) move_towards_closest(cambc::map::spawn_region(team::ally));
        return;
    }

    const auto goals = groups[target].spawns;
    const bool could_move = me.can_move();
    if (move_towards_closest(goals)) try_attack();
    else if (could_move && !goals.is_set(me.position())
             && distance(me.position(), groups[target].group.camps()[0].spawn_position()) > 2) {
        groups[target].ready_round = cambc::round_number() + 60;
        target = -1;
    }
    me.set_priority(static_cast<int>(me.health()));
}
