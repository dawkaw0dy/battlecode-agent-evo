#include <cambc.h>
#include "draft.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

using cambc::champion_draft;

// Static clear cost of one camp: health a strength team must chew through.
static float camp_cost(cambc::camp_type t) {
    switch (t) {
    case cambc::camp_type::wolf: return 1350.f;
    case cambc::camp_type::elk: return 2400.f;
    case cambc::camp_type::monolith: return 20000.f;
    case cambc::camp_type::golem: return 5000.f;
    default: return 120.f;
    }
}

void play_draft() {
    // Plan the sweep: rate every group by gems per unit of clear work and store it in draft memory.
    // Layout: byte 0 'S', byte 1 group count (capped), bytes 2.. one efficiency byte per group.
    const auto groups = cambc::map::camp_groups();
    std::array<std::uint8_t, 1024> plan{};
    plan[0] = 'S';
    const std::size_t n = std::min<std::size_t>(groups.size(), 1000);
    plan[1] = static_cast<std::uint8_t>(std::min<std::size_t>(n, 255));
    int multi = 0;
    for (std::size_t i = 0; i < n; ++i) {
        float hp = 0.f;
        int fighters = 0;
        for (const auto& c : groups[i].camps()) {
            hp += camp_cost(c.kind());
            if (c.kind() != cambc::camp_type::gem_cluster && c.kind() != cambc::camp_type::gold_cluster) ++fighters;
        }
        if (fighters >= 2) ++multi;
        const float value = groups[i].gem_value() + 0.05f * groups[i].gold_value();
        const float eff = value / (hp / 100.f + 4.f); // gems per ~turn of a 100-dps team
        plan[2 + i] = static_cast<std::uint8_t>(std::clamp(eff * 20.f, 1.f, 255.f));
    }
    cambc::draft_memory = std::string_view(reinterpret_cast<const char*>(plan.data()), plan.size());

    // Strong basic attackers clear camps fastest; Varg's splash rises when groups hold several fighters.
    std::vector<champion_draft> order{champion_draft::vendetta, champion_draft::vesper_vale, champion_draft::varg,
        champion_draft::ryth, champion_draft::marcella, champion_draft::tavi, champion_draft::willow,
        champion_draft::cassi_and_issac, champion_draft::ren, champion_draft::heart_of_ruin,
        champion_draft::stellar, champion_draft::prism};
    if (multi >= 3) std::rotate(order.begin(), order.begin() + 2, order.begin() + 3); // Varg first

    const auto chosen = cambc::draft::current_draft(cambc::team::ally);
    for (auto pick : order) {
        if (std::find(chosen.begin(), chosen.end(), pick) == chosen.end()) {
            cambc::draft::pick(pick);
            return;
        }
    }
}
