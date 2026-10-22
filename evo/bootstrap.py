"""One-time repository bootstrap (PLAN §1.1). Every step is skipped when already done."""
import os
import shutil
import subprocess
import sys
import tempfile
import tomllib
from pathlib import Path

from evo import manifest

ROOT = Path(__file__).resolve().parents[1]
INIT_STARTERS = ('starter-cpp', 'starter-rust', 'starter-python', 'starter-ocaml')  # created by cambc init
GITATTRIBUTES = '* text=auto eol=lf\n*.map26a binary\n*.repl26a binary\n'


def config(root=ROOT):
    return tomllib.loads((root / 'evo' / 'config.toml').read_text(encoding='utf-8'))


def pinned_version(cfg):
    spec = cfg['eval']['runner_version']
    name, _, version = spec.partition('==')
    if name != 'cambc' or not version:
        raise ValueError(f'[eval].runner_version must be cambc==X.Y.Z, got {spec!r}')
    return version


def venv_python(root=ROOT):
    return root / '.venv' / ('Scripts/python.exe' if os.name == 'nt' else 'bin/python')


def cambc_path(root=ROOT):
    return root / '.venv' / ('Scripts/cambc.exe' if os.name == 'nt' else 'bin/cambc')


def child_env():
    return dict(os.environ, CAMBC_NO_UPDATE_CHECK='1', PYTHONUTF8='1', PYTHONIOENCODING='utf-8')


def call(cmd, **kw):
    print('+', ' '.join(str(c) for c in cmd), flush=True)
    return subprocess.run([str(c) for c in cmd], cwd=ROOT, env=child_env(), stdin=subprocess.DEVNULL,
                          check=True, **kw)


def cambc_version():
    try:
        out = subprocess.run([str(cambc_path()), '--version'], env=child_env(), capture_output=True,
                             text=True, encoding='utf-8', errors='replace', check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return None
    return out.strip()


def ensure_venv(version):
    if not venv_python().exists():
        call([sys.executable, '-m', 'venv', '.venv'])
    if cambc_version() != f'cambc {version}':
        call([venv_python(), '-m', 'pip', 'install', '--quiet', f'cambc=={version}', 'pytest'])
    found = cambc_version()
    if found != f'cambc {version}':
        raise SystemExit(f'bootstrap: expected cambc {version}, venv has {found!r}')


def ensure_workspace(version):
    if not (ROOT / 'cambc.toml').exists():
        call([cambc_path(), 'init', '--yes', '.'])
    sdk = manifest.sdk_version(ROOT)
    if sdk != version:
        raise SystemExit(f'bootstrap: cambc.toml sdk_version {sdk} != pinned {version}')
    for name in INIT_STARTERS:
        if (ROOT / 'bots' / name).exists():
            shutil.rmtree(ROOT / 'bots' / name)
    if not (ROOT / 'bots' / 'starter' / 'src').is_dir():
        call([cambc_path(), 'new', 'starter', '--lang', 'cpp'])
    if not list((ROOT / 'bots' / 'starter' / 'src').glob('*.cpp')):
        raise SystemExit('bootstrap: bots/starter/src has no .cpp files')


def ensure_gitattributes():
    path = ROOT / '.gitattributes'
    if not path.exists() or path.read_bytes() != GITATTRIBUTES.encode():
        path.write_bytes(GITATTRIBUTES.encode())


def write_manifest():
    return manifest.write(ROOT, config()['eval']['runner_version'])


def smoke():
    call([venv_python(), '-m', 'pytest', '-q', 'tools/tests', 'evo/tests'])
    first_map = sorted((ROOT / 'maps').glob('*.map26a'))[0].stem
    with tempfile.TemporaryDirectory(prefix='evo-smoke-') as tmp:
        call([venv_python(), 'tools/benchmark.py', 'starter', 'starter', '--maps', first_map, '--jobs', '2',
             '--output', Path(tmp) / 'smoke', '--runner', cambc_path(), '--json', '--no-progress',
             '--color', 'never'])


def run(smoke_games=True):
    version = pinned_version(config())
    ensure_gitattributes()
    ensure_venv(version)
    ensure_workspace(version)
    if smoke_games:
        smoke()
    digest = write_manifest()
    print(f'manifest sha256 {digest}')
    print('bootstrap done: review `git status`, then commit and push')
    return 0
