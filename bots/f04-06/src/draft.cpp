#include <cambc.h>
#include "draft.h"
#include <algorithm>
#include <array>

// Early-power brawlers: high base stats, dashes, stuns and attack resets.
void play_draft() {
    using cd = cambc::champion_draft;
    static const std::array order{cd::vendetta, cd::tavi, cd::vesper_vale, cd::marcella, cd::varg,
        cd::heart_of_ruin, cd::ryth, cd::ren, cd::willow, cd::stellar, cd::prism, cd::cassi_and_issac};
    const auto chosen = cambc::draft::current_draft(cambc::team::ally);
    for (auto pick : order)
        if (std::find(chosen.begin(), chosen.end(), pick) == chosen.end()) {
            cambc::draft::pick(pick);
            return;
        }
}
