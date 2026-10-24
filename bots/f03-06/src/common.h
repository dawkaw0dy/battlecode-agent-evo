#pragma once
// Small helpers shared by the draft and the champions.
#include <cambc.h>
#include <array>
#include <cstdint>
#include <span>
#include <vector>

using cambc::vec2;
using cambc::team;

constexpr int kFar = 65535;  // unreached distance in a field

constexpr std::array<vec2, 8> kDirs{vec2{0, 1}, vec2{1, 1}, vec2{1, 0}, vec2{1, -1},
    vec2{0, -1}, vec2{-1, -1}, vec2{-1, 0}, vec2{-1, 1}};

// Map size, set once: function-local statics cost a guard check per call.
inline int g_w = 0, g_h = 0;
inline void init_dims() { g_w = cambc::map::width(); g_h = cambc::map::height(); }
inline bool inside(vec2 p) { return p.x >= 0 && p.y >= 0 && p.x < g_w && p.y < g_h; }
inline int tidx(vec2 p) { return p.y * g_w + p.x; }

inline int cheb(vec2 a, vec2 b) {
    const int dx = a.x > b.x ? a.x - b.x : b.x - a.x, dy = a.y > b.y ? a.y - b.y : b.y - a.y;
    return dx > dy ? dx : dy;
}

inline cambc::bitboard empty_board() {
    cambc::bitboard b;
    b.clear();
    return b;
}

// Chebyshev growth by whole-board shifts (a few host calls per step).
inline cambc::bitboard grow(const cambc::bitboard& b, int r) {
    cambc::bitboard out = b;
    for (int i = 0; i < r; ++i) out = out | out.shift_right(1) | out.shift_right(-1);
    for (int i = 0; i < r; ++i) out = out | out.shift_up(1) | out.shift_up(-1);
    return out;
}

// Local bit-packed copy of a board: host queries in loops are expensive.
struct Bits {
    std::vector<std::uint8_t> v;
    void load(const cambc::bitboard& b) {
        v.resize(cambc::bitboard::linear_buffer_size());
        b.export_linear_buffer(std::span<std::uint8_t>{v});
    }
    bool at(vec2 p) const {
        if (!inside(p) || v.empty()) return false;
        const int i = tidx(p);
        return (v[static_cast<size_t>(i >> 3)] >> (i & 7)) & 1;
    }
    void set(vec2 p) {
        if (!inside(p) || v.empty()) return;
        const int i = tidx(p);
        v[static_cast<size_t>(i >> 3)] |= static_cast<std::uint8_t>(1u << (i & 7));
    }
    void clear_bits() { for (auto& b : v) b = 0; }
    cambc::bitboard board() const {
        cambc::bitboard b;
        b.import_linear_buffer(std::span<const std::uint8_t>{v});
        return b;
    }
    void resize() { v.assign(cambc::bitboard::linear_buffer_size(), 0); }
};

// Local copy of a distance field (row-major, kFar = unreached).
struct Field {
    std::vector<std::uint16_t> v;
    void load(const cambc::distance_field& f) {
        v.resize(static_cast<size_t>(g_w * g_h));
        f.export_linear_buffer(std::span<std::uint16_t>{v});
    }
    int at(vec2 p) const {
        if (!inside(p) || v.empty()) return kFar;
        return v[static_cast<size_t>(tidx(p))];
    }
    bool empty() const { return v.empty(); }
};

struct CampSpec { float hp, str; int range; };
inline CampSpec camp_spec(cambc::camp_type t) {
    switch (t) {
    case cambc::camp_type::wolf: return {1350, 20, 3};
    case cambc::camp_type::elk: return {2400, 60, 4};
    case cambc::camp_type::gem_cluster: return {120, 0, 0};
    case cambc::camp_type::gold_cluster: return {120, 0, 0};
    case cambc::camp_type::monolith: return {20000, 200, 5};
    case cambc::camp_type::golem: return {5000, 120, 4};
    }
    return {1000, 50, 3};
}
inline bool is_cluster(cambc::camp_type t) {
    return t == cambc::camp_type::gem_cluster || t == cambc::camp_type::gold_cluster;
}
