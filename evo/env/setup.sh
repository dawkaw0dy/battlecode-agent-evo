#!/bin/bash
# Setup script for the `evo` cloud environment (PLAN §6.1). Paste this file's
# contents into the environment at claude.ai/code. The repository is not
# available while it runs, so it installs only repo-independent things.
set -euo pipefail
VER=3.0.5                      # keep equal to [eval].runner_version in evo/config.toml
export CAMBC_NO_UPDATE_CHECK=1 PIP_ROOT_USER_ACTION=ignore PIP_DISABLE_PIP_VERSION_CHECK=1
# Every line also goes to /opt/evo-setup.log; a failing command is named before exit.
mkdir -p /opt
LOG=/opt/evo-setup.log
exec > >(tee -a "$LOG") 2>&1
trap 'echo "evo setup FAILED at line $LINENO: $BASH_COMMAND (exit $?)"' ERR
step() { echo "== $(date -u +%FT%TZ) $*"; }

step "start user=$(id -un) pwd=$PWD home=$HOME"
echo "git-toplevel=$(git rev-parse --show-toplevel 2>/dev/null || echo none)"   # M0 item 11
echo "python3=$(command -v python3) $(python3 --version 2>&1)"

step "pip install cambc==$VER"
python3 -m pip install --quiet "cambc==$VER" \
  || python3 -m pip install --quiet --break-system-packages "cambc==$VER"
# The wheel ships only a binary in the interpreter's scripts dir; make sure it is on PATH.
SCRIPTS=$(python3 -c 'import sysconfig; print(sysconfig.get_path("scripts"))')
if ! command -v cambc >/dev/null; then ln -sf "$SCRIPTS/cambc" /usr/local/bin/cambc; fi
echo "cambc=$(command -v cambc) $(cambc --version)"

# Reference workspace: warms the cambc cache (wasi-sdk, CMake, Ninja) and
# gives check.sh a clean SDK copy to compare against.
step "cambc init (reference workspace /opt/cambc-ref)"
mkdir -p /opt/cambc-ref && cd /opt/cambc-ref
cambc init --yes . </dev/null
step "cambc new starter --lang cpp"
[ -d bots/starter ] || cambc new starter --lang cpp </dev/null
step "cambc build bots/starter --release"
cambc build bots/starter --release </dev/null | tail -5
step "done: evo setup OK ($(cambc --version))"
