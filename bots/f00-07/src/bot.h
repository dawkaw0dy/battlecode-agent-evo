#pragma once

// Name of the decision block running now; main.cpp prints it with any caught error.
extern const char* g_phase;

// Per-champion bot: vision-ambush farmer. State lives in bot.cpp's anonymous namespace.
class Bot {
public:
    // Shop, recall, fight (ambush isolated enemies), bank gems, farm camps, ward approaches.
    void play_turn();
};
