## Strategy (≤10 lines)
Scaling economy, immigrant on the starter's main loop. Draft Vesper Vale, Marcella, Vendetta, Varg, Tavi.
Each champion scores every camp group by (gems + w·gold)/(eta + respawn wait + clear time + trip cost + 8), with
one host BFS from its own tile every 4 rounds; a target is sticky once we are within reach + 6 of it. Claims in
shared memory (bytes 0–4) keep allies off small groups and let them share big ones (> 3000 hp: damage and clear
time split). Camps are remembered per tile (they never heal), so big groups are worn down over several trips.
Gold goes into shards at every spawn visit or death (Tavi: anywhere): Vesper treasure → jungle → monster,
others jungle → treasure → monster, then power/durability. Fights use a square law (Σhp·Σdps, allies within 7):
engage at ratio ≥ 1.3 + gems/150 (0.85 near home), trade back when already in contact at ≥ 0.75, otherwise run
home early and recall once 6+ tiles clear. Enemy sightings (bytes 5–7) steer target choice away for 24 rounds.

## Key parameters
- Score factors: enemy side ×0.7, current target ×1.5, reward waiting ×1.3, near a fresh sighting ×0.4,
  claimed small group ×0.35, claimed big group ×0.9. Gold weight 0.4 (Vesper 0.6) × max(0.5, 1 − round/1600).
- Trips: solo damage estimate (most dangerous camp per hp first) / (0.65·max hp); ≤ 8 trips (monolith ≤ 2),
  each extra trip costs d_home + 12.
- Retreat from camps at hp < 20% or < 3·incoming + 30; home at hp < 30%; heal first below 50% if the rest of
  the group would cost > 80% of our hp.
- Shop trip at ≥ 270 gold (≥ 120 within 8 of spawn). Tribute at ≥ 45 gems, ≥ 20 within 8, ≥ 8 within 3,
  ≥ 20 after a recent sighting, on a shop trip within 10, at the end, or when it wins the game.
- Chase only within attack range + 2 (+4 on a target below 35%), not into the enemy half, at most 5 turns.

## Changes this generation
- New bot (immigrant). All tests release builds, both sides; ~376 games; 0 faults and 0 `bot error:` lines from
  our side (the one caught exception in the final starter run was the starter's).
- v1: starter 8/8, f05-02 5/80 (−882). Champions chased enemies into the enemy half and died; melee barely hit.
- Chase limits + sighting penalty + earlier flight/recall + trade back in contact: deaths 172 → 131 per 8 games.
- Share big groups and allow up to 8 trips: Tavi had waited 369/750 turns on respawns (everyone crowded the few
  small groups that passed the 4-trip limit).
- Sticky target near the group: collected gems vs f00-03 (8 games) 3138 → 6150.
- Wait beside respawning groups, not on their spawn tiles (an occupied spawn tile delays the respawn: Vendetta
  waited 479/750 turns on 00corners): 00corners 381 → 700 gems collected.
- One capturer per group (a second one only splits the reward): vs f00-03 4/8 (+181).
- Final: f05-02 11/80 (−471), starter 79/79 valid (+797).

## Tried and failed (keep entries from the parent's notes; add yours)
- (Inherited from the task's list for this generation)
- Fuel: host calls cost ~30–40 fuel, but every bitboard temporary (&, |, ~, shift) ~600 and a grow(r) ~4k·r;
  local loops ~50 per element. Function-local statics add a guard check per call. Turns above 100k fuel resume
  next round with the champion possibly dead: position() then reads (-1,-1) and field reads throw engine error 5.
- A route-tracing loop with host field reads cost ~70k fuel; use an exported local copy (unreached = 65535).
- Enemy memory (3 rounds) + fight/flee hysteresis: 23–24/80 vs 34/80 without. Vision flicker is not the problem.
- Unseen ("unknown") camp groups blocking the route reset the plug logic every turn: champions stood still.
- Ranged champions stalled on range-3 tiles without line of sight: shrink the approach radius when nothing is hit.
- Boss raids gated at 2.5 tank-turns: everyone walked into monoliths and never hit them; 8 turns works.
- Raid and current-target bonuses on a respawning boss kept all five champions waiting 150 rounds.
- Rotation-aware retreat (exit after 1 step when an ally is as close to the camp): 43/80 vs 50/80 vs parent.
- Games are chaotic: judge changes on full runs, never on single maps.
- iostream float printing costs 10–30k fuel per line: traces distort fuel profiles.
- Hysteresis on the simulated fight value (±60 in favour of the last decision): 14/32 vs 17/32 without.
- Mine (8 maps × 2 sides vs f05-02, baseline 5/16, −214): tribute at ≥ 30 gems / ≥ 15 within 8: 4/16 (−351);
  shop trip at 200 gold: 3/16 (−322); shop trip at 340 gold: 4/16 (−245). All reverted.
- Standing on a respawning group's spawn tiles while waiting: the respawn never comes (occupied tiles delay it).

## Known weaknesses
- Fights are per champion with a crude square law; f05-02's simulated focus fire still kills us ~3× as often
  on contested maps (00swirl, 00gemfight, 01hooks). No grouping before contests, no team engage call.
- Approach (walking) is still ~30% of turns; no dash planning across holes beyond the engine's dash mask.
- No raids on monoliths/golems as a team, no altar locks, no wards, no teleport crystals.
- The capture/claim logic sees allies only inside our own vision.
