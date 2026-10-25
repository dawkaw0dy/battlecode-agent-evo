#include <cambc.h>
#include "draft.h"
#include <algorithm>
#include <array>
#include <string_view>

void play_draft() {
    // Durable farmers first: high health, strong basic attacks, gold from tributes.
    constexpr std::array order{cambc::champion_draft::marcella, cambc::champion_draft::vendetta,
        cambc::champion_draft::vesper_vale, cambc::champion_draft::varg, cambc::champion_draft::ryth,
        cambc::champion_draft::tavi, cambc::champion_draft::ren, cambc::champion_draft::heart_of_ruin};
    const auto chosen = cambc::draft::current_draft(cambc::team::ally);
    cambc::draft_memory = std::string_view("safe-banker");
    for (auto pick : order)
        if (std::find(chosen.begin(), chosen.end(), pick) == chosen.end()) {
            cambc::draft::pick(pick);
            return;
        }
}
