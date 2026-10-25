// Scaling-economy champion: farm gold and gems on our side, turn gold into stats at every
// spawn visit, bank gems at the nearest altar, and fight only when the numbers favour us.
#include <cambc.h>
#include "bot.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <string>
#include <span>
#include <utility>
#include <variant>

constexpr bool kDebugMap = false;
constexpr bool kStats = false;  // activity counters and positions in stdout
constexpr bool kTrace = false;
constexpr bool kTraceChoose = false;
constexpr bool kVisBench = false;
std::uint64_t g_f0 = 0;
const char* g_phase_tag = "";
int now_dbg() { return static_cast<int>(cambc::round_number()); }
std::string g_fp_log;
// Measurement builds only: fuel per phase (between fp tags) and failed fuel gates, every 500 rounds.
constexpr bool kPhase = false;
struct PhaseAcc { const char* tag; std::uint64_t sum; std::uint32_t n; };
std::array<PhaseAcc, 40> g_phase{};
void fp(const char* tag) {
    g_phase_tag = tag;
    if (kPhase) {
        const auto f = cambc::fuel_remaining();
        const std::uint64_t d = g_f0 > f ? g_f0 - f : 0;
        for (auto& a : g_phase)
            if (a.tag == tag || !a.tag) { a.tag = tag; a.sum += d; ++a.n; break; }
        g_f0 = f;
        return;
    }
    if (!kTrace) return;
    const auto f = cambc::fuel_remaining();
    if (g_f0 > f && g_f0 - f > 3000) { g_fp_log += ' '; g_fp_log += tag; g_fp_log += '='; g_fp_log += std::to_string((g_f0 - f) / 1000); }
    g_f0 = f;
}
constexpr bool kFoeMemory = false;  // remember enemies for 3 rounds
constexpr bool kHysteresis = false;
constexpr int kChaseTurns = 5;  // approach turns without a hit before giving up a chase  // solo clear time above which allies may join a group
constexpr float kBossTurns = 8.0f;  // turns a champion must be able to tank a boss
constexpr bool kLocks = true;       // carry an altar lock and drop it on enemy tributes
constexpr bool kDeny = true;        // hunt enemies channelling a tribute or recall
constexpr bool kPressureTribute = true;  // bank under enemy pressure when the channel outlasts their damage
constexpr bool kLockRunner = true;  // one champion carries locks to far altars
constexpr int kLockRange = 2;       // altar lock placement range
constexpr bool kLockAll = true;     // lock every altar we pass: our locks block only enemy tributes
constexpr int kLockDetour = 8;      // walk this far (Chebyshev) out of our way to lock an altar

namespace {
// Debug: turns spent per activity, printed every 200 rounds.
}  // namespace
int g_mode = 0;
namespace {
std::array<int, 20> g_modes{};
const char* kModeNames[20] = {"dead", "chan", "enemy", "fuel", "spwait", "hurt", "bank", "idle", "shop", "heal",
    "stuck", "travel", "fight", "capture", "respawn", "approach", "rally", "", "", ""};
struct ModeCount {
    std::uint64_t f0 = kStats ? cambc::fuel_remaining() : 0;
    ~ModeCount() {
        if (kPhase) {
            fp("end");
            const int r = static_cast<int>(cambc::round_number());
            if (r % 500 == 0 || r == 1990) {
                std::cout << "phase r" << r << ' ' << cambc::to_string(cambc::self().champion_name());
                for (const auto& a : g_phase) if (a.tag) std::cout << ' ' << a.tag << '=' << a.sum / 1000 << "k/" << a.n;
                std::cout << '\n';
            }
        }
        if (kStats) {
            const auto used = f0 - cambc::fuel_remaining();
            if (used > 70000)
                std::cout << "heavy r" << cambc::round_number() << ' ' << cambc::to_string(cambc::self().champion_name())
                          << ' ' << kModeNames[g_mode] << ' ' << used << g_fp_log << '\n';
            g_fp_log.clear();
        }
        ++g_modes[static_cast<size_t>(g_mode)];
        const int r = static_cast<int>(cambc::round_number());
        if (kStats && r % 200 == 0) {
            std::cout << "stat r" << r << ' ' << cambc::to_string(cambc::self().champion_name());
            for (int i = 0; i < 17; ++i) if (g_modes[static_cast<size_t>(i)]) std::cout << ' ' << kModeNames[i] << '=' << g_modes[static_cast<size_t>(i)];
            std::cout << '\n';
        }
    }
};
}  // namespace

using it = cambc::item_type;

void warn(const char* where, const std::exception& e) {
    static int count = 0;
    if (++count <= 20) std::cout << "warn: " << where << ": " << e.what() << '\n';
}

float ability_range(const cambc::ability& ab) {
    float r = 4;
    try {
        const auto& info = ab.info();
        if (info) {
            if (auto* d = std::get_if<cambc::ability_damage>(&*info)) r = d->range;
            else if (auto* c = std::get_if<cambc::ability_cc>(&*info)) r = c->range;
            else if (auto* a = std::get_if<cambc::ability_aoe_damage>(&*info)) r = a->range;
            else if (auto* h = std::get_if<cambc::ability_heal>(&*info)) r = h->range;
        }
    } catch (const std::exception&) {}
    return r;
}

bool has_lock() {
    try { return cambc::self().consumables()[cambc::consumable_type::altar_lock].count() > 0; }
    catch (const cambc::error&) { return false; }
}

// Shared memory: bytes 0-4 hold each slot's target group + 1; bytes 5-7 the latest enemy sighting.
Sighting read_sighting() {
    std::array<std::uint8_t, 3> b{};
    cambc::shared_memory.read(5, std::span<std::uint8_t>{b.data(), b.size()});
    Sighting s;
    if (b[0] == 0) return s;
    s.where = {b[0] - 1, b[1]};
    const int now = static_cast<int>(cambc::round_number() >> 2) & 0xff;
    s.age = ((now - b[2]) & 0xff) * 4;
    s.ok = inside(s.where);
    return s;
}
void write_sighting(vec2 p) {
    std::array<std::uint8_t, 3> b{static_cast<std::uint8_t>(p.x + 1), static_cast<std::uint8_t>(p.y),
        static_cast<std::uint8_t>((cambc::round_number() >> 2) & 0xff)};
    cambc::shared_memory.write(5, std::span<const std::uint8_t>{b.data(), b.size()});
}

struct Step { it shard, emblem; };
const std::vector<Step>& build_for(cambc::champion_type t) {
    static const std::vector<Step> plain{
        {it::jungle_shard_of_strength, it::jungle_emblem_of_strength},
        {it::monster_shard_of_strength, it::monster_emblem_of_strength},
        {it::power_shard_of_health, it::power_emblem_of_health},
        {it::power_shard_of_strength, it::power_emblem_of_strength},
        {it::durability_shard_of_health, it::durability_emblem_of_health},
    };
    static const std::vector<Step> vesper{
        {it::jungle_shard_of_strength, it::jungle_emblem_of_strength},
        {it::treasure_shard_of_strength, it::treasure_emblem_of_strength},
        {it::monster_shard_of_strength, it::monster_emblem_of_strength},
        {it::power_shard_of_health, it::power_emblem_of_health},
        {it::durability_shard_of_health, it::durability_emblem_of_health},
    };
    return t == cambc::champion_type::vesper_vale ? vesper : plain;
}

bool has_any(const cambc::enum_map<it>& c, it a, it b, it d) { return c[a] + c[b] + c[d] > 0; }

// ---------------------------------------------------------------- setup

void Bot::initialise() {
    init_ = true;
    init_dims();
    nav_setup();
    auto& me = cambc::self();
    g_ = analyse_map();
    h_ = cambc::map::camp_groups();
    k_.assign(g_.size(), Known{});
    for (size_t i = 0; i < g_.size(); ++i)
        for (auto kind : g_[i].kinds) k_[i].hp.push_back(camp_spec(kind).hp);
    gfield_.clear();
    gfield_.resize(g_.size());
    group_at_.assign(static_cast<size_t>(map_w() * map_h()), -1);
    camp_at_.assign(static_cast<size_t>(map_w() * map_h()), -1);
    for (size_t i = 0; i < g_.size(); ++i) {
        int r = 0;
        for (auto t : g_[i].tiles) r = std::max(r, cheb(t, g_[i].center));
        reach_.push_back(r);
        gtiles_.push_back(h_[i].spawn_tiles());
        for (size_t c = 0; c < g_[i].tiles.size(); ++c) {
            group_at_[static_cast<size_t>(tidx(g_[i].tiles[c]))] = static_cast<std::int16_t>(i);
            camp_at_[static_cast<size_t>(tidx(g_[i].tiles[c]))] = static_cast<std::int16_t>(c);
        }
    }
    size_ = static_cast<int>(cambc::champions(team::ally).size());
    // Cassi and Issac count as one pick.
    slot_ = slot_of(me.champion_name(), size_);
    const auto* mem = cambc::draft_memory.data();
    if (static_cast<char>(mem[dm::magic]) == 'E' && static_cast<int>(mem[dm::ngroups]) == static_cast<int>(std::min<size_t>(g_.size(), 250))) {
        size_ = static_cast<int>(mem[dm::size]);
        slot_ = slot_of(me.champion_name(), size_);
        for (size_t i = 0; i < g_.size(); ++i)
            home_slot_.push_back(i < 250 ? static_cast<std::uint8_t>(mem[dm::home + i]) : 255);
    } else {
        home_slot_ = assign_homes(g_, std::max(1, size_));
    }
    kit_ = read_kit();
    for (const auto& sl : kit_)
        if (sl.kind == cambc::ability_kind::dash) dasher_ = true;
    my_range_ = me.attack_range();
    build_threats();

    home_.compute(walk_walls(), cambc::map::spawn_region(team::ally));
    auto spots = cambc::map::altars();
    grow_in_place(spots, cambc::constants::submit_gems_range);
    spots &= ~walk_walls();
    altar_.compute(walk_walls(), spots);
    altar_spots_ = spots;
    for (auto t : cambc::map::spawn_region(team::ally).positions()) home_altar_ = std::min(home_altar_, altar_.at(t));
    for (auto a : cambc::map::altars().positions()) altars_.push_back(a);
    bank_d_.assign(g_.size(), kFar);
    for (size_t i = 0; i < g_.size(); ++i)
        for (auto t : g_[i].tiles) bank_d_[i] = std::min(bank_d_[i], altar_.at(t));

    for (size_t i = 0; i < g_.size(); ++i) fill_order_.push_back(static_cast<int>(i));
    std::stable_sort(fill_order_.begin(), fill_order_.end(), [&](int a, int b) {
        const bool ma = home_slot_[a] == slot_, mb = home_slot_[b] == slot_;
        if (ma != mb) return ma;
        return g_[a].d_ally < g_[b].d_ally;
    });
}

// ---------------------------------------------------------------- knowledge

void Bot::observe() {
    const int now = static_cast<int>(cambc::round_number());
    for (auto& k : k_)
        if (k.state == Known::respawning && now >= k.ready) { k.state = Known::unknown; block_dirty_ = true; }
    const auto& v = view();
    fp("o-view");
    for (size_t i = 0; i < g_.size(); ++i) {
        if (cheb(g_[i].center, pos_) > 8 + reach_[i]) continue;
        auto& k = k_[i];
        bool seen = false, living = false;
        for (size_t c = 0; c < g_[i].tiles.size(); ++c) {
            const vec2 p = g_[i].tiles[c];
            if (!v.lvis.at(p)) continue;
            seen = true;
            if (v.lcamps.at(p)) {
                living = true;
                if (k.hp[c] <= 0) { k.hp[c] = camp_spec(g_[i].kinds[c]).hp; block_dirty_ = true; }
            } else if (k.state == Known::alive && k.hp[c] > 0) {
                k.hp[c] = 0;
                block_dirty_ = true;
            }
        }
        if (!seen) continue;
        if (living) {
            if (k.state != Known::alive) {
                // Back alive: unseen camps at full health, visible empty tiles dead.
                for (size_t c = 0; c < g_[i].tiles.size(); ++c) {
                    const vec2 p = g_[i].tiles[c];
                    if (!v.lvis.at(p)) k.hp[c] = camp_spec(g_[i].kinds[c]).hp;
                    else if (!v.lcamps.at(p)) k.hp[c] = 0;
                }
                k.state = Known::alive;
                block_dirty_ = true;
            }
            continue;
        }
        if (k.state == Known::respawning) continue;
        // A reward we are not after only needs an occasional look.
        if (k.state == Known::reward && static_cast<int>(i) != target_ && (now + static_cast<int>(i)) % 4 != 0) continue;
        try {
            const auto st = h_[i].state();
            const auto before = k.state;
            if (std::holds_alternative<cambc::map::camp_group_alive>(st)) {
                k.state = Known::alive;
            } else if (std::holds_alternative<cambc::map::camp_group_reward_available>(st)) {
                k.state = Known::reward;
            } else {
                k.state = Known::respawning;
                k.ready = now + std::max(1, std::get<cambc::map::camp_group_respawning>(st).timer);
                for (size_t c = 0; c < g_[i].tiles.size(); ++c) k.hp[c] = camp_spec(g_[i].kinds[c]).hp;
            }
            if (k.state != before) block_dirty_ = true;
        } catch (const std::exception&) {}
    }
    // Health of the camps of the group we fight.
    if (target_ >= 0 && cheb(g_[target_].center, pos_) <= 8 + reach_[target_]) {
        auto& k = k_[target_];
        for (size_t c = 0; c < g_[target_].tiles.size(); ++c) {
            const vec2 p = g_[target_].tiles[c];
            if (cheb(p, pos_) > 8 || !v.lvis.at(p) || !v.lcamps.at(p) || camp_spec(g_[target_].kinds[c]).str <= 0) continue;
            try { k.hp[c] = cambc::vision::camp_at(p).health(); } catch (const std::exception&) {}
        }
    }
}

void Bot::rebuild_camp_block() {
    if (!block_dirty_) return;
    block_dirty_ = false;
    block_buf_.assign(cambc::bitboard::linear_buffer_size(), 0);
    for (size_t i = 0; i < g_.size(); ++i) {
        const auto& k = k_[i];
        if (k.state == Known::respawning || k.state == Known::reward) continue;
        for (size_t c = 0; c < g_[i].tiles.size(); ++c)
            if (k.hp[c] > 0) {
                const int t = tidx(g_[i].tiles[c]);
                block_buf_[static_cast<size_t>(t >> 3)] |= static_cast<std::uint8_t>(1u << (t & 7));
            }
    }
    camp_block_.import_linear_buffer(std::span<const std::uint8_t>{block_buf_});
}

void Bot::measure() {
    auto& me = cambc::self();
    incoming_ = 0;
    exit_steps_ = 0;
    const auto& v = view();
    awake_ = cambc::vision::awake_camp_tiles();
    // Camps see (and hit) through bushes that hide them from us: remember awake camps for a few rounds
    // after they drop out of sight, or we would recall (and keep being hit) right beside them.
    {
        const int now = static_cast<int>(cambc::round_number());
        if (!awake_.is_empty())
            for (auto p : awake_.positions()) {
                bool found = false;
                for (auto& m : awake_mem_)
                    if (m.first == p) { m.second = now; found = true; break; }
                if (!found) awake_mem_.push_back({p, now});
            }
        std::erase_if(awake_mem_, [&](const std::pair<vec2, int>& m) {
            if (now - m.second > 3 || v.lvis.at(m.first)) return now != m.second;
            const int gi = group_at_[static_cast<size_t>(tidx(m.first))];
            const int ci = camp_at_[static_cast<size_t>(tidx(m.first))];
            return gi < 0 || ci < 0 || k_[gi].hp[static_cast<size_t>(ci)] <= 0;
        });
        for (const auto& m : awake_mem_)
            if (m.second != now) awake_.set(m.first);
    }
    if (!awake_.is_empty())
        for (auto p : awake_.positions()) {
            const int d = cheb(p, pos_);
            if (d > 5) continue;
            const int gi = group_at_[static_cast<size_t>(tidx(p))];
            const int ci = camp_at_[static_cast<size_t>(tidx(p))];
            if (gi < 0 || ci < 0) continue;
            const auto s = camp_spec(g_[gi].kinds[static_cast<size_t>(ci)]);
            if (s.str <= 0 || d > s.range) continue;
            // A camp hits the nearest champion: an ally closer to it takes the blows.
            bool shielded = false;
            for (auto a : v.allies)
                if (cheb(a, p) < d) { shielded = true; break; }
            if (shielded) continue;
            incoming_ += s.str;
            exit_steps_ = std::max(exit_steps_, s.range + 1 - d);
        }
    const auto counts = me.item_count();
    taken_ = 1.0f;
    if (has_any(counts, it::jungle_emblem_of_strength, it::jungle_emblem_of_health, it::jungle_emblem_of_focus)) taken_ *= 0.7f;
    if (has_any(counts, it::durability_emblem_of_strength, it::durability_emblem_of_health, it::durability_emblem_of_focus)) taken_ *= 0.9f;
    float dps = me.strength();
    float spell = 0;
    bool reset = false;
    for (const auto& ab : me.abilities()) {
        if (ab.is_attack_reset()) reset = true;
        if (ab.is_damage() || ab.is_aoe_damage() || ab.is_cc()) {
            try { spell += ab.damage_amount(); } catch (const std::exception&) {}
        }
    }
    if (reset) dps *= 9.0f / 8.0f;
    if (me.champion_name() == cambc::champion_type::vendetta) dps *= 1.25f;
    dps += spell / 8.0f;
    if (has_any(counts, it::monster_emblem_of_strength, it::monster_emblem_of_health, it::monster_emblem_of_focus)) dps *= 1.1f;
    dps_ = std::max(10.0f, dps);
}

void Bot::write_claim(int group) {
    if (slot_ < 0 || slot_ > 4) return;
    std::uint8_t b = group >= 0 && group < 250 ? static_cast<std::uint8_t>(group + 1) : 0;
    cambc::shared_memory.write(static_cast<size_t>(slot_), std::span<const std::uint8_t>{&b, 1});
}

int Bot::claimed_by_other(int group) const {
    std::array<std::uint8_t, 5> b{};
    cambc::shared_memory.read(0, std::span<std::uint8_t>{b.data(), b.size()});
    for (int s = 0; s < 5; ++s)
        if (s != slot_ && b[static_cast<size_t>(s)] == group + 1) return s;
    return -1;
}

// ---------------------------------------------------------------- shopping

void Bot::shop() {
    auto& me = cambc::self();
    // An altar lock (10 gold) cancels an enemy tribute in progress: always carry one.
    if (kLocks && me.gold_count() >= 10 && fuel(15000) && live() && !has_lock()) {
        try {
            if (me.can_buy(cambc::consumable_type::altar_lock)) me.buy(cambc::consumable_type::altar_lock);
        } catch (const cambc::error& e) { warn("buy lock", e); }
    }
    if (me.gold_count() < 30) return;
    const auto& plan = build_for(me.champion_name());
    for (int guard = 0; guard < 30; ++guard) {
        if (me.gold_count() < 30 || !fuel(15000) || !live()) return;
        const auto counts = me.item_count();
        const Step* next = nullptr;
        std::array<int, static_cast<size_t>(it::enum_type::count)> need{};
        for (const auto& s : plan) {
            const auto e = static_cast<size_t>(static_cast<it::enum_type>(s.emblem));
            ++need[e];
            if (static_cast<int>(counts[s.emblem]) < need[e]) { next = &s; break; }
        }
        // Past the plan: power shards, alternating strength and health.
        const int str = static_cast<int>(counts[it::power_shard_of_strength] + 6 * counts[it::power_emblem_of_strength]);
        const int hp = static_cast<int>(counts[it::power_shard_of_health] + 6 * counts[it::power_emblem_of_health]);
        const it shard = next ? next->shard : (str <= hp ? it::power_shard_of_strength : it::power_shard_of_health);
        if (!me.can_buy(shard, 1)) { cant_buy_ = true; return; }
        try { me.buy(shard, 1); } catch (const cambc::error& e) { warn("buy", e); cant_buy_ = true; return; }
        cant_buy_ = false;
    }
}

// ---------------------------------------------------------------- movement

// Walk home when it is close, otherwise recall. True when the turn is used.
bool Bot::recall_or_walk(int walk_limit) {
    auto& me = cambc::self();
    if (cambc::map::spawn_region(team::ally).is_set(pos_)) return false;
    const int d = home_.at(pos_);
    if (d <= walk_limit) {
        go_home();
        return true;
    }
    if (moved_ || incoming_ > 0 || !live()) return moved_;
    try { me.recall(); } catch (const cambc::error& e) { warn("recall", e); }
    return true;
}

// The camp farming subsystem lives in camps.cpp.

// Step out of reach of awake camps.
bool Bot::escape_camps() {
    std::array<cambc::bitboard, 6> by_range;
    for (auto& b : by_range) b.clear();
    bool any = false;
    if (!awake_.is_empty())
        for (auto p : awake_.positions()) {
            if (cheb(p, pos_) > 7) continue;
            const int gi = group_at_[static_cast<size_t>(tidx(p))];
            const int ci = camp_at_[static_cast<size_t>(tidx(p))];
            if (gi < 0 || ci < 0) continue;
            const auto s = camp_spec(g_[gi].kinds[static_cast<size_t>(ci)]);
            if (s.str <= 0) continue;
            by_range[static_cast<size_t>(std::clamp(s.range, 0, 5))].set(p);
            any = true;
        }
    if (!any) return false;
    auto danger = empty_board();
    for (int r = 1; r <= 5; ++r)
        if (!by_range[static_cast<size_t>(r)].is_empty()) danger |= grow(by_range[static_cast<size_t>(r)], r);
    if (!danger.is_set(pos_)) return false;
    auto safe = empty_board();
    safe.fill();
    safe &= ~danger;
    safe &= ~walk_walls();
    return go(safe, true);
}

// ---------------------------------------------------------------- enemies

// Fight when the square law favours us (sum of health times sum of damage), run otherwise.
// True when the turn is spent.
bool Bot::enemies() {
    auto& me = cambc::self();
    const int now = static_cast<int>(cambc::round_number());
    auto list = foes(team::enemy);
    const auto& enemy_spawn = cambc::map::spawn_region(team::enemy);
    std::erase_if(list, [&](const Foe& e) { return enemy_spawn.is_set(e.pos) || cheb(e.pos, pos_) > 9; });
    if (list.empty()) {
        // Help an ally who reported an invader close by.
        const auto s = read_sighting();
        if (s.ok && s.age <= 4 && cheb(s.where, pos_) <= 10 && me.health() > me.max_health() * 0.6f
            && me.gem_count() < 15 && incoming_ == 0) {
            auto goal = empty_board();
            goal.set(s.where);
            goal = grow(goal, 2) & ~walk_walls();
            return go(goal, true);
        }
        return false;
    }
    last_enemy_seen_ = now;
    // Remember sightings for a few rounds: vision flickers and one turn's view undercounts.
    if (kFoeMemory) {
        const auto all = cambc::champions(team::enemy);
        if (foe_mem_.size() != all.size()) foe_mem_.assign(all.size(), Seen{});
        for (size_t i = 0; i < all.size(); ++i) {
            try {
                if (!all[i].is_visible()) continue;
                if (!all[i].is_alive()) { foe_mem_[i].round = -1000; continue; }
                foe_mem_[i] = {all[i].position(), all[i].health(), 1.1f * all[i].strength() + 0.25f * all[i].focus(), now};
            } catch (const cambc::error&) {}
        }
    }
    float their_hp = 0, their_dps = 0;
    int dmin = 99;
    if (kFoeMemory) {
        for (const auto& m : foe_mem_) {
            if (now - m.round > 3 || cheb(m.pos, pos_) > 9 || enemy_spawn.is_set(m.pos)) continue;
            their_hp += m.hp;
            their_dps += m.power;
        }
    } else {
        for (const auto& e : list) {
            their_hp += e.hp;
            // A tribute channel stops it acting (it only ends by finishing or cancelling); a recall breaks on our first hit.
            their_dps += kDeny && e.chan ? 0.2f * e.power : kDeny && e.recall ? 0.6f * e.power : e.power;
        }
    }
    const Foe* prey = nullptr;  // a channelling enemy worth hunting: its gems (and the kill) are ours
    if (kDeny)
        for (const auto& e : list)
            if ((e.chan || e.recall) && cheb(e.pos, pos_) <= 7 && (!prey || e.gems + (e.chan ? 30 : 0) > prey->gems + (prey->chan ? 30 : 0)))
                prey = &e;
    for (const auto& e : list) dmin = std::min(dmin, cheb(e.pos, pos_));
    float our_hp = me.health(), our_dps = 1.1f * me.strength() + 0.25f * me.focus();
    if (!kFoeMemory) {
        for (const auto& a : foes(team::ally))
            if (cheb(a.pos, pos_) <= 7) { our_hp += a.hp; our_dps += a.power; }
    } else {
        const int self_h = static_cast<int>(static_cast<cambc::handle_champion_t>(me));
        for (const auto& a : cambc::champions(team::ally)) {
            if (static_cast<int>(static_cast<cambc::handle_champion_t>(a)) == self_h) continue;
            try {
                if (!a.is_alive() || cheb(a.position(), pos_) > 7) continue;
                our_hp += a.health();
                our_dps += 1.1f * a.strength() + 0.25f * a.focus();
            } catch (const cambc::error&) {}
        }
    }
    write_sighting(list.front().pos);
    float ratio = our_hp * our_dps / std::max(1.0f, their_hp * their_dps);
    const int home_d = home_.at(pos_);
    if (home_d <= 3) ratio *= 1.5f;  // spawn heals us
    const bool home = cambc::map::spawn_region(team::ally).is_set(pos_);
    const int gems = static_cast<int>(me.gem_count());
    float need = (kHysteresis ? 1.1f : 1.0f) + std::min(1.0f, gems / 50.0f);  // carriers stay out of fights
    // Hysteresis: stick with the last decision unless the odds clearly changed.
    if (kHysteresis && flee_until_ >= now) need = 1.5f;
    else if (kHysteresis && fight_until_ >= now) need = 0.7f;
    if (kTrace && now < 300)
        std::cout << "fight r" << now << ' ' << cambc::to_string(me.champion_name()) << " hp " << me.health()
                  << " n " << list.size() << " dmin " << dmin << " ratio " << ratio << " need " << need << '\n';

    if (ratio < need && !home && kPressureTribute && gems > 0 && tribute_under_fire()) return true;
    if (ratio < need && !home) {
        // Far enemies that are not coming for us: carry on, but bank sooner (tribute checks sightings).
        if (dmin >= 6 && me.health() > me.max_health() * 0.5f && (!kHysteresis || flee_until_ < now)) return false;
        flee_until_ = now + 4;
        fight_until_ = -1;
        // Run: free hits and stuns on whoever is next to us (spells.cpp), recall once nobody is close.
        flee_spells(list);
        if (dmin >= 6 && !moved_ && incoming_ == 0) {
            if (live()) try { me.recall(); } catch (const cambc::error& ex) { warn("flee recall", ex); }
            return true;
        }
        go_home();
        return true;
    }
    // Strong enough but nobody close: keep farming unless the odds invite a hunt.
    if (dmin > 5 && ratio < 1.6f && !prey && (!kHysteresis || fight_until_ < now)) return false;
    // A chase that never lands a hit only wastes farming time.
    if (now < no_chase_until_ && dmin > me.attack_range() + 1) return false;
    fight_until_ = now + 3;
    flee_until_ = -1;

    // Fight: spells and basic attacks (spells.cpp), otherwise close in.
    if (!live()) return true;
    if (fight_spells(list)) { chase_turns_ = 0; return true; }
    if (cast_ && me.attack_range() > 1) return true;
    if (me.health() > me.max_health() * 0.3f) {
        if (++chase_turns_ > kChaseTurns) {
            chase_turns_ = 0;
            no_chase_until_ = now + 15;
            return false;
        }
        auto goal = empty_board();
        if (prey) goal.set(prey->pos);
        else for (const auto& e : list) goal.set(e.pos);
        goal = grow(goal, me.attack_range()) & ~walk_walls();
        go(goal, dmin > 2);
        if (strike_after_move(list)) chase_turns_ = 0;
        return true;
    }
    go_home();
    return true;
}

// ---------------------------------------------------------------- defense
//
// Gems only count once tributed, and a dead carrier hands them to its killer. A tribute channel is not
// broken by damage, only by a stun, death or an altar lock: so we bank under fire when the channel
// finishes before the enemies around can kill us, and we drop our own altar lock on any enemy tribute
// in reach (an enemy that channels cannot act, so it is also the first target for hits and stuns).

// Damage visible enemies can deal us over the next `turns` turns (they walk one tile a turn into range).
float Bot::foe_damage(float turns) {
    float dmg = 0;
    for (const auto& e : foes(team::enemy)) {
        if (e.chan) continue;
        const int reach = std::max(e.range, 4);  // spells reach 4
        const float late = static_cast<float>(std::max(0, cheb(e.pos, pos_) - reach));
        if (late >= turns) continue;
        dmg += (e.recall ? 0.5f : 1.0f) * (turns - late) * e.power;
    }
    return dmg;
}

// Next to an altar with gems while enemies close in: start the tribute if we live to see it finish.
bool Bot::tribute_under_fire() {
    auto& me = cambc::self();
    const int gems = static_cast<int>(me.gem_count());
    if (gems <= 0 || moved_ || !live()) return false;
    const auto locks = cambc::vision::altar_lock_tiles(team::enemy);
    for (auto a : altars_) {
        if (cheb(a, pos_) > cambc::constants::submit_gems_range || locks.is_set(a)) continue;
        const float turns = std::max(1.0f, std::ceil(gems / 10.0f)) + 1;
        if (me.health() <= incoming_ * taken_ * turns + foe_damage(turns) + 40) return false;
        try { me.submit_gems(a); } catch (const cambc::error& e) { warn("submit under fire", e); return false; }
        return true;
    }
    return false;
}

// An enemy tributing near us: lock its altar (cancels the channel) or walk into lock range in time.
bool Bot::deny() {
    if (!kLocks || !has_lock() || !live()) return false;
    auto& me = cambc::self();
    const Foe* aim = nullptr;
    auto list = foes(team::enemy);
    for (const auto& e : list)
        if (e.chan && e.gems >= 5 && cheb(e.pos, pos_) <= 9 && (!aim || e.gems > aim->gems)) aim = &e;
    if (!aim) return false;
    const auto locked = cambc::vision::altar_lock_tiles();
    vec2 target{-1, -1};
    int best = 99;
    for (auto a : altars_) {
        if (cheb(a, aim->pos) > cambc::constants::submit_gems_range || locked.is_set(a)) continue;
        const int d = cheb(a, pos_);
        if (d < best) { best = d; target = a; }
    }
    if (best > 9) return false;
    if (best <= kLockRange) {
        try {
            if (me.consumables()[cambc::consumable_type::altar_lock].use(target)) {
                invalidate();
                return false;  // the rest of the turn is free: fight on
            }
        } catch (const cambc::error& e) { warn("lock", e); }
        return false;
    }
    // Close enough to arrive before the channel ends?
    int left = static_cast<int>(std::ceil(aim->gems / 10.0f));
    try { left = cambc::vision::champion_at(aim->pos).remaining_channel_time(); } catch (const cambc::error&) {}
    if (best - kLockRange > left + 1) return false;
    if (me.health() < me.max_health() * 0.35f) return false;
    auto goal = empty_board();
    goal.set(target);
    goal = grow(goal, kLockRange) & ~walk_walls();
    if (!go(goal, true)) return false;
    // Arrived this step: drop it now.
    if (cheb(target, me.position()) <= kLockRange && live()) {
        try {
            if (me.consumables()[cambc::consumable_type::altar_lock].use(target)) invalidate();
        } catch (const cambc::error& e) { warn("lock2", e); }
    }
    return true;
}

// Our locks stop only enemy tributes: keep one on every altar we come near. The enemy then has to
// break it (five hits) or walk to another altar, and a channel already running there is cancelled.
// Returns true when the turn went into a detour towards an altar to lock.
bool Bot::lock_altars(bool detour, int reach) {
    if (!kLockAll || altars_.empty() || !live()) return false;
    auto& me = cambc::self();
    const int now = static_cast<int>(cambc::round_number());
    if (lock_seen_.size() != altars_.size()) lock_seen_.assign(altars_.size(), -1000);
    const auto& v = view();
    const auto ours = cambc::vision::altar_lock_tiles(team::ally);
    const auto any = cambc::vision::altar_lock_tiles();
    const auto theirs = cambc::vision::altar_lock_tiles(team::enemy);
    if (enemy_lock_.size() != altars_.size()) enemy_lock_.assign(altars_.size(), 0);
    for (size_t i = 0; i < altars_.size(); ++i) {
        const vec2 a = altars_[i];
        if (!v.lvis.at(a)) continue;
        if (ours.is_set(a)) lock_seen_[i] = now;
        else lock_seen_[i] = -1000;
        enemy_lock_[i] = theirs.is_set(a) ? 1 : 0;
    }
    if (!has_lock()) return false;
    int best = -1, best_d = 99;
    for (size_t i = 0; i < altars_.size(); ++i) {
        const vec2 a = altars_[i];
        if (lock_seen_[i] > -1000 && now - lock_seen_[i] < 150) continue;  // ours, as far as we know
        const int d = cheb(a, pos_);
        if (d <= kLockRange && v.lvis.at(a) && !any.is_set(a)) {
            try {
                if (me.consumables()[cambc::consumable_type::altar_lock].use(a)) {
                    lock_seen_[i] = now;
                    invalidate();
                    return false;
                }
            } catch (const cambc::error& e) { warn("lock altar", e); lock_seen_[i] = now; }
            return false;
        }
        if (d < best_d && !any.is_set(a)) { best_d = d; best = static_cast<int>(i); }
    }
    if (!detour || best < 0 || best_d > reach || incoming_ > 0 || moved_) return false;
    if (lock_try_g_ != best) { lock_try_g_ = best; lock_try_since_ = now; }
    if (now - lock_try_since_ > 2 * reach + 6) { lock_seen_[static_cast<size_t>(best)] = now; return false; }  // cannot get there
    auto goal = empty_board();
    goal.set(altars_[static_cast<size_t>(best)]);
    goal = grow(goal, kLockRange) & ~walk_walls();
    return go(goal, true);
}

// Some altar we have not seen our lock on lately (so a runner should fetch another lock).
bool Bot::lock_needed() const {
    const int now = static_cast<int>(cambc::round_number());
    if (lock_seen_.size() != altars_.size()) return true;
    for (size_t i = 0; i < altars_.size(); ++i)
        if (lock_seen_[i] <= -1000 || now - lock_seen_[i] >= 150) return true;
    return false;
}

// ---------------------------------------------------------------- gems

bool Bot::tribute() {
    auto& me = cambc::self();
    const int gems = static_cast<int>(me.gem_count());
    if (gems <= 0) return false;
    const auto locks = cambc::vision::altar_lock_tiles(team::enemy);
    for (auto a : cambc::map::altars().positions()) {
        if (cheb(a, pos_) > cambc::constants::submit_gems_range || locks.is_set(a)) continue;
        const float turns = std::ceil(gems / 10.0f) + 1;
        if (me.health() > incoming_ * taken_ * turns + foe_damage(turns) + 40) {
            if (live()) try { me.submit_gems(a); } catch (const cambc::error& e) { warn("submit", e); }
            return true;
        }
    }
    if (!locks.is_empty() && me.can_attack() && live())
        for (auto a : locks.positions())
            if (cheb(a, pos_) <= cambc::constants::submit_gems_range) {
                // A tribute at a locked altar becomes an attack on its lock (attack() itself refuses locks).
                try {
                    me.submit_gems(a);
                    attacked_ = true;
                    return true;
                } catch (const cambc::error& e) { warn("break lock", e); }
                break;
            }
    const int d = altar_.at(pos_);
    if (d >= kFar) return false;
    const int now = static_cast<int>(cambc::round_number());
    const int left = static_cast<int>(cambc::constants::max_rounds) - now;
    const auto s = read_sighting();
    const bool danger = (s.ok && s.age <= 30 && cheb(s.where, pos_) <= 16) || now - last_enemy_seen_ <= 20;
    const bool go_now = gems >= 45 || d <= 3 || (gems >= 20 && d <= 10) || (danger && gems >= 10)
        || target_ < 0 || target_wait_ > d || left < d + 25
        || cambc::global_submitted_gems(team::ally) + static_cast<unsigned>(gems) >= cambc::constants::winning_gems;
    if (!go_now) return false;
    if (kStats && now % 10 == 0)
        std::cout << "bank r" << now << ' ' << cambc::to_string(me.champion_name()) << " pos " << pos_.x << ',' << pos_.y
                  << " gems " << gems << " d " << d << " t " << target_ << " wait " << target_wait_ << '\n';
    // Far from every altar by road: the spawn is a recall away and close to one.
    if (d > home_altar_ + 10 && incoming_ == 0 && !moved_ && live()) {
        try { me.recall(); } catch (const cambc::error& e) { warn("bank recall", e); }
        return true;
    }
    auto goal = altar_spots_;
    for (auto a : locks.positions()) {
        auto one = empty_board();
        one.set(a);
        // An enemy lock close by: break it (five hits) rather than walk away and back as it drops out of sight.
        if (cheb(a, pos_) <= 4) {
            const auto hit = grow(one, cambc::constants::submit_gems_range) & ~walk_walls();
            if (!(hit & ~camp_block_).is_empty()) return go(hit & ~camp_block_, false);
        }
        goal &= ~grow(one, cambc::constants::submit_gems_range);
    }
    // Enemy locks we saw earlier (out of sight now): skip those altars, or break the nearest when all are locked.
    if (enemy_lock_.size() == altars_.size()) {
        int near = -1, near_d = 1 << 20;
        bool all = !altars_.empty();
        for (size_t i = 0; i < altars_.size(); ++i) {
            if (!enemy_lock_[i]) { all = false; continue; }
            auto one = empty_board();
            one.set(altars_[i]);
            goal &= ~grow(one, cambc::constants::submit_gems_range);
            const int d = cheb(altars_[i], pos_);
            if (d < near_d) { near_d = d; near = static_cast<int>(i); }
        }
        if (all && near >= 0) {
            auto one = empty_board();
            one.set(altars_[static_cast<size_t>(near)]);
            const auto hit = grow(one, cambc::constants::submit_gems_range) & ~walk_walls() & ~camp_block_;
            if (!hit.is_empty()) return go(hit, true);
        }
    }
    if ((goal & ~camp_block_).is_empty()) {
        // Every spot is under a living camp (a boss beside the only altar): clear it first.
        if (now < altar_wait_until_) return false;
        for (auto t : (goal & camp_block_).positions()) {
            const int gi = group_at_[static_cast<size_t>(tidx(t))];
            if (gi >= 0) {
                if (plug_ != gi) { plug_ = gi; plug_since_ = now; }
                plug_altar_ = true;
                break;
            }
        }
        return false;
    }
    return go(goal & ~camp_block_, true);
}

// ---------------------------------------------------------------- turn


void Bot::play_turn() {
    try {
        turn();
    } catch (const std::exception& e) {
        // Never let one failed action end the turn with an error; log where it happened.
        static int count = 0;
        if (++count <= 20) std::cout << "warn: turn r" << cambc::round_number() << " after " << g_phase_tag << ": " << e.what() << '\n';
    }
}

void Bot::turn() {
    ModeCount counter;
    g_mode = 0;
    g_f0 = cambc::fuel_remaining();
    g_fp_log.clear();
    g_round = cambc::round_number();
    auto& me = cambc::self();
    pos_ = me.position();
    if (!init_) {
        initialise();
        if (slot_ == 0 && kDebugMap) {
            Bits wl, ho, al, sa, se, bu;
            wl.load(cambc::map::walls()); ho.load(cambc::map::holes()); al.load(cambc::map::altars());
            sa.load(cambc::map::spawn_region(team::ally)); se.load(cambc::map::spawn_region(team::enemy)); bu.load(cambc::map::bushes());
            for (int y = map_h() - 1; y >= 0; --y) {
                std::string row = "map ";
                for (int x = 0; x < map_w(); ++x) {
                    const vec2 p{x, y};
                    char c = '.';
                    if (wl.at(p)) c = '#';
                    else if (ho.at(p)) c = 'o';
                    else if (al.at(p)) c = 'A';
                    else if (sa.at(p)) c = 'S';
                    else if (se.at(p)) c = 'E';
                    else if (bu.at(p)) c = ',';
                    const int gi = group_at_[static_cast<size_t>(tidx(p))];
                    if (gi >= 0) c = "wegGMK"[static_cast<int>(g_[gi].kinds[static_cast<size_t>(camp_at_[static_cast<size_t>(tidx(p))])])];
                    row += c;
                }
                std::cout << row << '\n';
            }
        }
    }
    moved_ = attacked_ = cast_ = move_failed_ = false;
    invalidate();
    if (kVisBench && now_dbg() % 100 == 50) {
        auto F = [] { return cambc::fuel_remaining(); };
        static cambc::bitboard* b1 = new cambc::bitboard;
        std::uint64_t t[12]; int k = 0;
        t[k++] = F();
        cambc::vision::camp_tiles_into(*b1);
        t[k++] = F();
        cambc::vision::visible_tiles_into(*b1);
        t[k++] = F();
        cambc::vision::visible_tiles_into(*b1);
        t[k++] = F();
        cambc::vision::champion_tiles_into(team::ally, *b1);
        t[k++] = F();
        cambc::vision::champion_tiles_into(team::enemy, *b1);
        t[k++] = F();
        cambc::vision::teleport_tiles_into(*b1);
        t[k++] = F();
        cambc::vision::awake_camp_tiles_into(*b1);
        t[k++] = F();
        { auto x = me.health(); (void)x; }
        t[k++] = F();
        invalidate(); (void)view();
        t[k++] = F();
        invalidate(); (void)view();
        t[k++] = F();
        { auto l = foes(team::enemy); (void)l; }
        t[k++] = F();
        { auto& vv = view(); int c = 0; for (int i = 0; i < 100; ++i) c += vv.lvis.at(vec2{i % 50, i / 50}); (void)c; }
        t[k++] = F();
        const char* names[] = {"camps", "vis1", "vis2", "ally", "enemy", "tele", "awake", "health", "view1", "view2", "foes", "lvis x100"};
        std::cout << "visbench r" << now_dbg();
        for (int i = 0; i + 1 < k; ++i) std::cout << ' ' << names[i] << '=' << (t[i] - t[i + 1]);
        std::cout << '\n';
    }

    if (!me.is_alive()) {
        g_mode = 0;
        write_claim(-1);
        shop();
        fill_fields(30000);
        return;
    }
    const bool at_spawn = cambc::map::spawn_region(team::ally).is_set(pos_);
    if (kTrace && now_dbg() == 50 && slot_ == 0) {
        auto f0 = cambc::fuel_remaining();
        std::uint32_t acc = 0;
        for (int i = 0; i < 100; ++i) acc += cambc::round_number();
        auto f1 = cambc::fuel_remaining();
        int c = 0;
        for (int i = 0; i < 1000; ++i) c += walls_local().at(vec2{i % 40, i / 40});
        auto f2 = cambc::fuel_remaining();
        for (int i = 0; i < 100; ++i) c += cambc::map::walls().is_set(vec2{i % 40, 3});
        auto f3 = cambc::fuel_remaining();
        auto vv = cambc::vision::visible_tiles();
        auto f4 = cambc::fuel_remaining();
        Bits bb; bb.load(vv);
        auto f5 = cambc::fuel_remaining();
        auto gr = grow(vv, 2);
        auto f6 = cambc::fuel_remaining();
        cambc::distance_field df; cambc::bitboard sn;
        df.compute(walk_walls(), cambc::map::spawn_region(team::ally), sn);
        auto f7 = cambc::fuel_remaining();
        std::cout << "bench round_number x100 " << (f0 - f1) << " Bits.at x1000 " << (f1 - f2) << " is_set x100 " << (f2 - f3)
                  << " visible_tiles " << (f3 - f4) << " Bits.load " << (f4 - f5) << " grow2 " << (f5 - f6) << " field " << (f6 - f7)
                  << ' ' << acc + c + gr.count() << '\n';
    }
    g_f0 = cambc::fuel_remaining();
    fp("start");
    if (at_spawn || me.champion_name() == cambc::champion_type::tavi) shop();
    fp("shop");
    observe();
    fp("observe");
    if (!live()) return;
    if (me.is_recalling() || me.is_submitting_gems() || me.is_teleporting()) {
        g_mode = 1;
        fill_fields(30000);
        return;
    }
    // Stunned: nothing to do this turn.
    if (!me.can_move() && !me.can_attack() && !me.can_cast()) return;
    measure();
    fp("measure");
    rebuild_camp_block();
    fp("block");
    if (!live()) return;
    g_mode = 2;
    try {
        if (deny()) return;
        lock_altars(false);
        if (enemies()) return;
        fp("enemies");
    } catch (const cambc::error& e) { warn("enemies", e); }
    g_mode = 3;
    if (!fuel(25000)) return;
    g_mode = 4;

    const float hp = me.health(), max_hp = me.max_health();
    if (!at_spawn) spawn_wait_ = 0;
    else if (hp < max_hp * 0.95f && spawn_wait_ < 2) { ++spawn_wait_; return; }

    // Badly hurt: leave camp range, then recall.
    g_mode = 5;
    if (!at_spawn && (hp < max_hp * 0.2f || (incoming_ > 0 && hp < incoming_ * taken_ * (exit_steps_ + 2) + 40))) {
        if (escape_camps()) return;
        if (incoming_ == 0 && recall_or_walk(4)) return;
    }

    const int now = static_cast<int>(cambc::round_number());
    if (target_ < 0 || now - chosen_at_ >= 4 || k_[target_].state == Known::respawning) {
        int wait = 0;
        fp("prechoose");
        const int t = choose(wait);
        fp("choose");
    if (!live()) return;
        if (t != target_) { stuck_g_ = t; stuck_since_ = now; best_eta_ = kFar; }
        target_ = t;
        target_wait_ = wait;
        chosen_at_ = now;
        write_claim(t);
    }
    if (plug_ >= 0) {
        float hits = 0;
        for (size_t c = 0; c < g_[plug_].kinds.size(); ++c)
            if (k_[plug_].hp[c] > 0) hits += camp_spec(g_[plug_].kinds[c]).str;
        // A boss beside the only altar guards every carried gem: raid it sooner than a boss in the way.
        const float need = plug_altar_ ? kBossTurns / 2 : kBossTurns;
        if (g_[plug_].boss && (max_hp - 100 - 0.1f * max_hp) / std::max(1.0f, hits * taken_) < need) {
            if (plug_altar_) altar_wait_until_ = now + 25;  // farm and shop meanwhile
            else if (target_ >= 0) {
                k_[target_].blocked_until = now + 60;
                target_ = -1;
            }
            plug_ = -1;
            if (!plug_altar_) return;
        }
        const auto st = k_[plug_].state;
        if (st == Known::respawning || st == Known::reward || now - plug_since_ > 40 || remaining(plug_) <= 0) plug_ = -1;
        else if (hp > max_hp * 0.4f) {
            farm(plug_);
            return;
        }
    }
    g_mode = 6;
    if (tribute()) return;
    fp("tribute");
    // The last pick runs locks to altars nobody passes (the enemy's own banks), between fights.
    const bool runner = kLockRunner && slot_ == size_ - 1 && size_ >= 3 && now < 1600
        && me.gem_count() == 0 && hp > max_hp * 0.6f;
    if (lock_altars(true, runner ? 60 : kLockDetour)) return;
    if (runner && !has_lock() && !at_spawn && incoming_ == 0 && me.gold_count() >= 10 && lock_needed()) {
        if (recall_or_walk(6)) return;
    }
    g_mode = 7;

    const int t = target_;
    if (t < 0 || target_wait_ > 15) {
        if (!at_spawn && (me.gold_count() >= 60 || hp < max_hp * 0.8f) && incoming_ == 0 && me.gem_count() == 0) {
            recall_or_walk(5);
            return;
        }
        if (t < 0) {
            fill_fields(30000);
            return;
        }
    }
    // Gold buys nothing in the field: go and spend a full purse between fights.
    g_mode = 8;
    if (!at_spawn && me.gold_count() >= 240 && me.gem_count() == 0 && incoming_ == 0 && !cant_buy_
        && me.champion_name() != cambc::champion_type::tavi
        && (k_[t].state != Known::alive || eta(t) > 6) && remaining(t) >= g_[t].hp * 0.99f) {
        if (recall_or_walk(8)) return;
    }
    // Heal first when the next fight would not finish at this health.
    g_mode = 9;
    if (!at_spawn && hp < max_hp * 0.5f && incoming_ == 0 && k_[t].state != Known::reward
        && estimate_damage(t) > hp - 100) {
        if (recall_or_walk(5)) return;
    }
    // No progress towards the target for a long time: give it up for a while.
    g_mode = 10;
    {
        const int e = eta(t);
        if (e < best_eta_ || e <= 3 || attacked_) stuck_since_ = now;
        best_eta_ = e;
        if (now - stuck_since_ > 20) {
            k_[t].blocked_until = now + 40;
            target_ = -1;
            return;
        }
    }
    if (kStats && now % 100 == 0)
        std::cout << "dbg r" << now << ' ' << cambc::to_string(me.champion_name()) << " pos " << pos_.x << ',' << pos_.y
                  << " t " << t << " eta " << eta(t) << " st " << int(k_[t].state) << " plug " << plug_
                  << " ctr " << g_[t].center.x << ',' << g_[t].center.y << " hp " << hp << '\n';
    fp("prefarm");
    if (!live()) return;
    farm(t);
    fp("farm");
    fill_fields(40000);
}
