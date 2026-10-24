## Strategy (≤10 lines)
Scaling economy. Draft Vesper Vale, Vendetta, Marcella, Tavi, Varg (gold makers first); the draft splits our side's
camp groups into one home region per champion (balanced k-means) and writes it to draft memory. Each champion
scores groups by (gems + w·gold)/(travel + respawn wait + clear time + trips), with the gold weight decaying over
the game; claims in shared memory keep allies off small groups, let them share big ones and join boss raids.
Every spawn visit (Tavi: anywhere) turns gold into shards: jungle(str) → (Vesper: treasure) → monster(str) →
power(hp) → power(str) → durability(hp) → alternating power shards. Bosses are raided once a champion can tank
8 turns (4 when the boss guards the only altar). Fights use a square law (Σhp·Σdps, allies within 7) with a
home bonus; weaker sides flee home and recall. Routes treat camps we believe alive as walls; a camp group that
closes the way (or holds the only altar tiles) becomes the target to clear.

## Key parameters
- Score: value·f/(eta + wait + clear + 10); f: home 1.0, unassigned 0.8, other's home 0.6, enemy side ×0.7,
  reward ×1.3, current target ×1.25 (not while it respawns), recent sighting ×0.5, claimed small group ×0.3.
- Gold weight 0.35 (Vesper 0.5) × max(0.4, 1 − round/1500). Shared group if solo clear > 15 turns.
- Multi-trip: ≥5 turns of tanking per trip, ≤5 trips (bosses ≤10 trips, raid size min(team, 2+claimers)).
- Retreat when hp < 20% or hp < incoming·(exit+2)+40; heal first below 50% if the fight would not finish.
- Shop trip at ≥240 gold between fights. Tribute: ≥45 gems, ≤3 tiles away, ≥20 within 10, ≥10 after a
  sighting, nothing to do, or end of game.
- Fight when ratio ≥ 1 + gems/100 (×1.5 near spawn); hunt only at ratio ≥ 1.6; give up a chase after 5 turns
  without a hit (15 rounds).

## Changes this generation
- New bot (immigrant, scaling-economy archetype) on the starter's main loop.
- Local results, final versions: 20/20 vs starter (10 maps), vs f00-03 34–35/80 on all 40 maps before the
  last two fixes; the respawn-wait fix took 8 maps from 6/16 to 9/16, the altar-guard fix boss-battle 0/2 → 2/2.

## Tried and failed (keep entries from the parent's notes; add yours)
- Fuel: host calls cost ~30–40 fuel, but every bitboard temporary (&, |, ~, shift) ~600 and a grow(r) ~4k·r;
  local loops ~50 per element. Function-local statics add a guard check per call. Turns above 100k fuel resume
  next round with the champion possibly dead: position() then reads (-1,-1) and field reads throw engine error 5.
- A route-tracing loop with host field reads cost ~70k fuel; use an exported local copy (unreached = 65535).
- Enemy memory (3 rounds) + fight/flee hysteresis: 23–24/80 vs 34/80 without. Vision flicker is not the problem.
- Unseen ("unknown") camp groups blocking the route reset the plug logic every turn: champions stood still.
- Ranged champions stalled on range-3 tiles without line of sight: shrink the approach radius when nothing is hit.
- Boss raids gated at 2.5 tank-turns: everyone walked into monoliths and never hit them; 8 turns works.
- Raid and current-target bonuses on a respawning boss kept all five champions waiting 150 rounds.

## Known weaknesses
- Still loses contested-centre maps (elk-arena, hooks, contest as A, quadrants) to f00-03: fights are decided
  per champion, no grouping before a contest.
- ~15% of turns go to enemy handling and ~10% to waiting for respawns; travel is ~25%.
- No wards, no altar locks, no teleport crystals.
