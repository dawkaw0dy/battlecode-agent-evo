#pragma once
#include <cambc.h>
#include <memory>
#include <vector>
#include "world.h"

struct Foe {
    vec2 pos;
    float hp, max_hp, str, focus;
    int range;
    int dist;
    bool channel;
};

struct Friend {
    vec2 pos;
    float hp, max_hp, str;
};

struct CampView {
    vec2 pos;
    float hp, str;
    int range;
    cambc::camp_type type;
};

// Per-champion state survives between turns.
class Bot {
    bool inited_ = false;
    int slot_ = -1;
    cambc::champion_type type_{};
    bool ranged_ = false;

    // Target group and its approach state.
    int target_ = -1;
    double target_score_ = 0;
    int target_round_ = -100;
    int reach_ = 3;
    int stall_ = 0;

    // Distance field from our position over static terrain.
    std::unique_ptr<cambc::distance_field> me_field_;
    int field_round_ = -100;
    vec2 field_origin_{-1, -1};

    std::unique_ptr<cambc::distance_field> travel_;
    int travel_key_ = -1000;

    int last_enemy_round_ = -1000;
    bool healing_ = false;
    bool jungle_ = false;

    // Per-turn context.
    int round_ = 0;
    vec2 pos_;
    float hp_ = 0, max_hp_ = 1, str_ = 0, focus_ = 0;
    int range_ = 1;
    int gems_ = 0;
    bool moved_ = false, exclusive_cast_ = false, attacked_ = false;
    std::vector<Foe> foes_;
    std::vector<Friend> friends_;
    cambc::bitboard blocked_;

    void init();
    void shop();
    void refresh_field();
    int field_at(vec2 p);
    int altar_distance();
    void scan_units();
    void compute_blocked();
    bool step_toward(const cambc::bitboard& goals, bool allow_dash);
    bool walk_field(const cambc::distance_field& field, bool allow_dash);
    bool travel(const cambc::bitboard& goals, int key, bool allow_dash);
    bool travel_on(const cambc::distance_field& field, const cambc::bitboard& goals, bool allow_dash);
    bool step_away(const cambc::bitboard& danger);

    bool handle_enemies();
    bool handle_heal();
    bool should_bank();
    bool bank();
    void farm();
    void choose_target();
    double score_group(int gi, int dist, int helpers);
    void fight_group(int gi);
    void capture_group(int gi);
    std::vector<CampView> visible_camps(int gi);
    bool camp_spells(const std::vector<CampView>& camps, bool exclusive_ok);
    bool attack_camp(const std::vector<CampView>& camps);
    void write_claim(int gi);
    int read_claims(int gi);
    float my_dps() const;
    float incoming_camp_damage();

public:
    void play_turn();
};
