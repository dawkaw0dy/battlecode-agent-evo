# Task probe (M0 items 10–13)

You are the M0 probe session for the evolutionary bot search in this repository
(docs/evolve/PLAN.md §0.5). Do **not** write or change any bot, and do not play any
games beyond what `evo/env/check.sh` runs. Finish within 30 minutes. Run each
step even if an earlier one fails, and record every result.

Never print, write or commit tokens or credentials. If a command's output contains
a URL with credentials (for example `https://x:TOKEN@...`), replace the secret part with `REDACTED`.

## Steps

1. **SessionStart hook.** Did the SessionStart hook print `evo environment OK`? Record
   `yes`, `no` or `not visible`, plus any hook output you can see.
2. **Check script.** Run `time bash evo/env/check.sh` with `CLAUDE_CODE_REMOTE` as it is.
   Record the exit code, the last 30 lines of output and the elapsed time. Then record the
   contents of `/tmp/evo-env.json` and `/opt/evo-setup.log` (or "missing").
3. **Environment.** Record the exact values of `CLAUDE_CODE_REMOTE` and
   `CLAUDE_CODE_REMOTE_SESSION_ID`, plus the output of `git branch --show-current`,
   `git log -1 --format=%H`, `git remote -v` (redacted), `nproc`, `python3 --version`,
   `node --version`, `cambc --version`, `ls -A ~/.cache/cambc` and `ls /opt`.
4. **Push tests.** Each test creates one throwaway commit that only adds a file named
   after the test. Record for each the exact `git push` output and the exit code. Use no
   force and no other flags.
   - a. `main`: `git fetch origin main && git switch -c probe-main origin/main`; add
     `evo/agents/probe/push-test-main.txt` containing `probe`; commit;
     `git push origin probe-main:main`.
   - b. `evo-test`: from the same commit, `git push origin probe-main:evo-test`.
   - c. `evo-results`: `git fetch origin evo-results && git switch -c probe-results origin/evo-results`;
     add `m0/probe-push-test.txt` containing `probe`; commit;
     `git push origin probe-results:evo-results`.
5. **Own branch.** Switch back to the branch you started on (step 3). Write
   `evo/agents/probe/DONE.json` with this shape, all values as strings unless stated:

   ```json
   {"agent": "probe",
    "hook_ok": "yes|no|not visible", "hook_output": "...",
    "check_exit": 0, "check_seconds": 0, "check_tail": "...",
    "evo_env_json": "...", "setup_log": "...",
    "CLAUDE_CODE_REMOTE": "...", "CLAUDE_CODE_REMOTE_SESSION_ID": "...",
    "start_branch": "...", "start_commit": "...", "remote": "...",
    "nproc": "...", "python": "...", "node": "...", "cambc": "...",
    "cache_listing": "...", "opt_listing": "...",
    "push_main": {"exit": 0, "output": "..."},
    "push_evo_test": {"exit": 0, "output": "..."},
    "push_evo_results": {"exit": 0, "output": "..."},
    "notes": "anything surprising, one paragraph"}
   ```

   Commit only that file and push your branch with a plain `git push -u origin HEAD`.
   Record the branch name the push used in your final message.
6. Stop. Do not open a pull request.
