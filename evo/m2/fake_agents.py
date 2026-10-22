"""M2 fake agents: play the cloud sessions' part by pushing branches with DONE markers.

usage: python -m evo.m2.fake_agents [--config evo/m2/config.toml]

For every agent the orchestrator marked `running`, this creates a branch `claude/fake-<id>` from
the session's base commit with one of eight behaviours, chosen by slot:
  0 starter copy   1 probe-random copy   2 stands still   3 faults every turn   4 fails to build
  5 edits tools/benchmark.py   6 adds a file outside its folder   7 uses #include "../…"
Slots 5–7 must be rejected at admission (§16 M2).
"""
import argparse
import hashlib
import json
import shutil
import sys
import tempfile
from pathlib import Path

from evo.orchestrator import config, gitops
from evo.orchestrator import state as st

KINDS = ['starter-copy', 'probe-random-copy', 'standstill', 'faulter', 'nobuild',
         'tamper-tools', 'tamper-outside', 'tamper-include']

IDLE_MAIN = '''// {note}
#include <cambc.h>
#include "draft.h"

int main() {{
    while (true) {{
        if (cambc::is_draft())
            play_draft();
        else {{
            {turn}
        }}
        cambc::end_turn();
    }}
}}
'''


def bot_files(root, kind, agent_id):
    """{relative path under bots/<id>: bytes} for the bot part of a fake agent."""
    note = f'fake agent {agent_id}: {kind}'
    src = Path(root) / 'bots' / ('probe-random' if kind == 'probe-random-copy' else 'starter') / 'src'
    files = {f'src/{p.name}': p.read_bytes() for p in sorted(src.iterdir()) if p.is_file()}
    if kind in ('standstill', 'faulter'):
        files = {k: v for k, v in files.items() if k in ('src/draft.cpp', 'src/draft.h', 'src/random.h')}
        files['src/main.cpp'] = IDLE_MAIN.format(
            note=note, turn='// stand still' if kind == 'standstill' else '__builtin_trap();').encode()
    elif kind == 'nobuild':
        files['src/main.cpp'] = f'// {note}\nint main() {{ this is not C++; }}\n'.encode()
    elif kind == 'tamper-include':
        files['src/main.cpp'] = f'// {note}\n#include "../../tools/evil.h"\n'.encode() + files['src/main.cpp']
    else:
        files['src/main.cpp'] = f'// {note}\n'.encode() + files['src/main.cpp']
    files['NOTES.md'] = (f'## Strategy (≤10 lines)\n{note}\n\n## Key parameters\nnone\n\n## Changes this generation\n'
                         f'fake\n\n## Tried and failed\n- {kind}: nothing (fake agent)\n\n## Known weaknesses\nfake\n').encode()
    return files


def plant(root, agent, push=True):
    """Push the fake agent's branch; returns (branch, commit)."""
    aid, slot = agent['id'], agent['slot']
    kind = KINDS[slot % len(KINDS)]
    session = agent['sessions'][-1]
    branch = f'claude/fake-{aid}'
    work = Path(tempfile.mkdtemp(prefix=f'evo-fake-{aid}-'))
    shutil.rmtree(work)
    gitops.git('worktree', 'add', '-q', '-B', branch, str(work), session['base_commit'], root=root)
    try:
        for rel, data in bot_files(root, kind, aid).items():
            gitops.write_file(f'bots/{aid}/{rel}', data, work)
        if kind == 'tamper-tools':
            p = work / 'tools/benchmark.py'
            p.write_bytes(p.read_bytes() + b'\n# tampered by a fake agent\n')
        if kind == 'tamper-outside':
            gitops.write_file('bots/starter/src/extra.h', b'#pragma once\n', work)
        task = (work / f'evo/tasks/{aid}.md').read_bytes()
        marker = {'agent': aid, 'task_sha256': hashlib.sha256(task).hexdigest(), 'base_commit': session['base_commit'],
                  'session_id': 'cse_' + session['id'].removeprefix('session_'), 'bot_dir': f'bots/{aid}',
                  'self_eval': {'vs': 'starter', 'games': 0, 'wins': 0}, 'games_played': 0, 'summary': f'fake {kind}'}
        gitops.write_file(f'evo/agents/{aid}/DONE.json', (json.dumps(marker, indent=1) + '\n').encode(), work)
        gitops.git('add', '-A', root=work)
        gitops.git('-c', 'user.name=fake-agent', '-c', 'user.email=fake@example.invalid', 'commit', '-q', '-m',
                   f'fake agent {aid} ({kind})', root=work)
        sha = gitops.head(work)
        if push:
            gitops.git('push', '-q', 'origin', f'{branch}:{branch}', root=work)
        return branch, sha, kind
    finally:
        gitops.git('worktree', 'remove', '--force', str(work), root=root)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--config', default='evo/m2/config.toml')
    args = ap.parse_args(argv)
    cfg = config.load(config.ROOT / args.config)
    state = st.load(config.path(cfg, 'state'))
    done = 0
    for aid, a in state['agents'].items():
        if a['status'] == 'running':
            branch, sha, kind = plant(config.ROOT, a)
            print(f'{aid}: {kind} -> {branch} {sha[:10]}')
            done += 1
    print(f'{done} fake agents pushed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
