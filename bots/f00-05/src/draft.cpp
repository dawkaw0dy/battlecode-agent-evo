#include <cambc.h>
#include "draft.h"
#include <algorithm>
#include <array>
#include <vector>

// Early-aggression draft: sturdy melee with resets, dashes and stuns first, then strong ranged attackers.
void play_draft() {
    using cambc::champion_draft;
    static constexpr std::array order{
        champion_draft::vendetta,      // 70 strength, reset + dash, kills refresh cooldowns
        champion_draft::ryth,          // 1200 health, area stun, reset
        champion_draft::marcella,      // 1200 health, stun immune, stunning enhanced attack
        champion_draft::vesper_vale,   // 55 strength ranged, reset + dash, bonus gold
        champion_draft::tavi,          // ranged stun, reset
        champion_draft::heart_of_ruin, // stunning dash
        champion_draft::varg,
        champion_draft::ren,
        champion_draft::willow,
        champion_draft::stellar,
        champion_draft::prism,
        champion_draft::cassi_and_issac,
    };
    const auto chosen = cambc::draft::current_draft(cambc::team::ally);
    for (auto pick : order) {
        if (std::find(chosen.begin(), chosen.end(), pick) == chosen.end()) {
            cambc::draft::pick(pick);
            return;
        }
    }
}
