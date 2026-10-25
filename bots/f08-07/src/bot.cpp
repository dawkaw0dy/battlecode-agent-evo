// Champion decisions: farmers claim camp groups through shared memory and join each other on big
// ones, share enemy sightings, lock enemy tributes and ambush gem carriers.
#include <cambc.h>
#include "bot.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <variant>
#include "draft.h"

namespace {

using cambc::champion_type;
using cambc::ability_kind;

int draft_rank(champion_type t) {
    using T = champion_type;
    using D = cambc::champion_draft;
    D d = D::ryth;
    switch (t) {
    case T::ryth: d = D::ryth; break;
    case T::vesper_vale: d = D::vesper_vale; break;
    case T::vendetta: d = D::vendetta; break;
    case T::cassi: case T::issac: d = D::cassi_and_issac; break;
    case T::stellar: d = D::stellar; break;
    case T::prism: d = D::prism; break;
    case T::marcella: d = D::marcella; break;
    case T::willow: d = D::willow; break;
    case T::ren: d = D::ren; break;
    case T::varg: d = D::varg; break;
    case T::tavi: d = D::tavi; break;
    case T::heart_of_ruin: d = D::heart_of_ruin; break;
    }
    const auto& order = draft_order();
    for (size_t i = 0; i < order.size(); ++i)
        if (order[i] == d) return static_cast<int>(i) * 2 + (t == T::issac ? 1 : 0);
    return 40 + static_cast<int>(t);
}

// Shard build per champion: the item bought at each step (the last entry repeats forever).
std::vector<cambc::item_type> build_for(champion_type t) {
    using I = cambc::item_type;
    using T = champion_type;
    std::vector<I> b;
    auto add = [&](I it, int n) { for (int i = 0; i < n; ++i) b.push_back(it); };
    switch (t) {
    case T::marcella: case T::varg: case T::ryth: case T::ren:
        add(I::jungle_shard_of_health, 5);
        add(I::monster_shard_of_strength, 5);
        add(I::durability_shard_of_health, 5);
        add(I::power_shard_of_strength, 1);
        break;
    case T::vendetta: case T::heart_of_ruin: case T::issac:
        add(I::monster_shard_of_strength, 5);
        add(I::jungle_shard_of_health, 5);
        add(I::power_shard_of_strength, 1);
        break;
    case T::vesper_vale:
        add(I::monster_shard_of_strength, 5);
        add(I::treasure_shard_of_strength, 5);
        add(I::power_shard_of_strength, 1);
        break;
    case T::willow:
        add(I::monster_shard_of_strength, 5);
        add(I::power_shard_of_health, 5);
        add(I::power_shard_of_focus, 1);
        break;
    default:
        add(I::monster_shard_of_strength, 5);
        add(I::power_shard_of_strength, 1);
        break;
    }
    return b;
}

std::vector<cambc::item_type> g_build;
int g_bought = 0;

float gold_weight(int round) { return round < 1000 ? 0.2f : (round < 1600 ? 0.1f : 0.03f); }

constexpr int kClaimBytes = 5;  // bytes 0-4: each champion's target group + 1; bytes 5-7: last sighting

}  // namespace

// ---------------------------------------------------------------------------------------------
// Initialisation

void Bot::initialise() {
    PROF(0);
    init_ = true;
    init_dims();
    auto& me = cambc::self();
    type_ = me.champion_name();
    g_build = build_for(type_);
    melee_ = is_melee_type(type_);
    healer_ = type_ == champion_type::willow;

    // Claim slots from the draft preference order of the allied champions.
    struct R { int rank; int idx; };
    std::vector<R> ranks;
    ranks.push_back({draft_rank(type_), -1});
    for (auto& c : cambc::champions(team::ally)) {
        if (c == me) continue;
        Mate m{c};
        m.type = c.champion_name();
        m.melee = is_melee_type(m.type);
        try {
            const auto s = c.base_stats();
            m.dps = s.base_strength * 1.125f + s.base_focus * 0.2f;
            m.hp = s.base_health;
        } catch (const std::exception&) {}
        mates_.push_back(m);
        ranks.push_back({draft_rank(m.type), static_cast<int>(mates_.size()) - 1});
    }
    std::sort(ranks.begin(), ranks.end(), [](const R& a, const R& b) { return a.rank < b.rank; });
    for (int i = 0; i < static_cast<int>(ranks.size()); ++i) {
        const int sl = std::min(i, kClaimBytes);  // a sixth champion has no claim byte
        if (ranks[i].idx < 0) slot_ = sl;
        else mates_[static_cast<size_t>(ranks[i].idx)].slot = sl;
    }
    if (healer_)
        for (size_t i = 0; i < mates_.size(); ++i)
            if (mates_[i].type == champion_type::marcella) partner_ = static_cast<int>(i);
    mate_alive_.assign(mates_.size(), true);

    // Terrain.
    block_ = cambc::map::walls() | cambc::map::holes() | cambc::map::altars() | cambc::map::spawn_region(team::enemy);
    opaque_ = cambc::map::walls() | cambc::map::bushes();
    spawn_ = cambc::map::spawn_region(team::ally);
    {
        auto sp = spawn_.positions();
        int sx = 0, sy = 0;
        for (auto p : sp) { sx += p.x; sy += p.y; }
        if (!sp.empty()) home_c_ = vec2{sx / static_cast<int>(sp.size()), sy / static_cast<int>(sp.size())};
        auto ep = cambc::map::spawn_region(team::enemy).positions();
        sx = sy = 0;
        for (auto p : ep) { sx += p.x; sy += p.y; }
        if (!ep.empty()) enemy_c_ = vec2{sx / static_cast<int>(ep.size()), sy / static_cast<int>(ep.size())};
    }
    home_.compute(block_, spawn_);
    altars_ = cambc::map::altars().positions();
    {
        cambc::bitboard roots;
        for (auto a : altars_) roots.draw_chebyshev(a, static_cast<float>(cambc::constants::submit_gems_range));
        roots &= ~block_;
        altar_.compute(block_, roots);
    }

    // Camp groups.
    camp_at_.assign(static_cast<size_t>(g_w) * g_h, -1);
    gh_ = cambc::map::camp_groups();
    for (const auto& grp : gh_) {
        GroupK G;
        G.tiles = grp.spawn_tiles().positions();
        G.gems = static_cast<int>(grp.gem_value());
        G.gold = static_cast<int>(grp.gold_value());
        G.respawn = grp.max_respawn_timer();
        const int gi = static_cast<int>(g_.size());
        int sx = 0, sy = 0;
        for (const auto& c : grp.camps()) {
            CampK k;
            k.pos = c.spawn_position();
            k.group = gi;
            try {
                const auto s = c.base_stats();
                k.max_hp = s.base_health;
                k.str = s.base_strength;
                k.range = s.base_attack_range;
            } catch (const std::exception&) {
                k.max_hp = 1350; k.str = 20; k.range = 3;
            }
            if (c.is_gem_cluster() || c.is_gold_cluster()) k.str = 0;
            k.hp = k.max_hp;
            sx += k.pos.x; sy += k.pos.y;
            G.total_hp += k.max_hp;
            G.total_str += k.str;
            if (k.str > 0) G.reach = std::max(G.reach, k.range);
            G.camps.push_back(static_cast<int>(c_.size()));
            if (inside(k.pos)) camp_at_[tidx(k.pos)] = static_cast<std::int16_t>(c_.size());
            c_.push_back(k);
        }
        if (!G.camps.empty()) G.center = vec2{sx / static_cast<int>(G.camps.size()), sy / static_cast<int>(G.camps.size())};
        // Spawn tiles we cannot walk onto cannot be captured.
        bool reach = false;
        for (auto t : G.tiles) if (home_.at(t) < kFar) reach = true;
        if (!reach) G.blocked_until = 1 << 30;
        g_.push_back(std::move(G));
    }

    // Abilities.
    for (const auto& a : me.abilities()) {
        if (!a.info()) continue;
        Abil x;
        x.slot = static_cast<int>(a.slot());
        try {
            if (a.is_damage()) { auto d = a.get<cambc::ability_damage>(); x.kind = ability_kind::damage; x.range = d.range; x.excl = d.exclusive_with_movement; }
            else if (a.is_aoe_damage()) { auto d = a.get<cambc::ability_aoe_damage>(); x.kind = ability_kind::aoe_damage; x.range = d.range; x.radius = d.radius; x.excl = d.exclusive_with_movement; }
            else if (a.is_cc()) { auto d = a.get<cambc::ability_cc>(); x.kind = ability_kind::cc; x.range = d.range; x.excl = d.exclusive_with_movement; }
            else if (a.is_heal()) { auto d = a.get<cambc::ability_heal>(); x.kind = ability_kind::heal; x.range = d.range; x.excl = d.exclusive_with_movement; }
            else if (a.is_dash()) { auto d = a.get<cambc::ability_dash>(); x.kind = ability_kind::dash; x.range = d.range; x.excl = d.exclusive_with_movement; }
            else if (a.is_shield()) { auto d = a.get<cambc::ability_shield>(); x.kind = ability_kind::shield; x.excl = d.exclusive_with_movement; }
            else if (a.is_attack_reset()) { auto d = a.get<cambc::ability_attack_reset>(); x.kind = ability_kind::attack_reset; x.excl = d.exclusive_with_movement; }
            else continue;
        } catch (const std::exception&) { continue; }
        ab_.push_back(x);
    }
}

// ---------------------------------------------------------------------------------------------
// Observation

void Bot::observe() {
    PROF(1);
    auto& me = cambc::self();
    round_ = static_cast<int>(cambc::round_number());
    pos_ = me.position();
    hp_ = me.health();
    max_hp_ = std::max(1.0f, me.max_health());
    gems_ = static_cast<int>(me.gem_count());
    gold_ = static_cast<int>(me.gold_count());
    moved_ = attacked_ = cast_ = move_fail_ = false;

    bool reset = false;
    float spell = 0;
    for (const auto& a : ab_) {
        if (a.kind == ability_kind::attack_reset) reset = true;
        if (a.kind == ability_kind::damage || a.kind == ability_kind::aoe_damage) spell += me.focus() * 1.5f;
    }
    base_dps_ = me.strength() * (reset ? 1.125f : 1.0f) + spell / 8.0f;

    occ_ = cambc::vision::champion_tiles();
    occ_ |= cambc::vision::camp_tiles();
    occ_ |= cambc::vision::teleport_tiles();
    vis_ = cambc::vision::visible_tiles();
    cambc::shared_memory.read(0, std::span<std::uint8_t>{claims_, 8});
    for (size_t i = 0; i < mates_.size(); ++i) {
        bool alive = false;
        try { alive = mates_[i].h.is_alive(); } catch (const std::exception&) {}
        mate_alive_[i] = alive;
    }

    std::vector<std::uint8_t> now(c_.size(), 0);
    {
        PROF(16);
        friends_.clear();
        foes_.clear();
        for (int side = 0; side < 2; ++side) {
            const team t = side == 0 ? team::ally : team::enemy;
            for (auto p : cambc::vision::champion_tiles(t).positions()) {
                if (p == pos_) continue;
                try {
                    auto c = cambc::vision::champion_at(p);
                    Seen s;
                    s.pos = p;
                    s.hp = c.health();
                    s.max_hp = std::max(1.0f, c.max_health());
                    s.str = c.strength();
                    s.focus = c.focus();
                    s.range = c.attack_range();
                    s.type = c.champion_name();
                    s.melee = is_melee_type(s.type);
                    if (side == 1) {
                        s.gems = static_cast<int>(c.gem_count());
                        s.tribute = c.is_submitting_gems();
                        s.channel = s.tribute || c.is_recalling();
                    }
                    (side == 0 ? friends_ : foes_).push_back(s);
                } catch (const std::exception&) {}
            }
        }
    }
    {
        PROF(17);
        camp_in_ = 0;
        for (const auto& c : cambc::vision::camps()) {
            const vec2 sp = c.spawn_position();
            if (!inside(sp)) continue;
            const int i = camp_at_[tidx(sp)];
            if (i < 0) continue;
            float hp = 0;
            bool awake = false;
            try {
                hp = c.health();
                awake = c.is_awake();
            } catch (const std::exception&) { continue; }
            CampK& k = c_[i];
            k.hp = hp;
            k.alive = hp > 0;
            now[i] = 1;
            if (awake && k.str > 0 && cheb(pos_, k.pos) <= k.range) {
                // Camps hit the nearest champion: count it if no visible ally is closer.
                const int d = cheb(pos_, k.pos);
                bool closer = false;
                for (const auto& f : friends_) if (cheb(f.pos, k.pos) < d) { closer = true; break; }
                if (!closer) camp_in_ += k.str;
            }
        }
    }
    {
        PROF(18);
        for (int gi = 0; gi < static_cast<int>(g_.size()); ++gi) {
            GroupK& G = g_[gi];
            bool visible = false;
            for (auto t : G.tiles) if (raw_bit(vis_.ptr(), t)) { visible = true; break; }
            if (!visible) {
                if (G.st == GroupK::respawning && round_ >= G.ready) {
                    G.st = GroupK::alive;
                    for (int ci : G.camps) { c_[ci].hp = c_[ci].max_hp; c_[ci].alive = true; }
                }
                // A reward nobody of ours watches is probably taken: assume its respawn started.
                if (G.st == GroupK::reward && round_ - G.seen > 12) {
                    G.st = GroupK::respawning;
                    G.ready = G.seen + 5 + G.respawn;
                }
                continue;
            }
            G.seen = round_;
            cambc::map::camp_group_state st;
            try { st = gh_[static_cast<size_t>(gi)].state(); } catch (const std::exception&) { continue; }
            if (std::holds_alternative<cambc::map::camp_group_alive>(st)) {
                if (G.st != GroupK::alive) for (int ci : G.camps) { c_[ci].hp = c_[ci].max_hp; c_[ci].alive = true; }
                G.st = GroupK::alive;
                for (int ci : G.camps)
                    if (!now[ci] && raw_bit(vis_.ptr(), c_[ci].pos)) { c_[ci].alive = false; c_[ci].hp = 0; }
            } else if (std::holds_alternative<cambc::map::camp_group_reward_available>(st)) {
                G.st = GroupK::reward;
                for (int ci : G.camps) { c_[ci].alive = false; c_[ci].hp = 0; }
            } else {
                const int timer = std::get<cambc::map::camp_group_respawning>(st).timer;
                G.st = GroupK::respawning;
                G.ready = round_ + std::max(1, timer);
                for (int ci : G.camps) { c_[ci].alive = false; c_[ci].hp = 0; }
            }
        }
    }

    if (!foes_.empty()) write_sighting();
    // Last sighting: x, y (7 bits each), round & 127, rich (≥ 10 gems), several enemies.
    const std::uint32_t v = claims_[5] | (claims_[6] << 8) | (claims_[7] << 16);
    if (v == 0) {
        sight_age_ = 999;
    } else {
        sight_pos_ = vec2{static_cast<int>(v & 127), static_cast<int>((v >> 7) & 127)};
        sight_age_ = (round_ - static_cast<int>((v >> 14) & 127)) & 127;
        sight_rich_ = (v >> 21) & 1;
        sight_many_ = (v >> 22) & 1;
    }
}

void Bot::write_sighting() {
    const Seen* best = nullptr;
    for (const auto& f : foes_)
        if (!best || f.gems > best->gems) best = &f;
    if (!best) return;
    const std::uint32_t v = static_cast<std::uint32_t>(best->pos.x & 127) | (static_cast<std::uint32_t>(best->pos.y & 127) << 7)
        | (static_cast<std::uint32_t>(round_ & 127) << 14) | (best->gems >= 10 ? 1u << 21 : 0u)
        | (foes_.size() >= 2 ? 1u << 22 : 0u) | (1u << 23);
    std::uint8_t b[3] = {static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(v >> 8), static_cast<std::uint8_t>(v >> 16)};
    cambc::shared_memory.write(5, std::span<std::uint8_t>{b, 3});
    claims_[5] = b[0]; claims_[6] = b[1]; claims_[7] = b[2];
}

void Bot::claim(int g) {
    if (slot_ >= kClaimBytes) return;
    const std::uint8_t v = static_cast<std::uint8_t>(g < 0 ? 0 : g + 1);
    if (claims_[slot_] == v) return;
    cambc::shared_memory[static_cast<size_t>(slot_)] = v;
    claims_[slot_] = v;
}

// ---------------------------------------------------------------------------------------------
// Shopping and support spells

void Bot::shop() {
    PROF(2);
    auto& me = cambc::self();
    for (int guard = 0; guard < 12 && !g_build.empty(); ++guard) {
        const auto item = g_build[std::min<size_t>(static_cast<size_t>(g_bought), g_build.size() - 1)];
        if (static_cast<int>(me.gold_count()) < static_cast<int>(item.cost())) break;
        if (!me.can_buy(item, 1)) break;
        me.buy(item, 1);
        ++g_bought;
    }
    // Keep an altar lock once the first emblem is done.
    if (g_bought >= 5) {
        const auto lock = cambc::consumable_type::altar_lock;
        if (me.consumables()[lock].count() == 0 && me.can_buy(lock)) me.buy(lock);
    }
}

void Bot::support_spells() {
    PROF(3);
    auto& me = cambc::self();
    if (!me.can_cast()) return;
    auto abil = me.abilities();
    for (const auto& a : ab_) {
        if (cast_) return;
        const auto& ab = abil[static_cast<size_t>(a.slot)];
        if (a.kind == ability_kind::heal) {
            if (!ab.is_ready()) continue;
            const Seen* best = nullptr;
            float need = 0;
            for (const auto& f : friends_) {
                if (cheb(f.pos, pos_) > static_cast<int>(a.range)) continue;
                const float miss = f.max_hp - f.hp;
                if (miss > need) { need = miss; best = &f; }
            }
            if (best && need > me.focus() * 2.2f) {
                try {
                    if (ab.use(cambc::vision::champion_at(best->pos))) cast_ = true;
                } catch (const std::exception&) {}
            }
        } else if (a.kind == ability_kind::shield) {
            if (!ab.is_ready()) continue;
            bool threat = camp_in_ > 0;
            for (const auto& f : foes_) if (cheb(f.pos, pos_) <= f.range + 1) threat = true;
            if (threat && hp_ < max_hp_ * 0.9f) {
                try { if (ab.use()) cast_ = true; } catch (const std::exception&) {}
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Movement

bool Bot::free_tile(vec2 t) const {
    return inside(t) && !raw_bit(block_.ptr(), t) && !raw_bit(occ_.ptr(), t);
}

bool Bot::step_down(const Field& f) {
    auto& me = cambc::self();
    if (moved_ || move_fail_ || !me.can_move()) return false;
    const int cur = f.at(pos_);
    int best = cur;
    vec2 to = pos_;
    for (auto d : kDirs) {
        const vec2 t = pos_ + d;
        if (!free_tile(t)) continue;
        const int v = f.at(t);
        if (v < best) { best = v; to = t; }
    }
    if (to == pos_) return false;
    return move_to(to);
}

// Every walk goes through here: re-check the tile live (vision changes after a dash).
bool Bot::move_to(vec2 to) {
    auto& me = cambc::self();
    if (moved_ || move_fail_ || !me.can_move()) return false;
    if (cambc::vision::has_champion(to) || cambc::vision::has_camp(to)) { move_fail_ = true; return false; }
    if (me.move_absolute(to)) {
        moved_ = true;
        pos_ = to;
        return true;
    }
    move_fail_ = true;
    return false;
}

bool Bot::dash_towards(const Field& f) {
    auto& me = cambc::self();
    if (cast_ || move_fail_ || !me.can_cast()) return false;
    for (const auto& a : ab_) {
        if (a.kind != ability_kind::dash) continue;
        auto abil = me.abilities();
        const auto& ab = abil[static_cast<size_t>(a.slot)];
        if (!ab.is_ready()) return false;
        const int cur = f.at(pos_);
        int best = cur - 1;  // must beat a plain step
        vec2 to = pos_;
        for (int dx = -2; dx <= 2; ++dx)
            for (int dy = -2; dy <= 2; ++dy) {
                const vec2 t{pos_.x + dx, pos_.y + dy};
                if (cheb(t, pos_) != 2 || !free_tile(t)) continue;
                // A legal path needs a non-wall tile between us.
                const vec2 mid{pos_.x + dx / 2, pos_.y + dy / 2};
                if (raw_bit(cambc::map::walls().ptr(), mid)) continue;
                const int v = f.at(t);
                if (v < best) { best = v; to = t; }
            }
        if (to == pos_) return false;
        try {
            if (ab.use(to)) {
                cast_ = true;
                pos_ = to;
                occ_ = cambc::vision::champion_tiles();
                occ_ |= cambc::vision::camp_tiles();
                occ_ |= cambc::vision::teleport_tiles();
                return true;
            }
            move_fail_ = true;
        } catch (const std::exception&) {}
        return false;
    }
    return false;
}

// Walk to a tile within r of target (with line of sight if los). Returns true if we moved.
bool Bot::approach(vec2 target, int r, bool los) {
    PROF(10);
    auto& me = cambc::self();
    if (moved_ || move_fail_ || !me.can_move()) return false;
    cambc::bitboard roots;
    if (los) roots = opaque_.shadow_cast(target, static_cast<std::uint16_t>(r));
    else roots.draw_chebyshev(target, static_cast<float>(r));
    roots &= ~block_;
    if (bit(roots, pos_)) return false;
    roots &= ~occ_;
    if (roots.is_empty()) return false;
    cambc::bitboard walls = block_ | occ_;
    walls.set(pos_, false);
    Field f;
    f.f = std::make_unique<cambc::distance_field>();
    cambc::bitboard seen;
    const int lim = cheb(pos_, target) + 12;
    f.f->compute(walls, roots, seen, static_cast<std::uint16_t>(lim));
    f.q = static_cast<const cambc::distance_field&>(*f.f).ptr();
    if (f.at(pos_) >= kFar) return false;
    return step_down(f);
}

void Bot::ensure_field(int g) {
    PROF(11);
    GroupK& G = g_[g];
    if (G.field.ready()) return;
    if (cambc::fuel_remaining() < 40000) return;
    cambc::bitboard roots;
    for (auto t : G.tiles) roots.set(t);
    cambc::bitboard walls = block_ & ~roots;
    G.field.compute(walls, roots);
}

int Bot::group_dist(int g) {
    GroupK& G = g_[g];
    if (G.field.ready()) return G.field.at(pos_);
    return cheb(pos_, G.center) * 5 / 4 + 2;
}

bool Bot::go_group(int g) {
    PROF(12);
    ensure_field(g);
    GroupK& G = g_[g];
    if (!G.field.ready()) return approach(G.center, 2, false);
    if (G.field.at(pos_) > 6) dash_towards(G.field);
    if (step_down(G.field)) return true;
    // Blocked by units: route around them.
    return approach(G.center, 2, false);
}

bool Bot::go_home() {
    if (home_.at(pos_) > 6) dash_towards(home_);
    return step_down(home_);
}

// ---------------------------------------------------------------------------------------------
// Attacks and spells

void Bot::reset_attack() {
    auto& me = cambc::self();
    if (!attacked_ || cast_ || !me.can_cast()) return;
    auto abil = me.abilities();
    for (const auto& a : ab_) {
        if (a.kind != ability_kind::attack_reset) continue;
        const auto& ab = abil[static_cast<size_t>(a.slot)];
        if (!ab.is_ready()) continue;
        try {
            if (ab.use()) { cast_ = true; attacked_ = false; }
        } catch (const std::exception&) {}
        return;
    }
}

bool Bot::cast_on_camp(int g) {
    auto& me = cambc::self();
    if (cast_ || !me.can_cast()) return false;
    auto abil = me.abilities();
    for (const auto& a : ab_) {
        if (a.kind != ability_kind::damage && a.kind != ability_kind::aoe_damage) continue;
        if (a.excl && moved_) continue;
        const auto& ab = abil[static_cast<size_t>(a.slot)];
        if (!ab.is_ready()) continue;
        // Strongest living attacker of the group in range (spells are worth most on high-hp camps).
        int best = -1;
        float bs = -1;
        for (int ci : g_[g].camps) {
            const CampK& k = c_[ci];
            if (!k.alive || !raw_bit(vis_.ptr(), k.pos) || !raw_bit(occ_.ptr(), k.pos)) continue;
            if (cheb(pos_, k.pos) > static_cast<int>(a.range)) continue;
            const float s = k.hp + (k.attacker() ? 5000 : 0);
            if (s > bs) { bs = s; best = ci; }
        }
        if (best < 0) continue;
        if (a.kind == ability_kind::aoe_damage && c_[best].hp < 300) continue;
        try {
            const bool ok = a.kind == ability_kind::aoe_damage ? ab.use(c_[best].pos)
                                                               : ab.use(cambc::vision::camp_at(c_[best].pos));
            if (ok) { cast_ = true; return true; }
        } catch (const std::exception&) {}
    }
    return false;
}

bool Bot::attack_camps(int g) {
    PROF(13);
    auto& me = cambc::self();
    cast_on_camp(g);
    bool any = false;
    for (int pass = 0; pass < 2; ++pass) {
        if (!me.can_attack()) break;
        const int range = me.attack_range();
        int best = -1;
        float bs = 1e9f;
        for (int ci : g_[g].camps) {
            const CampK& k = c_[ci];
            if (!k.alive || cheb(pos_, k.pos) > range || !raw_bit(vis_.ptr(), k.pos) || !raw_bit(occ_.ptr(), k.pos)) continue;
            // Attackers first, weakest first; clusters last.
            const float s = (k.attacker() ? 0.0f : 100000.0f) + k.hp;
            if (s < bs) { bs = s; best = ci; }
        }
        if (best < 0) break;
        try {
            auto camp = cambc::vision::camp_at(c_[best].pos);
            me.attack(camp);
            attacked_ = true;
            any = true;
            last_hit_round_ = round_;
            try {
                c_[best].hp = camp.health();
                if (c_[best].hp <= 0) c_[best].alive = false;
            } catch (const std::exception&) { c_[best].alive = false; }
        } catch (const std::exception&) { break; }
        if (pass == 0) reset_attack();
        if (attacked_) break;  // no reset happened
    }
    return any;
}

bool Bot::cast_on_foe(const Seen& s) {
    auto& me = cambc::self();
    if (cast_ || !me.can_cast()) return false;
    auto abil = me.abilities();
    for (const auto& a : ab_) {
        if (a.kind != ability_kind::damage && a.kind != ability_kind::aoe_damage && a.kind != ability_kind::cc) continue;
        if (a.excl && moved_) continue;
        if (cheb(pos_, s.pos) > static_cast<int>(a.range)) continue;
        const auto& ab = abil[static_cast<size_t>(a.slot)];
        if (!ab.is_ready()) continue;
        try {
            const bool ok = a.kind == ability_kind::aoe_damage ? ab.use(s.pos) : ab.use(cambc::vision::champion_at(s.pos));
            if (ok) { cast_ = true; return true; }
        } catch (const std::exception&) {}
    }
    return false;
}

bool Bot::attack_foe(const Seen* s) {
    auto& me = cambc::self();
    if (!s) return false;
    cast_on_foe(*s);
    bool any = false;
    for (int pass = 0; pass < 2; ++pass) {
        if (!me.can_attack()) break;
        if (cheb(pos_, s->pos) > me.attack_range()) break;
        try {
            auto c = cambc::vision::champion_at(s->pos);
            if (!c.is_alive()) break;
            me.attack(c);
            attacked_ = true;
            any = true;
        } catch (const std::exception&) { break; }
        if (pass == 0) reset_attack();
        if (attacked_) break;
    }
    return any;
}

// ---------------------------------------------------------------------------------------------
// Fights

// Drop our altar lock on an altar where an enemy is tributing: it cancels the channel.
bool Bot::lock_tributes() {
    auto& me = cambc::self();
    const auto lock = cambc::consumable_type::altar_lock;
    bool have = false;
    try { have = me.consumables()[lock].count() > 0; } catch (const std::exception&) {}
    if (!have) return false;
    for (const auto& f : foes_) {
        if (!f.tribute) continue;
        for (auto a : altars_) {
            if (cheb(a, f.pos) > cambc::constants::submit_gems_range || cheb(a, pos_) > 2) continue;
            if (cambc::vision::has_altar_lock(a)) continue;
            try {
                if (me.consumables()[lock].use(a)) return true;
            } catch (const std::exception&) {}
        }
    }
    return false;
}

bool Bot::retreat_step() {
    auto& me = cambc::self();
    if (moved_ || move_fail_ || !me.can_move()) return false;
    auto threat = [&](vec2 t) {
        float s = 0;
        for (const auto& f : foes_) {
            const int reach = f.melee ? 2 : f.range + 1;
            if (cheb(t, f.pos) <= reach) s += f.dps();
        }
        for (const auto& k : c_) {
            if (!k.alive || k.str <= 0 || cheb(t, k.pos) > k.range || cheb(pos_, k.pos) > 8) continue;
            s += k.str * 0.5f;
        }
        return s;
    };
    float best = threat(pos_) * 10 + home_.at(pos_);
    vec2 to = pos_;
    for (auto d : kDirs) {
        const vec2 t = pos_ + d;
        if (!free_tile(t)) continue;
        const float s = threat(t) * 10 + home_.at(t);
        if (s < best) { best = s; to = t; }
    }
    if (to == pos_) return false;
    return move_to(to);
}

bool Bot::fight() {
    PROF(4);
    if (foes_.empty()) return false;
    auto& me = cambc::self();
    lock_tributes();
    float our_hp = hp_, our_dps = base_dps_;
    for (const auto& f : friends_) {
        if (cheb(f.pos, pos_) > 5) continue;
        our_hp += f.hp;
        our_dps += f.dps();
    }
    float their_hp = 0, their_dps = 0;
    int dmin = 99;
    const Seen* weakest = nullptr;
    float wscore = 1e9f;
    int carried = 0;
    for (const auto& f : foes_) {
        const int d = cheb(f.pos, pos_);
        dmin = std::min(dmin, d);
        their_hp += f.hp;
        their_dps += f.dps();
        carried += f.gems;
        const float s = f.hp / (1.0f + f.gems * 0.05f) + d * 40.0f - (f.tribute ? 300.0f : 0.0f);
        if (s < wscore) { wscore = s; weakest = &f; }
    }
    if (their_dps <= 0) return false;
    // Square law with a margin; camps hitting us count against us.
    const float P = our_hp * our_dps;
    const float Q = their_hp * (their_dps + camp_in_);
    const float ratio = P / std::max(1.0f, Q);
    const bool rich = carried >= 10;
    const bool engage = ratio >= (rich ? 1.15f : 1.5f);
    const bool flee = ratio < 0.8f;

    if (engage && weakest) {
        if (bit(cambc::map::spawn_region(team::enemy), weakest->pos)) return false;  // never chase into their spawn
        if (attack_foe(weakest)) return true;
        const int d = cheb(weakest->pos, pos_);
        if (d > 7) return false;
        if (melee_) {
            approach(weakest->pos, 1, false);
            attack_foe(weakest);
        } else {
            approach(weakest->pos, me.attack_range(), true);
        }
        hunt_until_ = round_ + 4;
        return true;
    }
    if (flee) {
        int reach = 0;
        for (const auto& f : foes_) reach = std::max(reach, f.melee ? 2 : f.range + 1);
        if (dmin <= reach + 2) {
            // Shoot back first when we cannot get away anyway (ranged must choose).
            if (!melee_ && dmin <= me.attack_range() && hp_ > max_hp_ * 0.5f && attack_foe(weakest)) return true;
            if (melee_) attack_foe(weakest);
            if (home_.at(pos_) > 4) dash_towards(home_);
            retreat_step();
            return true;
        }
    }
    // Neutral: take free hits.
    if (weakest && cheb(weakest->pos, pos_) <= me.attack_range()) {
        if (attack_foe(weakest)) return !melee_;
    }
    return false;
}

// Go after a lone gem carrier an ally reported close by.
bool Bot::hunt() {
    if (sight_age_ > 3 || !sight_rich_ || sight_many_) return false;
    if (hp_ < max_hp_ * 0.6f || gems_ >= 20) return false;
    const int d = cheb(sight_pos_, pos_);
    if (d > 10 || d <= 1) return false;
    if (bit(cambc::map::spawn_region(team::enemy), sight_pos_)) return false;
    if (last_hit_round_ >= round_ - 2) return false;  // busy with camps
    return approach(sight_pos_, 1, false);
}

// ---------------------------------------------------------------------------------------------
// Health, shopping trips, banking

bool Bot::low_health() {
    PROF(5);
    auto& me = cambc::self();
    const bool on_spawn = bit(spawn_, pos_);
    if (on_spawn && hp_ < max_hp_ * 0.92f && foes_.empty()) return true;  // heal up
    if (hp_ >= max_hp_ * 0.35f) return false;
    if (on_spawn) return true;
    bool foe_near = false;
    for (const auto& f : foes_) if (cheb(f.pos, pos_) <= 6) foe_near = true;
    if (!foe_near && camp_in_ <= 0) {
        if (home_.at(pos_) <= 4) { go_home(); return true; }
        try { me.recall(); } catch (const std::exception&) {}
        return true;
    }
    if (home_.at(pos_) > 4) dash_towards(home_);
    if (!retreat_step()) go_home();
    return true;
}

// Go home to spend gold once it buys enough for the walk.
bool Bot::shop_trip() {
    auto& me = cambc::self();
    if (type_ == champion_type::tavi) return false;
    const int hd = home_.at(pos_);
    if (hd >= kFar || hd <= 2) return false;
    if (gold_ < 150 + 4 * std::min(hd, 60)) return false;
    if (last_hit_round_ >= round_ - 2 || camp_in_ > 0 || !foes_.empty()) return false;
    if (hd <= 6) return go_home();
    try { me.recall(); } catch (const std::exception&) { return false; }
    return true;
}

bool Bot::want_bank() const {
    if (gems_ <= 0) return false;
    const int d = altar_.at(pos_);
    if (d >= kFar) return false;
    if (gems_ >= std::clamp(d, 20, 50)) return true;
    if (d <= 3) return true;
    if (round_ + d + gems_ / 10 + 6 >= static_cast<int>(cambc::constants::max_rounds)) return true;
    if (gems_ >= 15 && !foes_.empty()) return true;
    return false;
}

bool Bot::bank() {
    PROF(6);
    auto& me = cambc::self();
    const int r = cambc::constants::submit_gems_range;
    for (auto a : altars_) {
        if (cheb(a, pos_) > r) continue;
        bool enemy_lock = false;
        try {
            if (cambc::vision::has_altar_lock(a)) enemy_lock = cambc::vision::altar_lock_at(a).is_enemy();
        } catch (const std::exception&) {}
        if (enemy_lock) {
            if (me.can_attack()) {
                try { me.attack(cambc::vision::altar_lock_at(a)); attacked_ = true; return true; } catch (const std::exception&) {}
            }
            continue;
        }
        try {
            me.submit_gems(a);
            return true;
        } catch (const std::exception&) {}
    }
    if (altar_.at(pos_) > 5) dash_towards(altar_);
    if (step_down(altar_)) return true;
    vec2 best = pos_;
    int bd = 9999;
    for (auto a : altars_) if (cheb(a, pos_) < bd) { bd = cheb(a, pos_); best = a; }
    return bd < 9999 && approach(best, r, false);
}

// ---------------------------------------------------------------------------------------------
// Targets

float Bot::score(int g) const {
    const GroupK& G = g_[g];
    if (G.blocked_until > round_ || G.tiles.empty()) return -1;
    const float value = G.gems + gold_weight(round_) * G.gold;
    if (value <= 0) return -1;
    int wait = 0;
    if (G.st == GroupK::respawning) {
        wait = G.ready - round_;
        if (wait > 90) return -1;
    }
    float hp_left = 0, inc = 0;
    if (G.st != GroupK::reward) {
        for (int ci : G.camps) {
            const CampK& k = c_[ci];
            const float h = G.st == GroupK::alive ? (k.alive ? k.hp : 0) : k.max_hp;
            hp_left += h;
            if (h > 0) inc += k.str;
        }
    }
    // Allies who claim it fight with us; if that is not enough, count who else would have to come.
    float dps = base_dps_, tank = melee_ ? max_hp_ : max_hp_ * 0.8f, heal = healer_ ? 45.0f : 0.0f;
    auto add = [&](const Mate& m) {
        dps += m.dps;
        if (m.melee) tank = std::max(tank, m.hp);
        if (m.type == champion_type::willow) heal = 45.0f;
    };
    auto trips_for = [&]() {
        const float kill = hp_left / std::max(10.0f, dps);
        return std::max(0.0f, inc * 0.85f - heal) * kill / std::max(100.0f, 0.75f * tank);
    };
    for (size_t i = 0; i < mates_.size(); ++i)
        if (mate_alive_[i] && mates_[i].slot < kClaimBytes && claims_[mates_[i].slot] == g + 1) add(mates_[i]);
    float trips = trips_for();
    int extra_mates = 0;
    if (trips > 1.0f) {
        for (size_t i = 0; i < mates_.size() && trips > 1.0f; ++i) {
            if (!mate_alive_[i] || (mates_[i].slot < kClaimBytes && claims_[mates_[i].slot] == g + 1)) continue;
            add(mates_[i]);
            ++extra_mates;
            trips = trips_for();
        }
    }
    if (trips > 3.0f) return -1;
    const int d = G.field.ready() ? G.field.at(pos_) : cheb(pos_, G.center) * 5 / 4 + 2;
    if (d >= kFar) return -1;
    const float kill = hp_left / std::max(10.0f, dps);
    const float back = static_cast<float>(std::min(60, home_.at(G.center))) * 2 + 10;
    const float extra = std::floor(trips) * back;
    const float bankd = static_cast<float>(std::min(40, altar_.at(G.center))) * 0.3f;
    const float time = static_cast<float>(std::max(d, wait)) + kill + 5 + extra + bankd + 3 + 8.0f * extra_mates;
    // Our share of the value is our share of the damage.
    float s = value * (base_dps_ / std::max(10.0f, dps)) / time;
    if (g == target_) s *= 1.2f;
    if (sight_age_ <= 15 && sight_many_ && cheb(sight_pos_, G.center) <= 8) s *= 0.6f;
    return s;
}

int Bot::choose_target() {
    PROF(8);
    int best = -1;
    float bs = 0;
    for (int g = 0; g < static_cast<int>(g_.size()); ++g) {
        const float s = score(g);
        if (s > bs) { bs = s; best = g; }
    }
    if (best >= 0) ensure_field(best);
    return best;
}

// A cluster-only group (no attackers) close by: grab it on the way.
int Bot::pickup() {
    int best = -1, bd = 6;
    for (int g = 0; g < static_cast<int>(g_.size()); ++g) {
        const GroupK& G = g_[g];
        if (G.st != GroupK::alive || G.total_str > 0 || G.gems + G.gold <= 0 || G.blocked_until > round_) continue;
        const int d = cheb(pos_, G.center);
        if (d < bd) { bd = d; best = g; }
    }
    return best;
}

void Bot::work(int g) {
    PROF(9);
    auto& me = cambc::self();
    GroupK& G = g_[g];
    const int d = group_dist(g);
    if (G.st == GroupK::respawning) {
        // Wait outside its reach; a spawn tile we stand on delays the respawn.
        int nearest = 99;
        for (auto t : G.tiles) nearest = std::min(nearest, cheb(t, pos_));
        if (nearest > G.reach + 2) go_group(g);
        else if (nearest == 0) {
            auto& me2 = cambc::self();
            for (auto dd : kDirs) {
                const vec2 t = pos_ + dd;
                bool tile = false;
                for (auto q : G.tiles) if (q == t) tile = true;
                if (tile || !free_tile(t) || moved_ || !me2.can_move()) continue;
                move_to(t);
                break;
            }
        }
        return;
    }
    if (G.st == GroupK::reward) {
        for (auto t : G.tiles) if (t == pos_) return;  // capturing: hold
        for (const auto& f : friends_)
            for (auto t : G.tiles)
                if (f.pos == t) {
                    // An ally captures it: move on.
                    G.blocked_until = round_ + 8;
                    target_at_ = -100;
                    return;
                }
        vec2 best = G.tiles[0];
        int bd = 9999;
        for (auto t : G.tiles) {
            if (!free_tile(t)) continue;
            if (cheb(t, pos_) < bd) { bd = cheb(t, pos_); best = t; }
        }
        if (d > 8 || !approach(best, 0, false)) go_group(g);
        return;
    }
    int nearest = 99, ni = -1;
    for (int ci : G.camps) {
        const CampK& k = c_[ci];
        if (!k.alive) continue;
        const int dd = cheb(k.pos, pos_);
        if (dd < nearest) { nearest = dd; ni = ci; }
    }
    if (ni < 0) {
        go_group(g);  // the group says alive but we saw nothing: go look
        return;
    }
    // Not started yet: wait outside its reach until enough of the allies who claim it are here.
    bool started = last_hit_round_ >= round_ - 3;
    for (int ci : G.camps) if (c_[ci].alive && c_[ci].hp < c_[ci].max_hp) started = true;
    if (!started && G.total_str > 0) {
        float dps = base_dps_, tank = melee_ ? hp_ : hp_ * 0.8f, heal = healer_ ? 45.0f : 0.0f;
        float hp_left = 0, inc = 0;
        for (int ci : G.camps) if (c_[ci].alive) { hp_left += c_[ci].hp; inc += c_[ci].str; }
        bool coming = false;
        for (size_t i = 0; i < mates_.size(); ++i) {
            const Mate& m = mates_[i];
            if (!mate_alive_[i] || m.slot >= kClaimBytes || claims_[m.slot] != g + 1) continue;
            bool here = false;
            for (const auto& f : friends_) if (f.type == m.type && cheb(f.pos, pos_) <= 7) { here = true; tank = std::max(tank, m.melee ? f.hp : 0.0f); }
            if (!here) { coming = true; continue; }
            dps += m.dps;
            if (m.type == champion_type::willow) heal = 45.0f;
        }
        const float trips = std::max(0.0f, inc * 0.85f - heal) * (hp_left / std::max(10.0f, dps)) / std::max(100.0f, 0.75f * tank);
        if (wait_g_ != g) { wait_g_ = g; wait_turns_ = 0; }
        if (trips > 1.2f && coming && wait_turns_ < 30 && nearest <= G.reach + 4) {
            ++wait_turns_;
            if (nearest <= G.reach + 1) retreat_step();
            return;
        }
    }
    if (attack_camps(g)) { no_hit_ = 0; return; }
    const int range = me.attack_range();
    if (nearest <= range + 1) ++no_hit_;
    if (shrink_g_ != g) { shrink_g_ = g; shrink_ = 0; }
    if (no_hit_ > 2 && shrink_ < range - 1) { ++shrink_; no_hit_ = 0; }
    if (d > 10) {
        go_group(g);
    } else {
        // Melee to the weakest living attacker; ranged to the nearest living camp in sight.
        int tgt = ni;
        if (melee_) {
            float bs = 1e9f;
            for (int ci : G.camps) {
                const CampK& k = c_[ci];
                if (!k.alive) continue;
                const float s = (k.attacker() ? 0.0f : 3000.0f) + k.hp * 0.2f + cheb(k.pos, pos_) * 100.0f;
                if (s < bs) { bs = s; tgt = ci; }
            }
        }
        const int r = std::max(1, range - shrink_);
        // Camps in bushes are only seen from inside the bush: fall back to adjacent tiles.
        // Camps in bushes are only seen from inside the bush: fall back to adjacent tiles.
        if (!approach(c_[tgt].pos, r, !melee_) && !approach(c_[tgt].pos, 1, false)) go_group(g);
    }
    if (me.can_attack()) attack_camps(g);
}

void Bot::farm() {
    PROF(7);
    // The healer follows its partner's claim.
    int g = -1;
    if (partner_ >= 0 && mate_alive_[static_cast<size_t>(partner_)]) {
        const int pc = claims_[mates_[static_cast<size_t>(partner_)].slot];
        if (pc > 0 && pc <= static_cast<int>(g_.size())) g = pc - 1;
    }
    if (g < 0) {
        bool valid = target_ >= 0 && g_[target_].blocked_until <= round_;
        if (valid && g_[target_].st == GroupK::respawning && g_[target_].ready - round_ > 15) valid = false;
        if (!valid || round_ - target_at_ >= 6) {
            target_ = choose_target();
            target_at_ = round_;
        }
        g = target_;
    }
    const int p = pickup();
    if (p >= 0 && (g < 0 || cheb(pos_, g_[p].center) <= 4)) g = p;
    claim(g);
    if (g < 0) {
        if (gems_ > 0) { bank(); return; }
        go_home();
        return;
    }
    work(g);
}

// ---------------------------------------------------------------------------------------------
// Wards: look into bushes around us.

void Bot::ward() {
    PROF(14);
    auto& me = cambc::self();
    if (round_ < ward_try_) return;
    ward_try_ = round_ + 4;
    try {
        if (me.ward_cooldown() > 0) return;
    } catch (const std::exception&) { return; }
    if (me.is_recalling() || me.is_submitting_gems()) return;
    const auto cands = me.ward_placement_tiles().positions();
    const auto& bushes = cambc::map::bushes();
    vec2 best = pos_;
    float bs = 0.5f;
    for (auto t : cands) {
        if (!inside(t) || raw_bit(block_.ptr(), t)) continue;
        float s = 0;
        for (int dx = -2; dx <= 2; ++dx)
            for (int dy = -2; dy <= 2; ++dy) {
                const vec2 q{t.x + dx, t.y + dy};
                if (inside(q) && raw_bit(bushes.ptr(), q) && !raw_bit(vis_.ptr(), q)) s += 1;
            }
        if (raw_bit(bushes.ptr(), t)) s += 3;
        s += 0.1f * static_cast<float>(cheb(t, pos_));
        if (s > bs) { bs = s; best = t; }
    }
    if (best == pos_ || bs < 2) return;
    try { me.place_ward(best); } catch (const std::exception&) {}
}

// ---------------------------------------------------------------------------------------------

void Bot::play_turn() {
    PROF(15);
    auto& me = cambc::self();
    if (!init_) {
        initialise();
        if (cambc::fuel_remaining() < 90000) return;
    }
    if (!me.is_alive()) {
        shop();
        return;
    }
    observe();
    if (slot_ == 0 && round_ % 250 == 0) prof_dump(round_);
    if (me.is_recalling() || me.is_submitting_gems() || me.is_teleporting()) return;
    debug_block({
        if (round_ % 20 == 0) {
            std::cout << "r" << round_ << " " << cambc::to_string(type_) << " s" << slot_ << " p" << pos_.x << "," << pos_.y
                      << " hp" << static_cast<int>(hp_) << "/" << static_cast<int>(max_hp_) << " gems" << gems_ << " gold" << gold_
                      << " tgt" << target_ << " mode" << mode_;
            if (target_ >= 0) {
                float hl = 0; int na = 0;
                for (int ci : g_[target_].camps) if (c_[ci].alive) { hl += c_[ci].hp; ++na; }
                std::cout << " gst" << static_cast<int>(g_[target_].st) << " gd" << group_dist(target_) << " gv" << g_[target_].gems
                          << "/" << g_[target_].gold << " left" << static_cast<int>(hl) << "x" << na;
            }
            std::cout << " foes" << foes_.size() << "\n";
        }
    });
    if (bit(spawn_, pos_) || type_ == champion_type::tavi) shop();
    support_spells();
    if (fight()) { mode_ = 4; ward(); return; }
    if (low_health()) { mode_ = 5; return; }
    if (want_bank()) { mode_ = 1; bank(); ward(); return; }
    if (shop_trip()) { mode_ = 2; return; }
    if (hunt()) { mode_ = 6; ward(); return; }
    mode_ = 3;
    farm();
    ward();
}
