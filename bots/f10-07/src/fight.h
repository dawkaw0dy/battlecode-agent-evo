#pragma once
// Small turn-by-turn fight model used to decide between engaging and retreating.
#include <cambc.h>
#include <vector>

struct Body {
    cambc::vec2 pos;
    float hp = 0;
    float dps = 0;    // sustained damage per turn against champions
    int range = 1;
    bool ranged = false;
    int arrive = 0;   // turns before it can hit anything
    float stake = 0;  // extra value lost when it dies (carried gems)
};

// Sustained damage per turn of a champion against champions, from its kit.
float champ_dps(cambc::champion_type t, float str, float focus);
// Extra effective health from a self shield.
float champ_shield(cambc::champion_type t, float focus);

// Value of fighting for `turns` turns: their losses minus ours (hp, deaths, stakes).
float fight_value(std::vector<Body> us, std::vector<Body> them, int turns);
