## Strategy (≤10 lines)
Split farm (immigrant, built on the starter's main loop only). The draft picks Vendetta, Marcella, Vesper Vale, Varg,
Tavi (first team_size()), BFS-splits our side's camp groups into angular regions around our spawn (equal value; the
hardest region to the toughest champion), marks middle groups "shared" and the far side "enemy", picks a home altar
per region, and writes all of it to draft memory. Each champion scores groups by value per turn (gems + w·gold over
travel + respawn wait + simulated clear time + extra heal trips), prefers its own region, claims its target in shared
memory (allies avoid it, except big groups and contested ones, which are worth doing together), clears alone, retreats
out of camp range and recalls before dying, captures, and tributes at its home altar. Groups holding ≥30% of the map's
gems are team "raids". Fights: square law (Σhp·Σdps of healthy allies within 4); flee to spawn when a stronger enemy
is approaching, hurt or outnumbered; hold on our spawn (full heal each turn) while enemies are near.

## Key parameters
- Value: gems + w·gold, w = 0.4 (<r400), 0.25 (<r1000), 0.1 (<r1600), 0.02. Own region ×1.0, other slot ×0.55,
  contested ×0.6 alone / ×1.6 with an ally, current target ×1.25, waiting reward ×1.3 if we hit it (else ×0.4).
- Multi-trip: ≤3 trips (budget 0.8·max hp per trip), each extra trip costs 12 + walking distance from spawn.
- Respawn wait ≤12 rounds, else re-target; recall while waiting if hp < 60%.
- Retreat from camps when the fight will not finish and hp < 3.5 rounds of incoming damage + 8% max hp.
- Fight need: ratio ≥ 1.25 + gems/150 (+0.5 if hp < 50%, +0.4 alone vs ≥2, ×0.5 on our spawn).
- Shop recall at 300 gold (150 when below 60% hp); Tavi buys in the field. Build: jungle(str)×5, (Vesper:
  treasure(str)×5), monster(str)×5, power(hp)×5, then alternating power str/hp.
- Shared memory: bytes 0–2 latest enemy sighting (x, y, count|fighting<<2, round&31), bytes 3–7 target claim per slot.

## Changes this generation
- New bot (immigrant, split-farm archetype).

## Tried and failed (keep entries from the parent's notes; add yours)
- (f03-05 notes kept by the task:) host calls ~30–40 fuel, bitboard temporaries ~600; turns over 100k resume next
  round; debug prints distort fuel and outcomes; judge on full runs, never single maps.
- Standing on a respawning group's spawn tile blocks its respawn forever (timer 0): never wait on the tiles.
- Moving right after a dash can hit a unit the dash revealed ("Known occupied destination"): re-check the tile.
- Recall / tribute while stunned throws "Champion cannot act"; recall with enemies in view gets interrupted.
- Lower gold weight (0.25 → 0.02 over the game): fewer items, clearly worse (0/6 vs f02-01).
- Treating every visible enemy as a reason to recall later (lanes: enemies are always in view): most recalls were
  wasted; only real flights count now.
- Shop threshold 450 and stricter post-flight recall (hp < 40%): worse (2/16 vs 4/16).
- Raids for every gem-rich group (≥40 gems): pulled champions off their regions, worse on ambush/big-world.

## Known weaknesses
- Rush maps (00gemfight, 01hooks, 00packs): lineage bots camp our spawn exit early; we score almost nothing.
- 01boss-battle: no monolith raids; the only altar is blocked, so we bank nothing on most runs.
- Fights are per champion; no focus-fire coordination beyond the sighting byte.
- Farming is slower than the lineage bots (~half their gems on 00lanes): long walks after each heal recall.
