// The spells subsystem: which ability slot, on which target, at what moment.
//
// Rules it is built on (GameRules.md, patches 100-102): one successful cast per turn across both slots, an
// 8-turn-end cooldown per slot, spells scale with Focus (damage_amount() already includes items and passives),
// "excludes walking" spells must come before any walk and forbid walking after them, an attack reset only
// restores a spent attack allowance (a ranged champion's walk spends it too), stuns take the target's next
// action and cancel tributes and recalls (damage cancels recalls only), camps hit the nearest champion in
// their reach. Every option for this turn's single cast is scored on the same scale (damage it adds, damage
// it prevents, kills and cancelled channels) and only the best one is cast; strong spells are held while an
// enemy champion is close, since a stun or burst decides a fight far more than it speeds a camp.
#include <cambc.h>
#include "bot.h"
#include "fuel.h"
#include <algorithm>
#include <cmath>
#include <variant>

namespace {

constexpr int kHoldRange = 0;          // an enemy champion this close: keep stuns and nukes for it (0: off;
                                       // 7 measured 44/80 vs f03-05 against 49/80 off)
constexpr float kMinCampValue = 30.0f; // a cast on camps must add at least this much

struct SpellInfo {
    float range = 0, radius = 0;
    bool exclusive = false, enhanced = false;
};

SpellInfo spell_info(const cambc::ability& ab) {
    SpellInfo s;
    try {
        const auto& info = ab.info();
        if (!info) return s;
        s.exclusive = std::visit([](const auto& x) { return x.exclusive_with_movement; }, *info);
        if (auto* d = std::get_if<cambc::ability_damage>(&*info)) s.range = d->range;
        else if (auto* c = std::get_if<cambc::ability_cc>(&*info)) s.range = c->range;
        else if (auto* a = std::get_if<cambc::ability_aoe_damage>(&*info)) { s.range = a->range; s.radius = a->radius; }
        else if (auto* h = std::get_if<cambc::ability_heal>(&*info)) s.range = h->range;
        else if (auto* r = std::get_if<cambc::ability_attack_reset>(&*info)) s.enhanced = r->enhanced_attack;
    } catch (const std::exception&) {
        s.exclusive = true;
    }
    return s;
}

bool offensive(const cambc::ability& ab) { return ab.is_damage() || ab.is_cc() || ab.is_aoe_damage(); }

// Nearest visible enemy champion off its own spawn (Chebyshev), 99 when none.
int nearest_foe(vec2 at) {
    const auto& v = view();
    if (v.foes.is_empty()) return 99;
    const auto& their_spawn = cambc::map::spawn_region(team::enemy);
    int best = 99;
    for (auto p : v.foes.positions())
        if (!their_spawn.is_set(p)) best = std::min(best, cheb(p, at));
    return best;
}

}  // namespace

// Cast a ready attack reset once our attack allowance is spent (by our own hit this turn, or by a ranged
// champion's walk). True when an attack is available again.
bool Bot::reset_attack() {
    auto& me = cambc::self();
    if (cast_ || !live() || !fuel(8000) || !me.can_cast() || me.can_attack()) return false;
    if (!attacked_ && !(moved_ && me.attack_range() > 1)) return false;
    for (const auto& ab : me.abilities()) {
        if (!ab.is_attack_reset() || !ab.is_ready()) continue;
        try {
            if (!ab.use()) return false;
        } catch (const cambc::error& e) { warn("reset", e); return false; }
        cast_ = true;
        return me.can_attack();
    }
    return false;
}

// After a hit on the group (or a ranged walk): reset and hit again, when something is left to hit.
void Bot::reset_hit(int group) {
    auto& me = cambc::self();
    if (cast_ || !live() || me.can_attack() || !me.can_cast()) return;
    if (!attacked_ && !(moved_ && me.attack_range() > 1)) return;
    const int range = me.attack_range();
    const vec2 pos = me.position();
    const auto& v = view();
    bool target = false;
    for (size_t c = 0; c < g_[group].tiles.size() && !target; ++c) {
        const vec2 p = g_[group].tiles[c];
        target = k_[group].hp[c] > 0 && cheb(p, pos) <= range && v.lvis.at(p) && v.lcamps.at(p);
    }
    if (!target) return;
    if (reset_attack()) hit_group(group);
}

// Before walking: the one cast of the turn on the group we farm. A shield when camps hit us (or are about
// to), otherwise the damage spell with the most value: damage that lands (no overkill), a camp killed (its
// hits stop), a stun on a camp that hits one of us (one hit fewer). Strong spells wait while an enemy
// champion is near.
void Bot::spells_on_group(int group) {
    auto& me = cambc::self();
    if (cast_ || !live() || !me.can_cast() || !fuel(12000)) return;
    const auto& v = view();
    const auto& g = g_[group];
    const auto& k = k_[group];
    const vec2 pos = me.position();
    const int range = me.attack_range();
    const float str = me.strength();
    const bool hold = kHoldRange > 0 && nearest_foe(pos) <= kHoldRange;
    // Camp damage on us next round: awake camps that reach us, or the ones our hit this turn wakes.
    float threat = incoming_;
    if (threat <= 0) {
        bool hitting = false;
        for (size_t c = 0; c < g.tiles.size() && !hitting; ++c)
            hitting = k.hp[c] > 0 && cheb(g.tiles[c], pos) <= range && v.lcamps.at(g.tiles[c]);
        if (hitting)
            for (size_t c = 0; c < g.tiles.size(); ++c) {
                const auto s = camp_spec(g.kinds[c]);
                if (k.hp[c] > 0 && s.str > 0 && cheb(g.tiles[c], pos) <= s.range) threat += s.str;
            }
    }
    const auto abs = me.abilities();
    int best = -1;
    float best_v = 0;
    vec2 best_at{};
    for (size_t i = 0; i < abs.size(); ++i) {
        const auto& ab = abs[i];
        if (!ab.is_shield() && !offensive(ab)) continue;
        if (!ab.is_ready()) continue;
        const SpellInfo si = spell_info(ab);
        if (ab.is_shield()) {
            if (threat <= 0) continue;
            float amount = 0;
            try { amount = ab.shield_amount(); } catch (const std::exception&) { amount = 2.0f * me.focus(); }
            // What it absorbs over its five turns, worth more when we are low.
            float val = std::min(amount, threat * taken_ * 4.0f);
            if (me.health() < me.max_health() * 0.5f) val *= 1.5f;
            if (val > best_v) { best_v = val; best = static_cast<int>(i); }
            continue;
        }
        if (si.exclusive && moved_) continue;
        if (hold) continue;
        if (!ab.can_target_camps() && !ab.can_target_ground()) continue;
        float dmg = 0;
        try { dmg = ab.damage_amount(); } catch (const std::exception&) { dmg = me.focus(); }
        const int r = static_cast<int>(si.range);
        for (size_t c = 0; c < g.tiles.size(); ++c) {
            const vec2 p = g.tiles[c];
            const float hp = k.hp[c];
            if (hp <= 0 || cheb(p, pos) > r || !v.lvis.at(p) || !v.lcamps.at(p)) continue;
            const auto s = camp_spec(g.kinds[c]);
            float val = std::min(hp, dmg);
            // Our basic attack finishes it anyway: the spell is better spent elsewhere.
            if (hp <= str && cheb(p, pos) <= range && me.can_attack()) val *= 0.3f;
            else if (hp <= dmg) val += 40.0f + 3.0f * s.str;
            if (ab.is_cc() && s.str > 0) {
                // A stunned camp skips its next hit, if it is going to hit any of us.
                bool hits = awake_.is_set(p) || cheb(p, pos) <= s.range;
                for (size_t a = 0; a < v.allies.size() && !hits; ++a) hits = cheb(v.allies[a], p) <= s.range;
                if (hits && hp > dmg) val += s.str * taken_;
            }
            if (si.radius > 0)
                for (size_t o = 0; o < g.tiles.size(); ++o)
                    if (o != c && k.hp[o] > 0 && cheb(g.tiles[o], p) <= static_cast<int>(si.radius) && v.lcamps.at(g.tiles[o]))
                        val += std::min(k.hp[o], dmg);
            if (val > best_v && val >= kMinCampValue) { best_v = val; best = static_cast<int>(i); best_at = p; }
        }
    }
    if (best < 0 || !live()) return;
    const auto& ab = abs[static_cast<size_t>(best)];
    try {
        bool ok = false;
        if (ab.is_shield()) ok = ab.use();
        else if (ab.can_target_camps() && !ab.is_aoe_damage()) ok = ab.use(cambc::vision::camp_at(best_at));
        else ok = ab.use(best_at);
        if (!ok) return;
        cast_ = true;
        if (spell_info(ab).exclusive) move_failed_ = true;  // no walking after this cast
        invalidate();
    } catch (const cambc::error& e) { warn("spell", e); }
}

// Before striking or walking in a fight: the one cast of the turn on enemy champions. A shield when they
// (or camps) can hit us; otherwise the unit or area spell with the most value: predicted damage after
// shields, a kill (with the basic attack that follows), a stun on the fighter that would hurt us most, a
// tribute or recall cancelled. Retreating, a spell that roots us is cast only for a kill, a stun on a chaser
// or a cancelled channel.
void Bot::fight_spells(std::vector<Fighter>& theirs, bool engage) {
    auto& me = cambc::self();
    if (cast_ || !live() || !me.can_cast() || !fuel(12000)) return;
    const auto& v = view();
    const vec2 pos = me.position();
    const int range = me.attack_range();
    const float str = me.strength();
    const bool can_hit = me.can_attack();
    const auto abs = me.abilities();
    int best = -1, best_e = -1;
    float best_v = 0, best_dmg = 0;
    for (size_t i = 0; i < abs.size(); ++i) {
        const auto& ab = abs[i];
        if (!ab.is_shield() && !offensive(ab)) continue;
        if (!ab.is_ready()) continue;
        const SpellInfo si = spell_info(ab);
        if (ab.is_shield()) {
            const float threat = threat_at(pos, theirs) + incoming_ * taken_;
            if (threat <= 0) continue;
            float amount = 0;
            try { amount = ab.shield_amount(); } catch (const std::exception&) { amount = 2.0f * me.focus(); }
            float val = std::min(amount, threat * 2.0f);
            if (me.health() < threat * 2.0f) val *= 1.5f;  // it may keep us alive
            if (val > best_v) { best_v = val; best = static_cast<int>(i); best_e = -1; }
            continue;
        }
        if (si.exclusive && moved_) continue;
        const bool unit = (ab.is_damage() || ab.is_cc()) && ab.can_target_enemies();
        const bool area = ab.is_aoe_damage() && ab.can_target_ground();
        if (!unit && !area) continue;
        float base = 0;
        try { base = ab.damage_amount(); } catch (const std::exception&) { base = me.focus(); }
        const int r = static_cast<int>(si.range);
        for (size_t j = 0; j < theirs.size(); ++j) {
            auto& e = theirs[j];
            if (e.hp <= 0 || cheb(e.pos, pos) > r || !v.lvis.at(e.pos)) continue;
            float dmg = std::min(base, e.hp);
            bool tribute = false;
            if (unit) {
                try {
                    if (!cambc::vision::has_champion(e.pos)) continue;
                    const auto c = cambc::vision::champion_at(e.pos);
                    if (!c.is_visible() || !c.is_alive()) continue;
                    if (base > 0) dmg = ab.damage_amount(c);
                    tribute = c.is_submitting_gems();
                } catch (const std::exception&) { continue; }
            }
            float val = dmg;
            const bool follow = can_hit && cheb(e.pos, pos) <= range;
            if (dmg >= e.hp - 0.5f) val += 1000.0f + e.value;
            else if (follow && dmg + str >= e.hp) val += 600.0f + 0.5f * e.value;
            else val += 150.0f * (1.0f - e.hp / e.max_hp) * (dmg / std::max(1.0f, e.hp));  // focus the wounded
            if (ab.is_cc() && !e.stunned) {
                // Its next turn of damage, if it can reach one of us then.
                const int reach = static_cast<int>(e.threat_reach());
                bool threatens = cheb(e.pos, pos) <= reach;
                for (size_t a = 0; a < v.allies.size() && !threatens; ++a) threatens = cheb(e.pos, v.allies[a]) <= reach;
                val += (threatens ? 1.0f : 0.3f) * (e.sustain + 0.5f * e.burst);
                if (tribute) val += 300.0f + 5.0f * e.gems;  // a stun cancels a tribute
                else if (e.channel) val += 150.0f;           // ... and a recall
            } else if (e.channel && !tribute && dmg > 0) {
                val += 150.0f;                               // damage cancels a recall
            }
            if (area)
                for (const auto& o : theirs)
                    if (&o != &e && o.hp > 0 && cheb(o.pos, e.pos) <= static_cast<int>(si.radius)) val += std::min(base, o.hp);
            // Retreating: rooting ourselves pays only for a kill, a stunned chaser or a cancelled channel.
            if (!engage && si.exclusive) {
                const bool kill = dmg >= e.hp - 0.5f;
                const bool chaser = ab.is_cc() && !e.stunned && cheb(e.pos, pos) <= static_cast<int>(e.threat_reach()) + 1;
                if (!kill && !chaser && !e.channel) continue;
            }
            if (val > best_v) { best_v = val; best = static_cast<int>(i); best_e = static_cast<int>(j); best_dmg = dmg; }
        }
    }
    if (best < 0 || !live()) return;
    const auto& ab = abs[static_cast<size_t>(best)];
    try {
        bool ok = false;
        if (best_e < 0) {
            ok = ab.use();
        } else {
            auto& e = theirs[static_cast<size_t>(best_e)];
            if (ab.is_aoe_damage()) ok = ab.use(e.pos);
            else {
                if (!cambc::vision::has_champion(e.pos)) return;
                ok = ab.use(cambc::vision::champion_at(e.pos));
            }
            if (ok) {
                e.hp -= best_dmg;
                if (ab.is_cc()) e.stunned = true;
            }
        }
        if (!ok) return;
        cast_ = true;
        if (spell_info(ab).exclusive) move_failed_ = true;  // no walking after this cast
        invalidate();
    } catch (const cambc::error& ex) { warn("enemy spell", ex); }
}
