"""Environment manifest: hashes of everything that defines the game (PLAN §6.1).

Shared by the orchestrator (writes ``evo/env/manifest.json``), the cloud
SessionStart check (``evo/env/verify_manifest.py``) and, from M1, the Actions
job check. Standard library only: it also runs on the cloud image's python3.
"""
import hashlib
import json
import subprocess
from pathlib import Path

from evo.eval.digest import digest

MANIFEST = Path('evo/env/manifest.json')

# Tracked paths hashed byte for byte, grouped as in the §6.1 table.
GROUPS = {
    'docs': ['GameRules.md', 'BotAPICpp.md'],
    'patch_notes': ['patch-notes/'],
    'sdk': ['sdk/', 'cmake/', 'cambc.toml'],
    'maps': ['maps/'],
    'tools': ['tools/', 'evo/__init__.py', 'evo/eval/'],
    'env': ['.claude/settings.json', 'evo/env/check.sh', 'evo/env/verify_manifest.py',
            'evo/env/fingerprint.py', 'evo/env/setup.sh', 'evo/env/TOOLS.md'],
}


def tracked_files(root, prefixes):
    """Tracked and untracked-but-not-ignored files under the prefixes, sorted, POSIX paths.

    Untracked files count, so a stray file in tools/ or sdk/ shows up as a difference.
    """
    out = subprocess.run(['git', 'ls-files', '-z', '--cached', '--others', '--exclude-standard',
                          '--deduplicate', '--', *prefixes], cwd=root, check=True,
                         capture_output=True).stdout
    paths = {p for p in out.decode('utf-8').split('\0') if p and '__pycache__' not in p}
    return sorted(p for p in paths if (Path(root) / p).is_file())


def sha256_file(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def sdk_version(root):
    for line in (Path(root) / 'cambc.toml').read_text(encoding='utf-8').splitlines():
        key, _, value = line.partition('=')
        if key.strip() == 'sdk_version':
            return value.strip().strip('"')
    raise ValueError('cambc.toml has no sdk_version')


def compute(root, runner_version):
    """Build the manifest dict for the repository at root."""
    root = Path(root)
    groups = {}
    for name, prefixes in GROUPS.items():
        files = tracked_files(root, prefixes)
        groups[name] = {p: sha256_file(root / p) for p in files}
    bots = {}
    for path in tracked_files(root, ['bots/']):
        parts = path.split('/')
        if len(parts) >= 4 and parts[2] == 'src':
            bots.setdefault(parts[1], None)
    return {
        'runner_version': runner_version,
        'sdk_version': sdk_version(root),
        'files': groups,
        'bots': {name: digest(root / 'bots' / name) for name in sorted(bots)},
    }


def dumps(manifest):
    """Canonical bytes: the manifest's sha256 is taken over exactly these."""
    return (json.dumps(manifest, indent=1, sort_keys=True) + '\n').encode('utf-8')


def write(root, runner_version):
    data = dumps(compute(root, runner_version))
    (Path(root) / MANIFEST).parent.mkdir(parents=True, exist_ok=True)
    (Path(root) / MANIFEST).write_bytes(data)
    return hashlib.sha256(data).hexdigest()


def differences(expected, actual):
    """Human-readable list of differences between two manifest dicts."""
    out = []
    for key in ('runner_version', 'sdk_version'):
        if expected.get(key) != actual.get(key):
            out.append(f'{key}: manifest {expected.get(key)!r}, found {actual.get(key)!r}')
    for group in sorted(set(expected.get('files', {})) | set(actual.get('files', {}))):
        want, have = expected.get('files', {}).get(group, {}), actual.get('files', {}).get(group, {})
        for path in sorted(set(want) | set(have)):
            if want.get(path) != have.get(path):
                state = 'missing' if path not in have else 'unexpected' if path not in want else 'changed'
                out.append(f'{group}: {path} {state}')
    want, have = expected.get('bots', {}), actual.get('bots', {})
    for bot in sorted(set(want) | set(have)):
        if want.get(bot) != have.get(bot):
            out.append(f'bots: {bot} differs')
    return out
