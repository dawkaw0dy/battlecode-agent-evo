#pragma once
// Small helpers shared by the draft and the champions.
#include <cambc.h>
#include <array>
#include <memory>
#include <cstdint>
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
// Map size and raw buffer layout, set once by init_dims(): function-local statics cost a guard check per call.
inline int g_map_w = 0, g_map_h = 0;
inline int g_bstride = 0;  // u64 words per row of 8x8 chunks in a bitboard
inline int g_fcw = 0;      // 8x8 chunks per row in a distance field
inline void init_dims() {
    const auto& l = cambc::detail::layout();
    g_map_w = l.width;
    g_map_h = l.height;
    g_bstride = static_cast<int>(l.chunk_stride);
    g_fcw = static_cast<int>(l.chunked_width);
}
inline int map_w() { return g_map_w; }
inline int map_h() { return g_map_h; }
inline bool inside(vec2 p) {
    return static_cast<unsigned>(p.x) < static_cast<unsigned>(g_map_w) && static_cast<unsigned>(p.y) < static_cast<unsigned>(g_map_h);
}
inline int tidx(vec2 p) { return p.y * g_map_w + p.x; }

// Engine buffers live in our own memory in 8x8 chunks: read them in place instead of a host call per tile
// (~35 fuel) or an export of the whole board (~2 fuel per tile). Callers check inside().
inline bool raw_bit(const std::uint64_t* w, vec2 p) {
    return (w[(p.y >> 3) * g_bstride + (p.x >> 3)] >> (((p.y & 7) << 3) | (p.x & 7))) & 1;
}
inline void raw_set(std::uint64_t* w, vec2 p) {
    w[(p.y >> 3) * g_bstride + (p.x >> 3)] |= std::uint64_t{1} << (((p.y & 7) << 3) | (p.x & 7));
}
inline void raw_clear(std::uint64_t* w, vec2 p) {
    w[(p.y >> 3) * g_bstride + (p.x >> 3)] &= ~(std::uint64_t{1} << (((p.y & 7) << 3) | (p.x & 7)));
}
inline int raw_dist(const std::uint16_t* f, vec2 p) {
    return f[(((p.y >> 3) * g_fcw + (p.x >> 3)) << 6) | ((p.y & 7) << 3) | (p.x & 7)];
}
// Bounds-checked tile test on any board, without a host call.
inline bool bit(const cambc::bitboard& b, vec2 p) { return inside(p) && raw_bit(b.ptr(), p); }

inline cambc::bitboard empty_board() {
    cambc::bitboard b;  // the constructor clears it
    return b;
}

// Chebyshev growth by in-place shifts: two boards allocated, four host calls per step and axis
// (a temporary per operator costs an allocation each).
inline cambc::bitboard grow(const cambc::bitboard& b, int r) {
    cambc::bitboard out = b;
    cambc::bitboard t{cambc::uninitialized};
    for (int i = 0; i < r; ++i) {
        out.shift_right_into(1, t);
        out |= t;
        out.shift_right_into(-1, t);
        out |= t;
    }
    for (int i = 0; i < r; ++i) {
        out.shift_up_into(1, t);
        out |= t;
        out.shift_up_into(-1, t);
        out |= t;
    }
    return out;
}

// Read-only tile set. bind() reads a board in place (it must outlive this and keep its storage);
// load() keeps a copy.
struct Bits {
    const std::uint64_t* p = nullptr;
    std::unique_ptr<cambc::bitboard> own;
    void bind(const cambc::bitboard& b) { p = b.ptr(); }
    void load(const cambc::bitboard& b) {
        if (!own) own = std::make_unique<cambc::bitboard>();
        own->clear();
        *own |= b;
        p = own->ptr();
    }
    bool at(vec2 q) const { return p && inside(q) && raw_bit(p, q); }
};

// Walking distances (kFar = unreached), computed into and read from engine storage: no export.
struct Dist {
    std::unique_ptr<cambc::distance_field> f;
    const std::uint16_t* q = nullptr;
    void load(const cambc::distance_field& src) {
        if (!f) f = std::make_unique<cambc::distance_field>();
        f->copy_from(src);
        q = static_cast<const cambc::distance_field&>(*f).ptr();
    }
    void compute(const cambc::bitboard& walls, const cambc::bitboard& roots, cambc::bitboard& seen) {
        if (!f) f = std::make_unique<cambc::distance_field>();
        f->compute(walls, roots, seen);
        q = static_cast<const cambc::distance_field&>(*f).ptr();
    }
    int at(vec2 p) const { return q && inside(p) ? raw_dist(q, p) : kFar; }
    bool empty() const { return q == nullptr; }
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
