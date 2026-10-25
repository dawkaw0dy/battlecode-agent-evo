#pragma once

// Per-champion state lives in bot.cpp (each champion runs its own instance).
class Bot {
public:
    // Shop, fight or flee, bank gems, capture camps, ambush carriers and ward approaches.
    void play_turn();
};
