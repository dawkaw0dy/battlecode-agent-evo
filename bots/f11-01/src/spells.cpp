// The spell subsystem: which ability slot, on which target, at what moment.
//
// Rules it is built on (GameRules.md + patches 100-102): one successful cast per turn across both slots,
// an 8-turn cooldown per slot, unit spells need a visible target within Chebyshev range 4, "excludes
// walking" spells cannot share a turn with a step (cast first, then stand), an attack reset only works
// right after our own basic attack (error 26 otherwise), stuns take the target's next turn (camps
// included) and cancel channels (tribute, recall, teleport), damage cancels a recall.
//
// Our kits: Vesper Vale and Vendetta (reset, dash), Marcella (enhanced reset that stuns, 2 x Focus
// nuke), Tavi (reset, 1.4 x Focus nuke + stun), Varg (reset, 2.5 x Focus self-shield for 5 turns).
//
// Camps: a nuke goes where it removes the most incoming damage (the most threat per health, a kill, a
// stun on a camp that would hit one of us next round); an exclusive nuke is cast only from a tile we
// would not leave this turn anyway. A reset is spent only when a target is left to hit after it.
// Fights: kills first (alone or with this turn's basic hit), nukes on the target the basic attacks
// focus, stuns on whoever would hurt us most next turn (or is channelling), Marcella's stunning hit
// before her nuke, the shield before blows land. Holding nukes or the dash for fights (kHoldRounds,
// kDashHoldRounds) is wired in but off: it measured no gain.
#include <cambc.h>
#include "bot.h"
#include "fuel.h"
#include <algorithm>
#include <array>
#include <iostream>
#include <variant>

namespace {

constexpr int kHoldRounds = 0;        // keep nukes/stuns for fights this many rounds after seeing an enemy
constexpr int kDashHoldRounds = 0;    // ... and the dash for escapes (0: never hold)
constexpr int kHoldSighting = 10;     // an ally's fresh sighting this close counts as seeing an enemy
constexpr float kStunCamp = 1.5f;     // value of a camp hit prevented, per point of its strength
constexpr bool kRetreatCaught = false; // retreat stuns only on chasers we cannot step away from
constexpr bool kWalkReset = false;    // ranged: walk into range, reset, shoot
constexpr float kFocus = 300.0f;      // fight nukes: value of taking a target's whole remaining health
constexpr bool kShieldEarly = false;  // camps: shield on the turn we wake a camp that will hit us
constexpr bool kMeleeWait = false;    // fights: a melee champion keeps its step instead of an exclusive nuke from afar
constexpr bool kCampStunDedup = false; // camps: no stun value on a camp that is stunned already

// Debug: casts per use, printed every 500 rounds.
constexpr bool kSpellStats = false;
enum Use { camp_nuke, camp_stun, camp_shield, camp_reset, camp_wait, fight_nuke, fight_stun, fight_shield,
    fight_reset, fight_hold_marc, flee_skip, fight_wait, n_use };
std::array<int, n_use> g_use{};
const char* kUseNames[n_use] = {"camp_nuke", "camp_stun", "camp_shield", "camp_reset", "camp_wait", "fight_nuke",
    "fight_stun", "fight_shield", "fight_reset", "fight_hold_marc", "flee_skip", "fight_wait"};
int g_use_round = -1;
void count(Use u) {
    if (!kSpellStats) return;
    ++g_use[u];
    const int r = static_cast<int>(cambc::round_number());
    if (r / 500 != g_use_round / 500 && g_use_round >= 0) {
        std::cout << "spells r" << r << ' ' << cambc::to_string(cambc::self().champion_name());
        for (int i = 0; i < n_use; ++i) std::cout << ' ' << kUseNames[i] << '=' << g_use[static_cast<size_t>(i)];
        std::cout << '\n';
    }
    g_use_round = r;
}

struct Kit {
    float range = 4, dmg = 0;
    bool exclusive = true;
};

Kit kit_of(const cambc::ability& ab) {
    Kit k;
    try {
        const auto& info = ab.info();
        if (info) {
            k.exclusive = std::visit([](const auto& x) { return x.exclusive_with_movement; }, *info);
            if (auto* d = std::get_if<cambc::ability_damage>(&*info)) k.range = d->range;
            else if (auto* c = std::get_if<cambc::ability_cc>(&*info)) k.range = c->range;
            else if (auto* a = std::get_if<cambc::ability_aoe_damage>(&*info)) k.range = a->range;
            else if (auto* h = std::get_if<cambc::ability_heal>(&*info)) k.range = h->range;
            else if (auto* s = std::get_if<cambc::ability_dash>(&*info)) k.range = s->range;
        }
    } catch (const std::exception&) {}
    if (ab.is_damage() || ab.is_cc() || ab.is_aoe_damage())
        try { k.dmg = ab.damage_amount(); } catch (const std::exception&) {}
    return k;
}

bool shielded_now() {
    try { return cambc::self().buffs().has_shield(); } catch (const std::exception&) { return false; }
}

bool enhanced_reset(const cambc::ability& ab) {
    try {
        const auto& info = ab.info();
        if (info)
            if (auto* r = std::get_if<cambc::ability_attack_reset>(&*info)) return r->enhanced_attack;
    } catch (const std::exception&) {}
    return false;
}

}  // namespace

// Enemies about: hold what a fight needs.
bool Bot::hold_for_fight(int rounds) const {
    if (rounds <= 0) return false;
    const int now = static_cast<int>(cambc::round_number());
    if (now - last_enemy_seen_ <= rounds) return true;
    const auto s = read_sighting();
    return s.ok && s.age <= rounds && cheb(s.where, pos_) <= kHoldSighting;
}

bool Bot::dash_for_travel() const {
    // A chase we decided on this turn may dash; walks between fights keep it for the way out.
    if (fight_engaged_ && fight_round_ == static_cast<int>(cambc::round_number())) return true;
    return !hold_for_fight(kDashHoldRounds);
}

// Spend the ready attack reset (we just attacked). True when the attack is back.
bool Bot::use_reset() {
    auto& me = cambc::self();
    if (cast_ || !me.can_cast() || !fuel(6000) || !live()) return me.can_attack();
    if (me.can_attack()) return true;  // a kill gave it back already (Vendetta)
    for (const auto& ab : me.abilities()) {
        if (!ab.is_attack_reset() || !ab.is_ready()) continue;
        try { ab.use(); cast_ = true; } catch (const cambc::error& e) { warn("reset", e); return false; }
        count(attacked_ && fight_round_ == static_cast<int>(cambc::round_number()) ? fight_reset : camp_reset);
        return me.can_attack();
    }
    return false;
}

bool Bot::reset_ready() const {
    auto& me = cambc::self();
    if (cast_ || !me.can_cast()) return false;
    for (const auto& ab : me.abilities())
        if (ab.is_attack_reset() && ab.is_ready()) return true;
    return false;
}

// ---------------------------------------------------------------- camps

// Before hitting the group: the shield under fire, a nuke where it removes the most damage. Cast before
// walking (nukes exclude it).
void Bot::spells_on_group(int group) {
    auto& me = cambc::self();
    if (cast_ || !live() || !me.can_cast() || !fuel(12000)) return;
    const auto& v = view();
    const auto& g = g_[group];
    const auto& k = k_[group];
    const int my_range = me.attack_range();
    // Can we hit something from here (then we stand this turn anyway)?
    bool in_reach = false;
    for (size_t c = 0; c < g.tiles.size() && !in_reach; ++c)
        if (k.hp[c] > 0 && cheb(g.tiles[c], pos_) <= my_range && v.lvis.at(g.tiles[c]) && v.lcamps.at(g.tiles[c])) in_reach = true;
    for (const auto& ab : me.abilities()) {
        if (cast_) return;
        if (!ab.is_ready()) continue;
        if (ab.is_shield()) {
            bool soon = false;
            if (kShieldEarly && in_reach && incoming_ == 0)
                for (size_t c = 0; c < g.tiles.size() && !soon; ++c) {
                    const auto s = camp_spec(g.kinds[c]);
                    const int d = cheb(g.tiles[c], pos_);
                    if (k.hp[c] <= 0 || s.str <= 0 || d > s.range) continue;
                    bool nearer = false;
                    for (size_t a = 0; a < v.allies.size() && !nearer; ++a) nearer = cheb(g.tiles[c], v.allies[a]) < d;
                    soon = !nearer;
                }
            if ((incoming_ > 0 || soon) && !shielded_now()) {
                try { ab.use(); cast_ = true; count(camp_shield); } catch (const cambc::error& e) { warn("shield", e); }
            }
            continue;
        }
        if (!(ab.is_damage() || ab.is_cc() || ab.is_aoe_damage())) continue;
        if (!ab.can_target_camps() && !ab.can_target_ground()) continue;
        const Kit kit = kit_of(ab);
        if (kit.exclusive && moved_) continue;
        if (hold_for_fight(kHoldRounds)) continue;
        const int r = static_cast<int>(kit.range);
        int best = -1;
        float best_s = 0;
        bool best_kill = false;
        for (size_t c = 0; c < g.tiles.size(); ++c) {
            const vec2 p = g.tiles[c];
            const float hp = k.hp[c];
            if (hp <= 0 || cheb(p, pos_) > r || !v.lvis.at(p) || !v.lcamps.at(p)) continue;
            const auto s = camp_spec(g.kinds[c]);
            const float take = std::min(kit.dmg, hp);
            // Damage weighted by the threat it removes: strength per health, as the basic attacks order kills.
            float score = take * (1.0f + 40.0f * s.str / std::max(1.0f, hp));
            const bool kill = hp <= kit.dmg;
            if (kill) score += 100.0f + 4.0f * s.str;
            bool stunned = false;
            if (kCampStunDedup && ab.is_cc())
                try { stunned = cambc::vision::camp_at(p).buffs().has_stun(); } catch (const std::exception&) {}
            if (ab.is_cc() && !kill && s.str > 0 && !stunned) {
                // A stun takes its next hit away when someone stands in its reach (the cast wakes it).
                bool threatens = cheb(p, pos_) <= s.range;
                for (size_t a = 0; a < v.allies.size() && !threatens; ++a) threatens = cheb(p, v.allies[a]) <= s.range;
                if (threatens) score += kStunCamp * s.str;
            }
            if (ab.is_aoe_damage())
                for (size_t o = 0; o < g.tiles.size(); ++o)
                    if (o != c && k.hp[o] > 0 && cheb(g.tiles[o], p) <= 1 && v.lcamps.at(g.tiles[o]))
                        score += std::min(kit.dmg, k.hp[o]);
            if (score > best_s) { best_s = score; best = static_cast<int>(c); best_kill = kill; }
        }
        if (best < 0) continue;
        // Out of reach the cast would cost the step that brings us in: wait until we stand anyway.
        if (kit.exclusive && !in_reach && !best_kill) { count(camp_wait); continue; }
        const vec2 p = g.tiles[static_cast<size_t>(best)];
        try {
            if (ab.is_aoe_damage() || !ab.can_target_camps()) ab.use(p);
            else ab.use(cambc::vision::camp_at(p));
            cast_ = true;
            count(ab.is_cc() ? camp_stun : camp_nuke);
            if (kit.exclusive) move_failed_ = true;
            k_[group].hp[static_cast<size_t>(best)] = std::max(1.0f, k.hp[static_cast<size_t>(best)] - kit.dmg);
            invalidate();
        } catch (const cambc::error& e) { warn("spell", e); }
    }
}

// After a basic hit on the group: a second hit through the reset, only when something is left in range.
// A ranged champion that walked into range gets its shot back the same way (a ready reset idles otherwise).
void Bot::reset_hit(int group) {
    auto& me = cambc::self();
    if (!(attacked_ || (kWalkReset && moved_ && !move_failed_ && my_range_ > 1)) || !live() || !fuel(8000)) return;
    if (!me.can_attack() && !reset_ready()) return;
    const auto& v = view();
    const int range = me.attack_range();
    bool target = false;
    for (size_t c = 0; c < g_[group].tiles.size() && !target; ++c) {
        const vec2 p = g_[group].tiles[c];
        target = k_[group].hp[c] > 0 && cheb(p, me.position()) <= range && v.lvis.at(p) && v.lcamps.at(p);
    }
    if (!target) return;
    if (use_reset()) hit_group(group);
}

// ---------------------------------------------------------------- fights

// Unit spells (damage, stun) and the shield, before the basic attacks and before walking.
void Bot::fight_spells(std::vector<Fighter>& theirs, bool engage) {
    auto& me = cambc::self();
    if (cast_ || !me.can_cast() || !live() || !fuel(12000)) return;
    const auto& v = view();
    const float str = me.strength();
    const int my_range = me.attack_range();
    const bool can_hit = me.can_attack();
    // An enhanced reset in hand and a free target beside us: the stun on the second hit comes first.
    bool enhanced_ready = false;
    for (const auto& ab : me.abilities())
        if (ab.is_attack_reset() && ab.is_ready() && enhanced_reset(ab)) enhanced_ready = true;
    bool stun_by_hit = false;
    if (enhanced_ready && can_hit)
        for (const auto& e : theirs)
            if (e.hp > 0 && !e.stunned && cheb(e.pos, pos_) <= my_range && v.lvis.at(e.pos)) stun_by_hit = true;
    for (const auto& ab : me.abilities()) {
        if (cast_) return;
        if (!ab.is_ready()) continue;
        const Kit kit = kit_of(ab);
        if (kit.exclusive && moved_) continue;
        if (ab.is_shield()) {
            if ((threat_at(pos_, theirs) > 0 || incoming_ > 0) && !shielded_now()) {
                try { ab.use(); cast_ = true; count(fight_shield); } catch (const cambc::error& e) { warn("shield", e); }
            }
            continue;
        }
        const bool unit_spell = (ab.is_damage() || ab.is_cc()) && ab.can_target_enemies();
        const bool area_spell = ab.is_aoe_damage() && ab.can_target_ground();
        if (!unit_spell && !area_spell) continue;
        const int r = static_cast<int>(kit.range);
        int best = -1;
        float bs = -1e9f;
        bool best_kill = false;
        for (size_t i = 0; i < theirs.size(); ++i) {
            const auto& e = theirs[i];
            const int d = cheb(e.pos, pos_);
            if (e.hp <= 0 || d > r || !v.lvis.at(e.pos)) continue;
            const float take = std::min(kit.dmg, e.hp);
            float s = take;
            // A kill now, alone or with our basic hit this turn.
            const bool kill = e.hp <= kit.dmg || (can_hit && d <= my_range && e.hp <= kit.dmg + str);
            if (kill) s += 1000 + e.value;
            // Focus fire: the share of its remaining life we take (the basic attacks pick the weakest too).
            s += kFocus * std::min(1.0f, kit.dmg / std::max(1.0f, e.hp));
            if (ab.is_cc()) {
                if (e.stunned) s -= 400;
                else {
                    if (e.channel) s += 400;  // cancels a tribute or a recall
                    // The hit it would deal next turn: in its reach of us (or our allies) after one step.
                    bool reaches = d <= static_cast<int>(e.threat_reach()) + (e.dasher ? 2 : 0);
                    for (size_t a = 0; a < v.allies.size() && !reaches; ++a)
                        reaches = cheb(e.pos, v.allies[a]) <= static_cast<int>(e.threat_reach());
                    s += reaches ? e.sustain + e.burst : 0.3f * e.sustain;
                }
            }
            if (area_spell)
                for (const auto& o : theirs)
                    if (&o != &e && o.hp > 0 && cheb(o.pos, e.pos) <= 1) s += std::min(kit.dmg, o.hp);
            s += 0.5f * e.gems;
            if (s > bs) { bs = s; best = static_cast<int>(i); best_kill = kill; }
        }
        if (best < 0) continue;
        auto& e = theirs[static_cast<size_t>(best)];
        // Marcella: hit, reset, stunning hit now; the nuke follows next turn (cooldowns run per slot).
        if (stun_by_hit && !ab.is_cc() && !best_kill) { count(fight_hold_marc); continue; }
        const int de = cheb(e.pos, pos_);
        // Retreating: only a kill, a stun on a chaser that hits us whatever step we take (melee beside us
        // or a dash away, ranged a tile inside its range) or a cancelled channel is worth the walk.
        const bool caught = kRetreatCaught ? (e.ranged ? de <= e.range - 1 : de <= 1 + (e.dasher ? 2 : 0)) : de <= 3;
        if (kMeleeWait && engage && my_range <= 1 && kit.exclusive && !ab.is_cc() && !best_kill && !e.channel && de >= 2) {
            count(fight_wait);
            continue;
        }
        if (!engage && kit.exclusive && !(best_kill || e.channel || (ab.is_cc() && !e.stunned && caught))) {
            count(flee_skip);
            continue;
        }
        try {
            if (unit_spell) {
                if (!cambc::vision::has_champion(e.pos)) continue;
                ab.use(cambc::vision::champion_at(e.pos));
            } else {
                ab.use(e.pos);
            }
            cast_ = true;
            count(ab.is_cc() ? fight_stun : fight_nuke);
            if (kit.exclusive) move_failed_ = true;  // no walking after this cast
            e.hp -= kit.dmg;
            if (ab.is_cc()) e.stunned = true;
            invalidate();
        } catch (const cambc::error& ex) { warn("enemy spell", ex); }
    }
}
