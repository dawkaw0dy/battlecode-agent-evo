## Strategy (≤10 lines)
Deathball. Draft Ryth, Willow, Varg, Vendetta, Vesper Vale (then Tavi, Marcella, Ren...): stun, heal, splash, shield, burst.
One leader (claimed via an 8-byte shared-memory heartbeat; re-claimed by the next champion when stale) picks the camp group
by reward / (distance + clear time), waits until the visible allies are within 3 tiles, then engages (flag in memory).
Followers stay within 2 tiles of the leader's broadcast position, join the engaged group's camps, never wake other camps.
Everyone fights enemies in range (weakest first) and chases only when the local group outnumbers them.
The leader decides banking (>= 24 gems) and team shop recalls (>= 180 gold); followers bank/recall with it.
Low HP (< 30%): step away from threats, recall; a leader hands leadership over first. Shop: Jungle emblem, then Power shards.

## Key parameters
kBankGems 24, kShopGold 180, recall < 30% HP, follow distance 2, gather radius 3, engage wait cap 8 rounds,
travel wait cap 5 rounds, stall skip 30 rounds -> skip group 80 rounds, Golem allowed after round 350, Monolith never.
Shared memory: [leader slot+1, x, y, heartbeat round, target group+1, flags(engage/bank/recall), -, -].

## Changes this generation
New deathball bot from the starter infrastructure (main loop, distance-field step, shop on spawn).
Units are enumerated from occupied visible tiles (champion_at/camp_at); stale-view lookups and rejected steps are caught
locally and treated as "nothing found"; main prints the last action tag and round with any `bot error:` line.

## Tried and failed (keep entries from the parent's notes; add yours)
- Iterating cambc::vision::champions()/camps() and querying handles after our own move threw engine errors 97/98.
- Checking is_visible + champion_tiles before queries was not enough; tile-based enumeration plus local catches fixed it.
- bitboard::draw_chebyshev near the map edge (01starvation) threw engine error 5; tiles are now set by hand with bounds checks.
- Attack reset after our own attack still sometimes threw error 26 (cause unknown); now caught inside fight().

## Known weaknesses
01gold-rush and 01corridors lose to the starter on both sides; 00packs side B loses narrowly.
01the-farm, 00contest, 01boss-battle end 0-0 (tiebreak decides). Slow coverage of large maps; no wards or altar locks.
