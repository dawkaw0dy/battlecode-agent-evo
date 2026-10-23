#include <cambc.h>
#include "plan.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>

namespace {

constexpr std::uint8_t kPlanVersion = 1;

int nearest_altar_distance() {
    const auto spawn = cambc::map::spawn_region(cambc::team::ally);
    const auto blocked = cambc::map::walls() | cambc::map::holes() | cambc::map::altars()
        | cambc::map::spawn_region(cambc::team::enemy);
    cambc::bitboard goals;
    goals.clear();
    for (auto altar : cambc::map::altars().positions())
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy) {
                const cambc::vec2 p{altar.x + dx, altar.y + dy};
                if (cambc::map::is_in_bounds(p) && !blocked.is_set(p)) goals.set(p, true);
            }
    cambc::distance_field field;
    cambc::bitboard visited;
    field.compute(blocked & ~spawn, spawn, visited);
    int best = 255;
    for (auto tile : goals.positions())
        if (visited.is_set(tile)) best = std::min<int>(best, field.get(tile));
    return best;
}

} // namespace

Plan compute_plan() {
    Plan p;
    p.version = kPlanVersion;
    try {
        const int area = cambc::map::width() * cambc::map::height();
        p.big_map = area >= 4000 ? 1 : 0;
        p.altars = static_cast<std::uint8_t>(std::min<std::size_t>(255, cambc::map::altars().positions().size()));
        p.altar_dist = static_cast<std::uint8_t>(std::clamp(nearest_altar_distance(), 0, 255));
    } catch (const cambc::error&) {}
    // Far altars: carry bigger bags per trip (small bags near altars lost to the parent).
    p.bank_gems = static_cast<std::uint8_t>(std::clamp(10 + p.altar_dist, 20, 30));
    // Big maps: altars are spread out, so the warden re-checks them less often.
    if (p.big_map) p.relock_div4 = 45;
    try {
        p.team_size = static_cast<std::uint8_t>(cambc::draft::team_size());
    } catch (const cambc::error&) {}
    return p;
}

const Plan& plan() {
    static const Plan result = [] {
        const auto& memory = cambc::draft_memory;
        if (memory.size() >= sizeof(Plan) && static_cast<std::uint8_t>(memory[0]) == kPlanVersion) {
            Plan p;
            std::memcpy(&p, memory.data(), sizeof(Plan));
            return p;
        }
        return compute_plan();
    }();
    return result;
}
