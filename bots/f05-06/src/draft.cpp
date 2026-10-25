// Draft: pick self-sufficient farmers and split our side's camp groups into one region per champion.
#include <cambc.h>
#include "draft.h"
#include "common.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <vector>

using cambc::champion_draft;

namespace {

// Preference order; the first team_size() picks form the lineup. Toughest farmers first.
constexpr std::array<champion_draft, 7> kOrder{champion_draft::vendetta, champion_draft::marcella,
    champion_draft::vesper_vale, champion_draft::tavi, champion_draft::varg, champion_draft::ryth,
    champion_draft::ren};

// How well a champion soaks a hard region (higher = gets the harder region).
int toughness(champion_draft c) {
    switch (c) {
    case champion_draft::marcella: return 5;
    case champion_draft::vendetta: return 4;
    case champion_draft::varg: return 3;
    case champion_draft::vesper_vale: return 2;
    default: return 1;
    }
}

std::array<std::uint8_t, 1024> g_mem{};
bool g_planned = false;

struct G {
    vec2 c;
    double value, hard, ang;
    std::uint16_t dus, dthem;
    int idx;
};

std::uint16_t group_dist(const cambc::distance_field& f, const cambc::bitboard& visited, const std::vector<vec2>& tiles) {
    std::uint16_t best = kFar;
    for (vec2 t : tiles)
        if (Raw::bit(visited, t)) best = std::min(best, Raw::dist(f, t));
    return best;
}

void plan(const std::vector<champion_draft>& lineup) {
    Raw::calibrate();
    const int n = static_cast<int>(lineup.size());
    g_mem.fill(0);
    g_mem[dm::kMagic0] = 'S';
    g_mem[dm::kMagic1] = 'F';
    g_mem[dm::kSlots] = static_cast<std::uint8_t>(n);

    const auto groups = cambc::map::camp_groups();
    const int ng = std::min<int>(static_cast<int>(groups.size()), dm::kMaxGroups);
    g_mem[dm::kGroupsLo] = static_cast<std::uint8_t>(ng & 255);
    g_mem[dm::kGroupsHi] = static_cast<std::uint8_t>(ng >> 8);

    const auto& sa = cambc::map::spawn_region(team::ally);
    const auto& se = cambc::map::spawn_region(team::enemy);
    const auto terrain = cambc::map::walls() | cambc::map::holes() | cambc::map::altars();
    cambc::distance_field fus, fthem;
    cambc::bitboard vus, vthem;
    fus.compute(terrain | se, sa, vus);
    fthem.compute(terrain | sa, se, vthem);

    // Spawn centroid.
    double sx = 0, sy = 0;
    const auto spawn_tiles = sa.positions();
    for (vec2 p : spawn_tiles) sx += p.x, sy += p.y;
    if (!spawn_tiles.empty()) sx /= spawn_tiles.size(), sy /= spawn_tiles.size();

    std::vector<G> ours;
    std::vector<std::vector<vec2>> tiles(ng);
    for (int i = 0; i < ng; ++i) {
        const auto& g = groups[i];
        tiles[i] = g.spawn_tiles().positions();
        G e{};
        e.idx = i;
        double cx = 0, cy = 0;
        for (vec2 t : tiles[i]) cx += t.x, cy += t.y;
        if (!tiles[i].empty()) cx /= tiles[i].size(), cy /= tiles[i].size();
        e.c = {static_cast<int>(std::lround(cx)), static_cast<int>(std::lround(cy))};
        e.value = g.gem_value() + 0.4 * g.gold_value();
        double hp = 0, str = 0;
        for (const auto& c : g.camps()) {
            const auto s = c.base_stats();
            hp += s.base_health;
            str += s.base_strength;
        }
        e.hard = hp * str;
        e.dus = group_dist(fus, vus, tiles[i]);
        e.dthem = group_dist(fthem, vthem, tiles[i]);
        e.ang = std::atan2(cy - sy, cx - sx);
        if (e.dus == kFar && e.dthem == kFar) {
            g_mem[dm::kGroupOwner + i] = dm::kShared;
        } else if (e.dus > e.dthem) {
            g_mem[dm::kGroupOwner + i] = dm::kEnemySide;
        } else if (e.dus + 3 >= e.dthem && e.dus != kFar) {
            g_mem[dm::kGroupOwner + i] = dm::kShared;  // contested middle: anyone
        } else {
            ours.push_back(e);
        }
    }

    // Altar distances (BFS from each altar).
    const auto altars = cambc::map::altars().positions();
    std::vector<std::vector<std::uint16_t>> adist(altars.size());
    for (size_t a = 0; a < altars.size(); ++a) {
        cambc::bitboard root, vis;
        root.clear();
        root.set(altars[a]);
        cambc::distance_field f;
        f.compute(cambc::map::walls() | cambc::map::holes() | se, root, vis);
        adist[a].resize(ng, kFar);
        for (int i = 0; i < ng; ++i) adist[a][i] = group_dist(f, vis, tiles[i]);
    }

    // Angular sweep around our spawn, starting after the widest empty arc; cut into n arcs of equal value.
    std::sort(ours.begin(), ours.end(), [](const G& a, const G& b) { return a.ang < b.ang; });
    if (ours.size() > 1) {
        size_t start = 0;
        double gap = -1;
        for (size_t i = 0; i < ours.size(); ++i) {
            const double next = i + 1 < ours.size() ? ours[i + 1].ang : ours[0].ang + 2 * M_PI;
            if (next - ours[i].ang > gap) gap = next - ours[i].ang, start = (i + 1) % ours.size();
        }
        std::rotate(ours.begin(), ours.begin() + start, ours.end());
    }
    double total = 0;
    for (const auto& e : ours) total += e.value;
    std::vector<std::vector<int>> region(std::max(n, 1));
    double acc = 0;
    for (const auto& e : ours) {
        int r = n > 0 ? static_cast<int>((acc + e.value * 0.5) / std::max(total, 1.0) * n) : 0;
        r = std::clamp(r, 0, std::max(n - 1, 0));
        region[r].push_back(e.idx);
        acc += e.value;
    }

    // Hardest region to the toughest champion.
    std::vector<int> rorder(region.size()), corder(n);
    for (size_t r = 0; r < region.size(); ++r) rorder[r] = static_cast<int>(r);
    std::vector<double> rhard(region.size(), 0);
    for (size_t r = 0; r < region.size(); ++r)
        for (int gi : region[r])
            for (const auto& e : ours)
                if (e.idx == gi) rhard[r] = std::max(rhard[r], e.hard);
    std::sort(rorder.begin(), rorder.end(), [&](int a, int b) { return rhard[a] > rhard[b]; });
    for (int s = 0; s < n; ++s) corder[s] = s;
    std::stable_sort(corder.begin(), corder.end(),
        [&](int a, int b) { return toughness(lineup[a]) > toughness(lineup[b]); });

    for (int k = 0; k < n; ++k) {
        const int slot = corder[k], r = rorder[k];
        g_mem[dm::kSlotDraft + slot] = static_cast<std::uint8_t>(lineup[slot]);
        for (int gi : region[r]) g_mem[dm::kGroupOwner + gi] = static_cast<std::uint8_t>(slot);
        // Home altar: lowest value-weighted mean distance to the region's groups.
        int best = 255;
        double bestd = 1e18;
        for (size_t a = 0; a < altars.size(); ++a) {
            double s = 0, w = 0;
            for (int gi : region[r]) {
                const double v = groups[gi].gem_value() + 1.0;
                s += v * std::min<double>(adist[a][gi], 400);
                w += v;
            }
            const double d = w > 0 ? s / w : 0;
            if (d < bestd) bestd = d, best = static_cast<int>(a);
        }
        g_mem[dm::kSlotAltar + slot] = static_cast<std::uint8_t>(best);
    }
}

}  // namespace

void play_draft() {
    const auto chosen = cambc::draft::current_draft(team::ally);
    if (!g_planned) {
        g_planned = true;
        const int n = std::clamp<int>(static_cast<int>(cambc::draft::team_size()), 1, 5);
        std::vector<champion_draft> lineup(kOrder.begin(), kOrder.begin() + n);
        plan(lineup);
    }
    cambc::draft_memory = std::span<const std::byte>(reinterpret_cast<const std::byte*>(g_mem.data()), g_mem.size());

    for (const auto c : kOrder)
        if (std::find(chosen.begin(), chosen.end(), c) == chosen.end()) {
            cambc::draft::pick(c);
            return;
        }
}
