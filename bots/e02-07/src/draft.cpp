#include <cambc.h>
#include "draft.h"
#include <algorithm>
#include <array>

void play_draft() {
    // Big-game hunters: durable melee with shields and resets, a healer for sustain, then burst.
    using D = cambc::champion_draft;
    constexpr std::array preference{D::ryth, D::willow, D::varg, D::marcella, D::vendetta,
        D::heart_of_ruin, D::vesper_vale, D::ren, D::tavi, D::cassi_and_issac, D::stellar, D::prism};
    const auto chosen = cambc::draft::current_draft(cambc::team::ally);
    for (auto pick : preference) {
        if (std::find(chosen.begin(), chosen.end(), pick) == chosen.end()) {
            cambc::draft::pick(pick);
            return;
        }
    }
}
