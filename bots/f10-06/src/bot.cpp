// Big-game hunter. Champions farm cheap groups alone, gather for groups one champion cannot clear,
// and the whole team commits to golems and monoliths once its shards make the fight survivable.
// Camps do not heal, so a raid that has to retreat keeps its progress.
#include <cambc.h>
#include "bot.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <optional>
#include <span>
#include <variant>

namespace {

constexpr bool kDebug = false;
constexpr int kFar = 65535;
constexpr float kPack = 0.0f;
constexpr int kRaidParty = 3;      // champions a boss raid waits for
constexpr bool kScout = true;      // ward unseen bushes beside us while enemies are about
constexpr int kAvoidRounds = 15;   // skip groups where an enemy was seen this recently
constexpr int kHelpRange = 0;      // answer an ally's fight within this many tiles
constexpr int kThreatPad = 5;      // retreat from a fight we would lose once enemies are this far beyond their range
constexpr float kEngage = 1.2f;   // fight when we kill them in this fraction of the time they need
constexpr int kBankGems = 12, kBankGemsHot = 6;
constexpr float kBankPerTile = 1.0f;  // bank once the bag holds this many gems per tile of walk to the altar
constexpr float kFleeHp = 0.3f, kFleeHpHot = 0.45f;
constexpr float kFleeHits = 4.0f;  // leave a camp fight with this many camp hits of health left   // score bonus per ally already on a group
constexpr std::array<vec2, 8> kDirs{vec2{0, 1}, vec2{1, 1}, vec2{1, 0}, vec2{1, -1},
    vec2{0, -1}, vec2{-1, -1}, vec2{-1, 0}, vec2{-1, 1}};

int g_act[16]{}; int g_act_now = 0;
int g_w = 0, g_h = 0, g_bstride = 0, g_fcw = 0;

void init_dims() {
    const auto& l = cambc::detail::layout();
    g_w = l.width;
    g_h = l.height;
    g_bstride = static_cast<int>(l.chunk_stride);
    g_fcw = static_cast<int>(l.chunked_width);
}
bool inside(vec2 p) {
    return static_cast<unsigned>(p.x) < static_cast<unsigned>(g_w) && static_cast<unsigned>(p.y) < static_cast<unsigned>(g_h);
}
int cheb(vec2 a, vec2 b) {
    const int dx = a.x > b.x ? a.x - b.x : b.x - a.x, dy = a.y > b.y ? a.y - b.y : b.y - a.y;
    return dx > dy ? dx : dy;
}
// Engine buffers are 8x8 chunks in our own memory: read them in place.
bool raw_bit(const std::uint64_t* w, vec2 p) {
    return (w[(p.y >> 3) * g_bstride + (p.x >> 3)] >> (((p.y & 7) << 3) | (p.x & 7))) & 1;
}
void raw_set(std::uint64_t* w, vec2 p) {
    w[(p.y >> 3) * g_bstride + (p.x >> 3)] |= std::uint64_t{1} << (((p.y & 7) << 3) | (p.x & 7));
}
void raw_clear(std::uint64_t* w, vec2 p) {
    w[(p.y >> 3) * g_bstride + (p.x >> 3)] &= ~(std::uint64_t{1} << (((p.y & 7) << 3) | (p.x & 7)));
}
int raw_dist(const std::uint16_t* f, vec2 p) {
    return f[(((p.y >> 3) * g_fcw + (p.x >> 3)) << 6) | ((p.y & 7) << 3) | (p.x & 7)];
}
bool bit(const cambc::bitboard& b, vec2 p) { return inside(p) && raw_bit(b.ptr(), p); }

struct Spec { float hp, str; int range; };
Spec spec(int t) {
    switch (static_cast<cambc::camp_type>(t)) {
    case cambc::camp_type::wolf: return {1350, 20, 3};
    case cambc::camp_type::elk: return {2400, 60, 4};
    case cambc::camp_type::gem_cluster: return {120, 0, 0};
    case cambc::camp_type::gold_cluster: return {120, 0, 0};
    case cambc::camp_type::monolith: return {20000, 200, 5};
    case cambc::camp_type::golem: return {5000, 120, 4};
    }
    return {1000, 50, 3};
}

std::uint8_t mem_get(int i) {
    std::uint8_t v = 0;
    cambc::shared_memory.read(static_cast<std::size_t>(i), std::span{&v, 1});
    return v;
}
void mem_put(int i, std::uint8_t v) {
    if (mem_get(i) == v) return;
    cambc::shared_memory.write(static_cast<std::size_t>(i), std::span{&v, 1});
}

bool exclusive(const cambc::ability_info& info) {
    return std::visit([](const auto& x) { return x.exclusive_with_movement; }, info);
}
float spell_range(const cambc::ability_info& info) {
    if (auto* d = std::get_if<cambc::ability_damage>(&info)) return d->range;
    if (auto* c = std::get_if<cambc::ability_cc>(&info)) return c->range;
    if (auto* a = std::get_if<cambc::ability_aoe_damage>(&info)) return a->range;
    if (auto* h = std::get_if<cambc::ability_heal>(&info)) return h->range;
    if (auto* s = std::get_if<cambc::ability_dash>(&info)) return s->range;
    return 0;
}

// Shop plans: {item, count} stages, the last one repeats.
using IT = cambc::item_type;
struct Stage { IT item; int n; };
std::vector<Stage> plan_for(cambc::champion_type t) {
    switch (t) {
    case cambc::champion_type::ryth:
        return {{IT::jungle_shard_of_health, 5}, {IT::power_shard_of_health, 6}, {IT::monster_shard_of_strength, 5},
            {IT::power_shard_of_health, 1000}};
    case cambc::champion_type::marcella:
        return {{IT::jungle_shard_of_health, 5}, {IT::monster_shard_of_strength, 5}, {IT::power_shard_of_strength, 1000}};
    case cambc::champion_type::willow:
        return {{IT::jungle_shard_of_health, 5}, {IT::power_shard_of_strength, 6}, {IT::power_shard_of_health, 6},
            {IT::power_shard_of_strength, 1000}};
    default:
        return {{IT::monster_shard_of_strength, 5}, {IT::jungle_shard_of_health, 5}, {IT::power_shard_of_strength, 1000}};
    }
}

}  // namespace

float Group::str_left() const {
    float s = 0;
    for (size_t i = 0; i < hp.size(); ++i)
        if (hp[i] > 0) s += spec(kind[i]).str;
    return s;
}

void Bot::initialise() {
    init_ = true;
    init_dims();
    auto& me = cambc::self();
    const auto& order = pick_order();
    const auto name = me.champion_name();
    for (size_t i = 0; i < order.size(); ++i) {
        const auto d = order[i];
        if ((d == cambc::champion_draft::vendetta && name == cambc::champion_type::vendetta)
            || (d == cambc::champion_draft::ryth && name == cambc::champion_type::ryth)
            || (d == cambc::champion_draft::willow && name == cambc::champion_type::willow)
            || (d == cambc::champion_draft::vesper_vale && name == cambc::champion_type::vesper_vale)
            || (d == cambc::champion_draft::marcella && name == cambc::champion_type::marcella))
            slot_ = static_cast<int>(i);
    }
    const auto* dm = reinterpret_cast<const unsigned char*>(cambc::draft_memory.data());
    if (dm[0] == 'B' && dm[1] >= 1 && dm[1] <= 5) size_ = dm[1];

    for (const auto& grp : cambc::map::camp_groups()) {
        Group g;
        g.gems = static_cast<int>(grp.gem_value());
        g.gold = static_cast<int>(grp.gold_value());
        g.respawn = grp.max_respawn_timer();
        long sx = 0, sy = 0;
        for (const auto& c : grp.camps()) {
            const auto p = c.spawn_position();
            g.tiles.push_back(p);
            const int k = static_cast<int>(c.kind());
            g.kind.push_back(k);
            const auto s = spec(k);
            g.hp.push_back(s.hp);
            g.max_hp += s.hp;
            if (c.is_golem() || c.is_monolith()) g.big = true;
            if (c.is_monolith()) g.mono = true;
            sx += p.x;
            sy += p.y;
        }
        if (g.tiles.empty()) continue;
        const vec2 mean{static_cast<int>(sx / static_cast<long>(g.tiles.size())), static_cast<int>(sy / static_cast<long>(g.tiles.size()))};
        g.center = g.tiles[0];
        for (auto t : g.tiles)
            if (cheb(t, mean) < cheb(g.center, mean)) g.center = t;
        g_.push_back(std::move(g));
    }
    for (auto a : cambc::map::altars().positions()) altars_.push_back(a);
    static_block_ = cambc::map::walls() | cambc::map::holes() | cambc::map::altars() | cambc::map::spawn_region(team::enemy);
    spawn_ = cambc::map::spawn_region(team::ally);
    const auto sp = spawn_.positions();
    if (!sp.empty()) spawn_center_ = sp[sp.size() / 2];
    field_ = std::make_unique<cambc::distance_field>();
}

// Update group memory from what we see this turn.
void Bot::observe() {
    const auto* v = vis_.ptr();
    const auto camps_here = cambc::vision::camp_tiles();
    const auto* ct = camps_here.ptr();
    for (auto& g : g_) {
        if (g.state == Group::respawning && now_ >= g.ready) {
            g.state = Group::alive;
            for (size_t i = 0; i < g.hp.size(); ++i) g.hp[i] = spec(g.kind[i]).hp;
        }
        bool seen = false;
        for (size_t i = 0; i < g.tiles.size(); ++i) {
            const auto p = g.tiles[i];
            if (!raw_bit(v, p)) continue;
            seen = true;
            if (raw_bit(ct, p)) {
                try {
                    const auto c = cambc::vision::camp_at(p);
                    g.hp[i] = c.health();
                } catch (const std::exception&) {}
            } else if (g.state == Group::alive) {
                g.hp[i] = 0;
            }
        }
        if (!seen) {
            // A teammate's report on a boss group we cannot see.
            const int gi = static_cast<int>(&g - g_.data());
            if (g.big && g.state == Group::alive && mem_[5] == gi + 1) {
                const float shared = static_cast<float>(mem_[6] | (mem_[7] << 8)) * 2.0f;
                const float mine = g.hp_left();
                if (shared > 0 && shared < mine - 1) {
                    const float k = shared / mine;
                    for (auto& h : g.hp) h *= k;
                }
            }
            continue;
        }
        if (g.big && g.state == Group::alive) {
            const int gi = static_cast<int>(&g - g_.data());
            const int v = std::min(65535, static_cast<int>(g.hp_left() / 2.0f));
            mem_put(5, static_cast<std::uint8_t>(gi + 1));
            mem_put(6, static_cast<std::uint8_t>(v & 255));
            mem_put(7, static_cast<std::uint8_t>(v >> 8));
        }
        // Find the group handle lazily through a visible camp tile.
        try {
            const auto st = cambc::map::camp(g.tiles[0]).camp_group().state();
            if (std::holds_alternative<cambc::map::camp_group_alive>(st)) {
                if (g.state != Group::alive) {
                    g.state = Group::alive;
                    for (size_t i = 0; i < g.hp.size(); ++i)
                        if (!raw_bit(v, g.tiles[i])) g.hp[i] = spec(g.kind[i]).hp;
                }
            } else if (std::holds_alternative<cambc::map::camp_group_reward_available>(st)) {
                g.state = Group::reward;
                for (auto& h : g.hp) h = 0;
            } else if (auto* r = std::get_if<cambc::map::camp_group_respawning>(&st)) {
                g.state = Group::respawning;
                g.ready = now_ + r->timer;
                for (auto& h : g.hp) h = 0;
            }
        } catch (const std::exception&) {}
    }
}

void Bot::read_units() {
    foes_.clear();
    allies_.clear();
    for (auto p : cambc::vision::champion_tiles(team::enemy).positions()) {
        try {
            const auto c = cambc::vision::champion_at(p);
            Foe f;
            f.pos = p;
            f.hp = c.health();
            f.max_hp = c.max_health();
            f.str = c.strength();
            f.range = c.attack_range();
            f.ranged = f.range > 1;
            f.dmg = f.str * 1.15f + c.focus() * 0.3f;
            if (f.hp > 0) foes_.push_back(f);
        } catch (const std::exception&) {}
    }
    for (auto p : cambc::vision::champion_tiles(team::ally).positions()) {
        if (p == pos_) continue;
        try {
            const auto c = cambc::vision::champion_at(p);
            allies_.push_back({p, c.health() / std::max(1.0f, c.max_health())});
        } catch (const std::exception&) {}
    }
    for (auto& g : g_)
        for (const auto& f : foes_)
            if (cheb(f.pos, g.center) <= 6) g.enemy_seen = now_;
    if (!foes_.empty()) last_enemy_ = now_;
}

void Bot::shop() {
    auto& me = cambc::self();
    const auto plan = plan_for(me.champion_name());
    for (int guard = 0; guard < 8; ++guard) {
        int done = 0;
        const Stage* st = nullptr;
        for (const auto& s : plan) {
            if (build_ < done + s.n) { st = &s; break; }
            done += s.n;
        }
        if (!st) st = &plan.back();
        const int left_in_stage = done + st->n - build_;
        const unsigned gold = me.gold_count();
        const unsigned cost = st->item.cost();
        if (cost == 0) return;
        const unsigned n = std::min<unsigned>(gold / cost, static_cast<unsigned>(std::max(1, left_in_stage)));
        if (n == 0) return;
        if (!me.can_buy(st->item, n)) return;
        me.buy(st->item, n);
        build_ += static_cast<int>(n);
    }
}

// Walk one step (plus a dash) towards the nearest goal tile. False when no step was taken.
bool Bot::go(const cambc::bitboard& goals_in, bool allow_dash) {
    auto& me = cambc::self();
    if (move_failed_ || !me.can_move()) return false;
    if (bit(goals_in, pos_)) return false;
    // Keep out of the reach of awake camps we are not fighting, unless already inside one.
    const cambc::bitboard* walls = &block_;
    cambc::bitboard with_hot{cambc::uninitialized};
    if (hot_any_ && !bit(hot_, pos_)) {
        with_hot = block_;
        with_hot |= hot_;
        walls = &with_hot;
    }
    cambc::bitboard goals = goals_in;
    goals &= ~*walls;
    if (goals.is_empty()) return false;
    cambc::bitboard dest;
    dest.set(pos_, true);
    cambc::bitboard seen;
    field_->compute_to(*walls, goals, dest, seen, 400);
    const auto* f = static_cast<const cambc::distance_field&>(*field_).ptr();
    const auto* sb = seen.ptr();
    const auto* bb = walls->ptr();
    int here = raw_bit(sb, pos_) ? raw_dist(f, pos_) : kFar;
    if (here == kFar) return false;

    if (allow_dash && here > 3 && !cast_ && me.can_cast()) {
        for (const auto& ab : me.abilities()) {
            if (!ab.is_dash() || !ab.is_ready()) continue;
            try {
                vec2 best = pos_;
                int bd = here - 1;
                for (auto t : ab.valid_tiles().positions()) {
                    if (!inside(t) || raw_bit(bb, t) || !raw_bit(sb, t)) continue;
                    const int d = raw_dist(f, t);
                    if (d < bd) { bd = d; best = t; }
                }
                if (best != pos_ && ab.use(best)) {
                    cast_ = true;
                    pos_ = best;
                    here = bd;
                    if (here == 0) return true;
                }
            } catch (const std::exception&) {}
            break;
        }
    }

    vec2 best = pos_;
    int bd = here;
    for (auto d : kDirs) {
        const vec2 q = pos_ + d;
        if (!inside(q) || raw_bit(bb, q) || !raw_bit(sb, q)) continue;
        const int v = raw_dist(f, q);
        if (v < bd) { bd = v; best = q; }
    }
    if (best == pos_) return false;
    bool ok = false;
    try { ok = me.move_absolute(best); } catch (const std::exception&) { ok = false; }
    if (!ok) { move_failed_ = true; return false; }
    moved_ = true;
    pos_ = best;
    return true;
}

bool Bot::go_to(vec2 t, int r) {
    cambc::bitboard goals;
    for (int dy = -r; dy <= r; ++dy)
        for (int dx = -r; dx <= r; ++dx) {
            const vec2 q{t.x + dx, t.y + dy};
            if (inside(q)) raw_set(goals.ptr(), q);
        }
    return go(goals, true);
}

bool Bot::in_range_of_awake(vec2 p) const { return danger_at(p) > 0; }

// Strength of visible awake camps whose range covers p.
float Bot::danger_at(vec2 p) const {
    float s = 0;
    for (const auto& a : awake_)
        if (cheb(p, a.pos) <= a.range) s += a.str;
    return s;
}

// How deep inside awake camps' reach a tile is, weighted by their strength.
float Bot::camp_pressure(vec2 p) const {
    float s = 0;
    for (const auto& a : awake_) {
        const int d = cheb(p, a.pos);
        if (d <= a.range) s += static_cast<float>(a.range + 1 - d) * a.str;
    }
    return s;
}

// Pressure enemy champions put on a tile: how far inside their reach (one step and a margin) it is.
float Bot::foe_pressure(vec2 p) const {
    float s = 0;
    for (const auto& f : foes_) {
        const int d = cheb(p, f.pos);
        const int reach = f.range + 2;
        if (d <= reach) s += static_cast<float>(reach + 1 - d) * f.dmg;
    }
    return s;
}

int Bot::claims(int gi) const {
    int n = 0;
    for (int s = 0; s < 5; ++s)
        if (s != slot_ && mem_[s] == gi + 1) ++n;
    return n;
}
void Bot::claim(int gi) {
    mem_put(slot_, static_cast<std::uint8_t>(gi >= 0 && gi < 254 ? gi + 1 : 0));
}

// Free tiles within r of the group's living camps.
cambc::bitboard Bot::approach(int gi, int r) const {
    const auto& g = g_[static_cast<size_t>(gi)];
    cambc::bitboard goals;
    auto* w = goals.ptr();
    for (size_t i = 0; i < g.tiles.size(); ++i) {
        if (g.hp[i] <= 0) continue;
        const auto t = g.tiles[i];
        for (int dy = -r; dy <= r; ++dy)
            for (int dx = -r; dx <= r; ++dx) {
                const vec2 q{t.x + dx, t.y + dy};
                if (inside(q)) raw_set(w, q);
            }
    }
    return goals;
}

// Run home: out of camp range first, recall once nothing can interrupt it.
bool Bot::flee() {
    auto& me = cambc::self();
    if (bit(spawn_, pos_)) return false;
    int foe_d = kFar;
    for (const auto& f : foes_) foe_d = std::min(foe_d, cheb(f.pos, pos_));
    const bool hot = in_range_of_awake(pos_);
    if (!hot && foe_d > 6 && now_ > ambush_until_ && cheb(pos_, spawn_center_) > 7) {
        try {
            me.recall();
            return true;
        } catch (const std::exception&) {}
    }
    if (hot && foe_d > 6) {
        // Step to the neighbour with the least camp fire and enemy reach, preferring the way home.
        vec2 best = pos_;
        float bv = camp_pressure(pos_) * 10 + foe_pressure(pos_) + static_cast<float>(cheb(pos_, spawn_center_)) * 3;
        for (auto d : kDirs) {
            const vec2 q = pos_ + d;
            if (!inside(q) || bit(block_, q)) continue;
            const float v = camp_pressure(q) * 10 + foe_pressure(q) + static_cast<float>(cheb(q, spawn_center_)) * 3;
            if (v < bv) { bv = v; best = q; }
        }
        if (best != pos_ && me.can_move()) {
            bool ok = false;
            try { ok = me.move_absolute(best); } catch (const std::exception&) {}
            if (ok) { moved_ = true; pos_ = best; return true; }
            move_failed_ = true;
        }
    }
    go(spawn_, true);
    return true;
}

// Hit by someone we cannot see: put the ward into the nearest unseen bush to reveal the shooter.
void Bot::reveal(int min_n) {
    auto& me = cambc::self();
    try {
        if (me.ward_cooldown() > 0) return;
        if (min_n > 0)
            for (auto w : me.ward_positions())
                if (cheb(w, pos_) <= 5) return;
        const auto tiles = me.ward_placement_tiles();
        const auto& bushes = cambc::map::bushes();
        vec2 best{-1, -1};
        int bd = kFar;
        for (auto t : tiles.positions()) {
            if (!bit(bushes, t) || bit(vis_, t)) continue;
            // Prefer bush tiles with more unseen bush around them.
            int n = 0;
            for (auto d : kDirs) {
                const vec2 q = t + d;
                if (bit(bushes, q) && !bit(vis_, q)) ++n;
            }
            if (n < min_n) continue;
            const int v = cheb(t, pos_) * 4 - n;
            if (v < bd) { bd = v; best = t; }
        }
        if (best.x >= 0 && me.place_ward(best)) {
            vis_ = cambc::vision::visible_tiles();
            read_units();
        }
    } catch (const std::exception&) {}
}

// An ally fights nearby: join it while healthy.
bool Bot::answer_call() {
    if (kHelpRange <= 0) return false;  // off: bytes 5-7 now carry the boss report
    const int age = (now_ - mem_[7]) & 255;
    if (age > 2) return false;
    const vec2 at{mem_[5], mem_[6]};
    if (!inside(at)) return false;
    const int d = cheb(at, pos_);
    if (d > kHelpRange || d <= 2) return false;
    if (hp_ < 0.6f * max_hp_ || cambc::self().gem_count() >= 20) return false;
    g_act_now = 14;
    return go_to(at, 2);
}

bool Bot::attack_foe() {
    auto& me = cambc::self();
    if (!me.can_attack() || foes_.empty()) return false;
    const int r = me.attack_range();
    const Foe* best = nullptr;
    for (const auto& f : foes_) {
        if (cheb(f.pos, pos_) > r) continue;
        if (!best || f.hp < best->hp) best = &f;
    }
    if (!best) return false;
    try {
        const auto c = cambc::vision::champion_at(best->pos);
        me.attack(c);
        attacked_ = true;
        return true;
    } catch (const std::exception&) {}
    return false;
}

// Enemy champions about: fight when the trade favours us, otherwise leave.
bool Bot::fight() {
    if (foes_.empty()) return false;
    auto& me = cambc::self();
    float their_hp = 0, their_dps = 0;
    int near = 0, nearest = kFar;
    for (const auto& f : foes_) {
        const int d = cheb(f.pos, pos_);
        nearest = std::min(nearest, d);
        if (d > 7) continue;
        ++near;
        their_hp += f.hp;
        their_dps += f.dmg;
    }
    if (near == 0) return false;
    float our_hp = hp_, our_dps = me.strength() * 1.15f + me.focus() * 0.25f;
    for (const auto& a : allies_)
        if (cheb(a.first, pos_) <= 5) {
            our_hp += a.second * max_hp_;
            our_dps += camp_dps_;
        }
    const float gems = static_cast<float>(me.gem_count());
    const float t_kill = their_hp / std::max(1.0f, our_dps);
    const float t_die = our_hp / std::max(1.0f, their_dps);
    bool engage = t_kill < t_die * (gems > 20 ? 0.7f : kEngage);
    // Finish anyone low in reach.
    for (const auto& f : foes_)
        if (cheb(f.pos, pos_) <= me.attack_range() + 1 && f.hp <= our_dps * 1.2f && hp_ > 0.3f * max_hp_) engage = true;
    if (hp_ < 0.25f * max_hp_) engage = false;

    // Foes far from us that cannot reach: ignore.
    bool threatened = false;
    for (const auto& f : foes_)
        if (cheb(f.pos, pos_) <= f.range + kThreatPad) threatened = true;

    if (kDebug)
        std::cout << "F r" << now_ << " s" << slot_ << " hp" << static_cast<int>(hp_) << "/" << static_cast<int>(max_hp_)
                  << " near" << near << " d" << nearest << " ohp" << static_cast<int>(our_hp) << " odps" << static_cast<int>(our_dps)
                  << " thp" << static_cast<int>(their_hp) << " tdps" << static_cast<int>(their_dps) << " eng" << engage << "\n";
    if (engage) {
        if (nearest > 5 && !threatened) return false;
        spells(true, -1);
        if (attack_foe()) {
            use_reset(-1);
            return true;
        }
        // Close in on the weakest within 5.
        const Foe* tgt = nullptr;
        for (const auto& f : foes_)
            if (cheb(f.pos, pos_) <= 5 && (!tgt || f.hp < tgt->hp)) tgt = &f;
        if (!tgt) return false;
        if (go_to(tgt->pos, me.attack_range())) {
            if (me.attack_range() <= 1) attack_foe();
        }
        return true;
    }
    if (!threatened) return false;
    fleeing_until_ = now_ + 3;
    g_act_now = 13;
    spells(false, -1);
    flee();
    if (me.attack_range() <= 1 || !moved_) attack_foe();
    return true;
}

// Bank carried gems when the bag is big or an altar is close.
bool Bot::bank() {
    auto& me = cambc::self();
    const int gems = static_cast<int>(me.gem_count());
    if (gems <= 0 || altars_.empty()) return false;
    int d = kFar;
    vec2 near{};
    for (auto a : altars_) {
        const int dd = cheb(a, pos_);
        if (dd < d) { d = dd; near = a; }
    }
    const bool hot = now_ - last_enemy_ < 25;
    const float fd = static_cast<float>(d);
    bool want = gems >= std::max(static_cast<float>(kBankGems), fd * kBankPerTile)
        || (hot && gems >= std::max(static_cast<float>(kBankGemsHot), fd * kBankPerTile * 0.5f)) || gems >= 45 || d <= 4
        || now_ > 1800;
    if (target_ >= 0 && target_ < static_cast<int>(g_.size())) {
        const auto& g = g_[static_cast<size_t>(target_)];
        if (g.str_left() >= 100 && gems >= 8) want = true;  // a camp kill would destroy the bag
    }
    if (!want) return false;
    if (d <= cambc::constants::submit_gems_range) {
        try {
            me.submit_gems(near);
            return true;
        } catch (const std::exception&) {}
    }
    cambc::bitboard goals;
    for (auto a : altars_)
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                const vec2 q{a.x + dx, a.y + dy};
                if (inside(q)) raw_set(goals.ptr(), q);
            }
    if (!go(goals, true)) return false;
    return true;
}

// How many champions a group needs so that its camp fire does not outlast their health.
static float full_str(const Group& g) {
    float s = 0;
    for (int k : g.kind) s += spec(k).str;
    return s;
}
static int need_for(const Group& g, float dps, float hp, int size) {
    const bool fresh = g.state == Group::respawning;
    const float str = (fresh ? full_str(g) : g.str_left()) * 0.8f;
    if (str <= 0) return 1;
    const float left = fresh ? g.max_hp : g.hp_left();
    for (int n = 1; n <= size; ++n) {
        const float t = left / (dps * static_cast<float>(n));
        if (str * t < 0.55f * hp * static_cast<float>(n)) return n;
    }
    // Camps do not heal: a full team can wear a boss down over several trips.
    const float t = left / (dps * static_cast<float>(size));
    if (str * t < 1.6f * hp * static_cast<float>(size)) return size;
    return 99;
}

int Bot::choose() {
    auto& me = cambc::self();
    // Walking distances from here, refreshed every few turns (terrain only: camps do not move).
    if (!path_ || now_ - path_round_ >= 6 || cheb(path_from_, pos_) > 3) {
        bool ok = true;
        try { ok = cambc::fuel_remaining() > 55000; } catch (const std::exception&) {}
        if (ok || !path_) {
            if (!path_) path_ = std::make_unique<cambc::distance_field>();
            cambc::bitboard roots, seen;
            roots.set(pos_, true);
            path_->compute(static_block_, roots, seen, 250);
            path_round_ = now_;
            path_from_ = pos_;
        }
    }
    const auto* pf = static_cast<const cambc::distance_field&>(*path_).ptr();
    const float gem_w = now_ < 500 ? 3.0f : 5.0f;
    int best = -1;
    float bs = 0, cur = -1;
    for (size_t i = 0; i < g_.size(); ++i) {
        const auto& g = g_[i];
        int travel = raw_dist(pf, g.center);
        if (travel >= kFar) travel = cheb(pos_, g.center) * 3;
        travel = std::max(0, travel - cheb(path_from_, pos_));
        if (g.state == Group::respawning && g.ready > now_ + travel + 2) continue;
        if (g.enemy_seen > now_ - kAvoidRounds && static_cast<int>(i) != target_) continue;
        const int c = claims(static_cast<int>(i));
        float score;
        if (g.state == Group::reward) {
            score = (g.gems * gem_w + g.gold) / static_cast<float>(c + 1) / static_cast<float>(travel + 6);
        } else {
            const int need = need_for(g, camp_dps_, max_hp_, size_);
            if (need > size_) continue;
            const float left = g.state == Group::respawning ? g.max_hp : g.hp_left();
            float time;
            int n;
            if (g.big) {
                // Raids: the party gathers first.
                n = std::max(need, c + 1);
                const float clear = left / (camp_dps_ * static_cast<float>(n));
                const float wait = c + 1 >= need ? 0.0f : 12.0f * static_cast<float>(need - c - 1);
                time = static_cast<float>(travel) + clear + 6 + wait;
            } else {
                // Alone (or with whoever is there): extra trips home when the camps outlast our health.
                n = c + 1;
                const float clear = left / (camp_dps_ * static_cast<float>(n));
                const float taken = (g.state == Group::respawning ? full_str(g) : g.str_left()) * 0.8f * clear;
                const float budget = 0.6f * max_hp_ * static_cast<float>(n);
                const float trips = taken > budget ? (taken - budget) / budget : 0.0f;
                time = static_cast<float>(travel) + clear + 6 + trips * (static_cast<float>(cheb(g.center, spawn_center_)) * 2 + 12);
            }
            score = (g.gems * gem_w + g.gold) / static_cast<float>(n) / time;
            if (g.big) score *= 1.3f;
            score *= 1.0f + kPack * static_cast<float>(std::min(c, 3));
        }
        if (static_cast<int>(i) == target_) cur = score;
        if (score > bs) { bs = score; best = static_cast<int>(i); }
    }
    if (target_ >= 0 && cur > 0 && bs < cur * 1.35f) best = target_;
    if (best != target_) {
        target_ = best;
        target_since_ = now_;
        shrink_ = 0;
        no_hit_turns_ = 0;
        rally_since_ = -1;
    }
    claim(target_);
    (void)me;
    return target_;
}

bool Bot::attack_group(int gi, bool any_awake) {
    auto& me = cambc::self();
    if (!me.can_attack()) return false;
    const int r = me.attack_range();
    const float my = me.strength();
    std::optional<cambc::camp> best_c;
    bool have = false;
    float bv = -1e9f;
    auto consider = [&](vec2 t, bool in_group) {
        if (cheb(t, pos_) > r || !bit(vis_, t)) return;
        try {
            const auto c = cambc::vision::camp_at(t);
            const float h = c.health();
            if (h <= 0) return;
            const bool awake = c.is_awake();
            if (!in_group && !awake) return;
            float v = c.strength() / std::max(1.0f, h) * 1000.0f;
            if (h <= my) v += 500;
            if (c.strength() <= 0) v = -h / 1000.0f;
            if (!in_group) v -= 100;
            if (v > bv) { bv = v; best_c = c; have = true; }
        } catch (const std::exception&) {}
    };
    if (gi >= 0) {
        const auto& g = g_[static_cast<size_t>(gi)];
        for (size_t i = 0; i < g.tiles.size(); ++i)
            if (g.hp[i] > 0) consider(g.tiles[i], true);
    }
    if (any_awake || !have)
        for (const auto& a : awake_) consider(a.pos, false);
    if (!have) return false;
    try {
        me.attack(*best_c);
        attacked_ = true;
        return true;
    } catch (const std::exception&) {}
    return false;
}

bool Bot::use_reset(int gi) {
    auto& me = cambc::self();
    if (!attacked_ || cast_ || !me.can_cast() || me.can_attack()) return false;
    for (const auto& ab : me.abilities()) {
        if (!ab.is_attack_reset() || !ab.is_ready()) continue;
        try {
            if (!ab.use()) return false;
            cast_ = true;
        } catch (const std::exception&) { return false; }
        if (!me.can_attack()) return false;
        if (!foes_.empty() && attack_foe()) return true;
        return attack_group(gi, true);
    }
    return false;
}

// One cast per turn: heal, shield, then damage on enemies before camps.
void Bot::spells(bool staying, int gi) {
    auto& me = cambc::self();
    if (cast_ || !me.can_cast()) return;
    for (const auto& ab : me.abilities()) {
        if (cast_) return;
        if (!ab.is_ready()) continue;
        const auto& info = ab.info();
        if (!info) continue;
        const bool excl = exclusive(*info);
        if (excl && (moved_ || !staying)) continue;
        const float range = spell_range(*info);
        try {
            if (ab.is_heal()) {
                const float amount = ab.heal_amount();
                vec2 tgt{};
                float worst = 1.0f;
                for (const auto& a : allies_) {
                    if (cheb(a.first, pos_) > static_cast<int>(range)) continue;
                    if (a.second < worst) { worst = a.second; tgt = a.first; }
                }
                if (worst < 0.75f) {
                    const auto c = cambc::vision::champion_at(tgt);
                    if (c.max_health() - c.health() >= amount * 0.5f || worst < 0.4f)
                        if (ab.use(c)) cast_ = true;
                }
            } else if (ab.is_shield()) {
                bool threat = in_range_of_awake(pos_);
                for (const auto& f : foes_)
                    if (cheb(f.pos, pos_) <= f.range + 1) threat = true;
                if (threat && hp_ < 0.9f * max_hp_ && ab.use()) cast_ = true;
            } else if (ab.is_damage() || ab.is_cc() || ab.is_aoe_damage()) {
                const bool ground = ab.is_aoe_damage() && !ab.can_target_enemies() && !ab.can_target_camps();
                // Enemy champions first.
                const Foe* tf = nullptr;
                for (const auto& f : foes_)
                    if (cheb(f.pos, pos_) <= static_cast<int>(range) && bit(vis_, f.pos) && (!tf || f.hp < tf->hp)) tf = &f;
                if (tf) {
                    if (ground || ab.is_aoe_damage()) {
                        if (ab.use(tf->pos)) cast_ = true;
                    } else {
                        const auto c = cambc::vision::champion_at(tf->pos);
                        if (ab.use(c)) cast_ = true;
                    }
                    continue;
                }
                if (gi < 0 || ab.is_cc()) {
                    // Stuns on camps: only on an awake one that would hit someone.
                    if (!ab.is_cc() || gi < 0) continue;
                }
                const auto& g = g_[static_cast<size_t>(gi)];
                float dmg = 0;
                try { dmg = ab.damage_amount(); } catch (const std::exception&) {}
                vec2 bt{};
                float bv = 0;
                for (size_t i = 0; i < g.tiles.size(); ++i) {
                    const auto t = g.tiles[i];
                    if (g.hp[i] <= 0 || cheb(t, pos_) > static_cast<int>(range) || !bit(vis_, t)) continue;
                    if (!cambc::vision::has_camp(t)) continue;
                    float v = std::min(g.hp[i], std::max(dmg, 1.0f)) + spec(g.kind[i]).str * 2;
                    if (ab.is_cc() && spec(g.kind[i]).str <= 0) continue;
                    if (ab.is_aoe_damage())
                        for (size_t j = 0; j < g.tiles.size(); ++j)
                            if (j != i && g.hp[j] > 0 && cheb(g.tiles[j], t) <= 1) v += std::min(g.hp[j], dmg);
                    if (v > bv) { bv = v; bt = t; }
                }
                if (bv <= 0) continue;
                if (ab.is_cc()) {
                    const auto c = cambc::vision::camp_at(bt);
                    if (!c.is_awake()) continue;
                    if (ab.use(c)) cast_ = true;
                } else if (ab.is_aoe_damage()) {
                    if (ab.use(bt)) cast_ = true;
                } else {
                    const auto c = cambc::vision::camp_at(bt);
                    if (ab.use(c)) cast_ = true;
                }
            }
        } catch (const std::exception&) {}
    }
}

void Bot::farm(int gi) {
    auto& me = cambc::self();
    if (gi < 0) {
        // Nothing worth doing: wait at home.
        g_act_now = 7;
        if (!bit(spawn_, pos_)) go(spawn_, true);
        return;
    }
    auto& g = g_[static_cast<size_t>(gi)];
    if (g.state == Group::respawning) {
        // Wait beside it: standing on a spawn tile delays the respawn.
        g_act_now = 8;
        cambc::bitboard goals;
        auto* w = goals.ptr();
        for (auto t : g.tiles)
            for (auto d : kDirs) {
                const vec2 q = t + d;
                if (inside(q)) raw_set(w, q);
            }
        for (auto t : g.tiles) raw_clear(w, t);
        bool on_tile = false;
        for (auto t : g.tiles) on_tile |= t == pos_;
        if (on_tile || !bit(goals, pos_)) go(goals, true);
        return;
    }
    if (g.state == Group::reward) {
        // Capture: stand on a spawn tile of the group.
        g_act_now = 9;
        cambc::bitboard goals;
        for (auto t : g.tiles) raw_set(goals.ptr(), t);
        if (bit(goals, pos_)) {
            spells(true, gi);
            attack_group(gi, true);
            return;
        }
        if (!go(goals, true)) go_to(g.center, 1);
        return;
    }

    const int need = need_for(g, camp_dps_, max_hp_, size_);
    bool awake = false;
    for (const auto& a : awake_)
        for (auto gt : g.tiles)
            if (gt == a.pos) awake = true;
    int with_me = 1;
    for (const auto& a : allies_)
        if (cheb(a.first, g.center) <= 9 && a.second > 0.4f) ++with_me;
    int max_range = 0;
    for (size_t i = 0; i < g.tiles.size(); ++i)
        if (g.hp[i] > 0) max_range = std::max(max_range, spec(g.kind[i]).range);

    if (g.big && need > 1 && !awake && with_me < std::min(need, kRaidParty)) {
        if (rally_since_ < 0) rally_since_ = now_;
        if (now_ - rally_since_ < 40) {
            // Wait just outside the camps' reach.
            g_act_now = 10;
            const auto ring = approach(gi, max_range + 2);
            if (!bit(ring, pos_)) go(ring, true);
            spells(false, gi);
            return;
        }
    }
    if (with_me >= std::min(need, kRaidParty)) rally_since_ = -1;

    const int r = std::max(1, me.attack_range() - shrink_);
    bool can_hit = false;
    for (size_t i = 0; i < g.tiles.size(); ++i)
        if (g.hp[i] > 0 && cheb(g.tiles[i], pos_) <= me.attack_range() && bit(vis_, g.tiles[i])
            && cambc::vision::has_camp(g.tiles[i]))
            can_hit = true;
    const bool ranged = me.attack_range() > 1;
    if (can_hit) {
        g_act_now = 11;
        spells(true, gi);
        if (attack_group(gi, false)) {
            no_hit_turns_ = 0;
            use_reset(gi);
        }
        return;
    }
    g_act_now = 12;
    const auto goals = approach(gi, r);
    if (bit(goals, pos_)) {
        // In position but nothing to hit (walls or bushes in the way): come closer.
        if (++no_hit_turns_ >= 2 && shrink_ < 3) { shrink_++; no_hit_turns_ = 0; }
    }
    go(goals, true);
    if (!ranged || !moved_) {
        if (attack_group(gi, false)) use_reset(gi);
    }
    spells(false, gi);
}

void Bot::play_turn() {
    g_act_now = 0;
    const bool was_alive = alive_prev_;
    turn();
    if (kDebug) {
        auto& me = cambc::self();
        const bool alive = me.is_alive();
        if (was_alive && !alive) {
            std::cout << "DEATH r" << now_ << " s" << slot_ << ":";
            for (int i = 0; i < 6; ++i) {
                const auto& h = hist_[(hist_i_ + i) % 6];
                std::cout << " [r" << h[0] << " a" << h[1] << " hp" << h[2] << " f" << h[3] << " fd" << h[4] << " cd" << h[5] << " g" << h[6] << "]";
            }
            std::cout << '\n';
        }
        alive_prev_ = alive;
        if (alive) {
            int fd = 99;
            for (const auto& f : foes_) fd = std::min(fd, cheb(f.pos, pos_));
            hist_[hist_i_ % 6] = {now_, g_act_now, static_cast<int>(hp_), static_cast<int>(foes_.size()), fd,
                static_cast<int>(danger_at(pos_)), static_cast<int>(me.gem_count())};
            hist_i_++;
        }
        g_act[g_act_now]++;
        if (now_ == 1500) {
            std::cout << "ACT s" << slot_;
            for (int i = 0; i < 15; ++i) std::cout << ' ' << g_act[i];
            std::cout << '\n';
        }
    }
}

void Bot::turn() {
    auto& me = cambc::self();
    if (!init_) initialise();
    now_ = static_cast<int>(cambc::round_number());
    if (!me.is_alive()) {
        claim(-1);
        shop();
        return;
    }
    pos_ = me.position();
    if (!inside(pos_)) return;
    hp_ = me.health();
    max_hp_ = std::max(1.0f, me.max_health());
    moved_ = attacked_ = cast_ = move_failed_ = false;
    vis_ = cambc::vision::visible_tiles();
    for (int i = 0; i < 8; ++i) mem_[i] = mem_get(i);
    awake_.clear();
    for (auto t : cambc::vision::awake_camp_tiles().positions()) {
        try {
            const auto c = cambc::vision::camp_at(t);
            awake_.push_back({t, c.attack_range(), c.strength()});
        } catch (const std::exception&) {}
    }
    observe();
    read_units();
    // Damage nobody in view explains: an enemy hidden in a bush. Recalls would be broken by every hit.
    if (last_hp_ >= 0 && last_round_ == now_ - 1 && hp_ < last_hp_ - 5) {
        bool seen_cause = danger_at(pos_) > 0;
        for (const auto& f : foes_)
            if (cheb(f.pos, pos_) <= f.range + 1) seen_cause = true;
        if (!seen_cause) {
            ambush_until_ = now_ + 4;
            reveal(0);
        }
    }
    last_hp_ = hp_;
    last_round_ = now_;
    if (me.is_recalling() && now_ <= ambush_until_) {
        try { me.cancel_recall(); } catch (const std::exception&) {}
    }
    try {
        if (me.buffs().has_stun()) return;
    } catch (const std::exception&) {}
    if (kScout && now_ - last_enemy_ < 40 && !me.is_recalling() && !me.is_submitting_gems()) reveal(3);

    // Our damage per turn on camps.
    camp_dps_ = me.strength();
    for (const auto& ab : me.abilities()) {
        try {
            if (ab.is_attack_reset()) camp_dps_ += me.strength() / 8.0f;
            else if (ab.is_damage() || ab.is_aoe_damage()) camp_dps_ += ab.damage_amount() / 8.0f;
        } catch (const std::exception&) {}
    }
    camp_dps_ = std::max(20.0f, camp_dps_);

    g_act_now = 1;
    if (me.is_recalling() || me.is_submitting_gems() || me.is_teleporting()) return;
    g_act_now = 2;

    // Blocked tiles: terrain, units in view, and camps we believe alive out of view.
    block_ = static_block_ | cambc::vision::champion_tiles() | cambc::vision::camp_tiles() | cambc::vision::teleport_tiles();
    {
        auto* w = block_.ptr();
        const auto* v = vis_.ptr();
        for (const auto& g : g_) {
            if (g.state != Group::alive) continue;
            for (size_t i = 0; i < g.tiles.size(); ++i)
                if (g.hp[i] > 0 && !raw_bit(v, g.tiles[i])) raw_set(w, g.tiles[i]);
        }
        raw_clear(w, pos_);
    }
    hot_.clear();
    hot_any_ = false;
    {
        auto* w = hot_.ptr();
        for (const auto& a : awake_) {
            bool ours = false;
            if (target_ >= 0 && target_ < static_cast<int>(g_.size()))
                for (auto t : g_[static_cast<size_t>(target_)].tiles) ours |= t == a.pos;
            if (ours) continue;
            hot_any_ = true;
            for (int dy = -a.range; dy <= a.range; ++dy)
                for (int dx = -a.range; dx <= a.range; ++dx) {
                    const vec2 q{a.pos.x + dx, a.pos.y + dy};
                    if (inside(q)) raw_set(w, q);
                }
        }
    }

    const bool at_spawn = bit(spawn_, pos_);
    if (at_spawn) shop();

    if (fight()) return;

    g_act_now = 3;
    if (at_spawn && hp_ < 0.92f * max_hp_) {
        spells(false, -1);
        return;
    }

    const float hit_cut = danger_at(pos_) * 0.85f * kFleeHits;
    if ((hp_ < (now_ - last_enemy_ < 30 ? kFleeHpHot : kFleeHp) * max_hp_ || hp_ < hit_cut) && !at_spawn) {
        g_act_now = 4;
        flee();
        return;
    }

    if (answer_call()) return;

    g_act_now = 5;
    if (bank()) return;
    g_act_now = 6;

    // Spend a large purse: recall to shop when it is safe.
    if (!at_spawn && me.gold_count() >= 240 && me.gem_count() == 0 && !in_range_of_awake(pos_) && foes_.empty() && now_ > ambush_until_
        && cheb(pos_, spawn_center_) > 8) {
        try {
            me.recall();
            return;
        } catch (const std::exception&) {}
    }

    const int gi = choose();
    farm(gi);
    if (kDebug && now_ % 25 == 0) {
        std::cout << "r" << now_ << " s" << slot_ << " p" << pos_.x << "," << pos_.y << " t" << gi;
        if (gi >= 0) std::cout << " st" << g_[gi].state << " hp" << static_cast<int>(g_[gi].hp_left()) << " need" << need_for(g_[gi], camp_dps_, max_hp_, size_) << " big" << g_[gi].big << " c" << g_[gi].center.x << "," << g_[gi].center.y;
        std::cout << " mv" << moved_ << " at" << attacked_ << " fail" << move_failed_ << " gold" << me.gold_count() << " b" << build_ << " dps" << static_cast<int>(camp_dps_) << "\n";
    }
}
