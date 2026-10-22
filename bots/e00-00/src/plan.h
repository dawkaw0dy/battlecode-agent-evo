#pragma once
#include <cambc.h>
#include <cstdint>

// Map-wide plan chosen at draft time; every champion reads the same bytes.
enum class PlanKind : std::uint8_t { farm = 0, hunt = 1, aggro = 2 };

struct Plan {
    std::uint8_t magic = 'E';
    PlanKind kind = PlanKind::farm;
    std::uint8_t bank = 15;        // carried gems that trigger a trip to an altar
    std::uint8_t danger = 45;      // % of own health a camp fight may cost
    std::uint8_t chase = 2;        // extra tiles to chase a weak enemy
    std::uint8_t helpers = 0;      // allies assumed to join a big camp when judging danger
    std::uint8_t cap = 2;          // allies allowed on one camp group
    std::uint8_t golem_round = 60; // golems allowed from this round / 10
    std::uint8_t recall = 33;      // recall below this % health
    std::uint8_t shop_gold = 20;   // recall to shop at this gold / 10
};

inline Plan read_plan() {
    Plan plan;
    const auto& mem = cambc::draft_memory;
    if (mem.size() >= sizeof(Plan) && static_cast<std::uint8_t>(mem[0]) == 'E') {
        auto* bytes = reinterpret_cast<std::uint8_t*>(&plan);
        for (std::size_t i = 0; i < sizeof(Plan); ++i) bytes[i] = static_cast<std::uint8_t>(mem[i]);
    }
    return plan;
}
