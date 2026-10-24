## Strategy (≤10 lines)
Altar control on top of a solid farm. Draft Vendetta, Marcella, Vesper Vale, Tavi, Varg. Everyone farms: groups are scored
by (gems + w·gold)/(travel + respawn wait + clear time + extra trips), claims in shared memory keep allies apart,
camps never heal so strong groups are worn down over several trips. While a target respawns we bank gems or shop.
Every spawn visit (or death) buys an altar lock first (10 gold), then shards. Any champion within 2 tiles of an altar
where an enemy is channelling a tribute (or stands next to it with 12+ gems) locks it, which cancels the channel;
stun spells are always spent on enemy tribute channels. Tavi (else the last slot) is the guard from round 250: he
farms near the altar closest to the enemy on our half and waits there when idle. Enemies clearing our lock or
channelling count as stuck (fight ratio ×1.3). Fights: square law, fight at ratio ≥ 1.3 (+gems/100), farm on when
foes are 6+ tiles away, recall to flee, hunt only at ≥ 1.6, give up chases after 5 turns; allies within 14 answer sightings.

## Key parameters
- kFightRatio 1.3 (+min(0.5, gems/100)), ×1.5 within 3 of spawn, kHuntRatio 1.6, chase ≤ 5 turns then 15 rounds off.
- Score f: enemy side ×0.7, reward ×1.3, current target ×1.25, recent sighting within 8 ×0.35, claimed small group ×0.3,
  guard: groups within 10 of its altar ×1.6, beyond 16 ×0.5. Gold weight 0.35 × max(0.4, 1 − round/1500). Trips ≤ 5.
- Bank at ≥30 gems, ≥20 within 10, ≥10 within 3, ≥10 after a sighting in the last 40 rounds, or end of game;
  give up a tribute walk that makes no progress for 12 rounds (50 rounds off).
- Locks: carry one always; place on an altar ≤2 away under an enemy channel or an enemy with ≥12 gems beside it.
- Hurt: < 25% hp or < 3 turns of camp fire; heal first below 50% if the next fight would not finish.
  Damage from nothing visible (bush) → walk home; never recall within 2 rounds of being hit.
- Shop trip at 250 gold; shop while waiting ≥12 rounds for a respawn with ≥90 gold.

## Changes this generation
- New bot (immigrant, altar-control archetype) built on the starter's main loop.
- Local results, final version: vs starter 32/32 (16 maps); vs f01-07 42/80 on all 40 maps (version before the last
  four tuning steps), 14–15/32 on its 16 worst maps after them; vs f00-03 19/40 on 20 maps.
- Lock measurement (16 maps × 2 vs f01-07): 21 locks in 32 games: 6 cancelled running tributes (357 gems delayed),
  15 pre-empted enemies carrying 521 gems beside the altar (gems are delayed, not lost); the same
  bot without locks also won 14/32 (mean margin −55 vs −74) — no measurable gain from locks alone.
  The guard role did help: guard off 12/32 vs guard on 16/32 on the same 16 maps (both opponents).

## Tried and failed (keep entries from the parent's notes; add yours)
- (parent = starter, no notes) From other agents this generation, confirmed here:
- Linear bitboard buffers are bit-packed; every bitboard temporary costs ~600 fuel and grow(r) several thousand:
  cache goals (approach, altar spots, guard post) and walk_walls(); turns over 100k resume next round stale.
- Reading every group's shared-memory claim and estimating every group's damage each turn cost ~40k fuel; read
  claims once and prune with an upper bound before the costly estimates.
- Standing on a respawning group's spawn tiles blocks the respawn forever (timer stays 0): wait beside it.
- Fleeing whenever any foe was within its range+3, fight ratio 1.0: lost 11/12 to f01-07; farming on while foes
  are 6+ tiles away and recalling to flee took it to 7/12.
- Ranged champions that "hit back" while fleeing never moved and died.
- Fight ratio 1.0 / 1.15 / 1.3 / 1.6: 11 / 11–14 / 15 / 10 wins of 32; hunt ratio 2.2 worse than 1.6, 1.3 equal.
- Vesper gold weight 0.55: no gain. Bank at 30 instead of 45: small gain.
- Allies answering sightings: worse with fight ratio 1.0, better with 1.3.
- Excluding camp tiles from altar spots made the only altar on 01boss-battle unreachable (0 tributes): fall back to
  all spots when the filtered set cannot be reached.

## Known weaknesses
- Loses contested small maps (hooks, gold-rush, quadrants, expanse, great-wall) where the enemy out-farms us early.
- Islands: walking-only planning (no dash over holes, no teleport crystals) leaves few reachable groups.
- No boss raids; stampede-like maps where the only gems sit in the contested centre are lost.
- Altar locks fire rarely (≈0.7 per game) and their effect is within noise.
