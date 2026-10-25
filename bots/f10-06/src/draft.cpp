#include <cambc.h>
#include "draft.h"
#include "bot.h"
#include <algorithm>
#include <array>
#include <cstddef>

// Fixed pick order (big-game hunter): a hard-hitting melee, a tank, the healer, a gold-rich ranged
// carry and a second tank. The champions find their slot from this same list.
void play_draft() {
    const auto& order = pick_order();
    std::array<std::byte, 8> mem{};
    mem[0] = std::byte{'B'};
    mem[1] = static_cast<std::byte>(cambc::draft::team_size());
    cambc::draft_memory = std::span<const std::byte>{mem.data(), mem.size()};

    const auto chosen = cambc::draft::current_draft(cambc::team::ally);
    for (auto p : order)
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
