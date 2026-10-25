// Scaling-economy champion: farm gold and gems on our side, turn gold into stats at every
// spawn visit, bank gems at the nearest altar, and fight only when the numbers favour us.
#include <cambc.h>
#include "bot.h"
#include "fuel.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <string>
#include <span>
#include <utility>
#include <variant>

constexpr bool kDebugMap = false;
constexpr bool kStats = false;  // activity counters and positions in stdout
constexpr bool kTrace = false;
constexpr bool kTraceChoose = false;
std::uint64_t g_f0 = 0;
const char* g_phase_tag = "";
int now_dbg() { return static_cast<int>(cambc::round_number()); }
std::string g_fp_log;
void fp(const char* tag) {
    g_phase_tag = tag;
    if (!kTrace) return;
    const auto f = cambc::fuel_remaining();
    if (g_f0 > f && g_f0 - f > 3000) { g_fp_log += ' '; g_fp_log += tag; g_fp_log += '='; g_fp_log += std::to_string((g_f0 - f) / 1000); }
    g_f0 = f;
}
constexpr float kShareTurns = 15.0f;
constexpr float kBossTurns = 8.0f;  // turns a champion must be able to tank a boss
constexpr bool kLocks = true;       // carry an altar lock: drop it on enemy tributes and on altars we pass
constexpr bool kLockRunner = true;  // the last pick carries locks to far altars
constexpr int kLockDetour = 8;      // walk this far (Chebyshev) out of our way to lock an altar

namespace {
// Debug: turns spent per activity, printed every 200 rounds.
int g_mode = 0;
std::array<int, 20> g_modes{};
const char* kModeNames[20] = {"dead", "chan", "enemy", "fuel", "spwait", "hurt", "bank", "idle", "shop", "heal",
    "stuck", "travel", "fight", "capture", "respawn", "approach", "rally", "", "", ""};
struct ModeCount {
    std::uint64_t f0 = kStats ? cambc::fuel_remaining() : 0;
    ~ModeCount() {
        if (kStats) {
            const auto used = f0 - cambc::fuel_remaining();
            if (used > 70000)
                std::cout << "heavy r" << cambc::round_number() << ' ' << cambc::to_string(cambc::self().champion_name())
                          << ' ' << kModeNames[g_mode] << ' ' << used << g_fp_log << '\n';
            g_fp_log.clear();
        }
        ++g_modes[static_cast<size_t>(g_mode)];
        const int r = static_cast<int>(cambc::round_number());
        if (kStats && r % 200 == 0) {
            std::cout << "stat r" << r << ' ' << cambc::to_string(cambc::self().champion_name());
            for (int i = 0; i < 17; ++i) if (g_modes[static_cast<size_t>(i)]) std::cout << ' ' << kModeNames[i] << '=' << g_modes[static_cast<size_t>(i)];
            std::cout << '\n';
        }
    }
};
}  // namespace

namespace {

using it = cambc::item_type;

float ability_range(const cambc::ability& ab) {
    float r = 4;
    try {
        const auto& info = ab.info();
        if (info) {
            if (auto* d = std::get_if<cambc::ability_damage>(&*info)) r = d->range;
            else if (auto* c = std::get_if<cambc::ability_cc>(&*info)) r = c->range;
            else if (auto* a = std::get_if<cambc::ability_aoe_damage>(&*info)) r = a->range;
            else if (auto* h = std::get_if<cambc::ability_heal>(&*info)) r = h->range;
        }
    } catch (const std::exception&) {}
    return r;
}

struct Step { it shard, emblem; };
const std::vector<Step>& build_for(cambc::champion_type t) {
    static const std::vector<Step> plain{
        {it::jungle_shard_of_strength, it::jungle_emblem_of_strength},
        {it::monster_shard_of_strength, it::monster_emblem_of_strength},
        {it::power_shard_of_health, it::power_emblem_of_health},
        {it::power_shard_of_strength, it::power_emblem_of_strength},
        {it::durability_shard_of_health, it::durability_emblem_of_health},
    };
    static const std::vector<Step> vesper{
        {it::jungle_shard_of_strength, it::jungle_emblem_of_strength},
        {it::treasure_shard_of_strength, it::treasure_emblem_of_strength},
        {it::monster_shard_of_strength, it::monster_emblem_of_strength},
        {it::power_shard_of_health, it::power_emblem_of_health},
        {it::durability_shard_of_health, it::durability_emblem_of_health},
    };
    return t == cambc::champion_type::vesper_vale ? vesper : plain;
}

bool has_any(const cambc::enum_map<it>& c, it a, it b, it d) { return c[a] + c[b] + c[d] > 0; }

}  // namespace

// Shared memory: bytes 0-4 hold each slot's target group + 1; bytes 5-7 the latest enemy sighting (x + 1, y,
// round / 4). x + 1 and y stay below 128: bit 7 of byte 5 says the turn order is known, bit 7 of byte 6 holds
// the parity of the rounds our team starts.
Sighting read_sighting() {
    std::array<std::uint8_t, 3> b{};
    cambc::shared_memory.read(5, std::span<std::uint8_t>{b.data(), b.size()});
    Sighting s;
    const int x1 = b[0] & 0x7f, y = b[1] & 0x7f;
    if (x1 == 0) return s;
    s.where = {x1 - 1, y};
    const int now = static_cast<int>(cambc::round_number() >> 2) & 0xff;
    s.age = ((now - b[2]) & 0xff) * 4;
    s.ok = inside(s.where);
    return s;
}
void write_sighting(vec2 p) {
    std::array<std::uint8_t, 2> old{};
    cambc::shared_memory.read(5, std::span<std::uint8_t>{old.data(), old.size()});
    std::array<std::uint8_t, 3> b{static_cast<std::uint8_t>(((p.x + 1) & 0x7f) | (old[0] & 0x80)),
        static_cast<std::uint8_t>((p.y & 0x7f) | (old[1] & 0x80)),
        static_cast<std::uint8_t>((cambc::round_number() >> 2) & 0xff)};
    cambc::shared_memory.write(5, std::span<const std::uint8_t>{b.data(), b.size()});
}
int read_start_parity() {
    std::array<std::uint8_t, 2> b{};
    cambc::shared_memory.read(5, std::span<std::uint8_t>{b.data(), b.size()});
    if (!(b[0] & 0x80)) return -1;
    return (b[1] >> 7) & 1;
}
void write_start_parity(int parity) {
    std::array<std::uint8_t, 2> b{};
    cambc::shared_memory.read(5, std::span<std::uint8_t>{b.data(), b.size()});
    b[0] = static_cast<std::uint8_t>(b[0] | 0x80);
    b[1] = static_cast<std::uint8_t>((b[1] & 0x7f) | (parity ? 0x80 : 0));
    cambc::shared_memory.write(5, std::span<const std::uint8_t>{b.data(), b.size()});
}

bool has_lock() {
    try { return cambc::self().consumables()[cambc::consumable_type::altar_lock].count() > 0; }
    catch (const cambc::error&) { return false; }
}

// ---------------------------------------------------------------- knowledge (map analysis, observe(): fuel.cpp)

void Bot::measure() {
    auto& me = cambc::self();
    incoming_ = 0;
    exit_steps_ = 0;
    const auto& v = view();
    awake_ = cambc::vision::awake_camp_tiles();
    // Camps see (and hit) through bushes that hide them from us: remember awake camps for a few rounds
    // after they drop out of sight, or we would recall (and keep being hit) right beside them.
    {
        const int now = static_cast<int>(cambc::round_number());
        if (!awake_.is_empty())
            for (auto p : awake_.positions()) {
                bool found = false;
                for (auto& m : awake_mem_)
                    if (m.first == p) { m.second = now; found = true; break; }
                if (!found) awake_mem_.push_back({p, now});
            }
        std::erase_if(awake_mem_, [&](const std::pair<vec2, int>& m) {
            if (now - m.second > 3 || v.lvis.at(m.first)) return now != m.second;
            const int gi = group_at_[static_cast<size_t>(tidx(m.first))];
            const int ci = camp_at_[static_cast<size_t>(tidx(m.first))];
            return gi < 0 || ci < 0 || k_[gi].hp[static_cast<size_t>(ci)] <= 0;
        });
        for (const auto& m : awake_mem_)
            if (m.second != now) awake_.set(m.first);
    }
    if (!awake_.is_empty())
        for (auto p : awake_.positions()) {
            const int d = cheb(p, pos_);
            if (d > 5) continue;
            const int gi = group_at_[static_cast<size_t>(tidx(p))];
            const int ci = camp_at_[static_cast<size_t>(tidx(p))];
            if (gi < 0 || ci < 0) continue;
            const auto s = camp_spec(g_[gi].kinds[static_cast<size_t>(ci)]);
            if (s.str <= 0 || d > s.range) continue;
            // A camp hits the nearest champion: an ally closer to it takes the blows.
            bool shielded = false;
            for (auto a : v.allies)
                if (cheb(a, p) < d) { shielded = true; break; }
            if (shielded) continue;
            incoming_ += s.str;
            exit_steps_ = std::max(exit_steps_, s.range + 1 - d);
        }
    const auto counts = me.item_count();
    taken_ = 1.0f;
    if (has_any(counts, it::jungle_emblem_of_strength, it::jungle_emblem_of_health, it::jungle_emblem_of_focus)) taken_ *= 0.7f;
    if (has_any(counts, it::durability_emblem_of_strength, it::durability_emblem_of_health, it::durability_emblem_of_focus)) taken_ *= 0.9f;
    float dps = me.strength();
    float spell = 0;
    bool reset = false;
    for (const auto& ab : me.abilities()) {
        if (ab.is_attack_reset()) reset = true;
        if (ab.is_damage() || ab.is_aoe_damage() || ab.is_cc()) {
            try { spell += ab.damage_amount(); } catch (const std::exception&) {}
        }
    }
    if (reset) dps *= 9.0f / 8.0f;
    if (me.champion_name() == cambc::champion_type::vendetta) dps *= 1.25f;
    dps += spell / 8.0f;
    if (has_any(counts, it::monster_emblem_of_strength, it::monster_emblem_of_health, it::monster_emblem_of_focus)) dps *= 1.1f;
    dps_ = std::max(10.0f, dps);
}

void Bot::write_claim(int group) {
    if (slot_ < 0 || slot_ > 4) return;
    std::uint8_t b = group >= 0 && group < 250 ? static_cast<std::uint8_t>(group + 1) : 0;
    cambc::shared_memory.write(static_cast<size_t>(slot_), std::span<const std::uint8_t>{&b, 1});
}

int Bot::claimed_by_other(int group) const {
    std::array<std::uint8_t, 5> b{};
    cambc::shared_memory.read(0, std::span<std::uint8_t>{b.data(), b.size()});
    for (int s = 0; s < 5; ++s)
        if (s != slot_ && b[static_cast<size_t>(s)] == group + 1) return s;
    return -1;
}

// ---------------------------------------------------------------- shopping

void Bot::shop() {
    auto& me = cambc::self();
    // An altar lock (10 gold) cancels an enemy tribute and blocks enemy tributes: always carry one.
    if (kLocks && me.gold_count() >= 10 && fuel(15000) && live() && !has_lock()
        && (!me.is_alive() || spawn_local_.at(pos_))) {
        try {
            if (me.can_buy(cambc::consumable_type::altar_lock)) me.buy(cambc::consumable_type::altar_lock);
        } catch (const cambc::error& e) { warn("buy lock", e); }
    }
    if (me.gold_count() < 30) return;
    const auto& plan = build_for(me.champion_name());
    for (int guard = 0; guard < 30; ++guard) {
        if (me.gold_count() < 30 || !fuel(15000) || !live()) return;
        const auto counts = me.item_count();
        const Step* next = nullptr;
        std::array<int, static_cast<size_t>(it::enum_type::count)> need{};
        for (const auto& s : plan) {
            const auto e = static_cast<size_t>(static_cast<it::enum_type>(s.emblem));
            ++need[e];
            if (static_cast<int>(counts[s.emblem]) < need[e]) { next = &s; break; }
        }
        // Past the plan: power shards, alternating strength and health.
        const int str = static_cast<int>(counts[it::power_shard_of_strength] + 6 * counts[it::power_emblem_of_strength]);
        const int hp = static_cast<int>(counts[it::power_shard_of_health] + 6 * counts[it::power_emblem_of_health]);
        const it shard = next ? next->shard : (str <= hp ? it::power_shard_of_strength : it::power_shard_of_health);
        if (!me.can_buy(shard, 1)) { cant_buy_ = true; return; }
        try { me.buy(shard, 1); } catch (const cambc::error& e) { warn("buy", e); cant_buy_ = true; return; }
        cant_buy_ = false;
    }
}

// ---------------------------------------------------------------- camps
//
// Camp farming. A group is worth gems + w·gold (w falls over the game) per champion-turn spent on it:
// travel, respawn wait, clear time, the trips home to heal when one health bar is not enough, and the
// capture. Clears are costed with a damage model: camps die in the order that hurts least (threat per
// health first, harmless clusters last) and a camp only hurts us while it reaches the side of the target we
// hit from, which is where approach_goal() puts us. Groups one champion can wear down are farmed alone (a
// heavy one an ally works on may be shared); bosses and packs too strong for that are raided: the raid
// size with the best rate is planned, raiders gather just outside the fighters' reach and engage together
// (a camp hits the nearest champion, so the tank rotates as the hurt ones step out), and they all stand on
// the region at capture so a big reward is split and banks in short tributes.

namespace {
constexpr float kCaptureTurns = 6.0f;  // five capture phases and the step onto the region
constexpr float kTripTurns = 8.0f;     // recall and the heal on spawn, before walking back
constexpr int kRallyRounds = 35;       // longest wait for raiders outside a boss's reach
constexpr float kGatherTurns = 10.0f;  // expected wait for raiders not yet on their way
}  // namespace

void Bot::build_threats() {
    threat_.assign(g_.size(), {});
    for (size_t i = 0; i < g_.size(); ++i) {
        const auto& g = g_[i];
        const size_t n = g.tiles.size();
        auto& t = threat_[i];
        t.assign(n * n, 0);
        for (size_t m = 0; m < n; ++m) {
            const auto s = camp_spec(g.kinds[m]);
            if (s.str <= 0) continue;
            for (size_t c = 0; c < n; ++c)
                if (m == c || cheb(g.tiles[m], g.tiles[c]) + my_range_ <= s.range) t[m * n + c] = 1;
        }
    }
    cost_.assign(g_.size(), ClearCost{});
}

float Bot::remaining(int group) const {
    float t = 0;
    for (float hp : k_[group].hp) t += hp;
    return t;
}

// Σ hp·incoming over the kill order (divide by team damage for the damage taken) and the worst incoming.
const Bot::ClearCost& Bot::clear_cost(int group) {
    auto& cc = cost_[group];
    const float left = remaining(group);
    if (cc.hp_sum == left) return cc;
    cc.hp_sum = left;
    const auto& g = g_[group];
    const auto& k = k_[group];
    const size_t n = g.tiles.size();
    std::array<std::pair<float, std::uint8_t>, 64> order;
    std::array<std::uint8_t, 64> alive{};
    size_t m = 0;
    for (size_t c = 0; c < n && c < 64; ++c) {
        if (k.hp[c] <= 0) continue;
        alive[c] = 1;
        order[m++] = {camp_spec(g.kinds[c]).str / std::max(1.0f, k.hp[c]), static_cast<std::uint8_t>(c)};
    }
    std::sort(order.begin(), order.begin() + m, [](auto a, auto b) { return a.first > b.first; });
    const auto& t = threat_[group];
    cc.base = 0;
    cc.peak = 0;
    for (size_t j = 0; j < m; ++j) {
        const size_t c = order[j].second;
        float rate = 0;
        for (size_t o = 0; o < n && o < 64; ++o)
            if (alive[o] && t[o * n + c]) rate += camp_spec(g.kinds[o]).str;
        cc.base += k.hp[c] * rate;
        cc.peak = std::max(cc.peak, rate);
        alive[c] = 0;
    }
    return cc;
}

// Damage each of n raiders takes clearing the group (the hurt one steps out and the next nearest tanks).
float Bot::estimate_damage(int group) {
    const int n = group == target_ && raid_n_ > 1 ? raid_n_ : 1;
    return clear_cost(group).base / (dps_ * n) * taken_ / n;
}

// Tiles within r of a living camp of the group, keeping the ones the fewest fighters reach (we hit from
// the side away from the others). Cached while the living set stays the same.
const cambc::bitboard& Bot::approach_goal(int group, int r) {
    int sig = 0;
    for (size_t c = 0; c < k_[group].hp.size(); ++c)
        if (k_[group].hp[c] > 0) sig = sig * 31 + static_cast<int>(c) + 1;
    const int now = static_cast<int>(cambc::round_number());
    const bool plain = plain_until_ > now;
    if (goal_group_ == group && goal_r_ == r && goal_sig_ == sig && goal_plain_ == plain) return goal_;
    goal_group_ = group;
    goal_r_ = r;
    goal_sig_ = sig;
    goal_plain_ = plain;
    const auto& g = g_[group];
    const auto& k = k_[group];
    const size_t n = g.tiles.size();
    // Box around the living camps.
    int x0 = map_w(), y0 = map_h(), x1 = -1, y1 = -1, hostile = 0, living = 0;
    for (size_t c = 0; c < n; ++c) {
        if (k.hp[c] <= 0) continue;
        ++living;
        if (camp_spec(g.kinds[c]).str > 0) ++hostile;
        x0 = std::min(x0, g.tiles[c].x); y0 = std::min(y0, g.tiles[c].y);
        x1 = std::max(x1, g.tiles[c].x); y1 = std::max(y1, g.tiles[c].y);
    }
    const int bw = x1 - x0 + 2 * r + 1, bh = y1 - y0 + 2 * r + 1;
    const long work = static_cast<long>(living) * (2 * r + 1) * (2 * r + 1) + static_cast<long>(hostile) * 121 + 2L * bw * bh;
    if (living == 0 || plain || hostile == 0 || work > 2500 || !fuel(60000 + 40 * static_cast<std::uint64_t>(work))) {
        auto live_b = empty_board();
        for (size_t c = 0; c < n; ++c)
            if (k.hp[c] > 0) live_b.set(g.tiles[c]);
        if (live_b.is_empty()) live_b = gtiles_[group];
        goal_ = grow(live_b, r) & ~walk_walls();
        return goal_;
    }
    const int bx = x0 - r, by = y0 - r;
    // reach[i]: 1 when the tile is within r of a living camp; danger[i]: strength of fighters reaching it.
    std::vector<std::uint8_t> reach(static_cast<size_t>(bw * bh), 0);
    std::vector<std::int16_t> danger(static_cast<size_t>(bw * bh), 0);
    for (size_t c = 0; c < n; ++c) {
        if (k.hp[c] <= 0) continue;
        const vec2 p = g.tiles[c];
        for (int y = p.y - r; y <= p.y + r; ++y) {
            std::uint8_t* row = reach.data() + (y - by) * bw + (p.x - r - bx);
            for (int x = 0; x <= 2 * r; ++x) row[x] = 1;
        }
    }
    for (size_t c = 0; c < n; ++c) {
        if (k.hp[c] <= 0) continue;
        const auto s = camp_spec(g.kinds[c]);
        if (s.str <= 0) continue;
        const vec2 p = g.tiles[c];
        const int ya = std::max(by, p.y - s.range), yb = std::min(by + bh - 1, p.y + s.range);
        const int xa = std::max(bx, p.x - s.range), xb = std::min(bx + bw - 1, p.x + s.range);
        const auto add = static_cast<std::int16_t>(s.str);
        for (int y = ya; y <= yb; ++y) {
            std::int16_t* row = danger.data() + (y - by) * bw;
            for (int x = xa; x <= xb; ++x) row[x - bx] = static_cast<std::int16_t>(row[x - bx] + add);
        }
    }
    // Living camps and walls are not standing spots.
    for (size_t c = 0; c < n; ++c)
        if (k.hp[c] > 0) reach[static_cast<size_t>((g.tiles[c].y - by) * bw + g.tiles[c].x - bx)] = 0;
    int best = 1 << 30;
    for (int y = 0; y < bh; ++y)
        for (int x = 0; x < bw; ++x) {
            const size_t i = static_cast<size_t>(y * bw + x);
            if (!reach[i]) continue;
            const vec2 p{bx + x, by + y};
            if (!inside(p) || walls_local().at(p)) { reach[i] = 0; continue; }
            best = std::min<int>(best, danger[i]);
        }
    goal_buf_.assign(cambc::bitboard::linear_buffer_size(), 0);
    bool any = false;
    for (int y = 0; y < bh; ++y)
        for (int x = 0; x < bw; ++x) {
            const size_t i = static_cast<size_t>(y * bw + x);
            if (!reach[i] || danger[i] > best) continue;
            const int t = tidx(vec2{bx + x, by + y});
            goal_buf_[static_cast<size_t>(t >> 3)] |= static_cast<std::uint8_t>(1u << (t & 7));
            any = true;
        }
    if (!any) {
        goal_plain_ = true;
        auto live_b = empty_board();
        for (size_t c = 0; c < n; ++c)
            if (k.hp[c] > 0) live_b.set(g.tiles[c]);
        goal_ = grow(live_b, r) & ~walk_walls();
        return goal_;
    }
    goal_.import_linear_buffer(std::span<const std::uint8_t>{goal_buf_});
    return goal_;
}

// Tiles one or two steps outside the reach of the group's living fighters: where raiders gather.
// Built on a local box (whole-board dilations by a boss's reach cost tens of thousands of fuel).
const cambc::bitboard& Bot::rally_goal(int group) {
    if (rally_goal_g_ == group && rally_goal_round_ + 10 > static_cast<int>(cambc::round_number())) return rally_goal_;
    rally_goal_g_ = group;
    rally_goal_round_ = static_cast<int>(cambc::round_number());
    const auto& g = g_[group];
    const auto& k = k_[group];
    int x0 = map_w(), y0 = map_h(), x1 = -1, y1 = -1;
    for (size_t c = 0; c < g.tiles.size(); ++c) {
        if (k.hp[c] <= 0 || camp_spec(g.kinds[c]).str <= 0) continue;
        x0 = std::min(x0, g.tiles[c].x); y0 = std::min(y0, g.tiles[c].y);
        x1 = std::max(x1, g.tiles[c].x); y1 = std::max(y1, g.tiles[c].y);
    }
    rally_goal_ = empty_board();
    if (x1 < 0) return rally_goal_;
    const int pad = 7;  // largest reach (5) plus the ring
    const int bx = x0 - pad, by = y0 - pad, bw = x1 - x0 + 2 * pad + 1, bh = y1 - y0 + 2 * pad + 1;
    // 2: inside some fighter's reach, 1: in the ring just outside it.
    std::vector<std::uint8_t> mark(static_cast<size_t>(bw * bh), 0);
    for (size_t c = 0; c < g.tiles.size(); ++c) {
        if (k.hp[c] <= 0) continue;
        const auto s = camp_spec(g.kinds[c]);
        if (s.str <= 0) continue;
        const vec2 p = g.tiles[c];
        for (int y = p.y - s.range - 2; y <= p.y + s.range + 2; ++y) {
            std::uint8_t* row = mark.data() + (y - by) * bw;
            const int dy = y > p.y ? y - p.y : p.y - y;
            for (int x = p.x - s.range - 2; x <= p.x + s.range + 2; ++x) {
                const int dx = x > p.x ? x - p.x : p.x - x;
                const std::uint8_t v = (dx > dy ? dx : dy) <= s.range ? 2 : 1;
                if (row[x - bx] < v) row[x - bx] = v;
            }
        }
    }
    goal_buf_.assign(cambc::bitboard::linear_buffer_size(), 0);
    for (int y = 0; y < bh; ++y)
        for (int x = 0; x < bw; ++x) {
            if (mark[static_cast<size_t>(y * bw + x)] != 1) continue;
            const vec2 p{bx + x, by + y};
            if (!inside(p) || walls_local().at(p)) continue;
            const int t = tidx(p);
            goal_buf_[static_cast<size_t>(t >> 3)] |= static_cast<std::uint8_t>(1u << (t & 7));
        }
    rally_goal_.import_linear_buffer(std::span<const std::uint8_t>{goal_buf_});
    return rally_goal_;
}

// Champions (us included) close enough to the group to join its fight within a few turns.
int Bot::raiders_near(int group) const {
    int n = 1;
    const int r = reach_[group] + g_[group].range + 4;
    for (auto a : view().allies)
        if (cheb(a, g_[group].center) <= r) ++n;
    return n;
}

bool Bot::hit_group(int group) {
    auto& me = cambc::self();
    if (!me.can_attack() || !live()) return false;
    const vec2 pos = me.position();
    const int range = me.attack_range();
    const float str = me.strength();
    float best_score = -1e9f;
    vec2 best{};
    bool found = false;
    const auto& v = view();
    for (size_t c = 0; c < g_[group].tiles.size(); ++c) {
        const vec2 p = g_[group].tiles[c];
        if (cheb(p, pos) > range || !v.lvis.at(p) || !v.lcamps.at(p)) continue;
        const float hp = std::max(1.0f, k_[group].hp[c]);
        const auto s = camp_spec(g_[group].kinds[c]);
        // Most strength removed per hit first; a camp that reaches us outranks one that does not.
        float score = s.str / hp * 1e4f - hp * 1e-3f;
        if (s.str > 0 && cheb(p, pos) <= s.range) score += 50;
        if (hp <= str) score += 1e6f;  // a kill this hit
        if (score > best_score) { best_score = score; best = p; found = true; }
    }
    if (!found) return false;
    try {
        auto c = cambc::vision::camp_at(best);
        me.attack(c);
        attacked_ = true;
        invalidate();
        const int ci = camp_at_[static_cast<size_t>(tidx(best))];
        if (ci >= 0) k_[group].hp[static_cast<size_t>(ci)] = std::max(1.0f, k_[group].hp[static_cast<size_t>(ci)] - str);
        return true;
    } catch (const cambc::error& e) {
        warn("hit", e);
        return false;
    }
}

namespace {
// Best plan for one group: champion-turn rate, raid size and whether raiders gather first.
struct Plan { float score = 0; int n = 1; bool rally = false; };
}  // namespace

int Bot::choose(int& wait_out) {
    auto& me = cambc::self();
    const int now = static_cast<int>(cambc::round_number());
    const float hp = me.health(), max_hp = me.max_health();
    const float margin = 100 + 0.1f * max_hp;
    const float cap = std::max(1.0f, max_hp - margin);  // damage one trip can absorb from full health
    const float spare = std::max(0.0f, hp - margin);      // ... and from where we stand now
    const auto s = read_sighting();
    const float gold_w = (me.champion_name() == cambc::champion_type::vesper_vale ? 0.5f : 0.35f)
        * std::max(0.4f, 1.0f - now / 1500.0f);
    const int alive = 1 + static_cast<int>(view().allies.size());
    int best = -1;
    Plan best_plan;
    wait_out = 0;
    std::array<std::uint8_t, 5> claims{};
    cambc::shared_memory.read(0, std::span<std::uint8_t>{claims.data(), claims.size()});
    // Turns for n champions of our strength to clear the group, and the trips each needs on top.
    auto trips_for = [&](const ClearCost& cc, int n, float& turns) {
        turns = cc.hp_sum / (dps_ * n);
        const float each = cc.base / (dps_ * n) * taken_ / n;
        return each <= spare ? 0.0f : (each - spare) / cap;  // expected trips home
    };
    // The current target first: a good early best prunes most of the others cheaply.
    const int n_groups = static_cast<int>(g_.size());
    for (int j = -1; j < n_groups; ++j) {
        const int i = j < 0 ? target_ : j;
        if (i < 0 || (j >= 0 && i == target_)) continue;
        const auto& g = g_[i];
        const auto& k = k_[i];
        if (k.blocked_until > now) continue;
        float value = g.gems + gold_w * g.gold;
        if (value <= 0) continue;
        const int e = eta(i);
        if (e >= kFar) continue;
        if (value * 1.7f / (e + kCaptureTurns) <= best_plan.score) continue;  // cannot beat the best
        const int avail = k.state == Known::respawning ? k.ready : now;
        const int wait = std::max(0, avail - (now + e));
        float f = 1.0f;
        if (home_slot_.size() == g_.size()) {
            if (home_slot_[i] == 255) f = 0.8f;
            else if (home_slot_[i] != slot_) f = 0.6f;
        }
        if (g.d_enemy + 3 < g.d_ally) f *= 0.7f;
        if (k.state == Known::reward) f *= 1.3f;
        const bool waiting = k.state == Known::respawning && wait > 0;
        if (i == target_ && !waiting) f *= 1.25f;
        if (s.ok && s.age <= 24 && cheb(s.where, g.center) <= 7) f *= 0.5f;
        int claimers = 0;
        for (int o = 0; o < 5; ++o)
            if (o != slot_ && claims[static_cast<size_t>(o)] == i + 1) ++claimers;
        const float trip = kTripTurns + std::min(60, g.d_ally);
        // Gems still have to reach an altar: about one walk there per 45 carried.
        const float bank = g.gems / 45.0f * std::min(bank_d_[static_cast<size_t>(i)], 6 + home_altar_);
        const float base_t = e + wait + kCaptureTurns;
        Plan plan;
        if (k.state == Known::reward) {
            // The claimers nearest the region capture (raiders split a boss's reward, as many as its
            // region holds; one is enough elsewhere); the others move on.
            const int room = g.boss ? static_cast<int>(g.tiles.size()) : 1;
            int nearer = 0;
            if (claimers > 0) {
                const int mine = cheb(pos_, g.center);
                const auto& v = view();
                for (size_t a = 0; a < v.allies.size(); ++a) {
                    const int o = slot_of(v.ally_types[a], size_);
                    if (o < 0 || o > 4 || claims[static_cast<size_t>(o)] != i + 1) continue;
                    const int theirs = cheb(v.allies[a], g.center);
                    if (theirs < mine || (theirs == mine && o < slot_)) ++nearer;
                }
            }
            if (nearer >= room) f *= 0.1f;
            else plan.n = std::min(claimers + 1, room);
            plan.score = value / plan.n * f / (e + kCaptureTurns + bank / plan.n);
        } else {
            if (value * f / (base_t + bank) <= best_plan.score) continue;
            const auto& cc = clear_cost(i);
            const float peak = cc.peak * taken_;
            // Alone: worn down over several trips when one health bar is not enough.
            if (!g.boss && cap >= 5 * peak) {
                float turns = 0;
                const float trips = trips_for(cc, 1, turns);
                if (trips <= 4) {
                    float sc = value * f / (base_t + bank + turns + trips * trip);
                    if (claimers > 0) {
                        // Someone is on it: share a long clear, leave a short one to them.
                        if (turns > kShareTurns || trips > 0) {
                            float t2 = 0;
                            const float tr2 = trips_for(cc, claimers + 1, t2);
                            sc = value / (claimers + 1) * f / (base_t + bank / (claimers + 1) + t2 + tr2 * trip);
                            if (sc > plan.score) { plan.score = sc; plan.n = claimers + 1; plan.rally = false; }
                            sc = 0;
                        } else {
                            for (int c = 0; c < claimers; ++c) sc *= 0.3f;
                        }
                    }
                    if (sc > plan.score) { plan.score = sc; plan.n = 1; plan.rally = false; }
                }
            }
            // Raid: n champions of our strength gather outside the fighters' reach, then engage together.
            if (alive >= 2 && (g.boss || plan.score <= 0)) {
                for (int n = std::max(2, claimers + 1); n <= alive; ++n) {
                    if (cap * n < 3 * peak) continue;
                    float turns = 0;
                    const float trips = trips_for(cc, n, turns);
                    if (trips > 3) continue;
                    const float gather = n > claimers + 1 ? kGatherTurns : 0.0f;
                    // Join a raid that is forming rather than start a second one.
                    const float join = waiting ? 1.0f : 1.0f + 0.5f * claimers;
                    const float sc = value / n * f * join / (base_t + bank / n + gather + turns + trips * trip);
                    if (sc > plan.score) { plan.score = sc; plan.n = n; plan.rally = true; }
                }
            }
        }
        if (plan.score > best_plan.score) { best_plan = plan; best = i; wait_out = wait; }
    }
    raid_n_ = best_plan.n;
    rally_need_ = best_plan.rally;
    if (kTraceChoose && best >= 0 && best != target_ && now < 700)
        std::cout << "choose r" << now << ' ' << cambc::to_string(me.champion_name()) << " g" << best << " st " << int(k_[best].state)
                  << " gems " << g_[best].gems << " gold " << g_[best].gold << " hp " << remaining(best) << " eta " << eta(best)
                  << " wait " << wait_out << " n " << raid_n_ << " rally " << rally_need_ << " score " << best_plan.score
                  << " dps " << dps_ << " myhp " << hp << '/' << max_hp << '\n';
    return best;
}

void Bot::farm(int group) {
    const auto& k = k_[group];
    const int now = static_cast<int>(cambc::round_number());
    const int e = eta(group);

    g_mode = e > 3 ? 11 : 15;
    if (k.state == Known::reward) {
        g_mode = 13;
        const auto& tiles = gtiles_[group];
        if (tiles.is_set(pos_)) {
            if (capture_g_ != group) { capture_g_ = group; capture_since_ = now; }
            if (now - capture_since_ > cambc::constants::capture_time + 6) {
                k_[group].blocked_until = now + 60;
                capture_g_ = -1;
            }
            return;
        }
        if (!walk_static(group, e)) go(tiles, e > 3);
        return;
    }
    capture_g_ = -1;
    if (k.state == Known::respawning) {
        g_mode = 14;
        // Wait close by but off the spawn tiles: standing on one delays the respawn.
        auto near = grow(gtiles_[group], 2) & ~gtiles_[group] & ~walk_walls();
        if (!near.is_set(pos_) && !walk_static(group, e)) go(near, true);
        return;
    }
    // A raid gathers outside the fighters' reach and engages together (or once someone has started).
    if (group == target_ && raid_n_ >= 2 && rally_need_ && !group_awake(group)) {
        if (rally_g_ != group) { rally_g_ = group; rally_since_ = -1; }
        const int near = raiders_near(group);
        const auto& rally = rally_goal(group);
        if (near < raid_n_ && !rally.is_empty()) {
            if (rally.is_set(pos_) && rally_since_ < 0) rally_since_ = now;
            if (rally_since_ < 0 || now - rally_since_ < kRallyRounds) {
                g_mode = 16;
                if (!rally.is_set(pos_) && !walk_static(group, e - g_[group].range - 2)) go(rally, e > 3);
                return;
            }
            if (near < 2) {
                // Nobody came: try something else for a while.
                k_[group].blocked_until = now + 80;
                target_ = -1;
                return;
            }
        }
    }
    spells_on_group(group);
    if (hit_group(group)) {
        g_mode = 12;
        reset_hit(group);
        return;
    }
    if (cast_) g_mode = 12;
    // Approach tiles in attack range of the camps we believe alive.
    if (shrink_g_ != group) { shrink_g_ = group; shrink_ = 0; }
    int r = std::max(1, cambc::self().attack_range() - shrink_);
    const auto& goal = approach_goal(group, r);
    if (goal.is_set(pos_) && !attacked_ && r > 1) {
        // In range but nothing to hit: walls hide the camps, come closer.
        ++shrink_;
        --r;
    }
    const auto& goal2 = approach_goal(group, r);
    const bool moved_before = moved_;
    fp("farm-goal");
    if (walk_static(group, e - r)) { fp("farm-static"); return; }
    const bool ok = go(goal2, e > 3);
    fp("farm-go");
    // The safest spots cannot be reached: fall back to any spot in range for a while.
    if (!ok && !moved_before && !moved_ && !goal2.is_set(pos_) && !goal_plain_) plain_until_ = now + 12;
    if (hit_group(group) || moved_) reset_hit(group);
}

// Step out of reach of awake camps.
bool Bot::escape_camps() {
    std::array<cambc::bitboard, 6> by_range;
    for (auto& b : by_range) b.clear();
    bool any = false;
    if (!awake_.is_empty())
        for (auto p : awake_.positions()) {
            if (cheb(p, pos_) > 7) continue;
            const int gi = group_at_[static_cast<size_t>(tidx(p))];
            const int ci = camp_at_[static_cast<size_t>(tidx(p))];
            if (gi < 0 || ci < 0) continue;
            const auto s = camp_spec(g_[gi].kinds[static_cast<size_t>(ci)]);
            if (s.str <= 0) continue;
            by_range[static_cast<size_t>(std::clamp(s.range, 0, 5))].set(p);
            any = true;
        }
    if (!any) return false;
    auto danger = empty_board();
    for (int r = 1; r <= 5; ++r)
        if (!by_range[static_cast<size_t>(r)].is_empty()) danger |= grow(by_range[static_cast<size_t>(r)], r);
    if (!danger.is_set(pos_)) return false;
    auto safe = empty_board();
    safe.fill();
    safe &= ~danger;
    safe &= ~walk_walls();
    return go(safe, true);
}

// ---------------------------------------------------------------- enemies: combat.cpp

// ---------------------------------------------------------------- gems and altar locks: endgame.cpp

// ---------------------------------------------------------------- turn


void Bot::play_turn() {
    try {
        turn();
    } catch (const std::exception& e) {
        // Never let one failed action end the turn with an error; log where it happened.
        static int count = 0;
        if (++count <= 20) std::cout << "warn: turn r" << cambc::round_number() << " after " << g_phase_tag << ": " << e.what() << '\n';
    }
}

void Bot::turn() {
    if (kTrace) last_mode_ = g_mode;
    ModeCount counter;
    g_mode = 0;
    g_f0 = cambc::fuel_remaining();
    g_fp_log.clear();
    g_round = cambc::round_number();
    auto& me = cambc::self();
    pos_ = me.position();
    if (!init_) {
        initialise();
        if (slot_ == 0 && kDebugMap) {
            Bits wl, ho, al, sa, se, bu;
            wl.load(cambc::map::walls()); ho.load(cambc::map::holes()); al.load(cambc::map::altars());
            sa.load(cambc::map::spawn_region(team::ally)); se.load(cambc::map::spawn_region(team::enemy)); bu.load(cambc::map::bushes());
            for (int y = map_h() - 1; y >= 0; --y) {
                std::string row = "map ";
                for (int x = 0; x < map_w(); ++x) {
                    const vec2 p{x, y};
                    char c = '.';
                    if (wl.at(p)) c = '#';
                    else if (ho.at(p)) c = 'o';
                    else if (al.at(p)) c = 'A';
                    else if (sa.at(p)) c = 'S';
                    else if (se.at(p)) c = 'E';
                    else if (bu.at(p)) c = ',';
                    const int gi = group_at_[static_cast<size_t>(tidx(p))];
                    if (gi >= 0) c = "wegGMK"[static_cast<int>(g_[gi].kinds[static_cast<size_t>(camp_at_[static_cast<size_t>(tidx(p))])])];
                    row += c;
                }
                std::cout << row << '\n';
            }
        }
    }
    moved_ = attacked_ = cast_ = move_failed_ = false;
    invalidate();
    if (kTrace && me.is_alive()) alive_round_ = static_cast<int>(g_round);

    if (!me.is_alive()) {
        if (kTrace && alive_round_ == static_cast<int>(g_round) - 1)
            std::cout << "died r" << g_round << ' ' << cambc::to_string(me.champion_name()) << " fight_r" << fight_round_
                      << " engaged " << fight_engaged_ << " mode " << last_mode_ << '\n';
        g_mode = 0;
        write_claim(-1);
        shop();
        fill_fields(30000);
        return;
    }
    const bool at_spawn = cambc::map::spawn_region(team::ally).is_set(pos_);
    if (kTrace && now_dbg() == 50 && slot_ == 0) {
        auto f0 = cambc::fuel_remaining();
        std::uint32_t acc = 0;
        for (int i = 0; i < 100; ++i) acc += cambc::round_number();
        auto f1 = cambc::fuel_remaining();
        int c = 0;
        for (int i = 0; i < 1000; ++i) c += walls_local().at(vec2{i % 40, i / 40});
        auto f2 = cambc::fuel_remaining();
        for (int i = 0; i < 100; ++i) c += cambc::map::walls().is_set(vec2{i % 40, 3});
        auto f3 = cambc::fuel_remaining();
        auto vv = cambc::vision::visible_tiles();
        auto f4 = cambc::fuel_remaining();
        Bits bb; bb.load(vv);
        auto f5 = cambc::fuel_remaining();
        auto gr = grow(vv, 2);
        auto f6 = cambc::fuel_remaining();
        cambc::distance_field df; cambc::bitboard sn;
        df.compute(walk_walls(), cambc::map::spawn_region(team::ally), sn);
        auto f7 = cambc::fuel_remaining();
        std::cout << "bench round_number x100 " << (f0 - f1) << " Bits.at x1000 " << (f1 - f2) << " is_set x100 " << (f2 - f3)
                  << " visible_tiles " << (f3 - f4) << " Bits.load " << (f4 - f5) << " grow2 " << (f5 - f6) << " field " << (f6 - f7)
                  << ' ' << acc + c + gr.count() << '\n';
    }
    g_f0 = cambc::fuel_remaining();
    fp("start");
    if (at_spawn || me.champion_name() == cambc::champion_type::tavi) shop();
    fp("shop");
    observe();
    fp("observe");
    if (!live()) return;
    // A recall that an enemy will cancel anyway: stop it and get away (or fight) instead of standing still.
    if (me.is_recalling() && live()) {
        const auto foes = cambc::vision::champion_tiles(team::enemy);
        if (!foes.is_empty())
            for (auto p : foes.positions())
                if (cheb(p, pos_) <= 5 && !cambc::map::spawn_region(team::enemy).is_set(p)) {
                    try { me.cancel_recall(); } catch (const cambc::error& e) { warn("cancel recall", e); }
                    break;
                }
    }
    if (me.is_recalling() || me.is_submitting_gems() || me.is_teleporting()) {
        g_mode = 1;
        fill_fields(30000);
        return;
    }
    // Stunned: nothing to do this turn.
    if (!me.can_move() && !me.can_attack() && !me.can_cast()) return;
    measure();
    fp("measure");
    rebuild_camp_block();
    fp("block");
    if (!live()) return;
    g_mode = 2;
    try {
        if (deny()) return;
        lock_altars(false, 0);
        if (enemies()) return;
        fp("enemies");
    } catch (const cambc::error& e) { warn("enemies", e); }
    g_mode = 3;
    if (!fuel(25000)) return;
    g_mode = 4;

    const float hp = me.health(), max_hp = me.max_health();
    if (!at_spawn) spawn_wait_ = 0;
    else if (hp < max_hp * 0.95f && spawn_wait_ < 2) { ++spawn_wait_; return; }

    // Badly hurt: leave camp range, then recall.
    g_mode = 5;
    if (!at_spawn && (hp < max_hp * 0.2f || (incoming_ > 0 && hp < incoming_ * taken_ * (exit_steps_ + 2) + 40))) {
        if (escape_camps()) return;
        if (incoming_ == 0 && recall_or_walk(4)) return;
    }

    const int now = static_cast<int>(cambc::round_number());
    if (target_ < 0 || now - chosen_at_ >= 4 || k_[target_].state == Known::respawning) {
        int wait = 0;
        fp("prechoose");
        const int t = choose(wait);
        fp("choose");
    if (!live()) return;
        if (t != target_) { stuck_g_ = t; stuck_since_ = now; best_eta_ = kFar; }
        target_ = t;
        target_wait_ = wait;
        chosen_at_ = now;
        write_claim(t);
    }
    if (plug_ >= 0) {
        float hits = 0;
        for (size_t c = 0; c < g_[plug_].kinds.size(); ++c)
            if (k_[plug_].hp[c] > 0) hits += camp_spec(g_[plug_].kinds[c]).str;
        // A boss beside the only altar guards every carried gem: raid it sooner than a boss in the way.
        const float need = plug_altar_ ? kBossTurns / 2 : kBossTurns;
        if (g_[plug_].boss && (max_hp - 100 - 0.1f * max_hp) / std::max(1.0f, hits * taken_) < need) {
            if (plug_altar_) altar_wait_until_ = now + 25;  // farm and shop meanwhile
            else if (target_ >= 0) {
                k_[target_].blocked_until = now + 60;
                target_ = -1;
            }
            plug_ = -1;
            if (!plug_altar_) return;
        }
        // (The altar guard may have just been dropped above: plug_ is -1 then.)
        const auto st = plug_ >= 0 ? k_[plug_].state : Known::unknown;
        if (plug_ < 0 || st == Known::respawning || st == Known::reward || now - plug_since_ > 40 || remaining(plug_) <= 0) plug_ = -1;
        else if (hp > max_hp * 0.4f) {
            farm(plug_);
            return;
        }
    }
    g_mode = 6;
    if (tribute()) return;
    fp("tribute");
    // The last pick runs locks to altars nobody passes (the enemy's own banks), between fights.
    const bool runner = kLockRunner && slot_ == size_ - 1 && size_ >= 3 && now < 1600
        && me.gem_count() == 0 && hp > max_hp * 0.6f;
    if (lock_altars(true, runner ? 60 : kLockDetour)) return;
    if (runner && !has_lock() && !at_spawn && incoming_ == 0 && me.gold_count() >= 10 && lock_needed()) {
        if (recall_or_walk(6)) return;
    }
    g_mode = 7;

    const int t = target_;
    if (t < 0 || target_wait_ > 15) {
        if (!at_spawn && (me.gold_count() >= 60 || hp < max_hp * 0.8f) && incoming_ == 0 && me.gem_count() == 0) {
            recall_or_walk(5);
            return;
        }
        if (t < 0) {
            fill_fields(30000);
            return;
        }
    }
    // Gold buys nothing in the field: go and spend a full purse between fights.
    g_mode = 8;
    if (!at_spawn && me.gold_count() >= 240 && me.gem_count() == 0 && incoming_ == 0 && !cant_buy_
        && me.champion_name() != cambc::champion_type::tavi
        && (k_[t].state != Known::alive || eta(t) > 6) && remaining(t) >= g_[t].hp * 0.99f) {
        if (recall_or_walk(8)) return;
    }
    // Heal first when the next fight would not finish at this health.
    g_mode = 9;
    if (!at_spawn && hp < max_hp * 0.5f && incoming_ == 0 && k_[t].state != Known::reward
        && estimate_damage(t) > hp - 100) {
        if (recall_or_walk(5)) return;
    }
    // No progress towards the target for a long time: give it up for a while.
    g_mode = 10;
    {
        const int e = eta(t);
        if (e < best_eta_ || e <= 3 || attacked_) stuck_since_ = now;
        best_eta_ = e;
        if (now - stuck_since_ > 20) {
            k_[t].blocked_until = now + 40;
            target_ = -1;
            return;
        }
    }
    if (kStats && now % 100 == 0)
        std::cout << "dbg r" << now << ' ' << cambc::to_string(me.champion_name()) << " pos " << pos_.x << ',' << pos_.y
                  << " t " << t << " eta " << eta(t) << " st " << int(k_[t].state) << " plug " << plug_
                  << " ctr " << g_[t].center.x << ',' << g_[t].center.y << " hp " << hp << '\n';
    fp("prefarm");
    if (!live()) return;
    farm(t);
    fp("farm");
    fill_fields(40000);
}
