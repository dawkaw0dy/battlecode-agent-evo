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
constexpr bool kTraceBank = false;
constexpr bool kTraceLock = false;
constexpr int kLockRange = 2;          // altar lock placement range
constexpr int kEnemyLockMemory = 200;  // an enemy lock seen this long ago may still stand
constexpr int kOurLockMemory = 150;    // ... and ours
constexpr int kRaceGap = 120;          // this close to 1000 tributed, every bag is banked promptly
constexpr bool kContest = true;        // go for channelling enemies we can kill before the channel ends

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
    // Standing on any spawn tile of the boss delays its respawn: hold one that takes no tribute, when there is one.
    hold_spots_.clear();
    if (hold_i_ >= 0)
        for (auto t : alt_[static_cast<size_t>(hold_i_)].spots) {
            const int gi = group_at_[static_cast<size_t>(tidx(t))];
            if (gi < 0) continue;
            for (auto c : g_[static_cast<size_t>(gi)].tiles)
                if (!bit(alt_[static_cast<size_t>(hold_i_)].goal, c) && !bit(walk_walls(), c) && home_.at(c) < kFar)
                    raw_set(hold_spots_.ptr(), c);
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
    if (hold_i_ < 0 || moved_) return false;
    const bool on_hold = bit(hold_spots_, pos_);
    if (!on_hold && !bit(alt_[static_cast<size_t>(hold_i_)].goal, pos_)) return false;
    auto& me = cambc::self();
    if (me.health() < me.max_health() * 0.4f || !me.can_move() || !live()) return false;
    if (on_hold) return true;  // the boss stays down and the tribute tiles stay free
    const auto& goal = alt_[static_cast<size_t>(hold_i_)].goal;
    // From a tribute tile to a free boss tile beside it that takes no tribute.
    for (auto d : kDirs) {
        const vec2 t{pos_.x + d.x, pos_.y + d.y};
        if (!bit(hold_spots_, t) || !free_tile(t)) continue;
        bool ok = false;
        try { ok = me.move_absolute(t); } catch (const cambc::error& e) { warn("hold move", e); }
        moved_ = true;
        if (!ok) move_failed_ = true;
        invalidate();
        return true;
    }
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
    // Beside an altar: bank when the channel outlasts the camp fire on us; a lock in the way is hit.
    int at_locked = -1;
    for (size_t i = 0; i < alt_.size(); ++i) {
        const vec2 a = alt_[i].at;
        if (cheb(a, pos_) > range) continue;
        if (bit(locks, a)) { at_locked = static_cast<int>(i); continue; }
        const float turns = std::ceil(gems / 10.0f) + 1;
        if (channel_safe(static_cast<int>(turns))) {
            if (live()) try { me.submit_gems(a); } catch (const cambc::error& e) { warn("submit", e); }
            return true;
        }
    }
    if (at_locked >= 0) return break_lock(alt_[static_cast<size_t>(at_locked)].at);

    // When to walk to an altar: a full bag, a short walk, enemies about, nothing else to do, the clock.
    const int d = altar_.at(pos_);  // nearest tribute tile we can walk to, locks aside
    const int left = static_cast<int>(cambc::constants::max_rounds) - now;
    const auto s = read_sighting();
    const bool danger = (s.ok && s.age <= 30 && cheb(s.where, pos_) <= 16) || now - last_enemy_seen_ <= 20;
    const int channel = static_cast<int>(std::ceil(gems / 10.0f));
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
    if (d > home_altar_ + 10 && incoming_ == 0 && !moved_ && live()) {
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
    if (kTraceBank && (now % 10 == 0 || !went)) {
        std::cout << "bank r" << now << " slot" << slot_ << " gems " << gems << " pos " << pos_.x << ',' << pos_.y
                  << " d " << d << " open " << n_open << " cand " << n_cand << " locked " << any_locked << " went " << went
                  << " goal " << goal.count() << '\n';
    }
    return went;
}

// ---------------------------------------------------------------- altar locks
//
// Our locks block only enemy tributes (we tribute through them). Keep a lock on every altar we come
// near, drop one on an enemy tribute in progress (placing it cancels the channel), and let the last pick
// carry locks to far altars.

// An enemy tributing near us. Nobody cancels a tribute, and a champion in the channel cannot move, hit or
// cast until it ends: when we kill it first its bag is ours (and its team loses the trip). Otherwise a lock
// on its altar cancels the channel (walking into lock range when we arrive in time).
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
            aim_left = static_cast<int>(std::ceil(g / 10.0f));
            try { aim_left = c.remaining_channel_time(); } catch (const cambc::error&) {}
            aim_hp = c.health();
            try {
                const auto b = c.buffs();
                if (b.has_shield()) aim_hp += b.get<cambc::buff_shield>().value;
            } catch (const std::exception&) {}
        } catch (const cambc::error&) {}
    }
    if (aim_gems == 0) return false;
    if (kTraceBank) std::cout << "chan r" << round_now() << " slot" << slot_ << " at " << aim.x << ',' << aim.y << " gems " << aim_gems << " left " << aim_left << " hp " << aim_hp << " d " << cheb(aim, pos_) << " free " << free_foes.size() << '\n';
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
    if (kTraceBank) std::cout << "contest r" << round_now() << " slot" << slot_ << " at " << p.x << ',' << p.y << " d " << d
                              << " hp " << hp << " left " << left << " reach " << reach << " kill " << kill << '\n';
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
