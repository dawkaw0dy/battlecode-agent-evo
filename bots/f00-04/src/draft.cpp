#include <cambc.h>
#include "draft.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

using cambc::champion_draft;

// Big-game hunters: tanky melee front line that soaks camp hits, a healer to sustain long fights,
// and steady damage. Order is the pick order; the first `team_size` free entries are taken.
void play_draft() {
    const auto chosen = cambc::draft::current_draft(cambc::team::ally);
    const std::array<champion_draft, 8> order{
        champion_draft::ryth, champion_draft::vendetta, champion_draft::willow, champion_draft::varg,
        champion_draft::vesper_vale, champion_draft::marcella, champion_draft::tavi, champion_draft::ren};
    champion_draft pick = order[0];
    for (auto c : order) {
        if (std::find(chosen.begin(), chosen.end(), c) == chosen.end()) {
            pick = c;
            break;
        }
    }
    const std::array<std::uint8_t, 1> memo{static_cast<std::uint8_t>(cambc::draft::team_size())};
    cambc::draft_memory = memo;
    cambc::draft::pick(pick);
}
