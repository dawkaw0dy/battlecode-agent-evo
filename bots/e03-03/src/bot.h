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
    bool banking = false;        // committed to tributing the carried gems
    int target_group = -1;       // camp group this champion is working on
    int stuck_turns = 0;         // turns spent without progress toward target_group
    int last_distance = 1 << 20; // path distance to target_group last turn
    int next_choice_round = 0;   // round of the next full camp-group re-score
    std::vector<int> skip_until; // per camp group: round until which it is skipped

    void buy_items();
    bool move_towards_closest(cambc::bitboard goals);
    bool move_away_from(const std::vector<cambc::vec2>& threats);
    std::optional<std::variant<cambc::champion, cambc::camp>> pick_target();
    template<class Target> void fight(const Target& target);
    bool fight_best_target();
    void heal_allies();
    std::vector<cambc::vec2> threats();
    bool bank_gems();
    int choose_group();
    void farm_camps();

public:
    // Priorities: shop and recall, heal, fight enemies in range, bank gems, then farm.
    void play_turn();
};
