// Safe banker: farm camp groups in small parties, bank gems early, never take an even fight.
#include <cambc.h>
#include "bot.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <span>
#include <variant>
#include <vector>

namespace {

struct CampSpec { float hp, str; int range; };
CampSpec camp_spec(cambc::camp_type t) {
    switch (t) {
    case cambc::camp_type::wolf: return {1350, 20, 3};
    case cambc::camp_type::elk: return {2400, 60, 4};
    case cambc::camp_type::gem_cluster: return {120, 0, 0};
    case cambc::camp_type::gold_cluster: return {120, 0, 0};
    case cambc::camp_type::monolith: return {20000, 200, 5};
    case cambc::camp_type::golem: return {5000, 120, 4};
    }
    return {1500, 50, 3};
}

constexpr int kAway = 255;
constexpr float kBudget = 0.8f;  // share of our health a clear may cost the nearest of us
constexpr int kShopGold = 150;
constexpr int kPlugByte = 6;      // shared memory: per party, a group closing the way to an altar (+1)

float ability_range(const cambc::ability& ab) {
    const auto& info = ab.info();
    if (!info) return 0;
    return std::visit([](const auto& p) -> float {
        if constexpr (requires { p.range; }) return p.range;
        else return 0.0f;
    }, *info);
}

bool exclusive(const cambc::ability& ab) {
    const auto& info = ab.info();
    if (!info) return false;
    return std::visit([](const auto& p) { return p.exclusive_with_movement; }, *info);
}

float aoe_radius(const cambc::ability& ab) {
    const auto& info = ab.info();
    if (!info) return 0;
    if (const auto* a = std::get_if<cambc::ability_aoe_damage>(&*info)) return a->radius;
    return 0;
}

}  // namespace

// ---------------------------------------------------------------- setup

void Bot::initialise() {
    init_ = true;
    init_dims();
    auto& me = cambc::self();
    const auto allies = cambc::champions(team::ally);
    size_ = static_cast<int>(allies.size());
    for (int i = 0; i < size_; ++i) {
        if (allies[i] == me) slot_ = i;
        const auto st = allies[i].base_stats();
        base_dps_.push_back(st.base_strength * 1.12f + st.base_focus * 0.2f);
    }
    // Parties: one of up to three, else two.
    party_of_.assign(size_, 0);
    if (size_ == 4) party_of_ = {0, 0, 1, 1};
    else if (size_ >= 5) { party_of_.assign(size_, 1); party_of_[0] = party_of_[1] = party_of_[2] = 0; }
    party_ = party_of_[slot_];

    terrain_ = cambc::map::walls() | cambc::map::holes() | cambc::map::altars() | cambc::map::spawn_region(team::enemy);
    altars_ = cambc::map::altars().positions();
    altar_spots_.clear();
    const int sr = cambc::constants::submit_gems_range;
    for (auto a : altars_)
        for (int dy = -sr; dy <= sr; ++dy)
            for (int dx = -sr; dx <= sr; ++dx) {
                const vec2 p{a.x + dx, a.y + dy};
                if (inside(p) && !raw_bit(terrain_.ptr(), p)) raw_set(altar_spots_.ptr(), p);
            }
    altar_t_f_.compute(terrain_, altar_spots_, altar_t_seen_);
    spawn_f_.compute(terrain_, cambc::map::spawn_region(team::ally), spawn_seen_);

    group_at_.assign(static_cast<std::size_t>(g_w) * g_h, -1);
    for (const auto& h : cambc::map::camp_groups()) {
        Group g{h};
        g.tiles = h.spawn_tiles().positions();
        if (g.tiles.empty()) continue;
        g.board.clear();
        for (auto t : g.tiles) set_bit(g.board, t);
        for (const auto& c : h.camps()) {
            const auto s = camp_spec(c.kind());
            g.kinds.push_back(c.kind());
            g.hp += s.hp;
            g.str += s.str;
            g.range = std::max(g.range, s.range);
        }
        g.gems = static_cast<int>(h.gem_value());
        g.gold = static_cast<int>(h.gold_value());
        g.respawn = h.max_respawn_timer();
        long sx = 0, sy = 0;
        for (auto t : g.tiles) { sx += t.x; sy += t.y; }
        const vec2 mean{static_cast<int>(sx / static_cast<long>(g.tiles.size())), static_cast<int>(sy / static_cast<long>(g.tiles.size()))};
        g.center = g.tiles[0];
        for (auto t : g.tiles)
            if (cheb(t, mean) < cheb(g.center, mean)) g.center = t;
        for (auto t : g.tiles) g.bank = std::min(g.bank, fdist(altar_t_f_, altar_t_seen_, t));
        const int gi = static_cast<int>(g_.size());
        for (auto t : g.tiles) group_at_[tidx(t)] = static_cast<std::int16_t>(gi);
        g_.push_back(std::move(g));
    }
    range_ = me.attack_range();
    melee_ = range_ <= 1;
    update_blocks();
    me.set_priority(size_ - slot_);
}

// ---------------------------------------------------------------- shared memory

void Bot::write_status(int value) {
    if (slot_ >= 8) return;
    std::uint8_t v = static_cast<std::uint8_t>(value);
    cambc::shared_memory.write(static_cast<std::size_t>(slot_), std::span{&v, 1});
}

int Bot::read_status(int slot) const {
    std::uint8_t v = 0;
    cambc::shared_memory.read(static_cast<std::size_t>(slot), std::span{&v, 1});
    return v;
}

int Bot::leader_slot() const {
    for (int s = 0; s < size_; ++s)
        if (party_of_[s] == party_ && (s == slot_ || read_status(s) != kAway)) return s;
    return slot_;
}

// ---------------------------------------------------------------- observation

void Bot::observe() {
    foes_.clear();
    friends_.clear();
    camps_.clear();
    incoming_ = 0;
    for (auto p : cambc::vision::champion_tiles(team::enemy).positions()) {
        try {
            const auto c = cambc::vision::champion_at(p);
            Unit u;
            u.pos = p;
            u.hp = c.health();
            u.max_hp = c.max_health();
            u.str = c.strength();
            u.focus = c.focus();
            u.range = c.attack_range();
            u.type = c.champion_name();
            foes_.push_back(u);
        } catch (const std::exception& e) { warn("foe", e); }
    }
    for (auto p : cambc::vision::champion_tiles(team::ally).positions()) {
        if (p == pos_) continue;
        try {
            const auto c = cambc::vision::champion_at(p);
            Unit u;
            u.pos = p;
            u.hp = c.health();
            u.max_hp = c.max_health();
            u.str = c.strength();
            u.focus = c.focus();
            u.range = c.attack_range();
            u.type = c.champion_name();
            friends_.push_back(u);
        } catch (const std::exception& e) { warn("friend", e); }
    }
    for (auto p : cambc::vision::camp_tiles().positions()) {
        try {
            const auto c = cambc::vision::camp_at(p);
            int gi = group_at_[tidx(c.spawn_position())];
            camps_.emplace_back(p, gi);
            if (gi >= 0 && g_[gi].state != Group::alive) { g_[gi].state = Group::alive; block_dirty_ = true; }
            if (cheb(p, pos_) <= 6 && c.is_awake() && cheb(p, pos_) <= c.attack_range()) incoming_ += c.strength();
        } catch (const std::exception& e) { warn("camp", e); }
    }
    // Groups in view without a visible living camp: ask the engine for their state.
    const auto vis = cambc::vision::visible_tiles();
    int queries = 0;
    for (int gi = 0; gi < static_cast<int>(g_.size()) && queries < 3; ++gi) {
        auto& g = g_[gi];
        if (cheb(g.center, pos_) > 9 && gi != target_) continue;
        if (g.state == Group::respawning && g.ready > round_ + 2) continue;
        bool seen = false, living = false;
        for (auto t : g.tiles)
            if (raw_bit(vis.ptr(), t)) seen = true;
        if (!seen) continue;
        for (const auto& [p, cg] : camps_)
            if (cg == gi) living = true;
        if (living) continue;
        ++queries;
        try {
            const auto st = g.h.state();
            const auto before = g.state;
            if (std::holds_alternative<cambc::map::camp_group_alive>(st)) g.state = Group::alive;
            else if (std::holds_alternative<cambc::map::camp_group_reward_available>(st)) g.state = Group::reward;
            else if (const auto* r = std::get_if<cambc::map::camp_group_respawning>(&st)) {
                g.state = Group::respawning;
                g.ready = round_ + std::max(1, r->timer);
            }
            if (g.state != before) block_dirty_ = true;
        } catch (const std::exception& e) { warn("group", e); }
    }
    // A respawn we expected has happened.
    for (auto& g : g_)
        if (g.state == Group::respawning && g.ready <= round_) { g.state = Group::alive; block_dirty_ = true; }
    update_blocks();
}

// Camps we cannot see still block: route around every group we believe alive.
void Bot::update_blocks() {
    if (!block_dirty_ || round_ - block_round_ < 4) return;
    if (cambc::fuel_remaining() < 60000) return;
    block_dirty_ = false;
    block_round_ = round_;
    camp_block_.clear();
    for (const auto& g : g_)
        if (g.state == Group::alive) camp_block_ |= g.board;
    cambc::bitboard roots = altar_spots_;
    for (const auto& g : g_)
        if (g.state == Group::alive) for (auto t : g.tiles) clear_bit(roots, t);
    altar_f_.compute(terrain_ | camp_block_, roots, altar_seen_);
}

// ---------------------------------------------------------------- shopping

void Bot::shop() {
    auto& me = cambc::self();
    using it = cambc::item_type;
    struct Step { it shard, emblem; };
    // Survive first (Durability: 10% less damage, Jungle: 30% less from camps), then a faster recall, then damage.
    const std::array plan{Step{it::durability_shard_of_health, it::durability_emblem_of_health},
        Step{it::jungle_shard_of_strength, it::jungle_emblem_of_strength},
        Step{it::home_shard_of_health, it::home_emblem_of_health}};
    for (int guard = 0; guard < 12; ++guard) {
        if (me.gold_count() < 30) return;
        const auto counts = me.item_count();
        it pick = it::power_shard_of_strength;
        for (const auto& s : plan)
            if (counts[s.emblem] == 0) { pick = s.shard; break; }
        try {
            if (!me.can_buy(pick, 1)) return;
            me.buy(pick, 1);
        } catch (const std::exception& e) { warn("buy", e); return; }
    }
}

// ---------------------------------------------------------------- movement

// Step towards the nearest root. Roots may be occupied (camps): the walk stops next to them.
bool Bot::step(const cambc::bitboard& roots_in, bool) {
    auto& me = cambc::self();
    nopath_ = false;
    if (moved_ || stop_move_ || !me.can_move()) return false;
    if (bit(roots_in, pos_)) return false;
    cambc::bitboard blocked = terrain_ | cambc::vision::champion_tiles() | cambc::vision::camp_tiles()
        | cambc::vision::teleport_tiles();
    blocked |= camp_block_;
    clear_bit(blocked, pos_);
    cambc::bitboard walls = blocked;
    walls &= ~roots_in;
    cambc::bitboard dest;
    set_bit(dest, pos_);
    cambc::distance_field f;
    cambc::bitboard seen;
    f.compute_to(walls, roots_in, dest, seen);
    const int here = fdist(f, seen, pos_);
    if (here >= kFar) { nopath_ = true; return false; }
    vec2 best = pos_;
    int bd = here;
    for (auto d : kDirs) {
        const vec2 q = pos_ + d;
        if (!inside(q) || raw_bit(blocked.ptr(), q)) continue;
        const int v = fdist(f, seen, q);
        if (v < bd) { bd = v; best = q; }
    }
    debug_block({
        if (round_ >= 100 && round_ <= 106) std::cout << "  step r" << round_ << " s" << slot_ << " at(" << pos_.x << "," << pos_.y << ") here" << here << " best(" << best.x << "," << best.y << ") bd" << bd << '\n';
    });
    if (best == pos_) return false;
    try {
        if (me.move_absolute(best)) {
            moved_ = true;
            pos_ = best;
            return true;
        }
        stop_move_ = true;
    } catch (const std::exception& e) { warn("move", e); stop_move_ = true; }
    return false;
}

void Bot::ensure_me_field() {
    if (me_f_round_ >= round_ - 3 && cheb(me_f_pos_, pos_) <= 2) return;
    me_f_.compute(terrain_, [&] { cambc::bitboard b; set_bit(b, pos_); return b; }(), me_seen_);
    me_f_round_ = round_;
    me_f_pos_ = pos_;
}

// ---------------------------------------------------------------- enemies

float Bot::my_dps() const {
    const auto& me = cambc::self();
    return me.strength() * 1.12f + me.focus() * 0.2f;
}

void Bot::retreat() {
    auto& me = cambc::self();
    // Bank on the way if an altar is right here: damage does not stop a tribute.
    if (me.gem_count() > 0)
        for (auto a : altars_)
            if (cheb(a, pos_) <= cambc::constants::submit_gems_range && !cambc::vision::has_altar_lock(a)) {
                try { me.submit_gems(a); return; } catch (const std::exception& e) { warn("submit", e); }
            }
    int dmin = 99;
    for (const auto& f : foes_) dmin = std::min(dmin, cheb(f.pos, pos_));
    const bool at_spawn = bit(cambc::map::spawn_region(team::ally), pos_);
    if (dmin >= 6 && incoming_ <= 0 && !at_spawn && fdist(spawn_f_, spawn_seen_, pos_) > 10) {
        try { me.recall(); write_status(kAway); return; } catch (const std::exception& e) { warn("recall", e); }
    }
    if (!me.can_move() || moved_ || stop_move_) return;
    const auto blocked = terrain_ | cambc::vision::champion_tiles() | cambc::vision::camp_tiles();
    auto score = [&](vec2 q) {
        int m = 99;
        for (const auto& f : foes_) m = std::min(m, cheb(f.pos, q));
        const int sd = fdist(spawn_f_, spawn_seen_, q);
        return std::min(m, 9) * 10 - std::min(sd, 200);
    };
    vec2 best = pos_;
    int bs = score(pos_);
    for (auto d : kDirs) {
        const vec2 q = pos_ + d;
        if (!inside(q) || raw_bit(blocked.ptr(), q)) continue;
        const int s = score(q);
        if (s > bs) { bs = s; best = q; }
    }
    if (best == pos_) return;
    try {
        if (me.move_absolute(best)) { moved_ = true; pos_ = best; }
        else stop_move_ = true;
    } catch (const std::exception& e) { warn("flee", e); stop_move_ = true; }
}

// Fight only when clearly stronger; otherwise keep out of reach. True when the turn is spent.
bool Bot::handle_enemies() {
    if (foes_.empty()) return false;
    auto& me = cambc::self();
    last_threat_ = round_;
    float our_hp = me.health(), our_dps = my_dps();
    for (const auto& f : friends_)
        if (cheb(f.pos, pos_) <= 6) { our_hp += f.hp; our_dps += f.str * 1.12f + f.focus * 0.2f; }
    float their_hp = 0, their_dps = 0;
    int dmin = 99;
    for (const auto& f : foes_) {
        dmin = std::min(dmin, cheb(f.pos, pos_));
        their_hp += f.hp;
        their_dps += f.str * 1.12f + f.focus * 0.25f;
    }
    const float ratio = (our_hp * our_dps) / std::max(1.0f, their_hp * their_dps);
    const int gems = static_cast<int>(me.gem_count());
    // Free kill: a foe in reach that our hits finish.
    const Unit* weak = nullptr;
    for (const auto& f : foes_)
        if (cheb(f.pos, pos_) <= range_ && (!weak || f.hp < weak->hp)) weak = &f;
    const bool strong = ratio >= 2.2f;
    const bool fearful = ratio < 1.3f || (gems >= 12 && ratio < 2.2f);
    if (weak && me.can_attack() && (strong || weak->hp <= me.strength() * 1.5f || !fearful)) {
        try {
            const auto c = cambc::vision::champion_at(weak->pos);
            me.attack(c);
            attacked_ = true;
            reset_attack();
        } catch (const std::exception& e) { warn("hit foe", e); }
        if (strong) return true;
    }
    if (strong && dmin <= 5 && !moved_) {
        cambc::bitboard roots;
        for (const auto& f : foes_) set_bit(roots, f.pos);
        if (step(roots, true) && melee_ && me.can_attack()) {
            for (const auto& f : foes_)
                if (cheb(f.pos, pos_) <= range_) {
                    try { me.attack(cambc::vision::champion_at(f.pos)); attacked_ = true; } catch (const std::exception& e) { warn("hit foe2", e); }
                    break;
                }
        }
        return true;
    }
    if (fearful) {
        int reach = 0;
        for (const auto& f : foes_) reach = std::max(reach, f.range + 3);
        if (dmin <= reach) {
            if (target_ >= 0 && cheb(g_[target_].center, pos_) <= 10) g_[target_].avoid_until = round_ + 40;
            retreat();
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------- banking

bool Bot::tribute() {
    auto& me = cambc::self();
    const int gems = static_cast<int>(me.gem_count());
    if (gems <= 0) return false;
    const int sr = cambc::constants::submit_gems_range;
    for (auto a : altars_) {
        if (cheb(a, pos_) > sr) continue;
        if (cambc::vision::has_altar_lock(a)) {
            try {
                const auto lock = cambc::vision::altar_lock_at(a);
                if (lock.is_enemy() && me.can_attack() && cheb(a, pos_) <= range_) {
                    me.attack(lock);
                    attacked_ = true;
                    return true;
                }
            } catch (const std::exception& e) { warn("lock", e); }
            continue;
        }
        try { me.submit_gems(a); return true; } catch (const std::exception& e) { warn("submit", e); }
    }
    const int d = fdist(altar_f_, altar_seen_, pos_);
    const int dt = fdist(altar_t_f_, altar_t_seen_, pos_);
    if (dt < kFar && (d >= kFar || d > dt + 10)) {
        // Camps close the short way to an altar: ask the party to clear them.
        const int plug = find_plug();
        if (plug >= 0) {
            std::uint8_t v = static_cast<std::uint8_t>(plug + 1);
            cambc::shared_memory.write(static_cast<std::size_t>(kPlugByte + party_), std::span{&v, 1});
        }
    }
    if (d >= kFar) return false;
    const bool fighting = incoming_ > 0;
    const bool late = static_cast<int>(round_) + d + 8 >= static_cast<int>(cambc::constants::max_rounds);
    if (!late && fighting && gems < 40) return false;
    if (!late && d > 4 + gems) return false;
    cambc::bitboard spots = altar_spots_;
    for (auto a : altars_)
        if (cambc::vision::has_altar_lock(a)) {
            try {
                if (cambc::vision::altar_lock_at(a).is_enemy())
                    for (int dy = -sr; dy <= sr; ++dy)
                        for (int dx = -sr; dx <= sr; ++dx) clear_bit(spots, vec2{a.x + dx, a.y + dy});
            } catch (const std::exception& e) { warn("lock2", e); }
        }
    if (spots.is_empty()) spots = altar_spots_;
    step(spots, false);
    return !nopath_;
}

// Follow the terrain route to the altar until it runs into a living camp group.
int Bot::find_plug() const {
    vec2 p = pos_;
    const auto vis = cambc::vision::visible_tiles();
    for (int i = 0; i < 300; ++i) {
        const int here = fdist(altar_t_f_, altar_t_seen_, p);
        if (here == 0 || here >= kFar) return -1;
        vec2 best = p;
        int bd = here;
        for (auto d : kDirs) {
            const vec2 q = p + d;
            const int v = fdist(altar_t_f_, altar_t_seen_, q);
            if (v < bd) { bd = v; best = q; }
        }
        if (best == p) return -1;
        p = best;
        const int gi = group_at_[tidx(p)];
        if (gi >= 0 && g_[gi].state == Group::alive) return gi;
        if (raw_bit(vis.ptr(), p) && cambc::vision::has_camp(p)) {
            const auto c = cambc::vision::camp_at(p);
            const int cg = group_at_[tidx(c.spawn_position())];
            if (cg >= 0) return cg;
        }
    }
    return -1;
}

// ---------------------------------------------------------------- camps

float Bot::kill_time(const Group& g, float dps) const {
    return g.hp / std::max(1.0f, dps);
}

// Damage the nearest of us takes while we clear the group, killing the hardest hitters first.
float Bot::damage_taken(const Group& g, float dps) const {
    std::vector<std::pair<float, float>> c;  // str, hp
    for (auto k : g.kinds) { const auto s = camp_spec(k); c.emplace_back(s.str, s.hp); }
    std::sort(c.begin(), c.end(), [](auto a, auto b) { return a.first * b.second > b.first * a.second; });
    float t = 0, dmg = 0;
    for (auto [s, h] : c) { t += h / std::max(1.0f, dps); dmg += s * t; }
    const auto counts = cambc::self().item_count();
    float mult = 1.0f;
    if (counts[cambc::item_type::durability_emblem_of_health] + counts[cambc::item_type::durability_emblem_of_strength]
        + counts[cambc::item_type::durability_emblem_of_focus] > 0) mult *= 0.9f;
    if (counts[cambc::item_type::jungle_emblem_of_strength] + counts[cambc::item_type::jungle_emblem_of_health]
        + counts[cambc::item_type::jungle_emblem_of_focus] > 0) mult *= 0.7f;
    return dmg * mult;
}

// Damage per turn of the party members not away, scaled by our own item progress for the unseen ones.
float Bot::party_dps() const {
    const float mine = my_dps();
    const float scale = mine / std::max(1.0f, base_dps_[slot_]);
    float sum = mine;
    for (int s = 0; s < size_; ++s) {
        if (s == slot_ || party_of_[s] != party_ || read_status(s) == kAway) continue;
        sum += base_dps_[s] * scale;
    }
    return sum;
}

int Bot::choose_target(float dps) {
    ensure_me_field();
    const auto& me = cambc::self();
    const float budget = me.max_health() * kBudget;
    // Groups the other party is on.
    std::vector<int> taken;
    for (int s = 0; s < size_; ++s)
        if (party_of_[s] != party_) {
            const int v = read_status(s);
            if (v > 0 && v != kAway) taken.push_back(v - 1);
        }
    int best = -1;
    float bs = 0;
    for (int gi = 0; gi < static_cast<int>(g_.size()); ++gi) {
        const auto& g = g_[gi];
        if (g.avoid_until > round_) continue;
        if (std::find(taken.begin(), taken.end(), gi) != taken.end()) continue;
        int d = kFar;
        for (auto t : g.tiles) d = std::min(d, fdist(me_f_, me_seen_, t));
        if (d >= kFar) continue;
        float wait = 0, kt = kill_time(g, dps);
        if (g.state == Group::respawning) {
            const int left = g.ready - round_;
            if (left > 60) continue;
            wait = static_cast<float>(std::max(0, left - d));
        } else if (g.state == Group::reward) kt = 0;
        if (g.state != Group::reward && damage_taken(g, dps) > budget) continue;
        const float value = static_cast<float>(g.gems) + static_cast<float>(g.gold) * 0.12f;
        const float cost = static_cast<float>(d) + wait + kt + 5.0f + 0.25f * static_cast<float>(std::min(g.bank, 200));
        float s = value / cost;
        if (gi == target_) s *= 1.25f;
        if (s > bs) { bs = s; best = gi; }
    }
    return best;
}

// Attack the target group's camp in reach: hardest hitter first, then the weakest.
bool Bot::attack_camps(int group) {
    auto& me = cambc::self();
    if (attacked_ || !me.can_attack()) return false;
    const auto reach = me.attack_tiles();
    const auto vis = cambc::vision::visible_tiles();
    vec2 best{-1, -1};
    float bs = -1e9f;
    for (const auto& [p, gi] : camps_) {
        if (gi != group || !raw_bit(reach.ptr(), p) || !raw_bit(vis.ptr(), p)) continue;
        try {
            const auto c = cambc::vision::camp_at(p);
            const float s = c.strength() * 10.0f - c.health() * 0.01f;
            if (s > bs) { bs = s; best = p; }
        } catch (const std::exception& e) { warn("camp read", e); }
    }
    if (best.x < 0) return false;
    try {
        me.attack(cambc::vision::camp_at(best));
        attacked_ = true;
        reset_attack();
        return true;
    } catch (const std::exception& e) { warn("hit camp", e); }
    return false;
}

// After our own basic attack: reset it and hit again if anything of the target group is still in reach.
bool Bot::reset_attack() {
    auto& me = cambc::self();
    if (!attacked_ || cast_ || me.can_attack() || !me.can_cast()) return false;
    for (const auto& ab : me.abilities()) {
        if (!ab.is_attack_reset() || !ab.is_ready()) continue;
        try {
            const auto reach = me.attack_tiles();
            vec2 tgt{-1, -1};
            bool champ = false;
            for (const auto& f : foes_)
                if (raw_bit(reach.ptr(), f.pos) && cambc::vision::has_champion(f.pos)) { tgt = f.pos; champ = true; break; }
            if (tgt.x < 0)
                for (const auto& [p, gi] : camps_)
                    if (raw_bit(reach.ptr(), p) && cambc::vision::has_camp(p) && (gi == target_ || gi < 0)) { tgt = p; break; }
            if (tgt.x < 0) return false;
            if (!ab.use()) return false;
            cast_ = true;
            if (me.can_attack()) {
                if (champ) me.attack(cambc::vision::champion_at(tgt));
                else me.attack(cambc::vision::camp_at(tgt));
            }
            return true;
        } catch (const std::exception& e) { warn("reset", e); return false; }
    }
    return false;
}

void Bot::use_spells(int group) {
    auto& me = cambc::self();
    if (cast_ || !me.can_cast()) return;
    for (const auto& ab : me.abilities()) {
        if (!ab.info() || !ab.is_ready()) continue;
        if (exclusive(ab) && moved_) continue;
        try {
            bool ok = false;
            if (ab.is_damage() || ab.is_cc()) {
                const float r = ability_range(ab);
                vec2 best{-1, -1};
                float bs = -1;
                for (const auto& [p, gi] : camps_) {
                    if (gi != group || cheb(p, pos_) > r) continue;
                    const auto c = cambc::vision::camp_at(p);
                    if (ab.is_cc() && (!c.is_awake() || c.strength() <= 0)) continue;
                    const float s = c.strength() + 1.0f;
                    if (s > bs) { bs = s; best = p; }
                }
                if (best.x >= 0) ok = ab.use(cambc::vision::camp_at(best));
            } else if (ab.is_aoe_damage()) {
                const float r = ability_range(ab), rad = aoe_radius(ab);
                vec2 best{-1, -1};
                int bn = 0;
                for (const auto& [p, gi] : camps_) {
                    if (gi != group || cheb(p, pos_) > r) continue;
                    int n = 0;
                    for (const auto& [q, gj] : camps_)
                        if (cheb(p, q) <= rad && gj == group) ++n;
                    if (n > bn) { bn = n; best = p; }
                }
                if (best.x >= 0) ok = ab.use(best);
            } else if (ab.is_shield()) {
                if (incoming_ > 0 || !foes_.empty()) ok = ab.use();
            } else if (ab.is_heal()) {
                const float r = ability_range(ab);
                for (const auto& f : friends_)
                    if (cheb(f.pos, pos_) <= r && f.max_hp - f.hp >= ab.heal_amount() * 0.6f) {
                        ok = ab.use(cambc::vision::champion_at(f.pos));
                        break;
                    }
            }
            if (ok) {
                cast_ = true;
                if (exclusive(ab)) stop_move_ = true;
                return;
            }
        } catch (const std::exception& e) { warn("spell", e); }
    }
}

void Bot::farm(int target, int present, int need) {
    auto& me = cambc::self();
    auto& g = g_[target];
    if (g.state == Group::reward) {
        // Capture: stand on a spawn tile of the group.
        if (bit(g.board, pos_)) return;
        cambc::bitboard spots = g.board;
        const auto occ = cambc::vision::champion_tiles() | cambc::vision::camp_tiles();
        for (auto t : g.tiles) if (raw_bit(occ.ptr(), t)) clear_bit(spots, t);
        if (spots.is_empty()) spots = g.board;
        step(spots, false);
        return;
    }
    if (g.state == Group::respawning) {
        // Wait next to the group, never on it: an occupied spawn tile delays the respawn.
        bool near = false;
        for (auto t : g.tiles) near = near || cheb(t, pos_) <= 2;
        if (near && !bit(g.board, pos_)) return;
        cambc::bitboard ring;
        for (auto t : g.tiles)
            for (int dy = -2; dy <= 2; ++dy)
                for (int dx = -2; dx <= 2; ++dx) {
                    const vec2 q{t.x + dx, t.y + dy};
                    if (inside(q) && !raw_bit(terrain_.ptr(), q) && !raw_bit(g.board.ptr(), q)) raw_set(ring.ptr(), q);
                }
        step(ring, false);
        return;
    }
    bool awake = false;
    cambc::bitboard roots;
    bool any = false;
    for (const auto& [p, gi] : camps_)
        if (gi == target) {
            set_bit(roots, p);
            any = true;
            try { if (cambc::vision::camp_at(p).is_awake()) awake = true; } catch (const std::exception& e) { warn("awake", e); }
        }
    if (!any) roots = g.board;
    const bool go = awake || present >= need;
    if (go) {
        use_spells(target);
        if (attack_camps(target)) {
            use_spells(target);
            return;
        }
        if (step(roots, true)) {
            if (melee_) attack_camps(target);
            use_spells(target);
        }
    } else {
        // Wait close by for the rest of the party; sleeping camps do not hit.
        int dmin = 99;
        for (const auto& [p, gi] : camps_) if (gi == target) dmin = std::min(dmin, cheb(p, pos_));
        if (dmin > 3) step(roots, true);
    }
    (void)me;
}

// ---------------------------------------------------------------- turn

void Bot::play_turn() {
    round_ = static_cast<int>(cambc::round_number());
    auto& me = cambc::self();
    if (!init_) {
        initialise();
        return;
    }
    moved_ = attacked_ = cast_ = stop_move_ = false;
    debug_block({
        if (round_ % 25 == 0) std::cout << "r" << round_ << " s" << slot_ << " mode " << mode_ << " hp" << (me.is_alive() ? me.health() : -1.0f) << " pos(" << pos_.x << "," << pos_.y << ") np" << nopath_ << " carry" << me.gem_count() << " alt" << fdist(altar_f_, altar_seen_, pos_) << '\n';
    });
    mode_ = "?";
    if (!me.is_alive()) {
        mode_ = "dead";
        shop();
        write_status(kAway);
        target_ = -1;
        return;
    }
    pos_ = me.position();
    if (me.is_recalling() || me.is_teleporting()) { mode_ = "recalling"; write_status(kAway); return; }
    if (me.is_submitting_gems()) { mode_ = "submitting"; return; }
    observe();
    const bool at_spawn = bit(cambc::map::spawn_region(team::ally), pos_);
    if (at_spawn) shop();

    mode_ = "enemies";
    if (handle_enemies()) return;

    // Heal up fully on spawn before heading out.
    if (at_spawn && me.health() < me.max_health() * 0.95f) {
        mode_ = "healing";
        write_status(kAway);
        return;
    }
    mode_ = "tribute";
    if (tribute()) return;

    // Badly hurt: break away from camps, then recall (or walk when home is close).
    const float hp = me.health();
    if (hp < me.max_health() * 0.4f || (incoming_ > 0 && hp < incoming_ * 3.0f)) {
        mode_ = "hurt";
        write_status(kAway);
        const int sd = fdist(spawn_f_, spawn_seen_, pos_);
        if (incoming_ <= 0 && sd > 8) {
            try { me.recall(); return; } catch (const std::exception& e) { warn("recall", e); }
        }
        step(cambc::map::spawn_region(team::ally), false);
        return;
    }

    // Spend gold: items are permanent.
    if (!at_spawn && me.gold_count() >= kShopGold && me.gem_count() == 0 && incoming_ <= 0 && foes_.empty()) {
        mode_ = "shoptrip";
        write_status(kAway);
        if (fdist(spawn_f_, spawn_seen_, pos_) > 8) {
            try { me.recall(); return; } catch (const std::exception& e) { warn("recall shop", e); }
        }
        step(cambc::map::spawn_region(team::ally), false);
        return;
    }

    // Party target: the leader chooses, the others follow.
    const int leader = leader_slot();
    int n_avail = 0;
    for (int s = 0; s < size_; ++s)
        if (party_of_[s] == party_ && (s == slot_ || read_status(s) != kAway)) ++n_avail;
    int t = -1;
    if (leader == slot_) {
        std::uint8_t pv = 0;
        cambc::shared_memory.read(static_cast<std::size_t>(kPlugByte + party_), std::span{&pv, 1});
        const int plug = pv > 0 && pv - 1 < static_cast<int>(g_.size()) ? pv - 1 : -1;
        if (plug >= 0 && g_[plug].state == Group::respawning) {
            std::uint8_t z = 0;
            cambc::shared_memory.write(static_cast<std::size_t>(kPlugByte + party_), std::span{&z, 1});
        }
        if (plug >= 0 && g_[plug].state != Group::respawning
            && damage_taken(g_[plug], party_dps()) <= me.max_health() * kBudget) t = plug;
        else if (target_ < 0 || round_ - target_since_ >= 6 || g_[target_].state == Group::respawning
            || g_[target_].avoid_until > round_) {
            t = choose_target(party_dps());
            target_since_ = round_;
        } else t = target_;
    } else {
        const int v = read_status(leader);
        t = (v > 0 && v != kAway && v - 1 < static_cast<int>(g_.size())) ? v - 1 : -1;
        if (t < 0 && (target_ < 0 || round_ - target_since_ >= 6)) {
            t = choose_target(party_dps());
            target_since_ = round_;
        } else if (t < 0) t = target_;
    }
    target_ = t;
    write_status(t >= 0 ? t + 1 : 0);
    debug_block({
        if (round_ % 25 == 0) {
            std::cout << "r" << round_ << " s" << slot_ << " p(" << pos_.x << "," << pos_.y << ") t" << t << " n" << n_avail << " L" << leader;
            if (t >= 0) std::cout << " g(" << g_[t].center.x << "," << g_[t].center.y << ") st" << int(g_[t].state) << " gems" << g_[t].gems << " hp" << g_[t].hp;
            std::cout << " fuel" << cambc::fuel_remaining() << " groups" << g_.size() << " carry" << me.gem_count() << " altarD" << fdist(altar_f_, altar_seen_, pos_) << " nalt" << altars_.size() << " gold" << me.gold_count() << '\n';
        }
    });
    mode_ = "farm";
    if (t < 0) { mode_ = "idle"; return; }

    int present = 1;
    for (const auto& f : friends_)
        if (cheb(f.pos, pos_) <= 6) ++present;
    present = std::min(present, n_avail);
    int need = 1;
    const auto& g = g_[t];
    const float avg = party_dps() / static_cast<float>(std::max(1, n_avail));
    while (need < n_avail && damage_taken(g, avg * static_cast<float>(need)) > me.max_health() * kBudget) ++need;
    farm(t, present, need);
}
