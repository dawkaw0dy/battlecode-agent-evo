#pragma once
#include <cstdint>

// Map-adaptive opening plan: computed once in the draft from the full map, written to draft
// memory and read back by every champion (recomputed from the map if draft memory is missing).
struct Plan {
    std::uint8_t version = 0;
    std::uint8_t bank_gems = 20;      // carried gems that justify a trip to an altar
    std::uint8_t relock_div4 = 30;    // warden re-checks an altar after relock_div4 * 4 rounds
    std::uint8_t shop_div10 = 15;     // gold that justifies a recall to shop, / 10
    std::uint8_t altar_dist = 0;      // walking distance from our spawn to the nearest altar (capped)
    std::uint8_t team_size = 0;
    std::uint8_t altars = 0;
    std::uint8_t big_map = 0;         // 1 when width * height >= 4000
    std::uint8_t warden = 1;          // 1 when one champion keeps re-locking altars
};

Plan compute_plan();
const Plan& plan();
