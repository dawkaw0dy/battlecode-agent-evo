"""Session launchers (PLAN §7, D27). A launcher returns {'id': 'session_…', 'url': …, 'output': …}
for a started session and raises LaunchError when nothing was started.
"""
import json
import re
import shutil
import subprocess

from evo.orchestrator.config import ROOT

SESSION = re.compile(r'View:\s*(https://claude\.ai/code/(session_[A-Za-z0-9]+))')
# §6.1, §7.2: a branch missing on GitHub or an upload of the local checkout means a bad launch.
FAILURE_MARKERS = ('is not on GitHub', 'Not uploading this working tree', 'Uploading', 'Bundling',
                   'requires an interactive terminal')


class LaunchError(RuntimeError):
    pass


class FakeLauncher:
    """M2: records a synthetic session; evo/m2/fake_agents.py then plays the agent's part."""

    def launch(self, agent_id, attempt, prompt):
        return {'id': f'session_fake{agent_id.replace("-", "")}a{attempt}', 'url': None}

    def nudge(self, session_id, message):
        return True


def parse_launch_output(text):
    """{'id', 'url'} from `claude --cloud` output, or LaunchError naming the problem (M0 item 10)."""
    lowered = text.lower()
    for marker in FAILURE_MARKERS:
        if marker.lower() in lowered:
            raise LaunchError(f'launch output says {marker!r}: {text.strip()[-500:]}')
    m = SESSION.search(text)
    if not m:
        raise LaunchError(f'no session id in launch output: {text.strip()[-500:]}')
    return {'id': m.group(2), 'url': m.group(1)}


class CloudLauncher:
    """`claude --cloud "<prompt>"` from the clone, inside a pseudo-terminal (D27).

    The CLI needs a TTY (M0 item 10). It prints `Created cloud session: …`, `View: https://claude.ai/code/session_…`
    and `Resume with: …`, then returns by itself after about 10 s. The environment comes from
    `.claude/settings.local.json` (§6.1).
    """

    def __init__(self, root=ROOT, claude=None, timeout=240):
        self.root, self.timeout = root, timeout
        self.claude = claude or shutil.which('claude')

    def launch(self, agent_id, attempt, prompt):
        from evo.orchestrator import conpty
        if not self.claude:
            raise LaunchError('claude is not on PATH')
        if not (self.root / '.claude' / 'settings.local.json').is_file():
            raise LaunchError('.claude/settings.local.json does not pin the evo environment (§6.1)')
        code, text = conpty.run([self.claude, '--cloud', prompt], cwd=self.root, timeout=self.timeout,
                                stop_when=lambda s: 'Resume with:' in s)
        info = parse_launch_output(text)  # raises if it failed
        if code not in (0, None):
            raise LaunchError(f'claude --cloud exited {code}: {text.strip()[-500:]}')
        info['output'] = text.strip()[-1500:]
        return info

    def nudge(self, session_id, message):
        """`claude -p "<message>" --cloud <id>` queues a message and exits (docs: Send follow-ups from the CLI)."""
        if not self.claude:
            return False
        proc = subprocess.run([self.claude, '-p', message, '--cloud', session_id, '--output-format', 'json'],
                              cwd=self.root, capture_output=True, text=True, encoding='utf-8', errors='replace',
                              stdin=subprocess.DEVNULL, timeout=180)
        try:
            return bool(json.loads(proc.stdout.strip().splitlines()[-1]).get('ok'))
        except (ValueError, IndexError):
            return False


def make(cfg):
    kind = cfg['run']['launcher']
    if kind == 'fake':
        return FakeLauncher()
    if kind == 'cloud':
        return CloudLauncher()
    raise ValueError(f'unknown launcher {kind!r}')


def same_session(marker_id, launched_id):
    """§7: a marker's cse_… id names the session_… id the launcher printed (M0 item 13)."""
    def norm(s):
        s = s or ''
        return 'session_' + s[4:] if s.startswith('cse_') else s
    return bool(marker_id) and norm(marker_id) == norm(launched_id)
