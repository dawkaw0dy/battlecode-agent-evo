// Defense: who threatens us and how hard, fight or flight, when a tribute or a recall is safe,
// and what to do with a channel once enemies show up.
#include <cambc.h>
#include "bot.h"
#include "shared.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <span>

using ct = cambc::champion_type;
using cambc::ability_kind;

constexpr int kRecallSlack = -2;      // recall_safe(): first possible hit later than recall_time + this
constexpr int kHorizon = 3;           // turns of enemy damage a channel decision looks at
constexpr int kGuardGems = 40;        // a tribute this big calls nearby allies to guard it
constexpr int kGuardClaim = 254;      // claim byte 255: "tributing a big bag, come and guard"
constexpr int kRelevantSteps = 6;     // enemies further than this many steps from hitting us are ignored
// Switches for variants that tested worse (NOTES.md, "Tried and failed"); 0/false is the tested bot.
constexpr int kPowerModel = 0;        // fight odds: 0 1.1 str + 0.25 focus, 1 kit-based
constexpr bool kCornered = false;     // fight back when running cannot save us
constexpr int kFleeMode = 0;          // 0 run home on the shortest route, 1 danger-scored steps
constexpr float kHomeWeight = 12.0f;  // flee steps: danger (hp) worth one tile closer to home
constexpr int kRecallRule = 0;        // fleeing: 0 recall when nobody is within 6, 1 recall_safe(), 2 walking reach
constexpr bool kRwSafe = false;       // recall_or_walk waits for recall_safe()

namespace {

// What a champion type can do to us beyond its basic attack (patched kits).
struct Reach {
    int dash = 0;          // tiles a dash adds to its approach
    int spell = 0;         // range of a damaging or stunning spell (0: none)
    float burst = 0;       // that spell's damage per focus
    float resets = 0;      // extra basic attacks from a reset
    bool stun = false;     // the spell stuns (it would break our channel)
};
Reach reach_of(ct t) {
    switch (t) {
    case ct::ryth: return {0, 4, 1.0f, 1, true};
    case ct::vesper_vale: return {2, 0, 0, 1, false};
    case ct::vendetta: return {2, 0, 0, 1, false};
    case ct::cassi: return {0, 4, 2.0f, 0, true};
    case ct::issac: return {2, 0, 0, 1, false};
    case ct::stellar: return {0, 4, 2.0f, 0, false};
    case ct::prism: return {0, 4, 1.5f, 0, true};
    case ct::marcella: return {0, 4, 2.0f, 1, true};
    case ct::willow: return {0, 0, 0, 1, false};
    case ct::ren: return {2, 4, 0.75f, 0, true};
    case ct::varg: return {0, 0, 0, 1, false};
    case ct::tavi: return {0, 4, 1.4f, 1, true};
    case ct::heart_of_ruin: return {2, 3, 1.5f, 0, true};
    }
    return {};
}

const cambc::ability* ready_kind(const std::array<cambc::ability, 2>& abs, const Kit& kit, ability_kind kind) {
    for (size_t i = 0; i < abs.size(); ++i)
        if (kit[i].used && kit[i].kind == kind && abs[i].is_ready()) return &abs[i];
    return nullptr;
}

}  // namespace

// Shared memory bytes 5-7: the latest enemy sighting (x + 1, y, round / 4).
Sighting read_sighting() {
    std::array<std::uint8_t, 3> b{};
    cambc::shared_memory.read(5, std::span<std::uint8_t>{b.data(), b.size()});
    Sighting s;
    if (b[0] == 0) return s;
    s.where = {b[0] - 1, b[1]};
    const int now = static_cast<int>(cambc::round_number() >> 2) & 0xff;
    s.age = ((now - b[2]) & 0xff) * 4;
    s.ok = inside(s.where);
    return s;
}
void write_sighting(vec2 p) {
    std::array<std::uint8_t, 3> b{static_cast<std::uint8_t>(p.x + 1), static_cast<std::uint8_t>(p.y),
        static_cast<std::uint8_t>((cambc::round_number() >> 2) & 0xff)};
    cambc::shared_memory.write(5, std::span<const std::uint8_t>{b.data(), b.size()});
}

// ---------------------------------------------------------------- threat model

// Visible enemy champions off their spawn with what they can do to us; allies in reach of the fight.
void Bot::assess() {
    threats_.clear();
    bool dash_threat = false;
    help_hp_ = 0;
    help_dps_ = 0;
    const vec2 me = pos_;
    const auto& enemy_spawn = cambc::map::spawn_region(team::enemy);
    for (int side = 0; side < 2; ++side) {
        const bool enemy = side == 0;
        const auto tiles = cambc::vision::champion_tiles(enemy ? team::enemy : team::ally);
        if (tiles.is_empty()) continue;
        for (auto p : tiles.positions()) {
            if (p == me) continue;
            const int d = cheb(p, me);
            if (d > (enemy ? 10 : 7)) continue;
            try {
                auto c = cambc::vision::champion_at(p);
                if (!c.is_alive()) continue;
                const auto r = reach_of(c.champion_name());
                const float str = c.strength(), foc = c.focus();
                const float burst = str * (1.0f + r.resets) + r.burst * foc;
                // Fight odds use the tried power estimate; burst is what one turn can do to us.
                const float dps = kPowerModel == 0 ? 1.1f * str + 0.25f * foc : str * (1.0f + r.resets / 8.0f) + r.burst * foc / 8.0f;
                if (!enemy) {
                    help_hp_ += c.health();
                    help_dps_ += dps;
                    continue;
                }
                if (enemy_spawn.is_set(p)) continue;
                Threat t;
                t.pos = p;
                t.d = d;
                t.hp = c.health();
                t.max_hp = c.max_health();
                t.dps = dps;
                t.burst = burst;
                t.range = c.attack_range();
                // Tiles from which it can hurt us on its next turn.
                const int walk_hit = t.range <= 1 ? 2 + r.dash : t.range + (r.dash > 0 ? r.dash : 0);
                t.reach = std::max(walk_hit, r.spell);
                t.stand = std::max(t.range, r.spell);
                t.walk = d;
                if (r.dash > 0) dash_threat = true;
                t.stun = r.stun;
                try {
                    const auto b = c.buffs();
                    t.stunned = b.has_stun();
                } catch (const cambc::error&) {}
                try { t.busy = c.is_submitting_gems() || c.is_recalling(); } catch (const cambc::error&) {}
                threats_.push_back(t);
            } catch (const cambc::error&) {}
        }
    }
    // Walking distances: an enemy across a hole or a wall is no threat however close it looks.
    if (threats_.empty() || !fuel(30000)) return;
    static const cambc::bitboard terrain = cambc::map::walls() | cambc::map::altars();
    static const cambc::bitboard terrain_holes = terrain | cambc::map::holes();
    auto here = empty_board();
    here.set(me);
    cambc::distance_field f;
    cambc::bitboard seen;
    f.compute(dash_threat ? terrain : terrain_holes, here, seen, 14);
    for (auto& t : threats_) t.walk = seen.is_set(t.pos) ? std::max<int>(t.d, f.get(t.pos)) : 99;
}

// Turns of walking before threat t can hit a champion standing at `at`.
int Bot::steps_to_hit(const Threat& t, vec2 at) const {
    const int c = cheb(t.pos, at);
    if (c <= t.stand) return 0;
    const int w = t.walk >= 99 ? 99 : std::max(c, t.walk + c - t.d);
    return std::max(0, w - t.reach);
}

// Damage the visible enemies can deal to a champion standing at `at` over the next `turns` turns
// if they all come for it.
float Bot::danger_at(vec2 at, int turns) const {
    float total = 0;
    for (const auto& t : threats_) {
        int k = turns - steps_to_hit(t, at);
        if (t.stunned) --k;
        if (k <= 0) continue;
        total += t.burst + t.dps * static_cast<float>(k - 1);
    }
    return total * taken_champ_;
}

// Turns before the first enemy can hit us at `at` (99: none in view).
int Bot::first_hit(vec2 at) const {
    int best = 99;
    for (const auto& t : threats_) best = std::min(best, steps_to_hit(t, at) + 1 + (t.stunned ? 1 : 0));
    return best;
}

bool Bot::recall_safe() const {
    // The recall completes after recall_time of our turn ends; any hit before that wastes it.
    return incoming_ == 0 && first_hit(pos_) > cambc::constants::recall_time + kRecallSlack;
}

// ---------------------------------------------------------------- movement away from danger

// One step (or a dash) that keeps us out of reach and brings us home; true when we moved.
bool Bot::flee() {
    auto& me = cambc::self();
    if (!live()) return false;
    const vec2 pos = me.position();
    auto score = [&](vec2 t) {
        float s = danger_at(t, 1) + 0.35f * danger_at(t, 3);
        s += kHomeWeight * static_cast<float>(std::min(home_.at(t), 200));
        return s;
    };
    const float stay = score(pos);
    float best = stay;
    vec2 to = pos;
    if (!moved_ && !move_failed_ && me.can_move())
        for (auto d : kDirs) {
            const vec2 t{pos.x + d.x, pos.y + d.y};
            if (!free_tile(t) || camp_block_.is_set(t) || home_.at(t) >= kFar) continue;
            const float s = score(t);
            if (s < best) { best = s; to = t; }
        }
    // A dash two tiles further from the chasers (cast before the walk).
    if (dasher_ && !cast_ && me.can_cast() && !moved_ && danger_at(to, 1) > 0 && fuel(15000)) {
        const auto abs = me.abilities();
        if (const auto* ab = ready_kind(abs, kit_, ability_kind::dash)) {
            float dbest = best - 1.0f;
            vec2 land = pos;
            for (int dx = -2; dx <= 2; ++dx)
                for (int dy = -2; dy <= 2; ++dy) {
                    if (std::max(std::abs(dx), std::abs(dy)) != 2) continue;
                    const vec2 t{pos.x + dx, pos.y + dy};
                    if (!free_tile(t) || camp_block_.is_set(t) || home_.at(t) >= kFar) continue;
                    bool path = false;
                    for (auto s : kDirs) {
                        const vec2 m{pos.x + s.x, pos.y + s.y};
                        if (!inside(m) || cheb(m, t) != 1) continue;
                        if (hard_local().at(m) || view().lchamps.at(m) || view().lcamps.at(m)) continue;
                        path = true;
                        break;
                    }
                    if (!path) continue;
                    const float s = score(t);
                    if (s < dbest) { dbest = s; land = t; }
                }
            if (land != pos && live()) {
                try {
                    const bool ok = ab->use(land);
                    cast_ = true;
                    invalidate();
                    if (!ok) move_failed_ = true;
                    else {
                        moved_ = true;
                        pos_ = me.position();
                        // Keep walking when the dash left the step.
                        if (me.can_move() && !move_failed_) {
                            moved_ = false;
                            flee_walk_only();
                        }
                        moved_ = true;
                        return true;
                    }
                } catch (const cambc::error& e) { warn("flee dash", e); }
            }
        }
    }
    if (to == pos || !live()) return false;
    moved_ = true;
    bool ok = false;
    try { ok = me.move_absolute(to); } catch (const cambc::error& e) { warn("flee move", e); }
    invalidate();
    if (!ok) move_failed_ = true;
    else pos_ = to;
    return ok;
}

void Bot::flee_walk_only() {
    auto& me = cambc::self();
    const vec2 pos = me.position();
    float best = danger_at(pos, 1) + 0.35f * danger_at(pos, 3) + kHomeWeight * static_cast<float>(std::min(home_.at(pos), 200));
    vec2 to = pos;
    for (auto d : kDirs) {
        const vec2 t{pos.x + d.x, pos.y + d.y};
        if (!free_tile(t) || camp_block_.is_set(t) || home_.at(t) >= kFar) continue;
        const float s = danger_at(t, 1) + 0.35f * danger_at(t, 3) + kHomeWeight * static_cast<float>(std::min(home_.at(t), 200));
        if (s < best) { best = s; to = t; }
    }
    if (to == pos || !live()) return;
    bool ok = false;
    try { ok = me.move_absolute(to); } catch (const cambc::error& e) { warn("flee step", e); }
    invalidate();
    if (!ok) move_failed_ = true;
    else pos_ = to;
}

void Bot::go_home() { go(cambc::map::spawn_region(team::ally), true); }

// Walk home when it is close, otherwise recall where nobody can break it. True when the turn is used.
bool Bot::recall_or_walk(int walk_limit) {
    auto& me = cambc::self();
    if (cambc::map::spawn_region(team::ally).is_set(pos_)) return false;
    const int d = home_.at(pos_);
    if (d <= walk_limit) {
        go_home();
        return true;
    }
    if (moved_ || incoming_ > 0 || !live()) return moved_;
    if (kRwSafe && !recall_safe()) {
        // Someone could break the recall: put distance between us first.
        if (!flee()) go_home();
        return true;
    }
    try { me.recall(); } catch (const cambc::error& e) { warn("recall", e); }
    return true;
}

// ---------------------------------------------------------------- channels

// Called while recalling or tributing: true keeps the channel (turn over), false cancels it and
// lets the turn continue.
bool Bot::hold_channel() {
    auto& me = cambc::self();
    if (me.is_teleporting()) return true;
    assess();
    if (me.is_submitting_gems() && static_cast<int>(me.gem_count()) >= kGuardGems) write_claim(kGuardClaim);
    if (threats_.empty()) return true;
    int left = 1;
    try { left = std::max(0, static_cast<int>(me.remaining_channel_time())); } catch (const cambc::error&) {}
    const float hp = me.health();
    if (me.is_submitting_gems()) {
        // Damage does not stop a tribute; only death (or a stun) does. Keep it unless the enemies
        // in view would kill us first while we could still walk away from them.
        const float dmg = danger_at(pos_, std::min(left + 1, kHorizon));
        bool stunner = false;
        for (const auto& t : threats_)
            if (t.stun && !t.stunned && steps_to_hit(t, pos_) <= std::min(left, kHorizon - 1)) stunner = true;
        const int first = first_hit(pos_);
        if ((dmg < hp * 0.9f && !(stunner && left >= 2)) || left == 0 || first <= 1) return true;
        try { me.cancel_submit_gems(); } catch (const cambc::error& e) { warn("cancel tribute", e); return true; }
        return false;
    }
    if (me.is_recalling()) {
        // Any hit breaks a recall: when one lands next turn anyway, use this turn to act instead.
        if (first_hit(pos_) > 1) return true;
        try { me.cancel_recall(); } catch (const cambc::error& e) { warn("cancel recall", e); return true; }
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- enemies

// No enemy in view: walk over to an ally who tributes a big bag with enemies reported close to it.
bool Bot::guard() {
    auto& me = cambc::self();
    if (me.health() < me.max_health() * 0.5f || me.gem_count() >= 30 || incoming_ > 0) return false;
    std::array<std::uint8_t, 5> b{};
    cambc::shared_memory.read(0, std::span<std::uint8_t>{b.data(), b.size()});
    int want = -1;
    for (int s = 0; s < 5; ++s)
        if (s != slot_ && b[static_cast<size_t>(s)] == kGuardClaim + 1) want = s;
    if (want < 0) return false;
    const auto sg = read_sighting();
    for (const auto& a : cambc::champions(team::ally)) {
        try {
            if (slot_of(a.champion_name(), size_) != want || !a.is_alive()) continue;
            const vec2 p = a.position();
            if (!inside(p) || cheb(p, pos_) > 18 || cheb(p, pos_) <= 2) return false;
            if (!sg.ok || sg.age > 12 || cheb(sg.where, p) > 10) return false;
            auto goal = empty_board();
            goal.set(p);
            goal = grow(goal, 2) & ~walk_walls();
            return go(goal, true);
        } catch (const cambc::error&) {}
    }
    return false;
}

// Fight when the square law favours us, run otherwise. True when the turn is spent.
bool Bot::enemies() {
    auto& me = cambc::self();
    const int now = static_cast<int>(cambc::round_number());
    assess();
    std::vector<Foe> list;
    for (const auto& t : threats_)
        if (t.d <= 9 && steps_to_hit(t, pos_) <= kRelevantSteps) list.push_back({t.pos, t.hp, t.max_hp, t.dps, t.range});
    const int gems = static_cast<int>(me.gem_count());
    if (list.empty()) {
        // Help an ally who reported an invader close by.
        const auto s = read_sighting();
        if (s.ok && s.age <= 4 && cheb(s.where, pos_) <= 10 && me.health() > me.max_health() * 0.6f
            && gems < 30 && incoming_ == 0) {
            auto goal = empty_board();
            goal.set(s.where);
            goal = grow(goal, 2) & ~walk_walls();
            return go(goal, true);
        }
        return guard();
    }
    last_enemy_seen_ = now;
    write_sighting(list.front().pos);

    float their_hp = 0, their_dps = 0, lethal = 0;
    int dmin = 99;
    for (const auto& t : threats_) {
        if (t.d > 9 || steps_to_hit(t, pos_) > kRelevantSteps) continue;
        their_hp += t.hp;
        their_dps += t.dps;
        dmin = std::min(dmin, t.d);
    }
    lethal = danger_at(pos_, 1);
    const float hp = me.health(), max_hp = me.max_health();
    const auto r = reach_of(me.champion_name());
    const float my_dps = kPowerModel == 0 ? 1.1f * me.strength() + 0.25f * me.focus()
                                          : me.strength() * (1.0f + r.resets / 8.0f) + r.burst * me.focus() / 8.0f;
    const float our_hp = hp + help_hp_, our_dps = my_dps + help_dps_;
    float ratio = our_hp * our_dps / std::max(1.0f, their_hp * their_dps);
    const bool home = cambc::map::spawn_region(team::ally).is_set(pos_);
    if (home_.at(pos_) <= 3) ratio *= 1.5f;  // spawn heals us
    // Carried gems change hands on death: demand better odds the more we carry.
    const float need = 1.0f + std::min(0.6f, gems / 80.0f);

    // Gems at hand and an altar within reach: bank them if the tribute finishes before we could die.
    if (gems > 0 && !moved_ && tribute_now(true)) return true;

    bool run = ratio < need && !home;
    // About to be burst down alone: step back even when the totals look fine.
    if (!run && !home && lethal >= hp * 0.95f && help_hp_ <= 0) run = true;
    // Cornered: chasers as fast as we are would kill us before we reach home, so hit back.
    if (run && kCornered && dmin <= 2 && hp > 0) {
        const int hd = home_.at(pos_);
        float chase = 0;
        for (const auto& t : threats_)
            if (t.d <= 2 && t.range <= 1 && !t.stunned) chase += t.dps;
        if (hd > 3 && hd < kFar && chase * static_cast<float>(hd) * taken_champ_ >= hp && our_hp * our_dps >= 0.5f * their_hp * their_dps) {
            run = false;
        }
    }
    if (run) {
        // Far enemies that are not coming for us: carry on, but bank sooner (tribute checks sightings).
        if (dmin >= 6 && hp > max_hp * 0.5f && first_hit(pos_) > 2) return false;
        // Run: free hits and stuns on whoever is next to us (spells.cpp), recall once nobody can break it.
        flee_spells(list);
        const bool recall_ok = kRecallRule == 1 ? recall_safe()
            : kRecallRule == 2 ? incoming_ == 0 && first_hit(pos_) >= 4 : dmin >= 6 && incoming_ == 0;
        if (!moved_ && recall_ok) {
            if (live()) try { me.recall(); } catch (const cambc::error& ex) { warn("flee recall", ex); }
            return true;
        }
        if (kFleeMode == 0 || first_hit(pos_) > 1 || (!flee() && !moved_ && !move_failed_)) go_home();
        return true;
    }
    // Strong enough but nobody close: keep farming unless the odds invite a hunt.
    if (dmin > 5 && ratio < 1.6f) return false;
    // A chase that never lands a hit only wastes farming time.
    if (now < no_chase_until_ && dmin > me.attack_range() + 1) return false;

    // Fight: spells and basic attacks (spells.cpp), otherwise close in.
    if (!live()) return true;
    if (fight_spells(list)) { chase_turns_ = 0; return true; }
    if (cast_ && me.attack_range() > 1) return true;
    if (hp > max_hp * 0.3f) {
        if (++chase_turns_ > kChaseTurns) {
            chase_turns_ = 0;
            no_chase_until_ = now + 15;
            return false;
        }
        auto goal = empty_board();
        for (const auto& e : list) goal.set(e.pos);
        goal = grow(goal, me.attack_range()) & ~walk_walls();
        go(goal, dmin > 2);
        if (strike_after_move(list)) chase_turns_ = 0;
        return true;
    }
    if (!flee() && !moved_ && !move_failed_) go_home();
    return true;
}

// ---------------------------------------------------------------- gems

// Start a tribute at an altar beside us when the channel ends before anything can stop it.
// `pressed`: enemies are around, so accept any tribute that completes before we would die.
bool Bot::tribute_now(bool pressed) {
    auto& me = cambc::self();
    const int gems = static_cast<int>(me.gem_count());
    if (gems <= 0 || !live()) return false;
    const auto locks = cambc::vision::altar_lock_tiles(team::enemy);
    const int turns = std::max(1, (gems + 9) / 10);
    const float hp = me.health();
    for (auto a : cambc::map::altars().positions()) {
        if (cheb(a, pos_) > cambc::constants::submit_gems_range || locks.is_set(a)) continue;
        const float camp = incoming_ * taken_ * static_cast<float>(turns + 1);
        const float champs = danger_at(pos_, std::min(turns + 1, kHorizon));
        bool stunner = false;
        for (const auto& t : threats_)
            if (t.stun && !t.stunned && steps_to_hit(t, pos_) <= std::min(turns, kHorizon) - 1) stunner = true;
        const bool safe = hp > camp + champs + 40 && !(stunner && turns >= 2 && !pressed);
        if (!safe) return false;
        try { me.submit_gems(a); } catch (const cambc::error& e) { warn("submit", e); return false; }
        if (gems >= kGuardGems) write_claim(kGuardClaim);
        return true;
    }
    return false;
}

bool Bot::tribute() {
    auto& me = cambc::self();
    const int gems = static_cast<int>(me.gem_count());
    if (gems <= 0) return false;
    const auto locks = cambc::vision::altar_lock_tiles(team::enemy);
    bool beside = false;
    for (auto a : cambc::map::altars().positions())
        if (cheb(a, pos_) <= cambc::constants::submit_gems_range && !locks.is_set(a)) beside = true;
    if (beside) {
        if (tribute_now(false)) return true;
        // Not safe yet: an enemy close by. Wait out of its way, or carry on.
        if (!threats_.empty()) return false;
    }
    if (!locks.is_empty() && me.can_attack() && live())
        for (auto a : locks.positions())
            if (cheb(a, pos_) <= std::min(cambc::constants::submit_gems_range, me.attack_range())) {
                try {
                    me.attack(cambc::vision::altar_lock_at(a));
                    attacked_ = true;
                    return true;
                } catch (const cambc::error& e) { warn("lock", e); }
                break;
            }
    const int d = altar_.at(pos_);
    if (d >= kFar) return false;
    const int now = static_cast<int>(cambc::round_number());
    const int left = static_cast<int>(cambc::constants::max_rounds) - now;
    const auto s = read_sighting();
    const bool danger = (s.ok && s.age <= 30 && cheb(s.where, pos_) <= 16) || now - last_enemy_seen_ <= 20;
    const bool go_now = gems >= 45 || d <= 3 || (gems >= 20 && d <= 10) || (danger && gems >= 10)
        || target_ < 0 || target_wait_ > d || left < d + 25
        || cambc::global_submitted_gems(team::ally) + static_cast<unsigned>(gems) >= cambc::constants::winning_gems;
    if (!go_now) return false;
    auto goal = altar_spots_;
    for (auto a : locks.positions()) {
        auto one = empty_board();
        one.set(a);
        goal &= ~grow(one, cambc::constants::submit_gems_range);
    }
    if ((goal & ~camp_block_).is_empty()) {
        // Every spot is under a living camp (a boss beside the only altar): clear it first.
        if (now < altar_wait_until_) return false;
        for (auto t : (goal & camp_block_).positions()) {
            const int gi = group_at_[static_cast<size_t>(tidx(t))];
            if (gi >= 0) {
                if (plug_ != gi) { plug_ = gi; plug_since_ = now; }
                plug_altar_ = true;
                break;
            }
        }
        return false;
    }
    return go(goal & ~camp_block_, true);
}
