// Spells: which ability slot to use, on which target, at what moment.
//
// One successful cast per turn is shared by both slots, each slot then waits 8 turn ends.
// Casts that exclude walking go before any step, and only when we would not walk anyway
// (a target already in attack range), unless they kill or break a channel. Attack resets
// follow a basic attack, or, for ranged champions, a step: walking spends their attack and
// the reset gives it back. Stuns go to whoever would hit us next, or to a channeling enemy.
#include "bot.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <variant>

using cambc::ability_kind;

Kit read_kit() {
    Kit k{};
    const auto abs = cambc::self().abilities();
    for (size_t i = 0; i < abs.size(); ++i) {
        const auto& ab = abs[i];
        auto& s = k[i];
        try {
            const auto& info = ab.info();
            if (!info) continue;
            s.used = true;
            std::visit([&](const auto& p) { s.excl = p.exclusive_with_movement; }, *info);
            if (auto* d = std::get_if<cambc::ability_damage>(&*info)) { s.kind = ability_kind::damage; s.range = d->range; }
            else if (auto* c = std::get_if<cambc::ability_cc>(&*info)) { s.kind = ability_kind::cc; s.range = c->range; s.stun = true; }
            else if (auto* a = std::get_if<cambc::ability_aoe_damage>(&*info)) {
                s.kind = ability_kind::aoe_damage; s.range = a->range; s.radius = a->radius; s.stun = a->duration > 0;
            }
            else if (auto* h = std::get_if<cambc::ability_heal>(&*info)) { s.kind = ability_kind::heal; s.range = h->range; }
            else if (auto* sh = std::get_if<cambc::ability_shield>(&*info)) { s.kind = ability_kind::shield; s.range = sh->range; }
            else if (auto* da = std::get_if<cambc::ability_dash>(&*info)) { s.kind = ability_kind::dash; s.range = da->range; }
            else if (auto* r = std::get_if<cambc::ability_attack_reset>(&*info)) { s.kind = ability_kind::attack_reset; s.enhanced = r->enhanced_attack; }
            s.enemies = ab.can_target_enemies();
            s.camps = ab.can_target_camps();
            s.ground = ab.can_target_ground();
            s.allies = ab.can_target_allies();
            s.self = ab.can_target_self();
        } catch (const std::exception& e) {
            s.used = false;
            warn("kit", e);
        }
    }
    return k;
}

namespace {

// A visible enemy champion and what matters for choosing a spell or a hit.
struct Mark {
    vec2 pos{};
    int d = 99, range = 1;
    float hp = 0, power = 0;
    bool tribute = false, recall = false, stunned = false, immune = false;
};

std::vector<Mark> marks(const std::vector<Foe>& list, int maxd) {
    std::vector<Mark> out;
    const vec2 me = cambc::self().position();
    const auto& v = view();
    for (const auto& f : list) {
        const int d = cheb(f.pos, me);
        if (d > maxd || !v.lvis.at(f.pos)) continue;
        try {
            if (!cambc::vision::has_champion(f.pos)) continue;
            auto c = cambc::vision::champion_at(f.pos);
            if (!c.is_visible() || !c.is_alive()) continue;
            Mark m;
            m.pos = f.pos;
            m.d = d;
            m.range = f.range;
            m.hp = c.health();
            m.power = f.power;
            m.tribute = c.is_submitting_gems();
            m.recall = c.is_recalling();
            m.stunned = c.buffs().has_stun();
            m.immune = c.champion_name() == cambc::champion_type::marcella;
            out.push_back(m);
        } catch (const cambc::error&) {}
    }
    return out;
}

// Order of basic-attack targets: kills, then channels a hit breaks or a kill stops, then the weakest.
float hit_score(const Mark& m, float str) {
    float s = -m.hp;
    if (m.hp <= str * 0.9f) s += 1e6f;
    if (m.recall) s += 1e4f;     // damage cancels a recall
    if (m.tribute) s += 5e3f;    // only a kill (or a stun) stops a tribute
    return s;
}

// Hit the best enemy in attack range; true when a hit landed.
bool strike_best(const std::vector<Mark>& ms, bool force = false) {
    auto& me = cambc::self();
    if ((!force && !me.can_attack()) || !live()) return false;
    const int r = me.attack_range();
    const float str = me.strength();
    std::array<std::pair<float, vec2>, 8> c;
    size_t n = 0;
    for (const auto& m : ms)
        if (cheb(m.pos, me.position()) <= r && n < c.size()) c[n++] = {hit_score(m, str), m.pos};
    std::sort(c.begin(), c.begin() + n, [](auto a, auto b) { return a.first > b.first; });
    for (size_t i = 0; i < n; ++i) {
        try {
            if (!cambc::vision::has_champion(c[i].second)) continue;
            auto t = cambc::vision::champion_at(c[i].second);
            if (!t.is_visible() || !t.is_alive()) continue;
            me.attack(t);
            invalidate();
            return true;
        } catch (const cambc::error& e) { warn("strike", e); }
    }
    return false;
}

// Value of a damage or stun cast on one enemy (0 when it does nothing useful).
float cast_value(const SpellSlot& sl, const cambc::ability& ab, const Mark& m) {
    float dmg = 0;
    try {
        if (cambc::vision::has_champion(m.pos)) dmg = ab.damage_amount(cambc::vision::champion_at(m.pos));
    } catch (const std::exception&) { dmg = 0; }
    float v = dmg;
    if (dmg > 0 && dmg >= m.hp - 0.5f) v += 1000;
    if (m.recall && dmg > 0) v += 200;
    if (sl.stun && !m.immune && !m.stunned) {
        if (m.tribute) v += 600;
        else if (m.recall) v += 250;
        else v += m.power * (m.d <= m.range + 1 ? 1.5f : 0.5f);
    }
    return v;
}

const cambc::ability* ready_slot(const std::array<cambc::ability, 2>& abs, const Kit& kit, ability_kind kind) {
    for (size_t i = 0; i < abs.size(); ++i)
        if (kit[i].used && kit[i].kind == kind && abs[i].is_ready()) return &abs[i];
    return nullptr;
}

bool has_shield() {
    try { return cambc::self().buffs().has_shield(); } catch (const std::exception&) { return true; }
}

}  // namespace

float Bot::spell_dps() const {
    auto& me = cambc::self();
    float s = 0;
    try {
        const auto abs = me.abilities();
        for (size_t i = 0; i < abs.size(); ++i) {
            const auto& sl = kit_[i];
            if (!sl.used) continue;
            if (sl.hits() && (sl.camps || sl.ground)) s += abs[i].damage_amount() / 8.0f;
            else if (sl.kind == ability_kind::attack_reset) s += me.strength() / 8.0f;
        }
    } catch (const std::exception&) {}
    return s;
}

// ---------------------------------------------------------------- camps

// Before walking: a shield under fire, damage and stuns on the group.
void Bot::spells_on_group(int group) {
    auto& me = cambc::self();
    if (cast_ || !live() || !me.can_cast() || !fuel(12000)) return;
    const auto& v = view();
    const vec2 pos = me.position();
    const int reach = me.attack_range();
    struct C { vec2 p; float hp, str; int d; bool on_us; };
    std::array<C, 48> cs;
    size_t n = 0;
    bool in_reach = false;
    for (size_t c = 0; c < g_[group].tiles.size() && n < cs.size(); ++c) {
        const vec2 p = g_[group].tiles[c];
        const int d = cheb(p, pos);
        if (d > 5 || !v.lvis.at(p) || !v.lcamps.at(p)) continue;
        const float hp = k_[group].hp[c];
        if (hp <= 0) continue;
        const auto spec = camp_spec(g_[group].kinds[c]);
        // A camp hits the nearest champion in its range.
        bool on_us = spec.str > 0 && d <= spec.range;
        if (on_us)
            for (auto a : v.allies)
                if (cheb(a, p) < d) { on_us = false; break; }
        cs[n++] = {p, hp, spec.str, d, on_us};
        if (d <= reach) in_reach = true;
    }
    const auto abs = me.abilities();
    float best = 0;
    int slot = -1;
    vec2 aim{};
    for (size_t i = 0; i < abs.size(); ++i) {
        const auto& sl = kit_[i];
        if (!sl.used || !abs[i].is_ready()) continue;
        if (sl.excl && moved_) continue;
        if (sl.kind == ability_kind::shield) {
            if (incoming_ <= 0 || has_shield()) continue;
            float amount = 0;
            try { amount = abs[i].shield_amount(); } catch (const std::exception&) {}
            const float val = std::min(amount, incoming_ * taken_ * 5.0f);
            if (val > best) { best = val; slot = static_cast<int>(i); aim = pos; }
            continue;
        }
        if (!sl.hits() || !(sl.camps || sl.ground) || n == 0) continue;
        float dmg = 0;
        try { dmg = abs[i].damage_amount(); } catch (const std::exception&) {}
        for (size_t a = 0; a < n; ++a) {
            if (cs[a].d > sl.range) continue;
            float val = 0;
            bool kill = false;
            for (size_t b = 0; b < n; ++b) {
                const bool hit = b == a || (sl.kind == ability_kind::aoe_damage && cheb(cs[b].p, cs[a].p) <= sl.radius);
                if (!hit) continue;
                val += std::min(dmg, cs[b].hp) * (cs[b].str > 0 ? 1.0f : 0.5f);
                if (dmg >= cs[b].hp) { kill = true; val += 100 + 3 * cs[b].str; }
                if (sl.stun && cs[b].str > 0) val += cs[b].str * taken_ * (cs[b].on_us ? 1.0f : 0.3f);
            }
            // A cast that stops our walk must not delay the fight: only from where we already hit.
            if (sl.excl && !in_reach && !kill) continue;
            if (val > best) { best = val; slot = static_cast<int>(i); aim = cs[a].p; }
        }
    }
    if (slot < 0 || !live()) return;
    const auto& sl = kit_[static_cast<size_t>(slot)];
    const auto& ab = abs[static_cast<size_t>(slot)];
    try {
        bool ok = false;
        if (sl.kind == ability_kind::shield) ok = ab.use();
        else if (sl.camps && v.lcamps.at(aim)) ok = ab.use(cambc::vision::camp_at(aim));
        else ok = ab.use(aim);
        cast_ = ok;
        if (ok && sl.excl) move_failed_ = true;  // no walk after this cast
        invalidate();
    } catch (const cambc::error& e) { warn("spell", e); }
}

// After a hit, or after a ranged champion's step: reset the attack and hit again.
void Bot::reset_hit(int group) {
    auto& me = cambc::self();
    if (cast_ || !live() || !me.can_cast() || !fuel(8000)) return;
    if (me.can_attack()) return;  // an available attack cannot be reset
    const auto abs = me.abilities();
    const auto* ab = ready_slot(abs, kit_, ability_kind::attack_reset);
    if (!ab) return;
    if (!attacked_) {
        // Only a ranged walk spends the attack without using it.
        if (!moved_ || move_failed_ || me.attack_range() <= 1) return;
    }
    const int r = me.attack_range();
    for (int tries = 0; tries < 3; ++tries) {
        // Is anything left to hit?
        if (!view_affordable()) return;
        const auto& v = view();
        const vec2 pos = me.position();
        bool any = false;
        for (size_t c = 0; c < g_[group].tiles.size() && !any; ++c) {
            const vec2 p = g_[group].tiles[c];
            any = cheb(p, pos) <= r && k_[group].hp[c] > 0 && v.lvis.at(p) && v.lcamps.at(p);
        }
        if (!any || !live() || !fuel(6000)) return;
        try {
            if (!ab->use()) return;
            cast_ = true;
        } catch (const cambc::error&) {
            // The attack is still there (Vendetta's kill gives it back without can_attack() showing it):
            // take the free hit, and the reset is still ours.
            if (!hit_group(group, true)) return;
            continue;
        }
        if (me.can_attack()) hit_group(group);
        return;
    }
}

// ---------------------------------------------------------------- dash

// Dash two tiles down field f (reached tiles in seen) when that gains two steps.
bool Bot::try_dash(const cambc::distance_field& f, const cambc::bitboard& seen) {
    auto& me = cambc::self();
    if (cast_ || !dasher_ || !me.can_cast() || !fuel(12000)) return false;
    const vec2 pos = me.position();
    if (!seen.is_set(pos)) return false;
    const auto abs = me.abilities();
    const auto* ab = ready_slot(abs, kit_, ability_kind::dash);
    if (!ab) return false;
    const int here = f.get(pos);
    std::array<std::pair<int, vec2>, 16> cand;
    size_t n = 0;
    for (int dx = -2; dx <= 2; ++dx)
        for (int dy = -2; dy <= 2; ++dy) {
            if (std::max(std::abs(dx), std::abs(dy)) != 2) continue;
            const vec2 t{pos.x + dx, pos.y + dy};
            if (!free_tile(t) || !seen.is_set(t)) continue;
            const int d = f.get(t);
            if (d > here - 2) continue;
            // Need a passable middle tile (holes may be jumped, walls not).
            bool path = false;
            for (auto s : kDirs) {
                const vec2 m{pos.x + s.x, pos.y + s.y};
                if (!inside(m) || cheb(m, t) != 1) continue;
                if (hard_local().at(m) || view().lchamps.at(m) || view().lcamps.at(m)) continue;
                path = true;
                break;
            }
            if (path) cand[n++] = {d, t};
        }
    std::sort(cand.begin(), cand.begin() + n, [](auto a, auto b) { return a.first < b.first; });
    for (size_t i = 0; i < n && i < 3; ++i) {
        if (!live()) return false;
        try {
            const bool ok = ab->use(cand[i].second);
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

// ---------------------------------------------------------------- champions

bool Bot::fight_spells(const std::vector<Foe>& list) {
    auto& me = cambc::self();
    if (!live()) return false;
    const int reach = me.attack_range();
    const bool melee = reach <= 1;
    auto ms = marks(list, 6);
    if (ms.empty()) return false;
    bool in_reach = false;
    int nearest = 99;
    for (const auto& m : ms) {
        if (m.d <= reach) in_reach = true;
        nearest = std::min(nearest, m.d);
    }
    // 1. Casts before the hit (a stun lands first) and before any step.
    if (!cast_ && me.can_cast() && fuel(15000)) {
        const auto abs = me.abilities();
        const float str = me.strength();
        // A reset that finishes someone beats a spell that does not.
        bool reset_kill = false;
        if (ready_slot(abs, kit_, ability_kind::attack_reset) && me.can_attack())
            for (const auto& m : ms)
                if (m.d <= reach && m.hp <= 2 * str * 0.9f) reset_kill = true;
        float best = 0;
        int slot = -1;
        vec2 aim{};
        for (size_t i = 0; i < abs.size(); ++i) {
            const auto& sl = kit_[i];
            if (!sl.used || !abs[i].is_ready()) continue;
            if (sl.excl && moved_) continue;
            if (sl.kind == ability_kind::shield) {
                if (nearest > 3 || has_shield()) continue;
                float amount = 0;
                try { amount = abs[i].shield_amount(); } catch (const std::exception&) {}
                if (amount * 0.8f > best) { best = amount * 0.8f; slot = static_cast<int>(i); aim = me.position(); }
                continue;
            }
            if (!sl.hits() || !(sl.enemies || sl.ground)) continue;
            for (const auto& a : ms) {
                if (a.d > sl.range) continue;
                float val = 0;
                if (sl.kind == ability_kind::aoe_damage) {
                    for (const auto& b : ms)
                        if (cheb(a.pos, b.pos) <= sl.radius) val += cast_value(sl, abs[i], b);
                } else {
                    if (!sl.enemies) continue;
                    val = cast_value(sl, abs[i], a);
                }
                const bool decisive = val >= 1000 || (sl.stun && a.tribute && !a.immune);
                // A melee champion keeps its walk unless the cast decides something, or the last
                // step of a chase did not bring the target into reach (it runs as fast as we walk).
                if (sl.excl && melee && !in_reach && !decisive && chase_turns_ < 1) continue;
                if (reset_kill && val < 1000) continue;
                if (val > best) { best = val; slot = static_cast<int>(i); aim = a.pos; }
            }
        }
        if (slot >= 0 && live()) {
            const auto& sl = kit_[static_cast<size_t>(slot)];
            const auto& ab = abs[static_cast<size_t>(slot)];
            try {
                bool ok = false;
                if (sl.kind == ability_kind::shield) ok = ab.use();
                else if (sl.kind == ability_kind::aoe_damage || !sl.enemies) ok = ab.use(aim);
                else if (cambc::vision::has_champion(aim)) ok = ab.use(cambc::vision::champion_at(aim));
                cast_ = ok;
                if (ok && sl.excl) move_failed_ = true;
                invalidate();
            } catch (const cambc::error& e) { warn("fight spell", e); }
            if (cast_) ms = marks(list, 6);
        }
    }
    // 2. The hit, then a reset for a second one.
    if (!strike_best(ms)) return false;
    attacked_ = true;
    if (!cast_ && me.can_cast() && !me.can_attack() && fuel(8000)) {
        const auto abs = me.abilities();
        if (const auto* ab = ready_slot(abs, kit_, ability_kind::attack_reset)) {
            try {
                if (ab->use()) {
                    cast_ = true;
                    strike_best(marks(list, reach));
                }
            } catch (const cambc::error&) { strike_best(marks(list, reach), true); }
        }
    }
    return true;
}

bool Bot::strike_after_move(const std::vector<Foe>& list) {
    auto& me = cambc::self();
    if (!live()) return false;
    const int reach = me.attack_range();
    auto ms = marks(list, reach);
    if (ms.empty()) return false;
    if (reach <= 1) {
        // Melee: walk and hit, then reset and hit again.
        if (!strike_best(ms)) return false;
        attacked_ = true;
    } else if (me.can_attack()) {
        // Ranged and the attack is still there (a dash does not spend it).
        if (!strike_best(ms)) return false;
        attacked_ = true;
    } else if (moved_ && !move_failed_ && !attacked_) {
        // Ranged: the step spent the attack; a reset gives it back.
        if (cast_ || !me.can_cast()) return false;
        const auto abs = me.abilities();
        const auto* ab = ready_slot(abs, kit_, ability_kind::attack_reset);
        if (!ab) return false;
        try {
            if (!ab->use()) return false;
            cast_ = true;
        } catch (const cambc::error& e) { warn("chase reset", e); return false; }
        if (!strike_best(ms)) return false;
        attacked_ = true;
        return true;
    } else {
        return false;
    }
    if (!cast_ && me.can_cast() && !me.can_attack() && fuel(8000)) {
        const auto abs = me.abilities();
        if (const auto* ab = ready_slot(abs, kit_, ability_kind::attack_reset)) {
            try {
                if (ab->use()) {
                    cast_ = true;
                    strike_best(marks(list, reach));
                }
            } catch (const cambc::error&) { strike_best(marks(list, reach), true); }
        }
    }
    return true;
}

void Bot::flee_spells(const std::vector<Foe>& list) {
    auto& me = cambc::self();
    if (!live()) return;
    auto ms = marks(list, 4);
    if (ms.empty()) return;
    // A spell that kills a chaser is worth the step it may cost.
    if (!cast_ && me.can_cast() && !moved_ && fuel(12000)) {
        const auto abs = me.abilities();
        for (size_t i = 0; i < abs.size() && !cast_; ++i) {
            const auto& sl = kit_[i];
            if (!sl.used || !sl.hits() || !sl.enemies || sl.kind == ability_kind::aoe_damage || !abs[i].is_ready()) continue;
            for (const auto& m : ms) {
                if (m.d > sl.range || cast_value(sl, abs[i], m) < 1000 || !live()) continue;
                try {
                    if (cambc::vision::has_champion(m.pos)) cast_ = abs[i].use(cambc::vision::champion_at(m.pos));
                    if (cast_ && sl.excl) move_failed_ = true;
                    invalidate();
                } catch (const cambc::error& e) { warn("flee spell", e); }
                break;
            }
        }
        if (cast_) ms = marks(list, 3);
    }
    // One chaser that can reach us: a stun buys a turn without its hits, and a ranged champion
    // that stands still can shoot it too. Running would not shake it off anyway.
    if (!cast_ && me.can_cast() && !moved_ && fuel(10000)) {
        int threats = 0;
        const Mark* chaser = nullptr;
        for (const auto& m : ms)
            if (m.d <= m.range + 1 && !m.stunned && !m.tribute && !m.recall) { ++threats; chaser = &m; }
        if (threats == 1 && !chaser->immune) {
            const auto abs = me.abilities();
            for (size_t i = 0; i < abs.size() && !cast_; ++i) {
                const auto& sl = kit_[i];
                if (!sl.used || !sl.stun || !sl.enemies || sl.kind == ability_kind::aoe_damage) continue;
                if (!abs[i].is_ready() || chaser->d > sl.range || !live()) continue;
                try {
                    if (cambc::vision::has_champion(chaser->pos)) cast_ = abs[i].use(cambc::vision::champion_at(chaser->pos));
                    if (cast_ && sl.excl) move_failed_ = true;
                    invalidate();
                } catch (const cambc::error& e) { warn("flee stun", e); }
            }
            if (cast_ && move_failed_ && me.attack_range() > 1) {
                if (strike_best(marks(list, me.attack_range()))) attacked_ = true;
                return;
            }
        }
    }
    // A shield costs no step.
    if (!cast_ && me.can_cast() && !has_shield()) {
        const auto abs = me.abilities();
        if (const auto* ab = ready_slot(abs, kit_, ability_kind::shield)) {
            try {
                cast_ = ab->use();
                invalidate();
            } catch (const cambc::error& e) { warn("flee shield", e); }
        }
    }
    // Melee champions can swing and still walk; a reset swings again (an enhanced one stuns the chaser).
    if (me.attack_range() > 1) return;
    if (!strike_best(ms)) return;
    attacked_ = true;
    if (!cast_ && me.can_cast() && !me.can_attack() && fuel(8000)) {
        const auto abs = me.abilities();
        if (const auto* ab = ready_slot(abs, kit_, ability_kind::attack_reset)) {
            try {
                if (ab->use()) {
                    cast_ = true;
                    strike_best(marks(list, 1));
                }
            } catch (const cambc::error&) { strike_best(marks(list, 1), true); }
        }
    }
}
