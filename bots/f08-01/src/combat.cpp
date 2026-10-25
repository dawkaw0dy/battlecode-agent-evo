// The combat subsystem: fights with enemy champions (Bot::enemies() and its helpers).
//
// Turn order drives everything here. Champions act interleaved by draft index (ours and theirs of the same
// index swap who goes first every round), so an enemy of another index acts exactly once between two of our
// turns, while our same-index counterpart alternates: it acts twice in a row (after us this round, first
// next round) or not at all. has_completed_turn() plus last round's flag tells which.
//
// Every visible fighter near us is read into a small model (health with shields, basic damage, ready burst,
// ready stun and dash, reach, camp fire it takes, what its death is worth, actions before our next turn).
// A focus-fire simulation decides engage or retreat against a fixed bias. Tactics use the threat on each
// tile counted over the actions every enemy really has before our next turn: a retreat steps to the tile
// that takes the fewest hits (an enemy that waits now acts twice after our next turn, so we keep away from
// it too), and a retreating ranged champion shoots while nobody can answer, if it can still step out of
// reach before the enemy's double turn. Measured variants are kept behind switches below: the fight
// simulated action by action in turn order, the cost of running in the engage decision, duels played move
// by move with a two-action lookahead, and new spell and retreat rules (all measured worse or neutral).
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

constexpr bool kTraceFight = false;
constexpr int kHorizon = 10;            // rounds the fight simulation looks ahead
constexpr float kEngageBias = -150.0f;  // fight value (above the value of running) needed to engage
constexpr float kChaseShare = 0.8f;     // hits a melee chaser lands per turn of our run home
constexpr int kRunCap = 12;             // turns of a run home we charge for (chasers give up, allies meet us)
constexpr int kChaseTurns = 8;          // approach turns without a hit before giving up a chase
constexpr float kTradeWinning = 0.5f;   // weight of hits taken against hits dealt when the fight is won
constexpr float kTradeEven = 1.0f;      // ... otherwise
constexpr bool kRunCost = false;     // engage against the cost of running (false: against a fixed bias)
constexpr bool kNeutralSim = true;   // engage decision averaged over both turn orders
constexpr bool kDuelsOnly = true;    // move-by-move tactics only in one-on-one duels
constexpr bool kFreeShots = true;       // retreating ranged: shoot when nobody can answer before our next turn
// Components of this rewrite that can be switched back to the parent's behaviour (A/B tests).
constexpr bool kSeqSim = false;          // fight simulated action by action in turn order (false: simultaneous rounds)
constexpr bool kTurnOrder = true;       // threats counted over each enemy's actions before our next turn (false: one)
constexpr bool kDuelTactics = false;     // duels played move by move (fight_move)
constexpr bool kNewSpells = false;       // stun value by actions; melee holds rooting spells until adjacent
constexpr bool kNewStrike = true;       // target choice prefers enemies about to act; a reset is kept when idle
constexpr bool kNewRetreat = false;      // recall only out of reach of everyone; keep two tiles from melee

float ability_range(const cambc::ability& ab) {
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

bool excludes_walking(const cambc::ability& ab) {
    bool exclusive = true;
    try {
        const auto& info = ab.info();
        if (info) exclusive = std::visit([](const auto& x) { return x.exclusive_with_movement; }, *info);
    } catch (const std::exception&) {}
    return exclusive;
}

std::uint32_t handle_of(const cambc::champion& c) {
    return static_cast<std::uint32_t>(static_cast<cambc::handle_champion_t>(c));
}

// Turn index (draft order) of every champion, read once: position in the team's handle list.
std::vector<std::uint32_t> g_ally_order, g_enemy_order;
int order_of(const std::vector<std::uint32_t>& order, std::uint32_t h) {
    for (size_t i = 0; i < order.size(); ++i)
        if (order[i] == h) return static_cast<int>(i);
    return static_cast<int>(order.size());
}

// Last round each enemy was read, and whether it had acted by our turn then.
struct DoneMem { std::uint32_t h; int round; bool done; };
std::vector<DoneMem> g_done_mem;
int g_ties_even = -1;  // 1: our team goes first at equal index on even rounds, 0: on odd rounds, -1: not seen yet

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
                    f.reset_ready = true;
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
                if (ready) {
                    f.burst += d;
                    f.spell_ready = std::max(f.spell_ready, d);
                    f.spell_range = std::max(f.spell_range, static_cast<int>(ability_range(ab)));
                }
                if (ready && ab.is_cc()) f.stun = true;
            }
        }
        f.sustain = f.str * (reset ? 9.0f / 8.0f : 1.0f) + spell / 8.0f;
        if (c.champion_name() == cambc::champion_type::heart_of_ruin) {
            const float bonus = 1.0f + 0.5f * (1.0f - f.hp / f.max_hp);
            f.sustain *= bonus;
            f.burst *= bonus;
        }
        try {
            const auto b = c.buffs();
            f.stunned = b.has_stun();
            if (b.has_shield()) f.hp += b.get<cambc::buff_shield>().value;
        } catch (const std::exception&) {}
        try { f.channel = c.is_recalling() || c.is_submitting_gems(); } catch (const std::exception&) {}
        try { f.gems = static_cast<int>(c.gem_count()); } catch (const std::exception&) { f.gems = 0; }
        try { f.done = c.has_completed_turn(); } catch (const std::exception&) { f.done = false; }
        f.value = 150.0f + 2.0f * static_cast<float>(f.gems);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

struct SimResult {
    float value = 0;        // kills minus deaths (by what they are worth) plus health traded, from our side
    bool me_dies = false;
    int our_deaths = 0, their_deaths = 0;
};

// The fight action by action in turn order. Round 0 is the rest of this round: us now, then everyone who
// has not acted yet; later rounds run by turn index, the team that goes first at equal index alternating.
// A fighter walks for `delay` actions before it can hit (its first hit adds its burst; a ready stun then
// takes the strongest opponent's next action), aims at the weakest opponent, takes its camp fire each
// action and is healed to full on its spawn.
SimResult simulate(const std::vector<Fighter>& a, const std::vector<Fighter>& b, bool we_first_ties, bool partial = true) {
    // Flat copies (units 0..na-1 ours, then theirs): a local loop costs fuel per element read.
    struct U { float hp, max_hp, hit, burst, camp; int delay, acted, side; bool skip, stun, safe; };
    const int na = static_cast<int>(std::min<size_t>(a.size(), 8)), nb = static_cast<int>(std::min<size_t>(b.size(), 8));
    const int n = na + nb;
    std::array<U, 16> u;
    std::array<int, 16> idx{};
    std::array<bool, 16> done{};
    for (int i = 0; i < n; ++i) {
        const Fighter& f = i < na ? a[static_cast<size_t>(i)] : b[static_cast<size_t>(i - na)];
        u[static_cast<size_t>(i)] = {f.hp, f.max_hp, f.sustain, f.burst, f.camp_in, f.delay, 0, i < na ? 0 : 1, f.stunned, f.stun, f.safe};
        idx[static_cast<size_t>(i)] = std::min(f.idx, 8);
        done[static_cast<size_t>(i)] = f.done;
    }
    // Action order of a full round, for either team first at equal index.
    std::array<std::array<int, 16>, 2> order{};
    for (int first = 0; first < 2; ++first) {
        int k = 0;
        for (int x = 0; x <= 8; ++x)
            for (int pass = 0; pass < 2; ++pass) {
                const int side = pass == 0 ? first : 1 - first;
                for (int i = 0; i < n; ++i)
                    if (u[static_cast<size_t>(i)].side == side && idx[static_cast<size_t>(i)] == x) order[static_cast<size_t>(first)][static_cast<size_t>(k++)] = i;
            }
    }
    int alive_side[2] = {na, nb};
    auto act = [&](int i) {
        U& f = u[static_cast<size_t>(i)];
        if (f.hp <= 0) return;
        if (f.skip) { f.skip = false; return; }
        if (f.acted >= f.delay) {
            const int lo = f.side == 0 ? na : 0, hi = f.side == 0 ? n : na;
            int k = -1;
            float kh = 1e30f;
            for (int j = lo; j < hi; ++j) {
                const float h = u[static_cast<size_t>(j)].hp;
                if (h > 0 && h < kh) { kh = h; k = j; }
            }
            if (k >= 0) {
                const bool first = f.acted == f.delay;
                U& t = u[static_cast<size_t>(k)];
                t.hp -= f.hit + (first ? f.burst : 0.0f);
                if (t.hp <= 0.5f) { t.hp = 0; --alive_side[t.side]; }
                if (first && f.stun) {
                    int s = -1;
                    for (int j = lo; j < hi; ++j)
                        if (u[static_cast<size_t>(j)].hp > 0 && (s < 0 || u[static_cast<size_t>(j)].hit > u[static_cast<size_t>(s)].hit)) s = j;
                    if (s >= 0) u[static_cast<size_t>(s)].skip = true;
                }
            }
        }
        ++f.acted;
        if (f.camp > 0) {
            f.hp -= f.camp;
            if (f.hp <= 0) { f.hp = 0; --alive_side[f.side]; return; }
        }
        if (f.safe) f.hp = f.max_hp;
    };
    // Round 0: us, then whoever has not acted yet (or, not partial: a full round from the start).
    const int f0 = we_first_ties ? 0 : 1;
    if (partial) {
        act(0);
        for (int k = 0; k < n; ++k) {
            const int i = order[static_cast<size_t>(f0)][static_cast<size_t>(k)];
            if (i != 0 && !done[static_cast<size_t>(i)]) act(i);
        }
    } else {
        for (int k = 0; k < n; ++k) act(order[static_cast<size_t>(f0)][static_cast<size_t>(k)]);
    }
    int first = 1 - f0;
    for (int t = 1; t < kHorizon && alive_side[0] > 0 && alive_side[1] > 0; ++t) {
        const auto& o = order[static_cast<size_t>(first)];
        for (int k = 0; k < n; ++k) act(o[static_cast<size_t>(k)]);
        first = 1 - first;
    }
    SimResult r;
    float lost_a = 0, lost_b = 0;
    for (int i = 0; i < n; ++i) {
        const Fighter& f = i < na ? a[static_cast<size_t>(i)] : b[static_cast<size_t>(i - na)];
        const float lost = (f.hp - u[static_cast<size_t>(i)].hp) / f.max_hp;
        const bool dead = u[static_cast<size_t>(i)].hp <= 0;
        if (i < na) {
            lost_a += lost;
            if (dead) { ++r.our_deaths; r.value -= f.value; if (i == 0) r.me_dies = true; }
        } else {
            lost_b += lost;
            if (dead) { ++r.their_deaths; r.value += f.value; }
        }
    }
    // Damage that does not kill still sends them home (and us).
    r.value += 60.0f * (lost_b - lost_a);
    return r;
}

// The parent's model, kept for comparison: whole rounds resolved at once, each side pouring its damage
// into its weakest living opponent (overflow moves on); first-turn burst and stun as above.
SimResult simulate_rounds(const std::vector<Fighter>& a, const std::vector<Fighter>& b) {
    const size_t na = std::min<size_t>(a.size(), 8), nb = std::min<size_t>(b.size(), 8);
    std::array<float, 8> ha{}, hb{};
    std::array<int, 8> stun_a{}, stun_b{};
    for (size_t i = 0; i < na; ++i) { ha[i] = a[i].hp; stun_a[i] = a[i].stunned ? a[i].delay : -1; }
    for (size_t i = 0; i < nb; ++i) { hb[i] = b[i].hp; stun_b[i] = b[i].stunned ? b[i].delay : -1; }
    auto pour = [](float dmg, std::array<float, 8>& h, size_t n) {
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
    auto strongest = [](const std::vector<Fighter>& side, const std::array<float, 8>& h, size_t n) {
        int k = -1;
        for (size_t i = 0; i < n; ++i)
            if (h[i] > 0 && (k < 0 || side[i].sustain > side[static_cast<size_t>(k)].sustain)) k = static_cast<int>(i);
        return k;
    };
    for (int t = 0; t < kHorizon; ++t) {
        float da = 0, db = 0;
        for (size_t i = 0; i < na; ++i) {
            if (ha[i] <= 0 || a[i].delay > t || stun_a[i] == t) continue;
            da += a[i].sustain + (t == a[i].delay ? a[i].burst : 0.0f);
            if (t == a[i].delay && a[i].stun) {
                const int k = strongest(b, hb, nb);
                if (k >= 0) stun_b[static_cast<size_t>(k)] = std::max(t + 1, b[static_cast<size_t>(k)].delay);
            }
        }
        for (size_t i = 0; i < nb; ++i) {
            if (hb[i] <= 0 || b[i].delay > t || stun_b[i] == t) continue;
            db += b[i].sustain + (t == b[i].delay ? b[i].burst : 0.0f);
            if (t == b[i].delay && b[i].stun) {
                const int k = strongest(a, ha, na);
                if (k >= 0) stun_a[static_cast<size_t>(k)] = std::max(t + 1, a[static_cast<size_t>(k)].delay);
            }
        }
        pour(da, hb, nb);
        pour(db, ha, na);
        bool any_a = false, any_b = false;
        for (size_t i = 0; i < na; ++i) {
            if (ha[i] <= 0) continue;
            ha[i] -= a[i].camp_in;
            if (ha[i] <= 0) { ha[i] = 0; continue; }
            if (a[i].safe) ha[i] = a[i].max_hp;
            any_a = true;
        }
        for (size_t i = 0; i < nb; ++i) {
            if (hb[i] <= 0) continue;
            hb[i] -= b[i].camp_in;
            if (hb[i] <= 0) { hb[i] = 0; continue; }
            if (b[i].safe) hb[i] = b[i].max_hp;
            any_b = true;
        }
        if (!any_a || !any_b) break;
    }
    SimResult r;
    float lost_a = 0, lost_b = 0;
    for (size_t i = 0; i < na; ++i) {
        lost_a += (a[i].hp - ha[i]) / a[i].max_hp;
        if (ha[i] <= 0) { ++r.our_deaths; r.value -= a[i].value; if (i == 0) r.me_dies = true; }
    }
    for (size_t i = 0; i < nb; ++i) {
        lost_b += (b[i].hp - hb[i]) / b[i].max_hp;
        if (hb[i] <= 0) { ++r.their_deaths; r.value += b[i].value; }
    }
    r.value += 60.0f * (lost_b - lost_a);
    return r;
}

// Damage one enemy deals to a champion standing on t before our next turn, given its actions until then.
float hits_on(const Fighter& e, vec2 t) {
    int acts = e.acts - (e.stunned ? 1 : 0);
    if (acts <= 0) return 0;
    int d = cheb(e.pos, t);
    float dmg = 0;
    bool first = true, dash = e.dasher;
    for (int k = 0; k < acts; ++k) {
        const float hit = e.str + (first ? e.burst * 0.5f : 0.0f);
        if (e.ranged) {
            // Shoot from where it stands, or walk (dash) closer and shoot only with a reset in hand.
            if (d <= e.range) { dmg += hit; first = false; continue; }
            if (e.spell_ready > 0 && first && d <= e.spell_range) { dmg += e.spell_ready; first = false; continue; }
            int step = 1;
            if (dash && d - e.range >= 2) { step = 3; dash = false; }
            d = std::max(1, d - step);
            if (d <= e.range && first && e.reset_ready) { dmg += hit; first = false; }
        } else {
            if (d > 1) {
                int step = 1;
                if (dash && d >= 3) { step = 3; dash = false; }
                d = std::max(1, d - step);
            }
            if (d <= 1) { dmg += hit; first = false; }
            else if (first && e.spell_ready > 0 && d <= e.spell_range) { dmg += e.spell_ready; first = false; }
        }
    }
    return dmg;
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

// Damage the enemies can put on tile t before our next turn.
float Bot::threat_at(vec2 t, const std::vector<Fighter>& theirs) const {
    float th = 0;
    if (!kTurnOrder) {
        // The parent's: one action each (melee step and hit, ranged from where it stands).
        for (const auto& e : theirs) {
            if (e.stunned) continue;
            const int d = cheb(e.pos, t);
            const int reach = e.ranged ? e.range : e.range + 1 + (e.dasher ? 2 : 0);
            if (d <= reach) th += e.str + e.burst * 0.5f;
            else if (d <= 4 && e.burst > e.str) th += (e.burst - e.str) * 0.5f;
        }
        return th;
    }
    for (const auto& e : theirs)
        if (e.hp > 0) th += hits_on(e, t);
    return th;
}

// Damage awake camps deal to a champion on t each turn (they hit the nearest champion in reach; an ally
// nearer to the camp takes it instead).
float Bot::camp_heat(vec2 t) const {
    float s = 0;
    for (const auto& h : hot_) {
        const int d = cheb(h.first, t);
        if (d > h.second.range) continue;
        bool shielded = false;
        for (auto a : view().allies)
            if (cheb(a, h.first) < d) { shielded = true; break; }
        if (!shielded) s += h.second.str * taken_;
    }
    return s;
}

// One engaged step. For staying and each free neighbouring tile: the damage we deal now (a ranged champion
// only when it stays, or with a reset in hand), against lambda times the hits we take there before our next
// turn. Against an enemy that does not act before our next turn (we act twice in a row) the next action is
// planned too: the tile we end on then faces its two actions in a row. Otherwise the turn after counts at
// half weight (enemies about to act are assumed to walk at us). So we shoot while nobody can answer, stand
// just outside the reach of an enemy about to act twice, and let it walk into ours.
// False when nobody is within a few steps (the route search walks us in).
bool Bot::fight_move(std::vector<Fighter>& theirs, float lambda) {
    auto& me = cambc::self();
    if (!live()) return false;
    const int range = me.attack_range();
    const bool ranged = range > 1;
    const float str = me.strength();
    bool reset_ready = false;
    if (!cast_ && me.can_cast())
        for (const auto& ab : me.abilities())
            if (ab.is_attack_reset() && ab.is_ready()) reset_ready = true;
    const bool can_attack = me.can_attack();
    const bool can_move = !moved_ && !move_failed_ && me.can_move();
    const auto& v = view();
    bool near = false, doubled = false;
    for (const auto& e : theirs) {
        if (e.hp <= 0) continue;
        const int d = cheb(e.pos, pos_);
        near = near || d <= range + 3;
        doubled = doubled || (e.acts == 0 && d <= 7);
    }
    if (!near && can_move) return false;
    // Only duels with our counterpart (one enemy near, acting twice in a row or not at all) are played
    // move by move; elsewhere (single actions both ways, several enemies) walk in and strike.
    int close = 0;
    bool waits = false;  // the enemy acts twice before our next turn
    for (const auto& e : theirs)
        if (e.hp > 0 && cheb(e.pos, pos_) <= 7) {
            ++close;
            waits = e.acts >= 2;
        }
    if (kDuelsOnly && close != 1) return false;
    if (!doubled && !(waits && threat_at(pos_, theirs) <= 0)) return false;
    // The enemies as they stand after our next action: those that wait now act twice then.
    std::vector<Fighter> later;
    if (doubled) {
        later = theirs;
        for (auto& e : later) e.acts = e.acts == 0 ? 2 : 1;
    }
    auto hit_value = [&](const Fighter& e, float dmg) { return e.hp <= dmg ? dmg + 300.0f + e.value : dmg; };
    // Our damage from t this action (moved: we walked there).
    auto dmg_at = [&](vec2 t, bool moved, bool reset) {
        float best = 0;
        for (const auto& e : theirs) {
            if (e.hp <= 0 || cheb(e.pos, t) > range || !v.lvis.at(e.pos)) continue;
            if (ranged && moved && !reset) continue;
            const float dmg = str + (reset ? str : 0.0f);
            best = std::max(best, hit_value(e, dmg));
        }
        return best;
    };
    auto score = [&](vec2 t, bool stay) {
        const float now_dmg = can_attack ? dmg_at(t, !stay, reset_ready) : 0.0f;
        const float taken = threat_at(t, theirs) + camp_heat(t);
        int dnear = 99;
        for (const auto& e : theirs)
            if (e.hp > 0) dnear = std::min(dnear, cheb(e.pos, t));
        float s = now_dmg - lambda * taken - 0.05f * static_cast<float>(dnear);
        if (doubled) {
            // Our next action from t (reset spent if used now), then their double.
            const bool reset2 = reset_ready && now_dmg <= 0;
            float best2 = -1e9f;
            auto try2 = [&](vec2 t2, bool moved2) {
                const float d2 = dmg_at(t2, moved2, reset2);
                const float v2 = d2 - lambda * 0.75f * (threat_at(t2, later) + camp_heat(t2));
                best2 = std::max(best2, v2);
            };
            try2(t, false);
            for (auto d : kDirs) {
                if (!fuel(30000)) break;
                const vec2 t2{t.x + d.x, t.y + d.y};
                if (t2 != pos_ && !free_tile(t2)) continue;
                if (camp_block_.is_set(t2)) continue;
                try2(t2, true);
            }
            s += best2;
        } else {
            float next_dmg = 0;
            for (const auto& e : theirs) {
                if (e.hp <= 0) continue;
                const int dn = std::max(1, cheb(e.pos, t) - (e.channel ? 0 : std::min(e.acts, 1)));
                if (dn <= (ranged ? range : 2)) next_dmg = std::max(next_dmg, str);
            }
            s += 0.5f * next_dmg;
        }
        return s;
    };
    vec2 best = pos_;
    float best_s = score(pos_, true);
    if (can_move)
        for (auto d : kDirs) {
            if (!fuel(30000)) break;
            const vec2 t{pos_.x + d.x, pos_.y + d.y};
            if (!free_tile(t) || camp_block_.is_set(t)) continue;
            const float s = score(t, false);
            if (s > best_s + 0.01f) { best_s = s; best = t; }
        }
    if (!live()) return true;
    if (best == pos_) {
        if (strike(theirs)) chase_turns_ = 0;
        return true;
    }
    moved_ = true;
    bool ok = false;
    try { ok = me.move_absolute(best); } catch (const cambc::error& e) { warn("fight move", e); }
    invalidate();
    if (!ok) { move_failed_ = true; return true; }
    pos_ = me.position();
    if (!live()) return true;
    if (me.can_attack()) {
        if (strike(theirs)) chase_turns_ = 0;
    } else if (ranged && reset_ready && !cast_ && me.can_cast() && !attacked_) {
        bool in_range = false;
        for (const auto& e : theirs)
            if (e.hp > 0 && cheb(e.pos, pos_) <= range && view().lvis.at(e.pos)) in_range = true;
        if (in_range)
            for (const auto& ab : me.abilities())
                if (ab.is_attack_reset() && ab.is_ready()) {
                    try { ab.use(); cast_ = true; } catch (const cambc::error& ex) { warn("walk reset", ex); break; }
                    if (me.can_attack() && strike(theirs)) chase_turns_ = 0;
                    break;
                }
    }
    return true;
}

// After shooting from here, can we still step out of reach next turn? Enemies that do not act before our
// next turn act twice right after it, so the shot is only free if a neighbouring tile is safe from that.
bool Bot::can_slip_away(const std::vector<Fighter>& theirs) const {
    if (!kTurnOrder) return true;
    bool any = false;
    std::vector<Fighter> later = theirs;
    for (auto& e : later) {
        if (e.hp > 0 && e.acts == 0) any = true;
        e.acts = e.acts == 0 ? 2 : 1;
    }
    if (!any) return true;
    if (threat_at(pos_, later) <= 0) return true;
    for (auto d : kDirs) {
        const vec2 t{pos_.x + d.x, pos_.y + d.y};
        if (!free_tile(t) || camp_block_.is_set(t)) continue;
        if (threat_at(t, later) <= 0) return true;
    }
    return false;
}

// Run from the enemies: the free neighbouring tile that takes the fewest hits before our next turn, nearer
// home on ties, a dash first when it gets us out of reach. Recall once no enemy can reach us soon.
bool Bot::retreat(std::vector<Fighter>& theirs, int dmin) {
    auto& me = cambc::self();
    const bool on_spawn = spawn_local_.at(pos_);
    if (on_spawn) {
        // The spawn heals us every turn and they cannot step on it: hold it and shoot from there.
        if (++spawn_hold_ > 20) return false;
        strike(theirs);
        return true;
    }
    spawn_hold_ = 0;
    // Out of reach of everyone for a few turns: recall (damage would cancel it).
    if (dmin >= 7 && !moved_ && incoming_ == 0 && live() && home_.at(pos_) > 6) {
        bool safe = true;
        for (const auto& e : theirs)
            if (kNewRetreat && e.hp > 0 && cheb(e.pos, pos_) <= 5 + e.range + (e.dasher ? 2 : 0)) safe = false;
        if (safe) {
            try { me.recall(); } catch (const cambc::error& e) { warn("flee recall", e); }
            return true;
        }
    }
    // Melee: a free swing at an adjacent enemy before stepping away (the cast stays free for a dash).
    if (me.attack_range() <= 1 && me.can_attack() && live())
        for (const auto& e : theirs)
            if (e.hp > 0 && cheb(e.pos, pos_) <= 1 && view().lvis.at(e.pos)) {
                try {
                    me.attack(cambc::vision::champion_at(e.pos));
                    attacked_ = true;
                    invalidate();
                } catch (const cambc::error& ex) { warn("swing", ex); }
                break;
            }
    // Ranged: a free shot when nobody can hit us here before our next turn.
    if (kFreeShots && me.attack_range() > 1 && me.can_attack() && !moved_ && incoming_ == 0 && live()
        && threat_at(pos_, theirs) <= 0 && can_slip_away(theirs) && strike(theirs))
        return true;
    if (moved_ || move_failed_ || !me.can_move() || !live()) return true;
    const Bits& sp = spawn_local_;
    // Where the enemies could stand next turn decides the turn after: keep distance from melee as well.
    // Enemies that do not act before our next turn act twice after it: keep away from them too.
    std::vector<Fighter> later;
    if (kTurnOrder)
        for (const auto& e : theirs)
            if (e.hp > 0 && e.acts == 0) {
                later = theirs;
                for (auto& x : later) x.acts = x.acts == 0 ? 2 : 0;
                break;
            }
    auto score = [&](vec2 t) {
        float s = threat_at(t, theirs) + camp_heat(t);
        if (!later.empty()) s += 0.5f * threat_at(t, later);
        s *= 4.0f;
        if (sp.at(t)) s *= 0.1f;
        const int hd = home_.at(t);
        s += static_cast<float>(hd >= kFar ? 200 : hd);
        for (const auto& e : theirs)
            if (kNewRetreat && e.hp > 0 && !e.ranged && cheb(e.pos, t) <= 2) s += 3.0f;
        if (camp_block_.is_set(t)) s += 500;
        return s;
    };
    const float here = score(pos_);
    // Dash out when one step does not escape.
    if (dasher_ && !cast_ && me.can_cast() && fuel(15000) && threat_at(pos_, theirs) > 0) {
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
    // A turn that ran out of fuel above resumes next round: the world has moved on.
    if (best == pos_ || !live() || !me.can_move()) return true;
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
    // A kill first (worth most), then whoever dies soonest to our team, weighted by what it is worth and by
    // how much it hurts us before our next turn.
    auto pick = [&]() -> int {
        int best = -1;
        float bs = -1e9f;
        for (size_t i = 0; i < theirs.size(); ++i) {
            const auto& e = theirs[i];
            if (e.hp <= 0 || cheb(e.pos, at) > range || !view().lvis.at(e.pos)) continue;
            float s = -e.hp / std::max(1.0f, e.sustain);
            if (e.hp <= str * 0.95f) s += 1000 + e.value;
            if (e.channel) s += 50;
            s += static_cast<float>(e.gems) * 0.5f;
            if (kNewStrike && e.acts > 0) s += 2.0f;
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
    if (!cast_ && me.can_cast() && fuel(8000) && live())
        for (const auto& ab : me.abilities())
            if (ab.is_attack_reset() && ab.is_ready()) {
                if (me.can_attack()) {
                    // A kill gave the attack back already (Vendetta): just hit again.
                    const int next = pick();
                    if (next >= 0) hit(next);
                    break;
                }
                if (kNewStrike && pick() < 0) break;  // nothing left in range: keep the reset
                try { ab.use(); cast_ = true; } catch (const cambc::error& ex) { warn("reset", ex); break; }
                if (me.can_attack()) {
                    const int next = pick();
                    if (next >= 0) hit(next);
                }
                break;
            }
    return true;
}

// Unit spells (damage, stun) on the best enemy in their range; a shield when they can hit us. Cast
// before walking: most of these exclude it.
void Bot::fight_spells(std::vector<Fighter>& theirs, bool engage) {
    auto& me = cambc::self();
    if (cast_ || !me.can_cast() || !live()) return;
    const float threat_here = threat_at(pos_, theirs);
    for (const auto& ab : me.abilities()) {
        if (cast_ || !ab.is_ready()) continue;
        const bool exclusive = excludes_walking(ab);
        if (exclusive && moved_) continue;
        if (ab.is_shield()) {
            if (threat_here > 0 || incoming_ > 0) {
                try { ab.use(); cast_ = true; } catch (const cambc::error& e) { warn("shield", e); }
            }
            continue;
        }
        const bool unit_spell = (ab.is_damage() || ab.is_cc()) && ab.can_target_enemies();
        const bool area_spell = ab.is_aoe_damage() && ab.can_target_ground();
        if (!unit_spell && !area_spell) continue;
        const int r = static_cast<int>(ability_range(ab));
        float dmg = 0;
        try { dmg = ab.damage_amount(); } catch (const std::exception&) {}
        int best = -1;
        float bs = -1e9f;
        for (size_t i = 0; i < theirs.size(); ++i) {
            const auto& e = theirs[i];
            if (e.hp <= 0 || cheb(e.pos, pos_) > r || !view().lvis.at(e.pos)) continue;
            float s = std::min(dmg, e.hp);
            if (e.hp <= dmg) s += 1000 + e.value;
            if (ab.is_cc()) {
                if (e.channel) s += 400;            // a stun cancels a tribute or a recall
                if (e.stunned) s -= 300;
                // A stun takes away its next action: worth most on one about to act, near us.
                s += kNewSpells ? e.sustain * static_cast<float>(std::max(0, e.acts)) * (hits_on(e, pos_) > 0 ? 1.5f : 0.5f) : e.sustain;
            }
            if (area_spell)
                for (const auto& o : theirs)
                    if (&o != &e && o.hp > 0 && cheb(o.pos, e.pos) <= 1) s += std::min(dmg, o.hp);
            if (s > bs) { bs = s; best = static_cast<int>(i); }
        }
        if (best < 0) continue;
        auto& e = theirs[static_cast<size_t>(best)];
        // Retreating: only a kill, a stun on a chaser about to act or a cancelled channel is worth the walk.
        const bool chaser = kNewSpells ? hits_on(e, pos_) > 0 : cheb(e.pos, pos_) <= 3;
        if (!engage && exclusive && !(e.hp <= dmg || (ab.is_cc() && chaser) || e.channel)) continue;
        // Engaged: a spell that roots us waits while we still have to walk to reach anyone.
        if (kNewSpells && engage && exclusive && me.attack_range() <= 1 && e.hp > dmg && !e.channel) {
            bool adjacent = false;
            for (const auto& o : theirs) adjacent = adjacent || (o.hp > 0 && cheb(o.pos, pos_) <= 1);
            if (!adjacent && !(ab.is_cc() && hits_on(e, pos_) > 0)) continue;
        }
        try {
            if (unit_spell) {
                if (!cambc::vision::has_champion(e.pos)) continue;
                ab.use(cambc::vision::champion_at(e.pos));
            } else {
                ab.use(e.pos);
            }
            cast_ = true;
            if (exclusive) move_failed_ = true;  // no walking after this cast
            e.hp -= dmg;
            if (ab.is_cc()) { e.stunned = true; e.acts = std::max(0, e.acts - 1); }
            invalidate();
        } catch (const cambc::error& ex) { warn("enemy spell", ex); }
    }
}

// True when the turn is spent.
bool Bot::enemies() {
    auto& me = cambc::self();
    const int now = static_cast<int>(cambc::round_number());
    const auto& enemy_spawn = cambc::map::spawn_region(team::enemy);
    if (g_ally_order.empty()) {
        for (const auto& c : cambc::champions(team::ally)) g_ally_order.push_back(handle_of(c));
        for (const auto& c : cambc::champions(team::enemy)) g_enemy_order.push_back(handle_of(c));
    }
    const int my_idx = order_of(g_ally_order, handle_of(me));
    std::vector<Fighter> theirs, ours;
    // Us first (index 0), then allies near enough to join.
    {
        Fighter f;
        if (!read_fighter(me, pos_, f)) return false;
        f.safe = spawn_local_.at(pos_);
        f.idx = my_idx;
        f.done = false;
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
                const std::uint32_t h = handle_of(c);
                f.idx = order_of(g_enemy_order, h);
                // Our counterpart alternates (twice in a row, then not at all); others act once between our turns.
                bool alternating = f.idx == my_idx;
                for (const auto& m : g_done_mem)
                    if (m.h == h && m.round == now - 1) alternating = m.done != f.done;
                f.acts = !kTurnOrder ? 1 : alternating ? (f.done ? 0 : 2) : 1;
                if (alternating) g_ties_even = ((now % 2 == 0) == !f.done) ? 1 : 0;
                bool found = false;
                for (auto& m : g_done_mem)
                    if (m.h == h) { m.round = now; m.done = f.done; found = true; }
                if (!found) g_done_mem.push_back({h, now, f.done});
                theirs.push_back(f);
                dmin = std::min(dmin, d);
            } catch (const std::exception&) {}
            if (theirs.size() >= 8) break;
        }
    // Our team goes first at equal index this round (round parity, once seen).
    const bool we_first_ties = g_ties_even < 0 || (now % 2 == 0) == (g_ties_even == 1);
    if (theirs.empty()) {
        // Help an ally who reported an invader close by.
        const auto s = read_sighting();
        if (s.ok && s.age <= 4 && cheb(s.where, pos_) <= 14 && me.health() > me.max_health() * 0.6f
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
            f.idx = order_of(g_ally_order, handle_of(a));
            ours.push_back(f);
            if (ours.size() >= 8) break;
        } catch (const std::exception&) {}
    hot_.clear();
    if (!awake_.is_empty())
        for (auto p : awake_.positions()) {
            if (cheb(p, pos_) > 8 || hot_.size() >= 16) continue;
            const int gi = group_at_[static_cast<size_t>(tidx(p))];
            const int ci = camp_at_[static_cast<size_t>(tidx(p))];
            if (gi < 0 || ci < 0) continue;
            const auto cs = camp_spec(g_[gi].kinds[static_cast<size_t>(ci)]);
            if (cs.str > 0) hot_.push_back({p, cs});
        }
    // Actions before each fighter can hit: walk to its range of the nearest opponent (melee hits on arrival).
    auto set_delay = [](Fighter& f, const std::vector<Fighter>& other) {
        int d = 99;
        for (const auto& o : other) d = std::min(d, cheb(o.pos, f.pos));
        f.delay = std::max(0, d - f.range - (f.ranged ? 0 : 1));
    };
    ally_near_.clear();
    for (size_t i = 1; i < ours.size(); ++i) ally_near_.push_back(ours[i].pos);
    for (auto& f : ours) set_delay(f, theirs);
    for (auto& f : theirs) set_delay(f, ours);
    ours[0].camp_in = 0;
    camp_fire(ours, theirs);

    // Who acts first at equal index flips every round and decides mirror duels: weigh the fight over both
    // orders, or a duel would look won and lost on alternate rounds (engage, retreat, engage...).
    SimResult fight;
    if (!kSeqSim) {
        fight = simulate_rounds(ours, theirs);
    } else if (kNeutralSim) {
        const SimResult x = simulate(ours, theirs, true, false), y = simulate(ours, theirs, false, false);
        fight.value = 0.5f * (x.value + y.value);
        fight.me_dies = x.me_dies && y.me_dies;
        fight.their_deaths = std::min(x.their_deaths, y.their_deaths);
        fight.our_deaths = std::max(x.our_deaths, y.our_deaths);
    } else {
        fight = simulate(ours, theirs, we_first_ties);
    }
    const float hp = me.health();
    const int gems = static_cast<int>(me.gem_count());
    // Running instead: the hits we take before our next turn, then the chasers that keep up with us all the
    // way home (melee within two steps, or a dash away) while ranged ones fall behind after a shot or two.
    float run_dmg = 0;
    if (!ours[0].safe) {
        const int home = std::min(kRunCap, home_.at(pos_) >= kFar ? kRunCap : home_.at(pos_));
        for (const auto& e : theirs) {
            if (e.hp <= 0) continue;
            const int d = cheb(e.pos, pos_);
            run_dmg += hits_on(e, pos_);
            if (!e.ranged && d <= 2 + (e.dasher ? 2 : 0)) run_dmg += kChaseShare * e.sustain * static_cast<float>(std::max(0, home - 1));
            else if (e.ranged && e.dasher && d <= e.range + 2) run_dmg += e.str;
        }
    }
    const bool run_dies = run_dmg >= hp;
    const float run_value = !kRunCost ? 0.0f : run_dies ? -ours[0].value : -60.0f * run_dmg / ours[0].max_hp;
    // Kill on the spot: an enemy our burst finishes this turn without moving.
    bool kill_now = false;
    {
        const float mine = ours[0].str + ours[0].burst;
        const int reach = ours[0].ranged ? ours[0].range : 2;
        for (const auto& e : theirs)
            if (cheb(e.pos, pos_) <= reach && e.hp <= mine * 0.85f && view().lvis.at(e.pos)) kill_now = true;
    }
    bool engage = fight.value - run_value > kEngageBias && !(fight.me_dies && fight.their_deaths == 0 && !(kRunCost && run_dies));
    // Carrying a big bag: only clearly winning fights, unless running loses it anyway.
    if (engage && fight.me_dies && !(kRunCost && run_dies) && gems >= 40 && fight.value < 2.0f * static_cast<float>(gems)) engage = false;
    if (!engage && kill_now && !(fight.me_dies && gems >= 60 && !(kRunCost && run_dies))) engage = true;
    // The spawn's heal wins this fight, not us: hold it rather than walk out.
    bool hold_spawn = false;
    if (engage && ours[0].safe) {
        auto off = ours;
        off[0].safe = false;
        const SimResult out = kSeqSim ? simulate(off, theirs, we_first_ties, !kNeutralSim) : simulate_rounds(off, theirs);
        hold_spawn = !(out.value > kEngageBias && !(out.me_dies && out.their_deaths == 0));
    }
    if (kTraceFight) {
        std::string line = "fight r" + std::to_string(now) + ' ' + cambc::to_string(me.champion_name()) + " v" + std::to_string(static_cast<int>(fight.value))
            + " die" + std::to_string(fight.me_dies) + " k" + std::to_string(fight.their_deaths) + " d" + std::to_string(fight.our_deaths)
            + " run" + std::to_string(static_cast<int>(run_dmg)) + " E" + std::to_string(engage) + " |";
        auto add = [&](const Fighter& f) {
            line += ' ' + std::to_string(f.pos.x) + ',' + std::to_string(f.pos.y) + ':' + std::to_string(static_cast<int>(f.hp)) + '/'
                + std::to_string(static_cast<int>(f.sustain)) + '+' + std::to_string(static_cast<int>(f.burst)) + '@' + std::to_string(f.delay)
                + 'a' + std::to_string(f.acts) + (f.camp_in > 0 ? "c" : "") + (f.safe ? "s" : "");
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
            if (cheb(e.pos, pos_) <= static_cast<int>(e.threat_reach()) + 3) reachable = true;
        if (!reachable && hp > me.max_health() * 0.5f) return false;
        fight_spells(theirs, false);
        return retreat(theirs, dmin);
    }
    // Strong enough but nobody close: keep farming unless they are worth a short hunt.
    if (dmin > me.attack_range() + 3 && fight.their_deaths == 0) return false;
    if (now < no_chase_until_ && dmin > me.attack_range() + 1) return false;
    if (!live()) return true;
    fight_spells(theirs, true);
    if (hold_spawn) {
        strike(theirs);
        return ++spawn_hold_ <= 20;
    }
    const int range = me.attack_range();
    if (attacked_ || move_failed_ || (cast_ && range > 1)) {
        if (!attacked_ && me.can_attack()) strike(theirs);
        return true;
    }
    if (++chase_turns_ > kChaseTurns) {
        chase_turns_ = 0;
        no_chase_until_ = now + 15;
        return false;
    }
    // Winning clearly: trade blows freely; otherwise only where we hit at least as hard as we are hit.
    const float lambda = (fight.their_deaths > 0 && !fight.me_dies) ? kTradeWinning : kTradeEven;
    if (kDuelTactics && kTurnOrder && fight_move(theirs, lambda)) return true;
    if (strike(theirs)) { chase_turns_ = 0; return true; }
    // Approach: tiles in basic range of the enemies.
    auto goal = empty_board();
    for (const auto& e : theirs)
        if (e.hp > 0) goal.set(e.pos);
    goal = grow(goal, range) & ~walk_walls();
    go(goal, dmin > range + 1);
    pos_ = me.position();
    // A melee step that arrived, or a ranged step with a reset in hand, still hits this turn.
    if (live() && me.can_attack()) {
        if (strike(theirs)) chase_turns_ = 0;
    } else if (live() && range > 1 && !cast_ && me.can_cast() && !attacked_) {
        bool in_range = false;
        for (const auto& e : theirs)
            if (e.hp > 0 && cheb(e.pos, pos_) <= range && view().lvis.at(e.pos)) in_range = true;
        if (in_range)
            for (const auto& ab : me.abilities())
                if (ab.is_attack_reset() && ab.is_ready()) {
                    try { ab.use(); cast_ = true; } catch (const cambc::error& ex) { warn("walk reset", ex); break; }
                    if (me.can_attack() && strike(theirs)) chase_turns_ = 0;
                    break;
                }
    }
    return true;
}
