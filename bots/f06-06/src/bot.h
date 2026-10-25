#pragma once
#include <cambc.h>
#include <cstdint>
#include <vector>
#include "util.h"

// One camp group as the map defines it, plus what this champion remembers of it.
struct Group {
    cambc::map::camp_group h;
    std::vector<vec2> tiles;      // original spawn tiles
    std::vector<cambc::camp_type> kinds;
    cambc::bitboard board;        // spawn tiles
    vec2 center{};
    int gems = 0, gold = 0, respawn = 0;
    float hp = 0, str = 0;
    int range = 0;
    // Memory.
    enum State : std::uint8_t { alive, reward, respawning } state = alive;
    int ready = 0;                // respawn round when respawning
    int avoid_until = -1;         // contested by enemies or unreachable: skip until then
    int bank = kFar;              // walk from the group to the nearest altar
};

// A visible champion.
struct Unit {
    vec2 pos{};
    float hp = 0, max_hp = 1, str = 0, focus = 0;
    int range = 1;
    int gems = 0;
    cambc::champion_type type{};
};

class Bot {
    bool init_ = false;
    int slot_ = 0, size_ = 1, party_ = 0;
    std::vector<int> party_of_;         // slot -> party
    std::vector<float> base_dps_;       // slot -> design damage per turn
    std::vector<Group> g_;
    std::vector<std::int16_t> group_at_;  // spawn tile -> group
    std::vector<vec2> altars_;
    cambc::bitboard terrain_;           // never walkable: walls, holes, altars, enemy spawn
    cambc::bitboard altar_spots_;       // walkable tiles next to an altar
    cambc::distance_field altar_f_, spawn_f_, me_f_, altar_t_f_;
    cambc::bitboard altar_seen_, spawn_seen_, me_seen_, altar_t_seen_;
    cambc::bitboard camp_block_;        // spawn tiles of groups we believe alive, seen or not
    bool block_dirty_ = true;
    int block_round_ = -100;
    void update_blocks();
    int me_f_round_ = -100;
    vec2 me_f_pos_{-1, -1};
    bool melee_ = true;
    int range_ = 1;

    // Per-turn state.
    int round_ = 0;
    vec2 pos_{};
    bool moved_ = false, attacked_ = false, cast_ = false, stop_move_ = false;
    std::vector<Unit> foes_, friends_;  // visible, friends exclude self
    std::vector<std::pair<vec2, int>> camps_;  // visible living camps: tile, group
    float incoming_ = 0;                // strength of awake camps that can reach us

    // Memory.
    int target_ = -1, target_since_ = -100;
    int last_threat_ = -100;
    int home_wait_ = 0;
    const char* mode_ = "";
    bool nopath_ = false;               // the last step found no route to its roots
    int find_plug() const;

    void initialise();
    void observe();
    void shop();
    void write_status(int value);
    int read_status(int slot) const;
    int leader_slot() const;

    bool step(const cambc::bitboard& roots, bool roots_occupied);
    bool walk_field(const cambc::distance_field& f, const cambc::bitboard& seen);
    void ensure_me_field();

    bool handle_enemies();
    void retreat();
    bool tribute();
    int choose_target(float dps);
    float party_dps() const;
    float kill_time(const Group& g, float dps) const;
    float damage_taken(const Group& g, float dps) const;
    void farm(int target, int present, int need);
    bool attack_camps(int group);
    void use_spells(int group);
    bool reset_attack();
    float my_dps() const;

public:
    void play_turn();
};
