#pragma once
#include <cambc.h>
#include <optional>
#include <variant>
#include <vector>

// Per-champion state survives between turns.
class Bot {
    bool moved = false;          // walked this turn
    bool attacked = false;       // used the basic attack this turn
    bool move_failed = false;    // a hidden unit blocked a step this turn
    bool cast_exclusive = false; // cast a spell that forbids walking this turn
    bool cast_done = false;      // the turn's one successful cast is spent
    bool banking = false;        // committed to tributing the carried gems
    int target_group = -1;       // camp group this champion is working on
    int stuck_turns = 0;         // turns spent without progress toward target_group
    int last_distance = 1 << 20; // path distance to target_group last turn
    std::vector<int> skip_until; // per camp group: round until which it is skipped
    int chase_turns = 0;         // consecutive turns spent chasing an enemy champion
    int chase_rest_until = 0;    // round until which chasing is off
    int rank = -1;               // order among allied champion types, fixed on the first turn
    bool duty_done = false;      // the opening altar lock assigned to this champion is placed

    bool carries_lock();
    void observe_altars();
    bool place_lock();
    bool clear_enemy_lock(const cambc::bitboard& altars);
    bool lock_duty();

    void buy_items();
    bool move_towards_closest(cambc::bitboard goals);
    bool move_away_from(const std::vector<cambc::vec2>& threats);
    std::optional<std::variant<cambc::champion, cambc::camp>> pick_target();
    template<class Target> void fight(const Target& target);
    bool cast_spells(bool stay);
    bool fight_best_target();
    bool engage();
    std::vector<cambc::vec2> threats();
    bool bank_gems();
    int choose_group();
    void farm_camps();
    void plan_turn();

public:
    // Priorities: shop and recall, spells, fight enemies in range, bank gems, then farm.
    void play_turn();
};
