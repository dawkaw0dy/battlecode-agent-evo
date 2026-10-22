# Evolutionary bot search: application plan

A population of Claude Code cloud sessions each writes a bot. GitHub Actions
plays the matches, a local orchestrator verifies the results, keeps the best
k% and refills the population with mutated children of the survivors. No
Claude API key is used: agents run as Claude Code cloud sessions on the
subscription, and evaluation runs no model at all.

## 0. Execution guide for the implementing agent

Read this section before anything else. The rest of the document is the
specification. This section says how to build it safely.

### 0.1 Where you run

Run as a **local Claude Code session on the owner's Windows PC**, in the local
clone of the new repository. Do not run as a cloud session. The orchestrator
starts cloud sessions with `claude --cloud` from this clone and replays games
locally for the spot-check, and neither works from inside a cloud VM.

| Part | Runs on | OS | Shell |
|---|---|---|---|
| You, the orchestrator, the spot-check | owner's PC | Windows | PowerShell or Git Bash; orchestrator is Python |
| Evolution agents | Claude Code cloud, `evo` environment | Ubuntu 24.04 | bash |
| Evaluation | GitHub Actions `ubuntu-latest` | Ubuntu | bash |

The repository starts with `GameRules.md`, `BotAPICpp.md`, `patch-notes/`,
`tools/`, `.gitignore`, `README.md` and this plan. Everything else is yours to create.

### 0.2 Ground rules

1. Build in milestone order, M0 to M5 (§16). A milestone is finished when its
   "Done when" test passes, the work is committed and pushed, and you have
   reported to the owner. Do not start the next milestone until the owner says so.
2. This document is the specification. If something you find contradicts it
   (an M0 result, a CLI that behaves differently, a step that cannot work), stop and
   ask. Record every agreed deviation in `docs/evolve/DECISIONS.md` with the date
   and the reason.
3. Never edit `GameRules.md`, `BotAPICpp.md` or `patch-notes/`. Change `tools/`
   only as §1.1 lists, plus moving `digest()` into `evo/eval/digest.py` (§8.4),
   the Windows interrupt fix in §0.6 and the asset-path fix in `replay.mjs` and
   `mapgen.mjs` (§1.1). Log each such change in `DECISIONS.md`.
4. Never run `cambc login`, `submit`, `bundle`, `match-groups`, `ladder` or
   `matches`. Never force-push. Push only to `main`, plus `evo-results` and
   `evo-test` during M0 and M1 tests.
5. Every orchestrator module gets `pytest` tests that run on Windows without
   `cambc` or network: rating, selection, mutation (same seed gives the same choices),
   admission (allowlist and scope against fixture repos), state transitions, result
   verification. Run them before each commit.
6. Make every orchestrator command idempotent and resumable (§5). The owner's
   PC sleeps and restarts.

### 0.3 Human checkpoints

Some steps happen in a browser or in GitHub settings, which you cannot reach.
For each one, write exact instructions for the owner (what to click, what to paste)
into `docs/evolve/SETUP.md`, then wait for the owner to confirm.

| # | When | Owner does | You need back |
|---|---|---|---|
| H1 | before M0 | confirm `claude auth login` is the subscription account; Python 3.12, Git and Node 20+ are installed; the clone can push | yes/no and versions |
| H2 | M0 | install the Claude GitHub App on this repository only | confirmation |
| H3 | M0 | create the `evo` environment at claude.ai/code with the setup script and variables you wrote into `SETUP.md` (§6.1) | the `env_…` id (you then write `.claude/settings.local.json`) |
| H4 | M0 | apply the rulesets on `main` and `evo-results` and the Actions settings from §9.5 | confirmation |
| H5 | M0, after the first cloud session | switch the environment's network to Custom with the hosts you recorded | confirmation |
| H6 | before every stop point in §0.4 | approve the number of cloud sessions | explicit yes |

### 0.4 Stop points before spending subscription usage

Every cloud session, and every message sent to one, counts against the owner's
plan limits. GitHub Actions minutes are free on a public repo, so workflow runs
need no approval. Never push job files in a loop, though: a failing workflow
re-pushed automatically is a bug.

Before each of these, tell the owner how many sessions will start, their deadline,
and what they will do, then wait for an explicit yes. An approval covers only
what was shown: for a generation that is at most `n − elites` sessions, up to
`relaunch_dead` relaunches per slot, and one nudge (§7.4) per session. Anything
beyond that needs a new approval.

| Stop point | Sessions |
|---|---|
| M0 probe session (§0.5 items 10–13; combine them into one session, at most two) | 1–2 |
| M3 real-agent test | 2 |
| M4: each generation | `n − elites` (plus relaunches) |
| M5 and every later generation | `n − elites` (plus relaunches) |

Build the same rule into the orchestrator. `[agents].require_approval = true`
(the default) stops every generation at `TASKS_PUSHED` until the owner runs
`python -m evo approve gGG`. `approve` first prints the maximum it approves
(sessions, relaunches, nudges, deadline) and records it in the state; the launcher
never exceeds it. `[agents].max_sessions_total` is a hard cap across
the whole run. Reaching it pauses the run.

### 0.5 M0 discoveries to record

Write each result in `docs/evolve/M0.md`: the command you ran, the output that
matters, and whether it matches the plan. The last column says what to do when it
does not match.

| # | Find out | Plan assumes | If different |
|---|---|---|---|
| 1 | newest `cambc` on PyPI, and evidence that its SDK/engine reflects `patch102.txt` (e.g. Stellar health 650, focus 70) | the newest release matches patch102 | **stop** and tell the owner if no installable version matches |
| 2 | files `cambc init --yes .` creates; whether it needs a login | `sdk/`, `maps/`, `bots/`, `cambc.toml`; no login | ask |
| 3 | non-interactive command that creates a C++ bot, and its layout | a single command; `bots/<name>/src/*.cpp, *.h` | if not `bots/<name>/src`, ask (`benchmark.py` depends on it) |
| 4 | map count M and names | some dozens | adjust `stage_a_maps` with the owner |
| 5 | every file a C++ bot needs to build (any CMakeLists, configs) | only sources | adjust the §8 allowlist, log it |
| 6 | SDK header names a bot includes | listable | becomes the §8.3 include allowlist |
| 7 | how a bot fault appears in the stats JSON (`diagnostics.botFaults`) and how to cause one | per-team counts | needed for the M2 faulting fake bot |
| 8 | same game twice on an Actions runner and once on the PC (§16 M0.4) | identical `outcome`, `gems`, `rounds` | **stop**: §10.4 must be redesigned |
| 9 | seconds per game, `nproc`, concurrent jobs on Actions | ~20 s, 4 cores, ~20 jobs | recompute §14, tell the owner |
| 10 | what `claude --cloud "<text>"` prints; whether it blocks; how to get the session id with stdin closed; whether `--output-format json` works for new sessions | id and URL are printed | design the launcher around what works, log it |
| 11 | in the probe session: is the repo present while the setup script runs; setup duration; failing hosts; a cache hit on the second session | repo absent, under 5 minutes | move steps between the setup script and the hook, log it |
| 12 | in the probe session: can it push to `main`? to `evo-test`? which branch name does it get? | own branch only, `main` refused | if `main` accepts, **stop** until H4 is fixed |
| 13 | in the probe session: values of `CLAUDE_CODE_REMOTE` and `CLAUDE_CODE_REMOTE_SESSION_ID` | `true` and `cse_…` | adjust the hook guard and `DONE.json` |
| 14 | can `cambc run-batch` run prebuilt WASM | unknown | optional optimisation (§17) |
| 15 | `tools/tests` and each kept tool on Linux (Actions) and on Windows | Linux all pass | mark Windows-only gaps in `TOOLS.md` |
| 16 | `tools/mapgen.mjs` and `tools/replay.mjs` find their assets under `~/.cache/cambc/assets` after `cambc init` (after the §1.1 glob fix) | yes | allow or forbid each in `TOOLS.md` |

Items 1–9 and 14–16 need no cloud session. Items 10–13 share one probe session
(stop point in §0.4). Its task: run the hook, print the two variables, try both
pushes, and write the findings to `evo/agents/probe/DONE.json` on its own branch.

Add a workflow `.github/workflows/evo-env-test.yml` (`workflow_dispatch` and
pushes touching `evo/env/**`). It runs the §6.1 setup script and then `check.sh`
with `EVO_FORCE_CHECK=1` on `ubuntu-latest`. That tests the cloud environment
scripts without using a cloud session.

### 0.6 Windows notes

- **Line endings decide the hashes.** `src_sha256` is computed on the PC at
  admission and again in Actions. Git's CRLF conversion would make them differ.
  The bootstrap commit adds `.gitattributes`: `* text=auto eol=lf`, plus
  `*.map26a binary` and `*.repl26a binary`. Hash files as bytes, never through text mode.
  Add a test that clones on Windows and compares digests with an Actions run.
- **`cambc` path.** On the PC it is `.venv\Scripts\cambc.exe`. Always pass `--runner`
  to `tools/benchmark.py`, whose default is the Linux path.
- **`benchmark.py` interrupts.** Its Ctrl+C path calls `os.killpg`, which does
  not exist on Windows. The spot-check runs without interrupts, but on Windows
  `process.terminate()`/`kill()` replaces `killpg` (logged in `DECISIONS.md`). This
  changes no game result.
- **Console encoding.** `benchmark.py` prints `─` and `’`. Run subprocesses with
  `encoding='utf-8', errors='replace'` and pass `--json --no-progress --color never`.
  Set `PYTHONIOENCODING=utf-8` for the orchestrator.
- **Python only, stdlib only.** The orchestrator uses `pathlib`, `subprocess`
  with argument lists (never `shell=True`), `tomllib`, `json`, `hashlib`, `random`.
  `pytest` is the only extra dependency. It never calls bash. The bash scripts
  (`check.sh`, setup script) run only in the cloud and in Actions, and their
  tests are the env-test workflow.
- **`claude --cloud` from Windows.** Run it with `cwd` set to the clone. If its
  output says it is uploading or bundling the local repository instead of cloning,
  the GitHub App is missing (H2): treat that as a launch failure. On Windows the
  bundle would include uncommitted changes.
- **Long-running loop.** `python -m evo run` runs in a terminal tab. Sleep or a
  reboot stops it, and starting it again resumes from `evo/state.json`. Offer the
  owner a Windows Task Scheduler entry only if they ask for one.
- **The SessionStart hook** also fires in your own local sessions. Its
  `CLAUDE_CODE_REMOTE` guard (§6.1) makes it exit at once there. Don't remove the guard.

## 1. Components and trust

| Component | Runs on | Trusted? | Does |
|---|---|---|---|
| Orchestrator (`evo/orchestrator`) | your PC, Python | yes | state machine, task files, launching sessions, admission, scoring, selection |
| Agent sessions | Claude Code cloud (`claude --cloud`) | **no** | write one bot on their own branch |
| Eval workflow (`.github/workflows/evo-eval.yml`) | GitHub Actions | yes | builds and plays games from `main` only |
| Spot-checker | your PC | yes | replays a random sample of games locally and compares |

Rules that make the trust split hold:

1. **Agent code reaches evaluation only through admission.** The orchestrator
   copies vetted files from an agent branch into `main` itself. The workflow
   never checks out an agent branch.
2. **Evaluation tooling comes only from `main`** (`tools/benchmark.py`, `evo/eval/*`,
   maps, the workflow file). An agent that edits them on its branch changes nothing.
3. **Bots are compiled C++ inside the engine's WASM sandbox.** The admission
   allowlist (§8) removes build-time code execution (no CMake, no `build.rs`,
   no Cargo), so a bot cannot touch the runner, the results or the token.
4. **Games are deterministic** (`benchmark.py` runs release, side-balanced,
   hashed inputs), so any result can be re-checked by replaying it.
5. **Match jobs have a read-only token**; only the aggregate job, which runs
   no bot code, can push, and only to `evo-results`.

Threats this covers: an agent editing the evaluator, maps, engine version or
workflow; an agent submitting a different bot than it described; a bot
faulting on purpose to void losses (§11 counts faults as losses); results
lost or duplicated in the pipeline; forged result files (spot-check plus
trigger-commit check). Collusion between sibling bots is irrelevant to
Stage A, which plays only fixed panel opponents.

### 1.1 The target repository starts empty

The system runs in a new repository that initially holds only:

- `GameRules.md`
- `BotAPICpp.md`
- `patch-notes/` (`patch100.txt` … `patch102.txt`)
- `tools/`
- `.gitignore`, `README.md`

There are no maps, SDK, bots, earlier reports or benchmark history.
`python -m evo bootstrap` creates the rest once, as the first commit after
those files (milestone M0):

1. Create `.venv` and `pip install cambc==<version>`. The version is pinned in
   `evo/config.toml` and in the §6.1 setup script, always together: the newest
   release on PyPI at M0 time whose SDK/engine reflects the newest patch note
   (`patch102.txt`). M0 records the evidence in `M0.md` (§0.5 item 1).
2. `cambc init --yes .` creates `sdk/`, `maps/`, `bots/` and `cambc.toml`. Check
   `sdk_version` in `cambc.toml` equals the pinned version.
3. Create the C++ starter bot as `bots/starter` (`cambc new`). Check it has the
   `bots/starter/src` layout `benchmark.py` expects.
4. Write `.gitattributes`: `* text=auto eol=lf`, `*.map26a binary`,
   `*.repl26a binary` (§0.6). Extend `.gitignore` with `.venv/`, `.cambc/`, `bots/*/.cambc/`, `bots/*/build/`,
   `benchmarks/`, `replays/`, `matches/`, `__pycache__/`, `.claude/settings.local.json`.
5. Prune `tools/`:
   - **Delete `scrim.py`.** It uploads bots to the platform account, and nothing
     in this system needs it.
   - **Delete `quick.sh`, `panel.sh`, `arena.sh` and the `sweep*.sh` and
     `variant*.sh` scripts.** They name bots from the old repo (v12-x, v14,
     v16), which don't exist here.
   - **Delete `bench.py`.** It calls a `cambc` binary at the repo root that
     does not exist; `bench2.py` covers it.
   - **Fix `pool.py`'s hard-coded Windows `cambc.exe` path**, so it also runs
     on Linux.
   - **Fix `replay.mjs` and `mapgen.mjs`:** find the asset directory under
     `~/.cache/cambc/assets/*/` by glob instead of the hard-coded
     `/root/.cache/cambc/assets/<hash>/` path, and read the replay zip in Node
     instead of calling `unzip`.
   - **Keep** `benchmark.py`, `bench2.py`, `analyze.py`, `teamstats.py`,
     `replay.mjs`, `mapgen.mjs` and `tests/`.
6. Add `evo/` (orchestrator, eval scripts, env scripts, prompts, archetypes)
   and `.github/workflows/evo-eval.yml`.
7. Run the `tools/tests` and one `starter` vs `starter` game. Write
   `evo/env/manifest.json` (§6.1), commit and push.

The map pool is whatever `cambc init` downloads for the pinned version. Its
size is written `M` below (the old repo had 40). From here on, the repo holds
nothing an agent did not get from these files or from earlier agents.
Agents can read `evo/`, but the scope check (§8) rejects any change to it.

## 2. Repository layout

```
evo/
  __main__.py                  CLI (`python -m evo`): bootstrap | init | step | run | status | approve | verify | spotcheck
  config.toml                  population and budget knobs (§3)
  state.json                   orchestrator state, committed after every step
  orchestrator/
    state.py                   load/save, generation state machine (§5)
    tasks.py                   renders evo/tasks/<id>.md from templates (§6)
    launch.py                  claude --cloud wrapper, session bookkeeping (§7)
    collect.py                 finds DONE markers on remote branches (§7)
    admit.py                   diff scope, allowlist, copy into main (§8)
    jobs.py                    builds eval job files, result cache (§9)
    verify.py                  result checks and local spot-check (§10)
    rating.py                  fault attribution, Bradley–Terry, bootstrap (§11)
    select.py                  survivors, allocation, diversity filter (§12)
    mutate.py                  seeded operator/focus/evidence sampling (§13)
  prompts/
    agent.md                   fixed contract every task includes
    operators/*.md             one fragment per mutation operator
    focus/*.md                 one fragment per focus area
    archetypes/*.md            immigrant starting strategies
  eval/
    digest.py                  src_sha256, shared with tools/benchmark.py (§8.4)
    check_job.py               re-verifies a job file inside Actions
    extract.py                 benchmark output dir -> per-game rows
    aggregate.py               shard rows -> one results file
  tasks/<agent>.md             rendered task per agent (committed before launch)
  jobs/<job_id>.json           eval requests (pushing one triggers the workflow)
  agents/<agent>/              admitted DONE.json per agent
  lineage.jsonl                one row per agent: parents, operator, seed, outcome
bots/<agent>/src/              admitted bot sources, e.g. bots/e03-05/src
bots/<agent>/NOTES.md          the agent's genome notes (outside src, not hashed)
.github/workflows/evo-eval.yml
branch evo-results             results/<job_id>.json, written only by Actions
```

Agent ids are `eGG-SS` (generation, slot), for example `e03-05`. They are valid
`benchmark.py` bot names (a single path component under `bots/`).

## 3. Configuration (`evo/config.toml`)

```toml
seed = 20261022            # master seed; every random choice derives from it
population = 8             # n
keep_fraction = 0.25       # k; survivors s = max(2, ceil(n*k))
elites = 1                 # top bots carried over frozen, no new session
immigrants = 1             # fresh agents from an archetype per generation
language = "cpp"           # only C++ is admitted (§8)

[agents]
max_parallel = 4           # cloud sessions running at once
require_approval = true    # pause each generation until `python -m evo approve gGG` (§0.4)
max_sessions_total = 40    # hard cap on cloud sessions for the whole run
deadline_minutes = 180     # no DONE marker by then -> slot is dead
relaunch_dead = 1          # relaunch attempts per dead slot per generation
local_game_budget = 120    # games an agent may play while developing

[eval]
runner_version = "cambc==X.Y.Z"      # pinned in M0 (§1.1); must equal sdk_version in cambc.toml
map_pool = "maps"                  # the M maps `cambc init` downloaded (§1.1)
heldout_pool = ""                  # optional private repo path (§9.4)
panel = ["starter"]                # fixed opponents; plus hall of fame below
hall_of_fame_in_panel = 3          # until 2 exist, Stage A is a round robin (§11)
stage_a_maps = 16                  # rotating subset, chosen by generation seed
stage_b_top = 4                    # bots promoted to the full round robin
shard_games = 40                   # games per matrix job
jobs_per_runner = 0                # 0 = nproc
spotcheck_fraction = 0.02
spotcheck_min = 4
max_fault_rate = 0.10              # above this a bot is disqualified

[mutation]
operator_weights = { tweak = 0.40, subsystem = 0.30, crossover = 0.20, radical = 0.10 }
focus = ["camps", "combat", "spells", "pathing", "opening", "endgame", "defense", "fuel", "map-adaptive"]
evidence_losses = 3
immigrant_base = "starter"         # or "hall_of_fame" from generation 2 (§13.4)
```

## 4. Data formats

**`evo/state.json`**

```json
{
  "generation": 3,
  "phase": "EVAL_A_PENDING",
  "agents": {
    "e03-05": {
      "slot": 5, "kind": "child", "parents": ["e02-01"], "operator": "subsystem",
      "focus": "combat", "seed": 918273, "task_sha256": "…",
      "session": {"id": "…", "url": "…", "launched_at": "…", "attempt": 1},
      "branch": null, "commit": null,
      "status": "running",
      "src_sha256": null
    }
  },
  "jobs": {"g03-A": {"pushed_commit": "…", "status": "pending", "retries": 0}},
  "hall_of_fame": ["e01-03", "e02-01"],
  "cache_file": "evo/cache.jsonl"
}
```

`status` is one of `planned, running, done, rejected, dead, admitted, disqualified, rated`.

**Task file `evo/tasks/e03-05.md`:** the rendered prompt (§6). Its SHA-256 goes
into the state and must come back in `DONE.json`.

**Agent marker `evo/agents/e03-05/DONE.json`** (written last, on the agent branch):

```json
{"agent": "e03-05", "task_sha256": "…", "base_commit": "…",
 "session_id": "<value of $CLAUDE_CODE_REMOTE_SESSION_ID>",
 "bot_dir": "bots/e03-05", "self_eval": {"vs": "e02-01", "games": 32, "wins": 19},
 "summary": "one line"}
```

**Job file `evo/jobs/g03-A.json`:**

```json
{"job_id": "g03-A", "generation": 3, "stage": "A", "runner_version": "cambc==X.Y.Z",
 "env_manifest_sha256": "…",
 "src_sha256": {"e03-05": "…", "starter": "…"},
 "map_sha256": {"<map1>": "…"},
 "shards": [{"a": "e03-05", "b": "starter", "maps": ["<map1>", "<map2>"]}]}
```

**Result row** (one per game, in `results/<job_id>.json` on `evo-results`):

```json
{"a": "e03-05", "b": "starter", "map": "<map1>", "side": "A",
 "outcome": "teamA|teamB|unfinished", "gems": {"teamA": 812, "teamB": 640},
 "rounds": 1432, "faults": {"teamA": 0, "teamB": 0}, "bot_errors": {"teamA": 0, "teamB": 0},
 "src_sha256": {"e03-05": "…", "starter": "…"}, "map_sha256": "…",
 "stats_sha256": "…", "replay_sha256": "…"}
```

The result file header carries `job_id`, `trigger_sha` (`github.sha`), `run_id`,
`run_attempt`, `workflow_sha256` and the `cambc --version` output.

Side `A` means bot `a` played team A. Both sides are always played.

**Cache `evo/cache.jsonl`:** result rows keyed by
`(src_sha256[a], src_sha256[b], map_sha256, side, runner_version)`. Games are
deterministic, so a cached pairing is never replayed. Elites and panel games
come from the cache in every later generation.

## 5. Generation lifecycle

`python -m evo step` advances the current generation as far as it can and
exits. It is idempotent and safe to interrupt. `python -m evo run` calls `step`
every 5 minutes. Every phase change commits `evo/state.json`.

| Phase | Action | Next when |
|---|---|---|
| `PLANNED` | `select`+`mutate` produced the slot list (gen 0: §12.4) | always |
| `TASKS_PUSHED` | render `evo/tasks/*.md`, commit and push to `main` | push succeeded |
| `RUNNING` | launch up to `max_parallel` sessions; poll markers (§7); relaunch dead slots | every slot is `done`, `dead` or `rejected` |
| `ADMITTED` | admission (§8) for each `done` agent, one commit to `main` | always |
| `EVAL_A_PENDING` | push `evo/jobs/gGG-A.json` (uncached games only) | results file for `trigger_sha` appears |
| `EVAL_A_DONE` | verify (§10), merge rows into cache, Stage A ranking | verify passed |
| `EVAL_B_PENDING` | push `gGG-B.json` | results file appears |
| `EVAL_B_DONE` | verify, rate (§11) | verify passed |
| `SELECTED` | survivors, hall of fame, write `lineage.jsonl`, plan gen+1 | → `PLANNED` of gen+1 |

Elites skip `TASKS_PUSHED` to `ADMITTED`; they keep their id and files.
Fewer than `s + 1` admitted bots after relaunches pauses the run with a
`status` message instead of evaluating a generation too small to select from.

## 6. Agent task

The prompt sent to the cloud session is one line, so the CLI argument stays short
and the full prompt is auditable in git:

```
You are evolution agent e03-05. Read evo/tasks/e03-05.md on main and follow it exactly.
```

`evo/tasks/e03-05.md` = `prompts/agent.md` (fixed) + rendered slots:

```markdown
# Task e03-05 (generation 3, slot 5)
Task sha256 is the hash of this file; copy it into DONE.json.

## Rules
- Confirm the SessionStart hook printed `evo environment OK`; otherwise run `bash evo/env/check.sh`. If it fails, follow §6.1 (env_failed) and stop.
- Read, in order: GameRules.md, patch-notes/*.txt (they override the rules), BotAPICpp.md, evo/env/TOOLS.md.
- Environment manifest sha256: {env_manifest_sha256}. C++ only.
- Your bot lives in bots/e03-05/src. Allowed files: *.cpp, *.h, *.hpp.
  Includes: standard headers, the SDK headers listed in evo/env/TOOLS.md, or files in your own src directory, plain names only.
- Change nothing outside bots/e03-05/ and evo/agents/e03-05/. Other changes reject the whole submission.
- Work on your own branch. Never push to main or evo-results.
- Test with: python tools/benchmark.py e03-05 <opponent> --maps <…> --jobs $(nproc) --output /tmp/<dir>
  Budget: at most 120 games in total. Your self-reported results are not used for selection.

## Starting point
Copy bots/e02-01/src into bots/e03-05/src. Read bots/e02-01/NOTES.md first,
especially "Tried and failed".

## Mutation: subsystem rewrite
{operators/subsystem.md}

## Focus: combat
{focus/combat.md}

## Evidence: the parent's worst losses last generation
| Opponent | Map | Side | Gems (parent – opp) | Rounds |
|---|---|---|---|---|
| e02-03 | <map> | B | 412 – 980 | 1201 |
| e02-04 | <map> | A | 0 – 300 | 640 |
| e01-02 (hall of fame) | <map> | A | 530 – 801 | 1500 |

## Finish
1. `cambc build bots/e03-05 --release` passes with no warnings you introduced.
2. bots/e03-05/NOTES.md in the format below.
3. evo/agents/e03-05/DONE.json last, then commit and push.

## NOTES.md format
## Strategy (≤10 lines)
## Key parameters
## Changes this generation
## Tried and failed (keep entries from the parent's notes; add yours)
## Known weaknesses
```

### 6.1 Starting conditions

Every agent starts from the same verified state, whichever slot or
generation it is. Bots are evaluated in Actions, so a broken agent
environment only costs that agent's quality, never the integrity of the scores.
The point here is that no agent wastes its session on setup or works
from stale rules.

**What `main` must contain before any launch.** The orchestrator checks this in
`TASKS_PUSHED` and refuses to launch otherwise:

| Item | Path | Check |
|---|---|---|
| Game rules | `GameRules.md` | present, hash recorded |
| Bot API | `BotAPICpp.md` | present, hash recorded |
| Patch notes | `patch-notes/patch100.txt` … `patch102.txt` (all files in the folder) | present, hashes recorded |
| SDK | `sdk/cpp/*`, `cambc.toml` | `sdk_version` in `cambc.toml` equals the pinned CLI version |
| Maps | `maps/*.map26a` (the M maps from `cambc init`) | committed, hashes recorded |
| Tools | `tools/`, pruned as in §1.1 | committed; `tools/tests` pass on Linux |
| Environment scripts | `.claude/settings.json` (SessionStart hook), `evo/env/check.sh`, `verify_manifest.py`, `fingerprint.py`, `TOOLS.md` | committed |
| Bots | `bots/starter`, then every admitted bot (panel and hall of fame included) | committed |
| No local leftovers | — | `git status --porcelain` is empty for every path in this table |

The bootstrap commit (§1.1) produces the first state that passes this table.
Cloud sessions clone from GitHub, so nothing counts until it is pushed.

The orchestrator writes `evo/env/manifest.json` with every hash above plus the
CLI version. Every task file embeds that manifest's hash. The Actions workflow
checks the same hashes (§9.2), so agents, workflow and spot-check share one
definition of the game.

**How a cloud session gets this repository and environment.** The repo is
picked by where `claude --cloud` runs, and the environment by a setting:

- **Repository and branch:** `claude --cloud` uses the GitHub remote of the
  directory it runs in, at the current branch. It clones from GitHub, not from
  the local files. The orchestrator therefore runs every launch with
  `cwd = <local clone of the new repo>`, on `main`, after checking that
  `git status --porcelain` is empty and `HEAD == origin/main`. If the branch is not on
  GitHub, the CLI prints `Branch <name> is not on GitHub` and starts from
  another commit. The launcher treats that line as a launch failure.
- **The Claude GitHub App must be installed on the new repo** (choose "only
  select repositories" and pick this one). Without it, `claude --cloud` does not
  clone. It uploads a bundle of the local checkout instead, and on native
  Windows that bundle includes uncommitted changes as they are. Agents could
  then start from a state that is not on `main`.
- **Environment:** create one personal environment, `evo`, in the environment
  selector at claude.ai/code. Environments belong to your account, not to a repo.
  To pin it for this repo only, put its id in the local clone's
  `.claude/settings.local.json` (gitignored, not committed):
  `{"remote": {"defaultEnvironmentId": "env_…"}}`. Running `/remote-env` once sets
  the same key, but at user level for every project. Project-level settings take
  precedence, so the other repos keep their own default.

**The `evo` environment** (configured at claude.ai/code):

- **Setup script:** installs only what does not depend on the repo, because the
  result is snapshotted and shared by every session for about 7 days. It must
  finish in about 5 minutes, or it is not cached:

  ```bash
  #!/bin/bash
  set -euo pipefail
  VER=X.Y.Z                      # pinned in M0; keep equal to [eval].runner_version
  pip install --quiet "cambc==$VER"
  # Reference workspace: warms ~/.cache/cambc (wasi-sdk, CMake, Ninja) and
  # gives check.sh a clean SDK copy to compare against.
  mkdir -p /opt/cambc-ref && cd /opt/cambc-ref
  cambc init --yes >/dev/null
  cambc new --help >/dev/null    # M0: replace with the non-interactive command that creates a C++ bot
  cambc build bots/<starter> --release
  ```

  Changing the script or the network settings rebuilds the snapshot.
  Bumping the CLI version means editing `VER` here and in `evo/config.toml` together.
- **Network:** Trusted covers PyPI. The toolchain downloads (wasi-sdk, CMake,
  Ninja) may come from hosts outside the Trusted list. The setup script runs
  before the session's GitHub proxy starts, so M0 runs setup once, notes the
  hosts that fail, and switches to Custom access with those hosts added.
- **Environment variables:** `CAMBC_NO_UPDATE_CHECK=1`,
  `BASH_DEFAULT_TIMEOUT_MS=600000` (so benchmark runs are not pushed to the
  background after 2 minutes). No secrets.

**Per-session checks** run from the repo itself, as a SessionStart hook in
`.claude/settings.json` (committed). It runs after the clone, on every session,
and agents cannot change it for other sessions:

```json
{"hooks": {"SessionStart": [{"matcher": "startup|resume",
  "hooks": [{"type": "command", "command": "bash \"$CLAUDE_PROJECT_DIR/evo/env/check.sh\"", "timeout": 300}]}]}}
```

`evo/env/check.sh`:

```bash
set -euo pipefail
# The hook fires in every Claude Code session opened in this repo, including the
# implementing agent's and the orchestrator's local sessions on Windows, where
# /opt/cambc-ref does not exist. Only cloud sessions set CLAUDE_CODE_REMOTE=true.
# EVO_FORCE_CHECK=1 runs the check anywhere else on purpose (Linux only).
if [ "${CLAUDE_CODE_REMOTE:-}" != "true" ] && [ "${EVO_FORCE_CHECK:-}" != "1" ]; then
  exit 0
fi
cd "${CLAUDE_PROJECT_DIR:-$(git rev-parse --show-toplevel)}"
VER=$(sed -n 's/^sdk_version *= *"\(.*\)"/\1/p' cambc.toml)
test "$(cambc --version)" = "cambc $VER"
diff -rq /opt/cambc-ref/sdk/cpp sdk/cpp          # committed SDK matches the CLI
python3 evo/env/verify_manifest.py               # docs, patch notes, maps, tools hashes
mkdir -p .venv/bin && ln -sf "$(command -v cambc)" .venv/bin/cambc   # benchmark.py's default runner
MAP=$(ls maps/*.map26a | head -1 | xargs basename -s .map26a)
python3 tools/benchmark.py starter starter --maps "$MAP" --jobs 2 \
  --output "$(mktemp -d)/smoke" --json --no-progress --color never >/dev/null
python3 evo/env/fingerprint.py > /tmp/evo-env.json
echo "evo environment OK"
```

`fingerprint.py` prints the CLI version, the manifest hash, `nproc`, the Python
and Node versions, and whether the smoke game finished.

The hook's output is visible to the agent. Its first instruction is to confirm
`evo environment OK` was printed, or to run `bash evo/env/check.sh` itself. If the
check fails, the agent writes `DONE.json` with `"env_failed": true` and the
output, then stops. The orchestrator relaunches the slot at once instead of
waiting for the deadline, if the generation's approval (§0.4) still has a
relaunch left for that slot; otherwise the slot is `dead`. It pauses the run if two slots fail this way in one
generation, because then the environment is broken, not the agent.

**Reading list** (in this order, part of `prompts/agent.md`):

1. `GameRules.md`
2. `patch-notes/*.txt` in numeric order. Where a patch note and `GameRules.md`
   disagree, the patch note wins. A later patch overrides an earlier one.
3. `BotAPICpp.md`
4. `evo/env/TOOLS.md`
5. The parent's `NOTES.md` (both, for crossover), or the archetype file for an immigrant.

Agents get no reports or research from earlier projects. Everything they know
about strategy comes from these files and from their ancestors' `NOTES.md`, so
the "Tried and failed" lists are the population's only memory. Children inherit
them and must keep them.

**`evo/env/TOOLS.md`** lists what agents may use:

| Tool | Use | Status |
|---|---|---|
| `tools/benchmark.py` | main A-vs-B test, both sides, deterministic | allowed (the eval tool) |
| `tools/bench2.py`, `tools/teamstats.py`, `tools/analyze.py` | quick matches and per-game stats | allowed |
| `tools/replay.mjs` | dump a replay to JSON to see why a game was lost | allowed if M0 confirms it finds its assets (§0.5 item 16) |
| `tools/pool.py` | many candidates vs many opponents | allowed (path fixed in bootstrap) |
| `tools/mapgen.mjs` | read or generate maps | allowed if M0 confirms it finds its assets (§0.5 item 16) |
| `cambc login`, `submit`, `bundle`, `match-groups`, `ladder`, `matches` | **forbidden**: they act on the team's platform account, and an upload replaces the live ladder bot |
| `cambc init`, `cambc maps` in the repo | **forbidden**: they rewrite tracked SDK and map files (scope check rejects it anyway) |

The game budget (`local_game_budget`) applies to all tools together. Agents
report games played in `DONE.json.self_eval`.

## 7. Launching and collecting sessions

`launch.py`:

1. Preconditions: the local clone is on `main`, clean, and equal to `origin/main`
   (the task files are pushed), and `.claude/settings.local.json` pins the
   `evo` environment.
2. Run `claude --cloud "<one-line prompt>"` in the local clone. Parse the
   session id and `claude.ai/code/…` URL from its output into
   `agents[*].session`. Treat a `Branch … is not on GitHub` line or a non-zero
   exit as a launch failure. The CLI shows a live setup checklist while the VM
   starts. M0 settles how to run it non-interactively (stdin closed, a
   timeout once the id is printed, or `-p`/`--output-format json` if
   supported for new sessions).
3. At most `max_parallel` sessions are `running` at once. Others wait as `planned`.
4. Nudges: at `deadline_minutes − 20` the launcher sends
   `claude -p "20 minutes left: finish, write NOTES.md and DONE.json, push." --cloud <session-id>`.
   This queues a message into the running session and exits. At most one nudge
   per session, and only within the generation's approval (§0.4).
5. A slot is `dead` after `deadline_minutes` with no valid marker. It is
   relaunched with the same task file up to `relaunch_dead` times, within the
   generation's approval (§0.4). The attempt number increments; the task, and
   so every sampled choice, stays the same.
6. After admission or death, the orchestrator archives nothing by itself.
   Finished sessions stay in your claude.ai/code sidebar for you to read.

`collect.py`, on each step:

1. `git fetch origin --prune`.
2. For each remote branch whose tip is newer than the earliest launch, check
   `git cat-file -e <branch>:evo/agents/<id>/DONE.json` for each running agent.
   The branch name does not matter, so cloud branch naming rules (for example
   a forced `claude/…` prefix) are harmless.
3. Take the newest such commit. Valid only if `DONE.json.agent == id`,
   `task_sha256` matches the state, `base_commit` is an ancestor of the
   branch, and `session_id` (with its `cse_` prefix read as `session_`) is the session
   the launcher started for this slot. A marker from another session, such as a
   stale earlier attempt, is ignored. Record `branch`, `commit`, set `done`.

## 8. Admission (orchestrator, local)

For each `done` agent, all checks against `git diff --name-only <base_commit> <commit>`:

1. **Scope.** Every changed path is under `bots/<id>/` or `evo/agents/<id>/`.
   Otherwise `rejected`.
2. **Allowlist.** `bots/<id>/src` contains only `*.cpp`, `*.h`, `*.hpp`, regular
   files, no symlinks; at most 64 files, 300 KB per file, 1 MB in total.
3. **Includes.** Every `#include` is `<name>` from the standard library list
   bundled with the SDK, `"cambc.h"`-style SDK headers, or `"name.h"` for a file in
   the same directory. `/`, `\`, `..`, `#embed`, `__has_include`, `asm` and
   `#pragma` other than `once` reject the bot.
4. **Digest.** Compute `src_sha256` with the `digest()` from
   `tools/benchmark.py`, moved into `evo/eval/digest.py` and imported from both.
   The one change: the relative path is hashed as `relative_to(...).as_posix()`,
   so Windows and Linux give the same digest (on Linux the value is unchanged).
5. **Copy.** Write `bots/<id>/src`, `bots/<id>/NOTES.md` and `evo/agents/<id>/DONE.json`
   from the agent commit into `main`, one commit `evo: admit generation GG`.
   Status `admitted`.

The build happens in Stage A. A bot that fails to build loses every game in
that shard and is `disqualified`.

## 9. Evaluation workflow

### 9.1 Trigger and structure

```yaml
name: evo-eval
on:
  push:
    branches: [main]
    paths: ['evo/jobs/*.json']
concurrency: { group: evo-eval, cancel-in-progress: false }
permissions: { contents: read }

jobs:
  plan:            # reads the job file added in this push, outputs the shard matrix
  shard:           # matrix: one entry per shard, fail-fast: false, timeout-minutes: 340
    needs: plan
    strategy: { fail-fast: false, matrix: { include: '${{ fromJSON(needs.plan.outputs.shards) }}' } }
  aggregate:       # needs: shard, if: always(), permissions: contents: write
```

### 9.2 `shard` steps

1. `actions/checkout` at `github.sha`, depth 1.
2. Python 3.12, `python -m venv .venv && .venv/bin/pip install <runner_version>`.
   Cache `~/.cache/cambc` keyed on the runner version.
3. `python evo/eval/check_job.py evo/jobs/<id>.json --shard <i>`: check
   `evo/env/manifest.json` against the job's `env_manifest_sha256` and the
   files on disk, recompute
   `src_sha256` of both bots and `map_sha256` of the shard's maps, compare with
   the job file, re-run the §8 allowlist. Any mismatch fails the shard.
4. `python tools/benchmark.py <a> <b> --maps … --jobs $(nproc) --output out --json --no-progress --color never`.
   A non-zero exit is expected when games fault; continue.
5. `python evo/eval/extract.py out --job <id> --shard <i> > rows.json`: reads
   `out/<map>-<side>.json` and the replay zip directly. `benchmark.py`'s own
   `results.json` keeps the fault counts, but it drops gems and outcome detail
   for games whose stats file is missing or unreadable, and the row needs the
   exact §4 fields.
6. Upload `rows.json` as an artifact (and `out/*.repl26a` with 7-day retention).

### 9.3 `aggregate` steps

1. Download all shard artifacts.
2. `python evo/eval/aggregate.py` writes `results/<id>.json` with the §4 header.
   Missing shards are listed in `missing_shards`, not silently dropped.
3. Check out `evo-results`, add the file, commit as `github-actions[bot]`, push.

### 9.4 Optional held-out maps

To keep agents from tuning to the M maps they can see, put a frozen pool of
generated maps (`tools/mapgen.mjs`) in a separate private repo that cloud
environments do not have access to. The workflow checks it out with a deploy
key secret and passes `--map-dir`. Held-out maps are used only in Stage B and
Stage C. Their hashes go in the job file as usual.

### 9.5 Repository settings

The repository is public and owned by you. Orchestration runs only on your PC.
Nothing Claude-related runs in the repo or in Actions.

- Ruleset on `main` and `evo-results`: only you (the orchestrator) and
  `github-actions[bot]` may push. Each cloud session pushes only to its own branch.
  Milestone M0 confirms this.
- Actions enabled, workflow token default read-only.
- The workflow triggers only on `push` to `main`, never on `pull_request` or
  `pull_request_target`. Anyone can fork a public repo and open a PR, and that
  must never run the eval or touch `evo-results`.
- Disable "Allow GitHub Actions to create and approve pull requests". Require
  approval for workflows from outside collaborators.
- Everything committed is public: bots, `NOTES.md`, results. A held-out map
  pool (§9.4) must live in a separate private repo, read through a deploy-key
  secret. Secrets are not exposed to fork PRs.
- Install the Claude GitHub App on this repo only. Cloud sessions use it to
  clone and push their branches, and `claude --cloud` needs it to clone from
  GitHub instead of uploading your local checkout (§6.1). It is a GitHub
  permission, not Claude Code running in the repo or in Actions.

## 10. Verification (orchestrator, after each results file)

1. The results commit is on `evo-results` and its header has
   `trigger_sha == jobs[id].pushed_commit`, the `job_id`, and a
   `workflow_sha256` equal to the hash of `.github/workflows/evo-eval.yml` at that commit.
2. Every requested (pair, map, side) appears exactly once. Missing games are
   re-queued in a `…-retry1` job, at most twice. After that they count as
   discarded, and the generation report lists them.
3. Every row's `src_sha256` and `map_sha256` equal the job file.
4. **Spot-check.** From the generation seed, pick `max(spotcheck_min,
   spotcheck_fraction × games)` games and replay each locally with
   `benchmark.py <a> <b> --maps <m>`. `outcome`, `gems` and `rounds` must match
   exactly. Any mismatch stops the run (`phase: HALTED`) for a human to look at.
   This is the only local compute: about 4–30 games per generation.

## 11. Scoring

Per game, from bot X's view:

| Game | Counts as |
|---|---|
| finished, no faults or caught exceptions | win or loss by `outcome` |
| only X faulted or threw | loss for X |
| both faulted | discarded |
| unfinished, no faults | discarded |

A bot with more than `max_fault_rate` of its games faulted is `disqualified`
(strength = −∞). This removes any reason to fault on purpose.

**Strength.** Bradley–Terry fit by MM iteration over all valid games of the
generation, panel bots included. Each bot gets a prior of one virtual win and
one virtual loss against a fixed average player, so the fit stays finite when
a bot wins or loses everything. Ties in strength break by mean gem margin.

**Uncertainty.** Cluster bootstrap over maps (resample maps, both sides
together), 200 replicates, report a 90% interval per bot.

**Stage A** (screen): every admitted bot of the generation, elites included, vs
every panel bot on `stage_a_maps` maps × 2 sides. The panel is `starter` plus the
last `hall_of_fame_in_panel` champions. Rank by Stage A strength.

There is no strong fixed opponent at the start, and every bot soon beats
`starter` on every map. So while the hall of fame has fewer than 2 bots
(generations 0 and 1), Stage A is a round robin of the whole generation on
`stage_a_maps`, plus `starter`. From generation 2, Stage A uses the panel as above.

**Stage B** (final): the top `stage_b_top` from Stage A play a round robin on
all maps (plus the held-out pool) and fill in their remaining panel games.
Final strength is the fit over Stage A + B.

**Stage C** (only when needed): if survivor s and the first non-survivor have
overlapping intervals and a held-out pool exists, play those two bots against
each other and the panel on held-out maps not used yet, then refit.

## 12. Selection and repopulation

### 12.1 Survivors

```
s = max(2, ceil(n * keep_fraction))
ranked = sort(non-disqualified bots of gen G by final strength, desc)
survivors = []
for b in ranked:
    if any(similar(b, x) for x in survivors): continue   # diversity filter
    survivors.append(b)
    if len(survivors) == s: break
```

`similar(b, x)`: the per-map win vectors against the panel correlate above
0.9 **and** their head-to-head is between 45% and 55%. Two bots that play the
same strategy count as one, and the next distinct bot survives instead.

Hall of fame: append `survivors[0]` if it is not already in it.

### 12.2 Next generation slots

```
next = survivors[:elites]                         # frozen, re-evaluated from cache
children = n - elites - immigrants
weights  = [s - r for r in range(s)]              # rank 0 gets s, last gets 1
alloc    = largest_remainder(children, weights)    # e.g. n=8,s=2 -> 4 and 2
for parent, count in zip(survivors, alloc):
    for _ in range(count):
        next.append(child_of(parent, rng))
for _ in range(immigrants):
    next.append(immigrant(rng))
```

Non-survivors are discarded. Their bots stay in git, their `NOTES.md`
"Tried and failed" entries are merged into the children's tasks as
"Ideas other agents tried this generation that did not survive".

### 12.3 Randomness

`rng = Random(int(sha256(f"{seed}:{generation}:{slot}").hexdigest(), 16))`.
A relaunch reuses the same task file and therefore the same choices, so the
attempt number is not part of the key. Every choice in §12–§13
draws from it and is written to `lineage.jsonl`, so a generation can be
regenerated exactly. The seed itself never appears in the prompt, because a
number gives the model nothing to act on. Variance comes from the sampled
instructions.

### 12.4 Generation 0

No survivors and no strong bot exist yet. All `n` slots are immigrants
starting from `bots/starter`, each with a different archetype (§13.4), so
`archetypes/` needs at least `n + 2` files: generation 0 uses `n`, and the
"not used in the previous two generations" rule then needs spares for
generations 1 and 2. Generation 0 explores whole strategies.
Mutation only starts in generation 1.

Generation-0 agents also have the most work: they write a whole bot from the
starter. Set their deadline to 1.5 × `deadline_minutes`.

## 13. Mutation

### 13.1 Operators

| Operator | Weight | Child is told to |
|---|---|---|
| `tweak` | 0.40 | keep the structure; change parameters and small decision rules in the focus area; measure each change |
| `subsystem` | 0.30 | rewrite the focus subsystem from scratch behind the same interface |
| `crossover` | 0.20 | start from parent A; port parent B's handling of the focus area; both NOTES.md and both sources are given |
| `radical` | 0.10 | keep only the parent's infrastructure (movement, pathing, API wrappers) and build a different overall strategy |

**Crossover partner.** Among the other survivors, pick the one maximising
`sum_m max(winA_m, winB_m) − sum_m winA_m` over the per-map win rates: the
partner that is strongest where A is weakest. With only one survivor,
crossover falls back to `subsystem`.

### 13.2 Focus

Sample from `[mutation].focus`, weighted by the parent's losses: each focus
area has a list of map tags (`collision`, `lanes`, `gem fields`, …). Weight =
1 + the number of evidence losses on maps with that tag. A focus the same
lineage used in the previous generation gets half weight.

### 13.3 Evidence

The parent's `evidence_losses` worst valid losses from the final ratings:
the lowest gem margin, at most one per map, with opponent, map, side, gems and rounds.
This is the strongest single input: it tells the child where to look.

### 13.4 Immigrants

`archetypes/*.md` hold at least `n + 2` one-page strategies (for example rush, camp
monopoly, defensive turtle, late-game gem hoard). They are written once in M3
from `GameRules.md` and the patch notes only, so they carry no knowledge
that agents could not have derived themselves. An immigrant gets one that
no immigrant used in the previous two generations, starting from `bots/starter`.
From generation 2 an immigrant may instead start from a hall-of-fame bot's
infrastructure with a new archetype, if the starter has become too far behind
to compete (config switch `immigrant_base`).

## 14. Budget

Per generation with the defaults (n = 8, panel 3, 16 Stage A maps, top 4),
shown for M = 40 maps:

| Stage | Games before cache |
|---|---|
| A: 8 bots × 3 panel × 16 maps × 2 | 768 |
| B: 6 pairs × M maps × 2 | 480 |
| B: top-4 remaining panel games, (M − 16) maps × 3 × 2 × 4 | 576 |
| **total** | **≈1,800** (≈1,300 after elite and panel cache hits) |

Generations 0 and 1 use the round-robin Stage A instead:
28 pairs × 16 × 2 + 8 × `starter` × 32 ≈ 1,150 games, plus Stage B.

Runner minutes ≈ games × t / cores / 60 + about 3 minutes setup and build per shard.
`t` (seconds per game on a runner) is measured in M0. The repo is public, so
standard runners are free with no monthly minute limit, and they have 4 cores.
At t = 20 s a generation takes ≈ 110 runner minutes. Spread over up to 20
concurrent jobs, that is under 15 minutes of wall-clock time. The real limits
are the 6-hour cap per job (`shard_games` keeps shards far below it) and
256 matrix entries per workflow run. A job with more shards is split into
several job files.

Subscription: each generation runs `n − elites` sessions of up to
`deadline_minutes`. Start with n = 4 and watch the usage before raising it.

## 15. Failure handling

| Failure | Handling |
|---|---|
| `claude --cloud` errors | slot stays `planned`, retried next step; 3 errors in a row pause the run |
| session never writes a marker | `dead` at deadline, relaunched up to `relaunch_dead` times within the generation's approval (§0.4) |
| submission out of scope or fails allowlist | `rejected`, reason in `lineage.jsonl`, no relaunch |
| build fails in Actions | the shard reports every game as a fault for that bot → `disqualified` |
| shard times out or fails | listed in `missing_shards`, re-queued (§10.2) |
| no results file after 8 hours | job re-pushed as `…-retry`; the second time the run pauses |
| spot-check mismatch | `HALTED` |
| PC off | nothing is lost; `run` resumes from `state.json` |

## 16. Milestones

**M0 — probes (no code beyond scripts).** Each answer goes in `docs/evolve/M0.md`.
1. `claude --cloud "<text>"`: what it prints, how it picks the environment,
   prompt length limit, whether it returns immediately.
2. A cloud session told to push to `main` and to a branch `evo-test`: which
   works. Confirms the §9.5 ruleset.
3. Bootstrap the new repository (§1.1) and push it. Record what `cambc init`
   and `cambc new` create, the map count M, and whether `cambc init` needs
   a login (it must not).
   Then create the `evo` environment with the §6.1 setup script and open
   network. Record its duration (must be under ~5 minutes to be cached), the hosts it
   contacted, the exact non-interactive `cambc new` command, whether
   `/opt/cambc-ref/sdk/cpp` matches `sdk/cpp`, and the hook's `fingerprint.py` output.
   Then switch the network to Custom with only the hosts it needed.
   Run `tools/tests` and each allowed tool once on Linux.
4. Write a probe bot `bots/probe-random`: the starter plus random movement
   from a seeded RNG, so games are not trivially identical. Play it against
   `starter` on two maps, both sides, twice on an Actions runner and once on the
   PC. `outcome`, `gems` and `rounds` must be identical (determinism and
   cross-platform equality, needed by §10.4).
5. Seconds per game and cores on the runner, and how many shard jobs run at once.

**M1 — evaluation pipeline.** `digest.py`, `check_job.py`, `extract.py`,
`aggregate.py`, the workflow. Done when a hand-written job `probe-random` vs
`starter` on 4 maps produces a results file whose rows match a local
`benchmark.py` run.

**M2 — orchestrator without Claude.** State machine, admission, jobs,
verification, rating, selection, mutation, using fake agents placed on branches
with DONE markers by a test script: `starter`, `probe-random`, a bot that
stands still, a bot that faults every turn, and a bot that fails to build.
Done when one generation runs end to end, the faulting bot is disqualified,
the build failure is handled, and three tampered branches are rejected: one
edits `tools/benchmark.py`, one adds a file outside its folder, one uses
`#include "../…"`.

**M3 — real agents.** Templates, operator/focus/archetype fragments, launcher,
collector. Done when 2 cloud agents complete tasks and are admitted.

**M4 — generation 0 and 1 at n = 4.** Read every NOTES.md and diff, adjust the
templates.

**M5 — scale.** n = 8, cache, hall of fame, held-out pool, Stage C.

## 17. Open points to settle in M0

- Exact `claude --cloud` behaviour and whether the cloud git proxy limits
  branch names (the collector does not depend on it).
- Whether `cambc run-batch` can take prebuilt WASM. If so, build each bot
  once in the `plan` job and pass the binaries to shards, which saves setup minutes.
- `claude --cloud` run from the new repo's clone on your PC starts a session
  that clones that repo from GitHub (not a bundle) on the `evo` environment
  pinned in `.claude/settings.local.json`, and pushes its branch.
- Whether the repo is present when the environment's setup script runs. The
  plan assumes it is not, which is why repo-dependent checks live in the
  SessionStart hook.
- Cross-platform determinism. If PC and runner differ, the spot-check moves to a
  separate Actions job on a different runner image. That is still independent of
  the shard that produced the result.
