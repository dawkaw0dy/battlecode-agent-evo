#pragma once
#include <cstddef>

// Draft memory layout for the region plan (bytes; 0xFF means unset).
namespace plan {
constexpr int kSize = 1024;
constexpr int kMagic = 0;       // 'S' when a plan was written
constexpr int kRegions = 1;     // number of regions
constexpr int kSlotOf = 2;      // 12 bytes: region slot per champion_draft value
constexpr int kDraftCount = 12;
constexpr int kAltar = 14;      // 2 bytes (x, y) per region: the region's altar
constexpr int kMaxRegions = 8;
constexpr int kGroupCount = 31; // camp group count (low byte), sanity check
constexpr int kGroupRegion = 32; // one byte per camp group
} // namespace plan
