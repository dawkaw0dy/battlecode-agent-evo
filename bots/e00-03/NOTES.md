## Strategy (≤10 lines)
Camp sweep (immigrant from starter infrastructure). Draft rates every camp group by gems per unit of clear work
and writes one efficiency byte per group to draft memory ('S', count, bytes); picks strong basic attackers
(Vendetta, Vesper Vale, Varg first when >= 3 multi-fighter groups, then Ryth, Marcella, Tavi, Willow, ...).
Champions score groups by (gems + 0.05*gold) / (distance + clear time + 5), scaled by the draft rating, skip
respawning groups (remembered timers) and monoliths, skip groups whose estimated camp damage exceeds 50% hp.
Each champion publishes its target in shared memory (byte = champion type % 8); small groups claimed by an ally
are discounted x0.4 so farmers spread instead of splitting rewards. Gems are banked in small sticky batches
(>= 8 near an altar, >= 30 anywhere, low hp, enemies seen, late game). AoE spells aim at the tile covering most
camps; attack resets follow basic attacks. Jungle emblem first, then power shards. Recall < 33% hp.

## Key parameters
bank: gems >= 30, or >= 8 with altar within 7, or altar within 2; danger 50% hp; recall 33%; heal on spawn to 90%;
claimed small group x0.4, big (>2000 hp / golem) x1.1; target hysteresis x1.3; stuck 8 turns -> skip 40 rounds.

## Changes this generation
New bot from the starter's main loop and pathing helper: draft-time group plan, sweep scoring, shared-memory
target claims, small-batch banking, AoE targeting, per-action exception guards.

## Tried and failed (keep entries from the parent's notes; add yours)
- vision::camps()/champions() can contain units whose reads throw (engine errors 97/98): check is_visible()
  first, and wrap each decision block in a local guard so a stale handle does not abort the turn.
- Casting "excludes walking" spells after a move throws: casts are skipped once we walked this turn.

## Known weaknesses
- Lost 00lanes badly (-773/-515) and 01boss-battle 0-0 (tiebreak) vs r00-01; not investigated for lack of time.
- No retreat logic vs aggressive enemies beyond the 33% recall; spread-out farmers can be picked off.
- Shared-memory slot by champion type % 8 can collide (harmless: only mis-weights claims).
- Self-eval vs r00-01 on 4 maps: 2/8 wins; vs r00-00 earlier version won most valid games.
