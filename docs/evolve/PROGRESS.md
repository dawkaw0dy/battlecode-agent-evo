# Progress

**Milestone:** M0, in progress. Stopped at §0.5 item 2 (init output differs from the plan); waiting on the owner.

**Done**
- H1 confirmed (2026-10-22): subscription account, Python 3.13 locally (D11), push ok,
  `core.autocrlf=false` set for this repo, no `gh`.
- `.gitattributes` committed; tracked files re-checked-out as LF.
- `.venv` (untracked) with `cambc==3.0.5`. It matches patch102 (evidence in M0.md item 1).
- M0.md: items 1–7, 14 and 16 recorded; items 8, 9 and 15 PC part only. All probing ran in the scratch
  workspace; the repo itself has no SDK, maps or bots yet.

**Waiting on owner**
- Decisions P1–P6 (asked in chat 2026-10-22):
  P1 what bootstrap commits from `cambc init`, P2 includes allowlist, P3 `.gitignore` anchoring,
  P4 manifest includes `cmake/`, P5 rewriting `tools/tests`, P6 the M0 probe workflow design.

**Next step**
- After the answers: record D12+ and update PLAN.md, then bootstrap (§1.1) in the repo, then add
  the probe workflow and `evo-env-test.yml` and push, then items 8/9 on Actions, then H2–H4.

**Open review issues (not yet decided)**
- 12 Build failure attribution (M0.md item 7 shows that `runner.log` names the bot). M1.
- 13 Wall-clock `--timeout` vs determinism. M1.
- 14 `similar()` undefined in gens 0–1 / without head-to-head. M2.
- 15 No source for map tags (§13.2). M2/M3.
- 16 Actions concurrency drops pending runs. M1.
- 17 Ruleset vs agents pushing as owner (§9.5). M0 item 12.
- 20 Network test vs §0.2.5. M1.
- 22 `claude --environment <id>` flag. M0 item 10.
- (18, 19, 21, 23 are now P3, P6, done via `PYTHONUTF8=1`, P5.)

**Local facts worth keeping**
- `claude` 2.1.293 has `--cloud [description|session_id|url]` and `--environment <environment_id>`.
- cambc cache on Windows: `%LOCALAPPDATA%\cambc\` (several old asset dirs).
- Run `benchmark.py` on Windows with `PYTHONUTF8=1` and `--runner .venv\Scripts\cambc.exe`.
- The scratch workspace (`cambc init` + probe bots) is in this session's scratchpad: disposable.
