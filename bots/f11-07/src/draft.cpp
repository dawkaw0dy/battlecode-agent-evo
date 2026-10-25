#include <cambc.h>
#include "draft.h"
#include <algorithm>
#include <array>
#include <vector>

// Scaling economy: a gold-boosted ranged carry, strength scalers with attack resets, and a splash
// fighter for grouped camps. Picks fall back down the list if the engine rejects nothing (picks
// are simultaneous, so the opponent taking a champion does not block us).
void play_draft() {
    const std::array order{cambc::champion_draft::vesper_vale, cambc::champion_draft::vendetta,
        cambc::champion_draft::marcella, cambc::champion_draft::varg, cambc::champion_draft::tavi,
        cambc::champion_draft::ryth, cambc::champion_draft::ren};
    const auto chosen = cambc::draft::current_draft(cambc::team::ally);
    for (auto pick : order) {
        if (std::find(chosen.begin(), chosen.end(), pick) == chosen.end()) {
            cambc::draft::pick(pick);
            return;
        }
    }
}
