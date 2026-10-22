# M0 probe helpers

Scripts used only by `.github/workflows/evo-m0-probe.yml` and `evo-env-test.yml` during milestone M0
(docs/evolve/M0.md). A push that changes this folder re-runs the probe workflow, which also
checks that the `evo-actions` deploy key can push `evo-results` (DECISIONS D20).
