// The fuel subsystem: turn clock, vision snapshot and cheap host reads (see shared.h).
//
// Measured on a 100x100 map (30x30 in brackets): a host call ~40; a new bitboard ~750 [260] (allocation
// and clear); a board op into an existing board ~200 [40]; positions() ~700 plus a vector; a champion
// handle (champion_at, champions()) ~800 because it reads both ability slots; camp_group::state() ~4k
// (it rebuilds the vision board); a full distance field 30k [1.1k] and its export 2.6k [0.3k].
// The old snapshot re-read four boards, exported four local copies and every ally after each move or
// hit: ~9k per refresh, several refreshes a turn.
#include "shared.h"
#include <algorithm>
#include <array>
#include <iostream>

std::uint32_t g_round = 0;

namespace {
std::uint64_t g_turn_f0 = 0;
}  // namespace

void begin_turn() {
    g_round = cambc::round_number();
    g_turn_f0 = cambc::fuel_remaining();
}
bool live() { return cambc::round_number() == g_round; }
bool suspended() { return !live(); }
bool fuel(std::uint64_t need) { return cambc::fuel_remaining() > need; }
std::uint64_t turn_budget() { return g_turn_f0; }
std::uint64_t turn_used() {
    const auto f = cambc::fuel_remaining();
    return g_turn_f0 > f ? g_turn_f0 - f : 0;
}

void warn(const char* where, const std::exception& e) {
    static int count = 0;
    if (++count <= 20) std::cout << "warn: " << where << ": " << e.what() << '\n';
}

// ---------------------------------------------------------------- snapshot

namespace {
// Built on first use: a bitboard sizes its storage from the SDK's map layout, which is itself captured during
// dynamic initialisation; a global board in this file may be constructed first, get no storage, and then
// every host write into it overruns the heap (it trapped on 01boss-battle).
View* g_view_p = nullptr;
View& gv() {
    if (!g_view_p) g_view_p = new View();
    return *g_view_p;
}

// What the current snapshot was taken from. Within our turn nobody else moves, so allies are read once a
// round, and the visible tiles only change when we move (we place no wards).
std::uint32_t g_vis_round = 0xffffffffu;
vec2 g_vis_pos{-1, -1};
std::uint32_t g_ally_round = 0xffffffffu;
std::vector<std::uint32_t> g_ally_handles;  // allied champions other than us
bool g_handles = false;
cambc::bitboard* g_tmp = nullptr;            // scratch for the enemy occupancy and foes()

std::array<std::vector<Foe>, 2> g_foes;
std::array<bool, 2> g_foes_ok{};
std::array<cambc::bitboard*, 3> g_locks{};   // ally, enemy, either
std::array<bool, 3> g_locks_ok{};

cambc::bitboard& tmp_board() {
    if (!g_tmp) g_tmp = new cambc::bitboard();
    return *g_tmp;
}

void read_ally_handles() {
    g_handles = true;
    std::uint32_t n = 0;
    if (cambc::detail::sdk_num_allies(&n) != 0) return;
    std::vector<std::uint32_t> h(n);
    if (cambc::detail::sdk_allies(h.data(), n, &n) != 0) return;
    h.resize(n);
    const auto me = static_cast<std::uint32_t>(static_cast<cambc::handle_champion_t>(cambc::self()));
    for (auto x : h)
        if (x != me) g_ally_handles.push_back(x);
}

void read_allies() {
    View& g_view = gv();
    if (!g_handles) read_ally_handles();
    g_view.allies.clear();
    g_view.ally_types.clear();
    for (auto h : g_ally_handles) {
        std::int32_t alive = 0;
        if (cambc::detail::sdk_champion_is_alive(h, &alive) != 0 || !alive) continue;
        cambc::detail::champion_info ci{};
        if (cambc::detail::sdk_champion(h, &ci) != 0) continue;
        const vec2 p{ci.x, ci.y};
        if (!inside(p)) continue;
        g_view.allies.push_back(p);
        g_view.ally_types.push_back(static_cast<cambc::champion_type>(ci.type));
    }
}
}  // namespace

void invalidate() {
    if (g_view_p) g_view_p->ok = false;
    g_foes_ok = {};
    g_locks_ok = {};
}

const View& view() {
    View& g_view = gv();
    if (g_view.ok) return g_view;
    const std::uint32_t now = cambc::round_number();
    const vec2 me = cambc::self().position();
    if (now != g_vis_round || me != g_vis_pos) {
        cambc::vision::visible_tiles_into(g_view.vis);
        g_vis_round = now;
        g_vis_pos = me;
        g_view.lvis.reset(&g_view.vis);
    }
    if (now != g_ally_round) {
        g_ally_round = now;
        read_allies();
    }
    // Our own hits can kill camps and champions; a dash moves us: units are re-read on every refresh.
    cambc::vision::camp_tiles_into(g_view.camps);
    cambc::vision::champion_tiles_into(team::ally, g_view.champs);
    auto& t = tmp_board();
    cambc::vision::champion_tiles_into(team::enemy, t);
    g_view.champs |= t;
    for (auto p : g_view.allies) g_view.champs.set(p);
    cambc::vision::teleport_tiles_into(g_view.tele);
    g_view.lcamps.reset(&g_view.camps);
    g_view.lchamps.reset(&g_view.champs);
    g_view.ltele.reset(&g_view.tele);
    g_view.ok = true;
    return g_view;
}

Bits g_walls_local, g_hard_local;  // filled in Bot::initialise()
const Bits& walls_local() { return g_walls_local; }
const Bits& hard_local() { return g_hard_local; }

bool free_tile(vec2 p) {
    if (!inside(p)) return false;
    if (p == cambc::self().position()) return true;
    const auto& v = view();
    return !walls_local().at(p) && !v.lcamps.at(p) && !v.lchamps.at(p) && !v.ltele.at(p);
}

// One host read per champion (the SDK handle would also query both ability slots), cached until the
// snapshot is invalidated.
std::vector<Foe> foes(team side) {
    const size_t s = side == team::enemy ? 1 : 0;
    if (g_foes_ok[s]) return g_foes[s];
    auto& out = g_foes[s];
    out.clear();
    const vec2 me = cambc::self().position();
    auto& b = tmp_board();
    cambc::vision::champion_tiles_into(side, b);
    std::array<vec2, 32> buf;
    const auto n = std::min<std::uint32_t>(b.positions_into(std::span<vec2>{buf}), static_cast<std::uint32_t>(buf.size()));
    for (std::uint32_t i = 0; i < n; ++i) {
        const vec2 p = buf[i];
        if (p == me) continue;
        std::uint32_t h = 0;
        if (cambc::detail::sdk_champion_at(p.x, p.y, &h) != 0) continue;
        std::int32_t alive = 0;
        if (cambc::detail::sdk_champion_is_alive(h, &alive) != 0 || !alive) continue;
        cambc::detail::champion_info ci{};
        if (cambc::detail::sdk_champion(h, &ci) != 0) continue;
        Foe f{p, ci.health, ci.max_health, 1.1f * ci.strength + 0.25f * ci.focus, ci.attack_range};
        if (side == team::enemy) {
            const auto st = cambc::champion_state::from_bits_retain(ci.state);
            f.chan = st.contains(cambc::champion_state::submitting_gems);
            f.recall = st.contains(cambc::champion_state::recalling);
            std::uint32_t g = 0;
            if (cambc::detail::sdk_champion_gems(h, &g) == 0) f.gems = static_cast<int>(g);
        }
        out.push_back(f);
    }
    g_foes_ok[s] = true;
    return out;
}

int group_state(const cambc::map::camp_group& g, int& timer) {
    const auto h = static_cast<std::uint32_t>(static_cast<cambc::handle_camp_group_t>(g));
    std::uint32_t st = 0;
    if (cambc::detail::sdk_camp_group_state(h, &st) != 0 || st > 3) return 0;
    if (st == 3) {
        std::int32_t t = 0;
        if (cambc::detail::sdk_camp_group_cooldown(h, &t) != 0) return 0;
        timer = t;
    }
    return static_cast<int>(st);
}

const cambc::bitboard& lock_tiles(team side) {
    const size_t s = side == team::enemy ? 1 : 0;
    if (!g_locks[s]) g_locks[s] = new cambc::bitboard();
    if (!g_locks_ok[s]) {
        cambc::vision::altar_lock_tiles_into(side, *g_locks[s]);
        g_locks_ok[s] = true;
    }
    return *g_locks[s];
}

const cambc::bitboard& lock_tiles_any() {
    if (!g_locks[2]) g_locks[2] = new cambc::bitboard();
    if (!g_locks_ok[2]) {
        const auto& a = lock_tiles(team::ally);
        const auto& e = lock_tiles(team::enemy);
        g_locks[2]->clear();
        *g_locks[2] |= a;
        *g_locks[2] |= e;
        g_locks_ok[2] = true;
    }
    return *g_locks[2];
}
