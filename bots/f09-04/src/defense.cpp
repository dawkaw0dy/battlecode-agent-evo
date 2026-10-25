// Defense: keeping our champions, their bags and our score alive between and inside fights.
//
// Enemies hit only what they see: a basic attack or unit spell needs a visible target, a champion in a bush is
// hidden from everyone outside that bush, and walls block the view. So the threat on a tile counts only the
// enemies whose vision from where they stand covers it (the engine's own shadowcast from their tile), and a
// retreat steps or dashes to the tile they hurt least, which is often a bush none of them stands in, then
// towards home. It recalls once nobody who sees us can reach us before the recall ends.
// Channels: damage does not stop a tribute (a stun, death or a lock does), so a tribute starts when the
// enemies and camps that reach us cannot kill us before it completes and is cancelled when they can (a death
// hands the whole bag to the killer). A recall that a hit is about to cancel anyway is cancelled at once, so
// the turn goes into running or fighting instead. Damage that nothing we see explains comes from an attacker
// hidden in a bush: we step out of reach of the bushes we cannot see into and do not recall beside them.

#include <cambc.h>
#include "bot.h"
#include "fuel.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>

namespace {
constexpr int kRecallTicks = 5;      // recall channel (no Home emblem in our builds)
constexpr int kRecallSafe = 5;       // recall only when every enemy that sees us needs this many turns to hit
constexpr int kBlindNear = 2;        // ... and every enemy that does not see us is further than this
constexpr bool kFreeShots = true;    // retreating ranged: shoot when nobody can hit us here next turn
constexpr int kAmbushRounds = 3;     // rounds an unexplained hit keeps us away from unseen bushes
constexpr float kAmbushDrop = 30.0f; // health lost beyond the camp fire we expected that counts as a hidden hit
constexpr float kChannelPad = 40.0f; // health a channel must leave us at the end

int now_round() { return static_cast<int>(cambc::round_number()); }

// Turns before fighter e can hit tile t (melee walks and hits in the same turn; a dash adds two steps).
int arrive(const Fighter& e, vec2 t) {
    const int d = cheb(e.pos, t);
    const int reach = e.ranged ? e.range : e.range + 1 + (e.dasher ? 2 : 0);
    if (d <= reach) return 0;
    return d - reach;
}
}  // namespace

// ---------------------------------------------------------------- vision of the enemies

// What a champion standing at p sees (every champion has the same sight): cached for this round.
const cambc::bitboard* Bot::eyes(vec2 p) const {
    const int now = now_round();
    if (eye_round_ != now) {
        eye_round_ = now;
        eye_n_ = 0;
    }
    for (size_t i = 0; i < eye_n_; ++i)
        if (eye_at_[i] == p) return &eye_[i];
    if (eye_n_ >= eye_.size() || !inside(p) || !fuel(12000)) return nullptr;
    try {
        cambc::self().visible_tiles_from_into(p, eye_[eye_n_]);
    } catch (const cambc::error& e) {
        warn("eyes", e);
        return nullptr;
    }
    eye_at_[eye_n_] = p;
    return &eye_[eye_n_++];
}

// Does the enemy at e see tile t? Unknown (no fuel, a failed read) counts as seen.
bool Bot::sees(const Fighter& e, vec2 t) const {
    if (cheb(e.pos, t) > 8) return false;
    const cambc::bitboard* b = eyes(e.pos);
    return !b || bit(*b, t);
}

// Damage the enemies can put on tile t next turn: melee step and hit, ranged from where they stand, a ranged
// spell from up to 4; only enemies that see t from where they stand now aim at it.
float Bot::threat_at(vec2 t, const std::vector<Fighter>& theirs) const {
    float th = 0;
    for (const auto& e : theirs) {
        if (e.stunned || e.hp <= 0) continue;
        const int d = cheb(e.pos, t);
        const int reach = e.ranged ? e.range : e.range + 1 + (e.dasher ? 2 : 0);
        const bool spell = d <= 4 && e.burst > e.str;
        if (d > reach && !spell) continue;
        if (!sees(e, t)) continue;
        if (d <= reach) th += e.str + e.burst * 0.5f;
        else th += (e.burst - e.str) * 0.5f;
    }
    return th;
}

// No enemy can interrupt a recall started at our tile: the ones that see us need kRecallSafe turns to hit
// us, the ones that do not are not close enough to stumble on us.
bool Bot::recall_safe(const std::vector<Fighter>& theirs) const {
    for (const auto& e : theirs) {
        if (e.hp <= 0) continue;
        if (sees(e, pos_)) {
            if (arrive(e, pos_) < kRecallSafe) return false;
        } else if (cheb(e.pos, pos_) <= kBlindNear) {
            return false;
        }
    }
    return true;
}

// Damage the visible enemies and the camps put on us over the next `turns` of our turns while we stand still.
float Bot::exposure(int turns) const {
    float dmg = incoming_ * taken_ * static_cast<float>(turns);
    for (const auto& e : foes_) {
        if (e.hp <= 0) continue;
        const int a = arrive(e, pos_) + (e.stunned ? 1 : 0);
        if (a >= turns) continue;
        if (!sees(e, pos_) && cheb(e.pos, pos_) > kBlindNear) continue;
        dmg += e.burst + e.sustain * static_cast<float>(turns - a);
    }
    return dmg;
}

// A channel of `turns` turns is safe to start (or keep): we are alive at its end with some health left.
bool Bot::channel_safe(int turns) const {
    return cambc::self().health() > exposure(turns) + kChannelPad;
}

// The enemies we can see (not on their spawn), read into the fight model.
void Bot::read_foes() {
    foes_.clear();
    const auto& v = view();
    if (v.foes.is_empty()) return;
    const auto& enemy_spawn = cambc::map::spawn_region(team::enemy);
    for (auto p : v.foes.positions()) {
        if (enemy_spawn.is_set(p) || cheb(p, pos_) > 10) continue;
        try {
            Fighter f;
            if (!fighter_at(p, f)) continue;
            foes_.push_back(f);
        } catch (const std::exception&) {}
        if (foes_.size() >= 8) break;
    }
}

// ---------------------------------------------------------------- every turn

// Start of turn, before the channel check: hidden attackers, and our own recall or tribute in danger.
void Bot::guard() {
    auto& me = cambc::self();
    const int now = now_round();
    const float hp = me.health();
    read_foes();
    // A hit that neither the camps we knew of nor any enemy we can see explains: someone hides in a bush.
    if (hp_round_ == now - 1 && foes_.empty() && hp_last_ - hp > camp_last_ + kAmbushDrop) ambush_until_ = now + kAmbushRounds;
    hp_round_ = now;
    hp_last_ = hp;
    if (me.is_recalling() && live()) {
        int left = kRecallTicks;
        try { left = me.remaining_channel_time(); } catch (const cambc::error&) {}
        // Someone who sees us hits us before we leave: the hit cancels it anyway, so act now.
        bool cancel = false;
        for (const auto& e : foes_)
            if (e.hp > 0 && (sees(e, pos_) || cheb(e.pos, pos_) <= kBlindNear) && arrive(e, pos_) < left + 1) cancel = true;
        if (cancel) try { me.cancel_recall(); } catch (const cambc::error& e) { warn("cancel recall", e); }
    }
    if (me.is_submitting_gems() && live() && !foes_.empty()) {
        int left = 1;
        try { left = me.remaining_channel_time(); } catch (const cambc::error&) {}
        // Damage does not stop a tribute, a death does and hands over the bag: cancel only for our life.
        if (!channel_safe(std::max(1, left)) && me.gem_count() > 0)
            try { me.cancel_submit_gems(); } catch (const cambc::error& e) { warn("cancel tribute", e); }
    }
}

// End of turn: what we expect the camps to take from us before our next turn.
void Bot::guard_end() { camp_last_ = incoming_ * taken_; }

// An attacker hidden in a bush hit us lately: step to the tile the fewest unseen bush tiles can reach
// (ranged reach 3), towards home on ties. True when the turn went into it.
bool Bot::evade_hidden() {
    const int now = now_round();
    if (now > ambush_until_ || moved_ || move_failed_ || !live()) return false;
    auto& me = cambc::self();
    if (!me.can_move() || spawn_local_.at(pos_)) return false;
    const auto& v = view();
    const auto& bushes = cambc::map::bushes();
    std::array<vec2, 48> hide{};
    size_t n = 0;
    for (int dy = -4; dy <= 4 && n < hide.size(); ++dy)
        for (int dx = -4; dx <= 4 && n < hide.size(); ++dx) {
            const vec2 t{pos_.x + dx, pos_.y + dy};
            if (!inside(t) || v.lvis.at(t) || !raw_bit(bushes.ptr(), t)) continue;
            hide[n++] = t;
        }
    if (n == 0) return false;
    auto score = [&](vec2 t) {
        int s = 0;
        for (size_t i = 0; i < n; ++i)
            if (cheb(hide[i], t) <= 3) s += 10;
        const int hd = home_.at(t);
        return s + (hd >= kFar ? 200 : hd) + (camp_block_.is_set(t) ? 500 : 0);
    };
    vec2 best = pos_;
    int best_s = score(pos_);
    if (best_s < 10) return false;  // out of their reach already
    for (auto d : kDirs) {
        const vec2 t{pos_.x + d.x, pos_.y + d.y};
        if (!free_tile(t) || camp_block_.is_set(t)) continue;
        const int s = score(t);
        if (s < best_s) { best_s = s; best = t; }
    }
    if (best == pos_) return false;
    moved_ = true;
    bool ok = false;
    try { ok = me.move_absolute(best); } catch (const cambc::error& e) { warn("evade move", e); }
    invalidate();
    if (!ok) move_failed_ = true;
    else pos_ = me.position();
    return true;
}

// ---------------------------------------------------------------- retreat

// Run from the enemies: the free neighbouring tile (or dash landing) with the least threat, nearer home on
// ties, a dash first when one step does not escape. Recall once nobody who sees us can reach us in time.
bool Bot::retreat(std::vector<Fighter>& theirs, int dmin) {
    auto& me = cambc::self();
    (void)dmin;
    if (spawn_local_.at(pos_)) {
        // The spawn heals us every turn and they cannot step on it: hold it and shoot from there.
        if (++spawn_hold_ > 20) return false;
        strike(theirs);
        return true;
    }
    spawn_hold_ = 0;
    if (!moved_ && incoming_ == 0 && now_round() > ambush_until_ && live() && home_.at(pos_) > 6 && recall_safe(theirs)) {
        try { me.recall(); } catch (const cambc::error& e) { warn("flee recall", e); }
        return true;
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
        && threat_at(pos_, theirs) == 0 && strike(theirs))
        return true;
    if (moved_ || move_failed_ || !me.can_move() || !live()) return true;
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
    auto score = [&](vec2 t) {
        float s = threat_at(t, theirs);
        for (size_t i = 0; i < n_hot; ++i)
            if (cheb(t, hot[i].first) <= hot[i].second.range) s += hot[i].second.str * taken_;
        s *= 4.0f;
        if (spawn_local_.at(t)) s *= 0.1f;
        const int hd = home_.at(t);
        s += static_cast<float>(hd >= kFar ? 200 : hd);
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
    if (best == pos_) return true;
    moved_ = true;
    bool ok = false;
    try { ok = me.move_absolute(best); } catch (const cambc::error& e) { warn("flee move", e); }
    invalidate();
    if (!ok) move_failed_ = true;
    else pos_ = me.position();
    return true;
}

// ---------------------------------------------------------------- going home

// Walk home when it is close, otherwise recall where no enemy we know of interrupts it. True when the turn
// is used.
bool Bot::recall_or_walk(int walk_limit) {
    auto& me = cambc::self();
    if (spawn_local_.at(pos_)) return false;
    const int d = home_.at(pos_);
    if (d <= walk_limit) {
        go_home();
        return true;
    }
    if (moved_ || incoming_ > 0 || !live()) return moved_;
    // A hidden attacker beside us or an enemy that will reach us first: walk while it lasts.
    if (now_round() <= ambush_until_ || !recall_safe(foes_)) {
        go_home();
        return true;
    }
    try { me.recall(); } catch (const cambc::error& e) { warn("recall", e); }
    return true;
}
