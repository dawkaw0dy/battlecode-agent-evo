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

# The cloud's HTTPS proxy uses its own CA. cambc (rustls, built-in roots) rejects it,
# while Python trusts the system store. So fetch cambc's toolchain with Python into
# cambc's cache and run cambc offline (M0 item 11). Versions match cambc 3.0.5.
CACHE="${XDG_CACHE_HOME:-$HOME/.cache}/cambc"
step "seed toolchain into $CACHE"
python3 - "$CACHE" <<'PY'
import io, os, shutil, sys, tarfile, tempfile, urllib.request, zipfile
cache = sys.argv[1]
os.makedirs(cache, exist_ok=True)
GH = 'https://github.com'
tools = [  # (cache dir, url, archive top-level dir or None, lock file)
    ('wasi-sdk-33', f'{GH}/WebAssembly/wasi-sdk/releases/download/wasi-sdk-33/wasi-sdk-33.0-x86_64-linux.tar.gz',
     'wasi-sdk-33.0-x86_64-linux', 'wasi-sdk-33.lock'),
    ('cmake-3.31.8-linux-x86_64', f'{GH}/Kitware/CMake/releases/download/v3.31.8/cmake-3.31.8-linux-x86_64.tar.gz',
     'cmake-3.31.8-linux-x86_64', 'cmake-3.31.lock'),
    ('ninja-1.12.1-linux-x86_64', f'{GH}/ninja-build/ninja/releases/download/v1.12.1/ninja-linux.zip',
     None, 'ninja-1.12.lock'),
]
for name, url, top, lock in tools:
    dest = os.path.join(cache, name)
    if os.path.isdir(dest) and os.listdir(dest):
        print('present', dest); continue
    print('fetch', url, flush=True)
    data = urllib.request.urlopen(url, timeout=300).read()
    tmp = tempfile.mkdtemp(dir=cache)
    if url.endswith('.zip'):
        with zipfile.ZipFile(io.BytesIO(data)) as z:
            z.extractall(tmp)
        for f in os.listdir(tmp):
            os.chmod(os.path.join(tmp, f), 0o755)
        os.replace(tmp, dest)
    else:
        with tarfile.open(fileobj=io.BytesIO(data)) as t:
            t.extractall(tmp)
        os.replace(os.path.join(tmp, top), dest)
        shutil.rmtree(tmp)
    open(os.path.join(cache, lock), 'a').close()
    print('ok', dest, flush=True)
PY
export CAMBC_OFFLINE=1         # cambc must not download anything itself

# Reference workspace: gives check.sh a clean SDK copy to compare against and
# proves the seeded toolchain builds a bot.
step "cambc init (reference workspace /opt/cambc-ref)"
mkdir -p /opt/cambc-ref && cd /opt/cambc-ref
cambc init --yes . </dev/null || { echo "init exit $? (maps are not needed here)"; test -f sdk/cpp/cambc.h; }
step "cambc new starter --lang cpp"
[ -d bots/starter ] || cambc new starter --lang cpp </dev/null
step "cambc build bots/starter --release"
cambc build bots/starter --release </dev/null | tail -5
step "done: evo setup OK ($(cambc --version))"
