#include <cambc.h>
#include "draft.h"
#include "plan.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <vector>

// Write the home-region plan once, then pick the next champion of our fixed order.
void play_draft() {
    const int size = static_cast<int>(cambc::draft::team_size());
    const auto picks = pick_order(size);
    static bool written = false;
    if (!written) {
        written = true;
        int home_altar = kFar;
        const auto groups = analyse_map(&home_altar);
        const int n = static_cast<int>(picks.size());
        const auto home = assign_homes(groups, region_count(n), region_weights(n));
        std::array<std::byte, 1024> mem{};
        mem[dm::magic] = std::byte{'E'};
        mem[dm::size] = static_cast<std::byte>(picks.size());
        const size_t ng = std::min<size_t>(groups.size(), 250);
        mem[dm::ngroups] = static_cast<std::byte>(ng);
        for (size_t i = 0; i < ng; ++i) mem[dm::home + i] = static_cast<std::byte>(home[i]);
        if (groups.size() <= 250 && write_table(groups, home_altar, mem.data(), dm::home + ng, mem.size())) mem[dm::table] = std::byte{'T'};
        cambc::draft_memory = std::span<const std::byte>{mem.data(), mem.size()};
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
