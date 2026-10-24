#pragma once
#include <cambc.h>
#include <cstdint>
#include <vector>
#include "common.h"

// What one champion knows about a camp group.
struct Group {
    std::vector<vec2> tiles;
    std::vector<cambc::camp_type> kinds;
    vec2 center{};
    int gems = 0, gold = 0, reach = 0;
    int d_ally = kFar, d_enemy = kFar;
    cambc::bitboard spawn;
    Dist field;
    enum State : std::uint8_t { unknown, alive, reward, respawning } state = unknown;
    int ready = 0;               // respawn round (respawning only)
    std::vector<float> hp;       // last seen health per camp (camps do not heal)
    int blocked_until = 0;       // given up (contested or stuck) until this round
};

struct Altar {
    vec2 pos{};
    int d_ally = kFar, d_enemy = kFar;
};

class Bot {
    bool init_ = false;
    int slot_ = 0, size_ = 1;
    bool guard_ = false, dasher_ = false, ranged_ = false;
    std::vector<Group> g_;
    std::vector<cambc::map::camp_group> h_;
    std::vector<std::int16_t> group_at_, camp_at_;
    std::vector<Altar> altars_;
    int guard_altar_ = -1;
    Dist home_, altar_;
    cambc::bitboard spots_, goal_, close_, post_;
    int goal_g_ = -1, goal_r_ = -1, goal_sig_ = 0;
    std::vector<int> fill_order_;

    int target_ = -1, chosen_at_ = -100;
    int spawn_wait_ = 0;
    int stuck_since_ = 0, best_eta_ = kFar, stuck_g_ = -1;
    cambc::bitboard camp_block_;
    bool block_dirty_ = true;
    bool shop_trip_ = false;
    int last_enemy_seen_ = -1000;
    int locks_placed_ = 0, locks_cancel_ = 0, lock_gems_ = 0;
    int flee_until_ = -1;

    // Per turn.
    vec2 pos_{};
    bool moved_ = false, attacked_ = false, cast_ = false, move_failed_ = false;
    float incoming_ = 0, dps_ = 50, taken_ = 1;

    void initialise();
    void fill_fields(std::uint64_t reserve);
    void observe();
    void rebuild_block();
    void measure();
    void shop();
    int eta(int g) const;

    bool go(const cambc::bitboard& goals, bool dash);
    bool try_dash(const cambc::distance_field& f, const cambc::bitboard& seen);
    bool go_home_or_recall(int walk_limit);

    bool use_lock();
    bool enemies();
    bool tribute(bool force);
    int choose(int& wait);
    void farm(int g);
    bool hit_camps(int g);
    void after_hit();
    void guard_post();

    float remaining(int g) const;
    float damage_taken(int g) const;
    void write_claim(int g);
    int claimed_by_other(int g) const;

    void turn();

public:
    void play_turn();
};
