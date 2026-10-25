#pragma once
// Small helpers: geometry, raw reads of engine buffers, warnings.
#include <cambc.h>
#include <array>
#include <cstdint>
#include <exception>
#include <iostream>

using cambc::vec2;
using cambc::team;

constexpr int kFar = 65535;
constexpr std::array<vec2, 8> kDirs{vec2{0, 1}, vec2{1, 1}, vec2{1, 0}, vec2{1, -1},
    vec2{0, -1}, vec2{-1, -1}, vec2{-1, 0}, vec2{-1, 1}};

inline int cheb(vec2 a, vec2 b) {
    const int dx = a.x > b.x ? a.x - b.x : b.x - a.x, dy = a.y > b.y ? a.y - b.y : b.y - a.y;
    return dx > dy ? dx : dy;
}

// Map size and buffer layout, set once at start.
inline int g_w = 0, g_h = 0, g_stride = 0, g_fcw = 0;
inline void init_dims() {
    const auto& l = cambc::detail::layout();
    g_w = l.width;
    g_h = l.height;
    g_stride = static_cast<int>(l.chunk_stride);
    g_fcw = static_cast<int>(l.chunked_width);
}
inline bool inside(vec2 p) {
    return static_cast<unsigned>(p.x) < static_cast<unsigned>(g_w) && static_cast<unsigned>(p.y) < static_cast<unsigned>(g_h);
}
inline int tidx(vec2 p) { return p.y * g_w + p.x; }

// Engine buffers are 8x8 chunks; read them in place (a host call per tile costs more).
inline bool raw_bit(const std::uint64_t* w, vec2 p) {
    return (w[(p.y >> 3) * g_stride + (p.x >> 3)] >> (((p.y & 7) << 3) | (p.x & 7))) & 1;
}
inline void raw_set(std::uint64_t* w, vec2 p) {
    w[(p.y >> 3) * g_stride + (p.x >> 3)] |= std::uint64_t{1} << (((p.y & 7) << 3) | (p.x & 7));
}
inline void raw_clear(std::uint64_t* w, vec2 p) {
    w[(p.y >> 3) * g_stride + (p.x >> 3)] &= ~(std::uint64_t{1} << (((p.y & 7) << 3) | (p.x & 7)));
}
inline int raw_dist(const std::uint16_t* f, vec2 p) {
    return f[(((p.y >> 3) * g_fcw + (p.x >> 3)) << 6) | ((p.y & 7) << 3) | (p.x & 7)];
}
inline bool bit(const cambc::bitboard& b, vec2 p) { return inside(p) && raw_bit(b.ptr(), p); }
inline void set_bit(cambc::bitboard& b, vec2 p) { if (inside(p)) raw_set(b.ptr(), p); }
inline void clear_bit(cambc::bitboard& b, vec2 p) { if (inside(p)) raw_clear(b.ptr(), p); }
// Distance read that treats tiles the search did not reach as far.
inline int fdist(const cambc::distance_field& f, const cambc::bitboard& seen, vec2 p) {
    if (!inside(p) || !raw_bit(seen.ptr(), p)) return kFar;
    return raw_dist(f.ptr(), p);
}

// A caught engine error that we survive: logged as a warning, never as a bot error.
inline int g_warns = 0;
inline void warn(const char* where, const std::exception& e) {
    if (g_warns++ < 20) std::cout << "warn: " << where << ": " << e.what() << '\n';
}
