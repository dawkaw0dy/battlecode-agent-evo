#pragma once
#include <cambc.h>
#include <cstdint>
#include <vector>
#include "common.h"

// One camp as this champion remembers it (camps do not move or heal).
struct CampK {
    vec2 pos{};
    int group = -1;
    float max_hp = 1, hp = 1, str = 0;
    int range = 0;
    bool alive = true, awake = false;
    bool attacker() const { return str > 0; }
};

// One camp group: static data plus what this champion last knew about it.
struct GroupK {
    std::vector<vec2> tiles;  // spawn tiles
    std::vector<int> camps;   // indices into Bot::c_
    int gems = 0, gold = 0, respawn = 60;
    vec2 center{};
    int reach = 0;            // largest camp attack range
    float total_hp = 0, total_str = 0;
    enum State : std::uint8_t { alive, reward, respawning } st = alive;
    int ready = 0;            // respawn round when respawning
    int seen = -1;            // last round any spawn tile was in vision
    int blocked_until = -1;   // given up on until this round
    bool plug = false;        // its spawn tiles are the only tribute tiles
    Field field;              // walking distance to the spawn tiles, filled lazily
};

// A visible champion, read once per turn.
struct Seen {
    vec2 pos{};
    float hp = 0, max_hp = 1, str = 0, focus = 0;
    int range = 1, gems = 0;
    bool melee = true, channel = false, tribute = false;
    float burst = 0;  // extra damage of its first hit (ready spells and attack resets)
    cambc::champion_type type{};
    float dps() const { return (str * 1.15f + focus * 0.2f) * (channel ? 0.3f : 1.0f); }
};

// A champion ability slot with the metadata we use.
struct Abil {
    int slot = 0;
    cambc::ability_kind kind{};
    float range = 0, radius = 0;
    bool excl = false;  // excludes walking
};

// An allied champion other than us.
struct Mate {
    cambc::champion h;
    int slot = 0;             // claim byte in shared memory
    float dps = 50, hp = 900;
    bool melee = true;
    cambc::champion_type type{};
};

class Bot {
    // Static knowledge.
    bool init_ = false;
    int slot_ = 0;              // our claim byte: place in the draft preference order among allies
    std::vector<Mate> mates_;
    int partner_ = -1;          // mate we follow (the healer follows the tank)
    std::vector<GroupK> g_;
    std::vector<cambc::map::camp_group> gh_;
    std::vector<CampK> c_;
    std::vector<std::int16_t> camp_at_;    // tile -> camp index
    cambc::bitboard block_;     // walls, holes, altars, enemy spawn
    cambc::bitboard opaque_;    // walls and bushes
    cambc::bitboard spawn_;     // our spawn tiles
    Field home_, altar_;
    std::vector<vec2> altars_;
    vec2 home_c_{}, enemy_c_{};
    std::vector<Abil> ab_;
    bool melee_ = true, healer_ = false;
    cambc::champion_type type_{};
    float base_dps_ = 50;

    // Per turn.
    int round_ = 0;
    vec2 pos_{};
    float hp_ = 1, max_hp_ = 1;
    int gems_ = 0, gold_ = 0;
    cambc::bitboard occ_;       // visible champions, camps and teleports
    cambc::bitboard vis_;
    std::vector<Seen> foes_, friends_;
    std::vector<bool> mate_alive_;
    std::uint8_t claims_[8]{};
    bool moved_ = false, attacked_ = false, cast_ = false, move_fail_ = false;
    vec2 sight_pos_{};
    int sight_age_ = 999;
    bool sight_rich_ = false, sight_many_ = false;
    float camp_in_ = 0;         // damage per turn awake camps can deal to us here

    // Memory.
    int target_ = -1, target_at_ = -100;
    int shrink_ = 0, shrink_g_ = -1, no_hit_ = 0;
    int last_hit_round_ = -100;
    int hunt_until_ = -1;
    int ward_try_ = 0;
    int detour_until_ = -1;
    int bank_stuck_ = 0, bank_block_until_ = -1;
    int prev_round_ = -10, champ_hit_round_ = -100;
    float prev_hp_ = 0, prev_camp_in_ = 0;
    int wait_g_ = -1, wait_turns_ = 0;
    int mode_ = 0, sub_ = 0;
    int acc_[16]{};

    void initialise();
    void observe();
    void shop();
    void support_spells();
    bool lock_tributes();
    bool fight();
    bool hunt();
    bool retreat_step();
    bool low_health();
    bool want_bank() const;
    bool shop_trip();
    bool bank();
    void farm();
    void ward();

    // Targets.
    void claim(int g);
    int choose_target();
    float score(int g) const;
    float clear_trips(int g, float dps, float tank, float others_hp, float heal, float& t) const;
    void work(int g);
    int pickup();

    // Movement and actions.
    bool free_tile(vec2 t) const;
    bool step_down(const Field& f);
    bool move_to(vec2 to);
    bool approach(vec2 target, int r, bool los);
    bool go_group(int g);
    bool go_home();
    bool dash_towards(const Field& f);
    bool dash_towards_tile(vec2 target);
    bool dash_path(vec2 t) const;
    bool attack_camps(int g);
    bool attack_foe(const Seen* s);
    void reset_attack();
    bool cast_on_camp(int g);
    bool cast_on_foe(const Seen& s);
    int group_dist(int g);
    void ensure_field(int g);
    void write_sighting();

public:
    void play_turn();
};
