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
    int range = 1;
    int type = 0;
    unsigned id = 0;
    unsigned gems = 0;
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
    float max_camp_strength = 0;
    int range = 3;
    bool boss = false;
    int home_dist = 0;      // walking distance from our spawn
    int enemy_dist = 0;     // walking distance from the enemy spawn
    int dead_until = 0;     // known to be respawning (or captured) until this round
    int state = 0;          // last seen state: 0 unknown, 1 alive, 2 reward, 3 respawning
    int seen = -1000;       // round of the last observation
    float hp_left = -1;     // last seen remaining monster health (-1: assume full)
    int danger_until = 0;   // enemies were seen near it
    int blocked_until = 0;  // we could not get there
    bool guards_altar = false; // holds the only way onto an altar
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
    cambc::bitboard terrain;          // walls, holes, altars
    cambc::bitboard ally_spawn, enemy_spawn;
    cambc::bitboard altar_goals;      // free tiles next to an altar
    cambc::bitboard camp_spawns;      // every camp's spawn tile
    cambc::distance_field home_field; // walking distance to our spawn (terrain only)
    cambc::distance_field enemy_field;// walking distance to the enemy spawn (terrain only)
    cambc::distance_field altar_field;// walking distance to the nearest altar tile
    std::vector<cambc::vec2> altars;
    std::vector<Group> groups;
    std::vector<std::int16_t> group_at; // camp spawn tile -> group index
    cambc::vec2 home_center, enemy_center;
    int my_index = 0;                 // position among allied champion handles
    int team_count = 0;
    std::vector<unsigned> ally_ids;
    std::array<Spell, 2> spells{};
    int shards_bought = 0;
    float my_dps = 50;                // our damage per turn against camps

    // ---- per-turn snapshot ----
    unsigned round = 0;
    cambc::vec2 pos;
    float hp = 0, max_hp = 1, strength = 0, focus = 0;
    int range = 1, type = 0;
    unsigned gems = 0, gold = 0;
    bool melee = true;
    bool moved = false, attacked = false, cast = false, cast_exclusive = false, move_blocked = false;
    std::vector<Body> enemies, allies;
    std::vector<CampSeen> camps;
    cambc::bitboard visible, occupied;
    int alive_allies = 0, alive_enemies = 0;

    // ---- unit memory (vision flickers in bushes) ----
    std::vector<Memory> ally_mem, enemy_mem;
    void remember(std::vector<Memory>& list, const Body& b);

    // ---- team coordination (shared memory) ----
    // [0..4] claimed group per ally index (255 none)  [5,6] alert x,y
    // [7] alert round (low 6 bits) | help wanted (bit 6) | valid (bit 7)
    std::array<std::uint8_t, 8> mem{};
    cambc::vec2 alert{-1, -1};
    int alert_age = 1000;
    bool alert_help = false;

    // ---- farming ----
    int target = -1;
    int last_plan = -100;
    cambc::distance_field my_field;
    int my_field_round = -100;
    bool banking = false;
    int bank_blocked_until = 0;
    cambc::vec2 stuck_pos{-1, -1};
    int stuck_turns = 0;

    // ---- combat memory ----
    unsigned chase_id = 0xFFFFFFFF;
    int chase_turns = 0, chase_best = 1000;
    int no_chase_until = 0;
    int last_enemy_round = -100;
    float prev_hp = -1;               // our health at the start of the previous turn
    bool damaged = false;             // lost health since then
    int ambush_until = -100;          // hit by something we cannot see
    const char* last_act = "";        // debug: what we did on our last living turn
    char last_info[160] = {};
    bool was_alive = true;

    // setup and snapshot
    void init();
    void snapshot();
    void read_body(const cambc::champion& c, Body& b, bool enemy);
    void update_groups();
    void read_shared();
    void post_alert(cambc::vec2 p, bool help);

    // decisions
    void choose_target();
    bool combat();
    void engage(const Body& target);
    void retreat();
    bool step_home();
    bool farm();
    bool bank();
    bool go_bank();
    void shop();

    // actions
    bool step_to(const cambc::bitboard& goals);
    bool step_toward(cambc::vec2 goal, int within);
    bool attack_body(const Body& b);
    bool attack_camp(const CampSeen& c);
    bool try_reset_attack();
    bool cast_on_enemy(const Body& target, bool allow_exclusive);
    bool cast_on_camp(const CampSeen& c);
    bool cast_support();
    bool try_dash(cambc::vec2 toward, bool away);
    bool free_tile(cambc::vec2 p) const;
    float threat_at(cambc::vec2 p) const;
    float camp_threat_at(cambc::vec2 p) const;
};
