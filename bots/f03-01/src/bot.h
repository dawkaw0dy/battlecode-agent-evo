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
    Bits foe_zone_;                     // tiles near the enemy spawn, filled lazily

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
    bool escape_camps();

    // Spells (spells.cpp): which slot, on which target, at what moment.
    Kit kit_{};
    void spells_on_group(int group);     // before walking: shield, heal, damage and stuns on camps
    void reset_hit(int group);           // after a hit (or a ranged walk): reset and hit again
    bool fight_spells(const std::vector<Foe>& list);       // casts and strikes; true when a hit landed
    bool strike_after_move(const std::vector<Foe>& list);  // hit (with a reset) after closing in
    void flee_spells(const std::vector<Foe>& list);        // free hits and stuns while running
    float spell_dps() const;             // spell damage per turn against camps

    float estimate_damage(int group) const;
    float remaining(int group) const;
    const cambc::bitboard& approach_goal(int group, int r);
    int eta(int group) const;

public:
    void play_turn();
};
