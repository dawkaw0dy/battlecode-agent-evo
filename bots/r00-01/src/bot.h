#pragma once
#include <cambc.h>
#include <optional>
#include <variant>
#include <vector>

// Per-champion state survives between turns.
class Bot {
    static constexpr unsigned kBankGems = 25;  // carried gems that justify a trip to an altar
    static constexpr unsigned kShopGold = 150; // gold that justifies a recall to shop

    bool moved = false;          // walked this turn
    bool cast_exclusive = false; // cast a spell that forbids walking this turn
    bool banking = false;        // committed to tributing the carried gems
    std::vector<int> skip_until; // per camp group: round until which it is known to be respawning

    void buy_items();
    bool move_towards_closest(cambc::bitboard goals);
    bool move_away_from(const std::vector<cambc::vec2>& threats);
    std::optional<std::variant<cambc::champion, cambc::camp>> pick_target();
    bool camp_allowed(cambc::camp_type kind) const;
    template<class Target> void fight(const Target& target);
    bool fight_best_target();
    void heal_allies();
    std::vector<cambc::vec2> threats();
    bool bank_gems();
    void farm_camps();

public:
    // Priorities: shop and recall, heal, fight enemies in range, bank gems, then farm.
    void play_turn();
};
