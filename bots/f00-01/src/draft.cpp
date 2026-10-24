#include <cambc.h>
#include "draft.h"
#include <algorithm>
#include <array>

void play_draft() {
    // Sturdy strength champions first: they survive at contested altars and punish
    // champions that stop to clear our locks. Vendetta is the preferred warden.
    using D = cambc::champion_draft;
    constexpr std::array preference{D::vesper_vale, D::marcella, D::vendetta, D::ryth, D::varg,
        D::tavi, D::willow, D::ren, D::cassi_and_issac, D::heart_of_ruin, D::stellar, D::prism};
    const auto chosen = cambc::draft::current_draft(cambc::team::ally);
    for (auto pick : preference) {
        if (std::find(chosen.begin(), chosen.end(), pick) == chosen.end()) {
            cambc::draft::pick(pick);
            return;
        }
    }
}
