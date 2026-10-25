// Defense: the score we have tributed and the gems we carry (rewritten in f09-01 behind the parent's calls:
// retreat(), tribute(), deny()/contest(), plus guard_channel(), evade()/scout(), bag_need() from turn()).
//
// - Ambushes: enemies standing in bushes see and hit us while we cannot see them (14% of the parent's deaths
//   vs f08-05: it recalled in place, the next hit broke the recall, and it stood there until dead). Damage
//   between our turns that no visible enemy and no camp in reach explains marks an ambush (sense_ambush()):
//   our ward goes into the unseen bush within reach (a hidden occupant is revealed even when the placement
//   fails) so the fight model sees the shooter; failing that, no recall while it lasts, and a hurt champion
//   steps away from unseen bushes towards home (evade()). While enemies are about, our ward also goes into
//   unseen bushes beside us before they shoot (scout()).
// - Carried gems change hands on death (the parent lost ~130 a game that way): a carried bag raises the fight
//   value needed to engage (bag_need()), a fleeing carrier runs for the nearest altar when it is nearer than
//   home and banks under fire beside it when it outlives the channel (a tribute survives damage, not death
//   or a stun), no tribute is started that we would die in, and a running one we would die in is cancelled.
// - One enemy model (read_fighter()): for a tile, the first enemy turn on which each enemy can hit it if it
//   chases (hit_turn()), and the damage it pours in from then on (channel_damage()).
// - retreat() keeps the parent's tile score and recall rule (the margin score and chase-based recall timing
//   below measured no better); a champion boxed in where it stands hits back instead of idling.
// - tribute(), deny()/contest(): endgame.cpp (rewritten in f10-03).
#include <cambc.h>
#include "bot.h"
#include "fuel.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <variant>

void dlog(const std::string& x);

namespace {
constexpr bool kTraceBank = false;
constexpr bool kTraceDef = false;     // retreat actions into the death trace (bot.cpp)
constexpr bool kFreeShots = true;      // retreating ranged: shoot when nobody can hit us here next turn
constexpr float kMarginW = 1.5f;       // retreat: score per tile of distance beyond the chasers' reach (≤ 3)
constexpr float kDeathTile = 1000.0f;  // retreat: a tile where the next enemy turn kills us
constexpr int kRecallSafe = 5;         // recall when no enemy can hit us within this many of its turns
constexpr int kRecallWalk = 6;         // ... and home is further than this walk
constexpr bool kBankUnderFire = true;  // tribute mid-retreat beside an altar when we outlive the channel
constexpr bool kGuardTribute = true;   // cancel a tribute we would die in
constexpr float kRecoverHp = 0.45f;    // enemies about: go home below this health fraction
constexpr int kRecoverRounds = 12;     // ... "about": seen by us this recently
constexpr bool kRecover = false;       // recover() on
constexpr bool kNewScore = false;      // retreat tile score with margins and death tiles (else the parent's)
constexpr bool kStayStrike = true;     // boxed in: hit back
constexpr float kBagNeed = 3.0f;       // fight value needed to engage, per carried gem
constexpr bool kEvade = true;          // hit by an unseen enemy: walk away from the bushes it hides in
constexpr int kAmbushRounds = 4;       // ... for this long after the last such hit
constexpr bool kWardReveal = true;     // ... and ward the bush it shoots from
constexpr bool kScout = true;          // ward unseen bushes beside us while enemies are about
constexpr int kScoutRounds = 40;       // ... "about": seen by us, or reported near us, this recently
constexpr int kScoutMin = 3;           // ... a bush of at least this many unseen tiles around the ward
constexpr int kScoutKeep = 5;          // ... unless our ward is already this close
constexpr int kFleeAltarGems = 10;     // fleeing with this many gems: run for the nearest altar when nearer than home
constexpr bool kChaseRecall = false;   // recall timing from hit_turn() (else: everyone at least kRecallDist away)
constexpr int kRecallDist = 7;         // retreat: recall once every enemy is at least this far
constexpr int kCancelDist = 5;         // ... and a running recall stops when an enemy comes this close

int round_now() { return static_cast<int>(cambc::round_number()); }

int channel_ticks(int gems) { return std::max(1, (gems + 9) / 10); }

// First enemy turn (1, 2, ...) on which e can damage a champion standing still at t, if it chases.
int hit_turn(const Fighter& e, vec2 t) {
    const int d = cheb(e.pos, t);
    const int dash = e.dasher ? 2 : 0;
    int n;
    if (!e.ranged) {
        n = std::max(1, d - e.range - dash);
    } else {
        const int steps = std::max(0, d - e.range - dash);
        n = d <= e.range ? 1 : steps + 1;  // walking takes the turn; a dash and a shot may share one
    }
    if (e.spell_reach > 0) {
        const int steps = std::max(0, d - e.spell_reach);
        n = std::min(n, steps == 0 ? 1 : steps + 1);  // spells mostly exclude walking
    }
    return n + (e.stunned ? 1 : 0);
}

// Tiles beyond e's reach at t next turn (negative: inside it).
int reach_margin(const Fighter& e, vec2 t) {
    const int reach = e.ranged ? std::max(e.range, e.spell_reach)
                               : std::max(e.range + 1 + (e.dasher ? 2 : 0), e.spell_reach);
    return cheb(e.pos, t) - reach;
}
}  // namespace

// ---------------------------------------------------------------- the enemy model

std::vector<Fighter> Bot::read_foes(int radius) const {
    std::vector<Fighter> out;
    const auto& v = view();
    if (v.foes.is_empty()) return out;
    const auto& enemy_spawn = cambc::map::spawn_region(team::enemy);
    for (auto p : v.foes.positions()) {
        if (cheb(p, pos_) > radius || enemy_spawn.is_set(p)) continue;
        try {
            Fighter f;
            if (read_fighter(cambc::vision::champion_at(p), p, f)) out.push_back(f);
        } catch (const std::exception&) {}
        if (out.size() >= 8) break;
    }
    return out;
}

// Damage we take standing at t for the next `turns` enemy turns (they chase; camp fire included).
float Bot::channel_damage(vec2 t, int turns, const std::vector<Fighter>& theirs) const {
    float dmg = incoming_ * taken_ * static_cast<float>(turns);
    for (const auto& e : theirs) {
        const int n = hit_turn(e, t);
        if (n > turns) continue;
        dmg += e.burst + e.sustain * static_cast<float>(turns - n + 1);
    }
    return dmg;
}

// ---------------------------------------------------------------- our channels

// Recall or tribute in progress (turn start): cancel it when an enemy will break it or kill us in it.
// True when the channel goes on.
bool Bot::guard_channel() {
    auto& me = cambc::self();
    const bool recalling = me.is_recalling();
    int left = 1;
    try { left = std::max(1, static_cast<int>(me.remaining_channel_time())); } catch (const cambc::error&) {}
    const auto theirs = read_foes(10);
    if (theirs.empty()) return true;
    if (recalling) {
        // Damage cancels a recall anyway: stop now and move while there is time.
        bool broken = false;
        for (const auto& e : theirs)
            if (kChaseRecall ? hit_turn(e, pos_) <= left : cheb(e.pos, pos_) <= kCancelDist) broken = true;
        if (!broken) return true;
        try { me.cancel_recall(); } catch (const cambc::error& e) { warn("cancel recall", e); return true; }
        invalidate();
        return false;
    }
    // A tribute survives damage, not a death: give it up only when we die before it lands.
    if (!kGuardTribute) return true;
    if (channel_damage(pos_, left, theirs) < me.health()) return true;
    try { me.cancel_submit_gems(); } catch (const cambc::error& e) { warn("cancel tribute", e); return true; }
    invalidate();
    return false;
}

// Retreating with gems beside an open altar: bank them when we outlive the channel (a stun would break it).
bool Bot::bank_under_fire(const std::vector<Fighter>& theirs) {
    if (!kBankUnderFire || !eg_ready_ || moved_ || !live()) return false;
    auto& me = cambc::self();
    const int gems = static_cast<int>(me.gem_count());
    if (gems <= 0) return false;
    const int range = cambc::constants::submit_gems_range;
    const auto locks = cambc::vision::altar_lock_tiles(team::enemy);
    for (const auto& A : alt_) {
        if (cheb(A.at, pos_) > range || bit(locks, A.at)) continue;
        const int k = channel_ticks(gems);
        for (const auto& e : theirs)
            if (e.stun && hit_turn(e, pos_) <= k) return false;
        if (channel_damage(pos_, k, theirs) >= me.health() * 0.9f) return false;
        try { me.submit_gems(A.at); } catch (const cambc::error& e) { warn("submit under fire", e); return false; }
        if (kTraceDef) dlog("  bank under fire");
        invalidate();
        return true;
    }
    return false;
}

// Hurt while enemies are about: go home now rather than farm into them.
bool Bot::recover() {
    auto& me = cambc::self();
    const int now = round_now();
    if (!kRecover || now - last_enemy_seen_ > kRecoverRounds) return false;
    if (me.health() >= me.max_health() * kRecoverHp || incoming_ > 0) return false;
    return recall_or_walk(4);
}

// A fight we would take empty-handed is not worth a bag: the killer banks it.
float Bot::bag_need(int gems) const { return kBagNeed * static_cast<float>(gems); }

// ---------------------------------------------------------------- ambushes

// Damage between our turns that neither a visible enemy nor camp fire explains: an enemy in a bush (it
// sees out, we do not see in). A recall started there is broken by the next hit, turn after turn.
void Bot::sense_ambush() {
    auto& me = cambc::self();
    const int now = round_now();
    const float hp = me.health();
    const bool fresh = prev_round_ == now - 1 && prev_hp_ > 0;
    const float drop = prev_hp_ - hp;
    prev_hp_ = hp;
    prev_round_ = now;
    if (!kEvade || !fresh || drop <= 1) return;
    const auto& v = view();
    if (!v.foes.is_empty())
        for (auto p : v.foes.positions())
            if (cheb(p, pos_) <= 5) return;  // a visible enemy did it: the fight model handles that
    // Camp hits are not always in incoming_ (a camp that hit us and died since): count every living camp
    // that reaches us (or nearly) as if it had hit us.
    float camps = 0;
    for (int dy = -6; dy <= 6; ++dy)
        for (int dx = -6; dx <= 6; ++dx) {
            const vec2 t{pos_.x + dx, pos_.y + dy};
            if (!inside(t) || !(bit(camp_block_, t) || v.lcamps.at(t))) continue;
            const int gi = group_at_[static_cast<size_t>(tidx(t))];
            const int ci = camp_at_[static_cast<size_t>(tidx(t))];
            if (gi < 0 || ci < 0) continue;
            const auto s = camp_spec(g_[gi].kinds[static_cast<size_t>(ci)]);
            if (cheb(t, pos_) <= s.range + 1) camps += s.str;
        }
    const float unexplained = drop - std::max(incoming_ * taken_, camps) - 10.0f;
    if (unexplained <= 0) return;
    ambush_until_ = now + kAmbushRounds;
    hidden_hit_ = unexplained;
    int nb = 0, nbv = 0, nvis = 0;
    if (kTraceDef) {
        if (!bush_.p) bush_.bind(cambc::map::bushes());
        for (int dy = -4; dy <= 4; ++dy)
            for (int dx = -4; dx <= 4; ++dx) {
                const vec2 b{pos_.x + dx, pos_.y + dy};
                if (v.lvis.at(b)) ++nvis;
                if (bush_.at(b)) { if (v.lvis.at(b)) ++nbv; else ++nb; }
            }
    }
    if (kTraceDef) std::cout << "AMB bush_unseen " << nb << " bush_seen " << nbv << " vis " << nvis << " stun " << me.buffs().has_stun() << " r" << now << ' ' << cambc::to_string(me.champion_name()) << " drop " << drop << " inc " << incoming_
                             << " hp " << hp << " at " << pos_.x << ',' << pos_.y << " foes " << v.foes.count() << " allies " << v.allies.size() << '\n';
}

// Our ward on the unseen bush tile within reach that shows the most unseen bush around it (at least
// min_n tiles). True when placed (or a hidden occupant was revealed trying).
bool Bot::ward_bushes(int min_n) {
    auto& me = cambc::self();
    if (!fuel(20000) || !live()) return false;
    int cd = 1;
    try { cd = me.ward_cooldown(); } catch (const cambc::error&) {}
    if (cd > 0) return false;
    if (!bush_.p) bush_.bind(cambc::map::bushes());
    const auto& v = view();
    vec2 best{-1, -1};
    int best_n = min_n - 1;
    try {
        const auto tiles = me.ward_placement_tiles();
        if (!tiles.is_empty())
            for (auto t : tiles.positions()) {
                if (!bush_.at(t) || v.lvis.at(t) || cheb(t, pos_) > 2) continue;
                int n = 0;
                for (int dy = -2; dy <= 2; ++dy)
                    for (int dx = -2; dx <= 2; ++dx) {
                        const vec2 b{t.x + dx, t.y + dy};
                        if (bush_.at(b) && !v.lvis.at(b)) ++n;
                    }
                if (n > best_n) { best_n = n; best = t; }
            }
    } catch (const cambc::error& e) { warn("ward tiles", e); return false; }
    if (best.x < 0) return false;
    try { me.place_ward(best); } catch (const cambc::error& e) { warn("ward", e); return false; }
    invalidate();
    return true;
}

// Bushes beside us hide enemies that see us: keep our ward in them while enemies are about.
void Bot::scout() {
    if (!kScout || !live()) return;
    const int now = round_now();
    if (now - last_enemy_seen_ > kScoutRounds) {
        const auto s = read_sighting();
        if (!s.ok || s.age > kScoutRounds || cheb(s.where, pos_) > 16) return;
    }
    if (spawn_local_.at(pos_)) return;
    try {
        for (auto w : cambc::self().ward_positions())
            if (cheb(w, pos_) <= kScoutKeep) return;  // ours already covers this ground
    } catch (const cambc::error&) { return; }
    ward_bushes(kScoutMin);
}

// Under fire from an unseen enemy: no recall (it would be broken), but steps away from the bushes it may
// hide in, towards home; while healthy, carry on. True when the turn is spent.
bool Bot::evade() {
    if (!kEvade || round_now() >= ambush_until_) return false;
    auto& me = cambc::self();
    const auto& v = view();
    if (!v.foes.is_empty())
        for (auto p : v.foes.positions())
            if (cheb(p, pos_) <= 9 && !cambc::map::spawn_region(team::enemy).is_set(p)) return false;
    if (spawn_local_.at(pos_)) return false;
    if (!bush_.p) bush_.bind(cambc::map::bushes());
    // Our ward in the bush shows the shooter to the fight model (a hidden occupant is revealed even when
    // the placement fails).
    if (kWardReveal && ward_bushes(1)) {
        const auto& v2 = view();
        if (!v2.foes.is_empty())
            for (auto p : v2.foes.positions())
                if (cheb(p, pos_) <= 9) return false;  // found: the fight model takes over
    }
    const auto& vv = view();
    const float hp = me.health();
    if (hp > me.max_health() * 0.5f && hp > 6.0f * hidden_hit_) return false;
    if (moved_ || move_failed_ || !me.can_move() || !live()) return false;
    // Unseen bush tiles within 4 (basic range 3, spells 4) of t: where the shooter may stand.
    auto cover = [&](vec2 t) {
        int n = 0;
        for (int dy = -4; dy <= 4; ++dy)
            for (int dx = -4; dx <= 4; ++dx) {
                const vec2 b{t.x + dx, t.y + dy};
                if (bush_.at(b) && !vv.lvis.at(b)) ++n;
            }
        return n;
    };
    auto score = [&](vec2 t) {
        if (spawn_local_.at(t)) return -100.0f;
        const int hd = home_.at(t);
        return 2.0f * static_cast<float>(cover(t)) + static_cast<float>(hd >= kFar ? 200 : hd)
            + (camp_block_.is_set(t) ? 500.0f : 0.0f);
    };
    vec2 best = pos_;
    float best_s = score(pos_) + 0.5f;
    for (auto d : kDirs) {
        const vec2 t{pos_.x + d.x, pos_.y + d.y};
        if (!free_tile(t)) continue;
        const float s = score(t);
        if (s < best_s) { best_s = s; best = t; }
    }
    if (best == pos_) {
        go_home();
        return true;
    }
    moved_ = true;
    bool ok = false;
    try { ok = me.move_absolute(best); } catch (const cambc::error& e) { warn("evade", e); }
    invalidate();
    if (!ok) move_failed_ = true;
    else pos_ = me.position();
    return true;
}

// ---------------------------------------------------------------- retreat

// Run from the enemies. True when the turn is spent.
bool Bot::retreat(std::vector<Fighter>& theirs, int dmin) {
    (void)dmin;
    auto& me = cambc::self();
    if (bank_under_fire(theirs)) return true;
    const bool on_spawn = spawn_local_.at(pos_);
    if (on_spawn) {
        // The spawn heals us every turn and they cannot step on it: hold it and shoot from there.
        if (++spawn_hold_ > 20) return false;
        strike(theirs);
        return true;
    }
    spawn_hold_ = 0;
    // Nobody can break a recall before it lands: take it (damage would cancel it).
    {
        int first = 99;
        for (const auto& e : theirs) first = std::min(first, kChaseRecall ? hit_turn(e, pos_) : cheb(e.pos, pos_));
        if (first >= (kChaseRecall ? kRecallSafe : kRecallDist) && round_now() >= ambush_until_ && !moved_ && incoming_ == 0 && live() && home_.at(pos_) > kRecallWalk) {
            try { me.recall(); } catch (const cambc::error& e) { warn("flee recall", e); }
            if (kTraceDef) dlog("  recall");
            return true;
        }
    }
    // Melee: a free swing at an adjacent enemy before stepping away.
    if (me.attack_range() <= 1 && me.can_attack() && live())
        for (const auto& e : theirs)
            if (cheb(e.pos, pos_) <= 1 && view().lvis.at(e.pos)) {
                try {
                    if (!cambc::vision::has_champion(e.pos)) break;
                    me.attack(cambc::vision::champion_at(e.pos));
                    attacked_ = true;
                    invalidate();
                } catch (const cambc::error& ex) { warn("swing", ex); }
                break;
            }
    // Ranged: a free shot when nobody can hit us here next turn.
    if (kFreeShots && me.attack_range() > 1 && me.can_attack() && !moved_ && incoming_ == 0 && live()
        && threat_at(pos_, theirs) == 0 && strike(theirs)) {
        if (kTraceDef) dlog("  freeshot");
        return true;
    }
    if (moved_ || move_failed_ || !me.can_move() || !live()) { if (kTraceDef) dlog(std::string("  nomove m") + std::to_string(moved_) + " f" + std::to_string(move_failed_) + " c" + std::to_string(cast_) + " a" + std::to_string(attacked_)); return true; }
    const float hp = me.health();
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
    // With a bag, the nearest altar banks it under fire (bank_under_fire()) where home only keeps it.
    const int gems = static_cast<int>(me.gem_count());
    const bool to_altar = gems >= kFleeAltarGems && eg_ready_ && altar_.at(pos_) < home_.at(pos_);
    auto dest = [&](vec2 t) { return to_altar ? altar_.at(t) : home_.at(t); };
    auto score = [&](vec2 t) {
        float danger = threat_at(t, theirs);
        for (size_t i = 0; i < n_hot; ++i)
            if (cheb(t, hot[i].first) <= hot[i].second.range) danger += hot[i].second.str * taken_;
        const bool sp = spawn_local_.at(t);
        if (!kNewScore) {
            float s = 4.0f * danger * (sp ? 0.1f : 1.0f);
            const int hd = dest(t);
            s += static_cast<float>(hd >= kFar ? 200 : hd);
            if (camp_block_.is_set(t)) s += 500;
            return s;
        }
        float s = 4.0f * danger * (sp ? 0.1f : 1.0f);
        if (!sp && danger >= hp) s += kDeathTile;
        const int hd = sp && !to_altar ? 0 : dest(t);
        s += static_cast<float>(hd >= kFar ? 200 : hd);
        int margin = 3;
        for (const auto& e : theirs)
            if (!e.stunned) margin = std::min(margin, reach_margin(e, t));
        s -= kMarginW * static_cast<float>(std::max(-2, margin));
        if (camp_block_.is_set(t)) s += 500;
        if (t == pos_) s += 0.25f;  // ties: keep moving
        return s;
    };
    const float here = score(pos_);
    // Dash out when it beats every step.
    if (dasher_ && !cast_ && me.can_cast() && fuel(15000) && threat_at(pos_, theirs) > 0) {
        float best_step = here;
        for (auto d : kDirs) {
            const vec2 t{pos_.x + d.x, pos_.y + d.y};
            if (free_tile(t) && !camp_block_.is_set(t)) best_step = std::min(best_step, score(t));
        }
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
            for (size_t i = 0; i < n && i < 3 && cand[i].first < (kNewScore ? best_step : here) - 2; ++i) {
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
    if (kTraceDef) dlog("  step " + std::to_string(best.x) + ',' + std::to_string(best.y) + " s" + std::to_string(static_cast<int>(best_s)) + " here" + std::to_string(static_cast<int>(here)) + " atk" + std::to_string(attacked_));
    if (best == pos_) {
        // Boxed in: we take the hits here anyway, so hit back.
        if (kStayStrike && me.can_attack() && live()) strike(theirs);
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

// Banking, enemy tributes, altar locks and the boss-altar hold: endgame.cpp.
