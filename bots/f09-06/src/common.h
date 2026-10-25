#pragma once
// Shared helpers: map geometry, in-place reads of engine buffers, small math.
#include <cambc.h>
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

using cambc::vec2;
using cambc::team;

constexpr int kFar = 65535;  // unreached distance in a computed field
constexpr std::array<vec2, 8> kDirs{vec2{0, 1}, vec2{1, 1}, vec2{1, 0}, vec2{1, -1},
    vec2{0, -1}, vec2{-1, -1}, vec2{-1, 0}, vec2{-1, 1}};

inline int cheb(vec2 a, vec2 b) {
    const int dx = a.x > b.x ? a.x - b.x : b.x - a.x, dy = a.y > b.y ? a.y - b.y : b.y - a.y;
    return dx > dy ? dx : dy;
}

// Map size and the engine's 8x8 chunk layout, set once by init_dims().
inline int g_w = 0, g_h = 0, g_bstride = 0, g_fcw = 0;
inline void init_dims() {
    const auto& l = cambc::detail::layout();
    g_w = l.width;
    g_h = l.height;
    g_bstride = static_cast<int>(l.chunk_stride);
    g_fcw = static_cast<int>(l.chunked_width);
}
inline bool inside(vec2 p) {
    return static_cast<unsigned>(p.x) < static_cast<unsigned>(g_w) && static_cast<unsigned>(p.y) < static_cast<unsigned>(g_h);
}

// Bitboards and fields are stored in our memory: read them without a host call. Callers check inside().
inline bool raw_bit(const std::uint64_t* w, vec2 p) {
    return (w[(p.y >> 3) * g_bstride + (p.x >> 3)] >> (((p.y & 7) << 3) | (p.x & 7))) & 1;
}
inline int raw_dist(const std::uint16_t* f, vec2 p) {
    return f[(((p.y >> 3) * g_fcw + (p.x >> 3)) << 6) | ((p.y & 7) << 3) | (p.x & 7)];
}
inline bool bit(const cambc::bitboard& b, vec2 p) { return inside(p) && raw_bit(b.ptr(), p); }

// A distance field kept in engine storage.
struct Field {
    std::unique_ptr<cambc::distance_field> f;
    int stamp = -1000;  // round computed
    void compute(const cambc::bitboard& walls, const cambc::bitboard& roots, int max_d = kFar) {
        if (!f) f = std::make_unique<cambc::distance_field>();
        cambc::bitboard seen;
        f->compute(walls, roots, seen, static_cast<std::uint16_t>(max_d));
    }
    int at(vec2 p) const {
        if (!f || !inside(p)) return kFar;
        return raw_dist(static_cast<const cambc::distance_field&>(*f).ptr(), p);
    }
    bool ready() const { return f != nullptr; }
};

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Fuel profile of the current turn (debug runs print it).
inline std::uint64_t g_prof[8] = {};
inline int g_prof_n = 0;
inline void prof_mark() { if (g_prof_n < 8) g_prof[g_prof_n++] = cambc::fuel_remaining(); }
