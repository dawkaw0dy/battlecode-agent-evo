#pragma once
#include <cambc.h>
#include <memory>
#include <optional>
#include <vector>
#include "plan.h"

// A static path field toward a set of root tiles, with the tiles it reached.
struct Field {
    cambc::distance_field dist;
    cambc::bitboard seen, roots;
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
    float hp = 0, max_hp = 1, str = 0;
};

enum class Fight { none, engage, hold, retreat };

// Per-champion state survives between turns.
class Bot {
    bool ready = false;
    Plan plan;
    std::vector<Group> groups;
    cambc::bitboard walls;
    std::unique_ptr<Field> detour;
    const Field* detour_for = nullptr;
    int detour_until = -1;
    Field altar_field, home_field;
    int slot = 0, bodies = 1;
    int target = -1, chosen_round = -100;
    bool ranged = false, tank = false;
    bool walked_fail = false, cast_exclusive = false, attacked = false, avoid_active = false;
    cambc::bitboard avoid;
    int purchases = 0;

    std::vector<Seen> enemies, allies;

    void init();
    void update_groups();
    void observe_units();
    const Field& group_field(int index);
    int choose_target(float keep_bonus);
    bool shopping_trip(Fight fight);
    void recall();
    float my_dps() const;
    bool walk(cambc::vec2 destination);
    bool step_along(const Field& field, bool roots_are_targets = false);
    bool step_away(const cambc::bitboard& threat);
    bool step_off(const cambc::bitboard& avoid);
    bool try_spells();
    bool basic_attack();
    bool try_bank();
    void shop();
    cambc::bitboard threat_tiles() const;
    bool farm();
    Fight assess_fight(const Seen** weakest) const;

public:
    void play_turn();
};
