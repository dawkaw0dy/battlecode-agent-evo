#pragma once
#include <cambc.h>
#include <array>
#include <cstdint>
#include <vector>
#include "plan.h"

// What one champion remembers about a camp group.
struct Known {
    enum State : std::uint8_t { unknown, alive, reward, respawning } state = unknown;
    int ready = 0;           // round the group respawns (respawning only)
    std::vector<float> hp;   // last seen health per camp (camps do not regenerate)
    int blocked_until = 0;   // capture never completed (contested): avoid until then
};

// Row-major local copies: every bitboard or field query is a host call.
struct Grid8 {
    std::vector<std::uint8_t> v;  // bit-packed
    void load(const cambc::bitboard& b);
    bool at(cambc::vec2 p) const;
};
struct Grid16 {
    std::vector<std::uint16_t> v;  // 65535 = unreached
    void load(const cambc::distance_field& f);
    int at(cambc::vec2 p) const;
    bool empty() const { return v.empty(); }
};

struct Ally { int slot; cambc::vec2 pos; bool alive; float hp_frac; };

class Bot {
    bool ready_ = false;
    int slot_ = -1, team_size_ = 1;
    std::array<int, 5> ally_target_{-1, -1, -1, -1, -1};
    std::vector<int> pick_slot_;          // draft index -> slot
    bool dasher_ = false;
    const cambc::bitboard* walls_ = nullptr;
    std::vector<GroupInfo> groups_;
    std::vector<cambc::map::camp_group> handles_;
    std::vector<std::uint8_t> owner_;
    std::vector<Known> known_;
    std::vector<Grid16> group_field_;
    std::vector<int> field_order_;
    std::vector<std::int16_t> group_at_;   // tile -> group of the camp spawned there
    std::vector<std::int8_t> kind_at_;     // tile -> camp_type spawned there
    Grid16 home_, altar_;
    cambc::bitboard altar_tiles_;
    std::vector<Ally> allies_;

    int target_ = -1, last_choice_ = -100;
    int stage_since_ = -1, stage_group_ = -1;
    int capture_group_ = -1, capture_since_ = 0;
    int last_captured_seen_ = 0;
    int spawn_wait_ = 0;
    int dynamic_until_ = 0;
    int hold_start_ = 0, last_hold_ = -10;
    bool cant_spend_ = false;

    // Per-turn state.
    bool moved_ = false, attacked_ = false, cast_ = false, move_failed_ = false;
    float incoming_ = 0, taken_mult_ = 1, dps_ = 50;
    int exit_steps_ = 0;

    void initialise();
    void fill_fields(std::uint64_t reserve);
    void update_knowledge();
    void read_allies();
    void buy_items();
    bool walk_field(const Grid16& field, bool allow_dash);
    bool step_to(const cambc::bitboard& goals, bool allow_dash);
    bool try_dash(const Grid16* local, const cambc::distance_field* field, const cambc::bitboard* reached);
    bool hold_for_dash();
    void go_home();
    bool attack_group(int group);
    bool reset_and_hit(int group);
    void cast_spells(int group);
    bool heal_ally();
    bool handle_enemies();
    bool handle_tribute();
    bool disengage();
    void squad_target();
    int choose_target();
    int eta(int group) const;
    cambc::bitboard tiles_of(int group) const;
    float my_dps() const;
    int helpers(int group) const;
    float estimate_damage(int group, float dps) const;
    float remaining_fight_hp(int group) const;
    int squad_near(int group) const;
    void farm(int group);
    void report_enemy(cambc::vec2 where, int count);
    void note_capture(int group);

public:
    void play_turn();
    void play_inner();
};
