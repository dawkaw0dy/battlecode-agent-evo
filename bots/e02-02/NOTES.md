## Strategy (≤10 lines)
Scaling economy, built from the starter infrastructure (main loop, distance-field pathing).
Draft (rewritten this generation): map-aware ratings of what this bot actually uses (basic attacks, simple damage/stun
spells, shields, heals; no dashes or resets). Base order Vendetta, Vesper Vale, Willow, Marcella, Ryth, C&I, Tavi, Varg;
elk/golem share raises Vendetta/Marcella/Ryth, share of 3+-monster groups raises Ryth/Varg, 3-pick maps raise C&I.
Every camp group is scored by (3*gems + gold) / (path distance + estimated clear turns + 8); clear time uses
own Strength + 0.2*Focus times nearby allies. Groups whose estimated camp damage exceeds 75% of current HP are
scaled down x0.05. Monolith groups are never targeted; sleeping golems are not woken below Strength 90.
Team target in shared memory (u16 at offset 0) x1.5, own target x1.3. Shop: Treasure, Jungle, then Power shards.
Gems banked at >= 20 (or >= 5 near an altar, late game); recall < 35% HP, recall to shop at >= 150 gold when safe.

## Key parameters
Draft ratings: Vendetta 10 (+2*heavy share), Vesper 9.6, Willow 9.4, Marcella 9 (+2*heavy), Ryth 8.4 (+3*big share
+heavy), C&I 9.3 on 3-pick maps else 7, Tavi 7.5, Varg 7.2 (+4*big share), Ren 5, Stellar 4, HoR 3, Prism 2.
kBankGems 20, kShopGold 150, recall 35% HP, spawn heal to 90%, danger 0.75*HP, stuck skip 12 turns -> 50 rounds.

## Changes this generation
Parent e01-01, mutation: subsystem rewrite, focus opening (draft). Rest of the bot unchanged; draft memory unused.
Parent and e00-01 had the identical fixed draft (Vesper, Marcella, Vendetta, Varg, Ryth, ...).
- Parent baseline vs e00-01 on 01hooks 00gem-fields 00gemfight 01starfish 00split 00lost-world: 5/12, -120.8 mean.
  New draft v1 (Willow 8.0): 9/12, +245.5 on the same maps; all 40 maps vs e00-01: 45/79 valid, +77.5 mean.
- v1 -> Willow 9.4 (kept), 12-map set (00packs 00quadrants 00rows 01corridors 01broken-curve 01shortcut 00gemfight
  01hooks 00gem-fields 00lanes 00fragment 01starfish): mean -81 -> +90.1, 13/24 vs e00-01.
- Validation of final: vs e01-01 on that 12-map set 15/24, +121.5; vs starter 15/16 (8 maps). No faults, no bot errors
  from this bot (the 2 caught exceptions seen were e00-01's).

## Tried and failed (keep entries from the parent's notes; add yours)
- Starter step selection `field.get(tile) < field.get(step)` picks blocked/unreached tiles (they read 0): "Known occupied
  destination". Fix: only step to tiles that are free and in `visited`.
- Iterating `vision::champions()/camps()` handles and calling health()/is_alive() throws "No visible entity" when the
  unit died this turn; scan `champion_tiles()/camp_tiles()` and use `champion_at/camp_at` instead.
- Attack-reset abilities throw "An available attack cannot be reset" in cases `can_attack()` does not reveal; resets disabled.
- After a move returns false (hidden unit), moving again the same turn throws: stop walking for the turn.
- Hard danger filter (skip groups) left champions idle on 01big-world/00lanes; softened to a x0.05 score factor (small gain).
- e01-01: Tribute before fighting when next to an unlocked altar with >= 5 gems: +761 vs +1331 (lost 00lanes B). Reverted.
- e01-01: Recall at 45% HP: 7/12 but -347 total margin (less farming time). Reverted.
- e01-01: Recall at 28% HP: 6/12, +961, below the 35% baseline. Reverted.
- e02-02: Cassi & Issac rated high on every map (10.5 on 3-pick, 9.3 otherwise): 12-map set -96.5 mean vs -81 for v1;
  00fragment -908. Reverted.

## Known weaknesses
- 00packs (-670), 00rows (-322), 01broken-curve, 01shortcut still lose vs e00-01 on both sides; not draft-related as far
  as tested. 00contest, 00stampede, 01boss-battle, 01yellow-brick-road end 0-0 (tiebreak).
- Draft ignores enemy picks; draft memory unused; no planned opening route (first targets come from choose_group).
- No dash travel, no attack resets, no wards, no altar locks, no deliberate fights beyond enemies in range.
