#pragma once
#include <cambc.h>
#include <cstddef>
#include <cstdint>

// Map plan chosen at draft time and stored in draft memory, so every champion follows it.
struct Plan {
    std::uint8_t magic = 0;       // kPlanMagic when the draft wrote a plan
    std::uint8_t team_size = 5;   // champions per team
    std::uint8_t size_class = 1;  // 0 small (area <= 1600), 1 medium, 2 large (area >= 3600)
    std::uint8_t pack_x10 = 25;   // team-target score multiplier x10 (packing up vs parallel farming)
    std::uint8_t bank_gems = 30;  // carried gems that justify a trip to an altar
    std::uint8_t wary_bank = 15;  // the same with enemies seen recently
    std::uint8_t altar_dist = 20; // path length from our spawn to the closest altar (capped)
    std::uint8_t groups = 0;      // reachable camp groups without a monolith (capped)
    std::uint8_t camp_gems = 0;   // gem value of reachable camp groups / 4 (capped)
    std::uint8_t draft_sturdy = 0; // 1: e04-04's sturdy farmer draft, 0: the spell pack
    std::uint8_t zones = 0;       // territories (one per champion) when the team spreads out, else 0
    std::uint8_t zone_x10 = 10;   // score multiplier x10 for groups in the champion's own territory
    std::uint16_t zone_count = 0; // camp groups with a stored territory byte (after the Plan in memory)
};

constexpr std::uint8_t kPlanMagic = 0xE7;
constexpr std::uint8_t kZoneNone = 0xFF;        // territory byte: no owner
constexpr std::uint8_t kZoneUnreachable = 0xFE; // territory byte: not walkable from our spawn

// The draft's plan, or defaults when the draft memory holds none. During the draft: the analysed plan.
const Plan& plan();
// Analyse the map and store the plan (draft phase only).
void write_plan();
// Territory of camp group `index` (0..zones-1), or -1 when the plan has none for it.
int group_zone(std::size_t index);
// True when the draft found no walking path from our spawn to camp group `index`.
bool group_unreachable(std::size_t index);
// This champion's territory: its draft-order index among our champions, modulo plan().zones.
int my_zone();
