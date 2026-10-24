#pragma once
#include <cambc.h>
#include <memory>
#include <optional>
#include <vector>
#include "plan.h"

// Row-major copy of a bitboard, read without host calls (each host call costs real fuel).
struct Bits {
    std::vector<std::uint8_t> bytes;
    int width = 0;
    Bits() = default;
    explicit Bits(const cambc::bitboard& board) : bytes(board.export_linear_buffer()), width(cambc::map::width()) {}
    // Position must be inside the map.
    bool get(cambc::vec2 p) const {
        const int i = p.y * width + p.x;
        return (bytes[static_cast<std::size_t>(i >> 3)] >> (i & 7)) & 1;
    }
};

// A path field toward a set of root tiles, with the tiles it reached, mirrored into wasm memory.
struct Field {
    cambc::distance_field dist;
    cambc::bitboard seen, roots;
    std::vector<std::uint16_t> values;
    Bits reached;
    void compute(const cambc::bitboard& walls, const cambc::bitboard& from);
    int at(cambc::vec2 p) const;
};

// One camp of a group, as spawned.
struct Member {
    cambc::vec2 pos;
    float max_hp = 0, str = 0;
    int range = 0;
    bool monster = false; // hits back (wolf, elk, golem, monolith)
};

// What one champion knows about a camp group.
struct Group {
    cambc::map::camp_group handle;
    GroupFacts facts;
    cambc::bitboard tiles;
    std::uint8_t flags = 0, zone = 0;
    int respawn_at = 0;  // round the group is next expected alive
    bool reward = false; // all members dead, reward waiting
    int contested_until = 0; // a capture here stalled: avoid until this round
    std::vector<Member> members;
    std::vector<float> member_hp; // last known health per member (0: dead)
    int enemy_near_until = 0;     // enemy champions seen nearby: risky until this round
    int lonely_until = 0;         // waited for helpers in vain: skip until this round
    bool deadly = false;          // golem or monolith: never woken alone
    int ally_capturing_until = 0; // an ally stands on the region of its reward
    // Living monsters in kill order (health, strength), cluster health and summed strength; rebuilt
    // when order_dirty is set (a member's health changed).
    std::vector<std::pair<float, float>> order;
    float cluster_hp = 0, hits = 0;
    bool order_dirty = true;
    cambc::vec2 wait_spot{-1, -1}; // where we wait for its respawn
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
    Bits wall_bits, avoid_bits;
    std::unique_ptr<Field> detour;
    const Field* detour_for = nullptr;
    int detour_until = -1;
    Field altar_field, home_field;
    int slot = 0, bodies = 1;
    int boss = -1; // group guarding every altar approach
    int target = -1, chosen_round = -100;
    bool ranged = false, tank = false;
    bool walked_fail = false, cast_exclusive = false, attacked = false, avoid_active = false;
    cambc::bitboard avoid;
    int purchases = 0;
    int chase_streak = 0, no_chase_until = 0;
    int hold_group = -1, hold_since = 0;

    std::vector<Seen> enemies, allies;

    // Camp farming state (camps.cpp).
    int want_n = 1;             // champions the current target is planned for
    int need_n = 1;             // champions that must be ready before we wake it
    float plan_damage = 0;      // health we expect to lose clearing the target
    int stand_group = -1, stand_round = -100, stand_alive = -1;
    cambc::vec2 stand{-1, -1};  // tile we attack the target from
    int gather_since = -1;      // round we started waiting for helpers
    int target_since = 0, tracked_target = -1; // round the current target was first chosen
    int capture_seen = 0;       // last capture news read from shared memory
    int stand_shrink = 0, stand_idle = 0; // attack range given up after idle turns at the stand
    float last_hp = 0;
    int last_hp_round = -1;
    bool heal_trip = false;     // left a clear to heal: go home before coming back

    void prepare_group(Group& g);
    float clear_cost(Group& g, float dps, float* clear_time) const;
    bool pick_stand(int index);
    void post_rally(int group);
    int rally_group() const;
    int rally_ready(int group) const;
    void set_ready(bool ready);
    float camp_incoming(int* exit_steps) const;
    int group_distance(int index, cambc::vec2 pos) const;
    bool farm_step();
    bool step_towards(cambc::vec2 goal);
    bool step_outward(const Field& field);
    cambc::vec2 wait_spot(int index);

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
