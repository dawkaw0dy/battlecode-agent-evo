// Safe banker: every champion farms alone, banks gems early, heals on spawn, never takes an even fight.
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

// Shared memory: bytes 0..4 hold each slot's target group + 1; 5..7 the latest enemy sighting (x, y, round).
constexpr int kSightX = 5, kSightY = 6, kSightRound = 7;
constexpr int kSightAge = 20;      // rounds a sighting stays relevant
constexpr float kTrip = 0.55f;     // share of max health one trip into a group may cost
constexpr int kMaxTrips = 4;
constexpr int kShopGold = 200;
constexpr float kGoldValue = 0.35f;
constexpr float kFight = 1.2f;     // our power over theirs needed to start a fight
constexpr float kTrapped = 0.25f;  // ... or to hit back when a melee foe has us and we cannot outrun it
constexpr int kReplan = 12;        // rounds between target re-plans
constexpr bool kLocks = true;
constexpr int kLockRange = 2;      // altar lock placement reach
constexpr float kHelperHp = 3000;  // one more champion may join a group per this much camp health

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

float unit_power(const Unit& u) { return u.str * 1.12f + u.focus * 0.25f; }

}  // namespace

// ---------------------------------------------------------------- setup

void Bot::initialise() {
    init_ = true;
    init_dims();
    auto& me = cambc::self();
    const auto allies = cambc::champions(team::ally);
    size_ = static_cast<int>(allies.size());
    for (int i = 0; i < size_; ++i)
        if (allies[i] == me) slot_ = i;

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
        std::vector<std::pair<cambc::camp_type, vec2>> members;
        for (const auto& c : h.camps()) members.emplace_back(c.kind(), c.spawn_position());
        // Hardest hitters per health first: the order we kill them in.
        std::stable_sort(members.begin(), members.end(), [](const auto& a, const auto& b) {
            const auto sa = camp_spec(a.first), sb = camp_spec(b.first);
            return sa.str * sb.hp > sb.str * sa.hp;
        });
        for (const auto& [kind, tile] : members) {
            const auto s = camp_spec(kind);
            g.kinds.push_back(kind);
            g.camp_tile.push_back(tile);
            g.hp.push_back(s.hp);
            g.max_hp.push_back(s.hp);
            g.camp_str.push_back(s.str);
            g.full_hp += s.hp;
            g.str += s.str;
            g.range = std::max(g.range, s.range);
        }
        g.gems = static_cast<int>(h.gem_value());
        g.gold = static_cast<int>(h.gold_value());
        g.respawn = h.max_respawn_timer();
        long sx = 0, sy = 0;
        for (auto t : g.tiles) { sx += t.x; sy += t.y; }
        const long n = static_cast<long>(g.tiles.size());
        const vec2 mean{static_cast<int>(sx / n), static_cast<int>(sy / n)};
        g.center = g.tiles[0];
        for (auto t : g.tiles)
            if (cheb(t, mean) < cheb(g.center, mean)) g.center = t;
        for (auto t : g.tiles) {
            g.bank = std::min(g.bank, fdist(altar_t_f_, altar_t_seen_, t));
            g.home = std::min(g.home, fdist(spawn_f_, spawn_seen_, t));
        }
        const int gi = static_cast<int>(g_.size());
        for (auto t : g.tiles) group_at_[tidx(t)] = static_cast<std::int16_t>(gi);
        g_.push_back(std::move(g));
    }
    range_ = me.attack_range();
    melee_ = range_ <= 1;
    update_blocks();
}

// Camps we cannot see still block: route around every group we believe alive.
void Bot::update_blocks() {
    if (!block_dirty_ || round_ - block_round_ < 4) return;
    if (cambc::fuel_remaining() < 60000) return;
    block_dirty_ = false;
    block_round_ = round_;
    camp_block_.clear();
    cambc::bitboard roots = altar_spots_;
    for (const auto& g : g_)
        if (g.state == Group::alive) {
            camp_block_ |= g.board;
            for (auto t : g.tiles) clear_bit(roots, t);
        }
    altar_f_.compute(terrain_ | camp_block_, roots, altar_seen_);
}

// ---------------------------------------------------------------- shared memory

void Bot::write_status(int value) {
    std::uint8_t v = static_cast<std::uint8_t>(value);
    cambc::shared_memory.write(static_cast<std::size_t>(slot_), std::span{&v, 1});
}

int Bot::read_status(int slot) const {
    std::uint8_t v = 0;
    cambc::shared_memory.read(static_cast<std::size_t>(slot), std::span{&v, 1});
    return v;
}

void Bot::report_enemy() {
    const Unit* near = nullptr;
    for (const auto& f : foes_)
        if (!near || cheb(f.pos, pos_) < cheb(near->pos, pos_)) near = &f;
    if (!near) return;
    std::array<std::uint8_t, 3> v{static_cast<std::uint8_t>(near->pos.x), static_cast<std::uint8_t>(near->pos.y),
        static_cast<std::uint8_t>(round_ & 255)};
    cambc::shared_memory.write(kSightX, std::span{v.data(), 3});
    sight_ok_ = true;
    sight_ = near->pos;
}

void Bot::read_sighting() {
    std::array<std::uint8_t, 3> v{};
    cambc::shared_memory.read(kSightX, std::span{v.data(), 3});
    const int age = (round_ - v[2]) & 255;
    sight_ok_ = age <= kSightAge && !(v[0] == 0 && v[1] == 0 && v[2] == 0);
    sight_ = vec2{v[0], v[1]};
}

bool Bot::enemy_near(vec2 p, int r) const {
    return sight_ok_ && cheb(p, sight_) <= r;
}

// ---------------------------------------------------------------- observation

void Bot::observe() {
    foes_.clear();
    friends_.clear();
    camps_.clear();
    awake_.clear();
    incoming_ = 0;
    const auto& enemy_spawn = cambc::map::spawn_region(team::enemy);
    for (auto p : cambc::vision::champion_tiles(team::enemy).positions()) {
        if (bit(enemy_spawn, p)) continue;  // they heal and shop there; we never go in
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
            const vec2 sp = c.spawn_position();
            const int gi = inside(sp) ? group_at_[tidx(sp)] : -1;
            camps_.emplace_back(p, gi);
            if (gi >= 0) {
                auto& g = g_[gi];
                if (g.state != Group::alive) {
                    if (g.state == Group::respawning) g.hp = g.max_hp;
                    g.state = Group::alive;
                    block_dirty_ = true;
                }
                for (std::size_t i = 0; i < g.camp_tile.size(); ++i)
                    if (g.camp_tile[i] == sp) g.hp[i] = c.health();
            }
            if (cheb(p, pos_) <= 7 && c.is_awake()) {
                const Awake a{p, c.attack_range(), c.strength()};
                awake_.push_back(a);
                if (cheb(p, pos_) <= a.range) incoming_ += a.str;
            }
        } catch (const std::exception& e) { warn("camp", e); }
    }
    const auto vis = cambc::vision::visible_tiles();
    const auto camp_board = cambc::vision::camp_tiles();
    living_.assign(g_.size(), 0);
    for (const auto& [p, cg] : camps_)
        if (cg >= 0) living_[static_cast<std::size_t>(cg)] = 1;
    int queries = 0;
    for (int gi = 0; gi < static_cast<int>(g_.size()); ++gi) {
        auto& g = g_[gi];
        if (cheb(g.center, pos_) > 10) continue;
        if (g.state == Group::respawning && g.ready > round_ + 1) continue;
        bool seen = false;
        for (auto t : g.tiles)
            if (raw_bit(vis.ptr(), t)) seen = true;
        if (!seen) continue;
        if (living_[static_cast<std::size_t>(gi)]) {
            // A visible spawn tile without its camp: that camp is dead.
            for (std::size_t i = 0; i < g.camp_tile.size(); ++i) {
                const vec2 t = g.camp_tile[i];
                if (raw_bit(vis.ptr(), t) && !raw_bit(camp_board.ptr(), t)) g.hp[i] = 0;
            }
            continue;
        }
        if (queries >= 3 || cambc::fuel_remaining() < 30000) continue;
        ++queries;
        try {
            const auto st = g.h.state();
            const auto before = g.state;
            if (std::holds_alternative<cambc::map::camp_group_alive>(st)) g.state = Group::alive;
            else if (std::holds_alternative<cambc::map::camp_group_reward_available>(st)) {
                g.state = Group::reward;
                g.reward_seen = round_;
            }
            else if (const auto* r = std::get_if<cambc::map::camp_group_respawning>(&st)) {
                g.state = Group::respawning;
                g.ready = round_ + std::max(1, r->timer);
            }
            if (g.state != Group::alive)
                for (auto& h : g.hp) h = 0;
            if (g.state != before) block_dirty_ = true;
        } catch (const std::exception& e) { warn("group", e); }
    }
    // A reward we saw a while ago has most likely been captured by now.
    for (auto& g : g_)
        if (g.state == Group::reward && round_ - g.reward_seen > 8) {
            g.state = Group::respawning;
            g.ready = g.reward_seen + 5 + g.respawn;
        }
    // A respawn we expected has happened.
    for (auto& g : g_)
        if (g.state == Group::respawning && g.ready <= round_) {
            g.state = Group::alive;
            g.hp = g.max_hp;
            block_dirty_ = true;
        }
    update_blocks();
}

// ---------------------------------------------------------------- shopping

void Bot::shop() {
    auto& me = cambc::self();
    using it = cambc::item_type;
    struct Step { it shard, emblem; };
    // Passives that keep us alive while farming faster: Jungle (30% less from camps, strength), Durability
    // (10% less from everything, health), Home (3-turn recall, strength); then plain stats.
    const std::array plan{Step{it::jungle_shard_of_strength, it::jungle_emblem_of_strength},
        Step{it::durability_shard_of_health, it::durability_emblem_of_health},
        Step{it::home_shard_of_strength, it::home_emblem_of_strength}};
    // An altar lock first: ours block only enemy tributes (10 gold, one carried at a time).
    try {
        if (kLocks && me.consumables()[cambc::consumable_type::altar_lock].count() == 0
            && me.can_buy(cambc::consumable_type::altar_lock))
            me.buy(cambc::consumable_type::altar_lock);
    } catch (const std::exception& e) { warn("buy lock", e); }
    for (int guard = 0; guard < 16; ++guard) {
        if (me.gold_count() < 30) return;
        const auto counts = me.item_count();
        it pick = it::power_shard_of_strength;
        bool planned = false;
        for (const auto& s : plan)
            if (counts[s.emblem] == 0) { pick = s.shard; planned = true; break; }
        if (!planned) {
            // Alternate damage and health once the passives are in.
            const unsigned st = counts[it::power_shard_of_strength] + 6 * counts[it::power_emblem_of_strength];
            const unsigned hp = counts[it::power_shard_of_health] + 6 * counts[it::power_emblem_of_health];
            pick = st <= hp * 2 ? it::power_shard_of_strength : it::power_shard_of_health;
        }
        try {
            if (!me.can_buy(pick, 1)) return;
            me.buy(pick, 1);
        } catch (const std::exception& e) { warn("buy", e); return; }
    }
}

// A recall only starts on a turn we have not acted in yet.
bool Bot::try_recall() {
    auto& me = cambc::self();
    if (moved_ || attacked_ || cast_ || !me.can_move()) return false;
    try {
        me.recall();
        return true;
    } catch (const std::exception& e) { warn("recall", e); }
    return false;
}

// ---------------------------------------------------------------- movement

// Step towards the nearest root. Roots may be occupied (camps): the walk stops next to them.
bool Bot::step(const cambc::bitboard& roots_in) {
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
    cambc::bitboard root;
    set_bit(root, pos_);
    me_f_.compute(terrain_, root, me_seen_);
    me_f_round_ = round_;
    me_f_pos_ = pos_;
}

// Follow the terrain route to the altar until it runs into a living camp group.
int Bot::find_plug() const {
    vec2 p = pos_;
    for (int i = 0; i < 300; ++i) {
        const int here = fdist(altar_t_f_, altar_t_seen_, p);
        if (here == 0 || here >= kFar) return -1;
        vec2 best = p;
        int bd = here;
        for (auto d : kDirs) {
            const int v = fdist(altar_t_f_, altar_t_seen_, p + d);
            if (v < bd) { bd = v; best = p + d; }
        }
        if (best == p) return -1;
        p = best;
        const int gi = group_at_[tidx(p)];
        if (gi >= 0 && g_[gi].state == Group::alive) return gi;
    }
    return -1;
}

// ---------------------------------------------------------------- strength estimates

float Bot::my_dps() const { return dps_; }
float Bot::taken_mult() const { return taken_; }

// Values that change only with items: read once a turn (item_count is a host call).
void Bot::measure() {
    using it = cambc::item_type;
    const auto& me = cambc::self();
    dps_ = me.strength() * 1.12f + me.focus() * 0.2f;
    const auto c = me.item_count();
    taken_ = 1.0f;
    if (c[it::durability_emblem_of_health] + c[it::durability_emblem_of_strength] + c[it::durability_emblem_of_focus] > 0) taken_ *= 0.9f;
    if (c[it::jungle_emblem_of_strength] + c[it::jungle_emblem_of_health] + c[it::jungle_emblem_of_focus] > 0) taken_ *= 0.7f;
}

float Bot::remaining_hp(const Group& g, bool full) const {
    if (full) return g.full_hp;
    float s = 0;
    for (auto h : g.hp) s += h;
    return s;
}

// Damage we take while clearing what is left of the group alone, hardest hitters first.
// Camps are stored hardest hitter first (sorted once at start), so this is one pass.
float Bot::damage_taken(const Group& g, float dps, bool full) const {
    const float inv = 1.0f / std::max(1.0f, dps);
    float t = 0, dmg = 0;
    const std::size_t n = g.hp.size();
    for (std::size_t i = 0; i < n; ++i) {
        const float h = full ? g.max_hp[i] : g.hp[i];
        if (h <= 0) continue;
        t += h * inv;
        dmg += g.camp_str[i] * t;
    }
    return dmg * taken_;
}

// ---------------------------------------------------------------- enemies

bool Bot::spell_on_foe(bool stun_only) {
    auto& me = cambc::self();
    if (cast_ || !me.can_cast() || foes_.empty()) return false;
    for (const auto& ab : me.abilities()) {
        if (!ab.info() || !ab.is_ready() || !ab.can_target_enemies()) continue;
        if (exclusive(ab) && moved_) continue;
        if (stun_only && !ab.is_cc()) continue;
        if (!(ab.is_damage() || ab.is_cc() || ab.is_aoe_damage())) continue;
        const float r = ability_range(ab);
        const Unit* best = nullptr;
        for (const auto& f : foes_) {
            if (cheb(f.pos, pos_) > r) continue;
            if (!best || (stun_only ? cheb(f.pos, pos_) < cheb(best->pos, pos_) : f.hp < best->hp)) best = &f;
        }
        if (!best) continue;
        try {
            bool ok = false;
            if (ab.is_aoe_damage() && ab.can_target_ground()) ok = ab.use(best->pos);
            else ok = ab.use(cambc::vision::champion_at(best->pos));
            if (ok) {
                cast_ = true;
                if (exclusive(ab)) stop_move_ = true;
                return true;
            }
        } catch (const std::exception& e) { warn("foe spell", e); }
    }
    return false;
}

// Score of a tile to run to: out of every foe's reach, then towards home and allies, away from awake camps.
float Bot::flee_score(vec2 q) const {
    int m = 99;
    for (const auto& f : foes_) m = std::min(m, cheb(f.pos, q) - f.range);
    float camp = 0;
    for (const auto& a : awake_) if (cheb(a.pos, q) <= a.range) camp += a.str;
    bool ally = false;
    for (const auto& f : friends_) ally = ally || cheb(f.pos, q) <= 2;
    const int sd = fdist(spawn_f_, spawn_seen_, q);
    return static_cast<float>(std::min(m, 4) * 12 - std::min(sd, 200) + (ally ? 6 : 0)) - camp * 0.2f;
}

const cambc::ability* Bot::ready_dash(std::array<cambc::ability, 2>& abs) const {
    const auto& me = cambc::self();
    if (cast_ || !me.can_cast()) return nullptr;
    for (const auto& ab : abs)
        if (ab.info() && ab.is_dash() && ab.is_ready()) return &ab;
    return nullptr;
}

void Bot::flee() {
    auto& me = cambc::self();
    // Bank on the way if an altar is right here: damage does not stop a tribute.
    if (me.gem_count() > 0 && me.gem_count() <= 20)
        for (auto a : altars_)
            if (cheb(a, pos_) <= cambc::constants::submit_gems_range && free_altar(a)) {
                try { me.submit_gems(a); return; } catch (const std::exception& e) { warn("submit", e); }
            }
    int dmin = 99;
    for (const auto& f : foes_) dmin = std::min(dmin, cheb(f.pos, pos_));
    if (bit(cambc::map::spawn_region(team::ally), pos_)) return;
    // Out of reach: recall only what is worth protecting (our health or a bag of gems).
    const bool worth = me.health() < me.max_health() * 0.6f || me.gem_count() >= 12;
    if (worth && dmin >= 6 && incoming_ <= 0 && fdist(spawn_f_, spawn_seen_, pos_) > 8) {
        if (try_recall()) { write_status(0); return; }
    }
    spell_on_foe(true);
    // A melee champion hits back on the way out.
    if (melee_ && me.can_attack())
        for (const auto& f : foes_)
            if (cheb(f.pos, pos_) <= range_) {
                try { me.attack(cambc::vision::champion_at(f.pos)); attacked_ = true; } catch (const std::exception& e) { warn("hit back", e); }
                break;
            }
    cambc::bitboard blocked = terrain_ | cambc::vision::champion_tiles() | cambc::vision::camp_tiles();
    blocked |= camp_block_;
    // Dash first (two tiles), then walk: three tiles outrun any chaser.
    auto abs = me.abilities();
    if (const auto* dash = ready_dash(abs)) {
        std::array<std::pair<float, vec2>, 25> cand{};
        std::size_t n = 0;
        for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -2; dx <= 2; ++dx) {
                const vec2 q{pos_.x + dx, pos_.y + dy};
                if (cheb(q, pos_) != 2 || !inside(q) || raw_bit(blocked.ptr(), q)) continue;
                cand[n++] = {flee_score(q), q};
            }
        std::sort(cand.begin(), cand.begin() + static_cast<long>(n), [](auto a, auto b) { return a.first > b.first; });
        const float here = flee_score(pos_);
        for (std::size_t i = 0; i < n && i < 3; ++i) {
            if (cand[i].first <= here + 6) break;
            try {
                if (dash->use(cand[i].second)) {
                    cast_ = true;
                    pos_ = me.position();
                    blocked = terrain_ | cambc::vision::champion_tiles() | cambc::vision::camp_tiles();
                    blocked |= camp_block_;
                    clear_bit(blocked, pos_);
                    break;
                }
            } catch (const std::exception& e) { (void)e; }  // no legal dash path there: try the next tile
        }
    }
    if (!me.can_move() || moved_ || stop_move_) return;
    vec2 best = pos_;
    float bs = flee_score(pos_);
    for (auto d : kDirs) {
        const vec2 q = pos_ + d;
        if (!inside(q) || raw_bit(blocked.ptr(), q)) continue;
        const float sc = flee_score(q);
        if (sc > bs) { bs = sc; best = q; }
    }
    if (best == pos_) return;
    try {
        if (me.move_absolute(best)) { moved_ = true; pos_ = best; }
        else stop_move_ = true;
    } catch (const std::exception& e) { warn("flee", e); stop_move_ = true; }
}

// Fight when stronger; hit back when a melee foe has us and running cannot shake it; otherwise run.
// True when the turn is spent.
bool Bot::handle_enemies() {
    if (foes_.empty()) return false;
    auto& me = cambc::self();
    report_enemy();
    float our_hp = me.health(), our_dps = my_dps();
    for (const auto& f : friends_)
        if (cheb(f.pos, pos_) <= 6) { our_hp += f.hp; our_dps += unit_power(f); }
    float their_hp = 0, their_dps = 0;
    int dmin = 99;
    bool caught = false, melee_on_us = false;
    for (const auto& f : foes_) {
        const int d = cheb(f.pos, pos_);
        dmin = std::min(dmin, d);
        if (d > 9) continue;
        their_hp += f.hp;
        their_dps += unit_power(f);
        if (d <= f.range + 1) caught = true;
        if (f.range <= 1 && d <= 2) melee_on_us = true;
    }
    if (their_hp <= 0) return false;
    const float ratio = (our_hp * our_dps) / std::max(1.0f, their_hp * their_dps);
    const Unit* weak = nullptr;
    for (const auto& f : foes_)
        if (cheb(f.pos, pos_) <= range_ && (!weak || f.hp < weak->hp)) weak = &f;
    const bool finish = weak && weak->hp <= me.strength() * 1.2f;
    auto abs = me.abilities();
    const bool can_dash = ready_dash(abs) != nullptr;
    const int sd = fdist(spawn_f_, spawn_seen_, pos_);
    const bool trapped = melee_on_us && !can_dash && sd > 3 && ratio >= kTrapped;
    const bool fight = ratio >= kFight || finish || trapped;
    debug_block({
        std::cout << "F r" << round_ << " s" << slot_ << " hp" << me.health() << "/" << me.max_health() << " p(" << pos_.x << "," << pos_.y << ") dmin" << dmin << " R" << ratio << " caught" << caught << " fight" << fight << " fr" << friends_.size();
        for (const auto& f : foes_) std::cout << " [" << int(f.type) << " " << f.pos.x << "," << f.pos.y << " hp" << f.hp << " s" << f.str << " r" << f.range << "]";
        std::cout << '\n';
    });
    if (!fight && !caught && dmin > 7) return false;
    if (fight) {
        spell_on_foe(false);
        if (weak && me.can_attack()) {
            try {
                me.attack(cambc::vision::champion_at(weak->pos));
                attacked_ = true;
                reset_attack();
            } catch (const std::exception& e) { warn("hit foe", e); }
        }
        // Chase only a short way, and only what we can finish: walking after champions does not bank gems.
        const Unit* prey = nullptr;
        for (const auto& f : foes_)
            if (cheb(f.pos, pos_) <= 3 && (!prey || f.hp < prey->hp)) prey = &f;
        const bool chase = prey && (prey->hp <= my_dps() * 3.0f || ratio >= 3.0f);
        if (!attacked_ && ratio >= kFight && chase) {
            cambc::bitboard roots;
            for (const auto& f : foes_) set_bit(roots, f.pos);
            if (step(roots) && melee_ && me.can_attack())
                for (const auto& f : foes_)
                    if (cheb(f.pos, pos_) <= range_ && cambc::vision::has_champion(f.pos)) {
                        try { me.attack(cambc::vision::champion_at(f.pos)); attacked_ = true; } catch (const std::exception& e) { warn("hit foe2", e); }
                        break;
                    }
        }
        mode_ = "fight";
        return attacked_ || moved_ || cast_ || trapped;
    }
    if (target_ >= 0 && cheb(g_[target_].center, pos_) <= 10) g_[target_].avoid_until = round_ + 30;
    mode_ = "flee";
    flee();
    return true;
}

// An altar we may tribute at: no lock, or our own.
bool Bot::free_altar(vec2 a) const {
    if (!cambc::vision::has_altar_lock(a)) return true;
    try {
        return cambc::vision::altar_lock_at(a).is_ally();
    } catch (const std::exception& e) { warn("lock read", e); }
    return false;
}

// Lock any free altar within reach: the enemy cannot tribute there until it breaks the lock.
void Bot::lock_altars() {
    if (!kLocks) return;
    auto& me = cambc::self();
    bool near = false;
    for (auto a : altars_) near = near || cheb(a, pos_) <= kLockRange;
    if (!near) return;
    const auto& lock = me.consumables()[cambc::consumable_type::altar_lock];
    if (lock.count() == 0) return;
    for (auto a : altars_) {
        if (cheb(a, pos_) > kLockRange || cambc::vision::has_altar_lock(a)) continue;
        try {
            if (lock.use(a)) return;
        } catch (const std::exception& e) { warn("lock altar", e); return; }
    }
}

// ---------------------------------------------------------------- banking

bool Bot::tribute() {
    auto& me = cambc::self();
    const int gems = static_cast<int>(me.gem_count());
    if (gems <= 0) return false;
    const int sr = cambc::constants::submit_gems_range;
    for (auto a : altars_) {
        if (cheb(a, pos_) > sr) continue;
        if (!free_altar(a)) {
            try {
                const auto lock = cambc::vision::altar_lock_at(a);
                if (me.can_attack() && cheb(a, pos_) <= range_) {
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
    if (round_ - plug_round_ >= 10) {
        plug_ = (dt < kFar && (d >= kFar || d > dt + 10)) ? find_plug() : -1;
        plug_round_ = round_;
    }
    if (d >= kFar) return false;
    const bool late = round_ + d + 8 >= static_cast<int>(cambc::constants::max_rounds);
    if (!late && incoming_ > 0 && gems < 40) return false;
    const bool threat = enemy_near(pos_, 25) && gems >= 8;
    if (!late && !threat && gems < 40 && static_cast<float>(d) > 6.0f + static_cast<float>(gems) * 0.8f) return false;
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
    step(spots);
    return !nopath_;
}

// ---------------------------------------------------------------- camps

int Bot::choose_target() {
    ensure_me_field();
    const auto& me = cambc::self();
    const float dps = std::max(1.0f, my_dps());
    const float per_trip = me.max_health() * kTrip;
    std::array<int, 8> taken{};
    taken.fill(-1);
    for (int s = 0; s < size_ && s < 5; ++s)
        if (s != slot_) {
            const int v = read_status(s);
            if (v > 0 && v <= static_cast<int>(g_.size())) taken[static_cast<std::size_t>(s)] = v - 1;
        }
    auto claimants = [&](int gi) {
        int n = 0;
        for (auto t : taken) n += t == gi;
        return n;
    };
    int best = -1;
    float bs = 0;
    for (int gi = 0; gi < static_cast<int>(g_.size()); ++gi) {
        auto& g = g_[gi];
        if (g.avoid_until > round_) continue;
        // Big groups take helpers: they share the clear time and the camp fire.
        const int others = claimants(gi);
        const int cap = 1 + static_cast<int>(g.full_hp / kHelperHp);
        if (others >= cap) continue;
        const float share = 1.0f / static_cast<float>(others + 1);
        const int d = fdist(me_f_, me_seen_, g.center);
        if (d >= kFar) continue;
        float wait = 0;
        const bool full = g.state == Group::respawning;
        if (full) wait = static_cast<float>(std::max(0, g.ready - round_ - d));
        float clear = 0, extra = 0;
        if (g.state != Group::reward) {
            const float dmg = damage_taken(g, dps / share, full) * share;
            const int trips = std::max(1, static_cast<int>(std::ceil(dmg / per_trip)));
            if (trips > kMaxTrips) continue;
            clear = remaining_hp(g, full) / dps * share;
            extra = static_cast<float>((trips - 1) * (6 + std::min(g.home, 150)));
        }
        float value = (static_cast<float>(g.gems) + static_cast<float>(g.gold) * kGoldValue) * share;
        if (gi == plug_) value += 25.0f;
        if (enemy_near(g.center, 10)) value *= 0.3f;
        const float cost = static_cast<float>(d) + wait + clear + extra + 6.0f + 0.3f * static_cast<float>(std::min(g.bank, 100));
        float s = value / cost;
        if (gi == target_) s *= 1.5f;
        if (s > bs) { bs = s; best = gi; }
    }
    return best;
}

// Leave the reach of awake camps when staying would cost too much of our health.
bool Bot::disengage() {
    if (incoming_ <= 0) return false;
    auto& me = cambc::self();
    const float hp = me.health();
    int exit_steps = 1;
    for (const auto& a : awake_) exit_steps = std::max(exit_steps, a.range - cheb(a.pos, pos_) + 1);
    if (hp >= incoming_ * taken_mult() * static_cast<float>(exit_steps + 1) + me.max_health() * 0.1f) return false;
    if (!me.can_move() || moved_ || stop_move_) return false;
    cambc::bitboard blocked = terrain_ | cambc::vision::champion_tiles() | cambc::vision::camp_tiles();
    blocked |= camp_block_;
    auto threat = [&](vec2 q) {
        float s = 0;
        for (const auto& a : awake_) if (cheb(a.pos, q) <= a.range) s += a.str;
        return s;
    };
    vec2 best = pos_;
    float bs = threat(pos_);
    int bd = fdist(spawn_f_, spawn_seen_, pos_);
    for (auto d : kDirs) {
        const vec2 q = pos_ + d;
        if (!inside(q) || raw_bit(blocked.ptr(), q)) continue;
        const float s = threat(q);
        const int sd = fdist(spawn_f_, spawn_seen_, q);
        if (s < bs || (s == bs && sd < bd)) { bs = s; bd = sd; best = q; }
    }
    if (best == pos_) return false;
    try {
        if (me.move_absolute(best)) { moved_ = true; pos_ = best; mode_ = "disengage"; return true; }
        stop_move_ = true;
    } catch (const std::exception& e) { warn("disengage", e); stop_move_ = true; }
    return false;
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
        if (!cambc::vision::has_camp(p)) continue;  // killed earlier this turn
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

// After our own basic attack: reset it and hit again if anything is still in reach.
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
                    if (raw_bit(reach.ptr(), p) && cambc::vision::has_camp(p) && gi == target_) { tgt = p; break; }
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
    // Spells that root us only when a camp is already in basic reach (we would not walk anyway).
    bool in_reach = false;
    for (const auto& [p, gi] : camps_)
        if (gi == group && cheb(p, pos_) <= range_) in_reach = true;
    for (const auto& ab : me.abilities()) {
        if (!ab.info() || !ab.is_ready()) continue;
        if (exclusive(ab) && (moved_ || !in_reach)) continue;
        try {
            bool ok = false;
            if ((ab.is_damage() || ab.is_cc()) && ab.can_target_camps()) {
                const float r = ability_range(ab);
                vec2 best{-1, -1};
                float bs = -1;
                for (const auto& [p, gi] : camps_) {
                    if (gi != group || cheb(p, pos_) > r || !cambc::vision::has_camp(p)) continue;
                    const auto c = cambc::vision::camp_at(p);
                    if (ab.is_cc() && (!c.is_awake() || c.strength() <= 0)) continue;
                    const float s = c.strength() + 1.0f;
                    if (s > bs) { bs = s; best = p; }
                }
                if (best.x >= 0) ok = ab.use(cambc::vision::camp_at(best));
            } else if (ab.is_aoe_damage() && ab.can_target_ground()) {
                const float r = ability_range(ab), rad = aoe_radius(ab);
                vec2 best{-1, -1};
                int bn = 0;
                for (const auto& [p, gi] : camps_) {
                    if (gi != group || cheb(p, pos_) > r || !cambc::vision::has_camp(p)) continue;
                    int n = 0;
                    for (const auto& [q, gj] : camps_)
                        if (gj == group && cheb(p, q) <= rad) ++n;
                    if (n > bn) { bn = n; best = p; }
                }
                if (best.x >= 0) ok = ab.use(best);
            } else if (ab.is_shield()) {
                if (incoming_ > 0) ok = ab.use();
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

void Bot::farm(int target) {
    auto& g = g_[target];
    if (g.state == Group::reward) {
        // Capture: stand on a spawn tile of the group.
        mode_ = "capture";
        if (bit(g.board, pos_)) return;
        cambc::bitboard spots = g.board;
        const auto occ = cambc::vision::champion_tiles() | cambc::vision::camp_tiles();
        for (auto t : g.tiles) if (raw_bit(occ.ptr(), t)) clear_bit(spots, t);
        if (spots.is_empty()) spots = g.board;
        step(spots);
        return;
    }
    if (g.state == Group::respawning) {
        // Wait next to the group, never on it: an occupied spawn tile delays the respawn.
        mode_ = "wait";
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
        step(ring);
        return;
    }
    mode_ = "farm";
    if (disengage()) return;
    use_spells(target);
    if (attack_camps(target)) {
        mode_ = "hit";
        use_spells(target);
        return;
    }
    cambc::bitboard roots;
    bool any = false;
    for (const auto& [p, gi] : camps_)
        if (gi == target) { set_bit(roots, p); any = true; }
    if (!any)
        for (std::size_t i = 0; i < g.camp_tile.size(); ++i)
            if (g.hp[i] > 0) { set_bit(roots, g.camp_tile[i]); any = true; }
    if (!any) roots = g.board;
    if (!step(roots) && !nopath_) mode_ = cast_ ? "cast" : "farm-stuck";
    if (moved_) {
        mode_ = cheb(g.center, pos_) <= 4 ? "walk-near" : "walk";
        if (melee_ && attack_camps(target)) mode_ = "walk-hit";
        use_spells(target);
    } else if (nopath_) {
        mode_ = "nopath";
        g.avoid_until = round_ + 20;
        target_ = -1;
    }
}

// ---------------------------------------------------------------- turn

void Bot::play_turn() {
    try {
        turn();
    } catch (const std::exception& e) { warn("turn", e); }
    debug_block({
        const auto left = cambc::fuel_remaining();
        if (left < 30000 || static_cast<int>(cambc::round_number()) != round_) {
            std::cout << "LOWFUEL r" << round_ << " s" << slot_ << " left" << left << " mode " << mode_;
            for (auto v : prof_) std::cout << " " << v;
            std::cout << '\n';
        }
        prof_.fill(0);
    });
}

void Bot::turn() {
    round_ = static_cast<int>(cambc::round_number());
    auto& me = cambc::self();
    if (!init_) {
        initialise();
        return;
    }
    moved_ = attacked_ = cast_ = stop_move_ = nopath_ = false;
    debug_block({
        if (round_ % 25 == 0) std::cout << "r" << round_ << " s" << slot_ << " mode " << mode_ << " hp" << (me.is_alive() ? me.health() : -1.0f) << " t" << target_ << " pos(" << pos_.x << "," << pos_.y << ") carry" << (me.is_alive() ? me.gem_count() : 0u) << " gold" << me.gold_count() << '\n';
    });
    debug_block({ std::cout << "M " << slot_ << " " << mode_ << " r" << round_ << " t" << target_ << " p(" << pos_.x << "," << pos_.y << ")";
        if (target_ >= 0) std::cout << " c(" << g_[target_].center.x << "," << g_[target_].center.y << ") st" << int(g_[target_].state);
        std::cout << '\n'; });
    const char* prev_mode = mode_;
    mode_ = "?";
    if (!me.is_alive()) {
        debug_block({ if (prev_mode[0] != 'd') std::cout << "DIED r" << round_ << " s" << slot_ << " lastmode " << prev_mode << '\n'; });
        mode_ = "dead";
        shop();
        write_status(0);
        target_ = -1;
        return;
    }
    pos_ = me.position();
    if (me.is_recalling() || me.is_teleporting()) { mode_ = "recalling"; return; }
    if (me.is_submitting_gems()) { mode_ = "submitting"; return; }
    if (!me.can_move() && !me.can_attack() && !me.can_cast()) { mode_ = "stunned"; return; }
    prof_[0] = cambc::fuel_remaining();
    observe();
    prof_[1] = cambc::fuel_remaining();
    const bool at_spawn = bit(cambc::map::spawn_region(team::ally), pos_);
    if (at_spawn) shop();
    measure();
    read_sighting();
    lock_altars();

    if (handle_enemies()) return;

    // Heal up on spawn before heading out.
    if (at_spawn && me.health() < me.max_health() * 0.9f) {
        mode_ = "healing";
        return;
    }
    mode_ = "tribute";
    prof_[2] = cambc::fuel_remaining();
    if (tribute()) return;
    prof_[3] = cambc::fuel_remaining();

    const float hp = me.health();
    const int sd = fdist(spawn_f_, spawn_seen_, pos_);
    // Hurt and out of camp reach: go home to heal unless the target is nearly finished.
    if (incoming_ <= 0 && !at_spawn && hp < me.max_health() * 0.45f) {
        bool finish = false;
        if (target_ >= 0 && g_[target_].state == Group::alive && cheb(g_[target_].center, pos_) <= 6)
            finish = damage_taken(g_[target_], std::max(1.0f, my_dps())) < hp - me.max_health() * 0.15f;
        if (!finish) {
            mode_ = "hurt";
            if (sd > 6) {
                if (try_recall()) return;
            }
            step(cambc::map::spawn_region(team::ally));
            return;
        }
    }

    // Spend gold between fights: items are permanent.
    if (!at_spawn && incoming_ <= 0 && me.gold_count() >= kShopGold && me.gem_count() == 0) {
        bool busy = false;
        if (target_ >= 0 && g_[target_].state == Group::alive)
            for (const auto& a : awake_) busy = busy || group_at_[tidx(a.pos)] == target_;
        if (!busy) {
            mode_ = "shoptrip";
            if (sd > 6) {
                if (try_recall()) return;
            }
            step(cambc::map::spawn_region(team::ally));
            return;
        }
    }

    bool replan = target_ < 0 || round_ - target_since_ >= kReplan || g_[target_].avoid_until > round_;
    if (replan && target_ >= 0 && cambc::fuel_remaining() < 45000) replan = false;  // a full-map search costs ~30k
    if (!replan && g_[target_].state == Group::respawning && g_[target_].ready - round_ > 12) replan = true;
    if (replan) {
        const int prev = target_;
        prof_[4] = cambc::fuel_remaining();
        target_ = choose_target();
        prof_[5] = cambc::fuel_remaining();
        target_since_ = round_;
        debug_block({
            if (prev != target_ && prev >= 0 && g_[prev].state == Group::alive && g_[prev].avoid_until <= round_)
                std::cout << "SW r" << round_ << " s" << slot_ << " " << prev << "->" << target_ << " dprev" << cheb(g_[prev].center, pos_) << " dnew" << (target_ >= 0 ? cheb(g_[target_].center, pos_) : -1) << '\n';
        });
    }
    write_status(target_ >= 0 ? target_ + 1 : 0);
    if (static_cast<int>(cambc::round_number()) != round_) return;  // out of fuel: this turn resumed on stale data
    if (target_ < 0) {
        mode_ = "idle";
        return;
    }
    prof_[6] = cambc::fuel_remaining();
    farm(target_);
}
