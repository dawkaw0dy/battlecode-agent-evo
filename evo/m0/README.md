# evo/m0

`annotate.py` prints stdin as one GitHub Actions annotation. `evo-env-test.yml` uses it to publish its setup log,
because job logs need a token to download and annotations do not. The M0 probe scripts that used to live here were
removed after M5; they are in git history and described in `docs/evolve/M0.md`.
