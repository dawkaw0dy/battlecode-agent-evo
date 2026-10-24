## Strategy (≤10 lines)
Early aggression, built from the starter's infrastructure (main loop, draft/bot split).
Draft: Vendetta, Ryth, Marcella, Vesper Vale, Tavi, Heart of Ruin, ... (sturdy melee with resets/dashes/stuns first).
The whole team follows one shared camp-group target (shared memory byte 0). Targets are scored by value / (path distance + clear time).
Each champion scans visible champions every turn and compares local power (sum HP x sum DPS, Lanchester-style).
It engages when the ratio is >= 1.0, when the target is killable, or when the target is under 35% HP; it retreats below a ratio of 0.65.
Engage: walking-exclusive stun/damage spells before moving, basic attack, then attack reset. Melee champions dash next to the target to chase it.
Flee: step away towards spawn (home distance field), and recall when hurt with no enemy within 6 tiles.
Banking at 25 gems, near an altar, or late in the game. Enemy altar locks are attacked. Shards: power strength x10, then durability/power health.

## Key parameters
kEngageRatio 1.0, kHoldRatio 0.65, kFleeHp 0.25, kRecallHp 0.35, kBankGems 25, kEnemyMemory 10 (spells kept for champions after a sighting).
Group score value = gems + 0.35 gold; monoliths skipped; golems only after round 250, and attacked only with 3 or more allies near.

## Changes this generation
- New bot (immigrant, early-aggression archetype) from bots/starter.
- Own pathing over a static-wall distance field: only steps to tiles that are free and that the field reached (avoids the starter's bug).
- Results (bench2, release, both sides): 12 maps vs starter 18/24 wins, mean margin +353, 0 faults, 3 caught `bot error:` lines (cause not found).
  Losses: 01gold-rush (11 gems tributed), 01yellow-brick-road (0 gems), 00corners/00contest side A.

## Tried and failed (keep entries from the parent's notes; add yours)
- Reading `self().position()` while the champion is dead throws engine error 5 ("Invalid metadata, alignment, overlap or argument"),
  once per dead turn. Check `is_alive()` first.

## Known weaknesses
- 3 `bot error:` lines remain in 24 games (00corners, 00gemfight); not investigated for lack of time.
- 01gold-rush / 01yellow-brick-road: almost no gems are tributed, likely because of the target choice, golem/monolith filtering or banking. Not investigated.
- 00contest: neither side tributes (0-0), so the result comes down to tiebreaks.
- A strict deathball shares every capture five ways. Splitting into squads was not tried. Fight thresholds are not tuned.
