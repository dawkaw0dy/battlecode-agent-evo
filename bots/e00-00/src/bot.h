#pragma once
#include <cambc.h>
#include <vector>
#include "plan.h"

struct GroupInfo {
    cambc::map::camp_group group;
    cambc::bitboard spawns;
    cambc::vec2 anchor;
    float hp;
    float str;
    bool golem;
    bool monolith;
    bool reachable;
    int available_round;
};

// Per-champion state survives between turns.
class Bot {
    bool initialised = false;
    Plan plan;
    std::vector<GroupInfo> groups;
    int slot = -1;
    int target = -1;
    bool banking = false;
    cambc::vec2 last_pos{-1, -1};
    int stuck_turns = 0;
    int bought = 0;

    void init();
    void buy_items();
    void claim(int group);
    int claimers(int group) const;
    int choose_target();
    bool go_bank();

public:
    // Priorities: shop and recall, fight enemies in range, bank gems, then farm the plan's best camp group.
    void play_turn();
};
