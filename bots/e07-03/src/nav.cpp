// Cache of terrain-only path fields keyed by goal set (see nav.h).
#include "nav.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <vector>

namespace nav {
namespace {

constexpr std::size_t kMaxFields = 64;  // unpinned entries, ~10 KB each on 70x70
constexpr std::size_t kMaxPending = 12; // goal sets seen once

std::vector<std::unique_ptr<Field>> g_fields;
std::vector<Field*> g_slots;
struct Key {
    std::uint32_t count = 0;
    cambc::vec2 first{-1, -1};
    bool operator==(const Key& o) const { return count == o.count && first == o.first; }
};
struct Pending { Key key; cambc::bitboard goals; };
std::vector<Pending> g_pending;

Key key_of(const cambc::bitboard& goals) {
    Key key;
    std::array<cambc::vec2, 1> head{};
    key.count = goals.count();
    if (key.count > 0 && goals.positions_into(head) > 0) key.first = head[0];
    return key;
}
std::uint64_t g_cost = 0; // largest measured field cost

Field* find(const cambc::bitboard& goals, const Key& key) {
    for (auto& entry : g_fields)
        if (entry->count == key.count && entry->first == key.first && entry->goals == goals) return entry.get();
    return nullptr;
}

Field* compute(const cambc::bitboard& goals, const Key& key) {
    const auto before = cambc::fuel_remaining();
    auto entry = std::make_unique<Field>();
    entry->goals = goals;
    entry->count = key.count;
    entry->first = key.first;
    entry->used = static_cast<int>(cambc::round_number());
    // Goal tiles on walls, holes or altars are never roots (an altar's approach square can contain
    // walls: as roots they read distance 0 and the field pointed into the wall).
    entry->field.compute(terrain(), goals & ~terrain(), entry->visited);
    const auto after = cambc::fuel_remaining();
    if (before > after) g_cost = std::max(g_cost, before - after);

    std::size_t unpinned = 0;
    for (const auto& e : g_fields) unpinned += e->pinned ? 0 : 1;
    if (unpinned >= kMaxFields) {
        auto oldest = g_fields.end();
        for (auto it = g_fields.begin(); it != g_fields.end(); ++it)
            if (!(*it)->pinned && (oldest == g_fields.end() || (*it)->used < (*oldest)->used)) oldest = it;
        if (oldest != g_fields.end()) {
            *oldest = std::move(entry);
            return oldest->get();
        }
    }
    g_fields.push_back(std::move(entry));
    return g_fields.back().get();
}

} // namespace

int Field::dist(cambc::vec2 tile) const {
    const int w = cambc::map::width(), h = cambc::map::height();
    if (tile.x < 0 || tile.y < 0 || tile.x >= w || tile.y >= h) return -1;
    if (!visited.is_set(tile)) return -1;
    return static_cast<int>(field.get(tile));
}

const cambc::bitboard& terrain() {
    static const cambc::bitboard result = cambc::map::walls() | cambc::map::holes() | cambc::map::altars()
        | cambc::map::spawn_region(cambc::team::enemy);
    return result;
}

std::uint64_t field_cost() {
    // Before the first measurement: ~4 fuel per tile, a safe overestimate of the measured ~3.3.
    const auto tiles = static_cast<std::uint64_t>(cambc::map::width() * cambc::map::height());
    return (g_cost > 0 ? g_cost : tiles * 4) + 4000;
}

const Field* cached(const cambc::bitboard& goals) {
    Field* entry = find(goals, key_of(goals));
    if (entry) entry->used = static_cast<int>(cambc::round_number());
    return entry;
}

const Field* ensure(const cambc::bitboard& goals, std::uint64_t reserve) {
    const Key key = key_of(goals);
    if (Field* hit = find(goals, key)) {
        hit->used = static_cast<int>(cambc::round_number());
        return hit;
    }
    if (key.count == 0 || cambc::fuel_remaining() < field_cost() + reserve) return nullptr;
    return compute(goals, key);
}

const Field* ensure_slot(int slot, const cambc::bitboard& goals, std::uint64_t reserve) {
    if (slot < 0) return nullptr;
    if (static_cast<std::size_t>(slot) >= g_slots.size()) g_slots.resize(slot + 1, nullptr);
    if (g_slots[slot]) return g_slots[slot];
    // Checked first: the key lookup alone (count() plus comparisons) costs a few hundred fuel.
    if (cambc::fuel_remaining() < field_cost() + reserve) return nullptr;
    const Key key = key_of(goals);
    Field* entry = find(goals, key);
    if (!entry) {
        if (key.count == 0) return nullptr;
        entry = compute(goals, key);
    }
    entry->pinned = true;
    g_slots[slot] = entry;
    return entry;
}

const Field* ensure_repeated(const cambc::bitboard& goals, std::uint64_t reserve) {
    const Key key = key_of(goals);
    if (Field* hit = find(goals, key)) {
        hit->used = static_cast<int>(cambc::round_number());
        return hit;
    }
    auto seen = g_pending.end();
    for (auto it = g_pending.begin(); it != g_pending.end(); ++it)
        if (it->key == key && it->goals == goals) { seen = it; break; }
    if (seen == g_pending.end()) {
        if (g_pending.size() >= kMaxPending) g_pending.erase(g_pending.begin());
        g_pending.push_back({key, goals});
        return nullptr;
    }
    if (key.count == 0 || cambc::fuel_remaining() < field_cost() + reserve) return nullptr;
    g_pending.erase(seen);
    return compute(goals, key);
}

} // namespace nav
