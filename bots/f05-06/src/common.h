#pragma once
// Shared helpers: distances, raw (in-place) reads of engine bitboards and distance fields, draft memory layout.
#include <cambc.h>
#include <cstdint>
#include <vector>

using cambc::vec2;
using cambc::team;

inline int cheb(vec2 a, vec2 b) {
    int dx = a.x - b.x, dy = a.y - b.y;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    return dx > dy ? dx : dy;
}

// Engine boards are stored in 8x8 chunks: one 64-bit word per chunk, bit (y&7)*8+(x&7).
// Distance fields use the same chunking with one uint16 per tile. The strides are calibrated
// against host reads once; on a mismatch every read goes through the host instead.
struct Raw {
    static inline int W = 0, H = 0, bcw = 0, fcw = 0;
    static inline bool bok = false, fok = false;

    static void calibrate();

    static bool bit(const cambc::bitboard& b, vec2 p) {
        if (bok) return (b.ptr()[(p.y >> 3) * bcw + (p.x >> 3)] >> ((p.y & 7) * 8 + (p.x & 7))) & 1;
        return b.is_set(p);
    }
    static std::uint16_t dist(const cambc::distance_field& f, vec2 p) {
        if (fok) return f.ptr()[((p.y >> 3) * fcw + (p.x >> 3)) * 64 + (p.y & 7) * 8 + (p.x & 7)];
        return f.get(p);
    }
    static bool in(vec2 p) { return p.x >= 0 && p.y >= 0 && p.x < W && p.y < H; }
};

constexpr std::uint16_t kFar = 65535;

// Draft memory layout (written by the draft bot, read by every champion).
namespace dm {
constexpr int kMagic0 = 0, kMagic1 = 1, kSlots = 2, kGroupsLo = 3, kGroupsHi = 4;
constexpr int kSlotDraft = 8;    // 5 bytes: champion_draft per slot
constexpr int kSlotAltar = 13;   // 5 bytes: home altar index per slot
constexpr int kGroupOwner = 24;  // one byte per camp group: slot, kShared or kEnemySide
constexpr std::uint8_t kShared = 250, kEnemySide = 251;
constexpr int kMaxGroups = 1024 - kGroupOwner;
}

// champion_type -> champion_draft (Cassi and Issac share one pick).
inline int draft_of(cambc::champion_type t) {
    int v = static_cast<int>(t);
    if (v <= 2) return v;
    if (v == 3 || v == 4) return 3;
    return v - 1;
}

// Last action attempted (reported with any uncaught error).
inline const char* g_where = "";
