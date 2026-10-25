#pragma once
#include <cambc.h>
#include <cstdint>
#include <vector>
#include "common.h"
#include "plan.h"
#include "shared.h"
#include "spells.h"

// What one champion remembers about a camp group it cannot always see.
struct Known {
    enum State : std::uint8_t { unknown, alive, reward, respawning } state = unknown;
    int ready = 0;              // respawn round (respawning only)
    std::vector<float> hp;      // last seen health per camp (camps do not heal)
    int blocked_until = 0;      // given up on (contested or stuck) until this round
};

// Camp spawn tile -> (group, camp index): a small open-addressing table (map-sized arrays cost
// tens of thousands of fuel just to fill on the first turn).
struct CampIndex {
    std::vector<std::int32_t> key;      // tile index + 1, 0 = empty
    std::vector<std::int16_t> grp, cmp;
    unsigned mask = 0;
    void init(std::size_t n) {
        std::size_t cap = 64;
        while (cap < 2 * n + 2) cap <<= 1;
        key.assign(cap, 0);
        grp.assign(cap, -1);
        cmp.assign(cap, -1);
        mask = static_cast<unsigned>(cap - 1);
    }
    unsigned slot(int t) const { return (static_cast<unsigned>(t) * 2654435761u >> 7) & mask; }
    void put(int t, int g, int c) {
        unsigned h = slot(t);
        while (key[h] != 0 && key[h] != t + 1) h = (h + 1) & mask;
        key[h] = t + 1;
        grp[h] = static_cast<std::int16_t>(g);
        cmp[h] = static_cast<std::int16_t>(c);
    }
    int find(int t) const {
        if (key.empty()) return -1;
        for (unsigned h = slot(t); key[h] != 0; h = (h + 1) & mask)
            if (key[h] == t + 1) return static_cast<int>(h);
        return -1;
    }
    int group(int t) const { const int h = find(t); return h < 0 ? -1 : grp[static_cast<std::size_t>(h)]; }
    int camp(int t) const { const int h = find(t); return h < 0 ? -1 : cmp[static_cast<std::size_t>(h)]; }
};

// Per-champion state survives between turns.
class Bot {
    // Static knowledge.
    bool init_ = false;
    int slot_ = -1, size_ = 1, region_ = -1;  // region_: our home region (shared in pairs)
    bool dasher_ = false;
    std::vector<GroupInfo> g_;
    std::vector<cambc::map::camp_group> h_;
    std::vector<cambc::bitboard> gtiles_;
    std::vector<std::uint8_t> home_slot_;
    std::vector<Dist> gfield_;          // walking distance to each group, filled lazily
    std::vector<int> fill_order_;
    CampIndex camp_index_;              // tile -> group of the camp spawned there, and its index inside the group
    Dist home_, altar_, enemy_home_;
    int home_altar_ = kFar;             // walk from our spawn to the nearest altar
    bool altar_ready_ = false;          // altar_ and altar_spots_ computed (deferred past the first turn)
    void compute_altars();
    void ensure_home();
    const cambc::map::camp_group& handle(int group);  // camp_groups() on first use
    cambc::bitboard altar_spots_;

    // Memory.
    std::vector<Known> k_;
    int target_ = -1, target_wait_ = 0, chosen_at_ = -100;
    int capture_g_ = -1, capture_since_ = 0;
    int spawn_wait_ = 0;
    int stuck_g_ = -1, stuck_since_ = 0, best_eta_ = kFar;
    bool cant_buy_ = false;
    int last_enemy_seen_ = -1000;
    cambc::bitboard camp_block_;        // tiles of camps we believe alive, seen or not
    bool block_dirty_ = true;
    std::vector<std::uint8_t> block_buf_;
    std::vector<int> reach_;            // largest distance from a group's centre to its camps
    cambc::bitboard awake_;             // awake camps in view this turn, and ones seen awake lately
    std::vector<std::pair<vec2, int>> awake_mem_;  // awake camp tile, last round seen awake
    cambc::bitboard goal_;              // cached approach goal
    int goal_group_ = -1, goal_r_ = -1, goal_sig_ = 0;
    int plug_ = -1, plug_since_ = 0;    // a living group that closes our way: clear it
    bool plug_altar_ = false;           // the plug guards the altar rather than a path
    int altar_wait_until_ = -1;         // too weak to clear the altar's guard until about then
    int shrink_ = 0, shrink_g_ = -1;    // approach closer when walls hide the camps from range
    struct Seen { vec2 pos{}; float hp = 0, power = 0; int round = -1000; };
    std::vector<Seen> foe_mem_;         // last sighting of each enemy champion
    int fight_until_ = -1, flee_until_ = -1;
    int chase_turns_ = 0, no_chase_until_ = -1;  // a chase that lands no hit is dropped

    // Per turn.
    vec2 pos_{};
    bool moved_ = false, attacked_ = false, cast_ = false, move_failed_ = false;
    float incoming_ = 0;        // strength of awake camps that can hit us
    int exit_steps_ = 0;
    float taken_ = 1.0f;        // camp damage multiplier on us
    float dps_ = 50.0f;         // our damage per turn against camps

    void turn();
    void initialise();
    void fill_fields(std::uint64_t reserve);
    void observe();
    void measure();
    void rebuild_camp_block();
    void shop();
    void write_claim(int group);
    int claimed_by_other(int group) const;

    bool go(const cambc::bitboard& goals, bool dash);
    bool step_on(const cambc::distance_field& f, const cambc::bitboard& seen, const cambc::bitboard& goals,
                 const cambc::bitboard& blocked, bool dash);
    bool try_dash(const cambc::distance_field& f, const cambc::bitboard& seen);
    void go_home();
    bool recall_or_walk(int walk_limit);

    bool enemies();
    bool tribute();
    int choose(int& wait);
    void farm(int group);
    bool hit_group(int group, bool force = false);  // force: try even when can_attack() says no

    // Spells (spells.cpp, from f02-01): which slot, on which target, at what moment.
    Kit kit_{};
    void spells_on_group(int group);     // before walking: shield, heal, damage and stuns on camps
    void reset_hit(int group);           // after a hit (or a ranged walk): reset and hit again
    bool fight_spells(const std::vector<Foe>& list);       // casts and strikes; true when a hit landed
    bool strike_after_move(const std::vector<Foe>& list);  // hit (with a reset) after closing in
    void flee_spells(const std::vector<Foe>& list);        // free hits and stuns while running
    float spell_dps() const;             // spell damage per turn against camps
    bool escape_camps();

    // Camp farming (see the camps section of bot.cpp).
    struct ClearCost { float hp_sum = -1, base = 0, peak = 0; };
    std::vector<std::vector<std::uint8_t>> threat_;  // [group][m * n + c]: camp m reaches us while we hit c
    std::vector<ClearCost> cost_;
    int my_range_ = 1;
    std::vector<int> bank_d_;           // walk from each group to the nearest altar
    int raid_n_ = 1;                    // champions the current target's plan counts on
    bool rally_need_ = false;           // ... gathering outside its reach first
    int rally_g_ = -1, rally_since_ = -1;
    cambc::bitboard rally_goal_;
    int rally_goal_g_ = -1, rally_goal_round_ = -100;
    bool goal_plain_ = false;           // goal_ is every tile in range, not just the safest
    int plain_until_ = -1;
    int static_off_until_ = -1;         // walk routed (not down the cached field) until then
    std::vector<std::uint8_t> goal_buf_;

    void build_threats();
    const std::vector<std::uint8_t>& threats_of(int group);
    const ClearCost& clear_cost(int group);
    float estimate_damage(int group);
    float remaining(int group) const;
    const cambc::bitboard& approach_goal(int group, int r);
    const cambc::bitboard& rally_goal(int group);
    bool walk_static(int group, int far);
    bool group_awake(int group) const;
    int raiders_near(int group) const;
    int eta(int group) const;

    // Defense (gem safety, altar locks).
    std::vector<vec2> altars_;
    bool deny();
    bool lock_altars(bool detour, int reach = 8);
    bool lock_needed() const;
    std::vector<int> lock_seen_;        // per altar: last round we saw our lock on it
    std::vector<std::uint8_t> enemy_lock_;  // per altar: an enemy lock was on it when we last looked
    int lock_try_g_ = -1, lock_try_since_ = 0;
    float foe_damage(float turns);
    bool tribute_under_fire();

public:
    void play_turn();
};
