# Progress

**Milestone:** M0. **HARD STOP (§0.5 item 12):** a cloud session pushed to `main`. Waiting on the owner
to agree to the H4 redesign below.

**Done**
- M0 items 1–11 and 13–16 recorded in M0.md. Probe session 2 (`session_01HY3quh12DiamDT3sG6K8bZ`) succeeded:
  setup 54 s, hook OK, repo absent during setup, `cse_`/`session_` ids match, branch `claude/m0-probe-eplb7b`.
- Two cloud sessions used so far (both approved): session 1 failed in setup (TLS proxy), session 2 succeeded.

**Item 12 result:** pushes to `main`, `evo-test` and `evo-results` were all accepted. The proxy pushes as the owner,
so the Repository-admin bypass covers agents too.

**Proposed H4 v2 (asked in chat 2026-10-22; not agreed yet)**
- Remove "Repository admin" from every bypass list. Bypass = **Deploy keys** only.
- `main`: Restrict updates + deletions + force pushes. `evo-results`: the same (this also replaces D19's interim setup).
- Two SSH deploy keys with write access: `evo-orchestrator` (private key in `~/.ssh` on the PC; this clone pushes over
  SSH with it via local `core.sshCommand`) and `evo-actions` (private key only in the Actions secret
  `EVO_RESULTS_DEPLOY_KEY`; the aggregate jobs push `evo-results` with it).
- Free test, no session: an HTTPS push from the PC as the owner (the same identity as the proxy) must be refused,
  and an SSH push with the deploy key must succeed.
- Clean up afterwards with normal commits: remove `evo/agents/probe/push-test-main.txt` from `main` and
  `m0/probe-push-test.txt` from `evo-results`; `evo-test` can stay.

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
