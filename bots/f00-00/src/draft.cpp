#include <cambc.h>
#include "draft.h"
#include "plan.h"
#include <algorithm>
#include <array>
#include <vector>

using cambc::champion_draft;

void play_draft() {
    // The first draft turn has 10x fuel: read the map once and keep the plan.
    static Plan plan;
    static bool planned = false;
    if (!planned) {
        build_plan(plan);
        planned = true;
        cambc::draft_memory = std::span<const Plan>{&plan, 1};
    }

    // Champion order per plan: sturdy solo farmers when spread, tank + damage + healer when packed,
    // burst and stuns when the enemy is close.
    static constexpr std::array spread{champion_draft::vesper_vale, champion_draft::vendetta, champion_draft::tavi,
        champion_draft::marcella, champion_draft::ryth, champion_draft::willow, champion_draft::varg};
    static constexpr std::array pack{champion_draft::vesper_vale, champion_draft::ryth, champion_draft::vendetta,
        champion_draft::willow, champion_draft::varg, champion_draft::marcella, champion_draft::tavi};
    static constexpr std::array skirmish{champion_draft::vendetta, champion_draft::vesper_vale, champion_draft::ryth,
        champion_draft::tavi, champion_draft::willow, champion_draft::marcella, champion_draft::varg};

    const auto chosen = cambc::draft::current_draft(cambc::team::ally);
    const auto taken = [&](champion_draft c) { return std::find(chosen.begin(), chosen.end(), c) != chosen.end(); };
    auto try_list = [&](const auto& list) {
        for (auto c : list)
            if (!taken(c)) { cambc::draft::pick(c); return true; }
        return false;
    };
    bool done = false;
    if (plan.mode == mode_spread) done = try_list(spread);
    else if (plan.mode == mode_skirmish) done = try_list(skirmish);
    else done = try_list(pack);
    if (!done) {
        for (int i = 0; i < 12; ++i) {
            const auto c = static_cast<champion_draft>(i);
            if (!taken(c)) { cambc::draft::pick(c); return; }
        }
    }
}
