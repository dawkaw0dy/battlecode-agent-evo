// Split-farm champion decisions. main.cpp dispatches here after the draft.
#include <cambc.h>
#include "bot.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <variant>
#include <iostream>

using cambc::item_type;

namespace {

bool fuel(std::uint64_t n) { return cambc::fuel_remaining() > n; }

// Tiles within Chebyshev `radius` of `c`, clipped to the map.
void box(cambc::bitboard& b, vec2 c, int radius) {
    if (c.x - radius >= 0 && c.y - radius >= 0 && c.x + radius < Raw::W && c.y + radius < Raw::H) {
        b.draw_chebyshev(c, static_cast<float>(radius));
        return;
    }
    for (int y = std::max(0, c.y - radius); y <= std::min(Raw::H - 1, c.y + radius); ++y)
        for (int x = std::max(0, c.x - radius); x <= std::min(Raw::W - 1, c.x + radius); ++x) b.set({x, y});
}

float ability_range(const cambc::ability& ab) {
    const auto& info = ab.info();
    if (!info) return 0;
    return std::visit([](const auto& v) -> float {
        if constexpr (requires { v.range; }) return v.range;
        else return 0.f;
    }, *info);
}

bool excludes_walking(const cambc::ability& ab) {
    const auto& info = ab.info();
    if (!info) return false;
    return std::visit([](const auto& v) -> bool { return v.exclusive_with_movement; }, *info);
}

}  // namespace

bool Bot::stunned() const {
    try {
        return cambc::self().buffs().has_stun();
    } catch (const cambc::error&) {
        return true;
    }
}

bool Bot::live() const { return static_cast<int>(cambc::round_number()) == round_; }
bool Bot::on_spawn() const { return Raw::bit(spawn_board_, pos_); }

// ------------------------------------------------------------------ initialisation

void Bot::initialize() {
    init_ = true;
    Raw::calibrate();
    const int W = Raw::W, H = Raw::H;
    auto& me = cambc::self();
    kind_ = static_cast<int>(me.champion_name());
    range_ = me.attack_range();
    ranged_ = range_ > 1;

    static_blocked_ = cambc::map::walls() | cambc::map::holes() | cambc::map::altars()
        | cambc::map::spawn_region(team::enemy);
    spawn_board_ = cambc::map::spawn_region(team::ally);
    spawn_ = spawn_board_.positions();
    long sx = 0, sy = 0;
    for (vec2 p : spawn_) sx += p.x, sy += p.y;
    if (!spawn_.empty()) spawn_center_ = {static_cast<int>(sx / (long)spawn_.size()), static_cast<int>(sy / (long)spawn_.size())};
    {
        const auto es = cambc::map::spawn_region(team::enemy).positions();
        long ex = 0, ey = 0;
        for (vec2 p : es) ex += p.x, ey += p.y;
        if (!es.empty()) espawn_center_ = {static_cast<int>(ex / (long)es.size()), static_cast<int>(ey / (long)es.size())};
    }
    altars_ = cambc::map::altars().positions();
    enemy_lock_seen_.assign(altars_.size(), -1000);

    handles_ = cambc::map::camp_groups();
    tile_group_.assign(static_cast<size_t>(W) * H, -1);
    tile_camp_.assign(static_cast<size_t>(W) * H, -1);
    groups_.resize(handles_.size());
    std::array<cambc::camp_stats, 8> stats{};
    std::array<bool, 8> stats_known{};
    for (size_t i = 0; i < handles_.size(); ++i) {
        auto& g = groups_[i];
        const auto& h = handles_[i];
        g.gems = static_cast<int>(h.gem_value());
        g.gold = static_cast<int>(h.gold_value());
        g.respawn = h.max_respawn_timer();
        g.camps.reserve(h.camps().size());
        g.tiles.reserve(h.camps().size());
        for (const auto& c : h.camps()) {
            Camp k{};
            k.pos = c.spawn_position();
            k.kind = static_cast<int>(c.kind());
            const int kk = std::clamp(k.kind, 0, 7);
            if (!stats_known[kk]) stats_known[kk] = true, stats[kk] = c.base_stats();
            const auto& s = stats[kk];
            k.maxhp = k.hp = s.base_health;
            k.str = s.base_strength;
            k.range = s.base_attack_range;
            g.camps.push_back(k);
        }
        long cx = 0, cy = 0;
        for (size_t k = 0; k < g.camps.size(); ++k) {
            const vec2 p = g.camps[k].pos;
            g.tiles.push_back(p);
            cx += p.x, cy += p.y;
            if (Raw::in(p)) {
                tile_group_[static_cast<size_t>(p.y) * W + p.x] = static_cast<std::int16_t>(i);
                tile_camp_[static_cast<size_t>(p.y) * W + p.x] = static_cast<std::int16_t>(k);
            }
        }
        if (!g.tiles.empty()) {
            const vec2 c{static_cast<int>(cx / (long)g.tiles.size()), static_cast<int>(cy / (long)g.tiles.size())};
            vec2 best = g.tiles[0];
            for (vec2 t : g.tiles)
                if (cheb(t, c) < cheb(best, c)) best = t;
            g.center = best;
        }
    }

    // Walking distances from both spawns.
    {
        cambc::distance_field fus, fthem;
        cambc::bitboard vus, vthem;
        const auto terrain = cambc::map::walls() | cambc::map::holes() | cambc::map::altars();
        fus.compute(terrain | cambc::map::spawn_region(team::enemy), spawn_board_, vus);
        fthem.compute(terrain | spawn_board_, cambc::map::spawn_region(team::enemy), vthem);
        for (auto& g : groups_)
            for (vec2 t : g.tiles) {
                if (Raw::bit(vus, t)) g.dspawn = std::min(g.dspawn, Raw::dist(fus, t));
                if (Raw::bit(vthem, t)) g.dthem = std::min(g.dthem, Raw::dist(fthem, t));
            }
    }

    // Region assignment from draft memory.
    const auto& mem = cambc::draft_memory;
    auto byte = [&](int i) -> int { return i < static_cast<int>(mem.size()) ? static_cast<int>(mem[i]) : 0; };
    const int ng = byte(dm::kGroupsLo) | (byte(dm::kGroupsHi) << 8);
    if (byte(dm::kMagic0) == 'S' && byte(dm::kMagic1) == 'F' && ng == static_cast<int>(groups_.size())) {
        n_slots_ = byte(dm::kSlots);
        const int mine = draft_of(me.champion_name());
        for (int s = 0; s < n_slots_ && s < 5; ++s)
            if (byte(dm::kSlotDraft + s) == mine) slot_ = s;
        if (slot_ >= 0) {
            home_altar_ = byte(dm::kSlotAltar + slot_);
            if (home_altar_ >= static_cast<int>(altars_.size())) home_altar_ = -1;
        }
        for (int i = 0; i < ng && i < dm::kMaxGroups; ++i) groups_[i].owner = byte(dm::kGroupOwner + i);
    } else {
        for (auto& g : groups_) g.owner = g.dspawn > g.dthem ? dm::kEnemySide : dm::kShared;
    }
}

// ------------------------------------------------------------------ observation

void Bot::update_group_from_vision(int gi) {
    auto& g = groups_[gi];
    int vis_tiles = 0, vis_alive = 0;
    for (auto& c : g.camps) {
        if (!Raw::in(c.pos) || !Raw::bit(vis_, c.pos)) continue;
        ++vis_tiles;
        if (Raw::bit(camps_now_, c.pos)) {
            ++vis_alive;
            float hp = c.hp;
            try {
                hp = cambc::vision::camp_at(c.pos).health();
            } catch (const cambc::error&) {
            }
            if (hp != c.hp) c.hp = hp, g.dirty = true;
        } else if (c.hp > 0) {
            c.hp = 0;
            g.dirty = true;
        }
    }
    if (vis_tiles == 0) return;
    g.seen = round_;
    if (vis_alive > 0) {
        if (g.status != 0) {
            // Respawned: camps we cannot see are back at full health too.
            for (auto& c : g.camps)
                if (!Raw::bit(vis_, c.pos)) c.hp = c.maxhp;
            g.status = 0;
            g.dirty = true;
        }
        return;
    }
    if (vis_tiles < static_cast<int>(g.camps.size())) return;
    // Every camp is dead: reward waiting or respawning. Query the engine only on transitions.
    if (g.status == 2 && round_ < g.respawn_at) return;
    if (!fuel(20000)) return;
    const auto st = handles_[gi].state();
    if (std::holds_alternative<cambc::map::camp_group_reward_available>(st)) {
        g.status = 1;
    } else if (const auto* r = std::get_if<cambc::map::camp_group_respawning>(&st)) {
        g.status = 2;
        g.respawn_at = round_ + std::max(1, r->timer);
    } else {
        g.status = 0;
    }
    if (g.status != 0)
        for (auto& c : g.camps) c.hp = 0;
    g.dirty = true;
}

void Bot::observe() {
    auto& me = cambc::self();
    cambc::vision::visible_tiles_into(vis_);
    cambc::vision::camp_tiles_into(camps_now_);
    cambc::vision::champion_tiles_into(units_);
    units_ |= camps_now_;
    units_ |= cambc::vision::teleport_tiles();

    if (prev_foes_round_ != round_) {
        prev_foes_.clear();
        if (foes_round_ == round_ - 1)
            for (const auto& f : foes_) prev_foes_.push_back(f.pos);
        prev_foes_round_ = round_;
    }
    foes_round_ = round_;
    foes_.clear();
    friends_.clear();
    for (const auto side : {team::enemy, team::ally}) {
        for (const vec2 p : cambc::vision::champion_tiles(side).positions()) {
            if (side == team::ally && p.x == pos_.x && p.y == pos_.y) continue;
            try {
                const auto c = cambc::vision::champion_at(p);
                Foe f{};
                f.pos = p;
                f.hp = c.health();
                f.maxhp = c.max_health();
                f.str = c.strength();
                f.focus = c.focus();
                f.range = c.attack_range();
                f.ranged = f.range > 1;
                f.channel = c.is_recalling() || c.is_submitting_gems() || c.is_teleporting();
                (side == team::enemy ? foes_ : friends_).push_back(f);
            } catch (const cambc::error&) {
            }
        }
    }

    // Camp knowledge near us.
    for (size_t gi = 0; gi < groups_.size(); ++gi) {
        const auto& g = groups_[gi];
        if (g.tiles.empty() || cheb(g.center, pos_) > 14) continue;
        update_group_from_vision(static_cast<int>(gi));
    }

    // Damage that awake camps can deal to us next round.
    incoming_ = incoming_other_ = 0;
    {
        std::vector<Awake> keep;
        for (const auto& a : awake_)
            if (round_ - a.seen <= 3 && !Raw::bit(vis_, a.pos)) keep.push_back(a);
        awake_.swap(keep);
    }
    float reduce = 1.0f;
    {
        const auto items = me.item_count();
        if (items[item_type::jungle_emblem_of_strength] + items[item_type::jungle_emblem_of_health]
                + items[item_type::jungle_emblem_of_focus] > 0)
            reduce *= 0.7f;
    }
    for (const vec2 p : camps_now_.positions()) {
        if (cheb(p, pos_) > 9) continue;
        try {
            const auto c = cambc::vision::camp_at(p);
            if (!c.is_awake()) continue;
            const int r = c.attack_range();
            const int gi = tile_group_[static_cast<size_t>(p.y) * Raw::W + p.x];
            awake_.push_back({p, r, gi, round_});
            if (cheb(p, pos_) <= r) {
                incoming_ += c.strength() * reduce;
                if (gi != target_) incoming_other_ += c.strength() * reduce;
            }
        } catch (const cambc::error&) {
        }
    }
}

// Tiles in range of awake camps that are not our target.
void Bot::add_danger(cambc::bitboard& walls) const {
    for (const auto& a : awake_) {
        if (a.group == target_ && target_ >= 0) continue;
        box(walls, a.pos, a.range);
    }
    if (avoid_foes_)
        for (const auto& f : foes_) box(walls, f.pos, f.ranged ? 2 : 1);
}

// ------------------------------------------------------------------ economy

std::optional<item_type> Bot::next_item() const {
    const int k = shards_;
    const bool vesper = kind_ == static_cast<int>(cambc::champion_type::vesper_vale);
    if (k < 5) return item_type::jungle_shard_of_strength;
    if (vesper && k < 10) return item_type::treasure_shard_of_strength;
    const int base = vesper ? 10 : 5;
    if (k < base + 5) return item_type::monster_shard_of_strength;
    if (k < base + 10) return item_type::power_shard_of_health;
    return (k % 2) ? item_type::power_shard_of_strength : item_type::power_shard_of_health;
}

void Bot::shop() {
    auto& me = cambc::self();
    for (int guard = 0; guard < 12; ++guard) {
        const auto it = next_item();
        if (!it || !live()) return;
        if (me.gold_count() < it->cost() || !me.can_buy(*it, 1)) return;
        g_where = "buy";
        me.buy(*it, 1);
        ++shards_;
        for (auto& g : groups_) g.dirty = true;
    }
}

float Bot::my_dps_camps() const {
    auto& me = cambc::self();
    float str = me.strength();
    float mult = me.base_stats().camp_damage_multiplier;
    if (mult <= 0) mult = 1;
    if (shards_ >= 10) mult *= 1.1f;  // monster emblem (vesper: approx)
    float dps = str * mult;
    for (const auto& ab : me.abilities()) {
        if (ab.is_attack_reset()) dps += str * mult / 9.0f;
        else if ((ab.is_damage() || ab.is_cc()) && ab.can_target_camps()) dps += ab.damage_amount() / 9.0f;
    }
    return std::max(dps, 10.0f);
}

void Bot::plan_group(Group& g) {
    g.dirty = false;
    if (!g.threats) {
        // Lazily: camps of the group that can hit someone attacking each camp.
        g.threats = true;
        for (size_t a = 0; a < g.camps.size(); ++a)
            for (size_t b = 0; b < g.camps.size(); ++b)
                if (cheb(g.camps[a].pos, g.camps[b].pos) <= g.camps[b].range + 1)
                    g.camps[a].threat.push_back(static_cast<int>(b));
    }
    const float dps = my_dps_camps();
    float reduce = shards_ >= 5 ? 0.7f : 1.0f;
    const size_t n = g.camps.size();
    std::vector<char> alive(n, 0);
    int left = 0;
    for (size_t i = 0; i < n; ++i)
        if (g.camps[i].hp > 0) alive[i] = 1, ++left;
    float t = 0, dmg = 0;
    while (left > 0) {
        // Smith's rule: highest damage removed per turn of work first.
        int best = -1;
        float bp = -1;
        for (size_t i = 0; i < n; ++i) {
            if (!alive[i]) continue;
            const float p = (g.camps[i].str + 1.0f) / std::max(g.camps[i].hp, 1.0f);
            if (p > bp) bp = p, best = static_cast<int>(i);
        }
        const auto& c = g.camps[best];
        float rate = 0;
        for (int b : c.threat)
            if (alive[b]) rate += g.camps[b].str;
        const float dt = std::ceil(c.hp / dps) + 0.5f;
        t += dt;
        dmg += rate * reduce * dt;
        alive[best] = 0;
        --left;
    }
    g.plan_time = t;
    g.plan_dmg = dmg;
}

// ------------------------------------------------------------------ movement

bool Bot::go(const cambc::bitboard& goals, bool allow_dash) {
    auto& me = cambc::self();
    if (moved_ || blocked_move_ || !me.can_move() || !live()) return false;
    cambc::bitboard walls = static_blocked_ | units_;
    walls.set(pos_, false);
    cambc::bitboard g = goals;
    g &= ~walls;
    if (g.is_empty() || Raw::bit(g, pos_)) return false;
    cambc::bitboard root, visited;
    root.clear();
    root.set(pos_);
    cambc::distance_field field;
    bool routed = false;
    if ((!awake_.empty() || (avoid_foes_ && !foes_.empty())) && fuel(40000)) {
        // Prefer routes outside the reach of awake camps we are not fighting.
        cambc::bitboard safe = walls;
        add_danger(safe);
        safe.set(pos_, false);
        cambc::bitboard sg = g;
        sg &= ~safe;
        if (!sg.is_empty()) {
            field.compute_to(safe, sg, root, visited);
            if (Raw::bit(visited, pos_) && field.next_move_from(pos_)) routed = true, walls = safe;
        }
    }
    if (!routed) {
        field.compute_to(walls, g, root, visited);
        if (!Raw::bit(visited, pos_)) return false;
    }
    auto d = field.next_move_from(pos_);
    if (!d) return false;

    // Dash two steps along the route when far from the goal.
    if (allow_dash && !cast_ && me.can_cast() && Raw::dist(field, pos_) >= 4 && fuel(15000)) {
        for (const auto& ab : me.abilities()) {
            if (!ab.is_dash() || !ab.is_ready()) continue;
            const vec2 p1{pos_.x + d->x, pos_.y + d->y};
            const auto d2 = field.next_move_from(p1);
            if (!d2) break;
            const vec2 p2{p1.x + d2->x, p1.y + d2->y};
            if (!Raw::in(p2) || Raw::bit(walls, p2) || Raw::bit(walls, p1)) break;
            try {
                g_where = "dash";
                if (ab.use(p2)) {
                    cast_ = true;
                    pos_ = p2;
                    d = field.next_move_from(pos_);
                    if (!d) return true;
                }
            } catch (const cambc::error&) {
            }
            break;
        }
    }
    if (!me.can_move() || !live()) return false;
    const vec2 dest{pos_.x + d->x, pos_.y + d->y};
    if (Raw::bit(walls, dest)) return false;
    // Vision may have changed this turn (a dash reveals new units).
    if (cambc::vision::has_champion(dest) || cambc::vision::has_camp(dest) || cambc::vision::has_teleport(dest))
        return false;
    g_where = "move";
    bool ok = false;
    try {
        ok = me.move(*d);
    } catch (const cambc::error&) {
        // A unit revealed to the team this round but outside our own vision.
        ok = false;
    }
    if (!ok) {
        blocked_move_ = true;
        return false;
    }
    moved_ = true;
    pos_ = dest;
    return true;
}

bool Bot::go_tile(vec2 t, int radius, bool allow_dash) {
    cambc::bitboard goals;
    box(goals, t, radius);
    return go(goals, allow_dash);
}

bool Bot::step_away_from_camps(bool others_only) {
    // Goal: nearby tiles outside the range of every visible living camp (or awake non-target camps).
    std::vector<std::pair<vec2, int>> threats;
    if (others_only) {
        for (const auto& a : awake_)
            if (a.group != target_) threats.push_back({a.pos, a.range});
    } else {
        for (const vec2 p : camps_now_.positions()) {
            if (cheb(p, pos_) > 8) continue;
            int r = 3;
            try {
                r = cambc::vision::camp_at(p).attack_range();
            } catch (const cambc::error&) {
            }
            threats.push_back({p, r});
        }
    }
    cambc::bitboard goals;
    int count = 0;
    for (int dy = -6; dy <= 6; ++dy)
        for (int dx = -6; dx <= 6; ++dx) {
            const vec2 q{pos_.x + dx, pos_.y + dy};
            if (!Raw::in(q) || Raw::bit(static_blocked_, q)) continue;
            bool safe = true;
            for (const auto& [p, r] : threats)
                if (cheb(p, q) <= r) { safe = false; break; }
            if (safe) goals.set(q), ++count;
        }
    if (count == 0) return go(spawn_board_, true);
    if (Raw::bit(goals, pos_)) return false;
    return go(goals, true);
}

// ------------------------------------------------------------------ actions

void Bot::after_attack() {
    auto& me = cambc::self();
    attacked_ = true;
    if (cast_ || !me.can_cast() || me.can_attack() || !live()) return;
    for (const auto& ab : me.abilities())
        if (ab.is_attack_reset() && ab.is_ready()) {
            try {
                if (ab.use()) cast_ = true;
            } catch (const cambc::error&) {
            }
            return;
        }
}

bool Bot::attack_unit(const cambc::unit& u) {
    auto& me = cambc::self();
    if (!me.can_attack() || !live()) return false;
    g_where = "attack_unit";
    me.attack(u);
    after_attack();
    return true;
}

bool Bot::cast_on_camp(const cambc::camp& c) {
    auto& me = cambc::self();
    if (cast_ || !me.can_cast() || !live()) return false;
    for (const auto& ab : me.abilities()) {
        if (!(ab.is_damage() || ab.is_cc()) || !ab.can_target_camps() || !ab.is_ready()) continue;
        if (excludes_walking(ab) && moved_) continue;
        if (cheb(pos_, c.position()) > static_cast<int>(ability_range(ab))) continue;
        g_where = "cast_camp";
        if (ab.use(c)) {
            cast_ = true;
            if (excludes_walking(ab)) moved_ = true;  // no walking after this cast
            return true;
        }
        return false;
    }
    return false;
}

void Bot::maybe_shield() {
    auto& me = cambc::self();
    if (cast_ || !me.can_cast() || !live()) return;
    const bool foes_near = std::any_of(foes_.begin(), foes_.end(), [&](const Foe& f) { return cheb(f.pos, pos_) <= 4; });
    if (incoming_ <= 0 && !foes_near) return;
    if (me.health() > me.max_health() * 0.85f) return;
    for (const auto& ab : me.abilities())
        if (ab.is_shield() && ab.is_ready() && ab.can_target_self()) {
            if (ab.use()) cast_ = true;
            return;
        }
}

void Bot::try_ward_bush() {
    auto& me = cambc::self();
    if (!live() || me.ward_cooldown() > 0) return;
    const auto& bushes = cambc::map::bushes();
    vec2 best{-1, -1};
    int bd = 99;
    for (int dy = -2; dy <= 2; ++dy)
        for (int dx = -2; dx <= 2; ++dx) {
            const vec2 q{pos_.x + dx, pos_.y + dy};
            if (!Raw::in(q) || !Raw::bit(bushes, q) || Raw::bit(vis_, q)) continue;
            if (Raw::bit(units_, q) || Raw::bit(static_blocked_, q)) continue;
            const int d = std::abs(dx) + std::abs(dy);
            if (d < bd) bd = d, best = q;
        }
    if (best.x < 0) return;
    try {
        g_where = "ward";
        me.place_ward(best);
    } catch (const cambc::error&) {
    }
}

bool Bot::try_recall(int why) {
    auto& me = cambc::self();
    if (on_spawn() || me.is_recalling() || me.is_submitting_gems() || me.is_teleporting() || !live()) return false;
    if (moved_ || attacked_ || cast_ || blocked_move_) return false;
    if (stunned()) return false;
    for (const auto& f : foes_)
        if (cheb(f.pos, pos_) <= 9 && !f.channel) return false;  // they would interrupt it
    g_where = "recall";
    me.recall();
    ++recalls_[why & 7];
    return true;
}

// Attack the best camp in range: camps of the target group, or awake camps hitting us.
bool Bot::attack_camp_in_range() {
    auto& me = cambc::self();
    if (!me.can_attack() || !live()) return false;
    const auto atk = me.attack_tiles();
    int best_g = -1, best_k = -1;
    float best = -1;
    for (const vec2 p : (camps_now_ & atk).positions()) {
        const int gi = tile_group_[static_cast<size_t>(p.y) * Raw::W + p.x];
        const int k = tile_camp_[static_cast<size_t>(p.y) * Raw::W + p.x];
        if (gi < 0 || k < 0) continue;
        const auto& c = groups_[gi].camps[k];
        float pri = (c.str + 1.0f) / std::max(c.hp, 1.0f);
        if (gi == target_) pri *= 4;
        else {
            bool awake = false;
            try {
                awake = cambc::vision::camp_at(p).is_awake();
            } catch (const cambc::error&) {
            }
            if (!awake) continue;  // never wake another group
        }
        if (pri > best) best = pri, best_g = gi, best_k = k;
    }
    if (best_g < 0) return false;
    auto& c = groups_[best_g].camps[best_k];
    if (!ranged_ || !moved_) cast_on_camp(cambc::vision::camp_at(c.pos));
    if (!me.can_attack() || !live()) return true;
    if (!cambc::vision::has_camp(c.pos)) return true;  // the spell finished it
    g_where = "attack_camp";
    me.attack(cambc::vision::camp_at(c.pos));
    groups_[best_g].dirty = true;
    groups_[best_g].my_hit = round_;
    after_attack();
    if (me.can_attack() && live()) {
        // Reset gave a second swing.
        try {
            if (cambc::vision::has_camp(c.pos)) me.attack(cambc::vision::camp_at(c.pos));
        } catch (const cambc::error&) {
        }
    }
    return true;
}

// ------------------------------------------------------------------ shared memory

// Bytes 0-2: latest enemy sighting (x, y, count<<5 | round&31). Bytes 3-7: target claim per slot (group%255+1).
void Bot::report(vec2 p, int count, bool fighting) {
    const int c = std::clamp(count, 1, 3) | (fighting ? 4 : 0);
    std::array<std::byte, 3> b{static_cast<std::byte>(p.x), static_cast<std::byte>(p.y),
        static_cast<std::byte>((c << 5) | (round_ & 31))};
    cambc::shared_memory.write(0, std::span<const std::byte>(b.data(), b.size()));
}

bool Bot::read_report(vec2& p, int& count, int& age, bool* fighting) const {
    std::array<std::byte, 3> b{};
    cambc::shared_memory.read(0, std::span<std::byte>(b.data(), b.size()));
    const int v = static_cast<int>(b[2]);
    count = (v >> 5) & 3;
    if (fighting) *fighting = (v >> 7) & 1;
    if (count == 0) return false;
    p = {static_cast<int>(b[0]), static_cast<int>(b[1])};
    age = (round_ - v) & 31;
    return age <= 6;
}

void Bot::claim(int group) {
    if (slot_ < 0 || slot_ > 4) return;
    const std::byte v = static_cast<std::byte>(group < 0 ? 0 : group % 255 + 1);
    cambc::shared_memory.write(3 + slot_, std::span<const std::byte>(&v, 1));
}

std::array<int, 5> Bot::claims() const {
    std::array<std::byte, 5> b{};
    cambc::shared_memory.read(3, std::span<std::byte>(b.data(), b.size()));
    std::array<int, 5> out{};
    for (int i = 0; i < 5; ++i) out[i] = (i == slot_ || i >= n_slots_) ? 0 : static_cast<int>(b[i]);
    return out;
}

// ------------------------------------------------------------------ fights

// Spells, then basic attacks, else a step towards the weakest reachable foe. True if we acted.
bool Bot::engage(const std::vector<const Foe*>& near) {
    auto& me = cambc::self();
    if (!cast_ && me.can_cast() && live()) {
        for (const auto& ab : me.abilities()) {
            if (!(ab.is_damage() || ab.is_cc()) || !ab.can_target_enemies() || !ab.is_ready()) continue;
            if (excludes_walking(ab) && moved_) continue;
            const int r = static_cast<int>(ability_range(ab));
            const Foe* t = nullptr;
            for (const Foe* f : near)
                if (cheb(f->pos, pos_) <= r && (!t || f->hp < t->hp)) t = f;
            if (!t) continue;
            // Melee: a walk-excluding cast must not cost a needed approach.
            if (excludes_walking(ab) && !ranged_ && cheb(t->pos, pos_) > 1) continue;
            if (!cambc::vision::has_champion(t->pos)) continue;
            try {
                g_where = "cast_foe";
                if (ab.use(cambc::vision::champion_at(t->pos))) {
                    cast_ = true;
                    if (excludes_walking(ab)) moved_ = true;
                }
            } catch (const cambc::error&) {
            }
            break;
        }
    }
    maybe_shield();
    auto hit_in_range = [&]() -> bool {
        const Foe* hit = nullptr;
        for (const Foe* f : near)
            if (cheb(f->pos, pos_) <= range_ && cambc::vision::has_champion(f->pos) && (!hit || f->hp < hit->hp)) hit = f;
        if (!hit || !me.can_attack() || !live()) return false;
        attack_unit(cambc::vision::champion_at(hit->pos));
        if (me.can_attack() && live() && cambc::vision::has_champion(hit->pos)) {
            g_where = "attack_foe2";
            me.attack(cambc::vision::champion_at(hit->pos));
        }
        return true;
    };
    if (hit_in_range()) return true;
    if (attacked_) return true;
    // Approach: nearest foe, weak ones first.
    const Foe* tgt = nullptr;
    float best = 1e9f;
    for (const Foe* f : near) {
        const float c = cheb(f->pos, pos_) + f->hp / 300.0f;
        if (c < best) best = c, tgt = f;
    }
    if (!tgt || cheb(tgt->pos, pos_) > 6) return false;
    if (hold_spawn_) {
        // Only shuffle within the spawn towards them.
        cambc::bitboard goals;
        box(goals, tgt->pos, ranged_ ? range_ : 1);
        goals &= spawn_board_;
        if (!goals.is_empty()) go(goals, false);
        if (!ranged_ || !moved_) hit_in_range();
        return true;
    }
    if (!go_tile(tgt->pos, ranged_ ? range_ : 1, true)) return false;
    if (!ranged_) hit_in_range();
    return true;
}

bool Bot::fight_or_flee() {
    auto& me = cambc::self();
    const auto& espawn = cambc::map::spawn_region(team::enemy);
    std::vector<const Foe*> near;
    bool threat = dmg_taken_ > incoming_ + 1;  // champion damage last round
    for (const auto& f : foes_)
        if (cheb(f.pos, pos_) <= 8 && !Raw::bit(espawn, f.pos)) {
            near.push_back(&f);
            if (cheb(f.pos, pos_) <= f.range + 2 || f.channel) threat = true;
            // Closing in on us since last round?
            const int d = cheb(f.pos, pos_);
            if (d <= 5)
                for (const vec2 q : prev_foes_)
                    if (cheb(q, f.pos) <= 1 && d < cheb(q, last_pos_)) { threat = true; break; }
        }
    if (near.empty()) {
        // Hit by someone we cannot see (a bush): reveal it with the ward and back off.
        if (dmg_taken_ > incoming_ + 15 && !on_spawn()) {
            try_ward_bush();
            flee_until_ = round_ + 8;
            flee_from_ = pos_;
            mode_ = 4;
            if (me.health() < me.max_health() * 0.6f || me.gem_count() >= 15) {
                avoid_foes_ = true;
                go(spawn_board_, true);
                avoid_foes_ = false;
                return true;
            }
        }
        return false;
    }

    // On our spawn we heal fully every turn and they cannot step on it: hold and shoot from here.
    if (on_spawn()) {
        int closest = 99;
        for (const Foe* f : near) closest = std::min(closest, cheb(f->pos, pos_));
        if (closest <= 7) {
            mode_ = 3;
            hold_spawn_ = true;
            engage(near);
            hold_spawn_ = false;
            return true;
        }
    }

    auto dps_of = [](float str, float focus) { return str * 1.1f + focus * 0.25f; };
    const float my_hp = me.health();
    double hp_us = my_hp, dps_us = dps_of(me.strength(), me.focus());
    int n_us = 1;
    for (const auto& a : friends_)
        if (cheb(a.pos, pos_) <= 4 && !a.channel && a.hp > a.maxhp * 0.4f)
            hp_us += a.hp, dps_us += dps_of(a.str, a.focus), ++n_us;
    double hp_them = 0, dps_them = 0;
    int n_them = 0, closest = 99;
    for (const Foe* f : near) {
        hp_them += f->hp;
        if (!f->channel) dps_them += dps_of(f->str, f->focus), ++n_them;
        closest = std::min(closest, cheb(f->pos, pos_));
    }
    // Camps hitting us fight for nobody.
    hp_us -= incoming_ * 4;
    const double ratio = (std::max(0.0, hp_us) * dps_us) / std::max(1.0, hp_them * dps_them);
    double need = 1.25 + me.gem_count() / 150.0;
    if (my_hp < me.max_health() * 0.5f) need += 0.5;
    if (n_us == 1 && n_them >= 2) need += 0.4;
    if (on_spawn()) need *= 0.5;  // we heal here every turn
    report(near[0]->pos, static_cast<int>(near.size()), threat && ratio >= need);

    if (!threat && ratio < need * 1.3) {
        // Busy foes at a distance: keep farming, but stay away from them.
        avoid_until_ = round_ + 6;
        avoid_pos_ = near[0]->pos;
        return false;
    }
    if (ratio >= need) {
        if (engage(near)) {
            mode_ = 3;
            fight_round_ = round_;
            return true;
        }
        if (closest > 4 && dmg_taken_ <= 0) return false;  // not close: keep farming
    }

    // Flee: run home, recall once clear.
    mode_ = 4;
    flee_until_ = round_ + 15;
    {
        const Foe* c = near[0];
        for (const Foe* f : near)
            if (cheb(f->pos, pos_) < cheb(c->pos, pos_)) c = f;
        flee_from_ = c->pos;
    }
    if (closest >= 6 && incoming_ <= 0 && dmg_taken_ <= 0 && !on_spawn()) {
        if (try_recall(1)) return true;
    }
    maybe_shield();
    avoid_foes_ = true;
    if (!on_spawn()) go(spawn_board_, true);
    avoid_foes_ = false;
    // Melee can still swing at an adjacent chaser while running.
    if (me.can_attack() && live() && (!ranged_ || !moved_)) {
        for (const Foe* f : near)
            if (cheb(f->pos, pos_) <= range_ && cambc::vision::has_champion(f->pos)) {
                attack_unit(cambc::vision::champion_at(f->pos));
                break;
            }
    }
    return true;
}

// ------------------------------------------------------------------ gems

void Bot::ensure_my_field() {
    if (my_field_round_ >= round_ - 3 && cheb(my_field_pos_, pos_) <= 2) return;
    cambc::bitboard root;
    root.clear();
    root.set(pos_);
    cambc::bitboard walls = static_blocked_;
    walls.set(pos_, false);
    my_field_.compute(walls, root, my_visited_);
    my_field_round_ = round_;
    my_field_pos_ = pos_;
}

bool Bot::tribute() {
    auto& me = cambc::self();
    const int gems = static_cast<int>(me.gem_count());
    if (gems <= 0) return false;
    const auto locks = cambc::vision::altar_lock_tiles(team::enemy);
    for (size_t i = 0; i < altars_.size(); ++i)
        if (Raw::bit(vis_, altars_[i])) enemy_lock_seen_[i] = Raw::bit(locks, altars_[i]) ? round_ : -1000;

    // Already beside an altar: bank (or break the enemy lock on it).
    const int range = cambc::constants::submit_gems_range;
    for (size_t i = 0; i < altars_.size(); ++i) {
        const vec2 a = altars_[i];
        if (cheb(a, pos_) > range) continue;
        if (Raw::bit(locks, a)) {
            if (!me.can_attack() || !live()) return true;
            if (stunned()) return true;
            g_where = "lock_break";
            me.submit_gems(a);  // converted into a hit on the lock
            after_attack();
            if (me.can_attack() && live() && cambc::vision::has_altar_lock(a)) me.submit_gems(a);
            return true;
        }
        if (me.health() > incoming_ * (std::ceil(gems / 10.0f) + 1) + 30 && live() && !stunned()) {
            g_where = "submit";
            me.submit_gems(a);
            return true;
        }
    }

    ensure_my_field();
    int best = -1;
    float bestc = 1e9f;
    int best_eta = kFar;
    for (size_t i = 0; i < altars_.size(); ++i) {
        const vec2 a = altars_[i];
        int eta = kFar;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                const vec2 q{a.x + dx, a.y + dy};
                if (Raw::in(q) && Raw::bit(my_visited_, q)) eta = std::min<int>(eta, Raw::dist(my_field_, q));
            }
        if (eta >= kFar) continue;
        float c = static_cast<float>(eta);
        if (static_cast<int>(i) == home_altar_) c *= 0.75f;
        if (round_ - enemy_lock_seen_[i] < 150) c += 25;
        if (c < bestc) bestc = c, best = static_cast<int>(i), best_eta = eta;
    }
    if (best < 0) return false;
    vec2 rp;
    int rc = 0, rage = 99;
    const bool danger = read_report(rp, rc, rage) && cheb(rp, pos_) <= 14;
    const bool go_now = gems >= 45 || (gems >= 15 && best_eta <= 10) || (gems >= 5 && best_eta <= 3)
        || (danger && gems >= 8) || round_ >= 1880 || (gems >= 25 && best_eta <= 18);
    if (!go_now) return false;
    return go_tile(altars_[best], range, true) || moved_;
}

// ------------------------------------------------------------------ farming

int Bot::choose_target() {
    auto& me = cambc::self();
    ensure_my_field();
    const float maxhp = me.max_health();
    const float hp = me.health();
    vec2 rp;
    int rc = 0, rage = 99;
    const bool rep = read_report(rp, rc, rage);
    const auto taken = claims();
    const float wgold = round_ < 400 ? 0.4f : round_ < 1000 ? 0.25f : round_ < 1600 ? 0.1f : 0.02f;
    struct Cand { int gi, eta, wait; float value, factor, bound; bool shared; };
    std::vector<Cand> cands;
    cands.reserve(groups_.size());
    for (size_t gi = 0; gi < groups_.size(); ++gi) {
        auto& g = groups_[gi];
        if (g.tiles.empty()) continue;
        if (static_cast<int>(gi) == banned_ && round_ < banned_until_) continue;
        int eta = kFar;
        for (vec2 t : g.tiles)
            if (Raw::bit(my_visited_, t)) eta = std::min<int>(eta, Raw::dist(my_field_, t));
        if (eta >= kFar) continue;
        if (g.status == 2 && round_ + eta >= g.respawn_at + 3 && g.seen < g.respawn_at) {
            // It should be back by the time we arrive.
            g.status = 0;
            for (auto& c : g.camps) c.hp = c.maxhp;
            g.dirty = true;
        }
        int wait = 0;
        if (g.status == 2) {
            wait = std::max(0, g.respawn_at - round_ - eta);
            if (wait > 12) continue;
        }
        const float value = g.gems + wgold * g.gold;
        if (value <= 0) continue;
        float factor = 1.0f;
        if (g.owner == slot_) factor = 1.0f;
        else if (g.owner == dm::kShared) factor = 0.75f;
        else if (g.owner == dm::kEnemySide) factor = 0.45f;
        else factor = 0.55f;
        if (static_cast<int>(gi) == target_) factor *= 1.25f;
        bool shared = false;
        for (int c : taken)
            if (c == static_cast<int>(gi % 255) + 1) shared = true;
        // Big groups are worth sharing: camps hit only the nearest champion.
        const bool big = !g.dirty && g.status == 0 && g.plan_dmg > maxhp * 0.6f;
        // Contested groups (middle / enemy side): go in company, or not at all.
        const bool contested = g.owner == dm::kShared || g.owner == dm::kEnemySide;
        if (contested) factor *= shared ? 1.6f : 0.6f;
        else if (shared && !big) factor *= 0.3f;
        if (g.status == 1) factor *= (round_ - g.my_hit < 40) ? 1.3f : 0.4f;
        if (round_ <= flee_until_ + 20 && cheb(flee_from_, g.center) <= 10) factor *= 0.2f;
        if (round_ <= avoid_until_ && cheb(avoid_pos_, g.center) <= 5) factor *= 0.5f;
        if (rep && rc >= 2 && cheb(rp, g.center) <= 9) factor *= 0.3f;
        cands.push_back({static_cast<int>(gi), eta, wait, value, factor, value * factor / (eta + wait + 13.0f), shared && (big || contested)});
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.bound > b.bound; });
    int best = -1;
    float bests = 0;
    int planned = 0;
    for (const auto& c : cands) {
        if (c.bound <= bests) break;  // nothing below can beat the best
        auto& g = groups_[c.gi];
        if (g.dirty) {
            if (planned >= 12 || !fuel(20000)) continue;
            plan_group(g);
            ++planned;
        }
        float clear = 0, trip = 0;
        if (g.status != 1) {
            clear = g.plan_time;
            if (g.plan_time <= 0 && g.status == 0) continue;
            const float budget = maxhp * 0.8f;
            float dmg = g.plan_dmg;
            if (c.shared) clear *= 0.5f, dmg *= 0.5f;  // reward split, but half the work
            const int trips = static_cast<int>(std::ceil((dmg + 1) / budget));
            if (trips > 3) continue;
            trip = (trips - 1) * (12.0f + g.dspawn);
            if (g.plan_dmg > hp - maxhp * 0.15f && hp < maxhp * 0.9f)
                trip += 10.0f + std::min<int>(g.dspawn, 60);  // heal first
        }
        const float time = c.eta + c.wait + clear + 5 + trip + 8;
        const float sc = c.value * c.factor * (c.shared ? 0.5f : 1.0f) / time;
        if (sc > bests) bests = sc, best = c.gi;
    }
    return best;
}

void Bot::farm() {
    auto& me = cambc::self();
    const float hp = me.health(), maxhp = me.max_health();

    // Retreat out of camp range, then recall.
    if (round_ <= retreat_until_) {
        mode_ = 8;
        if (incoming_ <= 0 && !on_spawn()) {
            bool camp_close = false;
            for (const vec2 p : camps_now_.positions())
                if (cheb(p, pos_) <= 4) { camp_close = true; break; }
            if (!camp_close || hp < maxhp * 0.5f) {
                if (try_recall(2)) { retreat_until_ = -1; return; }
            }
        }
        if (!step_away_from_camps() && incoming_ <= 0 && try_recall(2)) retreat_until_ = -1;
        return;
    }

    // Awake camps of another group hitting us keep hitting while we stay: take them on if we can.
    if (incoming_other_ > 0) {
        int gi = -1;
        for (const auto& a : awake_)
            if (a.group >= 0 && a.group != target_ && cheb(a.pos, pos_) <= a.range) { gi = a.group; break; }
        bool capturing = false;
        if (target_ >= 0 && groups_[target_].status == 1)
            for (vec2 t : groups_[target_].tiles)
                if (t.x == pos_.x && t.y == pos_.y) capturing = true;
        if (capturing) {
            if (hp < incoming_ * 3 + maxhp * 0.08f) {
                retreat_until_ = round_ + 12;
                step_away_from_camps();
                return;
            }
            // otherwise hold the capture
        } else if (gi >= 0 && target_ >= 0 && groups_[target_].status == 0 && incoming_ - incoming_other_ > 0) {
            // our own target hits us too: keep fighting it
        } else if (gi >= 0) {
            auto& og = groups_[gi];
            if (og.dirty) plan_group(og);
            if (og.status == 0 && og.plan_dmg < hp - maxhp * 0.15f) {
                target_ = gi;
                target_round_ = round_;
                claim(gi);
            } else {
                mode_ = 7;
                if (step_away_from_camps(true)) return;
            }
        }
    }

    if (osc_ >= 4 && target_ >= 0) {
        banned_ = target_;
        banned_until_ = round_ + 25;
        osc_ = 0;
        target_ = -1;
    }
    const bool rechoose = target_ < 0 || round_ - target_round_ >= 4 || (target_ >= 0 && groups_[target_].status == 2
            && groups_[target_].respawn_at - round_ > 12);
    if (rechoose && !fuel(60000)) return;  // next turn (initialisation or a busy turn)
    if (rechoose) {
        const int t = choose_target();
        if (t != target_) focus_camp_ = -1, approach_shrink_ = 0;
        target_ = t;
        target_round_ = round_;
        claim(t);
    }
    if (target_ < 0) {
        mode_ = 14;
        if (!on_spawn() && hp < maxhp * 0.7f) try_recall(3);
        return;
    }
    auto& g = groups_[target_];

    if (g.status == 1) {
        mode_ = 9;
        // Capture: stand on any spawn tile of the group.
        for (vec2 t : g.tiles)
            if (t.x == pos_.x && t.y == pos_.y) {
                attack_camp_in_range();
                return;
            }
        cambc::bitboard goals;
        for (vec2 t : g.tiles) goals.set(t);
        if (!go(goals, true)) {
            attack_camp_in_range();
        }
        return;
    }
    if (g.status == 2) {
        mode_ = 10;
        if (hp < maxhp * 0.6f && g.respawn_at - round_ > 6 && try_recall(4)) return;
        // Wait nearby, never on a spawn tile (an occupied tile blocks the respawn).
        bool on_tile = false;
        for (vec2 t : g.tiles)
            if (t.x == pos_.x && t.y == pos_.y) on_tile = true;
        if (on_tile || cheb(pos_, g.center) > 3) {
            cambc::bitboard goals;
            box(goals, g.center, 3);
            for (vec2 t : g.tiles) goals.set(t, false);
            go(goals, true);
        }
        return;
    }

    // Alive. Heal before a fight we cannot survive, unless already engaged.
    if (g.dirty) plan_group(g);
    const bool engaged = incoming_ > 0;
    if (!engaged && !on_spawn() && hp < maxhp * 0.45f && g.plan_dmg > hp * 0.8f) {
        if (try_recall(5)) return;
    }
    if (on_spawn() && hp < maxhp * 0.97f && heal_wait_ < 2) {
        mode_ = 13;
        ++heal_wait_;
        return;
    }
    if (!on_spawn()) heal_wait_ = 0;

    // In a fight: leave before the camps can kill us.
    if (engaged) {
        const float margin = maxhp * 0.08f + 20;
        float remaining = 0;
        for (const auto& c : g.camps) remaining += c.hp;
        const float finish = remaining / my_dps_camps();
        const bool will_finish = hp > incoming_ * (finish + 1) + margin;
        if (!will_finish && hp < incoming_ * 3.5f + margin) {
            retreat_until_ = round_ + 12;
            if (!step_away_from_camps() && incoming_ <= 0) try_recall(2);
            return;
        }
    }

    maybe_shield();
    mode_ = 11;
    if (attack_camp_in_range()) {
        approach_shrink_ = 0;
        stuck_ = 0;
        return;
    }

    // Approach the highest-priority living camp.
    mode_ = 12;
    int best = -1;
    float bp = -1;
    for (size_t k = 0; k < g.camps.size(); ++k) {
        const auto& c = g.camps[k];
        if (c.hp <= 0) continue;
        float p = (c.str + 1.0f) / std::max(c.hp, 1.0f) / (1.0f + cheb(c.pos, pos_) * 0.15f);
        if (p > bp) bp = p, best = static_cast<int>(k);
    }
    if (best < 0) {
        // Believed dead but not confirmed: walk into view.
        go_tile(g.center, 1, true);
        return;
    }
    const auto& c = g.camps[best];
    int radius = ranged_ ? std::max(1, range_ - approach_shrink_) : 1;
    const bool in_ring = cheb(c.pos, pos_) <= radius;
    if (in_ring && Raw::bit(vis_, c.pos)) {
        // In range but nothing hit (no line of sight): come closer.
        if (approach_shrink_ < range_ - 1) ++approach_shrink_;
        radius = std::max(1, range_ - approach_shrink_);
    }
    if (!go_tile(c.pos, radius, cheb(c.pos, pos_) > 5)) {
        ++stuck_;
        if (stuck_ > 6 && approach_shrink_ < range_ - 1) ++approach_shrink_;
    }
    // Melee: hit after the step.
    if (!ranged_) attack_camp_in_range();
}

// ------------------------------------------------------------------ turn

void Bot::play_turn() {
    round_ = static_cast<int>(cambc::round_number());
    struct Trace {
        Bot* b;
        std::uint64_t f0;
        ~Trace() {
            ++b->act_[b->mode_ & 15];
            if (b->mode_ == 12 && !b->moved_) ++b->act_[14];
            b->lap(4);
            debug_block({
                if (b->round_ == 1 && b->slot_ == 0) {
                    std::cout << "MAP altars";
                    for (auto a : b->altars_) std::cout << " " << a.x << "," << a.y;
                    std::cout << " home" << b->home_altar_ << "\n";
                    for (size_t i = 0; i < b->groups_.size(); ++i) {
                        const auto& g = b->groups_[i];
                        std::cout << "G" << i << " c" << g.center.x << "," << g.center.y << " n" << g.camps.size() << " k";
                        for (auto& c : g.camps) std::cout << c.kind;
                        std::cout << " gem" << g.gems << " gold" << g.gold << " own" << g.owner << " ds" << g.dspawn << " dt" << g.dthem << "\n";
                    }
                }
                std::cout.flush();
                if (b->round_ == 400) {
                    std::cout << "ACT k" << b->kind_;
                    for (int v : b->act_) std::cout << ' ' << v;
                    std::cout << " REC";
                    for (int v : b->recalls_) std::cout << ' ' << v;
                    std::cout << " PROF";
                    for (double v : b->prof_) std::cout << ' ' << (long)(v / 1000);
                    std::cout << "\n";
                }
                static int last_mode = -1, last_tg = -2;
                const bool change = b->round_ < 80 && b->round_ % 5 == 0;
                if (b->kind_ == 7) last_mode = b->mode_, last_tg = b->target_;
                if (b->kind_ == 2) last_mode = b->mode_, last_tg = b->target_;
                if (change || f0 - cambc::fuel_remaining() > 90000) {
                    auto& me = cambc::self();
                    std::cout << "T r" << b->round_ << " k" << b->kind_ << " s" << b->slot_ << " p" << b->pos_.x << ","
                              << b->pos_.y << " hp" << (int)me.health() << " g" << me.gem_count() << " $" << me.gold_count()
                              << " tg" << b->target_ << " st" << (b->target_ >= 0 ? b->groups_[b->target_].status : -1)
                              << " inc" << (int)b->incoming_ << " ret" << b->retreat_until_ << " foes" << b->foes_.size() << " fr" << b->friends_.size() << " dmg" << (int)b->dmg_taken_ << " m" << b->mode_ << " fuel"
                              << (f0 - cambc::fuel_remaining()) << "\n";
                }
            });
        }
    } trace{this, cambc::fuel_remaining()};
    auto& me = cambc::self();
    moved_ = attacked_ = cast_ = blocked_move_ = false;
    g_where = "init";
    pf_ = cambc::fuel_remaining();
    if (!init_) initialize();
    g_where = "turn";

    mode_ = 15;
    if (!me.is_alive()) {
        mode_ = 0;
        shop();
        if (target_ >= 0) claim(-1);
        target_ = -1;
        retreat_until_ = -1;
        return;
    }
    pos_ = me.position();
    if (!Raw::in(pos_)) return;
    // Two-tile oscillation: back where we were two turns ago after moving away.
    if (pos_.x == last_pos_.x && pos_.y == last_pos_.y && (cur_pos_.x != pos_.x || cur_pos_.y != pos_.y)) ++osc_;
    else if (round_ % 8 == 0) osc_ = std::max(0, osc_ - 1);
    last_pos_ = cur_pos_;
    cur_pos_ = pos_;
    {
        const float hp = me.health();
        dmg_taken_ = (last_hp_round_ == round_ - 1) ? std::max(0.0f, last_hp_ - hp) : 0.0f;
        last_hp_ = hp;
        last_hp_round_ = round_;
    }
    lap(0);
    observe();
    lap(1);
    if (me.is_recalling() || me.is_submitting_gems() || me.is_teleporting()) {
        mode_ = me.is_submitting_gems() ? 2 : 1;
        return;
    }

    const bool tavi = kind_ == static_cast<int>(cambc::champion_type::tavi);
    if (on_spawn() || tavi) shop();

    if (!fuel(12000)) return;
    if (fight_or_flee()) return;
    lap(2);
    if (!fuel(12000)) return;
    if (round_ <= flee_until_ && !on_spawn() && incoming_ <= 0
        && (me.health() < me.max_health() * 0.5f || me.gem_count() >= 30)) {
        if (try_recall(6)) return;
    }

    // Answer an invasion reported near us when healthy.
    {
        vec2 rp;
        int rc = 0, rage = 99;
        const bool ours = cheb(rp, spawn_center_) * 10 < cheb(rp, espawn_center_) * 8;
        bool fighting = false;
        if (read_report(rp, rc, rage, &fighting) && fighting && rage <= 2 && rc <= 2 && cheb(rp, pos_) <= 10 && cheb(rp, pos_) > 3 && ours
            && me.health() > me.max_health() * 0.7f && me.gem_count() < 30 && incoming_ <= 0 && !on_spawn()) {
            mode_ = 5;
            if (go_tile(rp, 2, true)) return;
        }
    }

    mode_ = 6;
    if (tribute()) return;
    lap(3);
    mode_ = 7;
    if (!fuel(12000)) return;

    // Shopping trip when rich and not in a fight (sooner when hurt anyway).
    const int gold_need = me.health() < me.max_health() * 0.6f ? 150 : 300;
    if (!tavi && !on_spawn() && incoming_ <= 0 && static_cast<int>(me.gold_count()) >= gold_need && round_ < 1850) {
        if (try_recall(7)) return;
    }
    farm();
}
