// Draft: pick self-sufficient farmers and split the map's camp groups into one region per champion.
#include <cambc.h>
#include "draft.h"
#include "plan.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

using cambc::vec2;
using cambc::team;

namespace {

// Strongest solo farmers first: sustain, strong basic attacks, cheap resets.
constexpr std::array<cambc::champion_draft, 5> kPicks{
    cambc::champion_draft::vendetta, cambc::champion_draft::vesper_vale, cambc::champion_draft::marcella,
    cambc::champion_draft::tavi, cambc::champion_draft::varg};

cambc::champion_type type_of(cambc::champion_draft pick) {
    switch (pick) {
    case cambc::champion_draft::vendetta: return cambc::champion_type::vendetta;
    case cambc::champion_draft::marcella: return cambc::champion_type::marcella;
    case cambc::champion_draft::varg: return cambc::champion_type::varg;
    case cambc::champion_draft::tavi: return cambc::champion_type::tavi;
    case cambc::champion_draft::vesper_vale: return cambc::champion_type::vesper_vale;
    default: return cambc::champion_type::varg;
    }
}

std::array<std::byte, 1024> memory{};
bool planned = false;

void make_plan() {
    planned = true;
    memory.fill(std::byte{0});
    const int slots = std::clamp<int>(static_cast<int>(cambc::draft::team_size()), 1, 5);
    memory[plan::kMagic0] = std::byte{'S'};
    memory[plan::kMagic1] = std::byte{'F'};
    memory[plan::kSlots] = std::byte(slots);
    for (int s = 0; s < 5; ++s) memory[plan::kTypes + s] = std::byte(static_cast<int>(type_of(kPicks[s])));

    const auto groups = cambc::map::camp_groups();
    const int n = std::min<int>(static_cast<int>(groups.size()), plan::kMaxGroups);
    memory[plan::kGroups] = std::byte(std::min(n, 255));

    auto walls = cambc::map::walls() | cambc::map::holes() | cambc::map::altars();
    const auto& ours = cambc::map::spawn_region(team::ally);
    const auto& theirs = cambc::map::spawn_region(team::enemy);
    cambc::distance_field from_us, from_them;
    cambc::bitboard seen_us, seen_them;
    from_us.compute(walls, ours, seen_us);
    from_them.compute(walls, theirs, seen_them);

    double sx = 0, sy = 0;
    const auto spawn_tiles = ours.positions();
    for (auto p : spawn_tiles) { sx += p.x; sy += p.y; }
    sx /= std::max<std::size_t>(1, spawn_tiles.size());
    sy /= std::max<std::size_t>(1, spawn_tiles.size());

    struct G { int idx; double angle, value, hard, cx, cy; };
    std::vector<G> mine;
    for (int i = 0; i < n; ++i) {
        const auto& g = groups[i];
        double cx = 0, cy = 0, hp = 0, str = 0;
        int du = 65535, dt = 65535, cnt = 0;
        for (const auto& c : g.camps()) {
            const auto p = c.spawn_position();
            cx += p.x; cy += p.y; ++cnt;
            const auto st = c.base_stats();
            hp += st.base_health; str += st.base_strength;
            if (seen_us.is_set(p)) du = std::min<int>(du, from_us.get(p));
            if (seen_them.is_set(p)) dt = std::min<int>(dt, from_them.get(p));
        }
        if (!cnt) { memory[plan::kOwners + i] = std::byte{plan::kNoOwner}; continue; }
        cx /= cnt; cy /= cnt;
        if (du >= 65535) { memory[plan::kOwners + i] = std::byte{plan::kNoOwner}; continue; }
        if (du > dt + 3) { memory[plan::kOwners + i] = std::byte{plan::kEnemySide}; continue; }
        const double value = g.gem_value() + 0.3 * g.gold_value();
        G e{i, std::atan2(cy - sy, cx - sx), value, hp * (1 + str / 40.0), cx, cy};
        if (std::abs(du - dt) <= 3) e.value *= 0.5;  // middle: shared, counts half
        mine.push_back(e);
        memory[plan::kOwners + i] = std::byte(std::abs(du - dt) <= 3 ? plan::kMiddleBit : 0);
    }

    // Angular sweep starting after the widest gap, cut into equal-value sectors.
    std::sort(mine.begin(), mine.end(), [](const G& a, const G& b) { return a.angle < b.angle; });
    const int m = static_cast<int>(mine.size());
    int start = 0;
    if (m > 1) {
        double best_gap = -1;
        for (int i = 0; i < m; ++i) {
            const double a = mine[i].angle;
            const double b = i + 1 < m ? mine[i + 1].angle : mine[0].angle + 2 * M_PI;
            if (b - a > best_gap) { best_gap = b - a; start = (i + 1) % m; }
        }
    }
    double total = 0;
    for (const auto& e : mine) total += e.value;
    std::vector<int> sector(m, 0);
    std::vector<double> hard(slots, 0), cxs(slots, 0), cys(slots, 0), cnts(slots, 0);
    double run = 0;
    for (int k = 0; k < m; ++k) {
        const auto& e = mine[(start + k) % m];
        const double mid = run + e.value / 2;
        int s = total > 0 ? static_cast<int>(mid / total * slots) : 0;
        s = std::clamp(s, 0, slots - 1);
        sector[(start + k) % m] = s;
        run += e.value;
        hard[s] += e.hard; cxs[s] += e.cx; cys[s] += e.cy; cnts[s] += 1;
    }
    // Hardest sector to the strongest pick (picks are ordered strongest first).
    std::vector<int> order(slots);
    for (int s = 0; s < slots; ++s) order[s] = s;
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return hard[a] > hard[b]; });
    std::vector<int> slot_of(slots);
    for (int r = 0; r < slots; ++r) slot_of[order[r]] = r;
    for (int k = 0; k < m; ++k) {
        const int i = mine[k].idx;
        const auto flag = std::to_integer<std::uint8_t>(memory[plan::kOwners + i]);
        memory[plan::kOwners + i] = std::byte(flag | static_cast<std::uint8_t>(slot_of[sector[k]]));
    }

    // Home altar: the reachable altar nearest to each sector's centre.
    const auto altars = cambc::map::altars().positions();
    for (int s = 0; s < slots; ++s) {
        const int sec = order[s];
        vec2 best{255, 255};
        double best_d = 1e18;
        const double cx = cnts[sec] ? cxs[sec] / cnts[sec] : sx, cy = cnts[sec] ? cys[sec] / cnts[sec] : sy;
        for (auto a : altars) {
            const double d = std::max(std::abs(a.x - cx), std::abs(a.y - cy));
            if (d < best_d) { best_d = d; best = a; }
        }
        memory[plan::kAltars + 2 * s] = std::byte(static_cast<std::uint8_t>(best.x));
        memory[plan::kAltars + 2 * s + 1] = std::byte(static_cast<std::uint8_t>(best.y));
    }
}

} // namespace

void play_draft() {
    if (!planned) {
        try { make_plan(); } catch (const cambc::error&) { planned = true; }
    }
    cambc::draft_memory = std::span<const std::byte>{memory.data(), memory.size()};

    const auto chosen = cambc::draft::current_draft(team::ally);
    for (auto pick : kPicks) {
        if (std::find(chosen.begin(), chosen.end(), pick) == chosen.end()) {
            cambc::draft::pick(pick);
            return;
        }
    }
}
