#pragma once
#include "world.h"
#include <array>

// One ability slot's design, read once.
struct Slot {
    bool valid = false;
    cambc::ability_kind kind{};
    float range = 0, modifier = 0, radius = 0;
    bool excl = false, enemies = false, camps = false, allies = false, ground = false, self = false, enhanced = false;
};

enum class Role { pack, warden };

class Bot {
public:
    void play_turn();

private:
    bool inited = false;
    Role role = Role::pack;
    int slot_index = 0;            // our position in the draft order
    int team_n = 5;
    int pack = 0, pack_n = 1;       // our pack and its size
    int packs = 1;                  // number of packs on the team
    std::array<Slot, 2> kit{};
    bool ranged = false;
    bool casted = false;            // a successful cast this turn
    bool walked = false;            // a successful walk this turn
    bool attacked = false;
    int stage = 0, stage_bought = 0;  // shopping plan progress
    int target = -1;                // camp group we farm
    int approach_r = 3;             // shrinks when a ranged target is out of sight
    int approach_target = -1;
    int stuck_turns = 0;
    int rally_turns = 0;
    int last_enemy_round = -1000;
    struct Seen { Unit u; int round; };
    std::vector<Seen> seen_enemies;  // last sighting per enemy champion
    int chase_turns = 0, chase_block_until = -1;
    int flee_until = -1;            // keep out of remembered enemies' reach until then
    bool banking = false;
    bool healing = false;
    const char* act = "";         // what this turn was spent on (debug)

    void init();
    void shop();
    // Actions; each returns true when it acted.
    bool attack_unit_at(vec2 p);   // basic attack a champion or camp on that tile
    bool attack_best_enemy();
    bool cast_on_enemy(const Unit& e);
    bool cast_on_camp(const CampSeen& c);
    bool cast_support();
    bool reset_and_hit(vec2 p);
    bool dash_towards(vec2 p, bool away);

    // Decisions.
    bool handle_fight();           // true when the turn is spent on enemies
    bool handle_health();          // retreat / recall / heal on spawn
    bool handle_bank();
    void handle_farm();
    int choose_group();
    int helpers_on(int gi) const;  // teammates whose target is this group
    bool work_group(int gi);
    bool make_room();
    bool can_tribute_safely(int gems) const;

    float my_dps() const;
};
