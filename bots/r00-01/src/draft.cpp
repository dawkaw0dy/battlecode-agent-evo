#include <cambc.h>
#include "draft.h"
#include <algorithm>
#include <array>

void play_draft() {
    // Gold scalers first (Vesper's bonus gold, Marcella's gold per gem), then strong farmers.
    using D = cambc::champion_draft;
    constexpr std::array preference{D::vesper_vale, D::marcella, D::vendetta, D::varg, D::ryth,
        D::tavi, D::ren, D::willow, D::cassi_and_issac, D::stellar, D::prism};
    const auto chosen = cambc::draft::current_draft(cambc::team::ally);
    for (auto pick : preference) {
        if (std::find(chosen.begin(), chosen.end(), pick) == chosen.end()) {
            cambc::draft::pick(pick);
            return;
        }
    }
}
