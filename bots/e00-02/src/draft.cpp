#include <cambc.h>
#include "draft.h"
#include <algorithm>
#include <array>
#include <string_view>

void play_draft() {
    // Deathball kit: a tanky stunner to lead, a healer, splash and burst damage, then strength carries.
    using D = cambc::champion_draft;
    constexpr std::array preference{D::ryth, D::willow, D::varg, D::vendetta, D::vesper_vale,
        D::tavi, D::marcella, D::ren, D::stellar, D::cassi_and_issac, D::prism};
    const auto chosen = cambc::draft::current_draft(cambc::team::ally);
    cambc::draft_memory = std::string_view("DB");
    for (auto pick : preference) {
        if (std::find(chosen.begin(), chosen.end(), pick) == chosen.end()) {
            cambc::draft::pick(pick);
            return;
        }
    }
}
