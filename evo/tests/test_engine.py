"""One generation end to end against a local bare origin: fake launcher, fake agents, simulated Actions."""
import copy
import hashlib
import json
import shutil
import subprocess
from pathlib import Path

import pytest

from evo import manifest
from evo.eval import jobfile
from evo.m2 import fake_agents
from evo.orchestrator import config, engine, gitops, launch, verify

REPO = Path(__file__).resolve().parents[2]
MAPS = ['00rows', '00lanes', '00eight']
STRENGTH = {'starter-copy': 2, 'probe-random-copy': 3, 'standstill': 0, 'faulter': 1, 'nobuild': 1, 'starter': 2}


def git(root, *args):
    return subprocess.run(['git', *args], cwd=root, check=True, capture_output=True, text=True).stdout.strip()


@pytest.fixture
def world(tmp_path):
    origin = tmp_path / 'origin.git'
    subprocess.run(['git', 'init', '-q', '--bare', '-b', 'main', str(origin)], check=True)
    work = tmp_path / 'work'
    subprocess.run(['git', 'clone', '-q', str(origin), str(work)], check=True, capture_output=True)
    for k, v in (('user.name', 'orchestrator'), ('user.email', 'o@example.invalid'), ('core.autocrlf', 'false')):
        git(work, 'config', k, v)
    for rel in ['cambc.toml', 'tools/benchmark.py', '.github/workflows/evo-eval.yml', 'evo/config.toml',
                *[f'maps/{m}.map26a' for m in MAPS]]:
        (work / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy(REPO / rel, work / rel)
    for name in ('starter', 'probe-random'):
        shutil.copytree(REPO / 'bots' / name / 'src', work / 'bots' / name / 'src')
    shutil.copytree(REPO / 'evo/m2/archetypes', work / 'evo/m2/archetypes')
    (work / '.gitattributes').write_text('* text=auto eol=lf\n*.map26a binary\n')
    manifest.write(work, 'cambc==3.0.5')
    git(work, 'add', '-A')
    git(work, 'commit', '-q', '-m', 'base')
    git(work, 'push', '-q', 'origin', 'HEAD:main')
    cfg = config.load(REPO / 'evo/config.toml')
    cfg = copy.deepcopy(cfg)
    cfg['run'].update(id_prefix='t', job_prefix='tg', state='evo/m2/state.json', lineage='evo/m2/lineage.jsonl',
                      cache='evo/m2/cache.jsonl', launcher='fake', max_generations=2,
                      archetypes_dir='evo/m2/archetypes', bootstrap_replicates=20)
    # Pin everything the assertions depend on, independent of the real run's evo/config.toml.
    cfg.update(population=8, keep_fraction=0.25, elites=1, immigrants=1)
    cfg['eval'].update(stage_a_maps=2, stage_b_top=2, spotcheck_min=2, max_fault_rate=0.10, exceptions_disqualify=False,
                       heldout_pool='', spotcheck_where='local')
    cfg['agents'].update(max_parallel=8, require_approval=True, max_sessions_total=40, relaunch_dead=1,
                         deadline_minutes=180)
    return work, origin, cfg


def kind_of(bot):
    if bot in STRENGTH:
        return bot
    return fake_agents.KINDS[int(bot.split('-')[1])]


def simulate(row_a, row_b, m, side):
    """Deterministic fake game: stronger kind wins, ties go to team B, faulter faults, nobuild cannot build."""
    team_a, team_b = (row_a, row_b) if side == 'A' else (row_b, row_a)
    ka, kb = kind_of(team_a), kind_of(team_b)
    if 'nobuild' in (ka, kb):
        return {'outcome': 'build_failed', 'build_failed': [b for b in (row_a, row_b) if kind_of(b) == 'nobuild']}
    sa, sb = STRENGTH[ka], STRENGTH[kb]
    outcome = 'teamA' if sa > sb else 'teamB'
    return {'outcome': outcome, 'gems': {'teamA': 100 * sa + len(m), 'teamB': 100 * sb + len(m)}, 'rounds': 2000,
            'faults': {'teamA': 2000 * (ka == 'faulter'), 'teamB': 2000 * (kb == 'faulter')},
            'bot_errors': {'teamA': 0, 'teamB': 0}}


def run_actions(work, origin, job_id):
    """Play the job like evo-eval.yml would and push results/<job>.json to evo-results in origin."""
    git(work, 'fetch', '-q', 'origin')
    pushed = git(work, 'log', '-1', '--format=%H', 'origin/main', '--', f'evo/jobs/{job_id}.json')
    job = json.loads(gitops.show(pushed, f'evo/jobs/{job_id}.json', work))
    rows = []
    for a, b, m, side in jobfile.requested_games(job):
        rows.append({'a': a, 'b': b, 'map': m, 'side': side, **simulate(a, b, m, side),
                     'src_sha256': {a: job['src_sha256'][a], b: job['src_sha256'][b]}, 'map_sha256': job['map_sha256'][m],
                     'stats_sha256': 'x', 'replay_sha256': 'y'})
    wf = hashlib.sha256(gitops.show(pushed, '.github/workflows/evo-eval.yml', work)).hexdigest()
    results = {'job_id': job_id, 'trigger_sha': pushed, 'run_id': '1', 'run_attempt': '1', 'workflow_sha256': wf,
               'cambc_version': ['cambc 3.0.5'], 'missing_shards': [], 'rows': rows}
    res = work.parent / 'results'
    if not res.exists():
        subprocess.run(['git', 'clone', '-q', str(origin), str(res)], check=True, capture_output=True)
        git(res, 'config', 'user.name', 'actions')
        git(res, 'config', 'user.email', 'a@example.invalid')
        if subprocess.run(['git', 'fetch', '-q', 'origin', 'evo-results'], cwd=res, capture_output=True).returncode == 0:
            git(res, 'checkout', '-q', '-B', 'evo-results', 'FETCH_HEAD')
        else:
            git(res, 'checkout', '-q', '--orphan', 'evo-results')
            git(res, 'rm', '-rq', '--cached', '.')
    (res / 'results').mkdir(exist_ok=True)
    (res / 'results' / f'{job_id}.json').write_text(json.dumps(results))
    git(res, 'add', 'results')
    git(res, 'commit', '-q', '-m', job_id)
    git(res, 'push', '-q', 'origin', 'HEAD:evo-results')
    return rows


def fake_replay_from(rows_seen):
    def replay(a, b, maps, root):
        out = {}
        for m in maps:
            for side in ('A', 'B'):
                r = simulate(a, b, m, side)
                out[(m, side)] = (r['outcome'], r.get('gems'), r.get('rounds'))
        return out
    return replay


def test_one_generation_end_to_end(world):
    work, origin, cfg = world
    eng = engine.Engine(cfg, root=work, launcher=launch.FakeLauncher(), replay=fake_replay_from([]), out=lambda *_: None)
    eng.init()
    assert len(eng.state['generations']['0']['slots']) == 8
    assert eng.step() == 'paused' and 'approve' in eng.state['paused']        # §0.4 approval gate
    eng.approve(0)
    assert eng.step() == 'waiting' and eng.state['phase'] == 'RUNNING'
    running = [a for a in eng.state['agents'].values() if a['status'] == 'running']
    assert len(running) == 8 and eng.state['sessions_launched'] == 8
    for a in running:
        fake_agents.plant(work, a)
    assert eng.step() == 'waiting' and eng.state['phase'] == 'EVAL_A_PENDING'
    agents = eng.state['agents']
    assert [agents[f't00-0{i}']['status'] for i in range(8)] == ['admitted'] * 5 + ['rejected'] * 3
    assert 'tools/benchmark.py' in agents['t00-05']['reason']
    assert 'bots/starter/src/extra.h' in agents['t00-06']['reason']
    assert 'not a plain name' in agents['t00-07']['reason']
    assert (work / 'bots/t00-01/NOTES.md').is_file() and not (work / 'bots/t00-05').exists()
    for stage in ('A', 'B'):
        jid = eng.state['generations']['0']['jobs'][stage][-1]
        run_actions(work, origin, jid)
        eng.step()
    gen = eng.state['generations']['0']
    assert eng.state['halted'] is None, eng.state['halted']
    ratings = gen['ratings']
    assert ratings['t00-03']['disqualified'] and ratings['t00-04']['disqualified']   # faulter, build failure
    assert gen['ranking'][0] == 't00-01'
    assert gen['survivors'] == ['t00-01', 't00-00'] and eng.state['hall_of_fame'] == ['t00-01']
    # generation 1 was planned and its tasks pushed; it now waits for approval
    assert eng.state['generation'] == 1 and eng.state['phase'] == 'TASKS_PUSHED' and 'approve 01' in eng.state['paused']
    agents = eng.state['agents']  # step() re-reads the state file, so take a fresh reference
    g1 = eng.state['generations']['1']
    assert g1['elites'] == ['t00-01'] and len(g1['slots']) == 8
    kids = [agents[x] for x in g1['slots'] if x.startswith('t01-')]
    assert [k['parents'][0] for k in kids if k['kind'] == 'child'] == ['t00-01'] * 4 + ['t00-00'] * 2
    imm = [k for k in kids if k['kind'] == 'immigrant']
    used0 = {agents[f't00-0{i}']['archetype'] for i in range(8)}
    assert len(imm) == 1 and imm[0]['archetype'] not in used0
    assert all(k['task_sha256'] for k in kids)
    assert 'Mutation:' in (work / f"evo/tasks/{kids[0]['id']}.md").read_text(encoding='utf-8')
    lineage = [json.loads(line) for line in (work / 'evo/m2/lineage.jsonl').read_text().splitlines()]
    assert {r['agent'] for r in lineage} >= {f't00-0{i}' for i in range(8)}
    # nothing is left uncommitted, and origin has everything
    assert gitops.dirty(work) == '' and git(work, 'rev-parse', 'HEAD') == git(work, 'rev-parse', 'origin/main')


def test_spotcheck_mismatch_is_reported():
    row = {'a': 'x', 'b': 'y', 'map': 'm', 'side': 'A', 'outcome': 'teamA', 'gems': {'teamA': 1, 'teamB': 0},
           'rounds': 10}
    same = verify.spotcheck([row], replay=lambda a, b, ms, root: {('m', 'A'): ('teamA', {'teamA': 1, 'teamB': 0}, 10)})
    other = verify.spotcheck([row], replay=lambda a, b, ms, root: {('m', 'A'): ('teamB', {'teamA': 0, 'teamB': 1}, 10)})
    assert same == [] and len(other) == 1


def test_marker_from_another_session_is_ignored(world):
    work, origin, cfg = world
    eng = engine.Engine(cfg, root=work, launcher=launch.FakeLauncher(), replay=fake_replay_from([]), out=lambda *_: None)
    eng.init()
    eng.step()
    eng.approve(0)
    eng.step()
    a = eng.state['agents']['t00-00']
    stale = dict(a, sessions=[dict(a['sessions'][-1], id='session_someoneelse')])
    fake_agents.plant(work, stale)
    eng.step()
    assert eng.state['agents']['t00-00']['status'] == 'running'     # wrong session_id: not done


def test_stage_c_boundary(tmp_path):
    """§11 Stage C: only when the last survivor and the first non-survivor overlap and held-out maps remain."""
    from evo.orchestrator import state as st
    (tmp_path / 'heldout' / 'maps').mkdir(parents=True)
    for i in range(4):
        (tmp_path / 'heldout' / 'maps' / f'h-{i}.map26a').write_bytes(b'x')
    cfg = copy.deepcopy(config.load(REPO / 'evo/config.toml'))
    cfg.update(population=4, keep_fraction=0.25)
    cfg['eval'].update(heldout_pool='o/p', heldout_stage_b=2)
    eng = engine.Engine(cfg, root=tmp_path, launcher=launch.FakeLauncher(), out=lambda *_: None)
    eng.state = st.empty()
    st.gen(eng.state)['members'] = ['a', 'b', 'c', 'd']
    r = lambda lo, hi: {'disqualified': False, 'interval': (lo, hi)}
    ratings = {'a': r(1.0, 2.0), 'b': r(0.4, 0.9), 'c': r(0.0, 0.5), 'd': r(-1, -0.5)}
    assert eng.stage_c_boundary(ratings, ['a', 'b', 'c', 'd'], []) == ['b', 'c']      # 0.4–0.9 overlaps 0.0–0.5
    ratings['c'] = r(-0.5, 0.3)
    assert eng.stage_c_boundary(ratings, ['a', 'b', 'c', 'd'], []) is None            # clear gap
    cfg['eval']['heldout_pool'] = ''
    ratings['c'] = r(0.0, 0.5)
    assert eng.stage_c_boundary(ratings, ['a', 'b', 'c', 'd'], []) is None            # no held-out pool


def test_resume_after_max_generations_plans_without_redoing_selection(world):
    """End of the e2e flow with max_generations=1: resume must plan gen 1 once, without new lineage rows."""
    work, origin, cfg = world
    cfg['run']['max_generations'] = 1
    eng = engine.Engine(cfg, root=work, launcher=launch.FakeLauncher(), replay=fake_replay_from([]), out=lambda *_: None)
    eng.init()
    eng.step()
    eng.approve(0)
    eng.step()
    for a in [a for a in eng.state['agents'].values() if a['status'] == 'running']:
        fake_agents.plant(work, a)
    eng.step()
    for stage in ('A', 'B'):
        run_actions(work, origin, eng.state['generations']['0']['jobs'][stage][-1])
        eng.step()
    assert 'max_generations' in eng.state['paused'] and eng.state['generation'] == 0
    rows = (work / 'evo/m2/lineage.jsonl').read_text().splitlines()
    with pytest.raises(SystemExit):
        eng.state['paused'] = 'waiting for approval: python -m evo approve 00'
        eng.resume('x')
    eng.state['paused'] = 'generation 00 selected; max_generations=1 reached'
    cfg['run']['max_generations'] = 2
    eng.resume('test')
    eng.step()
    assert eng.state['generation'] == 1 and 'approve 01' in eng.state['paused']
    assert (work / 'evo/m2/lineage.jsonl').read_text().splitlines() == rows
    assert eng.state['hall_of_fame'] == ['t00-01']


def test_spotcheck_runs_as_an_actions_job(world):
    """D35: the sample becomes job <jid>-spot on ubuntu-22.04; matching replays continue, a mismatch halts."""
    work, origin, cfg = world
    cfg['eval']['spotcheck_where'] = 'actions'
    cfg['run']['max_generations'] = 1
    eng = engine.Engine(cfg, root=work, launcher=launch.FakeLauncher(), out=lambda *_: None)
    eng.init()
    eng.step()
    eng.approve(0)
    eng.step()
    for a in [a for a in eng.state['agents'].values() if a['status'] == 'running']:
        fake_agents.plant(work, a)
    eng.step()
    jid = eng.state['generations']['0']['jobs']['A'][-1]
    run_actions(work, origin, jid)
    eng.step()                                            # results arrive -> spot job pushed, waiting
    spot = eng.state['jobs'][jid]['spotcheck']
    assert spot['job'] == f'{jid}-spot' and spot['status'] == 'pending'
    sjob = json.loads(gitops.show(spot['pushed_commit'], f'evo/jobs/{jid}-spot.json', work))
    assert sjob['runs_on'] == 'ubuntu-22.04' and len(jobfile.requested_games(sjob)) >= spot['games']
    assert eng.step() == 'waiting' and eng.state['phase'] == 'EVAL_A_DONE'   # nothing yet
    run_actions(work, origin, f'{jid}-spot')
    eng.step()
    assert eng.state['jobs'][jid]['spotcheck']['status'] == 'done' and eng.state['phase'] in ('EVAL_B_PENDING',)
    jid_b = eng.state['generations']['0']['jobs']['B'][-1]
    run_actions(work, origin, jid_b)
    eng.step()
    # a forged replay: the spot-check job reports a different outcome for one sampled game
    global simulate
    real = simulate

    def flipped(a, b, m, side):
        r = real(a, b, m, side)
        if r['outcome'] in ('teamA', 'teamB'):
            r = dict(r, outcome='teamB' if r['outcome'] == 'teamA' else 'teamA')
        return r
    simulate = flipped
    try:
        run_actions(work, origin, f'{jid_b}-spot')
    finally:
        simulate = real
    eng.step()
    assert eng.state['halted'] and 'spot-check mismatch' in eng.state['halted']


def test_emergency_stop_retires_running_sessions(world):
    work, origin, cfg = world
    sent = []

    class Recorder(launch.FakeLauncher):
        def nudge(self, session_id, message):
            sent.append((session_id, message))
            return True

    eng = engine.Engine(cfg, root=work, launcher=Recorder(), replay=fake_replay_from([]), out=lambda *_: None)
    eng.init()
    eng.step()
    eng.approve(0)
    eng.step()                                       # 8 sessions running
    other = engine.Engine(cfg, root=work, launcher=Recorder(), out=lambda *_: None)   # e.g. evo-stop.cmd
    stopped = other.stop('test')
    assert len(stopped) == 8 and len(sent) == 8 and all('STOP' in m for _, m in sent)
    for a in [a for a in eng.state['agents'].values() if a['sessions']]:
        fake_agents.plant(work, a)                    # agents that push anyway
    assert eng.step() == 'paused' and eng.state['paused'].startswith('STOPPED by owner')   # step re-read the state
    assert all(a['status'] == 'dead' for a in eng.state['agents'].values())
    eng.resume('test over')
    eng.step()                                       # every slot is dead: nothing is admitted, the run waits
    assert not [a for a in eng.state['agents'].values() if a['status'] == 'admitted']


def test_late_start_nudges_before_enforcing_the_deadline(world):
    # D42: the app was closed when the nudge was due; on restart it nudges first and allows 20 more minutes
    work, origin, cfg = world
    cfg['agents']['relaunch_dead'] = 0
    sent = []

    class Recorder(launch.FakeLauncher):
        def nudge(self, session_id, message):
            sent.append(session_id)
            return True

    eng = engine.Engine(cfg, root=work, launcher=Recorder(), replay=fake_replay_from([]), out=lambda *_: None)
    eng.init()
    eng.step()
    eng.approve(0)
    eng.step()                                       # 8 sessions running
    running = [a for a in eng.state['agents'].values() if a['sessions']]
    for a in running:
        a['sessions'][-1]['launched_at'] -= 10 * 3600  # launched 10 hours ago, never nudged
    eng.save('test: app was closed')
    eng.step()
    agents = eng.state['agents']
    assert len(sent) == 8 and all(agents[a['id']]['status'] == 'running' for a in running)
    for a in running:
        agents[a['id']]['sessions'][-1]['nudged_at'] -= 21 * 60
    eng.save('test: 21 minutes after the nudge')
    eng.step()
    assert all(eng.state['agents'][a['id']]['status'] == 'dead' for a in running) and len(sent) == 8
