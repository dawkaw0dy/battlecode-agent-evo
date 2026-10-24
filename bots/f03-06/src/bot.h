#pragma once
#include <cambc.h>
#include <cstdint>
#include <vector>
#include "common.h"
#include "plan.h"

// What one champion remembers about a camp group it cannot always see.
struct Known {
    enum State : std::uint8_t { alive, reward, respawning } st = alive;
    int ready = 0;     // respawn round (respawning only)
    int seen = -1000;  // last round the group was in view
    int fail = 0;      // round until which we avoid it (stuck or contested)
};

// One visible champion.
struct Unit {
    vec2 pos;
    float hp, max_hp, str, focus, power;
    int range;
    bool recalling, tributing, on_spawn;
    std::uint32_t gems;
    int slot = -1, squad = -1;
    bool fresh = true;  // in sight this turn (else remembered)
    int seen = -1000;
};

// One ability slot, read once from the engine.
struct Slot {
    cambc::ability_kind kind{};
    bool used = false, excl = false, camps = false, enemies = false;
    float range = 0, mod = 0;
};

class Bot {
    // Static knowledge.
    bool init_ = false;
    int slot_ = 0, bodies_ = 1, squad_ = 0, nsquads_ = 1, squad_n_ = 1;
    std::vector<cambc::champion_draft> picks_;
    std::vector<int> ally_slot_, ally_squad_;
    std::uint8_t squad_of_[6]{};
    Plan plan_ = kFarm;
    int boss_ = -1;
    cambc::champion_type type_{};
    std::vector<GroupInfo> g_;
    std::vector<cambc::map::camp_group> h_;
    std::vector<std::uint8_t> owner_;
    std::vector<int> goff_;               // offset of each group's camps in chp_
    std::vector<float> chp_;              // last seen health per camp
    std::vector<std::uint8_t> cawake_;    // last seen awake flag per camp
    std::vector<std::int16_t> group_at_;  // tile -> group spawned there
    std::vector<Field> gf_;               // walking distance to each group's camps
    std::vector<std::uint8_t> gf_tried_;
    Field home_, altar_;
    bool altar_plugged_ = false;
    std::vector<vec2> altar_plug_tiles_;  // camp tiles that are the only way next to an altar
    int raid_cand_ = -1;                  // boss group the team raids once strong enough
    bool raid_on_ = false;
    bool loose_ = true;                   // squad members may farm cheap neighbouring groups
    std::vector<int> d_home_;             // walking distance from our spawn to each group
    Bits terrain_, campspawn_, spawn_, hard_;
    cambc::bitboard walls_camps_;         // static walls + all camp spawn tiles
    std::vector<vec2> altars_;
    std::vector<cambc::champion> allies_;
    Slot kit_[2];
    int reset_slot_ = -1, dash_slot_ = -1;

    // Memory.
    std::vector<Known> k_;
    int target_ = -1, target_since_ = 0;
    int stuck_ = 0, last_dist_ = kFar;
    int chase_ = 0;
    int gather_ = 0, gather_g_ = -1;
    int last_hit_round_ = -100;
    int flee_until_ = -1;
    vec2 last_pos_{-1, -1};

    // Per turn.
    vec2 pos_{};
    int now_ = 0;
    bool moved_ = false, attacked_ = false, cast_ = false, excl_cast_ = false, move_failed_ = false;
    Bits vis_, campocc_, champocc_;
    std::vector<Unit> foes_, friends_, ally_mem_;
    int fight_until_ = -1;
    float taken_ = 1.0f, dealt_ = 1.0f;

    void initialise();
    void compute_group_field(int g);
    void observe();
    void read_units();
    void shop();
    void write_target(int g);
    int read_target() const;
    bool leader() const;
    float squad_dps() const;
    int needed(int g, int avail) const;

    bool free_tile(vec2 p) const;
    bool step(const Field& f, int stop_at = 0);
    bool step_dynamic(const cambc::bitboard& roots);
    bool step_away(const std::vector<vec2>& threats, const std::vector<int>& ranges);
    bool try_dash(const Field& f);

    bool fight();
    bool hurt();
    bool tribute();
    void farm();
    int choose(int& eta_out, int near_group = -1);
    void engage(int g);
    bool hit_group(int g);
    void spells_on_group(int g, bool before_move);
    void after_attack(int g, const Unit* foe);
    bool attack_foe(const Unit& u);
    void spells_on_foe(const Unit& u, bool before_move);
    void defensive_spells(float incoming);
    void heal_allies();
    void update_raid();
    bool raid_ready() const;

    float dps() const;
    float incoming_camp_damage() const;
    float group_remaining(int g) const;
    int dist_to_group(int g) const;
    int here(const Field& f) const;  // field value at our tile, through a neighbour if ours reads as a wall
    bool on_group_tile(int g) const;

    void turn();

public:
    void play_turn();
};
