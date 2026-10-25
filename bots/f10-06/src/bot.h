#pragma once
#include <cambc.h>
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

using cambc::vec2;
using cambc::team;

// Our draft order; a champion's slot is its index here.
inline const std::array<cambc::champion_draft, 5>& pick_order() {
    static const std::array<cambc::champion_draft, 5> order{cambc::champion_draft::vendetta,
        cambc::champion_draft::ryth, cambc::champion_draft::willow, cambc::champion_draft::vesper_vale,
        cambc::champion_draft::marcella};
    return order;
}

// What one champion remembers about a camp group.
struct Group {
    std::vector<vec2> tiles;      // camp spawn tiles
    std::vector<int> kind;        // camp_type per camp
    std::vector<float> hp;        // last known health per camp (camps do not heal)
    vec2 center{};
    float max_hp = 0;
    int gems = 0, gold = 0, respawn = 0;
    bool big = false, mono = false;
    enum State { alive, reward, respawning } state = alive;
    int ready = 0;                // round it is back (respawning)
    int enemy_seen = -1000;       // last round an enemy champion was seen near it
    float hp_left() const { float s = 0; for (float h : hp) s += h; return s; }
    float str_left() const;
};

struct Foe {
    vec2 pos{};
    float hp = 0, max_hp = 1, str = 0, dmg = 0;
    int range = 1, gems = 0;
    bool ranged = false;
};

class Bot {
    bool init_ = false;
    int slot_ = 0, size_ = 5;
    std::vector<Group> g_;
    std::vector<vec2> altars_;
    cambc::bitboard static_block_;   // walls, holes, altars, enemy spawn
    cambc::bitboard block_;          // this turn's blocked tiles (without our own)
    cambc::bitboard spawn_;
    vec2 spawn_center_{};
    std::unique_ptr<cambc::distance_field> field_;
    std::unique_ptr<cambc::distance_field> path_;
    int path_round_ = -100;
    vec2 path_from_{};

    // Memory.
    int target_ = -1;
    int target_since_ = 0;
    int no_hit_turns_ = 0;
    int shrink_ = 0;
    int last_enemy_ = -1000;
    int build_ = 0;               // shop plan
    int rally_since_ = -1;
    int fleeing_until_ = -1;
    float last_hp_ = -1;
    int last_round_ = -10;
    int ambush_until_ = -100;

    // Per turn.
    vec2 pos_{};
    int now_ = 0;
    bool moved_ = false, attacked_ = false, cast_ = false, move_failed_ = false;
    float hp_ = 0, max_hp_ = 1;
    float camp_dps_ = 50;
    std::vector<Foe> foes_;
    std::vector<std::pair<vec2, float>> allies_;   // visible allies: position, health fraction
    cambc::bitboard vis_;
    std::array<std::uint8_t, 8> mem_{};
    struct Awake { vec2 pos; int range; float str; };
    std::vector<Awake> awake_;

    void turn();
    bool alive_prev_ = true;
    std::array<std::array<int, 7>, 6> hist_{};
    int hist_i_ = 0;
    void initialise();
    void observe();
    void read_units();
    void shop();
    bool go(const cambc::bitboard& goals, bool allow_dash);
    bool go_to(vec2 t, int r);
    bool flee();
    bool fight();
    bool bank();
    int choose();
    int raid_target();
    void farm(int gi);
    bool attack_group(int gi, bool any_awake);
    bool attack_foe();
    bool answer_call();
    void reveal();
    void spells(bool staying, int gi);
    bool use_reset(int gi);
    bool in_range_of_awake(vec2 p) const;
    float danger_at(vec2 p) const;
    float foe_pressure(vec2 p) const;
    int claims(int gi) const;
    void claim(int gi);
    cambc::bitboard approach(int gi, int r) const;

public:
    void play_turn();
};
