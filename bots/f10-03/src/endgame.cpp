// Endgame (rewritten in f10-03 behind the parent's calls: tribute(), deny()/contest(), lock_altars(),
// lock_needed(), hold_tile(), break_lock(), eg_init()/eg_observe()).
//
// What the rules make of the late game:
// - Carried gems count for nothing until tributed, and a death hands the whole bag to the killer (a camp
//   kill destroys it). A bag is a liability that grows with its size, with the time we keep carrying it
//   and with the danger about; banking costs the detour off our way to the next camp. bank_due() weighs
//   the two with our own record this game: deaths per turn alive and gems picked up per turn alive.
// - The first team to 1000 tributed wins at once: a bag that takes us there goes in before anything
//   else (beside an altar it is tributed whatever the danger), and near the line every bag goes in. At
//   round 2000 carried gems are worth nothing: every bag is in with its walk (or recall) and channel to spare.
// - Altar locks: ours blocks only the enemy, theirs only us, five hits clear one, and placing one cancels
//   the enemy channels at that altar. A tribute attempt at a locked altar hits the lock; a basic attack
//   on the lock hits it from attack range, without a bag.
// - An enemy in its tribute channel cannot move, hit or cast, and damage does not stop the channel: kill
//   it before the channel ends and its bag is ours, else our lock on its altar cancels it. When the enemy
//   is about to win the race, every enemy tribute we can stop is worth a detour.
// - A boss whose spawn tiles are the only tribute tiles of the only altar (01boss-battle) respawns only
//   while all of its tiles are empty: one of us on a boss tile beside the altar keeps it open for us. A
//   boss tile that takes no tribute is held first, so allies bank beside the holder.

#include <cambc.h>
#include "bot.h"
#include "fuel.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <optional>
#include <variant>

namespace {
constexpr bool kTraceEg = false;
// Switches back to the parent's behaviour (all false/0: f09-01's endgame, move for move).
constexpr bool kTrack = false;          // keep our record and the score history (bank model, race posture)
constexpr bool kBankModel = false;      // bank_due(): risk against detour (else the parent's thresholds)
constexpr bool kRaceDeny = false;       // behind in the race: deny every enemy tribute, detour further for locks
constexpr int kHoldMode = 0;           // boss altar: 0 the parent's (stay after banking), 1 take up the hold actively
constexpr int kLockRange = 2;          // altar lock placement range
constexpr int kEnemyLockMemory = 200;  // an enemy lock seen this long ago may still stand
constexpr int kOurLockMemory = 150;    // ... and ours
// Banking (bank_due()).
constexpr float kSwing = 1.6f;         // a bag lost on death costs this many times its size (the killer banks it)
constexpr float kHazardPrior = 0.004f; // deaths per turn alive, before our own record counts
constexpr float kRatePrior = 0.15f;    // gems picked up per turn alive, before our own record counts
constexpr float kPriorTurns = 200.0f;  // weight of those priors, in turns
constexpr float kDangerMul = 4.0f;     // death rate while enemies are about
constexpr float kClearTurns = 12.0f;   // turns to clear and capture the next target
constexpr int kFullBag = 60;           // always bank a bag this big
constexpr int kRaceGap = 120;          // this close to 1000 tributed, every bag is banked promptly
constexpr int kClockPad = 15;          // turns to spare when banking before round 2000
constexpr bool kWinAnyway = false;      // a bag that wins the game is tributed beside an altar whatever the danger
// Locks.
constexpr bool kAttackLock = false;     // hit enemy locks with basic attacks from attack range
constexpr bool kRunner = true;         // the last pick carries locks to far altars (bot.cpp asks with a long reach)
constexpr int kLockDetour = 8;         // a lock detour for anybody else (bot.cpp's kLockDetour)
constexpr int kDenyDetour = 16;        // ... while the enemy is about to win the race
// Enemy tributes.
constexpr bool kContest = true;        // go for channelling enemies we can kill before the channel ends
constexpr int kDenyGap = 200;          // "about to win": this close to 1000 and finishing before us
// The boss altar.
constexpr bool kHoldBossTile = false;  // hold a boss tile that takes no tribute rather than the tribute tile
constexpr int kHoldWalk = 8;           // walk this far to take up the hold
constexpr float kHoldHp = 0.4f;        // ... and hold it above this health fraction
constexpr int kHoldEarly = 20;         // ... from this many rounds before the boss's respawn is due

int round_now() { return static_cast<int>(cambc::round_number()); }
int channel_ticks(int gems) { return std::max(1, (gems + 9) / 10); }
int win_gems() { return static_cast<int>(cambc::constants::winning_gems); }
int left_rounds() { return static_cast<int>(cambc::constants::max_rounds) - round_now(); }

bool use_lock(vec2 a) {
    if (kTraceEg) std::cout << "LOCK r" << round_now() << " altar " << a.x << ',' << a.y << '\n';
    try { return cambc::self().consumables()[cambc::consumable_type::altar_lock].use(a); }
    catch (const cambc::error& e) { warn("lock", e); }
    return false;
}

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
}  // namespace

// ---------------------------------------------------------------- altar model

void Bot::eg_init() {
    // Nothing to bank or lock in the first rounds, which the map analysis already fills with work.
    if (eg_ready_ || round_now() < 6 || !fuel(85000) || !live()) return;
    const int ar = cambc::constants::submit_gems_range;
    alt_.clear();
    alt_.resize(altars_.size());
    enemy_use_.assign(altars_.size(), -1000);
    bool dropped = false;
    for (size_t i = 0; i < altars_.size(); ++i) {
        Altar& A = alt_[i];
        A.at = altars_[i];
        A.goal.clear();
        A.lock_goal.clear();
        for (int dy = -kLockRange; dy <= kLockRange; ++dy)
            for (int dx = -kLockRange; dx <= kLockRange; ++dx) {
                const vec2 t{A.at.x + dx, A.at.y + dy};
                if (!inside(t) || bit(walk_walls(), t) || home_.at(t) >= kFar) {
                    if (inside(t) && cheb(t, A.at) <= ar && !bit(walk_walls(), t)) dropped = true;
                    continue;
                }
                raw_set(A.lock_goal.ptr(), t);
                if (cheb(t, A.at) > ar) continue;
                A.spots.push_back(t);
                raw_set(A.goal.ptr(), t);
            }
    }
    // The only altar we can reach takes tributes only from a boss's spawn tiles: hold one (hold_tile()).
    hold_i_ = -1;
    hold_g_ = -1;
    hold_spots_.clear();
    int reachable = 0;
    for (size_t i = 0; i < alt_.size(); ++i) {
        if (alt_[i].spots.empty()) continue;
        ++reachable;
        bool boss_tiles = true;
        int gi0 = -1;
        for (auto t : alt_[i].spots) {
            const int gi = group_at_[static_cast<size_t>(tidx(t))];
            if (gi < 0 || !g_[static_cast<size_t>(gi)].boss || (gi0 >= 0 && gi != gi0)) boss_tiles = false;
            gi0 = gi;
        }
        if (boss_tiles && gi0 >= 0) { hold_i_ = static_cast<int>(i); hold_g_ = gi0; }
    }
    if (reachable != 1) hold_i_ = hold_g_ = -1;
    if (hold_i_ >= 0) {
        // The boss's tiles we can stand on next to the altar, tiles that take no tribute first.
        const auto& A = alt_[static_cast<size_t>(hold_i_)];
        if (kHoldBossTile)
            for (auto t : g_[static_cast<size_t>(hold_g_)].tiles)
                if (!bit(A.goal, t) && cheb(t, A.at) <= 3 && home_.at(t) < kFar) hold_spots_.push_back(t);
        for (auto t : A.spots) hold_spots_.push_back(t);
    }
    if (dropped) {
        // Some tribute tiles cannot be walked to (sealed pockets): bank and plan with the others only.
        altar_spots_.clear();
        for (const auto& A : alt_) altar_spots_ |= A.goal;
        altar_.compute(walk_walls(), altar_spots_, eg_seen_);
        home_altar_ = kFar;
        for (auto t : cambc::map::spawn_region(team::ally).positions()) home_altar_ = std::min(home_altar_, altar_.at(t));
        for (size_t g = 0; g < g_.size() && g < bank_d_.size(); ++g) {
            bank_d_[g] = kFar;
            for (auto t : g_[g].tiles) bank_d_[g] = std::min(bank_d_[g], altar_.at(t));
        }
    }
    eg_ready_ = true;
}

bool Bot::enemy_locked(size_t i) const {
    return enemy_lock_[i] >= 0 && round_now() - enemy_lock_[i] < kEnemyLockMemory;
}

// Once per round: the locks on the altars we see, enemies tributing, our own record (turns alive, deaths,
// gems picked up) and both teams' tributed totals.
void Bot::eg_observe() {
    const int now = round_now();
    if (!eg_ready_ || eg_round_ == now) return;
    eg_round_ = now;
    if (kTrack) {
    auto& me = cambc::self();
    const int gems = static_cast<int>(me.gem_count());
    if (eg_last_alive_ >= 0) {
        const int gap = now - eg_last_alive_;
        if (gap > 40) { ++eg_deaths_; eg_prev_gems_ = 0; }  // respawns take 60 rounds; channels far less
        else eg_alive_ += gap;
    }
    eg_last_alive_ = now;
    if (gems > eg_prev_gems_) eg_gained_ += gems - eg_prev_gems_;
    eg_prev_gems_ = gems;
    if (score_round_.empty() || now - score_round_.back() >= 25) {
        score_round_.push_back(now);
        score_theirs_.push_back(static_cast<int>(cambc::global_submitted_gems(team::enemy)));
        score_ours_.push_back(static_cast<int>(cambc::global_submitted_gems(team::ally)));
        if (score_round_.size() > 9) {
            score_round_.erase(score_round_.begin());
            score_theirs_.erase(score_theirs_.begin());
            score_ours_.erase(score_ours_.begin());
        }
    }
    }
    const auto& v = view();
    const auto ours = cambc::vision::altar_lock_tiles(team::ally);
    const auto theirs = cambc::vision::altar_lock_tiles(team::enemy);
    for (size_t i = 0; i < alt_.size(); ++i) {
        const vec2 a = alt_[i].at;
        if (!v.lvis.at(a)) continue;
        lock_seen_[i] = bit(ours, a) ? now : -1000;
        enemy_lock_[i] = bit(theirs, a) ? now : -1;
    }
}

// The enemy finishes the race before us at the recent rates, and soon.
bool Bot::behind_race() const {
    if (!kRaceDeny || !kTrack) return false;
    const int theirs = static_cast<int>(cambc::global_submitted_gems(team::enemy));
    const int ours = static_cast<int>(cambc::global_submitted_gems(team::ally));
    const int win = win_gems();
    if (theirs < win - kDenyGap || theirs <= ours || score_round_.size() < 2) return false;
    const float dt = static_cast<float>(std::max(1, score_round_.back() - score_round_.front()));
    const float rt = std::max(0.01f, static_cast<float>(score_theirs_.back() - score_theirs_.front()) / dt);
    const float ro = std::max(0.01f, static_cast<float>(score_ours_.back() - score_ours_.front()) / dt);
    const float t_them = static_cast<float>(win - theirs) / rt;
    const float t_us = static_cast<float>(win - ours) / ro;
    return t_them < t_us && t_them < static_cast<float>(left_rounds());
}

// ---------------------------------------------------------------- banking

// Bank this bag now (d: our walk to the nearest tribute tile)?
bool Bot::bank_due(int gems, int d) {
    const int now = round_now();
    const int ours = static_cast<int>(cambc::global_submitted_gems(team::ally));
    const int theirs = static_cast<int>(cambc::global_submitted_gems(team::enemy));
    const int win = win_gems();
    const int channel = channel_ticks(gems);
    if (ours + gems >= win) return true;  // this bag wins
    // Round 2000: a bag still carried then counts for nothing.
    const int trip = std::min(d, 6 + home_altar_);
    if (left_rounds() < trip + channel + kClockPad) return true;
    if (gems >= kFullBag || target_ < 0 || target_wait_ > d) return true;
    // The race's last stretch: our bags together may finish it (we only know our own), and a bag still
    // carried when the enemy gets there counts for nothing.
    const bool race = ours >= win - kRaceGap || (theirs >= win - kRaceGap && ours >= win - 2 * kRaceGap);
    if (race && (gems >= 8 || d <= 8)) return true;
    // What carrying the bag through the next target risks, against the detour banking it now costs.
    float hazard = (static_cast<float>(eg_deaths_) + kHazardPrior * kPriorTurns) / (static_cast<float>(eg_alive_) + kPriorTurns);
    const auto s = read_sighting();
    const bool danger = (s.ok && s.age <= 30 && cheb(s.where, pos_) <= 16) || now - last_enemy_seen_ <= 20;
    if (danger) hazard *= kDangerMul;
    const float rate = (static_cast<float>(eg_gained_) + kRatePrior * kPriorTurns) / (static_cast<float>(eg_alive_) + kPriorTurns);
    const int e = eta(target_);
    const int bd = bank_d_[static_cast<size_t>(target_)];
    const int detour = e >= kFar || bd >= kFar ? d : std::max(0, d + bd - e);
    const float horizon = static_cast<float>(e >= kFar ? 30 : e) + kClearTurns + static_cast<float>(bd >= kFar ? 0 : bd);
    const float loss = hazard * horizon * static_cast<float>(gems) * kSwing;
    const float cost = static_cast<float>(detour + 1) * rate;
    if (kTraceEg && now % 50 == 0)
        std::cout << "bankdue r" << now << " slot" << slot_ << " g" << gems << " d" << d << " det" << detour << " h" << hazard
                  << " rate" << rate << " loss" << loss << " cost" << cost << '\n';
    return loss >= cost;
}

// One hit on the enemy lock on altar a: a basic attack from attack range, or a tribute attempt beside it
// (the engine turns it into a hit). False when neither is possible from here.
bool Bot::hit_lock(vec2 a) {
    auto& me = cambc::self();
    if (!me.can_attack() || !live()) return false;
    const int d = cheb(a, me.position());
    if (kAttackLock && !lock_attack_bad_ && d <= me.attack_range() && view().lvis.at(a)) {
        try {
            if (cambc::vision::has_altar_lock(a)) {
                me.attack(cambc::vision::altar_lock_at(a));
                if (kTraceEg) std::cout << "lock attack ok r" << round_now() << " d" << d << " range" << me.attack_range() << '\n';
                attacked_ = true;
                invalidate();
                return true;
            }
        } catch (const cambc::error& e) {
            lock_attack_bad_ = true;
            if (kTraceEg) std::cout << "lock attack refused r" << round_now() << ": " << e.what() << '\n';
        }
    }
    if (d <= cambc::constants::submit_gems_range && me.gem_count() > 0) {
        try {
            me.submit_gems(a);
            attacked_ = true;
            invalidate();
            return true;
        } catch (const cambc::error& e) { warn("break lock", e); }
    }
    return false;
}

// Hit the enemy lock on altar a, twice with an attack reset (only right after our own attack: error 26
// otherwise). True when the turn went into it (or waits here for the next hit).
bool Bot::break_lock(vec2 a) {
    auto& me = cambc::self();
    if (!me.can_attack() || !live()) return true;  // wait here for the next hit
    if (!kAttackLock) {
        // The parent's: tribute attempts only.
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
    if (!hit_lock(a)) return false;
    if (!cast_ && !me.can_attack() && me.can_cast() && fuel(8000) && live() && cambc::vision::has_altar_lock(a))
        for (const auto& ab : me.abilities())
            if (ab.is_attack_reset() && ab.is_ready()) {
                try {
                    ab.use();
                    cast_ = true;
                } catch (const cambc::error& e) { warn("break lock reset", e); break; }
                invalidate();
                hit_lock(a);
                break;
            }
    return true;
}

// The boss altar (01boss-battle): while one of us stands on a tile of its boss, the boss cannot respawn,
// so the altar stays open, and the enemy banks there only past us. An empty-bagged champion near the
// altar takes up the hold when nobody of ours holds it; on a tribute tile it steps off for an ally with gems.
bool Bot::hold_tile() {
    if (kHoldMode == 0) {
        // The parent's: whoever banked on the tribute tile stays, and steps off for an ally with gems.
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
    if (hold_i_ < 0 || hold_spots_.empty() || moved_ || !live()) return false;
    auto& me = cambc::self();
    for (auto t : g_[static_cast<size_t>(hold_g_)].tiles)
        if (bit(camp_block_, t)) return false;  // the boss stands: nothing to hold
    // The boss comes back only when its timer runs out: hold from shortly before.
    const auto& k = k_[static_cast<size_t>(hold_g_)];
    if (k.state == Known::respawning && k.ready - round_now() > kHoldEarly) return false;
    const auto& A = alt_[static_cast<size_t>(hold_i_)];
    const bool healthy = me.health() >= me.max_health() * kHoldHp;
    bool on = false;
    for (auto t : hold_spots_) if (t == pos_) on = true;
    if (on) {
        if (!healthy || !me.can_move()) return false;
        if (!bit(A.goal, pos_)) return true;  // beside the tribute tile: allies bank past us
        for (const auto& a : others_) {
            cambc::detail::champion_info info{};
            if (!peek(a, info)) continue;
            const vec2 p{info.x, info.y};
            if (!inside(p) || cheb(p, pos_) > 2) continue;
            int g = 0;
            try { g = static_cast<int>(a.gem_count()); } catch (const std::exception&) { continue; }
            if (g <= 0) continue;
            // Step aside, onto another hold tile when there is one free.
            vec2 best{-1, -1};
            for (auto d : kDirs) {
                const vec2 t{pos_.x + d.x, pos_.y + d.y};
                if (!free_tile(t) || bit(A.goal, t) || bit(camp_block_, t) || bit(walk_walls(), t)) continue;
                bool hold = false;
                for (auto h : hold_spots_) if (h == t) hold = true;
                if (best.x < 0 || hold) best = t;
                if (hold) break;
            }
            if (best.x < 0) return false;
            bool ok = false;
            try { ok = me.move_absolute(best); } catch (const cambc::error& e) { warn("hold step", e); }
            moved_ = true;
            if (!ok) move_failed_ = true;
            invalidate();
            return true;
        }
        return true;
    }
    // Somebody of ours holds it already?
    for (auto a : view().allies)
        for (auto t : hold_spots_)
            if (a == t) return false;
    if (!healthy || incoming_ > 0) return false;
    int best_d = kFar;
    for (auto t : hold_spots_) best_d = std::min(best_d, cheb(t, pos_));
    if (best_d > kHoldWalk) return false;
    auto goal = empty_board();
    for (auto t : hold_spots_) if (free_tile(t)) goal.set(t);
    if (goal.is_empty()) return false;
    return go(goal, true);
}

// When to walk to an altar (bank_due()), where (the nearest open altar by road; through an enemy lock only
// when it is close or every altar is locked), and when to start the channel (we outlive it: a death in it
// hands the whole bag to the killer; a bag that wins the game goes in regardless).
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
    const int ours = static_cast<int>(cambc::global_submitted_gems(team::ally));
    const bool winning = ours + gems >= win_gems();
    // Beside an altar: bank when we outlive the channel; a lock in the way is hit.
    int at_locked = -1;
    for (size_t i = 0; i < alt_.size(); ++i) {
        const vec2 a = alt_[i].at;
        if (cheb(a, pos_) > range) continue;
        if (bit(locks, a)) { at_locked = static_cast<int>(i); continue; }
        float dmg = incoming_ * taken_ * static_cast<float>(channel + 1);
        dmg = std::max(dmg, channel_damage(pos_, channel, read_foes(10)));
        if (me.health() > dmg + 40 || (kWinAnyway && winning)) {
            if (live()) try { me.submit_gems(a); } catch (const cambc::error& e) { warn("submit", e); }
            return true;
        }
    }
    if (at_locked >= 0) return break_lock(alt_[static_cast<size_t>(at_locked)].at);

    const int d = altar_.at(pos_);  // nearest tribute tile we can walk to, locks aside
    const int left = left_rounds();
    // No walk from here to any altar (a dash crossed holes): only the recall brings the bag back.
    const bool stranded = d >= kFar;
    if (kBankModel) {
        if (stranded && !(gems >= kFullBag || winning || left < home_altar_ + channel + 30)) return false;
        if (!stranded && !bank_due(gems, d)) return false;
    } else {
        // The parent's rule: a full bag, a short walk, enemies about, the clock, the race.
        const auto s = read_sighting();
        const bool danger = (s.ok && s.age <= 30 && cheb(s.where, pos_) <= 16) || now - last_enemy_seen_ <= 20;
        const int theirs = static_cast<int>(cambc::global_submitted_gems(team::enemy));
        const int win = win_gems();
        const bool race = ours >= win - kRaceGap || (theirs >= win - kRaceGap && ours >= win - 2 * kRaceGap);
        const bool go_now = gems >= 45 || d <= 3 || (gems >= 20 && d <= 10) || (danger && gems >= 10)
            || target_ < 0 || target_wait_ > d || left < d + channel + 20
            || (race && (gems >= 8 || d <= 8)) || ours + gems >= win;
        if (stranded && !(gems >= 45 || (race && gems >= 8) || ours + gems >= win || left < home_altar_ + channel + 30)) return false;
        if (!go_now) return false;
    }
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
        const auto& A = alt_[static_cast<size_t>(near_locked >= 0 ? near_locked : any_locked)];
        // A ranged champion hits the lock from where it stands.
        if (kAttackLock && bit(locks, A.at) && cheb(A.at, pos_) <= me.attack_range() && view().lvis.at(A.at) && break_lock(A.at)) return true;
        goal.clear();
        goal |= A.goal;
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
    const int now = round_now();
    const bool behind = behind_race();
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
            for (size_t i = 0; i < alt_.size(); ++i)
                if (kTrack && cheb(alt_[i].at, p) <= cambc::constants::submit_gems_range) enemy_use_[i] = now;
            if (g < (behind ? 1 : 5) || g <= aim_gems) continue;
            aim = p;
            aim_gems = g;
            aim_left = channel_ticks(g);
            try { aim_left = static_cast<int>(c.remaining_channel_time()); } catch (const cambc::error&) {}
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
        if (use_lock(A.at)) { invalidate(); if (kTrack) lock_seen_[static_cast<size_t>(target)] = now; }
        return false;  // the rest of the turn is free: fight on
    }
    // Close enough to arrive before the channel ends?
    if (best - kLockRange > aim_left + 1) return false;
    if (me.health() < me.max_health() * (behind ? 0.2f : 0.35f)) return false;
    if (A.lock_goal.is_empty() || !go(A.lock_goal, true)) return false;
    // Arrived this step: drop it now.
    if (cheb(A.at, me.position()) <= kLockRange && live() && use_lock(A.at)) { invalidate(); if (kTrack) lock_seen_[static_cast<size_t>(target)] = now; }
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
    const float per_turn = str * static_cast<float>(1 + helpers);
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

// ---------------------------------------------------------------- altar locks
//
// Our locks block only enemy tributes (we tribute through them). Keep a lock on every altar we come
// near, drop one on an enemy tribute in progress (placing it cancels the channel), and let the last pick
// carry locks to far altars. While the enemy is about to win, everybody detours further for it.

// Keep one of our locks on every altar we come near. True when the turn went into a detour to lock one.
bool Bot::lock_altars(bool detour, int reach) {
    eg_init();
    if (!eg_ready_ || alt_.empty() || !live()) return false;
    eg_observe();
    if (!has_lock()) return false;
    const int now = round_now();
    const auto& v = view();
    const auto any = cambc::vision::altar_lock_tiles();
    if (!kRunner && reach > kLockDetour) reach = kLockDetour;
    if (detour && behind_race()) reach = std::max(reach, kDenyDetour);
    int best = -1, best_d = 99;
    for (size_t i = 0; i < alt_.size(); ++i) {
        const vec2 a = alt_[i].at;
        if (lock_seen_[i] > -1000 && now - lock_seen_[i] < kOurLockMemory) continue;  // ours, as far as we know
        const int d = cheb(a, pos_);
        if (d <= kLockRange && v.lvis.at(a) && !bit(any, a)) {
            if (use_lock(a)) invalidate();
            lock_seen_[i] = now;
            return false;
        }
        if (alt_[i].lock_goal.is_empty()) continue;  // nowhere to stand within lock range
        if (d < best_d && !bit(any, a)) { best_d = d; best = static_cast<int>(i); }
    }
    if (!detour || best < 0 || best_d > reach || incoming_ > 0 || moved_) return false;
    if (lock_try_g_ != best) { lock_try_g_ = best; lock_try_since_ = now; }
    if (now - lock_try_since_ > 2 * reach + 6) { lock_seen_[static_cast<size_t>(best)] = now; return false; }  // cannot get there
    return go(alt_[static_cast<size_t>(best)].lock_goal, true);
}

// Some altar we have not seen our lock on lately (so the runner should fetch another lock).
bool Bot::lock_needed() const {
    if (!kRunner) return false;
    const int now = round_now();
    for (size_t i = 0; i < altars_.size(); ++i) {
        if (i < alt_.size() && alt_[i].lock_goal.is_empty()) continue;
        if (lock_seen_[i] <= -1000 || now - lock_seen_[i] >= kOurLockMemory) return true;
    }
    return false;
}
