#pragma once
#include <cambc.h>
#include <vector>

// Per-champion state survives between turns.
class Bot {
    bool initialised = false;
    int my_index = 0;        // index among allied champions (draft order)
    bool warden = false;     // the champion that holds enemy altars
    int warden_rank = 0;
    int target_group = -1;   // camp group we are farming
    int retarget_round = -1000;
    float last_health = 0;
    int last_hit_round = -1000;
    int capture_wait = 0;
    int progress_round = 0;
    unsigned last_wealth = 0;

    void init();
    void buy_items();
    bool use_spells();
    bool fight();
    bool bank_gems();
    bool warden_turn();
    void farm();
    int choose_group();
    void turn();

public:
    void play_turn();
};
