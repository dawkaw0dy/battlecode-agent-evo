#include <cambc.h>
#include "draft.h"
#include "plan.h"
#include <algorithm>
#include <array>

void play_draft() {
    static bool written = false;
    if (!written) {
        written = true;
        const Plan p = compute_plan();
        try {
            cambc::draft_memory = std::array<Plan, 1>{p};
        } catch (const cambc::error&) {}
    }
    // Sturdy farmers that can also punish champions who stop to clear a lock.
    using D = cambc::champion_draft;
    constexpr std::array preference{D::marcella, D::vesper_vale, D::vendetta, D::ryth, D::varg,
        D::tavi, D::willow, D::ren, D::cassi_and_issac, D::heart_of_ruin, D::stellar, D::prism};
    const auto chosen = cambc::draft::current_draft(cambc::team::ally);
    for (auto pick : preference) {
        if (std::find(chosen.begin(), chosen.end(), pick) == chosen.end()) {
            cambc::draft::pick(pick);
            return;
        }
    }
}
