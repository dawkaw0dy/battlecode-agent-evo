#!/usr/bin/env python3
"""Re-verify an evaluation job inside Actions before any bot is built (PLAN §9.2 step 3).

  check_job.py evo/jobs/<id>.json --matrix          print the shard matrix as JSON (plan job)
  check_job.py evo/jobs/<id>.json --shard <i> [--github-output FILE]
      check manifest, runner version, both bots' src_sha256 and allowlist, and the shard's map
      hashes; with --github-output also write a=, b=, maps= for later steps. Exit 1 on any mismatch.
"""
import argparse
import hashlib
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from evo import manifest  # noqa: E402
from evo.eval import allowlist, jobfile  # noqa: E402
from evo.eval.digest import digest  # noqa: E402


def job_problems(job, root=ROOT):
    problems = []
    data = (root / manifest.MANIFEST).read_bytes()
    if hashlib.sha256(data).hexdigest() != job['env_manifest_sha256']:
        problems.append('evo/env/manifest.json does not match env_manifest_sha256')
    expected = json.loads(data)
    if job['runner_version'] != expected['runner_version']:
        problems.append(f"runner_version {job['runner_version']} != manifest {expected['runner_version']}")
    if job['runner_version'] != jobfile.runner_version(root):
        problems.append('runner_version differs from evo/config.toml')
    problems += ['manifest: ' + d for d in manifest.differences(expected, manifest.compute(root, expected['runner_version']))]
    return problems


def shard_problems(job, index, root=ROOT):
    if not 0 <= index < len(job['shards']):
        return [f'no shard {index}']
    shard = job['shards'][index]
    problems = []
    for bot in dict.fromkeys((shard['a'], shard['b'])):
        found = digest(root / 'bots' / bot)
        if found != job['src_sha256'][bot]:
            problems.append(f'{bot}: src_sha256 {found} != job {job["src_sha256"][bot]}')
        problems += [f'{bot}: {p}' for p in allowlist.check_bot(root / 'bots' / bot)]
    for m in shard['maps']:
        path = jobfile.map_dir(root, m) / f'{m}.map26a'
        if not path.is_file():
            problems.append(f'map {m} missing')
        elif jobfile.map_sha256(root, m) != job['map_sha256'][m]:
            problems.append(f'map {m}: sha256 differs from the job')
    return problems


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('job')
    group = ap.add_mutually_exclusive_group(required=True)
    group.add_argument('--matrix', action='store_true')
    group.add_argument('--shard', type=int)
    ap.add_argument('--github-output')
    args = ap.parse_args(argv)
    job = jobfile.load(args.job)
    if args.matrix:
        print(json.dumps([{'shard': i, 'a': s['a'], 'b': s['b'], 'games': 2 * len(s['maps']),
                           'heldout': bool(s.get('heldout'))} for i, s in enumerate(job['shards'])]))
        return 0
    problems = job_problems(job) + shard_problems(job, args.shard)
    for p in problems:
        print('check_job:', p, file=sys.stderr)
    if problems:
        return 1
    shard = job['shards'][args.shard]
    if args.github_output:
        with open(args.github_output, 'a', encoding='utf-8') as out:
            map_dir = jobfile.HELDOUT_DIR if shard.get('heldout') else 'maps'
            out.write(f"a={shard['a']}\nb={shard['b']}\nmaps={' '.join(shard['maps'])}\nmap_dir={map_dir}\n")
    print(f"check_job: {job['job_id']} shard {args.shard} OK ({shard['a']} vs {shard['b']}, {len(shard['maps'])} maps)")
    return 0


if __name__ == '__main__':
    sys.exit(main())
