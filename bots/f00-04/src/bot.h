#pragma once

// The whole team hunts as one pack: shared memory holds the camp group the pack is attacking,
// every champion walks there, and big camps (golem, monolith) wait until the pack has gathered.
class Bot {
    bool initialised = false;

public:
    void play_turn();
};
