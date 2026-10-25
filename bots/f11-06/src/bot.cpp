// Champion decisions. main.cpp dispatches here after the draft.
#include <cambc.h>
#include "bot.h"
#include <cmath>
#include <cstdio>
#include <span>
#include <variant>

using cambc::champion_type;
namespace vision = cambc::vision;

namespace {

constexpr int kEvalEvery = 8;

cambc::item_type next_item(champion_type t, int shards) {
    using it = cambc::item_type;
    const int e = shards / 5;
    if (t == champion_type::vesper_vale) {
        static const it vv[] = {it::treasure_shard_of_strength, it::jungle_shard_of_strength, it::monster_shard_of_strength};
        if (e < 3) return vv[e];
    } else {
        static const it base[] = {it::jungle_shard_of_strength, it::monster_shard_of_strength};
        if (e < 2) return base[e];
    }
    // Then two power-strength emblems for every power-health emblem.
    return (e % 3 == 2) ? it::power_shard_of_health : it::power_shard_of_strength;
}

bool dash_capable(champion_type t) {
    return t == champion_type::vesper_vale || t == champion_type::vendetta || t == champion_type::issac
        || t == champion_type::ren || t == champion_type::heart_of_ruin;
}

}  // namespace

// ---------------------------------------------------------------- setup

int Bot::body_slot() const {
    const int v = static_cast<int>(type_);
    if (type_ == champion_type::issac) return 5;
    const int as_draft = v <= 2 ? v : (v == 3 ? 3 : v - 1);
    for (size_t i = 0; i < plan_.picks.size(); ++i)
        if (static_cast<int>(plan_.picks[i]) == as_draft) return static_cast<int>(i);
    return 4;
}

void Bot::read_kit() {
    const auto abilities = cambc::self().abilities();
    for (int i = 0; i < 2; ++i) {
        const auto& opt = abilities[static_cast<size_t>(i)].info();
        if (!opt) continue;
        const auto& v = *opt;
        if (std::holds_alternative<cambc::ability_attack_reset>(v)) {
            kit_.reset = i;
        } else if (auto d = std::get_if<cambc::ability_damage>(&v)) {
            kit_.nuke = i;
            kit_.nuke_range = d->range;
            kit_.nuke_mod = d->modifier;
            kit_.nuke_excl = d->exclusive_with_movement;
            kit_.nuke_targets_camps = d->valid_targets.contains(cambc::ability_target_mask::camps);
        } else if (auto a = std::get_if<cambc::ability_aoe_damage>(&v)) {
            kit_.aoe = i;
            kit_.aoe_range = a->range;
            kit_.aoe_mod = a->modifier;
            kit_.aoe_radius = a->radius;
            kit_.aoe_excl = a->exclusive_with_movement;
        } else if (auto c = std::get_if<cambc::ability_cc>(&v)) {
            kit_.cc = i;
            kit_.cc_range = c->range;
            kit_.cc_mod = c->modifier;
            kit_.cc_excl = c->exclusive_with_movement;
            kit_.cc_targets_camps = c->valid_targets.contains(cambc::ability_target_mask::camps);
        } else if (auto h = std::get_if<cambc::ability_heal>(&v)) {
            kit_.heal = i;
            kit_.heal_range = h->range;
        } else if (auto ds = std::get_if<cambc::ability_dash>(&v)) {
            kit_.dash = i;
            kit_.dash_range = ds->range;
        } else if (std::holds_alternative<cambc::ability_shield>(v)) {
            kit_.shield = i;
        }
    }
}

void Bot::init() {
    auto& me = cambc::self();
    type_ = me.champion_name();
    const auto& m = map_info();
    plan_ = read_plan(m);
    slot_ = body_slot();
    gk_.assign(m.groups.size(), GroupKnow{});
    approach_.resize(m.groups.size());
    ranged_ = me.attack_range() > 1;
    tavi_ = me.passive() == cambc::passive_type::remote_shop;
    read_kit();
    last_hp_ = me.health();
    inited_ = true;
}

int Bot::home_dist(vec2 p) const {
    const auto& m = map_info();
    if (!in_bounds(p) || !m.home_reached.is_set(p)) return kFar;
    return m.home.get(p);
}

// ---------------------------------------------------------------- turn

void Bot::play_turn() {
    auto& me = cambc::self();
    round_ = static_cast<int>(cambc::round_number());
    if (!inited_) {
        init();
        if (cambc::fuel_remaining() < 70000) return;
    }
    if (!me.is_alive()) {
        at_spawn_ = false;
        gold_ = static_cast<int>(me.gold_count());
        shop();
        banking_ = 0;
        target_ = -1;
        return;
    }
    snapshot();
    update_groups();
    if (me.is_recalling() || me.is_submitting_gems() || me.is_teleporting()) {
        finish_turn();
        return;
    }
    shop();
    cast_support();
    // Spawn heals at the end of a turn spent on it: wait there until healed.
    if (at_spawn_ && hp_ < 0.97f * maxhp_ && enemies_.empty()) {
        finish_turn();
        return;
    }

    bool done = false;
    try {
        done = handle_enemies();
    } catch (const std::exception& e) {
        ++warns_;
        std::printf("warn: enemies %s\n", e.what());
    }
    if (!done && should_recall()) {
        me.recall();
        done = true;
    }
    if (!done) {
        try {
            done = bank();
        } catch (const std::exception& e) {
            ++warns_;
            std::printf("warn: bank %s\n", e.what());
        }
    }
    if (!done) {
        try {
            farm();
        } catch (const std::exception& e) {
            ++warns_;
            std::printf("warn: farm %s\n", e.what());
        }
    }
    finish_turn();
}

void Bot::snapshot() {
    auto& me = cambc::self();
    pos_ = me.position();
    hp_ = me.health();
    maxhp_ = std::max(1.0f, me.max_health());
    str_ = me.strength();
    gems_ = static_cast<int>(me.gem_count());
    gold_ = static_cast<int>(me.gold_count());
    range_ = me.attack_range();
    at_spawn_ = cambc::map::spawn_region(team::ally).is_set(pos_);
    damaged_ = !at_spawn_ && hp_ < last_hp_ - 0.5f;
    last_hp_ = hp_;
    moved_ = attacked_ = cast_ = move_failed_ = false;

    vision::visible_tiles_into(visible_);
    vision::champion_tiles_into(team::enemy, enemy_tiles_);
    vision::champion_tiles_into(team::ally, ally_tiles_);
    vision::camp_tiles_into(camp_tiles_);

    enemies_.clear();
    if (!enemy_tiles_.is_empty()) {
        for (auto p : enemy_tiles_.positions()) {
            try {
                const auto c = vision::champion_at(p);
                EnemyView e;
                e.pos = p;
                e.hp = c.health();
                e.maxhp = std::max(1.0f, c.max_health());
                e.str = c.strength() + 0.15f * c.focus();
                e.range = c.attack_range();
                e.type = c.champion_name();
                const auto st = c.state();
                e.channel = st.contains(cambc::champion_state::submitting_gems) || st.contains(cambc::champion_state::recalling);
                try {
                    e.gems = static_cast<int>(c.gem_count());
                } catch (...) {
                    e.gems = 0;
                }
                enemies_.push_back(e);
            } catch (...) {
            }
        }
        last_enemy_round_ = round_;
        last_enemy_pos_ = enemies_.empty() ? last_enemy_pos_ : enemies_.front().pos;
    }
    allies_.clear();
    for (auto p : ally_tiles_.positions()) {
        if (p == pos_) continue;
        try {
            const auto c = vision::champion_at(p);
            AllyView a;
            a.pos = p;
            a.hp = c.health();
            a.maxhp = std::max(1.0f, c.max_health());
            a.str = c.strength() + 0.15f * c.focus();
            a.range = c.attack_range();
            allies_.push_back(a);
        } catch (...) {
        }
    }
}

void Bot::update_groups() {
    const auto& m = map_info();
    const int n = static_cast<int>(m.groups.size());
    for (int g = 0; g < n; ++g) {
        const auto& G = m.groups[static_cast<size_t>(g)];
        if (cheb(G.center, pos_) > 12) continue;
        bool vis = false;
        for (auto t : G.tiles)
            if (visible_.is_set(t)) {
                vis = true;
                break;
            }
        if (!vis) continue;
        auto& K = gk_[static_cast<size_t>(g)];
        K.seen = round_;
        try {
            const auto st = m.raw[static_cast<size_t>(g)].state();
            if (std::holds_alternative<cambc::map::camp_group_alive>(st)) {
                K.ready = 0;
                K.reward = false;
            } else if (std::holds_alternative<cambc::map::camp_group_reward_available>(st)) {
                K.ready = 0;
                K.reward = true;
            } else if (auto r = std::get_if<cambc::map::camp_group_respawning>(&st)) {
                K.ready = round_ + r->timer;
                K.reward = false;
            }
        } catch (...) {
        }
        for (const auto& e : enemies_)
            if (cheb(e.pos, G.center) <= 5) K.enemy_round = round_;
    }
}

void Bot::shop() {
    auto& me = cambc::self();
    const bool alive = me.is_alive();
    if (alive && !at_spawn_ && !tavi_) return;
    int guard = 0;
    while (gold_ >= 30 && guard++ < 8) {
        const auto item = next_item(type_, shards_);
        const int left_in_emblem = 5 - shards_ % 5;
        const int count = std::min(left_in_emblem, gold_ / 30);
        if (count <= 0) break;
        if (!me.can_buy(item, static_cast<std::uint32_t>(count))) break;
        me.buy(item, static_cast<std::uint32_t>(count));
        shards_ += count;
        gold_ -= 30 * count;
    }
}

void Bot::finish_turn() {
    write_claim(target_);
    if (kTrace && (round_ % 25 == 0 || (slot_ == 2 && round_ > 100 && round_ < 125)))
        std::printf("T r%d s%d (%d,%d) hp%.0f/%.0f g%d gold%d str%.0f tgt%d rdy%d bank%d br%d mv%d fuel%llu w%d\n", round_, slot_, pos_.x, pos_.y, hp_, maxhp_, gems_, gold_, str_,
            target_, target_ >= 0 ? gk_[static_cast<size_t>(target_)].ready - round_ : -1, banking_, branch_, moved_ ? 1 : (move_failed_ ? 2 : 0),
            static_cast<unsigned long long>(cambc::fuel_remaining()), warns_);
    if (kTrace && round_ % 25 == 0 && target_ >= 0) {
        const auto& G = map_info().groups[static_cast<size_t>(target_)];
        const auto& ap = approach_tiles(target_);
        std::printf("   G%d n%zu t0(%d,%d) hp%.0f dps%.0f gems%d dA%d dE%d appr%u onap%d vis%d camps%d\n", target_, G.tiles.size(), G.tiles[0].x, G.tiles[0].y, G.hp, G.dps, G.gems, G.dA, G.dE,
            ap.count(), ap.is_set(pos_) ? 1 : 0, visible_.is_set(G.tiles[0]) ? 1 : 0, camp_tiles_.is_set(G.tiles[0]) ? 1 : 0);
    }
    if (!enemies_.empty()) {
        try {
            cambc::shared_memory[sm::alert] = static_cast<std::uint8_t>(enemies_.front().pos.x);
            cambc::shared_memory[sm::alert + 1] = static_cast<std::uint8_t>(enemies_.front().pos.y);
        } catch (...) {
        }
    }
}

void Bot::write_claim(int g) {
    if (slot_ < 0 || slot_ > 5) return;
    try {
        cambc::shared_memory[static_cast<size_t>(sm::claims + slot_)] = static_cast<std::uint8_t>(g < 0 ? 0 : (g + 1) & 255);
    } catch (...) {
    }
}

std::array<int, 6> Bot::read_claims() const {
    std::array<int, 6> out{};
    std::array<std::uint8_t, 6> raw{};
    try {
        cambc::shared_memory.read(sm::claims, std::span<std::uint8_t>{raw});
    } catch (...) {
    }
    for (int i = 0; i < 6; ++i) out[static_cast<size_t>(i)] = static_cast<int>(raw[static_cast<size_t>(i)]) - 1;
    return out;
}

// ---------------------------------------------------------------- movement

bitboard Bot::blocked_now() const {
    // Camps never move: an unseen camp tile counts as occupied, so routes do not flip with vision.
    bitboard b = visible_ & ~camp_tiles_;
    b = map_info().camp_tiles & ~b;
    b |= static_walls();
    b |= enemy_tiles_;
    b |= ally_tiles_;
    b |= vision::teleport_tiles();
    b.set(pos_, false);
    return b;
}

bool Bot::step_towards(bitboard goals, bool allow_dash) {
    auto& me = cambc::self();
    if (moved_ || move_failed_ || !me.can_move()) return false;
    const auto blocked = blocked_now();
    goals &= ~blocked;
    goals.set(pos_, false);
    if (goals.is_empty()) return false;

    cambc::distance_field field;
    bitboard visited, roots;
    roots.clear();
    roots.set(pos_);
    field.compute_to(blocked, goals, roots, visited);
    if (!visited.is_set(pos_)) return false;
    const int d0 = field.get(pos_);
    if (d0 <= 0) return false;

    auto free_step = [&](vec2 t) { return in_bounds(t) && !blocked.is_set(t) && visited.is_set(t); };

    // Dash two tiles along the field when the walk is long.
    if (allow_dash && kit_.dash >= 0 && d0 >= 4 && !cast_) {
        const auto ab = me.abilities()[static_cast<size_t>(kit_.dash)];
        if (ab.is_ready()) {
            for (int dy = -1; dy <= 1 && !moved_; ++dy)
                for (int dx = -1; dx <= 1 && !moved_; ++dx) {
                    const vec2 a{pos_.x + dx, pos_.y + dy};
                    if ((dx == 0 && dy == 0) || !free_step(a) || field.get(a) != d0 - 1) continue;
                    for (int ey = -1; ey <= 1 && !moved_; ++ey)
                        for (int ex = -1; ex <= 1 && !moved_; ++ex) {
                            const vec2 b{a.x + ex, a.y + ey};
                            if ((ex == 0 && ey == 0) || !free_step(b) || field.get(b) != d0 - 2) continue;
                            if (cheb(b, pos_) != 2) continue;
                            try {
                                if (ab.use(b)) {
                                    cast_ = true;
                                    pos_ = b;
                                } else {
                                    move_failed_ = true;
                                }
                            } catch (...) {
                                ++warns_;
                            }
                            goto dashed;
                        }
                }
        }
    }
dashed:
    if (move_failed_ || !me.can_move()) return pos_ != me.position() || cast_;
    {
        const int dcur = field.get(pos_);
        vec2 best = pos_;
        int bestd = dcur;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                const vec2 t{pos_.x + dx, pos_.y + dy};
                if ((dx == 0 && dy == 0) || !free_step(t)) continue;
                const int d = field.get(t);
                if (d < bestd) {
                    bestd = d;
                    best = t;
                }
            }
        if (best == pos_) return cast_;
        if (me.move_absolute(best)) {
            moved_ = true;
            pos_ = best;
            return true;
        }
        move_failed_ = true;
        return cast_;
    }
}

// Tiles from which this champion can hit some camp of group g (static terrain only).
const bitboard& Bot::approach_tiles(int g) {
    auto& slot = approach_[static_cast<size_t>(g)];
    if (slot) return *slot;
    const auto& G = map_info().groups[static_cast<size_t>(g)];
    auto b = std::make_unique<bitboard>();
    b->clear();
    const auto& bushes = cambc::map::bushes();
    const bitboard opaque = cambc::map::walls() | bushes;
    for (auto c : G.tiles) {
        if (!ranged_ || bushes.is_set(c)) {
            mark_square(*b, c, 1);
            if (bushes.is_set(c)) {
                bitboard near;
                near.clear();
                mark_square(near, c, range_);
                *b |= near & bushes;
            }
        } else {
            *b |= opaque.shadow_cast(c, static_cast<std::uint16_t>(range_));
        }
    }
    for (auto c : G.tiles) b->set(c, false);
    *b &= ~static_walls();
    slot = std::move(b);
    return *slot;
}

// ---------------------------------------------------------------- abilities

float Bot::my_camp_dps() const {
    float s = str_;
    if (kit_.reset >= 0) s *= 1.12f;
    if (shards_ >= 10) s *= 1.1f;   // monster emblem
    if (kit_.nuke >= 0 && kit_.nuke_targets_camps) s += kit_.nuke_mod * cambc::self().focus() / 8.0f;
    return std::max(10.0f, s);
}

bool Bot::use_reset_and_attack(const cambc::unit& first) {
    (void)first;
    auto& me = cambc::self();
    if (kit_.reset < 0 || cast_ || !attacked_) return false;
    const auto ab = me.abilities()[static_cast<size_t>(kit_.reset)];
    if (!ab.is_ready()) return false;
    try {
        if (!ab.use()) return false;
        cast_ = true;
    } catch (...) {
        ++warns_;
        return false;
    }
    return true;
}

bool Bot::cast_on_camp(const cambc::camp& c, bool will_move) {
    auto& me = cambc::self();
    if (cast_) return false;
    const vec2 cp = c.position();
    const int d = cheb(cp, pos_);
    auto try_cast = [&](int slot, bool excl, float range, bool unit_target) {
        if (slot < 0) return false;
        if (excl && (moved_ || will_move)) return false;
        if (d > static_cast<int>(range)) return false;
        const auto ab = me.abilities()[static_cast<size_t>(slot)];
        if (!ab.is_ready()) return false;
        try {
            const bool ok = unit_target ? ab.use(c) : ab.use(cp);
            if (ok) cast_ = true;
            return ok;
        } catch (...) {
            ++warns_;
            return false;
        }
    };
    if (kit_.nuke_targets_camps && try_cast(kit_.nuke, kit_.nuke_excl, kit_.nuke_range, true)) return true;
    if (kit_.cc_targets_camps && try_cast(kit_.cc, kit_.cc_excl, kit_.cc_range, true)) return true;
    if (try_cast(kit_.aoe, kit_.aoe_excl, kit_.aoe_range, false)) return true;
    return false;
}

void Bot::cast_support() {
    auto& me = cambc::self();
    if (cast_) return;
    try {
        if (kit_.heal >= 0) {
            const auto ab = me.abilities()[static_cast<size_t>(kit_.heal)];
            if (ab.is_ready()) {
                float best = 120.0f;
                vec2 who{-1, -1};
                for (const auto& a : allies_) {
                    if (cheb(a.pos, pos_) > static_cast<int>(kit_.heal_range)) continue;
                    const float miss = a.maxhp - a.hp;
                    if (miss > best) {
                        best = miss;
                        who = a.pos;
                    }
                }
                if (who.x >= 0) {
                    const auto target = vision::champion_at(who);
                    if (ab.use(target)) cast_ = true;
                }
            }
        }
        if (!cast_ && kit_.shield >= 0 && damaged_ && hp_ < 0.85f * maxhp_) {
            const auto ab = me.abilities()[static_cast<size_t>(kit_.shield)];
            if (ab.is_ready() && ab.use()) cast_ = true;
        }
    } catch (...) {
        ++warns_;
    }
}

// ---------------------------------------------------------------- enemies

float Bot::reach_of(const EnemyView& e) const {
    float r = static_cast<float>(e.range) + (e.range <= 1 ? 1.0f : 0.0f);
    if (dash_capable(e.type)) r += 2.0f;
    return r;
}

bool Bot::handle_enemies() {
    if (enemies_.empty()) return false;
    auto& me = cambc::self();

    // Forces near us: our visible allies within 6, enemies within 9.
    float us_hp = hp_, us_dps = str_ * (kit_.reset >= 0 ? 1.12f : 1.0f);
    for (const auto& a : allies_)
        if (cheb(a.pos, pos_) <= 6) {
            us_hp += a.hp;
            us_dps += a.str;
        }
    float them_hp = 0, them_dps = 0;
    std::vector<vec2> threats;
    int close = 0;
    for (const auto& e : enemies_) {
        const int d = cheb(e.pos, pos_);
        if (d > 9) continue;
        ++close;
        them_hp += e.hp;
        if (!e.channel) them_dps += e.str;
        if (static_cast<float>(d) <= reach_of(e) + 1.0f) threats.push_back(e.pos);
    }
    if (close == 0) return false;
    const float ratio = (us_hp * us_dps) / std::max(1.0f, them_hp * std::max(5.0f, them_dps));
    const bool engage = ratio >= 1.25f;

    if (!engage) {
        if (threats.empty()) return false;
        return retreat_from(threats);
    }

    // Pick the enemy we kill fastest, preferring those already in range.
    const EnemyView* best = nullptr;
    float best_score = 1e9f;
    for (const auto& e : enemies_) {
        const int d = cheb(e.pos, pos_);
        if (d > 10) continue;
        float s = e.hp / std::max(10.0f, str_) + 1.5f * static_cast<float>(std::max(0, d - range_));
        if (e.channel) s -= 3.0f;
        s -= 0.02f * static_cast<float>(e.gems);
        if (s < best_score) {
            best_score = s;
            best = &e;
        }
    }
    if (!best) return false;
    const vec2 tp = best->pos;
    const int d = cheb(tp, pos_);

    auto try_hit = [&]() {
        if (attacked_ || !me.can_attack()) return false;
        if (cheb(tp, pos_) > range_) return false;
        try {
            const auto target = vision::champion_at(tp);
            // Spells first: stuns and nukes on the target we focus.
            if (!cast_ && !moved_) {
                for (int slot : {kit_.cc, kit_.nuke}) {
                    if (slot < 0 || cast_) continue;
                    const float r = slot == kit_.cc ? kit_.cc_range : kit_.nuke_range;
                    if (static_cast<float>(cheb(tp, pos_)) > r) continue;
                    const auto ab = me.abilities()[static_cast<size_t>(slot)];
                    if (ab.is_ready() && ab.use(target)) cast_ = true;
                }
            }
            if (!vision::has_champion(tp)) return true;
            me.attack(target);
            attacked_ = true;
            if (!cast_ && kit_.reset >= 0 && vision::has_champion(tp)) {
                const auto ab = me.abilities()[static_cast<size_t>(kit_.reset)];
                if (ab.is_ready() && ab.use()) {
                    cast_ = true;
                    if (vision::has_champion(tp) && me.can_attack()) me.attack(vision::champion_at(tp));
                }
            }
        } catch (...) {
            ++warns_;
        }
        return true;
    };

    if (d <= range_) {
        try_hit();
        return true;
    }
    // Close in (ranged champions stop at their range).
    bitboard goals;
    goals.clear();
    mark_square(goals, tp, ranged_ ? range_ : 1);
    step_towards(goals, true);
    if (!ranged_) try_hit();
    return true;
}

bool Bot::retreat_from(const std::vector<vec2>& threats) {
    auto& me = cambc::self();
    if (!me.can_move()) return false;
    const auto blocked = blocked_now();
    auto score = [&](vec2 t) {
        float s = 0;
        for (const auto& e : enemies_) {
            const int d = cheb(t, e.pos);
            const float gap = static_cast<float>(d) - reach_of(e);
            if (gap < 1.0f) s += (gap - 1.0f) * 10.0f;
        }
        const int hd = home_dist(t);
        s -= 0.6f * static_cast<float>(std::min(hd, 200));
        return s;
    };
    (void)threats;
    vec2 best = pos_;
    float best_s = score(pos_);
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            const vec2 t{pos_.x + dx, pos_.y + dy};
            if ((dx == 0 && dy == 0) || !in_bounds(t) || blocked.is_set(t)) continue;
            const float s = score(t);
            if (s > best_s) {
                best_s = s;
                best = t;
            }
        }
    // A dash of two tiles when the walk alone stays in reach.
    if (kit_.dash >= 0 && !cast_ && best_s < -5.0f) {
        const auto ab = me.abilities()[static_cast<size_t>(kit_.dash)];
        if (ab.is_ready()) {
            vec2 dbest = pos_;
            float ds = best_s;
            for (int dy = -2; dy <= 2; ++dy)
                for (int dx = -2; dx <= 2; ++dx) {
                    const vec2 t{pos_.x + dx, pos_.y + dy};
                    if (std::max(std::abs(dx), std::abs(dy)) != 2 || !in_bounds(t) || blocked.is_set(t)) continue;
                    const float s = score(t);
                    if (s > ds + 4.0f) {
                        ds = s;
                        dbest = t;
                    }
                }
            if (dbest != pos_) {
                try {
                    if (ab.use(dbest)) {
                        cast_ = true;
                        pos_ = dbest;
                        return true;
                    }
                } catch (...) {
                    ++warns_;
                }
            }
        }
    }
    if (best == pos_) return false;
    if (me.move_absolute(best)) {
        moved_ = true;
        pos_ = best;
        return true;
    }
    move_failed_ = true;
    return false;
}

// ---------------------------------------------------------------- recall & bank

bool Bot::should_recall() {
    if (at_spawn_ || damaged_) return false;
    for (const auto& e : enemies_)
        if (cheb(e.pos, pos_) <= 8) return false;
    if (hp_ < 0.3f * maxhp_) return true;
    if (!tavi_ && gold_ >= 240 && hp_ < 0.7f * maxhp_ && home_dist(pos_) > 12) return true;
    return false;
}

bool Bot::bank() {
    auto& me = cambc::self();
    if (gems_ <= 0) {
        banking_ = 0;
        return false;
    }
    const auto& m = map_info();
    // Capturing a group comes first.
    if (target_ >= 0 && gk_[static_cast<size_t>(target_)].reward) {
        for (auto t : m.groups[static_cast<size_t>(target_)].tiles)
            if (t == pos_) return false;
    }
    int near_altar = kFar;
    for (auto a : m.altars) near_altar = std::min(near_altar, cheb(a, pos_));
    const int submitted = static_cast<int>(cambc::global_submitted_gems(team::ally));
    bool need = banking_ != 0 || gems_ >= plan_.bank || submitted + gems_ >= 1000
        || (gems_ >= 8 && near_altar <= 4) || (round_ >= 1930 - near_altar)
        || (gems_ >= 15 && round_ - last_enemy_round_ < 20);
    if (!need) return false;
    banking_ = 1;

    bitboard goals;
    goals.clear();
    for (auto a : m.altars) {
        bool enemy_lock = false;
        try {
            if (vision::has_altar_lock(a)) enemy_lock = vision::altar_lock_at(a).is_enemy();
        } catch (...) {
        }
        if (enemy_lock) {
            if (cheb(a, pos_) <= range_ && me.can_attack()) {
                try {
                    me.attack(vision::altar_lock_at(a));
                    attacked_ = true;
                    return true;
                } catch (...) {
                    ++warns_;
                }
            }
            continue;
        }
        if (cheb(a, pos_) <= cambc::constants::submit_gems_range) {
            try {
                me.submit_gems(a);
                return true;
            } catch (...) {
                ++warns_;
            }
        }
        mark_square(goals, a, cambc::constants::submit_gems_range);
    }
    if (goals.is_empty()) {
        // Every altar we know is locked by the enemy: go and break the nearest lock.
        for (auto a : m.altars) mark_square(goals, a, range_);
    }
    step_towards(goals, true);
    return true;
}

// ---------------------------------------------------------------- farming

int Bot::group_value(int g) const {
    const auto& G = map_info().groups[static_cast<size_t>(g)];
    const int submitted = static_cast<int>(cambc::global_submitted_gems(team::ally));
    float goldw = round_ < 700 ? 0.4f : (round_ < 1200 ? 0.25f : 0.1f);
    if (submitted + gems_ >= 800) goldw = 0.05f;
    return static_cast<int>(static_cast<float>(G.gems) + goldw * static_cast<float>(G.gold));
}

void Bot::choose_target() {
    const auto& m = map_info();
    last_eval_ = round_;
    cambc::distance_field field;
    bitboard visited, roots;
    roots.clear();
    roots.set(pos_);
    field.compute(static_walls(), roots, visited);
    const auto claims = read_claims();
    const float dps = my_camp_dps();
    const float camp_mult = shards_ >= 5 ? 0.7f : 1.0f;   // jungle emblem first

    int best = -1;
    float best_score = 0;
    bool best_needs_recall = false;
    const int n = static_cast<int>(m.groups.size());
    for (int g = 0; g < n; ++g) {
        const auto& G = m.groups[static_cast<size_t>(g)];
        const auto cls = plan_.cls[static_cast<size_t>(g)];
        if (cls == kGUnreachable) continue;
        const auto& K = gk_[static_cast<size_t>(g)];
        int d = kFar;
        for (auto t : G.tiles)
            if (visited.is_set(t)) d = std::min(d, static_cast<int>(field.get(t)));
        if (d >= kFar) continue;
        const int wait = std::max(0, K.ready - round_ - d);
        if (wait > 40) continue;

        int claimers = 0;
        for (int s = 0; s < 6; ++s)
            if (s != slot_ && claims[static_cast<size_t>(s)] == g) ++claimers;

        const float hp_left = K.reward ? 0.0f : G.hp;
        const float solo_clear = hp_left / dps;
        const float solo_dmg = G.dps * solo_clear * 0.55f * camp_mult;
        const bool solo_ok = solo_dmg < hp_ - 0.12f * maxhp_;
        if (claimers > 0 && (solo_ok || K.reward)) continue;   // an ally has it covered
        const int team_n = 1 + claimers;
        const float clear = hp_left / (dps * static_cast<float>(team_n));
        const float dmg = G.dps * clear * 0.55f * camp_mult;
        bool needs_recall = false;
        if (dmg >= hp_ - 0.12f * maxhp_) {
            if (dmg >= 0.85f * maxhp_) continue;
            needs_recall = true;
        }
        float value = static_cast<float>(group_value(g)) / static_cast<float>(team_n);
        if (cls == kGEnemy) value *= 0.6f;
        else if (cls == kGContested) value *= 0.85f;
        if (round_ - K.enemy_round < 30) value *= 0.5f;
        float time = static_cast<float>(needs_recall ? 6 + G.dA : d) + static_cast<float>(wait) + clear + 5.0f;
        float score = value / (time + 3.0f);
        if (g == target_) score *= 1.15f;
        if (score > best_score) {
            best_score = score;
            best = g;
            best_needs_recall = needs_recall;
        }
    }
    if (best != target_) {
        target_since_ = round_;
        stall_ = 0;
        shrink_ = false;
    }
    target_ = best;
    if (best >= 0 && best_needs_recall && !at_spawn_) {
        bool enemy_near = false;
        for (const auto& e : enemies_)
            if (cheb(e.pos, pos_) <= 8) enemy_near = true;
        if (!enemy_near && !damaged_) {
            try {
                cambc::self().recall();
                moved_ = true;
            } catch (...) {
                ++warns_;
            }
        }
    }
}

bool Bot::attack_camps_in_range(int g) {
    auto& me = cambc::self();
    if (attacked_ || !me.can_attack()) return false;
    const auto& G = map_info().groups[static_cast<size_t>(g)];
    // Best camp in range: most damage stopped per health first, clusters last.
    vec2 best{-1, -1};
    float best_pri = -1;
    auto scan = [&](const std::vector<vec2>& tiles) {
        for (auto t : tiles) {
            if (!camp_tiles_.is_set(t) || cheb(t, pos_) > range_) continue;
            try {
                const auto c = vision::camp_at(t);
                const float h = std::max(1.0f, c.health());
                const float pri = (camp_spec(c.kind()).str + 1.0f) / h;
                if (pri > best_pri) {
                    best_pri = pri;
                    best = t;
                }
            } catch (...) {
            }
        }
    };
    scan(G.tiles);
    if (best.x < 0) {
        // Other camps in range (an awake neighbour group, for instance).
        std::vector<vec2> others;
        bitboard near;
        near.clear();
        mark_square(near, pos_, range_);
        near &= camp_tiles_;
        if (!near.is_empty()) others = near.positions();
        // Only hit camps of groups that are awake or already ours to clear.
        std::vector<vec2> awake;
        for (auto t : others) {
            try {
                if (vision::camp_at(t).is_awake()) awake.push_back(t);
            } catch (...) {
            }
        }
        scan(awake);
    }
    if (best.x < 0) return false;
    try {
        const auto c = vision::camp_at(best);
        cast_on_camp(c, false);
        if (!vision::has_camp(best)) return true;
        me.attack(vision::camp_at(best));
        attacked_ = true;
        if (use_reset_and_attack(c)) {
            // Re-pick: the first target may have died.
            vec2 again{-1, -1};
            for (auto t : G.tiles)
                if (camp_tiles_.is_set(t) && cheb(t, pos_) <= range_ && vision::has_camp(t)) {
                    again = t;
                    break;
                }
            if (again.x >= 0 && me.can_attack()) me.attack(vision::camp_at(again));
        }
    } catch (const std::exception& e) {
        ++warns_;
        std::printf("warn: attack %s\n", e.what());
    }
    return true;
}

bool Bot::execute_target(int g) {
    auto& me = cambc::self();
    const auto& m = map_info();
    const auto& G = m.groups[static_cast<size_t>(g)];
    auto& K = gk_[static_cast<size_t>(g)];

    bool any_vis = false, any_alive = false;
    for (auto t : G.tiles) {
        if (visible_.is_set(t)) any_vis = true;
        if (camp_tiles_.is_set(t)) any_alive = true;
    }

    if (any_vis && K.ready > round_) {
        // Respawning: wait only if it is back soon.
        if (K.ready - round_ > 6) return false;
    }

    branch_ = 1;
    if (any_alive) {
        // Low on health while the group fights back: step out and recall later.
        if (damaged_ && hp_ < 0.25f * maxhp_) {
            bitboard goals;
            goals.clear();
            for (auto t : G.tiles) mark_square(goals, t, G.range + 2);
            goals = ~goals;
            step_towards(goals, true);
            return true;
        }
        if (attack_camps_in_range(g)) return true;
        bitboard goals = approach_tiles(g);
        if (shrink_ && stall_group_ == g) {
            goals.clear();
            for (auto t : G.tiles)
                if (camp_tiles_.is_set(t)) mark_square(goals, t, 1);
        }
        if (goals.is_set(pos_) && !shrink_) {
            if (++stall_ >= 2) {
                shrink_ = true;
                stall_group_ = g;
            }
        }
        step_towards(goals, true);
        if (!ranged_) attack_camps_in_range(g);
        return true;
    }

    branch_ = 2;
    if (any_vis && K.reward) {
        // Capture: hold a free spawn tile of the group.
        for (auto t : G.tiles)
            if (t == pos_) return true;
        for (auto t : G.tiles)
            if (ally_tiles_.is_set(t)) {
                // An ally captures it already.
                K.ready = round_ + G.respawn + 5;
                K.reward = false;
                return false;
            }
        bitboard goals;
        goals.clear();
        for (auto t : G.tiles) goals.set(t);
        step_towards(goals, true);
        return true;
    }

    branch_ = 3;
    if (any_vis && !K.reward && K.ready <= round_) {
        // Alive per state but no camp visible (bush): get closer.
        bitboard goals;
        goals.clear();
        for (auto t : G.tiles) mark_square(goals, t, 1);
        step_towards(goals, true);
        if (!ranged_) attack_camps_in_range(g);
        return true;
    }

    branch_ = 4;
    // Not in vision yet: walk to the approach tiles.
    (void)me;
    step_towards(approach_tiles(g), true);
    return true;
}

void Bot::idle() {
    // Nothing worth doing: wait at home (heals, shops).
    if (at_spawn_) return;
    step_towards(cambc::map::spawn_region(team::ally), true);
}

void Bot::farm() {
    const bool invalid = target_ < 0 || gk_[static_cast<size_t>(target_)].ready > round_ + 6;
    if (invalid || round_ - last_eval_ >= kEvalEvery) {
        // Never abandon a capture in progress.
        bool holding = false;
        if (target_ >= 0 && gk_[static_cast<size_t>(target_)].reward)
            for (auto t : map_info().groups[static_cast<size_t>(target_)].tiles)
                if (t == pos_) holding = true;
        if (!holding) choose_target();
        if (moved_) return;   // recall started
    }
    if (target_ < 0) {
        idle();
        return;
    }
    if (!execute_target(target_)) {
        choose_target();
        if (moved_) return;
        if (target_ >= 0) execute_target(target_);
        else idle();
    }
}
