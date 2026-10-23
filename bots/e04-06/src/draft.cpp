#include <cambc.h>
#include "draft.h"
#include <algorithm>
#include <array>

void play_draft() {
    // Vesper Vale first for gold (33% bonus), then a spell team: stuns that chain (Ryth's area stun, Tavi's stun bolt), Marcella's ranged burst and
    // stun-immunity, Willow's heals to keep the pack on the map, then the strongest strength farmers.
    using D = cambc::champion_draft;
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
