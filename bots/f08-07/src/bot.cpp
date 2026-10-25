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

constexpr int kClaimBytes = 5;  // bytes 0-4: each champion's target group + 1; bytes 5-7: last sighting
constexpr bool kRichGold = true;  // gold worth 0.35 gems (0.5 for Vesper Vale), fading to 40% by round 900

std::vector<cambc::item_type> g_build;
int g_bought = 0;

float gold_weight(int round, bool bonus) {
    if (!kRichGold) return round < 1000 ? 0.2f : (round < 1600 ? 0.1f : 0.03f);
    return (bonus ? 0.5f : 0.35f) * std::max(0.4f, 1.0f - static_cast<float>(round) / 1500.0f);
}

constexpr bool kCapturePriority = false;  // Marcella (gems) or Vesper Vale (gold) captures when near
constexpr bool kRetreatOnHit = false;  // leave a fight only when champions are hitting us
constexpr float kEngageBias = -60.0f;   // simulated value margin to take a fight
constexpr float kShareTurns = 20.0f;   // clears shorter than this (alone) are left to whoever claimed them
constexpr bool kSharePenalty = false;  // ... measured worse
constexpr int kBankMin = 15, kBankMax = 40;  // carried gems that start a walk to the altar (by its distance)
constexpr float kTogether = 1.0f;      // bonus for a group allies claim (1.3: 15/40 vs 19/40)
constexpr int kRallyTurns = 40;        // longest wait for the allies a strong group needs
constexpr bool kRallyAll = false;      // strong groups wait for every claimer (measured worse)
constexpr bool kRallyBoss = true;      // bosses (monolith, golem) wait for every claimer  // bytes 0-4: each champion's target group + 1; bytes 5-7: last sighting

}  // namespace

// ---------------------------------------------------------------------------------------------
// Initialisation

void Bot::initialise() {
    PROF(0); g_stage = 0;
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

    // Altars whose every tribute tile is a camp spawn tile: those camps must die before anyone banks.
    {
        bool open_spot = false;
        std::vector<int> plugs;
        for (auto a : altars_)
            for (int dx = -1; dx <= 1; ++dx)
                for (int dy = -1; dy <= 1; ++dy) {
                    const vec2 t{a.x + dx, a.y + dy};
                    if (!inside(t) || raw_bit(block_.ptr(), t) || home_.at(t) >= kFar) continue;
                    const int ci = camp_at_[tidx(t)];
                    if (ci < 0) open_spot = true;
                    else plugs.push_back(c_[ci].group);
                }
        if (!open_spot)
            for (int gi : plugs) g_[gi].plug = true;
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
    PROF(1); g_stage = 1;
    auto& me = cambc::self();
    round_ = static_cast<int>(cambc::round_number());
    pos_ = me.position();
    hp_ = me.health();
    max_hp_ = std::max(1.0f, me.max_health());
    gems_ = static_cast<int>(me.gem_count());
    gold_ = static_cast<int>(me.gold_count());
    moved_ = attacked_ = cast_ = move_fail_ = false;
    // Damage since our last turn that the camps we stood under do not explain came from champions.
    if (prev_round_ == round_ - 1 && prev_hp_ - hp_ > prev_camp_in_ * 1.2f + 15.0f) champ_hit_round_ = round_;
    prev_round_ = round_;
    prev_hp_ = hp_;

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
        PROF(16); g_stage = 16;
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
                    if (cheb(p, pos_) <= 8) {
                        for (const auto& ab : c.abilities()) {
                            try {
                                if (ab.cooldown() > 0) continue;
                                if (ab.is_attack_reset()) s.burst += s.str;
                                else if (ab.is_damage() || ab.is_aoe_damage() || ab.is_cc()) s.burst += ab.damage_amount();
                            } catch (const std::exception&) {}
                        }
                    }
                    (side == 0 ? friends_ : foes_).push_back(s);
                } catch (const std::exception&) {}
            }
        }
    }
    {
        PROF(17); g_stage = 17;
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
            k.awake = awake;
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
        PROF(18); g_stage = 18;
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

    prev_camp_in_ = camp_in_;
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
    PROF(2); g_stage = 2;
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
    PROF(3); g_stage = 3;
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
    if (cambc::vision::has_champion(to) || cambc::vision::has_camp(to) || cambc::vision::has_teleport(to)) {
        move_fail_ = true;
        return false;
    }
    bool ok = false;
    try {
        ok = me.move_absolute(to);
    } catch (const cambc::error& e) {
        // The engine refuses a walk can_move() allowed (after a dash): stop walking this turn.
        debug_block({ std::cout << "move refused r" << round_ << " " << cambc::to_string(type_) << " cast" << cast_ << " " << e.what() << "\n"; });
        move_fail_ = true;
        return false;
    }
    if (ok) {
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
                if (cheb(t, pos_) != 2 || !free_tile(t) || !dash_path(t)) continue;
                const int v = f.at(t);
                if (v < best) { best = v; to = t; }
            }
        if (to == pos_) return false;
        if (cambc::vision::has_champion(to) || cambc::vision::has_camp(to) || cambc::vision::has_teleport(to)) return false;
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

// A dash needs a free, non-wall tile between us and its landing tile (holes are fine).
bool Bot::dash_path(vec2 t) const {
    const auto& walls = cambc::map::walls();
    for (auto d : kDirs) {
        const vec2 m = pos_ + d;
        if (!inside(m) || cheb(m, t) != 1) continue;
        if (raw_bit(walls.ptr(), m) || raw_bit(occ_.ptr(), m)) continue;
        return true;
    }
    return false;
}

// Dash two tiles closer to a tile (chasing).
bool Bot::dash_towards_tile(vec2 target) {
    auto& me = cambc::self();
    if (cast_ || move_fail_ || moved_ || !me.can_cast()) return false;
    for (const auto& a : ab_) {
        if (a.kind != ability_kind::dash) continue;
        auto abil = me.abilities();
        const auto& ab = abil[static_cast<size_t>(a.slot)];
        if (!ab.is_ready()) return false;
        int best = cheb(pos_, target) - 1;
        vec2 to = pos_;
        for (int dx = -2; dx <= 2; ++dx)
            for (int dy = -2; dy <= 2; ++dy) {
                const vec2 t{pos_.x + dx, pos_.y + dy};
                if (cheb(t, pos_) != 2 || !free_tile(t) || !dash_path(t)) continue;
                const int v = cheb(t, target);
                if (v < best && v >= 1) { best = v; to = t; }
            }
        if (to == pos_) return false;
        if (cambc::vision::has_champion(to) || cambc::vision::has_camp(to) || cambc::vision::has_teleport(to)) return false;
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
    PROF(10); g_stage = 10;
    auto& me = cambc::self();
    if (moved_ || move_fail_ || !me.can_move()) return false;
    cambc::bitboard roots;
    g_stage = 101;
    // Shadow casting from an opaque tile (a target in a bush) is rejected by the engine.
    if (los && inside(target) && !raw_bit(opaque_.ptr(), target)) roots = opaque_.shadow_cast(target, static_cast<std::uint16_t>(r));
    else roots.draw_chebyshev(target, static_cast<float>(r));
    g_stage = 102;
    roots &= ~block_;
    if (bit(roots, pos_)) return false;
    roots &= ~occ_;
    if (roots.is_empty()) return false;
    cambc::bitboard walls = block_ | occ_;
    g_stage = 103;
    walls.set(pos_, false);
    g_stage = 104;
    Field f;
    f.f = std::make_unique<cambc::distance_field>();
    cambc::bitboard seen;
    const int lim = cheb(pos_, target) + 12;
    f.f->compute(walls, roots, seen, static_cast<std::uint16_t>(lim));
    f.q = static_cast<const cambc::distance_field&>(*f.f).ptr();
    if (f.at(pos_) >= kFar) {
        // The way round is long (camps can plug a gap): search the whole map.
        if (cambc::fuel_remaining() < 35000) return false;
        f.f->compute(walls, roots, seen);
        if (f.at(pos_) >= kFar) return false;
    }
    return step_down(f);
}

void Bot::ensure_field(int g) {
    PROF(11); g_stage = 11;
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
    PROF(12); g_stage = 12;
    ensure_field(g);
    GroupK& G = g_[g];
    if (!G.field.ready()) return approach(G.center, 2, false);
    if (round_ > detour_until_) {
        if (G.field.at(pos_) > 6) dash_towards(G.field);
        if (step_down(G.field)) return true;
    }
    // Blocked by units or camps: route around them for a while.
    const bool ok = approach(G.center, 2, false);
    if (ok && round_ > detour_until_) detour_until_ = round_ + 8;
    return ok;
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
    PROF(13); g_stage = 13;
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
    // Living attackers that could reach any tile next to us.
    int near[24];
    int nn = 0;
    for (int ci = 0; ci < static_cast<int>(c_.size()) && nn < 24; ++ci) {
        const CampK& k = c_[ci];
        if (k.alive && k.str > 0 && cheb(pos_, k.pos) <= k.range + 1) near[nn++] = ci;
    }
    auto threat = [&](vec2 t) {
        float s = 0;
        for (const auto& f : foes_) {
            const int reach = f.melee ? 2 : f.range + 1;
            if (cheb(t, f.pos) <= reach) s += f.dps();
        }
        for (int i = 0; i < nn; ++i) {
            const CampK& k = c_[near[i]];
            if (cheb(t, k.pos) <= k.range) s += k.str * 0.5f;
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

namespace {

// One fighter in the focus-fire simulation.
struct FU {
    float hp = 0, dps = 0, value = 0, burst = 0;
    int start = 0;  // first turn it can deal damage
};

// Both sides focus the weakest living enemy each turn (overflow goes to the next). Returns the value
// side b loses minus the value side a loses, over `turns`.
float simulate(std::vector<FU> a, std::vector<FU> b, int turns) {
    auto lost = [](const std::vector<FU>& side, const std::vector<float>& hp0) {
        float v = 0;
        for (size_t i = 0; i < side.size(); ++i) {
            v += (hp0[i] - std::max(0.0f, side[i].hp)) * 0.4f;
            if (side[i].hp <= 0) v += side[i].value;
        }
        return v;
    };
    std::vector<float> a0, b0;
    for (const auto& u : a) a0.push_back(u.hp);
    for (const auto& u : b) b0.push_back(u.hp);
    auto hit = [](std::vector<FU>& side, float dmg) {
        while (dmg > 0) {
            int t = -1;
            for (size_t i = 0; i < side.size(); ++i)
                if (side[i].hp > 0 && (t < 0 || side[i].hp < side[static_cast<size_t>(t)].hp)) t = static_cast<int>(i);
            if (t < 0) return;
            const float d = std::min(dmg, side[static_cast<size_t>(t)].hp);
            side[static_cast<size_t>(t)].hp -= d;
            dmg -= d;
            if (side[static_cast<size_t>(t)].hp > 0) return;
        }
    };
    for (int t = 0; t < turns; ++t) {
        float da = 0, db = 0;
        for (const auto& u : a) if (u.hp > 0 && t >= u.start) da += u.dps + (t == u.start ? u.burst : 0.0f);
        for (const auto& u : b) if (u.hp > 0 && t >= u.start) db += u.dps + (t == u.start ? u.burst : 0.0f);
        hit(b, da);
        hit(a, db);
    }
    return lost(b, b0) - lost(a, a0);
}

}  // namespace

bool Bot::fight() {
    PROF(4); g_stage = 4;
    if (foes_.empty()) return false;
    auto& me = cambc::self();
    lock_tributes();

    // Our side: us and allies within 6; theirs: every enemy in view.
    auto start_turn = [](int d, int range, bool melee) {
        if (melee) return std::max(0, d - 2);
        return std::max(0, d - range);
    };
    std::vector<FU> ours, theirs;
    FU self_u;
    self_u.hp = hp_;
    self_u.dps = base_dps_;
    self_u.value = 350.0f + gems_ * 2.0f;
    int my_d = 99;
    for (const auto& f : foes_) my_d = std::min(my_d, cheb(f.pos, pos_));
    self_u.start = start_turn(my_d, me.attack_range(), melee_);
    {
        auto abil = me.abilities();
        for (const auto& a : ab_) {
            const auto& ab = abil[static_cast<size_t>(a.slot)];
            try {
                if (!ab.is_ready()) continue;
                if (a.kind == ability_kind::attack_reset) self_u.burst += me.strength();
                else if (a.kind == ability_kind::damage || a.kind == ability_kind::aoe_damage || a.kind == ability_kind::cc)
                    self_u.burst += ab.damage_amount();
            } catch (const std::exception&) {}
        }
    }
    ours.push_back(self_u);
    for (const auto& f : friends_) {
        if (f.hp < f.max_hp * 0.25f) continue;  // badly hurt allies are leaving
        int d = 99;
        for (const auto& e : foes_) d = std::min(d, cheb(f.pos, e.pos));
        // Near us or near the enemies, so that our own step does not change who counts.
        if (cheb(f.pos, pos_) > 7 && d > 8) continue;
        FU u;
        u.hp = f.hp;
        u.dps = f.dps();
        u.burst = f.burst;
        u.value = 350.0f;
        u.start = start_turn(d, f.range, f.melee);
        ours.push_back(u);
    }
    const Seen* target = nullptr;
    float tscore = 1e9f;
    int dmin = 99, reach_max = 0;
    float threat_me = 0;  // damage per turn the enemies can put on us if we stay
    for (const auto& f : foes_) {
        int d = 99;
        d = std::min(d, cheb(f.pos, pos_));
        for (const auto& fr : friends_) if (cheb(fr.pos, pos_) <= 6) d = std::min(d, cheb(f.pos, fr.pos));
        FU u;
        u.hp = f.hp;
        u.dps = f.dps();
        u.burst = f.channel ? 0.0f : f.burst;
        u.value = 350.0f + f.gems * 2.0f;
        u.start = f.channel ? 2 : start_turn(d, f.range, f.melee);
        theirs.push_back(u);
        const int dm = cheb(f.pos, pos_);
        dmin = std::min(dmin, dm);
        const int reach = f.melee ? 2 : f.range + 1;
        reach_max = std::max(reach_max, reach);
        if (dm <= reach && !f.channel) threat_me += f.dps();
        // Weakest first (focus fire), carriers and channelers are worth more, near ones are easier.
        const float sc = f.hp - f.gems * 4.0f - (f.channel ? 250.0f : 0.0f) + dm * 60.0f;
        if (sc < tscore) { tscore = sc; target = &f; }
    }
    if (camp_in_ > 0) {
        FU u;
        u.hp = 100000;  // camps do not die in this window
        u.dps = camp_in_;
        theirs.push_back(u);
    }
    const float v_fight = simulate(ours, theirs, 8);
    // Fleeing: melee in contact keep hitting us, ranged get about one shot.
    float v_flee = 0;
    for (const auto& f : foes_) {
        const int dm = cheb(f.pos, pos_);
        if (f.channel) continue;
        if (f.melee && dm <= 2) v_flee -= f.dps() * (melee_ ? 2.0f : 3.0f) * 0.4f;
        else if (!f.melee && dm <= f.range + 1) v_flee -= f.dps() * 1.2f * 0.4f;
    }
    if (hp_ + v_flee / 0.4f <= 0) v_flee -= self_u.value;
    const bool engage = v_fight > v_flee + kEngageBias;
    // A badly hurt champion under focus leaves the fight unless it finishes someone now.
    const bool doomed = threat_me * 2.0f >= hp_ && !(target && cheb(target->pos, pos_) <= me.attack_range()
                                                     && target->hp <= base_dps_ * 1.2f);
    debug_block({
        if (round_ % 5 == 0)
            std::cout << "fight r" << round_ << " " << cambc::to_string(type_) << " hp" << static_cast<int>(hp_) << " ours" << ours.size()
                      << " theirs" << foes_.size() << " vf" << static_cast<int>(v_fight) << " vr" << static_cast<int>(v_flee)
                      << " thr" << static_cast<int>(threat_me) << " dmin" << dmin << " eng" << engage << " doom" << doomed << "\n";
    });

    if (engage && !doomed && target) {
        if (bit(cambc::map::spawn_region(team::enemy), target->pos)) return false;  // never chase into their spawn
        if (attack_foe(target)) return true;
        const int d = cheb(target->pos, pos_);
        if (d > 8) return false;
        if (melee_) {
            if (d > 3) dash_towards_tile(target->pos);
            approach(target->pos, 1, false);
            attack_foe(target);
        } else {
            approach(target->pos, me.attack_range(), true);
        }
        return true;
    }
    // Not worth a fight: leave only when they are really hurting us (or we are low); otherwise keep farming.
    const bool hit_by_champs = round_ - champ_hit_round_ <= 1;
    const bool low = hp_ < max_hp_ * 0.4f;
    if (doomed || (low && threat_me > 0) || (hit_by_champs && v_fight < -60.0f) || (!kRetreatOnHit && (threat_me > 0 || dmin <= reach_max + 1))) {
        if (!doomed && !melee_ && target && cheb(target->pos, pos_) <= me.attack_range() && threat_me * 3.0f < hp_
            && attack_foe(target)) return true;
        if (melee_ && target && cheb(target->pos, pos_) <= 1) attack_foe(target);
        if (home_.at(pos_) > 3) dash_towards(home_);
        retreat_step();
        return true;
    }
    // Out of their reach: carry on, taking free shots.
    if (target && cheb(target->pos, pos_) <= me.attack_range()) {
        if (attack_foe(target)) return !melee_;
    }
    return false;
}

// Converge on a lone enemy an ally reported close by (from further away when it carries gems).
bool Bot::hunt() {
    if (sight_age_ > 3 || sight_many_) return false;
    if (hp_ < max_hp_ * 0.6f || gems_ >= 30) return false;
    const int d = cheb(sight_pos_, pos_);
    const int reach = sight_rich_ ? 12 : 7;
    if (d > reach || d <= 2) return false;
    if (bit(cambc::map::spawn_region(team::enemy), sight_pos_)) return false;
    if (last_hit_round_ >= round_ - 2) return false;  // busy with camps
    return approach(sight_pos_, 2, false);
}

// ---------------------------------------------------------------------------------------------
// Health, shopping trips, banking

bool Bot::low_health() {
    PROF(5); g_stage = 5;
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
    if (gems_ <= 0 || round_ < bank_block_until_) return false;
    const int d = altar_.at(pos_);
    if (d >= kFar) return false;
    if (gems_ >= std::clamp(d, kBankMin, kBankMax)) return true;
    if (d <= 3) return true;
    if (round_ + d + gems_ / 10 + 6 >= static_cast<int>(cambc::constants::max_rounds)) return true;
    if (gems_ >= 15 && !foes_.empty()) return true;
    // Near the end of a race: what we carry decides it.
    const int win = static_cast<int>(cambc::constants::winning_gems);
    if (static_cast<int>(cambc::global_submitted_gems(team::ally)) + gems_ >= win) return true;
    if (static_cast<int>(cambc::global_submitted_gems(team::enemy)) >= win - 150 && gems_ >= 5) return true;
    return false;
}

bool Bot::bank() {
    PROF(6); g_stage = 6;
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
    if (round_ > detour_until_) {
        if (altar_.at(pos_) > 5) dash_towards(altar_);
        if (step_down(altar_)) { bank_stuck_ = 0; return true; }
    }
    vec2 best = pos_;
    int bd = 9999;
    for (auto a : altars_) if (cheb(a, pos_) < bd) { bd = cheb(a, pos_); best = a; }
    // The static way is plugged (units or camps): keep routing around for a while.
    const bool ok = bd < 9999 && approach(best, r, false);
    if (ok && round_ > detour_until_) detour_until_ = round_ + 8;
    // No way to an open tribute tile (camps or units on all of them): farm for a while instead.
    if (ok || bd <= r) bank_stuck_ = 0;
    else if (++bank_stuck_ >= 3) { bank_stuck_ = 0; bank_block_until_ = round_ + 40; }
    return ok;
}

// ---------------------------------------------------------------------------------------------
// Targets

// Trips needed to clear a group: damage taken while focus-firing its attackers weakest first
// (incoming drops as they die), against the coalition's health and healing. t = turns to clear.
// Focus-firing a group's attackers weakest first, the damage taken is K / dps (incoming drops as they
// die) and the clear takes total_hp / dps. K and total_hp are computed once per group and round.
void Bot::clear_profile(int g, float& K, float& total) const {
    const GroupK& G = g_[g];
    if (G.prof_round == round_) { K = G.prof_k; total = G.prof_hp; return; }
    float att_hp[32], att_str[32];
    int n = 0;
    float rest = 0, inc = 0;
    for (int ci : G.camps) {
        const CampK& k = c_[ci];
        const float h = G.st == GroupK::alive ? (k.alive ? k.hp : 0) : (G.st == GroupK::reward ? 0 : k.max_hp);
        if (h <= 0) continue;
        if (k.str > 0 && n < 32) { att_hp[n] = h; att_str[n] = k.str; inc += k.str; ++n; }
        else rest += h;
    }
    for (int i = 1; i < n; ++i)
        for (int j = i; j > 0 && att_hp[j] < att_hp[j - 1]; --j) { std::swap(att_hp[j], att_hp[j - 1]); std::swap(att_str[j], att_str[j - 1]); }
    K = 0;
    total = rest;
    for (int i = 0; i < n; ++i) {
        K += att_hp[i] * inc * 0.85f;
        total += att_hp[i];
        inc -= att_str[i];
    }
    G.prof_round = round_;
    G.prof_k = K;
    G.prof_hp = total;
}

// Trips needed to clear a group against the coalition's health and healing. t = turns to clear.
float Bot::clear_trips(int g, float dps, float tank, float others_hp, float heal, float& t) const {
    float K = 0, total = 0;
    clear_profile(g, K, total);
    dps = std::max(10.0f, dps);
    t = total / dps;
    const float cap = 0.75f * (tank + 0.5f * others_hp) + heal * t;
    return K / dps / std::max(100.0f, cap);
}

float Bot::score(int g, float floor) const {
    const GroupK& G = g_[g];
    if (G.blocked_until > round_ || G.tiles.empty()) return -1;
    const float value = G.gems + gold_weight(round_, type_ == champion_type::vesper_vale) * G.gold;
    if (value <= 0) return -1;
    int wait = 0;
    if (G.st == GroupK::respawning) {
        wait = G.ready - round_;
        if (wait > 90) return -1;
    }
    // Cheap bound first (whole value, no clear time, every bonus): most groups stop here.
    {
        const int d0 = G.field.ready() ? G.field.at(pos_) : cheb(pos_, G.center) * 5 / 4 + 2;
        if (d0 >= kFar || value * 2.4f / static_cast<float>(std::max(d0, wait) + 8) <= floor) return -1;
    }
    // Allies who claim it fight with us; if that is not enough, count who else would have to come.
    float dps = base_dps_, tank = melee_ ? max_hp_ : max_hp_ * 0.8f, others_hp = 0, heal = healer_ ? 45.0f : 0.0f;
    auto add = [&](const Mate& m) {
        dps += m.dps;
        if (m.melee && m.hp > tank) { others_hp += tank; tank = m.hp; }
        else others_hp += m.hp;
        if (m.type == champion_type::willow) heal = 45.0f;
    };
    for (size_t i = 0; i < mates_.size(); ++i)
        if (mate_alive_[i] && mates_[i].slot < kClaimBytes && claims_[mates_[i].slot] == g + 1) add(mates_[i]);
    float kill = 0;
    float trips = clear_trips(g, dps, tank, others_hp, heal, kill);
    int extra_mates = 0;
    if (trips > 1.0f) {
        for (size_t i = 0; i < mates_.size() && trips > 1.0f; ++i) {
            if (!mate_alive_[i] || (mates_[i].slot < kClaimBytes && claims_[mates_[i].slot] == g + 1)) continue;
            add(mates_[i]);
            ++extra_mates;
            trips = clear_trips(g, dps, tank, others_hp, heal, kill);
        }
    }
    if (trips > 3.0f) return -1;
    const int d = G.field.ready() ? G.field.at(pos_) : cheb(pos_, G.center) * 5 / 4 + 2;
    if (d >= kFar) return -1;
    const float back = static_cast<float>(std::min(60, home_.at(G.center))) * 2 + 10;
    const float extra = std::floor(trips) * back;
    const float bankd = static_cast<float>(std::min(40, altar_.at(G.center))) * 0.3f;
    const float time = static_cast<float>(std::max(d, wait)) + kill + 5 + extra + bankd + 3 + 8.0f * extra_mates;
    // Our share of the value is our share of the damage.
    float s = value * (base_dps_ / std::max(10.0f, dps)) / time;
    // A short clear somebody already has is theirs.
    int claimers = 0;
    for (size_t i = 0; i < mates_.size(); ++i)
        if (mate_alive_[i] && mates_[i].slot < kClaimBytes && claims_[mates_[i].slot] == g + 1) ++claimers;
    if (kSharePenalty && claimers > 0 && extra_mates == 0 && trips < 0.5f && kill * (claimers + 1) < kShareTurns)
        for (int c = 0; c < claimers; ++c) s *= 0.3f;
    // Together we fight better: a group an ally works on is worth a little more.
    if (claimers > 0) s *= kTogether;
    if (g == target_) s *= 1.2f;
    if (G.plug) s *= 2.0f;  // nobody banks until it dies
    if (sight_age_ <= 15 && sight_many_ && cheb(sight_pos_, G.center) <= 8) s *= 0.6f;
    return s;
}

int Bot::choose_target() {
    PROF(8); g_stage = 8;
    int best = -1;
    float bs = 0;
    // The current target first: a good early best prunes most of the others.
    if (target_ >= 0 && target_ < static_cast<int>(g_.size())) {
        const float s = score(target_, 0);
        if (s > bs) { bs = s; best = target_; }
    }
    for (int g = 0; g < static_cast<int>(g_.size()); ++g) {
        if (g == target_) continue;
        if (best >= 0 && cambc::fuel_remaining() < 30000) break;  // out of budget: keep the best so far
        const float s = score(g, bs);
        if (s > bs) { bs = s; best = g; }
    }
    if (best >= 0) ensure_field(best);
    return best;
}

// A cluster-only group (no attackers) a short walk away: grab it on the way.
int Bot::pickup() {
    int best = -1, bd = 7;
    for (int g = 0; g < static_cast<int>(g_.size()); ++g) {
        const GroupK& G = g_[g];
        if (G.st != GroupK::alive || G.total_str > 0 || G.gems + G.gold <= 0 || G.blocked_until > round_) continue;
        if (cheb(pos_, G.center) > 6) continue;
        ensure_field(g);  // walls can make it far
        if (!G.field.ready()) continue;
        const int d = G.field.at(pos_);
        if (d < bd) { bd = d; best = g; }
    }
    return best;
}

void Bot::work(int g) {
    PROF(9); g_stage = 9;
    auto& me = cambc::self();
    GroupK& G = g_[g];
    const int d = group_dist(g);
    if (G.st == GroupK::respawning) {
        // Wait outside its reach; a spawn tile we stand on delays the respawn.
        sub_ = 13;
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
        // Who captures matters: Marcella earns gold for the gems she tributes, Vesper Vale more gold.
        auto cap_rank = [&](champion_type t) {
            if (!kCapturePriority) return 2;
            if (t == champion_type::marcella) return G.gems > 0 ? 0 : 1;
            if (t == champion_type::vesper_vale) return G.gems > 0 ? 1 : 0;
            return 2;
        };
        const int mine = cap_rank(type_);
        bool on_tile = false;
        for (auto t : G.tiles) if (t == pos_) on_tile = true;
        bool better_near = false, ally_on = false;
        for (const auto& f : friends_) {
            bool f_on = false;
            for (auto t : G.tiles) if (f.pos == t) f_on = true;
            if (f_on) ally_on = true;
            if (cap_rank(f.type) < mine && (f_on || cheb(f.pos, G.center) <= 5)) better_near = true;
        }
        debug_block({
            if (on_tile && round_ % 25 == 0) {
                std::cout << "cap r" << round_ << " " << cambc::to_string(type_) << " g" << g << " p" << pos_.x << "," << pos_.y << " tiles";
                for (auto t : G.tiles) std::cout << " " << t.x << "," << t.y;
                std::cout << " camps";
                for (int ci : G.camps) std::cout << " " << c_[ci].pos.x << "," << c_[ci].pos.y << (c_[ci].alive ? "A" : "d") << cambc::vision::has_camp(c_[ci].pos);
                try {
                    auto st = gh_[static_cast<size_t>(g)].state();
                    std::cout << " live" << st.index();
                } catch (const std::exception& e) { std::cout << " err" << e.what(); }
                std::cout << " foes" << foes_.size() << " friends" << friends_.size() << "\n";
            }
        });
        if (on_tile && !better_near) { sub_ = 11; return; }  // capturing: hold
        if (ally_on || better_near) {
            // An ally captures it: move on.
            G.blocked_until = round_ + 8;
            target_at_ = -100;
            if (on_tile) retreat_step();
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
    // Bosses: a damaged boss is not a started fight (camps do not heal); only an awake one is.
    bool boss = false;
    for (int ci : G.camps) if (c_[ci].alive && c_[ci].str >= 120) boss = true;
    bool started = last_hit_round_ >= round_ - 3;
    for (int ci : G.camps)
        if (c_[ci].alive && (boss ? c_[ci].awake : c_[ci].hp < c_[ci].max_hp)) started = true;
    if (!started && G.total_str > 0) {
        float dps = base_dps_, tank = melee_ ? hp_ : hp_ * 0.8f, others_hp = 0, heal = healer_ ? 45.0f : 0.0f;
        bool coming = false;
        int claimers = 0, present = 0, alive_mates = 0;
        for (size_t i = 0; i < mates_.size(); ++i) {
            const Mate& m = mates_[i];
            if (mate_alive_[i]) ++alive_mates;
            if (!mate_alive_[i] || m.slot >= kClaimBytes || claims_[m.slot] != g + 1) continue;
            ++claimers;
            const Seen* here = nullptr;
            for (const auto& f : friends_) if (f.type == m.type && cheb(f.pos, pos_) <= 7) here = &f;
            if (!here) { coming = true; continue; }
            ++present;
            dps += m.dps;
            if (m.melee && here->hp > tank) { others_hp += tank; tank = here->hp; }
            else others_hp += here->hp;
            if (m.type == champion_type::willow) heal = 45.0f;
        }
        float tclear = 0;
        const float trips = clear_trips(g, dps, tank, others_hp, heal, tclear);
        if (wait_g_ != g) { wait_g_ = g; wait_turns_ = 0; }
        // Wait while we are too weak for it and help is coming.
        const bool strong = kRallyAll ? (G.total_str >= 120 || trips > 1.2f) : trips > 1.2f;
        // A boss waits for every ally who claims it, and for at least two of us.
        const bool boss_wait = kRallyBoss && boss
            && (present < claimers || present + 1 < std::min(3, alive_mates + 1)) && wait_turns_ < kRallyTurns;
        if (((strong && coming && wait_turns_ < (kRallyAll ? kRallyTurns : 30)) || boss_wait) && nearest <= G.reach + 4) {
            ++wait_turns_;
            sub_ = 12;
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
    PROF(7); g_stage = 7;
    // The healer follows its partner's claim.
    int g = -1;
    if (partner_ >= 0 && mate_alive_[static_cast<size_t>(partner_)]) {
        // ... while the partner is in sight and its group still has camps to fight.
        const Mate& pm = mates_[static_cast<size_t>(partner_)];
        bool seen = false;
        for (const auto& f : friends_) if (f.type == pm.type) seen = true;
        const int pc = claims_[pm.slot];
        if (seen && pc > 0 && pc <= static_cast<int>(g_.size()) && g_[pc - 1].st == GroupK::alive
            && g_[pc - 1].blocked_until <= round_)
            g = pc - 1;
    }
    if (g < 0) {
        bool valid = target_ >= 0 && g_[target_].blocked_until <= round_;
        if (valid && g_[target_].st == GroupK::respawning && g_[target_].ready - round_ > 15) valid = false;
        // Re-plan every 6 rounds (later when this turn is already expensive) or when the target is gone.
        if (!valid || (round_ - target_at_ >= 6 && cambc::fuel_remaining() > 65000) || round_ - target_at_ >= 12) {
            target_ = choose_target();
            target_at_ = round_;
        }
        g = target_;
    }
    // A capture in progress is never interrupted for a pickup.
    if (g < 0 || g_[g].st != GroupK::reward) {
        const int p = pickup();
        if (p >= 0 && (g < 0 || g_[p].field.at(pos_) <= 5)) g = p;
    }
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
    PROF(14); g_stage = 14;
    auto& me = cambc::self();
    if (round_ < ward_try_) return;
    ward_try_ = round_ + 4;
    try {
        if (me.ward_cooldown() > 0) return;
    } catch (const std::exception&) { return; }
    if (me.is_recalling() || me.is_submitting_gems()) return;
    std::vector<vec2> cands;
    try { cands = me.ward_placement_tiles().positions(); } catch (const std::exception&) { return; }
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
    PROF(15); g_stage = 15;
    auto& me = cambc::self();
    if (!init_) {
        initialise();
        if (cambc::fuel_remaining() < 90000) return;
    }
    if (!me.is_alive()) {
        ++acc_[0];
        shop();
        return;
    }
    observe();
    debug_block({
        if (round_ % 400 == 0) {
            std::cout << "acc r" << round_ << " " << cambc::to_string(type_) << " dead" << acc_[0] << " recall" << acc_[1] << " trib" << acc_[2]
                      << " fight" << acc_[3] << " low" << acc_[4] << " bankw" << acc_[5] << " shop" << acc_[6] << " hunt" << acc_[7]
                      << " hit" << acc_[8] << " walk" << acc_[9] << " idle" << acc_[10] << " cap" << acc_[11] << " stage" << acc_[12]
                      << " respw" << acc_[13] << "\n";
        }
    });
    if (me.is_recalling()) ++acc_[1];
    if (me.is_submitting_gems()) ++acc_[2];
    if (round_ % 250 == 0) prof_dump(round_);
    if (me.is_recalling() || me.is_submitting_gems() || me.is_teleporting()) return;
    debug_block({
        if (round_ % 20 == 0) {
            std::cout << "r" << round_ << " " << cambc::to_string(type_) << " s" << slot_ << " p" << pos_.x << "," << pos_.y
                      << " hp" << static_cast<int>(hp_) << "/" << static_cast<int>(max_hp_) << " gems" << gems_ << " gold" << gold_
                      << " tgt" << target_ << " cl" << (static_cast<int>(claims_[slot_]) - 1) << " mode" << mode_ << "/" << sub_;
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
    // Stunned: nothing else is allowed this turn.
    try {
        if (me.buffs().has_stun()) return;
    } catch (const std::exception&) {}
    support_spells();
    if (fight()) { mode_ = 4; ++acc_[3]; ward(); return; }
    if (low_health()) { mode_ = 5; ++acc_[4]; return; }
    if (want_bank()) { mode_ = 1; ++acc_[5]; bank(); ward(); return; }
    if (shop_trip()) { mode_ = 2; ++acc_[6]; return; }
    if (hunt()) { mode_ = 6; ++acc_[7]; ward(); return; }
    mode_ = 3;
    sub_ = 0;
    farm();
    if (sub_) ++acc_[sub_];
    else if (attacked_) ++acc_[8];
    else if (moved_) ++acc_[9];
    else ++acc_[10];
    ward();
}
