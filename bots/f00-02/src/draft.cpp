#include <cambc.h>
#include "draft.h"
#include <algorithm>
#include <array>
#include <string_view>
#include <vector>

// Safe banker draft: durable champions with strong basic attacks farm camps fastest
// and survive mistakes; Willow's heal keeps the team on the map between recalls.
void play_draft() {
    using cambc::champion_draft;
    const auto chosen = cambc::draft::current_draft(cambc::team::ally);
    constexpr std::array preference{
        champion_draft::vendetta, champion_draft::vesper_vale, champion_draft::marcella,
        champion_draft::willow, champion_draft::ryth, champion_draft::varg,
        champion_draft::tavi, champion_draft::cassi_and_issac, champion_draft::ren,
        champion_draft::heart_of_ruin, champion_draft::stellar, champion_draft::prism,
    };
    cambc::draft_memory = std::string_view("safe-banker");
    for (auto pick : preference) {
        if (std::find(chosen.begin(), chosen.end(), pick) == chosen.end()) {
            cambc::draft::pick(pick);
            return;
        }
    }
}
