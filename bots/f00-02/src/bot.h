#pragma once
#include <cambc.h>
#include <cstdint>
#include <vector>

// Static description of one camp group, read once from the map.
struct GroupInfo {
    cambc::bitboard spawns;
    std::vector<cambc::vec2> tiles;      // camp spawn tiles
    std::vector<int> kinds;              // camp_type per tile
    int gems = 0, gold = 0;
    float monster_hp = 0, cluster_hp = 0, strength = 0;
    int max_range = 0, monsters = 0, respawn = 0;
    int altar_dist = 0;                  // path distance from the group to the nearest altar
};

// A visible champion snapshot.
struct Seen { cambc::vec2 pos; float hp, str; };

// Per-champion state survives between turns.
class Bot {
    bool initialised = false;
    int my_index = 0;
    std::vector<GroupInfo> groups;
    std::vector<int> ready_round;        // round from which a group is expected to be alive again
    std::vector<int> banned_until;       // groups we gave up on
    int target = -1;                     // current camp group
    int wait_turns = 0;
    int stuck_turns = 0;
    int last_enemy_round = -1000;
    bool moved = false, cast_lock = false;
    cambc::bitboard static_walls, approach, own_spawn, all_spawns;
    cambc::distance_field spawn_field, altar_field;

    void init();
    void buy_items();
    void update_groups();
    void choose_target(const cambc::distance_field& from_me, const cambc::bitboard& reach);
    bool step_to(const cambc::bitboard& goals);
    bool flee(const std::vector<cambc::vec2>& threats);
    bool go_bank();
    bool fight_group();
    void use_support_spells();
    void cast_on_enemy(const std::vector<Seen>& enemies);
    bool cast_on_camp(const cambc::vec2& tile);
    void write_claim(int group);
    int read_claim(int index);

public:
    void play_turn();
    void play_turn_inner();
};
