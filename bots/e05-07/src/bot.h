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
    int stuck_turns = 0;         // turns without progress toward target_group
    int last_distance = 1 << 20; // distance to target_group last turn
    std::vector<int> skip_until; // per camp group: round until which it is skipped
    std::vector<int> lock_seen;  // per altar: last round our own lock was seen there (-1: never)
    std::vector<int> respawn_at;    // per camp group: round its respawn is expected (0: alive)
    std::vector<float> group_hp;    // per camp group: last known remaining health of living camps
    std::vector<char> reward_ready; // per camp group: cleared, reward waiting for a capture
    int next_choose = 0;            // round at which the group choice is re-evaluated
    bool target_invalid();

    void observe_groups();
    void observe_groups_unchecked();

    bool is_warden();
    bool carries_lock();
    void buy_items();
    void observe_altars();
    bool place_locks();
    bool warden_duty();
    bool clear_enemy_lock(cambc::bitboard altars);
    bool move_towards_closest(cambc::bitboard goals);
    bool move_away_from(const std::vector<cambc::vec2>& threats);
    std::optional<std::variant<cambc::champion, cambc::camp>> pick_target();
    template<class Target> void fight(const Target& target);
    bool fight_best_target();
    bool heal_allies();
    std::vector<cambc::vec2> threats();
    bool bank_gems();
    int choose_group();
    void farm_camps();

public:
    // Priorities: lock altars, shop and recall, fight enemies in range, bank gems, then farm.
    void play_turn();
};
