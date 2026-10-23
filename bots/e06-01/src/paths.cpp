// Cached terrain distance fields keyed by goal set (see paths.h).
#include <cambc.h>
#include "paths.h"
#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

using cambc::vec2;

namespace paths {
namespace {

constexpr std::size_t kMaxEntries = 96; // ~22 KB per entry on a 100x100 map
constexpr int kUnreachable = 1 << 20;

struct Entry {
    std::uint64_t hash = 0;
    cambc::bitboard goals;
    cambc::distance_field field;
    cambc::bitboard visited;
    std::uint32_t last_used = 0;
};

std::vector<std::unique_ptr<Entry>>& cache() {
    static std::vector<std::unique_ptr<Entry>> entries;
    return entries;
}

std::uint32_t& clock() {
    static std::uint32_t tick = 0;
    return tick;
}

std::uint64_t hash_of(const cambc::bitboard& goals) {
    const std::uint64_t* words = goals.ptr();
    const std::size_t count = cambc::bitboard::raw_buffer_size() / sizeof(std::uint64_t);
    std::uint64_t h = 1469598103934665603ull;
    for (std::size_t i = 0; i < count; ++i) {
        h ^= words[i];
        h *= 1099511628211ull;
    }
    return h;
}

std::uint64_t& measured() {
    static std::uint64_t cost = 0;
    return cost;
}

const cambc::bitboard& terrain() {
    static const cambc::bitboard blocked = cambc::map::walls() | cambc::map::holes() | cambc::map::altars()
        | cambc::map::spawn_region(cambc::team::enemy);
    return blocked;
}

Entry* find(const cambc::bitboard& goals, std::uint64_t hash) {
    for (auto& entry : cache())
        if (entry->hash == hash && entry->goals == goals) {
            entry->last_used = ++clock();
            return entry.get();
        }
    return nullptr;
}

Entry* compute(const cambc::bitboard& goals, std::uint64_t hash) {
    auto& entries = cache();
    std::unique_ptr<Entry> entry;
    if (entries.size() >= kMaxEntries) {
        // Reuse the least recently used slot (and its buffers).
        auto oldest = std::min_element(entries.begin(), entries.end(),
            [](const auto& a, const auto& b) { return a->last_used < b->last_used; });
        entry = std::move(*oldest);
        entries.erase(oldest);
    } else {
        entry = std::make_unique<Entry>();
    }
    entry->hash = hash;
    entry->goals = goals;
    const auto& blocked = terrain();
    const auto roots = goals & ~blocked;
    if (roots.is_empty()) {
        entry->visited.clear();
    } else {
        const auto before = cambc::fuel_remaining();
        entry->field.compute(blocked & ~goals, roots, entry->visited);
        const auto after = cambc::fuel_remaining();
        // Measure what a field really costs on this map (walls make it cheaper than the estimate).
        if (before > after) measured() = std::max(measured(), before - after);
    }
    entry->last_used = ++clock();
    entries.push_back(std::move(entry));
    return entries.back().get();
}

int read(const Entry& entry, vec2 from) {
    if (!entry.visited.is_set(from)) return kUnreachable;
    return entry.field.get(from);
}

int estimate(const cambc::bitboard& goals, vec2 from) {
    int best = kUnreachable;
    for (auto tile : goals.positions())
        best = std::min(best, static_cast<int>(from.chebyshev_distance(tile)) * 5 / 4);
    return best;
}

} // namespace

std::uint64_t field_cost() {
    if (measured() > 0) return measured() + measured() / 8 + 4000;
    return static_cast<std::uint64_t>(cambc::map::width() * cambc::map::height()) * 11 + 12000;
}

int cached_distance(const cambc::bitboard& goals, vec2 from) {
    const Entry* entry = find(goals, hash_of(goals));
    return entry ? read(*entry, from) : -1;
}

bool ensure(const cambc::bitboard& goals, std::uint64_t reserve) {
    const auto hash = hash_of(goals);
    if (find(goals, hash)) return true;
    if (cambc::fuel_remaining() < field_cost() + reserve) return false;
    compute(goals, hash);
    return true;
}

int distance(const cambc::bitboard& goals, vec2 from) {
    const auto hash = hash_of(goals);
    const Entry* entry = find(goals, hash);
    if (!entry && cambc::fuel_remaining() >= field_cost() + 20000) entry = compute(goals, hash);
    return entry ? read(*entry, from) : estimate(goals, from);
}

bool step_towards(const cambc::bitboard& goals, vec2 from, const cambc::bitboard& free_steps, vec2& step,
    bool allow_equal) {
    const Entry* entry = find(goals, hash_of(goals));
    if (!entry || !entry->visited.is_set(from)) return false;
    int best = entry->field.get(from) + (allow_equal ? 1 : 0);
    bool found = false;
    for (auto tile : free_steps.positions()) {
        if (tile == from || !entry->visited.is_set(tile)) continue;
        const int d = entry->field.get(tile);
        if (d < best) { best = d; step = tile; found = true; }
    }
    return found;
}

} // namespace paths
