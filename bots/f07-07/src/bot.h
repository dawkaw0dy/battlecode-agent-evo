#pragma once
#include <cambc.h>
#include <array>
#include <cstdint>
#include <vector>

// A visible champion, read once per turn.
struct Body {
    cambc::vec2 pos;
    float hp = 0, max_hp = 1, strength = 0, focus = 0;
    float dps = 0;          // estimated damage per turn, spells averaged over their cooldown
    float burst = 0;        // damage it can deal this turn with ready resources (enemies: guessed)
    int range = 1;
    int type = 0;
    unsigned id = 0;
    bool melee = true, stunned = false, channelling = false;
    bool on_spawn = false;  // standing on its own spawn (heals fully, untouchable for us)
};

// A champion remembered for a few rounds after it leaves our sight.
struct Memory {
    Body body;
    int round = -1000;
};

// A visible camp monster.
struct CampSeen {
    cambc::vec2 pos;
    float hp = 0, max_hp = 1, strength = 0;
    int range = 3;
    int group = -1;
    bool awake = false;
};

// A static camp group plus what we remember about it.
struct Group {
    unsigned handle = 0;
    std::vector<cambc::vec2> spawns;
    cambc::vec2 center;
    int gems = 0, gold = 0, respawn = 0;
    float max_hp = 0, strength = 0;
    int range = 3;
    bool boss = false;
    int dead_until = 0;     // known to be respawning (or captured) until this round
    int state = 0;          // last seen state: 0 unknown, 1 alive, 2 reward, 3 respawning
    int seen = -1000;       // round of the last observation
    float hp_left = -1;     // last seen remaining monster health (-1: assume full)
};

// One of our ability slots, decoded once.
struct Spell {
    int kind = 0;           // cambc::ability_kind, 0 for an unused slot
    float range = 0, modifier = 0, radius = 0;
    bool exclusive = false; // excludes walking in the same turn
    bool enhanced = false;
};

class Bot {
public:
    void play_turn();

private:
    // ---- static map knowledge (first turn) ----
    bool ready = false;
    int width = 0, height = 0;
    cambc::bitboard static_block;     // walls, holes, altars, enemy spawn
    cambc::bitboard ally_spawn, enemy_spawn;
    cambc::distance_field home_field; // walking distance to our spawn (terrain only)
    cambc::distance_field enemy_field;// walking distance to the enemy spawn (terrain only)
    std::vector<cambc::vec2> altars;
    std::vector<Group> groups;
    std::vector<std::int16_t> group_at; // camp spawn tile -> group index
    cambc::vec2 home_center, enemy_center;
    int my_index = 0;                 // position among allied champion handles
    int team_count = 0;
    std::vector<Body> ally_design;    // base-stat estimate per ally index, for allies out of sight
    std::vector<unsigned> ally_ids;
    std::array<Spell, 2> spells{};
    int shards_bought = 0;

    // ---- per-turn snapshot ----
    unsigned round = 0;
    cambc::vec2 pos;
    float hp = 0, max_hp = 1, strength = 0, focus = 0;
    int range = 1, type = 0;
    bool melee = true;
    bool moved = false, attacked = false, cast = false, cast_exclusive = false, move_blocked = false;
    std::vector<Body> enemies, allies;
    std::vector<CampSeen> camps;
    cambc::bitboard visible, occupied;
    std::vector<int> alive_enemy_types;
    int alive_allies = 0;
    int alive_enemies = 0;

    // ---- team coordination (shared memory) ----
    std::array<std::uint8_t, 8> mem{};
    int leader = -1;
    bool is_leader = false;
    cambc::vec2 leader_pos{-1, -1};
    bool leader_fresh = false;
    int mode = 0, mode_arg = 255;     // leader plan: 0 none, 1 farm, 2 bank, 3 hunt
    cambc::vec2 sighting{-1, -1};
    int sighting_age = 1000;
    bool unavailable = false;
    int pack_mask = 0;                // allies standing near the leader (shared, bits by index)
    bool in_pack = false;
    bool leashed = false;             // while engaging: stay near the pack
    cambc::vec2 leash_center{-1, -1};
    int leash_radius = 99;
    bool leash_ok(cambc::vec2 p) const { return !leashed || p.chebyshev_distance(leash_center) <= leash_radius; }

    // ---- leader planning ----
    int target = -1;
    int target_since = 0;
    int last_plan = -100;
    cambc::distance_field leader_field;
    int leader_field_round = -100;

    // ---- unit memory (vision flickers in bushes) ----
    std::vector<Memory> ally_mem, enemy_mem;
    void remember(std::vector<Memory>& list, const Body& b);

    // ---- team fight call (shared memory) ----
    bool call_fresh = false;          // a teammate already decided this round
    bool call_engage = false;
    cambc::vec2 call_target{-1, -1};
    int call_age = 1000;
    int wait_turns = 0;

    // ---- combat memory ----
    unsigned chase_id = 0xFFFFFFFF;
    int chase_turns = 0, chase_best = 1000;
    int no_chase_until = 0;
    int last_enemy_round = -100;

    // setup and snapshot
    void init();
    void snapshot();
    void read_body(const cambc::champion& c, Body& b, bool enemy);
    void update_groups();
    void read_shared();
    void write_shared();
    void elect_leader();
    void plan();
    int nearest_altar(cambc::vec2 from) const;

    // decisions
    bool combat();
    void engage(const Body& target);
    void retreat();
    void farm();
    bool bank();
    void shop();
    void follow(cambc::vec2 goal, int slack);

    // actions
    bool step_to(const cambc::bitboard& goals, bool slow = false);
    bool step_toward(cambc::vec2 goal, int within);
    bool attack_body(const Body& b);
    bool attack_camp(const CampSeen& c);
    bool try_reset_attack(bool camp_target);
    bool cast_on_enemy(const Body& target, bool allow_exclusive);
    bool cast_on_camp(const CampSeen& c);
    bool cast_support();
    bool try_dash(cambc::vec2 toward, bool away);
    bool free_tile(cambc::vec2 p) const;
    float threat_at(cambc::vec2 p) const;
};
