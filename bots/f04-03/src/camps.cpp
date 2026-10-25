// Camp farming: which group to clear next, with how many champions, and how to fight it.
//
// A group is worth gems + w·gold (w falls over the game), shared by the champions it occupies, per
// champion-turn spent on it: travel, respawn wait, clear time, trips home when one health bar is not
// enough, the capture and the walk to an altar. Clears are costed by a kill-by-kill simulation: camps die in
// the order that removes the most strength per hit (harmless clusters last); while we hit one camp, every
// fighter that reaches that spot hits the nearest champion. A champion only fights down to the health it
// needs to walk out of reach (the hurt rule in Bot::turn), so a trip's useful health is what lies above that
// reserve; a group whose fighters burn through the reserve in a few turns is not farmable by that team at
// all. Groups one champion can clear are farmed alone; the rest are raided by the smallest team whose
// simulation succeeds, which gathers just outside the fighters' reach and engages only once the champions
// actually present can carry the clear (rotation: a camp hits the nearest champion, so the hurt one steps
// out and the next takes over). Groups near the enemy spawn are discounted, less so for a raid.
#include <cambc.h>
#include "bot.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <utility>

const Bits& walls_local();
extern int g_mode;

namespace {
constexpr float kCaptureTurns = 6.0f;  // five capture phases and the step onto the region
constexpr float kTripTurns = 8.0f;     // recall and the heal on spawn, before walking back
constexpr int kMaxTrips = 3;           // trips home a clear may need
constexpr int kRallyRounds = 30;       // longest wait for raiders outside the fighters' reach
constexpr float kGatherTurns = 8.0f;   // expected wait for raiders not yet on their way
constexpr float kShareTurns = 15.0f;   // a solo clear longer than this is worth sharing with a claimer
constexpr bool kPairs = false;         // slots 0-1 and 2-3 farm together (melee tank and ranged)

// Discount for groups the enemy reaches sooner than we do (rel = our walk − theirs).
float territory(int rel, int n) {
    float f = rel <= -3 ? 1.0f : rel <= 3 ? 0.9f : rel <= 10 ? 0.7f : 0.5f;
    if (n >= 2) f = std::sqrt(f);
    return f;
}
}  // namespace

void Bot::build_threats() {
    threat_.assign(g_.size(), {});
    exit_.assign(g_.size(), 1);
    for (size_t i = 0; i < g_.size(); ++i) {
        const auto& g = g_[i];
        const size_t n = g.tiles.size();
        auto& t = threat_[i];
        t.assign(n * n, 0);
        int ex = 1;
        for (size_t m = 0; m < n; ++m) {
            const auto s = camp_spec(g.kinds[m]);
            if (s.str <= 0) continue;
            ex = std::max(ex, s.range + 1 - std::min(my_range_, s.range));
            for (size_t c = 0; c < n; ++c)
                if (m == c || cheb(g.tiles[m], g.tiles[c]) + my_range_ <= s.range) t[m * n + c] = 1;
        }
        exit_[i] = ex;
    }
    cost_.assign(g_.size(), ClearCost{});
}

float Bot::remaining(int group) const {
    float t = 0;
    for (float hp : k_[group].hp) t += hp;
    return t;
}

// Kill order and the strength reaching us during each kill; cached while the group's health is unchanged.
const Bot::ClearCost& Bot::clear_cost(int group) {
    auto& cc = cost_[group];
    const float left = remaining(group);
    if (cc.hp_sum == left) return cc;
    cc.hp_sum = left;
    cc.steps.clear();
    cc.peak = 0;
    const auto& g = g_[group];
    const auto& k = k_[group];
    const size_t n = std::min<size_t>(g.tiles.size(), 64);
    std::array<std::pair<float, std::uint8_t>, 64> order;
    std::array<std::uint8_t, 64> alive{};
    size_t m = 0;
    for (size_t c = 0; c < n; ++c) {
        if (k.hp[c] <= 0) continue;
        alive[c] = 1;
        order[m++] = {camp_spec(g.kinds[c]).str / std::max(1.0f, k.hp[c]), static_cast<std::uint8_t>(c)};
    }
    std::sort(order.begin(), order.begin() + m, [](auto a, auto b) { return a.first > b.first; });
    const auto& t = threat_[group];
    const size_t tn = g.tiles.size();
    for (size_t j = 0; j < m; ++j) {
        const size_t c = order[j].second;
        float rate = 0;
        for (size_t o = 0; o < n; ++o)
            if (alive[o] && t[o * tn + c]) rate += camp_spec(g.kinds[o]).str;
        cc.steps.push_back({k.hp[c], rate});
        cc.peak = std::max(cc.peak, rate);
        alive[c] = 0;
    }
    return cc;
}

// n champions of our strength clear the group, starting with pool_now health between them (full health
// after each trip home). Each fights down to the reserve that walks it out of reach.
Bot::Sim Bot::simulate(int group, int n, float pool_now) {
    Sim s;
    const auto& cc = clear_cost(group);
    auto& me = cambc::self();
    const float full_hp = me.max_health();
    const float dps = dps_ * static_cast<float>(n);
    const float exit = static_cast<float>(exit_[group]);
    float used = 0, total = 0;
    bool first = true;
    int trips = 0;
    for (const auto& st : cc.steps) {
        float left = st.hp;
        if (st.rate <= 0) {
            s.turns += left / dps;
            continue;
        }
        const float hit = st.rate * taken_;
        const float reserve = std::max(0.2f * full_hp, hit * (exit + 2) + 40);
        const float per_trip = n * (full_hp - reserve);
        if (per_trip < 3 * hit) return s;  // a trip buys less than three turns of fighting
        total += left / dps * hit;
        for (int guard = 0; left > 0 && guard < 16; ++guard) {
            const float cap = first ? pool_now - n * reserve : per_trip;
            const float avail = cap - used;
            if (avail < hit) {
                if (++trips > kMaxTrips) return s;
                first = false;
                used = 0;
                continue;
            }
            const float need = left / dps;
            if (need * hit <= avail) {
                used += need * hit;
                s.turns += need;
                if (first) s.first += need * hit;
                left = 0;
            } else {
                const float tt = avail / hit;
                s.turns += tt;
                left -= tt * dps;
                used += avail;
                if (first) s.first += avail;
            }
        }
        if (left > 0) return s;
    }
    s.ok = true;
    s.trips = static_cast<float>(trips);
    s.each = total / n;
    return s;
}

// Damage each champion of the planned team takes over the clear.
float Bot::estimate_damage(int group) {
    const int n = group == target_ && raid_n_ > 1 ? raid_n_ : 1;
    auto& me = cambc::self();
    const Sim s = simulate(group, n, me.max_health() * n);
    if (!s.ok) return me.max_health() * 2;
    return s.each;
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
    int x0 = map_w(), y0 = map_h(), x1 = -1, y1 = -1, hostile = 0, living = 0;
    for (size_t c = 0; c < n; ++c) {
        if (k.hp[c] <= 0) continue;
        ++living;
        if (camp_spec(g.kinds[c]).str > 0) ++hostile;
        x0 = std::min(x0, g.tiles[c].x); y0 = std::min(y0, g.tiles[c].y);
        x1 = std::max(x1, g.tiles[c].x); y1 = std::max(y1, g.tiles[c].y);
    }
    auto plain_goal = [&]() -> const cambc::bitboard& {
        auto live_b = empty_board();
        for (size_t c = 0; c < n; ++c)
            if (k.hp[c] > 0) live_b.set(g.tiles[c]);
        if (live_b.is_empty()) live_b = gtiles_[group];
        goal_ = grow(live_b, r) & ~walk_walls();
        return goal_;
    };
    const int bw = x1 - x0 + 2 * r + 1, bh = y1 - y0 + 2 * r + 1;
    const long work = static_cast<long>(living) * (2 * r + 1) * (2 * r + 1) + static_cast<long>(hostile) * 121 + 2L * bw * bh;
    if (living == 0 || plain || hostile == 0 || work > 2500 || !fuel(60000 + 40 * static_cast<std::uint64_t>(work)))
        return plain_goal();
    const int bx = x0 - r, by = y0 - r;
    // reach: within r of a living camp; danger: strength of the fighters reaching the tile.
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
        return plain_goal();
    }
    goal_.import_linear_buffer(std::span<const std::uint8_t>{goal_buf_});
    return goal_;
}

// Tiles one or two steps outside the reach of the group's living fighters: where raiders gather.
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

// Far from the group: one step down its cached walking field instead of a fresh route search (a host
// BFS each turn is most of our fuel). Not while a dash is ready (the routed walk uses it), and not for a
// while after the static step was blocked.
bool Bot::walk_static(int group, int far) {
    auto& me = cambc::self();
    const int now = static_cast<int>(cambc::round_number());
    if (far <= 6 || now < static_off_until_ || moved_ || move_failed_ || !live() || !me.can_move()) return false;
    const auto& f = gfield_[group];
    if (f.empty()) return false;
    if (dasher_ && !cast_ && me.can_cast())
        for (const auto& ab : me.abilities())
            if (ab.is_dash() && ab.is_ready()) return false;
    const vec2 pos = me.position();
    vec2 best = pos;
    int best_d = f.at(pos);
    if (best_d >= kFar) return false;
    for (auto d : kDirs) {
        const vec2 t{pos.x + d.x, pos.y + d.y};
        const int fd = f.at(t);
        if (fd >= best_d || !free_tile(t) || camp_block_.is_set(t)) continue;
        best_d = fd;
        best = t;
    }
    if (best == pos) { static_off_until_ = now + 12; return false; }
    moved_ = true;
    bool ok = false;
    try { ok = me.move_absolute(best); } catch (const cambc::error& e) { warn("move", e); }
    invalidate();
    if (!ok) { move_failed_ = true; static_off_until_ = now + 12; }
    return true;
}

bool Bot::group_awake(int group) const { return !(awake_ & gtiles_[group]).is_empty(); }

// Champions (us included) close enough to join the group's fight within a few turns; pool gets their health.
int Bot::raiders_near(int group, float* pool) const {
    int n = 1;
    const int r = reach_[group] + g_[group].range + 4;
    auto& me = cambc::self();
    if (pool) *pool = me.health();
    if (!pool) {
        for (auto a : view().allies)
            if (cheb(a, g_[group].center) <= r) ++n;
        return n;
    }
    const int self_h = static_cast<int>(static_cast<cambc::handle_champion_t>(me));
    for (const auto& a : cambc::champions(team::ally)) {
        if (static_cast<int>(static_cast<cambc::handle_champion_t>(a)) == self_h) continue;
        try {
            if (!a.is_alive()) continue;
            const vec2 p = a.position();
            if (!inside(p) || cheb(p, g_[group].center) > r) continue;
            ++n;
            *pool += a.health();
        } catch (const cambc::error&) {}
    }
    return n;
}

int Bot::buddy() const {
    if (!kPairs || slot_ < 0 || slot_ > 3) return -1;
    const int b = slot_ ^ 1;
    return b < size_ ? b : -1;
}

bool Bot::buddy_at(vec2& where) const {
    const int b = buddy();
    if (b < 0) return false;
    const auto& v = view();
    for (size_t a = 0; a < v.allies.size(); ++a)
        if (slot_of(v.ally_types[a], size_) == b) { where = v.allies[a]; return true; }
    return false;
}

bool Bot::hit_group(int group, bool force) {
    auto& me = cambc::self();
    if ((!force && !me.can_attack()) || !live()) return false;
    const vec2 pos = me.position();
    const int range = me.attack_range();
    const float str = me.strength();
    const bool splash = me.champion_name() == cambc::champion_type::varg;
    const auto& v = view();
    const auto& g = g_[group];
    const auto& k = k_[group];
    bool fighter_in_range = false;
    for (size_t c = 0; c < g.tiles.size(); ++c) {
        const vec2 p = g.tiles[c];
        if (cheb(p, pos) <= range && v.lvis.at(p) && v.lcamps.at(p) && camp_spec(g.kinds[c]).str > 0) fighter_in_range = true;
    }
    float best_score = -1e9f;
    vec2 best{};
    bool found = false;
    for (size_t c = 0; c < g.tiles.size(); ++c) {
        const vec2 p = g.tiles[c];
        if (cheb(p, pos) > range || !v.lvis.at(p) || !v.lcamps.at(p)) continue;
        const float hp = std::max(1.0f, k.hp[c]);
        const auto s = camp_spec(g.kinds[c]);
        // Most strength removed per hit first; a camp that reaches us outranks one that does not;
        // clusters wait until no fighter is in range (they never hit back).
        float score = s.str / hp * 1e4f - hp * 1e-3f;
        if (s.str > 0 && cheb(p, pos) <= s.range) score += 50;
        if (hp <= str && (s.str > 0 || !fighter_in_range)) score += 1e6f;  // a kill this hit
        if (splash)
            for (size_t o = 0; o < g.tiles.size(); ++o)
                if (o != c && k.hp[o] > 0 && cheb(g.tiles[o], p) <= 1 && v.lcamps.at(g.tiles[o]))
                    score += std::min(str, k.hp[o]) * (camp_spec(g.kinds[o]).str > 0 ? 0.5f : 0.1f);
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
// Best plan for one group: champion-turn rate, team size and whether the team gathers first.
struct Plan { float score = 0; int n = 1; bool rally = false; };
}  // namespace

int Bot::choose(int& wait_out) {
    auto& me = cambc::self();
    const int now = static_cast<int>(cambc::round_number());
    const float hp = me.health(), max_hp = me.max_health();
    const auto s = read_sighting();
    const float gold_w = (me.champion_name() == cambc::champion_type::vesper_vale ? 0.5f : 0.35f)
        * std::max(0.4f, 1.0f - now / 1500.0f);
    const int alive = 1 + static_cast<int>(view().allies.size());
    int best = -1;
    Plan best_plan;
    wait_out = 0;
    std::array<std::uint8_t, 5> claims{};
    cambc::shared_memory.read(0, std::span<std::uint8_t>{claims.data(), claims.size()});
    // A pair: the even slot leads and plans for both, the odd one follows the leader's claim.
    const int bud = buddy();
    vec2 bud_pos{};
    const bool paired = bud >= 0 && buddy_at(bud_pos);
    if (paired && (slot_ & 1)) {
        const int c = static_cast<int>(claims[static_cast<size_t>(bud)]) - 1;
        if (c >= 0 && c < static_cast<int>(g_.size()) && !g_[c].boss && k_[c].blocked_until <= now && eta(c) < kFar) {
            const int avail = k_[c].state == Known::respawning ? k_[c].ready : now;
            wait_out = std::max(0, avail - (now + eta(c)));
            raid_n_ = 2;
            rally_need_ = false;
            return c;
        }
    }
    const float bud_hp = paired ? 0.9f * max_hp : 0.0f;
    const int unit = paired ? 2 : 1;
    const int n_groups = static_cast<int>(g_.size());
    // The current target first: a good early best prunes most of the others cheaply.
    for (int j = -1; j < n_groups; ++j) {
        const int i = j < 0 ? target_ : j;
        if (i < 0 || (j >= 0 && i == target_)) continue;
        const auto& g = g_[i];
        const auto& k = k_[i];
        if (k.blocked_until > now) continue;
        const float value = g.gems + gold_w * g.gold;
        if (value <= 0) continue;
        const int e = eta(i);
        if (e >= kFar) continue;
        if (value * 1.7f / (e + kCaptureTurns) <= best_plan.score) continue;  // cannot beat the best
        const int avail = k.state == Known::respawning ? k.ready : now;
        const int wait = std::max(0, avail - (now + e));
        float f = 1.0f;
        if (home_slot_.size() == g_.size()) {
            if (home_slot_[i] == 255) f = 0.8f;
            else if (home_slot_[i] != slot_ && !(paired && home_slot_[i] == bud)) f = 0.6f;
        }
        if (k.state == Known::reward) f *= 1.3f;
        const bool waiting = k.state == Known::respawning && wait > 0;
        if (i == target_ && !waiting) f *= 1.25f;
        if (s.ok && s.age <= 24 && cheb(s.where, g.center) <= 7) f *= 0.5f;
        int claimers = 0;
        for (int o = 0; o < 5; ++o)
            if (o != slot_ && !(paired && o == bud) && claims[static_cast<size_t>(o)] == i + 1) ++claimers;
        const int rel = g.d_ally - g.d_enemy;
        const float trip = kTripTurns + std::min(60, g.d_ally) + exit_[i];
        // Gems still have to reach an altar: about one walk there per 45 carried.
        const float bank = g.gems / 45.0f * std::min(bank_d_[static_cast<size_t>(i)], 6 + home_altar_);
        const float base_t = e + wait + kCaptureTurns;
        Plan plan;
        if (k.state == Known::reward) {
            // The claimers nearest the region capture (raiders split a boss's reward, as many as its
            // region holds; one is enough elsewhere, two for a pair); the others move on.
            const int room = g.boss ? static_cast<int>(g.tiles.size()) : unit;
            int nearer = 0;
            if (claimers > 0) {
                const int mine = cheb(pos_, g.center);
                const auto& v = view();
                for (size_t a = 0; a < v.allies.size(); ++a) {
                    const int o = slot_of(v.ally_types[a], size_);
                    if (o < 0 || o > 4 || (paired && o == bud) || claims[static_cast<size_t>(o)] != i + 1) continue;
                    const int theirs = cheb(v.allies[a], g.center);
                    if (theirs < mine || (theirs == mine && o < slot_)) ++nearer;
                }
            }
            if (nearer >= room) f *= 0.1f;
            else plan.n = std::min(claimers + unit, room);
            plan.score = value / plan.n * f * territory(rel, plan.n) / (e + kCaptureTurns + bank / plan.n);
        } else {
            if (value * f / (base_t + bank) <= best_plan.score) continue;
            // Alone (or as a pair), from the health we have now (a trip home first when it is not enough).
            if (!g.boss) {
                const Sim one = simulate(i, unit, hp + bud_hp);
                if (one.ok) {
                    const float tf = territory(rel, unit);
                    float sc = value * f * tf / (base_t + bank + one.turns + one.trips * trip);
                    if (claimers > 0) {
                        // Someone is on it: share a long clear, leave a short one to them.
                        if (one.turns > kShareTurns || one.trips > 0) {
                            const int n = claimers + unit;
                            const Sim many = simulate(i, n, hp + bud_hp + claimers * 0.9f * max_hp);
                            if (many.ok) {
                                const float s2 = value / (n - unit + 1) * f * territory(rel, n)
                                    / (base_t + bank / n + many.turns + many.trips * trip);
                                if (s2 > plan.score) { plan.score = s2; plan.n = n; plan.rally = false; }
                            }
                            sc = 0;
                        } else {
                            for (int c = 0; c < claimers; ++c) sc *= 0.3f;
                        }
                    }
                    if (sc > plan.score) { plan.score = sc; plan.n = unit; plan.rally = false; }
                }
            }
            // Raid: the team gathers outside the fighters' reach, then engages together.
            if (alive >= 2 && (g.boss || plan.score <= 0 || plan.n == 1)) {
                for (int n = std::max(unit + 1, claimers + 1); n <= alive; ++n) {
                    const Sim sim = simulate(i, n, hp + (n - 1) * 0.9f * max_hp);
                    if (!sim.ok || sim.trips > 2) continue;
                    const float gather = n > claimers + unit ? kGatherTurns : 0.0f;
                    // Join a raid that is forming rather than start a second one.
                    const float join = waiting ? 1.0f : 1.0f + 0.5f * claimers;
                    const float sc = value / (n - unit + 1) * f * join * territory(rel, n)
                        / (base_t + bank / n + gather + sim.turns + sim.trips * trip);
                    if (sc > plan.score) { plan.score = sc; plan.n = n; plan.rally = true; }
                    break;  // the smallest team that can do it
                }
            }
        }
        if (plan.score > best_plan.score) { best_plan = plan; best = i; wait_out = wait; }
    }
    raid_n_ = best_plan.n;
    rally_need_ = best_plan.rally;
    return best;
}

void Bot::farm(int group) {
    const auto& k = k_[group];
    auto& me = cambc::self();
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
    const bool awake = group_awake(group);
    const int planned = group == target_ ? raid_n_ : 1;
    // Before waking a group: is the team that is actually here enough to carry the clear?
    if (!awake && k.state == Known::alive && e <= g_[group].range + reach_[group] + 4) {
        float pool = 0;
        const int near = raiders_near(group, &pool);
        const int n = std::max(1, std::min(near, std::max(planned, 1)));
        if (n == 1) pool = me.health();
        const Sim here = simulate(group, n, pool);
        const bool go_now = here.ok && (here.trips == 0 || near >= planned);
        if (!go_now && planned >= 2 && group == target_) {
            // Wait just outside the fighters' reach for the rest of the team.
            if (rally_g_ != group) { rally_g_ = group; rally_since_ = -1; }
            const auto& rally = rally_goal(group);
            if (rally.is_set(pos_) && rally_since_ < 0) rally_since_ = now;
            if (!rally.is_empty() && (rally_since_ < 0 || now - rally_since_ < kRallyRounds)) {
                g_mode = 16;
                if (!rally.is_set(pos_) && !walk_static(group, e - g_[group].range - 2)) go(rally, e > 3);
                return;
            }
            if (!here.ok || here.trips > 1) {
                // Nobody came: try something else for a while.
                k_[group].blocked_until = now + 80;
                target_ = -1;
                return;
            }
        } else if (!here.ok) {
            // Too strong for us even with trips home: leave it for a while.
            k_[group].blocked_until = now + 60;
            if (group == target_) target_ = -1;
            if (group == plug_) plug_ = -1;
            return;
        } else if (near == 1 && me.health() < 0.95f * me.max_health()
                   && !cambc::map::spawn_region(team::ally).is_set(pos_)) {
            // Heal first when our health now costs an extra trip.
            const Sim fresh = simulate(group, 1, me.max_health());
            if (fresh.ok && here.trips > fresh.trips && incoming_ == 0 && recall_or_walk(5)) return;
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
    int r = std::max(1, me.attack_range() - shrink_);
    const auto& goal = approach_goal(group, r);
    if (goal.is_set(pos_) && !attacked_ && r > 1) {
        // In range but nothing to hit: walls hide the camps, come closer.
        ++shrink_;
        --r;
    }
    const auto& goal2 = approach_goal(group, r);
    const bool moved_before = moved_;
    if (walk_static(group, e - r)) return;
    const bool ok = go(goal2, e > 3);
    // The safest spots cannot be reached: fall back to any spot in range for a while.
    if (!ok && !moved_before && !moved_ && !goal2.is_set(pos_) && !goal_plain_) plain_until_ = now + 12;
    if (hit_group(group)) reset_hit(group);
}
