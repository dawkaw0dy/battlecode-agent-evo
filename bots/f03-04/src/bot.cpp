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
std::uint64_t g_f0 = 0;
const char* g_phase_tag = "";
int now_dbg() { return static_cast<int>(cambc::round_number()); }
std::string g_fp_log;
void fp(const char* tag) {
    g_phase_tag = tag;
    if (!kTrace) return;
    const auto f = cambc::fuel_remaining();
    if (g_f0 > f && g_f0 - f > 3000) { g_fp_log += ' '; g_fp_log += tag; g_fp_log += '='; g_fp_log += std::to_string((g_f0 - f) / 1000); }
    g_f0 = f;
}
constexpr float kShareTurns = 15.0f;
constexpr int kChaseTurns = 5;      // approach turns without a hit before giving up a chase
constexpr float kEngageBias = -150.0f;  // fight value needed to engage
constexpr float kStick = 0.0f;       // fight value margin in favour of the last decision
constexpr float kBossTurns = 8.0f;  // turns a champion must be able to tank a boss

namespace {
// Debug: turns spent per activity, printed every 200 rounds.
int g_mode = 0;
std::array<int, 20> g_modes{};
const char* kModeNames[20] = {"dead", "chan", "enemy", "fuel", "spwait", "hurt", "bank", "idle", "shop", "heal",
    "stuck", "travel", "fight", "capture", "respawn", "approach", "rally", "", "", ""};
struct ModeCount {
    std::uint64_t f0 = kStats ? cambc::fuel_remaining() : 0;
    ~ModeCount() {
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

namespace {

using it = cambc::item_type;

// A turn that ran out of fuel resumes next round on stale data: act only in the round we started.
std::uint32_t g_round = 0;
bool live() { return cambc::round_number() == g_round; }
bool fuel(std::uint64_t need) { return cambc::fuel_remaining() > need; }

void warn(const char* where, const std::exception& e) {
    static int count = 0;
    if (++count <= 20) std::cout << "warn: " << where << ": " << e.what() << '\n';
}

// Vision snapshot, refreshed after anything that changes what we see.
struct View {
    cambc::bitboard vis, camps, champs, tele;
    Bits lvis, lcamps, lchamps, ltele;
    std::vector<vec2> allies;  // allied positions, including ones hidden in bushes
    std::vector<cambc::champion_type> ally_types;  // ... and who stands there
    bool ok = false;
};
View g_view;
void invalidate() { g_view.ok = false; }
const View& view() {
    if (!g_view.ok) {
        g_view.vis = cambc::vision::visible_tiles();
        g_view.camps = cambc::vision::camp_tiles();
        g_view.champs = cambc::vision::champion_tiles();
        g_view.tele = cambc::vision::teleport_tiles();
        g_view.allies.clear();
        g_view.ally_types.clear();
        static std::vector<cambc::champion> others;  // allied handles other than ours, built once
        static bool built = false;
        if (!built) {
            built = true;
            const int me = static_cast<int>(static_cast<cambc::handle_champion_t>(cambc::self()));
            for (const auto& a : cambc::champions(team::ally))
                if (static_cast<int>(static_cast<cambc::handle_champion_t>(a)) != me) others.push_back(a);
        }
        for (const auto& a : others) {
            try {
                if (!a.is_alive()) continue;
                const vec2 p = a.position();
                if (inside(p)) {
                    g_view.allies.push_back(p);
                    g_view.ally_types.push_back(a.champion_name());
                    g_view.champs.set(p);
                }
            } catch (const cambc::error&) {}
        }
        g_view.lvis.load(g_view.vis);
        g_view.lcamps.load(g_view.camps);
        g_view.lchamps.load(g_view.champs);
        g_view.ltele.load(g_view.tele);
        g_view.ok = true;
    }
    return g_view;
}

Bits g_walls_local, g_hard_local;  // filled in initialise()
const Bits& walls_local() { return g_walls_local; }
const Bits& hard_local() { return g_hard_local; }  // what a dash cannot cross

bool free_tile(vec2 p) {
    if (!inside(p)) return false;
    if (p == cambc::self().position()) return true;
    const auto& v = view();
    return !walls_local().at(p) && !v.lcamps.at(p) && !v.lchamps.at(p) && !v.ltele.at(p);
}

cambc::bitboard blocked_now(const cambc::bitboard& camps) {
    const auto& v = view();
    auto b = walk_walls() | camps | v.camps | v.champs | v.tele;
    b.set(cambc::self().position(), false);
    return b;
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

// Shared memory: bytes 0-4 hold each slot's target group + 1; bytes 5-7 the latest enemy sighting.
struct Sighting { bool ok = false; vec2 where{}; int age = 999; };
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

}  // namespace

// ---------------------------------------------------------------- setup

void Bot::initialise() {
    init_ = true;
    init_dims();
    g_walls_local.load(walk_walls());
    g_hard_local.load(cambc::map::walls() | cambc::map::altars());
    auto& me = cambc::self();
    g_ = analyse_map();
    h_ = cambc::map::camp_groups();
    k_.assign(g_.size(), Known{});
    for (size_t i = 0; i < g_.size(); ++i)
        for (auto kind : g_[i].kinds) k_[i].hp.push_back(camp_spec(kind).hp);
    gfield_.assign(g_.size(), Dist{});
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
    for (const auto& ab : me.abilities())
        if (ab.is_dash()) dasher_ = true;
    my_range_ = me.attack_range();
    build_threats();
    spawn_local_.load(cambc::map::spawn_region(team::ally));
    {
        const int self_h = static_cast<int>(static_cast<cambc::handle_champion_t>(me));
        for (const auto& a : cambc::champions(team::ally))
            if (static_cast<int>(static_cast<cambc::handle_champion_t>(a)) != self_h) others_.push_back(a);
    }

    cambc::distance_field f;
    cambc::bitboard seen;
    f.compute(walk_walls(), cambc::map::spawn_region(team::ally), seen);
    home_.load(f);
    auto spots = empty_board();
    for (auto a : cambc::map::altars().positions()) {
        auto one = empty_board();
        one.set(a);
        spots |= grow(one, cambc::constants::submit_gems_range);
    }
    spots &= ~walk_walls();
    f.compute(walk_walls(), spots, seen);
    altar_.load(f);
    altar_spots_ = spots;
    for (auto t : cambc::map::spawn_region(team::ally).positions()) home_altar_ = std::min(home_altar_, altar_.at(t));
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

void Bot::fill_fields(std::uint64_t reserve) {
    for (int i : fill_order_) {
        if (!gfield_[i].empty()) continue;
        if (!fuel(reserve)) return;
        cambc::distance_field f;
        cambc::bitboard seen;
        f.compute(walk_walls(), gtiles_[i], seen);
        gfield_[i].load(f);
    }
}

int Bot::eta(int group) const {
    if (!gfield_[group].empty()) {
        const int d = gfield_[group].at(pos_);
        return d >= kFar ? kFar : d;
    }
    if (g_[group].d_ally >= kFar) return kFar;
    return cheb(pos_, g_[group].center) * 5 / 4 + 2;
}

// ---------------------------------------------------------------- knowledge

void Bot::observe() {
    const int now = static_cast<int>(cambc::round_number());
    for (auto& k : k_)
        if (k.state == Known::respawning && now >= k.ready) { k.state = Known::unknown; block_dirty_ = true; }
    const auto& v = view();
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

bool Bot::try_dash(const cambc::distance_field& f, const cambc::bitboard& seen) {
    auto& me = cambc::self();
    if (cast_ || !dasher_ || !me.can_cast() || !fuel(12000)) return false;
    fp("dash-0");
    const vec2 pos = me.position();
    if (!seen.is_set(pos)) return false;
    const int here = f.get(pos);
    fp("dash-1");
    for (const auto& ab : me.abilities()) {
        if (!ab.is_dash() || !ab.is_ready()) continue;
        std::array<std::pair<int, vec2>, 16> cand;
        size_t n = 0;
        for (int dx = -2; dx <= 2; ++dx)
            for (int dy = -2; dy <= 2; ++dy) {
                if (std::max(std::abs(dx), std::abs(dy)) != 2) continue;
                const vec2 t{pos.x + dx, pos.y + dy};
                if (!free_tile(t) || !seen.is_set(t)) continue;
                const int d = f.get(t);
                if (d > here - 2) continue;
                // Need a passable middle tile (holes may be jumped, walls not).
                bool path = false;
                for (auto s : kDirs) {
                    const vec2 m{pos.x + s.x, pos.y + s.y};
                    if (!inside(m) || cheb(m, t) != 1) continue;
                    if (hard_local().at(m) || view().lchamps.at(m) || view().lcamps.at(m)) continue;
                    path = true;
                    break;
                }
                if (path) cand[n++] = {d, t};
            }
        fp("dash-sort");
        std::sort(cand.begin(), cand.begin() + n, [](auto a, auto b) { return a.first < b.first; });
        for (size_t i = 0; i < n && i < 3; ++i) {
            if (!live()) return false;
            try {
                fp("dash-use");
                const bool ok = ab.use(cand[i].second);
                fp("dash-used");
                cast_ = true;
                invalidate();
                if (!ok) move_failed_ = true;
                return ok;
            } catch (const cambc::error&) {
                // The engine's dash path rules can be stricter than ours: try the next landing.
            }
        }
        return false;
    }
    return false;
}

// One step (and a dash when it gains two) along the shortest route around known camps and
// visible units. When only living camps close the way, remember their group in plug_.
bool Bot::go(const cambc::bitboard& goals_in, bool dash) {
    auto& me = cambc::self();
    if (moved_ || move_failed_ || !me.can_move() || !live()) return false;
    if (!fuel(25000)) return false;
    const vec2 pos = me.position();
    auto blocked = blocked_now(camp_block_);
    auto goals = goals_in & ~blocked;
    if (goals.is_empty() || goals.is_set(pos)) return false;
    cambc::distance_field f;
    cambc::bitboard seen;
    auto here = empty_board();
    here.set(pos);
    fp("go-compute");
    f.compute_to(blocked, goals, here, seen);
    fp("go-computed");
    if (!seen.is_set(pos)) {
        // Camps (or units) close the way: find what plugs the walk-only route.
        cambc::distance_field w;
        cambc::bitboard wseen;
        fp("go-fallback");
        w.compute_to(walk_walls(), goals_in & ~walk_walls(), here, wseen);
        fp("go-plugwalk");
        if (!wseen.is_set(pos)) return false;
        // Follow that route through tiles until the first camp on it (on a local copy: host reads add up).
        Dist wl;
        wl.load(w);
        vec2 at = pos;
        for (int steps = 0; steps < 60; ++steps) {
            vec2 next = at;
            int nd = wl.at(at);
            for (auto d : kDirs) {
                const vec2 t{at.x + d.x, at.y + d.y};
                const int fd = wl.at(t);
                if (fd < nd && !walls_local().at(t)) { nd = fd; next = t; }
            }
            if (next == at) break;
            const int gi = group_at_[static_cast<size_t>(tidx(next))];
            if (gi >= 0 && camp_block_.is_set(next)) {
                if (plug_ != gi) { plug_ = gi; plug_since_ = static_cast<int>(cambc::round_number()); plug_altar_ = false; }
                return false;
            }
            at = next;
        }
        // Units in the gap: walk the plain route and wait there for them to move.
        cambc::distance_field plain;
        cambc::bitboard plain_seen;
        plain.compute_to(walk_walls(), goals_in & ~walk_walls(), here, plain_seen);
        return step_on(plain, plain_seen, goals, blocked, dash);
    }
    return step_on(f, seen, goals, blocked, dash);
}

// Dash and/or walk one step down field f (reached tiles in seen) towards goals.
bool Bot::step_on(const cambc::distance_field& f, const cambc::bitboard& seen, const cambc::bitboard& goals,
                  const cambc::bitboard& blocked, bool dash) {
    auto& me = cambc::self();
    const vec2 pos = me.position();
    fp("go-dash");
    if (!live() || !inside(pos)) return false;
    if (dash && f.get(pos) >= 3 && try_dash(f, seen)) {
        fp("go-dashed");
        moved_ = true;
        if (move_failed_ || !me.can_move()) return true;
    }
    const vec2 from = me.position();
    if (goals.is_set(from)) return true;
    fp("go-step");
    vec2 best = from;
    int best_d = seen.is_set(from) ? f.get(from) : kFar;
    for (auto d : kDirs) {
        const vec2 t{from.x + d.x, from.y + d.y};
        if (!free_tile(t) || !seen.is_set(t) || blocked.is_set(t)) continue;
        const int fd = f.get(t);
        if (fd < best_d) { best_d = fd; best = t; }
    }
    if (best == from || !live()) return moved_;
    moved_ = true;
    bool ok = false;
    try { ok = me.move_absolute(best); } catch (const cambc::error& e) { warn("move", e); }
    invalidate();
    if (!ok) move_failed_ = true;
    return ok;
}

void Bot::go_home() { go(cambc::map::spawn_region(team::ally), true); }

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

// ---------------------------------------------------------------- camps
//
// Camp farming. A group is worth gems + w·gold (w falls over the game) per champion-turn spent on it:
// travel, respawn wait, clear time, the trips home to heal when one health bar is not enough, and the
// capture. Clears are costed with a damage model: camps die in the order that hurts least (threat per
// health first, harmless clusters last) and a camp only hurts us while it reaches the side of the target we
// hit from, which is where approach_goal() puts us. Groups one champion can wear down are farmed alone (a
// heavy one an ally works on may be shared); bosses and packs too strong for that are raided: the raid
// size with the best rate is planned, raiders gather just outside the fighters' reach and engage together
// (a camp hits the nearest champion, so the tank rotates as the hurt ones step out), and they all stand on
// the region at capture so a big reward is split and banks in short tributes.

namespace {
constexpr float kCaptureTurns = 6.0f;  // five capture phases and the step onto the region
constexpr float kTripTurns = 8.0f;     // recall and the heal on spawn, before walking back
constexpr int kRallyRounds = 35;       // longest wait for raiders outside a boss's reach
constexpr float kGatherTurns = 10.0f;  // expected wait for raiders not yet on their way
}  // namespace

void Bot::build_threats() {
    threat_.assign(g_.size(), {});
    for (size_t i = 0; i < g_.size(); ++i) {
        const auto& g = g_[i];
        const size_t n = g.tiles.size();
        auto& t = threat_[i];
        t.assign(n * n, 0);
        for (size_t m = 0; m < n; ++m) {
            const auto s = camp_spec(g.kinds[m]);
            if (s.str <= 0) continue;
            for (size_t c = 0; c < n; ++c)
                if (m == c || cheb(g.tiles[m], g.tiles[c]) + my_range_ <= s.range) t[m * n + c] = 1;
        }
    }
    cost_.assign(g_.size(), ClearCost{});
}

float Bot::remaining(int group) const {
    float t = 0;
    for (float hp : k_[group].hp) t += hp;
    return t;
}

// Σ hp·incoming over the kill order (divide by team damage for the damage taken) and the worst incoming.
const Bot::ClearCost& Bot::clear_cost(int group) {
    auto& cc = cost_[group];
    const float left = remaining(group);
    if (cc.hp_sum == left) return cc;
    cc.hp_sum = left;
    const auto& g = g_[group];
    const auto& k = k_[group];
    const size_t n = g.tiles.size();
    std::array<std::pair<float, std::uint8_t>, 64> order;
    std::array<std::uint8_t, 64> alive{};
    size_t m = 0;
    for (size_t c = 0; c < n && c < 64; ++c) {
        if (k.hp[c] <= 0) continue;
        alive[c] = 1;
        order[m++] = {camp_spec(g.kinds[c]).str / std::max(1.0f, k.hp[c]), static_cast<std::uint8_t>(c)};
    }
    std::sort(order.begin(), order.begin() + m, [](auto a, auto b) { return a.first > b.first; });
    const auto& t = threat_[group];
    cc.base = 0;
    cc.peak = 0;
    for (size_t j = 0; j < m; ++j) {
        const size_t c = order[j].second;
        float rate = 0;
        for (size_t o = 0; o < n && o < 64; ++o)
            if (alive[o] && t[o * n + c]) rate += camp_spec(g.kinds[o]).str;
        cc.base += k.hp[c] * rate;
        cc.peak = std::max(cc.peak, rate);
        alive[c] = 0;
    }
    return cc;
}

// Damage each of n raiders takes clearing the group (the hurt one steps out and the next nearest tanks).
float Bot::estimate_damage(int group) {
    const int n = group == target_ && raid_n_ > 1 ? raid_n_ : 1;
    return clear_cost(group).base / (dps_ * n) * taken_ / n;
}

// Tiles within r of a living camp of the group, keeping the ones the fewest fighters reach (we hit from
// the side away from the others). Cached while the living set stays the same.
const cambc::bitboard& Bot::approach_goal(int group, int r) {
    int sig = 0;
    for (size_t c = 0; c < k_[group].hp.size(); ++c)
        if (k_[group].hp[c] > 0) sig = sig * 31 + static_cast<int>(c) + 1;
    const int now = static_cast<int>(cambc::round_number());
    const bool plain = plain_until_ > now;
    if (goal_group_ == group && goal_r_ == r && goal_sig_ == sig && goal_plain_ == plain) return goal_;
    goal_group_ = group;
    goal_r_ = r;
    goal_sig_ = sig;
    goal_plain_ = plain;
    const auto& g = g_[group];
    const auto& k = k_[group];
    const size_t n = g.tiles.size();
    // Box around the living camps.
    int x0 = map_w(), y0 = map_h(), x1 = -1, y1 = -1, hostile = 0, living = 0;
    for (size_t c = 0; c < n; ++c) {
        if (k.hp[c] <= 0) continue;
        ++living;
        if (camp_spec(g.kinds[c]).str > 0) ++hostile;
        x0 = std::min(x0, g.tiles[c].x); y0 = std::min(y0, g.tiles[c].y);
        x1 = std::max(x1, g.tiles[c].x); y1 = std::max(y1, g.tiles[c].y);
    }
    const int bw = x1 - x0 + 2 * r + 1, bh = y1 - y0 + 2 * r + 1;
    const long work = static_cast<long>(living) * (2 * r + 1) * (2 * r + 1) + static_cast<long>(hostile) * 121 + 2L * bw * bh;
    if (living == 0 || plain || hostile == 0 || work > 2500 || !fuel(60000 + 40 * static_cast<std::uint64_t>(work))) {
        auto live_b = empty_board();
        for (size_t c = 0; c < n; ++c)
            if (k.hp[c] > 0) live_b.set(g.tiles[c]);
        if (live_b.is_empty()) live_b = gtiles_[group];
        goal_ = grow(live_b, r) & ~walk_walls();
        return goal_;
    }
    const int bx = x0 - r, by = y0 - r;
    // reach[i]: 1 when the tile is within r of a living camp; danger[i]: strength of fighters reaching it.
    std::vector<std::uint8_t> reach(static_cast<size_t>(bw * bh), 0);
    std::vector<std::int16_t> danger(static_cast<size_t>(bw * bh), 0);
    for (size_t c = 0; c < n; ++c) {
        if (k.hp[c] <= 0) continue;
        const vec2 p = g.tiles[c];
        for (int y = p.y - r; y <= p.y + r; ++y) {
            std::uint8_t* row = reach.data() + (y - by) * bw + (p.x - r - bx);
            for (int x = 0; x <= 2 * r; ++x) row[x] = 1;
        }
    }
    for (size_t c = 0; c < n; ++c) {
        if (k.hp[c] <= 0) continue;
        const auto s = camp_spec(g.kinds[c]);
        if (s.str <= 0) continue;
        const vec2 p = g.tiles[c];
        const int ya = std::max(by, p.y - s.range), yb = std::min(by + bh - 1, p.y + s.range);
        const int xa = std::max(bx, p.x - s.range), xb = std::min(bx + bw - 1, p.x + s.range);
        const auto add = static_cast<std::int16_t>(s.str);
        for (int y = ya; y <= yb; ++y) {
            std::int16_t* row = danger.data() + (y - by) * bw;
            for (int x = xa; x <= xb; ++x) row[x - bx] = static_cast<std::int16_t>(row[x - bx] + add);
        }
    }
    // Living camps and walls are not standing spots.
    for (size_t c = 0; c < n; ++c)
        if (k.hp[c] > 0) reach[static_cast<size_t>((g.tiles[c].y - by) * bw + g.tiles[c].x - bx)] = 0;
    int best = 1 << 30;
    for (int y = 0; y < bh; ++y)
        for (int x = 0; x < bw; ++x) {
            const size_t i = static_cast<size_t>(y * bw + x);
            if (!reach[i]) continue;
            const vec2 p{bx + x, by + y};
            if (!inside(p) || walls_local().at(p)) { reach[i] = 0; continue; }
            best = std::min<int>(best, danger[i]);
        }
    goal_buf_.assign(cambc::bitboard::linear_buffer_size(), 0);
    bool any = false;
    for (int y = 0; y < bh; ++y)
        for (int x = 0; x < bw; ++x) {
            const size_t i = static_cast<size_t>(y * bw + x);
            if (!reach[i] || danger[i] > best) continue;
            const int t = tidx(vec2{bx + x, by + y});
            goal_buf_[static_cast<size_t>(t >> 3)] |= static_cast<std::uint8_t>(1u << (t & 7));
            any = true;
        }
    if (!any) {
        goal_plain_ = true;
        auto live_b = empty_board();
        for (size_t c = 0; c < n; ++c)
            if (k.hp[c] > 0) live_b.set(g.tiles[c]);
        goal_ = grow(live_b, r) & ~walk_walls();
        return goal_;
    }
    goal_.import_linear_buffer(std::span<const std::uint8_t>{goal_buf_});
    return goal_;
}

// Tiles one or two steps outside the reach of the group's living fighters: where raiders gather.
// Built on a local box (whole-board dilations by a boss's reach cost tens of thousands of fuel).
const cambc::bitboard& Bot::rally_goal(int group) {
    if (rally_goal_g_ == group && rally_goal_round_ + 10 > static_cast<int>(cambc::round_number())) return rally_goal_;
    rally_goal_g_ = group;
    rally_goal_round_ = static_cast<int>(cambc::round_number());
    const auto& g = g_[group];
    const auto& k = k_[group];
    int x0 = map_w(), y0 = map_h(), x1 = -1, y1 = -1;
    for (size_t c = 0; c < g.tiles.size(); ++c) {
        if (k.hp[c] <= 0 || camp_spec(g.kinds[c]).str <= 0) continue;
        x0 = std::min(x0, g.tiles[c].x); y0 = std::min(y0, g.tiles[c].y);
        x1 = std::max(x1, g.tiles[c].x); y1 = std::max(y1, g.tiles[c].y);
    }
    rally_goal_ = empty_board();
    if (x1 < 0) return rally_goal_;
    const int pad = 7;  // largest reach (5) plus the ring
    const int bx = x0 - pad, by = y0 - pad, bw = x1 - x0 + 2 * pad + 1, bh = y1 - y0 + 2 * pad + 1;
    // 2: inside some fighter's reach, 1: in the ring just outside it.
    std::vector<std::uint8_t> mark(static_cast<size_t>(bw * bh), 0);
    for (size_t c = 0; c < g.tiles.size(); ++c) {
        if (k.hp[c] <= 0) continue;
        const auto s = camp_spec(g.kinds[c]);
        if (s.str <= 0) continue;
        const vec2 p = g.tiles[c];
        for (int y = p.y - s.range - 2; y <= p.y + s.range + 2; ++y) {
            std::uint8_t* row = mark.data() + (y - by) * bw;
            const int dy = y > p.y ? y - p.y : p.y - y;
            for (int x = p.x - s.range - 2; x <= p.x + s.range + 2; ++x) {
                const int dx = x > p.x ? x - p.x : p.x - x;
                const std::uint8_t v = (dx > dy ? dx : dy) <= s.range ? 2 : 1;
                if (row[x - bx] < v) row[x - bx] = v;
            }
        }
    }
    goal_buf_.assign(cambc::bitboard::linear_buffer_size(), 0);
    for (int y = 0; y < bh; ++y)
        for (int x = 0; x < bw; ++x) {
            if (mark[static_cast<size_t>(y * bw + x)] != 1) continue;
            const vec2 p{bx + x, by + y};
            if (!inside(p) || walls_local().at(p)) continue;
            const int t = tidx(p);
            goal_buf_[static_cast<size_t>(t >> 3)] |= static_cast<std::uint8_t>(1u << (t & 7));
        }
    rally_goal_.import_linear_buffer(std::span<const std::uint8_t>{goal_buf_});
    return rally_goal_;
}

// Far from the group: one step down its cached walking field instead of a fresh route search (a host
// BFS each turn is most of our fuel). Not while a dash is ready (the routed walk uses it), and not for a
// while after the static step was blocked (mixing the two makes champions flip-flop).
bool Bot::walk_static(int group, int far) {
    auto& me = cambc::self();
    const int now = static_cast<int>(cambc::round_number());
    if (far <= 6 || now < static_off_until_ || moved_ || move_failed_ || !live() || !me.can_move()) return false;
    const auto& f = gfield_[group];
    if (f.empty()) return false;
    if (dasher_ && !cast_ && me.can_cast())
        for (const auto& ab : me.abilities())
            if (ab.is_dash() && ab.is_ready()) return false;
    const vec2 pos = me.position();
    vec2 best = pos;
    int best_d = f.at(pos);
    if (best_d >= kFar) return false;
    for (auto d : kDirs) {
        const vec2 t{pos.x + d.x, pos.y + d.y};
        const int fd = f.at(t);
        if (fd >= best_d || !free_tile(t) || camp_block_.is_set(t)) continue;
        best_d = fd;
        best = t;
    }
    if (best == pos) { static_off_until_ = now + 12; return false; }
    moved_ = true;
    bool ok = false;
    try { ok = me.move_absolute(best); } catch (const cambc::error& e) { warn("move", e); }
    invalidate();
    if (!ok) { move_failed_ = true; static_off_until_ = now + 12; }
    return true;
}

bool Bot::group_awake(int group) const { return !(awake_ & gtiles_[group]).is_empty(); }

// Champions (us included) close enough to the group to join its fight within a few turns.
int Bot::raiders_near(int group) const {
    int n = 1;
    const int r = reach_[group] + g_[group].range + 4;
    for (auto a : view().allies)
        if (cheb(a, g_[group].center) <= r) ++n;
    return n;
}

bool Bot::hit_group(int group) {
    auto& me = cambc::self();
    if (!me.can_attack() || !live()) return false;
    const vec2 pos = me.position();
    const int range = me.attack_range();
    const float str = me.strength();
    float best_score = -1e9f;
    vec2 best{};
    bool found = false;
    const auto& v = view();
    for (size_t c = 0; c < g_[group].tiles.size(); ++c) {
        const vec2 p = g_[group].tiles[c];
        if (cheb(p, pos) > range || !v.lvis.at(p) || !v.lcamps.at(p)) continue;
        const float hp = std::max(1.0f, k_[group].hp[c]);
        const auto s = camp_spec(g_[group].kinds[c]);
        // Most strength removed per hit first; a camp that reaches us outranks one that does not.
        float score = s.str / hp * 1e4f - hp * 1e-3f;
        if (s.str > 0 && cheb(p, pos) <= s.range) score += 50;
        if (hp <= str) score += 1e6f;  // a kill this hit
        if (score > best_score) { best_score = score; best = p; found = true; }
    }
    if (!found) return false;
    try {
        auto c = cambc::vision::camp_at(best);
        me.attack(c);
        attacked_ = true;
        invalidate();
        const int ci = camp_at_[static_cast<size_t>(tidx(best))];
        if (ci >= 0) k_[group].hp[static_cast<size_t>(ci)] = std::max(1.0f, k_[group].hp[static_cast<size_t>(ci)] - str);
        return true;
    } catch (const cambc::error& e) {
        warn("hit", e);
        return false;
    }
}

void Bot::reset_hit(int group) {
    auto& me = cambc::self();
    if (!attacked_ || cast_ || !fuel(8000) || !live() || me.can_attack() || !me.can_cast()) return;
    for (const auto& ab : me.abilities()) {
        if (!ab.is_attack_reset() || !ab.is_ready()) continue;
        cast_ = true;
        try { ab.use(); } catch (const cambc::error& e) { warn("reset", e); return; }
        if (me.can_attack()) hit_group(group);
        return;
    }
}

// Damage spells on camps; shield while under fire. Cast before walking.
void Bot::spells_on_group(int group) {
    auto& me = cambc::self();
    if (cast_ || moved_ || !live() || !me.can_cast()) return;
    const auto& v = view();
    for (const auto& ab : me.abilities()) {
        if (!ab.is_ready()) continue;
        if (ab.is_shield()) {
            if (incoming_ > 0 && me.health() < me.max_health() * 0.9f) {
                cast_ = true;
                try { ab.use(); } catch (const cambc::error& e) { warn("shield", e); }
                return;
            }
            continue;
        }
        if (!(ab.is_damage() || ab.is_cc() || ab.is_aoe_damage())) continue;
        if (!ab.can_target_camps() && !ab.can_target_ground()) continue;
        const int r = static_cast<int>(ability_range(ab));
        vec2 best{};
        float best_score = -1;
        for (size_t c = 0; c < g_[group].tiles.size(); ++c) {
            const vec2 p = g_[group].tiles[c];
            if (cheb(p, me.position()) > r || !v.lvis.at(p) || !v.lcamps.at(p)) continue;
            const float hp = k_[group].hp[c];
            if (hp <= 0) continue;
            float score = std::min(hp, 400.0f) + camp_spec(g_[group].kinds[c]).str;
            if (ab.is_aoe_damage())
                for (const auto& o : g_[group].tiles)
                    if (o != p && cheb(o, p) <= 1 && v.lcamps.at(o)) score += 300;
            if (score > best_score) { best_score = score; best = p; }
        }
        if (best_score < 0) continue;
        try {
            cast_ = true;
            if (ab.is_aoe_damage() || !ab.can_target_camps()) ab.use(best);
            else ab.use(cambc::vision::camp_at(best));
            invalidate();
        } catch (const cambc::error& e) { warn("spell", e); }
        return;
    }
}

namespace {
// Best plan for one group: champion-turn rate, raid size and whether raiders gather first.
struct Plan { float score = 0; int n = 1; bool rally = false; };
}  // namespace

int Bot::choose(int& wait_out) {
    auto& me = cambc::self();
    const int now = static_cast<int>(cambc::round_number());
    const float hp = me.health(), max_hp = me.max_health();
    const float margin = 100 + 0.1f * max_hp;
    const float cap = std::max(1.0f, max_hp - margin);  // damage one trip can absorb from full health
    const float spare = std::max(0.0f, hp - margin);      // ... and from where we stand now
    const auto s = read_sighting();
    const float gold_w = (me.champion_name() == cambc::champion_type::vesper_vale ? 0.5f : 0.35f)
        * std::max(0.4f, 1.0f - now / 1500.0f);
    const int alive = 1 + static_cast<int>(view().allies.size());
    int best = -1;
    Plan best_plan;
    wait_out = 0;
    std::array<std::uint8_t, 5> claims{};
    cambc::shared_memory.read(0, std::span<std::uint8_t>{claims.data(), claims.size()});
    // Turns for n champions of our strength to clear the group, and the trips each needs on top.
    auto trips_for = [&](const ClearCost& cc, int n, float& turns) {
        turns = cc.hp_sum / (dps_ * n);
        const float each = cc.base / (dps_ * n) * taken_ / n;
        return each <= spare ? 0.0f : (each - spare) / cap;  // expected trips home
    };
    // The current target first: a good early best prunes most of the others cheaply.
    const int n_groups = static_cast<int>(g_.size());
    for (int j = -1; j < n_groups; ++j) {
        const int i = j < 0 ? target_ : j;
        if (i < 0 || (j >= 0 && i == target_)) continue;
        const auto& g = g_[i];
        const auto& k = k_[i];
        if (k.blocked_until > now) continue;
        float value = g.gems + gold_w * g.gold;
        if (value <= 0) continue;
        const int e = eta(i);
        if (e >= kFar) continue;
        if (value * 1.7f / (e + kCaptureTurns) <= best_plan.score) continue;  // cannot beat the best
        const int avail = k.state == Known::respawning ? k.ready : now;
        const int wait = std::max(0, avail - (now + e));
        float f = 1.0f;
        if (home_slot_.size() == g_.size()) {
            if (home_slot_[i] == 255) f = 0.8f;
            else if (home_slot_[i] != slot_) f = 0.6f;
        }
        if (g.d_enemy + 3 < g.d_ally) f *= 0.7f;
        if (k.state == Known::reward) f *= 1.3f;
        const bool waiting = k.state == Known::respawning && wait > 0;
        if (i == target_ && !waiting) f *= 1.25f;
        if (s.ok && s.age <= 24 && cheb(s.where, g.center) <= 7) f *= 0.5f;
        int claimers = 0;
        for (int o = 0; o < 5; ++o)
            if (o != slot_ && claims[static_cast<size_t>(o)] == i + 1) ++claimers;
        const float trip = kTripTurns + std::min(60, g.d_ally);
        // Gems still have to reach an altar: about one walk there per 45 carried.
        const float bank = g.gems / 45.0f * std::min(bank_d_[static_cast<size_t>(i)], 6 + home_altar_);
        const float base_t = e + wait + kCaptureTurns;
        Plan plan;
        if (k.state == Known::reward) {
            // The claimers nearest the region capture (raiders split a boss's reward, as many as its
            // region holds; one is enough elsewhere); the others move on.
            const int room = g.boss ? static_cast<int>(g.tiles.size()) : 1;
            int nearer = 0;
            if (claimers > 0) {
                const int mine = cheb(pos_, g.center);
                const auto& v = view();
                for (size_t a = 0; a < v.allies.size(); ++a) {
                    const int o = slot_of(v.ally_types[a], size_);
                    if (o < 0 || o > 4 || claims[static_cast<size_t>(o)] != i + 1) continue;
                    const int theirs = cheb(v.allies[a], g.center);
                    if (theirs < mine || (theirs == mine && o < slot_)) ++nearer;
                }
            }
            if (nearer >= room) f *= 0.1f;
            else plan.n = std::min(claimers + 1, room);
            plan.score = value / plan.n * f / (e + kCaptureTurns + bank / plan.n);
        } else {
            if (value * f / (base_t + bank) <= best_plan.score) continue;
            const auto& cc = clear_cost(i);
            const float peak = cc.peak * taken_;
            // Alone: worn down over several trips when one health bar is not enough.
            if (!g.boss && cap >= 5 * peak) {
                float turns = 0;
                const float trips = trips_for(cc, 1, turns);
                if (trips <= 4) {
                    float sc = value * f / (base_t + bank + turns + trips * trip);
                    if (claimers > 0) {
                        // Someone is on it: share a long clear, leave a short one to them.
                        if (turns > kShareTurns || trips > 0) {
                            float t2 = 0;
                            const float tr2 = trips_for(cc, claimers + 1, t2);
                            sc = value / (claimers + 1) * f / (base_t + bank / (claimers + 1) + t2 + tr2 * trip);
                            if (sc > plan.score) { plan.score = sc; plan.n = claimers + 1; plan.rally = false; }
                            sc = 0;
                        } else {
                            for (int c = 0; c < claimers; ++c) sc *= 0.3f;
                        }
                    }
                    if (sc > plan.score) { plan.score = sc; plan.n = 1; plan.rally = false; }
                }
            }
            // Raid: n champions of our strength gather outside the fighters' reach, then engage together.
            if (alive >= 2 && (g.boss || plan.score <= 0)) {
                for (int n = std::max(2, claimers + 1); n <= alive; ++n) {
                    if (cap * n < 3 * peak) continue;
                    float turns = 0;
                    const float trips = trips_for(cc, n, turns);
                    if (trips > 3) continue;
                    const float gather = n > claimers + 1 ? kGatherTurns : 0.0f;
                    // Join a raid that is forming rather than start a second one.
                    const float join = waiting ? 1.0f : 1.0f + 0.5f * claimers;
                    const float sc = value / n * f * join / (base_t + bank / n + gather + turns + trips * trip);
                    if (sc > plan.score) { plan.score = sc; plan.n = n; plan.rally = true; }
                }
            }
        }
        if (plan.score > best_plan.score) { best_plan = plan; best = i; wait_out = wait; }
    }
    raid_n_ = best_plan.n;
    rally_need_ = best_plan.rally;
    if (kTraceChoose && best >= 0 && best != target_ && now < 700)
        std::cout << "choose r" << now << ' ' << cambc::to_string(me.champion_name()) << " g" << best << " st " << int(k_[best].state)
                  << " gems " << g_[best].gems << " gold " << g_[best].gold << " hp " << remaining(best) << " eta " << eta(best)
                  << " wait " << wait_out << " n " << raid_n_ << " rally " << rally_need_ << " score " << best_plan.score
                  << " dps " << dps_ << " myhp " << hp << '/' << max_hp << '\n';
    return best;
}

void Bot::farm(int group) {
    const auto& k = k_[group];
    const int now = static_cast<int>(cambc::round_number());
    const int e = eta(group);

    g_mode = e > 3 ? 11 : 15;
    if (k.state == Known::reward) {
        g_mode = 13;
        const auto& tiles = gtiles_[group];
        if (tiles.is_set(pos_)) {
            if (capture_g_ != group) { capture_g_ = group; capture_since_ = now; }
            if (now - capture_since_ > cambc::constants::capture_time + 6) {
                k_[group].blocked_until = now + 60;
                capture_g_ = -1;
            }
            return;
        }
        if (!walk_static(group, e)) go(tiles, e > 3);
        return;
    }
    capture_g_ = -1;
    if (k.state == Known::respawning) {
        g_mode = 14;
        // Wait close by but off the spawn tiles: standing on one delays the respawn.
        auto near = grow(gtiles_[group], 2) & ~gtiles_[group] & ~walk_walls();
        if (!near.is_set(pos_) && !walk_static(group, e)) go(near, true);
        return;
    }
    // A raid gathers outside the fighters' reach and engages together (or once someone has started).
    if (group == target_ && raid_n_ >= 2 && rally_need_ && !group_awake(group)) {
        if (rally_g_ != group) { rally_g_ = group; rally_since_ = -1; }
        const int near = raiders_near(group);
        const auto& rally = rally_goal(group);
        if (near < raid_n_ && !rally.is_empty()) {
            if (rally.is_set(pos_) && rally_since_ < 0) rally_since_ = now;
            if (rally_since_ < 0 || now - rally_since_ < kRallyRounds) {
                g_mode = 16;
                if (!rally.is_set(pos_) && !walk_static(group, e - g_[group].range - 2)) go(rally, e > 3);
                return;
            }
            if (near < 2) {
                // Nobody came: try something else for a while.
                k_[group].blocked_until = now + 80;
                target_ = -1;
                return;
            }
        }
    }
    spells_on_group(group);
    if (hit_group(group)) {
        g_mode = 12;
        reset_hit(group);
        return;
    }
    if (cast_) g_mode = 12;
    // Approach tiles in attack range of the camps we believe alive.
    if (shrink_g_ != group) { shrink_g_ = group; shrink_ = 0; }
    int r = std::max(1, cambc::self().attack_range() - shrink_);
    const auto& goal = approach_goal(group, r);
    if (goal.is_set(pos_) && !attacked_ && r > 1) {
        // In range but nothing to hit: walls hide the camps, come closer.
        ++shrink_;
        --r;
    }
    const auto& goal2 = approach_goal(group, r);
    const bool moved_before = moved_;
    fp("farm-goal");
    if (walk_static(group, e - r)) { fp("farm-static"); return; }
    const bool ok = go(goal2, e > 3);
    fp("farm-go");
    // The safest spots cannot be reached: fall back to any spot in range for a while.
    if (!ok && !moved_before && !moved_ && !goal2.is_set(pos_) && !goal_plain_) plain_until_ = now + 12;
    if (hit_group(group)) reset_hit(group);
}

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

// Fights with enemy champions. Every visible fighter near us (allies within 7, enemies within 9) is read
// into a small model: health (plus shields), basic damage, the burst it has ready (attack reset, damage
// spell), the stun it has ready, how many turns it needs to come into reach, camp fire it is taking, and
// what its death is worth (kill gold plus the gems it carries, which change hands). A turn-by-turn
// focus-fire simulation of that fight (both sides hit their weakest opponent; spawn tiles heal fully)
// decides: engage when the kills we expect are worth more than the deaths, otherwise retreat. A retreat
// steps to the neighbouring tile the fewest enemies can reach next turn (towards our spawn, dashing when
// it helps) and recalls once nobody is close. A kill within reach this turn is always taken.
namespace {

float spell_range_of(const cambc::ability& ab) { return ability_range(ab); }

// Read one champion into the model; false when the handle went stale.
bool read_fighter(const cambc::champion& c, vec2 p, Fighter& f) {
    try {
        if (!c.is_alive()) return false;
        f.pos = p;
        f.hp = c.health();
        f.max_hp = std::max(1.0f, c.max_health());
        f.str = c.strength();
        f.range = c.attack_range();
        f.ranged = f.range > 1;
        const float focus = c.focus();
        bool reset = false;
        float spell = 0;
        for (const auto& ab : c.abilities()) {
            bool ready = true;
            try { ready = ab.cooldown() <= 0; } catch (const std::exception&) {}
            if (ab.is_dash()) { if (ready) f.dasher = true; continue; }
            if (ab.is_attack_reset()) {
                reset = true;
                if (ready) {
                    f.burst += f.str;
                    try {
                        const auto& info = ab.info();
                        if (info)
                            if (auto* r = std::get_if<cambc::ability_attack_reset>(&*info); r && r->enhanced_attack) f.stun = true;
                    } catch (const std::exception&) {}
                }
                continue;
            }
            if (ab.is_shield()) {
                if (ready) {
                    float s = 2.0f * focus;
                    try {
                        const auto& info = ab.info();
                        if (info)
                            if (auto* sh = std::get_if<cambc::ability_shield>(&*info)) s = sh->modifier * focus;
                    } catch (const std::exception&) {}
                    f.hp += s;
                }
                continue;
            }
            if (ab.is_damage() || ab.is_cc() || ab.is_aoe_damage()) {
                float d = 0;
                try { d = ab.damage_amount(); } catch (const std::exception&) { d = focus; }
                spell += d;
                if (ready) f.burst += d;
                if (ready && ab.is_cc()) f.stun = true;
            }
        }
        f.sustain = f.str * (reset ? 9.0f / 8.0f : 1.0f) + spell / 8.0f;
        if (c.champion_name() == cambc::champion_type::heart_of_ruin) {
            const float bonus = 1.0f + 0.5f * (1.0f - f.hp / f.max_hp);
            f.sustain *= bonus;
            f.burst *= bonus;
        }
        try {
            const auto b = c.buffs();
            f.stunned = b.has_stun();
            if (b.has_shield()) f.hp += b.get<cambc::buff_shield>().value;
        } catch (const std::exception&) {}
        try { f.channel = c.is_recalling() || c.is_submitting_gems(); } catch (const std::exception&) {}
        try { f.gems = static_cast<int>(c.gem_count()); } catch (const std::exception&) { f.gems = 0; }
        f.value = 150.0f + 2.0f * f.gems;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

struct SimResult {
    float value = 0;        // expected kill value minus loss value, from our side
    bool me_dies = false;
    int our_deaths = 0, their_deaths = 0;
    float their_lost = 0, our_lost = 0;  // health fractions lost
};

// Turn-by-turn focus fire: each side pours its damage into its weakest living opponent (overflow moves on),
// fighters join once their delay has passed (burst on that first turn), a ready stun takes the strongest
// opposing fighter's next turn away, camps keep hitting whoever they hit, and spawn tiles heal fully.
SimResult simulate(std::vector<Fighter> a, std::vector<Fighter> b, int turns) {
    SimResult r;
    const size_t na = a.size(), nb = b.size();
    std::array<float, 16> ha{}, hb{};
    std::array<int, 16> stun_a{}, stun_b{};  // turn on which the fighter is stunned (-1 none)
    for (size_t i = 0; i < na; ++i) { ha[i] = a[i].hp; stun_a[i] = a[i].stunned ? a[i].delay : -1; }
    for (size_t i = 0; i < nb; ++i) { hb[i] = b[i].hp; stun_b[i] = b[i].stunned ? b[i].delay : -1; }
    auto pour = [](float dmg, std::vector<Fighter>& side, std::array<float, 16>& h, size_t n) {
        while (dmg > 0) {
            int k = -1;
            for (size_t i = 0; i < n; ++i)
                if (h[i] > 0 && (k < 0 || h[i] < h[static_cast<size_t>(k)])) k = static_cast<int>(i);
            if (k < 0) return;
            const float take = std::min(dmg, h[static_cast<size_t>(k)]);
            h[static_cast<size_t>(k)] -= take;
            dmg -= take;
            if (h[static_cast<size_t>(k)] <= 0.5f) h[static_cast<size_t>(k)] = 0;
        }
        (void)side;
    };
    auto strongest = [](const std::vector<Fighter>& side, const std::array<float, 16>& h, size_t n) {
        int k = -1;
        for (size_t i = 0; i < n; ++i)
            if (h[i] > 0 && (k < 0 || side[i].sustain > side[static_cast<size_t>(k)].sustain)) k = static_cast<int>(i);
        return k;
    };
    for (int t = 0; t < turns; ++t) {
        float da = 0, db = 0;
        for (size_t i = 0; i < na; ++i) {
            if (ha[i] <= 0 || a[i].delay > t || stun_a[i] == t) continue;
            da += a[i].sustain + (t == a[i].delay ? a[i].burst : 0.0f);
            if (t == a[i].delay && a[i].stun) {
                const int k = strongest(b, hb, nb);
                if (k >= 0) stun_b[static_cast<size_t>(k)] = std::max(t + 1, b[static_cast<size_t>(k)].delay);
            }
        }
        for (size_t i = 0; i < nb; ++i) {
            if (hb[i] <= 0 || b[i].delay > t || stun_b[i] == t) continue;
            db += b[i].sustain + (t == b[i].delay ? b[i].burst : 0.0f);
            if (t == b[i].delay && b[i].stun) {
                const int k = strongest(a, ha, na);
                if (k >= 0) stun_a[static_cast<size_t>(k)] = std::max(t + 1, a[static_cast<size_t>(k)].delay);
            }
        }
        pour(da, b, hb, nb);
        pour(db, a, ha, na);
        bool any_a = false, any_b = false;
        for (size_t i = 0; i < na; ++i) {
            if (ha[i] <= 0) continue;
            ha[i] -= a[i].camp_in;
            if (ha[i] <= 0) { ha[i] = 0; continue; }
            if (a[i].safe) ha[i] = a[i].max_hp;
            any_a = true;
        }
        for (size_t i = 0; i < nb; ++i) {
            if (hb[i] <= 0) continue;
            hb[i] -= b[i].camp_in;
            if (hb[i] <= 0) { hb[i] = 0; continue; }
            if (b[i].safe) hb[i] = b[i].max_hp;
            any_b = true;
        }
        if (!any_a || !any_b) break;
    }
    for (size_t i = 0; i < na; ++i) {
        r.our_lost += (a[i].hp - ha[i]) / a[i].max_hp;
        if (ha[i] <= 0) { ++r.our_deaths; r.value -= a[i].value; if (i == 0) r.me_dies = true; }
    }
    for (size_t i = 0; i < nb; ++i) {
        r.their_lost += (b[i].hp - hb[i]) / b[i].max_hp;
        if (hb[i] <= 0) { ++r.their_deaths; r.value += b[i].value; }
    }
    // Damage that does not kill still sends them home (and us).
    r.value += 60.0f * (r.their_lost - r.our_lost);
    return r;
}

}  // namespace

// Camp damage each fighter takes per turn: an awake camp hits the nearest champion within its reach.
void Bot::camp_fire(std::vector<Fighter>& ours, std::vector<Fighter>& theirs) const {
    if (awake_.is_empty()) return;
    for (auto p : awake_.positions()) {
        if (cheb(p, pos_) > 10) continue;
        const int gi = group_at_[static_cast<size_t>(tidx(p))];
        const int ci = camp_at_[static_cast<size_t>(tidx(p))];
        if (gi < 0 || ci < 0) continue;
        const auto s = camp_spec(g_[gi].kinds[static_cast<size_t>(ci)]);
        if (s.str <= 0) continue;
        Fighter* best = nullptr;
        int bd = 99;
        for (auto& f : ours) { const int d = cheb(f.pos, p); if (d < bd) { bd = d; best = &f; } }
        for (auto& f : theirs) { const int d = cheb(f.pos, p); if (d < bd) { bd = d; best = &f; } }
        if (best && bd <= s.range) best->camp_in += s.str * (best == &ours[0] ? taken_ : 1.0f);
    }
}

// Damage the enemies can put on tile t next turn (melee step and hit, ranged from where they stand).
float Bot::threat_at(vec2 t, const std::vector<Fighter>& theirs) const {
    float th = 0;
    for (const auto& e : theirs) {
        const int d = cheb(e.pos, t);
        if (e.stunned) continue;
        const int reach = e.ranged ? e.range : e.range + 1 + (e.dasher ? 2 : 0);
        if (d <= reach) th += e.str + e.burst * 0.5f;
        else if (d <= 4 && e.burst > e.str) th += (e.burst - e.str) * 0.5f;  // a ranged spell
    }
    return th;
}

// Run from the enemies: the free neighbouring tile with the least threat, nearer home on ties, a dash
// first when it gets us out of reach. Recall once no enemy can reach us soon.
bool Bot::retreat(std::vector<Fighter>& theirs, int dmin) {
    auto& me = cambc::self();
    const bool on_spawn = cambc::map::spawn_region(team::ally).is_set(pos_);
    if (on_spawn) {
        // The spawn heals us every turn and they cannot step on it: hold it and shoot from there.
        if (++spawn_hold_ > 20) return false;
        strike(theirs);
        return true;
    }
    spawn_hold_ = 0;
    // Out of reach of everyone for a few turns: recall (damage would cancel it).
    if (dmin >= 7 && !moved_ && incoming_ == 0 && live() && home_.at(pos_) > 6) {
        try { me.recall(); } catch (const cambc::error& e) { warn("flee recall", e); }
        return true;
    }
    // Melee: a free swing at an adjacent enemy before stepping away.
    if (me.attack_range() <= 1 && me.can_attack() && live())
        for (const auto& e : theirs)
            if (cheb(e.pos, pos_) <= 1 && view().lvis.at(e.pos)) {
                try {
                    me.attack(cambc::vision::champion_at(e.pos));
                    attacked_ = true;
                    invalidate();
                } catch (const cambc::error& ex) { warn("swing", ex); }
                break;
            }
    if (moved_ || move_failed_ || !me.can_move() || !live()) return true;
    const Bits& sp = spawn_local_;
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
        if (sp.at(t)) s *= 0.1f;
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

// Attack the best enemy in basic range, then reset and hit again. True when a hit landed.
bool Bot::strike(std::vector<Fighter>& theirs) {
    auto& me = cambc::self();
    if (!me.can_attack() || !live()) return false;
    const vec2 at = me.position();
    const int range = me.attack_range();
    const float str = me.strength();
    auto pick = [&]() -> int {
        int best = -1;
        float bs = -1e9f;
        for (size_t i = 0; i < theirs.size(); ++i) {
            const auto& e = theirs[i];
            if (e.hp <= 0 || cheb(e.pos, at) > range || !view().lvis.at(e.pos)) continue;
            float s = -e.hp / std::max(1.0f, e.sustain);
            if (e.hp <= str * 0.9f) s += 1000 + e.value;
            if (e.channel) s += 50;
            s += e.gems * 0.5f;
            if (s > bs) { bs = s; best = static_cast<int>(i); }
        }
        return best;
    };
    auto hit = [&](int i) -> bool {
        auto& e = theirs[static_cast<size_t>(i)];
        try {
            if (!cambc::vision::has_champion(e.pos)) return false;
            auto c = cambc::vision::champion_at(e.pos);
            if (!c.is_visible() || !c.is_alive()) return false;
            me.attack(c);
            attacked_ = true;
            invalidate();
            e.hp -= str;
            return true;
        } catch (const cambc::error& ex) { warn("strike", ex); }
        return false;
    };
    const int first = pick();
    if (first < 0 || !hit(first)) return false;
    if (!cast_ && me.can_cast() && fuel(8000) && live())
        for (const auto& ab : me.abilities())
            if (ab.is_attack_reset() && ab.is_ready()) {
                if (me.can_attack()) {
                    // A kill gave the attack back already (Vendetta): just hit again.
                    const int next = pick();
                    if (next >= 0) hit(next);
                    break;
                }
                try { ab.use(); cast_ = true; } catch (const cambc::error& ex) { warn("reset", ex); break; }
                if (me.can_attack()) {
                    const int next = pick();
                    if (next >= 0) hit(next);
                }
                break;
            }
    return true;
}

// Unit spells (damage, stun) on the best enemy in their range; a shield when they can hit us. Cast
// before walking: most of these exclude it.
void Bot::fight_spells(std::vector<Fighter>& theirs, bool engage) {
    auto& me = cambc::self();
    if (cast_ || !me.can_cast() || !live()) return;
    for (const auto& ab : me.abilities()) {
        if (cast_ || !ab.is_ready()) continue;
        bool exclusive = true;
        try {
            const auto& info = ab.info();
            if (info) exclusive = std::visit([](const auto& x) { return x.exclusive_with_movement; }, *info);
        } catch (const std::exception&) {}
        if (exclusive && moved_) continue;
        if (ab.is_shield()) {
            if (threat_at(pos_, theirs) > 0 || incoming_ > 0) {
                try { ab.use(); cast_ = true; } catch (const cambc::error& e) { warn("shield", e); }
            }
            continue;
        }
        const bool unit_spell = (ab.is_damage() || ab.is_cc()) && ab.can_target_enemies();
        const bool area_spell = ab.is_aoe_damage() && ab.can_target_ground();
        if (!unit_spell && !area_spell) continue;
        const int r = static_cast<int>(spell_range_of(ab));
        float dmg = 0;
        try { dmg = ab.damage_amount(); } catch (const std::exception&) {}
        int best = -1;
        float bs = -1e9f;
        for (size_t i = 0; i < theirs.size(); ++i) {
            const auto& e = theirs[i];
            if (e.hp <= 0 || cheb(e.pos, pos_) > r || !view().lvis.at(e.pos)) continue;
            float s = std::min(dmg, e.hp);
            if (e.hp <= dmg) s += 1000 + e.value;
            if (ab.is_cc()) {
                if (e.channel) s += 400;            // a stun cancels a tribute or a recall
                if (e.stunned) s -= 300;
                s += e.sustain;
            }
            if (area_spell)
                for (const auto& o : theirs)
                    if (&o != &e && o.hp > 0 && cheb(o.pos, e.pos) <= 1) s += std::min(dmg, o.hp);
            if (s > bs) { bs = s; best = static_cast<int>(i); }
        }
        if (best < 0) continue;
        auto& e = theirs[static_cast<size_t>(best)];
        // Retreating: only a kill, a stun on a chaser or a cancelled channel is worth giving up the walk.
        if (!engage && exclusive && !(e.hp <= dmg || (ab.is_cc() && cheb(e.pos, pos_) <= 3) || e.channel)) continue;
        try {
            if (unit_spell) {
                if (!cambc::vision::has_champion(e.pos)) continue;
                ab.use(cambc::vision::champion_at(e.pos));
            } else {
                ab.use(e.pos);
            }
            cast_ = true;
            if (exclusive) move_failed_ = true;  // no walking after this cast
            e.hp -= dmg;
            if (ab.is_cc()) e.stunned = true;
            invalidate();
        } catch (const cambc::error& ex) { warn("enemy spell", ex); }
    }
}

// True when the turn is spent.
bool Bot::enemies() {
    auto& me = cambc::self();
    const int now = static_cast<int>(cambc::round_number());
    const auto& enemy_spawn = cambc::map::spawn_region(team::enemy);
    std::vector<Fighter> theirs, ours;
    // Us first (index 0), then allies near enough to join.
    {
        Fighter f;
        if (!read_fighter(me, pos_, f)) return false;
        f.safe = spawn_local_.at(pos_);
        f.value = 150.0f + 2.0f * static_cast<float>(me.gem_count());
        ours.push_back(f);
    }
    int dmin = 99;
    const auto tiles = cambc::vision::champion_tiles(team::enemy);
    if (!tiles.is_empty())
        for (auto p : tiles.positions()) {
            if (enemy_spawn.is_set(p)) continue;
            const int d = cheb(p, pos_);
            if (d > 9) continue;
            try {
                Fighter f;
                if (!read_fighter(cambc::vision::champion_at(p), p, f)) continue;
                theirs.push_back(f);
                dmin = std::min(dmin, d);
            } catch (const std::exception&) {}
            if (theirs.size() >= 8) break;
        }
    if (theirs.empty()) {
        // Help an ally who reported an invader close by.
        const auto s = read_sighting();
        if (s.ok && s.age <= 4 && cheb(s.where, pos_) <= 10 && me.health() > me.max_health() * 0.6f
            && me.gem_count() < 30 && incoming_ == 0) {
            auto goal = empty_board();
            goal.set(s.where);
            goal = grow(goal, 2) & ~walk_walls();
            return go(goal, true);
        }
        return false;
    }
    last_enemy_seen_ = now;
    write_sighting(theirs.front().pos);
    for (const auto& a : others_)
        try {
            if (!a.is_alive()) continue;
            const vec2 p = a.position();
            if (!inside(p) || cheb(p, pos_) > 7) continue;
            if (a.is_recalling()) continue;
            Fighter f;
            if (!read_fighter(a, p, f)) continue;
            f.safe = spawn_local_.at(p);
            ours.push_back(f);
            if (ours.size() >= 8) break;
        } catch (const std::exception&) {}
    // Turns before each fighter can hit: walk to its range of the nearest opponent (melee hits on arrival).
    auto set_delay = [](Fighter& f, const std::vector<Fighter>& other) {
        int d = 99;
        for (const auto& o : other) d = std::min(d, cheb(o.pos, f.pos));
        f.delay = std::max(0, d - f.range - (f.ranged ? 0 : 1));
        if (f.stunned) f.delay = std::max(f.delay, 1);
    };
    for (auto& f : ours) set_delay(f, theirs);
    for (auto& f : theirs) set_delay(f, ours);
    ours[0].camp_in = 0;
    camp_fire(ours, theirs);

    const SimResult fight = simulate(ours, theirs, 10);
    // Fleeing instead: the enemies still in reach this turn get their hits, adjacent melee keeps hitting.
    float flee_dmg = 0;
    for (const auto& e : theirs) {
        if (e.stunned) continue;
        const int d = cheb(e.pos, pos_);
        if (d <= e.range) flee_dmg += e.str + e.burst;
        if (!e.ranged && (d <= 1 || (e.dasher && d <= 3))) flee_dmg += e.sustain * 3;
    }
    const float hp = me.health();
    const int gems = static_cast<int>(me.gem_count());
    const bool doomed_if_flee = !ours[0].safe && flee_dmg >= hp;
    // Kill on the spot: an enemy our burst finishes this turn without moving.
    bool kill_now = false;
    {
        const float mine = ours[0].str + ours[0].burst;
        const int reach = ours[0].ranged ? ours[0].range : 2;
        for (const auto& e : theirs)
            if (cheb(e.pos, pos_) <= reach && e.hp <= mine * 0.85f && view().lvis.at(e.pos)) kill_now = true;
    }
    // Stick with the last decision when the odds barely changed: flip-flopping takes hits and lands none.
    float need = kEngageBias;
    if (now - fight_round_ <= 2) need += fight_engaged_ ? -kStick : kStick;
    bool engage = fight.value > need && !(fight.me_dies && fight.their_deaths == 0);
    // Carrying a big bag: only clearly winning fights.
    if (engage && fight.me_dies && gems >= 40 && fight.value < 2.0f * gems) engage = false;
    if (!engage && (doomed_if_flee || kill_now) && !(fight.me_dies && gems >= 60 && !doomed_if_flee)) engage = true;
    if (kTrace) {
        std::string line = "fight r" + std::to_string(now) + ' ' + cambc::to_string(me.champion_name()) + " v" + std::to_string(static_cast<int>(fight.value))
            + " die" + std::to_string(fight.me_dies) + " k" + std::to_string(fight.their_deaths) + " d" + std::to_string(fight.our_deaths)
            + " fl" + std::to_string(static_cast<int>(flee_dmg)) + " E" + std::to_string(engage) + " |";
        auto add = [&](const Fighter& f) {
            line += ' ' + std::to_string(f.pos.x) + ',' + std::to_string(f.pos.y) + ':' + std::to_string(static_cast<int>(f.hp)) + '/'
                + std::to_string(static_cast<int>(f.sustain)) + '+' + std::to_string(static_cast<int>(f.burst)) + '@' + std::to_string(f.delay)
                + (f.camp_in > 0 ? "c" : "") + (f.safe ? "s" : "");
        };
        for (const auto& f : ours) add(f);
        line += " ||";
        for (const auto& f : theirs) add(f);
        std::cout << line << '\n';
    }

    fight_round_ = now;
    fight_engaged_ = engage;
    if (!engage) {
        // Far enemies that are not about to reach us: carry on, but bank sooner (tribute checks sightings).
        bool reachable = false;
        for (const auto& e : theirs)
            if (cheb(e.pos, pos_) <= static_cast<int>(e.threat_reach()) + 3) reachable = true;
        if (!reachable && hp > me.max_health() * 0.5f) return false;
        fight_spells(theirs, false);
        return retreat(theirs, dmin);
    }
    // Strong enough but nobody close: keep farming unless they are worth a short hunt.
    if (dmin > me.attack_range() + 3 && fight.their_deaths == 0) return false;
    if (now < no_chase_until_ && dmin > me.attack_range() + 1) return false;
    if (!live()) return true;
    fight_spells(theirs, true);
    if (strike(theirs)) { chase_turns_ = 0; return true; }
    if (move_failed_ || (cast_ && me.attack_range() > 1)) return true;
    // Approach: tiles in basic range of the enemies; ranged champions prefer the ones melee cannot reach.
    if (++chase_turns_ > kChaseTurns) {
        chase_turns_ = 0;
        no_chase_until_ = now + 15;
        return false;
    }
    const int range = me.attack_range();
    auto goal = empty_board();
    for (const auto& e : theirs)
        if (e.hp > 0) goal.set(e.pos);
    goal = grow(goal, range) & ~walk_walls();
    go(goal, dmin > range + 1);
    pos_ = me.position();
    // A melee step that arrived, or a ranged step with a reset in hand, still hits this turn.
    if (live() && me.can_attack()) {
        if (strike(theirs)) chase_turns_ = 0;
    } else if (live() && range > 1 && !cast_ && me.can_cast() && attacked_ == false) {
        bool in_range = false;
        for (const auto& e : theirs)
            if (cheb(e.pos, pos_) <= range && view().lvis.at(e.pos)) in_range = true;
        if (in_range)
            for (const auto& ab : me.abilities())
                if (ab.is_attack_reset() && ab.is_ready()) {
                    try { ab.use(); cast_ = true; } catch (const cambc::error& ex) { warn("walk reset", ex); break; }
                    if (me.can_attack() && strike(theirs)) chase_turns_ = 0;
                    break;
                }
    }
    return true;
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
        if (me.health() > incoming_ * taken_ * turns + 40) {
            if (live()) try { me.submit_gems(a); } catch (const cambc::error& e) { warn("submit", e); }
            return true;
        }
    }
    if (!locks.is_empty() && me.can_attack() && live())
        for (auto a : locks.positions())
            if (cheb(a, pos_) <= std::min(cambc::constants::submit_gems_range, me.attack_range())) {
                try {
                    me.attack(cambc::vision::altar_lock_at(a));
                    attacked_ = true;
                    return true;
                } catch (const cambc::error& e) { warn("lock", e); }
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
        goal &= ~grow(one, cambc::constants::submit_gems_range);
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
    // A recall that an enemy will cancel anyway: stop it and get away (or fight) instead of standing still.
    if (me.is_recalling() && live()) {
        const auto foes = cambc::vision::champion_tiles(team::enemy);
        if (!foes.is_empty())
            for (auto p : foes.positions())
                if (cheb(p, pos_) <= 5 && !cambc::map::spawn_region(team::enemy).is_set(p)) {
                    try { me.cancel_recall(); } catch (const cambc::error& e) { warn("cancel recall", e); }
                    break;
                }
    }
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
