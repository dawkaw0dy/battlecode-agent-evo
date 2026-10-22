# Progress

**Milestone:** M0, in progress. Every item that needs no cloud session is done (1–9, 14–16).
Waiting on the owner: the probe-session yes (stop point).

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
- H2, H4 done (D19 for `evo-results`). H3: env `env_01HkiFrTm9YE8qLL1T7rKYdV` is pinned in the
  gitignored `.claude/settings.local.json`. `/remote-env` also set it at user level; the owner has
  not said yet whether to remove that.
- The owner must re-paste `evo/env/setup.sh` into the `evo` environment (it changed in `61dc38c`: it
  now logs to `/opt/evo-setup.log`).
- Stop point: a yes for ONE probe session (task `evo/tasks/probe.md`, 30 min deadline).

**Launch plan for the probe (item 10)**
- From the clone on `main` (clean, `HEAD == origin/main`): `claude --cloud "You are the M0 probe
  session. Read evo/tasks/probe.md on main and follow it exactly."` with stdin closed. Capture all
  output, and kill the local process after 5 minutes if it is still attached (the docs say the cloud
  session persists). Record what it prints and whether it returned.
- Docs (code.claude.com, 2026-10-22): the git proxy does **not** limit which branches a push updates;
  only GitHub rulesets do. So `main` should refuse (Restrict updates) and `evo-results` will probably
  accept, which triggers D19's deploy-key switch.

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
