# Progress

**Milestone:** pre-M0. Waiting on H1.

**Done**
- Read PLAN.md, GameRules.md, patch notes 100–102, BotAPICpp.md, all of `tools/`.
- Review issues 1–11 answered by the owner; recorded as D1–D10 in
  `DECISIONS.md` and applied to PLAN.md (2026-10-22).
- H1 checklist in `docs/evolve/SETUP.md`.

**Waiting on owner**
- H1 reply (SETUP.md "Reply with" line).

**Open review issues (owner has not decided; raise again when the milestone needs them)**
- 12 Build failure attribution in a shard (§8, §15, §11): build each bot separately first. Needed by M1.
- 13 `benchmark.py --timeout` is wall-clock; slow runners can make "unfinished" games and spot-check mismatches (§10.4). M1.
- 14 `similar()` undefined in gens 0–1 (panel = starter only) and for bots without head-to-head games (§12.1). M2.
- 15 No source for map tags used by focus weighting (§13.2). M2/M3.
- 16 Actions concurrency keeps only one pending run; multi-job-file pushes unspecified (§9.1, §14). M1.
- 17 Ruleset may not separate orchestrator pushes from agent pushes if agents push as the owner (§9.5); depends on M0 item 12.
- 18 Unanchored `cambc/` and `build/` in `.gitignore`, and `eol=lf` on SDK files, may break `diff -rq` in check.sh (§6.1). M0 bootstrap.
- 19 M0.4 needs an Actions workflow before M1's `evo-eval.yml` exists (§16). M0.
- 20 §0.6 clone-and-compare test needs network, against §0.2.5. M1.
- 21 `benchmark.py` uses locale encoding for `runner.log` and stats; set `PYTHONUTF8=1` for the subprocess (§0.6). M0.
- 22 `claude --environment <id>` exists and may replace `settings.local.json` pinning (§6.1, §7). M0 item 10.
- 23 (new) `tools/tests/test_benchmark.py` runs `benchmark.py` with bots `v10`/`v06` and map `rows`, which do not exist in this repo, and uses a shebang runner plus SIGINT, which fail on Windows. It will fail as-is (§0.5 #15, §1.1 step 7). M0.

**Next step**
- After H1: M0 bootstrap (§1.1), then §0.5 items 1–9 and 14–16 into `docs/evolve/M0.md`.

**Local facts worth keeping**
- `core.autocrlf=true` globally; tracked text files are CRLF in the working tree.
- `claude` 2.1.293 has `--cloud [description|session_id|url]` and `--environment <environment_id>`.
- `gh` is not installed.
- `tools/tests/` is untracked in the owner's working copy (one file).
