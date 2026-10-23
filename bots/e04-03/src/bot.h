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
    bool cast_done = false;      // the one successful cast of this turn is spent
    bool banking = false;        // committed to tributing the carried gems
    int target_group = -1;       // camp group this champion is working on
    int stuck_turns = 0;         // turns without progress toward target_group
    int last_distance = 1 << 20; // distance to target_group last turn
    std::vector<int> skip_until; // per camp group: round until which it is skipped

    void buy_items();
    bool clear_enemy_lock(cambc::bitboard altars);
    bool move_towards_closest(cambc::bitboard goals);
    bool dash_towards(cambc::bitboard goals);
    bool move_away_from(const std::vector<cambc::vec2>& threats);
    std::optional<std::variant<cambc::champion, cambc::camp>> pick_target();
    template<class Target> void fight(const Target& target);
    bool fight_best_target();
    bool cast_spells();
    std::vector<cambc::vec2> threats();
    bool bank_gems();
    int choose_group();
    void farm_camps();

public:
    // Priorities: shop and recall, spells, fight enemies in range, bank gems, then farm as a pack.
    void play_turn();
};
