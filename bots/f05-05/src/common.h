#pragma once
// Small helpers shared by the draft and the champions.
//
// Fuel model (measured): a host call costs ~40 fuel plus a per-tile charge for whole-board work, and every
// fresh bitboard or distance field is a wasm allocation. On a 100x100 map a full distance field costs ~30k,
// exporting it ~23k, exporting a bitboard ~1.7k, a bitboard temporary ~0.9k. Boards and fields live in wasm
// memory in the engine's chunked layout (8x8 tiles per chunk), so we read tiles straight from that storage
// (no exports at all) and build boards in place.
#include <cambc.h>
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

using cambc::vec2;
using cambc::team;

constexpr int kFar = 65535;  // unreached distance (what the engine stores too)
constexpr std::array<vec2, 8> kDirs{vec2{0, 1}, vec2{1, 1}, vec2{1, 0}, vec2{1, -1},
    vec2{0, -1}, vec2{-1, -1}, vec2{-1, 0}, vec2{-1, 1}};

inline int cheb(vec2 a, vec2 b) {
    const int dx = a.x > b.x ? a.x - b.x : b.x - a.x, dy = a.y > b.y ? a.y - b.y : b.y - a.y;
    return dx > dy ? dx : dy;
}
// Map size and chunk layout, set once by init_dims(): function-local statics cost a guard check per call.
inline int g_map_w = 0, g_map_h = 0, g_board_stride = 0, g_field_stride = 0;
inline bool g_raw = false;  // raw chunk reads verified against the host (raw_check)
inline void init_dims() {
    g_map_w = cambc::map::width();
    g_map_h = cambc::map::height();
    const auto& l = cambc::detail::layout();
    g_board_stride = static_cast<int>(l.chunk_stride);
    g_field_stride = static_cast<int>(l.chunked_width);
}
inline int map_w() { return g_map_w; }
inline int map_h() { return g_map_h; }
inline bool inside(vec2 p) { return p.x >= 0 && p.y >= 0 && p.x < g_map_w && p.y < g_map_h; }
inline int tidx(vec2 p) { return p.y * g_map_w + p.x; }

// Raw reads of host-format storage; p must be inside the map.
inline bool raw_bit(const std::uint64_t* w, vec2 p) {
    return (w[(p.y >> 3) * g_board_stride + (p.x >> 3)] >> (((p.y & 7) << 3) | (p.x & 7))) & 1;
}
inline int raw_dist(const std::uint16_t* f, vec2 p) {
    return f[(((p.y >> 3) * g_field_stride + (p.x >> 3)) << 6) | ((p.y & 7) << 3) | (p.x & 7)];
}

// Set a tile of a board in place: a raw write when the layout check passed (a host set costs about as
// much as a whole-board operation). p must be inside the map.
inline void board_set(cambc::bitboard& b, vec2 p, bool on = true) {
    if (!g_raw) { b.set(p, on); return; }
    std::uint64_t& w = b.ptr()[(p.y >> 3) * g_board_stride + (p.x >> 3)];
    const std::uint64_t bit = std::uint64_t{1} << (((p.y & 7) << 3) | (p.x & 7));
    w = on ? (w | bit) : (w & ~bit);
}

// Fuel a full distance-field search costs on this map (~3 per tile, measured), with a margin.
inline std::uint64_t field_cost() { return 3500 + 4 * static_cast<std::uint64_t>(g_map_w * g_map_h); }

inline cambc::bitboard empty_board() {
    cambc::bitboard b;
    b.clear();
    return b;
}

// Scratch board for in-place growth, allocated on first use.
inline cambc::bitboard* g_scratch = nullptr;
inline cambc::bitboard& scratch() {
    if (!g_scratch) g_scratch = new cambc::bitboard;
    return *g_scratch;
}
// Chebyshev growth of b by r, in place: four host calls per step and axis, no temporaries.
inline void grow_in_place(cambc::bitboard& b, int r) {
    auto& s = scratch();
    for (int i = 0; i < r; ++i) {
        b.shift_right_into(1, s); b |= s;
        b.shift_right_into(-1, s); b |= s;
    }
    for (int i = 0; i < r; ++i) {
        b.shift_up_into(1, s); b |= s;
        b.shift_up_into(-1, s); b |= s;
    }
}
inline cambc::bitboard grow(const cambc::bitboard& b, int r) {
    cambc::bitboard out = b;
    grow_in_place(out, r);
    return out;
}

// Tile reads of a board. ref() follows a board that outlives us and is only ever updated in place (its
// storage never moves); load() keeps its own copy. Reads are raw when the layout check passed.
struct Bits {
    const cambc::bitboard* b = nullptr;
    const std::uint64_t* w = nullptr;  // b's storage (it never moves) when raw reads are on
    std::unique_ptr<cambc::bitboard> own;
    void ref(const cambc::bitboard& src) { b = &src; w = g_raw ? src.ptr() : nullptr; }
    void load(const cambc::bitboard& src) {
        if (!own) own = std::make_unique<cambc::bitboard>();
        own->clear();
        *own |= src;
        ref(*own);
    }
    bool at(vec2 p) const {
        if (static_cast<unsigned>(p.x) >= static_cast<unsigned>(g_map_w) || static_cast<unsigned>(p.y) >= static_cast<unsigned>(g_map_h))
            return false;
        if (w) return raw_bit(w, p);
        return b && b->is_set(p);
    }
};

// A distance field we keep (walking distance to a fixed goal); unreached reads kFar.
struct Dist {
    std::unique_ptr<cambc::distance_field> f;
    const std::uint16_t* raw = nullptr;  // f's storage when raw reads are on
    // Search from roots around walls straight into our own storage.
    void compute(const cambc::bitboard& walls, const cambc::bitboard& roots) {
        if (!f) f = std::make_unique<cambc::distance_field>();
        cambc::bitboard seen;
        f->compute(walls, roots, seen);
        raw = g_raw ? f->ptr() : nullptr;
    }
    void load(const cambc::distance_field& src) {
        if (!f) f = std::make_unique<cambc::distance_field>();
        f->copy_from(src);
        raw = g_raw ? f->ptr() : nullptr;
    }
    int at(vec2 p) const {
        if (static_cast<unsigned>(p.x) >= static_cast<unsigned>(g_map_w) || static_cast<unsigned>(p.y) >= static_cast<unsigned>(g_map_h))
            return kFar;
        if (raw) return raw_dist(raw, p);
        return f ? f->get(p) : kFar;
    }
    bool empty() const { return !f; }
};

// Compare raw reads with host reads on a spread of tiles; raw reads are used only when all agree.
inline bool raw_check(const cambc::bitboard& b, const cambc::distance_field& f) {
    int checked = 0;
    const int step = (g_map_w * g_map_h) / 97 + 1;
    for (int i = 0; i < g_map_w * g_map_h; i += step) {
        const vec2 p{i % g_map_w, i / g_map_w};
        if (raw_bit(b.ptr(), p) != b.is_set(p) || raw_dist(f.ptr(), p) != f.get(p)) return false;
        ++checked;
    }
    const vec2 corner{g_map_w - 1, g_map_h - 1};
    return checked > 0 && raw_bit(b.ptr(), corner) == b.is_set(corner) && raw_dist(f.ptr(), corner) == f.get(corner);
}

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
