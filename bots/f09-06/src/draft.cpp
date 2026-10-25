#include <cambc.h>
#include "draft.h"
#include <algorithm>

using cambc::champion_draft;

std::vector<champion_draft> draft_order() {
    return {champion_draft::varg, champion_draft::vendetta, champion_draft::marcella,
        champion_draft::vesper_vale, champion_draft::tavi, champion_draft::ryth, champion_draft::ren};
}

bool draft_type_matches(champion_draft pick, cambc::champion_type type) {
    using T = cambc::champion_type;
    switch (pick) {
    case champion_draft::ryth: return type == T::ryth;
    case champion_draft::vesper_vale: return type == T::vesper_vale;
    case champion_draft::vendetta: return type == T::vendetta;
    case champion_draft::cassi_and_issac: return type == T::cassi || type == T::issac;
    case champion_draft::stellar: return type == T::stellar;
    case champion_draft::prism: return type == T::prism;
    case champion_draft::marcella: return type == T::marcella;
    case champion_draft::willow: return type == T::willow;
    case champion_draft::ren: return type == T::ren;
    case champion_draft::varg: return type == T::varg;
    case champion_draft::tavi: return type == T::tavi;
    case champion_draft::heart_of_ruin: return type == T::heart_of_ruin;
    }
    return false;
}

void play_draft() {
    const auto chosen = cambc::draft::current_draft(cambc::team::ally);
    for (auto pick : draft_order())
        if (std::find(chosen.begin(), chosen.end(), pick) == chosen.end()) {
            cambc::draft::pick(pick);
            return;
        }
}
