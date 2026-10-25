#include "common.h"

void Raw::calibrate() {
    W = cambc::map::width();
    H = cambc::map::height();
    const int rows = (H + 7) / 8;
    bcw = static_cast<int>(cambc::bitboard::raw_buffer_size() / 8 / rows);
    fcw = static_cast<int>(cambc::distance_field::raw_buffer_size() / 2 / 64 / rows);
    bok = fok = false;

    const vec2 probes[] = {{0, 0}, {W - 1, H - 1}, {W - 1, 0}, {0, H - 1}, {W / 2, H / 3}, {W / 3 + 1, H / 2 + 1}};
    cambc::bitboard b;
    bool good = bcw > 0;
    for (const vec2 p : probes) {
        if (!good) break;
        b.clear();
        b.set(p);
        bok = true;
        good = bit(b, p);
        const vec2 q{p.x == 0 ? 1 : p.x - 1, p.y};
        if (good && in(q)) good = !bit(b, q);
        const vec2 r{p.x, p.y == 0 ? 1 : p.y - 1};
        if (good && in(r)) good = !bit(b, r);
        bok = false;
    }
    bok = good;

    // Field check: an empty map from the origin gives Chebyshev distances.
    if (fcw <= 0) return;
    cambc::bitboard walls, roots, visited;
    walls.clear();
    roots.clear();
    roots.set({0, 0});
    cambc::distance_field f;
    f.compute(walls, roots, visited, 12);
    fok = true;
    good = true;
    for (int y = 0; y < 12 && y < H && good; y += 3)
        for (int x = 0; x < 12 && x < W && good; x += 2) {
            const vec2 p{x, y};
            good = dist(f, p) == static_cast<std::uint16_t>(x > y ? x : y);
        }
    fok = good;
}
