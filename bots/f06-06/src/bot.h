#pragma once
#include <cambc.h>
#include <array>
#include <cstdint>
#include <vector>
#include "util.h"

// One camp group as the map defines it, plus what this champion remembers of it.
struct Group {
    cambc::map::camp_group h;
    std::vector<vec2> tiles;          // original spawn tiles
    std::vector<vec2> camp_tile;      // spawn tile of each camp
    std::vector<cambc::camp_type> kinds;
    std::vector<float> hp;            // remembered health of each camp (camps never heal)
    std::vector<float> max_hp, camp_str;
    float full_hp = 0;
    cambc::bitboard board;            // spawn tiles
    vec2 center{};
    int gems = 0, gold = 0, respawn = 0;
    float str = 0;
    int range = 0;
    int home = kFar;                  // walk from our spawn
    int bank = kFar;                  // walk to the nearest altar
    enum State : std::uint8_t { alive, reward, respawning } state = alive;
    int ready = 0;                    // respawn round when respawning
    int reward_seen = -100;           // round we last saw its reward waiting
    int avoid_until = -1;             // skip until then (enemies, no route)
};

// A visible champion.
struct Unit {
    vec2 pos{};
    float hp = 0, max_hp = 1, str = 0, focus = 0;
    int range = 1;
    cambc::champion_type type{};
};

struct Awake {
    vec2 pos;
    int range;
    float str;
};

class Bot {
    bool init_ = false;
    int slot_ = 0, size_ = 1;
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
    int me_f_round_ = -100;
    vec2 me_f_pos_{-1, -1};
    bool melee_ = true;
    int range_ = 1;

    // Per turn.
    int round_ = 0;
    vec2 pos_{};
    bool moved_ = false, attacked_ = false, cast_ = false, stop_move_ = false, nopath_ = false;
    std::vector<Unit> foes_, friends_;
    std::vector<std::pair<vec2, int>> camps_;  // visible living camps: tile, group
    std::vector<Awake> awake_;          // awake camps in view
    float incoming_ = 0;                // strength of awake camps that reach us
    float dps_ = 50, taken_ = 1;        // our damage per turn, camp damage multiplier on us
    bool sight_ok_ = false;             // a recent enemy sighting from any of us
    vec2 sight_{};
    std::vector<std::uint8_t> living_;  // per group: a living camp in view

    // Memory.
    int target_ = -1, target_since_ = -100;
    int plug_ = -1, plug_round_ = -100;
    const char* mode_ = "";
    std::array<std::uint64_t, 7> prof_{};

    void initialise();
    void turn();
    void update_blocks();
    void observe();
    void shop();
    void write_status(int value);
    int read_status(int slot) const;
    void report_enemy();
    void read_sighting();
    void measure();
    bool enemy_near(vec2 p, int r) const;

    bool step(const cambc::bitboard& roots);
    bool try_recall();
    void ensure_me_field();
    int find_plug() const;

    bool handle_enemies();
    void flee();
    float flee_score(vec2 q) const;
    const cambc::ability* ready_dash(std::array<cambc::ability, 2>& abs) const;
    bool spell_on_foe(bool stun_only);
    bool tribute();
    void lock_altars();
    bool free_altar(vec2 a) const;
    float my_dps() const;
    float taken_mult() const;
    float remaining_hp(const Group& g, bool full = false) const;
    float damage_taken(const Group& g, float dps, bool full = false) const;
    int choose_target();
    void farm(int target);
    bool disengage();
    bool attack_camps(int group);
    void use_spells(int group);
    bool reset_attack();

public:
    void play_turn();
};
