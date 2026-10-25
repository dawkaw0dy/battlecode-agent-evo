#pragma once
#include <cambc.h>
#include <cstdint>
#include <vector>

// One camp group as the bot remembers it.
struct Group {
    std::vector<cambc::vec2> tiles;  // original camp spawn tiles (camps never move)
    cambc::vec2 center{};
    int gems = 0, gold = 0;
    float hp = 0, str = 0;           // design totals
    float known_hp = -1;             // last seen remaining health (bosses keep damage), -1 unknown
    int range = 3;
    bool boss = false, monolith = false;
    int respawn_at = 0;              // round the group is alive again (0: alive or unknown)
    bool reward_ready = false;
    int max_respawn = 60;
    int last_seen = -1000;
};

// Per-champion state survives between turns.
class Bot {
    bool ready_ = false;
    int slot_ = 0, team_n_ = 1;
    bool ranged_ = false;
    std::vector<cambc::map::camp_group> handles_;
    std::vector<Group> groups_;
    std::vector<cambc::vec2> altars_;
    cambc::vec2 home_{};
    int bought_ = 0;
    int round_ = 0;
    bool moved_ = false, move_failed_ = false, cast_excl_ = false;
    int approach_ = 3;          // ranged approach radius, shrinks when nothing can be hit
    int no_hit_turns_ = 0;
    int engage_since_ = -1;     // leader: round the staging for the current boss target started
    int last_target_ = -1;
    int last_scan_ = -100;
    std::uint8_t mem_[8]{};

    void init();
    void shop();
    void scan_groups(bool all);
    void lead();
    bool step_to(const cambc::bitboard& goals);
    bool step_away(cambc::vec2 from, int want);
    void box(cambc::bitboard& b, cambc::vec2 p, int r) const;
    bool fight_enemies(bool& flee);
    bool attack_unit_camp(cambc::vec2 p);
    void cast_at_camp(cambc::vec2 p, bool boss);
    void after_attack_reset(cambc::vec2 p, bool camp);
    void support_casts(float incoming);
    bool bank();
    bool work_target(int t, bool engage);
    float incoming_damage(bool& boss_awake);
    void write_status(bool alive, bool near, bool busy);

public:
    void play_turn();
};
