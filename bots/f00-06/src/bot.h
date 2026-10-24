#pragma once

// Deathball: one leader picks camp groups, the rest of the team stays within a few tiles of it
// and fights only together. Per-champion state survives between turns.
class Bot {
public:
    // Shop, keep the ball together, fight, bank and capture.
    void play_turn();
};
