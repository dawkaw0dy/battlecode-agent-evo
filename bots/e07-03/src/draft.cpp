#include <cambc.h>
#include "draft.h"
#include "plan.h"
#include <algorithm>
#include <array>

void play_draft() {
    // First draft turn (10x fuel): read the map and store the plan for the champions.
    static bool planned = false;
    if (!planned) {
        planned = true;
        try {
            write_plan();
        } catch (const cambc::error&) {
        }
    }
    // Large or medium maps with far altars: sturdy farmers (e04-04's draft) survive the long walks and
    // punish champions who stop at altars; small maps: the spell pack below.
    using D = cambc::champion_draft;
    if (plan().draft_sturdy) {
        constexpr std::array sturdy{D::marcella, D::vesper_vale, D::vendetta, D::ryth, D::varg,
            D::tavi, D::willow, D::ren, D::cassi_and_issac, D::heart_of_ruin, D::stellar, D::prism};
        const auto chosen = cambc::draft::current_draft(cambc::team::ally);
        for (auto pick : sturdy)
            if (std::find(chosen.begin(), chosen.end(), pick) == chosen.end()) {
                cambc::draft::pick(pick);
                return;
            }
    }
    // Vesper Vale first for gold (33% bonus), then a spell team: stuns that chain (Ryth's area stun, Tavi's stun bolt), Marcella's ranged burst and
    // stun-immunity, Willow's heals to keep the pack on the map, then the strongest strength farmers.
    constexpr std::array preference{D::vesper_vale, D::ryth, D::tavi, D::marcella, D::willow, D::vendetta,
        D::varg, D::ren, D::cassi_and_issac, D::stellar, D::prism, D::heart_of_ruin};
    const auto chosen = cambc::draft::current_draft(cambc::team::ally);
    for (auto pick : preference) {
        if (std::find(chosen.begin(), chosen.end(), pick) == chosen.end()) {
            cambc::draft::pick(pick);
            return;
        }
    }
}
