#pragma once
#include "common.h"
#include <memory>
#include <vector>

// A camp group: static design data plus what this champion last learned about it.
struct Group {
    std::vector<vec2> tiles;
    std::vector<vec2> ring;   // free tiles beside the group
    std::vector<TileRef> tr, rr;   // raw offsets of tiles and of (up to 12) ring tiles
    std::uint32_t handle = 0;
    int gems = 0, gold = 0, respawn = 0, range = 0, monsters = 0;
    float hp = 0, str = 0;
    bool boss = false;
    int dspawn = kFar, denemy = kFar;
    int st = 1;            // 1 alive, 2 reward waiting, 3 respawning
    int ready_at = 0;      // respawn round when st == 3
    float hp_left = 0;     // monster health left when last seen
    float str_left = 0;    // monster strength left when last seen
    int ally_cap = -100;   // last round an ally was seen standing on it after the clear
};

struct EnemyView {
    vec2 pos;
    float hp, maxhp, str, focus;
    int range;
    bool channel;
};

struct CampView {
    vec2 pos;
    float hp, str;
    int range, gi;
    bool awake, monster;
};

class Bot {
    bool inited = false;
    int slot = 0, nallies = 1;
    int round = 0;
    std::vector<Group> groups;
    std::vector<cambc::map::camp_group> gobj;
    std::vector<short> tile_group;     // group index per tile, -1 if none
    std::vector<vec2> altars;
    std::unique_ptr<cambc::bitboard> swalls;   // walls, holes, altars, enemy spawn
    std::unique_ptr<cambc::bitboard> myspawn;
    std::unique_ptr<cambc::distance_field> home;
    std::unique_ptr<cambc::bitboard> home_v;
    std::unique_ptr<cambc::distance_field> dme;
    std::unique_ptr<cambc::bitboard> dme_v;
    std::unique_ptr<cambc::distance_field> work;
    std::unique_ptr<cambc::bitboard> work_v;

    int target = -1;
    int last_choice = -100;
    int stall = 0, shrink = 0;
    int bought = 0;
    int spawn_wait = 0;
    bool retreat = false;
    int enemy_round = -100;     // last enemy sighting (ours or shared)
    vec2 enemy_pos{-100, -100};
    int chase = 0, no_chase_until = 0;

    // Turn snapshot.
    vec2 pos;
    float hp = 0, maxhp = 1, str = 0, focus = 0;
    int gems = 0, gold = 0, range = 1;
    bool ranged = false, moved = false, attacked = false, cast = false, move_failed = false;
    std::vector<EnemyView> enemies;
    std::vector<CampView> camps;
    std::unique_ptr<cambc::bitboard> blocked;
    std::unique_ptr<cambc::bitboard> campwalls;   // tiles of groups that may still hold camps

    void init();
    void snapshot();
    void update_groups();
    void shop();
    void choose_target(bool want_bank, int& bank_d);
    int altar_distance(vec2& best_altar);
    bool step_to(cambc::bitboard& goals, bool avoid);
    bool flee();
    float threat_at(vec2 p) const;
    bool fight_enemies();
    bool farm();
    bool bank();
    bool try_recall();
    bool strike_camps(bool can_excl);
    bool strike_enemy(bool can_excl);
    bool use_spells(bool can_excl, bool vs_enemy);
    void basic_and_reset(bool vs_enemy);

public:
    void play_turn();
};
