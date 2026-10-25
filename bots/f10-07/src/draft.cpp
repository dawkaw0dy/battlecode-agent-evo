#include <cambc.h>
#include "draft.h"
#include <algorithm>
#include <array>
#include <vector>

// Fast camp clearers: splash, high strength, gold makers, field shopping.
void play_draft() {
    using C = cambc::champion_draft;
    const auto chosen = cambc::draft::current_draft(cambc::team::ally);
    constexpr std::array order{C::varg, C::vendetta, C::vesper_vale, C::marcella, C::tavi,
                               C::ryth, C::ren, C::heart_of_ruin, C::willow, C::stellar, C::prism};
    for (auto pick : order) {
        if (std::find(chosen.begin(), chosen.end(), pick) != chosen.end()) continue;
        cambc::draft::pick(pick);
        return;
    }
}
