# Progress

**Milestone:** M0, in progress. Every item that needs no cloud session is done (1–9, 14–16).
Waiting on the owner for H2–H4.

**Done**
- H1 (2026-10-22). D1–D18 recorded and applied to PLAN.md.
- Bootstrap (§1.1) committed: cambc 3.0.5 workspace, `bots/starter`, pruned and fixed `tools/`,
  `evo/` (config, manifest, `python -m evo bootstrap|manifest`, digest, tests), `evo/env/*`,
  `.claude/settings.json` hook, `bots/probe-random`, workflows `evo-env-test.yml` and `evo-m0-probe.yml`.
- Actions run 39799008988 (`evo-m0-probe`) and the `evo-env-test` run passed. Determinism holds
  on two runners and the PC, down to the replay bytes. About 7 s per game, 4 cores, ≥22 jobs at once.
  Setup script 36 s. Everything is in M0.md.
- Tests: `tools/tests` + `evo/tests` give 11 passed on Linux, and 10 passed + 1 skipped on Windows.

**Waiting on owner**
- H2 (Claude GitHub App), H3 (`evo` environment, reply with `env_…`), H4 (rulesets, Actions settings).
  Instructions in SETUP.md.

**Next steps after H2–H4**
1. Write `.claude/settings.local.json` with the env id.
2. Write the probe task (`evo/tasks/probe.md`) and commit it.
3. Stop point: ask for a yes to **one** cloud session (items 10–13), with its deadline and task.
4. Run it; record items 10–13; H5 (network → Custom with the recorded hosts).
5. Report M0 done.

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
