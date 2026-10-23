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

## H3 — M0: the `evo` cloud environment  ✅ done 2026-10-22 (`env_01HkiFrTm9YE8qLL1T7rKYdV`; setup script re-pasted after the TLS fix, `CAMBC_OFFLINE=1` added)

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

## H4 v2 — M0: deploy keys are the only bypass (DECISIONS D20)  ✅ done 2026-10-22 (verified: HTTPS as owner refused, deploy keys accepted; local `evo_actions` private key deleted)

Why: the probe session's push to `main` was **accepted**, because cloud sessions push as you and
the rulesets let Repository admin bypass them. From now on only two deploy keys may bypass.
I generated both key pairs on this PC (2026-10-22). Only the **public** halves appear below.

### 1. Add two deploy keys

Repo → **Settings** → **Deploy keys** → **Add deploy key**, twice:

| Title | Key (paste the whole line) | Allow write access |
|---|---|---|
| `evo-orchestrator` | `ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIC1VpZe1U3TYwmRYupdz6nKUY7lugsjxFsZWYoSaV4o8 evo-orchestrator (battlecode-agent-evo deploy key, PC)` | **ticked** |
| `evo-actions` | `ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIMKiVd2WeSeYQKw5LIRVTXKqc8+NPbhHjBGpwseoLpes evo-actions (battlecode-agent-evo deploy key, Actions secret)` | **ticked** |

### 2. Add the Actions secret

1. In PowerShell run `notepad $env:USERPROFILE\.ssh\evo_actions`. This is the **private** key for `evo-actions`.
   Copy the whole file, including the `-----BEGIN OPENSSH PRIVATE KEY-----` and `-----END …-----` lines.
2. Repo → **Settings** → **Secrets and variables** → **Actions** → **New repository secret**.
   Name: `EVO_RESULTS_DEPLOY_KEY`. Secret: paste. **Add secret**.
3. Do not paste it anywhere else (not in chat, not in a file in the repo). Once the secret is saved,
   tell me and I will delete `evo_actions` from this PC (its public half stays).

The PC key `~/.ssh/evo_orchestrator` stays only on this PC. Do not upload it anywhere.

### 3. Change both rulesets

Repo → **Settings** → **Rules** → **Rulesets**:

1. **protect-main** → **Bypass list**: remove **Repository admin**. **Add bypass** → **Deploy keys** →
   **Always allow**. Rules stay: **Restrict updates**, **Restrict deletions**, **Block force pushes**. **Save changes**.
2. **protect-evo-results** → **Bypass list**: **Add bypass** → **Deploy keys** → **Always allow**
   (no Repository admin). Rules: tick **Restrict updates** as well, so all three are on. **Save changes**.

After this, your own normal HTTPS pushes to `main` are refused too. This clone will push over SSH with
`evo-orchestrator`; I set that up locally (`git config` in this repo only).

**Reply with:** `H4 v2: done`. I then test it without any cloud session: an HTTPS push as you must be refused
and an SSH push with the deploy key accepted. The test push is the cleanup commit that removes the probe's test file.

## H5 — M0: narrow the `evo` environment's network to Custom  ✅ done 2026-10-22 (not yet exercised by a session)

The setup script and the sessions need only these hosts (M0.md items 11 and 16, checked 2026-10-22):

| Host | Used for |
|---|---|
| `pypi.org` | `pip install cambc` (index) |
| `files.pythonhosted.org` | `pip install cambc` (the wheel) |
| `github.com` | wasi-sdk, CMake and Ninja release downloads (setup script) |
| `release-assets.githubusercontent.com` | where those GitHub release downloads redirect |

cambc itself runs offline (`CAMBC_OFFLINE=1`); its map host `2026a.battlecode.co.uk` is **not** needed (maps are in the repo).

1. claude.ai/code → environment selector → hover **evo** → settings icon.
2. **Network access** → **Custom**. If the dialog offers to keep the default/trusted list in addition to
   your own entries, keep it on (Claude Code's own and the GitHub proxy's traffic may rely on it).
3. Add the four hosts above, one per entry. Save.

Changing the network rebuilds the environment's snapshot. The next cloud session (M3, a stop point with
its own approval) will show whether setup still passes. I will not start one just to test this.

**Reply with:** `H5: done` (and whether a "default list" option existed and what you chose).

## H7 — M5: the private held-out map repository (DECISIONS D33)  ✅ done 2026-10-23

Why: §9.4 keeps a pool of generated maps that agents never see. Agents can read everything in
`battlecode-agent-evo`, so these maps live in a separate **private** repository. The Claude GitHub App is
**not** installed on it, so cloud sessions cannot reach it. I generated two key pairs on this PC
(2026-10-23); only their public halves appear below.

### 1. Create the private repository

1. <https://github.com/new>. Owner **dawkaw0dy**, name **battlecode-agent-evo-heldout**.
2. **Private**. Do **not** add a README, .gitignore or license (it must start empty). **Create repository**.
3. Do **not** install the Claude GitHub App on it. If the app is set to "All repositories", switch it to
   "Only select repositories" with just `battlecode-agent-evo` (that is how H2 set it up).

### 2. Two deploy keys on the **private** repo

**battlecode-agent-evo-heldout** → **Settings** → **Deploy keys** → **Add deploy key**, twice:

| Title | Key | Allow write access |
|---|---|---|
| `evo-heldout-pc` | `ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIKtTn4hCxBtoOffQ43ixlSdYmOciRUNtKT8QL4woOUIR evo-heldout-pc (battlecode-agent-evo-heldout, PC, write)` | **ticked** (this PC pushes the maps) |
| `evo-heldout-actions` | `ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIH4qxfCX1q9cCux2zX/WXWxdQBgG7VO/TEDG0OodPNCq evo-heldout-actions (battlecode-agent-evo-heldout, Actions secret, read-only)` | **not** ticked (Actions only reads) |

### 3. One secret on the **public** repo

1. In PowerShell run `notepad $env:USERPROFILE\.ssh\evo_heldout_actions`. This is the private half of the
   read-only key. Copy the whole file, including the BEGIN/END lines.
2. **battlecode-agent-evo** (the public repo) → **Settings** → **Secrets and variables** → **Actions** →
   **New repository secret**. Name `HELDOUT_DEPLOY_KEY`, paste, **Add secret**.
3. Paste it nowhere else. Once it is saved I delete the local copy of `evo_heldout_actions`.

**Reply with:** `H7: done`. I then push the generator and the 24 maps to the private repo over SSH,
clone it to `heldout/` here, set `[eval].heldout_pool`, and run a free Actions test job on 2 held-out maps
(like M1) before generation 02 uses them.

## H8 — effort level of the evo agents (DECISIONS D36)

1. <https://claude.ai/code> → environment selector → hover **evo** → settings icon.
2. **Environment variables**: add the line

   ```
   CLAUDE_CODE_EFFORT_LEVEL=high
   ```

   (keep the existing `CAMBC_NO_UPDATE_CHECK=1`, `CAMBC_OFFLINE=1`, `BASH_DEFAULT_TIMEOUT_MS=600000`).
   Optional: `ANTHROPIC_MODEL=opus` to pin the model too.
3. Save. The environment's snapshot is rebuilt; nothing else changes.

Check: after the next generation's agents finish, the app's Agents list shows `(effort high, …)` for each one,
taken from the agents' own environment fingerprint. `effort default` means the variable did not reach them.
