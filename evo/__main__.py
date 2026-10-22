"""Command line: python -m evo [--config FILE] <command> (PLAN §2, D5).

  bootstrap   create the venv, SDK, maps, starter bot and manifest (§1.1)
  manifest    rewrite evo/env/manifest.json and print its sha256
  maptags     derive evo/prompts/map_tags.json from the maps (D26)
  init        create the state with generation 0 (§12.4)
  approve GG  approve generation GG's sessions (§0.4); prints the maximum it approves
  step        advance the current generation as far as it can, then exit (§5)
  run         call step every [run].step_interval_seconds until paused or halted
  status      print the phase, agents and jobs
  verify JOB  re-check a results file (no spot-check)
  spotcheck JOB  replay JOB's spot-check sample locally
"""
import argparse
import json
import os
import sys
import time

from evo import bootstrap


def main(argv=None):
    os.environ.setdefault('PYTHONIOENCODING', 'utf-8')
    os.environ.setdefault('PYTHONUTF8', '1')
    parser = argparse.ArgumentParser(prog='python -m evo', description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--config', help='config file (default evo/config.toml or $EVO_CONFIG)')
    sub = parser.add_subparsers(dest='command', required=True)
    boot = sub.add_parser('bootstrap')
    boot.add_argument('--no-smoke', action='store_true', help='skip tools/tests and the starter game')
    for name in ('manifest', 'maptags', 'init', 'step', 'run', 'status'):
        sub.add_parser(name)
    sub.add_parser('approve').add_argument('generation', help='GG or gGG')
    sub.add_parser('verify').add_argument('job')
    sub.add_parser('spotcheck').add_argument('job')
    args = parser.parse_args(argv)
    if args.command == 'bootstrap':
        return bootstrap.run(smoke_games=not args.no_smoke)
    if args.command == 'manifest':
        print(bootstrap.write_manifest())
        return 0

    from evo.orchestrator import config, engine, maptags, verify
    from evo.orchestrator import gitops, jobs
    cfg = config.load(args.config)
    if args.command == 'maptags':
        target, tags = maptags.build(cfg)
        print(f'{target.relative_to(config.ROOT).as_posix()}: {len(tags)} maps')
        return 0
    eng = engine.Engine(cfg)
    if args.command == 'init':
        eng.init()
        return 0
    if args.command == 'approve':
        eng.approve(int(args.generation.lstrip('gG')))
        return 0
    if args.command == 'step':
        print(eng.step())
        return 0
    if args.command == 'run':
        while True:
            result = eng.step()
            print(time.strftime('%H:%M:%S'), result, eng.state['phase'], flush=True)
            if result in ('paused', 'halted'):
                return 0
            time.sleep(cfg['run']['step_interval_seconds'])
    if args.command == 'status':
        return status(eng)
    rec = eng.state['jobs'].get(args.job) if eng.state else None
    if not rec:
        print(f'unknown job {args.job}', file=sys.stderr)
        return 1
    gitops.fetch()
    commit, results = jobs.fetch_results(args.job)
    if results is None:
        print('no results file yet')
        return 1
    job = json.loads(gitops.show(rec['pushed_commit'], f'evo/jobs/{args.job}.json'))
    problems, missing = verify.check_results(job, rec['pushed_commit'], results)
    print(json.dumps({'problems': problems, 'missing': missing}, indent=1))
    if args.command == 'spotcheck':
        sample = verify.spotcheck_sample(results['rows'], cfg, rec['generation'], args.job)
        print(json.dumps({'games': len(sample), 'mismatches': verify.spotcheck(sample)}, indent=1))
    return 1 if problems else 0


def status(eng):
    s = eng.state
    if s is None:
        print('no state yet: python -m evo init')
        return 0
    print(f"generation {s['generation']:02d}  phase {s['phase']}  sessions {s['sessions_launched']}"
          f"/{eng.cfg['agents']['max_sessions_total']}")
    if s['paused']:
        print(f"PAUSED: {s['paused']}")
    if s['halted']:
        print(f"HALTED: {s['halted']}")
    gen = s['generations'].get(str(s['generation']), {})
    for aid in gen.get('slots', []):
        a = s['agents'][aid]
        extra = a['reason'] or (a['branch'] or '')
        print(f"  {aid}  {a['kind']:<9} {a['status']:<12} {(a['operator'] or a['archetype'] or ''):<12} {extra[:80]}")
    for jid, j in s['jobs'].items():
        if j['generation'] == s['generation']:
            print(f"  job {jid}: {j['status']} (pushed {j['pushed_commit'][:10]})")
    if gen.get('ranking'):
        print('  ranking: ' + ', '.join(gen['ranking']))
    for line in s['log'][-8:]:
        print('  · ' + line)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
