#pragma once
// Debug output for local diagnosis only; kDebug stays false in submitted builds.
#include <cambc.h>
#include <iostream>
#include <string>

constexpr bool kDebug = false;

inline void dbg_dump_map() {
    if (!kDebug) return;
    const int w = cambc::map::width(), h = cambc::map::height();
    const auto& walls = cambc::map::walls();
    const auto& holes = cambc::map::holes();
    const auto& bushes = cambc::map::bushes();
    const auto& altars = cambc::map::altars();
    const auto& sa = cambc::map::spawn_region(cambc::team::ally);
    const auto& se = cambc::map::spawn_region(cambc::team::enemy);
    std::string grid(static_cast<size_t>(w * h), '.');
    auto put = [&](const cambc::bitboard& b, char c) {
        for (auto p : b.positions()) grid[static_cast<size_t>(p.y * w + p.x)] = c;
    };
    put(bushes, '"');
    put(walls, '#');
    put(holes, 'o');
    put(altars, 'A');
    put(sa, 'a');
    put(se, 'e');
    for (const auto& g : cambc::map::camp_groups())
        for (const auto& c : g.camps()) {
            const auto p = c.spawn_position();
            const char k = "WEgcMG"[static_cast<int>(c.kind())];
            grid[static_cast<size_t>(p.y * w + p.x)] = k;
        }
    std::cout << "MAP " << w << "x" << h << "\n";
    for (int y = h - 1; y >= 0; --y) {
        std::cout << (y < 10 ? " " : "") << y << " ";
        std::cout << grid.substr(static_cast<size_t>(y * w), static_cast<size_t>(w)) << "\n";
    }
    std::cout << std::flush;
}

// Detailed trace window (rounds) for one slot; -1 = all slots.
inline int g_dbg_from = 0, g_dbg_to = -1, g_dbg_slot = -1;
#define DBG(slot, expr) do { if (kDebug && static_cast<int>(cambc::round_number()) >= g_dbg_from && static_cast<int>(cambc::round_number()) <= g_dbg_to && (g_dbg_slot < 0 || g_dbg_slot == (slot))) { std::cout << "D r" << cambc::round_number() << " s" << (slot) << " " << expr << std::endl; } } while (0)
