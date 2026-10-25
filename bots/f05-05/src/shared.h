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
const Bits& walls_local();  // walk_walls()
const Bits& hard_local();   // what a dash cannot cross
const cambc::bitboard& open_tiles();  // ~walk_walls(), kept
void nav_setup();           // after init_dims(): the boards above
// Tile reads of host-format boards and fields (raw when the layout check passed); outside reads unreached.
int field_at(const cambc::distance_field& f, vec2 p);
bool board_at(const cambc::bitboard& b, vec2 p);

// A visible champion; power is a rough damage per turn. chan: channelling a tribute (it cannot act
// until it finishes or cancels); recall: channelling a recall; gems: what a kill would take.
struct Foe { vec2 pos; float hp, max_hp, power; int range; bool chan = false, recall = false; int gems = 0; };
std::vector<Foe> foes(team side);

// A camp group's state when a tile of it is in view: 1 alive, 2 reward, 3 respawning (timer), 0 unknown.
int group_state(const cambc::map::camp_group& g, int& timer);

// Allies whose position we can read this round (read once a round: they only move between our turns).
struct AllySeen { vec2 pos; cambc::champion_type type; float hp; };
const std::vector<AllySeen>& allies_seen();

// The latest enemy sighting any of us wrote to shared memory.
struct Sighting { bool ok = false; vec2 where{}; int age = 999; };
Sighting read_sighting();
