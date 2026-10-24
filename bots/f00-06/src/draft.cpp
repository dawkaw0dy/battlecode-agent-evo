#include <cambc.h>
#include "draft.h"
#include <algorithm>
#include <array>
#include <span>
#include <vector>

// Deathball draft. Large teams: two bodies from one pick, a healer, then damage that hits clustered
// camps. Three-champion teams cannot afford a linked pair: sturdy stunners with ranged damage.
void play_draft() {
    using cambc::champion_draft;
    static constexpr std::array big{
        champion_draft::cassi_and_issac, champion_draft::willow, champion_draft::vendetta,
        champion_draft::varg, champion_draft::vesper_vale, champion_draft::ryth,
        champion_draft::tavi, champion_draft::marcella, champion_draft::heart_of_ruin,
        champion_draft::ren, champion_draft::stellar, champion_draft::prism};
    static constexpr std::array small{
        champion_draft::vesper_vale, champion_draft::ryth, champion_draft::tavi,
        champion_draft::willow, champion_draft::vendetta, champion_draft::varg,
        champion_draft::marcella, champion_draft::heart_of_ruin, champion_draft::ren,
        champion_draft::cassi_and_issac, champion_draft::stellar, champion_draft::prism};
    std::uint32_t size = 5;
    try {
        size = cambc::draft::team_size();
    } catch (...) {
    }
    const std::span<const champion_draft> order = size <= 3 ? std::span<const champion_draft>(small)
                                                            : std::span<const champion_draft>(big);
    const auto chosen = cambc::draft::current_draft(cambc::team::ally);
    for (auto pick : order) {
        if (std::find(chosen.begin(), chosen.end(), pick) == chosen.end()) {
            cambc::draft::pick(pick);
            return;
        }
    }
}
