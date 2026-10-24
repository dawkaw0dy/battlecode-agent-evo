#pragma once
#include <cambc.h>
#include <functional>
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

// What one champion knows about a camp group.
struct Group {
    cambc::map::camp_group handle;
    GroupFacts facts;
    cambc::bitboard tiles;
    std::uint8_t flags = 0, zone = 0;
    int respawn_at = 0;  // round the group is next expected alive
    bool reward = false; // all members dead, reward waiting
    int contested_until = 0; // a capture here stalled: avoid until this round
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
    bool try_dash(const std::function<int(cambc::vec2)>& cost, int min_gain, const Bits& blocked_bits);
    bool step_away(const cambc::bitboard& threat);
    bool step_off(const cambc::bitboard& avoid);
    bool try_spells();
    bool basic_attack();
    bool try_bank();
    void shop();
    cambc::bitboard threat_tiles() const;
    cambc::bitboard occupied() const;
    bool farm();
    Fight assess_fight(const Seen** weakest) const;

public:
    void play_turn();
};
