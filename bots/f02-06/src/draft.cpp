#include <cambc.h>
#include "draft.h"
#include <algorithm>
#include <vector>

// Big-game hunters: sturdy melee with stuns for the boss camps, one ranged gold maker.
void play_draft() {
    using cd = cambc::champion_draft;
    const std::vector<cd> order{cd::vendetta, cd::ryth, cd::marcella, cd::varg, cd::vesper_vale,
        cd::tavi, cd::willow, cd::ren, cd::stellar, cd::prism, cd::heart_of_ruin};
    const auto chosen = cambc::draft::current_draft(cambc::team::ally);
    for (auto p : order)
        if (std::find(chosen.begin(), chosen.end(), p) == chosen.end()) {
            cambc::draft::pick(p);
            return;
        }
}
