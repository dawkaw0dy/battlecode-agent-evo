#include <cambc.h>
#include "draft.h"
#include <algorithm>
#include <array>
#include <vector>

// Camp sweepers: strong basic attacks with resets, splash for grouped monsters.
void play_draft() {
    using cd = cambc::champion_draft;
    static const std::array<cd, 8> order{cd::vendetta, cd::varg, cd::vesper_vale, cd::marcella, cd::tavi,
        cd::ryth, cd::heart_of_ruin, cd::ren};
    const auto chosen = cambc::draft::current_draft(cambc::team::ally);
    for (auto p : order)
        if (std::find(chosen.begin(), chosen.end(), p) == chosen.end()) {
            cambc::draft::pick(p);
            return;
        }
    for (int i = 0; i <= static_cast<int>(cd::heart_of_ruin); ++i) {
        const auto p = static_cast<cd>(i);
        if (p == cd::cassi_and_issac) continue;
        if (std::find(chosen.begin(), chosen.end(), p) == chosen.end()) {
            cambc::draft::pick(p);
            return;
        }
    }
}
