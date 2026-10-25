#include <cambc.h>
#include "draft.h"
#include "plan.h"
#include <algorithm>
#include <vector>

// Write the plan once (the first draft turn has 10x fuel), then pick the next champion of our order.
void play_draft() {
    const int size = static_cast<int>(cambc::draft::team_size());
    const auto picks = pick_order(size);
    static bool written = false;
    if (!written) {
        written = true;
        write_plan(analyse_map(), static_cast<int>(picks.size()));
    }
    const auto chosen = cambc::draft::current_draft(team::ally);
    for (auto p : picks)
        if (std::find(chosen.begin(), chosen.end(), p) == chosen.end()) {
            cambc::draft::pick(p);
            return;
        }
    for (int i = 0; i <= static_cast<int>(cambc::champion_draft::heart_of_ruin); ++i) {
        const auto p = static_cast<cambc::champion_draft>(i);
        if (p == cambc::champion_draft::cassi_and_issac) continue;
        if (std::find(chosen.begin(), chosen.end(), p) == chosen.end()) {
            cambc::draft::pick(p);
            return;
        }
    }
}
