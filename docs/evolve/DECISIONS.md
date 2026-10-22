# Decisions

Agreed deviations from the original plan. Each entry: date, PLAN.md sections,
what changed, why. PLAN.md is updated to match each entry.

| # | Date | Sections | Change | Why |
|---|---|---|---|---|
| D1 | 2026-10-22 | §8.4, §2 | `evo/eval/digest.py` hashes the relative path as `relative_to(...).as_posix()` instead of `str(...)`. `tools/benchmark.py` imports it. | `str()` gives `src\main.cpp` on Windows and `src/main.cpp` on Linux, so admission on the PC and the Actions check would never agree. Linux digests are unchanged. |
| D2 | 2026-10-22 | §0.5 #1, §1.1 step 1, §3, §4, §6.1 | Pin the newest `cambc` on PyPI at M0 time, after verifying its SDK/engine reflects `patch102.txt` (e.g. Stellar health 650, focus 70). Evidence goes in `M0.md`. The version is set in `evo/config.toml` and the environment setup script together. No installable version matching patch102 is a hard stop. Examples now show `X.Y.Z`. | The patch notes name no version number, so "3.0.4 matches the newest patch note" could not be checked. |
| D3 | 2026-10-22 | §0.4, §6.1, §7.4, §7.5, §15 | `approve gGG` prints and records the maximum it approves: `n − elites` sessions, up to `relaunch_dead` relaunches per slot, one nudge per session. The launcher never exceeds it; anything more needs a new approve. | Relaunches and nudges spend subscription usage. The owner requires approval for every session and message; the plan made them automatic. |
| D4 | 2026-10-22 | §12.4, §13.4 | At least `n + 2` archetypes; the "not used in the previous two generations" rule stays. | Generation 0 uses `n`, so generations 1 and 2 need spares; `n` was not enough. |
| D5 | 2026-10-22 | §2 | CLI is `evo/__main__.py` (`python -m evo`) with `bootstrap, init, step, run, status, approve, verify, spotcheck`; `digest.py` added to the layout. | The layout put `__main__.py` under `evo/orchestrator/` and omitted `bootstrap` and `approve`, which §1.1 and §0.4 use. |
| D6 | 2026-10-22 | §6 | The task's include rule also allows the SDK headers that §8.3 allows (listed in `TOOLS.md`). | The task text forbade the SDK header every bot needs, contradicting §8.3. |
| D7 | 2026-10-22 | §12.3, §7.5 | `rng = Random(int(sha256(f"{seed}:{generation}:{slot}").hexdigest(), 16))`; `attempt` dropped from the key. | A relaunch reuses the same task file and choices. `hash()` was unspecified and not a stable cross-version hash. |
| D8 | 2026-10-22 | §0.2.3, §0.5 #16, §1.1 step 5, §6.1 TOOLS.md | `tools/replay.mjs` and `tools/mapgen.mjs` may be changed: find the asset directory under `~/.cache/cambc/assets/*/` by glob instead of the hard-coded `/root/.cache/cambc/assets/<hash>/`, and read the replay zip in Node instead of calling `unzip`. Both tools are allowed in `TOOLS.md` only if M0 item 16 confirms they work. | The hard-coded path only works as root with one exact asset version, and `unzip` is missing on Windows. |
| D9 | 2026-10-22 | §0.1, §1.1 | Delete `tools/bench.py` in bootstrap. Add `README.md` to the starting file list. | `bench.py` calls a nonexistent `ROOT/cambc`; `bench2.py` covers it. `README.md` exists in the repo. |
| D10 | 2026-10-22 | §9.2.5 | Corrected the reason `extract.py` reads the stats files directly: `benchmark.py`'s `results.json` keeps fault counts, but loses detail for missing/unreadable stats and lacks the exact §4 fields. | The original sentence was factually wrong. |
