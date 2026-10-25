#include <cambc.h>
#include "draft.h"
#include <algorithm>
#include <array>
#include <vector>

// Early-power picks first: high base damage, stuns, dashes and attack resets.
void play_draft() {
    using cambc::champion_draft;
    static constexpr std::array order{
        champion_draft::vendetta, champion_draft::marcella, champion_draft::ryth,
        champion_draft::tavi, champion_draft::vesper_vale, champion_draft::varg,
        champion_draft::heart_of_ruin, champion_draft::willow, champion_draft::ren,
        champion_draft::cassi_and_issac, champion_draft::stellar, champion_draft::prism};
    const auto chosen = cambc::draft::current_draft(cambc::team::ally);
    for (const auto pick : order) {
        if (std::find(chosen.begin(), chosen.end(), pick) == chosen.end()) {
            cambc::draft::pick(pick);
            return;
        }
    }
}
