#include <cambc.h>
#include "draft.h"
#include <algorithm>
#include <array>

void play_draft() {
    // A spell pack: a kill-resetting farmer, the only healer, then stun and nuke kits.
    using D = cambc::champion_draft;
    constexpr std::array preference{D::vendetta, D::willow, D::marcella, D::tavi, D::vesper_vale,
        D::ryth, D::varg, D::ren, D::heart_of_ruin, D::cassi_and_issac, D::stellar, D::prism};
    const auto chosen = cambc::draft::current_draft(cambc::team::ally);
    for (auto pick : preference) {
        if (std::find(chosen.begin(), chosen.end(), pick) == chosen.end()) {
            cambc::draft::pick(pick);
            return;
        }
    }
}
