#pragma once

// Per-champion state survives between turns.
class Bot {
public:
    // Shop, fight or flee, bank gems, then farm with the group.
    void play_turn();
};
