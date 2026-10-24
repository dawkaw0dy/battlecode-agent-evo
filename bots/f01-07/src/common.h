#pragma once
// Small helpers shared by the draft and the champions.
#include <cambc.h>
#include <array>
#include <cstdint>
#include <vector>

using cambc::vec2;
using cambc::team;

constexpr int kFar = 65535;  // unreached distance
constexpr std::array<vec2, 8> kDirs{vec2{0, 1}, vec2{1, 1}, vec2{1, 0}, vec2{1, -1},
    vec2{0, -1}, vec2{-1, -1}, vec2{-1, 0}, vec2{-1, 1}};

inline int cheb(vec2 a, vec2 b) { return static_cast<int>(a.chebyshev_distance(b)); }
inline int map_w() { static const int w = cambc::map::width(); return w; }
inline int map_h() { static const int h = cambc::map::height(); return h; }
inline bool inside(vec2 p) { return p.x >= 0 && p.y >= 0 && p.x < map_w() && p.y < map_h(); }
inline int tidx(vec2 p) { return p.y * map_w() + p.x; }

inline cambc::bitboard empty_board() {
    cambc::bitboard b;
    b.clear();
    return b;
}

// Chebyshev growth by whole-board shifts (a handful of host calls).
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
};

// Local copy of a distance field.
struct Dist {
    std::vector<std::uint16_t> v;
    void load(const cambc::distance_field& f) {
        v.resize(static_cast<size_t>(map_w() * map_h()));
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
