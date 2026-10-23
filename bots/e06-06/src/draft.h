#pragma once
#include <cstdint>

constexpr std::uint8_t kPlanMagic = 0xE6;
constexpr int kOpeningMargin = 3;   // opening group must be at least this much closer to us than to the enemy

void play_draft();
