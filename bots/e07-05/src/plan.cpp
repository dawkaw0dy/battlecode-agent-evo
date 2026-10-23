// Draft-time map analysis: size, champion count, altar distance and camp composition decide how the
// team packs up and how many gems it carries before banking. The result lives in draft memory.
#include "plan.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <cmath>
#include <vector>

using cambc::vec2;

namespace {

std::vector<std::uint8_t> g_draft_zones;

// Territories: reachable groups sorted by angle around our spawn and cut into `zones` arcs of equal
// gem value, so champions fan out instead of walking to the same few groups.
std::vector<std::uint8_t> assign_zones(int zones, const cambc::bitboard& visited) {
    const auto all = cambc::map::camp_groups();
    std::vector<std::uint8_t> result(all.size(), kZoneNone);
    // Groups our champions cannot walk to (islands, walled pockets) are marked once for good.
    for (std::size_t i = 0; i < all.size(); ++i) {
        bool reached = false;
        for (auto t : all[i].spawn_tiles().positions())
            if (visited.is_set(t)) { reached = true; break; }
        if (!reached) result[i] = kZoneUnreachable;
    }
    if (zones <= 1) return result;
    const auto spawn = cambc::map::spawn_region(cambc::team::ally).positions();
    if (spawn.empty()) return result;
    float cx = 0, cy = 0;
    for (auto t : spawn) { cx += t.x; cy += t.y; }
    cx /= spawn.size(); cy /= spawn.size();
    struct Entry { float angle; float value; std::size_t index; };
    std::vector<Entry> entries;
    float total = 0;
    for (std::size_t i = 0; i < all.size(); ++i) {
        const auto tiles = all[i].spawn_tiles().positions();
        bool reached = false;
        float gx = 0, gy = 0;
        for (auto t : tiles) { gx += t.x; gy += t.y; if (visited.is_set(t)) reached = true; }
        if (!reached || tiles.empty()) continue;
        gx /= tiles.size(); gy /= tiles.size();
        const float value = static_cast<float>(all[i].gem_value()) + 0.3f * static_cast<float>(all[i].gold_value()) + 1.0f;
        entries.push_back({std::atan2(gy - cy, gx - cx), value, i});
        total += value;
    }
    if (entries.empty()) return result;
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.angle < b.angle; });
    // Start the cut at the widest angular gap so no territory straddles two far-apart clusters.
    std::size_t start = 0;
    float widest = -1;
    for (std::size_t k = 0; k < entries.size(); ++k) {
        const float next = k + 1 < entries.size() ? entries[k + 1].angle : entries[0].angle + 6.2831853f;
        if (next - entries[k].angle > widest) { widest = next - entries[k].angle; start = (k + 1) % entries.size(); }
    }
    float acc = 0;
    for (std::size_t k = 0; k < entries.size(); ++k) {
        const auto& e = entries[(start + k) % entries.size()];
        const int zone = std::min(zones - 1, static_cast<int>((acc + e.value * 0.5f) / total * zones));
        result[e.index] = static_cast<std::uint8_t>(zone);
        acc += e.value;
    }
    return result;
}

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

    // Map-adaptive spreading: where the team farms in parallel and there are enough groups to share,
    // every champion owns a territory. Bigger maps make walking between territories dearer.
    const bool spread = p.pack_x10 <= 10 && reachable >= 3 * p.team_size;
    p.zones = spread ? p.team_size : 0;
    p.zone_x10 = p.size_class == 2 ? 18 : 14;
    g_draft_zones = assign_zones(p.zones, visited);
    const std::size_t room = std::size_t{1024} > sizeof(Plan)
        ? std::size_t{1024} - sizeof(Plan) : 0;
    if (g_draft_zones.size() > room) g_draft_zones.resize(room);
    p.zone_count = static_cast<std::uint16_t>(g_draft_zones.size());
    return p;
}

Plan g_draft_plan;

} // namespace

void write_plan() {
    const Plan p = analyse();
    g_draft_plan = p;
    std::vector<std::byte> bytes(sizeof(Plan) + g_draft_zones.size());
    std::memcpy(bytes.data(), &p, sizeof(Plan));
    if (!g_draft_zones.empty()) std::memcpy(bytes.data() + sizeof(Plan), g_draft_zones.data(), g_draft_zones.size());
    cambc::draft_memory = std::span<const std::byte>{bytes};
}

namespace {
const std::vector<std::uint8_t>& zones_table() {
    if (cambc::is_draft()) return g_draft_zones;
    static const std::vector<std::uint8_t> result = [] {
        std::vector<std::uint8_t> z;
        const Plan& p = plan();
        if (p.zone_count == 0) return z;
        try {
            const cambc::draft_memory_buffer buffer;
            if (buffer.size() >= sizeof(Plan) + p.zone_count) {
                z.resize(p.zone_count);
                std::memcpy(z.data(), buffer.data() + sizeof(Plan), p.zone_count);
            }
        } catch (...) {
            z.clear();
        }
        return z;
    }();
    return result;
}
} // namespace

int group_zone(std::size_t index) {
    const auto& z = zones_table();
    if (index >= z.size() || z[index] >= plan().zones) return -1;
    return z[index];
}

bool group_unreachable(std::size_t index) {
    const auto& z = zones_table();
    return index < z.size() && z[index] == kZoneUnreachable;
}

int my_zone() {
    if (plan().zones == 0 || cambc::is_draft()) return -1;
    static const int result = [] {
        try {
            const auto allies = cambc::champions(cambc::team::ally);
            const auto& me = cambc::self();
            for (std::size_t i = 0; i < allies.size(); ++i)
                if (allies[i] == me) return static_cast<int>(i % plan().zones);
        } catch (...) {
        }
        return -1;
    }();
    return result;
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
