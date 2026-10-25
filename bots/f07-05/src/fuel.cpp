// The fuel subsystem (see fuel.h): vision snapshot, map analysis, camp knowledge, distance fields, movement.
//
// Rewritten around what fuel_remaining() shows a turn actually pays for (release build):
// - The heap: every allocation is hundreds of fuel and a growing vector reallocates log(n) times. The parent's
//   initialisation (camp_groups(), analyse_map(), per-group vectors) cost ~450k on 01boss-battle: every
//   champion lost its first two to four turns on all five-champion maps. Here the camp layout is read raw
//   into one reused buffer, every vector is sized before it is filled, and nothing is allocated per turn.
// - Host BFS (~3 fuel per reached tile): one walking field from our spawn serves the home distance, the
//   distance of every group from our spawn and, through the map's symmetry, from the enemy spawn.
// - Local loops (~50-80 fuel per element): every loop is bounded by what it needs (near groups, our tiles).
// A turn that runs out is suspended and resumes next round on stale state, so expensive steps check fuel().
#include <cambc.h>
#include "bot.h"
#include "fuel.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <utility>
#include <string>

std::uint32_t g_round = 0;

void warn(const char* where, const std::exception& e) {
    static int count = 0;
    if (++count <= 20) std::cout << "warn: " << where << ": " << e.what() << '\n';
}

namespace {
// Scratch boards and fields, allocated once on first use (the map layout must be known by then).
struct Scratch {
    cambc::bitboard tmp, blocked, goals, goals2, here, seen, wseen, hard;
    cambc::distance_field f, w;
};
Scratch* g_s = nullptr;
Scratch& scratch() {
    if (!g_s) g_s = new Scratch;
    return *g_s;
}

// Walking distances from where we stand (terrain only), for exact etas to groups whose own field is not
// filled yet: d(us, group) = min over its tiles. Recomputed only when we stand somewhere else.
struct Here {
    cambc::distance_field f;
    cambc::bitboard seen;
    vec2 origin{-1, -1};
    bool ok = false;
};
Here* g_here = nullptr;

View* g_view = nullptr;
std::vector<cambc::champion> g_others;  // allied handles other than ours, read once
std::vector<cambc::champion_type> g_other_types;
bool g_others_read = false;

Bits g_walls_local, g_hard_local;

// Every camp tile as its word and bit in a board's raw buffer (key = word << 6 | bit), groups contiguous:
// observe() tests ~100 camp tiles against vision each turn without coordinate math or bounds checks.
std::vector<std::uint32_t> g_key;
std::vector<std::uint32_t> g_first;  // group i's camps are g_key[g_first[i] .. g_first[i + 1])
inline bool kbit(const std::uint64_t* w, std::uint32_t key) { return (w[key >> 6] >> (key & 63)) & 1; }

// Groups whose alive/dead knowledge changed since the camp-block board was last written.
bool g_block_full = true;
std::vector<int> g_block_groups;
std::vector<std::uint8_t> g_block_mark;
void block_changed(size_t i, bool& dirty) {
    dirty = true;
    if (g_block_full || g_block_mark[i]) return;
    g_block_mark[i] = 1;
    g_block_groups.push_back(static_cast<int>(i));
}

constexpr bool kSafeSteps = true;                 // break ties between equally short steps by danger
constexpr bool kHoldDash = true;                  // no travel dash while enemies are about
constexpr int kHoldRounds = 8;
constexpr std::uint64_t kFieldFuel = 40000;      // left over after a walking field (plus ~6 per reachable tile)
std::uint64_t g_reachable = 10000;                // tiles we can walk to (set at initialisation)
size_t g_unfilled = 0;                            // groups whose own walking field is not computed yet
int g_stage = 0;                                  // initialisation steps done
size_t g_warm = 0;                                // groups whose clear cost is cached
std::uint64_t g_pairs = 0;                        // (fighting camp, camp) pairs inside groups
std::uint64_t g_altars = 0;
bool g_chose = false;                             // the first choice was made ahead (its goals cached)
constexpr std::uint64_t kStepMargin = 6000;       // kept on top of a step's expected cost
constexpr std::uint64_t kTurnFuel = 75000;        // a first turn once every cache is warm
constexpr std::uint64_t kChooseFuel = 15000;      // making the first choice ahead (clear costs cached; ~1k a group)
constexpr std::uint64_t kGoalFuel = 55000;        // ... and caching its goals (up to ~45k)
constexpr size_t kHereMin = 3;                    // fewer unfilled groups than this: guess their etas instead

// The walking field from tile `at` (~3-6 fuel per reachable tile), unless it is already there; false when it
// is not there and fuel does not allow it (more than `reserve` must be left after it).
bool here_field(vec2 at, std::uint64_t reserve) {
    if (!inside(at) || g_walls_local.at(at)) return false;
    if (!g_here) g_here = new Here;
    Here& h = *g_here;
    if (h.ok && h.origin == at) return true;
    if (!fuel(reserve + 6 * g_reachable) || !live()) return false;
    auto& s = scratch();
    s.here.clear();
    raw_set(s.here.ptr(), at);
    h.f.compute(walk_walls(), s.here, h.seen);
    h.origin = at;
    h.ok = true;
    return true;
}

void fill(Dist& d, const cambc::bitboard& tiles) {
    d.compute(walk_walls(), tiles, scratch().seen);
    if (g_unfilled > 0) --g_unfilled;
}

int fdist(const cambc::distance_field& f, vec2 p) { return inside(p) ? raw_dist(f.ptr(), p) : kFar; }

// The tile mirrored by the map's symmetry (the enemy's view of p).
vec2 mirror(vec2 p) {
    switch (cambc::map::symmetry()) {
    case cambc::symmetry::horizontal: return {map_w() - 1 - p.x, p.y};
    case cambc::symmetry::vertical: return {p.x, map_h() - 1 - p.y};
    case cambc::symmetry::rotational180: return {map_w() - 1 - p.x, map_h() - 1 - p.y};
    }
    return {map_w() - 1 - p.x, map_h() - 1 - p.y};
}
}  // namespace

void andnot_into(const cambc::bitboard& a, const cambc::bitboard& b, cambc::bitboard& out) {
    auto& t = scratch().tmp;  // a & b, taken before out (which may be a or b) is written
    t.clear();
    t |= a;
    t &= b;
    if (&out != &a) {
        out.clear();
        out |= a;
    }
    out ^= t;
}

void invalidate() {
    if (g_view) g_view->ok = false;
}

const View& view() {
    if (!g_view) {
        g_view = new View;
        g_view->lvis.bind(g_view->vis);
        g_view->lcamps.bind(g_view->camps);
        g_view->lchamps.bind(g_view->champs);
        g_view->ltele.bind(g_view->tele);
    }
    View& v = *g_view;
    if (v.ok) return v;
    if (!g_others_read) {
        g_others_read = true;
        const int me = static_cast<int>(static_cast<cambc::handle_champion_t>(cambc::self()));
        const auto all = cambc::champions(team::ally);
        g_others.reserve(all.size());
        g_other_types.reserve(all.size());
        v.allies.reserve(all.size());
        v.ally_types.reserve(all.size());
        for (const auto& a : all)
            if (static_cast<int>(static_cast<cambc::handle_champion_t>(a)) != me) {
                g_others.push_back(a);
                cambc::champion_type t = cambc::champion_type::vesper_vale;
                try { t = a.champion_name(); } catch (const cambc::error&) {}
                g_other_types.push_back(t);
            }
    }
    cambc::vision::visible_tiles_into(v.vis);
    cambc::vision::camp_tiles_into(v.camps);
    cambc::vision::champion_tiles_into(team::enemy, v.foes);
    cambc::vision::champion_tiles_into(team::ally, v.champs);
    v.champs |= v.foes;
    cambc::vision::teleport_tiles_into(v.tele);
    v.self = cambc::self().position();
    v.allies.clear();
    v.ally_types.clear();
    for (size_t i = 0; i < g_others.size(); ++i) {
        cambc::detail::champion_info info{};
        if (!peek(g_others[i], info)) continue;
        const vec2 p{info.x, info.y};
        if (!inside(p)) continue;
        v.allies.push_back(p);
        v.ally_types.push_back(g_other_types[i]);
        raw_set(v.champs.ptr(), p);
    }
    v.ok = true;
    return v;
}

const Bits& walls_local() { return g_walls_local; }
const Bits& hard_local() { return g_hard_local; }

bool free_tile(vec2 p) {
    if (!inside(p)) return false;
    const auto& v = view();
    if (p == v.self) return true;
    return !g_walls_local.at(p) && !v.lcamps.at(p) && !v.lchamps.at(p) && !v.ltele.at(p);
}

// ---------------------------------------------------------------- setup

// Profiling (kProbe): fuel per initialisation step, printed once by the next turn (prints cost fuel too).
constexpr bool kProbe = false;
std::string g_probe;
namespace {
std::uint64_t g_mark = 0;
void mark(const char* tag) {
    if constexpr (kProbe) {
        const auto f = cambc::fuel_remaining();
        if (g_mark > f) { g_probe += ' '; g_probe += tag; g_probe += '='; g_probe += std::to_string(g_mark - f); }
        g_mark = cambc::fuel_remaining();
    }
}
}  // namespace

// Initialisation is a queue of steps, each run only when what is left of the turn covers its expected cost:
// a turn that runs dry is suspended and its remainder wastes the next round too (live() fails there). The map
// step comes first (the first turn's 2x fuel always has room for it); then the team, the altar walk, the
// field order, the camp threat tables and the first choice's caches (every group's clear cost, the field from
// our tile). The first real turn is played once all are done and a turn's worth of fuel is left.
void Bot::initialise() {
    if (g_stage == 0) {
        init_map();
        g_stage = 1;
    }
    while (g_stage < 6) {
        if (!live()) return;
        if (g_stage == 5) {
            if (!prewarm()) { g_round = ~std::uint32_t{0}; return; }
        } else {
            if (!fuel(step_cost(g_stage) + kStepMargin)) { g_round = ~std::uint32_t{0}; return; }
            init_step(g_stage);
        }
        ++g_stage;
    }
    init_ = true;
    if constexpr (kProbe) g_probe += " left=" + std::to_string(cambc::fuel_remaining());
    if (!fuel(kTurnFuel)) g_round = ~std::uint32_t{0};
}

void Bot::init_map() {
    if constexpr (kProbe) { g_probe.clear(); g_mark = cambc::fuel_remaining(); }
    init_dims();
    auto& s = scratch();
    g_walls_local.bind(walk_walls());
    s.hard.clear();
    s.hard |= cambc::map::walls();
    s.hard |= cambc::map::altars();
    g_hard_local.bind(s.hard);
    auto& me = cambc::self();
    const auto& ally_spawn = cambc::map::spawn_region(team::ally);
    mark("boards");

    // Camp layout in one host call into one buffer: [group, gold, gems, respawn, n, n x (camp, type, x, y)].
    std::vector<std::uint32_t> words(1024);
    std::uint32_t count = 0;
    if (cambc::detail::sdk_map_camp_layout(words.data(), static_cast<std::uint32_t>(words.size()), &count) != 0) count = 0;
    if (count > words.size()) {
        words.resize(count);
        if (cambc::detail::sdk_map_camp_layout(words.data(), count, &count) != 0) count = 0;
    }
    const size_t cap = count / 5 + 1;  // groups at most
    mark("layout");

    // Walking distance from our spawn: the home field, every group's distance from us and, mirrored by the
    // map's symmetry, from the enemy spawn (the enemy's walls are ours mirrored). Unreached tiles read kFar.
    home_.compute(walk_walls(), ally_spawn, s.seen);
    g_reachable = s.seen.count();
    bool symmetric = true;
    const auto sym = cambc::map::symmetry();
    const bool flip_x = sym != cambc::symmetry::vertical, flip_y = sym != cambc::symmetry::horizontal;
    const int w1 = map_w() - 1, h1 = map_h() - 1;
    {
        const auto& enemy_spawn = cambc::map::spawn_region(team::enemy);
        for (auto p : ally_spawn.positions())
            if (!bit(enemy_spawn, vec2{flip_x ? w1 - p.x : p.x, flip_y ? h1 - p.y : p.y})) { symmetric = false; break; }
    }
    // Never seen: a field from the enemy spawn with our spawn as their wall.
    if (!symmetric) {
        s.goals.clear();
        s.goals |= cambc::map::walls();
        s.goals |= cambc::map::holes();
        s.goals |= cambc::map::altars();
        s.goals |= ally_spawn;
        s.w.compute(s.goals, cambc::map::spawn_region(team::enemy), s.wseen);
    }
    const std::uint16_t* from_us = home_.q;
    const std::uint16_t* from_them = symmetric ? home_.q : static_cast<const cambc::distance_field&>(s.w).ptr();
    mark("home");

    h_.clear();
    h_.reserve(cap);
    g_.clear();
    g_.reserve(cap);
    k_.clear();
    k_.reserve(cap);
    gtiles_.clear();
    gtiles_.reserve(cap);
    reach_.clear();
    reach_.reserve(cap);
    group_at_.assign(static_cast<size_t>(map_w() * map_h()), -1);
    camp_at_.assign(static_cast<size_t>(map_w() * map_h()), -1);
    g_key.clear();
    g_key.reserve(count / 4 + 1);
    g_first.clear();
    g_first.reserve(cap + 1);
    std::int16_t* gat = group_at_.data();
    std::int16_t* cat = camp_at_.data();
    const int mw = map_w();
    for (std::uint32_t i = 0; i + 5 <= count;) {
        const std::uint32_t n = std::min<std::uint32_t>(words[i + 4], (count - i - 5) / 4);
        const auto gi = static_cast<std::int16_t>(g_.size());
        h_.push_back(words[i]);
        GroupInfo& d = g_.emplace_back();
        Known& k = k_.emplace_back();
        d.gold = static_cast<int>(words[i + 1]);
        d.gems = static_cast<int>(words[i + 2]);
        d.respawn = static_cast<std::int32_t>(words[i + 3]);
        i += 5;
        d.tiles.resize(n);
        d.kinds.resize(n);
        k.hp.resize(n);
        auto* tw = gtiles_.emplace_back().ptr();  // cleared board
        g_first.push_back(static_cast<std::uint32_t>(g_key.size()));
        int sx = 0, sy = 0, da = kFar, de = kFar;
        for (std::uint32_t m = 0; m < n; ++m, i += 4) {
            const auto kind = static_cast<cambc::camp_type>(words[i + 1]);
            const vec2 p{static_cast<std::int32_t>(words[i + 2]), static_cast<std::int32_t>(words[i + 3])};
            d.tiles[m] = p;
            d.kinds[m] = kind;
            sx += p.x;
            sy += p.y;
            const auto sp = camp_spec(kind);
            k.hp[m] = sp.hp;
            d.hp += sp.hp;
            d.str += sp.str;
            if (sp.range > d.range) d.range = sp.range;
            if (kind == cambc::camp_type::monolith || kind == cambc::camp_type::golem) d.boss = true;
            // A tile outside the map (never seen) gets a key that reads 0 from the first word.
            g_key.push_back(inside(p) ? static_cast<std::uint32_t>(((p.y >> 3) * g_bstride + (p.x >> 3)) << 6 | ((p.y & 7) << 3 | (p.x & 7))) : 0u);
            if (!inside(p)) continue;
            raw_set(tw, p);
            const int t = p.y * mw + p.x;
            gat[t] = gi;
            cat[t] = static_cast<std::int16_t>(m);
            da = std::min(da, raw_dist(from_us, p));
            const vec2 q = symmetric ? vec2{flip_x ? w1 - p.x : p.x, flip_y ? h1 - p.y : p.y} : p;
            de = std::min(de, raw_dist(from_them, q));
        }
        d.d_ally = da;
        d.d_enemy = de;
        int r = 0;
        if (n > 0) {
            d.center = {sx / static_cast<int>(n), sy / static_cast<int>(n)};
            for (auto t : d.tiles) r = std::max(r, cheb(t, d.center));
        }
        reach_.push_back(r);
    }
    const size_t ng = g_.size();
    g_first.push_back(static_cast<std::uint32_t>(g_key.size()));
    gfield_.clear();
    gfield_.resize(ng);
    g_block_mark.assign(ng, 0);
    g_unfilled = ng;
    g_block_groups.reserve(ng);
    g_altars = cambc::map::altars().count();
    // The threat tables cost ~90 per pair of (fighting camp, camp) inside a group.
    g_pairs = 0;
    for (size_t i = 0; i < ng; ++i) {
        std::uint64_t hostile = 0;
        for (auto kind : g_[i].kinds) hostile += camp_spec(kind).str > 0;
        g_pairs += hostile * g_[i].kinds.size();
    }
    mark("groups");
}

std::uint64_t Bot::step_cost(int step) const {
    const std::uint64_t ng = g_.size();
    switch (step) {
    case 1: return 8000;                            // team, slots, home regions
    case 2: return 6000 + 3 * g_reachable + 1500 * g_altars;  // altar spots and the walk to them
    case 3: return 2000 + 250 * ng;                 // field order
    case 4: return 2000 + 90 * g_pairs + 60 * g_key.size();  // camp threat tables
    }
    return 0;
}

void Bot::init_step(int step) {
    auto& s = scratch();
    auto& me = cambc::self();
    const auto& ally_spawn = cambc::map::spawn_region(team::ally);
    const size_t ng = g_.size();
    if (step == 1) {
    const auto allies = cambc::champions(team::ally);
    size_ = static_cast<int>(allies.size());
    // Cassi and Issac count as one pick.
    const auto my_type = me.champion_name();
    slot_ = slot_of(my_type, size_);
    const auto* mem = cambc::draft_memory.data();
    home_slot_.clear();
    if (static_cast<char>(mem[dm::magic]) == 'E' && static_cast<int>(mem[dm::ngroups]) == static_cast<int>(std::min<size_t>(ng, 250))) {
        size_ = static_cast<int>(mem[dm::size]);
        slot_ = slot_of(my_type, size_);
        home_slot_.assign(ng, 255);
        const size_t nm = std::min<size_t>(ng, 250);
        for (size_t i = 0; i < nm; ++i) home_slot_[i] = static_cast<std::uint8_t>(mem[dm::home + i]);
    } else {
        home_slot_ = assign_homes(g_, std::max(1, size_));
    }
    for (const auto& ab : me.abilities())
        if (ab.is_dash()) dasher_ = true;
    my_range_ = me.attack_range();
    spawn_local_.bind(ally_spawn);
    {
        const int self_h = static_cast<int>(static_cast<cambc::handle_champion_t>(me));
        others_.reserve(allies.size());
        for (const auto& a : allies)
            if (static_cast<int>(static_cast<cambc::handle_champion_t>(a)) != self_h) others_.push_back(a);
    }
    mark("team");
    } else if (step == 4) {
    build_threats();
    mark("threats");
    } else if (step == 2) {

    // Tiles from which an altar takes a tribute, written straight into the board, then the walk to them.
    const int ar = cambc::constants::submit_gems_range;
    altars_ = cambc::map::altars().positions();
    enemy_lock_.assign(altars_.size(), -1);
    lock_seen_.assign(altars_.size(), -1000);
    altar_spots_.clear();
    for (auto a : altars_)
        for (int dy = -ar; dy <= ar; ++dy)
            for (int dx = -ar; dx <= ar; ++dx) {
                const vec2 t{a.x + dx, a.y + dy};
                if (inside(t) && !g_walls_local.at(t)) raw_set(altar_spots_.ptr(), t);
            }
    altar_.compute(walk_walls(), altar_spots_, s.seen);
    for (auto t : ally_spawn.positions()) home_altar_ = std::min(home_altar_, altar_.at(t));
    bank_d_.assign(ng, kFar);
    {
        const std::uint16_t* aq = altar_.q;
        for (size_t i = 0; i < ng; ++i) {
            int b = kFar;
            for (auto t : g_[i].tiles)
                if (inside(t)) b = std::min(b, raw_dist(aq, t));
            bank_d_[i] = b;
        }
    }
    mark("altars");
    } else if (step == 3) {

    // Order in which the groups' own walking fields are filled: our home groups first, then by distance
    // (ties by index, as a stable sort).
    {
        std::vector<std::uint64_t> key(ng);
        const bool homes = home_slot_.size() == ng;
        for (size_t i = 0; i < ng; ++i) {
            const bool mine = homes && home_slot_[i] == slot_;
            key[i] = (static_cast<std::uint64_t>(mine ? 0 : 1) << 40) | (static_cast<std::uint64_t>(g_[i].d_ally) << 20) | i;
        }
        std::sort(key.begin(), key.end());
        fill_order_.resize(ng);
        for (size_t i = 0; i < ng; ++i) fill_order_[i] = static_cast<int>(key[i] & 0xfffff);
    }
    mark("order");
    }
    if constexpr (kProbe) g_probe += " r" + std::to_string(cambc::round_number()) + "s" + std::to_string(step) + " est=" + std::to_string(step_cost(step));
}

// The first choice's caches, resumable over turns: every group's clear cost (~1k + 40 per pair of its camps;
// the first choice weighs every group), then the field from our tile. True when all are done.
bool Bot::prewarm() {
    for (; g_warm < g_.size(); ++g_warm) {
        const std::uint64_t n = g_[g_warm].tiles.size();
        if (!fuel(kStepMargin + 1000 + 40 * n * n) || !live()) return false;
        clear_cost(static_cast<int>(g_warm));
    }
    if (g_unfilled >= kHereMin && !here_field(pos_, kStepMargin)) return false;
    // With room to spare: make the first choice now and cache its goals (a raid's rally ring or the approach
    // tiles cost up to ~50k on a big group); the turn that plays makes the same choice and finds them cached.
    if (!g_chose && fuel(kChooseFuel + 1200 * g_.size() + kGoalFuel) && live()) {
        g_chose = true;
        init_ = true;  // choose() and the goals read the finished tables
        int wait = 0;
        const int t = choose(wait);
        if (t >= 0 && k_[t].state != Known::reward && k_[t].state != Known::respawning && fuel(kGoalFuel)) {
            if (raid_n_ >= 2 && rally_need_) rally_goal(t);
            else approach_goal(t, std::max(1, my_range_));
        }
        init_ = false;
    }
    if (fuel(kStepMargin + 80 * g_key.size()) && live()) rebuild_camp_block();
    mark("prewarm");
    return true;
}

// Walking fields to each group's tiles, one host BFS each (no export), while fuel lasts: the current target
// first (its field steers the stuck check and the dash decisions every turn), then in fill order.
void Bot::fill_fields(std::uint64_t reserve) {
    if (!init_) return;
    if (target_ >= 0 && gfield_[target_].empty()) {
        if (!fuel(reserve) || !live()) return;
        fill(gfield_[target_], gtiles_[target_]);
    }
    if (g_unfilled == 0) return;
    for (int i : fill_order_) {
        if (!gfield_[i].empty()) continue;
        if (!fuel(reserve) || !live()) return;
        fill(gfield_[i], gtiles_[i]);
    }
}

// Walking turns from us to the group: its own field when filled (the current target's is filled on first use:
// it is read every turn), else one field from our tile serves every group (exact, ~3-6 fuel per reachable
// tile, once per tile we stand on: a choice of target), else a straight-line guess.
int Bot::eta(int group) const {
    const auto& own = gfield_[group];
    if (own.empty() && group == target_ && fuel(kFieldFuel + 6 * g_reachable) && live())
        fill(const_cast<Dist&>(own), gtiles_[group]);
    if (!own.empty()) {
        const int d = own.at(pos_);
        return d >= kFar ? kFar : d;
    }
    if (g_[group].d_ally >= kFar) return kFar;
    if (g_unfilled >= kHereMin && here_field(pos_, kFieldFuel)) {
        {
            const std::uint16_t* q = g_here->f.ptr();
            int best = kFar;
            for (auto t : g_[group].tiles)
                if (inside(t)) best = std::min(best, raw_dist(q, t));
            return best;
        }
    }
    return cheb(pos_, g_[group].center) * 5 / 4 + 2;
}

bool Bot::group_awake(int group) const {
    for (auto t : g_[group].tiles)
        if (bit(awake_, t)) return true;
    return false;
}

// ---------------------------------------------------------------- knowledge

void Bot::observe() {
    if constexpr (kProbe) {
        if (!g_probe.empty() && live()) { std::cout << "PROBE r" << g_round << g_probe << " left=" << cambc::fuel_remaining() << std::endl; g_probe.clear(); }
    }
    if (!live()) return;  // a suspended turn resumed on the next round, or the turn that initialised
    const int now = static_cast<int>(cambc::round_number());
    const size_t ng = g_.size();
    for (size_t i = 0; i < ng; ++i) {
        auto& k = k_[i];
        if (k.state == Known::respawning && now >= k.ready) { k.state = Known::unknown; block_changed(i, block_dirty_); }
    }
    const auto& v = view();
    const std::uint64_t* vw = v.vis.ptr();
    const std::uint64_t* cw = v.camps.ptr();
    const std::uint32_t* key = g_key.data();
    for (size_t i = 0; i < ng; ++i) {
        if (cheb(g_[i].center, pos_) > 8 + reach_[i]) continue;
        auto& k = k_[i];
        const std::uint32_t* kk = key + g_first[i];
        const size_t n = g_first[i + 1] - g_first[i];
        bool seen = false, living = false;
        for (size_t c = 0; c < n; ++c) {
            if (!kbit(vw, kk[c])) continue;
            seen = true;
            if (kbit(cw, kk[c])) {
                living = true;
                if (k.hp[c] <= 0) { k.hp[c] = camp_spec(g_[i].kinds[c]).hp; block_changed(i, block_dirty_); }
            } else if (k.state == Known::alive && k.hp[c] > 0) {
                k.hp[c] = 0;
                block_changed(i, block_dirty_);
            }
        }
        if (!seen) continue;
        if (living) {
            if (k.state != Known::alive) {
                // Back alive: unseen camps at full health, visible empty tiles dead.
                for (size_t c = 0; c < n; ++c) {
                    if (!kbit(vw, kk[c])) k.hp[c] = camp_spec(g_[i].kinds[c]).hp;
                    else if (!kbit(cw, kk[c])) k.hp[c] = 0;
                }
                k.state = Known::alive;
                block_changed(i, block_dirty_);
            }
            continue;
        }
        if (k.state == Known::respawning) continue;
        // A reward we are not after only needs an occasional look.
        if (k.state == Known::reward && static_cast<int>(i) != target_ && (now + static_cast<int>(i)) % 4 != 0) continue;
        // One host query (camp_group::state() rebuilds the whole vision board first, ~4k fuel).
        std::uint32_t st = 0;
        if (cambc::detail::sdk_camp_group_state(h_[i], &st) != 0 || st == 0 || st > 3) continue;
        const auto before = k.state;
        if (st == 1) {
            k.state = Known::alive;
        } else if (st == 2) {
            k.state = Known::reward;
        } else {
            std::int32_t timer = 1;
            if (cambc::detail::sdk_camp_group_cooldown(h_[i], &timer) != 0) timer = 1;
            k.state = Known::respawning;
            k.ready = now + std::max(1, static_cast<int>(timer));
            for (size_t c = 0; c < n; ++c) k.hp[c] = camp_spec(g_[i].kinds[c]).hp;
        }
        if (k.state != before) block_changed(static_cast<size_t>(i), block_dirty_);
    }
    // Health of the camps of the group we fight.
    if (target_ >= 0 && cheb(g_[target_].center, pos_) <= 8 + reach_[target_]) {
        auto& k = k_[target_];
        const auto& tiles = g_[target_].tiles;
        const std::uint32_t* kk = key + g_first[target_];
        for (size_t c = 0; c < tiles.size(); ++c) {
            const vec2 p = tiles[c];
            if (!kbit(vw, kk[c]) || !kbit(cw, kk[c]) || cheb(p, pos_) > 8 || camp_spec(g_[target_].kinds[c]).str <= 0) continue;
            try { k.hp[c] = cambc::vision::camp_at(p).health(); } catch (const std::exception&) {}
        }
    }
}

// Tiles of camps we believe alive, written straight into the board: the groups whose knowledge changed are
// rewritten (a full pass over every camp costs ~10k on 160-camp maps), the whole board only at first.
void Bot::rebuild_camp_block() {
    if (!block_dirty_) return;
    block_dirty_ = false;
    auto* w = camp_block_.ptr();
    auto write = [&](size_t i) {
        const auto& k = k_[i];
        const bool off = k.state == Known::respawning || k.state == Known::reward;
        const std::uint32_t* kk = g_key.data() + g_first[i];
        const size_t n = g_first[i + 1] - g_first[i];
        for (size_t c = 0; c < n; ++c) {
            const std::uint64_t m = std::uint64_t{1} << (kk[c] & 63);
            if (!off && k.hp[c] > 0) w[kk[c] >> 6] |= m;
            else w[kk[c] >> 6] &= ~m;
        }
    };
    if (g_block_full) {
        g_block_full = false;
        camp_block_.clear();
        for (size_t i = 0; i < g_.size(); ++i) write(i);
    } else {
        for (int i : g_block_groups) write(static_cast<size_t>(i));
    }
    for (int i : g_block_groups) g_block_mark[static_cast<size_t>(i)] = 0;
    g_block_groups.clear();
}

// ---------------------------------------------------------------- movement

bool Bot::try_dash(const cambc::distance_field& f, const cambc::bitboard& seen) {
    auto& me = cambc::self();
    if (cast_ || !dasher_ || !me.can_cast() || !fuel(12000)) return false;
    const vec2 pos = me.position();
    if (!bit(seen, pos)) return false;
    const int here = fdist(f, pos);
    for (const auto& ab : me.abilities()) {
        if (!ab.is_dash() || !ab.is_ready()) continue;
        std::array<std::pair<int, vec2>, 16> cand;
        size_t n = 0;
        const auto& v = view();
        for (int dx = -2; dx <= 2; ++dx)
            for (int dy = -2; dy <= 2; ++dy) {
                if (std::max(std::abs(dx), std::abs(dy)) != 2) continue;
                const vec2 t{pos.x + dx, pos.y + dy};
                if (!free_tile(t) || !bit(seen, t)) continue;
                const int d = fdist(f, t);
                if (d > here - 2) continue;
                // Need a passable middle tile (holes may be jumped, walls not).
                bool path = false;
                for (auto s : kDirs) {
                    const vec2 m{pos.x + s.x, pos.y + s.y};
                    if (!inside(m) || cheb(m, t) != 1) continue;
                    if (hard_local().at(m) || v.lchamps.at(m) || v.lcamps.at(m)) continue;
                    path = true;
                    break;
                }
                if (path) cand[n++] = {d, t};
            }
        std::sort(cand.begin(), cand.begin() + n, [](auto a, auto b) { return a.first < b.first; });
        for (size_t i = 0; i < n && i < 3; ++i) {
            if (!live()) return false;
            try {
                const bool ok = ab.use(cand[i].second);
                cast_ = true;
                invalidate();
                if (!ok) move_failed_ = true;
                return ok;
            } catch (const cambc::error&) {
                // The engine's dash path rules can be stricter than ours: try the next landing.
            }
        }
        return false;
    }
    return false;
}

// One step (and a dash when it gains two) along the shortest route around known camps and
// visible units. When only living camps close the way, remember their group in plug_.
// One host BFS from the goals that stops at our tile; boards are refilled in place.
bool Bot::go(const cambc::bitboard& goals_in, bool dash) {
    auto& me = cambc::self();
    if (moved_ || move_failed_ || !me.can_move() || !live()) return false;
    if (!fuel(25000)) return false;
    const vec2 pos = me.position();
    if (!inside(pos)) return false;
    const auto& v = view();
    auto& s = scratch();
    s.blocked.clear();
    s.blocked |= walk_walls();
    s.blocked |= camp_block_;
    s.blocked |= v.camps;
    s.blocked |= v.champs;
    s.blocked |= v.tele;
    raw_clear(s.blocked.ptr(), pos);
    andnot_into(goals_in, s.blocked, s.goals);
    if (bit(s.goals, pos) || s.goals.is_empty()) return false;
    s.here.clear();
    raw_set(s.here.ptr(), pos);
    s.seen.clear();
    s.f.compute_to(s.blocked, s.goals, s.here, s.seen);
    if (bit(s.seen, pos)) return step_on(s.f, s.seen, s.goals, s.blocked, dash);
    // Camps (or units) close the way: find what plugs the walk-only route.
    andnot_into(goals_in, walk_walls(), s.goals2);
    s.wseen.clear();
    s.w.compute_to(walk_walls(), s.goals2, s.here, s.wseen);
    if (!bit(s.wseen, pos)) return false;
    // Follow that route until the first camp on it (field read in place).
    vec2 at = pos;
    for (int steps = 0; steps < 60; ++steps) {
        vec2 next = at;
        int nd = fdist(s.w, at);
        for (auto d : kDirs) {
            const vec2 t{at.x + d.x, at.y + d.y};
            if (!bit(s.wseen, t) || walls_local().at(t)) continue;
            const int fd = raw_dist(s.w.ptr(), t);
            if (fd < nd) { nd = fd; next = t; }
        }
        if (next == at) break;
        const int gi = group_at_[static_cast<size_t>(tidx(next))];
        if (gi >= 0 && bit(camp_block_, next)) {
            if (plug_ != gi) { plug_ = gi; plug_since_ = static_cast<int>(cambc::round_number()); plug_altar_ = false; }
            return false;
        }
        at = next;
    }
    // Units in the gap: walk the plain route and wait there for them to move.
    return step_on(s.w, s.wseen, s.goals, s.blocked, dash);
}

// Keep the dash for a fight (escape or chase, 8 turns of cooldown) rather than gain one tile of travel with it
// while enemy champions are about: one in sight within 12, or seen lately. A chase (the goal surrounds a foe)
// dashes as before.
bool Bot::hold_dash(const cambc::bitboard& goals, vec2 pos) const {
    if (!kHoldDash) return false;
    bool near = static_cast<int>(cambc::round_number()) - last_enemy_seen_ <= kHoldRounds;
    const auto& v = view();
    if (v.foes.is_empty()) return near;
    const auto& enemy_spawn = cambc::map::spawn_region(team::enemy);
    for (auto p : v.foes.positions()) {
        if (cheb(p, pos) > 12 || bit(enemy_spawn, p)) continue;
        near = true;
        for (auto d : kDirs)
            if (bit(goals, vec2{p.x + d.x, p.y + d.y})) return false;
    }
    return near;
}

// Dash and/or walk one step down field f (reached tiles in seen) towards goals. A step onto a tile that an
// unseen unit holds (a bush) fails without using the move and reveals the tile: try the next best tile once.
bool Bot::step_on(const cambc::distance_field& f, const cambc::bitboard& seen, const cambc::bitboard& goals,
                  const cambc::bitboard& blocked, bool dash) {
    auto& me = cambc::self();
    const vec2 pos = me.position();
    if (!live() || !inside(pos)) return false;
    if (dash && fdist(f, pos) >= 3 && !hold_dash(goals, pos) && try_dash(f, seen)) {
        moved_ = true;
        if (move_failed_ || !me.can_move()) return true;
    }
    const vec2 from = me.position();
    if (!inside(from)) return moved_;
    if (bit(goals, from)) return true;
    // Among the steps that gain the most, the one where the fewest awake camps (they hit the nearest champion
    // in reach) and enemy champions can hit us next round.
    std::array<std::pair<vec2, int>, 16> hot;  // awake fighting camp, its reach
    std::array<int, 16> hot_str;
    size_t n_hot = 0;
    if (kSafeSteps && !awake_.is_empty())
        for (auto p : awake_.positions()) {
            if (cheb(p, from) > 7 || n_hot >= hot.size()) continue;
            const int gi = group_at_[static_cast<size_t>(tidx(p))];
            const int ci = camp_at_[static_cast<size_t>(tidx(p))];
            if (gi < 0 || ci < 0) continue;
            const auto cs = camp_spec(g_[gi].kinds[static_cast<size_t>(ci)]);
            if (cs.str <= 0) continue;
            hot_str[n_hot] = static_cast<int>(cs.str);
            hot[n_hot++] = {p, cs.range};
        }
    std::array<vec2, 8> foes;
    size_t n_foes = 0;
    if (kSafeSteps) {
        const auto& v = view();
        if (!v.foes.is_empty())
            for (auto p : v.foes.positions())
                if (cheb(p, from) <= 6 && n_foes < foes.size()) foes[n_foes++] = p;
    }
    auto danger = [&](vec2 t) {
        int d = 0;
        for (size_t i = 0; i < n_hot; ++i)
            if (cheb(t, hot[i].first) <= hot[i].second) d += hot_str[i];
        for (size_t i = 0; i < n_foes; ++i)
            if (cheb(t, foes[i]) <= 4) d += 150;
        return d;
    };
    vec2 failed{-1, -1};
    for (int attempt = 0; attempt < 2; ++attempt) {
        vec2 best = from;
        int best_d = bit(seen, from) ? fdist(f, from) : kFar, best_risk = 0;
        for (auto d : kDirs) {
            const vec2 t{from.x + d.x, from.y + d.y};
            if (t == failed || !free_tile(t) || !bit(seen, t) || bit(blocked, t)) continue;
            const int fd = raw_dist(f.ptr(), t);
            if (fd > best_d || (fd == best_d && best == from)) continue;
            const int risk = n_hot + n_foes > 0 ? danger(t) : 0;
            if (fd < best_d || risk < best_risk) { best_d = fd; best = t; best_risk = risk; }
        }
        if (best == from || !live()) return moved_;
        bool ok = false;
        try { ok = me.move_absolute(best); } catch (const cambc::error& e) { warn("move", e); invalidate(); move_failed_ = true; moved_ = true; return false; }
        invalidate();
        if (ok) {
            moved_ = true;
            move_failed_ = false;
            return true;
        }
        failed = best;
        if (!me.can_move()) break;
    }
    moved_ = true;
    move_failed_ = true;
    return false;
}

void Bot::go_home() { go(cambc::map::spawn_region(team::ally), true); }

// Walk home when it is close, otherwise recall. True when the turn is used.
bool Bot::recall_or_walk(int walk_limit) {
    auto& me = cambc::self();
    if (spawn_local_.at(pos_)) return false;
    const int d = home_.at(pos_);
    if (d <= walk_limit) {
        go_home();
        return true;
    }
    if (moved_ || incoming_ > 0 || !live()) return moved_;
    try { me.recall(); } catch (const cambc::error& e) { warn("recall", e); }
    return true;
}

// Far from the group: one step down its cached walking field instead of a fresh route search. Off, as in the
// parent (routing every step with go() is cheap and also dashes and steps around units); kept as a switch.
bool Bot::walk_static(int, int) { return false; }
