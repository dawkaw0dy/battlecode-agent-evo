#pragma once
#include <cambc.h>
#include <cstdint>
#include <vector>

// One camp group with what we remember about it.
struct Group {
    std::vector<cambc::vec2> tiles;
    std::vector<int> types;
    std::vector<float> hp;      // last known health per camp; 0 means dead
    std::vector<float> maxhp;
    std::vector<float> str;
    std::vector<int> range;
    int gems = 0, gold = 0, respawn = 0;
    int state = 0;              // 0 alive (assumed), 2 reward available, 3 respawning
    int respawn_at = 0;         // expected respawn round while state == 3
    int seen = -1000;           // last round a tile of the group was visible
    int home = 0, enemy_home = 0; // walking distance from each spawn
    int owner = -1;              // champion index whose sector this is
    bool boss = false;
};

// One ability slot, read once from the engine.
struct Kit {
    bool used = false;
    cambc::ability_kind kind{};
    float range = 0;
    bool excl = false, camps = false, enemies = false, ground = false;
};

struct Threat {
    cambc::vec2 pos;
    float str;
    int range;
};

struct Foe {
    cambc::vec2 pos;
    float hp, maxhp, dps;
    int range;
    std::uint32_t gems;
    bool recalling, tributing;
};

class Bot {
public:
    void play_turn();

private:
    bool inited = false;
    int W = 0, H = 0;
    std::uint32_t turn_round = 0;
    int my_index = 0;
    bool ranged = false;
    std::vector<Group> groups;
    std::vector<cambc::map::camp_group> map_groups;
    std::vector<cambc::vec2> altars;
    std::vector<std::uint16_t> home_dist, enemy_dist, field_buf;
    std::vector<std::uint8_t> vis_buf;
    cambc::bitboard static_block, altar_goal;
    Kit kit[2];
    int reset_slot = -1, shield_slot = -1;

    // Per-turn state.
    cambc::vec2 pos{};
    bool moved = false, move_failed = false, attacked = false;
    std::vector<Foe> foes;
    std::vector<Threat> threats;
    std::vector<cambc::vec2> ally_pos;
    std::vector<float> ally_hp, ally_dps;
    cambc::bitboard visible, walls, danger;
    bool any_danger = false;

    // Plans.
    int target = -1;
    int target_radius = 0;
    int stuck_turns = 0;
    int heal_wait = 0;
    int bought = 0;
    int tribute_ban = 0, trib_best = 1 << 20, trib_since = 0, trib_last = -100;
    int modes[12] = {};
    int flee_until = -1;
    cambc::vec2 last_enemy{-1, -1};
    int last_enemy_round = -1000;

    void init();
    void assign_sectors(int n);
    void dbg(const char* what);
    void mark(const char* what);
    void buy_items();
    void read_kit();
    void observe();
    bool handle_enemies();
    bool fight(const std::vector<int>& near);
    void flee();
    bool want_tribute();
    bool go_tribute();
    bool maybe_recall();
    void farm();
    int choose_target();
    bool attack_camps(int g);
    bool step_towards(const cambc::bitboard& goals, bool enter_goals = false);
    bool step_inner(const cambc::bitboard& goals);
    bool step_core(const cambc::bitboard& goals);
    bool step_away_from_camps();
    bool cast_on_camp(const cambc::camp& c);
    bool cast_on_champion(const cambc::champion& c, bool kill_only);
    void after_hit(const cambc::unit& target);
    float incoming_at(cambc::vec2 p) const;
    float my_dps() const;
    int hd(cambc::vec2 p) const;
    int ed(cambc::vec2 p) const;
    bool fresh() const;
    void box(cambc::bitboard& b, cambc::vec2 c, int r) const;
};
