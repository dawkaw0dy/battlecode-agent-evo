#include <cambc.h>
#include "draft.h"
#include <algorithm>
#include <array>

void play_draft() {
    // Stun and burst for ambushes, sturdy strength farmers for camps, Vesper Vale first for gold.
    using D = cambc::champion_draft;
    constexpr std::array preference{D::vesper_vale, D::ryth, D::tavi, D::vendetta, D::marcella, D::varg,
        D::willow, D::ren, D::cassi_and_issac, D::heart_of_ruin, D::stellar, D::prism};
    const auto chosen = cambc::draft::current_draft(cambc::team::ally);
    for (auto pick : preference) {
        if (std::find(chosen.begin(), chosen.end(), pick) == chosen.end()) {
            cambc::draft::pick(pick);
            return;
        }
    }
}
