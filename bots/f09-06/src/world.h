#pragma once
// What one champion knows about the map: static terrain, camp groups, altars, and this turn's units.
#include "common.h"
#include <vector>

struct Member {
    vec2 pos;
    cambc::camp_type kind;
    float max_hp, hp, str;
    int range;
};

enum class GroupState { alive, reward, respawning };

struct Group {
    int id = 0;
    std::vector<Member> m;
    std::vector<vec2> tiles;      // spawn tiles
    cambc::bitboard spawn;        // spawn tiles as a board
    vec2 center;
    int gems = 0, gold = 0, respawn_max = 0;
    GroupState state = GroupState::alive;
    int respawn_at = 0;           // estimated round the group is back
    int seen = -1000;             // last round its state was read
    int ours = 0;                 // static distance from our spawn
    int theirs = 0;               // static distance from the enemy spawn
    bool boss = false;            // contains a golem or monolith
    int radius = 0;               // Chebyshev radius of the spawn tiles around the centre
    int max_range = 0;            // longest camp attack range in the group
    float full_str = 0;           // strength of the whole group when alive
    bool island = false;          // no spawn tile can be walked to from our spawn
    int sector = -1;              // pack that owns this group (-1: anyone)
    bool gate = false;            // stands on the only way to tribute
    Field field;                  // terrain distance to the tiles around the group (filled lazily)
    float hp() const { float s = 0; for (auto& x : m) if (x.hp > 0) s += x.hp; return s; }
    float str() const { float s = 0; for (auto& x : m) if (x.hp > 0) s += x.str; return s; }
    int alive_count() const { int n = 0; for (auto& x : m) if (x.hp > 0) ++n; return n; }
};

struct Altar {
    vec2 pos;
    std::vector<vec2> approach;   // free tiles within submit range
    int ours = kFar, theirs = kFar;  // static distance from each spawn to the approach tiles
    int enemy_lock_seen = -1000;  // last round we saw an enemy lock here
    int ally_lock_seen = -1000;
    bool gated = false;           // every approach tile is a camp tile
};

// A visible champion this turn.
struct Unit {
    vec2 pos;
    float hp = 0, max_hp = 1, str = 0, focus = 0;
    int range = 1;
    int gems = 0;
    cambc::champion_type type{};
    bool channel = false;   // recalling or tributing
    bool tribute = false;
    bool stunned = false;
    bool enemy = false;
};

// A visible camp this turn.
struct CampSeen {
    vec2 pos;
    float hp, str;
    int range;
    int group;
    bool awake;
    cambc::camp_type kind;
};

struct World {
    bool ready = false;
    cambc::bitboard blocked;      // walls | holes | altars | enemy spawn
    cambc::bitboard our_spawn, their_spawn;
    vec2 our_center, their_center;
    std::vector<Group> groups;
    std::vector<cambc::map::camp_group> handles;  // aligned with groups
    std::vector<Altar> altars;
    std::vector<std::int16_t> tile_group;  // spawn tile -> group index, -1 elsewhere
    Field from_spawn;             // terrain distance from our spawn; the enemy's is its mirror image
    cambc::symmetry sym{};
    vec2 mirror(vec2 p) const;    // the tile matching p on the other side

    // Turn snapshot.
    int round = 0;
    cambc::bitboard visible, occupied, campb;
    cambc::bitboard alive_camps;  // tiles of camps we believe alive
    cambc::bitboard camp_walls;   // every camp spawn tile
    bool all_gated = false;       // no altar can be used before a camp dies
    int camps_version = 0;        // bumps when alive_camps changes
    std::vector<Unit> enemies, allies;
    std::vector<CampSeen> camps;

    void init();
    void refresh();               // snapshot units, camps and group states
    void mark(Member& m, bool alive);
    // Terrain field of a group; computed now when fuel allows, else nullptr.
    const Field* group_field(int gi, bool compute = true);
    void precompute_some();       // fill missing group fields with spare fuel
    void partition(int packs);    // split our side's groups into one sector per pack
    int group_at(vec2 p) const {
        return inside(p) ? tile_group[p.y * g_w + p.x] : -1;
    }
    bool passable(vec2 p) const { return inside(p) && !raw_bit(blocked.ptr(), p); }
    bool free(vec2 p) const { return passable(p) && !raw_bit(occupied.ptr(), p); }
};

extern World W;
