#pragma once
// The opening subsystem: map analysis shared by the draft and the champions, the picks, the draft memory
// and the team's opening route (plan.cpp, draft.cpp).
#include "common.h"

struct GroupInfo {
    std::vector<vec2> tiles;          // camp spawn tiles
    std::vector<cambc::camp_type> kinds;
    vec2 center{};
    int gems = 0, gold = 0, respawn = 0;
    float hp = 0, str = 0;            // total monster health and strength (clusters excluded from str)
    int range = 0;                    // largest monster attack range
    bool boss = false;                // monolith or golem inside
    int d_ally = kFar, d_enemy = kFar;  // walking distance from each spawn
    std::uint32_t handle = 0;         // the engine's camp-group handle
};

// Static walls for our walking: terrain plus the enemy spawn.
const cambc::bitboard& walk_walls();
std::vector<GroupInfo> analyse_map();
std::vector<GroupInfo> analyse_map(const std::vector<cambc::map::camp_group>& groups);  // reuses a camp_groups() read

// Champion picks in slot order for a team of `size`.
std::vector<cambc::champion_draft> pick_order(int size);
// Slot of a champion type in the pick order (-1 if not there).
int slot_of(cambc::champion_type t, int size);

// Home slot per group for `slots` champions (255 = nobody's in particular).
std::vector<std::uint8_t> assign_homes(const std::vector<GroupInfo>& groups, int slots);

// The team's opening: the first groups the whole team takes together, in order, each with the round by
// which it should be done (a step that is late is skipped).
struct Opening {
    std::vector<int> route, due;
    int end = 0;          // last round of the opening
    std::size_t step = 0; // progress (per champion)
    // Group the opening sends us to now (-1: none). `done(g)` tells whether group g was taken.
    template <class Done> int current(int now, Done done) {
        if (now > end) return -1;
        while (step < route.size() && (now > due[step] || done(route[step]))) ++step;
        return step < route.size() ? route[step] : -1;
    }
};
// Route for a team of `size` from our spawn (draft time, or at init without a plan in draft memory).
Opening plan_opening(const std::vector<GroupInfo>& groups, int size);

// Draft memory: written once in the draft (the map analysis, home slots and the opening route), read by
// every champion at init in place of camp_groups() and analyse_map() (~150k fuel on a big camp layout,
// which pushed the first action of every champion to round 2-4).
void write_plan(const std::vector<GroupInfo>& groups, int size);
// Groups, team size, home slots and the opening from draft memory, or made again from the map (`size`
// is then kept). False without a plan.
bool read_plan(std::vector<GroupInfo>& groups, int& size, std::vector<std::uint8_t>& home, Opening& open);

// Weight of group `i` for champion `slot` (home regions).
float home_factor(const std::vector<std::uint8_t>& home, int i, int slot);
// Value multiplier of group `i` when the opening sends us to `current` (1 for every group outside it).
float opening_boost(int i, int current);
