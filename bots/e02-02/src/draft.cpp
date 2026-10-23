// Map-aware draft. The gameplay bot fights with basic attacks, simple damage/stun spells, shields and
// heals (no dashes, no attack resets), so champions are rated on what that bot actually uses, then
// adjusted for the map: big camp groups favour splash and area spells, small teams favour the extra
// body of Cassi & Issac, strong camps favour health. Picks are made one by one, best rating first.
#include <cambc.h>
#include "draft.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>

namespace {

using D = cambc::champion_draft;

struct MapFeatures {
    int team_size = 4;
    int monsters = 0;     // fightable monsters (no clusters, no monoliths)
    int big_groups = 0;   // groups with 3+ fightable monsters
    int heavy = 0;        // elks and golems
    int groups = 0;
};

MapFeatures read_map() {
    MapFeatures f;
    try { f.team_size = static_cast<int>(cambc::draft::team_size()); } catch (const cambc::error&) {}
    try {
        for (const auto& group : cambc::map::camp_groups()) {
            int fighters = 0;
            bool monolith = false;
            for (const auto& camp : group.camps()) {
                using C = cambc::camp_type;
                const auto kind = camp.kind();
                if (kind == C::monolith) monolith = true;
                if (kind == C::wolf || kind == C::elk || kind == C::golem) ++fighters;
                if (kind == C::elk || kind == C::golem) ++f.heavy;
            }
            if (monolith) continue;
            ++f.groups;
            f.monsters += fighters;
            if (fighters >= 3) ++f.big_groups;
        }
    } catch (const cambc::error&) {}
    return f;
}

float rating(D pick, const MapFeatures& f) {
    const float big_share = f.groups > 0 ? static_cast<float>(f.big_groups) / f.groups : 0.0f;
    const float heavy_share = f.monsters > 0 ? static_cast<float>(f.heavy) / f.monsters : 0.0f;
    switch (pick) {
    case D::vendetta: return 10.0f + 2.0f * heavy_share;           // 70 Strength melee, 1000 health
    case D::vesper_vale: return 9.6f;                              // 33% gold, 55 Strength ranged
    case D::marcella: return 9.0f + 2.0f * heavy_share;            // 1200 health, gold per tributed gem
    case D::ryth: return 8.4f + 3.0f * big_share + heavy_share;    // 1200 health, area stun
    case D::varg: return 7.2f + 4.0f * big_share;                  // splash basic attacks, shield
    case D::willow: return 9.4f;                                   // ally heal keeps the team farming, 55 Strength
    case D::tavi: return 7.5f;                                     // stun spell, 900 health
    case D::cassi_and_issac: return f.team_size <= 3 ? 9.3f : 7.0f; // two bodies for one pick
    case D::ren: return 5.0f;
    case D::stellar: return 4.0f;
    case D::heart_of_ruin: return 3.0f;                            // relies on its dash
    case D::prism: return 2.0f;
    }
    return 0.0f;
}

} // namespace

void play_draft() {
    static const MapFeatures features = read_map();
    constexpr std::array all{D::ryth, D::vesper_vale, D::vendetta, D::cassi_and_issac, D::stellar, D::prism,
        D::marcella, D::willow, D::tavi, D::ren, D::varg, D::heart_of_ruin};
    const auto chosen = cambc::draft::current_draft(cambc::team::ally);
    D best = D::vendetta;
    float best_score = -1.0f;
    for (auto pick : all) {
        if (std::find(chosen.begin(), chosen.end(), pick) != chosen.end()) continue;
        const float score = rating(pick, features);
        if (score > best_score) { best_score = score; best = pick; }
    }
    if (best_score >= 0.0f) cambc::draft::pick(best);
}
