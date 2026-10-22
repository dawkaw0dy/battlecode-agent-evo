#!/bin/bash
# SessionStart check for evo cloud sessions (PLAN §6.1).
set -euo pipefail
# The hook fires in every Claude Code session opened in this repo, including the
# implementing agent's and the orchestrator's local sessions on Windows, where
# /opt/cambc-ref does not exist. Only cloud sessions set CLAUDE_CODE_REMOTE=true.
# EVO_FORCE_CHECK=1 runs the check anywhere else on purpose (Linux only).
if [ "${CLAUDE_CODE_REMOTE:-}" != "true" ] && [ "${EVO_FORCE_CHECK:-}" != "1" ]; then
  exit 0
fi
export CAMBC_NO_UPDATE_CHECK=1
cd "${CLAUDE_PROJECT_DIR:-$(git rev-parse --show-toplevel)}"
VER=$(sed -n 's/^sdk_version *= *"\(.*\)"/\1/p' cambc.toml)
test "$(cambc --version)" = "cambc $VER"
diff -rq /opt/cambc-ref/sdk/cpp sdk/cpp          # committed SDK matches the CLI
python3 evo/env/verify_manifest.py               # docs, patch notes, SDK, maps, tools hashes
mkdir -p .venv/bin && ln -sf "$(command -v cambc)" .venv/bin/cambc   # benchmark.py's default runner
MAP=$(ls maps/*.map26a | head -1 | xargs basename -s .map26a)
SMOKE="$(mktemp -d)/smoke"
python3 tools/benchmark.py starter starter --maps "$MAP" --jobs 2 \
  --output "$SMOKE" --json --no-progress --color never >/dev/null
python3 evo/env/fingerprint.py "$SMOKE" > /tmp/evo-env.json
cat /tmp/evo-env.json
echo "evo environment OK"
