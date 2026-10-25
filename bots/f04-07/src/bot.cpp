// Vision-ambush champion: farm camps in squads, bank gems, and pick off isolated or gem-carrying enemies.
// Sightings travel through the 8-byte shared memory; wards watch approaches and ambush spots.
#include <cambc.h>
#include "bot.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace bc = cambc;
using bc::team;
using bc::vec2;

namespace {

// ---------------------------------------------------------------- geometry and local masks
int W = 0, H = 0;

int dist(vec2 a, vec2 b) { return std::max(std::abs(a.x - b.x), std::abs(a.y - b.y)); }
bool inb(vec2 p) { return p.x >= 0 && p.y >= 0 && p.x < W && p.y < H; }

// Row-major local copy of a bitboard: bit tests without host calls.
struct Bits {
    std::vector<std::uint8_t> b;
    bool get(vec2 p) const {
        if (!inb(p)) return false;
        const int i = p.y * W + p.x;
        return (b[i >> 3] >> (i & 7)) & 1;
    }
};
Bits bits(const bc::bitboard& board) { return Bits{board.export_linear_buffer()}; }

constexpr std::array<vec2, 8> kDirs{vec2{0, 1}, vec2{1, 1}, vec2{1, 0}, vec2{1, -1},
                                    vec2{0, -1}, vec2{-1, -1}, vec2{-1, 0}, vec2{-1, 1}};

// Camp stats after the patches (index = camp_type).
constexpr float kCampHp[6] = {1350, 2400, 120, 120, 20000, 5000};
constexpr float kCampStr[6] = {20, 60, 0, 0, 200, 120};
constexpr int kCampRange[6] = {3, 4, 0, 0, 5, 4};

// ---------------------------------------------------------------- tuning
constexpr float kDangerPool = 2.5f;   // expected camp damage vs the squad's health
constexpr float kTankRounds = 3.0f;  // one champion must survive this many rounds of the group's hits
constexpr float kFightNeed = 1.3f;    // square-law advantage needed to start a fight
constexpr int kFleeRadius = 7;
constexpr int kSquads = 2;
constexpr int kChaseMargin = 4;
constexpr float kRetreatHp = 0.35f;
constexpr int kBankGems = 25;
constexpr int kCaptureTimeout = 14;
constexpr int kGatherTurns = 12;
constexpr float kHealHp = 0.75f;
constexpr int kJungleShards = 5;
constexpr float kOtherSquadFactor = 1.0f;
constexpr bool kSplitCapture = true;
constexpr int kRangedApproach = 2;
constexpr int kSight = 3;  // shared-memory offset of the 4 sighting bytes (0..2 hold squad targets)
constexpr bool kFuelProbe = false;
constexpr int kShopGold = 180;
constexpr float kAloneFactor = 1.0f;  // 1.6 (cautious when alone) lost badly
constexpr float kCampRetreatRounds = 5.0f;  // leave when awake camps in range would kill us within this many rounds

// ---------------------------------------------------------------- static map knowledge
struct Group {
    std::vector<vec2> tiles;
    vec2 center;
    int gems = 0, gold = 0, respawn = 0;
    float hp_full = 0, str = 0;
    int range = 0;
    // Knowledge from our own vision.
    int respawn_until = -1;   // round the group is back, if seen respawning
    float hp_seen = -1;       // remaining health when last seen alive
    bool reward = false;      // seen dead with the reward waiting
    int skip_until = -1;      // contested: leave it alone until this round
    std::vector<int> approach; // walkable tiles next to the group (row-major indices)
    std::vector<vec2> wait_ring; // just outside camp range, never on a spawn tile
    int capture_turns = 0;
};

bool g_init = false;
std::vector<Group> groups;
std::vector<std::int16_t> group_at;  // tile -> group index
std::vector<vec2> altars;
std::vector<int> altar_enemy_lock;   // round we last saw an enemy lock (-1 none)
std::vector<int> altar_enemy_use;    // sightings of enemies next to the altar
bc::bitboard terrain;                // walls | holes | altars | enemy spawn
Bits terrain_bits, bush_bits, wall_bits, hole_bits, ally_spawn_bits;
std::vector<vec2> ally_spawn_tiles, enemy_spawn_tiles;
std::vector<std::uint16_t> spawn_field;  // walking distance from our spawn over terrain (0xFFFF unreachable)
vec2 ally_spawn_center, enemy_spawn_center;

// ---------------------------------------------------------------- per champion
int my_index = 0, team_size = 1, squad = 0, squads = 1, squad_size = 1;
bool ranged = false;
float my_base_dps = 50;
int shards = 0;
int target_group = -1;
int wait_turns = 0;
int last_ward_round = -100;
bool heal_mode = false;

struct Kit {
    bool present = false;
    bc::ability_kind kind{};
    float range = 0, modifier = 0;
    bool excl = false, camps = false, enemies = false;
};
std::array<Kit, 2> kit;

// ---------------------------------------------------------------- shared memory (8 bytes)
// 0,1: squad target group + 1      2,3: sighting x,y      4: sighting round & 255
// 5: sighting gems / 4 (cap 63) | 64 if an ally is fighting there | 128 if the enemy is alone
// 6,7: ally help call x,y (255 = none) written with round in sighting? (kept simple: x,y only)
std::uint8_t mem_get(int i) { return static_cast<std::uint8_t>(bc::shared_memory[i]); }
void mem_set(int i, std::uint8_t v) { bc::shared_memory[i] = v; }

void warn(const char* where, const std::exception& e) {
    std::cout << "warn " << where << ": " << e.what() << '\n';
}

void init_static() {
    W = bc::map::width();
    H = bc::map::height();
    terrain = bc::map::walls() | bc::map::holes() | bc::map::altars() | bc::map::spawn_region(team::enemy);
    terrain_bits = bits(terrain);
    bush_bits = bits(bc::map::bushes());
    wall_bits = bits(bc::map::walls());
    hole_bits = bits(bc::map::holes());
    ally_spawn_bits = bits(bc::map::spawn_region(team::ally));
    ally_spawn_tiles = bc::map::spawn_region(team::ally).positions();
    enemy_spawn_tiles = bc::map::spawn_region(team::enemy).positions();
    auto centre = [](const std::vector<vec2>& t) {
        long sx = 0, sy = 0;
        for (auto p : t) sx += p.x, sy += p.y;
        if (t.empty()) return vec2{0, 0};
        return vec2{static_cast<int>(sx / static_cast<long>(t.size())), static_cast<int>(sy / static_cast<long>(t.size()))};
    };
    ally_spawn_center = centre(ally_spawn_tiles);
    enemy_spawn_center = centre(enemy_spawn_tiles);
    {
        bc::distance_field field;
        bc::bitboard visited;
        field.compute(terrain, bc::map::spawn_region(team::ally), visited);
        spawn_field = field.export_linear_buffer();
    }
    altars = bc::map::altars().positions();
    altar_enemy_lock.assign(altars.size(), -1);
    altar_enemy_use.assign(altars.size(), 0);
    group_at.assign(static_cast<std::size_t>(W * H), -1);
    for (const auto& g : bc::map::camp_groups()) {
        Group gr;
        gr.gems = static_cast<int>(g.gem_value());
        gr.gold = static_cast<int>(g.gold_value());
        gr.respawn = g.max_respawn_timer();
        for (const auto& c : g.camps()) {
            const int k = static_cast<int>(c.kind());
            gr.tiles.push_back(c.spawn_position());
            if (k >= 0 && k < 6) {
                gr.hp_full += kCampHp[k];
                gr.str += kCampStr[k];
                gr.range = std::max(gr.range, kCampRange[k]);
            }
        }
        if (gr.tiles.empty()) continue;
        gr.center = centre(gr.tiles);
        // Use a real member tile as the centre so distance lookups land on a reachable neighbour.
        vec2 best = gr.tiles[0];
        for (auto t : gr.tiles)
            if (dist(t, gr.center) < dist(best, gr.center)) best = t;
        gr.center = best;
        for (auto t : gr.tiles) group_at[t.y * W + t.x] = static_cast<std::int16_t>(groups.size());
        groups.push_back(std::move(gr));
    }
    for (auto& gr : groups) {
        const int inner = std::max(1, gr.range);
        int x0 = W, y0 = H, x1 = -1, y1 = -1;
        for (auto t : gr.tiles) x0 = std::min(x0, t.x), y0 = std::min(y0, t.y), x1 = std::max(x1, t.x), y1 = std::max(y1, t.y);
        for (int y = y0 - inner - 1; y <= y1 + inner + 1; ++y)
            for (int x = x0 - inner - 1; x <= x1 + inner + 1; ++x) {
                const vec2 p{x, y};
                if (!inb(p) || terrain_bits.get(p) || group_at[y * W + x] >= 0) continue;
                int dmin = 1 << 20;
                for (auto t : gr.tiles) dmin = std::min(dmin, dist(t, p));
                if (dmin == inner + 1) gr.wait_ring.push_back(p);
            }
        for (auto t : gr.tiles)
            for (auto d : kDirs) {
                const vec2 n = t + d;
                if (!inb(n) || terrain_bits.get(n) || group_at[n.y * W + n.x] >= 0) continue;
                const int idx = n.y * W + n.x;
                if (std::find(gr.approach.begin(), gr.approach.end(), idx) == gr.approach.end()) gr.approach.push_back(idx);
            }
        // Keep the travel scan cheap: at most 8 evenly spread approach tiles per group.
        if (gr.approach.size() > 8) {
            std::vector<int> thin;
            for (std::size_t i = 0; i < 8; ++i) thin.push_back(gr.approach[i * gr.approach.size() / 8]);
            gr.approach = std::move(thin);
        }
    }
}

void init_self() {
    auto& me = bc::self();
    const auto allies = bc::champions(team::ally);
    team_size = static_cast<int>(allies.size());
    for (int i = 0; i < team_size; ++i)
        if (allies[i] == me) my_index = i;
    squads = kSquads > 1 && team_size >= 4 ? kSquads : 1;
    squad = my_index % squads;
    squad_size = (team_size + squads - 1 - squad) / squads;
    if (kSquads == 0) squad_size = 1;
    ranged = me.attack_range() > 1;
    const auto abilities = me.abilities();
    for (int s = 0; s < 2; ++s) {
        const auto& ab = abilities[s];
        Kit k;
        try {
            const auto& info = ab.info();
            if (info) {
                k.present = true;
                std::visit([&](const auto& p) {
                    using T = std::decay_t<decltype(p)>;
                    k.kind = T::kind;
                    k.excl = p.exclusive_with_movement;
                    k.camps = p.valid_targets.contains(bc::ability_target_mask::camps);
                    k.enemies = p.valid_targets.contains(bc::ability_target_mask::enemies);
                    if constexpr (requires { p.range; }) k.range = p.range;
                    if constexpr (requires { p.modifier; }) k.modifier = p.modifier;
                }, *info);
            }
        } catch (const std::exception& e) { warn("kit", e); }
        kit[s] = k;
    }
    my_base_dps = me.strength() + me.focus() * 0.25f;
}

// ---------------------------------------------------------------- per-turn view
struct Unit {
    vec2 pos;
    float hp = 0, max_hp = 1, str = 0, focus = 0;
    int range = 1, gems = 0;
    bool channel = false;  // tributing or recalling: cannot fight back
};

struct View {
    vec2 me;
    int round = 0;
    Bits vis, blocked_bits, enemy_bits, camp_bits;
    bc::bitboard blocked;
    std::vector<Unit> enemies, allies;
    bool moved = false, move_failed = false, attacked = false;
};
View v;

float dps_of(const Unit& u) { return u.str + u.focus * 0.25f; }

// Our half of the map, with a margin: chasing beyond it runs into the enemy's spawn and squads.
bool own_side(vec2 p, int margin) {
    return dist(p, ally_spawn_center) <= dist(p, enemy_spawn_center) + margin;
}

bool step_towards(const bc::bitboard& goals) {
    auto& me = bc::self();
    if (v.moved || v.move_failed || !me.can_move()) return false;
    if (goals.is_set(v.me)) return false;
    bc::bitboard walls = v.blocked & ~goals;
    bc::bitboard start;
    start.set(v.me);
    bc::distance_field field;
    bc::bitboard visited;
    field.compute_to(walls, goals, start, visited);
    if (!visited.is_set(v.me)) return false;
    const int here = field.get(v.me);
    vec2 best = v.me;
    int best_d = here;
    for (auto d : kDirs) {
        const vec2 n = v.me + d;
        if (!inb(n) || v.blocked_bits.get(n)) continue;
        if (!visited.is_set(n)) continue;
        const int fd = field.get(n);
        if (fd < best_d) best_d = fd, best = n;
    }
    if (best == v.me) return false;
    try {
        if (me.move_absolute(best)) {
            v.moved = true;
            v.me = best;
            return true;
        }
    } catch (const std::exception& e) { warn("move", e); }
    v.move_failed = true;
    return false;
}

bc::bitboard ring_around(const std::vector<vec2>& centres, int radius) {
    bc::bitboard out;
    for (auto c : centres)
        for (int dy = -radius; dy <= radius; ++dy)
            for (int dx = -radius; dx <= radius; ++dx) {
                const vec2 p{c.x + dx, c.y + dy};
                if (inb(p) && !terrain_bits.get(p)) out.set(p);
            }
    return out;
}

bool step_to_tile(vec2 t) {
    bc::bitboard g;
    if (!inb(t)) return false;
    g.set(t);
    return step_towards(g);
}

void read_view() {
    auto& me = bc::self();
    v = View{};
    v.me = me.position();
    v.round = static_cast<int>(bc::round_number());
    v.vis = bits(bc::vision::visible_tiles());
    const auto enemy_tiles = bc::vision::champion_tiles(team::enemy);
    const auto ally_tiles = bc::vision::champion_tiles(team::ally);
    const auto camp_tiles = bc::vision::camp_tiles();
    v.blocked = terrain | enemy_tiles | ally_tiles | camp_tiles | bc::vision::teleport_tiles();
    v.blocked.set(v.me, false);
    v.blocked_bits = bits(v.blocked);
    v.enemy_bits = bits(enemy_tiles);
    v.camp_bits = bits(bc::vision::awake_camp_tiles() | bc::vision::asleep_camp_tiles());
    for (auto p : enemy_tiles.positions()) {
        try {
            auto c = bc::vision::champion_at(p);
            Unit u;
            u.pos = p;
            u.hp = c.health();
            u.max_hp = c.max_health();
            u.str = c.strength();
            u.focus = c.focus();
            u.range = c.attack_range();
            u.gems = static_cast<int>(c.gem_count());
            u.channel = c.is_submitting_gems() || c.is_recalling();
            if (u.hp > 0) v.enemies.push_back(u);
        } catch (const std::exception& e) { warn("enemy", e); }
    }
    for (auto p : ally_tiles.positions()) {
        if (p == v.me) continue;
        try {
            auto c = bc::vision::champion_at(p);
            Unit u;
            u.pos = p;
            u.hp = c.health();
            u.max_hp = c.max_health();
            u.str = c.strength();
            u.focus = c.focus();
            u.range = c.attack_range();
            if (u.hp > 0) v.allies.push_back(u);
        } catch (const std::exception& e) { warn("ally", e); }
    }
}

// Remember camp-group states and healths we can see.
void update_groups() {
    for (std::size_t gi = 0; gi < groups.size(); ++gi) {
        auto& g = groups[gi];
        bool seen = false;
        for (auto t : g.tiles)
            if (v.vis.get(t)) { seen = true; break; }
        if (!seen) {
            if (g.respawn_until >= 0 && v.round >= g.respawn_until) {
                g.respawn_until = -1;
                g.hp_seen = -1;
                g.reward = false;
            }
            continue;
        }
        float hp = 0;
        int alive = 0;
        for (auto t : g.tiles)
            if (v.camp_bits.get(t)) {
                try {
                    auto c = bc::vision::camp_at(t);
                    hp += c.health();
                    ++alive;
                } catch (const std::exception& e) { warn("camp", e); }
            }
        if (alive > 0) {
            g.capture_turns = 0;
            g.hp_seen = hp;
            g.respawn_until = -1;
            g.reward = false;
            continue;
        }
        // Nothing alive: reward pending or respawning.
        try {
            bc::map::camp_group cg = bc::map::camp(g.tiles[0]).camp_group();
            const auto st = cg.state();
            if (std::holds_alternative<bc::map::camp_group_respawning>(st)) {
                g.respawn_until = v.round + std::get<bc::map::camp_group_respawning>(st).timer;
                g.capture_turns = 0;
                g.reward = false;
                g.hp_seen = -1;
            } else if (std::holds_alternative<bc::map::camp_group_reward_available>(st)) {
                g.reward = true;
                g.respawn_until = -1;
                g.hp_seen = 0;
            }
        } catch (const std::exception& e) { warn("group", e); }
    }
}

// ---------------------------------------------------------------- shopping
void shop() {
    auto& me = bc::self();
    try {
        const auto lock = bc::consumable_type::altar_lock;
        if (me.consumables()[lock].count() == 0 && me.gold_count() >= 10 && me.can_buy(lock)) me.buy(lock);
    } catch (const std::exception& e) { warn("lock buy", e); }
    for (int guard = 0; guard < 20 && me.gold_count() >= 30; ++guard) {
        bc::item_type item = bc::item_type::power_shard_of_strength;
        if (shards < kJungleShards) item = bc::item_type::jungle_shard_of_strength;
        else if ((shards - 5) % 3 == 2) item = bc::item_type::power_shard_of_health;
        if (!me.can_buy(item)) break;
        me.buy(item);
        ++shards;
    }
}

// ---------------------------------------------------------------- abilities
bool cast_on_enemy(const Unit& t, bool allow_excl) {
    auto& me = bc::self();
    if (!me.can_cast()) return false;
    const auto abilities = me.abilities();
    for (int s = 0; s < 2; ++s) {
        const auto& k = kit[s];
        if (!k.present || !k.enemies) continue;
        if (k.kind != bc::ability_kind::damage && k.kind != bc::ability_kind::cc && k.kind != bc::ability_kind::aoe_damage)
            continue;
        if (k.excl && (v.moved || !allow_excl)) continue;
        if (dist(v.me, t.pos) > static_cast<int>(k.range)) continue;
        try {
            if (!abilities[s].is_ready()) continue;
            bool ok;
            if (k.kind == bc::ability_kind::aoe_damage) ok = abilities[s].use(t.pos);
            else ok = abilities[s].use(bc::vision::champion_at(t.pos));
            if (ok) return true;
        } catch (const std::exception& e) { warn("cast", e); }
    }
    return false;
}

bool cast_on_camp(vec2 pos, bool allow_excl) {
    auto& me = bc::self();
    if (!me.can_cast()) return false;
    const auto abilities = me.abilities();
    for (int s = 0; s < 2; ++s) {
        const auto& k = kit[s];
        if (!k.present || !k.camps) continue;
        if (k.kind != bc::ability_kind::damage && k.kind != bc::ability_kind::cc && k.kind != bc::ability_kind::aoe_damage)
            continue;
        if (k.excl && (v.moved || !allow_excl)) continue;
        if (dist(v.me, pos) > static_cast<int>(k.range)) continue;
        try {
            if (!abilities[s].is_ready()) continue;
            bool ok;
            if (k.kind == bc::ability_kind::aoe_damage) ok = abilities[s].use(pos);
            else ok = abilities[s].use(bc::vision::camp_at(pos));
            if (ok) return true;
        } catch (const std::exception& e) { warn("camp cast", e); }
    }
    return false;
}

bool self_cast(bc::ability_kind kind) {
    auto& me = bc::self();
    if (!me.can_cast()) return false;
    const auto abilities = me.abilities();
    for (int s = 0; s < 2; ++s) {
        if (!kit[s].present || kit[s].kind != kind) continue;
        if (kit[s].excl && v.moved) continue;
        try {
            if (!abilities[s].is_ready()) continue;
            if (abilities[s].use()) return true;
        } catch (const std::exception& e) { warn("self cast", e); }
    }
    return false;
}

// Dash to a free tile next to (within attack range of) a target position.
bool dash_towards(vec2 target) {
    auto& me = bc::self();
    if (!me.can_cast()) return false;
    const auto abilities = me.abilities();
    for (int s = 0; s < 2; ++s) {
        if (!kit[s].present || kit[s].kind != bc::ability_kind::dash) continue;
        if (kit[s].excl && v.moved) continue;
        const int r = static_cast<int>(kit[s].range);
        try {
            if (!abilities[s].is_ready()) continue;
        } catch (const std::exception& e) { warn("dash ready", e); continue; }
        const int want = me.attack_range();
        vec2 best{-1, -1};
        int best_score = 1 << 20;
        for (int dy = -r; dy <= r; ++dy)
            for (int dx = -r; dx <= r; ++dx) {
                const vec2 p{v.me.x + dx, v.me.y + dy};
                if (p == v.me || !inb(p) || v.blocked_bits.get(p)) continue;
                const int d = dist(p, target);
                if (d > want) continue;
                // A legal dash path: the middle tile must not be a wall when travelling 2.
                if (dist(p, v.me) == 2) {
                    const vec2 mid{v.me.x + (dx > 0) - (dx < 0), v.me.y + (dy > 0) - (dy < 0)};
                    if (wall_bits.get(mid)) continue;
                }
                const int score = d * 4 + dist(p, v.me);
                if (score < best_score) best_score = score, best = p;
            }
        if (best.x < 0) return false;
        try {
            if (abilities[s].use(best)) {
                v.me = best;
                return true;
            }
            v.move_failed = true;
        } catch (const std::exception& e) { warn("dash", e); }
        return false;
    }
    return false;
}

// Basic attack on an enemy champion at pos, followed by an attack reset and a second hit.
bool hit_enemy(vec2 pos) {
    auto& me = bc::self();
    if (!me.can_attack()) return false;
    if (dist(v.me, pos) > me.attack_range()) return false;
    try {
        auto c = bc::vision::champion_at(pos);
        me.attack(c);
        v.attacked = true;
    } catch (const std::exception& e) { warn("hit", e); return false; }
    try {
        if (bc::vision::has_champion(pos) && self_cast(bc::ability_kind::attack_reset)) {
            if (bc::vision::has_champion(pos)) {
                auto c = bc::vision::champion_at(pos);
                if (c.health() > 0 && me.can_attack()) me.attack(c);
            }
        }
    } catch (const std::exception& e) { warn("hit2", e); }
    return true;
}

bool hit_camp(vec2 pos) {
    auto& me = bc::self();
    if (!me.can_attack()) return false;
    if (dist(v.me, pos) > me.attack_range()) return false;
    try {
        auto c = bc::vision::camp_at(pos);
        me.attack(c);
        v.attacked = true;
    } catch (const std::exception& e) { warn("hitc", e); return false; }
    try {
        if (bits(bc::vision::awake_camp_tiles()).get(pos) && self_cast(bc::ability_kind::attack_reset)) {
            {
                auto c = bc::vision::camp_at(pos);
                if (c.health() > 0 && me.can_attack()) me.attack(c);
            }
        }
    } catch (const std::exception& e) { warn("hitc2", e); }
    return true;
}

// ---------------------------------------------------------------- sightings
void write_sighting(const Unit& u, bool fighting, bool alone) {
    mem_set(kSight + 0, static_cast<std::uint8_t>(u.pos.x));
    mem_set(kSight + 1, static_cast<std::uint8_t>(u.pos.y));
    mem_set(kSight + 2, static_cast<std::uint8_t>(v.round & 255));
    std::uint8_t info = static_cast<std::uint8_t>(std::min(63, u.gems / 4));
    if (fighting) info |= 64;
    if (alone) info |= 128;
    mem_set(kSight + 3, info);
}

struct Sighting {
    bool fresh = false;
    vec2 pos;
    int gems = 0;
    bool fighting = false, alone = false;
};
Sighting read_sighting() {
    Sighting s;
    const int age = (v.round - mem_get(kSight + 2)) & 255;
    if (age > 4) return s;
    s.pos = {mem_get(kSight + 0), mem_get(kSight + 1)};
    if (!inb(s.pos)) return s;
    const auto info = mem_get(kSight + 3);
    s.gems = (info & 63) * 4;
    s.fighting = info & 64;
    s.alone = info & 128;
    s.fresh = true;
    return s;
}

// ---------------------------------------------------------------- altars
int nearest_altar(vec2 from, bool skip_locked) {
    int best = -1, bd = 1 << 20;
    for (std::size_t i = 0; i < altars.size(); ++i) {
        if (skip_locked && altar_enemy_lock[i] >= 0 && v.round - altar_enemy_lock[i] < 150) continue;
        const int d = dist(from, altars[i]);
        if (d < bd) bd = d, best = static_cast<int>(i);
    }
    return best;
}

void update_altars() {
    for (std::size_t i = 0; i < altars.size(); ++i) {
        const vec2 a = altars[i];
        if (!v.vis.get(a)) continue;
        try {
            bool enemy_lock = false;
            if (bc::vision::has_altar_lock(a)) {
                for (const auto& l : bc::vision::altar_locks(team::enemy))
                    if (l.position() == a) enemy_lock = true;
            }
            altar_enemy_lock[i] = enemy_lock ? v.round : -1;
        } catch (const std::exception& e) { warn("altar", e); }
        for (const auto& e : v.enemies)
            if (dist(e.pos, a) <= 1) ++altar_enemy_use[i];
    }
}

// Place our lock on any altar within reach that has none (blocks only enemy tributes).
void place_locks() {
    auto& me = bc::self();
    try {
        const auto& lock = me.consumables()[bc::consumable_type::altar_lock];
        if (lock.count() == 0) return;
        for (std::size_t i = 0; i < altars.size(); ++i) {
            const vec2 a = altars[i];
            if (dist(v.me, a) > 2 || !v.vis.get(a)) continue;
            if (bc::vision::has_altar_lock(a)) continue;
            if (lock.use(a)) return;
        }
    } catch (const std::exception& e) { warn("lock", e); }
}

// ---------------------------------------------------------------- wards
void place_ward(vec2 goal) {
    auto& me = bc::self();
    try {
        if (me.ward_cooldown() > 0) return;
        if (v.round - last_ward_round < 10) return;
        // Prefer an unseen bush tile in reach (bushes hide ambushers), else the tile nearest the goal.
        const Bits ward_bits = bits(bc::vision::ward_tiles());
        vec2 best{-1, -1};
        int best_score = -(1 << 20);
        for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -2; dx <= 2; ++dx) {
                const vec2 p{v.me.x + dx, v.me.y + dy};
                if (!inb(p) || wall_bits.get(p) || hole_bits.get(p) || v.blocked_bits.get(p)) continue;
                if (p == v.me || ward_bits.get(p)) continue;
                int score = -dist(p, goal) * 2;
                if (bush_bits.get(p) && !v.vis.get(p)) score += 6;
                if (score > best_score) best_score = score, best = p;
            }
        if (best.x < 0) return;
        if (me.place_ward(best)) last_ward_round = v.round;
    } catch (const std::exception& e) { warn("ward", e); }
}

// ---------------------------------------------------------------- fighting
// Square-law odds over the champions near us: (Σhp)·(Σdps) on each side.
struct Odds { float ours = 0, theirs = 0; };
Odds fight_odds(vec2 centre) {
    auto& me = bc::self();
    Odds o;
    float hp = me.health(), dps = me.strength() + me.focus() * 0.25f;
    for (const auto& a : v.allies)
        if (dist(a.pos, centre) <= 7) hp += a.hp, dps += dps_of(a);
    o.ours = hp * dps;
    float ehp = 0, edps = 0;
    for (const auto& e : v.enemies)
        if (dist(e.pos, centre) <= 8) {
            ehp += e.hp;
            edps += e.channel ? dps_of(e) * 0.2f : dps_of(e);
        }
    o.theirs = ehp * edps;
    return o;
}

// Returns true when the turn was spent on PvP (fight or flight).
bool handle_enemies() {
    auto& me = bc::self();
    if (v.enemies.empty()) return false;
    // Nearest threat distance.
    int nearest = 1 << 20;
    for (const auto& e : v.enemies) nearest = std::min(nearest, dist(e.pos, v.me));
    const Odds o = fight_odds(v.me);
    const float hp_frac = me.health() / std::max(1.0f, me.max_health());
    // Pick the best target: killable, carrying gems, channelling.
    const Unit* target = nullptr;
    float best = -1e9f;
    const float my_dps = me.strength() + me.focus() * 0.25f;
    for (const auto& e : v.enemies) {
        const int d = dist(e.pos, v.me);
        float score = -e.hp / std::max(1.0f, my_dps) - d * 2.0f + e.gems * 0.3f + (e.channel ? 8.0f : 0.0f);
        if (score > best) best = score, target = &e;
    }
    if (!target) return false;
    const bool alone = v.enemies.size() == 1;
    write_sighting(*target, true, alone);
    float need = kFightNeed;
    if (target->channel) need = 0.5f;
    if (alone && target->hp < my_dps * 3) need *= 0.6f;
    if (target->gems >= 20) need *= 0.85f;
    if (hp_frac < 0.3f) need *= 1.6f;
    // Alone, only easy prey is worth a duel: lineage champions out-scale ours on gold.
    int mates = 0;
    for (const auto& a : v.allies)
        if (dist(a.pos, v.me) <= 6) ++mates;
    if (kAloneFactor != 1.0f && mates == 0 && !target->channel && target->hp > my_dps * 3) need *= kAloneFactor;
    const bool favourable = o.ours >= o.theirs * need;
    bool fight = favourable;
    const bool reachable = dist(target->pos, v.me) <= me.attack_range() + (ranged ? 0 : 1);
    if (fight && !target->channel && !own_side(target->pos, kChaseMargin) && !reachable) fight = false;
    if (fight && dist(target->pos, enemy_spawn_center) <= 6 && !reachable) fight = false;
    // Stronger here but the target is out of bounds: carry on with our own business.
    if (favourable && !fight) return false;
    if (v.round % 25 == 2) {
        debug_block({ std::cout << "  odds " << o.ours << " vs " << o.theirs << " need " << need << " near " << nearest << " fight " << fight << " t=(" << target->pos.x << "," << target->pos.y << ") hp " << target->hp << '\n'; });
    }
    if (fight) {
        const Unit t = *target;
        // Lock the altar under a tributing enemy (placing a lock cancels its channel).
        if (t.channel) place_locks();
        const int d = dist(v.me, t.pos);
        const int range = me.attack_range();
        if (d <= range) {
            cast_on_enemy(t, true);
            if (bc::vision::has_champion(t.pos)) hit_enemy(t.pos);
            return true;
        }
        // Out of attack range: spells that reach, then close in.
        if (!ranged) {
            if (d <= range + 2 && dash_towards(t.pos)) {
                cast_on_enemy(t, false);
                if (bc::vision::has_champion(t.pos)) hit_enemy(t.pos);
                return true;
            }
            bc::bitboard g = ring_around({t.pos}, range);
            step_towards(g);
            if (bc::vision::has_champion(t.pos)) {
                cast_on_enemy(t, false);
                hit_enemy(t.pos);
            }
            return true;
        }
        // Ranged: a damaging spell from here if it reaches, else step closer.
        if (cast_on_enemy(t, true)) return true;
        bc::bitboard g = ring_around({t.pos}, range);
        step_towards(g);
        return true;
    }
    // Not favourable: leave if anyone can reach us soon.
    if (nearest <= kFleeRadius) {
        // Parting shots from where we stand never cost a step for melee; ranged would lose the step.
        if (!ranged) {
            for (const auto& e : v.enemies)
                if (dist(e.pos, v.me) <= 1) { hit_enemy(e.pos); break; }
        }
        bc::bitboard home;
        for (auto p : ally_spawn_tiles) home.set(p);
        step_towards(home);
        if (!v.moved) {
            // Cornered: fight back.
            for (const auto& e : v.enemies)
                if (dist(e.pos, v.me) <= me.attack_range()) { cast_on_enemy(e, true); hit_enemy(e.pos); break; }
        }
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- farming
float squad_dps_est() { return my_base_dps * static_cast<float>(std::max(1, squad_size)); }

int choose_target() {
    auto& me = bc::self();
    const float gold_w = v.round < 700 ? 0.6f : (v.round < 1300 ? 0.35f : 0.15f);
    const float dps = std::max(20.0f, squad_dps_est());
    const float hp_pool = me.max_health() * static_cast<float>(std::max(1, squad_size));
    int best = -1;
    float best_score = 0;
    const int other_target = squads == 2 ? static_cast<int>(mem_get(1 - squad)) - 1 : -1;
    for (std::size_t gi = 0; gi < groups.size(); ++gi) {
        const auto& g = groups[gi];
        // Travel estimate without a per-turn BFS: Chebyshev distance, or the difference of walking distances
        // from our spawn when that is larger (a lower bound that sees around walls).
        int travel = 1 << 15;
        const int here = spawn_field[v.me.y * W + v.me.x];
        for (int idx : g.approach) {
            const int f = spawn_field[idx];
            if (f == 0xFFFF) continue;
            const vec2 t{idx % W, idx / W};
            const int est = std::max(dist(t, v.me), here == 0xFFFF ? 0 : std::abs(f - here));
            if (est < travel) travel = est;
        }
        if (travel >= (1 << 15)) continue;
        if (g.skip_until > v.round) continue;
        if (v.round + travel > 1990) continue;
        float wait = 0;
        if (g.respawn_until >= 0) wait = std::max(0.0f, static_cast<float>(g.respawn_until - v.round - travel));
        const float hp = g.reward ? 0 : (g.hp_seen >= 0 ? g.hp_seen : g.hp_full);
        const float clear = hp / dps;
        const float damage = g.str * clear * 0.6f;
        if (damage > hp_pool * kDangerPool) continue;
        if (g.str * kTankRounds > me.max_health() * static_cast<float>(std::max(1, squad_size))) continue;
        const float value = g.gems + g.gold * gold_w;
        if (value <= 0) continue;
        float score = value / (travel + wait * 2.0f + clear + 6.0f + (g.reward ? 0 : 5.0f));
        // Deep enemy territory is where their squads roam.
        const int de = dist(g.center, enemy_spawn_center), da = dist(g.center, ally_spawn_center);
        if (de < da) score *= 0.75f;
        if (static_cast<int>(gi) == target_group) score *= 1.25f;
        if (static_cast<int>(gi) == other_target) score *= kOtherSquadFactor;
        if (score > best_score) best_score = score, best = static_cast<int>(gi);
    }
    return best;
}


// Attack camps in range: awake ones first, then the target group's.
bool fight_camps() {
    auto& me = bc::self();
    const int range = me.attack_range();
    vec2 best{-1, -1};
    float best_score = -1e9f;
    for (int dy = -range; dy <= range; ++dy)
        for (int dx = -range; dx <= range; ++dx) {
            const vec2 p{v.me.x + dx, v.me.y + dy};
            if (!v.camp_bits.get(p) || !v.vis.get(p)) continue;
            try {
                auto c = bc::vision::camp_at(p);
                const bool awake = c.is_awake();
                const int gi = group_at[c.spawn_position().y * W + c.spawn_position().x];
                if (!awake && gi != target_group) continue;
                const float hp = c.health();
                if (hp <= 0) continue;
                float score = -hp / 1000.0f + (awake ? 5.0f : 0.0f) + c.strength() / 50.0f;
                if (score > best_score) best_score = score, best = p;
            } catch (const std::exception& e) { warn("campscan", e); }
        }
    if (best.x < 0) return false;
    if (v.enemies.empty()) cast_on_camp(best, true);
    hit_camp(best);
    return true;
}

}  // namespace

const char* g_mode = "";
int mode_counts[12] = {};
const char* mode_names[12] = {"pvp","recall","bank","answer","campfight","capture","respawn","gather","approach","ambush","heal","other"};
void set_mode(int m) { g_mode = mode_names[m]; ++mode_counts[m]; }

std::uint64_t sec_max[9] = {};
std::uint64_t sec_last = 0;
void sec(int i) {
    if (!kFuelProbe) return;
    const std::uint64_t left = bc::fuel_remaining();
    if (sec_last > left && sec_last - left > sec_max[i]) sec_max[i] = sec_last - left;
    sec_last = left;
}
struct FuelProbe {
    static inline std::uint64_t max_used = 0;
    static inline int over = 0;
    ~FuelProbe() {
        const std::uint64_t left = bc::fuel_remaining();
        const std::uint64_t used = left < 100000 ? 100000 - left : 0;
        if (used > max_used) max_used = used;
        if (used > 90000) ++over;
        if (kFuelProbe && bc::round_number() % 250 == 0) {
            std::cout << "fuel max " << max_used << " over " << over << " sec";
            for (auto x : sec_max) std::cout << ' ' << x;
            std::cout << '\n';
        }
    }
};

void Bot::play_turn() {
    FuelProbe probe;
    struct EndSec { ~EndSec() { sec(6); } } end_sec;
    struct ChooseTimer {};
    if (!g_init) {
        init_static();
        init_self();
        g_init = true;
    }
    auto& me = bc::self();
    if (!me.is_alive()) {
        shop();
        heal_mode = false;
        return;
    }
    if (me.is_recalling() || me.is_submitting_gems() || me.is_teleporting()) {
        if (me.is_recalling() && ally_spawn_bits.get(me.position())) {
            // Already home; nothing to wait for.
        }
        return;
    }
    if (!me.can_move() && !me.can_attack() && !me.can_cast()) return;
    sec_last = bc::fuel_remaining();
    read_view();
    sec(0);
    if (v.round % 25 == 1) {
        debug_block({ std::cout << "r" << v.round << " me" << my_index << " (" << v.me.x << "," << v.me.y << ") tg=" << target_group << " hp=" << me.health() << " gems=" << me.gem_count() << " en=" << v.enemies.size() << '\n'; });
    }
    update_groups();
    sec(1);
    update_altars();
    sec(2);

    const bool at_spawn = ally_spawn_bits.get(v.me);
    if (at_spawn || me.passive() == bc::passive_type::remote_shop) shop();
    place_locks();

    const float hp = me.health(), max_hp = std::max(1.0f, me.max_health());
    const int gems = static_cast<int>(me.gem_count());

    // Heal on spawn before heading out again.
    if (at_spawn && hp < max_hp * kHealHp && v.enemies.empty()) {
        heal_mode = true;
        set_mode(10);
        me.set_priority(team_size - my_index);
        return;
    }
    heal_mode = false;

    // PvP first.
    if (v.round % 200 == 0) {
        debug_block({ std::cout << "modes me" << my_index; for (int i = 0; i < 12; ++i) std::cout << ' ' << mode_names[i] << '=' << mode_counts[i]; std::cout << '\n'; });
    }
    sec(3);
    const bool pvp = handle_enemies();
    sec(4);
    if (pvp) { set_mode(0); return; }

    // Recall when hurt and nothing is close enough to interrupt.
    bool camp_threat = false;
    float camp_dps = 0;  // awake camps that can hit us now (they hit the nearest champion)
    for (int dy = -5; dy <= 5; ++dy)
        for (int dx = -5; dx <= 5; ++dx) {
            const vec2 p{v.me.x + dx, v.me.y + dy};
            if (!v.camp_bits.get(p)) continue;
            try {
                auto c = bc::vision::camp_at(p);
                const int dc = dist(p, v.me);
                if (c.is_awake() && dc <= c.attack_range()) {
                    camp_threat = true;
                    bool nearest = true;
                    for (const auto& a : v.allies)
                        if (dist(a.pos, p) < dc) { nearest = false; break; }
                    if (nearest) camp_dps += c.strength();
                }
            } catch (const std::exception& e) { warn("threat", e); }
        }
    if (!at_spawn && (hp < max_hp * kRetreatHp || hp < camp_dps * kCampRetreatRounds)) {
        set_mode(1);
        if (!camp_threat && v.enemies.empty()) {
            try { me.recall(); } catch (const std::exception& e) { warn("recall", e); }
            return;
        }
        // Walk out of camp range first.
        bc::bitboard home;
        for (auto p : ally_spawn_tiles) home.set(p);
        step_towards(home);
        return;
    }

    // Recall to turn a pile of gold into shards (Tavi shops anywhere).
    if (!at_spawn && gems == 0 && !camp_threat && v.enemies.empty() && me.passive() != bc::passive_type::remote_shop
        && static_cast<int>(me.gold_count()) >= kShopGold && dist(v.me, ally_spawn_center) > 8
        && !(target_group >= 0 && groups[target_group].reward)) {
        set_mode(1);
        try { me.recall(); } catch (const std::exception& e) { warn("shop recall", e); }
        return;
    }

    // Bank gems.
    const int team_score = static_cast<int>(bc::global_submitted_gems(team::ally));
    const bool winning_bag = team_score + gems >= 1000;
    const bool late = v.round > 1850;
    int altar_i = nearest_altar(v.me, true);
    if (altar_i < 0) altar_i = nearest_altar(v.me, false);
    const int altar_d = altar_i >= 0 ? dist(v.me, altars[altar_i]) : 1 << 20;
    bool bank = gems > 0 && (gems >= kBankGems || winning_bag || late || (gems >= 10 && altar_d <= 12));
    if (!bank && gems > 0 && target_group < 0) bank = true;
    if (bank && !camp_threat && altar_i >= 0) {
        set_mode(2);
        // Adjacent to any altar: tribute there (a locked altar takes the hits instead).
        for (std::size_t i = 0; i < altars.size(); ++i)
            if (dist(v.me, altars[i]) <= bc::constants::submit_gems_range) {
                try { me.submit_gems(altars[i]); } catch (const std::exception& e) { warn("submit", e); }
                return;
            }
        std::vector<vec2> targets;
        for (std::size_t i = 0; i < altars.size(); ++i)
            if (!(altar_enemy_lock[i] >= 0 && v.round - altar_enemy_lock[i] < 150)) targets.push_back(altars[i]);
        if (targets.empty()) targets = altars;
        bc::bitboard g = ring_around(targets, 1);
        long key = 2;
        for (auto t : targets) key = key * 31 + t.x * 128 + t.y;
        if (step_towards(g)) {
            for (std::size_t i = 0; i < altars.size(); ++i)
                if (dist(v.me, altars[i]) <= bc::constants::submit_gems_range) {
                    place_locks();
                    break;
                }
            place_ward(altars[altar_i]);
            return;
        }
    }

    // Answer a fresh sighting: allies fighting, or an enemy carrying gems close by.
    const Sighting s = read_sighting();
    if (s.fresh && hp > max_hp * 0.55f && gems < 30) {
        const int d = dist(v.me, s.pos);
        if (own_side(s.pos, kChaseMargin) && ((s.fighting && d <= 14) || (s.gems >= 12 && d <= 12))) {
            bc::bitboard g = ring_around({s.pos}, 2);
            set_mode(3);
            if (step_towards(g)) {
                place_ward(s.pos);
                return;
            }
        }
    }

    // Fight camps in reach.
    if (fight_camps()) {
        set_mode(4);
        place_ward(groups.empty() || target_group < 0 ? v.me : groups[target_group].center);
        return;
    }

    sec(5);
    // Choose a camp group (shared per squad).

    int shared = kSquads == 0 ? -1 : static_cast<int>(mem_get(squad)) - 1;
    if (shared >= static_cast<int>(groups.size())) shared = -1;
    if (shared >= 0) {
        const auto& g = groups[shared];
        const bool gone = (g.respawn_until >= 0 && g.respawn_until - v.round > 25) || g.skip_until > v.round;
        if (gone) shared = -1;
    }
    if (target_group >= 0) {
        const auto& g = groups[target_group];
        if ((g.respawn_until >= 0 && g.respawn_until - v.round > 25) || g.skip_until > v.round) target_group = -1;
    }
    if (shared >= 0) target_group = shared;
    if (target_group < 0 || v.round % 20 == 0) {
        sec(5);
        const int pick = choose_target();
        sec(8);
        if (pick != target_group) {
            target_group = pick;
            wait_turns = 0;
        }
        if (kSquads != 0) mem_set(squad, static_cast<std::uint8_t>(target_group + 1));
    }

    sec(7);
    if (target_group >= 0) {
        auto& g = groups[target_group];
        if (v.round % 25 == 1) {
            debug_block({ std::cout << "  tgt c=(" << g.center.x << "," << g.center.y << ") reward=" << g.reward << " ru=" << g.respawn_until << " hp=" << g.hp_seen << " allies=" << v.allies.size() << " wait=" << wait_turns << '\n'; });
        }
        // Capture: stand on a spawn tile of a defeated group.
        if (g.reward && ++g.capture_turns > kCaptureTimeout) {
            g.capture_turns = 0;
            g.skip_until = v.round + 40;
            target_group = -1;
            if (kSquads != 0) mem_set(squad, 0);
            return;
        }
        if (g.reward) {
            set_mode(5);
            for (auto t : g.tiles)
                if (t == v.me) {
                    place_ward(g.center);
                    return;
                }
            // One capturer is enough: the others move on to the next group.
            bool ally_capturing = false;
            for (const auto& a : v.allies)
                for (auto t : g.tiles)
                    if (a.pos == t) ally_capturing = true;
            if (ally_capturing && kSplitCapture) {
                g.skip_until = v.round + 8;
                const int next = choose_target();
                g.skip_until = -1;
                if (next >= 0 && next != target_group) {
                    auto& n = groups[next];
                    bc::bitboard goal = ring_around(n.tiles, ranged ? 2 : 1);
                    step_towards(goal);
                    place_ward(n.center);
                    return;
                }
            }
            bc::bitboard goal;
            for (auto t : g.tiles)
                if (!v.blocked_bits.get(t)) goal.set(t);
            if (!goal.is_empty()) step_towards(goal);
            place_ward(g.center);
            return;
        }
        // Wait for a respawn just outside camp range.
        if (g.respawn_until >= 0 && g.respawn_until > v.round) {
            set_mode(6);
            // Never stand on a spawn tile: an occupied tile delays the whole group's respawn.
            bc::bitboard goal;
            for (auto p : g.wait_ring) goal.set(p);
            if (g.wait_ring.empty()) goal = ring_around(g.tiles, std::max(1, g.range) + 1);
            if (!goal.is_set(v.me)) step_towards(goal);
            place_ward(g.center);
            return;
        }
        // Gather before a dangerous group: wait for squadmates near the edge of its range.
        const float my_hp = me.health();
        const float clear = (g.hp_seen >= 0 ? g.hp_seen : g.hp_full) / std::max(20.0f, my_base_dps);
        const bool dangerous = g.str * clear * 0.6f > my_hp * 0.8f;
        int dmin = 1 << 20;
        for (auto t : g.tiles) dmin = std::min(dmin, dist(t, v.me));
        if (dangerous && squad_size > 1 && dmin <= g.range + 2 && wait_turns < kGatherTurns) {
            int near = 0;
            for (const auto& a : v.allies)
                if (dist(a.pos, v.me) <= 4) ++near;
            if (near < squad_size - 1) {
                set_mode(7);
                ++wait_turns;
                place_ward(g.center);
                return;
            }
        }
        // Approach: melee to adjacent tiles, ranged to within range 2 for line of sight.
        const int r = ranged ? kRangedApproach : 1;
        std::vector<vec2> live;
        bool any_seen = false;
        for (auto t : g.tiles) {
            if (v.vis.get(t)) any_seen = true;
            if (v.camp_bits.get(t) || !v.vis.get(t)) live.push_back(t);
        }
        if (live.empty()) live = g.tiles;
        (void)any_seen;
        bc::bitboard goal = ring_around(live, r);
        set_mode(8);
        step_towards(goal);
        if (v.moved) fight_camps();
        place_ward(g.center);
        return;
    }

    // Nothing worth farming: bank, or lie in wait in a bush near the altar the enemy uses.
    if (gems > 0 && altar_i >= 0) {
        bc::bitboard g = ring_around({altars[altar_i]}, 1);
        step_towards(g);
        return;
    }
    set_mode(9);
    int amb = -1, best_use = -1;
    for (std::size_t i = 0; i < altars.size(); ++i) {
        const int score = altar_enemy_use[i] * 10 - dist(altars[i], v.me);
        if (score > best_use) best_use = score, amb = static_cast<int>(i);
    }
    if (amb >= 0) {
        bc::bitboard goal;
        const vec2 a = altars[amb];
        for (int dy = -3; dy <= 3; ++dy)
            for (int dx = -3; dx <= 3; ++dx) {
                const vec2 p{a.x + dx, a.y + dy};
                if (inb(p) && bush_bits.get(p) && !terrain_bits.get(p)) goal.set(p);
            }
        if (goal.is_empty()) goal = ring_around({a}, 2);
        if (!goal.is_set(v.me)) step_towards(goal);
        place_ward(a);
    }
}
