#pragma once
#include <cstddef>
#include <cstdint>

// Draft memory layout (written by the draft, read-only in play).
inline constexpr std::uint8_t kMagic = 0xE7;
inline constexpr std::size_t kPlanMagic = 0;     // kMagic when the plan is valid
inline constexpr std::size_t kPlanRouteLen = 1;  // number of route entries
inline constexpr std::size_t kPlanRoute = 2;     // camp group index + 1, in clearing order
inline constexpr int kRouteMax = 8;
inline constexpr std::size_t kPlanAltarLen = 10; // number of altar entries
inline constexpr std::size_t kPlanAltars = 11;   // altar index + 1 (map::altars() order), nearest first
inline constexpr int kAltarMax = 16;
inline constexpr std::size_t kPlanBytes = 32;

void play_draft();
