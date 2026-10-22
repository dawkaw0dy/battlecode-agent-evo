#pragma once
#include <cambc.h>
#include <vector>

// One camp group of the sweep loop, with the respawn round we last observed.
struct Group {
    cambc::map::camp_group group;
    cambc::bitboard spawns;
    float hp = 0.f;       // full health of all members
    float str = 0.f;      // summed strength of fighters
    float plan_eff = 1.f; // draft-time efficiency rating
    bool big = false;     // golem or monolith inside
    int ready_round = 0;  // earliest round the group is worth visiting
};

// Per-champion state survives between turns.
class Bot {
    bool initialised = false;
    std::vector<Group> groups;
    int target = -1;
    bool banking = false;
    int jungle_bought = 0;
    int stuck_turns = 0;
    cambc::vec2 last_pos{-1, -1};

    void init();
    void buy_items();
    int choose_target();
    void go_bank();
    bool should_bank(int nearest_altar);

public:
    // Priorities: shop and recall, fight enemies in range, bank small batches, then sweep camps.
    void play_turn();
};
