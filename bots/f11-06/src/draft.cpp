#include <cambc.h>
#include "draft.h"
#include "mapinfo.h"
#include "plan.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <cstdio>
#include <vector>

// The draft sees the whole map: measure it, choose the plan and the picks, and write both
// into draft memory so every champion follows the same plan.
void play_draft() {
    static bool written = false;
    static std::vector<cambc::champion_draft> order;
    if (!written) {
        written = true;
        const int size = static_cast<int>(cambc::draft::team_size());
        const auto& m = map_info();
        const Plan plan = make_plan(m, size);
        order = plan.picks;
        if (kTrace) {
            int nb = 0, ng = 0, nc = 0;
            for (auto c : plan.cls) nb += c == kGHome, ng += c == kGContested, nc += c == kGEnemy;
            std::printf("PLAN %dx%d size%d gap%d mode%d bank%d raid%d groups%zu home%d cont%d enemy%d altars%zu\n", m.w, m.h, size, m.spawn_gap,
                plan.mode, plan.bank, plan.raid, m.groups.size(), nb, ng, nc, m.altars.size());
        }
        std::array<std::byte, 1024> mem{};
        write_plan(plan, m, mem);
        cambc::draft_memory = std::span<const std::byte>{mem.data(), mem.size()};
    }
    const auto chosen = cambc::draft::current_draft(team::ally);
    for (auto p : order)
        if (std::find(chosen.begin(), chosen.end(), p) == chosen.end()) {
            cambc::draft::pick(p);
            return;
        }
    for (int i = 0; i <= static_cast<int>(cambc::champion_draft::heart_of_ruin); ++i) {
        const auto p = static_cast<cambc::champion_draft>(i);
        if (std::find(chosen.begin(), chosen.end(), p) == chosen.end()) {
            cambc::draft::pick(p);
            return;
        }
    }
}
