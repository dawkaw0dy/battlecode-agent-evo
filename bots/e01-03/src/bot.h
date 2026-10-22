#pragma once
#include <cambc.h>
#include <optional>
#include <variant>
#include <vector>

// Per-champion state survives between turns.
class Bot {
    bool initialised = false;
    int region = -1;                  // region slot from the draft plan, -1 when there is none
    std::optional<cambc::vec2> home_altar; // altar nearest to our region
    std::vector<int> group_region;    // region per camp group
    bool moved = false;               // walked this turn
    bool attacked = false;            // used the basic attack this turn
    bool move_failed = false;         // a hidden unit blocked a step this turn
    bool cast_exclusive = false;      // cast a spell that forbids walking this turn
    bool banking = false;             // committed to tributing the carried gems
    int target_group = -1;
    int stuck_turns = 0;
    int last_distance = 1 << 20;
    std::vector<int> skip_until;      // per camp group: round until which it is skipped

    void init();
    void buy_items();
    bool move_towards_closest(cambc::bitboard goals);
    bool move_away_from(const std::vector<cambc::vec2>& threats);
    std::optional<std::variant<cambc::champion, cambc::camp>> pick_target();
    template<class Target> void fight(const Target& target);
    bool fight_best_target();
    void heal_allies();
    std::vector<cambc::vec2> threats(int enemy_range);
    bool bank_gems();
    int choose_group();
    void farm_camps();
    bool answer_invasion();

public:
    // Priorities: shop and recall, avoid lone fights, fight enemies in range, regroup, bank gems, then farm.
    void play_turn();
};
