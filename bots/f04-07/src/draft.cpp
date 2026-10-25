#include <cambc.h>
#include "draft.h"
#include <algorithm>
#include <array>
#include <string_view>
#include <vector>

// Fixed preference: sturdy burst fighters with resets, dashes and stuns for picking off isolated champions.
void play_draft() {
    const auto chosen = cambc::draft::current_draft(cambc::team::ally);
    constexpr std::array order{
        cambc::champion_draft::vendetta, cambc::champion_draft::vesper_vale, cambc::champion_draft::varg,
        cambc::champion_draft::tavi, cambc::champion_draft::ryth, cambc::champion_draft::marcella,
        cambc::champion_draft::ren, cambc::champion_draft::heart_of_ruin, cambc::champion_draft::willow,
        cambc::champion_draft::stellar, cambc::champion_draft::prism};
    cambc::draft_memory = std::string_view("va");
    for (auto pick : order)
        if (std::find(chosen.begin(), chosen.end(), pick) == chosen.end()) {
            cambc::draft::pick(pick);
            return;
        }
}
