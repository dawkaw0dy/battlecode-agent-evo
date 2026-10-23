#include <cambc.h>
#include "draft.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <vector>

namespace {

using D = cambc::champion_draft;

bool has(const std::vector<D>& picks, D pick) {
    return std::find(picks.begin(), picks.end(), pick) != picks.end();
}

int camp_strength(cambc::camp_type kind) {
    using C = cambc::camp_type;
    switch (kind) {
    case C::wolf: return 20;
    case C::elk: return 60;
    case C::golem: return 120;
    case C::monolith: return 200;
    default: return 0;
    }
}

// Opening plan from the whole map: per camp group, how much closer it is to our spawn than to the enemy's.
// Layout: [0] magic, [1] group count, [2] opening group + 1, [3..] signed side margin per group.
void write_plan() {
    using cambc::team;
    const auto blocked = cambc::map::walls() | cambc::map::holes() | cambc::map::altars();
    cambc::distance_field ours, theirs;
    cambc::bitboard reached_ours, reached_theirs;
    ours.compute(blocked | cambc::map::spawn_region(team::enemy), cambc::map::spawn_region(team::ally), reached_ours);
    theirs.compute(blocked | cambc::map::spawn_region(team::ally), cambc::map::spawn_region(team::enemy), reached_theirs);

    const auto& list = cambc::map::camp_groups();
    std::array<std::uint8_t, 1024> plan{};
    const std::size_t count = std::min<std::size_t>(list.size(), plan.size() - 3);
    plan[0] = kPlanMagic;
    plan[1] = static_cast<std::uint8_t>(count);
    int opening = -1;
    float opening_score = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const auto& group = list[i];
        constexpr int kFar = 1 << 12;
        int mine = kFar, enemy = kFar;
        for (auto tile : group.spawn_tiles().positions()) {
            if (reached_ours.is_set(tile)) mine = std::min<int>(mine, ours.get(tile));
            if (reached_theirs.is_set(tile)) enemy = std::min<int>(enemy, theirs.get(tile));
        }
        const int margin = std::clamp(enemy - mine, -127, 127);
        plan[3 + i] = static_cast<std::uint8_t>(static_cast<std::int8_t>(margin));

        // Opening group: richest cheap group the enemy pack cannot reach first.
        int strength = 0;
        bool monolith = false;
        for (const auto& camp : group.camps()) {
            strength += camp_strength(camp.kind());
            if (camp.kind() == cambc::camp_type::monolith) monolith = true;
        }
        if (monolith || mine >= kFar || margin < kOpeningMargin) continue;
        const float value = group.gem_value() * 3.0f + group.gold_value();
        const float score = value / (static_cast<float>(mine) + 8.0f + strength * 0.1f);
        if (opening < 0 || score > opening_score) { opening = static_cast<int>(i); opening_score = score; }
    }
    plan[2] = static_cast<std::uint8_t>(opening + 1);
    cambc::draft_memory = plan;
}

} // namespace

void play_draft() {
    static bool planned = false;
    if (!planned) {
        planned = true;
        try {
            write_plan();
        } catch (const cambc::error&) {
        }
    }

    // Vesper Vale for gold, Ryth's area stun, then Marcella (stun immune, ranged burst, gold per tributed gem)
    // and Willow's heals ahead of Tavi: the pack survives the first contested fights instead of trading stuns.
    constexpr std::array preference{D::vesper_vale, D::ryth, D::marcella, D::willow, D::tavi, D::vendetta,
        D::varg, D::ren, D::cassi_and_issac, D::stellar, D::prism, D::heart_of_ruin};
    const auto chosen = cambc::draft::current_draft(cambc::team::ally);
    for (auto pick : preference) {
        if (!has(chosen, pick)) {
            cambc::draft::pick(pick);
            return;
        }
    }
}
