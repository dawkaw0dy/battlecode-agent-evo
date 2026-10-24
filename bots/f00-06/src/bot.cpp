// Deathball champion logic. main.cpp dispatches here after the draft.
//
// Shared memory (8 bytes): [leader id+1, leader x, leader y, heartbeat round, target group+1, mode,
// hunt x, hunt y]. The leader writes every byte each turn; followers only read, except to claim a
// stale leadership.
#include <cambc.h>
#include "bot.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <memory>
#include <utility>
#include <span>
#include <string>
#include <variant>
#include <vector>

using cambc::team;
using cambc::vec2;

namespace {

// ---- tuning -----------------------------------------------------------------------------------
constexpr int kBankGems = 12;        // leader banks when it carries this many (team carries ~5x)
constexpr int kNearAltarGems = 4;    // bank smaller bags when an altar is this close
constexpr int kNearAltarDist = 7;
constexpr int kShopGold = 150;       // team recall to shop once the leader holds this much gold
constexpr int kEndgameRounds = 140;  // bank everything this many rounds before the limit
constexpr int kFollowDist = 2;
constexpr int kGatherDist = 3;
constexpr int kGatherWait = 8;
constexpr int kLeadAhead = 3;
constexpr float kHuntBalance = 1.1f;    // chase only with this much more health nearby
constexpr float kRetreatBalance = 0.55f;  // fall back towards spawn below this
constexpr int kGolemRound = 400;
constexpr int kMonoRound = 700;
constexpr int kStallRounds = 90;
constexpr float kSoloRecallHp = 0.25f;

enum Mode : std::uint8_t { kTravel = 0, kFight = 1, kCapture = 2, kBank = 3, kShop = 4 };

int W = 0, H = 0;

int dist(vec2 a, vec2 b) { return static_cast<int>(a.chebyshev_distance(b)); }
bool in_bounds(vec2 p) { return p.x >= 0 && p.y >= 0 && p.x < W && p.y < H; }

constexpr std::array<vec2, 8> kDirs{vec2{0, 1}, vec2{1, 1}, vec2{1, 0}, vec2{1, -1},
                                    vec2{0, -1}, vec2{-1, -1}, vec2{-1, 0}, vec2{-1, 1}};

// Design values per camp_type: wolf, elk, gem cluster, gold cluster, monolith, golem.
constexpr std::array<float, 6> kCampHp{1350, 2400, 120, 120, 20000, 5000};
constexpr std::array<float, 6> kCampStr{20, 60, 0, 0, 200, 120};
constexpr std::array<int, 6> kCampRange{3, 4, 0, 0, 5, 4};

struct Group {
    std::vector<vec2> tiles;
    cambc::bitboard mask;
    vec2 center{};
    int gems = 0, gold = 0;
    bool mono = false, golem = false;
    float hp = 0, str = 0;
    cambc::distance_field field;
    cambc::bitboard reach;
    bool ready = false;
    bool reachable = true;
    int respawn_until = -1;  // round the group is expected back
    int skip_until = -1;
};

struct World {
    std::vector<cambc::map::camp_group> handles;
    std::vector<Group> groups;
    std::vector<std::int16_t> group_of;  // spawn tile -> group index
    cambc::bitboard terrain;             // never walkable for us
    std::vector<vec2> altars;
    cambc::distance_field altar_field;
    cambc::bitboard altar_reach;
    bool altar_ready = false;
    cambc::distance_field spawn_field;
    cambc::bitboard spawn_reach;
    bool spawn_ready = false;
    std::vector<int> field_order;
    std::size_t next_field = 0;
};
std::unique_ptr<World> world;

void init_world() {
    world = std::make_unique<World>();
    auto& w = *world;
    W = cambc::map::width();
    H = cambc::map::height();
    w.terrain = cambc::map::walls() | cambc::map::holes() | cambc::map::altars()
        | cambc::map::spawn_region(team::enemy);
    w.altars = cambc::map::altars().positions();
    w.group_of.assign(static_cast<std::size_t>(W * H), -1);
    w.handles = cambc::map::camp_groups();
    const auto spawn = cambc::map::spawn_region(team::ally).positions();
    const vec2 home = spawn.empty() ? vec2{0, 0} : spawn[0];
    for (std::size_t i = 0; i < w.handles.size(); ++i) {
        const auto& h = w.handles[i];
        Group g;
        g.mask = h.spawn_tiles();
        g.tiles = g.mask.positions();
        g.gems = static_cast<int>(h.gem_value());
        g.gold = static_cast<int>(h.gold_value());
        long sx = 0, sy = 0;
        for (const auto& c : h.camps()) {
            const auto k = static_cast<int>(c.kind());
            if (k >= 0 && k < 6) {
                g.hp += kCampHp[k];
                g.str += kCampStr[k];
            }
            if (c.kind() == cambc::camp_type::monolith) g.mono = true;
            if (c.kind() == cambc::camp_type::golem) g.golem = true;
        }
        for (auto t : g.tiles) {
            sx += t.x;
            sy += t.y;
            w.group_of[static_cast<std::size_t>(t.y * W + t.x)] = static_cast<std::int16_t>(i);
        }
        if (!g.tiles.empty())
            g.center = vec2{static_cast<int>(sx / static_cast<long>(g.tiles.size())),
                            static_cast<int>(sy / static_cast<long>(g.tiles.size()))};
        w.groups.push_back(std::move(g));
        w.field_order.push_back(static_cast<int>(i));
    }
    std::sort(w.field_order.begin(), w.field_order.end(), [&](int a, int b) {
        return dist(w.groups[a].center, home) < dist(w.groups[b].center, home);
    });
}

// Spend spare fuel on static distance fields, one at a time.
void progress_fields(std::uint64_t reserve) {
    auto& w = *world;
    while (cambc::fuel_remaining() > reserve) {
        if (!w.altar_ready) {
            cambc::bitboard roots;
            roots.clear();
            for (auto a : w.altars)
                for (auto d : kDirs) {
                    const vec2 p = a + d;
                    if (in_bounds(p) && !w.terrain.is_set(p)) roots.set(p);
                }
            w.altar_field.compute(w.terrain, roots, w.altar_reach);
            w.altar_ready = true;
            continue;
        }
        if (!w.spawn_ready) {
            w.spawn_field.compute(w.terrain, cambc::map::spawn_region(team::ally), w.spawn_reach);
            w.spawn_ready = true;
            continue;
        }
        if (w.next_field >= w.field_order.size()) return;
        auto& g = w.groups[w.field_order[w.next_field++]];
        if (g.tiles.empty()) continue;
        g.field.compute(w.terrain, g.mask, g.reach);
        g.ready = true;
        g.reachable = !(g.reach & cambc::map::spawn_region(team::ally)).is_empty();
    }
}

int group_distance(const Group& g, vec2 p) {
    if (g.ready && g.reach.is_set(p)) return g.field.get(p);
    return dist(g.center, p) * 3 / 2 + 2;
}

// ---- shared memory -----------------------------------------------------------------------------
struct Shared {
    std::array<std::uint8_t, 8> b{};
    int leader() const { return b[0]; }
    vec2 lpos() const { return vec2{b[1], b[2]}; }
    int hb() const { return b[3]; }
    int target() const { return static_cast<int>(b[4]) - 1; }
    int mode() const { return b[5]; }
    bool hunting() const { return b[6] != 255 && b[7] != 255; }
    vec2 hunt() const { return vec2{b[6], b[7]}; }
};

Shared read_shared() {
    Shared s;
    cambc::shared_memory.read(0, std::span<std::uint8_t>(s.b));
    return s;
}
void write_shared(const Shared& s) { cambc::shared_memory.write(0, std::span<const std::uint8_t>(s.b)); }

// ---- per-turn view ------------------------------------------------------------------------------
struct CampView {
    cambc::camp unit;
    vec2 pos;
    int group;
    int kind;
    float hp;
    bool awake;
};
// Monoliths and golems outside our plan: never engage, keep out of their reach once awake.
bool hazard(const CampView& cv, int target) { return (cv.kind == 4 || cv.kind == 5) && cv.group != target; }

struct ChampView {
    cambc::champion unit;
    vec2 pos;
    float hp, max_hp;
};

struct Ctx {
    int round = 0;
    int my_id = 0;  // index + 1
    bool leader = false;
    bool ranged = false;
    bool at_spawn = false;
    bool moved = false;
    bool move_failed = false;
    bool attacked = false;
    bool cast = false;
    vec2 pos{};
    Shared sm;
    std::vector<CampView> camps;
    std::vector<ChampView> enemies;
    std::vector<ChampView> allies;  // excluding self
    cambc::bitboard blocked;
};

// Per-champion memory.
std::vector<std::pair<vec2, int>> g_hidden;  // tiles where a step bounced off a hidden unit
int g_index = -1;
bool g_solo_recall = false;
int g_target_since = 0;
int g_gather_wait = 0;
int g_shop_arrive = -1;
int g_last_target = -2;
int g_hunt_rounds = 0;
int g_last_choice = -100;
vec2 g_still_pos{-1, -1};
int g_still_rounds = 0;
int g_bank_stuck = 0;
int g_dyn_until = -1;
int g_bank_block = -1;
int g_bank_branch = 0;

void collect(Ctx& c) {
    auto& w = *world;
    c.camps.clear();
    c.enemies.clear();
    c.allies.clear();
    for (auto p : cambc::vision::camp_tiles().positions()) {
        try {
            auto camp = cambc::vision::camp_at(p);
            if (!camp.is_visible()) continue;
            const auto st = camp.state();
            if (st.is_dead()) continue;
            const float hp = camp.health();
            if (hp <= 0) continue;
            const vec2 sp = camp.spawn_position();
            int g = in_bounds(sp) ? w.group_of[static_cast<std::size_t>(sp.y * W + sp.x)] : -1;
            c.camps.push_back(CampView{camp, p, g, static_cast<int>(camp.kind()), hp, st.is_awake()});
        } catch (...) {
        }
    }
    const auto self_pos = c.pos;
    for (int side = 0; side < 2; ++side) {
        const team t = side == 0 ? team::enemy : team::ally;
        for (auto p : cambc::vision::champion_tiles(t).positions()) {
            if (p == self_pos) continue;
            try {
                auto ch = cambc::vision::champion_at(p);
                if (!ch.is_visible() || !ch.is_alive()) continue;
                ChampView v{ch, p, ch.health(), ch.max_health()};
                if (v.hp <= 0) continue;
                // Enemies on their own spawn heal and cannot be reached: not worth a turn.
                if (side == 0 && cambc::map::spawn_region(team::enemy).is_set(p)) continue;
                (side == 0 ? c.enemies : c.allies).push_back(v);
            } catch (...) {
            }
        }
    }
    // Dead camps keep reporting their tile but do not block walking: mark living ones only.
    c.blocked = w.terrain | cambc::vision::champion_tiles() | cambc::vision::teleport_tiles();
    for (const auto& cv : c.camps) c.blocked.set(cv.pos);
    std::erase_if(g_hidden, [&](const auto& h) { return h.second < c.round; });
    for (const auto& h : g_hidden) c.blocked.set(h.first);
    const int target = c.sm.target();
    for (const auto& cv : c.camps) {
        if (!cv.awake || !hazard(cv, target)) continue;
        const int r = kCampRange[cv.kind] + 1;
        for (int dx = -r; dx <= r; ++dx)
            for (int dy = -r; dy <= r; ++dy) {
                const vec2 p{cv.pos.x + dx, cv.pos.y + dy};
                if (in_bounds(p)) c.blocked.set(p);
            }
    }
    c.blocked.set(c.pos, false);
}

// Groups with a spawn tile in view report their lifecycle; remember respawn timers.
void update_groups(Ctx& c) {
    auto& w = *world;
    const auto visible = cambc::vision::visible_tiles();
    for (std::size_t i = 0; i < w.groups.size(); ++i) {
        auto& g = w.groups[i];
        if (g.tiles.empty() || dist(g.center, c.pos) > 14) continue;
        if ((g.mask & visible).is_empty()) continue;
        try {
            const auto st = w.handles[i].state();
            if (const auto* r = std::get_if<cambc::map::camp_group_respawning>(&st))
                g.respawn_until = c.round + std::max(1, static_cast<int>(r->timer));
            else
                g.respawn_until = -1;
        } catch (...) {
        }
    }
}

enum class GState { unknown, alive, reward, respawning };

GState group_state(int gi) {
    auto& w = *world;
    if (gi < 0 || gi >= static_cast<int>(w.groups.size())) return GState::unknown;
    const auto visible = cambc::vision::visible_tiles();
    if ((w.groups[gi].mask & visible).is_empty()) return GState::unknown;
    try {
        const auto st = w.handles[gi].state();
        if (std::holds_alternative<cambc::map::camp_group_alive>(st)) return GState::alive;
        if (std::holds_alternative<cambc::map::camp_group_reward_available>(st)) return GState::reward;
        return GState::respawning;
    } catch (...) {
        return GState::unknown;
    }
}

// ---- movement -----------------------------------------------------------------------------------
bool try_step(Ctx& c, vec2 to) {
    auto& me = cambc::self();
    if (c.moved || c.move_failed || !me.can_move()) return false;
    if (!in_bounds(to) || c.blocked.is_set(to) || dist(to, c.pos) != 1) return false;
    bool ok = false;
    try {
        ok = me.move_absolute(to);
    } catch (...) {
        ok = false;
    }
    if (ok) {
        c.moved = true;
        c.pos = to;
    } else {
        // A hidden unit sits there; remember it so we path around it for a while.
        c.move_failed = true;
        g_hidden.push_back({to, c.round + 12});
    }
    return ok;
}

// Step down a static field towards its roots; stop once the value is at most `stop`.
bool step_field(Ctx& c, const cambc::distance_field& f, const cambc::bitboard& reach, int stop) {
    if (!reach.is_set(c.pos)) return false;
    const int cur = f.get(c.pos);
    if (cur <= stop) return false;
    vec2 best = c.pos;
    int best_v = cur;
    for (auto d : kDirs) {
        const vec2 p = c.pos + d;
        if (!in_bounds(p) || c.blocked.is_set(p) || !reach.is_set(p)) continue;
        const int v = f.get(p);
        if (v < best_v) {
            best_v = v;
            best = p;
        }
    }
    return best != c.pos && try_step(c, best);
}

// Walk towards a tile with the SDK's path search (handles occupied targets by aiming beside them).
bool walk_to(Ctx& c, vec2 target, int stop_dist) {
    auto& me = cambc::self();
    if (c.moved || c.move_failed || !me.can_move()) return false;
    if (dist(c.pos, target) <= stop_dist) return false;
    // Pick a free tile near the target, closest to us.
    vec2 goal = target;
    if (!in_bounds(goal) || c.blocked.is_set(goal)) {
        int best = 1 << 30;
        bool found = false;
        for (int r = 1; r <= 2 && !found; ++r)
            for (int dx = -r; dx <= r; ++dx)
                for (int dy = -r; dy <= r; ++dy) {
                    if (std::max(std::abs(dx), std::abs(dy)) != r) continue;
                    const vec2 p{target.x + dx, target.y + dy};
                    if (!in_bounds(p) || c.blocked.is_set(p)) continue;
                    const int d = dist(p, c.pos);
                    if (d < best) {
                        best = d;
                        goal = p;
                        found = true;
                    }
                }
        if (!found) return false;
    }
    if (goal == c.pos) return false;
    bool ok = false;
    try {
        ok = me.move_towards(goal);
    } catch (...) {
        ok = false;
    }
    if (ok) {
        c.moved = true;
        c.pos = me.position();
    } else {
        c.move_failed = true;
    }
    return ok;
}

// Step to the free neighbour that maximises distance from threats.
bool step_away(Ctx& c, const std::vector<vec2>& threats) {
    vec2 best = c.pos;
    int best_score = -1;
    auto score = [&](vec2 p) {
        int m = 1 << 20;
        for (auto t : threats) m = std::min(m, dist(p, t));
        return m;
    };
    best_score = score(c.pos);
    for (auto d : kDirs) {
        const vec2 p = c.pos + d;
        if (!in_bounds(p) || c.blocked.is_set(p)) continue;
        const int s = score(p);
        if (s > best_score) {
            best_score = s;
            best = p;
        }
    }
    return best != c.pos && try_step(c, best);
}

// ---- combat ------------------------------------------------------------------------------------
bool camp_targetable(const Ctx& c, const CampView& cv) {
    if (hazard(cv, c.sm.target())) return false;
    if (cv.awake) return true;
    const int mode = c.sm.mode();
    const int tg = c.sm.target();
    return cv.group == tg && (mode == kFight || mode == kCapture);
}

bool do_attack(Ctx& c, const cambc::unit& u) {
    auto& me = cambc::self();
    try {
        me.attack(u);
        c.attacked = true;
        return true;
    } catch (...) {
        return false;
    }
}

// Basic attack: enemy champions first (lowest health), then damaging camps, then clusters.
bool basic_attack(Ctx& c) {
    auto& me = cambc::self();
    if (!me.can_attack()) return false;
    const int range = me.attack_range();
    const ChampView* best_e = nullptr;
    for (const auto& e : c.enemies)
        if (dist(e.pos, c.pos) <= range && (!best_e || e.hp < best_e->hp)) best_e = &e;
    if (best_e) {
        try {
            if (best_e->unit.is_visible() && best_e->unit.health() > 0) return do_attack(c, best_e->unit);
        } catch (...) {
        }
    }
    const CampView* best = nullptr;
    float best_key = 1e30f;
    for (const auto& cv : c.camps) {
        if (dist(cv.pos, c.pos) > range || !camp_targetable(c, cv)) continue;
        const float key = cv.hp + (kCampStr[cv.kind] > 0 ? 0.f : 100000.f);
        if (key < best_key) {
            best_key = key;
            best = &cv;
        }
    }
    if (best) {
        try {
            if (best->unit.is_visible() && best->unit.health() > 0) return do_attack(c, best->unit);
        } catch (...) {
        }
    }
    return false;
}

bool has_target_in_range(const Ctx& c, int range) {
    for (const auto& e : c.enemies)
        if (dist(e.pos, c.pos) <= range) return true;
    for (const auto& cv : c.camps)
        if (dist(cv.pos, c.pos) <= range && camp_targetable(c, cv)) return true;
    return false;
}

bool threatened(const Ctx& c) {
    for (const auto& e : c.enemies)
        if (dist(e.pos, c.pos) <= 4) return true;
    for (const auto& cv : c.camps)
        if (cv.awake && kCampStr[cv.kind] > 0 && dist(cv.pos, c.pos) <= kCampRange[cv.kind]) return true;
    return false;
}

// Our nearby health against the enemies we can see; above 1 means we are the stronger side here.
float local_balance(const Ctx& c) {
    float ours = cambc::self().health();
    for (const auto& a : c.allies)
        if (dist(a.pos, c.pos) <= 4) ours += a.hp;
    float theirs = 0;
    for (const auto& e : c.enemies)
        if (dist(e.pos, c.pos) <= 6) theirs += e.hp;
    return theirs <= 0 ? 100.f : ours / theirs;
}

bool exclusive(const cambc::ability& a) {
    const auto& info = a.info();
    if (!info) return false;
    return std::visit([](const auto& p) { return p.exclusive_with_movement; }, *info);
}

bool cast_spell(Ctx& c, bool allow_exclusive) {
    auto& me = cambc::self();
    if (c.cast || !me.can_cast()) return false;
    auto abilities = me.abilities();
    for (auto& a : abilities) {
        try {
            if (!a.info() || !a.is_ready()) continue;
            if (a.is_attack_reset() || a.is_dash()) continue;
            const bool excl = exclusive(a);
            if (excl && (!allow_exclusive || c.moved)) continue;
            if (a.is_heal()) {
                const ChampView* best = nullptr;
                float best_missing = 0;
                const auto tiles = a.valid_tiles();
                for (const auto& al : c.allies) {
                    if (!tiles.is_set(al.pos)) continue;
                    const float missing = al.max_hp - al.hp;
                    if (missing > best_missing) {
                        best_missing = missing;
                        best = &al;
                    }
                }
                if (best && (best_missing >= 0.6f * a.heal_amount() || best->hp < 0.5f * best->max_hp)) {
                    if (a.use(best->unit)) {
                        c.cast = true;
                        return true;
                    }
                }
                continue;
            }
            if (a.is_shield()) {
                if (threatened(c) && me.health() < 0.85f * me.max_health()) {
                    if (a.use()) {
                        c.cast = true;
                        return true;
                    }
                }
                continue;
            }
            if (a.is_damage() || a.is_cc()) {
                const auto tiles = a.valid_tiles();
                const ChampView* be = nullptr;
                for (const auto& e : c.enemies)
                    if (tiles.is_set(e.pos) && (!be || e.hp < be->hp)) be = &e;
                if (be) {
                    if (a.use(be->unit)) {
                        c.cast = true;
                        return true;
                    }
                    continue;
                }
                const CampView* bc = nullptr;
                for (const auto& cv : c.camps) {
                    if (!tiles.is_set(cv.pos) || !camp_targetable(c, cv)) continue;
                    if (a.is_cc() && !a.is_damage() && (!cv.awake || kCampStr[cv.kind] < 60)) continue;
                    if (!bc || kCampStr[cv.kind] > kCampStr[bc->kind]
                        || (kCampStr[cv.kind] == kCampStr[bc->kind] && cv.hp > bc->hp))
                        bc = &cv;
                }
                if (bc && (a.is_damage() || kCampStr[bc->kind] >= 60)) {
                    if (a.use(bc->unit)) {
                        c.cast = true;
                        return true;
                    }
                }
                continue;
            }
            if (a.is_aoe_damage()) {
                const auto tiles = a.valid_tiles();
                vec2 best{};
                int best_score = 0;
                auto consider = [&](vec2 center) {
                    if (!in_bounds(center) || !tiles.is_set(center)) return;
                    int s = 0;
                    for (const auto& e : c.enemies)
                        if (dist(e.pos, center) <= 1) s += 3;
                    for (const auto& cv : c.camps)
                        if (dist(cv.pos, center) <= 1 && camp_targetable(c, cv)) s += (kCampStr[cv.kind] > 0 ? 2 : 1);
                    if (s > best_score) {
                        best_score = s;
                        best = center;
                    }
                };
                for (const auto& e : c.enemies) consider(e.pos);
                for (const auto& cv : c.camps)
                    if (camp_targetable(c, cv))
                        for (int dx = -1; dx <= 1; ++dx)
                            for (int dy = -1; dy <= 1; ++dy) consider(vec2{cv.pos.x + dx, cv.pos.y + dy});
                if (best_score >= 3) {
                    if (a.use(best)) {
                        c.cast = true;
                        return true;
                    }
                }
                continue;
            }
        } catch (...) {
        }
    }
    return false;
}

// Dash next to a wounded enemy champion that is just out of reach.
bool dash_chase(Ctx& c) {
    auto& me = cambc::self();
    if (c.cast || !me.can_cast() || c.enemies.empty()) return false;
    const int range = me.attack_range();
    const ChampView* prey = nullptr;
    for (const auto& e : c.enemies) {
        const int d = dist(e.pos, c.pos);
        if (d <= range || d > range + 2 || e.hp > 0.35f * e.max_hp) continue;
        if (!prey || e.hp < prey->hp) prey = &e;
    }
    if (!prey) return false;
    auto abilities = me.abilities();
    for (auto& a : abilities) {
        try {
            if (!a.info() || !a.is_dash() || !a.is_ready()) continue;
            if (exclusive(a) && c.moved) continue;
            const auto tiles = a.valid_tiles();
            vec2 best = c.pos;
            int bd = dist(prey->pos, c.pos);
            for (int dx = -2; dx <= 2; ++dx)
                for (int dy = -2; dy <= 2; ++dy) {
                    const vec2 p{c.pos.x + dx, c.pos.y + dy};
                    if (!in_bounds(p) || c.blocked.is_set(p) || !tiles.is_set(p)) continue;
                    if (cambc::map::spawn_region(team::enemy).is_set(p)) continue;
                    const int d = dist(prey->pos, p);
                    if (d < bd) {
                        bd = d;
                        best = p;
                    }
                }
            if (best == c.pos || bd > range) return false;
            if (a.use(best)) {
                c.cast = true;
                c.pos = me.position();
                return true;
            }
            return false;
        } catch (...) {
            return false;
        }
    }
    return false;
}

// After a basic attack this turn, an attack reset grants a second one.
void attack_reset(Ctx& c) {
    auto& me = cambc::self();
    if (!c.attacked || c.cast || me.can_attack() || !me.can_cast()) return;
    if (!has_target_in_range(c, me.attack_range())) return;
    auto abilities = me.abilities();
    for (auto& a : abilities) {
        try {
            if (!a.info() || !a.is_attack_reset() || !a.is_ready()) continue;
            if (exclusive(a) && c.moved) continue;
            if (a.use()) {
                c.cast = true;
                basic_attack(c);
            }
            return;
        } catch (...) {
            return;
        }
    }
}

// ---- shopping -----------------------------------------------------------------------------------
void shop() {
    auto& me = cambc::self();
    using it = cambc::item_type;
    const auto type = me.champion_name();
    using ct = cambc::champion_type;
    std::array<it, 2> plan{it::power_shard_of_strength, it::power_shard_of_health};
    switch (type) {
    case ct::issac: case ct::varg: case ct::ryth: case ct::marcella: case ct::ren: case ct::heart_of_ruin:
        plan = {it::jungle_shard_of_health, it::power_shard_of_strength};
        break;
    case ct::cassi:
        plan = {it::jungle_shard_of_health, it::power_shard_of_focus};
        break;
    case ct::vendetta: case ct::vesper_vale: case ct::tavi: case ct::willow:
        plan = {it::monster_shard_of_strength, it::power_shard_of_strength};
        break;
    case ct::stellar: case ct::prism:
        plan = {it::monster_shard_of_focus, it::power_shard_of_focus};
        break;
    default:
        break;
    }
    try {
        const auto counts = me.item_count();
        for (int guard = 0; guard < 40; ++guard) {
            if (me.gold_count() < 30) return;
            // First family until its emblem forms, then the second.
            const auto first = plan[0];
            const auto emblem = first.result();
            const bool have_emblem = emblem && counts[*emblem] > 0;
            it pick = have_emblem ? plan[1] : first;
            if (!me.can_buy(pick, 1)) {
                pick = plan[1];
                if (!me.can_buy(pick, 1)) return;
            }
            me.buy(pick, 1);
            if (!have_emblem) {
                // Re-read counts so the emblem check sees the combination.
                const auto fresh = me.item_count();
                if (emblem && fresh[*emblem] > 0) plan[0] = plan[1];
            }
        }
    } catch (...) {
    }
}

// ---- leader planning ----------------------------------------------------------------------------
int choose_target(Ctx& c) {
    auto& w = *world;
    const int bodies = 1 + static_cast<int>(c.allies.size());
    const float dps = 45.f * std::max(3, bodies);
    int best = -1;
    float best_score = 0;
    for (std::size_t i = 0; i < w.groups.size(); ++i) {
        const auto& g = w.groups[i];
        if (g.tiles.empty() || !g.reachable) continue;
        if (g.mono && (c.round < kMonoRound || bodies < 4)) continue;
        if (g.golem && c.round < kGolemRound) continue;
        if (g.skip_until > c.round) continue;
        const int d = group_distance(g, c.pos);
        if (g.respawn_until > c.round + d) continue;
        const float clear = g.hp / dps;
        const float danger = g.str * clear / 600.f;
        const float value = g.gems * 3.f + g.gold * 0.6f;
        const float score = value / (d + clear + 6.f + danger * 4.f);
        if (score > best_score) {
            best_score = score;
            best = static_cast<int>(i);
        }
    }
    return best;
}

int nearest_altar_dist(vec2 p) {
    auto& w = *world;
    if (w.altar_ready && w.altar_reach.is_set(p)) return w.altar_field.get(p);
    int best = 1 << 20;
    for (auto a : w.altars) best = std::min(best, dist(a, p) * 3 / 2);
    return best;
}

void leader_plan(Ctx& c) {
    auto& me = cambc::self();
    auto& sm = c.sm;
    int mode = sm.mode();
    int target = sm.target();
    const int gems = static_cast<int>(me.gem_count());
    const bool endgame = c.round + kEndgameRounds >= static_cast<int>(cambc::constants::max_rounds);
    bool fighting = false;
    bool target_seen = false;
    for (const auto& cv : c.camps) {
        if (cv.awake && dist(cv.pos, c.pos) <= 6) fighting = true;
        if (cv.group == target && target >= 0) target_seen = true;
    }

    // Far from an altar, carry a bigger bag before walking back.
    const int altar_d = nearest_altar_dist(c.pos);
    const int bank_at = altar_d <= 12 ? kBankGems : altar_d <= 25 ? kBankGems * 3 / 2 : kBankGems * 2;

    // Hunting: chase visible enemies when we have local superiority.
    sm.b[6] = sm.b[7] = 255;
    if (!c.enemies.empty() && mode != kShop) {
        int near_allies = 1;
        for (const auto& a : c.allies)
            if (dist(a.pos, c.pos) <= 4) ++near_allies;
        const ChampView* weakest = nullptr;
        int near_enemies = 0;
        for (const auto& e : c.enemies) {
            if (dist(e.pos, c.pos) > 7) continue;
            ++near_enemies;
            if (!weakest || e.hp < weakest->hp) weakest = &e;
        }
        if (weakest && near_allies >= near_enemies && local_balance(c) >= kHuntBalance && g_hunt_rounds < 15) {
            sm.b[6] = static_cast<std::uint8_t>(weakest->pos.x);
            sm.b[7] = static_cast<std::uint8_t>(weakest->pos.y);
            ++g_hunt_rounds;
        }
    } else {
        g_hunt_rounds = std::max(0, g_hunt_rounds - 1);
    }

    if (mode == kShop) {
        if (c.at_spawn) {
            if (g_shop_arrive < 0) g_shop_arrive = c.round;
            if (me.health() >= 0.9f * me.max_health() && c.round - g_shop_arrive >= 2) {
                mode = kTravel;
                g_shop_arrive = -1;
                target = -1;
            }
        } else if (gems > 0 && !me.is_recalling()) {
            mode = kBank;  // never carry gems into a recall plan
        }
    }
    if (mode == kBank) {
        if (gems == 0) {
            mode = kTravel;
            if (me.gold_count() >= kShopGold && !endgame && c.enemies.empty()) mode = kShop;
        }
    } else if (mode != kShop && !fighting && gems > 0 && !(mode == kFight && target_seen)
               && (gems >= bank_at || endgame
                   || (gems >= kNearAltarGems && nearest_altar_dist(c.pos) <= kNearAltarDist && mode == kTravel))) {
        mode = kBank;
    } else if (mode == kTravel && gems == 0 && !fighting && me.gold_count() >= kShopGold + 60 && !endgame
               && c.enemies.empty()) {
        mode = kShop;
    }

    if (mode == kBank && g_bank_stuck >= 3) {
        // Camps block the way to the altar: clear the nearest group first.
        const CampView* near = nullptr;
        for (const auto& cv : c.camps)
            if (!hazard(cv, -1) && cv.group >= 0 && (!near || dist(cv.pos, c.pos) < dist(near->pos, c.pos))) near = &cv;
        if (near || g_bank_block >= 0) {
            target = g_bank_block >= 0 ? g_bank_block : near->group;
            g_bank_block = -1;
            mode = kFight;
            g_bank_stuck = 0;
        }
    }
    if (mode == kTravel || mode == kFight || mode == kCapture) {
        auto& w = *world;
        if (target >= 0 && target < static_cast<int>(w.groups.size())) {
            auto& g = w.groups[target];
            if (g.respawn_until > c.round + 3) target = -1;
            else if (c.round - g_target_since > kStallRounds) {
                g.skip_until = c.round + 120;
                target = -1;
            }
        } else {
            target = -1;
        }
        const bool rethink = mode == kTravel && c.round - g_last_choice >= 15;
        if (target < 0 || rethink) {
            const int t = choose_target(c);
            g_last_choice = c.round;
            if (t != target) {
                target = t;
                mode = kTravel;
                g_gather_wait = 0;
            }
        }
        if (target != g_last_target) {
            g_target_since = c.round;
            g_last_target = target;
        }
        if (target >= 0) {
            const auto st = group_state(target);
            bool my_camps = false;
            bool awake = false;
            for (const auto& cv : c.camps)
                if (cv.group == target) {
                    my_camps = true;
                    awake |= cv.awake;
                }
            if (st == GState::reward && !my_camps) {
                mode = kCapture;
            } else if (st == GState::respawning) {
                target = choose_target(c);
                mode = kTravel;
                g_gather_wait = 0;
            } else if (my_camps) {
                if (mode == kTravel) {
                    const int d = group_distance(w.groups[target], c.pos);
                    if (awake) mode = kFight;
                    else if (d <= kGatherDist + 2) {
                        int far = 0;
                        for (const auto& a : c.allies)
                            if (dist(a.pos, c.pos) > kGatherDist) ++far;
                        ++g_gather_wait;
                        if (far == 0 || g_gather_wait >= kGatherWait || w.groups[target].str < 40) mode = kFight;
                    }
                }
            } else if (mode == kCapture && st == GState::unknown) {
                mode = kTravel;
            } else if (group_distance(w.groups[target], c.pos) <= 1 && st != GState::reward) {
                // Standing on the group and nothing there: it is down; look elsewhere for a while.
                w.groups[target].respawn_until = c.round + 25;
                target = choose_target(c);
                mode = kTravel;
                g_gather_wait = 0;
            }
        }
        // No progress for a while in travel: give up on this target.
        if (mode == kTravel && target >= 0) {
            if (c.pos == g_still_pos) {
                if (++g_still_rounds >= 12) {
                    w.groups[target].skip_until = c.round + 60;
                    target = choose_target(c);
                    g_still_rounds = 0;
                }
            } else {
                g_still_pos = c.pos;
                g_still_rounds = 0;
            }
        }
    }

    sm.b[0] = static_cast<std::uint8_t>(c.my_id);
    sm.b[4] = static_cast<std::uint8_t>(target + 1);
    sm.b[5] = static_cast<std::uint8_t>(mode);
}

// ---- turn ---------------------------------------------------------------------------------------
int my_index() {
    if (g_index >= 0) return g_index;
    const auto& me = cambc::self();
    const auto all = cambc::champions(team::ally);
    for (std::size_t i = 0; i < all.size(); ++i)
        if (all[i] == me) g_index = static_cast<int>(i);
    if (g_index < 0) g_index = 0;
    return g_index;
}

// Path around units with a fresh field from the goal tiles (multi-root move_towards).
bool step_dynamic(Ctx& c, const cambc::bitboard& goals) {
    auto& me = cambc::self();
    if (c.moved || c.move_failed || !me.can_move()) return false;
    auto roots = goals & ~c.blocked;
    if (roots.is_empty()) return false;
    cambc::bitboard here;
    here.clear();
    here.set(c.pos);
    cambc::distance_field f;
    cambc::bitboard visited;
    f.compute_to(c.blocked, roots, here, visited);
    if (!visited.is_set(c.pos)) return false;
    vec2 best = c.pos;
    int bv = f.get(c.pos);
    for (auto d : kDirs) {
        const vec2 p = c.pos + d;
        if (!in_bounds(p) || c.blocked.is_set(p) || !visited.is_set(p)) continue;
        if (f.get(p) < bv) {
            bv = f.get(p);
            best = p;
        }
    }
    return best != c.pos && try_step(c, best);
}

void bank(Ctx& c) {
    auto& me = cambc::self();
    auto& w = *world;
    for (auto a : w.altars) {
        if (dist(a, c.pos) > cambc::constants::submit_gems_range) continue;
        bool enemy_lock = false;
        if (cambc::vision::has_altar_lock(a)) {
            try {
                const auto lock = cambc::vision::altar_lock_at(a);
                enemy_lock = lock.is_enemy();
                if (enemy_lock) {
                    // Break the lock: three to five basic attacks clear it.
                    if (me.can_attack() && dist(a, c.pos) <= me.attack_range()) {
                        me.attack(lock);
                        c.attacked = true;
                    }
                    g_bank_branch = 5;
                    g_bank_stuck = 0;
                    return;
                }
            } catch (...) {
                return;
            }
        }
        try {
            g_bank_branch = 1;
            me.submit_gems(a);
            g_bank_stuck = 0;
        } catch (...) {
            g_bank_branch = 9;
        }
        return;
    }
    g_bank_branch = 2;
    if (c.round >= g_dyn_until && w.altar_ready && step_field(c, w.altar_field, w.altar_reach, 0)) {
        g_bank_stuck = 0;
        return;
    }
    if (c.moved) return;
    // A living camp group sits on the way down the field: clear it rather than detour.
    if (w.altar_ready && w.altar_reach.is_set(c.pos)) {
        const int cur = w.altar_field.get(c.pos);
        for (const auto& cv : c.camps)
            if (dist(cv.pos, c.pos) == 1 && w.altar_reach.is_set(cv.pos) && w.altar_field.get(cv.pos) < cur
                && cv.group >= 0 && !hazard(cv, -1)) {
                g_bank_block = cv.group;
                g_bank_stuck = 3;
                return;
            }
    }
    // Units in the way: route around them.
    cambc::bitboard goals;
    goals.clear();
    for (auto a : w.altars) {
        for (auto d : kDirs) {
            const vec2 p = a + d;
            if (in_bounds(p)) goals.set(p);
        }
    }
    g_bank_branch = 3;
    if (step_dynamic(c, goals)) {
        // Keep the detour for a few turns, or the static field walks us straight back.
        if (c.round >= g_dyn_until) g_dyn_until = c.round + 6;
        g_bank_stuck = 0;
        return;
    }
    ++g_bank_stuck;
    g_bank_branch = 4;
}

// Move into attack range of the target group's nearest living camp.
bool approach_camps(Ctx& c, int target) {
    auto& me = cambc::self();
    const int range = me.attack_range();
    const CampView* best = nullptr;
    for (const auto& cv : c.camps)
        if ((cv.group == target || (cv.awake && !hazard(cv, target))) && (!best || dist(cv.pos, c.pos) < dist(best->pos, c.pos))) best = &cv;
    if (!best) return false;
    if (dist(best->pos, c.pos) <= range) return true;
    if (range <= 1) return walk_to(c, best->pos, 1);
    // Ranged: step to the free neighbour that gets closest without overshooting.
    vec2 step = c.pos;
    int bd = dist(best->pos, c.pos);
    for (auto d : kDirs) {
        const vec2 p = c.pos + d;
        if (!in_bounds(p) || c.blocked.is_set(p)) continue;
        if (dist(best->pos, p) < bd) {
            bd = dist(best->pos, p);
            step = p;
        }
    }
    if (step != c.pos && try_step(c, step)) return true;
    return walk_to(c, best->pos, range);
}

// Everyone flows down the target's field; followers never run ahead of the leader.
void travel_along(Ctx& c, int target, vec2 anchor, int base_stop) {
    auto& g = world->groups[target];
    int stop = base_stop;
    if (!c.leader) {
        if (dist(anchor, c.pos) > 6 || !g.ready || !g.reach.is_set(anchor)) {
            walk_to(c, anchor, kFollowDist);
            return;
        }
        // Stay level with the leader or a few steps ahead, so nobody blocks its path from behind.
        stop = std::max(base_stop, static_cast<int>(g.field.get(anchor)) - kLeadAhead);
    }
    if (c.round >= g_dyn_until && g.ready && step_field(c, g.field, g.reach, stop)) return;
    if (c.moved) return;
    if (c.leader) {
        if (group_distance(g, c.pos) > stop && walk_to(c, g.center, std::max(stop, 1)) && c.round >= g_dyn_until)
            g_dyn_until = c.round + 6;
    } else if (dist(anchor, c.pos) > kFollowDist) {
        walk_to(c, anchor, kFollowDist);
    }
}

void act(Ctx& c) {
    auto& me = cambc::self();
    auto& w = *world;
    const int mode = c.sm.mode();
    const int target = c.sm.target();
    const vec2 anchor = c.leader ? c.pos : c.sm.lpos();
    const int range = me.attack_range();
    const int gems = static_cast<int>(me.gem_count());
    const bool endgame = c.round + kEndgameRounds >= static_cast<int>(cambc::constants::max_rounds);

    // Badly hurt: break contact, then recall.
    if (!c.at_spawn && me.health() < kSoloRecallHp * me.max_health()) {
        std::vector<vec2> threats;
        for (const auto& e : c.enemies)
            if (dist(e.pos, c.pos) <= 4) threats.push_back(e.pos);
        for (const auto& cv : c.camps)
            if (cv.awake && kCampStr[cv.kind] > 0 && dist(cv.pos, c.pos) <= kCampRange[cv.kind] + 1)
                threats.push_back(cv.pos);
        if (threats.empty()) {
            try {
                me.recall();
                g_solo_recall = true;
                return;
            } catch (...) {
            }
        } else if (c.enemies.empty() || basic_attack(c) == false) {
            cast_spell(c, true);
            step_away(c, threats);
            return;
        }
    }
    if (g_solo_recall && c.at_spawn) {
        if (me.health() >= 0.9f * me.max_health()) g_solo_recall = false;
        else return;  // heal up first
    }

    // Shop mode: recall together (only when nothing is hitting us).
    if (mode == kShop && !c.at_spawn && gems == 0) {
        if (!threatened(c)) {
            try {
                me.recall();
                return;
            } catch (...) {
            }
        }
    }
    if (mode == kShop && c.at_spawn) return;

    // Act before moving when something is already in reach.
    const bool in_reach = has_target_in_range(c, range);
    if (in_reach) {
        cast_spell(c, true);
        basic_attack(c);
        attack_reset(c);
    } else {
        cast_spell(c, false);
    }

    // Finish a fleeing enemy with a dash, then strike.
    if (dash_chase(c)) {
        basic_attack(c);
        attack_reset(c);
    }

    // Movement by mode.
    int enemy_close = 1 << 20;
    const ChampView* closest = nullptr;
    for (const auto& e : c.enemies)
        if (dist(e.pos, c.pos) < enemy_close) {
            enemy_close = dist(e.pos, c.pos);
            closest = &e;
        }
    bool sees_target = false;
    for (const auto& cv : c.camps)
        if (cv.group == target) sees_target = true;
    const bool must_bank = gems > 0 && (mode == kBank || endgame || (gems >= 2 * kBankGems));
    const auto& enemy_spawn = cambc::map::spawn_region(team::enemy);
    bool handled = false;
    if (closest && enemy_close <= 5 && !c.at_spawn && local_balance(c) < kRetreatBalance) {
        // Outmatched: fall back towards spawn, where the rest of the team regroups.
        if (w.spawn_ready && w.spawn_reach.is_set(c.pos)) handled = step_field(c, w.spawn_field, w.spawn_reach, 0);
        if (!handled) {
            std::vector<vec2> threats;
            for (const auto& e : c.enemies) threats.push_back(e.pos);
            handled = step_away(c, threats);
        }
    } else if (must_bank && (enemy_close > 2 || c.enemies.size() <= c.allies.size())) {
        bank(c);
        handled = true;
    } else if (c.sm.hunting() && dist(c.sm.hunt(), c.pos) <= 9 && !enemy_spawn.is_set(c.sm.hunt())) {
        const vec2 h = c.sm.hunt();
        // An unreachable quarry must not freeze the ball: fall through when the chase cannot step.
        handled = dist(h, c.pos) <= range || walk_to(c, h, range);
    } else if (closest && enemy_close <= 5 && !in_reach && !enemy_spawn.is_set(closest->pos)) {
        // Enemy near but no hunt order: close in only if our group is locally stronger.
        int near_allies = 1;
        for (const auto& a : c.allies)
            if (dist(a.pos, c.pos) <= 4) ++near_allies;
        if (near_allies >= static_cast<int>(c.enemies.size()) && dist(closest->pos, anchor) <= 6)
            handled = walk_to(c, closest->pos, range);
    }
    if (handled) {
    } else if (mode == kFight && target >= 0) {
        bool sees = sees_target;
        for (const auto& cv : c.camps)
            if (cv.awake && !hazard(cv, target)) sees = true;
        if (sees) approach_camps(c, target);
        else travel_along(c, target, anchor, 1);
    } else if (mode == kCapture && target >= 0) {
        auto& g = w.groups[target];
        if (c.leader || dist(anchor, c.pos) <= 3) {
            if (!g.mask.is_set(c.pos)) {
                // Nearest free spawn tile of the group.
                vec2 best{};
                int bd = 1 << 20;
                for (auto t : g.tiles)
                    if (!c.blocked.is_set(t) && dist(t, c.pos) < bd) {
                        bd = dist(t, c.pos);
                        best = t;
                    }
                if (bd < (1 << 20)) {
                    if (bd == 1) try_step(c, best);
                    else if (c.leader || bd <= 3) walk_to(c, best, 0);
                }
            }
        } else {
            walk_to(c, anchor, kFollowDist);
        }
    } else if (mode == kTravel && target >= 0) {
        // Without sight of the camps (bushes, walls), walk all the way in to learn the group's state.
        travel_along(c, target, anchor, sees_target ? kGatherDist + 2 : 0);
    } else if (!c.leader && dist(anchor, c.pos) > kFollowDist) {
        walk_to(c, anchor, kFollowDist);
    } else if (c.leader && gems > 0 && target < 0) {
        bank(c);
    }

    // Fresh position may bring targets into reach.
    if (!c.attacked && (range <= 1 || !c.moved)) {
        // Refresh enemy/camp positions are unchanged; only our tile moved.
        basic_attack(c);
        attack_reset(c);
    }
    cast_spell(c, false);
    if (false && !c.enemies.empty()) {
        std::string d = "F r" + std::to_string(c.round) + " id" + std::to_string(c.my_id) + " t" + std::to_string(static_cast<int>(me.champion_name())) + " hp" + std::to_string(static_cast<int>(me.health())) + " @" + std::to_string(c.pos.x) + "," + std::to_string(c.pos.y) + " m" + std::to_string(mode) + " al" + std::to_string(c.allies.size()) + " atk" + std::to_string(c.attacked) + " cast" + std::to_string(c.cast) + " mv" + std::to_string(c.moved) + " |";
        for (const auto& e : c.enemies) d += " " + std::to_string(e.pos.x) + "," + std::to_string(e.pos.y) + ":" + std::to_string(static_cast<int>(e.hp));
        std::cout << d << std::endl;
    }
}

}  // namespace

void Bot::play_turn() {
    if (!world) init_world();
    auto& me = cambc::self();
    Ctx c;
    c.round = static_cast<int>(cambc::round_number());
    c.my_id = my_index() + 1;
    c.sm = read_shared();

    if (!me.is_alive()) {
        if (c.sm.leader() == c.my_id) {
            c.sm.b[0] = 0;
            write_shared(c.sm);
        }
        shop();
        g_solo_recall = false;
        progress_fields(30000);
        return;
    }

    c.pos = me.position();
    c.ranged = me.attack_range() > 1;
    c.at_spawn = cambc::map::spawn_region(team::ally).is_set(c.pos);
    // Tavi shops anywhere; everyone else on spawn.
    if (c.at_spawn || me.passive() == cambc::passive_type::remote_shop) shop();

    // Leadership: keep it while fresh, claim it when stale.
    const bool fresh = c.sm.leader() != 0 && ((c.round - c.sm.hb()) & 0xFF) <= 2;
    const bool eligible = !g_solo_recall;
    if (c.sm.leader() == c.my_id) c.leader = eligible;
    else if (!fresh && eligible) c.leader = true;
    if (!eligible && c.sm.leader() == c.my_id) {
        c.sm.b[0] = 0;
        write_shared(c.sm);
    }
    if (c.leader && c.sm.leader() != c.my_id) {
        // New leader: start a fresh plan from our own view.
        c.sm.b[4] = 0;
        c.sm.b[5] = kTravel;
        c.sm.b[6] = c.sm.b[7] = 255;
    }

    const bool channeling = me.is_recalling() || me.is_submitting_gems() || me.is_teleporting();
    if (!channeling) {
        collect(c);
        update_groups(c);
    }
    if (c.leader) {
        if (!channeling) leader_plan(c);
        if (c.round % 20 == 0)
            std::cout << "L r" << c.round << " id" << c.my_id << " pos " << c.pos.x << "," << c.pos.y << " mode "
                      << c.sm.mode() << " tgt " << c.sm.target() << " gems " << me.gem_count() << " gold "
                      << me.gold_count() << " hp " << me.health() << " allies " << c.allies.size() << " camps "
                      << c.camps.size() << " fuel " << cambc::fuel_remaining() << " ch" << channeling << " rec" << me.is_recalling() << " still" << g_still_rounds << " bs" << g_bank_stuck << " br" << g_bank_branch << " v" << (world->altar_reach.is_set(c.pos) ? world->altar_field.get(c.pos) : -1) << " dyn" << g_dyn_until << " en" << c.enemies.size() << std::endl;
        c.sm.b[0] = static_cast<std::uint8_t>(c.my_id);
        c.sm.b[1] = static_cast<std::uint8_t>(c.pos.x);
        c.sm.b[2] = static_cast<std::uint8_t>(c.pos.y);
        c.sm.b[3] = static_cast<std::uint8_t>(c.round & 0xFF);
        write_shared(c.sm);
        me.set_priority(100);
    } else {
        me.set_priority(10);
    }

    if (!channeling) {
        act(c);
        if (c.leader && c.moved) {
            c.sm.b[1] = static_cast<std::uint8_t>(c.pos.x);
            c.sm.b[2] = static_cast<std::uint8_t>(c.pos.y);
            write_shared(c.sm);
        }
    }
    progress_fields(30000);
}
