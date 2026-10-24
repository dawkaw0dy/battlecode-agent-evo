#pragma once

// Per-champion state lives in bot.cpp; the class only dispatches the turn.
class Bot {
public:
    // Order: channels, shop and heal, enemies, retreat, tribute, boss raid, farm.
    void play_turn();
};
