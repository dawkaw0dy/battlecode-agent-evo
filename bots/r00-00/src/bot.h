#pragma once
#include <cambc.h>
#include <vector>
#include "plan.h"

struct GroupInfo {
    cambc::map::camp_group group;
    cambc::bitboard spawns;
    float hp;
    float str;
    int available_round;
};

// Per-champion state survives between turns.
class Bot {
    bool initialised = false;
    Plan plan;
    std::vector<GroupInfo> groups;
    cambc::vec2 home{0, 0};
    int target = -1;
    cambc::vec2 last_pos{-1, -1};
    int stuck_turns = 0;
    int jungle_bought = 0;
    int power_bought = 0;

    void init();
    void buy_items();
    int choose_target();
    bool go_bank();

public:
    // Priorities: shop and recall, fight enemies in range, bank gems, then farm the best camp group.
    void play_turn();
};
