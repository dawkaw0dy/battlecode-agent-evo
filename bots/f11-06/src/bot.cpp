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

#ifndef EVO_EVAL
#define EVO_EVAL 8
#endif
#ifndef EVO_TCONST
#define EVO_TCONST 3
#endif
constexpr int kEvalEvery = EVO_EVAL;
constexpr float kTimeConst = EVO_TCONST;
constexpr int kShopGold = 250;
constexpr int kNavSpawn = 5000, kNavAltar = 6000;   // cached-route keys (groups use their index)
constexpr std::uint16_t kChooseRadius = 70;   // target search radius in walking steps
#ifndef EVO_NAV_CACHE
#define EVO_NAV_CACHE 1
#endif
constexpr bool kNavCache = EVO_NAV_CACHE;
constexpr int kNavCacheArea = 6400;   // cached routes only on big maps (fuel); elsewhere exact searches
constexpr int kMaxExtraTrips = 3;
#ifndef EVO_BANK_WARY
#define EVO_BANK_WARY 15
#endif
constexpr int kBankWary = EVO_BANK_WARY;   // bank this many gems once enemies were seen lately
#ifndef EVO_ALTAR_DANGER
#define EVO_ALTAR_DANGER 15
#endif
constexpr int kAltarDanger = EVO_ALTAR_DANGER;   // walking-tile penalty for an altar enemies were seen at
#ifndef EVO_LOCKS
#define EVO_LOCKS 1
#endif
constexpr bool kLocks = EVO_LOCKS;   // carry an altar lock and place it on altars we pass
#ifndef EVO_BUDDY
#define EVO_BUDDY 0.0f
#endif
constexpr float kBuddy = EVO_BUDDY;   // value bonus for groups near an ally's target
#ifndef EVO_RACE_ENEMY
#define EVO_RACE_ENEMY 800
#endif
constexpr int kRaceEnemy = EVO_RACE_ENEMY, kRaceOurs = 750, kRaceBag = 5;   // bank everything late in the race
#ifndef EVO_ENEMY_SEEN
#define EVO_ENEMY_SEEN 0.8f
#endif
constexpr float kEnemySeenFactor = EVO_ENEMY_SEEN;   // value of a group enemies were seen at lately
#ifndef EVO_OTHER_SECTOR
#define EVO_OTHER_SECTOR 0.5f
#endif
constexpr float kOtherSector = EVO_OTHER_SECTOR, kSharedSector = 0.75f;   // value factors outside our sector
#ifndef EVO_FLEE_PERIOD
#define EVO_FLEE_PERIOD 0
#endif
constexpr bool kFleePeriod = EVO_FLEE_PERIOD;   // walk home for a few turns after every flee

// Shard build per emblem (5 shards each): jungle first (camps hit 30% softer), then health and the
// camp-damage emblem; Vesper Vale opens with treasure (gold compounds). Afterwards power shards.
cambc::item_type build_item(champion_type t, int emblem) {
    using it = cambc::item_type;
    static const it base[] = {it::jungle_shard_of_strength, it::power_shard_of_health, it::monster_shard_of_strength,
                              it::power_shard_of_strength, it::durability_shard_of_health};
    static const it vv[] = {it::treasure_shard_of_strength, it::jungle_shard_of_strength, it::power_shard_of_health,
                            it::monster_shard_of_strength, it::power_shard_of_strength, it::durability_shard_of_health};
    if (t == champion_type::vesper_vale) {
        if (emblem < 6) return vv[emblem];
    } else if (emblem < 5) {
        return base[emblem];
    }
    return (emblem % 2 == 0) ? it::power_shard_of_strength : it::power_shard_of_health;
}

cambc::item_type next_item(champion_type t, int shards) { return build_item(t, shards / 5); }

bool has_emblem(champion_type t, int shards, cambc::item_type shard) {
    for (int e = 0; e < shards / 5; ++e)
        if (build_item(t, e) == shard) return true;
    return false;
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
    altar_locked_.assign(m.altars.size(), false);
    altar_enemy_.assign(m.altars.size(), -1000);
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
        act_ = kActDead;
        if (slot_ >= 0 && slot_ < sm::kSlots) {
            try {
                cambc::shared_memory[static_cast<size_t>(sm::claims + slot_)] = sm::kDead;
            } catch (...) {
            }
        }
        if (kTrace) ++acts_[kActDead];
        at_spawn_ = false;
        try {
            gold_ = static_cast<int>(me.gold_count());
            shop();
        } catch (const std::exception& e) {
            ++warns_;
            std::printf("warn: dead shop %s\n", e.what());
        }
        banking_ = 0;
        target_ = -1;
        return;
    }
    fuel_mark_[0] = cambc::fuel_remaining();
    snapshot();
    fuel_mark_[1] = cambc::fuel_remaining();
    update_groups();
    update_squad();
    fuel_mark_[2] = cambc::fuel_remaining();
    act_ = kActIdle;
    // Spawn heals only the champion whose recall lands this turn (it must end the turn there).
    just_recalled_ = was_recalling_ && !me.is_recalling();
    was_recalling_ = me.is_recalling();
    if (me.is_recalling() || me.is_submitting_gems() || me.is_teleporting()) {
        act_ = kActChannel;
        finish_turn();
        return;
    }
    {
        // Stunned: nothing can act this turn (actions would throw "cannot act").
        const auto st = me.state();
        if (!st.contains(cambc::champion_state::can_move) && !st.contains(cambc::champion_state::can_attack)
            && !st.contains(cambc::champion_state::can_cast)) {
            finish_turn();
            return;
        }
    }
    try {
        shop();
        cast_support();
        place_lock();
    } catch (const std::exception& e) {
        ++warns_;
        std::printf("warn: shop %s\n", e.what());
    }
    // Spawn heals at the end of a turn spent on it: wait there until healed.
    if (at_spawn_ && just_recalled_ && hp_ < 0.97f * maxhp_ && enemies_.empty()) {
        act_ = kActHeal;
        if (kTrace) std::printf("H r%d s%d (%d,%d) hp%.0f/%.0f\n", round_, slot_, pos_.x, pos_.y, hp_, maxhp_);
        finish_turn();
        return;
    }

    bool done = false;
    fuel_mark_[3] = cambc::fuel_remaining();
    try {
        done = deny_tribute();
    } catch (const std::exception& e) {
        ++warns_;
        std::printf("warn: deny %s\n", e.what());
    }
    try {
        if (!done) done = handle_enemies();
    } catch (const std::exception& e) {
        ++warns_;
        std::printf("warn: enemies %s\n", e.what());
    }
    if (kFleePeriod && !done && round_ < flee_until_) {
        // Just escaped a stronger force: keep going home (heal there) instead of walking back into it.
        act_ = kActFlee;
        try {
            if (!at_spawn_) step_towards(cambc::map::spawn_region(team::ally), true, kNavSpawn);
        } catch (...) {
            ++warns_;
        }
        done = true;
    }
    if (!done && should_recall()) {
        act_ = kActRecall;
        try {
            me.recall();
            done = true;
        } catch (const std::exception& e) {
            ++warns_;
            std::printf("warn: recall %s\n", e.what());
        }
    }
    fuel_mark_[4] = cambc::fuel_remaining();
    if (!done) {
        try {
            done = bank();
        } catch (const std::exception& e) {
            ++warns_;
            std::printf("warn: bank %s\n", e.what());
        }
    }
    fuel_mark_[5] = cambc::fuel_remaining();
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
    dmg_taken_ = damaged_ ? last_hp_ - hp_ : 0.0f;
    last_hp_ = hp_;
    moved_ = attacked_ = cast_ = move_failed_ = false;

    champion_power(type_, str_, me.focus(), &my_dps_, &my_burst_);
    my_burst_ = 0;
    for (int slot : {kit_.nuke, kit_.cc, kit_.aoe}) {
        if (slot < 0) continue;
        try {
            const auto ab = me.abilities()[static_cast<size_t>(slot)];
            if (ab.is_ready()) my_burst_ += ab.damage_amount();
        } catch (...) {
        }
    }
    vision::visible_tiles_into(visible_);
    vision::champion_tiles_into(team::enemy, enemy_tiles_);
    {
        const auto& alt = map_info().altars;
        if (!enemy_tiles_.is_empty())
            for (auto p : enemy_tiles_.positions())
                for (size_t i = 0; i < alt.size(); ++i)
                    if (cheb(p, alt[i]) <= 7) altar_enemy_[i] = round_;
        for (size_t i = 0; i < alt.size(); ++i) {
            if (!visible_.is_set(alt[i])) continue;
            bool lk = false;
            try {
                lk = vision::has_altar_lock(alt[i]) && vision::altar_lock_at(alt[i]).is_enemy();
            } catch (...) {
            }
            altar_locked_[i] = lk;
        }
    }
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
                e.str = c.strength();
                e.range = c.attack_range();
                e.type = c.champion_name();
                e.dasher = dash_capable(e.type);
                champion_power(e.type, e.str, c.focus(), &e.dps, &e.burst);
                const auto st = c.state();
                e.tribute = st.contains(cambc::champion_state::submitting_gems);
                e.channel = e.tribute || st.contains(cambc::champion_state::recalling);
                try {
                    e.gems = static_cast<int>(c.gem_count());
                } catch (...) {
                    e.gems = 0;
                }
                enemies_.push_back(e);
            } catch (const std::exception& ex) {
                if (kTrace) std::printf("warn: enemy view %s\n", ex.what());
            }
        }
        last_enemy_round_ = round_;
        last_enemy_pos_ = enemies_.empty() ? last_enemy_pos_ : enemies_.front().pos;
    }
    // Short enemy memory: a champion that steps out of sight for a turn is still there.
    threats_ = enemies_;
    for (const auto& e : enemies_) {
        const auto k = static_cast<size_t>(e.type) & 15;
        mem_[k] = e;
        mem_round_[k] = round_;
    }
    if (round_ > 2)
        for (size_t k = 0; k < mem_.size(); ++k) {
            if (mem_round_[k] <= 0 || mem_round_[k] == round_ || round_ - mem_round_[k] > 2) continue;
            if (cheb(mem_[k].pos, pos_) > 12) continue;
            threats_.push_back(mem_[k]);
        }
    awake_.clear();
    {
        bitboard aw;
        vision::awake_camp_tiles_into(aw);
        if (!aw.is_empty())
            for (auto p : aw.positions()) {
                if (cheb(p, pos_) > 9) continue;
                try {
                    const auto c = vision::camp_at(p);
                    const auto spec = camp_spec(c.kind());
                    if (spec.str <= 0) continue;
                    awake_.push_back({p, spec.str, c.health(), spec.range});
                } catch (...) {
                }
            }
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
            a.str = c.strength();
            a.range = c.attack_range();
            float burst = 0;
            champion_power(c.champion_name(), a.str, c.focus(), &a.dps, &burst);
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
                float left = 0;
                bool all_seen = true;
                for (auto t : G.tiles) {
                    if (!visible_.is_set(t)) {
                        all_seen = false;
                        continue;
                    }
                    if (!camp_tiles_.is_set(t)) continue;
                    try {
                        left += vision::camp_at(t).health();
                    } catch (...) {
                        all_seen = false;
                    }
                }
                if (all_seen) K.hp_seen = left;
            } else if (std::holds_alternative<cambc::map::camp_group_reward_available>(st)) {
                K.ready = 0;
                K.reward = true;
            } else if (auto r = std::get_if<cambc::map::camp_group_respawning>(&st)) {
                K.ready = round_ + r->timer;
                K.reward = false;
                K.hp_seen = -1;
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
    if (kLocks && (at_spawn_ || !alive) && gold_ >= 10) {
        const auto lock = cambc::consumable_type::altar_lock;
        if (me.can_buy(lock)) {
            me.buy(lock);
            gold_ -= 10;
        }
    }
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

// An altar we pass without a lock gets ours: enemy tributes there wait until they break it.
void Bot::place_lock() {
    if (!kLocks || (gems_ > 0 && banking_)) return;   // never delay our own tribute
    auto& me = cambc::self();
    const auto& alt = map_info().altars;
    for (size_t i = 0; i < alt.size(); ++i) {
        if (cheb(alt[i], pos_) > 2 || !visible_.is_set(alt[i])) continue;
        if (vision::has_altar_lock(alt[i])) continue;
        const auto& c = me.consumables()[cambc::consumable_type::altar_lock];
        if (c.count() == 0) return;
        try {
            c.use(alt[i]);
        } catch (const std::exception& e) {
            ++warns_;
            if (kTrace) std::printf("warn: lock place %s\n", e.what());
        }
        return;
    }
}

// An enemy tributing at an altar near us: a lock placed on it cancels the channel.
bool Bot::deny_tribute() {
    if (!kLocks || enemies_.empty()) return false;
    auto& me = cambc::self();
    if (me.consumables()[cambc::consumable_type::altar_lock].count() == 0) return false;
    const auto& alt = map_info().altars;
    for (const auto& e : enemies_) {
        if (!e.tribute) continue;
        for (size_t i = 0; i < alt.size(); ++i) {
            if (cheb(e.pos, alt[i]) > 1 || cheb(alt[i], pos_) > 6) continue;
            try {
                if (vision::has_altar_lock(alt[i])) continue;
            } catch (...) {
                continue;
            }
            act_ = kActFight;
            if (cheb(alt[i], pos_) <= 2) {
                place_lock();
                return true;
            }
            bitboard goals;
            mark_square(goals, alt[i], 2);
            step_towards(goals, true);
            if (cheb(alt[i], pos_) <= 2) place_lock();
            return true;
        }
    }
    return false;
}

void Bot::finish_turn() {
    write_claim(target_);
    if (kTrace && cambc::fuel_remaining() < 20000)
        std::printf("FUEL r%d s%d left%llu start%llu snap%llu groups%llu pre%llu enemies%llu bank%llu act%d tgt%d\n", round_, slot_,
            static_cast<unsigned long long>(cambc::fuel_remaining()), static_cast<unsigned long long>(fuel_mark_[0]),
            static_cast<unsigned long long>(fuel_mark_[0] - fuel_mark_[1]), static_cast<unsigned long long>(fuel_mark_[1] - fuel_mark_[2]),
            static_cast<unsigned long long>(fuel_mark_[2] - fuel_mark_[3]), static_cast<unsigned long long>(fuel_mark_[3] - fuel_mark_[4]),
            static_cast<unsigned long long>(fuel_mark_[4] - fuel_mark_[5]), act_, target_);
    if (kTrace && cambc::fuel_remaining() < 20000)
        std::printf("    choose%llu step%llu\n", static_cast<unsigned long long>(fuel_choose_), static_cast<unsigned long long>(fuel_step_));
    fuel_choose_ = fuel_step_ = 0;
    if (kTrace) {
        ++acts_[static_cast<size_t>(act_)];
        if (round_ % 100 == 0 || (slot_ == kTraceSlot && round_ >= kTraceFrom && round_ < kTraceTo))
            std::printf("T a%d br%d r%d s%d (%d,%d) hp%.0f/%.0f g%d gold%d str%.0f shards%d tgt%d fuel%llu w%d\n", act_, branch_, round_, slot_, pos_.x, pos_.y, hp_, maxhp_, gems_, gold_, str_, shards_,
                target_, static_cast<unsigned long long>(cambc::fuel_remaining()), warns_);
        if (round_ % 250 == 0) {
            std::printf("A r%d s%d", round_, slot_);
            static const char* names[] = {"dead", "chan", "heal", "fight", "flee", "bank", "hit", "walk", "capt", "idle", "wait", "recall"};
            for (int i = 0; i < kActs; ++i) std::printf(" %s=%d", names[i], acts_[static_cast<size_t>(i)]);
            std::printf("\n");
        }
    }
}

void Bot::write_claim(int g) {
    if (slot_ < 0 || slot_ >= sm::kSlots) return;
    try {
        cambc::shared_memory[static_cast<size_t>(sm::claims + slot_)] = static_cast<std::uint8_t>(g < 0 ? 0 : (g + 1) & 255);
    } catch (...) {
    }
}

// Claimed group per slot: -1 none, -2 dead.
std::array<int, 6> Bot::read_claims() const {
    std::array<int, 6> out{};
    out.fill(-1);
    std::array<std::uint8_t, sm::kSlots> raw{};
    try {
        cambc::shared_memory.read(sm::claims, std::span<std::uint8_t>{raw});
    } catch (...) {
    }
    for (int i = 0; i < sm::kSlots; ++i) {
        const int v = raw[static_cast<size_t>(i)];
        out[static_cast<size_t>(i)] = v == sm::kDead ? -2 : v - 1;
    }
    return out;
}

// Squad bookkeeping: the lowest living slot of our squad leads and picks the target.
void Bot::update_squad() {
    const auto claims = read_claims();
    const int sq = slot_ < 5 ? plan_.squad[static_cast<size_t>(slot_)] : -1;
    leader_slot_ = slot_;
    squad_n_ = 1;
    if (sq < 0) return;
    for (int s = 0; s < sm::kSlots; ++s) {
        if (s == slot_ || s >= static_cast<int>(plan_.picks.size())) continue;
        if (plan_.squad[static_cast<size_t>(s)] != sq || claims[static_cast<size_t>(s)] == -2) continue;
        ++squad_n_;
        if (s < leader_slot_) leader_slot_ = s;
    }
    leader_target_ = leader_slot_ != slot_ ? claims[static_cast<size_t>(leader_slot_)] : -1;
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

// One ordinary step; never throws (a hidden or newly seen unit just ends our walking).
bool Bot::walk_to(vec2 t) {
    auto& me = cambc::self();
    if (moved_ || move_failed_) return false;
    try {
        if (me.move_absolute(t)) {
            moved_ = true;
            pos_ = t;
            return true;
        }
    } catch (const std::exception& e) {
        ++warns_;
        if (kTrace) std::printf("warn: walk %s\n", e.what());
    }
    move_failed_ = true;
    return false;
}

// Step towards the nearest tile no awake camp can reach.
void Bot::disengage_step() {
    act_ = kActFlee;
    bitboard danger;
    for (const auto& c : awake_) mark_square(danger, c.pos, c.range);
    if (target_ >= 0)
        for (auto t : map_info().groups[static_cast<size_t>(target_)].tiles)
            if (camp_tiles_.is_set(t)) mark_square(danger, t, map_info().groups[static_cast<size_t>(target_)].range);
    bitboard goals;
    mark_square(goals, pos_, 9);
    goals &= ~danger;
    if (!goals.is_empty()) step_towards(goals, true);
}

bool Bot::step_towards(bitboard goals, bool allow_dash, int key) {
    auto& me = cambc::self();
    struct Spent {
        std::uint64_t f0;
        std::uint64_t* out;
        ~Spent() { *out += f0 - cambc::fuel_remaining(); }
    } spent{cambc::fuel_remaining(), &fuel_step_};
    if (moved_ || move_failed_ || !me.can_move()) return false;
    // Long walks follow a cached static field (one full search per destination); the last tiles
    // use the exact search with the units we can see.
    if (key >= 0 && kNavCache && map_info().w * map_info().h >= kNavCacheArea) {
        if (key != nav_key_ || round_ - nav_round_ > 80) {
            bitboard walls = static_walls() | map_info().camp_tiles;
            walls &= ~goals;
            bitboard roots = goals & ~walls;
            nav_field_.compute(walls, roots, nav_reached_);
            nav_key_ = key;
            nav_round_ = round_;
        }
        if (nav_reached_.is_set(pos_) && nav_field_.get(pos_) > 6) {
            const int d0 = nav_field_.get(pos_);
            const auto blocked = blocked_now();
            auto ok = [&](vec2 t) { return in_bounds(t) && !blocked.is_set(t) && nav_reached_.is_set(t); };
            if (allow_dash && kit_.dash >= 0 && !cast_ && d0 >= 8) {
                const auto ab = me.abilities()[static_cast<size_t>(kit_.dash)];
                if (ab.is_ready()) {
                    vec2 best{-1, -1};
                    int bd = d0;
                    for (int dy = -2; dy <= 2; ++dy)
                        for (int dx = -2; dx <= 2; ++dx) {
                            const vec2 t{pos_.x + dx, pos_.y + dy};
                            if (std::max(std::abs(dx), std::abs(dy)) != 2 || !ok(t)) continue;
                            const vec2 mid{pos_.x + dx / 2, pos_.y + dy / 2};
                            if (!ok(mid) || nav_field_.get(t) != d0 - 2) continue;
                            if (nav_field_.get(t) < bd) {
                                bd = nav_field_.get(t);
                                best = t;
                            }
                        }
                    if (best.x >= 0) {
                        try {
                            if (ab.use(best)) {
                                cast_ = true;
                                pos_ = best;
                            } else {
                                move_failed_ = true;
                            }
                        } catch (...) {
                            ++warns_;
                        }
                    }
                }
            }
            if (cast_ || move_failed_ || !me.can_move()) return cast_;
            const int dc = nav_field_.get(pos_);
            vec2 step = pos_;
            int sd = dc;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const vec2 t{pos_.x + dx, pos_.y + dy};
                    if ((dx == 0 && dy == 0) || !ok(t)) continue;
                    if (nav_field_.get(t) < sd) {
                        sd = nav_field_.get(t);
                        step = t;
                    }
                }
            if (step != pos_) return walk_to(step);
        }
    }
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
    if (move_failed_ || !me.can_move()) return cast_;
    {
        // A dash shows new tiles: refresh the occupancy before the walk.
        bitboard occupied;
        if (cast_) {
            vision::champion_tiles_into(occupied);
            bitboard camps;
            vision::camp_tiles_into(camps);
            occupied |= camps;
            occupied |= vision::teleport_tiles();
            occupied.set(pos_, false);
        }
        auto free_walk = [&](vec2 t) { return free_step(t) && !occupied.is_set(t); };
        const int dcur = field.get(pos_);
        vec2 best = pos_;
        int bestd = dcur;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                const vec2 t{pos_.x + dx, pos_.y + dy};
                if ((dx == 0 && dy == 0) || !free_walk(t)) continue;
                const int d = field.get(t);
                if (d < bestd) {
                    bestd = d;
                    best = t;
                }
            }
        if (best == pos_) return cast_;
        return walk_to(best) || cast_;
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
    if (has_emblem(type_, shards_, cambc::item_type::monster_shard_of_strength)) s *= 1.1f;
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

// ---------------------------------------------------------------- recall & bank

bool Bot::should_recall() {
    if (at_spawn_ || damaged_) return false;
    for (const auto& e : enemies_)
        if (cheb(e.pos, pos_) <= 8) return false;
    if (hp_ < 0.3f * maxhp_) return true;
    // Enemies about: do not roam half dead.
    if (round_ - last_enemy_round_ < 30 && hp_ < 0.5f * maxhp_) return true;
    if (!tavi_ && gold_ >= 240 && hp_ < 0.7f * maxhp_ && home_dist(pos_) > 12) return true;
    return false;
}

bool Bot::bank() {
    auto& me = cambc::self();
    if (gems_ <= 0) {
        banking_ = 0;
        bank_altar_ = -1;
        return false;
    }
    const auto& m = map_info();
    if (m.altars.empty()) return false;
    // Capturing a group comes first.
    if (target_ >= 0 && gk_[static_cast<size_t>(target_)].reward) {
        for (auto t : m.groups[static_cast<size_t>(target_)].tiles)
            if (t == pos_) return false;
    }
    int near_altar = kFar;
    for (auto a : m.altars) near_altar = std::min(near_altar, cheb(a, pos_));
    const int submitted = static_cast<int>(cambc::global_submitted_gems(team::ally));
    const int enemy_sub = static_cast<int>(cambc::global_submitted_gems(team::enemy));
    // The race's last stretch: carried gems are worth nothing when the game ends.
    const bool race = enemy_sub >= kRaceEnemy || submitted >= kRaceOurs;
    bool need = banking_ != 0 || gems_ >= plan_.bank || submitted + gems_ >= 1000 || (race && gems_ >= kRaceBag)
        || (gems_ >= 8 && near_altar <= 4) || (round_ >= 1930 - near_altar)
        || (gems_ >= kBankWary && round_ - last_enemy_round_ < 20);
    if (!need) return false;
    banking_ = 1;
    act_ = kActBank;

    if (bank_altar_ < 0 || round_ - bank_choice_ >= 12
        || (round_ == altar_enemy_[static_cast<size_t>(bank_altar_)] && round_ - bank_choice_ >= 3))
        choose_altar();
    if (bank_altar_ < 0) return false;
    const vec2 a = m.altars[static_cast<size_t>(bank_altar_)];
    const bool locked = altar_locked_[static_cast<size_t>(bank_altar_)];
    const int d = cheb(a, pos_);
    if (!locked && d <= cambc::constants::submit_gems_range) {
        try {
            me.submit_gems(a);
            return true;
        } catch (const std::exception& e) {
            ++warns_;
            if (kTrace) std::printf("warn: submit %s\n", e.what());
        }
    }
    if (locked && d <= range_ && me.can_attack()) {
        try {
            if (vision::has_altar_lock(a)) {
                me.attack(vision::altar_lock_at(a));
                attacked_ = true;
                return true;
            }
        } catch (const std::exception& e) {
            ++warns_;
            if (kTrace) std::printf("warn: lock %s\n", e.what());
        }
    }
    bitboard goals;
    goals.clear();
    mark_square(goals, a, locked ? range_ : cambc::constants::submit_gems_range);
    step_towards(goals, true, kNavAltar + bank_altar_ * 2 + (locked ? 1 : 0));
    return true;
}

// Pick the altar to bank at: walking distance, plus a detour for an enemy lock we must break.
void Bot::choose_altar() {
    const auto& m = map_info();
    if (bank_altar_ >= 0 && cambc::fuel_remaining() < 45000u) return;
    bank_choice_ = round_;
    cambc::distance_field field;
    bitboard visited, roots;
    roots.clear();
    roots.set(pos_);
    field.compute(static_walls(), roots, visited);
    int best = -1, best_cost = kFar;
    for (size_t i = 0; i < m.altars.size(); ++i) {
        const vec2 a = m.altars[i];
        int d = kFar;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                const vec2 t{a.x + dx, a.y + dy};
                if (in_bounds(t) && visited.is_set(t)) d = std::min(d, static_cast<int>(field.get(t)));
            }
        if (d >= kFar) continue;
        int cost = d;
        if (altar_locked_[i]) cost += 10;
        if (round_ - altar_enemy_[i] < 40) cost += kAltarDanger * (40 - (round_ - altar_enemy_[i])) / 40;
        if (static_cast<int>(i) == bank_altar_) cost -= 3;
        if (cost < best_cost) {
            best_cost = cost;
            best = static_cast<int>(i);
        }
    }
    if (kTrace && best != bank_altar_)
        std::printf("K r%d s%d (%d,%d) gems%d altar%d->%d cost%d lock%d danger%d\n", round_, slot_, pos_.x, pos_.y, gems_, bank_altar_, best, best_cost,
            best >= 0 ? static_cast<int>(altar_locked_[static_cast<size_t>(best)]) : -1, best >= 0 ? round_ - altar_enemy_[static_cast<size_t>(best)] : -1);
    bank_altar_ = best;
}

// ---------------------------------------------------------------- farming

int Bot::group_value(int g) const {
    const auto& G = map_info().groups[static_cast<size_t>(g)];
    const int submitted = submitted_;
    float goldw = round_ < 700 ? 0.4f : (round_ < 1200 ? 0.25f : 0.1f);
    if (submitted + gems_ >= 800) goldw = 0.05f;
    return static_cast<int>(static_cast<float>(G.gems) + goldw * static_cast<float>(G.gold));
}

// Time to clear a group at `dps` and the damage taken meanwhile: monsters die in priority order
// (most strength per health first) and every living fighter hits us until it dies.
void Bot::clear_model(const GroupInfo& G, float hp_scale, float dps, float* time, float* dmg) const {
    float alive_str = G.dps;
    float t = 0, d = 0;
    const float inv = 1.0f / std::max(1.0f, dps);
    for (const auto& [hp, str] : G.kill_order) {
        const float dt = hp * hp_scale * inv;
        t += dt;
        d += alive_str * dt;
        alive_str -= str;
    }
    *time = t;
    *dmg = d;
}

void Bot::choose_target(bool force) {
    const auto& m = map_info();
    const auto f0 = cambc::fuel_remaining();
    struct Spent {
        std::uint64_t f0;
        std::uint64_t* out;
        ~Spent() { *out += f0 - cambc::fuel_remaining(); }
    } spent{f0, &fuel_choose_};
    // Optional re-evaluations wait for a turn with fuel to spare.
    if (cambc::fuel_remaining() < (force ? 40000u : 55000u) && target_ >= 0) return;
    last_eval_ = round_;
    cambc::distance_field field;
    bitboard visited, roots;
    roots.clear();
    roots.set(pos_);
    field.compute(static_walls(), roots, visited, kChooseRadius);
    const auto claims = read_claims();
    submitted_ = static_cast<int>(cambc::global_submitted_gems(team::ally));
    const float dps = my_camp_dps() * (0.4f + 0.6f * static_cast<float>(squad_n_));
    const float camp_mult = has_emblem(type_, shards_, cambc::item_type::jungle_shard_of_strength) ? 0.7f : 1.0f;

    // A shopping trip home first when enough gold waits to be spent (recall heals as well).
    bool safe = !damaged_;
    for (const auto& e : enemies_)
        if (cheb(e.pos, pos_) <= 9) safe = false;
    const bool via_home = force && safe && !at_spawn_ && !tavi_ && gold_ >= kShopGold && home_dist(pos_) > 8;

    int best = -1;
    float best_score = 0, cur_score = 0;
    bool best_needs_recall = false;
    const int n = static_cast<int>(m.groups.size());
    for (int g = 0; g < n; ++g) {
        const auto& G = m.groups[static_cast<size_t>(g)];
        const auto cls = plan_.cls[static_cast<size_t>(g)];
        if (cls == kGUnreachable) continue;
        const auto& K = gk_[static_cast<size_t>(g)];
        // Walking distance from at most three member tiles; beyond the search radius a scaled
        // straight-line estimate (only when the group is reachable from our spawn at all).
        int d = kFar;
        const size_t nt = G.tiles.size();
        const size_t picks[3] = {0, nt / 2, nt - 1};
        for (size_t k = 0; k < 3 && nt > 0; ++k) {
            const vec2 t = G.tiles[picks[k]];
            if (visited.is_set(t)) d = std::min(d, static_cast<int>(field.get(t)));
        }
        if (d >= kFar) {
            if (G.dA >= kFar) continue;
            // Beyond the search radius: at least the radius, at least the spawn-distance difference.
            d = std::max({static_cast<int>(kChooseRadius) + 1, G.dA - std::min(home_dist(pos_), 500), cheb(G.center, pos_) * 6 / 5});
        }

        int claimers = 0;
        for (int s = 0; s < sm::kSlots; ++s)
            if (s != slot_ && claims[static_cast<size_t>(s)] == g && !same_squad(s)) ++claimers;

        const float hp_scale = (K.hp_seen >= 0 && K.ready <= round_ && G.hp > 0) ? K.hp_seen / G.hp : 1.0f;
        float solo_clear = 0, solo_dmg = 0;
        if (!K.reward) clear_model(G, hp_scale, dps, &solo_clear, &solo_dmg);
        solo_dmg *= camp_mult;
        const bool solo_ok = solo_dmg < 0.85f * maxhp_;
        if (claimers > 0 && (solo_ok || K.reward)) continue;   // an ally has it covered
        const int team_n = 1 + claimers;
        float clear = solo_clear, dmg = solo_dmg;
        if (team_n > 1 && !K.reward) {
            clear_model(G, hp_scale, dps * static_cast<float>(team_n), &clear, &dmg);
            dmg *= camp_mult;
        }
        // Groups too strong for one trip are worn down over several (camps never heal). Compare going
        // now with the health we have against recalling first (full health, gold spent).
        const float per_trip = 0.75f * maxhp_;
        const float budget = std::max(0.0f, hp_ - 0.15f * maxhp_);
        const int extra_now = dmg > budget ? static_cast<int>(std::ceil((dmg - budget) / per_trip)) : 0;
        const int extra_home = static_cast<int>(std::ceil(std::max(0.0f, dmg - per_trip) / per_trip));
        const float trip_cost = static_cast<float>(7 + G.dA);
        const float t_now = static_cast<float>(d) + static_cast<float>(extra_now) * trip_cost;
        const float t_home = (at_spawn_ ? static_cast<float>(G.dA) : static_cast<float>(6 + G.dA))
            + static_cast<float>(extra_home) * trip_cost - (gold_ >= 90 && !tavi_ ? 6.0f : 0.0f);
        bool needs_recall = via_home || (!at_spawn_ && t_home < t_now);
        const int extra = needs_recall ? extra_home : extra_now;
        if (extra > kMaxExtraTrips) continue;
        const int travel = needs_recall ? (at_spawn_ ? G.dA : 6 + G.dA) : d;
        const int wait = std::max(0, K.ready - round_ - travel);
        if (wait > 40) continue;
        float value = static_cast<float>(group_value(g)) / static_cast<float>(team_n);
        if (cls == kGEnemy) value *= 0.6f;
        else if (cls == kGContested) value *= 0.85f;
        if (round_ - K.enemy_round < 30) value *= kEnemySeenFactor;
        if (round_ < avoid_until_ && cheb(G.center, avoid_pos_) <= 8) value *= 0.2f;
        if (kBuddy > 0.0f) {
            // Stay within reach of an ally: fights near a friend go better than alone.
            for (int s2 = 0; s2 < sm::kSlots; ++s2) {
                const int cg = claims[static_cast<size_t>(s2)];
                if (s2 == slot_ || cg < 0 || cg == g || cg >= n) continue;
                if (cheb(m.groups[static_cast<size_t>(cg)].center, G.center) <= 10) {
                    value *= 1.0f + kBuddy;
                    break;
                }
            }
        }
        {
            const int own = static_cast<size_t>(g) < plan_.owner.size() ? plan_.owner[static_cast<size_t>(g)] : -1;
            if (own >= 0) value *= own == slot_ ? 1.0f : kOtherSector;
            else value *= kSharedSector;
        }
        const float time = static_cast<float>(travel + wait) + clear + 5.0f + static_cast<float>(extra) * trip_cost;
        const float score = value / (time + kTimeConst);
        if (g == target_) cur_score = score;
        if (score > best_score) {
            best_score = score;
            best = g;
            best_needs_recall = needs_recall;
        }
    }
    if (kTrace && slot_ == kTraceSlot && round_ >= kTraceFrom && round_ < kTraceTo)
        std::printf("C r%d s%d force%d cur%d %.3f best%d %.3f lasthit%d\n", round_, slot_, force ? 1 : 0, target_, cur_score, best, best_score, last_hit_round_);
    // Keep the current target unless something is clearly better: switching wastes the walk.
    if (!force && target_ >= 0 && cur_score > 0 && best_score < 1.4f * cur_score) return;
    if (best != target_) {
        target_since_ = round_;
        stall_ = 0;
        shrink_ = false;
    }
    target_ = best;
    if (best >= 0 && best_needs_recall && !at_spawn_ && safe) {
        try {
            act_ = kActRecall;
            cambc::self().recall();
            moved_ = true;
        } catch (...) {
            ++warns_;
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

    if (K.ready > round_) {
        // Respawning: wait on the approach tiles if it is back soon enough.
        if (K.ready - round_ > 40) return false;
        act_ = kActWait;
        // A long wait is better spent at home: heal and shop, then walk back.
        if (!at_spawn_ && !damaged_ && K.ready - round_ > 8 + G.dA && (gold_ >= 60 || hp_ < 0.8f * maxhp_)) {
            bool enemy_near = false;
            for (const auto& e : enemies_)
                if (cheb(e.pos, pos_) <= 9) enemy_near = true;
            if (!enemy_near) {
                act_ = kActRecall;
                me.recall();
                return true;
            }
        }
        const auto& ap = approach_tiles(g);
        if (!ap.is_set(pos_)) step_towards(ap, true, g);
        return true;
    }

    branch_ = 1;
    if (any_alive) {
        // Fight until our health runs low (unless the group is about to die), then leave its reach,
        // recall and come back: camps never heal, so every trip makes progress.
        const float exit_turns = static_cast<float>(std::max(3, G.range - range_ + 2));
        if (damaged_ && hp_ - exit_turns * dmg_taken_ < 0.2f * maxhp_) {
            float left = 0, team_dps = my_camp_dps();
            for (auto t : G.tiles)
                if (camp_tiles_.is_set(t)) {
                    try {
                        left += vision::camp_at(t).health();
                    } catch (...) {
                    }
                }
            for (const auto& a : allies_)
                if (cheb(a.pos, G.center) <= 5) team_dps += a.dps;
            if (left > 2.0f * team_dps || hp_ < 2.0f * dmg_taken_) {
                disengage_ = true;
                disengage_step();
                return true;
            }
        }
        act_ = kActHit;
        if (attack_camps_in_range(g)) {
            last_hit_round_ = round_;
            return true;
        }
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
        act_ = kActCapture;
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
    act_ = kActWalk;
    step_towards(approach_tiles(g), true, g);
    return true;
}

// One turn of the raid on group g. False when the raid gives nothing to do (farm instead).
bool Bot::raid_step(int g) {
    const auto& G = map_info().groups[static_cast<size_t>(g)];
    if (target_ != g) {
        target_ = g;
        stall_ = 0;
        shrink_ = false;
    }
    int flag = 0, stamp = 0;
    try {
        flag = cambc::shared_memory[sm::raid];
        stamp = cambc::shared_memory[sm::raid_round];
    } catch (...) {
    }
    const bool engaged = flag == g + 1 && ((round_ - stamp) & 255) <= 30;
    auto mark_engaged = [&]() {
        try {
            cambc::shared_memory[sm::raid] = static_cast<std::uint8_t>(g + 1);
            cambc::shared_memory[sm::raid_round] = static_cast<std::uint8_t>(round_ & 255);
        } catch (...) {
        }
    };
    // Heal (and shop) before a trip.
    if (!at_spawn_ && hp_ < 0.6f * maxhp_ && !damaged_ && round_ - last_hit_round_ > 3) {
        bool enemy_near = false;
        for (const auto& e : enemies_)
            if (cheb(e.pos, pos_) <= 9) enemy_near = true;
        if (!enemy_near) {
            act_ = kActRecall;
            cambc::self().recall();
            return true;
        }
    }
    bool boss_alive = false;
    for (auto t : G.tiles)
        if (camp_tiles_.is_set(t)) boss_alive = true;
    if (engaged || round_ - last_hit_round_ <= 3 || !boss_alive) {
        const bool ok = execute_target(g);
        if (round_ == last_hit_round_) mark_engaged();
        return ok;
    }
    // Gather on the ring just outside its reach until enough raiders are here.
    act_ = kActWait;
    bitboard inner, ring;
    for (auto t : G.tiles) {
        mark_square(ring, t, G.range + 3);
        mark_square(inner, t, G.range + 1);
    }
    ring &= ~inner;
    ring &= ~static_walls();
    if (!ring.is_set(pos_)) {
        step_towards(ring, true);
        return true;
    }
    int near = 0;
    for (const auto& a : allies_)
        if (cheb(a.pos, pos_) <= 6) ++near;
    int alive = 0;
    const auto claims = read_claims();
    for (int s = 0; s < static_cast<int>(plan_.picks.size()) && s < sm::kSlots; ++s)
        if (claims[static_cast<size_t>(s)] != -2) ++alive;
    const int need = std::max(1, std::min(G.dps >= 200 ? 4 : 2, alive - 1));
    if (near >= need || ++rally_wait_ > 40) {
        rally_wait_ = 0;
        mark_engaged();
        return execute_target(g);
    }
    return true;
}

int Bot::raid_target() const {
    const int n = static_cast<int>(gk_.size());
    const int groups[2] = {plan_.raid, plan_.raid2};
    const int pows[2] = {plan_.raid_pow, plan_.raid2_pow};
    for (int i = 0; i < 2; ++i) {
        const int g = groups[i];
        if (g < 0 || g >= n) continue;
        if (str_ < static_cast<float>(pows[i]) && round_ < plan_.raid_round + 200 * i) continue;
        const auto& K = gk_[static_cast<size_t>(g)];
        if (K.ready - round_ > 25) continue;
        return g;
    }
    return -1;
}

void Bot::idle() {
    // Nothing worth doing: wait at home (heals, shops).
    if (at_spawn_) return;
    step_towards(cambc::map::spawn_region(team::ally), true, kNavSpawn);
}

void Bot::farm() {
    const auto& m = map_info();
    // Leaving a group too strong for this trip: get out of reach, then recall and come back.
    if (disengage_) {
        if (at_spawn_) {
            disengage_ = false;
        } else if (damaged_ || camp_danger_at(pos_) > 0) {
            disengage_step();
            return;
        } else {
            bool enemy_near = false;
            for (const auto& e : enemies_)
                if (cheb(e.pos, pos_) <= 9) enemy_near = true;
            disengage_ = false;
            if (!enemy_near) {
                act_ = kActRecall;
                cambc::self().recall();
                return;
            }
        }
    }
    // Never abandon a capture in progress.
    if (target_ >= 0 && gk_[static_cast<size_t>(target_)].reward)
        for (auto t : m.groups[static_cast<size_t>(target_)].tiles)
            if (t == pos_) {
                execute_target(target_);
                return;
            }
    // Boss raid from the plan: raiders gather just outside the boss's reach and engage together.
    const int raid = raid_target();
    if (raid >= 0 && raid_step(raid)) return;
    if (leader_slot_ != slot_ && leader_target_ >= 0 && leader_target_ < static_cast<int>(gk_.size())) {
        if (target_ != leader_target_) {
            target_ = leader_target_;
            stall_ = 0;
            shrink_ = false;
        }
        if (execute_target(target_)) return;
    }
    bool invalid = target_ < 0 || gk_[static_cast<size_t>(target_)].ready > round_ + 40;
    if (!invalid && round_ < avoid_until_ && cheb(m.groups[static_cast<size_t>(target_)].center, avoid_pos_) <= 8) invalid = true;
    const bool engaged = round_ - last_hit_round_ <= 3;
    if (invalid || (!engaged && round_ - last_eval_ >= kEvalEvery)) {
        choose_target(invalid);
        if (moved_) return;   // recall started
    }
    if (target_ < 0) {
        idle();
        return;
    }
    if (!execute_target(target_)) {
        choose_target(true);
        if (moved_) return;
        if (target_ >= 0) execute_target(target_);
        else idle();
    }
}
