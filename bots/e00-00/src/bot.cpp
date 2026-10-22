// Champion decisions. main.cpp dispatches here after the draft.
#include <cambc.h>
#include "bot.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <type_traits>
#include <variant>
#include <vector>

using cambc::vec2;
using cambc::team;

int distance(vec2 a, vec2 b) {
    return static_cast<int>(a.chebyshev_distance(b));
}

cambc::bitboard tiles_around(vec2 position, int radius) {
    cambc::bitboard tiles;
    tiles.clear();
    tiles.draw_chebyshev(position, radius);
    return tiles;
}

// Combine terrain and visible occupants using native bitboard operations.
cambc::bitboard blocked_tiles() {
    auto blocked = cambc::map::walls() | cambc::map::holes() | cambc::map::altars()
        | cambc::map::spawn_region(team::enemy) | cambc::vision::champion_tiles()
        | cambc::vision::camp_tiles() | cambc::vision::teleport_tiles();
    blocked.set(cambc::self().position(), false);
    return blocked;
}

// A few live queries can still fail on units at the edge of vision (seen with engine errors 97/98);
// such a unit is simply skipped for this turn.
template<class F>
static bool guarded(F&& f) {
    try {
        return f();
    } catch (const cambc::error&) {
        return false;
    }
}

// Spells that exclude walking throw after a move, so remember whether we moved.
static bool moved_this_turn = false;

bool move_towards_closest(cambc::bitboard goals) {
    auto& me = cambc::self();
    if (!me.can_move() || goals.is_set(me.position()))
        return false;

    // Only free goals or occupied camp tiles can seed the field.
    const auto blocked = blocked_tiles();
    goals &= ~blocked | cambc::vision::camp_tiles();
    if (goals.is_empty())
        return false;

    // Expand from the goals until our current tile is reached.
    const auto path_walls = blocked & ~goals;
    cambc::distance_field field;
    cambc::bitboard visited;
    field.compute_to(path_walls, goals, tiles_around(me.position(), 0), visited);

    // Choose a reachable step with a smaller distance to the goal.
    auto step = me.position();
    for (auto tile : blocked.reachable_tiles(step, me.base_stats().base_move_speed).positions())
        if (!blocked.is_set(tile) && field.get(tile) < field.get(step)) step = tile;

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

static bool focus_user() {
    const auto t = cambc::self().champion_name();
    return t == cambc::champion_type::stellar || t == cambc::champion_type::prism || t == cambc::champion_type::cassi;
}

void Bot::init() {
    plan = read_plan();
    for (const auto& group : cambc::map::camp_groups()) {
        GroupInfo info{group, group.spawn_tiles(), {0, 0}, 0.f, 0.f, false, false, true, 0};
        bool first = true;
        for (const auto& c : group.camps()) {
            if (first) { info.anchor = c.spawn_position(); first = false; }
            info.hp += camp_hp(c.kind());
            info.str += camp_str(c.kind());
            if (c.kind() == cambc::camp_type::golem) info.golem = true;
            if (c.kind() == cambc::camp_type::monolith) info.monolith = true;
        }
        groups.push_back(std::move(info));
    }
    // Drop groups we cannot walk to from spawn (islands behind walls or holes).
    {
        const auto walls = cambc::map::walls() | cambc::map::holes() | cambc::map::altars()
            | cambc::map::spawn_region(team::enemy);
        cambc::distance_field field;
        cambc::bitboard visited;
        field.compute(walls, tiles_around(cambc::self().position(), 0), visited);
        for (auto& g : groups) {
            cambc::bitboard near = g.spawns;
            for (auto t : g.spawns.positions()) near |= tiles_around(t, 1);
            g.reachable = !(near & visited).is_empty();
        }
    }
    // Our index among allied champions picks our shared-memory claim byte.
    const auto allies = cambc::champions(team::ally);
    for (int i = 0; i < static_cast<int>(allies.size()); ++i)
        if (allies[i] == cambc::self()) slot = i;
    if (slot >= static_cast<int>(cambc::constants::shared_memory_bytes)) slot = -1;
    initialised = true;
}

void Bot::claim(int group) {
    if (slot < 0) return;
    std::array<std::uint8_t, 1> value{static_cast<std::uint8_t>(group >= 0 && group < 254 ? group + 1 : 0)};
    cambc::shared_memory.write(static_cast<std::size_t>(slot), std::span{value});
}

int Bot::claimers(int group) const {
    std::array<std::uint8_t, 8> bytes{};
    const std::size_t n = std::min<std::size_t>(8, cambc::constants::shared_memory_bytes);
    cambc::shared_memory.read(0, std::span{bytes.data(), n});
    int count = 0;
    for (int i = 0; i < static_cast<int>(n); ++i)
        if (i != slot && bytes[i] == group + 1) ++count;
    return count;
}

void Bot::buy_items() {
    auto& me = cambc::self();
    const auto main_stat = focus_user() ? cambc::item_type::power_shard_of_focus : cambc::item_type::power_shard_of_strength;
    for (int guard = 0; guard < 20; ++guard) {
        // Every third shard is health, the rest the champion's damage stat.
        const auto item = (bought % 3 == 2) ? cambc::item_type::power_shard_of_health : main_stat;
        if (!me.can_buy(item, 1)) break;
        me.buy(item, 1);
        ++bought;
    }
}

static float my_dps() {
    auto& me = cambc::self();
    return me.strength() * 1.1f + me.focus() * 2.f / 8.f;
}

// A unit may die (and leave vision) from our own cast; check its tile before touching it.
// Visibility is checked before any other query: touching an unseen unit throws.
static bool still_there(const cambc::camp& c) {
    return guarded([&] {
        if (!c.is_visible()) return false;
        const vec2 pos = c.position();
        return cambc::vision::camp_tiles().is_set(pos) && c.health() > 0;
    });
}
static bool still_there(const cambc::champion& c) {
    return guarded([&] {
        if (!c.is_visible() || !c.is_alive()) return false;
        const vec2 pos = c.position();
        return cambc::vision::champion_tiles().is_set(pos) && c.health() > 0;
    });
}

template<class Target>
static bool cast_damage(const Target& target) {
    auto& me = cambc::self();
    if (!me.can_cast() || moved_this_turn || !still_there(target)) return false;
    for (const auto& ab : me.abilities()) {
        if (!still_there(target)) return false;
        if (!(ab.is_damage() || ab.is_aoe_damage() || ab.is_cc()) || ab.is_dash() || !ab.is_ready()) continue;
        if (!guarded([&] { return ab.valid_tiles().is_set(target.position()); })) continue;
        if (ab.can_target_ground()) {
            if (guarded([&] { return ab.use(target.position()); })) return true;
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
    bool acted = cast_damage(target);
    if (me.can_attack() && still_there(target) && me.attack_tiles().is_set(target.position())) {
        acted = guarded([&] { me.attack(target); return true; });
        if (still_there(target) && use_attack_reset() && me.can_attack() && still_there(target)
            && me.attack_tiles().is_set(target.position()))
            guarded([&] { me.attack(target); return true; });
    }
    return acted;
}

static void support_abilities() {
    auto& me = cambc::self();
    if (!me.can_cast()) return;
    for (const auto& ab : me.abilities()) {
        if (!ab.is_ready()) continue;
        if (ab.is_shield() && !ab.is_dash() && ab.can_target_self() && me.health() < me.max_health() * 0.85f) {
            bool threat = !cambc::vision::champions(team::enemy).empty();
            for (const auto& c : cambc::vision::camps())
                if (still_there(c) && c.is_awake() && distance(c.position(), me.position()) <= 5) threat = true;
            if (threat && guarded([&] { return ab.use(); })) return;
        }
        if (ab.is_heal() && ab.can_target_allies()) {
            cambc::bitboard tiles;
            tiles.clear();
            guarded([&] { tiles = ab.valid_tiles(); return true; });
            for (const auto& ally : cambc::vision::champions(team::ally)) {
                if (ally == me || !still_there(ally)) continue;
                if (ally.health() < ally.max_health() * 0.7f && tiles.is_set(ally.position()) && guarded([&] { return ab.use(ally); })) return;
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

    int best = -1, fallback = -1;
    float best_score = 0.f, fallback_score = 0.f;
    for (int i = 0; i < static_cast<int>(groups.size()); ++i) {
        auto& g = groups[i];
        if (g.monolith || !g.reachable) continue;
        if (g.golem && now < plan.golem_round * 10) continue;
        float hp = g.hp, str = g.str;
        bool reward = false;
        if (!(g.spawns & visible).is_empty()) {
            float seen_hp = 0.f, seen_str = 0.f;
            bool alive = false;
            const bool read = guarded([&] {
                const auto state = g.group.state();
                if (const auto* r = std::get_if<cambc::map::camp_group_respawning>(&state)) {
                    g.available_round = now + r->timer;
                } else if (std::holds_alternative<cambc::map::camp_group_reward_available>(state)) {
                    reward = true;
                    g.available_round = 0;
                } else {
                    g.available_round = 0;
                    alive = true;
                    for (const auto& c : g.group.camps())
                        if (!visible.is_set(c.spawn_position())) { seen_hp += camp_hp(c.kind()); seen_str += camp_str(c.kind()); }
                    for (const auto& c : visible_camps)
                        if (g.spawns.is_set(c.spawn_position()) && still_there(c)) { seen_hp += c.health(); seen_str += camp_str(c.kind()); }
                }
                return true;
            });
            if (read && alive) { hp = seen_hp; str = seen_str; }
        }
        const int dist = distance(me.position(), g.anchor);
        if (g.available_round > now + dist) continue;
        if (reward) { hp = 0.f; str = 0.f; }

        // Allies already on a group speed it up; past the plan's cap they waste time.
        const int others = claimers(i);
        if (i != target && others >= plan.cap) continue;
        const int helpers = std::max(others, (g.golem || hp > 3000.f) ? static_cast<int>(plan.helpers) : 0);
        const float clear = hp / (dps * (1.f + 0.85f * static_cast<float>(helpers)));
        const float damage = clear * str / static_cast<float>(1 + helpers);
        const bool risky = damage > me.health() * static_cast<float>(plan.danger) / 100.f;

        const float value = static_cast<float>(g.group.gem_value()) + 0.1f * static_cast<float>(g.group.gold_value());
        if (value <= 0.f) continue;
        // Gems are shared when several allies capture together.
        float score = value / static_cast<float>(1 + others) / (static_cast<float>(dist) + clear + 6.f);
        if (hp > 3000.f && others > 0) score *= 1.5f; // join a big fight already started
        if (i == target) score *= 1.3f;              // hysteresis
        if (risky) {
            // Risky groups are only a fallback; recalls cover the danger.
            if (!g.golem && score > fallback_score) { fallback_score = score; fallback = i; }
        } else if (score > best_score) {
            best_score = score;
            best = i;
        }
    }
    return best >= 0 ? best : fallback;
}

bool Bot::go_bank() {
    auto& me = cambc::self();
    cambc::bitboard goals;
    goals.clear();
    for (auto altar : cambc::map::altars().positions()) {
        const bool locked = cambc::vision::has_altar_lock(altar);
        if (distance(me.position(), altar) <= cambc::constants::submit_gems_range && !locked) {
            guarded([&] { me.submit_gems(altar); return true; });
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
        claim(-1);
        buy_items();
        return;
    }
    if (me.is_recalling() || me.is_submitting_gems() || me.is_teleporting()) return;

    const bool at_spawn = cambc::map::spawn_region(team::ally).is_set(me.position());
    if (at_spawn) {
        buy_items();
        banking = false;
    }
    if (!me.can_move() && !me.can_attack() && !me.can_cast()) return;
    support_abilities();

    // Fight enemy champions within reach of attacks or spells.
    const auto enemies = cambc::vision::champions(team::enemy);
    for (const auto& e : enemies) {
        if (!still_there(e)) continue;
        const vec2 pos = e.position();
        if (me.attack_tiles().is_set(pos) || distance(pos, me.position()) <= 4)
            if (hit(e)) return;
    }

    const float hp_frac = me.health() / me.max_health();
    if (!at_spawn && hp_frac * 100.f < static_cast<float>(plan.recall)) {
        claim(-1);
        guarded([&] { return me.recall(), true; });
        return;
    }
    if (at_spawn && hp_frac < 0.95f) return;

    // Chase weak enemies nearby; the aggro plan chases further.
    for (const auto& e : enemies) {
        float enemy_hp = 0.f;
        if (!still_there(e) || !guarded([&] { enemy_hp = e.health(); return true; })) continue;
        const vec2 pos = e.position();
        if (enemy_hp < me.health() * 0.7f && hp_frac > 0.6f
            && distance(pos, me.position()) <= me.attack_range() + plan.chase
            && !cambc::map::spawn_region(team::enemy).is_set(pos)) {
            move_towards_closest(tiles_around(pos, 1));
            if (still_there(e)) hit(e);
            return;
        }
    }

    // Standing still without fighting means the target cannot be worked.
    if (me.position() == last_pos && target >= 0) {
        if (++stuck_turns > 8) {
            groups[target].available_round = cambc::round_number() + 40 + distance(me.position(), groups[target].anchor);
            target = -1;
            stuck_turns = 0;
        }
    } else {
        stuck_turns = 0;
    }
    last_pos = me.position();
    target = choose_target();
    claim(target);

    // Banking is sticky until the bag is empty, so we do not oscillate.
    const unsigned gems = me.gem_count();
    if (gems == 0) banking = false;
    else if (gems >= plan.bank || target < 0 || hp_frac < 0.5f || cambc::round_number() > 1940
)
        banking = true;

    auto try_attack = [&]() {
        const cambc::camp* best = nullptr;
        const auto camps = cambc::vision::camps();
        float best_hp = 0.f;
        const auto reach = me.attack_tiles();
        for (const auto& c : camps) {
            float hp = 0.f;
            const bool ok = guarded([&] {
                if (!still_there(c)) return false;
                const bool ours = target >= 0 && groups[target].spawns.is_set(c.spawn_position());
                if (!ours && !c.is_awake()) return false;
                if (!reach.is_set(c.position()) && distance(c.position(), me.position()) > 4) return false;
                hp = c.health();
                return true;
            });
            if (ok && (!best || hp < best_hp)) { best = &c; best_hp = hp; }
        }
        return best && hit(*best);
    };

    // Finish an awake camp before walking off with gems.
    if (try_attack()) { stuck_turns = 0; return; }
    if (banking) {
        claim(-1);
        go_bank();
        return;
    }

    // Shop trip when rich and nothing is fighting us.
    if (!at_spawn && gems == 0 && me.gold_count() >= plan.shop_gold * 10u && enemies.empty()) {
        bool awake = false;
        for (const auto& c : cambc::vision::camps())
            if (still_there(c) && c.is_awake() && distance(c.position(), me.position()) <= 5) awake = true;
        if (!awake) {
            claim(-1);
            guarded([&] { return me.recall(), true; });
            return;
        }
    }

    if (target < 0) {
        if (!at_spawn) move_towards_closest(cambc::map::spawn_region(team::ally));
        return;
    }

    const auto goals = groups[target].spawns;
    const bool could_move = me.can_move();
    if (move_towards_closest(goals)) try_attack();
    else if (could_move && !goals.is_set(me.position()) && distance(me.position(), groups[target].anchor) > 2) {
        // Unreachable or blocked target: skip it for a while.
        groups[target].available_round = cambc::round_number() + 60 + distance(me.position(), groups[target].anchor);
        target = -1;
        claim(-1);
    }
    me.set_priority(static_cast<int>(me.health()));
}
