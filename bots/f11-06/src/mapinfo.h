#pragma once
// Static map analysis shared by the draft and the champions.
#include "common.h"

struct GroupInfo {
    std::vector<vec2> tiles;
    std::vector<cambc::camp_type> kinds;
    vec2 center{};
    int gems = 0, gold = 0, respawn = 0;
    float hp = 0, dps = 0;      // total monster health and strength
    int range = 0;              // largest monster attack range
    int fighters = 0;           // monsters that attack (not clusters)
    bool boss = false;          // golem or monolith inside
    bool mono = false;
    std::vector<std::pair<float, float>> kill_order;   // (hp, strength) in kill-priority order
    int dA = kFar, dE = kFar;   // walking distance from our / the enemy spawn
};

struct MapInfo {
    int w = 0, h = 0;
    std::vector<GroupInfo> groups;
    std::vector<cambc::map::camp_group> raw;   // engine groups, same order as `groups`
    cambc::distance_field home;                // walking distance from our spawn (static walls)
    bitboard home_reached;
    bitboard camp_tiles;                       // every camp spawn tile
    std::vector<vec2> altars;
    std::vector<int> altar_dA, altar_dE;
    vec2 spawn_center{}, enemy_center{};
    int spawn_gap = 0;          // walking distance between the spawns
};

// Terrain our champions can never enter: walls, holes, altars and the enemy spawn.
const bitboard& static_walls();

// Analyse the map once (camp layout, distances from both spawns). Costs ~100k fuel on big maps.
const MapInfo& map_info();
