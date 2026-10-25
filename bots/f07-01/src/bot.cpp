// Scaling-economy champion: farm gold and gems on our side, turn gold into stats at every
// spawn visit, bank gems at the nearest altar, and fight only when the numbers favour us.
#include <cambc.h>
#include "bot.h"
#include "fuel.h"
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
constexpr int kChaseTurns = 8;      // approach turns without a hit before giving up a chase
constexpr float kEngageBias = -150.0f;  // fight value needed to engage
constexpr float kStick = 0.0f;       // fight value margin in favour of the last decision
constexpr bool kDoomedFights = false;  // fight back when fleeing looks fatal anyway
constexpr bool kCaughtFights = false;  // fight back when a melee chaser would hit us all the way home
constexpr bool kFreeShots = true;     // retreating ranged: shoot when nobody can hit us here next turn
constexpr float kAllyPull = 0.0f;      // retreat score per tile of distance to the nearest fighting ally
constexpr float kBossTurns = 8.0f;  // turns a champion must be able to tank a boss
constexpr int kSimTurns = 10;       // turns the fight simulation looks ahead
constexpr bool kLocks = true;       // carry an altar lock: drop it on enemy tributes and on altars we pass
constexpr bool kLockRunner = true;  // the last pick carries locks to far altars
constexpr int kLockDetour = 8;      // walk this far (Chebyshev) out of our way to lock an altar

// Debug: turns spent per activity, printed every 200 rounds (camps.cpp sets it too).
int g_mode = 0;
namespace {
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

bool has_lock() {
    try { return cambc::self().consumables()[cambc::consumable_type::altar_lock].count() > 0; }
    catch (const cambc::error&) { return false; }
}

// ---------------------------------------------------------------- knowledge (map analysis, observe(): fuel.cpp)

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
    // An altar lock (10 gold) cancels an enemy tribute and blocks enemy tributes: always carry one.
    if (kLocks && me.gold_count() >= 10 && fuel(15000) && live() && !has_lock()
        && (!me.is_alive() || spawn_local_.at(pos_))) {
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

// ---------------------------------------------------------------- camps: camps.cpp

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
    // Ranged: a free shot when nobody can hit us here next turn.
    if (kFreeShots && me.attack_range() > 1 && me.can_attack() && !moved_ && incoming_ == 0 && live()
        && threat_at(pos_, theirs) == 0 && strike(theirs))
        return true;
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
        if (kAllyPull > 0 && !ally_near_.empty()) {
            int ad = 10;
            for (auto a : ally_near_) ad = std::min(ad, cheb(a, t));
            s += kAllyPull * static_cast<float>(ad);
        }
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
        if (s.ok && s.age <= 4 && cheb(s.where, pos_) <= 14 && me.health() > me.max_health() * 0.6f
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
            cambc::detail::champion_info info{};
            if (!peek(a, info)) continue;  // out of our sight (position() would throw)
            const vec2 p{info.x, info.y};
            if (!inside(p)) continue;
            // Near us or near the enemies: our own step must not change who counts.
            bool near = cheb(p, pos_) <= 7;
            for (size_t i = 0; i < theirs.size() && !near; ++i) near = cheb(p, theirs[i].pos) <= 8;
            if (!near || a.is_recalling()) continue;
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
    ally_near_.clear();
    for (size_t i = 1; i < ours.size(); ++i) ally_near_.push_back(ours[i].pos);
    for (auto& f : ours) set_delay(f, theirs);
    for (auto& f : theirs) set_delay(f, ours);
    ours[0].camp_in = 0;
    camp_fire(ours, theirs);

    const SimResult fight = simulate(ours, theirs, kSimTurns);
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
    // Caught by a melee chaser: it walks and hits every turn of the way home, so running only trades worse.
    bool caught = false;
    if (kCaughtFights && !ours[0].safe) {
        float chase = 0;
        for (const auto& e : theirs)
            if (!e.stunned && !e.ranged && cheb(e.pos, pos_) <= (e.dasher ? 3 : 2)) chase += e.sustain;
        chase *= static_cast<float>(std::min(home_.at(pos_), 12)) * (ours[0].ranged ? 1.0f : 0.6f);
        caught = chase >= hp;
    }
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
    if (!engage && ((kDoomedFights && doomed_if_flee) || caught || kill_now) && !(fight.me_dies && gems >= 60 && !doomed_if_flee)) engage = true;
    // The spawn's heal wins this fight, not us: hold it rather than walk out.
    bool hold_spawn = false;
    if (engage && ours[0].safe) {
        auto off = ours;
        off[0].safe = false;
        const SimResult out = simulate(off, theirs, kSimTurns);
        hold_spawn = !(out.value > need && !(out.me_dies && out.their_deaths == 0));
    }
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
    if (hold_spawn) return ++spawn_hold_ <= 20;
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

// ---------------------------------------------------------------- gems and altar locks: endgame.cpp

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
    if (kTrace) last_mode_ = g_mode;
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
    if (kTrace && me.is_alive()) alive_round_ = static_cast<int>(g_round);

    if (!me.is_alive()) {
        if (kTrace && alive_round_ == static_cast<int>(g_round) - 1)
            std::cout << "died r" << g_round << ' ' << cambc::to_string(me.champion_name()) << " fight_r" << fight_round_
                      << " engaged " << fight_engaged_ << " mode " << last_mode_ << '\n';
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
        if (deny()) return;
        lock_altars(false, 0);
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
        // (The altar guard may have just been dropped above: plug_ is -1 then.)
        const auto st = plug_ >= 0 ? k_[plug_].state : Known::unknown;
        if (plug_ < 0 || st == Known::respawning || st == Known::reward || now - plug_since_ > 40 || remaining(plug_) <= 0) plug_ = -1;
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
