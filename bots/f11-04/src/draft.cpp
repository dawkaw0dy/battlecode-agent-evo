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
        const auto groups = analyse_map();
        const auto home = assign_homes(groups, static_cast<int>(picks.size()));
        std::array<std::byte, 1024> mem{};
        mem[dm::magic] = std::byte{'E'};
        mem[dm::size] = static_cast<std::byte>(picks.size());
        const size_t n = std::min<size_t>(groups.size(), 250);
        mem[dm::ngroups] = static_cast<std::byte>(n);
        for (size_t i = 0; i < n; ++i) mem[dm::home + i] = static_cast<std::byte>(home[i]);
        const auto routes = plan_routes(groups, static_cast<int>(picks.size()));
        mem[dm::route_magic] = std::byte{'R'};
        for (size_t s = 0; s < routes.size() && s < 5; ++s) {
            const size_t len = std::min<size_t>(routes[s].size(), dm::kRouteMax);
            mem[dm::route_len + s] = static_cast<std::byte>(len);
            for (size_t k = 0; k < len; ++k) mem[dm::route + s * dm::kRouteMax + k] = static_cast<std::byte>(routes[s][k]);
        }
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
