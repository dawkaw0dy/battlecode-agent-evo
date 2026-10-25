#pragma once
// Small helpers shared by the draft and the champions.
#include <cambc.h>
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

using cambc::vec2;
using cambc::team;

constexpr int kFar = 65535;  // unreached distance
constexpr std::array<vec2, 8> kDirs{vec2{0, 1}, vec2{1, 1}, vec2{1, 0}, vec2{1, -1},
    vec2{0, -1}, vec2{-1, -1}, vec2{-1, 0}, vec2{-1, 1}};

inline int cheb(vec2 a, vec2 b) {
    const int dx = a.x > b.x ? a.x - b.x : b.x - a.x, dy = a.y > b.y ? a.y - b.y : b.y - a.y;
    return dx > dy ? dx : dy;
}
// Map size, set once by init_dims(): function-local statics cost a guard check per call.
inline int g_map_w = 0, g_map_h = 0;
inline void init_dims() { g_map_w = cambc::map::width(); g_map_h = cambc::map::height(); }
inline int map_w() { return g_map_w; }
inline int map_h() { return g_map_h; }
inline bool inside(vec2 p) { return p.x >= 0 && p.y >= 0 && p.x < g_map_w && p.y < g_map_h; }
inline int tidx(vec2 p) { return p.y * g_map_w + p.x; }

inline cambc::bitboard empty_board() {
    cambc::bitboard b;
    b.clear();
    return b;
}

// Scratch board for in-place work (a new board costs an allocation and a clear).
inline cambc::bitboard* g_scratch = nullptr;
inline cambc::bitboard& scratch() {
    if (!g_scratch) g_scratch = new cambc::bitboard();
    return *g_scratch;
}

// Chebyshev growth by whole-board shifts into a scratch board: one allocation for the result.
// out | out>>1, then that | its <<1, equals out | out>>1 | out<<1 (a shifted-back copy loses only an edge).
inline cambc::bitboard grow(const cambc::bitboard& b, int r) {
    cambc::bitboard out = b;
    if (r <= 0) return out;
    auto& t = scratch();
    for (int i = 0; i < r; ++i) {
        out.shift_right_into(1, t); out |= t;
        out.shift_right_into(-1, t); out |= t;
    }
    for (int i = 0; i < r; ++i) {
        out.shift_up_into(1, t); out |= t;
        out.shift_up_into(-1, t); out |= t;
    }
    return out;
}

// Local bit-packed copy of a board: host queries in loops are expensive. Storage is allocated once and
// never zero-filled (the export writes every byte).
struct Bits {
    std::unique_ptr<std::uint8_t[]> v;
    void load(const cambc::bitboard& b) {
        const size_t n = cambc::bitboard::linear_buffer_size();
        if (!v) v.reset(new std::uint8_t[n]);
        b.export_linear_buffer(std::span<std::uint8_t>{v.get(), n});
    }
    bool at(vec2 p) const {
        if (!inside(p) || !v) return false;
        const int i = tidx(p);
        return (v[static_cast<size_t>(i >> 3)] >> (i & 7)) & 1;
    }
};

// Local copy of a distance field (unreached = 65535). A value-initialised vector would zero-fill
// 2·w·h bytes in local code: ~20k fuel on a 100x100 map before the export overwrites it.
struct Dist {
    std::unique_ptr<std::uint16_t[]> v;
    void load(const cambc::distance_field& f) {
        const size_t n = static_cast<size_t>(map_w() * map_h());
        if (!v) v.reset(new std::uint16_t[n]);
        f.export_linear_buffer(std::span<std::uint16_t>{v.get(), n});
    }
    int at(vec2 p) const {
        if (!inside(p) || !v) return kFar;
        return v[static_cast<size_t>(tidx(p))];
    }
    bool empty() const { return !v; }
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
