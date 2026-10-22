## Strategy (≤10 lines)
Scaling economy. Draft gold scalers first (Vesper Vale, Marcella), then strong farmers (Vendetta, Varg, Ryth, Tavi, ...).
Every champion buys shards on spawn or while dead: Treasure shards first (+20% gold emblem), then Power shards (6-for-5).
Stat by archetype: Focus for Stellar/Prism/Cassi, Strength for everyone else.
Farm all camp groups except Monolith (Golem only once Strength >= 85 or after round 900).
Abilities: attack resets after a basic attack, damage/cc/aoe spells on the current target, shields when hurt, Willow heals.
Gems are banked in batches (>= 25, near an altar, late game, or when it would reach 1000); the decision is sticky.
Recall at < 35% HP (stepping away from threats first) and to shop at >= 150 gold when nothing is fighting us.
Respawn timers of seen camp groups are remembered so goals do not flicker when the group leaves vision.

## Key parameters
kBankGems = 25, kShopGold = 150, recall HP 35%, golem gate Strength 85 / round 900, near-altar bank radius 5.

## Changes this generation
New bot from the starter infrastructure (pathing field, main loop). Draft preference list, Treasure-then-Power shop,
ability usage, gem batching with hysteresis, shop recalls, respawn-timer memory, camp-type gating.

## Tried and failed (keep entries from the parent's notes; add yours)
- Non-sticky bank decision ("bank if altar within 5"): champions oscillated between two tiles forever (00gem-fields loss -950).
- Excluding respawning groups only while visible: goal flickered when stepping off the spawn tile.

## Known weaknesses
01the-farm: team earns no gold at all (0-0 game, lost as side A on the Team-B tiebreak); likely stuck on elk-heavy start.
01gold-rush and 00gem-fields side B still lost vs starter. No contesting/altar locks/wards; no dash for travel.
