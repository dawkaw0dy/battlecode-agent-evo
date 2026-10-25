#pragma once
#include <cambc.h>
#include <cstdint>
#include <vector>

// One camp of a group, with what this champion last saw of it.
struct CampMem {
    cambc::vec2 pos;
    float max_hp = 0, hp = 0, str = 0;
    int range = 3;
};

// A camp group: static layout plus this champion's memory of its state.
struct GroupMem {
    std::vector<int> camps;     // indices into Bot::camps
    std::vector<cambc::vec2> tiles;
    cambc::vec2 center;
    int gems = 0, gold = 0, respawn = 0;
    std::uint8_t owner = 0xFF;  // draft owner byte
    int state = 0;              // 0 alive (assumed), 1 reward waiting, 2 respawning
    int respawn_at = 0;         // round the group is expected back
    int seen = -1000;           // last round a spawn tile was in vision
    int home_dist = 999;        // walking distance from our spawn
    int enemy_dist = 999;       // walking distance from the enemy spawn
    int trips = 0;              // heal trips spent on the current life
    int travel = -1;            // walking distance from us at the last target choice
    int skip_until = 0;         // unreachable or stuck: leave it alone until then
    int danger_until = 0;       // we fled enemies here
};

// Packed row-major tile bits, read locally (one export per board, no per-tile host calls).
struct Bits {
    std::vector<std::uint8_t> b;
    void load(const cambc::bitboard& board) {
        b.resize(cambc::bitboard::linear_buffer_size());
        board.export_linear_buffer(b);
    }
    bool get(int i) const { return (b[i >> 3] >> (i & 7)) & 1; }
    void clear(int i) { b[i >> 3] &= static_cast<std::uint8_t>(~(1u << (i & 7))); }
};

struct Foe {
    cambc::vec2 pos;
    float hp, max_hp, str, power;
    int range;
    bool channel;
};

struct Ability {
    int slot = -1;
    cambc::ability_kind kind{};
    float range = 0, modifier = 0;
    bool exclusive = false, camps = false, enemies = false;
};

class Bot {
public:
    void play_turn();

private:
    bool ready = false;
    int slot = 0, slots = 1;
    cambc::vec2 home_altar{-1, -1};
    int width = 0, height = 0;
    std::vector<cambc::map::camp_group> raw;
    std::vector<CampMem> camps;
    std::vector<GroupMem> groups;
    std::vector<int> camp_at_tile;   // tile -> camp index, -1 none
    Bits blocked_static;  // walls, holes, altars, enemy spawn
    Bits our_spawn;
    Bits vis;   // this turn's visible tiles
    Bits occ;   // this turn's visible occupants
    float dps_now = 10, mult_now = 1;
    cambc::distance_field home_field;   // distance to our spawn
    cambc::bitboard home_seen;
    cambc::distance_field enemy_field;  // distance to the enemy spawn
    cambc::bitboard enemy_seen;
    bool enemy_field_ready = false;
    std::vector<Ability> abilities;
    std::vector<cambc::vec2> altars;
    std::vector<int> altar_ban;
    int trib_round = -100, trib_since = 0, trib_best = 999;
    cambc::vec2 prev_pos{-1, -1};

    // Per-turn state.
    int round = 0;
    cambc::vec2 pos;
    float hp = 0, max_hp = 1;
    bool ranged = false, moved = false, move_failed = false, cast = false, cast_exclusive = false;
    std::vector<Foe> enemies;
    std::vector<Foe> allies;
    std::vector<int> visible_camps;  // camp indices visible and alive
    float incoming = 0;              // camp damage per turn on our tile

    // Plans.
    int target = -1, target_round = -1000, target_since = 0;
    int retreating_until = -1;
    int fleeing_until = -1;
    int spawn_wait = 0;
    int build_step = 0;
    int last_enemy_seen = -1000;
    int last_hit_round = 0;
    int report_count = 0, report_age = 999;
    cambc::vec2 report_pos{-100, -100};

    bool travel_ok = false;
    int stat[12] = {}, mstat[8] = {}, purpose = 0;
    void play_inner();
    void init();
    void init_fields();
    int init_stage = 0;
    void scan();
    void update_groups();
    void read_report(int& rx, int& ry, int& count, int& age);
    void write_report();
    void claim(int group);

    bool on_our_spawn(cambc::vec2 p) const;
    bool in_bounds(cambc::vec2 p) const { return p.x >= 0 && p.y >= 0 && p.x < width && p.y < height; }
    int idx(cambc::vec2 p) const { return p.y * width + p.x; }
    bool free_tile(cambc::vec2 p) const;
    float danger_at(cambc::vec2 p) const;
    float my_dps() const;
    float camp_mult() const;

    void buy_items();
    bool step_with(const cambc::distance_field& field, const cambc::bitboard& seen, bool away_from_enemies);
    bool route_to(const cambc::bitboard& goals);
    bool flee_home();
    bool escape_camps();
    bool hole_jump(const cambc::distance_field& field, const cambc::bitboard& seen);
    bool has_dash = false, dash_map = false;
    cambc::bitboard wide_holes;
    cambc::vec2 last_pos{-1, -1};
    int last_progress = 0;
    bool try_dash(const cambc::distance_field& field, const cambc::bitboard& seen);
    bool attack_camps(int group);
    bool attack_enemies();
    bool use_reset_and_hit();
    void spells_on_camp(const cambc::camp& c);
    void spells_on_enemy();
    void shield_if_hit();
    bool tribute(bool force);
    bool fight_or_flight();
    bool farm();
    bool answer_report();
    int choose_target();
    double group_score(int g, double& trips_out, double floor);
};
