// Camp farming: what a champion knows about each camp group, which group to farm and with how many
// champions, where to stand while clearing it, and capturing the reward.
//
// Model (GameRules.md as changed by patch 100): camps never regenerate, awake monsters hit the nearest
// champion in range every round, a capture pays the group's reward split among the allies present, and
// the group respawns together afterwards. A champion values a group as its share of the reward per round
// it spends on it: travel or respawn wait, clear time at the team's damage, heal trips for the health the
// clear costs it, and the capture. Hard groups get cheaper with more champions (damage taken shrinks with
// the square of the team size), so a champion can ask for helpers through shared memory (a rally).
#include <cambc.h>
#include "bot.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <variant>
#include <vector>

using cambc::vec2;
using cambc::team;

namespace {
constexpr int far_away = 9999;
// Shared memory: byte s holds slot s's target + 1; byte 6 the rally group + 1; byte 7 the slots ready there.
constexpr int rally_group_byte = 6, rally_ready_byte = 7;
// Byte 5: the last group whose capture someone saw complete + 1 (teams of up to five bodies).
constexpr int capture_byte = 5;
constexpr int max_slots = 6;
constexpr bool camp_trace = false;
constexpr bool camp_tally = false;
// Rounds per farm branch, for tuning: heal, capture, respawn wait, gather, walk to stand, at stand, none.
std::array<int, 12> tally{};
long fuel_mark[6] = {};
bool camp_profile = false;
int tally_round = 0;
const char* const tally_names[] = {"other", "heal", "capt", "capwalk", "respwait", "lowhp", "gather", "stand", "walk", "none", "acting", "spawn"};

int cheb(vec2 a, vec2 b) { return static_cast<int>(a.chebyshev_distance(b)); }
bool in_bounds(vec2 p) { return cambc::map::is_in_bounds(p); }

std::uint8_t read_shared(int offset) {
    if (offset < 0 || offset >= static_cast<int>(cambc::shared_memory.size())) return 0;
    return cambc::shared_memory[static_cast<std::size_t>(offset)];
}
void write_shared(int offset, std::uint8_t value) {
    if (offset < 0 || offset >= static_cast<int>(cambc::shared_memory.size())) return;
    cambc::shared_memory[static_cast<std::size_t>(offset)] = value;
}

Member member_of(const cambc::camp& camp) {
    Member m;
    m.pos = camp.spawn_position();
    switch (camp.kind()) {
    case cambc::camp_type::wolf: m.max_hp = 1350; m.str = 20; m.range = 3; break;
    case cambc::camp_type::elk: m.max_hp = 2400; m.str = 60; m.range = 4; break;
    case cambc::camp_type::gem_cluster: m.max_hp = 120; break;
    case cambc::camp_type::gold_cluster: m.max_hp = 120; break;
    case cambc::camp_type::monolith: m.max_hp = 20000; m.str = 200; m.range = 5; break;
    case cambc::camp_type::golem: m.max_hp = 5000; m.str = 120; m.range = 4; break;
    default: m.max_hp = 1000; m.str = 50; m.range = 3; break;
    }
    m.monster = m.str > 0;
    return m;
}
} // namespace

void Bot::prepare_group(Group& g) {
    if (!g.members.empty()) return;
    for (const auto& camp : g.handle.camps()) g.members.push_back(member_of(camp));
    g.member_hp.resize(g.members.size());
    for (std::size_t i = 0; i < g.members.size(); ++i) {
        g.member_hp[i] = g.members[i].max_hp;
        if (g.members[i].str >= 120) g.deadly = true;
    }
}

float Bot::group_hp(const Group& g) const {
    float hp = 0;
    for (float h : g.member_hp) hp += h;
    return hp;
}

// Damage the team takes clearing the living members at `dps`: monsters die in the order that removes
// the most strength per health first; clusters never hit back and die last. The order is cached per
// group until a member's health changes (scoring runs over every group, and fuel is instructions).
float Bot::clear_cost(Group& g, float dps, float* clear_time) const {
    if (g.order_dirty) {
        g.order.clear();
        g.cluster_hp = 0;
        g.hits = 0;
        for (std::size_t i = 0; i < g.members.size(); ++i) {
            const float hp = g.member_hp[i];
            if (hp <= 0) continue;
            if (g.members[i].monster) {
                g.order.emplace_back(hp, g.members[i].str);
                g.hits += g.members[i].str;
            } else {
                g.cluster_hp += hp;
            }
        }
        std::sort(g.order.begin(), g.order.end(),
            [](const auto& a, const auto& b) { return a.second * b.first > b.second * a.first; });
        g.order_dirty = false;
    }
    float elapsed = 0, damage = 0;
    for (const auto& [hp, str] : g.order) {
        elapsed += hp / dps;
        damage += str * elapsed;
    }
    *clear_time = elapsed + g.cluster_hp / dps;
    return damage;
}

// Walking distance to a group: its cached field, or a Chebyshev guess until that field exists.
int Bot::group_distance(int index, vec2 pos) const {
    const auto& g = groups[static_cast<std::size_t>(index)];
    if (g.field) return g.field->at(pos);
    return cheb(g.facts.center, pos) * 5 / 4 + 2;
}

void Bot::update_groups() {
    fuel_mark[0] = static_cast<long>(cambc::fuel_remaining());
    const auto pos = cambc::self().position();
    const int round = static_cast<int>(cambc::round_number());
    if (camp_tally) {
        ++tally[10];
        if (cambc::map::spawn_region(team::ally).is_set(pos)) ++tally[11];
    }
    const Bits visible(cambc::vision::visible_tiles());
    const Bits living(cambc::vision::awake_camp_tiles() | cambc::vision::asleep_camp_tiles());
    // A capture an ally saw complete: the group is respawning, wherever we are.
    if (bodies <= capture_byte) {
        const int news = read_shared(capture_byte);
        if (news != capture_seen) {
            capture_seen = news;
            const int gi = news - 1;
            if (gi >= 0 && gi < static_cast<int>(groups.size())) {
                auto& g = groups[static_cast<std::size_t>(gi)];
                prepare_group(g);
                g.reward = false;
                g.respawn_at = std::max(g.respawn_at, round + static_cast<int>(g.handle.max_respawn_timer()) - 1);
                for (std::size_t i = 0; i < g.members.size(); ++i) g.member_hp[i] = g.members[i].max_hp;
                g.order_dirty = true;
            }
        }
    }
    for (std::size_t gi = 0; gi < groups.size(); ++gi) {
        auto& g = groups[gi];
        prepare_group(g);
        const int d = cheb(g.facts.center, pos);
        for (const auto& e : enemies)
            if (cheb(e.pos, g.facts.center) <= 7) g.enemy_near_until = round + 30;
        // State reads are host calls: our target every turn, other groups in sight every third turn.
        const bool mine = static_cast<int>(gi) == target;
        if (d > 11 || (!mine && (round + static_cast<int>(gi)) % 3 != 0)) continue;
        bool seen = false;
        for (auto t : g.facts.tiles)
            if (visible.get(t)) { seen = true; break; }
        if (!seen) continue;
        try {
            const auto state = g.handle.state();
            if (std::holds_alternative<cambc::map::camp_group_alive>(state)) {
                g.respawn_at = 0;
                g.reward = false;
                // Health of the members we can see; others keep their last known value.
                for (std::size_t i = 0; i < g.members.size(); ++i) {
                    const vec2 t = g.members[i].pos;
                    if (!visible.get(t)) continue;
                    float hp = g.member_hp[i];
                    if (!living.get(t)) hp = 0;
                    else if (!g.members[i].monster) hp = g.members[i].max_hp;
                    else if (mine) {
                        try {
                            hp = cambc::vision::camp_at(t).health();
                        } catch (const std::exception&) {
                        }
                    }
                    if (hp != g.member_hp[i]) { g.member_hp[i] = hp; g.order_dirty = true; }
                }
            } else if (std::holds_alternative<cambc::map::camp_group_reward_available>(state)) {
                g.reward = true;
                g.respawn_at = 0;
                std::fill(g.member_hp.begin(), g.member_hp.end(), 0.0f);
                g.order_dirty = true;
                for (const auto& s : allies)
                    if (g.tiles.is_set(s.pos)) g.ally_capturing_until = round + 8;
            } else if (const auto* r = std::get_if<cambc::map::camp_group_respawning>(&state)) {
                if (g.reward && bodies <= capture_byte) {
                    write_shared(capture_byte, static_cast<std::uint8_t>(gi + 1));
                    capture_seen = static_cast<int>(gi) + 1;
                }
                g.reward = false;
                g.respawn_at = round + std::max(1, static_cast<int>(r->timer));
                for (std::size_t i = 0; i < g.members.size(); ++i) g.member_hp[i] = g.members[i].max_hp;
                g.order_dirty = true;
            }
        } catch (const std::exception&) {
        }
    }
    fuel_mark[1] = static_cast<long>(cambc::fuel_remaining());
}

// The rally group is live while some champion still targets it.
int Bot::rally_group() const {
    const int rally = static_cast<int>(read_shared(rally_group_byte)) - 1;
    if (rally < 0 || rally >= static_cast<int>(groups.size())) return -1;
    for (int s = 0; s < std::min(bodies, max_slots); ++s)
        if (s != slot && static_cast<int>(read_shared(s)) - 1 == rally) return rally;
    return target == rally ? rally : -1;
}

// Champions waiting at (or fighting) this group, other than us.
int Bot::rally_ready(int group) const {
    const int bits = read_shared(rally_ready_byte);
    int ready = 0;
    for (int s = 0; s < std::min(bodies, max_slots); ++s)
        if (s != slot && ((bits >> s) & 1) && static_cast<int>(read_shared(s)) - 1 == group) ++ready;
    return ready;
}

void Bot::set_ready(bool ready) {
    if (slot >= max_slots) return;
    const int bits = read_shared(rally_ready_byte);
    const int next = ready ? (bits | (1 << slot)) : (bits & ~(1 << slot));
    if (next != bits) write_shared(rally_ready_byte, static_cast<std::uint8_t>(next));
}

void Bot::post_rally(int group) {
    const int current = rally_group();
    if (current >= 0 && current != group) return;
    write_shared(rally_group_byte, static_cast<std::uint8_t>(group + 1));
}

int Bot::choose_target(float keep_bonus) {
    auto& me = cambc::self();
    const auto pos = me.position();
    const int round = static_cast<int>(cambc::round_number());
    // A monolith or golem guard waits until we have shards; a small guard is cleared at once.
    const int boss_start = boss >= 0 && (groups[static_cast<std::size_t>(boss)].facts.monolith
        || groups[static_cast<std::size_t>(boss)].facts.golem) ? 400 : 0;

    std::vector<int> claims(groups.size(), 0);
    for (int s = 0; s < std::min(bodies, max_slots); ++s) {
        if (s == slot) continue;
        const int t = static_cast<int>(read_shared(s)) - 1;
        if (t >= 0 && t < static_cast<int>(groups.size())) ++claims[static_cast<std::size_t>(t)];
    }
    // One group at a time is a rally: champions gather there and engage together.
    const int rally = rally_group();

    const float dps = std::max(25.0f, my_dps());
    const float hp_now = me.health(), hp_max = std::max(1.0f, me.max_health());
    const float gold_weight = plan.gemless ? 1.0f
        : (purchases < 10 && round < 900) ? 0.25f : (purchases < 20 && round < 1400) ? 0.12f : 0.05f;
    const float margin = 120.0f + 0.1f * hp_max;
    const bool support = me.champion_name() == cambc::champion_type::willow;
    const int follow = slot != 0 ? static_cast<int>(read_shared(0)) - 1 : static_cast<int>(read_shared(1)) - 1;
    const int zone_lo = slot * 256 / std::max(1, bodies), zone_hi = (slot + 1) * 256 / std::max(1, bodies);
    const int team = std::max(1, std::min(bodies, 5));
    const bool packed = plan.mode != mode_spread;
    const int leader_target = slot != 0 ? static_cast<int>(read_shared(0)) - 1 : -1;

    int best = -1, best_n = 1, best_need = 1;
    float best_score = 0, best_damage = 0;
    for (std::size_t i = 0; i < groups.size(); ++i) {
        auto& g = groups[i];
        prepare_group(g);
        const int gi = static_cast<int>(i);
        const bool is_boss = gi == boss;
        if (is_boss && round < boss_start) continue;
        if (!(g.flags & group_reachable) || (g.facts.gems <= 0 && g.facts.gold <= 0)) continue;
        if (g.facts.monolith && !is_boss) continue;
        if (g.lonely_until > round && !is_boss) continue;
        if (g.reward && g.ally_capturing_until > round) continue;
        const int d = group_distance(gi, pos);
        if (d >= far_away) continue;
        const int wait = std::max(0, g.respawn_at - round - d);
        const float home = static_cast<float>(std::min(home_field.at(g.facts.tiles.front()), 60));
        float gold_part = gold_weight * static_cast<float>(g.facts.gold);
        if (!plan.gemless) gold_part = std::min(gold_part, std::max(15.0f, 1.5f * static_cast<float>(g.facts.gems)));
        const float value = static_cast<float>(g.facts.gems) + gold_part;
        const int n_now = 1 + claims[i];
        // Upper bound (no clear time, no trips): skip groups that cannot beat the best so far.
        if (!support && !is_boss && value / static_cast<float>(n_now) / static_cast<float>(d + wait + 12) * 2.0f < best_score) continue;
        float unused = 0;
        clear_cost(g, dps, &unused);
        const float hits = g.hits;

        // Fewest champions that clear it without anyone leaving to heal.
        int need_any = 1;
        if (!g.reward) {
            need_any = team;
            for (int n = 1; n <= team; ++n) {
                float clear = 0;
                if (clear_cost(g, dps * static_cast<float>(n), &clear) / static_cast<float>(n) <= hp_now - margin) {
                    need_any = n;
                    break;
                }
            }
        }
        // Our share of the reward per round spent with n champions on it; < 0 when one champion's
        // trips would be too short (under six rounds of fighting) or too many (over six).
        const auto rate = [&](int n, float* mine_out, bool* trips_out) {
            float clear = 0, total = 0;
            if (!g.reward) total = clear_cost(g, dps * static_cast<float>(n), &clear);
            const float mine = total / static_cast<float>(n);
            float trip_time = 0;
            *trips_out = mine > hp_now - margin;
            if (*trips_out) {
                const float cap = hp_max - margin;
                const float per = hits / static_cast<float>(n);
                if (per > 0 && cap / per < 6.0f) return -1.0f;
                const float trips = std::ceil(mine / std::max(1.0f, cap));
                if (trips > 6.0f) return -1.0f;
                trip_time = trips * (10.0f + home);
            }
            *mine_out = mine;
            // Spread teams value their own share; packed teams the whole reward (they farm together)
            // less 45% per champion beyond those the group needs.
            const float share = packed ? value * std::pow(0.55f, static_cast<float>(std::max(0, n - need_any)))
                                       : value / static_cast<float>(n);
            return share / (static_cast<float>(d + wait) + clear + trip_time + 6.0f / static_cast<float>(n) + 12.0f);
        };
        float mine = 0;
        bool trips = false;
        float score = std::max(0.0f, rate(n_now, &mine, &trips));
        float damage_at_best = mine;
        int n_at_best = n_now;
        // A team clear: the fewest champions that need no trips home.
        if (!g.reward && team > n_now && (rally < 0 || rally == gi)) {
            for (int n = n_now + 1; n <= team; ++n) {
                float m = 0;
                bool t = false;
                const float r = rate(n, &m, &t);
                if (r < 0 || t) continue;
                const float s = r * (rally == gi ? 0.85f : 0.5f);
                if (s > score) { score = s; n_at_best = n; damage_at_best = m; }
                break;
            }
        }
        const int need = n_at_best;
        if (g.reward && claims[i] > 0) score *= 0.2f;
        if (g.reward) score *= 1.3f;
        if (plan.mode == mode_spread && g.zone >= zone_lo && g.zone < zone_hi) score *= 1.3f;
        if (gi == target && wait <= 10) score *= keep_bonus;
        if (packed && gi == leader_target) score *= 2.0f;
        // The altar guard: the whole team chips it down, then everyone banks.
        if (is_boss) {
            if (wait > 0) continue; // respawning: farm elsewhere meanwhile
            score = 100.0f / static_cast<float>(d + 10);
            n_at_best = team;
        }
        if (g.contested_until > round) score *= 0.1f;
        // Lone farmers avoid where enemies were seen; a packed team contests it.
        if (!packed && g.enemy_near_until > round) score *= 0.6f;
        if ((g.flags & group_enemy_side) && !plan.gemless) score *= g.facts.gems > 0 ? 0.5f : 0.25f;
        if (support && gi == follow) score *= 6.0f;
        if (score > best_score) {
            best_score = score;
            best = gi;
            best_n = n_at_best;
            best_damage = damage_at_best;
            best_need = need;
        }
    }
    if (camp_trace && best >= 0) {
        const auto& g = groups[static_cast<std::size_t>(best)];
        const int d = group_distance(best, pos);
        std::cout << "CH r" << round << " s" << slot << " " << cambc::to_string(pos) << " -> g" << best << " gems" << g.facts.gems
                  << " gold" << g.facts.gold << " hp" << int(group_hp(g)) << " d" << d << " wait" << std::max(0, g.respawn_at - round)
                  << " n" << best_n << " need" << best_need << " rew" << g.reward << " score" << best_score << " prev" << target << '\n';
    }
    want_n = best_n;
    need_n = std::min(best_need, best_n);
    plan_damage = best_damage;
    if (best != target) {
        set_ready(false);
        stand_shrink = 0;
    }
    if (best >= 0 && need_n > 1 + claims[static_cast<std::size_t>(best)]) post_rally(best);
    return best;
}

// Pick the tile to attack the target from: in our range of a member we should hit, out of reach of
// as many other monsters as possible, close to us.
bool Bot::pick_stand(int index) {
    auto& me = cambc::self();
    const auto& g = groups[static_cast<std::size_t>(index)];
    const auto pos = me.position();
    const int range = std::max(1, me.attack_range() - stand_shrink);
    struct Live { vec2 pos; float str; int range; bool monster; };
    std::vector<Live> live;
    bool any_monster = false;
    for (std::size_t i = 0; i < g.members.size(); ++i) {
        if (g.member_hp[i] <= 0) continue;
        live.push_back({g.members[i].pos, g.members[i].str, g.members[i].range, g.members[i].monster});
        any_monster |= g.members[i].monster;
    }
    if (live.empty()) return false;
    // First monster in kill order: melee champions stand next to it.
    vec2 first{-1, -1};
    float first_key = -1;
    for (std::size_t i = 0; i < g.members.size(); ++i) {
        if (g.member_hp[i] <= 0 || !g.members[i].monster) continue;
        const float key = g.members[i].str / g.member_hp[i];
        if (key > first_key) { first_key = key; first = g.members[i].pos; }
    }

    const Bits blocked(cambc::vision::champion_tiles() | cambc::vision::awake_camp_tiles()
        | cambc::vision::asleep_camp_tiles() | cambc::vision::teleport_tiles());

    int x0 = 1 << 20, y0 = 1 << 20, x1 = -1, y1 = -1;
    for (const auto& l : live) {
        x0 = std::min(x0, l.pos.x); y0 = std::min(y0, l.pos.y);
        x1 = std::max(x1, l.pos.x); y1 = std::max(y1, l.pos.y);
    }
    vec2 best{-1, -1};
    float best_cost = 1e9f;
    for (int y = y0 - range; y <= y1 + range; ++y)
        for (int x = x0 - range; x <= x1 + range; ++x) {
            const vec2 t{x, y};
            if (!in_bounds(t) || wall_bits.get(t)) continue;
            if (t != pos && blocked.get(t)) continue;
            bool reach = false;
            float threat = 0;
            for (const auto& l : live) {
                const int dist = cheb(t, l.pos);
                if (dist == 0) { reach = false; threat = 1e6f; break; }
                if (dist <= range && (l.monster || !any_monster)) reach = true;
                if (l.monster && dist <= l.range) threat += l.str;
            }
            if (!reach) continue;
            float cost = static_cast<float>(cheb(t, pos)) + threat * 0.12f;
            if (range <= 1 && first.x >= 0 && cheb(t, first) <= 1) cost -= 3.0f;
            if (cost < best_cost) { best_cost = cost; best = t; }
        }
    if (best.x < 0) return false;
    stand_group = index;
    stand_round = static_cast<int>(cambc::round_number());
    int alive_count = 0;
    for (float h : g.member_hp) alive_count += h > 0 ? 1 : 0;
    stand_alive = alive_count;
    stand = best;
    return true;
}

// Camp damage we take next round where we stand: awake monsters that reach us.
float Bot::camp_incoming(int* exit_steps) const {
    const auto pos = cambc::self().position();
    float incoming = 0;
    int steps = 0;
    for (auto p : cambc::vision::awake_camp_tiles().positions()) {
        if (cheb(p, pos) > 6) continue;
        for (const auto& g : groups) {
            if (cheb(g.facts.center, p) > 40 || !g.tiles.is_set(p)) continue;
            for (std::size_t i = 0; i < g.members.size(); ++i) {
                const auto& m = g.members[i];
                if (m.pos != p || !m.monster) continue;
                const int dist = cheb(p, pos);
                if (dist > m.range) break;
                incoming += m.str;
                steps = std::max(steps, m.range + 1 - dist);
                break;
            }
            break;
        }
    }
    *exit_steps = steps;
    return incoming;
}

// One step that brings us closer to `goal` (Chebyshev), onto a free tile; false when none does.
bool Bot::step_towards(vec2 goal) {
    auto& me = cambc::self();
    if (walked_fail || cast_exclusive || !me.can_move()) return false;
    const auto pos = me.position();
    const Bits blocked(cambc::vision::champion_tiles() | cambc::vision::awake_camp_tiles()
        | cambc::vision::asleep_camp_tiles() | cambc::vision::teleport_tiles());
    vec2 best = pos;
    int best_d = cheb(pos, goal);
    for (int dx = -1; dx <= 1; ++dx)
        for (int dy = -1; dy <= 1; ++dy) {
            const vec2 q{pos.x + dx, pos.y + dy};
            if (q == pos || !in_bounds(q) || wall_bits.get(q) || blocked.get(q)) continue;
            const int d = cheb(q, goal);
            if (d < best_d) { best_d = d; best = q; }
        }
    if (best == pos) return false;
    return walk(best);
}

// Where to wait for a group's respawn: two to four steps from it, nearest our spawn. Cached.
vec2 Bot::wait_spot(int index) {
    auto& g = groups[static_cast<std::size_t>(index)];
    if (g.wait_spot.x >= 0 || !g.field) return g.wait_spot;
    vec2 best{-1, -1};
    int best_home = far_away;
    int x0 = 1 << 20, y0 = 1 << 20, x1 = -1, y1 = -1;
    for (auto t : g.facts.tiles) {
        x0 = std::min(x0, t.x); y0 = std::min(y0, t.y);
        x1 = std::max(x1, t.x); y1 = std::max(y1, t.y);
    }
    for (int y = y0 - 4; y <= y1 + 4; ++y)
        for (int x = x0 - 4; x <= x1 + 4; ++x) {
            const vec2 t{x, y};
            if (!in_bounds(t) || wall_bits.get(t)) continue;
            const int v = g.field->at(t);
            if (v < 2 || v > 4) continue;
            const int h = home_field.at(t) * 4 + v;
            if (h < best_home) { best_home = h; best = t; }
        }
    g.wait_spot = best;
    return best;
}

// One step to a free tile farther along `field` from its roots.
bool Bot::step_outward(const Field& field) {
    auto& me = cambc::self();
    if (walked_fail || cast_exclusive || !me.can_move()) return false;
    const auto pos = me.position();
    const Bits blocked(cambc::vision::champion_tiles() | cambc::vision::awake_camp_tiles()
        | cambc::vision::asleep_camp_tiles() | cambc::vision::teleport_tiles());
    vec2 best = pos;
    int best_v = field.at(pos);
    for (int dx = -1; dx <= 1; ++dx)
        for (int dy = -1; dy <= 1; ++dy) {
            const vec2 q{pos.x + dx, pos.y + dy};
            if (q == pos || !in_bounds(q) || wall_bits.get(q) || blocked.get(q)) continue;
            const int v = field.at(q);
            if (v > best_v && v < far_away) { best_v = v; best = q; }
        }
    if (best == pos) return false;
    return walk(best);
}

bool Bot::farm() {
    const bool acted = farm_step();
    // Path fields are large host calls (about 5.5 fuel per tile): build one for a group we lack
    // only when this turn has fuel to spare, nearest groups first.
    const long cost = 6L * cambc::map::width() * cambc::map::height();
    if (static_cast<long>(cambc::fuel_remaining()) > cost + 30000) {
        const auto pos = cambc::self().position();
        int pick = -1, pick_d = far_away;
        for (std::size_t gi = 0; gi < groups.size(); ++gi) {
            const auto& g = groups[gi];
            if (g.field || !(g.flags & group_reachable)) continue;
            const int d = cheb(g.facts.center, pos);
            if (d < pick_d) { pick_d = d; pick = static_cast<int>(gi); }
        }
        if (pick >= 0) {
            const long before = static_cast<long>(cambc::fuel_remaining());
            group_field(pick);
            if (camp_profile && slot == 0)
                std::cout << "BFS r" << cambc::round_number() << " g" << pick << " cost " << before - static_cast<long>(cambc::fuel_remaining()) << '\n';
        }
    }
    return acted;
}

bool Bot::farm_step() {
    auto& me = cambc::self();
    const int round = static_cast<int>(cambc::round_number());
    const auto pos = me.position();
    fuel_mark[2] = static_cast<long>(cambc::fuel_remaining());
    struct Report {
        const Bot* bot; int round;
        ~Report() {
            if (camp_profile && bot->slot == 0 && round >= 100 && round <= 260)
                std::cout << "FUEL r" << round << " upd " << fuel_mark[0] - fuel_mark[1] << " mid " << fuel_mark[1] - fuel_mark[2]
                          << " farm " << fuel_mark[2] - static_cast<long>(cambc::fuel_remaining()) << " left " << cambc::fuel_remaining() << '\n';
        }
    } report{this, round};
    if (camp_trace && round % 10 == 0 && round <= 300) {
        int es = 0;
        const float inc = camp_incoming(&es);
        std::cout << "T r" << round << " s" << slot << " " << cambc::to_string(pos) << " hp" << int(me.health()) << " t" << target
                  << " n" << want_n << " dmg" << int(plan_damage) << " heal" << heal_trip << " inc" << int(inc) << " st" << cambc::to_string(stand)
                  << " gems" << me.gem_count();
        if (target >= 0) std::cout << " ghp" << int(group_hp(groups[static_cast<std::size_t>(target)])) << " rew" << groups[static_cast<std::size_t>(target)].reward << " resp" << groups[static_cast<std::size_t>(target)].respawn_at;
        std::cout << '\n';
    }
    if (camp_tally) {
        if (round > tally_round + 1) tally[0] += round - tally_round - 1;
        tally_round = round;
        if (round % 250 == 0) {
            std::cout << "TALLY r" << round << " s" << slot;
            for (std::size_t k = 0; k < tally.size(); ++k) std::cout << ' ' << tally_names[k] << '=' << tally[k];
            std::cout << '\n';
        }
    }
    const auto mark = [](int k) { if (camp_tally) ++tally[static_cast<std::size_t>(k)]; return true; };
    const auto awake = cambc::vision::awake_camp_tiles();
    const float hp = me.health(), hp_max = std::max(1.0f, me.max_health());

    // Leave a clear while we can still walk out of reach, then heal before coming back.
    int exit_steps = 0;
    float incoming = camp_incoming(&exit_steps);
    // Health lost to nothing we can see: a camp hidden in a bush is hitting us.
    if (last_hp_round == round - 1 && hp < last_hp - 1.0f && incoming <= 0 && enemies.empty()) {
        incoming = last_hp - hp;
        exit_steps = 3;
    }
    last_hp = hp;
    last_hp_round = round;
    if (incoming > 0 && hp <= incoming * static_cast<float>(exit_steps + 2) + 40.0f) heal_trip = true;
    if (heal_trip) {
        // Out of reach the target model decides whether the next clear needs a trip home first.
        const auto threat = threat_tiles();
        if (threat.is_set(pos)) {
            set_ready(false);
            step_away(threat);
            return mark(1);
        }
        heal_trip = false;
        chosen_round = -100;
        if (hp < 0.5f * hp_max) {
            if (home_field.at(pos) <= 6) step_along(home_field);
            else recall();
            return mark(1);
        }
    }

    // Engaged: members of our target are awake within reach of us.
    bool engaged = false;
    if (target >= 0) {
        const auto& g = groups[static_cast<std::size_t>(target)];
        for (auto p : (awake & g.tiles).positions())
            if (cheb(p, pos) <= 6) { engaged = true; break; }
    }

    bool retarget = target < 0;
    if (target >= 0) {
        const auto& g = groups[static_cast<std::size_t>(target)];
        const bool capturing = g.reward && g.tiles.is_set(pos);
        if (!capturing && !engaged && round - chosen_round >= 5) retarget = true;
        if (g.respawn_at > round + 6 && round - chosen_round >= 3) retarget = true;
        if (g.reward && !g.tiles.is_set(pos))
            for (const auto& s : allies)
                if (g.tiles.is_set(s.pos)) retarget = true;
    }
    if (retarget) {
        const int before = target;
        target = choose_target(target >= 0 ? 1.3f : 1.0f);
        chosen_round = round;
        if (target != before) gather_since = -1;
    }
    if (target < 0) return !mark(9);
    auto& g = groups[static_cast<std::size_t>(target)];
    if (!g.field && cambc::fuel_remaining() < 45000) return mark(9);
    const auto& field = group_field(target);

    if (g.reward) {
        if (g.tiles.is_set(pos)) {
            // Hold the capture; a region that never pays out is contested somewhere out of sight.
            if (hold_group != target) {
                hold_group = target;
                hold_since = round;
            }
            if (round - hold_since <= cambc::constants::capture_time + 6) return mark(2);
            g.contested_until = round + 60;
            hold_group = -1;
            target = choose_target(1.0f);
            chosen_round = round;
            if (target < 0) return false;
            step_along(group_field(target), !groups[static_cast<std::size_t>(target)].reward);
            return true;
        }
        if (camp_trace && false) std::cout << "CW r" << round << " s" << slot << " " << cambc::to_string(pos) << " t" << target << " d" << field.at(pos) << " claims" << int(read_shared(0)) << ',' << int(read_shared(1)) << ',' << int(read_shared(2)) << ',' << int(read_shared(3)) << ',' << int(read_shared(4)) << '\n';
        step_along(field);
        return mark(3);
    }
    hold_group = -1;
    // Never stand on a captured region: an occupied spawn tile stops the group respawning.
    // Wait three or four tiles out, leaving a lane free so whoever stands on the region (blocking its
    // respawn) can step off, even in a one-tile corridor.
    // Everyone waits on the same spot (2 to 4 steps out, on our side) so the team meets enemies together.
    if (g.respawn_at > round) {
        const int here = field.at(pos);
        if (here == 0) {
            if (!step_outward(field)) step_off(g.tiles);
            return mark(4);
        }
        const vec2 spot = wait_spot(target);
        if (spot.x < 0 || pos == spot) return mark(4);
        if (cheb(pos, spot) <= 1 && here >= 2) return mark(4);
        if (cheb(pos, spot) > 3 || !step_towards(spot)) {
            if (here > 4) step_along(field, true);
            else if (here < 2) step_outward(field);
        }
        return mark(4);
    }

    // Alive. Come back healthy when the clear would cost more than we can spend now.
    if (!engaged && hp < 0.5f * hp_max && plan_damage > hp - 100.0f && awake.is_empty()) {
        if (home_field.at(pos) <= 8) step_along(home_field);
        else recall();
        return mark(5);
    }

    // Groups too strong for one champion are engaged together: wait just out of reach until
    // enough champions are ready, unless the group is already awake (someone is fighting it).
    const bool group_awake = !(awake & g.tiles).is_empty();
    int reach = 0;
    for (const auto& m : g.members) reach = std::max(reach, m.range);
    const bool close = field.at(pos) <= reach + 3;
    set_ready(close && need_n > 1);
    // Wait only while other champions are on their way to this group.
    int coming = 0;
    for (int s = 0; s < std::min(bodies, max_slots); ++s)
        if (s != slot && static_cast<int>(read_shared(s)) - 1 == target) ++coming;
    // Packed teams move together anyway and wait less (monolith and golem groups excepted).
    const int patience = plan.mode == mode_spread || g.deadly ? 30 : 15;
    // Nobody else is coming to a group that needs a team: drop it for a while.
    if (need_n > 1 && !group_awake && coming == 0) {
        g.lonely_until = round + 60;
        set_ready(false);
        target = choose_target(1.0f);
        chosen_round = round;
        return mark(6);
    }
    if (need_n > 1 && !group_awake && rally_ready(target) + 1 < need_n) {
        if (gather_since < 0) gather_since = round;
        if (round - gather_since > patience) {
            // Nobody came: farm something else for a while.
            g.lonely_until = round + 120;
            gather_since = -1;
            set_ready(false);
            target = choose_target(1.0f);
            chosen_round = round;
            return mark(6);
        }
        if (field.at(pos) <= reach + 2) return mark(6);
        step_along(field, true);
        return mark(6);
    }
    gather_since = -1;

    // Attack position: re-picked when members die, the tile gets taken, or every few rounds.
    int alive_count = 0;
    for (float h : g.member_hp) alive_count += h > 0 ? 1 : 0;
    const bool stale = stand_group != target || alive_count != stand_alive || round - stand_round >= 8
        || (stand != pos && cambc::vision::champion_tiles().is_set(stand));
    if (stale && !pick_stand(target)) {
        step_along(field, true);
        return true;
    }
    if (pos == stand) {
        // Standing in range without a hit: no line of sight from here, so come closer.
        if (!attacked && ++stand_idle >= 2 && stand_shrink < me.attack_range() - 1) {
            ++stand_shrink;
            stand_idle = 0;
            stand_round = -100;
        }
        return mark(7);
    }
    stand_idle = 0;
    // Ranged champions already in reach of a member hold rather than walk into more fire.
    if (me.attack_range() > 1 && !me.can_attack()) return mark(7);
    // Far off: the group's path field. Close: greedy steps onto the stand tile.
    if (cheb(pos, stand) > 3 || !step_towards(stand)) {
        if (!walked_fail) step_along(field, true);
    }
    return mark(8);
}
