#pragma once
#include <cambc.h>
#include <cstdint>
#include <vector>
#include "plan.h"

// What one champion remembers about a camp group it cannot always see.
struct Known {
    enum State : std::uint8_t { unknown, alive, reward, respawning } state = unknown;
    int ready = 0;      // round the group respawns (respawning only)
    int seen = -1000;   // last round its state was read
    std::vector<float> hp;  // last seen health per camp (camps do not regenerate)
    int blocked_until = 0;  // a capture that never completes (contested): leave it alone until then
    mutable float est_dps = -1, est_mult = -1, est_hp_sum = -1, est_dmg = 0;  // estimate_damage cache
};

// Row-major local copies: every bitboard or field query is a host call, so loops read these.
struct Grid8 {
    std::vector<std::uint8_t> v;  // bit-packed, tile i is bit (i & 7) of byte i >> 3
    void load(const cambc::bitboard& b);
    bool at(cambc::vec2 p) const;
};
struct Grid16 {
    std::vector<std::uint16_t> v;  // 65535 = unreached
    void load(const cambc::distance_field& f);
    int at(cambc::vec2 p) const;
    bool empty() const { return v.empty(); }
};

// Per-champion state survives between turns.
class Bot {
    bool ready_ = false;
    int slot_ = -1;                       // index into the draft's pick order, -1 if unknown
    bool dasher_ = false;                 // has a dash: one-tile holes are passable
    const cambc::bitboard* walls_ = nullptr;
    std::vector<GroupData> groups_;
    std::vector<cambc::map::camp_group> handles_;
    std::vector<std::uint8_t> owner_;
    std::vector<Known> known_;
    std::vector<Grid16> group_field_;     // walking distance to each group's camp tiles, filled lazily
    std::vector<int> field_order_;
    Grid16 home_, altar_;
    cambc::bitboard altar_tiles_;          // where we can stand to tribute
    std::vector<cambc::vec2> altars_;
    std::vector<Grid16> altar_field_;      // walking distance to the tribute tiles of each altar, filled lazily
    std::vector<bool> locked_;             // an enemy lock was last seen on this altar (shared memory bytes 6-7)
    int target_ = -1, target_wait_ = 0, last_choice_ = -100;
    int approach_shrink_ = 0;
    int capture_group_ = -1, capture_since_ = 0;
    int dynamic_until_ = 0;
    int hold_start_ = 0, last_hold_ = -10;
    bool cant_spend_ = false;             // the shop refused our last purchase
    int spawn_wait_ = 0;                  // turns spent waiting on spawn to heal
    int gap_start_ = 0, gap_last_ = -10;  // waiting for a champion to clear a gap
    cambc::vec2 pos_{};                   // our position, refreshed before position-dependent planning
    std::vector<cambc::bitboard> group_tiles_;   // camp spawn tiles per group
    std::vector<std::int8_t> camp_kind_at_;      // tile -> camp_type spawned there, -1 if none
    std::vector<std::int16_t> camp_group_at_;  // tile -> group index of the camp spawned there, -1 if none
    int blocker_group_ = -1, blocker_since_ = 0;  // living camps walling us in: clear them
    float last_hp_ = 0;                   // health at the start of our previous turn
    int last_hp_round_ = -10, threat_until_ = -1;  // hit by an unseen enemy: walk home until then

    // Per-turn flags.
    bool moved_ = false, attacked_ = false, cast_ = false, move_failed_ = false;
    float incoming_ = 0;                  // strength of awake camps that can hit us
    int exit_steps_ = 0;                  // steps to get out of their range
    float taken_mult_ = 1.0f, dps_ = 50.0f;

    void initialise();
    void fill_fields(std::uint64_t reserve);
    void update_knowledge();
    void update_locks();
    void buy_items();
    bool walk_field(const Grid16& field, bool allow_dash);
    bool step_to(const cambc::bitboard& goals, bool allow_dash);
    bool try_dash(const Grid16* local, const cambc::distance_field* field, const cambc::bitboard* reached);
    bool attack_group(int group);
    void cast_spells(int group);
    bool reset_and_hit(int group);
    bool handle_enemies();
    bool handle_tribute(int next_target, int next_wait);
    int choose_target(int& wait_out);
    int eta(int group) const;
    void farm(int group);
    void go_home();
    bool hold_for_dash();
    float my_dps() const;
    float estimate_damage(int group, float dps) const;
    float remaining_hp(int group) const;
    bool disengage_from_camps();
    void report_enemy(cambc::vec2 where, int count);

public:
    void play_turn();
};
