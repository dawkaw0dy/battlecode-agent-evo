#pragma once
// Map model shared by every turn: camp groups, altars, static terrain and distance fields.
#include <cambc.h>
#include <cstdint>
#include <memory>
#include <vector>

using cambc::vec2;

inline int cheb(vec2 a, vec2 b) {
    const int dx = a.x > b.x ? a.x - b.x : b.x - a.x;
    const int dy = a.y > b.y ? a.y - b.y : b.y - a.y;
    return dx > dy ? dx : dy;
}

constexpr std::uint16_t kFar = 65535;

// Design numbers per camp type (after patch 100), indexed by camp_type.
struct CampDesign { float hp; float str; int range; };
inline CampDesign camp_design(cambc::camp_type t) {
    switch (t) {
    case cambc::camp_type::wolf: return {1350, 20, 3};
    case cambc::camp_type::elk: return {2400, 60, 4};
    case cambc::camp_type::gem_cluster: return {120, 0, 0};
    case cambc::camp_type::gold_cluster: return {120, 0, 0};
    case cambc::camp_type::monolith: return {20000, 200, 5};
    case cambc::camp_type::golem: return {5000, 120, 4};
    }
    return {1000, 50, 3};
}

enum GroupState : std::uint8_t { kUnknown, kAlive, kReward, kRespawning };

struct Member {
    cambc::camp_type type;
    vec2 pos;
    float hp_max, str;
    int range;
    float hp;  // last known health (hp_max when never seen)
};

struct Group {
    std::uint32_t handle = 0;
    std::vector<Member> m;
    vec2 center;
    int gems = 0, gold = 0, respawn = 0;
    float hp_max = 0, str_total = 0;
    bool boss = false;
    int span = 0;  // largest Chebyshev distance from the centre to a member
    int d_home = kFar, d_enemy = kFar;  // walking distance from each spawn region
    // Memory.
    GroupState state = kUnknown;
    int ready = 0;      // round from which the group is expected alive again
    int seen = -1000;   // last round a member tile was in vision
    int reward_seen = -1000;
    float hp_left() const {
        float s = 0;
        for (const auto& x : m) s += x.hp;
        return s;
    }
};

struct World {
    int w = 0, h = 0;
    std::vector<Group> groups;
    std::vector<vec2> altars;
    cambc::bitboard terrain;       // walls | holes | altars | enemy spawn
    cambc::bitboard spawn_ally;
    cambc::bitboard altar_adj;     // free tiles within submit range of an altar
    cambc::distance_field home;    // distance to our spawn region
    cambc::distance_field altar;   // distance to the nearest altar approach tile
    std::vector<std::unique_ptr<cambc::distance_field>> gfield;  // per group, distance to its tiles
    std::vector<int> field_order;
    bool enemy_done = false;                                // groups by distance from home
    std::vector<vec2> spawn_tiles;
    vec2 home_center;
    vec2 enemy_center;
};

World& world();
void init_world();
void update_groups(int round, vec2 me);
int group_of_tile(vec2 p);
// Compute missing group fields while fuel stays above the floor.
void fill_group_fields(std::uint64_t floor);
const cambc::distance_field* group_field(int gi);
