import os
import sys
from pathlib import Path

import pytest

from evo.orchestrator import launch

REAL_OUTPUT = """Created cloud session: M0 probe task
View: https://claude.ai/code/session_01HY3quh12DiamDT3sG6K8bZ?from=cli&m=0
Resume with: claude --teleport session_01HY3quh12DiamDT3sG6K8bZ
"""


def test_parse_real_launch_output():
    info = launch.parse_launch_output(REAL_OUTPUT)
    assert info == {'id': 'session_01HY3quh12DiamDT3sG6K8bZ',
                    'url': 'https://claude.ai/code/session_01HY3quh12DiamDT3sG6K8bZ'}


@pytest.mark.parametrize('text', [
    'Branch feature is not on GitHub\nView: https://claude.ai/code/session_01X\n',
    'Not uploading this working tree: it has uncommitted changes',
    'Error: --cloud requires an interactive terminal.',
    'Created cloud session: x\n',
])
def test_bad_launch_output_raises(text):
    with pytest.raises(launch.LaunchError):
        launch.parse_launch_output(text)


def test_same_session_maps_cse_to_session():
    assert launch.same_session('cse_01AB', 'session_01AB')
    assert launch.same_session('session_01AB', 'session_01AB')
    assert not launch.same_session('cse_01AB', 'session_01AC')
    assert not launch.same_session(None, 'session_01AB')


@pytest.mark.skipif(os.name != 'nt', reason='ConPTY is Windows-only')
def test_cloud_launcher_through_conpty_with_a_fake_cli(tmp_path):
    """The fake CLI refuses non-terminals exactly like `claude --cloud`; the launcher must still get the id."""
    script = tmp_path / 'fake_claude.py'
    script.write_text('import sys\n'
                      'if not sys.stdout.isatty():\n'
                      '    print("Error: --cloud requires an interactive terminal."); sys.exit(1)\n'
                      'assert sys.argv[1] == "--cloud"\n'
                      f'print({REAL_OUTPUT!r})\n')
    cmd = tmp_path / 'claude.cmd'
    cmd.write_text(f'@"{sys.executable}" "{script}" %*\n')
    root = tmp_path / 'clone'
    (root / '.claude').mkdir(parents=True)
    (root / '.claude' / 'settings.local.json').write_text('{"remote": {"defaultEnvironmentId": "env_x"}}')
    info = launch.CloudLauncher(root=root, claude=str(cmd), timeout=60).launch('e00-00', 1, 'You are evolution agent e00-00.')
    assert info['id'] == 'session_01HY3quh12DiamDT3sG6K8bZ' and 'Resume with' in info['output']


def test_cloud_launcher_requires_pinned_environment(tmp_path):
    with pytest.raises(launch.LaunchError, match='settings.local.json'):
        launch.CloudLauncher(root=Path(tmp_path), claude='claude').launch('e00-00', 1, 'x')


class FakeEngine:
    """Just enough of Engine for the `run` app loop."""

    def __init__(self, script):
        self.script, self.approved, self.resumed = list(script), [], []
        self.state = {'generation': 3, 'phase': 'TASKS_PUSHED', 'paused': None, 'halted': None, 'sessions_launched': 0,
                      'hall_of_fame': [], 'generations': {}, 'agents': {}, 'jobs': {}, 'log': []}
        self.cfg = {'agents': {'max_sessions_total': 30, 'max_parallel': 4}, 'run': {'max_generations': 5,
                                                                                    'step_interval_seconds': 0}}

    def step(self):
        paused, halted = self.script.pop(0)
        self.state.update(paused=paused, halted=halted)
        return 'paused' if paused else 'halted' if halted else 'waiting'

    def approval_terms(self, g):
        return {}, f'generation {g:02d}: at most 7 sessions'

    def approve(self, g):
        self.approved.append(g)

    def resume(self, reason):
        self.resumed.append(reason)
        self.state['generation'] += 1  # the real engine then plans and runs the next generation

    def new_agents(self, g=None):
        return []


def run_app(monkeypatch, script, answers):
    import builtins
    import evo.__main__ as app
    eng = FakeEngine(script)
    feed = list(answers)
    monkeypatch.setattr(builtins, 'input', lambda prompt='': feed.pop(0) if feed else (_ for _ in ()).throw(EOFError()))
    monkeypatch.setattr(app.time, 'sleep', lambda s: None)
    monkeypatch.setattr(app.os, 'system', lambda c: 0)
    return app.run(eng, eng.cfg, redraw_seconds=0), eng


def test_run_app_asks_before_launching(monkeypatch, capsys):
    code, eng = run_app(monkeypatch, [('waiting for approval: python -m evo approve 03', None)], ['n'])
    assert code == 0 and eng.approved == [] and 'APPROVAL NEEDED' in capsys.readouterr().out
    code, eng = run_app(monkeypatch, [('waiting for approval: python -m evo approve 03', None)], [])  # no keyboard
    assert eng.approved == []
    code, eng = run_app(monkeypatch, [('waiting for approval: python -m evo approve 03', None), (None, 'boom')], ['y'])
    assert eng.approved == [3] and code == 1


def test_run_app_resumes_when_max_generations_was_raised(monkeypatch):
    code, eng = run_app(monkeypatch, [('generation 03 selected; max_generations=4 reached', None),
                                      ('generation 04 selected; max_generations=5 reached', None)], [])
    assert len(eng.resumed) == 1  # g=3 < 5-1: resumed once; the scripted second pause is then reported


def test_best_copies_the_winner(tmp_path, capsys):
    import evo.__main__ as app

    class E:
        state = {'hall_of_fame': ['e02-01'], 'generations': {
            '2': {'survivors': ['e02-01'], 'ranking': ['e02-01', 'starter'],
                  'ratings': {'e02-01': {'strength': 1.0, 'interval': [0.5, 1.5]},
                              'starter': {'strength': None, 'interval': [-2, -1]}}},
            '3': {'slots': []}}}
    assert app.best(E(), target_root=tmp_path) == 0
    assert (tmp_path / 'e02-01' / 'src').is_dir() and 'Best bot: e02-01' in capsys.readouterr().out
