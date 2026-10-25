// The combat subsystem: fights with enemy champions (enemies(), strike(), retreat(), threat_at(), camp_fire()).
//
// Rules it is built on (GameRules.md + patches 100-102): basic attacks reach 1 (melee) or 3 (ranged) tiles,
// Chebyshev; ranged champions either walk or attack in a turn (a reset gives the attack back), melee do both;
// stuns take the target's next turn; a kill pays 100 gold plus the victim's carried gems, a camp kill destroys
// them; the dead sit out 60 rounds; spawn tiles heal and enemies cannot step on them; recall takes 5 turns
// and damage cancels it.
//
// Turn order decides even fights. Champions act one at a time, alternating teams (A0 B0 A1 B1 ...), ranked
// by priority and then draft order (nobody we meet sets priorities; the starter's random ones only blur the
// model), and the starting team swaps every round. So a champion and the enemy of the same rank get two
// turns in a row each in turn (us, them, them, us, us, ...), while differently ranked pairs strictly
// alternate. Drafts are mirrored in this league (both teams pick the same five in the same order), so the
// duels that decide a lane are exactly the equal-rank pairs, and a mirror duel is won by whoever lands the
// first hit of a double turn. The fight model therefore plays the fight hit by hit in the engine's order:
// who has already acted this round (has_completed_turn), who starts the next ones (learnt from the first
// equal-rank enemy we see, shared in two spare bits of the sighting bytes), each fighter's walk into reach,
// its burst on the first hit and every 8th turn (resets and spells come back), a ready stun skipping the
// most dangerous opponent's next turn, camp fire at each round start, spawn heals at turn end.
//
// Model: every visible fighter near us (allies near us or near the enemies, enemies within 9) becomes a
// Fighter. We engage when the kills we expect are worth more than the deaths (parent rule). A retreat walks
// a field that routes around camps (the plain walking field leads into camp pockets), never stands still
// under fire with a target in range, and recalls once nobody is close. Ranged champions approach to tiles
// melee enemies cannot reach next turn.
#include <cambc.h>
#include "bot.h"
#include "fuel.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <string>
#include <variant>

namespace {

constexpr bool kTrace = false;
constexpr int kSimRounds = 10;         // rounds the fight model plays out
constexpr bool kOrdered = true;        // hit by hit in turn order (false: both sides hit at once each round)
constexpr float kEngageBias = -150.0f; // fight value needed to engage
constexpr float kHurtValue = 60.0f;    // value of a full health bar lost (it sends the fighter home)
constexpr float kDecisive = 0.3f;      // winner's health margin at which the simulated kills count in full
constexpr bool kSmooth = false;        // ... (off: every simulated kill counts in full)
constexpr int kChaseTurns = 8;         // approach turns without a hit before a chase is dropped
constexpr int kNoChaseRounds = 15;     // ... for this long
constexpr int kAllyDelayMax = 3;       // allies further than this many turns from the fight are not counted
constexpr float kKillNow = 0.85f;      // take a fight when our burst finishes an enemy (fraction of its health)
constexpr int kRecallDist = 7;         // recall from a retreat once every enemy is at least this far
constexpr int kReachPad = 3;           // enemies further than their reach + this are ignored while healthy
constexpr float kFleeHp = 0.5f;        // ... above this health fraction
constexpr int kHuntPad = 3;            // engaged, but nobody within our range + this: farm unless a kill is expected
constexpr float kHelpHp = 0.6f;        // answer an ally's sighting above this health fraction
constexpr int kSpawnHold = 20;         // turns we hold our spawn against enemies before walking out
constexpr bool kSafeApproach = true;   // close in so that they walk into our reach (see the approach)
constexpr bool kStandAndFight = true;  // a retreat that cannot get away shoots back instead of standing
constexpr bool kCaughtShots = false;   // ranged, caught by a melee chaser: shoot back instead of stepping
constexpr bool kEngageDash = true;     // melee dashers dash in and hit from up to four tiles
constexpr int kHoldMax = 3;            // turns we wait at the edge of an enemy's reach before farming on
constexpr bool kMarginal = true;       // stay out of a fight the allies win without us when joining costs us
constexpr bool kPressRunners = false;  // the approach ignores the reach of enemies walking away from us

std::uint32_t handle_of(const cambc::champion& c) {
    return static_cast<std::uint32_t>(static_cast<cambc::handle_champion_t>(c));
}

// Turn ranks: each team's champions in draft order, read once.
struct Ranks {
    bool read = false;
    std::vector<std::uint32_t> ally, enemy;
    int parity = -1;  // rounds r with r % 2 == parity are started by our team (-1 unknown)
    std::array<vec2, 8> last_pos{};   // where each enemy (by rank) stood at our last turn ...
    std::array<int, 8> last_round{};  // ... and that round
};
Ranks g_ranks;
int g_hold = 0;  // consecutive turns spent waiting for an enemy to walk into our reach
Ranks& ranks() {
    if (!g_ranks.read) {
        g_ranks.read = true;
        try {
            for (const auto& c : cambc::champions(team::ally)) g_ranks.ally.push_back(handle_of(c));
            for (const auto& c : cambc::champions(team::enemy)) g_ranks.enemy.push_back(handle_of(c));
        } catch (const std::exception&) {}
    }
    return g_ranks;
}
int rank_in(const std::vector<std::uint32_t>& v, std::uint32_t h) {
    for (size_t i = 0; i < v.size(); ++i)
        if (v[i] == h) return static_cast<int>(i);
    return static_cast<int>(v.size());
}

// Read one champion into the model; false when the handle went stale.
bool read_fighter(const cambc::champion& c, vec2 p, Fighter& f) {
    try {
        if (!c.is_alive()) return false;
        f.pos = p;
        f.hp = c.health();
        f.max_hp = std::max(1.0f, c.max_health());
        f.str = c.strength();
        f.range = c.attack_range();
        f.ranged = f.range > 1;
        const float focus = c.focus();
        bool reset = false;
        float spell = 0;
        for (const auto& ab : c.abilities()) {
            bool ready = true;
            try { ready = ab.cooldown() <= 0; } catch (const std::exception&) {}
            if (ab.is_dash()) { if (ready) f.dasher = true; continue; }
            if (ab.is_attack_reset()) {
                reset = true;
                if (ready) {
                    f.burst += f.str;
                    try {
                        const auto& info = ab.info();
                        if (info)
                            if (auto* r = std::get_if<cambc::ability_attack_reset>(&*info); r && r->enhanced_attack) f.stun = true;
                    } catch (const std::exception&) {}
                }
                continue;
            }
            if (ab.is_shield()) {
                if (ready) {
                    float s = 2.0f * focus;
                    try {
                        const auto& info = ab.info();
                        if (info)
                            if (auto* sh = std::get_if<cambc::ability_shield>(&*info)) s = sh->modifier * focus;
                    } catch (const std::exception&) {}
                    f.hp += s;
                }
                continue;
            }
            if (ab.is_damage() || ab.is_cc() || ab.is_aoe_damage()) {
                float d = 0;
                try { d = ab.damage_amount(); } catch (const std::exception&) { d = focus; }
                                spell += d;
                if (ready) f.burst += d;
                if (ready && ab.is_cc()) f.stun = true;
            }
        }
        // A reset every 8 turns is one more basic hit; a damage spell lands once per cooldown.
        f.sustain = f.str * (reset ? 9.0f / 8.0f : 1.0f) + spell / 8.0f;
        f.cycle = (reset ? f.str : 0.0f) + spell;
        const auto name = c.champion_name();
        if (name == cambc::champion_type::heart_of_ruin) {
            const float bonus = 1.0f + 0.5f * (1.0f - f.hp / f.max_hp);
            f.sustain *= bonus;
            f.burst *= bonus;
            f.cycle *= bonus;
            f.str *= bonus;
        }
        f.immune = name == cambc::champion_type::marcella;
        try {
            const auto b = c.buffs();
            f.stunned = b.has_stun();
            if (b.has_shield()) f.hp += b.get<cambc::buff_shield>().value;
        } catch (const std::exception&) {}
        try { f.channel = c.is_recalling() || c.is_submitting_gems(); } catch (const std::exception&) {}
        try { f.gems = static_cast<int>(c.gem_count()); } catch (const std::exception&) { f.gems = 0; }
        f.value = 150.0f + 2.0f * static_cast<float>(f.gems);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

struct Outcome {
    float value = 0;        // kills minus deaths (and health lost) from our side
    bool me_dies = false;
    int our_deaths = 0, their_deaths = 0;
    int turns = 0;          // turns until one side was gone
};

// Focus fire to the end: each side pours its damage into its weakest living opponent (overflow moves on),
// fighters join once their delay has passed (with their burst on that turn), a ready stun takes the most
// dangerous opposing fighter's next turn, camps keep hitting whoever they hit, spawn tiles heal fully.
Outcome play_out(const std::vector<Fighter>& a, const std::vector<Fighter>& b, int turns) {
    Outcome r;
    const size_t na = std::min<size_t>(a.size(), 16), nb = std::min<size_t>(b.size(), 16);
    std::array<float, 16> ha{}, hb{};
    std::array<int, 16> stun_a{}, stun_b{};  // turn on which the fighter is stunned (-1 none)
    for (size_t i = 0; i < na; ++i) { ha[i] = a[i].hp; stun_a[i] = a[i].stunned ? a[i].delay : -1; }
    for (size_t i = 0; i < nb; ++i) { hb[i] = b[i].hp; stun_b[i] = b[i].stunned ? b[i].delay : -1; }
    auto pour = [](float dmg, std::array<float, 16>& h, size_t n) {
        while (dmg > 0) {
            int k = -1;
            for (size_t i = 0; i < n; ++i)
                if (h[i] > 0 && (k < 0 || h[i] < h[static_cast<size_t>(k)])) k = static_cast<int>(i);
            if (k < 0) return;
            const float take = std::min(dmg, h[static_cast<size_t>(k)]);
            h[static_cast<size_t>(k)] -= take;
            dmg -= take;
            if (h[static_cast<size_t>(k)] <= 0.5f) h[static_cast<size_t>(k)] = 0;
        }
    };
    auto stun_target = [](const std::vector<Fighter>& side, const std::array<float, 16>& h, size_t n) {
        int k = -1;
        for (size_t i = 0; i < n; ++i)
            if (h[i] > 0 && !side[i].immune && (k < 0 || side[i].sustain > side[static_cast<size_t>(k)].sustain)) k = static_cast<int>(i);
        return k;
    };
    auto side_damage = [&](const std::vector<Fighter>& s, size_t n, const std::array<float, 16>& h, const std::array<int, 16>& st,
                           const std::vector<Fighter>& other, const std::array<float, 16>& oh, size_t on,
                           std::array<int, 16>& ost, int t) {
        float d = 0;
        for (size_t i = 0; i < n; ++i) {
            if (h[i] <= 0 || s[i].delay > t || st[i] == t) continue;
            d += s[i].sustain + (t == s[i].delay ? s[i].burst : 0.0f);
            if (t == s[i].delay && s[i].stun) {
                const int k = stun_target(other, oh, on);
                if (k >= 0) ost[static_cast<size_t>(k)] = std::max(t + 1, other[static_cast<size_t>(k)].delay);
            }
        }
        return d;
    };
    for (int t = 0; t < turns; ++t) {
        const float da = side_damage(a, na, ha, stun_a, b, hb, nb, stun_b, t);
        const float db = side_damage(b, nb, hb, stun_b, a, ha, na, stun_a, t);
        pour(da, hb, nb);
        pour(db, ha, na);
        bool any_a = false, any_b = false, hurt_a = false, hurt_b = false;
        for (size_t i = 0; i < na; ++i) {
            if (ha[i] <= 0) continue;
            ha[i] -= a[i].camp_in;
            if (ha[i] <= 0) { ha[i] = 0; continue; }
            if (a[i].safe) ha[i] = a[i].max_hp;
            any_a = true;
            if (a[i].sustain > 0) hurt_a = true;
        }
        for (size_t i = 0; i < nb; ++i) {
            if (hb[i] <= 0) continue;
            hb[i] -= b[i].camp_in;
            if (hb[i] <= 0) { hb[i] = 0; continue; }
            if (b[i].safe) hb[i] = b[i].max_hp;
            any_b = true;
            if (b[i].sustain > 0) hurt_b = true;
        }
        r.turns = t + 1;
        if (!any_a || !any_b || (!hurt_a && !hurt_b)) break;
    }
    float our_lost = 0, their_lost = 0, kills = 0;
    float start_a = 0, left_a = 0, start_b = 0, left_b = 0;
    for (size_t i = 0; i < na; ++i) {
        our_lost += (a[i].hp - ha[i]) / a[i].max_hp;
        start_a += a[i].hp;
        left_a += ha[i];
        if (ha[i] <= 0) { ++r.our_deaths; kills -= a[i].value; if (i == 0) r.me_dies = true; }
    }
    for (size_t i = 0; i < nb; ++i) {
        their_lost += (b[i].hp - hb[i]) / b[i].max_hp;
        start_b += b[i].hp;
        left_b += hb[i];
        if (hb[i] <= 0) { ++r.their_deaths; kills += b[i].value; }
    }
    // A fight played to the end is won by a thin margin as often as by a wide one, and a thin one turns
    // with the order of the next few hits: kills count in full only when the winner keeps a clear margin
    // of its health (a mirror 1v1 reads as even instead of flipping between won and lost every turn).
    const float margin = std::abs(left_a / std::max(1.0f, start_a) - left_b / std::max(1.0f, start_b));
    r.value = kSmooth ? kills * std::min(1.0f, margin / kDecisive) : kills;
    // Damage that does not kill still sends them home (and us).
    r.value += kHurtValue * (their_lost - our_lost);
    return r;
}

// The same fight hit by hit, in the engine's turn order. a[0] is us, acting now; a fighter marked done has
// acted this round already. Each fighter walks for `delay` of its turns, then hits the weakest living
// opponent: strength, plus its ready burst on the first hit and its reset and spells again every 8th turn.
// A ready stun on the first hit takes the most dangerous opponent's next turn. Camps fire at every round
// start; a fighter on its own spawn heals fully at the end of each of its turns.
Outcome play_ordered(const std::vector<Fighter>& a, const std::vector<Fighter>& b, int rounds, bool we_start, bool me_in = true) {
    struct Unit { float hp; int side, k, rank, active, skip, delay; };
    std::array<Unit, 16> u{};
    size_t n = 0;
    for (size_t i = 0; i < a.size() && n < u.size(); ++i)
        u[n++] = {a[i].hp, 0, static_cast<int>(i), a[i].rank, 0, a[i].stunned ? 1 : 0, a[i].delay};
    for (size_t i = 0; i < b.size() && n < u.size(); ++i)
        u[n++] = {b[i].hp, 1, static_cast<int>(i), b[i].rank, 0, b[i].stunned ? 1 : 0, b[i].delay};
    auto fighter = [&](const Unit& x) -> const Fighter& { return x.side == 0 ? a[static_cast<size_t>(x.k)] : b[static_cast<size_t>(x.k)]; };
    // Turn slots of a round: rank-major, the starting team first within a rank.
    std::array<size_t, 16> order{};
    auto sort_round = [&](bool ours_first) {
        for (size_t i = 0; i < n; ++i) order[i] = i;
        std::sort(order.begin(), order.begin() + n, [&](size_t x, size_t y) {
            const int kx = u[x].rank * 2 + ((u[x].side == 0) == ours_first ? 0 : 1);
            const int ky = u[y].rank * 2 + ((u[y].side == 0) == ours_first ? 0 : 1);
            return kx != ky ? kx < ky : x < y;
        });
    };
    auto alive = [&](int side) {
        for (size_t i = 0; i < n; ++i)
            if (u[i].side == side && u[i].hp > 0) return true;
        return false;
    };
    auto act = [&](size_t i) {
        Unit& x = u[i];
        if (x.hp <= 0) return;
        const Fighter& f = fighter(x);
        if (x.skip > 0) { --x.skip; return; }
        if (x.delay > 0) { --x.delay; return; }
        float dmg = f.str;
        if (x.active == 0) dmg += f.burst;
        else if (x.active % 8 == 0) dmg += f.cycle;
        int t = -1, s = -1;
        for (size_t j = 0; j < n; ++j) {
            if (u[j].side == x.side || u[j].hp <= 0) continue;
            if (t < 0 || u[j].hp < u[static_cast<size_t>(t)].hp) t = static_cast<int>(j);
            const Fighter& o = fighter(u[j]);
            if (!o.immune && (s < 0 || o.sustain > fighter(u[static_cast<size_t>(s)]).sustain)) s = static_cast<int>(j);
        }
        if (t >= 0) {
            u[static_cast<size_t>(t)].hp -= dmg;
            if (u[static_cast<size_t>(t)].hp <= 0.5f) u[static_cast<size_t>(t)].hp = 0;
        }
        if (x.active == 0 && f.stun && s >= 0 && u[static_cast<size_t>(s)].hp > 0) u[static_cast<size_t>(s)].skip = 1;
        ++x.active;
        if (f.safe && x.hp > 0) x.hp = f.max_hp;
    };
    Outcome r;
    for (int t = 0; t < rounds; ++t) {
        const bool ours_first = we_start == (t % 2 == 0);
        if (t > 0)
            for (size_t i = 0; i < n; ++i) {
                if (u[i].hp <= 0) continue;
                u[i].hp -= fighter(u[i]).camp_in;
                if (u[i].hp <= 0.5f) u[i].hp = 0;
            }
        sort_round(ours_first);
        if (t == 0) {
            // We act now; who acted before us this round sits this one out.
            if (me_in) act(0);
            for (size_t o = 0; o < n; ++o) {
                const size_t i = order[o];
                if ((me_in && i == 0) || fighter(u[i]).done) continue;
                act(i);
            }
        } else {
            for (size_t o = 0; o < n; ++o) act(order[o]);
        }
        r.turns = t + 1;
        if (!alive(0) || !alive(1)) break;
    }
    float our_lost = 0, their_lost = 0, kills = 0;
    float start_a = 0, left_a = 0, start_b = 0, left_b = 0;
    for (size_t i = 0; i < n; ++i) {
        const Fighter& f = fighter(u[i]);
        const float left = std::max(0.0f, u[i].hp);
        if (u[i].side == 0) {
            our_lost += (f.hp - left) / f.max_hp;
            start_a += f.hp;
            left_a += left;
            if (left <= 0) { ++r.our_deaths; kills -= f.value; if (u[i].k == 0 && me_in) r.me_dies = true; }
        } else {
            their_lost += (f.hp - left) / f.max_hp;
            start_b += f.hp;
            left_b += left;
            if (left <= 0) { ++r.their_deaths; kills += f.value; }
        }
    }
    const float margin = std::abs(left_a / std::max(1.0f, start_a) - left_b / std::max(1.0f, start_b));
    r.value = kSmooth ? kills * std::min(1.0f, margin / kDecisive) : kills;
    r.value += kHurtValue * (their_lost - our_lost);
    return r;
}

// Scratch for the retreat field.
struct Boards {
    cambc::bitboard walls, roots, dest, seen;
    cambc::distance_field f;
};
Boards* g_b = nullptr;
Boards& boards() {
    if (!g_b) g_b = new Boards;
    return *g_b;
}

}  // namespace

// Camp damage each fighter takes per turn: an awake camp hits the nearest champion within its reach.
void Bot::camp_fire(std::vector<Fighter>& ours, std::vector<Fighter>& theirs) const {
    if (awake_.is_empty()) return;
    for (auto p : awake_.positions()) {
        if (cheb(p, pos_) > 10) continue;
        const int gi = group_at_[static_cast<size_t>(tidx(p))];
        const int ci = camp_at_[static_cast<size_t>(tidx(p))];
        if (gi < 0 || ci < 0) continue;
        const auto s = camp_spec(g_[gi].kinds[static_cast<size_t>(ci)]);
        if (s.str <= 0) continue;
        Fighter* best = nullptr;
        int bd = 99;
        for (auto& f : ours) { const int d = cheb(f.pos, p); if (d < bd) { bd = d; best = &f; } }
        for (auto& f : theirs) { const int d = cheb(f.pos, p); if (d < bd) { bd = d; best = &f; } }
        if (best && bd <= s.range) best->camp_in += s.str * (best == &ours[0] ? taken_ : 1.0f);
    }
}

// Damage the enemies can put on tile t before our next turn. An enemy gets `moves` turns until then (0, 1
// or 2: equal-rank pairs take turns in pairs). A melee enemy steps and hits in one turn, a dash adds two
// steps once; a ranged one walks or shoots; its spells reach 4.
static float hits_before_us(const Fighter& e, int d) {
    if (e.stunned || e.hp <= 0 || e.moves <= 0) return 0;
    int steps = std::max(0, d - e.range);
    if (!e.ranged && e.dasher) steps = std::max(0, steps - 2);
    const int first = e.ranged ? steps + 1 : std::max(1, steps);  // the turn of its first hit
    return static_cast<float>(std::max(0, e.moves - first + 1));
}
float Bot::threat_at(vec2 t, const std::vector<Fighter>& theirs) const {
    float th = 0;
    for (const auto& e : theirs) {
        const float hits = hits_before_us(e, cheb(e.pos, t));
        if (hits > 0) th += hits * e.str + e.burst * 0.5f;
        else if (e.moves > 0 && !e.stunned && e.hp > 0 && cheb(e.pos, t) <= 4 && e.burst > e.str)
            th += (e.burst - e.str) * 0.5f;  // a ranged spell
    }
    return th;
}

// Run from the enemies: the free neighbouring tile (or dash landing) with the least threat, nearer home by a
// walk that goes around camps. Recall once nobody can reach us soon. Cornered, we hit back.
bool Bot::retreat(std::vector<Fighter>& theirs, int dmin) {
    auto& me = cambc::self();
    if (spawn_local_.at(pos_)) {
        // The spawn heals us every turn and they cannot step on it: hold it and shoot from there.
        if (++spawn_hold_ > kSpawnHold) return false;
        strike(theirs);
        return true;
    }
    spawn_hold_ = 0;
    // Out of reach of everyone for a few turns: recall (damage would cancel it).
    if (dmin >= kRecallDist && !moved_ && incoming_ == 0 && live() && home_.at(pos_) > 6) {
        try { me.recall(); } catch (const cambc::error& e) { warn("flee recall", e); }
        return true;
    }
    // Melee: a free swing at an adjacent enemy before stepping away.
    if (me.attack_range() <= 1 && me.can_attack() && live())
        for (const auto& e : theirs)
            if (e.hp > 0 && cheb(e.pos, pos_) <= 1 && view().lvis.at(e.pos)) {
                try {
                    if (!cambc::vision::has_champion(e.pos)) break;
                    me.attack(cambc::vision::champion_at(e.pos));
                    attacked_ = true;
                    invalidate();
                } catch (const cambc::error& ex) { warn("swing", ex); }
                break;
            }
    // Ranged: a free shot when nobody can hit us here next turn.
    if (me.attack_range() > 1 && me.can_attack() && !moved_ && incoming_ == 0 && live()
        && threat_at(pos_, theirs) == 0 && strike(theirs))
        return true;
    // Ranged and caught: a melee chaser hits us wherever we step (it walks and hits in one turn), so a step
    // only gives it a free hit. Shoot back unless home is a few steps away or a dash could break contact.
    if (kCaughtShots && me.attack_range() > 1 && me.can_attack() && !moved_ && live() && home_.at(pos_) > 3) {
        auto melee_hits = [&](vec2 t) {
            float h = 0;
            for (const auto& e : theirs)
                if (!e.ranged) h += hits_before_us(e, cheb(e.pos, t));
            return h;
        };
        bool caught = melee_hits(pos_) > 0;
        for (auto d : kDirs) {
            if (!caught) break;
            const vec2 t{pos_.x + d.x, pos_.y + d.y};
            if (free_tile(t) && !camp_block_.is_set(t) && melee_hits(t) == 0) caught = false;
        }
        bool dash = false;
        if (caught && dasher_ && !cast_ && me.can_cast())
            for (const auto& ab : me.abilities())
                if (ab.is_dash() && ab.is_ready()) dash = true;
        if (caught && !dash && strike(theirs)) return true;
    }
    if (moved_ || move_failed_ || !me.can_move() || !live()) return true;
    // Walking distance home around walls and the camps we know of (cut off at 40: far beyond, the plain
    // walking field decides).
    const Dist* plain = &home_;
    const std::uint16_t* routed = nullptr;
    if (fuel(45000)) {
        auto& b = boards();
        b.walls.clear();
        b.walls |= walk_walls();
        b.walls |= camp_block_;
        b.walls |= view().camps;
        raw_clear(b.walls.ptr(), pos_);
        b.dest.clear();
        for (int dx = -2; dx <= 2; ++dx)
            for (int dy = -2; dy <= 2; ++dy) {
                const vec2 t{pos_.x + dx, pos_.y + dy};
                if (inside(t)) raw_set(b.dest.ptr(), t);
            }
        b.seen.clear();
        b.f.compute_to(b.walls, cambc::map::spawn_region(team::ally), b.dest, b.seen, 60);
        if (raw_bit(b.seen.ptr(), pos_)) routed = static_cast<const cambc::distance_field&>(b.f).ptr();
    }
    auto home_dist = [&](vec2 t) -> int {
        if (routed) {
            if (!inside(t) || !raw_bit(boards().seen.ptr(), t)) return kFar;
            return raw_dist(routed, t);
        }
        return plain->at(t);
    };
    const Bits& sp = spawn_local_;
    // Awake camps keep hitting whoever is nearest within their reach: stay out of it too.
    std::array<std::pair<vec2, CampSpec>, 24> hot;
    size_t n_hot = 0;
    if (!awake_.is_empty())
        for (auto p : awake_.positions()) {
            if (cheb(p, pos_) > 7 || n_hot >= hot.size()) continue;
            const int gi = group_at_[static_cast<size_t>(tidx(p))];
            const int ci = camp_at_[static_cast<size_t>(tidx(p))];
            if (gi < 0 || ci < 0) continue;
            const auto cs = camp_spec(g_[gi].kinds[static_cast<size_t>(ci)]);
            if (cs.str > 0) hot[n_hot++] = {p, cs};
        }
    auto danger = [&](vec2 t) {
        float s = threat_at(t, theirs);
        for (size_t i = 0; i < n_hot; ++i)
            if (cheb(t, hot[i].first) <= hot[i].second.range) s += hot[i].second.str * taken_;
        if (sp.at(t)) s *= 0.1f;
        return s;
    };
    auto score = [&](vec2 t) {
        float s = danger(t) * 4.0f;
        const int hd = home_dist(t);
        s += static_cast<float>(hd >= kFar ? 200 : hd);
        if (camp_block_.is_set(t)) s += 500;
        return s;
    };
    const float here = score(pos_);
    const float here_danger = danger(pos_);
    // Dash out when one step does not escape.
    if (dasher_ && !cast_ && me.can_cast() && fuel(15000) && here_danger > 0) {
        for (const auto& ab : me.abilities()) {
            if (!ab.is_dash() || !ab.is_ready()) continue;
            std::array<std::pair<float, vec2>, 16> cand;
            size_t n = 0;
            for (int dx = -2; dx <= 2; ++dx)
                for (int dy = -2; dy <= 2; ++dy) {
                    if (std::max(std::abs(dx), std::abs(dy)) != 2) continue;
                    const vec2 t{pos_.x + dx, pos_.y + dy};
                    if (!free_tile(t) || camp_block_.is_set(t)) continue;
                    bool path = false;
                    for (auto s : kDirs) {
                        const vec2 m{pos_.x + s.x, pos_.y + s.y};
                        if (!inside(m) || cheb(m, t) != 1) continue;
                        if (hard_local().at(m) || view().lchamps.at(m) || view().lcamps.at(m)) continue;
                        path = true;
                        break;
                    }
                    if (path) cand[n++] = {score(t), t};
                }
            std::sort(cand.begin(), cand.begin() + n, [](auto x, auto y) { return x.first < y.first; });
            for (size_t i = 0; i < n && i < 3 && cand[i].first < here - 2; ++i) {
                if (!live()) return true;
                try {
                    const bool ok = ab.use(cand[i].second);
                    cast_ = true;
                    invalidate();
                    if (!ok) { move_failed_ = true; break; }
                    pos_ = me.position();
                    break;
                } catch (const cambc::error&) {}
            }
            break;
        }
    }
    if (move_failed_ || !me.can_move() || !live()) return true;
    vec2 best = pos_;
    float best_s = score(pos_);
    for (auto d : kDirs) {
        const vec2 t{pos_.x + d.x, pos_.y + d.y};
        if (!free_tile(t) || camp_block_.is_set(t)) continue;
        const float s = score(t);
        if (s < best_s) { best_s = s; best = t; }
    }
    if (best == pos_) {
        // Nowhere better to go: hit back rather than stand.
        if (kStandAndFight && me.can_attack() && live()) strike(theirs);
        return true;
    }
    moved_ = true;
    bool ok = false;
    try { ok = me.move_absolute(best); } catch (const cambc::error& e) { warn("flee move", e); }
    invalidate();
    if (!ok) move_failed_ = true;
    else pos_ = me.position();
    return true;
}

// Attack the best enemy in basic range, then reset and hit again. True when a hit landed.
bool Bot::strike(std::vector<Fighter>& theirs) {
    auto& me = cambc::self();
    if (!me.can_attack() || !live()) return false;
    const vec2 at = me.position();
    const int range = me.attack_range();
    const float str = me.strength();
    auto pick = [&]() -> int {
        int best = -1;
        float bs = -1e9f;
        for (size_t i = 0; i < theirs.size(); ++i) {
            const auto& e = theirs[i];
            if (e.hp <= 0 || cheb(e.pos, at) > range || !view().lvis.at(e.pos)) continue;
            // The most damage removed per hit: its damage over the hits it takes to kill.
            float s = -e.hp / std::max(1.0f, e.sustain);
            if (e.hp <= str * 0.9f) s += 1000 + e.value;
            if (e.channel) s += 50;
            s += static_cast<float>(e.gems) * 0.5f;
            if (s > bs) { bs = s; best = static_cast<int>(i); }
        }
        return best;
    };
    auto hit = [&](int i) -> bool {
        auto& e = theirs[static_cast<size_t>(i)];
        try {
            if (!cambc::vision::has_champion(e.pos)) return false;
            auto c = cambc::vision::champion_at(e.pos);
            if (!c.is_visible() || !c.is_alive()) return false;
            me.attack(c);
            attacked_ = true;
            invalidate();
            e.hp -= str;
            return true;
        } catch (const cambc::error& ex) { warn("strike", ex); }
        return false;
    };
    const int first = pick();
    if (first < 0 || !hit(first)) return false;
    // A second hit through the reset, only when someone is left in range for it.
    if ((me.can_attack() || reset_ready()) && live() && pick() >= 0 && use_reset()) {
        const int next = pick();
        if (next >= 0) hit(next);
    }
    return true;
}

// True when the turn is spent.
bool Bot::enemies() {
    auto& me = cambc::self();
    const int now = static_cast<int>(cambc::round_number());
    const auto& enemy_spawn = cambc::map::spawn_region(team::enemy);
    std::vector<Fighter> theirs, ours;
    auto& rk = ranks();
    const int my_rank = rank_in(rk.ally, handle_of(me));
    if (rk.parity < 0) rk.parity = read_start_parity();
    // Us first (index 0), then allies near enough to join.
    {
        Fighter f;
        if (!read_fighter(me, pos_, f)) return false;
        f.rank = my_rank;
        f.safe = spawn_local_.at(pos_);
        f.value = 150.0f + 2.0f * static_cast<float>(me.gem_count());
        ours.push_back(f);
    }
    int dmin = 99;
    const auto tiles = cambc::vision::champion_tiles(team::enemy);
    if (!tiles.is_empty())
        for (auto p : tiles.positions()) {
            if (enemy_spawn.is_set(p)) continue;
            const int d = cheb(p, pos_);
            if (d > 9) continue;
            try {
                Fighter f;
                const auto c = cambc::vision::champion_at(p);
                if (!read_fighter(c, p, f)) continue;
                f.rank = rank_in(rk.enemy, handle_of(c));
                f.done = c.has_completed_turn();
                // Its turns before our next one: one, or for the enemy of our rank none after it started
                // this round (we start the next) and two when we started it.
                if (f.rank == my_rank) f.moves = f.done ? 0 : 2;
                // Moving away from us since our last turn: running, not coming to fight.
                if (f.rank >= 0 && f.rank < 8) {
                    const auto r = static_cast<size_t>(f.rank);
                    if (rk.last_round[r] == now - 1 && cheb(p, pos_) > cheb(rk.last_pos[r], pos_)) f.fleeing = true;
                    rk.last_pos[r] = p;
                    rk.last_round[r] = now;
                }
                // The enemy of our own rank acted before us exactly when its team started this round.
                if (f.rank == my_rank && rk.parity < 0) {
                    rk.parity = f.done ? (now + 1) % 2 : now % 2;
                    write_start_parity(rk.parity);
                }
                theirs.push_back(f);
                dmin = std::min(dmin, d);
            } catch (const std::exception&) {}
            if (theirs.size() >= 8) break;
        }
    if (theirs.empty()) {
        // Help an ally who reported an invader close by.
        const auto s = read_sighting();
        if (s.ok && s.age <= 4 && cheb(s.where, pos_) <= 14 && me.health() > me.max_health() * kHelpHp
            && me.gem_count() < 30 && incoming_ == 0) {
            auto goal = empty_board();
            goal.set(s.where);
            goal = grow(goal, 2) & ~walk_walls();
            return go(goal, true);
        }
        return false;
    }
    last_enemy_seen_ = now;
    write_sighting(theirs.front().pos);
    for (const auto& a : others_)
        try {
            cambc::detail::champion_info info{};
            if (!peek(a, info)) continue;  // out of our sight (position() would throw)
            const vec2 p{info.x, info.y};
            if (!inside(p)) continue;
            // Near us or near the enemies: our own step must not change who counts.
            bool near = cheb(p, pos_) <= 7;
            for (size_t i = 0; i < theirs.size() && !near; ++i) near = cheb(p, theirs[i].pos) <= 8;
            if (!near || a.is_recalling()) continue;
            Fighter f;
            if (!read_fighter(a, p, f)) continue;
            f.safe = spawn_local_.at(p);
            f.rank = rank_in(rk.ally, handle_of(a));
            f.done = f.rank < my_rank;
            ours.push_back(f);
            if (ours.size() >= 8) break;
        } catch (const std::exception&) {}
    // Turns before each fighter can hit: walk to its range of the nearest opponent (melee hits on arrival).
    auto set_delay = [](Fighter& f, const std::vector<Fighter>& other) {
        int d = 99;
        for (const auto& o : other) d = std::min(d, cheb(o.pos, f.pos));
        f.delay = std::max(0, d - f.range - (f.ranged ? 0 : 1));
        if (f.stunned) f.delay = std::max(f.delay, 1);
    };
    ally_near_.clear();
    for (size_t i = 1; i < ours.size(); ++i) ally_near_.push_back(ours[i].pos);
    for (auto& f : ours) set_delay(f, theirs);
    for (size_t i = ours.size(); i-- > 1;)
        if (ours[i].delay > kAllyDelayMax) ours.erase(ours.begin() + static_cast<long>(i));
    for (auto& f : theirs) set_delay(f, ours);
    ours[0].camp_in = 0;
    camp_fire(ours, theirs);

    // Unknown order (no enemy of our rank seen yet): assume they start the next round.
    const bool we_start = rk.parity < 0 ? true : now % 2 == rk.parity;
    auto model = [&](const std::vector<Fighter>& a, const std::vector<Fighter>& b) {
        return kOrdered ? play_ordered(a, b, kSimRounds, we_start) : play_out(a, b, kSimRounds);
    };
    const Outcome fight = model(ours, theirs);
    // The allies' fight without us (we would get away).
    float without = 0;
    if (kMarginal && ours.size() > 1 && kOrdered) {
        std::vector<Fighter> rest(ours.begin() + 1, ours.end());
        without = play_ordered(rest, theirs, kSimRounds, we_start, false).value;
    }
    const float hp = me.health();
    const int gems = static_cast<int>(me.gem_count());
    // Kill on the spot: an enemy our burst finishes this turn without moving.
    bool kill_now = false;
    {
        const float mine = ours[0].str + ours[0].burst;
        const int reach = ours[0].ranged ? ours[0].range : 2;
        for (const auto& e : theirs)
            if (cheb(e.pos, pos_) <= reach && e.hp <= mine * kKillNow && view().lvis.at(e.pos)) kill_now = true;
    }
    // Allies who win without us are not helped by our death; a fight they lose is decided by the plain rule
    // (every champion assuming the others stay would walk into it together).
    bool engage = fight.value > kEngageBias && (without <= 0 || fight.value - without > kEngageBias)
        && !(fight.me_dies && fight.their_deaths == 0);
    // Carrying a big bag: only clearly winning fights.
    if (engage && fight.me_dies && gems >= 40 && fight.value < 2.0f * static_cast<float>(gems)) engage = false;
    if (!engage && kill_now && !(fight.me_dies && gems >= 60)) engage = true;
    // The spawn's heal wins this fight, not us: hold it rather than walk out.
    bool hold_spawn = false;
    if (engage && ours[0].safe) {
        auto off = ours;
        off[0].safe = false;
        const Outcome out = model(off, theirs);
        hold_spawn = !(out.value > kEngageBias && !(out.me_dies && out.their_deaths == 0));
    }
    if (kTrace) {
        std::string line = "fight r" + std::to_string(now) + ' ' + cambc::to_string(me.champion_name()) + " v" + std::to_string(static_cast<int>(fight.value))
            + " wo" + std::to_string(static_cast<int>(without)) + " ws" + std::to_string(we_start) + std::to_string(rk.parity) + " die" + std::to_string(fight.me_dies) + " k" + std::to_string(fight.their_deaths)
            + " d" + std::to_string(fight.our_deaths) + " t" + std::to_string(fight.turns) + " E" + std::to_string(engage) + " |";
        auto add = [&](const Fighter& f) {
            line += ' ' + std::to_string(f.pos.x) + ',' + std::to_string(f.pos.y) + ':' + std::to_string(static_cast<int>(f.hp)) + '/'
                + std::to_string(static_cast<int>(f.sustain)) + '+' + std::to_string(static_cast<int>(f.burst)) + '@' + std::to_string(f.delay)
                + (f.camp_in > 0 ? "c" : "") + (f.safe ? "s" : "") + 'k' + std::to_string(f.rank) + (f.done ? "d" : "");
        };
        for (const auto& f : ours) add(f);
        line += " ||";
        for (const auto& f : theirs) add(f);
        std::cout << line << '\n';
    }

    fight_round_ = now;
    fight_engaged_ = engage;
    if (!engage) {
        // Far enemies that are not about to reach us: carry on, but bank sooner (tribute checks sightings).
        bool reachable = false;
        for (const auto& e : theirs)
            if (cheb(e.pos, pos_) <= static_cast<int>(e.threat_reach()) + kReachPad) reachable = true;
        if (!reachable && hp > me.max_health() * kFleeHp) return false;
        fight_spells(theirs, false);
        return retreat(theirs, dmin);
    }
    // Strong enough but nobody close: keep farming unless they are worth a short hunt.
    if (dmin > me.attack_range() + kHuntPad && fight.their_deaths == 0) return false;
    if (now < no_chase_until_ && dmin > me.attack_range() + 1) return false;
    if (!live()) return true;
    fight_spells(theirs, true);
    if (strike(theirs)) { chase_turns_ = 0; return true; }
    if (hold_spawn) return ++spawn_hold_ <= kSpawnHold;
    if (move_failed_ || (cast_ && me.attack_range() > 1)) return true;
    if (++chase_turns_ > kChaseTurns) {
        chase_turns_ = 0;
        no_chase_until_ = now + kNoChaseRounds;
        return false;
    }
    const int range = me.attack_range();
    // Melee with the dash ready: dash and step close three tiles in one turn, so the first hit is ours from up
    // to four tiles away. Land beside the target, or one step from it.
    if (kEngageDash && range <= 1 && dasher_ && !cast_ && !moved_ && me.can_cast() && me.can_attack() && dmin <= 4 && fuel(15000)
        && live()) {
        int target = -1;
        float ts = -1e9f;
        for (size_t i = 0; i < theirs.size(); ++i) {
            const auto& e = theirs[i];
            const int d = cheb(e.pos, pos_);
            if (e.hp <= 0 || d < 2 || d > 4 || !view().lvis.at(e.pos)) continue;
            const float s = -e.hp / std::max(1.0f, e.sustain);
            if (s > ts) { ts = s; target = static_cast<int>(i); }
        }
        for (const auto& ab : me.abilities()) {
            if (target < 0 || !ab.is_dash() || !ab.is_ready()) continue;
            const vec2 at = theirs[static_cast<size_t>(target)].pos;
            vec2 land = pos_;
            int best = 99;
            for (int dx = -2; dx <= 2; ++dx)
                for (int dy = -2; dy <= 2; ++dy) {
                    const vec2 t{pos_.x + dx, pos_.y + dy};
                    if (t == pos_ || !free_tile(t) || camp_block_.is_set(t)) continue;
                    const int dt = cheb(t, at);
                    if (dt < 1 || dt > 2) continue;
                    if (std::max(std::abs(dx), std::abs(dy)) == 2) {
                        bool path = false;  // a passable middle tile (holes may be jumped, walls not)
                        for (auto s : kDirs) {
                            const vec2 m{pos_.x + s.x, pos_.y + s.y};
                            if (!inside(m) || cheb(m, t) != 1) continue;
                            if (hard_local().at(m) || view().lchamps.at(m) || view().lcamps.at(m)) continue;
                            path = true;
                            break;
                        }
                        if (!path) continue;
                    }
                    // Beside the target is best; then the landing the fewest enemies can hit.
                    const int score = dt * 100 + static_cast<int>(threat_at(t, theirs) / 10.0f);
                    if (score < best) { best = score; land = t; }
                }
            // A dash that only lands two tiles away from a target we could reach with one step is no gain.
            if (land == pos_ || (cheb(at, pos_) == 2 && cheb(land, at) == 2)) break;
            try {
                const bool ok = ab.use(land);
                cast_ = true;
                invalidate();
                if (!ok) { move_failed_ = true; return true; }
                pos_ = me.position();
            } catch (const cambc::error& e) { warn("engage dash", e); return true; }
            if (cheb(pos_, at) == 2 && me.can_move() && live()) {
                vec2 step = pos_;
                for (auto d : kDirs) {
                    const vec2 t{pos_.x + d.x, pos_.y + d.y};
                    if (free_tile(t) && !camp_block_.is_set(t) && cheb(t, at) == 1) { step = t; break; }
                }
                if (step != pos_) {
                    moved_ = true;
                    bool ok = false;
                    try { ok = me.move_absolute(step); } catch (const cambc::error& e) { warn("dash step", e); }
                    invalidate();
                    if (!ok) move_failed_ = true;
                    pos_ = me.position();
                }
            }
            if (live() && me.can_attack() && strike(theirs)) chase_turns_ = 0;
            return true;
        }
    }
    // Close: one step, or none, chosen so that they walk into our reach rather than we into theirs. The first
    // hit of an even duel decides it, so we stop just outside the reach they have before our next turn (two
    // turns for the enemy of our rank every other round) unless we can close and hit first. A melee
    // champion only weighs melee enemies here (a ranged one hits it on the way in whatever it does).
    bool stepped = false;
    if (kSafeApproach && dmin <= range + 3 && !moved_ && me.can_move()) {
        vec2 best = pos_;
        auto tile_score = [&](vec2 t) {
            int gap = 99;
            float th = 0;
            for (const auto& e : theirs) {
                if (e.hp <= 0) continue;
                gap = std::min(gap, std::max(0, cheb(e.pos, t) - range));
                if ((range > 1 || !e.ranged) && !(kPressRunners && e.fleeing)) {
                    const float hits = hits_before_us(e, cheb(e.pos, t));
                    if (hits > 0) th += hits * e.str + e.burst * 0.5f;
                }
            }
            return 60.0f * static_cast<float>(gap) + th;
        };
        float best_s = tile_score(pos_);
        for (auto d : kDirs) {
            const vec2 t{pos_.x + d.x, pos_.y + d.y};
            if (!free_tile(t) || camp_block_.is_set(t)) continue;
            const float s = tile_score(t) - 1.0f;  // ties: step rather than stand
            if (s < best_s) { best_s = s; best = t; }
        }
        if (best == pos_) {
            // Holding the edge of their reach: a few turns at most (they may not be coming), and an awake
            // camp of our target in range takes the hit meanwhile.
            if (++g_hold > kHoldMax) { g_hold = 0; return false; }
            if (target_ >= 0 && group_awake(target_) && me.can_attack() && live()) hit_group(target_);
            return true;
        }
        g_hold = 0;
        if (live()) {
            moved_ = true;
            bool ok = false;
            try { ok = me.move_absolute(best); } catch (const cambc::error& e) { warn("approach move", e); }
            invalidate();
            if (!ok) move_failed_ = true;
        }
        stepped = true;
    }
    if (!stepped) {
        auto goal = empty_board();
        for (const auto& e : theirs)
            if (e.hp > 0) goal.set(e.pos);
        goal = grow(goal, range) & ~walk_walls();
        go(goal, dmin > range + 1);
    }
    pos_ = me.position();
    // A melee step that arrived, or a ranged step with a reset in hand, still hits this turn.
    if (live() && me.can_attack()) {
        if (strike(theirs)) chase_turns_ = 0;
    } else if (live() && range > 1 && !cast_ && me.can_cast() && !attacked_ && !move_failed_) {
        bool in_range = false;
        for (const auto& e : theirs)
            if (e.hp > 0 && cheb(e.pos, pos_) <= range && view().lvis.at(e.pos)) in_range = true;
        if (in_range && reset_ready() && use_reset() && strike(theirs)) chase_turns_ = 0;
    }
    return true;
}
