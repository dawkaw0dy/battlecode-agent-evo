#include <cambc.h>
#include "draft.h"
#include "common.h"
#include <algorithm>

// Pick the next champion of our fixed order; fall back to any unpicked one.
void play_draft() {
    const auto chosen = cambc::draft::current_draft(team::ally);
    for (auto p : pick_order())
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
