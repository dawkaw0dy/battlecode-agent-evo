#include "fight.h"
#include <algorithm>

namespace {

constexpr float kKillValue = 350.0f;   // a death costs the respawn, kill gold and tempo
constexpr float kGemValue = 2.0f;      // carried gems change hands on a kill

int delay(const SimUnit& u, const std::vector<SimUnit>& foes) {
    int d = 99;
    for (const auto& f : foes) d = std::min(d, cheb(u.pos, f.pos));
    const int walk = std::max(0, d - u.range);
    return u.channel ? 99 : walk;
}

}  // namespace

SimResult simulate(const std::vector<SimUnit>& ours, const std::vector<SimUnit>& theirs, int turns) {
    std::vector<float> hp_o, hp_t;
    std::vector<int> wait_o, wait_t;
    for (const auto& u : ours) { hp_o.push_back(u.hp); wait_o.push_back(delay(u, theirs)); }
    for (const auto& u : theirs) { hp_t.push_back(u.hp); wait_t.push_back(delay(u, ours)); }
    SimResult r{0, false, 0, 0};
    auto weakest = [](const std::vector<float>& hp) {
        int best = -1;
        for (int i = 0; i < static_cast<int>(hp.size()); ++i)
            if (hp[static_cast<std::size_t>(i)] > 0 && (best < 0 || hp[static_cast<std::size_t>(i)] < hp[static_cast<std::size_t>(best)])) best = i;
        return best;
    };
    for (int t = 0; t < turns; ++t) {
        // Both sides strike at once: damage computed from the state at the start of the turn.
        float to_t = 0, to_o = 0;
        for (std::size_t i = 0; i < ours.size(); ++i)
            if (hp_o[i] > 0 && wait_o[i] <= t) to_t += ours[i].dps;
        for (std::size_t i = 0; i < theirs.size(); ++i)
            if (hp_t[i] > 0 && wait_t[i] <= t) to_o += theirs[i].dps;
        // Focus fire on the weakest; spill over to the next one.
        while (to_t > 0) {
            const int k = weakest(hp_t);
            if (k < 0) break;
            const float h = hp_t[static_cast<std::size_t>(k)];
            const float d = std::min(h, to_t);
            hp_t[static_cast<std::size_t>(k)] -= d;
            to_t -= d;
            r.value += d;
            if (hp_t[static_cast<std::size_t>(k)] <= 0) {
                ++r.their_deaths;
                r.value += kKillValue + kGemValue * static_cast<float>(theirs[static_cast<std::size_t>(k)].gems);
            }
        }
        while (to_o > 0) {
            const int k = weakest(hp_o);
            if (k < 0) break;
            const float h = hp_o[static_cast<std::size_t>(k)];
            const float d = std::min(h, to_o);
            hp_o[static_cast<std::size_t>(k)] -= d;
            to_o -= d;
            r.value -= d;
            if (hp_o[static_cast<std::size_t>(k)] <= 0) {
                ++r.our_deaths;
                if (k == 0) r.we_die = true;
                r.value -= kKillValue + kGemValue * static_cast<float>(ours[static_cast<std::size_t>(k)].gems);
            }
        }
        if (weakest(hp_t) < 0 || weakest(hp_o) < 0) break;
    }
    return r;
}
