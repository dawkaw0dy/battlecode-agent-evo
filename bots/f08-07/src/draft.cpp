#include <cambc.h>
#include "draft.h"
#include <algorithm>
#include <string_view>
#include <vector>
#include <iostream>

// Preference order: damage dealers first; the healer joins from four champions up.
const std::vector<cambc::champion_draft>& draft_order() {
    using D = cambc::champion_draft;
    static const std::vector<D> order{D::vesper_vale, D::vendetta, D::marcella, D::willow, D::tavi, D::varg, D::ryth,
        D::ren, D::stellar, D::prism, D::heart_of_ruin};
    return order;
}

void play_draft() {
    const auto chosen = cambc::draft::current_draft(cambc::team::ally);
    if (chosen.empty()) cambc::draft_memory = std::string_view("f0807");
    debug_block({
        if (chosen.empty()) {
            for (int i = 0; i <= static_cast<int>(cambc::champion_type::heart_of_ruin); ++i) {
                try {
                    cambc::champion c(static_cast<cambc::champion_type>(i));
                    const auto st = c.base_stats();
                    std::cout << "stats " << cambc::to_string(static_cast<cambc::champion_type>(i)) << " hp" << st.base_health << " str" << st.base_strength
                              << " foc" << st.base_focus << " campx" << st.camp_damage_multiplier << " range" << st.base_attack_range << "\n";
                } catch (const std::exception& e) { std::cout << "stats err " << e.what() << "\n"; }
            }
        }
    });
    for (auto pick : draft_order()) {
        if (std::find(chosen.begin(), chosen.end(), pick) != chosen.end()) continue;
        cambc::draft::pick(pick);
        return;
    }
}
