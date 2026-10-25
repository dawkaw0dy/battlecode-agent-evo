// Dispatch draft and gameplay; a failed turn is logged and still yields.
#include <cambc.h>
#include <cstdint>
#include <iostream>
#include "bot.h"
#include "draft.h"

// Measurement builds only: per-champion histogram of fuel per turn in stdout (prints cost fuel too).
constexpr bool kFuelProfile = false;

int main() {
    Bot bot;
    std::uint64_t hist[6] = {0, 0, 0, 0, 0, 0};  // <10k <30k <60k <90k, more, cut short
    std::uint64_t total = 0, worst = 0;
    while (true) {
        const std::uint32_t r0 = kFuelProfile ? cambc::round_number() : 0;
        const std::uint64_t f0 = kFuelProfile ? cambc::fuel_remaining() : 0;
        try {
            if (cambc::is_draft())
                play_draft();
            else
                bot.play_turn();
        } catch (const std::exception& error) {
            std::cout << "bot error: " << error.what() << std::endl;
        } catch (...) {
            std::cout << "bot error: unknown exception" << std::endl;
        }
        if (kFuelProfile && !cambc::is_draft()) {
            const std::uint32_t r1 = cambc::round_number();
            const std::uint64_t f1 = cambc::fuel_remaining(), used = f0 > f1 ? f0 - f1 : 0;
            if (r1 != r0) ++hist[5];
            else {
                ++hist[used < 10000 ? 0 : used < 30000 ? 1 : used < 60000 ? 2 : used < 90000 ? 3 : 4];
                total += used;
                if (used > worst && r0 > 1) worst = used;
            }
            if (r1 % 250 == 0 || r1 >= 1995) {
                std::cout << "fuelprof r" << r1 << ' ' << cambc::to_string(cambc::self().champion_name());
                for (auto h : hist) std::cout << ' ' << h;
                std::cout << " avg " << total / (1 + hist[0] + hist[1] + hist[2] + hist[3] + hist[4]) << " worst " << worst << '\n';
            }
        }

        cambc::end_turn();
    }
}
