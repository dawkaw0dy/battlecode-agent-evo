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
