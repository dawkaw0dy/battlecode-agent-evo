#pragma once
#include <cambc.h>
#include <array>
#include <cstdint>
#include <vector>
#include "common.h"
#include "plan.h"

constexpr int kDashReserve = 10;  // rounds after an enemy sighting in which the dash is kept for a retreat

// What one champion remembers about a camp group it cannot always see.
struct Known {
    enum State : std::uint8_t { unknown, alive, reward, respawning } state = unknown;
    int ready = 0;              // respawn round (respawning only)
    std::vector<float> hp;      // last seen health per camp (camps do not heal)
    int blocked_until = 0;      // given up on (contested or stuck) until this round
};

struct Fighter {
    vec2 pos{};
    float hp = 0, max_hp = 1, str = 0;
    float burst = 0;        // extra damage on its first turn in reach (ready reset or damage spell)
    float sustain = 0;      // damage per turn in a long fight
    float camp_in = 0;      // camp damage it takes each turn (it is the nearest to an awake camp)
    float value = 150;      // what its death is worth to the other side
    int range = 1, delay = 0, gems = 0;
    bool ranged = false, stun = false, stunned = false, channel = false, safe = false, dasher = false;
    float threat_reach() const { return ranged ? static_cast<float>(range) : static_cast<float>(range + 1); }
};

// Shared memory: bytes 0-4 hold each slot's target group + 1; bytes 5-7 the latest enemy sighting.
struct Sighting { bool ok = false; vec2 where{}; int age = 999; };
Sighting read_sighting();
void write_sighting(vec2 p);
bool has_lock();  // we carry an altar lock

// One altar as the endgame sees it (endgame.cpp).
struct Altar {
    vec2 at{};
    std::vector<vec2> spots;     // tiles we can walk to that take a tribute here
    cambc::bitboard goal;        // ... as a board
    cambc::bitboard lock_goal;   // reachable tiles in lock range
};

// Per-champion state survives between turns.
class Bot {
    // Static knowledge.
    bool init_ = false;
    int slot_ = -1, size_ = 1;
    bool dasher_ = false;
    std::vector<GroupInfo> g_;
    std::vector<cambc::map::camp_group> h_;
    std::vector<cambc::bitboard> gtiles_;
    std::vector<std::uint8_t> home_slot_;
    std::vector<Dist> gfield_;          // walking distance to each group, filled lazily
    std::vector<int> fill_order_;
    std::vector<std::int16_t> group_at_; // tile -> group of the camp spawned there
    std::vector<std::int16_t> camp_at_;  // tile -> index of that camp inside its group
    Dist home_, altar_;
    int home_altar_ = kFar;             // walk from our spawn to the nearest altar
    cambc::bitboard altar_spots_;
    std::vector<vec2> altars_;          // altar tiles, read once
    std::vector<int> enemy_lock_;       // per altar: round we last saw an enemy lock on it, -1 if seen free since
    std::vector<int> lock_seen_;        // per altar: round we last saw our own lock on it
    int lock_try_g_ = -1, lock_try_since_ = 0;
    // Endgame (endgame.cpp): altars, lock memory, the banking walk.
    std::vector<Altar> alt_;
    bool eg_ready_ = false;
    int eg_round_ = -1;                 // last round eg_observe() ran
    int hold_i_ = -1;                   // the only altar, whose tribute tiles a boss respawns on
    cambc::bitboard eg_seen_, bank_goal_;

    // Memory.
    std::vector<Known> k_;
    int target_ = -1, target_wait_ = 0, chosen_at_ = -100;
    int capture_g_ = -1, capture_since_ = 0;
    int spawn_wait_ = 0;
    int stuck_g_ = -1, stuck_since_ = 0, best_eta_ = kFar;
    bool cant_buy_ = false;
    int last_enemy_seen_ = -1000;
    bool chase_dash_ = false;           // go() below is a chase: the dash reserve does not apply
    cambc::bitboard camp_block_;        // tiles of camps we believe alive, seen or not
    bool block_dirty_ = true;
    std::vector<std::uint8_t> block_buf_;
    std::vector<int> reach_;            // largest distance from a group's centre to its camps
    cambc::bitboard awake_;             // awake camps in view this turn, and ones seen awake lately
    std::vector<std::pair<vec2, int>> awake_mem_;  // awake camp tile, last round seen awake
    cambc::bitboard goal_;              // cached approach goal
    int goal_group_ = -1, goal_r_ = -1, goal_sig_ = 0;
    int plug_ = -1, plug_since_ = 0;    // a living group that closes our way: clear it
    bool plug_altar_ = false;           // the plug guards the altar rather than a path
    int altar_wait_until_ = -1;         // too weak to clear the altar's guard until about then
    int shrink_ = 0, shrink_g_ = -1;    // approach closer when walls hide the camps from range
    std::vector<cambc::champion> others_;  // allied champions other than us
    Bits spawn_local_;                  // our spawn tiles
    int fight_round_ = -100;            // last round we weighed a fight ...
    bool fight_engaged_ = false;        // ... and whether we took it
    int spawn_hold_ = 0;
    int alive_round_ = -1, last_mode_ = 0;  // trace only                // turns spent holding our spawn against enemies
    int chase_turns_ = 0, no_chase_until_ = -1;  // a chase that lands no hit is dropped
    std::vector<vec2> ally_near_;       // allies counted in this turn's fight

    // Per turn.
    vec2 pos_{};
    bool moved_ = false, attacked_ = false, cast_ = false, move_failed_ = false;
    float incoming_ = 0;        // strength of awake camps that can hit us
    int exit_steps_ = 0;
    float taken_ = 1.0f;        // camp damage multiplier on us
    float dps_ = 50.0f;         // our damage per turn against camps

    void turn();
    void initialise();
    void fill_fields(std::uint64_t reserve);
    void observe();
    void measure();
    void rebuild_camp_block();
    void shop();
    void write_claim(int group);
    int claimed_by_other(int group) const;

    bool go(const cambc::bitboard& goals, bool dash);
    bool step_on(const cambc::distance_field& f, const cambc::bitboard& seen, const cambc::bitboard& goals,
                 const cambc::bitboard& blocked, bool dash);
    bool try_dash(const cambc::distance_field& f, const cambc::bitboard& seen);
    void go_home();
    bool recall_or_walk(int walk_limit);

    // Defense (defense.cpp): enemy vision, channels, retreats, hidden attackers.
    std::vector<Fighter> foes_;                    // enemies in sight this turn (guard())
    mutable std::array<cambc::bitboard, 8> eye_;   // vision from enemy tiles, this round
    mutable std::array<vec2, 8> eye_at_{};
    mutable size_t eye_n_ = 0;
    mutable int eye_round_ = -1;
    int hp_round_ = -1;                            // last round we read our health ...
    float hp_last_ = 0, camp_last_ = 0;            // ... it, and the camp fire we expected after it
    int ambush_until_ = -1;                        // an unseen attacker hit us lately
    const cambc::bitboard* eyes(vec2 p) const;
    bool sees(const Fighter& e, vec2 t) const;
    bool recall_safe(const std::vector<Fighter>& theirs) const;
    float exposure(int turns) const;
    bool channel_safe(int turns) const;
    void read_foes();
    void guard();
    void guard_end();
    bool evade_hidden();
    bool fighter_at(vec2 p, Fighter& f) const;

    bool enemies();
    void camp_fire(std::vector<Fighter>& ours, std::vector<Fighter>& theirs) const;
    float threat_at(vec2 t, const std::vector<Fighter>& theirs) const;
    bool retreat(std::vector<Fighter>& theirs, int dmin);
    bool strike(std::vector<Fighter>& theirs);
    void fight_spells(std::vector<Fighter>& theirs, bool engage);
    bool tribute();
    bool deny();
    bool lock_altars(bool detour, int reach);
    bool lock_needed() const;
    void eg_init();
    void eg_observe();
    bool enemy_locked(size_t i) const;
    bool break_lock(vec2 a);
    bool hold_tile();
    bool contest(vec2 p, int left, float hp, const std::vector<vec2>& free_foes);
    int choose(int& wait);
    void farm(int group);
    bool hit_group(int group);
    void spells_on_group(int group);
    void reset_hit(int group);
    bool escape_camps();

    // Camp farming (see the camps section of bot.cpp).
    struct ClearCost { float hp_sum = -1, base = 0, peak = 0; };
    std::vector<std::vector<std::uint8_t>> threat_;  // [group][m * n + c]: camp m reaches us while we hit c
    std::vector<ClearCost> cost_;
    int my_range_ = 1;
    std::vector<int> bank_d_;           // walk from each group to the nearest altar
    int raid_n_ = 1;                    // champions the current target's plan counts on
    bool rally_need_ = false;           // ... gathering outside its reach first
    int rally_g_ = -1, rally_since_ = -1;
    cambc::bitboard rally_goal_;
    int rally_goal_g_ = -1, rally_goal_round_ = -100;
    bool goal_plain_ = false;           // goal_ is every tile in range, not just the safest
    int plain_until_ = -1;
    int static_off_until_ = -1;         // walk routed (not down the cached field) until then
    std::vector<std::uint8_t> goal_buf_;

    void build_threats();
    const ClearCost& clear_cost(int group);
    float estimate_damage(int group);
    float remaining(int group) const;
    const cambc::bitboard& approach_goal(int group, int r);
    const cambc::bitboard& rally_goal(int group);
    bool walk_static(int group, int far);
    bool group_awake(int group) const;
    int raiders_near(int group) const;
    int eta(int group) const;

public:
    void play_turn();
};
