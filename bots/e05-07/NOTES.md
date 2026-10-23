## Strategy (≤10 lines)
Parent A e04-04 (altar control + per-group camp scoring) with parent B e04-06's opening crossed in:
- Draft by team size (`draft::team_size()`): 3-4 picks keep A's farmers (Marcella, Vesper Vale, Vendetta, Ryth);
  5 picks take B's pack in B's order (Vesper Vale, Ryth, Tavi, Marcella, Willow).
- Draft memory byte 0 names the altar warden (champion_type + 1 of Marcella). Before, the first champion to read
  shared memory became warden, so B's Vesper-first order made the gold-bonus farmer the warden.
- B's ally-heal scoring ported as `heal_allies()` (A never cast heals), so Willow is useful in A's code.
Everything else (camps, altars, locks, banking, combat) is A's.

## Key parameters
kSmallTeam 4 (team size up to which A's picks are used). Heal: skip allies missing < min(150, 0.6 x heal),
score min(missing, heal), x2 under 40% HP. A's parameters unchanged (kBankGems 20, kShopGold 150, recall 35%).

## Changes this generation
Crossover (focus: opening), all tools/benchmark.py, both sides, release; 322 games total.
- Baseline A vs B, 20 maps: 23/40. Child (final) vs B on 10 five-pick maps: 15/20 (A got 9/20 there:
  elk-arena 1/2 vs 0/2, contest 2/2 vs 0/2, quadrants 2/2 vs 0/2, eight 1/2 vs 0/2, starfish 2/2 vs 1/2).
- Child (final) vs A on the same 10 maps: 15/20 (elk-arena +846, eight +786, starfish +1000; big-world 0/2).
- Child (final) vs starter, e00-01, e01-01, e02-01, e03-07 on elk-arena, quadrants, big-world, pilgrimage: 39/40.
- 3-4 pick maps play exactly as A (same picks, same warden). 0 engine faults, 0 caught exceptions.

## Tried and failed (keep entries from the parent's notes; add yours)
- Starter step selection `field.get(tile) < field.get(step)` picks blocked/unreached tiles (they read 0): "Known occupied
  destination". Fix: only step to tiles that are free and in `visited`.
- Iterating `vision::champions()/camps()` handles and calling health()/is_alive() throws "No visible entity" when the
  unit died this turn; scan `champion_tiles()/camp_tiles()` and use `champion_at/camp_at` instead.
- Attack-reset abilities throw "An available attack cannot be reset" in cases `can_attack()` does not reveal; only
  cast them right after our own basic attack this turn.
- After a move returns false (hidden unit), moving again the same turn throws: stop walking for the turn.
- `vision::has_altar_lock` is true for our own locks too: use `altar_lock_at(...).is_ally()` before skipping an altar.
- Running out of fuel mid-turn (100k) suspends the bot; on resume, engine calls throw error 5 "Invalid metadata,
  alignment, overlap or argument" at random places. A full-map distance_field costs ~50-60k on 70x70 maps. Fix:
  check `cambc::fuel_remaining()` before the field and inside the group scan. bitboard::positions() is costly:
  precompute group tile lists once.
- `vision::has_camp(tile)` threw error 5 on in-vision spawn tiles; use `vision::camp_tiles().is_set(tile)`.
- Waiting ON a respawning group's spawn tile blocks its respawn forever (the champion idles): -700 gems/game on
  00corners until fixed.
- Re-choosing only every 6 turns, a Chebyshev fallback when fuel is short (targets flip between metrics), and an
  "ally already there" x0.6 factor: champions flip targets and walk more than they clear. Reverted.
- Skipping sleeping camps of non-target groups (to avoid waking them): -400 gems on 00lanes/00corners. Reverted.
- Parent-style danger (damage > 0.75 HP -> x0.05) vs the smooth budget: 8/12 +162 vs 10/12 +410 vs the parent.
- Lowest-HP-first camp order (parent) vs strength/health order: no gain.
- (e05-07) Blended draft Vesper, Marcella, Ryth, Tavi, Vendetta for every team size with first-come warden: vs A lost
  all three 3-pick maps (01broken-curve -1027 side A) and 00corners (-982); the warden became Vesper Vale.
- (e05-07) Same blend with the drafted warden (no Willow): vs B 6/16 on 8 new maps, 0/2 on 01elk-arena (-953).
- (e05-07) B's pack on 4-pick maps too (kSmallTeam 3): vs A 7/10 but vs B 3/10 (01hooks 0/2, -869); A's picks get 5/10.

## Known weaknesses
- 01the-farm (3 picks) still loses narrowly to e00-01, e01-01, e02-01 (tiebreak-level margins).
- 4-pick maps are A unchanged: 01shortcut 0/2 and 00gemfight/00interlock 1/2 vs B.
- 01big-world loses to A both sides (-386/-202) with B's pack; 01islands is 0/2 vs B; 00eight side A loses to B.
- Draft ignores the opponent's revealed picks and the map's camp mix; only the pick count decides.
