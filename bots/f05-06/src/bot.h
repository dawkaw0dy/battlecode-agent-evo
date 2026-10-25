#pragma once
// Split-farm champion: farms its own region alone, tributes at its home altar, regroups only on invasions.
#include <cambc.h>
#include <array>
#include <cstdint>
#include <optional>
#include <vector>
#include "common.h"

struct Camp {
    vec2 pos;
    int kind;
    float maxhp, str, hp;
    int range;
    std::vector<int> threat;  // camps of the group that can hit someone attacking this one
};

struct Group {
    std::vector<vec2> tiles;
    std::vector<Camp> camps;
    vec2 center;
    int gems = 0, gold = 0, respawn = 0;
    int owner = dm::kShared;
    int status = 0;          // 0 believed alive, 1 reward waiting, 2 respawning
    int respawn_at = 0;      // round the group is expected back (status 2)
    int seen = -1000;        // last round any spawn tile was in vision
    bool dirty = true;       // clear plan must be recomputed
    bool threats = false;    // threat lists built
    float plan_time = 0, plan_dmg = 0;
    int my_hit = -1000;      // last round we hit one of its camps
    std::uint16_t dspawn = kFar;  // walking distance from our spawn
    std::uint16_t dthem = kFar;
};

struct Foe {
    vec2 pos;
    float hp, maxhp, str, focus;
    bool channel, ranged;
    int range;
};

class Bot {
public:
    void play_turn();

private:
    // ---- static map knowledge
    bool init_ = false;
    std::vector<cambc::map::camp_group> handles_;
    std::vector<Group> groups_;
    std::vector<std::int16_t> tile_group_;  // W*H, group index of a camp spawn tile, -1 otherwise
    std::vector<std::int16_t> tile_camp_;   // index within the group
    std::vector<vec2> altars_;
    std::vector<vec2> spawn_;
    cambc::bitboard static_blocked_;
    cambc::bitboard spawn_board_;
    vec2 spawn_center_{0, 0};
    vec2 espawn_center_{0, 0};
    int slot_ = -1;
    int home_altar_ = -1;
    int n_slots_ = 0;
    int total_gems_ = 0;
    bool ranged_ = false;
    int range_ = 1;
    int kind_ = 0;

    // ---- per-turn
    int round_ = 0;
    vec2 pos_{0, 0};
    bool moved_ = false, attacked_ = false, cast_ = false, blocked_move_ = false;
    std::vector<Foe> foes_, friends_;
    std::vector<vec2> prev_foes_;
    int foes_round_ = -10, prev_foes_round_ = -10;
    vec2 last_pos_{-1, -1}, cur_pos_{-1, -1};
    bool avoid_foes_ = false;
    bool hold_spawn_ = false;
    int osc_ = 0, banned_ = -1, banned_until_ = -1;
    cambc::bitboard vis_, camps_now_, units_;
    float incoming_ = 0;  // camp damage that can hit our tile next round (estimate)
    float incoming_other_ = 0;  // part of it from camps outside the target group
    struct Awake { vec2 pos; int range; int group; int seen; };
    std::vector<Awake> awake_;

    // ---- plans
    int target_ = -1;
    int target_round_ = -1000;
    int retreat_until_ = -1;     // retreat from camps (then recall)
    int focus_camp_ = -1;
    int approach_shrink_ = 0;    // reduce approach radius when no line of sight
    int stuck_ = 0;
    int last_hp_round_ = -1;
    float last_hp_ = 0;
    float dmg_taken_ = 0;
    int mode_ = 0;
    std::array<int, 16> act_{};
    std::array<double, 8> prof_{};
    std::uint64_t pf_ = 0;
    void lap(int i) { const auto f = cambc::fuel_remaining(); prof_[i] += double(pf_ - f); pf_ = f; }
    int flee_until_ = -1;
    vec2 flee_from_{-100, -100};
    int avoid_until_ = -1;
    vec2 avoid_pos_{-100, -100};
    int fight_round_ = -1000;
    int shards_ = 0;
    int heal_wait_ = 0;
    std::vector<int> enemy_lock_seen_;  // per altar: last round an enemy lock was seen there

    cambc::distance_field my_field_;
    cambc::bitboard my_visited_;
    int my_field_round_ = -1000;
    vec2 my_field_pos_{-1, -1};

    // ---- steps
    void initialize();
    void observe();
    void update_group_from_vision(int gi);
    void plan_group(Group& g);
    bool live() const;
    bool stunned() const;

    void shop();
    std::optional<cambc::item_type> next_item() const;
    float my_dps_camps() const;

    bool fight_or_flee();
    bool engage(const std::vector<const Foe*>& near);
    bool tribute();
    void farm();
    int choose_target();
    void ensure_my_field();

    bool go(const cambc::bitboard& goals, bool allow_dash);
    bool go_tile(vec2 t, int radius, bool allow_dash);
    bool step_away_from_camps(bool others_only = false);
    void add_danger(cambc::bitboard& walls) const;
    bool attack_camp_in_range();
    bool attack_unit(const cambc::unit& u);
    void after_attack();
    bool cast_on_camp(const cambc::camp& c);
    void maybe_shield();
    bool try_recall(int why = 0);
    std::array<int, 8> recalls_{};
    void try_ward_bush();
    bool on_spawn() const;

    // shared memory sighting: x, y, count, round&255
    void report(vec2 p, int count, bool fighting);
    bool read_report(vec2& p, int& count, int& age, bool* fighting = nullptr) const;
    void claim(int group);
    std::array<int, 5> claims() const;
};
