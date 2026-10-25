#include <cambc.h>
#include "draft.h"
#include <algorithm>
#include <array>
#include <vector>

// Tanky farmers first: health and sustain matter more than burst for a team that never fights even.
// Tavi shops anywhere, so it never walks home to spend gold.
void play_draft() {
    using cd = cambc::champion_draft;
    constexpr std::array order{cd::vendetta, cd::marcella, cd::vesper_vale, cd::varg, cd::tavi,
        cd::ryth, cd::ren, cd::heart_of_ruin, cd::willow, cd::stellar, cd::prism};
    const auto chosen = cambc::draft::current_draft(cambc::team::ally);
    for (auto p : order)
        if (std::find(chosen.begin(), chosen.end(), p) == chosen.end()) {
            cambc::draft::pick(p);
            return;
        }
}
