// Camp sweep: farm camp groups in an efficient loop, bank gems often, avoid fair fights.
#include <cambc.h>
#include "bot.h"
#include "fight.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <string>
#include <cmath>
#include <cstdint>
#include <span>
#include <type_traits>
#include <variant>
#include <vector>

using cambc::team;

namespace {

constexpr std::array<vec2, 8> kDirs{vec2{0, 1}, vec2{1, 1}, vec2{1, 0}, vec2{1, -1},
                                    vec2{0, -1}, vec2{-1, -1}, vec2{-1, 0}, vec2{-1, 1}};
constexpr int kClaimBytes = 5;     // bytes 0..4: target group + 1 per ally slot
constexpr int kSightByte = 5;      // bytes 5..7: x, y, round & 255 of the latest foe sighting
constexpr int kSightRounds = 20;
constexpr float kEngage = 60.0f;   // fight value needed to engage
constexpr double kCohesion = 0.25; // score bonus per ally target within 10 tiles
constexpr int kWaryRounds = 15;    // after a sighting, heal sooner
constexpr float kBankPerTile = 1.5f; // carried gems per tile of altar distance that justify a trip
constexpr int kBankMin = 8;
constexpr int kBankCap = 50;
constexpr int kThreatPad = 2;      // retreat once a foe is this far outside its reach
constexpr bool kCornered = false;
constexpr bool kAvoidAwake = false;
constexpr int kSquads = 5;          // 5 = everyone farms alone; fewer = squads follow a leader
constexpr int kHoldMax = 25;       // rounds a squad member waits for the others at a big group
constexpr double kTheirSide = 0.6;
constexpr double kContested = 1.0;
constexpr float kWaryHeal = 0.5f;
constexpr bool kHelp = false;       // answer allies' sightings
constexpr int kHelpRange = 12;
constexpr bool kDumpMap = false;
constexpr bool kDeathTrace = false;
constexpr int kActReport = 590;
constexpr int kTraceFrom = 100000, kTraceTo = 0;

struct Facts {
    bool valid = false;
    cambc::ability_kind kind{};
    bool exclusive = false;
    float range = 0, radius = 0, modifier = 0;
};

Facts facts(const cambc::ability& a) {
    Facts f;
    const auto& inf = a.info();
    if (!inf) return f;
    f.valid = true;
    std::visit([&](const auto& x) {
        using T = std::decay_t<decltype(x)>;
        f.kind = T::kind;
        f.exclusive = x.exclusive_with_movement;
        if constexpr (requires { x.range; }) f.range = x.range;
        if constexpr (requires { x.radius; }) f.radius = x.radius;
        if constexpr (requires { x.modifier; }) f.modifier = x.modifier;
    }, *inf);
    return f;
}

bool in_bounds(vec2 p) {
    const auto& w = world();
    return p.x >= 0 && p.y >= 0 && p.x < w.w && p.y < w.h;
}

float camp_priority(const CampView& c) {
    // Damage removed per damage dealt; harmless clusters go last.
    return c.str > 0 ? 1000.0f * c.str / std::max(1.0f, c.hp) + 1.0f : 1.0f / std::max(1.0f, c.hp);
}

}  // namespace

void Bot::init() {
    inited_ = true;
    init_world();
    auto& me = cambc::self();
    type_ = me.champion_name();
    range_ = me.attack_range();
    ranged_ = range_ > 1;
    reach_ = range_;
    const auto allies = cambc::champions(team::ally);
    for (std::size_t i = 0; i < allies.size(); ++i)
        if (static_cast<cambc::handle_champion_t>(allies[i]) == static_cast<cambc::handle_champion_t>(me))
            slot_ = static_cast<int>(i);
    me_field_ = std::make_unique<cambc::distance_field>();
    travel_ = std::make_unique<cambc::distance_field>();
}

// Monster emblem (camp damage), Jungle emblem (camp damage taken), then raw strength.
void Bot::shop() {
    auto& me = cambc::self();
    using I = cambc::item_type;
    const auto counts = me.item_count();
    struct Step { I shard, emblem; };
    const std::array<Step, 2> plan{Step{I::monster_shard_of_strength, I::monster_emblem_of_strength},
                                   Step{I::jungle_shard_of_health, I::jungle_emblem_of_health}};
    for (int guard = 0; guard < 4; ++guard) {
        const unsigned gold = me.gold_count();
        if (gold < 30) return;
        I item = I::power_shard_of_strength;
        unsigned want = gold / 30;
        bool found = false;
        for (const auto& s : plan) {
            if (counts[s.emblem] > 0) continue;
            item = s.shard;
            const unsigned have = counts[s.shard];
            want = std::min(want, have < 5 ? 5 - have : 1u);
            found = true;
            break;
        }
        (void)found;
        if (want == 0) return;
        if (!me.can_buy(item, want)) return;
        me.buy(item, want);
        if (item == I::power_shard_of_strength) return;
        return;  // one purchase per turn keeps the emblem counts fresh
    }
}

void Bot::refresh_field() {
    if (round_ - field_round_ < 8 && cheb(field_origin_, pos_) <= 6) return;
    cambc::bitboard roots;
    roots.set(pos_, true);
    cambc::bitboard visited;
    me_field_->compute(world().terrain, roots, visited);
    field_round_ = round_;
    field_origin_ = pos_;
}

int Bot::field_at(vec2 p) {
    const int d = me_field_->get(p);
    if (d >= kFar) return kFar;
    return d + cheb(field_origin_, pos_);
}

int Bot::altar_distance() {
    if (world().altars.empty()) return kFar;
    return world().altar.get(pos_);
}

void Bot::scan_units() {
    foes_.clear();
    friends_.clear();
    for (auto p : cambc::vision::champion_tiles(team::enemy).positions()) {
        try {
            auto c = cambc::vision::champion_at(p);
            Foe f;
            f.pos = p;
            f.hp = c.health();
            f.max_hp = c.max_health();
            f.str = c.strength();
            f.focus = c.focus();
            f.range = c.attack_range();
            f.dist = cheb(p, pos_);
            f.channel = c.is_recalling() || c.is_submitting_gems();
            f.type = c.champion_name();
            f.gems = static_cast<int>(c.gem_count());
            foes_.push_back(f);
        } catch (...) {}
    }
    for (auto p : cambc::vision::champion_tiles(team::ally).positions()) {
        if (p == pos_) continue;
        try {
            auto c = cambc::vision::champion_at(p);
            friends_.push_back({p, c.health(), c.max_health(), c.strength(), c.focus(), c.attack_range(), c.champion_name()});
        } catch (...) {}
    }
}

void Bot::compute_blocked() {
    blocked_ = world().terrain | cambc::vision::champion_tiles() | cambc::vision::camp_tiles()
        | cambc::vision::teleport_tiles();
    // Awake camps of other groups: stay out of their reach.
    for (auto p : cambc::vision::awake_camp_tiles().positions()) {
        if (!kAvoidAwake) break;
        const int gi = group_of_tile(p);
        if (gi < 0 || gi == target_) continue;
        for (const auto& m : world().groups[gi].m)
            if (m.pos == p) blocked_.draw_chebyshev(p, static_cast<float>(m.range));
    }
    blocked_.set(pos_, false);
    phys_ = blocked_;
}

// Dash and step down a distance field that is valid at our position.
bool Bot::walk_field(const cambc::distance_field& field, bool allow_dash) {
    auto& me = cambc::self();
    if (moved_ || exclusive_cast_ || !me.can_move()) return false;
    const int here = field.get(pos_);
    if (here >= kFar || here == 0) return false;

    if (allow_dash && here >= 4 && me.can_cast()) {
        for (const auto& a : me.abilities()) {
            if (!a.is_dash() || !a.is_ready()) continue;
            vec2 best = pos_;
            int bd = here;
            for (int dx = -2; dx <= 2; ++dx)
                for (int dy = -2; dy <= 2; ++dy) {
                    const vec2 p{pos_.x + dx, pos_.y + dy};
                    if ((dx == 0 && dy == 0) || !in_bounds(p) || blocked_.is_set(p)) continue;
                    const int d = field.get(p);
                    if (d + 2 > here || d >= bd) continue;
                    // Need a free middle tile for a legal two-step path.
                    if (cheb(p, pos_) == 2) {
                        bool mid = false;
                        for (auto dd : kDirs) {
                            const vec2 q = pos_ + dd;
                            if (in_bounds(q) && !blocked_.is_set(q) && cheb(q, p) <= 1) { mid = true; break; }
                        }
                        if (!mid) continue;
                    }
                    best = p;
                    bd = d;
                }
            if (best != pos_) {
                try {
                    if (a.use(best)) {
                        blocked_.set(pos_, true);
                        pos_ = best;
                        blocked_.set(pos_, false);
                    }
                } catch (...) {}
            }
            break;
        }
    }

    if (!me.can_move()) return true;
    const int now = field.get(pos_);
    vec2 best = pos_;
    int bd = now;
    for (auto d : kDirs) {
        const vec2 p = pos_ + d;
        if (!in_bounds(p) || blocked_.is_set(p)) continue;
        const int v = field.get(p);
        if (v < bd) { bd = v; best = p; }
    }
    if (best == pos_) return false;
    bool ok = false;
    try { ok = me.move_absolute(best); } catch (...) { ok = false; }
    moved_ = true;  // a refused step must not be retried this turn
    if (ok) {
        blocked_.set(pos_, true);
        pos_ = best;
        blocked_.set(pos_, false);
    }
    return ok;
}

// Walk (and dash) towards the nearest free goal tile, routing around visible units.
bool Bot::step_toward(const cambc::bitboard& goals_in, bool allow_dash) {
    if (moved_ || exclusive_cast_ || !cambc::self().can_move()) return false;
    cambc::bitboard goals = goals_in & ~blocked_;
    if (goals.is_empty() || goals.is_set(pos_)) return false;
    cambc::distance_field field;
    cambc::bitboard dest;
    dest.set(pos_, true);
    cambc::bitboard visited;
    field.compute_to(blocked_, goals, dest, visited);
    return walk_field(field, allow_dash);
}

// Long trips follow a cached field over static terrain; the last tiles route around units.
bool Bot::travel(const cambc::bitboard& goals, int key, bool allow_dash) {
    if (moved_ || exclusive_cast_ || !cambc::self().can_move()) return false;
    if (const auto* f = group_field(key / 32)) {
        if (f->get(pos_) > 8) return walk_field(*f, allow_dash) || step_toward(goals, allow_dash);
        return step_toward(goals, allow_dash);
    }
    if (key != travel_key_) {
        cambc::bitboard roots = goals & ~world().terrain;
        if (roots.is_empty()) return step_toward(goals, allow_dash);
        cambc::bitboard visited;
        travel_->compute(world().terrain, roots, visited);
        travel_key_ = key;
    }
    return travel_on(*travel_, goals, allow_dash);
}

bool Bot::travel_on(const cambc::distance_field& field, const cambc::bitboard& goals, bool allow_dash) {
    const int d = field.get(pos_);
    if (d >= kFar || d <= 6) return step_toward(goals, allow_dash);
    if (walk_field(field, allow_dash)) return true;
    return step_toward(goals, allow_dash);
}

bool Bot::step_away(const cambc::bitboard& danger) {
    cambc::bitboard goals;
    goals.draw_chebyshev(pos_, 5);
    goals &= ~danger;
    return step_toward(goals, false);
}

float Bot::my_dps() const {
    return str_ * 1.2f + focus_ * 0.15f;
}

float Bot::incoming_camp_damage() {
    float inc = 0;
    for (auto p : cambc::vision::awake_camp_tiles().positions()) {
        const int gi = group_of_tile(p);
        if (gi < 0) continue;
        for (const auto& m : world().groups[gi].m)
            if (m.pos == p && cheb(p, pos_) <= m.range) inc += m.str;
    }
    return inc;
}

void Bot::update_squad() {
    leader_ = slot_;
    squad_alive_ = 1;
    if (kSquads >= 5 || slot_ < 0) return;
    const auto allies = cambc::champions(team::ally);
    int leader = -1, alive = 0;
    for (int i = 0; i < static_cast<int>(allies.size()) && i < kClaimBytes; ++i) {
        if (i % kSquads != slot_ % kSquads) continue;
        bool up = false;
        try { up = allies[i].is_alive(); } catch (...) {}
        if (!up) continue;
        ++alive;
        if (leader < 0) leader = i;
    }
    leader_ = leader < 0 ? slot_ : leader;
    squad_alive_ = std::max(1, alive);
}

void Bot::write_claim(int gi) {
    if (slot_ < 0 || slot_ >= kClaimBytes) return;
    std::uint8_t v = gi < 0 || gi > 250 ? 0 : static_cast<std::uint8_t>(gi + 1);
    cambc::shared_memory.write(static_cast<std::size_t>(slot_), std::span<std::uint8_t, 1>{&v, 1});
}

int Bot::read_claims(int gi) {
    std::array<std::uint8_t, kClaimBytes> buf{};
    cambc::shared_memory.read(0, std::span<std::uint8_t, kClaimBytes>{buf});
    int n = 0;
    for (int i = 0; i < kClaimBytes; ++i)
        if (i != slot_ && buf[i] == gi + 1) ++n;
    return n;
}

namespace {
// Damage a group deals while we kill it strongest-first at the given team dps.
// Members are stored strongest-first already (world.cpp), so no sort is needed.
float sim_damage(const Group& G, float dps, bool full) {
    float total = full ? G.str_total : 0;
    if (!full)
        for (const auto& m : G.m)
            if (m.hp > 0) total += m.str;
    float dmg = 0;
    const float inv = 1.0f / dps;
    for (const auto& m : G.m) {
        const float hp = full ? m.hp_max : m.hp;
        if (hp <= 0) continue;
        dmg += total * hp * inv;
        total -= m.str;
    }
    return dmg;
}
}  // namespace

double Bot::score_group(int gi, int dist, int helpers, int squad) {
    const auto& G = world().groups[gi];
    if (dist >= kFar) return -1;
    const double wgold = round_ < 600 ? 0.2 : round_ < 1200 ? 0.1 : 0.03;
    const double value = G.gems + wgold * G.gold;
    if (value <= 0) return -1;
    if (G.state == kReward) {
        const double s = value / (dist + 6.0);
        return helpers > 0 ? s * 0.2 : s;
    }
    const int avail = G.state == kRespawning ? std::max(0, G.ready - round_) : 0;
    const int wait = std::max(0, avail - dist);
    const bool full = G.state == kRespawning;
    const float hp_left = full ? G.hp_max : G.hp_left();
    const float jungle = jungle_ ? 0.7f : 1.0f;
    auto trips_for = [&](int n) {
        const float dmg = sim_damage(G, my_dps() * n, full) * jungle;
        const float first = std::max(0.0f, hp_ - 0.25f * max_hp_) + 0.75f * max_hp_ * (n - 1);
        if (dmg <= first) return 1;
        return 1 + static_cast<int>(std::ceil((dmg - first) / (0.75f * max_hp_ * n)));
    };
    // Join a claimed group only while its claimers cannot clear it in one trip.
    if (helpers > 0 && trips_for(helpers) == 1) return value / (dist + wait + hp_left / my_dps() + 6.0) * 0.15;
    const int solo = trips_for(1);
    const int n = helpers + squad;
    const int trips = n == 1 ? solo : trips_for(n);
    const double clear = hp_left / (my_dps() * n);
    if (squad > 1 && helpers == 0) {
        // A squad shares one reward: rate the group by squad throughput.
        const double t = dist + wait + clear + 6.0 + (trips - 1) * (2.0 * G.d_home + 12.0);
        double s = value / t / squad;
        if (G.d_enemy + 4 < G.d_home) s *= kTheirSide;
        else if (G.d_enemy < G.d_home + 4) s *= kContested;
        if (trips > 3) s *= 0.5;
        return s;
    }
    const double extra = (trips - 1) * (2.0 * G.d_home + 12.0);
    const double t = dist + wait + clear + 6.0 + extra;
    double s = value / n / t;
    // Their side: likely farmed (and guarded) by them.
    if (G.d_enemy + 4 < G.d_home) s *= kTheirSide;
    else if (G.d_enemy < G.d_home + 4) s *= kContested;
    if (trips > 3) s *= 0.5;
    return s;
}

void Bot::choose_target() {
    const auto& groups = world().groups;
    std::array<std::uint8_t, kClaimBytes> buf{};
    cambc::shared_memory.read(0, std::span<std::uint8_t, kClaimBytes>{buf});
    // Squad followers take their leader's target.
    if (kSquads < 5 && leader_ >= 0 && leader_ != slot_ && leader_ < kClaimBytes && buf[leader_] > 0
        && buf[leader_] - 1 < static_cast<int>(groups.size())) {
        const int t = buf[leader_] - 1;
        if (t != target_) {
            target_ = t;
            reach_ = range_;
            stall_ = 0;
            hold_ = 0;
        }
        target_round_ = round_;
        write_claim(target_);
        return;
    }
    int best = -1;
    double bs = 0;
    vec2 seen{};
    const bool danger = sighting(seen);
    jungle_ = cambc::self().item_count()[cambc::item_type::jungle_emblem_of_health] > 0;
    for (int gi = 0; gi < static_cast<int>(groups.size()); ++gi) {
        const auto& G = groups[gi];
        int dist = kFar;
        if (const auto* f = group_field(gi)) dist = f->get(pos_);
        else continue;  // fields are filled nearest-to-home first
        int helpers = 0;
        for (int i = 0; i < kClaimBytes; ++i)
            if (i != slot_ && buf[i] == gi + 1 && (kSquads >= 5 || i % kSquads != slot_ % kSquads)) ++helpers;
        double s = score_group(gi, dist, helpers, kSquads < 5 ? std::max(1, squad_alive_) : 1);
        if (gi == target_) s *= 1.25;
        if (danger && cheb(G.center, seen) <= 7) s *= 0.3;
        // Loose packs: farm near allies so fights are rarely one against several.
        for (int i = 0; i < kClaimBytes && kSquads >= 5; ++i)
            if (i != slot_ && buf[i] > 0 && buf[i] - 1 < static_cast<int>(groups.size())
                && cheb(groups[buf[i] - 1].center, G.center) <= 10) s *= 1.0 + kCohesion;
        debug_block({
            if (slot_ == 0 && round_ >= 100 && round_ <= 130 && s > 0)
                std::printf("  r%d g%d st%d ready%d d%d help%d gems%d hp%.0f s%.4f%s\n", round_, gi, (int)G.state, G.ready - round_, dist, helpers, G.gems, G.hp_left(), s, gi == target_ ? " *" : "");
        });
        if (s > bs) { bs = s; best = gi; }
    }
    if (best != target_) {
        target_ = best;
        reach_ = range_;
        stall_ = 0;
        hold_ = 0;
    }
    target_score_ = bs;
    target_round_ = round_;
    write_claim(target_);
}

std::vector<CampView> Bot::visible_camps(int gi) {
    std::vector<CampView> out;
    if (gi < 0) return out;
    const auto tiles = cambc::vision::camp_tiles();
    for (const auto& m : world().groups[gi].m) {
        if (!tiles.is_set(m.pos)) continue;
        try {
            const auto c = cambc::vision::camp_at(m.pos);
            const float hp = c.health();
            if (hp <= 0) continue;
            out.push_back({m.pos, hp, m.str, m.range, m.type});
        } catch (...) {}
    }
    return out;
}

bool Bot::camp_spells(const std::vector<CampView>& camps, bool exclusive_ok) {
    auto& me = cambc::self();
    if (camps.empty() || !me.can_cast()) return false;
    for (const auto& a : me.abilities()) {
        const auto f = facts(a);
        if (!f.valid) continue;
        if (f.exclusive && (!exclusive_ok || moved_)) continue;
        using K = cambc::ability_kind;
        if (f.kind != K::damage && f.kind != K::cc && f.kind != K::aoe_damage && f.kind != K::shield) continue;
        if (!a.is_ready()) continue;
        try {
            if (f.kind == K::shield) {
                if (incoming_camp_damage() > 0 && hp_ < 0.9f * max_hp_ && a.use()) {
                    if (f.exclusive) exclusive_cast_ = true;
                    return true;
                }
                continue;
            }
            const float dmg = f.modifier * focus_;
            if (f.kind == K::aoe_damage) {
                vec2 best{-1, -1};
                float bv = 0;
                for (const auto& c : camps) {
                    if (cheb(c.pos, pos_) > f.range) continue;
                    float v = 0;
                    for (const auto& o : camps)
                        if (cheb(o.pos, c.pos) <= f.radius) v += std::min(dmg, o.hp) * (1.0f + o.str / 50.0f);
                    if (v > bv) { bv = v; best = c.pos; }
                }
                if (best.x >= 0 && a.use(best)) {
                    if (f.exclusive) exclusive_cast_ = true;
                    return true;
                }
                continue;
            }
            // Single target damage or stun: strongest attacker first, a kill beats everything.
            const CampView* best = nullptr;
            float bv = -1;
            for (const auto& c : camps) {
                if (cheb(c.pos, pos_) > f.range) continue;
                if (dmg <= 0 && c.str <= 0) continue;
                float v = std::min(dmg, c.hp) + c.str * 3.0f + (dmg >= c.hp ? 200.0f : 0.0f);
                if (v > bv) { bv = v; best = &c; }
            }
            if (best && a.use(cambc::vision::camp_at(best->pos))) {
                if (f.exclusive) exclusive_cast_ = true;
                return true;
            }
        } catch (...) {}
    }
    return false;
}

bool Bot::attack_camp(const std::vector<CampView>& camps) {
    auto& me = cambc::self();
    if (!me.can_attack()) return false;
    const bool splash = type_ == cambc::champion_type::varg;
    auto pick = [&]() -> const CampView* {
        const CampView* best = nullptr;
        float bv = -1;
        for (const auto& c : camps) {
            if (c.hp <= 0 || cheb(c.pos, pos_) > range_) continue;
            float v = camp_priority(c);
            if (c.hp <= str_) v += 500;
            if (splash)
                for (const auto& o : camps)
                    if (&o != &c && o.hp > 0 && cheb(o.pos, c.pos) <= 1) v += camp_priority(o) * 0.8f + 0.5f;
            if (v > bv) { bv = v; best = &c; }
        }
        return best;
    };
    const CampView* t = pick();
    if (!t) return false;
    try {
        me.attack(cambc::vision::camp_at(t->pos));
        attacked_ = true;
    } catch (...) { return false; }
    // Attack reset right after our own hit, while something remains in range.
    if (!me.can_cast()) return true;
    for (const auto& a : me.abilities()) {
        if (!a.is_attack_reset() || !a.is_ready()) continue;
        std::vector<CampView> left;
        const auto tiles = cambc::vision::camp_tiles();
        for (const auto& c : camps)
            if (tiles.is_set(c.pos) && cheb(c.pos, pos_) <= range_) left.push_back(c);
        if (left.empty()) break;
        try {
            if (a.use() && me.can_attack()) {
                for (auto& c : left) {
                    try { c.hp = cambc::vision::camp_at(c.pos).health(); } catch (...) { c.hp = 0; }
                }
                const CampView* best = nullptr;
                float bv = -1;
                for (const auto& c : left) {
                    if (c.hp <= 0) continue;
                    const float v = camp_priority(c);
                    if (v > bv) { bv = v; best = &c; }
                }
                if (best) me.attack(cambc::vision::camp_at(best->pos));
            }
        } catch (...) {}
        break;
    }
    return true;
}

void Bot::fight_group(int gi) {
    const auto& G = world().groups[gi];
    auto camps = visible_camps(gi);
    // Big groups wait for the squad: one champion alone only feeds the camp.
    if (kSquads < 5 && squad_alive_ > 1 && hold_ < kHoldMax
        && sim_damage(G, my_dps(), false) > 0.75f * max_hp_) {
        int present = 1;
        for (const auto& f : friends_)
            if (cheb(f.pos, G.center) <= G.span + 8) ++present;
        bool awake = false;
        for (const auto& c : camps)
            if (cambc::vision::awake_camp_tiles().is_set(c.pos)) { awake = true; break; }
        if (present < squad_alive_ && !awake) {
            int reach = 0;
            for (const auto& m : G.m) reach = std::max(reach, m.range);
            ++hold_;
            act_ = 11;
            int dmin = 99;
            for (const auto& m : G.m)
                if (m.hp > 0) dmin = std::min(dmin, cheb(m.pos, pos_));
            if (dmin <= reach + 1) {
                cambc::bitboard danger;
                for (const auto& m : G.m) danger.draw_chebyshev(m.pos, static_cast<float>(reach + 1));
                step_away(danger);
            } else if (dmin > reach + 4) {
                cambc::bitboard goals;
                goals.draw_chebyshev(G.center, static_cast<float>(G.span + reach + 3));
                travel(goals, gi * 32 + 28, true);
            }
            return;
        }
    }
    bool in_range = false;
    for (const auto& c : camps)
        if (cheb(c.pos, pos_) <= range_) in_range = true;

    camp_spells(camps, in_range);
    if (in_range) {
        act_ = 12;
        attack_camp(camps);
        return;
    }
    act_ = 13;
    // Approach the nearest living member.
    vec2 app{-1, -1};
    int bd = 1 << 20;
    for (const auto& m : G.m) {
        if (m.hp <= 0) continue;
        const int d = cheb(m.pos, pos_);
        if (d < bd) { bd = d; app = m.pos; }
    }
    if (app.x < 0) app = G.m.front().pos;
    bool visible = false;
    for (const auto& c : camps)
        if (c.pos == app) visible = true;
    // In reach but out of sight (walls, bushes): close in.
    if (cheb(app, pos_) <= reach_ && !visible && reach_ > 1) --reach_;
    cambc::bitboard goals;
    goals.draw_chebyshev(app, static_cast<float>(reach_));
    if (cambc::map::bushes().is_set(app)) {
        cambc::bitboard bush = goals & cambc::map::bushes();
        if (!(bush & ~blocked_).is_empty()) goals = bush;
    }
    if (!travel(goals, target_ * 32 + reach_, true)) {
        cambc::bitboard wide;
        wide.draw_chebyshev(app, static_cast<float>(reach_ + 2));
        step_toward(wide, true);
    }
    if (cambc::self().can_attack()) attack_camp(camps);
}

void Bot::capture_group(int gi) {
    const auto& G = world().groups[gi];
    for (const auto& m : G.m)
        if (m.pos == pos_) return;  // hold the tile until the capture completes
    cambc::bitboard goals;
    for (const auto& m : G.m) goals.set(m.pos, true);
    travel(goals, gi * 32 + 16, true);
}

bool Bot::should_bank() {
    if (gems_ <= 0) return false;
    const int d = altar_distance();
    if (d >= kFar) return false;
    const int ours = static_cast<int>(cambc::global_submitted_gems(team::ally));
    if (ours + gems_ >= static_cast<int>(cambc::constants::winning_gems)) return true;
    if (round_ + d + gems_ / 10 + 12 >= static_cast<int>(cambc::constants::max_rounds)) return true;
    if (d <= 3) return true;
    // Worth the detour once the bag outweighs the walk; foes about make bags riskier.
    const float need = last_enemy_round_ >= round_ - kWaryRounds ? kBankPerTile * 0.6f : kBankPerTile;
    return gems_ >= std::min(kBankCap, std::max(kBankMin, static_cast<int>(need * d)));
}

// Tribute at the nearest altar; an enemy lock on it is broken with basic attacks first.
bool Bot::bank_here() {
    auto& me = cambc::self();
    for (auto a : world().altars) {
        const int d = cheb(a, pos_);
        if (d > std::max(range_, cambc::constants::submit_gems_range)) continue;
        bool enemy_lock = false;
        try {
            if (cambc::vision::has_altar_lock(a)) enemy_lock = cambc::vision::altar_lock_at(a).is_enemy();
        } catch (...) {}
        if (enemy_lock) {
            if (!me.can_attack()) continue;
            try {
                me.attack(cambc::vision::altar_lock_at(a));
                attacked_ = true;
                if (me.can_cast())
                    for (const auto& ab : me.abilities())
                        if (ab.is_attack_reset() && ab.is_ready()) {
                            if (cambc::vision::has_altar_lock(a) && ab.use() && me.can_attack())
                                me.attack(cambc::vision::altar_lock_at(a));
                            break;
                        }
                return true;
            } catch (...) {}
            continue;
        }
        if (d > cambc::constants::submit_gems_range || exclusive_cast_) continue;
        try {
            me.submit_gems(a);
            return true;
        } catch (...) {}
    }
    return false;
}

bool Bot::bank() {
    if (bank_here()) return true;
    if (!travel_on(world().altar, world().altar_adj, true)) return false;
    bank_here();
    return true;
}

void Bot::broadcast_sighting() {
    const Foe* near = nullptr;
    for (const auto& f : foes_)
        if (!near || f.dist < near->dist) near = &f;
    if (!near) return;
    std::array<std::uint8_t, 3> v{static_cast<std::uint8_t>(near->pos.x), static_cast<std::uint8_t>(near->pos.y),
                                  static_cast<std::uint8_t>(round_ & 255)};
    cambc::shared_memory.write(kSightByte, std::span<std::uint8_t, 3>{v});
}

bool Bot::sighting(vec2& where) {
    std::array<std::uint8_t, 3> v{};
    cambc::shared_memory.read(kSightByte, std::span<std::uint8_t, 3>{v});
    const int age = (round_ - v[2]) & 255;
    if (age > kSightRounds || (v[0] == 0 && v[1] == 0 && v[2] == 0)) return false;
    where = {v[0], v[1]};
    return true;
}

// Attack the foe we can finish soonest; channelling and gem-carrying foes first.
bool Bot::hit_foe() {
    auto& me = cambc::self();
    if (!me.can_attack()) return false;
    const Foe* best = nullptr;
    float bv = -1;
    for (const auto& f : foes_) {
        if (cheb(f.pos, pos_) > range_) continue;
        const float v = 10000.0f / std::max(1.0f, f.hp) + (f.channel ? 30.0f : 0.0f) + f.gems * 0.5f;
        if (v > bv) { bv = v; best = &f; }
    }
    if (!best) return false;
    try {
        me.attack(cambc::vision::champion_at(best->pos));
        attacked_ = true;
        if (me.can_cast())
            for (const auto& a : me.abilities())
                if (a.is_attack_reset() && a.is_ready()) {
                    if (cambc::vision::has_champion(best->pos) && a.use() && me.can_attack())
                        me.attack(cambc::vision::champion_at(best->pos));
                    break;
                }
        return true;
    } catch (...) { return false; }
}

void Bot::foe_spells(bool retreating) {
    auto& me = cambc::self();
    if (!me.can_cast()) return;
    int dmin = 99;
    for (const auto& f : foes_) dmin = std::min(dmin, cheb(f.pos, pos_));
    for (const auto& a : me.abilities()) {
        const auto f = facts(a);
        if (!f.valid || (f.exclusive && (moved_ || retreating))) continue;
        using K = cambc::ability_kind;
        if (f.kind != K::damage && f.kind != K::cc && f.kind != K::shield && f.kind != K::aoe_damage) continue;
        if (!a.is_ready()) continue;
        try {
            if (f.kind == K::shield) {
                if (dmin <= 4 && a.use()) { if (f.exclusive) exclusive_cast_ = true; return; }
                continue;
            }
            const Foe* best = nullptr;
            float bv = -1;
            for (const auto& e : foes_) {
                if (cheb(e.pos, pos_) > f.range) continue;
                const float v = 10000.0f / std::max(1.0f, e.hp) + e.str + (e.channel ? 50.0f : 0.0f);
                if (v > bv) { bv = v; best = &e; }
            }
            if (!best) continue;
            const bool ok = f.kind == K::aoe_damage ? a.use(best->pos) : a.use(cambc::vision::champion_at(best->pos));
            if (ok) {
                if (f.exclusive) exclusive_cast_ = true;
                return;
            }
        } catch (...) {}
    }
}

// Step to the tile that keeps us furthest outside every foe's reach, towards allies and home.
void Bot::retreat() {
    auto& me = cambc::self();
    auto margin_at = [&](vec2 p) {
        float m = 99;
        for (const auto& f : foes_) {
            const int reach = f.range + (f.range > 1 ? 0 : 1);
            m = std::min(m, static_cast<float>(cheb(p, f.pos) - reach));
        }
        return m;
    };
    auto tile_value = [&](vec2 p) {
        const int hd = world().home.get(p);
        float v = std::min(margin_at(p), 3.0f) * 10.0f - (hd >= kFar ? 500.0f : hd * 1.0f);
        int ad = 99;
        for (const auto& f : friends_) ad = std::min(ad, cheb(p, f.pos));
        if (ad <= 12) v -= 2.0f * ad;
        return v;
    };
    // A dash first when a step alone cannot leave every reach.
    if (!moved_ && !exclusive_cast_ && me.can_cast()) {
        float best_step = -1e9f;
        for (int i = -1; i < 8; ++i) {
            const vec2 p = i < 0 ? pos_ : pos_ + kDirs[i];
            if (!in_bounds(p) || (i >= 0 && phys_.is_set(p))) continue;
            best_step = std::max(best_step, margin_at(p));
        }
        if (best_step < 1)
            for (const auto& a : me.abilities()) {
                if (!a.is_dash() || !a.is_ready()) continue;
                vec2 best = pos_;
                float bv = tile_value(pos_) + 5.0f;
                for (int dx = -2; dx <= 2; ++dx)
                    for (int dy = -2; dy <= 2; ++dy) {
                        const vec2 p{pos_.x + dx, pos_.y + dy};
                        if (cheb(p, pos_) != 2 || !in_bounds(p) || phys_.is_set(p)) continue;
                        bool mid = false;
                        for (auto dd : kDirs) {
                            const vec2 q = pos_ + dd;
                            if (in_bounds(q) && !phys_.is_set(q) && cheb(q, p) <= 1) { mid = true; break; }
                        }
                        if (!mid) continue;
                        const float v = tile_value(p);
                        if (v > bv) { bv = v; best = p; }
                    }
                if (best != pos_) {
                    try {
                        if (a.use(best)) {
                            blocked_.set(pos_, true);
                            pos_ = best;
                            blocked_.set(pos_, false);
                        }
                    } catch (...) {}
                }
                break;
            }
    }
    if (me.can_move() && !moved_ && !exclusive_cast_) {
        vec2 best = pos_;
        float bv = tile_value(pos_) - 1.0f;
        for (auto d : kDirs) {
            const vec2 p = pos_ + d;
            if (!in_bounds(p) || phys_.is_set(p)) continue;
            const float v = tile_value(p);
            if (v > bv) { bv = v; best = p; }
        }
        if (best != pos_) {
            try {
                if (me.move_absolute(best)) {
                    blocked_.set(pos_, true);
                    pos_ = best;
                    blocked_.set(pos_, false);
                }
            } catch (...) {}
            moved_ = true;
        }
    }
    // Melee can still hit whoever stayed beside us.
    if (!attacked_) hit_foe();
}

bool Bot::handle_enemies() {
    if (foes_.empty()) return false;
    auto& me = cambc::self();
    last_enemy_round_ = round_;
    broadcast_sighting();

    // Local fight model: us within 7 of me, them within 10.
    std::vector<Body> us, them;
    auto arrive = [](int d, int range) { return d <= range ? 0 : d - range + (range > 1 ? 1 : 0); };
    int dmin = 99;
    for (const auto& f : foes_) dmin = std::min(dmin, f.dist);
    {
        Body b;
        b.pos = pos_;
        b.hp = hp_ + champ_shield(type_, focus_) * 0.5f;
        b.dps = champ_dps(type_, str_, focus_);
        b.range = range_;
        b.ranged = ranged_;
        b.arrive = arrive(dmin, range_);
        b.stake = gems_ * 10.0f;
        us.push_back(b);
    }
    for (const auto& a : friends_) {
        if (cheb(a.pos, pos_) > 7) continue;
        int d = 99;
        for (const auto& f : foes_) d = std::min(d, cheb(a.pos, f.pos));
        Body b;
        b.pos = a.pos;
        b.hp = a.hp + champ_shield(a.type, a.focus) * 0.5f;
        b.dps = champ_dps(a.type, a.str, a.focus);
        b.range = a.range;
        b.ranged = a.range > 1;
        b.arrive = arrive(d, a.range) + 1;
        us.push_back(b);
    }
    for (const auto& f : foes_) {
        if (f.dist > 10) continue;
        int d = f.dist;
        for (const auto& a : friends_) d = std::min(d, cheb(a.pos, f.pos));
        Body b;
        b.pos = f.pos;
        b.hp = f.hp + champ_shield(f.type, f.focus) * 0.5f;
        b.dps = champ_dps(f.type, f.str, f.focus);
        b.range = f.range;
        b.ranged = f.range > 1;
        b.arrive = arrive(d, f.range);
        b.stake = f.gems * 10.0f;
        them.push_back(b);
    }
    if (them.empty()) return false;
    const float value = fight_value(us, them, 8);
    fight_value_ = value;

    if (value > kEngage) {
        foe_spells(false);
        if (hit_foe()) return true;
        if (dmin <= 5) {
            const Foe* near = nullptr;
            for (const auto& f : foes_)
                if (!near || f.dist < near->dist) near = &f;
            cambc::bitboard goals;
            goals.draw_chebyshev(near->pos, static_cast<float>(range_));
            step_toward(goals, false);
            hit_foe();
            return true;
        }
        return false;
    }

    // Keep out of reach of foes we would not fight, whatever we do next (retreat uses phys_).
    phys_ = blocked_;
    for (const auto& f : foes_) {
        cambc::bitboard reach;
        reach.draw_chebyshev(f.pos, static_cast<float>(f.range + (f.range > 1 ? 1 : 2)));
        blocked_ |= reach;
    }
    blocked_.set(pos_, false);
    bool threat = false;
    for (const auto& f : foes_)
        if (f.dist <= f.range + (f.range > 1 ? kThreatPad : kThreatPad + 1)) threat = true;
    if (!threat) return false;

    // Cornered by a melee chaser we cannot outrun: trade blows instead of taking free hits.
    if (kCornered && world().home.get(pos_) > 4) {
        bool adjacent_melee = false;
        for (const auto& f : foes_)
            if (f.range <= 1 && f.dist <= 1) adjacent_melee = true;
        if (adjacent_melee) {
            bool escape = false;
            for (auto d : kDirs) {
                const vec2 p = pos_ + d;
                if (!in_bounds(p) || phys_.is_set(p)) continue;
                bool safe = true;
                for (const auto& f : foes_)
                    if (cheb(p, f.pos) <= f.range + (f.range > 1 ? 0 : 1)) safe = false;
                if (safe) { escape = true; break; }
            }
            if (!escape) {
                foe_spells(false);
                hit_foe();
                return true;
            }
        }
    }

    // Outmatched: bank if an altar is right here, otherwise run home.
    if (gems_ > 0 && dmin >= 3) {
        for (auto a : world().altars)
            if (cheb(a, pos_) <= cambc::constants::submit_gems_range) {
                try {
                    if (!(cambc::vision::has_altar_lock(a) && cambc::vision::altar_lock_at(a).is_enemy())) {
                        me.submit_gems(a);
                        return true;
                    }
                } catch (...) {}
            }
    }
    foe_spells(true);
    retreat();
    return true;
}

// Healthy champions near a fresh sighting join the fight there.
bool Bot::answer_sighting() {
    if (!kHelp || hp_ < 0.6f * max_hp_ || gems_ >= 30) return false;
    vec2 where;
    if (!sighting(where)) return false;
    std::array<std::uint8_t, 3> v{};
    cambc::shared_memory.read(kSightByte, std::span<std::uint8_t, 3>{v});
    if (((round_ - v[2]) & 255) > 2) return false;
    const int d = cheb(where, pos_);
    if (d > kHelpRange || d <= 3) return false;
    cambc::bitboard goals;
    goals.draw_chebyshev(where, 3);
    return step_toward(goals, true);
}

bool Bot::handle_heal() {
    auto& me = cambc::self();
    const bool at_spawn = world().spawn_ally.is_set(pos_);
    if (at_spawn) {
        if (hp_ < 0.95f * max_hp_) { healing_ = true; return true; }
        healing_ = false;
        return false;
    }
    const float inc = incoming_camp_damage();
    const float floor = last_enemy_round_ >= round_ - kWaryRounds ? kWaryHeal : 0.3f;
    const bool low = hp_ < floor * max_hp_ || (inc > 0 && hp_ < inc * 2.5f + 40.0f);
    if (!low && !healing_) return false;
    healing_ = true;
    if (inc > 0) {
        cambc::bitboard danger;
        for (auto p : cambc::vision::awake_camp_tiles().positions()) {
            const int gi = group_of_tile(p);
            if (gi < 0) continue;
            for (const auto& m : world().groups[gi].m)
                if (m.pos == p) danger.draw_chebyshev(p, static_cast<float>(m.range));
        }
        if (step_away(danger)) return true;
    }
    if (gems_ > 0 && altar_distance() <= 2 && bank()) return true;
    const int hd = world().home.get(pos_);
    // Hit by something we cannot see (a foe in a bush): a recall would only be interrupted.
    if (hd <= 6 || last_enemy_round_ >= round_ - 2 || hidden_hit_round_ >= round_ - 2) {
        travel_on(world().home, world().spawn_ally, true);
        return true;
    }
    if (inc <= 0) {
        try { me.recall(); } catch (...) {}
    }
    return true;
}

void Bot::farm() {
    if (target_ < 0 || round_ - target_round_ >= 3) choose_target();
    if (target_ < 0) return;
    auto G = world().groups[target_];
    const auto& T = world().groups[target_];
    // Not enough health for what is left of this group: heal first.
    if (T.state != kReward && hp_ < 0.6f * max_hp_) {
        const float need = sim_damage(T, my_dps() * (1 + read_claims(target_)), T.state == kRespawning);
        if (need > hp_ - 0.2f * max_hp_) {
            healing_ = true;
            handle_heal();
            return;
        }
    }
    if (T.state == kReward) {
        act_ = 10;
        capture_group(target_);
        return;
    }
    if (T.state == kRespawning) {
        // Spend the wait at an altar when the round trip fits.
        if (gems_ > 0 && T.ready - round_ > 2 * altar_distance() + 2 && bank()) { act_ = 3; return; }
        act_ = 11;
        bool on_tile = false;
        for (const auto& m : T.m)
            if (m.pos == pos_) on_tile = true;
        if (on_tile || cheb(T.center, pos_) > 3) {
            cambc::bitboard goals;
            goals.draw_chebyshev(T.center, 3);
            for (const auto& m : T.m) goals.set(m.pos, false);
            travel(goals, target_ * 32 + 24, true);
        }
        return;
    }
    fight_group(target_);
}

static int g_act[5][16];
static std::string g_hist[8];
static int g_hist_n = 0;
static bool g_was_alive = true;

static void dump_area(vec2 c, int r, vec2 me) {
    debug_block({
        const auto& w = world();
        for (int y = c.y + r; y >= c.y - r; --y) {
            std::string line;
            for (int x = c.x - r; x <= c.x + r; ++x) {
                const vec2 p{x, y};
                char ch = '.';
                if (!in_bounds(p)) ch = ' ';
                else if (p == me) ch = '@';
                else if (cambc::map::walls().is_set(p)) ch = '#';
                else if (cambc::map::holes().is_set(p)) ch = 'o';
                else if (cambc::map::altars().is_set(p)) ch = 'A';
                else if (group_of_tile(p) >= 0) ch = 'C';
                else if (cambc::map::bushes().is_set(p)) ch = '"';
                else if (w.spawn_ally.is_set(p)) ch = 's';
                line += ch;
            }
            std::printf("%3d %s\n", y, line.c_str());
        }
    });
}

void Bot::play_turn() {

    const auto fstart = cambc::fuel_remaining();

    round_ = static_cast<int>(cambc::round_number());
    if (!inited_) {
        // The first turn only builds the map model (~140k fuel on 100x100 maps).
        init();
        return;
    }
    auto& me = cambc::self();
    moved_ = exclusive_cast_ = attacked_ = false;

    if (!me.is_alive()) {
        debug_block({
            if (g_was_alive && kDeathTrace) {
                std::printf("DEATH r%d %s\n", round_, cambc::to_string(type_).c_str());
                for (int i = std::max(0, g_hist_n - 6); i < g_hist_n; ++i) std::printf("   %s\n", g_hist[i % 8].c_str());
            }
            g_was_alive = false;
        });
        shop();
        return;
    }
    debug_block({ g_was_alive = true; });
    pos_ = me.position();
    update_squad();
    update_groups(round_, pos_);
    if (me.is_submitting_gems() || me.is_teleporting()) return;
    hp_ = me.health();
    max_hp_ = std::max(1.0f, me.max_health());
    str_ = me.strength();
    focus_ = me.focus();
    gems_ = static_cast<int>(me.gem_count());
    scan_units();
    if (hp_ < last_hp_ - 1.0f && foes_.empty() && incoming_camp_damage() <= 0) hidden_hit_round_ = round_;
    last_hp_ = hp_;
    if (me.is_recalling()) {
        // Keep the channel unless a foe is about to hit us.
        return;
    }
    const bool at_spawn = world().spawn_ally.is_set(pos_);
    if (at_spawn || type_ == cambc::champion_type::tavi) shop();
    compute_blocked();

    const auto f0 = cambc::fuel_remaining();
    int mode = 0;
    fight_value_ = 0;
    act_ = 0;
    if (handle_enemies()) mode = 1;
    else if (handle_heal()) mode = 2;
    else if (answer_sighting()) mode = 4;
    else if (should_bank() && bank()) mode = 3;
    else farm();
    debug_block({
        const int a = mode == 2 && world().spawn_ally.is_set(pos_) ? 14 : act_ ? act_ : mode;
        g_act[slot_ < 0 ? 0 : slot_ % 5][a % 16]++;
        if (round_ == kActReport) {
            std::printf("ACT %s:", cambc::to_string(type_).c_str());
            const char* names[16] = {"farm?", "foe", "heal", "bank", "help", "", "", "", "", "", "capture", "wait", "hit", "travel", "spawn", ""};
            for (int i = 0; i < 16; ++i)
                if (g_act[slot_ % 5][i]) std::printf(" %s=%d", names[i], g_act[slot_ % 5][i]);
            std::printf("\n");
        }
    });
    fill_group_fields(60000);
    debug_block({
        if (round_ == 3 && slot_ == 0 && kDumpMap) {
            for (const auto& G : world().groups) {
                std::printf("group c(%d,%d) gems%d gold%d hp%.0f str%.0f resp%d dh%d de%d n%d\n", G.center.x, G.center.y, G.gems, G.gold, G.hp_max, G.str_total, G.respawn, G.d_home, G.d_enemy, (int)G.m.size());
            }
            dump_area({world().w / 2, world().h / 2}, std::max(world().w, world().h) / 2, pos_);
            for (auto a : world().altars) std::printf("altar (%d,%d) home %d\n", a.x, a.y, world().home.get(a + vec2{1, 0}));
        }
        if (kDeathTrace) {
            char buf[512];
            int n = std::snprintf(buf, sizeof buf, "r%d (%d,%d) hp%.0f gems%d mode%d fv%.0f |", round_, pos_.x, pos_.y, hp_, gems_, mode, fight_value_);
            for (const auto& f : foes_)
                if (n < 400) n += std::snprintf(buf + n, sizeof buf - n, " %s(%d,%d)hp%.0f d%d", cambc::to_string(f.type).substr(0, 4).c_str(), f.pos.x, f.pos.y, f.hp, f.dist);
            if (n < 480) std::snprintf(buf + n, sizeof buf - n, " fr%d", (int)friends_.size());
            g_hist[g_hist_n++ % 8] = buf;
        }
        if (round_ >= kTraceFrom && round_ <= kTraceTo) {
            std::printf("T r%d %s (%d,%d) hp%.0f gems%d mode%d tgt%d fv%.0f |", round_, cambc::to_string(type_).c_str(), pos_.x, pos_.y, hp_, gems_, mode, target_, fight_value_);
            for (const auto& f : foes_) std::printf(" %s(%d,%d) hp%.0f d%d", cambc::to_string(f.type).substr(0, 4).c_str(), f.pos.x, f.pos.y, f.hp, f.dist);
            std::printf(" friends%d altar%d\n", (int)friends_.size(), altar_distance());
        }
        if (fstart - cambc::fuel_remaining() > 85000 && round_ > 2) {
            std::printf("r%d %s pos(%d,%d) hp%.0f/%.0f gems%d gold%u mode%d tgt%d st%d pre%llu used%llu\n", round_,
                cambc::to_string(type_).c_str(), pos_.x, pos_.y, hp_, max_hp_, gems_, me.gold_count(), mode, target_,
                target_ >= 0 ? (int)world().groups[target_].state : -1,
                (unsigned long long)(fstart - f0), (unsigned long long)(f0 - cambc::fuel_remaining()));
        }
    });
}
