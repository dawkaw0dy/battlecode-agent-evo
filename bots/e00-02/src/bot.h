#pragma once
#include <cambc.h>
#include <optional>
#include <variant>
#include <vector>

// Per-champion state survives between turns. One champion leads; the rest follow it as one group.
class Bot {
    bool moved = false;
    bool cast_exclusive = false;
    int slot = -1;

    // Leader-only memory: chosen camp group, respawn timers and progress towards it.
    int target = -1;
    int wait_rounds = 0;
    int best_dist = 1 << 20;
    int stall_rounds = 0;
    bool banking = false;
    std::vector<int> skip_until;

    void buy_items();
    bool move_towards_closest(cambc::bitboard goals);
    bool move_away_from(const std::vector<cambc::vec2>& threats);
    std::vector<cambc::vec2> threats();

    bool camp_allowed(cambc::camp_type kind) const;
    std::optional<std::variant<cambc::champion, cambc::camp>> pick_target(int group, bool engaged);
    template<class Target> void fight(const Target& target);
    bool fight_best(int group, bool engaged);
    void heal_allies();
    void use_shield();
    bool chase_enemies(std::optional<cambc::vec2> anchor);
    bool submit_if_adjacent();
    bool go_bank();

    void update_respawns();
    int choose_target();
    void lead(int round);
    void follow(cambc::vec2 leader, int group, int flags);

public:
    // Priorities: shop and recall, fight enemies together, follow the leader to camps, bank gems.
    void play_turn();
};
