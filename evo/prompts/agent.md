# Task {{id}} (generation {{generation}}, slot {{slot}})

You are evolution agent {{id}}. You write one Battlecode bot in C++ on your own branch.
The sha256 of this file (`sha256sum evo/tasks/{{id}}.md`) goes into DONE.json as `task_sha256`.

## Rules
- Confirm the SessionStart hook printed `evo environment OK`; otherwise run `bash evo/env/check.sh`.
  If it fails, write `evo/agents/{{id}}/DONE.json` with `"env_failed": true` and the output, push, and stop.
- Read, in order: GameRules.md, patch-notes/*.txt in numeric order (they override the rules; a later patch
  overrides an earlier one), BotAPICpp.md, evo/env/TOOLS.md.
- Environment manifest sha256: {{env_manifest_sha256}}. C++ only.
- Your bot lives in bots/{{id}}/src. Allowed files: *.cpp, *.h, *.hpp, flat (no subdirectories).
  Includes: standard headers, the SDK headers listed in evo/env/TOOLS.md, or files in your own src
  directory, plain names only.
- Change nothing outside bots/{{id}}/ and evo/agents/{{id}}/. Other changes reject the whole submission.
- Work on your own branch. Never push to main or evo-results.
- Test with: python3 tools/benchmark.py {{id}} <opponent> --maps <…> --jobs $(nproc) --output /tmp/<dir>
  Opponents to test against (the bots you are evaluated with): {{opponents}}.
  Budget: at most {{game_budget}} games in total, all tools together. Your self-reported results are not used for selection.
- Use all your time to make the bot as strong as you can: keep improving and testing it until you receive the
  message "20 minutes left". Then start no new work and do the Finish steps at once. Do not write DONE.json
  before that message. Commit and push work in progress to your branch now and then.

## How you are scored
- Your bot is evaluated in GitHub Actions, not here: release builds, both sides of every map, against a
  fixed panel (the starter and recent champions) and, early on, against the other bots of your generation.
- A game where your bot faults, or prints `bot error:` (a caught exception, as the starter's main loop
  prints), counts as a **loss**. If more than 10% of your games fault, the bot is disqualified. A bot
  that does not build is disqualified. Avoid exceptions rather than hiding them: check preconditions
  before each action (BotAPICpp.md: actions on known-invalid targets throw), and check the replay
  stdout of your test games for `bot error:` lines.
- Games are deterministic, so a result you see locally is the result Actions will see.

## Starting point
{{starting_point}}

{{mutation}}

{{focus}}

{{evidence}}

{{failed_ideas}}

## Finish
1. `cambc build bots/{{id}} --release` passes with no warnings you introduced.
2. bots/{{id}}/NOTES.md in the format below.
3. evo/agents/{{id}}/DONE.json last, then commit and push your branch:
   {"agent": "{{id}}", "task_sha256": "<sha256 of this file>", "base_commit": "<git rev-parse HEAD at start>",
    "session_id": "<value of $CLAUDE_CODE_REMOTE_SESSION_ID>", "bot_dir": "bots/{{id}}",
    "self_eval": {"vs": "<opponent>", "games": <n>, "wins": <n>}, "games_played": <n>, "summary": "one line",
    "env_fingerprint": <the JSON object in /tmp/evo-env.json, written by the SessionStart check>}

## NOTES.md format
## Strategy (≤10 lines)
## Key parameters
## Changes this generation
## Tried and failed (keep entries from the parent's notes; add yours)
## Known weaknesses
