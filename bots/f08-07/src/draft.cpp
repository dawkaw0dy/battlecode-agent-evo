#include <cambc.h>
#include "draft.h"
#include <algorithm>
#include <string_view>
#include <vector>

// Preference order; the first three form the main squad (tank, healer, ranged), the rest a second squad.
const std::vector<cambc::champion_draft>& draft_order() {
    using D = cambc::champion_draft;
    static const std::vector<D> order{D::marcella, D::willow, D::vesper_vale, D::vendetta, D::tavi, D::varg, D::ryth,
        D::ren, D::stellar, D::prism, D::heart_of_ruin};
    return order;
}

void play_draft() {
    const auto chosen = cambc::draft::current_draft(cambc::team::ally);
    if (chosen.empty()) cambc::draft_memory = std::string_view("f0807");
    for (auto pick : draft_order()) {
        if (std::find(chosen.begin(), chosen.end(), pick) != chosen.end()) continue;
        cambc::draft::pick(pick);
        return;
    }
}
