// Map-reader champion: follow the draft's plan, farm the best camp group per turn of effort,
// bank gems at the nearest altar, turn gold into shards on every spawn visit, fight only when ahead.
#include <cambc.h>
#include "bot.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <span>
#include <variant>

using it = cambc::item_type;

namespace {

std::uint32_t g_round = 0;
bool live() { return cambc::round_number() == g_round; }
int g_warns = 0;
constexpr bool kStats = true;   // one activity line per champion every 250 rounds
constexpr bool kTable = false;  // group table at the start (costs fuel)
int g_mode = 0;
int g_modes[16]{};
std::uint64_t g_ph[24]{};
int g_nph = 0;
inline void ph() { if (g_nph < 24) g_ph[g_nph++] = cambc::fuel_remaining(); }
const char* kModeNames[16] = {"dead", "chan", "fight", "hurt", "bank", "idle", "hit", "walk", "capture", "wait", "heal", "", "", "", "", ""};
void warn(const char* where, const std::exception& e) {
    if (++g_warns <= 40) std::cout << "warn: " << where << ": " << e.what() << '\n';
}

cambc::champion_type type_of(cambc::champion_draft d) {
    const int v = static_cast<int>(d);
    return static_cast<cambc::champion_type>(v >= 4 ? v + 1 : v);
}

struct Step { it shard, emblem; };
const Step kBuild[] = {
    {it::jungle_shard_of_strength, it::jungle_emblem_of_strength},
    {it::monster_shard_of_strength, it::monster_emblem_of_strength},
    {it::power_shard_of_health, it::power_emblem_of_health},
    {it::power_shard_of_strength, it::power_emblem_of_strength},
    {it::durability_shard_of_health, it::durability_emblem_of_health},
};

}  // namespace

// ---------------------------------------------------------------- setup

void Bot::initialise() {
    init_ = true;
    init_dims();
    auto& me = cambc::self();
    type_ = me.champion_name();

    // The draft's plan.
    std::vector<cambc::champion_draft> picks;
    const auto* mem = reinterpret_cast<const std::uint8_t*>(cambc::draft_memory.data());
    const size_t msize = cambc::draft_memory.size();
    bool have_plan = msize >= 1024 && mem[dm::magic] == 'M';
    if (have_plan) {
        plan_ = static_cast<Plan>(mem[dm::plan]);
        for (int i = 0; i < mem[dm::npicks] && i < 5; ++i) picks.push_back(static_cast<cambc::champion_draft>(mem[dm::picks + i]));
        boss_ = mem[dm::boss] == 255 ? -1 : mem[dm::boss];
    }
    picks_ = picks;
    slot_ = std::max(0, slot_of(type_, picks_));
    if (have_plan) {
        nsquads_ = std::clamp<int>(mem[dm::nsquads], 1, 6);
        for (int i = 0; i < 6; ++i) squad_of_[i] = static_cast<std::uint8_t>(std::min<int>(mem[dm::squad + i], nsquads_ - 1));
    }
    bodies_ = std::max(1, static_cast<int>(cambc::champions(team::ally).size()));
    slot_ = std::clamp(slot_, 0, 5);
    squad_ = squad_of_[slot_];
    squad_n_ = 0;
    for (int i = 0; i < std::min(bodies_, 6); ++i)
        if (squad_of_[i] == squad_) ++squad_n_;
    squad_n_ = std::max(1, squad_n_);

    g_ = analyse_groups();
    h_ = cambc::map::camp_groups();
    const size_t ng = std::min(g_.size(), h_.size());
    g_.resize(ng);
    h_.erase(h_.begin() + static_cast<long>(ng), h_.end());
    owner_.assign(ng, 255);
    if (have_plan && mem[dm::ngroups] == ng)
        for (size_t i = 0; i < ng && dm::owner + i < msize; ++i) owner_[i] = mem[dm::owner + i];
    k_.assign(ng, Known{});
    group_at_.assign(static_cast<size_t>(g_w * g_h), -1);
    goff_.clear();
    chp_.clear();
    for (size_t i = 0; i < ng; ++i) {
        goff_.push_back(static_cast<int>(chp_.size()));
        for (size_t c = 0; c < g_[i].tiles.size(); ++c) {
            chp_.push_back(camp_spec(g_[i].kinds[c]).hp);
            group_at_[static_cast<size_t>(tidx(g_[i].tiles[c]))] = static_cast<std::int16_t>(i);
        }
    }
    goff_.push_back(static_cast<int>(chp_.size()));
    cawake_.assign(chp_.size(), 0);

    const auto terrain = static_walls();
    terrain_.load(terrain);
    walls_camps_ = terrain;
    campspawn_.resize();
    for (const auto& g : g_)
        for (auto t : g.tiles) { walls_camps_.set(t); campspawn_.set(t); }
    spawn_.load(cambc::map::spawn_region(team::ally));
    hard_.load(cambc::map::walls() | cambc::map::altars() | cambc::map::spawn_region(team::enemy));
    altars_ = cambc::map::altars().positions();

    // Home field.
    {
        cambc::distance_field f;
        cambc::bitboard visited;
        f.compute(walls_camps_, cambc::map::spawn_region(team::ally), visited);
        home_.load(f);
    }
    // Altar field: free tiles next to an altar.
    {
        auto roots = empty_board();
        for (auto a : altars_)
            for (auto d : kDirs) {
                const vec2 q{a.x + d.x, a.y + d.y};
                if (inside(q) && !terrain_.at(q) && !campspawn_.at(q)) roots.set(q);
            }
        cambc::distance_field f;
        cambc::bitboard visited;
        f.compute(walls_camps_, roots, visited);
        altar_.load(f);
        // Altars closed in by camps: plan through them (the plug gets cleared on the way).
        const auto sp = cambc::map::spawn_region(team::ally).positions();
        if (!sp.empty() && altar_.at(sp[0]) >= kFar) {
            // Camp tiles beside an altar become tribute spots once their camp is dead.
            for (auto a : altars_)
                for (auto d : kDirs) {
                    const vec2 q{a.x + d.x, a.y + d.y};
                    if (inside(q) && !terrain_.at(q) && campspawn_.at(q)) { roots.set(q); altar_plug_tiles_.push_back(q); }
                }
            f.compute(terrain, roots, visited);
            altar_.load(f);
            altar_plugged_ = true;
        }
    }
    // The boss the team raids: the altar's plug, the draft's boss, else the nearest monolith.
    if (!altar_plug_tiles_.empty()) raid_cand_ = group_at_[static_cast<size_t>(tidx(altar_plug_tiles_[0]))];
    else if (plan_ == kBoss && boss_ >= 0 && boss_ < static_cast<int>(ng)) raid_cand_ = boss_;
    d_home_.assign(ng, kFar);
    for (size_t i = 0; i < ng; ++i)
        for (auto t : g_[i].tiles)
            for (auto d : kDirs) {
                const int v = home_.at(vec2{t.x + d.x, t.y + d.y});
                if (v < kFar && v + 1 < d_home_[i]) d_home_[i] = v + 1;
            }
    if (raid_cand_ < 0) {
        int bd = kFar;
        for (size_t i = 0; i < ng; ++i)
            if (g_[i].monolith && d_home_[i] < bd) { bd = d_home_[i]; raid_cand_ = static_cast<int>(i); }
    }

    const auto mine = static_cast<cambc::handle_champion_t>(me);
    for (const auto& a : cambc::champions(team::ally))
        if (static_cast<cambc::handle_champion_t>(a) != mine) {
            allies_.push_back(a);
            const int sl = std::clamp(slot_of(a.champion_name(), picks_), -1, 5);
            ally_slot_.push_back(sl);
            ally_squad_.push_back(sl >= 0 ? squad_of_[sl] : -1);
        }

    // Kit.
    const auto abs = me.abilities();
    for (int i = 0; i < 2; ++i) {
        const auto& inf = abs[static_cast<size_t>(i)].info();
        if (!inf) continue;
        Slot& sl = kit_[i];
        sl.used = true;
        std::visit([&](const auto& x) {
            sl.kind = x.kind;
            sl.excl = x.exclusive_with_movement;
            sl.camps = x.valid_targets.contains(cambc::ability_target_mask::camps);
            sl.enemies = x.valid_targets.contains(cambc::ability_target_mask::enemies);
            if constexpr (requires { x.range; }) sl.range = x.range;
            if constexpr (requires { x.modifier; }) sl.mod = x.modifier;
        }, *inf);
        if (sl.kind == cambc::ability_kind::attack_reset) reset_slot_ = i;
        if (sl.kind == cambc::ability_kind::dash) dash_slot_ = i;
    }

    gf_.assign(ng, Field{});
    gf_tried_.assign(ng, 0);
    if (kTable && type_ == cambc::champion_type::vendetta) {
        for (auto a : altars_) std::cout << "altar " << a.x << ',' << a.y << " home=" << home_.at(a) << " fld=" << altar_.at(vec2{a.x, a.y + 1}) << ',' << altar_.at(vec2{a.x, a.y - 1}) << '\n';
        std::cout << "plan=" << int(plan_) << " slot=" << slot_ << " bodies=" << bodies_ << " altars=" << altars_.size() << " ng=" << ng << '\n';
    }
}

void Bot::compute_group_field(int g) {
    if (g < 0 || gf_tried_[static_cast<size_t>(g)]) return;
    if (cambc::fuel_remaining() < 30000) return;
    gf_tried_[static_cast<size_t>(g)] = 1;
    auto walls = walls_camps_;
    auto roots = empty_board();
    for (auto t : g_[static_cast<size_t>(g)].tiles) { walls.set(t, false); roots.set(t); }
    cambc::distance_field f;
    cambc::bitboard visited;
    f.compute(walls, roots, visited);
    gf_[static_cast<size_t>(g)].load(f);
    // Closed in by other camps: plan through them instead (they will be cleared on the way).
    const auto sp = cambc::map::spawn_region(team::ally).positions();
    if (!sp.empty() && gf_[static_cast<size_t>(g)].at(sp[0]) >= kFar) {
        auto w2 = static_walls();
        for (auto t : g_[static_cast<size_t>(g)].tiles) w2.set(t, false);
        f.compute(w2, roots, visited);
        gf_[static_cast<size_t>(g)].load(f);
    }
}

// ---------------------------------------------------------------- observation

void Bot::observe() {
    vis_.load(cambc::vision::visible_tiles());
    campocc_.load(cambc::vision::camp_tiles());
    champocc_.load(cambc::vision::champion_tiles());
    for (size_t g = 0; g < g_.size(); ++g) {
        auto& k = k_[g];
        if (k.st == Known::respawning && now_ >= k.ready) {
            k.st = Known::alive;
            for (int c = goff_[g]; c < goff_[g + 1]; ++c)
                chp_[static_cast<size_t>(c)] = camp_spec(g_[g].kinds[static_cast<size_t>(c - goff_[g])]).hp;
        }
        bool seen = false;
        for (auto t : g_[g].tiles)
            if (vis_.at(t)) { seen = true; break; }
        if (!seen || cheb(g_[g].center, pos_) > 12) continue;
        if (cambc::fuel_remaining() < 30000) continue;
        try {
            const auto st = h_[g].state();
            k.seen = now_;
            if (std::holds_alternative<cambc::map::camp_group_alive>(st)) {
                k.st = Known::alive;
                int c = goff_[g];
                for (const auto& camp : h_[g].camps()) {
                    const vec2 sp = camp.spawn_position();
                    if (vis_.at(sp)) {
                        cawake_[static_cast<size_t>(c)] = 0;
                        if (!campocc_.at(sp)) chp_[static_cast<size_t>(c)] = 0;
                        else if (camp.is_visible()) {
                            const auto cs = camp.state();
                            chp_[static_cast<size_t>(c)] = cs.is_dead() ? 0.0f : camp.health();
                            cawake_[static_cast<size_t>(c)] = cs.is_awake() ? 1 : 0;
                        }
                    }
                    ++c;
                }
            } else if (std::holds_alternative<cambc::map::camp_group_reward_available>(st)) {
                k.st = Known::reward;
                for (int c = goff_[g]; c < goff_[g + 1]; ++c) { chp_[static_cast<size_t>(c)] = 0; cawake_[static_cast<size_t>(c)] = 0; }
            } else {
                const auto& r = std::get<cambc::map::camp_group_respawning>(st);
                k.st = Known::respawning;
                k.ready = now_ + std::max(1, r.timer);
                for (int c = goff_[g]; c < goff_[g + 1]; ++c) cawake_[static_cast<size_t>(c)] = 0;
            }
        } catch (const std::exception& e) { warn("observe", e); }
    }
}

void Bot::read_units() {
    foes_.clear();
    friends_.clear();
    const auto& enemy_spawn = cambc::map::spawn_region(team::enemy);
    try {
        for (auto p : cambc::vision::champion_tiles(team::enemy).positions()) {
            try {
                const auto c = cambc::vision::champion_at(p);
                if (!c.is_alive()) continue;
                Unit u{};
                u.pos = p;
                u.hp = c.health();
                u.max_hp = c.max_health();
                u.str = c.strength();
                u.focus = c.focus();
                u.range = c.attack_range();
                u.power = 1.1f * u.str + 0.3f * u.focus;
                u.recalling = c.is_recalling();
                u.tributing = c.is_submitting_gems();
                u.on_spawn = enemy_spawn.is_set(p);
                u.gems = 0;
                foes_.push_back(u);
            } catch (const cambc::error&) {}
        }
    } catch (const std::exception& e) { warn("foes", e); }
    if (ally_mem_.size() != allies_.size()) ally_mem_.assign(allies_.size(), Unit{});
    for (size_t ai = 0; ai < allies_.size(); ++ai) {
        const auto& a = allies_[ai];
        try {
            if (!a.is_alive()) { ally_mem_[ai].seen = -1000; continue; }
            Unit u{};
            u.slot = ally_slot_[ai];
            u.squad = ally_squad_[ai];
            u.pos = a.position();
            if (!inside(u.pos)) continue;
            u.hp = a.health();
            u.max_hp = a.max_health();
            u.str = a.strength();
            u.focus = a.focus();
            u.range = a.attack_range();
            u.power = 1.1f * u.str + 0.3f * u.focus;
            u.recalling = a.is_recalling();
            u.tributing = a.is_submitting_gems();
            u.on_spawn = spawn_.at(u.pos);
            u.seen = now_;
            ally_mem_[ai] = u;
        } catch (const cambc::error&) {}
    }
    // Allies seen in the last two rounds still count (vision of them flickers).
    for (const auto& u : ally_mem_) {
        if (u.seen < now_ - 2) continue;
        Unit v = u;
        v.fresh = u.seen == now_;
        friends_.push_back(v);
    }
}

// ---------------------------------------------------------------- shared memory

void Bot::write_target(int g) {
    const std::uint8_t v = static_cast<std::uint8_t>(g < 0 || g > 250 ? 0 : g + 1);
    try {
        cambc::shared_memory.write(static_cast<size_t>(sm::target + squad_), std::span<const std::uint8_t>{&v, 1});
    } catch (const std::exception& e) { warn("target", e); }
}

int Bot::read_target() const {
    std::uint8_t v = 0;
    try {
        cambc::shared_memory.read(static_cast<size_t>(sm::target + squad_), std::span<std::uint8_t>{&v, 1});
    } catch (const std::exception&) { return -1; }
    if (v == 0 || v > g_.size()) return -1;
    return v - 1;
}

// The squad's leader: its lowest living slot that is not recalling home.
bool Bot::leader() const {
    for (const auto& f : friends_)
        if (f.squad == squad_ && f.slot >= 0 && f.slot < slot_ && !f.recalling) return false;
    return true;
}

float Bot::squad_dps() const {
    float d = dps();
    for (const auto& f : friends_)
        if (f.squad == squad_ && !f.recalling) d += std::max(10.0f, f.power) * dealt_;
    return d;
}

// Champions needed so that nobody tanks more than ~600 health of the group's damage.
int Bot::needed(int g, int avail) const {
    const auto& G = g_[static_cast<size_t>(g)];
    if (G.str <= 0) return 1;
    const float x = G.str * taken_ * group_remaining(g) / (std::max(30.0f, dps()) * 600.0f);
    return std::clamp(static_cast<int>(std::ceil(std::sqrt(x))), 1, std::max(1, avail));
}

// ---------------------------------------------------------------- shop

void Bot::shop() {
    auto& me = cambc::self();
    try {
        std::uint32_t gold = me.gold_count();
        if (gold < 30) return;
        const auto counts = me.item_count();
        for (const auto& st : kBuild) {
            const int owned = static_cast<int>(counts[st.shard] + 5 * counts[st.emblem]);
            if (owned >= 5) continue;
            const std::uint32_t n = std::min<std::uint32_t>(gold / st.shard.cost(), static_cast<std::uint32_t>(5 - owned));
            if (n == 0) return;
            if (!me.can_buy(st.shard, n)) return;
            me.buy(st.shard, n);
            gold = me.gold_count();
            if (n < static_cast<std::uint32_t>(5 - owned)) return;
        }
        // Past the build: alternate strength and health power shards.
        while (gold >= 30) {
            const it pick = (counts[it::power_shard_of_strength] <= counts[it::power_shard_of_health])
                ? it::power_shard_of_strength : it::power_shard_of_health;
            if (!me.can_buy(pick, 1)) return;
            me.buy(pick, 1);
            gold = me.gold_count();
            if (gold < 30) return;
            const it other = pick == it::power_shard_of_strength ? it::power_shard_of_health : it::power_shard_of_strength;
            if (!me.can_buy(other, 1)) return;
            me.buy(other, 1);
            gold = me.gold_count();
        }
    } catch (const std::exception& e) { warn("shop", e); }
}

// ---------------------------------------------------------------- estimates

float Bot::dps() const {
    const auto& me = cambc::self();
    float d = me.strength();
    if (reset_slot_ >= 0) d *= 1.125f;
    for (int i = 0; i < 2; ++i)
        if (kit_[i].used && (kit_[i].kind == cambc::ability_kind::damage || kit_[i].kind == cambc::ability_kind::aoe_damage))
            d += kit_[i].mod * me.focus() / 8.0f;
    return std::max(10.0f, d * dealt_);
}

float Bot::group_remaining(int g) const {
    float s = 0;
    for (int c = goff_[static_cast<size_t>(g)]; c < goff_[static_cast<size_t>(g) + 1]; ++c) s += chp_[static_cast<size_t>(c)];
    return s;
}

int Bot::here(const Field& f) const {
    int d = f.at(pos_);
    if (d < kFar) return d;
    for (auto dd : kDirs) {
        const int v = f.at(vec2{pos_.x + dd.x, pos_.y + dd.y});
        if (v < kFar && v + 1 < d) d = v + 1;
    }
    return d;
}

int Bot::dist_to_group(int g) const {
    const auto& f = gf_[static_cast<size_t>(g)];
    if (!f.empty()) return here(f);
    return cheb(pos_, g_[static_cast<size_t>(g)].center) * 5 / 4 + 2;
}

bool Bot::on_group_tile(int g) const {
    for (auto t : g_[static_cast<size_t>(g)].tiles)
        if (t == pos_) return true;
    return false;
}

// Strength of awake visible camps that can hit us now.
float Bot::incoming_camp_damage() const {
    float s = 0;
    for (size_t g = 0; g < g_.size(); ++g) {
        if (k_[g].st != Known::alive || cheb(g_[g].center, pos_) > 10) continue;
        for (size_t c = 0; c < g_[g].tiles.size(); ++c) {
            const vec2 t = g_[g].tiles[c];
            const auto spec = camp_spec(g_[g].kinds[c]);
            if (spec.str <= 0 || cheb(t, pos_) > spec.range || !campocc_.at(t)) continue;
            if (chp_[static_cast<size_t>(goff_[g]) + c] <= 0) continue;
            if (cawake_[static_cast<size_t>(goff_[g]) + c]) s += spec.str;
        }
    }
    return s * taken_;
}

// ---------------------------------------------------------------- movement

bool Bot::free_tile(vec2 p) const {
    if (!inside(p) || terrain_.at(p)) return false;
    if (champocc_.at(p) || campocc_.at(p)) return false;
    if (campspawn_.at(p) && !vis_.at(p)) {
        const int g = group_at_[static_cast<size_t>(tidx(p))];
        if (g >= 0 && k_[static_cast<size_t>(g)].st == Known::alive) return false;
    }
    return true;
}

bool Bot::try_dash(const Field& f) {
    if (dash_slot_ < 0 || cast_ || moved_ || move_failed_) return false;
    auto& me = cambc::self();
    const int cur = f.at(pos_);
    if (cur >= kFar || cur <= 3) return false;
    const auto ab = me.abilities()[static_cast<size_t>(dash_slot_)];
    if (!ab.is_ready() || !me.can_cast()) return false;
    vec2 best{-1, -1};
    int bv = cur - 2;
    for (auto d : kDirs) {
        const vec2 q1{pos_.x + d.x, pos_.y + d.y}, q2{pos_.x + 2 * d.x, pos_.y + 2 * d.y};
        if (!inside(q1) || hard_.at(q1) || champocc_.at(q1) || campocc_.at(q1)) continue;
        if (!free_tile(q2)) continue;
        const int v = f.at(q2);
        if (v < bv) { bv = v; best = q2; }
    }
    if (best.x < 0) return false;
    try {
        if (ab.use(best)) {
            cast_ = true;
            pos_ = best;
            return true;
        }
        move_failed_ = true;
    } catch (const std::exception& e) { warn("dash", e); move_failed_ = true; }
    return false;
}

bool Bot::step(const Field& f, int stop_at) {
    auto& me = cambc::self();
    if (moved_ || excl_cast_ || move_failed_ || !live()) return false;
    const int cur = here(f);
    if (cur <= stop_at) return false;
    if (!me.can_move()) return false;
    vec2 best{-1, -1};
    int bv = cur;
    for (auto d : kDirs) {
        const vec2 q{pos_.x + d.x, pos_.y + d.y};
        if (!free_tile(q)) continue;
        const int v = f.at(q);
        if (v < bv) { bv = v; best = q; }
    }
    if (best.x < 0) {
        // Blocked by units: take a sideways tile with the same distance once in a while.
        if (++stuck_ >= 2 && cur < kFar) {
            for (auto d : kDirs) {
                const vec2 q{pos_.x + d.x, pos_.y + d.y};
                if (free_tile(q) && f.at(q) <= cur && q != last_pos_) { best = q; break; }
            }
        }
        if (best.x < 0) return false;
    }
    try {
        const vec2 from = pos_;
        if (me.move_absolute(best)) {
            last_pos_ = from;
            pos_ = best;
            moved_ = true;
            if (bv < cur) stuck_ = 0;
            return true;
        }
        move_failed_ = true;
    } catch (const std::exception& e) { warn("step", e); move_failed_ = true; }
    return false;
}

// Short route around units to the given roots, computed live (cheap when we are close).
bool Bot::step_dynamic(const cambc::bitboard& roots) {
    auto& me = cambc::self();
    if (moved_ || excl_cast_ || move_failed_ || !me.can_move() || !live()) return false;
    if (cambc::fuel_remaining() < 25000) return false;
    try {
        auto walls = walls_camps_ | cambc::vision::champion_tiles() | cambc::vision::camp_tiles();
        // Dead camps leave their tiles free.
        for (size_t g = 0; g < g_.size(); ++g)
            for (size_t c = 0; c < g_[g].tiles.size(); ++c) {
                const vec2 t = g_[g].tiles[c];
                if (vis_.at(t) && !campocc_.at(t)) walls.set(t, false);
            }
        walls.set(pos_, false);
        auto dest = empty_board();
        dest.set(pos_);
        cambc::distance_field f;
        cambc::bitboard visited;
        f.compute_to(walls & ~roots, roots, dest, visited, 40);
        const int cur = f.get(pos_);
        if (cur >= kFar) return false;
        vec2 best{-1, -1};
        int bv = cur;
        for (auto d : kDirs) {
            const vec2 q{pos_.x + d.x, pos_.y + d.y};
            if (!free_tile(q)) continue;
            const int v = f.get(q);
            if (v < bv) { bv = v; best = q; }
        }
        if (best.x < 0) return false;
        if (me.move_absolute(best)) {
            last_pos_ = pos_;
            pos_ = best;
            moved_ = true;
            return true;
        }
        move_failed_ = true;
    } catch (const std::exception& e) { warn("dyn", e); move_failed_ = true; }
    return false;
}

bool Bot::step_away(const std::vector<vec2>& threats, const std::vector<int>& ranges) {
    auto& me = cambc::self();
    if (moved_ || excl_cast_ || move_failed_ || !me.can_move() || !live()) return false;
    auto score = [&](vec2 p) {
        int s = 0;
        for (size_t i = 0; i < threats.size(); ++i) {
            const int d = cheb(p, threats[i]);
            s += std::min(d, ranges[i] + 3) * 100;
        }
        const int h = home_.at(p);
        return s - std::min(h, 500);
    };
    vec2 best = pos_;
    int bs = score(pos_);
    for (auto d : kDirs) {
        const vec2 q{pos_.x + d.x, pos_.y + d.y};
        if (!free_tile(q)) continue;
        const int s = score(q);
        if (s > bs) { bs = s; best = q; }
    }
    if (best == pos_) return false;
    try {
        if (me.move_absolute(best)) {
            pos_ = best;
            moved_ = true;
            return true;
        }
        move_failed_ = true;
    } catch (const std::exception& e) { warn("away", e); move_failed_ = true; }
    return false;
}

// ---------------------------------------------------------------- combat with camps

bool Bot::hit_group(int g) {
    auto& me = cambc::self();
    if (attacked_ && !me.can_attack()) return false;
    if (!me.can_attack() || !live()) return false;
    const int range = me.attack_range();
    const cambc::camp* best = nullptr;
    float bs = -1e9f;
    int ci = goff_[static_cast<size_t>(g)];
    for (const auto& camp : h_[static_cast<size_t>(g)].camps()) {
        const int idx = ci++;
        const vec2 sp = camp.spawn_position();
        if (cheb(sp, pos_) > range || !vis_.at(sp) || !campocc_.at(sp)) continue;
        try {
            if (!camp.is_visible()) continue;
            if (camp.state().is_dead()) continue;
            const float hp = camp.health();
            if (hp <= 0) continue;
            chp_[static_cast<size_t>(idx)] = hp;
            const auto spec = camp_spec(camp.kind());
            float s = spec.str > 0 ? 1000.0f * spec.str / hp : 0.0f;
            s -= hp * 0.001f;
            if (altar_plugged_ && g == raid_cand_)
                for (auto t : altar_plug_tiles_)
                    if (t == sp) s += 1e6f;
            if (s > bs) { bs = s; best = &camp; }
        } catch (const cambc::error&) {}
    }
    if (!best) return false;
    try {
        me.attack(*best);
        attacked_ = true;
        last_hit_round_ = now_;
    } catch (const std::exception& e) { warn("hit", e); return false; }
    return true;
}

void Bot::after_attack(int g, const Unit* foe) {
    auto& me = cambc::self();
    for (int depth = 0; depth < 4 && live(); ++depth) {
        bool again = false;
        try { again = me.can_attack(); } catch (const cambc::error&) {}
        if (!again) {
            if (reset_slot_ < 0 || cast_) return;
            const auto ab = me.abilities()[static_cast<size_t>(reset_slot_)];
            try {
                if (!ab.is_ready() || !me.can_cast()) return;
                if (!ab.use()) return;
                cast_ = true;
            } catch (const std::exception& e) { warn("reset", e); return; }
        }
        bool hit = false;
        if (g >= 0) hit = hit_group(g);
        else if (foe) hit = attack_foe(*foe);
        if (!hit) return;
    }
}

void Bot::spells_on_group(int g, bool before_move) {
    auto& me = cambc::self();
    if (cast_ || !live()) return;
    const auto abs = me.abilities();
    for (int i = 0; i < 2; ++i) {
        const Slot& sl = kit_[i];
        if (!sl.used || cast_) continue;
        if (sl.kind == cambc::ability_kind::attack_reset || sl.kind == cambc::ability_kind::dash) continue;
        if (sl.excl && (moved_ || !before_move)) continue;
        const auto& ab = abs[static_cast<size_t>(i)];
        try {
            if (!ab.is_ready() || !me.can_cast()) continue;
            if (sl.kind == cambc::ability_kind::shield) {
                if (incoming_camp_damage() > 0 && ab.use()) { cast_ = true; if (sl.excl) excl_cast_ = true; }
                continue;
            }
            if (sl.kind == cambc::ability_kind::heal) {
                const Unit* best = nullptr;
                float miss = ab.heal_amount() * 0.7f;
                for (const auto& f : friends_)
                    if (cheb(f.pos, pos_) <= static_cast<int>(sl.range) && f.max_hp - f.hp > miss) { miss = f.max_hp - f.hp; best = &f; }
                if (best) {
                    const auto c = cambc::vision::champion_at(best->pos);
                    if (ab.use(c)) { cast_ = true; if (sl.excl) excl_cast_ = true; }
                }
                continue;
            }
            if (!sl.camps) continue;
            // Damage, area damage or stun on a camp of this group within range.
            const int r = static_cast<int>(sl.range);
            const cambc::camp* best = nullptr;
            vec2 best_tile{-1, -1};
            float bs = -1e9f;
            int ci = goff_[static_cast<size_t>(g)];
            for (const auto& camp : h_[static_cast<size_t>(g)].camps()) {
                const int idx = ci++;
                const vec2 sp = camp.spawn_position();
                if (cheb(sp, pos_) > r || !vis_.at(sp) || !campocc_.at(sp)) continue;
                if (chp_[static_cast<size_t>(idx)] <= 0) continue;
                if (!camp.is_visible() || camp.state().is_dead()) continue;
                const auto spec = camp_spec(camp.kind());
                float s = 0;
                if (sl.kind == cambc::ability_kind::cc) {
                    if (spec.str <= 0 || !camp.is_awake()) continue;
                    s = spec.str;
                } else if (sl.kind == cambc::ability_kind::aoe_damage) {
                    int n = 0;
                    for (int cj = goff_[static_cast<size_t>(g)]; cj < goff_[static_cast<size_t>(g) + 1]; ++cj)
                        if (chp_[static_cast<size_t>(cj)] > 0 && cheb(g_[static_cast<size_t>(g)].tiles[static_cast<size_t>(cj - goff_[static_cast<size_t>(g)])], sp) <= 1) ++n;
                    s = static_cast<float>(n);
                } else {
                    const float hp = camp.health();
                    s = std::min(hp, ab.damage_amount()) + (spec.str > 0 ? 50.0f : 0.0f);
                }
                if (s > bs) { bs = s; best = &camp; best_tile = sp; }
            }
            if (!best) continue;
            if (sl.kind == cambc::ability_kind::cc && bs < 30) continue;
            if (!ab.valid_tiles().is_set(best_tile)) continue;
            bool ok = false;
            if (sl.kind == cambc::ability_kind::aoe_damage) ok = ab.use(best_tile);
            else ok = ab.use(*best);
            if (ok) { cast_ = true; if (sl.excl) excl_cast_ = true; }
        } catch (const std::exception& e) { warn("spell", e); }
    }
}

void Bot::engage(int g) {
    auto& me = cambc::self();
    const int range = me.attack_range();
    // Is any living camp of the group already within reach?
    bool in_range = false;
    for (size_t c = 0; c < g_[static_cast<size_t>(g)].tiles.size(); ++c) {
        const vec2 t = g_[static_cast<size_t>(g)].tiles[c];
        if (chp_[static_cast<size_t>(goff_[static_cast<size_t>(g)]) + c] > 0 && campocc_.at(t) && vis_.at(t) && cheb(t, pos_) <= range) {
            in_range = true;
            break;
        }
    }
    if (in_range) {
        if (hit_group(g)) after_attack(g, nullptr);
        spells_on_group(g, true);
        return;
    }
    debug_block({
        if (false) {
            std::cout << "engage r" << now_ << " g=" << g << " pos=" << pos_.x << ',' << pos_.y << " range=" << range;
            for (size_t c = 0; c < g_[static_cast<size_t>(g)].tiles.size(); ++c) {
                const vec2 t = g_[static_cast<size_t>(g)].tiles[c];
                std::cout << " [" << t.x << ',' << t.y << " hp=" << chp_[static_cast<size_t>(goff_[static_cast<size_t>(g)]) + c] << " occ=" << campocc_.at(t) << " vis=" << vis_.at(t) << ']';
            }
            std::cout << " moved=" << moved_ << " canmove=" << me.can_move() << " canatk=" << me.can_attack() << '\n';
        }
    });
    // Walk in, then strike if the step brought us within reach.
    compute_group_field(g);
    const auto& f = gf_[static_cast<size_t>(g)];
    const int d = here(f);
    ph();
    if (d <= range + 3) {
        // Close: aim at tiles from which a living camp is within reach.
        auto alive = empty_board();
        bool any = false;
        for (size_t c = 0; c < g_[static_cast<size_t>(g)].tiles.size(); ++c) {
            const vec2 t = g_[static_cast<size_t>(g)].tiles[c];
            if (chp_[static_cast<size_t>(goff_[static_cast<size_t>(g)]) + c] <= 0 || cheb(t, pos_) > range + 8) continue;
            alive.set(t);
            any = true;
        }
        const auto roots = any && cambc::fuel_remaining() > 40000 ? grow(alive, range) & ~cambc::map::walls() : empty_board();
        any = any && cambc::fuel_remaining() > 40000;
        ph();
        if (any) step_dynamic(roots);
        ph();
    }
    if (!moved_) {
        try_dash(f);
        step(f, range >= 3 ? 1 : 0);
    }
    if (hit_group(g)) after_attack(g, nullptr);
    else if (range >= 3 && attacked_ == false && reset_slot_ >= 0) {
        // Ranged: the walk spent the attack; a reset gives it back.
        bool target = false;
        for (size_t c = 0; c < g_[static_cast<size_t>(g)].tiles.size(); ++c) {
            const vec2 t = g_[static_cast<size_t>(g)].tiles[c];
            if (chp_[static_cast<size_t>(goff_[static_cast<size_t>(g)]) + c] > 0 && campocc_.at(t) && vis_.at(t) && cheb(t, pos_) <= range) target = true;
        }
        if (target && !cast_) {
            try {
                const auto ab = me.abilities()[static_cast<size_t>(reset_slot_)];
                if (ab.is_ready() && me.can_cast() && !me.can_attack() && ab.use()) {
                    cast_ = true;
                    hit_group(g);
                }
            } catch (const std::exception& e) { warn("walkreset", e); }
        }
    }
    spells_on_group(g, false);
}

// Heal the most hurt ally in reach (Willow); compatible with walking, so do it first.
void Bot::heal_allies() {
    auto& me = cambc::self();
    for (int i = 0; i < 2; ++i) {
        const Slot& sl = kit_[i];
        if (!sl.used || sl.kind != cambc::ability_kind::heal || cast_) continue;
        if (sl.excl && moved_) continue;
        try {
            const auto ab = me.abilities()[static_cast<size_t>(i)];
            if (!ab.is_ready() || !me.can_cast()) continue;
            const float amount = ab.heal_amount();
            const Unit* best = nullptr;
            float miss = amount * 0.6f;
            for (const auto& f : friends_) {
                if (f.recalling || f.on_spawn || !f.fresh) continue;
                if (cheb(f.pos, pos_) > static_cast<int>(sl.range)) continue;
                if (f.max_hp - f.hp > miss) { miss = f.max_hp - f.hp; best = &f; }
            }
            if (!best) continue;
            if (!cambc::vision::has_champion(best->pos)) continue;
            const auto c = cambc::vision::champion_at(best->pos);
            if (ab.use(c)) { cast_ = true; if (sl.excl) excl_cast_ = true; }
        } catch (const std::exception& e) { warn("heal", e); }
    }
}

// ---------------------------------------------------------------- combat with champions

bool Bot::attack_foe(const Unit& u) {
    auto& me = cambc::self();
    if (!me.can_attack() || !live()) return false;
    if (cheb(u.pos, pos_) > me.attack_range()) return false;
    try {
        if (!cambc::vision::has_champion(u.pos)) return false;
        const auto c = cambc::vision::champion_at(u.pos);
        if (!c.is_visible() || !c.is_alive()) return false;
        me.attack(c);
        attacked_ = true;
        return true;
    } catch (const std::exception& e) { warn("hitfoe", e); }
    return false;
}

void Bot::spells_on_foe(const Unit& u, bool before_move) {
    auto& me = cambc::self();
    if (cast_ || !live()) return;
    const auto abs = me.abilities();
    for (int i = 0; i < 2; ++i) {
        const Slot& sl = kit_[i];
        if (!sl.used || cast_) continue;
        if (sl.excl && (moved_ || !before_move)) continue;
        const auto& ab = abs[static_cast<size_t>(i)];
        try {
            if (!ab.is_ready() || !me.can_cast()) continue;
            if (sl.kind == cambc::ability_kind::shield) {
                if (ab.use()) { cast_ = true; if (sl.excl) excl_cast_ = true; }
                continue;
            }
            if (!sl.enemies) continue;
            if (sl.kind != cambc::ability_kind::damage && sl.kind != cambc::ability_kind::cc
                && sl.kind != cambc::ability_kind::aoe_damage) continue;
            if (cheb(u.pos, pos_) > static_cast<int>(sl.range)) continue;
            if (!ab.valid_tiles().is_set(u.pos)) continue;
            const auto c = cambc::vision::champion_at(u.pos);
            if (!c.is_visible()) continue;
            bool ok = sl.kind == cambc::ability_kind::aoe_damage ? ab.use(u.pos) : ab.use(c);
            if (ok) { cast_ = true; if (sl.excl) excl_cast_ = true; }
        } catch (const std::exception& e) { warn("foespell", e); }
    }
}

bool Bot::fight() {
    auto& me = cambc::self();
    if (foes_.empty()) return false;
    const float my_hp = me.health(), my_max = me.max_health();
    const float my_power = 1.1f * me.strength() + 0.3f * me.focus();
    const int my_range = me.attack_range();
    float ours = my_hp * my_power, theirs = 0;
    for (const auto& f : friends_)
        if (cheb(f.pos, pos_) <= 7 && !f.recalling) ours += f.hp * f.power;
    std::vector<vec2> threats;
    std::vector<int> ranges;
    float threat_dmg = 0;
    std::vector<const Unit*> reach;  // foes we can hit now
    for (const auto& u : foes_) {
        const int d = cheb(u.pos, pos_);
        if (d > 8) continue;
        if (u.on_spawn && d > my_range) continue;
        theirs += u.hp * u.power;
        if (d <= u.range + 1) { threats.push_back(u.pos); ranges.push_back(u.range); threat_dmg += u.power; }
        if (d <= my_range) reach.push_back(&u);
    }
    if (theirs <= 0) return false;
    const float ratio = ours / theirs;
    const bool low = my_hp < my_max * 0.3f || (threat_dmg > 0 && my_hp < threat_dmg * 2.5f);
    // A foe we can finish this turn is always worth the swing.
    float burst = me.strength() * (reset_slot_ >= 0 ? 2.0f : 1.0f);
    for (int i = 0; i < 2; ++i)
        if (kit_[i].used && kit_[i].kind == cambc::ability_kind::damage) burst += kit_[i].mod * me.focus();
    const Unit* tgt = nullptr;
    float bs = -1e9f;
    for (const Unit* u : reach) {
        float s = -u->hp;
        if (u->hp <= burst * 0.9f) s += 5000;
        if (u->recalling || u->tributing) s += 300;
        if (s > bs) { bs = s; tgt = u; }
    }
    const bool killshot = tgt && tgt->hp <= burst * 0.9f;
    debug_block({
        if (now_ < 400) {
            std::cout << "fight r" << now_ << ' ' << cambc::to_string(type_) << " pos=" << pos_.x << ',' << pos_.y << " hp=" << my_hp << " ratio=" << ratio << " low=" << low << " reach=" << reach.size() << " threats=" << threats.size() << " foes:";
            for (const auto& u : foes_) std::cout << " (" << u.pos.x << ',' << u.pos.y << " hp=" << u.hp << " pw=" << u.power << " r=" << u.range << ")";
            std::cout << " friends:";
            for (const auto& u : friends_) std::cout << " (" << u.pos.x << ',' << u.pos.y << " hp=" << u.hp << (u.fresh ? "" : "*") << ")";
            std::cout << '\n';
        }
    });
    const bool retreat = !killshot && (low ? ratio < 2.0f : ratio < (now_ <= fight_until_ ? 0.6f : 0.8f));
    if (tgt && !retreat) {
        // Stand and hit what is in reach: spells, attack, reset and attack again.
        fight_until_ = now_ + 3;
        spells_on_foe(*tgt, true);
        if (attack_foe(*tgt)) after_attack(-1, tgt);
        spells_on_foe(*tgt, false);
        chase_ = 0;
        return true;
    }
    if (!tgt && !retreat && !low && ratio >= 1.3f) {
        // Nobody in reach: step in only on a target one or two steps away.
        const Unit* near = nullptr;
        int nd = 99;
        for (const auto& u : foes_) {
            const int d = cheb(u.pos, pos_);
            if (u.on_spawn) continue;
            // Reach out further for a foe that is on one of our champions.
            bool on_friend = false;
            for (const auto& f : friends_)
                if (!f.recalling && cheb(f.pos, u.pos) <= std::max(u.range, 2) + 1) on_friend = true;
            if (d > my_range + (on_friend ? 6 : 2)) continue;
            if (d < nd || (d == nd && near && u.hp < near->hp)) { nd = d; near = &u; }
        }
        if (near && ++chase_ <= 8) {
            fight_until_ = now_ + 3;
            if (dash_slot_ >= 0 && !cast_ && nd - my_range == 2) {
                try {
                    const auto ab = me.abilities()[static_cast<size_t>(dash_slot_)];
                    if (ab.is_ready() && me.can_cast()) {
                        vec2 best{-1, -1};
                        int bd = nd;
                        for (int dy = -2; dy <= 2; ++dy)
                            for (int dx = -2; dx <= 2; ++dx) {
                                const vec2 q{pos_.x + dx, pos_.y + dy};
                                if ((dx == 0 && dy == 0) || !free_tile(q)) continue;
                                const vec2 mid{pos_.x + dx / 2, pos_.y + dy / 2};
                                if (mid != pos_ && hard_.at(mid)) continue;
                                const int v = cheb(q, near->pos);
                                if (v < bd) { bd = v; best = q; }
                            }
                        if (best.x >= 0 && ab.use(best)) { cast_ = true; pos_ = best; }
                    }
                } catch (const std::exception& e) { warn("chasedash", e); }
            }
            if (cheb(near->pos, pos_) > my_range && me.can_move() && !moved_ && !move_failed_) {
                vec2 best{-1, -1};
                int bd = cheb(near->pos, pos_);
                for (auto dd : kDirs) {
                    const vec2 q{pos_.x + dd.x, pos_.y + dd.y};
                    if (!free_tile(q)) continue;
                    const int v = cheb(q, near->pos);
                    if (v < bd) { bd = v; best = q; }
                }
                if (best.x >= 0) {
                    try {
                        if (me.move_absolute(best)) { pos_ = best; moved_ = true; }
                        else move_failed_ = true;
                    } catch (const std::exception& e) { warn("chase", e); move_failed_ = true; }
                }
            }
            if (attack_foe(*near)) after_attack(-1, near);
            else if (my_range >= 3 && reset_slot_ >= 0 && cheb(near->pos, pos_) <= my_range && !cast_) {
                try {
                    const auto ab = me.abilities()[static_cast<size_t>(reset_slot_)];
                    if (ab.is_ready() && me.can_cast() && !me.can_attack() && ab.use()) {
                        cast_ = true;
                        attack_foe(*near);
                    }
                } catch (const std::exception& e) { warn("chasereset", e); }
            }
            spells_on_foe(*near, false);
            return true;
        }
        return false;
    }
    if (threats.empty() && !(retreat && me.gem_count() >= 20)) {
        if (!retreat) chase_ = 0;
        return false;
    }
    // Losing or low: back off towards home, hitting back when it is free.
    fight_until_ = -1;
    flee_until_ = now_ + 4;
    if (tgt && my_range <= 1 && attack_foe(*tgt)) after_attack(-1, tgt);
    for (const auto& u : foes_)
        if (cheb(u.pos, pos_) <= 4) { spells_on_foe(u, true); break; }
    if (!step_away(threats.empty() ? std::vector<vec2>{foes_[0].pos} : threats,
                   ranges.empty() ? std::vector<int>{foes_[0].range} : ranges)) {
        if (tgt && attack_foe(*tgt)) after_attack(-1, tgt);  // cornered: fight back
    }
    return true;
}

// ---------------------------------------------------------------- health, gems

bool Bot::hurt() {
    auto& me = cambc::self();
    const float hp = me.health(), mx = me.max_health();
    if (spawn_.at(pos_) && hp < mx * 0.95f) return true;  // healing on spawn this turn
    const float inc = incoming_camp_damage();
    bool low = hp < mx * 0.25f || (inc > 0 && hp < inc * 2.5f + 40);
    if (!low) return false;
    // Finish a nearly dead group if we survive it.
    if (target_ >= 0 && k_[static_cast<size_t>(target_)].st == Known::alive && inc > 0) {
        const float left = group_remaining(target_) / dps();
        if (left <= 2.0f && hp > inc * (left + 1.5f)) return false;
    }
    // Out of camp reach first, then recall.
    std::vector<vec2> threats;
    std::vector<int> ranges;
    for (size_t g = 0; g < g_.size(); ++g) {
        if (k_[g].st != Known::alive || cheb(g_[g].center, pos_) > 10) continue;
        for (size_t c = 0; c < g_[g].tiles.size(); ++c) {
            const auto spec = camp_spec(g_[g].kinds[c]);
            if (spec.str <= 0 || chp_[static_cast<size_t>(goff_[g]) + c] <= 0 || !campocc_.at(g_[g].tiles[c])) continue;
            if (!cawake_[static_cast<size_t>(goff_[g]) + c]) continue;
            if (cheb(g_[g].tiles[c], pos_) <= spec.range) { threats.push_back(g_[g].tiles[c]); ranges.push_back(spec.range); }
        }
    }
    if (inc > 0 || !threats.empty()) {
        if (step_away(threats, ranges)) return true;
        if (inc > 0) return false;  // cannot get out: keep hitting
    }
    if (here(home_) > 6 && !me.is_recalling()) {
        try {
            me.recall();
            return true;
        } catch (const std::exception& e) { warn("recall", e); }
    }
    step(home_, 0);
    return true;
}

bool Bot::tribute() {
    auto& me = cambc::self();
    const int gems = static_cast<int>(me.gem_count());
    if (gems <= 0) return false;
    const int da = here(altar_);
    debug_block({
        if (type_ == cambc::champion_type::vendetta && now_ == 50) {
            std::cout << "range=" << cambc::constants::submit_gems_range << " da=" << da << "\n";
            for (auto a : altars_) for (int dy=-2; dy<=2; ++dy) { for (int dx=-2; dx<=2; ++dx) { vec2 q{a.x+dx,a.y+dy}; std::cout << (cambc::map::walls().is_set(q)?'#':cambc::map::holes().is_set(q)?'o':campspawn_.at(q)?'c':cambc::map::altars().is_set(q)?'A':'.'); } std::cout << '\n'; }
        }
    });
    if (da >= kFar) return false;
    const int total = static_cast<int>(cambc::global_submitted_gems(team::ally));
    const bool endgame = now_ + da + (gems + 9) / 10 + 12 >= 2000;
    bool want = gems >= 45 || (gems >= 20 && da <= 8) || (gems >= 8 && da <= 2) || endgame
        || total + gems >= 1000 || (gems >= 10 && !foes_.empty());
    debug_block({
        if (type_ == cambc::champion_type::vendetta && now_ % 10 == 0)
            std::cout << "trib r" << now_ << " pos=" << pos_.x << ',' << pos_.y << " gems=" << gems << " da=" << da << " want=" << want << " plugged=" << altar_plugged_ << '\n';
    });
    if (!want) return false;
    if (altar_plugged_) {
        // Only once the camp on a tribute spot is dead (as last seen).
        bool open = false;
        for (auto t : altar_plug_tiles_) {
            const int pg = group_at_[static_cast<size_t>(tidx(t))];
            if (pg < 0) continue;
            if (k_[static_cast<size_t>(pg)].st != Known::alive) { open = true; continue; }
            for (size_t c = 0; c < g_[static_cast<size_t>(pg)].tiles.size(); ++c)
                if (g_[static_cast<size_t>(pg)].tiles[c] == t && chp_[static_cast<size_t>(goff_[static_cast<size_t>(pg)]) + c] <= 0) open = true;
        }
        if (!open) return false;
    }
    if (da == 0) {
        for (auto a : altars_) {
            if (cheb(a, pos_) > 1) continue;
            try {
                if (cambc::vision::has_altar_lock(a) && cambc::vision::altar_lock_at(a).is_enemy()) continue;
                me.submit_gems(a);
                return true;
            } catch (const std::exception& e) { warn("tribute", e); }
        }
        return false;
    }
    try_dash(altar_);
    step(altar_, 0);
    return true;
}

// ---------------------------------------------------------------- boss raids

// Judged on our own stats as a stand-in for the team: can everyone together outlast the boss?
float Bot::plug_hp() const {
    float s = 0;
    for (auto t : altar_plug_tiles_) {
        const int pg = group_at_[static_cast<size_t>(tidx(t))];
        if (pg < 0) continue;
        for (size_t c = 0; c < g_[static_cast<size_t>(pg)].tiles.size(); ++c)
            if (g_[static_cast<size_t>(pg)].tiles[c] == t) s += chp_[static_cast<size_t>(goff_[static_cast<size_t>(pg)]) + c];
    }
    return s;
}

bool Bot::raid_ready() const {
    if (raid_cand_ < 0 || now_ < 150) return false;
    const auto& me = cambc::self();
    const auto& G = g_[static_cast<size_t>(raid_cand_)];
    const float rem = group_remaining(raid_cand_);
    const float str = std::max(1.0f, G.str * taken_);
    const float pool = me.max_health() * bodies_ * 0.75f;
    const float turns = pool / str;
    const float team = dps() * bodies_ * 0.8f;
    return team * turns >= rem * 1.1f;
}

void Bot::update_raid() {
    raid_on_ = false;
    if (raid_cand_ < 0) return;
    std::uint8_t v = 0;
    try { cambc::shared_memory.read(sm::raid, std::span<std::uint8_t>{&v, 1}); } catch (const std::exception&) { return; }
    const auto& k = k_[static_cast<size_t>(raid_cand_)];
    if (v == raid_cand_ + 1) {
        if (k.st == Known::respawning) {
            const std::uint8_t z = 0;
            try { cambc::shared_memory.write(sm::raid, std::span<const std::uint8_t>{&z, 1}); } catch (const std::exception&) {}
            return;
        }
        raid_on_ = true;
        return;
    }
    if (squad_ == 0 && leader() && k.st == Known::alive && raid_ready()) {
        const std::uint8_t w = static_cast<std::uint8_t>(raid_cand_ + 1);
        try { cambc::shared_memory.write(sm::raid, std::span<const std::uint8_t>{&w, 1}); } catch (const std::exception&) {}
        raid_on_ = true;
    }
}

// ---------------------------------------------------------------- farming

int Bot::choose(int& eta_out, int near_group) {
    auto& me = cambc::self();
    // Plan with the squad as drafted: allies out of sight cannot be inspected.
    const float team = dps() * (1.0f + 0.85f * (squad_n_ - 1));
    const float team_hp = me.health() + 0.8f * me.max_health() * (squad_n_ - 1);
    const float gold_w = std::max(0.15f, 0.45f * (1.0f - now_ / 1600.0f));
    int best = -1;
    float bs = 0, cur = -1;
    eta_out = kFar;
    for (size_t gi = 0; gi < g_.size(); ++gi) {
        const int g = static_cast<int>(gi);
        const auto& G = g_[gi];
        const auto& k = k_[gi];
        if (G.tiles.empty() || k.fail > now_) continue;
        if (near_group >= 0 && cheb(G.center, g_[static_cast<size_t>(near_group)].center) > 7) continue;
        if (G.monolith && !(plan_ == kBoss && g == boss_)) continue;
        if (g == raid_cand_ && G.boss && !raid_on_ && !(plan_ == kBoss && g == boss_)) continue;
        const int d = dist_to_group(g);
        if (d >= kFar) continue;
        float wait = 0;
        if (k.st == Known::respawning) wait = std::max(0, k.ready - now_ - d);
        if (wait > 80) continue;
        const float rem = k.st == Known::alive ? group_remaining(g) : (k.st == Known::respawning ? G.hp : 0.0f);
        const float clear = rem / team;
        // Damage the squad would take, against what it can absorb.
        // Big groups cannot all reach one champion: count at most ~6 attackers' worth.
        const float reach_str = std::min(G.str, 6.0f * G.str / std::max<size_t>(1, G.tiles.size()) + 120.0f);
        const float hits = reach_str * taken_ * clear;
        const float trips = std::floor(hits / std::max(200.0f, team_hp * 0.6f));
        if (trips > 10) continue;
        const float trip_cost = std::min(d_home_[gi], 60) + 10.0f;
        const float value = G.gems + gold_w * G.gold;
        float f = 1.0f;
        const auto own = owner_[gi];
        if (own == squad_ || nsquads_ == 1) f = 1.0f;
        else if (own == 255) f = 0.8f;
        else f = 0.6f;
        if (G.d_enemy < G.d_ally) f *= 0.85f;
        // Loose squad: a member spreads to a cheap neighbour of the leader's target.
        if (near_group >= 0) {
            if (g == near_group) f *= 0.6f;
            else if (needed(g, squad_n_) > 1) f *= 0.3f;
        }
        const float score = value * f / (d + wait + clear + 5.0f + trips * trip_cost + 6.0f);
        if (g == target_) cur = score;
        if (score > bs) { bs = score; best = g; eta_out = d; }
    }
    // Commit: switch only for a clearly better target.
    if (cur > 0 && bs < cur * 1.4f) return target_;
    return best;
}

void Bot::farm() {
    int eta = kFar;
    int g = -1;
    const bool lead = leader();
    if (raid_on_) g = raid_cand_;
    else if (!lead) {
        g = read_target();
        // The leader's target is cheap: farm a group next to it instead of crowding it.
        if (g >= 0 && k_[static_cast<size_t>(g)].st == Known::alive && needed(g, squad_n_) <= 1 && loose_) {
            int e2 = kFar;
            const int own = choose(e2, g);
            if (own >= 0) g = own;
        }
    }
    if (g >= 0 && k_[static_cast<size_t>(g)].fail > now_ && !raid_on_) g = -1;
    ph();
    if (g < 0) {
        g = choose(eta);
        if (lead) write_target(g);
    }
    ph();
    if (g != target_) { target_ = g; target_since_ = now_; last_dist_ = kFar; gather_ = 0; }
    if (g < 0) {
        step(home_, 2);
        return;
    }
    compute_group_field(g);
    ph();
    const auto& k = k_[static_cast<size_t>(g)];
    const auto& f = gf_[static_cast<size_t>(g)];
    // Give up on a target we cannot get closer to.
    const int d = dist_to_group(g);
    if (d < last_dist_ || attacked_) { last_dist_ = d; target_since_ = now_; }
    else if (now_ - target_since_ > 25 && now_ - last_hit_round_ > 25 && !raid_on_) {
        k_[static_cast<size_t>(g)].fail = now_ + 60;
        target_ = -1;
        if (lead) write_target(-1);
        return;
    }
    debug_block({
        if (type_ == cambc::champion_type::vendetta && now_ < 300)
            std::cout << "trace r" << now_ << " pos=" << pos_.x << ',' << pos_.y << " tgt=" << g << " st=" << int(k.st) << " d=" << d
                      << " lead=" << lead << " hp=" << cambc::self().health() << " gather=" << gather_ << " fuel=" << cambc::fuel_remaining() << '\n';
    });
    if (k.st == Known::alive) {
        // A dangerous group still asleep: wait just outside its reach for the squad.
        const auto& G = g_[static_cast<size_t>(g)];
        bool awake = false;
        for (int c = goff_[static_cast<size_t>(g)]; c < goff_[static_cast<size_t>(g) + 1]; ++c)
            if (chp_[static_cast<size_t>(c)] < camp_spec(G.kinds[static_cast<size_t>(c - goff_[static_cast<size_t>(g)])]).hp) awake = true;
        if (!awake && G.str > 0) {
            auto& me = cambc::self();
            if (me.health() < me.max_health() * 0.5f && foes_.empty() && here(home_) > 6 && !me.is_recalling()) {
                try { me.recall(); g_mode = 3; return; } catch (const std::exception& e) { warn("healrecall", e); }
            }
            int near = 1;
            const bool raid = raid_on_ && g == raid_cand_;
            for (const auto& fr : friends_) {
                if ((fr.squad != squad_ && !raid) || fr.recalling) continue;
                if (cheb(fr.pos, pos_) <= (raid ? 6 : 4)) ++near;
            }
            const int need = raid ? std::max(2, bodies_ - 1) : needed(g, squad_n_);
            if (near < need && gather_ < (raid ? 25 : 12)) {
                if (d > G.range + 1) { try_dash(f); step(f, G.range + 1); }
                else ++gather_;
                g_mode = 9;
                return;
            }
        }
        engage(g);
        g_mode = attacked_ ? 6 : 7;
        return;
    }
    if (k.st == Known::reward) {
        g_mode = 8;
        if (on_group_tile(g)) return;  // capturing
        try_dash(f);
        step(f, 0);
        return;
    }
    // Respawning: wait next to it, off its spawn tiles.
    g_mode = 9;
    if (d > 2) { try_dash(f); step(f, 2); }
    else if (on_group_tile(g)) {
        std::vector<vec2> t{pos_};
        std::vector<int> r{0};
        step_away(t, r);
    }
}

// ---------------------------------------------------------------- turn

void Bot::turn() {
    auto& me = cambc::self();
    if (!me.is_alive()) {
        g_mode = 0;
        shop();
        return;
    }
    pos_ = me.position();
    if (!inside(pos_)) return;
    ph();
    observe();
    ph();
    if (me.is_recalling() || me.is_submitting_gems() || me.is_teleporting()) { g_mode = 1; return; }
    read_units();
    ph();
    update_raid();
    debug_block({
        if (altar_plugged_ && now_ % 20 == 0)
            std::cout << "RAID r" << now_ << ' ' << cambc::to_string(type_) << " on=" << raid_on_ << " ready=" << raid_ready() << " plug=" << plug_hp()
                      << " gems=" << me.gem_count() << " pos=" << pos_.x << ',' << pos_.y << " hp=" << me.health() << " mode=" << g_mode << '\n';
    });
    taken_ = 1.0f;
    dealt_ = 1.0f;
    try {
        const auto counts = me.item_count();
        if (counts[it::jungle_emblem_of_strength] + counts[it::jungle_emblem_of_health] + counts[it::jungle_emblem_of_focus] > 0) taken_ *= 0.7f;
        if (counts[it::durability_emblem_of_health] + counts[it::durability_emblem_of_strength] + counts[it::durability_emblem_of_focus] > 0) taken_ *= 0.9f;
        if (counts[it::monster_emblem_of_strength] + counts[it::monster_emblem_of_health] + counts[it::monster_emblem_of_focus] > 0) dealt_ *= 1.1f;
    } catch (const cambc::error&) {}
    if (spawn_.at(pos_) || type_ == cambc::champion_type::tavi) shop();
    if (!live()) return;
    heal_allies();
    ph();
    if (fight()) { g_mode = 2; return; }
    ph();
    if (!live()) return;
    if (hurt()) { g_mode = 3; return; }
    if (!live()) return;
    ph();
    if (tribute()) { g_mode = 4; return; }
    ph();
    if (!live()) return;
    farm();
}

void Bot::play_turn() {
    if (kStats && init_) {
        ++g_modes[g_mode];
        if (g_round % 250 == 0) {
            std::cout << "stat r" << g_round << ' ' << cambc::to_string(type_) << " gems=" << cambc::self().gem_count();
            for (int i = 0; i < 11; ++i) if (g_modes[i]) std::cout << ' ' << kModeNames[i] << '=' << g_modes[i];
            std::cout << '\n';
        }
    }
    g_mode = 5;
    g_round = cambc::round_number();
    now_ = static_cast<int>(g_round);
    moved_ = attacked_ = cast_ = excl_cast_ = move_failed_ = false;
    if (!init_) {
        // The first turn's double fuel goes to the static fields; acting waits a round.
        initialise();
        return;
    }
    const std::uint64_t fuel0 = cambc::fuel_remaining();
    g_nph = 0;
    turn();
    ph();
    debug_block({
        const std::uint64_t f1 = cambc::fuel_remaining();
        if (!live() || fuel0 - f1 > 70000)
        {
            std::cout << "FUEL r" << now_ << ' ' << cambc::to_string(type_) << " used=" << (live() ? fuel0 - f1 : 999999) << " mode=" << g_mode << " tgt=" << target_ << " ph:";
            for (int i = 1; i < g_nph; ++i) std::cout << ' ' << (g_ph[i - 1] - g_ph[i]);
            std::cout << '\n';
        }
    });
    if (kTable && type_ == cambc::champion_type::vendetta && now_ >= 2 && now_ < 2 + static_cast<int>(g_.size())) {
        const size_t i = static_cast<size_t>(now_ - 2);
        std::cout << "g" << i << " c=" << g_[i].center.x << ',' << g_[i].center.y << " n=" << g_[i].tiles.size() << " gems=" << g_[i].gems
                  << " gold=" << g_[i].gold << " hp=" << g_[i].hp << " str=" << g_[i].str << " resp=" << g_[i].respawn << " own=" << int(owner_[i])
                  << " home=" << home_.at(g_[i].center) << " altar=" << altar_.at(g_[i].center) << '\n';
    }
    if (kStats && g_round % 250 == 0) std::cout.flush();
    // Spend leftover fuel on distance fields for groups we may want later.
    if (live() && cambc::self().is_alive()) {
        while (cambc::fuel_remaining() > 50000) {
            int best = -1, bd = kFar;
            for (size_t g = 0; g < g_.size(); ++g)
                if (!gf_tried_[g]) {
                    const int d = cheb(g_[g].center, pos_);
                    if (d < bd) { bd = d; best = static_cast<int>(g); }
                }
            if (best < 0) break;
            const std::uint64_t fb = cambc::fuel_remaining();
            compute_group_field(best);
            debug_block({
                if (!live() || fb - cambc::fuel_remaining() > 30000) std::cout << "FIELD r" << now_ << " g=" << best << " cost=" << (live() ? fb - cambc::fuel_remaining() : 999999) << '\n';
            });
        }
    }
}
