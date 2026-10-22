#!/bin/bash
# Setup script for the `evo` cloud environment (PLAN §6.1). Paste this file's
# contents into the environment at claude.ai/code. The repository is not
# available while it runs, so it installs only repo-independent things.
set -euo pipefail
VER=3.0.5                      # keep equal to [eval].runner_version in evo/config.toml
export CAMBC_NO_UPDATE_CHECK=1
# M0 item 11: record where setup runs, whether a repo is visible, and step times.
LOG=/opt/evo-setup.log
mkdir -p /opt && {
  echo "start $(date -u +%FT%TZ) pwd=$PWD user=$(id -un) home=$HOME"
  echo "git-toplevel=$(git rev-parse --show-toplevel 2>/dev/null || echo none)"
  echo "home-listing: $(ls -A "$HOME" 2>/dev/null | tr '\n' ' ')"
} > "$LOG"
pip install --quiet "cambc==$VER" || pip install --quiet --break-system-packages "cambc==$VER"
echo "pip-done $(date -u +%FT%TZ)" >> "$LOG"
# Reference workspace: warms the cambc cache (wasi-sdk, CMake, Ninja) and
# gives check.sh a clean SDK copy to compare against.
mkdir -p /opt/cambc-ref && cd /opt/cambc-ref
cambc init --yes . </dev/null >/dev/null
cambc new starter --lang cpp </dev/null >/dev/null
cambc build bots/starter --release </dev/null >/dev/null
echo "done $(date -u +%FT%TZ) $(cambc --version)" >> "$LOG"
echo "evo setup done: $(cambc --version)"
