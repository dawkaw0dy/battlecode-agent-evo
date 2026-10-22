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
