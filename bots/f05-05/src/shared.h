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
    std::vector<cambc::champion_type> ally_types;  // ... and who stands there
    bool ok = false;
};
const View& view();
void invalidate();
bool free_tile(vec2 p);
const Bits& hard_local();  // what a dash cannot cross

// A visible champion; power is a rough damage per turn. chan: channelling a tribute (it cannot act
// until it finishes or cancels); recall: channelling a recall; gems: what a kill would take.
struct Foe { vec2 pos; float hp, max_hp, power; int range; bool chan = false, recall = false; int gems = 0; };
std::vector<Foe> foes(team side);

// The latest enemy sighting any of us wrote to shared memory.
struct Sighting { bool ok = false; vec2 where{}; int age = 999; };
Sighting read_sighting();
