#include <cambc.h>
#include "draft.h"
#include "plan.h"
#include <algorithm>
#include <vector>

// Read the map once, decide the plan (raid squad, regions) and record it, then pick the next champion.
void play_draft() {
    const int size = static_cast<int>(cambc::draft::team_size());
    static std::vector<GroupData> groups;
    static Plan plan;
    static bool planned = false;
    if (!planned) {
        planned = true;
        groups = analyse_groups();
        plan = make_plan(groups, size);
        write_plan(plan, static_cast<int>(groups.size()));
    }
    const auto chosen = cambc::draft::current_draft(cambc::team::ally);
    for (auto pick : plan.picks) {
        if (std::find(chosen.begin(), chosen.end(), pick) != chosen.end()) continue;
        cambc::draft::pick(pick);
        return;
    }
    // Fallback: anything not yet taken by us.
    for (int i = 0; i <= static_cast<int>(cambc::champion_draft::heart_of_ruin); ++i) {
        auto pick = static_cast<cambc::champion_draft>(i);
        if (pick == cambc::champion_draft::cassi_and_issac) continue;
        if (std::find(chosen.begin(), chosen.end(), pick) == chosen.end()) {
            cambc::draft::pick(pick);
            return;
        }
    }
}
