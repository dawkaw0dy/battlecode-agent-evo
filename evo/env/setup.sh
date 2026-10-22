#!/bin/bash
# Setup script for the `evo` cloud environment (PLAN §6.1). Paste this file's
# contents into the environment at claude.ai/code. The repository is not
# available while it runs, so it installs only repo-independent things.
set -euo pipefail
VER=3.0.5                      # keep equal to [eval].runner_version in evo/config.toml
export CAMBC_NO_UPDATE_CHECK=1
pip install --quiet "cambc==$VER" || pip install --quiet --break-system-packages "cambc==$VER"
# Reference workspace: warms the cambc cache (wasi-sdk, CMake, Ninja) and
# gives check.sh a clean SDK copy to compare against.
mkdir -p /opt/cambc-ref && cd /opt/cambc-ref
cambc init --yes . </dev/null >/dev/null
cambc new starter --lang cpp </dev/null >/dev/null
cambc build bots/starter --release </dev/null >/dev/null
echo "evo setup done: $(cambc --version)"
