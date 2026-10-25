#pragma once
#include <cambc.h>
#include <cstdint>
#include <vector>

// Static facts about one camp group, plus what this champion last saw of it.
struct Group {
    cambc::map::camp_group handle;
    cambc::bitboard spawns;
    cambc::vec2 center;
    int radius = 0;
    float hp = 0, dps = 0;
    float gems = 0, gold = 0;
    std::uint8_t flags = 0;
    int dist_us = 0;
    // Observations.
    int ready_round = 0;     // round the group is next alive (0 = alive as far as we know)
    bool reward = false;     // seen with a reward waiting
    float hp_left = 0;       // last seen remaining health
    int blocked_until = 0;   // no route around other camps until this round
};

// Per-champion state survives between turns.
class Bot {
    bool ready = false;
    int slot = 0;
    int plan_kind = 0;
    int bank_at = 30;
    int team_size = 5;
    int width = 0, height = 0;
    std::vector<Group> groups;
    std::vector<std::uint8_t> owner; // tile -> group index + 1
    cambc::bitboard terrain;         // walls, holes, altars, enemy spawn
    cambc::bitboard all_spawns;      // every camp spawn tile
    cambc::distance_field altar_field, spawn_field, target_field;
    int target = -1, field_group = -1;
    int shards = 0;
    int idle_hits = 0;               // turns at the approach radius with nothing to hit
    int stuck = 0;
    bool moved = false, attacked = false, casted = false;

    void init();
    void observe();
    void shop();
    int choose_target();
    void farm();
    bool bank(bool force);
    bool fight();
    bool step_field(const cambc::distance_field& field, int stop_at, const cambc::bitboard& roots);
    bool flee(const std::vector<cambc::vec2>& threats);
    bool attack_unit_at(cambc::vec2 p, bool champion);
    bool cast_on(cambc::vec2 p, bool champion);
    void after_attack(cambc::vec2 p, bool champion);
    void claim(int group);
    float camp_threat_near(cambc::vec2 p, int range_pad);

public:
    void play_turn();
};
