"""Session launchers (PLAN §7). M2 uses FakeLauncher; the cloud launcher arrives in M3.

A launcher returns {'id': 'session_…', 'url': … or None} for a started session and raises
LaunchError when nothing was started.
"""


class LaunchError(RuntimeError):
    pass


class FakeLauncher:
    """M2: records a synthetic session; evo/m2/fake_agents.py then plays the agent's part."""

    def launch(self, agent_id, attempt, prompt):
        return {'id': f'session_fake{agent_id.replace("-", "")}a{attempt}', 'url': None}

    def nudge(self, session_id, message):
        return True


class CloudLauncher:
    """`claude --cloud` needs a TTY (M0 item 10); the pseudo-terminal launcher is built in M3."""

    def launch(self, agent_id, attempt, prompt):
        raise LaunchError('cloud launcher not implemented yet (M3)')

    def nudge(self, session_id, message):
        raise LaunchError('cloud launcher not implemented yet (M3)')


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
