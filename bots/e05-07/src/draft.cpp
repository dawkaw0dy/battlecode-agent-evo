#include <cambc.h>
#include "draft.h"
#include <algorithm>
#include <array>

constexpr unsigned kSmallTeam = 4;

void play_draft() {
    using D = cambc::champion_draft;
    // 3-4 picks: e04-04's sturdy farmers (Marcella, Vesper Vale, Vendetta) clear small maps fastest.
    constexpr std::array small_team{D::marcella, D::vesper_vale, D::vendetta, D::ryth, D::varg,
        D::tavi, D::willow, D::ren, D::cassi_and_issac, D::heart_of_ruin, D::stellar, D::prism};
    // 5 picks: e04-06's pack. Vesper Vale first for gold (33% bonus), stunners (Ryth's area stun, Tavi's stun
    // bolt), Marcella's stun immunity and bulk, Willow's heals to keep the pack on the map.
    constexpr std::array large_team{D::vesper_vale, D::ryth, D::tavi, D::marcella, D::willow, D::vendetta,
        D::varg, D::ren, D::cassi_and_issac, D::stellar, D::prism, D::heart_of_ruin};
    const auto& preference = cambc::draft::team_size() <= kSmallTeam ? small_team : large_team;
    const auto chosen = cambc::draft::current_draft(cambc::team::ally);
    for (auto pick : preference) {
        if (std::find(chosen.begin(), chosen.end(), pick) == chosen.end()) {
            cambc::draft::pick(pick);
            // Draft memory byte 0: champion_type + 1 of the altar warden. Marcella keeps the job whatever
            // the pick order, so Vesper Vale's gold bonus stays on farming.
            if (pick == D::marcella) {
                cambc::draft_memory_buffer memory;
                const auto warden = static_cast<std::byte>(static_cast<int>(cambc::champion_type::marcella) + 1);
                memory = std::array{warden};
            }
            return;
        }
    }
}
