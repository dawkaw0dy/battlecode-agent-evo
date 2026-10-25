#pragma once
// What each of our two ability slots does, read once from the engine's ability metadata.
#include <cambc.h>
#include <array>

struct SpellSlot {
    bool used = false;
    cambc::ability_kind kind = cambc::ability_kind::attack_reset;
    float range = 0, radius = 0;
    bool excl = false;      // excludes walking: cast before moving, and no walk after
    bool enemies = false, camps = false, ground = false, allies = false, self = false;
    bool stun = false;      // crowd control (damage and a one-round stun)
    bool enhanced = false;  // enhanced attack reset: the next basic attack stuns
    bool hits() const {
        return kind == cambc::ability_kind::damage || kind == cambc::ability_kind::cc
            || kind == cambc::ability_kind::aoe_damage;
    }
};
using Kit = std::array<SpellSlot, 2>;
Kit read_kit();
