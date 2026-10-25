#pragma once

// Per-champion state survives between turns; the shared world model lives in bot.cpp.
class Bot {
public:
    // Order: shop, fight or flee, bank, recall, then farm the best camp group.
    void play_turn();
};
