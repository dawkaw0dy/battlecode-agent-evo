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
// - tribute(): when and where the bag is banked; deny()/contest(): enemy tributes we see.
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
constexpr int kLockRange = 2;          // altar lock placement range
constexpr int kRaceGap = 120;          // this close to 1000 tributed, every bag is banked promptly
constexpr bool kContest = true;        // go for channelling enemies we can kill before the channel ends
constexpr bool kFreeShots = true;      // retreating ranged: shoot when nobody can hit us here next turn
constexpr float kMarginW = 1.5f;       // retreat: score per tile of distance beyond the chasers' reach (≤ 3)
constexpr float kDeathTile = 1000.0f;  // retreat: a tile where the next enemy turn kills us
constexpr int kRecallSafe = 5;         // recall when no enemy can hit us within this many of its turns
constexpr int kRecallWalk = 6;         // ... and home is further than this walk
constexpr bool kBankUnderFire = true;  // tribute mid-retreat beside an altar when we outlive the channel
constexpr bool kGuardTribute = true;   // cancel a tribute we would die in
constexpr bool kSafeStart = true;      // do not start a tribute we would die in
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

float ability_reach(const cambc::ability& ab) {
    float r = 4;
    try {
        const auto& info = ab.info();
        if (info) {
            if (auto* d = std::get_if<cambc::ability_damage>(&*info)) r = d->range;
            else if (auto* c = std::get_if<cambc::ability_cc>(&*info)) r = c->range;
        }
    } catch (const std::exception&) {}
    return r;
}

bool use_lock(vec2 a) {
    try { return cambc::self().consumables()[cambc::consumable_type::altar_lock].use(a); }
    catch (const cambc::error& e) { warn("lock", e); }
    return false;
}

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

// ---------------------------------------------------------------- banking

// Hit the enemy lock on altar a (in range): a tribute attempt there is the hit, and an attack reset
// right after it gives a second one (only right after our own attack: error 26 otherwise).
bool Bot::break_lock(vec2 a) {
    auto& me = cambc::self();
    if (!me.can_attack() || !live()) return true;  // wait here for the next hit
    try {
        me.submit_gems(a);
        attacked_ = true;
    } catch (const cambc::error& e) { warn("break lock", e); return false; }
    if (!cast_ && !me.can_attack() && me.can_cast() && fuel(8000) && live())
        for (const auto& ab : me.abilities())
            if (ab.is_attack_reset() && ab.is_ready()) {
                try {
                    ab.use();
                    cast_ = true;
                    if (me.can_attack()) me.submit_gems(a);
                } catch (const cambc::error& e) { warn("break lock reset", e); }
                break;
            }
    invalidate();
    return true;
}

// The only altar takes tributes only from a boss's spawn tiles (01boss-battle): while one of us stands on
// such a tile the boss cannot respawn, so the altar stays open to us, and the enemy can bank there only by
// killing us first. Stand there with an empty bag, and step off when an ally with gems waits beside us.
bool Bot::hold_tile() {
    if (hold_i_ < 0 || moved_ || !bit(alt_[static_cast<size_t>(hold_i_)].goal, pos_)) return false;
    auto& me = cambc::self();
    if (me.health() < me.max_health() * 0.4f || !me.can_move() || !live()) return false;
    const auto& goal = alt_[static_cast<size_t>(hold_i_)].goal;
    for (const auto& a : others_) {
        cambc::detail::champion_info info{};
        if (!peek(a, info)) continue;
        const vec2 p{info.x, info.y};
        if (!inside(p) || cheb(p, pos_) > 2) continue;
        int g = 0;
        try { g = static_cast<int>(a.gem_count()); } catch (const std::exception&) { continue; }
        if (g <= 0) continue;
        for (auto d : kDirs) {
            const vec2 t{pos_.x + d.x, pos_.y + d.y};
            if (!free_tile(t) || bit(goal, t) || bit(camp_block_, t) || bit(walk_walls(), t)) continue;
            bool ok = false;
            try { ok = me.move_absolute(t); } catch (const cambc::error& e) { warn("hold step", e); }
            moved_ = true;
            if (!ok) move_failed_ = true;
            invalidate();
            return true;
        }
        return false;
    }
    return true;
}

// When to walk to an altar (a full bag, a short walk, enemies about, the clock, the race), where (the
// nearest open altar by road; through an enemy lock only when it is close or every altar is locked), and
// when to start the channel (we outlive it: a death in it hands the whole bag to the killer).
bool Bot::tribute() {
    auto& me = cambc::self();
    const int gems = static_cast<int>(me.gem_count());
    eg_init();
    if (!eg_ready_) return false;
    eg_observe();
    if (gems <= 0) return hold_tile();
    const int now = round_now();
    const int range = cambc::constants::submit_gems_range;
    const auto locks = cambc::vision::altar_lock_tiles(team::enemy);
    const int channel = channel_ticks(gems);
    // Beside an altar: bank when we outlive the channel; a lock in the way is hit.
    int at_locked = -1;
    for (size_t i = 0; i < alt_.size(); ++i) {
        const vec2 a = alt_[i].at;
        if (cheb(a, pos_) > range) continue;
        if (bit(locks, a)) { at_locked = static_cast<int>(i); continue; }
        float dmg = incoming_ * taken_ * static_cast<float>(channel + 1);
        if (kSafeStart) dmg = std::max(dmg, channel_damage(pos_, channel, read_foes(10)));
        if (me.health() > dmg + 40) {
            if (live()) try { me.submit_gems(a); } catch (const cambc::error& e) { warn("submit", e); }
            return true;
        }
    }
    if (at_locked >= 0) return break_lock(alt_[static_cast<size_t>(at_locked)].at);

    const int d = altar_.at(pos_);  // nearest tribute tile we can walk to, locks aside
    const int left = static_cast<int>(cambc::constants::max_rounds) - now;
    const auto s = read_sighting();
    const bool danger = (s.ok && s.age <= 30 && cheb(s.where, pos_) <= 16) || now - last_enemy_seen_ <= 20;
    // The race's last stretch: our bags together may finish it (we only know our own), and a bag still
    // carried when the enemy gets there counts for nothing.
    const int ours = static_cast<int>(cambc::global_submitted_gems(team::ally));
    const int theirs = static_cast<int>(cambc::global_submitted_gems(team::enemy));
    const int win = static_cast<int>(cambc::constants::winning_gems);
    const bool race = ours >= win - kRaceGap || (theirs >= win - kRaceGap && ours >= win - 2 * kRaceGap);
    const bool go_now = gems >= 45 || d <= 3 || (gems >= 20 && d <= 10) || (danger && gems >= 10)
        || target_ < 0 || target_wait_ > d || left < d + channel + 20
        || (race && (gems >= 8 || d <= 8)) || ours + gems >= win;
    // No walk from here to any altar (a dash crossed holes): only the recall brings the bag back.
    const bool stranded = d >= kFar;
    if (stranded && !(gems >= 45 || (race && gems >= 8) || ours + gems >= win || left < home_altar_ + channel + 30)) return false;
    if (!go_now) return false;
    // Far by road: the spawn is a recall away and close to an altar.
    if (stranded && (incoming_ > 0 || moved_)) return false;
    if (d > home_altar_ + 10 && incoming_ == 0 && now >= ambush_until_ && !moved_ && live()) {
        try { me.recall(); } catch (const cambc::error& e) { warn("bank recall", e); }
        return true;
    }

    // Where: the nearest open altar by road (the route search picks it from all of them). An enemy lock
    // costs five hits: break one close by rather than walk away and back, or the nearest when every
    // open altar is locked.
    auto& goal = bank_goal_;
    int n_open = 0, n_cand = 0, near_locked = -1, any_locked = -1, nl_d = 1 << 20, al_d = 1 << 20;
    goal.clear();
    for (size_t i = 0; i < alt_.size(); ++i) {
        const auto& A = alt_[i];
        if (A.spots.empty()) continue;
        ++n_cand;
        bool open = false;
        for (auto t : A.spots)
            if (!bit(camp_block_, t)) { open = true; break; }
        if (!open) continue;
        if (enemy_locked(i)) {
            const int dd = cheb(A.at, pos_);
            if (dd <= 4 && dd < nl_d) { nl_d = dd; near_locked = static_cast<int>(i); }
            if (dd < al_d) { al_d = dd; any_locked = static_cast<int>(i); }
            continue;
        }
        goal |= A.goal;
        ++n_open;
    }
    if (near_locked >= 0 || (n_open == 0 && any_locked >= 0)) {
        goal.clear();
        goal |= alt_[static_cast<size_t>(near_locked >= 0 ? near_locked : any_locked)].goal;
    } else if (n_open == 0) {
        // Every tribute tile is under a living camp (a boss beside the only altar): clear it first.
        if (n_cand == 0 || now < altar_wait_until_) return false;
        int near_d = 1 << 20;
        for (const auto& A : alt_) {
            const int dd = cheb(A.at, pos_);
            if (A.spots.empty() || dd >= near_d) continue;
            for (auto t : A.spots) {
                const int gi = group_at_[static_cast<size_t>(tidx(t))];
                if (gi >= 0 && bit(camp_block_, t)) {
                    near_d = dd;
                    if (plug_ != gi) { plug_ = gi; plug_since_ = now; }
                    plug_altar_ = true;
                    break;
                }
            }
        }
        return false;
    }
    andnot_into(goal, camp_block_, goal);
    const bool went = go(goal, true);
    // Every tribute tile taken, by an ally among others (one tile, a long channel): wait beside it.
    if (!went && d <= 3 && !moved_) {
        const auto& v = view();
        bool all_taken = true, ally = false;
        for (auto t : goal.positions()) {
            if (!v.lchamps.at(t)) { all_taken = false; break; }
            for (auto a : v.allies)
                if (a == t) ally = true;
        }
        if (all_taken && ally) return true;
    }
    if (kTraceBank && (now % 10 == 0 || !went))
        std::cout << "bank r" << now << " slot" << slot_ << " gems " << gems << " d " << d << " open " << n_open
                  << " went " << went << '\n';
    return went;
}

// ---------------------------------------------------------------- enemy tributes

// An enemy tributing near us cannot move, hit or cast until its channel ends: when we kill it first its bag
// is ours (and its team loses the trip). Otherwise a lock on its altar cancels the channel (walking into
// lock range when we arrive in time).
bool Bot::deny() {
    eg_init();
    eg_observe();
    if (!eg_ready_ || !live()) return false;
    auto& me = cambc::self();
    const auto& v = view();
    if (v.foes.is_empty()) return false;
    vec2 aim{-1, -1};
    int aim_gems = 0, aim_left = 0;
    float aim_hp = 0;
    std::vector<vec2> free_foes;
    for (auto p : v.foes.positions()) {
        if (cheb(p, pos_) > 10 || !v.lvis.at(p)) continue;
        try {
            const auto c = cambc::vision::champion_at(p);
            if (!c.is_alive()) continue;
            if (!c.is_submitting_gems()) { free_foes.push_back(p); continue; }
            const int g = static_cast<int>(c.gem_count());
            if (g < 5 || g <= aim_gems) continue;
            aim = p;
            aim_gems = g;
            aim_left = channel_ticks(g);
            try { aim_left = c.remaining_channel_time(); } catch (const cambc::error&) {}
            aim_hp = c.health();
            try {
                const auto b = c.buffs();
                if (b.has_shield()) aim_hp += b.get<cambc::buff_shield>().value;
            } catch (const std::exception&) {}
        } catch (const cambc::error&) {}
    }
    if (aim_gems == 0) return false;
    if (kContest && contest(aim, aim_left, aim_hp, free_foes)) return true;
    if (!has_lock()) return false;
    const auto locked = cambc::vision::altar_lock_tiles();
    int target = -1, best = 99;
    for (size_t i = 0; i < alt_.size(); ++i) {
        const vec2 a = alt_[i].at;
        if (cheb(a, aim) > cambc::constants::submit_gems_range || bit(locked, a)) continue;
        const int d = cheb(a, pos_);
        if (d < best) { best = d; target = static_cast<int>(i); }
    }
    if (target < 0 || best > 9) return false;
    const auto& A = alt_[static_cast<size_t>(target)];
    if (best <= kLockRange) {
        if (use_lock(A.at)) invalidate();
        return false;  // the rest of the turn is free: fight on
    }
    // Close enough to arrive before the channel ends?
    if (best - kLockRange > aim_left + 1) return false;
    if (me.health() < me.max_health() * 0.35f) return false;
    if (A.lock_goal.is_empty() || !go(A.lock_goal, true)) return false;
    // Arrived this step: drop it now.
    if (cheb(A.at, me.position()) <= kLockRange && live() && use_lock(A.at)) invalidate();
    return true;
}

// Go for a channelling enemy at p when we (with the allies beside it) kill it before the channel ends and
// no free enemy is close enough to make it a real fight (the fight model decides those). Damage only: a
// stun would cancel the channel and free it. True when the turn went into it.
bool Bot::contest(vec2 p, int left, float hp, const std::vector<vec2>& free_foes) {
    auto& me = cambc::self();
    for (auto f : free_foes)
        if (cheb(f, pos_) <= 7 || cheb(f, p) <= 5) return false;
    if (me.health() < me.max_health() * 0.3f || incoming_ * taken_ * 2 > me.health()) return false;
    const int range = me.attack_range();
    const bool melee = range <= 1;
    const int d = cheb(p, pos_);
    const float str = me.strength();
    bool reset = false;
    float spell = 0;
    for (const auto& ab : me.abilities()) {
        if (!ab.is_ready()) continue;
        if (ab.is_attack_reset()) reset = true;
        else if (ab.is_damage() && !ab.is_cc() && ab.can_target_enemies()) try { spell += ab.damage_amount(); } catch (const std::exception&) {}
    }
    int helpers = 0;
    for (auto a : view().allies)
        if (cheb(a, p) <= 3) ++helpers;
    const float per_turn = str * (1 + helpers);
    const int reach = std::max(0, melee ? d - 2 : d - range);
    const float first = (reset ? str : 0.0f) + spell;
    const int kill = static_cast<int>(std::ceil(std::max(0.0f, hp - first) / std::max(1.0f, per_turn)));
    if (reach + kill > left) return false;
    auto handle = [&]() -> std::optional<cambc::champion> {
        try {
            if (!cambc::vision::has_champion(p)) return std::nullopt;
            auto c = cambc::vision::champion_at(p);
            if (!c.is_visible() || !c.is_alive()) return std::nullopt;
            return c;
        } catch (const cambc::error&) {}
        return std::nullopt;
    };
    // Damage spells first (most exclude walking).
    if (!cast_ && !moved_ && me.can_cast() && live())
        for (const auto& ab : me.abilities()) {
            if (cast_ || !ab.is_ready() || !ab.is_damage() || ab.is_cc() || !ab.can_target_enemies()) continue;
            if (d > static_cast<int>(ability_reach(ab))) continue;
            auto c = handle();
            if (!c) break;
            try {
                ab.use(*c);
                cast_ = true;
                bool exclusive = true;
                try {
                    const auto& info = ab.info();
                    if (info) exclusive = std::visit([](const auto& x) { return x.exclusive_with_movement; }, *info);
                } catch (const std::exception&) {}
                if (exclusive) move_failed_ = true;
                invalidate();
            } catch (const cambc::error& e) { warn("contest spell", e); }
        }
    auto hit = [&]() {
        if (!me.can_attack() || !live() || cheb(p, me.position()) > range) return;
        auto c = handle();
        if (!c) return;
        try {
            me.attack(*c);
            attacked_ = true;
            invalidate();
        } catch (const cambc::error& e) { warn("contest hit", e); return; }
        if (!cast_ && me.can_cast() && fuel(8000) && live())
            for (const auto& ab : me.abilities())
                if (ab.is_attack_reset() && ab.is_ready()) {
                    try { if (!me.can_attack()) { ab.use(); cast_ = true; } } catch (const cambc::error& e) { warn("contest reset", e); break; }
                    auto c2 = handle();
                    if (c2 && me.can_attack()) try { me.attack(*c2); } catch (const cambc::error& e) { warn("contest hit2", e); }
                    break;
                }
    };
    hit();
    if (attacked_ && !melee) return true;
    if (cheb(p, me.position()) > range && !move_failed_) {
        auto goal = empty_board();
        goal.set(p);
        goal = grow(goal, range);
        andnot_into(goal, walk_walls(), goal);
        go(goal, cheb(p, me.position()) > range + 1);
        pos_ = me.position();
        hit();
    }
    return true;
}
