#pragma once
// A few turns of focus fire between two sides, to decide whether to fight.
#include "common.h"
#include <vector>

struct SimUnit {
    vec2 pos;
    float hp, dps;
    int range;
    int gems;
    bool ranged;
    bool channel;  // cannot act until the channel ends
};

struct SimResult {
    float value;       // their losses minus ours (health, kills, carried gems)
    bool we_die;       // the first unit of our side dies within the horizon
    int their_deaths, our_deaths;
};

// ours[0] is us. Turns until a unit can strike = walking distance beyond its range (ranged lose a turn per step).
SimResult simulate(const std::vector<SimUnit>& ours, const std::vector<SimUnit>& theirs, int turns);
