## Strategy (≤10 lines)
f00-02, immigrant (safe-banker archetype), rebuilt from the starter's main loop only.
Draft: Vesper Vale, Marcella, Vendetta, Varg, Ryth (then Willow, Tavi, ...). Draft memory unused.
Each champion scores camp groups by (gems + 0.33 gold) / (path dist or respawn wait + clear turns + 6 + 0.4 altar dist);
danger (camp damage spread over nearby helpers > 75% HP) is a soft x0.05 penalty. Harmless groups an ally
already claims (shared bytes 0..5) get x0.1, so cluster fields are farmed in parallel. Vesper captures (bonus gold).
Banking: tribute when an altar is close (dist <= 3 + gems/4), at 24 gems, under threat (own sighting, or a team
sighting in shared bytes 6..7 with >= 16 gems) and after round 1900. Enemy altar locks are remembered and broken.
Enemies: fight back when one is in range (basic attack + attack reset + damage spells), flee only when outmatched.
Recall at 35% HP (after stepping out of camp range), recall to shop at 150 gold, heal on spawn to 90%.
Shop: Durability emblem of Strength, Home emblem of Strength (3-turn recall), then Power Strength/Health 2:1.

## Key parameters
DANGER 0.75 (camp damage / own HP), THREAT_R 5, OUTMATCH 1.4 (flee), routed 1.6 or HP < 25% or bag >= 15 and weaker,
SHOP_GOLD 150, recall 35% HP, spawn heal to 90%, target re-choice every 8 rounds (x1.25 stickiness),
stuck 12 idle turns -> ban 60 rounds, blocked respawn wait 25 turns -> ban 40, contested capture: leave if foes > friends.

## Changes this generation
New bot (immigrant). Final test (891 games used in total): vs starter 80/80 on all 40 maps (+719 mean), 0 bot errors;
vs e00-01 (run-1 gen-0 winner) 47/80 on all maps at mid-development, later 12-13/16 on an 8-map set;
vs e03-07 (altar control) 6/8; vs e07-05 (run-1 champion) 4/16 (-246).
Biggest gains, in order: fighting back when engaged instead of fleeing (worst maps -766 -> -283; full set 35/80 -> 47/80),
altar-lock memory (vs e03-07 -149 -> +368), stepping off captured spawn tiles (they blocked respawns), attack resets
(+110 margin), Home emblem (+120 margin), soft danger instead of coalition waiting.

## Tried and failed (keep entries from the parent's notes; add yours)
- distance_field::compute_to appears to modify the walls board passed in: a later neighbour check against the same
  board stepped onto a known champion ("Known occupied destination"). Pass a copy to every compute.
- Engine still rejects some moves as "Known occupied destination" (wards / revealed units): safe_move checks
  has_champion/has_camp/has_teleport/has_ward and treats error 22 as a failed move.
- Stunned champions throw on recall/submit_gems (engine errors 44/45): skip the turn when buffs().has_stun().
- camp_at/champion_at can throw error 98 right after occupancy masks: camp_here/champion_here helpers.
- Coalition targeting (wait at a monster group until enough allies are present, x0.45 proposals): champions idled
  and the whole team blobbed onto one group; replaced by the soft danger penalty (equal, simpler).
- Hard rule "leave a reward as soon as an enemy is near": the enemy captured our kills (elk-arena).
- Sharing respawn timers in shared memory, and waiting at groups that respawn within 20 rounds: both worse (-150..-220).
- Avoiding groups near the last team enemy sighting (x0.6): worse vs e00-01 (+81 -> +13), no gain vs e07-05.
- Recall to shop at 120 gold instead of 150: worse (+374 -> +275 on the 8-map set).
- Fuel: full-map BFS costs ~40k fuel on 100x100 maps; per-tile bitboard set() loops are expensive too. Use cached
  terrain fields for far travel, draw_chebyshev away from edges, proximity filters before group state() calls.

## Known weaknesses
- 00lost-world and 01elk-arena still lose to e00-01 (cluster fields / contested centre group farmed faster by them).
- 00contest and 01boss-battle: every camp is tied to a monolith; 0-0 games go to the tiebreak (idle champions
  gather at the centre hoping for a kill), and 01boss-battle is still decided by the Team B rule.
- Strong spell/focus-fire teams (e07-05) still win most fights; no team focus target, no stuns timing, no dashes.
- Fuel still runs short on some turns on 100x100 maps (execution resumes next turn).
