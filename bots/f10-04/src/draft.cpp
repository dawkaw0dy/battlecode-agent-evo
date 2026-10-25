#include <cambc.h>
#include "draft.h"
#include "plan.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <vector>

// Make the map plan and the home regions once, write both to draft memory, then pick the plan's next champion.
void play_draft() {
    const int size = std::clamp(static_cast<int>(cambc::draft::team_size()), 1, 5);
    static bool written = false;
    if (!written) {
        written = true;
        const auto groups = analyse_map();
        set_plan(make_plan(groups, size));
        const auto& plan = map_plan();
        const auto home = assign_homes(groups, static_cast<int>(plan.picks.size()));
        std::array<std::byte, 1024> mem{};
        mem[dm::magic] = std::byte{'E'};
        mem[dm::size] = static_cast<std::byte>(plan.picks.size());
        const size_t n = std::min<size_t>(groups.size(), 250);
        mem[dm::ngroups] = static_cast<std::byte>(n);
        for (size_t i = 0; i < n; ++i) mem[dm::home + i] = static_cast<std::byte>(home[i]);
        write_plan(std::span<std::byte>{mem.data(), mem.size()}, plan);
        cambc::draft_memory = std::span<const std::byte>{mem.data(), mem.size()};
    }
    const auto picks = pick_order(size);
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
