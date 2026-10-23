# Running the evolution

## Start

Double-click **`evo-run.cmd`** in this folder (or run `.venv\Scripts\python.exe -m evo run` in a terminal here).
A window opens with a live dashboard. It checks the run every minute and redraws every 10 seconds.

The dashboard shows:
- the generation and phase, sessions used against the cap, and the hall of fame;
- **Agents** (while sessions run): each agent's elapsed time against its deadline and when it will likely finish;
- **Evaluation** (Stage A, B and C jobs and their spot-checks): `n/total shards` done on GitHub Actions;
- **when the generation should be selected**, from this run's own timings.
- Below that: the last log lines and what this window did.

## When it needs you

- **Approval.** Before any cloud session starts, the window shows exactly what it would launch (number of
  sessions, deadline, nudges, total so far) and asks `Approve generation NN? Type y to launch`. Only `y`
  launches. Anything else, or closing the window, launches nothing. You can read the tasks first in
  `evo/tasks/eNN-*.md`.
- **Halt.** If verification finds something wrong, the window says `HALTED` and why, and stops. Nothing more
  happens until someone looks. After the review: `python -m evo unhalt --reason "…"`.
- **Waiting.** `cannot launch yet: working tree is not clean` means a file in this folder was changed but not
  committed. Commit and push it (below); the window re-checks by itself.
- **End.** At `max_generations` the window stops. To go further, raise `max_generations` (and
  `max_sessions_total` if needed) in `evo/config.toml`, commit and push, and start it again. It continues by itself
  up to the next approval.

Ctrl+C or closing the window is always safe: everything is in `evo/state.json` and on GitHub, and starting it
again continues where it was.

## Emergency stop

Double-click **`evo-stop.cmd`** (or `python -m evo stop`). Immediately:
- the run is marked **STOPPED by owner**: no new sessions, no nudges; a running app window stops at its next check;
- every running agent's slot is retired, so nothing it pushes later is admitted;
- one message is queued into each running session ("stop now, push nothing"). A queued message is read when the
  agent finishes its current step, usually within minutes, not instantly;
- each running session's page opens in your browser. **Stopping it there is the only immediate stop**: Claude Code
  has no documented command that stops a cloud session.

Sessions that you do not stop end by themselves at the latest at their deadline (`deadline_minutes`).
To continue later: `python -m evo resume --reason "…"`, then start `evo-run.cmd` again.

## Changing settings (and why GitHub Desktop cannot push)

`main` only accepts pushes made with this PC's deploy key (DECISIONS D20), so agents, which push as you, cannot
change it. GitHub Desktop pushes as you, so it is refused too. After editing a file here, push from a terminal
in this folder, which uses the deploy key automatically:

```
git commit -am "config: …"
git push
```

Common settings in `evo/config.toml`: `population`, `[agents].deadline_minutes`, `max_sessions_total`,
`local_game_budget`, and `[run].max_generations`.

## The result

`python -m evo best` prints the latest selected generation's ranking and copies the #1 bot (`src/` and NOTES.md)
to `best-bot/<id>/`. The same code is always on `main` in `bots/<id>/`. Submitting to the ladder is up to you
(evo never touches the platform account).

## Unattended runs

With `[agents].require_approval = false` (D37), generations follow each other without the `y` prompt; the run
stops at `max_generations` or when `max_sessions_total` sessions have been used.

## Other commands

`python -m evo status` (one-off summary), `python -m evo verify <job>`, `python -m evo spotcheck <job>`
(a local replay; your PC plays the games), `python -m evo resume --reason "…"` (clears a pause that is not an approval).
