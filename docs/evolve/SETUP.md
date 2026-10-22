# Owner setup checklist

Each section is one human checkpoint from PLAN.md §0.3. Do the steps, then reply
in chat with what the "Reply with" line asks for. Never paste tokens or
passwords into chat or into any file in this repo.

## H1 — before M0: local machine

What the implementing session already saw on this PC (2026-10-22):

| Item | Found | Plan wants |
|---|---|---|
| Python | 3.13.1 only (`py -0p` lists no 3.12) | 3.12 |
| Git | 2.47.1.windows.1 | any recent |
| Node | v24.15.0 | 20+ |
| Claude Code | 2.1.293, has `--cloud` and `--environment` flags | `claude --cloud` |
| `gh` (GitHub CLI) | not installed | not required by the plan |
| `git config core.autocrlf` | `true` | see step 5 |
| Remote | `https://github.com/dawkaw0dy/battlecode-agent-evo.git`, `main` = `528cfe1` | public repo you own |

Steps (PowerShell, in this repo's folder):

1. **Claude account.** Run:

   ```
   claude auth status
   ```

   Check that it shows the account that holds your Claude subscription (Pro/Max),
   not an API key / Console account. If it is wrong, run `claude auth logout`,
   then `claude auth login` and pick the subscription account in the browser.
   Also make sure no `ANTHROPIC_API_KEY` is set: `echo $env:ANTHROPIC_API_KEY`
   must print nothing.

2. **Python 3.12.** The plan pins 3.12 (Actions uses 3.12). Either:
   - install Python 3.12 from python.org (Windows installer, 64-bit, tick
     "Add python.exe to PATH" is *not* needed; the `py` launcher is enough),
     then check `py -3.12 --version`; **or**
   - tell me to use 3.13 for the local `.venv`. That is fine only if
     `pip install cambc==<pinned>` has a Windows wheel for 3.13; I will check
     this in M0 item 1 and come back to you if not.

3. **Git push works.** Run:

   ```
   git push --dry-run origin main
   ```

   It must end with `Everything up-to-date` and must not ask for a password in
   the terminal (Git Credential Manager may open a browser once; that is fine).

4. **Node.** Nothing to do (v24 ≥ 20).

5. **Line endings.** Your global `core.autocrlf=true` checks this repo's files
   out with CRLF (e.g. `GameRules.md` is CRLF in the working tree, LF in git).
   Hashes in the plan must be of LF bytes. Bootstrap will add `.gitattributes`
   (`* text=auto eol=lf`) and re-check-out the files. Reply whether I may also
   set `git config core.autocrlf false` **for this repo only** (does not touch
   your other repos).

6. **Optional: GitHub CLI.** M0 items 8 and 9 need run timings and per-job
   data from Actions. Without `gh` I will make the workflows write everything
   I need to a branch and read it with `git fetch`. If you prefer `gh`:
   `winget install --id GitHub.cli`, then `gh auth login` (GitHub.com, HTTPS,
   browser login). The token stays in gh's own store.

7. **`tools/tests` is not committed yet.** It exists only in your working copy
   (untracked). Nothing to do: bootstrap commits it with the rest of `tools/`.

Plan questions 1–11 are answered and recorded in `DECISIONS.md` (D1–D10).
Open questions 12–22 are listed in `PROGRESS.md`; none blocks starting M0.

**Reply with:** `H1: claude=<ok/fixed>, python=<3.12.x | use 3.13>, push=<ok>,
autocrlf-local-false=<yes/no>, gh=<installed/skip>`.
