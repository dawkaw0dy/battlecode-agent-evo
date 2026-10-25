#include "fight.h"
#include <algorithm>

using T = cambc::champion_type;

namespace {
constexpr float kDeath = 450.0f;  // a death costs 60 rounds away plus the kill gold

// Fold this turn's damage into the lowest-health bodies first (focus fire).
void apply(std::vector<Body>& side, float dmg, std::vector<float>& lost, std::vector<char>& dead, int t) {
    while (dmg > 0.5f) {
        int k = -1;
        for (int i = 0; i < static_cast<int>(side.size()); ++i)
            if (!dead[i] && side[i].arrive <= t && (k < 0 || side[i].hp < side[k].hp)) k = i;
        if (k < 0) return;
        const float take = std::min(dmg, side[k].hp);
        side[k].hp -= take;
        lost[k] += take;
        dmg -= take;
        if (side[k].hp <= 0.5f) dead[k] = 1;
    }
}
}  // namespace

float champ_dps(T t, float str, float focus) {
    // Basic attacks, attack resets (one extra hit per 8 turns) and damage spells (per 8 turns).
    float reset = 0, spell = 0;
    switch (t) {
    case T::ryth: reset = 1; spell = 1.0f; break;
    case T::vesper_vale: reset = 1; break;
    case T::vendetta: reset = 1.5f; break;
    case T::cassi: spell = 2.0f; break;
    case T::issac: reset = 1; break;
    case T::stellar: spell = 2.0f; break;
    case T::prism: spell = 2.5f; break;
    case T::marcella: reset = 1; spell = 2.0f; break;
    case T::willow: reset = 1; break;
    case T::ren: spell = 0.75f; break;
    case T::varg: reset = 1; break;
    case T::tavi: reset = 1; spell = 1.4f; break;
    case T::heart_of_ruin: spell = 1.5f; break;
    }
    return str * (1.0f + reset / 8.0f) + focus * spell / 8.0f;
}

float champ_shield(T t, float focus) {
    if (t == T::varg) return 2.5f * focus;
    if (t == T::heart_of_ruin) return 1.5f * focus;
    return 0;
}

float fight_value(std::vector<Body> us, std::vector<Body> them, int turns) {
    std::vector<float> lost_us(us.size()), lost_them(them.size());
    std::vector<char> dead_us(us.size()), dead_them(them.size());
    for (int t = 0; t < turns; ++t) {
        float to_them = 0, to_us = 0;
        for (int i = 0; i < static_cast<int>(us.size()); ++i)
            if (!dead_us[i] && us[i].arrive <= t) to_them += us[i].dps;
        for (int i = 0; i < static_cast<int>(them.size()); ++i)
            if (!dead_them[i] && them[i].arrive <= t) to_us += them[i].dps;
        apply(them, to_them, lost_them, dead_them, t);
        apply(us, to_us, lost_us, dead_us, t);
    }
    float v = 0;
    for (std::size_t i = 0; i < them.size(); ++i) v += lost_them[i] + (dead_them[i] ? kDeath + them[i].stake : 0);
    for (std::size_t i = 0; i < us.size(); ++i) v -= lost_us[i] + (dead_us[i] ? kDeath + us[i].stake : 0);
    return v;
}
