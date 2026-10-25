// The fuel subsystem (see fuel.h): vision snapshot, map analysis, camp knowledge, distance fields, movement.
#include <cambc.h>
#include "bot.h"
#include "fuel.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <utility>

std::uint32_t g_round = 0;

void warn(const char* where, const std::exception& e) {
    static int count = 0;
    if (++count <= 20) std::cout << "warn: " << where << ": " << e.what() << '\n';
}

namespace {
// Scratch boards are allocated once, on first use (map layout must be known).
struct Scratch {
    cambc::bitboard tmp, blocked, goals, goals2, here, seen, wseen;
    cambc::distance_field f, w;
};
Scratch* g_s = nullptr;
Scratch& scratch() {
    if (!g_s) g_s = new Scratch;
    return *g_s;
}

View* g_view = nullptr;
std::vector<cambc::champion> g_others;  // allied handles other than ours, read once
std::vector<cambc::champion_type> g_other_types;
bool g_others_read = false;

Bits g_walls_local, g_hard_local;
}  // namespace

void andnot_into(const cambc::bitboard& a, const cambc::bitboard& b, cambc::bitboard& out) {
    auto& t = scratch().tmp;  // a & b, taken before out (which may be a or b) is written
    t.clear();
    t |= a;
    t &= b;
    if (&out != &a) {
        out.clear();
        out |= a;
    }
    out ^= t;
}

void invalidate() {
    if (g_view) g_view->ok = false;
}

const View& view() {
    if (!g_view) {
        g_view = new View;
        g_view->lvis.bind(g_view->vis);
        g_view->lcamps.bind(g_view->camps);
        g_view->lchamps.bind(g_view->champs);
        g_view->ltele.bind(g_view->tele);
    }
    View& v = *g_view;
    if (v.ok) return v;
    if (!g_others_read) {
        g_others_read = true;
        const int me = static_cast<int>(static_cast<cambc::handle_champion_t>(cambc::self()));
        for (const auto& a : cambc::champions(team::ally))
            if (static_cast<int>(static_cast<cambc::handle_champion_t>(a)) != me) {
                g_others.push_back(a);
                cambc::champion_type t = cambc::champion_type::vesper_vale;
                try { t = a.champion_name(); } catch (const cambc::error&) {}
                g_other_types.push_back(t);
            }
    }
    cambc::vision::visible_tiles_into(v.vis);
    cambc::vision::camp_tiles_into(v.camps);
    cambc::vision::champion_tiles_into(team::enemy, v.foes);
    cambc::vision::champion_tiles_into(team::ally, v.champs);
    v.champs |= v.foes;
    cambc::vision::teleport_tiles_into(v.tele);
    v.self = cambc::self().position();
    v.allies.clear();
    v.ally_types.clear();
    for (size_t i = 0; i < g_others.size(); ++i) {
        cambc::detail::champion_info info{};
        if (!peek(g_others[i], info)) continue;
        const vec2 p{info.x, info.y};
        if (!inside(p)) continue;
        v.allies.push_back(p);
        v.ally_types.push_back(g_other_types[i]);
        raw_set(v.champs.ptr(), p);
    }
    v.ok = true;
    return v;
}

const Bits& walls_local() { return g_walls_local; }
const Bits& hard_local() { return g_hard_local; }

bool free_tile(vec2 p) {
    if (!inside(p)) return false;
    const auto& v = view();
    if (p == v.self) return true;
    return !g_walls_local.at(p) && !v.lcamps.at(p) && !v.lchamps.at(p) && !v.ltele.at(p);
}

// ---------------------------------------------------------------- setup

void Bot::initialise() {
    init_ = true;
    init_dims();
    g_walls_local.bind(walk_walls());
    {
        static const cambc::bitboard hard = cambc::map::walls() | cambc::map::altars();
        g_hard_local.bind(hard);
    }
    auto& me = cambc::self();
    h_ = cambc::map::camp_groups();  // ~30k fuel: once
    g_ = analyse_map(h_);
    const size_t ng = g_.size();
    k_.assign(ng, Known{});
    for (size_t i = 0; i < ng; ++i)
        for (auto kind : g_[i].kinds) k_[i].hp.push_back(camp_spec(kind).hp);
    gfield_.clear();
    gfield_.resize(ng);
    group_at_.assign(static_cast<size_t>(map_w() * map_h()), -1);
    camp_at_.assign(static_cast<size_t>(map_w() * map_h()), -1);
    gtiles_.reserve(ng);
    for (size_t i = 0; i < ng; ++i) {
        int r = 0;
        for (auto t : g_[i].tiles) r = std::max(r, cheb(t, g_[i].center));
        reach_.push_back(r);
        gtiles_.emplace_back();  // cleared board
        auto* w = gtiles_.back().ptr();
        for (size_t c = 0; c < g_[i].tiles.size(); ++c) {
            const vec2 t = g_[i].tiles[c];
            raw_set(w, t);
            group_at_[static_cast<size_t>(tidx(t))] = static_cast<std::int16_t>(i);
            camp_at_[static_cast<size_t>(tidx(t))] = static_cast<std::int16_t>(c);
        }
    }
    size_ = static_cast<int>(cambc::champions(team::ally).size());
    // Cassi and Issac count as one pick.
    slot_ = slot_of(me.champion_name(), size_);
    const auto* mem = cambc::draft_memory.data();
    if (static_cast<char>(mem[dm::magic]) == 'E' && static_cast<int>(mem[dm::ngroups]) == static_cast<int>(std::min<size_t>(ng, 250))) {
        size_ = static_cast<int>(mem[dm::size]);
        slot_ = slot_of(me.champion_name(), size_);
        for (size_t i = 0; i < ng; ++i)
            home_slot_.push_back(i < 250 ? static_cast<std::uint8_t>(mem[dm::home + i]) : 255);
    } else {
        home_slot_ = assign_homes(g_, std::max(1, size_));
    }
    for (const auto& ab : me.abilities())
        if (ab.is_dash()) dasher_ = true;
    my_range_ = me.attack_range();
    build_threats();
    spawn_local_.bind(cambc::map::spawn_region(team::ally));
    {
        const int self_h = static_cast<int>(static_cast<cambc::handle_champion_t>(me));
        for (const auto& a : cambc::champions(team::ally))
            if (static_cast<int>(static_cast<cambc::handle_champion_t>(a)) != self_h) others_.push_back(a);
    }

    auto& s = scratch();
    home_.compute(walk_walls(), cambc::map::spawn_region(team::ally), s.seen);
    // Tiles from which an altar can take a tribute, written straight into the board.
    altar_spots_.clear();
    const int ar = cambc::constants::submit_gems_range;
    altars_ = cambc::map::altars().positions();
    enemy_lock_.assign(altars_.size(), -1);
    lock_seen_.assign(altars_.size(), -1000);
    for (auto a : altars_)
        for (int dy = -ar; dy <= ar; ++dy)
            for (int dx = -ar; dx <= ar; ++dx) {
                const vec2 t{a.x + dx, a.y + dy};
                if (inside(t)) raw_set(altar_spots_.ptr(), t);
            }
    andnot_into(altar_spots_, walk_walls(), s.goals);
    altar_spots_.clear();
    altar_spots_ |= s.goals;
    altar_.compute(walk_walls(), altar_spots_, s.seen);
    for (auto t : cambc::map::spawn_region(team::ally).positions()) home_altar_ = std::min(home_altar_, altar_.at(t));
    bank_d_.assign(ng, kFar);
    for (size_t i = 0; i < ng; ++i)
        for (auto t : g_[i].tiles) bank_d_[i] = std::min(bank_d_[i], altar_.at(t));

    for (size_t i = 0; i < ng; ++i) fill_order_.push_back(static_cast<int>(i));
    std::stable_sort(fill_order_.begin(), fill_order_.end(), [&](int a, int b) {
        const bool ma = home_slot_[a] == slot_, mb = home_slot_[b] == slot_;
        if (ma != mb) return ma;
        return g_[a].d_ally < g_[b].d_ally;
    });
}

// Walking fields to each group's tiles, one host BFS each (no export), while fuel lasts.
void Bot::fill_fields(std::uint64_t reserve) {
    for (int i : fill_order_) {
        if (!gfield_[i].empty()) continue;
        if (!fuel(reserve) || !live()) return;
        gfield_[i].compute(dasher_ ? jump_walls() : walk_walls(), gtiles_[i], scratch().seen);
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

bool Bot::group_awake(int group) const {
    for (auto t : g_[group].tiles)
        if (bit(awake_, t)) return true;
    return false;
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
        // One host query: camp_group::state() would rebuild the whole vision board first (~4k fuel).
        const auto handle = static_cast<std::uint32_t>(static_cast<cambc::handle_camp_group_t>(h_[i]));
        std::uint32_t st = 0;
        if (cambc::detail::sdk_camp_group_state(handle, &st) != 0 || st == 0 || st > 3) continue;
        const auto before = k.state;
        if (st == 1) {
            k.state = Known::alive;
        } else if (st == 2) {
            k.state = Known::reward;
        } else {
            std::int32_t timer = 1;
            if (cambc::detail::sdk_camp_group_cooldown(handle, &timer) != 0) timer = 1;
            k.state = Known::respawning;
            k.ready = now + std::max(1, static_cast<int>(timer));
            for (size_t c = 0; c < g_[i].tiles.size(); ++c) k.hp[c] = camp_spec(g_[i].kinds[c]).hp;
        }
        if (k.state != before) block_dirty_ = true;
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

// Tiles of camps we believe alive, written straight into the board.
void Bot::rebuild_camp_block() {
    if (!block_dirty_) return;
    block_dirty_ = false;
    camp_block_.clear();
    auto* w = camp_block_.ptr();
    for (size_t i = 0; i < g_.size(); ++i) {
        const auto& k = k_[i];
        if (k.state == Known::respawning || k.state == Known::reward) continue;
        for (size_t c = 0; c < g_[i].tiles.size(); ++c)
            if (k.hp[c] > 0) raw_set(w, g_[i].tiles[c]);
    }
}

// Movement (go, step_on, try_dash, go_home, recall_or_walk, walk_static): path.cpp.
