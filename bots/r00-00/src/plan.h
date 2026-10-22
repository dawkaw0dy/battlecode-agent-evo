#pragma once
#include <cambc.h>
#include <cstdint>

// The plan chosen at draft time and shared with every champion through draft memory.
enum class PlanKind : std::uint8_t { farm = 0, big_camp = 1, aggro = 2 };

struct Plan {
    PlanKind kind = PlanKind::farm;
    std::uint8_t bank_threshold = 12; // carried gems before walking to an altar
    std::uint8_t danger_pct = 45;     // max % of health a solo camp fight may cost
    std::uint8_t chase_range = 2;     // extra tiles we walk to hit an enemy champion
};

inline Plan read_plan() {
    Plan plan;
    if (cambc::draft_memory.size() >= 5) {
        const auto* b = reinterpret_cast<const std::uint8_t*>(cambc::draft_memory.data());
        if (b[0] == 'R' && b[1] <= 2) {
            plan.kind = static_cast<PlanKind>(b[1]);
            plan.bank_threshold = b[2];
            plan.danger_pct = b[3];
            plan.chase_range = b[4];
        }
    }
    return plan;
}
