## Strategy (≤10 lines)
Immigrant (early-aggression archetype), rebuilt from the starter's infrastructure only.
- Draft Vendetta, Tavi, Vesper Vale, Marcella, Varg: high base stats, dashes, stuns, attack resets.
- Champions farm in packs (Vendetta+Tavi+Vesper, Marcella+Varg); each pack's camp target lives in shared
  memory and packs never share a target. Camps are chosen by value (gems×5 + gold) / (walk + clear turns + 6),
  skipping groups the pack cannot clear (camp damage over the clear time > 70% of pack health).
- PvP: square-law odds (Σhp × Σdps) over visible champions; fight at ≥1.1× (×0.6 near our spawn), punish what
  is in reach while retreating, never chase melee baiters or foes near their spawn; stun/dash/reset generically
  by ability kind. Kills pay 100 gold + the victim's gems, so not dying is half the archetype.
- Bank at 20 gems (or any bag when an altar is close), commit to one altar chosen by walking distance, bank
  before recalling when hurt near an altar (damage does not stop a tribute), carry and place an altar lock.
- Shards: power strength / power health in blocks of five; Tavi shops in the field.

## Key parameters
- kEngageRatio 1.1, kCarrierRatio 1.6 (≥25 gems), home multiplier 0.6 within 8 walking tiles of spawn.
- kRecallHp 0.50 (walk instead when spawn ≤ 10 away), kFleeHp 0.22, kShopGold 270, kKeepAway 5.
- kBankGems 20, kNearAltar 7 (crow flies) but small bags only when the altar is ≤ 10 walking tiles.
- Packs: pack_of {0,0,0,1,1} by draft slot, shared-memory bytes {0,1,5,6,7} hold pack targets, 2–4 a sighting.
- Target choice for foes: path distance ≤ 1 (melee), ≤ 4 for ranged foes shooting us, ≤ 3 weak, ≤ 6 channelling.

## Changes this generation
All bench2.py vs f03-05 on the same 10 maps × 2 sides unless noted; 0 faults / 0 bot errors at the end.
- v1 pack of five + hunting sightings: 3/12 vs f03-05, 0/6 vs f00-03, ~30 deaths per game (chasing kiters).
- Reach-based targeting, no chasing, safe() wrapper for every action (bot errors 40 → 0).
- Packs of two → solo → 3+2 packs: −710 → −501 → −500 (4/20). Recall at 50%: −445. Engage 1.1: −439.
- Fuel: cached camp groups, lazy approach masks, bounded searches; champions on camp-heavy maps had been
  acting every other round (01boss-battle and 00stampede 0 gems → 2/4 wins vs f01-07).
- Bank at 20: −379. Tavi for Ryth: −288. Walking-distance camp choice: −296 (kept, neutral).
- Bank fixes (one committed altar, bank before recall near altars): −231 (8/20).
- Init fuel (in-place groups, exported home field: init no longer spills into rounds 2–4), altar by walking
  distance + keep banking once committed: −151 (8/20).
- Final eval (benchmark.py): vs f02-01 10/20, vs f01-07 13/20, vs f00-03 14/20 and 6/20 on two 10-map sets,
  vs starter 10/10, vs f03-05 8/20. 884 games in total.

## Tried and failed (keep entries from the parent's notes; add yours)
- Hunting shared-memory sightings with the whole pack: chased kiting enemies across the map into their team.
- Treating every foe within 4 as a target: melee foes two tiles away were bait; chasing them killed us.
- Restricting farming to our half of the map: −759 vs −693.
- Walking home instead of recalling whenever a foe was seen in the last 8 rounds: −693 vs −596.
- Health-first shard order: −406 vs −296. Shop threshold 180: −366, 400: −333 (270 kept).
- Heart of Ruin for Marcella: −618. Ryth for Vesper Vale: −340. Solo farming on maps ≥ 4000 tiles: no gain.
- Finishing nearly dead camps instead of leaving them at low health: −407 vs −231.
- Counting allies within 8 instead of 6 for fight odds: neutral.
- Fuel (found here): a bitboard temporary per camp group per turn and a Group struct with three bitboards
  pushed by copy cost ~8k fuel per group: on 50-group maps the first turn spilled over three rounds.
  draw_chebyshev away from the edge, spawn_tiles_into, emplace_back and export_linear_buffer fix it
  (walls and unreached tiles export as 65535).
- Crow-flies altar distance on walled maps (00road-of-riches): champions flickered between banking and
  farming at the kNearAltar boundary and walked half the game without tributing.

## Known weaknesses
- Still dies ~10 times per game to f03-05, mostly while retreating; fights are per champion, packs split under
  pressure and the flee step is a greedy one-tile choice.
- Loses big on maze-like or long-road maps (00road-of-riches, 01concentrated-wealth, 00rows, 01fracture).
- Large maps: ~60% of turns are spent walking (packs of three travel together).
- No wards, no teleport crystals, no boss raids beyond what the camp filter allows.
