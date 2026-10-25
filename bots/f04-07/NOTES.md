## Strategy (≤10 lines)
Vision-ambush immigrant, written from the starter's infrastructure (main loop, random.h) only.
Draft Vendetta, Vesper Vale, Varg, Tavi, Ryth (burst, resets, dashes, stuns). Two squads (index % 2) share a camp
target in shared bytes 0–1; groups are scored by value/(travel + 2·respawn wait + clear time) with a camp-danger
filter; one squad member captures a defeated group while the rest move on; nobody waits on a spawn tile (that
blocks the respawn). Bank at ≥25 gems (≥10 within 12 tiles, late, or a winning bag); recall to shop at 180 gold.
PvP: square law (Σhp·Σdps) over visible champions; fight when ahead ×1.3 (less vs channelling, lone or gem-carrying
targets), never chase into the enemy half, flee home only when behind. Sightings (pos, round, gems, fighting/alone)
in shared bytes 3–6; healthy gem-less champions answer fights and carriers on our half. Wards every 10 rounds on
unseen bushes or towards the goal; idle champions wait in bushes near altars enemies used. Altar locks everywhere.

## Key parameters
kSquads 2, kFightNeed 1.3, kFleeRadius 7, kChaseMargin 4, kRetreatHp 0.35, kCampRetreatRounds 5 (leave when the
awake camps targeting us — we are their nearest champion — would kill us in 5 rounds), kBankGems 25,
kCaptureTimeout 14 (then skip the group 40 rounds), kGatherTurns 12, kHealHp 0.75, kDangerPool 2.5,
kTankRounds 3, kJungleShards 5 (then power str/str/hp), kShopGold 180, kSplitCapture on.

## Changes this generation
bench2.py throughout; 0 faults and 0 `bot error:` lines in every run after the first. "speed" = mean rounds to
1000 vs starter on 8 maps (2000 + missing gems when not reached), deterministic, lower is better.
- v1: vs starter 6/8, vs f03-05 0/12 (−873). Stall fixes (approach living camps only, never wait on spawn
  tiles, favourable-but-out-of-bounds ≠ flee, chase only on our half): vs f03-05 1/12 (−724).
- Squads: 1 squad −824, solo −774, 2 squads −638 vs f03-05; speed 3 squads 1814 vs 2 squads 1693.
- Capture timeout + bank at 25: vs f00-03 6/16 (−192, 8 maps). Split capture: speed 1779 → 1693.
- Fuel: turns hit ~100k on 100×100 maps (up to 135 turns/game > 90k). Per-turn full-map BFS for target choice
  replaced by a static spawn field (travel ≈ max(Chebyshev, |Δ spawn distance|)); wait rings and approach tiles
  precomputed at init. Now 0 turns > 90k, max ~89k. Speed 1624 → 1586.
- Recall to shop at 180 gold: speed 1542. Camp-aware retreat: speed 1616 but vs f00-03 8/32 → 10/32 (−246 → −192).
- Final: vs f00-03 10/32 (−192, 16 maps), vs f03-05 0/16 (−667), vs starter 16/16 (+854).

## Tried and failed (keep entries from the parent's notes; add yours)
- (parent = starter, no inherited entries)
- One squad of 5: waits on tiny respawns near spawn, 2/16 vs f00-03 (−490) vs 6/16 with two squads. Solo worse too.
- Other-squad target penalty ×0.3 (spread squads): 2/16 vs 6/16.
- kFightNeed 1.0: 3/16 vs 4/16. kGatherTurns 6: 5/16 vs 6/16. Ranged approach radius 3: speed 1789 vs 1693.
- No jungle emblem (power shards from the start): 3/16 (−450) vs 6/16 (−192). Camp damage limits farming.
- Danger pool 0.7 vs 2.5 with the fuel fixes: 7/32 vs 9/32 vs f00-03, and slower.
- Fight caution when alone (need ×1.6 without a mate within 6): 4/32 (−429) vs 10/32 (−192).
- Path-field cache reused across turns (static terrain, recomputed per goal key): more fuel overruns, not fewer;
  the guest-side loop marking unvisited tiles (10k iterations) costs ~60k. Exported fields read 0xFFFF unreached.
- Host BFS fuel grows with the tiles expanded: a full-map distance_field.compute costs ~50k on 100×100.
- camp_tiles() includes dead camps: use awake | asleep tiles for targets.

## Known weaknesses
- Farming still ~2× slower than the lineage bots; f03-05 wins every game, f00-03 about 2 in 3.
- No multi-trip wear-down of strong groups, no boss raids; squads are split by index, not by map region.
- Small contested maps (hooks, packs, swirl): many PvP deaths; fight odds ignore unseen enemies.
