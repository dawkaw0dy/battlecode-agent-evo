#include <cambc.h>
#include "draft.h"
#include "plan.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <vector>

// Plan the sweep (picks, squads and each squad's camp groups) once, record it, then pick.
void play_draft() {
    const int size = static_cast<int>(cambc::draft::team_size());
    static std::vector<GroupInfo> groups;
    static bool analysed = false;
    if (!analysed) {
        analysed = true;
        groups = analyse_groups();
    }
    const bool dash_map = is_dash_map(groups);
    const auto picks = preferred_picks(size, dash_map);
    const auto chosen = cambc::draft::current_draft(cambc::team::ally);

    static bool written = false;
    if (!written) {
        written = true;
        const auto owner = split_groups(groups, size, dash_map);
        std::array<std::byte, 1024> mem{};
        mem[layout::magic] = std::byte{'W'};
        mem[layout::team_size] = static_cast<std::byte>(picks.size());
        mem[layout::dash_map] = static_cast<std::byte>(dash_map ? 1 : 0);
        for (size_t i = 0; i < picks.size() && i < 5; ++i)
            mem[layout::picks + i] = static_cast<std::byte>(static_cast<int>(picks[i]));
        const size_t count = std::min<size_t>(groups.size(), mem.size() - layout::owner);
        mem[layout::group_count] = static_cast<std::byte>(std::min<size_t>(count, 255));
        for (size_t i = 0; i < count; ++i) mem[layout::owner + i] = static_cast<std::byte>(owner[i]);
        cambc::draft_memory = std::span<const std::byte>{mem.data(), mem.size()};
    }

    for (auto pick : picks) {
        if (std::find(chosen.begin(), chosen.end(), pick) != chosen.end()) continue;
        cambc::draft::pick(pick);
        return;
    }
    for (int i = 0; i <= static_cast<int>(cambc::champion_draft::heart_of_ruin); ++i) {
        auto pick = static_cast<cambc::champion_draft>(i);
        if (pick == cambc::champion_draft::cassi_and_issac) continue;
        if (std::find(chosen.begin(), chosen.end(), pick) == chosen.end()) {
            cambc::draft::pick(pick);
            return;
        }
    }
}
