#pragma once
#include <cambc.h>
#include <cstdint>
#include <vector>
#include "common.h"

// Static facts about one camp group.
struct Group {
    std::vector<vec2> tiles;
    std::vector<cambc::camp_type> kinds;
    vec2 center{};
    int gems = 0, gold = 0, respawn = 0, reach = 0, range = 0;
    float hp = 0, str = 0;
    bool boss = false, monolith = false;
    int d_home = kFar, d_enemy = kFar;  // walking distance from each spawn
};

// What this champion believes about a group it cannot always see.
struct Known {
    enum State : std::uint8_t { unknown, alive, reward, respawning } state = unknown;
    int ready = 0;           // respawn round
    std::vector<float> hp;   // last known health per camp (camps never heal)
    int skip_until = -1;     // given up on (contested or stuck) until this round
};

struct Unit { vec2 pos; float hp, max_hp, str, focus; int range; bool melee; };

class Bot {
    bool init_ = false;
    int slot_ = 0;
    bool dasher_ = false, ranged_ = false;
    cambc::champion_type type_{};
    std::vector<Group> g_;
    std::vector<cambc::map::camp_group> h_;
    std::vector<cambc::bitboard> gtiles_;
    std::vector<std::int16_t> group_at_, camp_at_;
    std::vector<Known> k_;
    Dist home_, altar_;
    cambc::bitboard altar_spots_;
    Bits walls_l_, hard_l_;

    // Memory.
    int target_ = -1, chosen_at_ = -100, target_since_ = 0;
    int shrink_ = 0, shrink_g_ = -1, no_hit_turns_ = 0;
    int last_enemy_ = -1000;
    int flee_until_ = -1;
    bool shop_trip_ = false;
    int best_eta_ = kFar, stuck_since_ = 0;

    // Per turn.
    std::uint32_t round_ = 0;
    vec2 pos_{};
    bool moved_ = false, attacked_ = false, cast_ = false, move_failed_ = false;
    float incoming_ = 0, taken_ = 1, dps_ = 50;
    cambc::bitboard vis_, camps_, champs_, awake_;
    Bits lvis_, lcamps_, lchamps_;
    std::vector<Unit> foes_, allies_;

    void initialise();
    void refresh_view();
    void observe();
    void measure();
    void shop();

    bool live() const;
    bool fuel(std::uint64_t need) const;
    bool free_tile(vec2 p) const;
    bool go(const cambc::bitboard& goals, bool dash);
    bool try_dash(const cambc::distance_field& f, const cambc::bitboard& seen);
    bool go_home_or_recall(int walk_limit);
    bool escape_camps();

    bool fight();
    bool attack_enemy(const std::vector<Unit>& list);
    bool spell_enemy(const std::vector<Unit>& list);
    void reset_attack();
    bool tribute();
    int choose();
    void farm(int group);
    bool hit_group(int group);
    void spells_on_group(int group);
    float estimate_damage(int group) const;
    float remaining(int group) const;
    void write_claim(int group);
    int claims_on(int group) const;

public:
    void play_turn();
};
