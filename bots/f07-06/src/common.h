#pragma once
// Small helpers: map layout, raw tile access to engine buffers (no host call per tile), distances.
#include <cambc.h>
#include <cstdint>

using cambc::vec2;
using cambc::team;

namespace lay {
inline int W = 0, H = 0, BS = 0, FCW = 0;
}

inline void init_layout() {
    const auto& l = cambc::detail::layout();
    lay::W = l.width;
    lay::H = l.height;
    lay::BS = static_cast<int>(l.chunk_stride);
    lay::FCW = static_cast<int>(l.chunked_width);
}

inline bool inside(vec2 p) {
    return static_cast<unsigned>(p.x) < static_cast<unsigned>(lay::W)
        && static_cast<unsigned>(p.y) < static_cast<unsigned>(lay::H);
}

inline int cheb(vec2 a, vec2 b) {
    const int dx = a.x > b.x ? a.x - b.x : b.x - a.x;
    const int dy = a.y > b.y ? a.y - b.y : b.y - a.y;
    return dx > dy ? dx : dy;
}

// Bitboards are stored in 8x8 chunks of one u64 each.
inline bool rbit(const cambc::bitboard& b, vec2 p) {
    if (!inside(p)) return false;
    return (b.ptr()[(p.y >> 3) * lay::BS + (p.x >> 3)] >> (((p.y & 7) << 3) | (p.x & 7))) & 1;
}
inline void rset(cambc::bitboard& b, vec2 p) {
    if (!inside(p)) return;
    b.ptr()[(p.y >> 3) * lay::BS + (p.x >> 3)] |= std::uint64_t{1} << (((p.y & 7) << 3) | (p.x & 7));
}
inline void rclr(cambc::bitboard& b, vec2 p) {
    if (!inside(p)) return;
    b.ptr()[(p.y >> 3) * lay::BS + (p.x >> 3)] &= ~(std::uint64_t{1} << (((p.y & 7) << 3) | (p.x & 7)));
}
// Add every in-bounds tile within Chebyshev radius r of c.
inline void rbox(cambc::bitboard& b, vec2 c, int r) {
    for (int y = c.y - r; y <= c.y + r; ++y)
        for (int x = c.x - r; x <= c.x + r; ++x) rset(b, vec2{x, y});
}

constexpr int kFar = 30000;
// Distance read from a field; kFar when the tile was not reached (visited bit clear).
inline int rdist(const cambc::distance_field& f, const cambc::bitboard& visited, vec2 p) {
    if (!rbit(visited, p)) return kFar;
    return f.ptr()[(((p.y >> 3) * lay::FCW + (p.x >> 3)) << 6) | ((p.y & 7) << 3) | (p.x & 7)];
}

constexpr vec2 kDirs[8] = {vec2{0, 1}, vec2{1, 1}, vec2{1, 0}, vec2{1, -1},
    vec2{0, -1}, vec2{-1, -1}, vec2{-1, 0}, vec2{-1, 1}};

// Precomputed raw offsets of one tile in a bitboard and a distance field.
struct TileRef {
    std::uint32_t w, f;
    std::uint32_t b;
};
inline TileRef tref(vec2 p) {
    const std::uint32_t b = static_cast<std::uint32_t>(((p.y & 7) << 3) | (p.x & 7));
    return TileRef{static_cast<std::uint32_t>((p.y >> 3) * lay::BS + (p.x >> 3)),
        static_cast<std::uint32_t>((((p.y >> 3) * lay::FCW + (p.x >> 3)) << 6)) | b, b};
}
inline bool tbit(const std::uint64_t* v, const TileRef& r) { return (v[r.w] >> r.b) & 1; }
inline void tset(std::uint64_t* v, const TileRef& r) { v[r.w] |= std::uint64_t{1} << r.b; }
inline int tdist(const std::uint16_t* f, const std::uint64_t* v, const TileRef& r) {
    return tbit(v, r) ? f[r.f] : kFar;
}
