#include <cambc.h>
#include "draft.h"
#include <algorithm>
#include <array>
#include <string_view>
#include <vector>

// Durable farmers first; Tavi (shops anywhere, stuns) is the altar guard.
void play_draft() {
    using cd = cambc::champion_draft;
    const auto chosen = cambc::draft::current_draft(cambc::team::ally);
    constexpr std::array<cd, 9> order{cd::vendetta, cd::marcella, cd::vesper_vale, cd::tavi, cd::varg,
        cd::ryth, cd::ren, cd::heart_of_ruin, cd::willow};
    cambc::draft_memory = std::string_view("AC1");
    for (auto pick : order)
        if (std::find(chosen.begin(), chosen.end(), pick) == chosen.end()) {
            cambc::draft::pick(pick);
            return;
        }
}
