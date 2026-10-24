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
    int target_ = -1, target_wait_ = 0, last_choice_ = -100;
    int approach_shrink_ = 0;

    // Per-turn flags.
    bool moved_ = false, attacked_ = false, cast_ = false, move_failed_ = false;
    float incoming_ = 0;                  // strength of awake camps that can hit us
    float taken_mult_ = 1.0f, dps_ = 50.0f;

    void initialise();
    void fill_fields(std::uint64_t reserve);
    void update_knowledge();
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
    float my_dps() const;
    float estimate_damage(int group, float dps) const;
    bool disengage_from_camps();
    void report_enemy(cambc::vec2 where, int count);

public:
    void play_turn();
};
