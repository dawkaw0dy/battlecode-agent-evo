// The camp subsystem: which group each champion farms, in what order and with how many allies, and how it
// clears and captures it.
//
// Rules that shape it (GameRules.md with patch 100): a group pays only when every monster in it is dead
// and an ally then stands on one of its spawn tiles, uncontested, for five capture phases. Allies present
// split the reward, so one champion captures while the others move on (a boss's big bag is split on
// purpose: it banks in short tributes). The whole group respawns together on its longest cooldown, counted
// from the capture. Awake camps hit the nearest champion in their reach every round (wolf 3, elk and golem
// 4, monolith 5), never chase and never heal: a clear can be split over trips home, and n champions
// together take 1/n of the time and share the blows, so each takes 1/n² of the damage. Clusters (120 hp)
// never hit back.
//
// Choice: every group gets its best plan (alone, shared with the allies already on it, or a raid of n)
// costed in champion-turns: the walk, the respawn wait, the clear, the trips home to heal, the capture and
// the walk to an altar for its gems. Its rate is value (gems + weighted gold) per champion-turn. A two-step
// lookahead then adds the best follow-up group, reached from the end of each candidate when its own
// respawn timer may be over: a short detour while the home camps respawn beats waiting beside them, and a
// rich neighbourhood beats a lone camp. Groups near recent enemy sightings and our deaths are discounted.
#include <cambc.h>
#include "bot.h"
#include "fuel.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <span>
#include <utility>
#include <variant>

extern int g_mode;
void fp(const char* tag);

namespace {
constexpr float kCaptureTurns = 6.0f;   // five capture phases and the step onto the region
constexpr float kTripTurns = 8.0f;      // recall and the heal on spawn, before walking back
constexpr int kRallyRounds = 35;        // longest wait for raiders outside a strong group's reach
constexpr float kGatherTurns = 10.0f;   // expected wait for raiders not yet on their way
constexpr bool kLookahead = true;       // score a group with its best follow-up
constexpr int kFirstK = 12;             // candidates that get a follow-up ...
constexpr int kNextK = 8;               // ... chosen among this many
constexpr int kDangerRounds = 80;       // a sighting or a death fades out over this many rounds
constexpr float kDangerWeight = 0.5f;   // value divided by 1 + weight · faded danger near the group
constexpr float kDeathWeight = 2.0f;    // a death counts as this many enemies seen
constexpr bool kTraceChoose = false;

float spell_range(const cambc::ability& ab) {
    float r = 4;
    try {
        const auto& info = ab.info();
        if (info) {
            if (auto* d = std::get_if<cambc::ability_damage>(&*info)) r = d->range;
            else if (auto* c = std::get_if<cambc::ability_cc>(&*info)) r = c->range;
            else if (auto* a = std::get_if<cambc::ability_aoe_damage>(&*info)) r = a->range;
        }
    } catch (const std::exception&) {}
    return r;
}

// One group's best plan from where we stand.
struct Plan {
    float rate = 0;    // value per champion-turn
    float value = 0;   // our share of the value, with the group's factors
    float time = 0;    // champion-turns from now until it is captured and banked
    float tail = 0;    // the part after the walk and the wait: clear, trips, capture, bank
    int n = 1;         // champions on it
    int avail = 0;     // round it can be fought (respawn), now when alive
    bool rally = false;
};
}  // namespace

// ---------------------------------------------------------------- static analysis

void Bot::build_threats() {
    const size_t ng = g_.size();
    threat_.assign(ng, {});
    for (size_t i = 0; i < ng; ++i) {
        const auto& g = g_[i];
        const size_t n = g.tiles.size();
        auto& t = threat_[i];
        t.assign(n * n, 0);
        for (size_t m = 0; m < n; ++m) {
            const auto s = camp_spec(g.kinds[m]);
            if (s.str <= 0) continue;
            // m reaches every tile within our range of c: no standing spot dodges it.
            for (size_t c = 0; c < n; ++c)
                if (m == c || cheb(g.tiles[m], g.tiles[c]) + my_range_ <= s.range) t[m * n + c] = 1;
        }
    }
    cost_.assign(ng, ClearCost{});
    hop_.assign(ng, std::vector<int>(ng, -1));
}

// Walk from one group to another: exact once the target's field exists, an estimate before.
int Bot::hop(int from, int to) {
    if (from == to) return 0;
    int& h = hop_[static_cast<size_t>(from)][static_cast<size_t>(to)];
    if (h >= 0) return h;
    const auto& f = gfield_[static_cast<size_t>(to)];
    if (!f.empty() && !g_[from].tiles.empty()) {
        int d = kFar;
        for (auto t : g_[from].tiles) d = std::min(d, f.at(t));
        if (d < kFar) return h = d;
    }
    return cheb(g_[from].center, g_[to].center) * 5 / 4 + 2;
}

float Bot::remaining(int group) const {
    float t = 0;
    for (float hp : k_[group].hp) t += hp;
    return t;
}

// Damage model of a clear: camps die in the order that hurts least (strength removed per health first,
// harmless clusters last). base = Σ health · strength reaching us while that camp dies (divide by our team
// damage for the damage taken), peak = the strongest fire at any point. Cached while the health is unchanged.
const Bot::ClearCost& Bot::clear_cost(int group) {
    auto& cc = cost_[group];
    const float left = remaining(group);
    if (cc.hp_sum == left) return cc;
    cc.hp_sum = left;
    cc.base = 0;
    cc.peak = 0;
    const auto& g = g_[group];
    const auto& k = k_[group];
    const size_t n = std::min<size_t>(g.tiles.size(), 64);
    std::array<std::pair<float, std::uint8_t>, 64> order;
    std::array<std::uint8_t, 64> alive{};
    std::array<float, 64> str{};
    size_t m = 0;
    for (size_t c = 0; c < n; ++c) {
        str[c] = camp_spec(g.kinds[c]).str;
        if (k.hp[c] <= 0) continue;
        alive[c] = 1;
        order[m++] = {str[c] / std::max(1.0f, k.hp[c]), static_cast<std::uint8_t>(c)};
    }
    std::sort(order.begin(), order.begin() + m, [](auto a, auto b) { return a.first > b.first; });
    const auto& t = threat_[group];
    const size_t stride = g.tiles.size();
    for (size_t j = 0; j < m; ++j) {
        const size_t c = order[j].second;
        float fire = 0;
        for (size_t o = 0; o < n; ++o)
            if (alive[o] && t[o * stride + c]) fire += str[o];
        cc.base += k.hp[c] * fire;
        cc.peak = std::max(cc.peak, fire);
        alive[c] = 0;
    }
    return cc;
}

// Damage each of n raiders takes clearing the group (the hurt one steps out and the next nearest tanks).
float Bot::estimate_damage(int group) {
    const int n = group == target_ && raid_n_ > 1 ? raid_n_ : 1;
    return clear_cost(group).base / (dps_ * static_cast<float>(n)) * taken_ / static_cast<float>(n);
}

// ---------------------------------------------------------------- danger memory

// Enemies we (or an ally, through the shared sighting) saw, and where we died, fade out over kDangerRounds.
void Bot::note_danger() {
    const int now = static_cast<int>(cambc::round_number());
    // A minute or more since the camp code last ran with us alive and back on spawn: we died where we stood.
    if (alive_seen_ >= 0 && now - alive_seen_ >= 50 && spawn_local_.at(pos_))
        danger_.push_back({alive_pos_, alive_seen_, kDeathWeight});
    alive_seen_ = now;
    alive_pos_ = pos_;
    auto add = [&](vec2 p, int round, float w) {
        for (auto& d : danger_)
            if (cheb(d.at, p) <= 3 && round >= d.round && round - d.round < 10) {
                d.at = p;
                d.round = round;
                d.w = std::max(d.w, w);
                return;
            }
        danger_.push_back({p, round, w});
    };
    const auto s = read_sighting();
    if (s.ok && s.age <= 8) add(s.where, now - s.age, 1.0f);
    const auto& v = view();
    if (!v.foes.is_empty()) {
        const auto& theirs = cambc::map::spawn_region(team::enemy);
        int count = 0;
        vec2 first{};
        for (auto p : v.foes.positions())
            if (!theirs.is_set(p)) {
                if (count++ == 0) first = p;
            }
        if (count > 0) add(first, now, static_cast<float>(count));
    }
    std::erase_if(danger_, [&](const Danger& d) { return now - d.round > kDangerRounds; });
    if (danger_.size() > 32) danger_.erase(danger_.begin(), danger_.begin() + static_cast<long>(danger_.size() - 32));
}

float Bot::danger_near(int group) const {
    const int now = static_cast<int>(cambc::round_number());
    const int r = reach_[group] + 6;
    float sum = 0;
    for (const auto& d : danger_)
        if (cheb(d.at, g_[group].center) <= r)
            sum += d.w * std::max(0.0f, 1.0f - static_cast<float>(now - d.round) / kDangerRounds);
    return sum;
}

// ---------------------------------------------------------------- choice

int Bot::choose(int& wait_out) {
    auto& me = cambc::self();
    note_danger();
    const int now = static_cast<int>(cambc::round_number());
    const float hp = me.health(), max_hp = me.max_health();
    const float margin = 100 + 0.1f * max_hp;
    const float cap = std::max(1.0f, max_hp - margin);   // damage one trip can absorb from full health
    const float spare = std::max(0.0f, hp - margin);       // ... and from where we stand now
    const float gold_w = (me.champion_name() == cambc::champion_type::vesper_vale ? 0.5f : 0.35f)
        * std::max(0.4f, 1.0f - static_cast<float>(now) / 1500.0f);
    const auto& v = view();
    // Raids are planned with the allies in sight (and the ones already on a group): counting allies across
    // the map had champions converge from everywhere (rounds to win vs the starter 1210 against 1116).
    const int alive = 1 + static_cast<int>(v.allies.size());
    std::array<std::uint8_t, 5> claims{};
    cambc::shared_memory.read(0, std::span<std::uint8_t>{claims.data(), claims.size()});
    // Clear turns for n champions of our strength, and the trips home each needs on top.
    auto trips_for = [&](const ClearCost& cc, int n, float& turns) {
        const float nf = static_cast<float>(n);
        turns = cc.hp_sum / (dps_ * nf);
        const float each = cc.base / (dps_ * nf) * taken_ / nf;
        return each <= spare ? 0.0f : (each - spare) / cap;
    };
    const int ng = static_cast<int>(g_.size());
    std::vector<Plan> plans(static_cast<size_t>(ng));
    int best = -1;
    float best_rate = 0;
    for (int i = 0; i < ng; ++i) {
        const auto& g = g_[i];
        const auto& k = k_[i];
        if (k.blocked_until > now) {
            if (kTraceChoose && now % 50 < 4 && (g.gems >= 40 || g.boss))
                std::cout << "plan r" << now << ' ' << cambc::to_string(me.champion_name()) << " g" << i << " blocked\n";
            continue;
        }
        const float value = static_cast<float>(g.gems) + gold_w * static_cast<float>(g.gold);
        if (value <= 0) continue;
        const int e = eta(i);
        if (e >= kFar) continue;
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
        f /= 1.0f + kDangerWeight * danger_near(i);
        int claimers = 0;
        for (int o = 0; o < 5; ++o)
            if (o != slot_ && claims[static_cast<size_t>(o)] == i + 1) ++claimers;
        const float trip = kTripTurns + static_cast<float>(std::min(60, g.d_ally));
        // Gems still have to reach an altar: about one walk there per 45 carried.
        const float bank = static_cast<float>(g.gems) / 45.0f
            * static_cast<float>(std::min(bank_d_[static_cast<size_t>(i)], 6 + home_altar_));
        const float lead = static_cast<float>(e + wait);
        Plan p;
        p.avail = avail;
        auto consider = [&](float share, float tail, int n, bool rally) {
            const float time = lead + tail;
            const float rate = share / std::max(1.0f, time);
            if (rate > p.rate) { p.rate = rate; p.value = share; p.time = time; p.tail = tail; p.n = n; p.rally = rally; }
        };
        if (k.state == Known::reward) {
            // The claimers nearest the region capture (raiders split a boss's reward, as many as its region
            // holds; one is enough elsewhere); the others move on.
            const int room = g.boss ? static_cast<int>(g.tiles.size()) : 1;
            int nearer = 0;
            if (claimers > 0) {
                const int mine = cheb(pos_, g.center);
                for (size_t a = 0; a < v.allies.size(); ++a) {
                    const int o = slot_of(v.ally_types[a], size_);
                    if (o < 0 || o > 4 || claims[static_cast<size_t>(o)] != i + 1) continue;
                    const int theirs = cheb(v.allies[a], g.center);
                    if (theirs < mine || (theirs == mine && o < slot_)) ++nearer;
                }
            }
            if (nearer >= room) f *= 0.1f;
            const int n = nearer >= room ? 1 : std::min(claimers + 1, room);
            consider(value / static_cast<float>(n) * f, kCaptureTurns + bank / static_cast<float>(n), n, false);
        } else {
            const auto& cc = clear_cost(i);
            const float peak = cc.peak * taken_;
            // n champions of our strength (allies already on it need no gathering) clear in 1/n of the time
            // and share the blows, 1/n² of the damage each; one of them captures (all of them at a boss,
            // whose bag banks faster split). A group one champion cannot hold until help arrives is raided:
            // the raiders gather outside its reach and engage together.
            const int top = std::max(claimers + 1, alive);
            float solo_turns = 0;
            trips_for(cc, claimers > 0 ? claimers : 1, solo_turns);  // the claimers' own clear time
            for (int n = 1; n <= top; ++n) {
                const float nf = static_cast<float>(n);
                if (n == 1 && (g.boss || cap < 5 * peak)) continue;
                if (n > 1 && n <= claimers) continue;
                if (cap * nf < 3 * peak) continue;
                float turns = 0;
                const float trips = trips_for(cc, n, turns);
                if (trips > (n == 1 ? 4.0f : 3.0f)) continue;
                // Joining a short clear that will be over before we arrive gains nothing.
                if (claimers > 0 && n == claimers + 1 && solo_turns < static_cast<float>(e)) continue;
                const float gather = n > claimers + 1 ? kGatherTurns : 0.0f;
                const bool rally = n >= 2 && (g.boss || peak * kGatherTurns > spare);
                const float join = rally && !waiting ? 1.0f + 0.5f * static_cast<float>(claimers) : 1.0f;
                const float capture = g.boss ? kCaptureTurns : kCaptureTurns / nf;
                float share = value / nf * f * join;
                if (n <= claimers)
                    for (int c = 0; c < claimers; ++c) share *= 0.3f;  // racing its claimers alone: they get there first
                consider(share, capture + bank / nf + (n <= claimers ? 0.0f : gather) + turns + trips * trip, n, rally && n > 1);
            }
        }
        plans[static_cast<size_t>(i)] = p;
        if (kTraceChoose && now % 50 < 4 && (g.gems >= 40 || g.boss))
            std::cout << "plan r" << now << ' ' << cambc::to_string(me.champion_name()) << " g" << i << " st "
                      << static_cast<int>(k.state) << " gems " << g.gems << " e " << e << " wait " << wait << " f " << f
                      << " danger " << danger_near(i) << " claim " << claimers << " alive " << alive << " rate " << p.rate
                      << " n " << p.n << " peak " << clear_cost(i).peak << " cap " << cap << " dps " << dps_ << '\n';
        if (p.rate > best_rate) { best_rate = p.rate; best = i; }
    }
    // Two-step lookahead: the best candidates, each followed by the best group reachable from it.
    if (kLookahead && best >= 0 && fuel(30000) && live()) {
        std::vector<int> first, next;
        for (int i = 0; i < ng; ++i)
            if (plans[static_cast<size_t>(i)].rate > 0) first.push_back(i);
        next = first;
        auto by_rate = [&](int a, int b) { return plans[static_cast<size_t>(a)].rate > plans[static_cast<size_t>(b)].rate; };
        // Follow-ups ranked by their rate once we stand beside them.
        auto by_tail = [&](int a, int b) {
            const auto& pa = plans[static_cast<size_t>(a)];
            const auto& pb = plans[static_cast<size_t>(b)];
            return pa.value / (pa.tail + 5.0f) > pb.value / (pb.tail + 5.0f);
        };
        if (static_cast<int>(first.size()) > kFirstK) {
            std::partial_sort(first.begin(), first.begin() + kFirstK, first.end(), by_rate);
            first.resize(kFirstK);
        }
        if (static_cast<int>(next.size()) > kNextK) {
            std::partial_sort(next.begin(), next.begin() + kNextK, next.end(), by_tail);
            next.resize(kNextK);
        }
        float best_pair = 0;
        int best_first = best;
        for (int a : first) {
            const auto& pa = plans[static_cast<size_t>(a)];
            const float done = static_cast<float>(now) + pa.time;  // round the first group is ours
            float pair = pa.rate;                                  // nothing worth doing after it
            for (int b : next) {
                const auto& pb = plans[static_cast<size_t>(b)];
                float walk, avail;
                if (b == a) {
                    // Stay for its respawn (the timer starts at the capture).
                    walk = 0;
                    avail = done + static_cast<float>(g_[a].respawn);
                } else {
                    walk = static_cast<float>(hop(a, b));
                    avail = static_cast<float>(pb.avail);
                }
                const float arrive = done + walk;
                const float t2 = walk + std::max(0.0f, avail - arrive) + pb.tail;
                pair = std::max(pair, (pa.value + pb.value) / std::max(1.0f, pa.time + t2));
            }
            if (pair > best_pair) { best_pair = pair; best_first = a; }
        }
        best = best_first;
    }
    wait_out = 0;
    raid_n_ = 1;
    rally_need_ = false;
    if (best >= 0) {
        const auto& p = plans[static_cast<size_t>(best)];
        wait_out = std::max(0, p.avail - (now + eta(best)));
        raid_n_ = p.n;
        rally_need_ = p.rally;
    }
    if (kTraceChoose && best >= 0 && best != target_ && now < 700)
        std::cout << "choose r" << now << ' ' << cambc::to_string(me.champion_name()) << " g" << best << " st "
                  << static_cast<int>(k_[best].state) << " gems " << g_[best].gems << " gold " << g_[best].gold
                  << " eta " << eta(best) << " wait " << wait_out << " n " << raid_n_ << " rally " << rally_need_
                  << " rate " << plans[static_cast<size_t>(best)].rate << '\n';
    return best;
}

// ---------------------------------------------------------------- positioning

// Tiles within r of a living camp of the group, keeping the ones the fewest fighters reach (we hit from
// the side away from the others). Cached while the living set stays the same.
const cambc::bitboard& Bot::approach_goal(int group, int r) {
    const auto& g = g_[group];
    const auto& k = k_[group];
    const size_t n = g.tiles.size();
    int sig = 0;
    for (size_t c = 0; c < n; ++c)
        if (k.hp[c] > 0) sig = sig * 31 + static_cast<int>(c) + 1;
    const int now = static_cast<int>(cambc::round_number());
    const bool plain = plain_until_ > now;
    if (goal_group_ == group && goal_r_ == r && goal_sig_ == sig && goal_plain_ == plain) return goal_;
    goal_group_ = group;
    goal_r_ = r;
    goal_sig_ = sig;
    goal_plain_ = plain;
    auto every_tile = [&]() -> const cambc::bitboard& {
        auto live_b = empty_board();
        for (size_t c = 0; c < n; ++c)
            if (k.hp[c] > 0) live_b.set(g.tiles[c]);
        if (live_b.is_empty()) live_b = gtiles_[group];
        goal_ = grow(live_b, r) & ~walk_walls();
        return goal_;
    };
    // Box around the living camps.
    int x0 = map_w(), y0 = map_h(), x1 = -1, y1 = -1, hostile = 0, living = 0;
    for (size_t c = 0; c < n; ++c) {
        if (k.hp[c] <= 0) continue;
        ++living;
        if (camp_spec(g.kinds[c]).str > 0) ++hostile;
        x0 = std::min(x0, g.tiles[c].x);
        y0 = std::min(y0, g.tiles[c].y);
        x1 = std::max(x1, g.tiles[c].x);
        y1 = std::max(y1, g.tiles[c].y);
    }
    if (living == 0 || plain || hostile == 0) return every_tile();
    const int bx = x0 - r, by = y0 - r, bw = x1 - x0 + 2 * r + 1, bh = y1 - y0 + 2 * r + 1;
    const long work = static_cast<long>(living) * (2 * r + 1) * (2 * r + 1) + static_cast<long>(hostile) * 121 + 2L * bw * bh;
    if (work > 2500 || !fuel(60000 + 40 * static_cast<std::uint64_t>(work))) return every_tile();
    // in_range[i]: within r of a living camp; fire[i]: strength of the fighters reaching the tile.
    std::vector<std::uint8_t> in_range(static_cast<size_t>(bw * bh), 0);
    std::vector<std::int16_t> fire(static_cast<size_t>(bw * bh), 0);
    for (size_t c = 0; c < n; ++c) {
        if (k.hp[c] <= 0) continue;
        const vec2 p = g.tiles[c];
        for (int y = p.y - r; y <= p.y + r; ++y) {
            std::uint8_t* row = in_range.data() + (y - by) * bw + (p.x - r - bx);
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
            std::int16_t* row = fire.data() + (y - by) * bw;
            for (int x = xa; x <= xb; ++x) row[x - bx] = static_cast<std::int16_t>(row[x - bx] + add);
        }
    }
    for (size_t c = 0; c < n; ++c)
        if (k.hp[c] > 0) in_range[static_cast<size_t>((g.tiles[c].y - by) * bw + g.tiles[c].x - bx)] = 0;
    int least = 1 << 30;
    for (int y = 0; y < bh; ++y)
        for (int x = 0; x < bw; ++x) {
            const size_t i = static_cast<size_t>(y * bw + x);
            if (!in_range[i]) continue;
            const vec2 p{bx + x, by + y};
            if (!inside(p) || walls_local().at(p)) { in_range[i] = 0; continue; }
            least = std::min<int>(least, fire[i]);
        }
    goal_buf_.assign(cambc::bitboard::linear_buffer_size(), 0);
    bool any = false;
    for (int y = 0; y < bh; ++y)
        for (int x = 0; x < bw; ++x) {
            const size_t i = static_cast<size_t>(y * bw + x);
            if (!in_range[i] || fire[i] > least) continue;
            const int t = tidx(vec2{bx + x, by + y});
            goal_buf_[static_cast<size_t>(t >> 3)] |= static_cast<std::uint8_t>(1u << (t & 7));
            any = true;
        }
    if (!any) {
        goal_plain_ = true;
        return every_tile();
    }
    goal_.import_linear_buffer(std::span<const std::uint8_t>{goal_buf_});
    return goal_;
}

// Tiles one or two steps outside the reach of the group's living fighters: where raiders gather.
// Built on a local box (whole-board dilations by a boss's reach cost tens of thousands of fuel).
const cambc::bitboard& Bot::rally_goal(int group) {
    const int now = static_cast<int>(cambc::round_number());
    if (rally_goal_g_ == group && rally_goal_round_ + 10 > now) return rally_goal_;
    rally_goal_g_ = group;
    rally_goal_round_ = now;
    const auto& g = g_[group];
    const auto& k = k_[group];
    int x0 = map_w(), y0 = map_h(), x1 = -1, y1 = -1;
    for (size_t c = 0; c < g.tiles.size(); ++c) {
        if (k.hp[c] <= 0 || camp_spec(g.kinds[c]).str <= 0) continue;
        x0 = std::min(x0, g.tiles[c].x);
        y0 = std::min(y0, g.tiles[c].y);
        x1 = std::max(x1, g.tiles[c].x);
        y1 = std::max(y1, g.tiles[c].y);
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
                const std::uint8_t val = (dx > dy ? dx : dy) <= s.range ? 2 : 1;
                if (row[x - bx] < val) row[x - bx] = val;
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

// ---------------------------------------------------------------- fighting camps

bool Bot::hit_group(int group) {
    auto& me = cambc::self();
    if (!me.can_attack() || !live()) return false;
    const vec2 pos = me.position();
    const int range = me.attack_range();
    const float str = me.strength();
    const auto& v = view();
    float best_score = -1e9f;
    vec2 best{};
    bool found = false;
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

// A second hit this turn from an attack reset (only right after our own hit: error 26 otherwise).
void Bot::reset_hit(int group) {
    auto& me = cambc::self();
    if (!attacked_ || cast_ || !fuel(8000) || !live() || me.can_attack() || !me.can_cast()) return;
    for (const auto& ab : me.abilities()) {
        if (!ab.is_attack_reset() || !ab.is_ready()) continue;
        cast_ = true;
        try { ab.use(); } catch (const cambc::error& e) { warn("reset", e); return; }
        if (me.can_attack()) hit_group(group);
        return;
    }
}

// Damage spells on camps; shield while under fire. Cast before walking (some spells exclude it).
void Bot::spells_on_group(int group) {
    auto& me = cambc::self();
    if (cast_ || moved_ || !live() || !me.can_cast()) return;
    const auto& v = view();
    for (const auto& ab : me.abilities()) {
        if (!ab.is_ready()) continue;
        if (ab.is_shield()) {
            if (incoming_ > 0 && me.health() < me.max_health() * 0.9f) {
                cast_ = true;
                try { ab.use(); } catch (const cambc::error& e) { warn("shield", e); }
                return;
            }
            continue;
        }
        if (!(ab.is_damage() || ab.is_cc() || ab.is_aoe_damage())) continue;
        if (!ab.can_target_camps() && !ab.can_target_ground()) continue;
        const int r = static_cast<int>(spell_range(ab));
        vec2 best{};
        float best_score = -1;
        for (size_t c = 0; c < g_[group].tiles.size(); ++c) {
            const vec2 p = g_[group].tiles[c];
            if (cheb(p, me.position()) > r || !v.lvis.at(p) || !v.lcamps.at(p)) continue;
            const float hp = k_[group].hp[c];
            if (hp <= 0) continue;
            float score = std::min(hp, 400.0f) + camp_spec(g_[group].kinds[c]).str;
            if (ab.is_aoe_damage())
                for (const auto& o : g_[group].tiles)
                    if (o != p && cheb(o, p) <= 1 && v.lcamps.at(o)) score += 300;
            if (score > best_score) { best_score = score; best = p; }
        }
        if (best_score < 0) continue;
        try {
            cast_ = true;
            if (ab.is_aoe_damage() || !ab.can_target_camps()) ab.use(best);
            else ab.use(cambc::vision::camp_at(best));
            invalidate();
        } catch (const cambc::error& e) { warn("spell", e); }
        return;
    }
}

void Bot::farm(int group) {
    const auto& k = k_[group];
    const int now = static_cast<int>(cambc::round_number());
    const int e = eta(group);
    alive_seen_ = now;
    alive_pos_ = pos_;
    if (capture_g_ != group) capture_g_ = -1;

    g_mode = e > 3 ? 11 : 15;
    if (k.state == Known::reward) {
        g_mode = 13;
        const auto& tiles = gtiles_[group];
        // The clock starts once we are close: an enemy on the region (contested), units on its tiles or a
        // way we cannot find would otherwise hold us here for hundreds of rounds.
        if (e <= 3 || tiles.is_set(pos_)) {
            if (capture_g_ != group) { capture_g_ = group; capture_since_ = now; }
            if (now - capture_since_ > cambc::constants::capture_time + 10) {
                k_[group].blocked_until = now + 60;
                capture_g_ = -1;
                target_ = -1;
                return;
            }
        }
        if (tiles.is_set(pos_)) return;
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
    if (hit_group(group)) reset_hit(group);
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
