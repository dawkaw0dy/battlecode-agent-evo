#pragma once
// Draft memory layout shared by the draft bot and the champions.
#include <cstdint>

namespace plan {
inline constexpr int kMagic0 = 0;        // 'S'
inline constexpr int kMagic1 = 1;        // 'F'
inline constexpr int kSlots = 2;         // number of planned champions
inline constexpr int kGroups = 3;        // number of groups described (capped)
inline constexpr int kTypes = 4;         // champion_type per slot (5 bytes)
inline constexpr int kAltars = 16;       // home altar x, y per slot (10 bytes)
inline constexpr int kOwners = 32;       // one owner byte per camp group
inline constexpr int kMaxGroups = 990;

inline constexpr std::uint8_t kEnemySide = 0xFE;
inline constexpr std::uint8_t kNoOwner = 0xFF;
// Middle groups carry the owner slot with this bit set.
inline constexpr std::uint8_t kMiddleBit = 0x80;
}
