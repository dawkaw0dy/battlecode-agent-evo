// Draft-time map analysis: size, champion count, altar distance and camp composition decide how the
// team packs up and how many gems it carries before banking. The result lives in draft memory.
#include "plan.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <limits>

using cambc::vec2;

namespace {

Plan analyse() {
    Plan p;
    p.magic = kPlanMagic;
    const int w = cambc::map::width(), h = cambc::map::height();
    const int area = w * h;
    p.size_class = area <= 1600 ? 0 : (area >= 3600 ? 2 : 1);
    try {
        p.team_size = static_cast<std::uint8_t>(std::clamp<std::uint32_t>(cambc::draft::team_size(), 1, 8));
    } catch (const cambc::error&) {
    }

    // Path lengths from our spawn region.
    const auto blocked = cambc::map::walls() | cambc::map::holes() | cambc::map::altars()
        | cambc::map::spawn_region(cambc::team::enemy);
    cambc::distance_field field;
    cambc::bitboard visited;
    field.compute(blocked, cambc::map::spawn_region(cambc::team::ally), visited);

    int altar = std::numeric_limits<int>::max();
    for (auto a : cambc::map::altars().positions())
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy) {
                const vec2 t{a.x + dx, a.y + dy};
                if (cambc::map::is_in_bounds(t) && visited.is_set(t)) altar = std::min<int>(altar, field.get(t));
            }
    p.altar_dist = static_cast<std::uint8_t>(std::min(altar, 255));

    int reachable = 0, gems = 0;
    for (const auto& group : cambc::map::camp_groups()) {
        bool monolith = false;
        for (const auto& camp : group.camps())
            if (camp.kind() == cambc::camp_type::monolith) monolith = true;
        if (monolith) continue;
        bool reached = false;
        for (auto tile : group.spawn_tiles().positions())
            if (visited.is_set(tile)) { reached = true; break; }
        if (reached) { ++reachable; gems += static_cast<int>(group.gem_value()); }
    }
    p.groups = static_cast<std::uint8_t>(std::min(reachable, 255));
    p.camp_gems = static_cast<std::uint8_t>(std::min(gems / 4, 255));
    // Gem-starved maps (elk-arena, starvation): camps cannot decide the game, kills and stolen bags do.
    // The spell pack (chained stuns) fighting together wins there; elsewhere sturdy farmers spread out.
    const bool starved = gems < 80;

    // Packing: on small maps and with three champions the team fights together (camps are close, enemies
    // are near); elsewhere every champion farms its own best group (packing walked more than it cleared).
    p.pack_x10 = (starved || p.size_class == 0 || p.team_size <= 3) ? 25 : 10;

    // Banking: far altars justify bigger bags; small maps keep the enemy close, so bank sooner.
    p.bank_gems = 30;
    p.wary_bank = 15;
    if (p.size_class == 0) { p.bank_gems = 20; p.wary_bank = 12; }
    else if (p.altar_dist > 30 && p.size_class == 2) p.bank_gems = 40;
    p.draft_sturdy = (!starved && p.size_class == 2 && reachable >= 20) ? 1 : 0;
    return p;
}

Plan g_draft_plan;

} // namespace

void write_plan() {
    const Plan p = analyse();
    g_draft_plan = p;
    std::array<std::byte, sizeof(Plan)> bytes;
    std::memcpy(bytes.data(), &p, sizeof(Plan));
    cambc::draft_memory = std::span<const std::byte>{bytes};
}

const Plan& plan() {
    if (cambc::is_draft()) return g_draft_plan;
    static const Plan result = [] {
        Plan p;
        try {
            const cambc::draft_memory_buffer buffer;
            if (buffer.size() >= sizeof(Plan)) {
                Plan stored;
                std::memcpy(&stored, buffer.data(), sizeof(Plan));
                if (stored.magic == kPlanMagic && stored.bank_gems > 0 && stored.wary_bank > 0) p = stored;
            }
        } catch (...) {
        }
        return p;
    }();
    return result;
}
