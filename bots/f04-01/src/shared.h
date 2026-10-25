#pragma once
// Turn helpers shared by the champion code (bot.cpp) and the spell code (spells.cpp): the fuel subsystem
// (fuel.cpp). Every turn has 100k fuel (2x on the first game turn); a turn that runs out is suspended and
// resumes next round. Host calls are cheap (~40), but each new bitboard costs an allocation and a clear,
// each champion handle two ability queries, and board-sized local loops ~2 fuel per tile: so the snapshot
// below is filled in place, read lazily, and only re-read where our own action could have changed it.
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

// Turn clock: begin_turn() marks the round we act in; suspended() tells that the round moved on
// under us (play_turn() then plays the new round instead of losing it).
void begin_turn();
bool suspended();
std::uint64_t turn_budget();   // fuel this turn started with
std::uint64_t turn_used();     // fuel spent since begin_turn()

// A board-sized local copy of a host board, exported on first use (one host call) and read locally after.
struct LazyBits {
    const cambc::bitboard* src = nullptr;
    mutable Bits bits;
    mutable bool loaded = false;
    void reset(const cambc::bitboard* s) { src = s; loaded = false; }
    bool at(vec2 p) const {
        if (!inside(p) || !src) return false;
        if (!loaded) { bits.load(*src); loaded = true; }
        return bits.at(p);
    }
};

// Vision snapshot, refreshed after anything that changes what we see.
struct View {
    cambc::bitboard vis, camps, champs, tele;
    LazyBits lvis, lcamps, lchamps, ltele;
    std::vector<vec2> allies;  // allied positions, including ones hidden in bushes
    std::vector<cambc::champion_type> ally_types;  // ... and who stands there
    bool ok = false;
};
const View& view();
void invalidate();
bool free_tile(vec2 p);
const Bits& hard_local();  // what a dash cannot cross
const Bits& walls_local(); // what a walk cannot enter
extern Bits g_walls_local, g_hard_local;  // filled once in Bot::initialise()

// A visible champion; power is a rough damage per turn. chan: channelling a tribute (it cannot act
// until it finishes or cancels); recall: channelling a recall; gems: what a kill would take.
struct Foe { vec2 pos; float hp, max_hp, power; int range; bool chan = false, recall = false; int gems = 0; };
std::vector<Foe> foes(team side);

// Raw host reads that skip building SDK handles (a camp group's state() re-reads the whole vision
// board first; a champion handle queries both ability slots).
// Camp group state: 1 alive, 2 reward available, 3 respawning (timer set), 0 not in vision or unknown.
int group_state(const cambc::map::camp_group& g, int& timer);
// Altar locks in view, read once per snapshot.
const cambc::bitboard& lock_tiles(team side);
const cambc::bitboard& lock_tiles_any();
