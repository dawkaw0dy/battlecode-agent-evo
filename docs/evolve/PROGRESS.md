# Progress

**Milestone:** M5 in progress. **Stop point:** generation 02 (7 sessions) waits for the owner's yes (`approve 02`).

**M5 so far**
- H7 done; private repo `dawkaw0dy/battlecode-agent-evo-heldout` has the generator and 24 maps (`ac09548`), cloned at `heldout/`.
- Free Actions test `m5-heldout`: 6/6 rows identical to the local run (2 held-out maps); held-out shard published no replays.
- D33 (held-out, Stage C), D34 (n = 8, cap 15, gen 02 only, `resume`). Real run resumed; gen 02 planned:
  elite e01-01; children e02-01..04 of e01-01, e02-05..06 of e00-01; immigrant e02-07 (big-game-hunter).
  Observation: e02-01/e02-04 (tweak/combat) and e02-02/e02-03 (subsystem/opening) got identical briefs.

**Since M4:** `local_game_budget` 120 → 400 (D32, owner).

**Done in M4**
- Generations 00 and 01 at n = 4; D28–D31; fix `dca675e` for duplicate Stage B games (halt cleared with the owner's OK).

**M5 (§16):** n = 8, cache, hall of fame, held-out pool, Stage C. To continue the real run: raise
`max_generations` (or remove it), set n = 8 and the session cap, then `python -m evo step` → approval gate for gen 02.

**Done in M3**
- ConPTY launcher (D27); real prompts and 10 archetypes. 2 real agents (r00-00, r00-01) launched automatically,
  finished in 7–9 min, admitted. The M3 run is paused "too small", as expected.

**M4 plan (to confirm with the owner)**
- Real run `evo/config.toml` with n = 4 (§14 "start with n = 4"): gen 0 = 4 immigrants, then gen 1. Stop points: each
  generation needs `approve`. Ask the owner about `deadline_minutes` (M3 agents needed < 10 min) and about n.

**Done in M2**
- Orchestrator (`evo/orchestrator/*`, CLI `python -m evo`), D25–D26. A real one-generation run with fake agents (`t00`) on
  GitHub/Actions: 3 tampered branches rejected, faulter and build failure disqualified, spot-checks 7/7 matched.

**M3 needs (to propose when it starts)**
- A cloud launcher: `claude --cloud` refuses a non-TTY (M0 item 10); plan a ConPTY pseudo-terminal via `ctypes`.
- Real prompt fragments (`evo/prompts/operators`, `focus`) and n + 2 = 10 archetypes in `evo/prompts/archetypes`.
- 2 real sessions (stop point). Also check H5 (Custom network) and the second session's setup cache.

**Done in M1**
- Eval pipeline built (D21–D24). Hand-written job `m1-probe` → `results/m1-probe.json` matches the local run 8/8, down to the replay bytes.

**Done in M0**
- §0.5 items 1–13 and 15–16 recorded in M0.md. Item 14 (prebuilt WASM via `run-batch --jsonl`) is deferred as optional (§17).
- Hard stops: item 8 (determinism) matched; item 12 failed, was fixed by H4 v2 (D20) and verified.
- Two cloud sessions used, both approved: probe 1 (setup failed: TLS proxy), probe 2 (succeeded).
- D1–D20 recorded and applied to PLAN.md.
- This clone pushes over SSH with deploy key `~/.ssh/evo_orchestrator` (`remote.origin.pushurl`,
  `core.sshCommand` in the local git config); HTTPS pushes as the owner are refused by the rulesets.

**Waiting on owner**
- H5 (SETUP.md). Then the owner's go for M1.

**Facts the next milestones need**
- The launcher cannot capture `claude --cloud` output from a pipe ("requires an interactive terminal"). M3 needs a
  pseudo-terminal launcher (ConPTY via `ctypes`, stdlib) or another design. Propose it when M3 starts. From a TTY
  it returns in about 10 s and prints `View: https://claude.ai/code/session_…`.
- Cloud: root, Python 3.13, TLS-intercepting proxy; cambc must run with `CAMBC_OFFLINE=1` and the seeded toolchain.
- Games take about 7 s each on 4-core runners; at least 22 concurrent jobs; replays are byte-identical between the PC and Linux.

**Open review issues (not yet decided)**
- 12 Build-failure attribution (`runner.log` names the bot, M0.md item 7). M1.
- 13 The wall-clock `--timeout` vs determinism. Games take about 7 s on runners, so 300 s has a big margin. M1.
- 14 `similar()` is undefined in gens 0–1 and without head-to-head games. M2.
- 15 No source for map tags (§13.2). M2/M3.
- 16 Actions concurrency drops pending runs. M1.
- 17 Ruleset vs agents pushing as the owner (§9.5). M0 item 12.
- 20 Network test vs §0.2.5. M1.
- 22 `claude --environment <id>` flag. M0 item 10.
- New: `shard_games = 40` makes setup dominate (about 70 s of play per shard); consider 200. M1.

**Local facts worth keeping**
- On Windows, run `benchmark.py` with `PYTHONUTF8=1` and an absolute `--runner ...\.venv\Scripts\cambc.exe`.
- cambc cache: `%LOCALAPPDATA%\cambc\` on the PC, `~/.cache/cambc/` on Linux.
- No `gh`: Actions status comes from the public REST API (`/repos/dawkaw0dy/battlecode-agent-evo/actions/runs`,
  unauthenticated, 60 requests/hour), and results come from `git fetch` of `evo-results`.
- `.venv` uses Python 3.13 with cambc 3.0.5 and pytest.
