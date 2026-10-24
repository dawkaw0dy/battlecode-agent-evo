#pragma once
#include <cambc.h>
#include <cstdint>
#include <vector>

// Cached metadata of one ability slot.
struct Spell {
    bool used = false;           // slot holds an ability
    cambc::ability_kind kind{};
    bool exclusive = false;      // excludes walking this turn
    bool enemies = false, camps = false, ground = false, self = false, allies = false;
    float range = 0, modifier = 0, radius = 0;
    int duration = 0;            // stun duration (cc / aoe / dash landing)
};

// A visible champion, read once per turn.
struct Seen {
    cambc::vec2 pos;
    float hp = 0, max_hp = 1, str = 0, focus = 0;
    int range = 1;
    bool stunned = false, self = false;
};

// Per-champion state survives between turns.
class Bot {
public:
    void play_turn();

private:
    bool initialised = false;
    Spell spells[2];
    int purchases = 0;
    int target_group = -1;
    int field_group = -2;        // group the cached target field belongs to
    cambc::distance_field target_field;
    cambc::bitboard target_visited;
    std::vector<int> respawn_until; // per group: round we expect it back (from observation)
    int last_enemy_round = -100;
    int stuck_turns = 0;
    cambc::vec2 last_pos{-1, -1};

    // Turn flags.
    bool moved = false, attacked = false, cast_done = false, move_failed = false;

    std::vector<Seen> allies, enemies;

    void init();
    void scan();
    void shop();
    void observe_groups();
    int choose_group();
    void sync_target();
    bool fight();
    void engage(const Seen& target);
    void flee();
    bool bank();
    void farm();

    bool step_field(const cambc::distance_field& field, const cambc::bitboard& visited, bool allow_equal);
    bool step_towards(cambc::vec2 goal, int max_dist);
    bool step_away();
    bool try_attack_champion(cambc::vec2 pos);
    bool attack_camps(bool use_spells);
    bool cast_on_champion(const Seen& target, bool before_move);
    bool try_reset_attack(cambc::vec2 pos, bool champion_target);
    bool dash_to(cambc::vec2 target);
    void support_spells();
};
