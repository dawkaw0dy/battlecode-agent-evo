#pragma once
#include <cambc.h>
#include <memory>
#include <optional>
#include <vector>
#include "plan.h"

// A static path field toward a set of root tiles, with the tiles it reached.
struct Field {
    cambc::distance_field dist;
    cambc::bitboard seen;
    int at(cambc::vec2 p) const;
};

// What one champion knows about a camp group.
struct Group {
    cambc::map::camp_group handle;
    GroupFacts facts;
    cambc::bitboard tiles;
    std::uint8_t flags = 0, zone = 0;
    int respawn_at = 0;  // round the group is next expected alive
    bool reward = false; // all members dead, reward waiting
    std::unique_ptr<Field> field;
};

// A visible unit captured at turn start.
struct Seen {
    cambc::vec2 pos;
    float hp = 0, max_hp = 1;
};

// Per-champion state survives between turns.
class Bot {
    bool ready = false;
    Plan plan;
    std::vector<Group> groups;
    cambc::bitboard walls;
    Field altar_field, home_field;
    int slot = 0, bodies = 1;
    int target = -1, chosen_round = -100;
    bool ranged = false, tank = false;
    bool walked_fail = false, cast_exclusive = false, attacked = false;
    int purchases = 0;

    std::vector<Seen> enemies, allies;

    void init();
    void update_groups();
    void observe_units();
    const Field& group_field(int index);
    int choose_target();
    float my_dps() const;
    bool step_along(const Field& field);
    bool step_away(const cambc::bitboard& threat);
    bool step_off(const cambc::bitboard& avoid);
    bool try_spells();
    bool basic_attack();
    bool try_bank();
    void shop();
    cambc::bitboard threat_tiles() const;
    bool farm();

public:
    void play_turn();
};
