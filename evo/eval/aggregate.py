#!/usr/bin/env python3
"""Merge shard rows into results/<job_id>.json with the §4 header (PLAN §9.3).

  aggregate.py --job evo/jobs/<id>.json --parts <dir> --workflow .github/workflows/evo-eval.yml --out <file>

<dir> holds rows-<i>.json (extract.py output) and meta-<i>.json (shard metadata) per finished shard.
The header takes trigger_sha, run_id and run_attempt from GITHUB_SHA, GITHUB_RUN_ID, GITHUB_RUN_ATTEMPT.
"""
import argparse
import hashlib
import json
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from evo.eval import jobfile  # noqa: E402


def aggregate(job, parts, workflow_sha256, env):
    parts = Path(parts)
    rows, missing, cli, shard_meta = [], [], set(), {}
    for i in range(len(job['shards'])):
        rows_path = parts / f'rows-{i}.json'
        if not rows_path.is_file():
            missing.append(i)
            continue
        data = json.loads(rows_path.read_text(encoding='utf-8'))
        if data.get('job_id') != job['job_id'] or data.get('shard') != i:
            missing.append(i)
            continue
        rows += data['rows']
        meta_path = parts / f'meta-{i}.json'
        if meta_path.is_file():
            meta = json.loads(meta_path.read_text(encoding='utf-8'))
            shard_meta[str(i)] = meta
            if meta.get('cambc_version'):
                cli.add(meta['cambc_version'])
    return {
        'job_id': job['job_id'],
        'trigger_sha': env.get('GITHUB_SHA'),
        'run_id': env.get('GITHUB_RUN_ID'),
        'run_attempt': env.get('GITHUB_RUN_ATTEMPT'),
        'workflow_sha256': workflow_sha256,
        'cambc_version': sorted(cli),
        'requested_games': len(jobfile.requested_games(job)),
        'missing_shards': missing,
        'shards': shard_meta,
        'rows': rows,
    }


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--job', required=True)
    ap.add_argument('--parts', required=True)
    ap.add_argument('--workflow', required=True)
    ap.add_argument('--out', required=True)
    args = ap.parse_args(argv)
    job = jobfile.load(args.job)
    result = aggregate(job, args.parts, hashlib.sha256(Path(args.workflow).read_bytes()).hexdigest(), os.environ)
    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    Path(args.out).write_text(json.dumps(result, indent=1, sort_keys=True) + '\n', encoding='utf-8')
    print(f"aggregate: {len(result['rows'])} rows, missing shards {result['missing_shards']}")
    return 0


if __name__ == '__main__':
    sys.exit(main())
