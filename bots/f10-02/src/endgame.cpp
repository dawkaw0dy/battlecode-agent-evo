// Endgame: when and where carried gems are banked, the altars both teams use, and the locks on them.
//
// Carried gems count for nothing until tributed, and the first team to tribute 1000 wins. Every altar is
// read once: the tiles we can actually walk to that take a tribute there (an altar in a sealed pocket has
// none; a tile under a living camp counts once it is cleared). Each turn the visible altars update what
// we know of the locks on them: an enemy lock blocks us until five hits break it (attack() refuses locks:
// a tribute at the locked altar is what hits it), ours blocks only the enemy.
// A bag goes to the bank when it is full, the walk is short, enemies are about, the clock runs out, or
// the race is in its last stretch (then every bag, since ours together may finish it); to the nearest open
// altar by road, through a lock only when it is close or every altar is locked, by recall when the spawn
// is nearer an altar or no walk leads to one. Nobody cancels a tribute and a champion in the channel
// cannot act: one we can kill before its channel ends is ours for the taking, otherwise our lock on its
// altar cancels the channel. Our locks also go on every altar we pass, and the last pick runs them to
// far altars. Where the only altar takes tributes from a boss's spawn tiles alone, we hold one.

#include <cambc.h>
#include "bot.h"
#include "fuel.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <optional>
#include <variant>

namespace {
constexpr bool kTraceLock = false;
constexpr int kLockRange = 2;          // altar lock placement range
constexpr int kEnemyLockMemory = 200;  // an enemy lock seen this long ago may still stand
constexpr int kOurLockMemory = 150;    // ... and ours

int round_now() { return static_cast<int>(cambc::round_number()); }

bool use_lock(vec2 a) {
    if (kTraceLock) std::cout << "LOCKUSE r" << cambc::round_number() << " altar " << a.x << ',' << a.y << '\n';
    try { return cambc::self().consumables()[cambc::consumable_type::altar_lock].use(a); }
    catch (const cambc::error& e) { warn("lock", e); }
    return false;
}
}  // namespace

// ---------------------------------------------------------------- altar model

void Bot::eg_init() {
    // Nothing to bank or lock in the first rounds, which the map analysis already fills with work.
    if (eg_ready_ || round_now() < 6 || !fuel(85000) || !live()) return;
    const int ar = cambc::constants::submit_gems_range;
    alt_.clear();
    alt_.resize(altars_.size());
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
    int reachable = 0;
    for (size_t i = 0; i < alt_.size(); ++i) {
        if (alt_[i].spots.empty()) continue;
        ++reachable;
        bool boss_tiles = true;
        for (auto t : alt_[i].spots) {
            const int gi = group_at_[static_cast<size_t>(tidx(t))];
            if (gi < 0 || !g_[static_cast<size_t>(gi)].boss) boss_tiles = false;
        }
        if (boss_tiles) hold_i_ = static_cast<int>(i);
    }
    if (reachable != 1) hold_i_ = -1;
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

// What the visible altars tell us about the locks on them: once per turn.
void Bot::eg_observe() {
    const int now = round_now();
    if (!eg_ready_ || eg_round_ == now) return;
    eg_round_ = now;
    const auto& v = view();
    const auto ours = cambc::vision::altar_lock_tiles(team::ally);
    const auto theirs = cambc::vision::altar_lock_tiles(team::enemy);
    for (size_t i = 0; i < alt_.size(); ++i) {
        const vec2 a = alt_[i].at;
        if (!v.lvis.at(a)) continue;
        const bool o = bit(ours, a);
        lock_seen_[i] = o ? now : -1000;
        enemy_lock_[i] = bit(theirs, a) ? now : -1;
    }
}

// Banking, enemy tributes and our own channels: defense.cpp.

// ---------------------------------------------------------------- altar locks
//
// Our locks block only enemy tributes (we tribute through them). Keep a lock on every altar we come
// near, drop one on an enemy tribute in progress (placing it cancels the channel), and let the last pick
// carry locks to far altars.

// Keep one of our locks on every altar we come near. True when the turn went into a detour to lock one.
bool Bot::lock_altars(bool detour, int reach) {
    eg_init();
    if (!eg_ready_ || alt_.empty() || !live()) return false;
    eg_observe();
    if (!has_lock()) return false;
    const int now = round_now();
    const auto& v = view();
    const auto any = cambc::vision::altar_lock_tiles();
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
    const int now = round_now();
    for (size_t i = 0; i < altars_.size(); ++i) {
        if (i < alt_.size() && alt_[i].lock_goal.is_empty()) continue;
        if (lock_seen_[i] <= -1000 || now - lock_seen_[i] >= kOurLockMemory) return true;
    }
    return false;
}
