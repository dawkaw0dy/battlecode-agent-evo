# Progress

**Milestone:** M0 complete except H5 (owner: narrow the network to Custom; steps in SETUP.md).
Not started: M1 (waiting for the owner's go).

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
