import hashlib
import json
import shutil
import zipfile
from pathlib import Path

import pytest

from evo import manifest
from evo.eval import aggregate, allowlist, check_job, extract, jobfile

REPO = Path(__file__).resolve().parents[2]


def bot(tmp_path, files, name='x'):
    src = tmp_path / name / 'src'
    src.mkdir(parents=True)
    for rel, text in files.items():
        (src / rel).parent.mkdir(parents=True, exist_ok=True)
        (src / rel).write_text(text, encoding='utf-8')
    return tmp_path / name


GOOD_MAIN = '#include <cambc.h>\n#include "cambc.h"\n#include <vector>\n#include "util.h"\nint main() {}\n'


def test_real_bots_pass_the_allowlist():
    for name in ('starter', 'probe-random'):
        assert allowlist.check_bot(REPO / 'bots' / name) == []


def test_good_bot_passes(tmp_path):
    assert allowlist.check_bot(bot(tmp_path, {'main.cpp': GOOD_MAIN, 'util.h': '#pragma once\n'})) == []


@pytest.mark.parametrize('text, needle', [
    ('#include "../tools/x.h"\n', 'not a plain name'),
    ('#include <sys/stat.h>\n', 'not a plain name'),
    ('#include "missing.h"\n', 'neither an SDK header'),
    ('#include <secret>\n', 'not a standard or SDK header'),
    ('#include MACRO\n', 'must be <name>'),
    ('#embed "x.bin"\n', '#embed'),
    ('#  include_next <vector>\n', '#include_next'),
    ('#pragma clang diagnostic ignored "-W"\n', '#pragma other than once'),
    ('int f() { asm("nop"); }\n', 'forbidden token asm'),
    ('#if __has_include(<x>)\n#endif\n', '__has_include'),
    ('#inc\\\nlude "../a.h"\n', 'not a plain name'),
    ('%:include <vector>\n', 'digraph'),
])
def test_bad_sources_are_rejected(tmp_path, text, needle):
    problems = allowlist.check_bot(bot(tmp_path, {'main.cpp': text}))
    assert any(needle in p for p in problems), problems


def test_layout_rules(tmp_path):
    b = bot(tmp_path, {'main.cpp': 'int main(){}\n', 'run.py': 'print()\n', 'sub/a.h': '\n'})
    problems = allowlist.check_bot(b)
    assert any('run.py' in p for p in problems)
    assert any('subdirectories' in p for p in problems)
    (b / 'src' / 'big.cpp').write_bytes(b'/' * (allowlist.MAX_FILE_BYTES + 1))
    assert any('big.cpp' in p and 'max' in p for p in allowlist.check_bot(b))


def make_repo(tmp_path):
    """A throwaway repo with the files check_job needs; the manifest is written over it."""
    import subprocess
    root = tmp_path / 'repo'
    for rel in ('GameRules.md', 'BotAPICpp.md', 'cambc.toml', 'evo/config.toml', 'maps/00rows.map26a',
                'maps/00lanes.map26a'):
        (root / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy(REPO / rel, root / rel)
    for name in ('starter', 'probe-random'):
        shutil.copytree(REPO / 'bots' / name / 'src', root / 'bots' / name / 'src')
    subprocess.run(['git', 'init', '-q', str(root)], check=True)
    manifest.write(root, jobfile.runner_version(root))
    return root


def test_job_build_load_and_check(tmp_path):
    root = make_repo(tmp_path)
    job = jobfile.build('m1-test', [('probe-random', 'starter')], ['00rows', '00lanes'], shard_games=2, root=root)
    assert [s['maps'] for s in job['shards']] == [['00rows'], ['00lanes']]
    path = root / 'evo/jobs/m1-test.json'
    path.parent.mkdir(parents=True)
    path.write_bytes(jobfile.dumps(job))
    loaded = jobfile.load(path)
    assert len(jobfile.requested_games(loaded)) == 4
    assert check_job.job_problems(loaded, root) == []
    assert check_job.shard_problems(loaded, 0, root) == []
    # A bot changed after the job was written fails its shard.
    (root / 'bots/probe-random/src/main.cpp').write_text('int main() { return 1; }\n')
    assert any('src_sha256' in p for p in check_job.shard_problems(loaded, 0, root))
    # So does a changed map, and the manifest notices both.
    (root / 'maps/00lanes.map26a').write_bytes(b'tampered')
    assert any('00lanes' in p for p in check_job.shard_problems(loaded, 1, root))
    assert any('manifest' in p for p in check_job.job_problems(loaded, root))


def test_load_rejects_mismatched_name_and_paths(tmp_path):
    job = {'job_id': 'a', 'runner_version': 'x', 'env_manifest_sha256': 'y', 'src_sha256': {'s': '1'},
           'map_sha256': {'m': '2'}, 'shards': [{'a': '../s', 'b': 's', 'maps': ['m']}]}
    (tmp_path / 'b.json').write_text(json.dumps(job))
    with pytest.raises(ValueError, match='does not match'):
        jobfile.load(tmp_path / 'b.json')
    (tmp_path / 'a.json').write_text(json.dumps(job))
    with pytest.raises(ValueError, match='bad bot'):
        jobfile.load(tmp_path / 'a.json')


JOB = {'job_id': 'j1', 'runner_version': 'cambc==3.0.5', 'env_manifest_sha256': 'e',
       'src_sha256': {'p': 'sp', 's': 'ss'}, 'map_sha256': {'m1': 'h1', 'm2': 'h2'},
       'shards': [{'a': 'p', 'b': 's', 'maps': ['m1', 'm2']}]}


def fake_game(out, tag, outcome='teamA', gems=(500, 400), faults=(0, 0), errors=(0, 0)):
    out.mkdir(exist_ok=True)
    (out / f'{tag}.json').write_text(json.dumps({
        'teams': [{'team': 'teamA', 'submittedGems': str(gems[0])}, {'team': 'teamB', 'submittedGems': str(gems[1])}],
        'match': {'outcome': outcome, 'roundsStarted': 1234},
        'diagnostics': {'botFaults': {'teamA': faults[0], 'teamB': faults[1]}}}))
    with zipfile.ZipFile(out / f'{tag}.repl26a', 'w') as z:
        z.writestr('team-a.stdout', 'bot error: x\n' * errors[0])
        z.writestr('team-b.stdout', 'bot error: x\n' * errors[1])


def test_extract_rows(tmp_path):
    out = tmp_path / 'out'
    fake_game(out, 'm1-A')
    fake_game(out, 'm1-B', outcome='draw', faults=(3, 0), errors=(0, 2))
    fake_game(out, 'm2-A', outcome='teamB', gems=(0, 1000))
    rows = extract.rows(out, JOB, 0)
    assert [(r['map'], r['side'], r['outcome']) for r in rows] == [
        ('m1', 'A', 'teamA'), ('m1', 'B', 'unfinished'), ('m2', 'A', 'teamB')]  # m2-B missing: no row
    r = rows[1]
    assert r['faults'] == {'teamA': 3, 'teamB': 0} and r['bot_errors'] == {'teamA': 0, 'teamB': 2}
    assert r['gems'] == {'teamA': 500, 'teamB': 400} and r['rounds'] == 1234
    assert r['src_sha256'] == {'p': 'sp', 's': 'ss'} and r['map_sha256'] == 'h1'
    assert r['replay_sha256'] == hashlib.sha256((out / 'm1-B.repl26a').read_bytes()).hexdigest()


def test_extract_build_failed(tmp_path):
    rows = extract.rows(tmp_path, JOB, 0, build_failed=['p'])
    assert len(rows) == 4 and all(r['outcome'] == 'build_failed' and r['build_failed'] == ['p'] for r in rows)


def test_aggregate_header_and_missing_shards(tmp_path):
    job = dict(JOB, shards=JOB['shards'] + [{'a': 'p', 'b': 's', 'maps': ['m2']}])
    parts = tmp_path / 'parts'
    parts.mkdir()
    (parts / 'rows-0.json').write_text(json.dumps({'job_id': 'j1', 'shard': 0, 'rows': [{'x': 1}]}))
    (parts / 'meta-0.json').write_text(json.dumps({'cambc_version': 'cambc 3.0.5'}))
    env = {'GITHUB_SHA': 'abc', 'GITHUB_RUN_ID': '7', 'GITHUB_RUN_ATTEMPT': '1'}
    res = aggregate.aggregate(job, parts, 'wf', env)
    assert res['missing_shards'] == [1] and res['rows'] == [{'x': 1}]
    assert (res['trigger_sha'], res['run_id'], res['workflow_sha256']) == ('abc', '7', 'wf')
    assert res['cambc_version'] == ['cambc 3.0.5'] and res['requested_games'] == 6
