"""Orchestrator state (PLAN §4, §5): load/save evo/state.json atomically; phases and statuses."""
import json
import os
import time
from pathlib import Path

PHASES = ['PLANNED', 'TASKS_PUSHED', 'RUNNING', 'ADMITTED', 'EVAL_A_PENDING', 'EVAL_A_DONE',
          'EVAL_B_PENDING', 'EVAL_B_DONE', 'SELECTED']
STATUSES = ['planned', 'running', 'done', 'rejected', 'dead', 'admitted', 'disqualified', 'rated']


def empty():
    return {'version': 1, 'generation': 0, 'phase': 'PLANNED', 'paused': None, 'halted': None,
            'agents': {}, 'generations': {}, 'jobs': {}, 'hall_of_fame': [],
            'sessions_launched': 0, 'launch_errors_in_a_row': 0, 'log': []}


def load(path):
    path = Path(path)
    if not path.exists():
        return None
    return json.loads(path.read_text(encoding='utf-8'))


def save(state, path):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + '.tmp')
    tmp.write_text(json.dumps(state, indent=1, sort_keys=True) + '\n', encoding='utf-8', newline='\n')
    os.replace(tmp, path)


def gen(state, g=None):
    g = state['generation'] if g is None else g
    return state['generations'].setdefault(str(g), {'slots': [], 'approval': None, 'jobs': {'A': [], 'B': []},
                                                    'env_failed': 0})


def set_phase(state, phase, note=''):
    assert phase in PHASES, phase
    state['phase'] = phase
    log(state, f'phase {phase}' + (f': {note}' if note else ''))


def pause(state, reason):
    state['paused'] = reason
    log(state, f'PAUSED: {reason}')


def halt(state, reason):
    state['halted'] = reason
    log(state, f'HALTED: {reason}')


def log(state, message):
    state['log'].append(f"{time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())} g{state['generation']:02d} {message}")
    del state['log'][:-200]


def now():
    return int(time.time())
