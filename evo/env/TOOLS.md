# Tools for evolution agents

The CLI is `cambc` 3.0.5 (on `PATH` in the cloud environment; `.venv/bin/cambc`
is linked to it by the SessionStart check). All tools run from the repository root.
The game budget (`local_game_budget` in your task) counts games from all tools together.

| Tool | Use | Status |
|---|---|---|
| `python3 tools/benchmark.py A B --maps m1 m2 --jobs $(nproc) --output /tmp/<dir>` | main A-vs-B test: both sides, deterministic, release build | allowed (the evaluation tool) |
| `python3 tools/bench2.py A B --maps m1,m2` | quick matches with gem margins and fault counts | allowed |
| `python3 tools/teamstats.py <dir>` | per-game team stats for a `bench2.py --keep <dir>` run | allowed |
| `python3 tools/analyze.py <stats.json>` | per-champion summary of one game's stats file | allowed |
| `python3 tools/pool.py <out> CAND... --opps a,b` | many candidates vs many opponents | allowed |
| `node tools/mapgen.mjs info maps/<m>.map26a` | read a map's layout, spawns and camps | allowed |
| `node tools/replay.mjs` | dump a replay to JSON | **not usable**: written for an older engine; fails on engine 1.0.3 replays |
| `cambc login`, `submit`, `bundle`, `match-groups`, `ladder`, `matches` | — | **forbidden**: they act on the team's platform account, and an upload replaces the live ladder bot |
| `cambc init`, `cambc maps` in the repo | — | **forbidden**: they rewrite tracked SDK and map files (the scope check rejects it anyway) |

`cambc build bots/<you> --release` and `cambc run bots/<you> bots/<opponent> --map <m>`
are allowed; a `cambc run` game counts towards the budget.

## Includes your bot may use

- SDK headers: `cambc.h` (the API), `imports.h`, `wasm_simd128.h`, written as
  `#include <cambc.h>` or `#include "cambc.h"`.
- Standard C and C++ headers from the toolchain (`<vector>`, `<algorithm>`, `<cstdint>`, …),
  plain names only, no `__`-prefixed internals.
- Headers in your own `src/` directory as `#include "name.h"`.

Anything with `/`, `\` or `..`, and `#embed`, `__has_include`, `asm` or a
`#pragma` other than `once`, rejects the bot.

## Known SDK pitfalls (found by earlier agents)

Collected from the agents' own NOTES.md "Tried and failed" lists (generation 00). Each of these raised a
caught exception (`bot error:` in stdout, so the game counts as a loss) until the agent worked around it.

- Reading a unit taken from `cambc::vision::champions()` / `camps()` (health, state, position…) can throw
  engine error 97 "Unit is not in vision" or 98 "not found", even after `is_visible()`, when the unit died or
  moved this turn (including by your own attack). Several agents enumerate `vision::champion_tiles()` /
  `camp_tiles()` and use `champion_at` / `camp_at` instead, and guard each decision block so one stale
  handle does not end the turn.
- Casting an ability marked "excludes walking" after moving this turn throws (engine error 126). Cast first, or skip it.
- An attack-reset ability can throw engine error 26 "An available attack cannot be reset" in cases
  `can_attack()` does not reveal. Use it only right after your own basic attack this turn.
- After a move returns `false` (a hidden unit was revealed), a second move in the same turn can throw. Stop walking for that turn.
- The starter's `move_towards_closest` picks the step with `field.get(tile) < field.get(step)`, but blocked or
  unreached tiles read 0 there, so it can choose an occupied tile ("known occupied destination"). Only step
  to free tiles that the distance field actually reached.
- `bitboard::draw_chebyshev` near the map edge threw engine error 5 for one agent (01starvation). Set tiles
  with your own bounds checks there.
