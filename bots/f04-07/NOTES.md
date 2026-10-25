## Strategy (≤10 lines)
Vision-ambush immigrant, written from the starter's infrastructure (main loop, random.h) only.
Draft Vendetta, Vesper Vale, Varg, Tavi, Ryth (burst, resets, dashes, stuns). Two squads (index % 2) share a camp
target through shared memory bytes 0–1; each champion scores groups by value/(travel + 2·respawn wait + clear time)
with a camp-danger filter, captures rewards (never standing on a spawn tile while waiting: that blocks the respawn),
and banks at altars (≥25 gems, or ≥10 within 12 tiles, or late / winning bag). PvP: square law (Σhp·Σdps) over
visible champions; fight when ahead (×1.3, less vs channelling/lone/gem carriers), never chase into the enemy half;
flee home only when behind. Sightings (pos, round, gems, fighting/alone flags) go to shared bytes 2–5; healthy
champions without gems answer fights/carriers on our half. Wards every 10 rounds on unseen bushes or towards the
goal; idle champions lie in bushes near the altar enemies were seen at. Altar locks bought and placed on every altar.

## Key parameters
kSquads = 2, kFightNeed = 1.3, kFleeRadius = 7, kChaseMargin = 4 (own_side margin), kRetreatHp = 0.35,
kBankGems = 25, kCaptureTimeout = 14 (then skip the group 40 rounds), kGatherTurns = 12, kHealHp = 0.75,
kDangerPool = 0.7 (expected camp damage vs squad hp), kTankRounds = 5, kJungleShards = 5 (then power str/str/hp).

## Changes this generation
All bench2.py, 0 faults / 0 bot errors in every run after v2.
- v1 (first working): vs starter 6/8 (4 maps). vs f03-05 0/12 (6 maps), margin −873.
- Stall fixes: approach only living camps; wait for respawns off the spawn tiles (we blocked respawns forever);
  "favourable but out of bounds" no longer means flee. vs f03-05 1/12 (−724).
- 1 squad (−824) and solo farming (−774) both worse than 2 squads (−638).
- Relaxed camp danger, bank at 25: vs f00-03 4/16 (−336, 8 maps); capture timeout: 6/16 (−192).
- Final: vs f00-03 6/16 (−192), f01-07 3/16 (−354), f02-01 2/16 (−540), starter 14/16 (+697).

## Tried and failed (keep entries from the parent's notes; add yours)
- (parent = starter, no inherited entries)
- One squad of 5: waits on tiny respawns near spawn, 2/16 vs f00-03 (−490) vs 6/16 with two squads.
- Solo farming (no shared target): worse than two squads vs f03-05.
- kFightNeed 1.0: 3/16 vs 4/16 at 1.3. kGatherTurns 6: 5/16 vs 6/16. kDangerPool 0.9: same margin, 5/16.
- No jungle emblem (power shards from the start): 3/16 (−450) vs 6/16 (−192). Camp damage is the farming limit.
- Chasing sightings anywhere led squads into the enemy spawn; limit answers and chases to our half.
- camp_tiles() includes dead camps; use awake | asleep tiles for targets.

## Known weaknesses
- Farming is ~2× slower than the lineage bots (most turns are "approach"); f03-05 wins every game but one.
- No multi-trip wear-down of strong groups, no boss raids; squads split only by index, not by map region.
- Fight odds ignore unseen enemies and gold advantage; strong lineage carries (Vesper Vale) still pick us off.
