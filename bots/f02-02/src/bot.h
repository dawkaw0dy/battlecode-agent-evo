#pragma once
#include <cambc.h>
#include <cstdint>
#include <vector>
#include "common.h"
#include "plan.h"

// What one champion remembers about a camp group it cannot always see.
struct Known {
    enum State : std::uint8_t { unknown, alive, reward, respawning } state = unknown;
    int ready = 0;              // respawn round (respawning only)
    std::vector<float> hp;      // last seen health per camp (camps do not heal)
    int blocked_until = 0;      // given up on (contested or stuck) until this round
};

// Per-champion state survives between turns.
class Bot {
    // Static knowledge.
    bool init_ = false;
    int slot_ = -1, size_ = 1;
    bool dasher_ = false;
    std::vector<GroupInfo> g_;
    std::vector<cambc::map::camp_group> h_;
    std::vector<cambc::bitboard> gtiles_;
    std::vector<std::uint8_t> home_slot_;
    std::vector<Dist> gfield_;          // walking distance to each group, filled lazily
    std::vector<int> fill_order_;
    std::vector<std::int16_t> group_at_; // tile -> group of the camp spawned there
    std::vector<std::int16_t> camp_at_;  // tile -> index of that camp inside its group
    Dist home_, altar_;
    int home_altar_ = kFar;             // walk from our spawn to the nearest altar
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
    cambc::bitboard awake_;             // awake camps in view this turn
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
    bool hit_group(int group);
    void spells_on_group(int group);
    void reset_hit(int group);
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
    const ClearCost& clear_cost(int group);
    float estimate_damage(int group);
    float remaining(int group) const;
    const cambc::bitboard& approach_goal(int group, int r);
    const cambc::bitboard& rally_goal(int group);
    bool walk_static(int group, int far);
    bool group_awake(int group) const;
    int raiders_near(int group) const;
    int eta(int group) const;

public:
    void play_turn();
};
