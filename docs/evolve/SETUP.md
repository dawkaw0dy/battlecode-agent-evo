# Owner setup checklist

Each section is one human checkpoint from PLAN.md §0.3. Do the steps, then reply
in chat with what the "Reply with" line asks for. Never paste tokens or
passwords into chat or into any file in this repo.

## H1 — before M0: local machine  ✅ done 2026-10-22 (3.13 locally, autocrlf off, no gh)

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

---

The three sections below are independent; do them in this order if you can.
Web UIs change, so a label may read slightly differently. If something does not
match, stop and tell me what you see instead of guessing.

## H2 — M0: Claude GitHub App on this repository only  ✅ done 2026-10-22 (new install, this repo only)

1. Open <https://github.com/apps/claude> and click **Configure** (or **Install**
   if it was never installed).
2. Pick your account **dawkaw0dy**.
3. Under **Repository access** choose **Only select repositories**, then select
   **dawkaw0dy/battlecode-agent-evo**. If the app already has access to other
   repositories you want to keep, leave them; the plan only needs this repo in the list.
   Do **not** choose "All repositories".
4. Click **Save** (or **Install**).

**Reply with:** `H2: done` (and whether it was a new install or a change).

## H3 — M0: the `evo` cloud environment

1. Open <https://claude.ai/code>.
2. Open the **environment selector** (next to the repository picker, where a new
   cloud session is configured) and choose **Add environment** / **New environment**.
3. Name: `evo`.
4. **Network access:** choose the **open / full** internet option for now. M0 records which
   hosts the setup script needs, and H5 then narrows it to Custom.
5. **Environment variables** (one per line, no quotes):

   ```
   CAMBC_NO_UPDATE_CHECK=1
   CAMBC_OFFLINE=1
   BASH_DEFAULT_TIMEOUT_MS=600000
   ```

   (`CAMBC_OFFLINE=1` was added after the first probe: cambc cannot pass the cloud's TLS proxy, so
   the setup script downloads its toolchain instead; M0.md item 11.)

   No secrets or tokens.
6. **Setup script:** paste the whole contents of
   [`evo/env/setup.sh`](../../evo/env/setup.sh) from `main` (it starts with `#!/bin/bash`
   and pins `VER=3.0.5`).
7. Save. Do not start a session from the web UI; the probe session (a stop point)
   is started by me from this PC after your yes.
8. The web UI does not show the id (confirmed in the Claude Code docs). Get it from the CLI:
   1. Open a **new** PowerShell window (not this session) and run `claude` in any folder.
   2. Type `/remote-env` and press Enter. A picker of your environments opens. Choose **evo**.
   3. Type `/exit`.

   This writes `"remote": {"defaultEnvironmentId": "env_…"}` into your **user** settings,
   `C:\Users\dawka\.claude\settings.json`. Before this step that file had no `remote` key
   (checked 2026-10-22), so nothing of yours is overwritten. I then copy the id into this
   repo's `.claude/settings.local.json` (gitignored). That file takes precedence over user
   settings for this repo. With your OK I then remove the user-level key again, so
   `claude --cloud` in your other projects does not default to `evo`.

**Reply with:** `H3: picked evo`, and whether I should remove the user-level key afterwards.

## H4 — M0: rulesets and Actions settings (PLAN §9.5)  ✅ done 2026-10-22 (GitHub Actions not offered as bypass, so `evo-results` has no Restrict updates)

### Ruleset for `main`

1. Repo page → **Settings** → **Rules** → **Rulesets** → **New ruleset** →
   **New branch ruleset**.
2. **Ruleset name:** `protect-main`. **Enforcement status:** **Active**.
3. **Bypass list:** **Add bypass** → **Repository admin** (that is you; the orchestrator
   pushes as you). Set it to **Always allow**.
4. **Target branches:** **Add target** → **Include by pattern** → `main`.
5. **Rules:** tick **Restrict updates**, **Restrict deletions**, **Block force pushes**.
   Leave the other rules off.
6. **Create**.

### Ruleset for `evo-results`

1. Same steps, name `protect-evo-results`, target pattern `evo-results`.
2. **Rules:** tick **Restrict deletions** and **Block force pushes**.
3. **Bypass list / Restrict updates:** if the bypass list offers **GitHub Actions**
   (an app), add it with **Always allow**, add **Repository admin** too, and also tick
   **Restrict updates**. If **GitHub Actions** is *not* offered, leave **Restrict updates**
   off and tell me. The results branch would then rely on the spot-check (§10) and
   on the cloud proxy's own branch limits, which the probe session (M0 item 12) tests.

### Actions settings

**Settings** → **Actions** → **General**:

1. **Actions permissions:** **Allow all actions and reusable workflows** (the
   workflows use only `actions/checkout`, `setup-python`, `upload-artifact`,
   `download-artifact`).
2. **Fork pull request workflows from outside collaborators:**
   **Require approval for all outside collaborators**.
3. **Workflow permissions:** **Read repository contents and packages permissions**.
   Untick **Allow GitHub Actions to create and approve pull requests**. Save.
   (The aggregate jobs ask for `contents: write` themselves; nothing else gets it.)

**Reply with:** `H4: done`, plus whether **GitHub Actions** was offered as a bypass actor.
