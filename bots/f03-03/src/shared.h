#pragma once
// Turn helpers shared by the champion code (bot.cpp) and the spell code (spells.cpp).
#include <cambc.h>
#include <cstdint>
#include <exception>
#include <vector>
#include "common.h"

// A turn that ran out of fuel resumes next round on stale data: act only in the round we started.
extern std::uint32_t g_round;
bool live();
bool fuel(std::uint64_t need);
void warn(const char* where, const std::exception& e);

// Vision snapshot, refreshed after anything that changes what we see.
struct View {
    cambc::bitboard vis, camps, champs, tele;
    Bits lvis, lcamps, lchamps, ltele;
    std::vector<vec2> allies;  // allied positions, including ones hidden in bushes
    bool ok = false;
};
const View& view();
bool view_affordable();  // a refresh fits in the fuel left (or none is needed)
void invalidate();
bool free_tile(vec2 p);
const Bits& hard_local();  // what a dash cannot cross

// A visible champion; power is a rough damage per turn.
struct Foe { vec2 pos; float hp, max_hp, power; int range; };
std::vector<Foe> foes(team side);
