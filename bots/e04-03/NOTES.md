## Strategy (≤10 lines)
Spell pack, built on the parent's infrastructure (pathing, distance-field steps, try_action, group table, banking).
The altar-lock warden is gone. The team now farms one shared camp group (shared byte 1, x1.5 score), replaced only
when it is captured, respawning or unreachable. Each turn a spell engine scores every ready slot against every legal
target and casts the best one before walking: kill shots (+250), stuns on channelling enemies (tribute/recall, +200),
Willow heals the most wounded ally in range, AoE goes on the tile that hits the most enemies and fair camps (never
wakes a monolith or a sleeping golem), and shields go up when hurt near threats. Sleeping camps are spell targets only in
our chosen group or in basic-attack range. Dashes add 2 tiles of travel when walking. Attack resets only follow our own hit.
Why it beats the parent: on 01elk-arena and 00lanes (its worst losses) the spell burst and heals win the fights.

## Key parameters
kPackBonus 1.5, kStickiness 2.0 (current target), kDisengageHealth 0.7 (flee when hurt and enemies within 5 outnumber
allies), kRecallHealth 0.35, kBankGems 20, kShopGold 150. Draft: Vendetta, Willow, Marcella, Tavi, Vesper Vale, Ryth, Varg...
Shop: Stellar/Prism/Cassi buy Focus (Treasure, Jungle health, Power focus); everyone else Strength (Treasure, Jungle, Power).

## Changes this generation
Radical mutation of e03-07 (spells focus). All tests used tools/benchmark.py (release build, both sides), 309 games in total:
- First version vs e03-07 on 01elk-arena 00lanes 00pilgrimage 00contest 01the-farm 01hooks: 7/12, +233 (elk-arena +864).
- vs e01-01 on 8 maps: 13/16, +233. vs e02-01 on 8 maps: 11/15, +203 (01islands lost both sides).
- Final version on 01islands 01the-farm 00pilgrimage 00lanes 00corners 01elk-arena 01hooks 01big-world:
  vs e02-01 10/15 (+160), vs e03-07 10/15 (+63). vs starter 8/8, vs e00-01 5/7 (00contest 01the-farm 00lanes 01islands).
- 0 engine faults and 0 caught exceptions of ours; every `bot error:` seen came from the opponent (e02-01, e01-01, e03-07, e00-01).
- Kept: disengage rule, stickiness 2.0, lock break when beside an enemy-locked altar, Strength builds for Willow/Tavi/Marcella
  (+2 wins over Focus builds on 30 games).

## Tried and failed (keep entries from the parent's notes; add yours)
- Starter step selection `field.get(tile) < field.get(step)` picks blocked/unreached tiles (they read 0): "Known occupied
  destination". Fix: only step to tiles that are free and in `visited`.
- Iterating `vision::champions()/camps()` handles and calling health()/is_alive() throws "No visible entity" when the
  unit died this turn; scan `champion_tiles()/camp_tiles()` and use `champion_at/camp_at` instead.
- Attack-reset abilities throw "An available attack cannot be reset" in cases `can_attack()` does not reveal; only
  cast them right after our own basic attack this turn.
- After a move returns false (hidden unit), moving again the same turn throws: stop walking for the turn.
- `vision::has_altar_lock` is true for our own locks too: use `altar_lock_at(...).is_ally()` before skipping an altar.
- e04-03: Draft Vendetta, Marcella, Willow, Ryth, Varg (AoE farmers ahead of Tavi/Vesper): 2/8 vs e02-01 vs 3-4/8. Reverted.
- e04-03: No pack (kPackBonus 1.0): 18/32 vs 18/29 with the pack on the same 8 maps vs e02-01 + e03-07, lower margins.
- e04-03: Stronger pack (kPackBonus 2.5): 19/32, below 1.5 (20/30).
- e04-03: Every champion overwriting the shared group each turn makes the pack churn between groups; write it only
  when the current pack group is invalid for the writer.
- e04-03: Focus builds for Willow/Tavi/Marcella: Focus shards add less damage per turn than Strength for 1.4-3x kits on
  an 8-turn cooldown.

## Known weaknesses
- 01islands: lost on every test (out-farmed ~3x; champions travel between distant groups; no crystals used).
- 00corners vs e03-07: out-farmed 4x with 0 deaths, so farming throughput is still below the lock bot's.
- 0-0 games (00contest, 00stampede) are decided by tiebreaks. No wards or teleport crystals used.
