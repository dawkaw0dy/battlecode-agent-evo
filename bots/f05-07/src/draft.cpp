#include <cambc.h>
#include "draft.h"
#include <algorithm>
#include <array>
#include <vector>

// Line-up: two ranged gold/damage dealers and three sturdy melee with resets, stuns and a shield.
// The order doubles as the champion's slot index in shared memory (see bot.cpp).
const std::array<cambc::champion_draft, 5> kPicks{
    cambc::champion_draft::vesper_vale, cambc::champion_draft::vendetta, cambc::champion_draft::marcella,
    cambc::champion_draft::tavi, cambc::champion_draft::varg};

void play_draft() {
    const auto chosen = cambc::draft::current_draft(cambc::team::ally);
    for (auto pick : kPicks)
        if (std::find(chosen.begin(), chosen.end(), pick) == chosen.end()) {
            cambc::draft::pick(pick);
            return;
        }
    // Fallback: any champion we have not picked yet (never Cassi & Issac: two bodies, one slot).
    for (int i = 0; i < 12; ++i) {
        auto pick = static_cast<cambc::champion_draft>(i);
        if (pick == cambc::champion_draft::cassi_and_issac) continue;
        if (std::find(chosen.begin(), chosen.end(), pick) == chosen.end()) {
            cambc::draft::pick(pick);
            return;
        }
    }
}
