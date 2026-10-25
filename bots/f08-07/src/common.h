#pragma once
// #define F0807_PROFILE
// Geometry and raw engine-buffer helpers shared by the draft and the champions.
#include <cambc.h>
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

using cambc::vec2;
using cambc::team;

inline int g_stage = 0;  // last code stage reached (diagnostics)
constexpr int kFar = 65535;  // unreached tile in a distance field
constexpr std::array<vec2, 8> kDirs{vec2{0, 1}, vec2{1, 1}, vec2{1, 0}, vec2{1, -1},
    vec2{0, -1}, vec2{-1, -1}, vec2{-1, 0}, vec2{-1, 1}};

inline int cheb(vec2 a, vec2 b) {
    const int dx = a.x > b.x ? a.x - b.x : b.x - a.x, dy = a.y > b.y ? a.y - b.y : b.y - a.y;
    return dx > dy ? dx : dy;
}

// Map size and raw layout, read once (globals: function-local statics cost a guard per call).
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

// Bitboards are stored as 8x8 chunks of one u64 each; distance fields as 8x8 chunks of 64 u16.
// Reading them in place avoids a host call per tile. Callers check inside().
inline bool raw_bit(const std::uint64_t* w, vec2 p) {
    return (w[(p.y >> 3) * g_stride + (p.x >> 3)] >> (((p.y & 7) << 3) | (p.x & 7))) & 1;
}
inline int raw_dist(const std::uint16_t* f, vec2 p) {
    return f[(((p.y >> 3) * g_fcw + (p.x >> 3)) << 6) | ((p.y & 7) << 3) | (p.x & 7)];
}
inline bool bit(const cambc::bitboard& b, vec2 p) { return inside(p) && raw_bit(b.ptr(), p); }

// A distance field kept in engine storage and read in place (kFar outside or unreached).
struct Field {
    std::unique_ptr<cambc::distance_field> f;
    const std::uint16_t* q = nullptr;
    void compute(const cambc::bitboard& walls, const cambc::bitboard& roots) {
        if (!f) f = std::make_unique<cambc::distance_field>();
        cambc::bitboard seen;
        f->compute(walls, roots, seen);
        q = static_cast<const cambc::distance_field&>(*f).ptr();
    }
    int at(vec2 p) const { return q && inside(p) ? raw_dist(q, p) : kFar; }
    bool ready() const { return q != nullptr; }
};

inline bool is_melee_type(cambc::champion_type t) {
    using T = cambc::champion_type;
    return t == T::ryth || t == T::vendetta || t == T::issac || t == T::marcella || t == T::ren || t == T::varg
        || t == T::heart_of_ruin;
}

// Fuel profiler (compiled in only with -DF0807_PROFILE edits; off in submissions).
#ifdef F0807_PROFILE
#include <iostream>
inline std::uint64_t g_prof[24];
inline std::uint32_t g_prof_n[24];
struct ProfScope {
    int i;
    std::uint64_t start;
    explicit ProfScope(int k) : i(k), start(cambc::fuel_remaining()) {}
    ~ProfScope() { g_prof[i] += start - cambc::fuel_remaining(); ++g_prof_n[i]; }
};
#define PROF(k) ProfScope prof_scope_##k(k)
inline void prof_dump(int round) {
    std::cout << "prof r" << round;
    for (int i = 0; i < 24; ++i)
        if (g_prof_n[i]) std::cout << ' ' << i << ':' << (g_prof[i] / g_prof_n[i]) << 'x' << g_prof_n[i];
    std::cout << '\n';
}
#else
#define PROF(k)
inline void prof_dump(int) {}
#endif
