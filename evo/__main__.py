"""Command line: python -m evo [--config FILE] <command> (PLAN §2, D5).

  bootstrap   create the venv, SDK, maps, starter bot and manifest (§1.1)
  manifest    rewrite evo/env/manifest.json and print its sha256
  maptags     derive evo/prompts/map_tags.json from the maps (D26)
  init        create the state with generation 0 (§12.4)
  approve GG  approve generation GG's sessions (§0.4); prints the maximum it approves
  unhalt --reason TEXT  clear a HALT after a human looked at it (§10.4)
  stop        emergency stop: no new sessions, retire the running ones and open them in the browser
  best        show the best bot so far and copy it to best-bot/<id>/
  resume --reason TEXT  clear a pause such as max_generations (not approvals)
  step        advance the current generation as far as it can, then exit (§5)
  run         call step every [run].step_interval_seconds until paused or halted
  status      print the phase, agents and jobs
  verify JOB  re-check a results file (no spot-check)
  spotcheck JOB  replay JOB's spot-check sample locally
"""
import argparse
from pathlib import Path
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
    sub.add_parser('unhalt', help='clear a HALT after a human review').add_argument('--reason', required=True)
    sub.add_parser('resume', help='clear a pause such as max_generations').add_argument('--reason', required=True)
    sub.add_parser('best', help='show the best bot so far and copy it to best-bot/<id>/')
    stop = sub.add_parser('stop', help='emergency stop: no new sessions, retire running ones, open them to stop')
    stop.add_argument('--reason', default='emergency stop')
    stop.add_argument('--no-message', action='store_true', help='do not queue a stop message into the sessions')
    stop.add_argument('--no-open', action='store_true', help='do not open the running sessions in the browser')
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
    if args.command == 'resume':
        eng.resume(args.reason)
        return 0
    if args.command == 'unhalt':
        eng.unhalt(args.reason)
        return 0
    if args.command == 'step':
        print(eng.step())
        return 0
    if args.command == 'run':
        return run(eng, cfg)
    if args.command == 'status':
        return status(eng)
    if args.command == 'best':
        return best(eng)
    if args.command == 'stop':
        return emergency_stop(eng, args)
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


def run(eng, cfg, redraw_seconds=10):
    """The interactive app: step, show progress, ask at approval gates, stop on a halt or at the end.

    Nothing is launched without a typed `y` at an approval prompt (§0.4). Ctrl+C stops it; starting it
    again resumes from evo/state.json.
    """
    from evo.orchestrator import progress
    os.system('')  # enable ANSI escapes in the Windows console
    actions = progress.Actions()
    log = []

    def draw(note=''):
        out = progress.render(eng, actions)
        if log:
            out += ['', 'This session:'] + ['  ' + x for x in log[-6:]]
        if note:
            out += ['', note]
        sys.stdout.write('\x1b[H\x1b[2J' + '\n'.join(out) + '\n')
        sys.stdout.flush()

    if eng.state is None:
        print('No run yet. `python -m evo init` creates generation 00 (no session is started).')
        return 1
    try:
        while True:
            draw('working…')
            result = eng.step()
            log.append(f"{time.strftime('%H:%M:%S')} step: {result} ({eng.state['phase']})")
            state = eng.state
            if state['halted']:
                draw('The run is HALTED. Nothing more will happen until a human looks at it.')
                return 1
            if state['paused']:
                g = state['generation']
                maxg = cfg['run']['max_generations']
                if 'max_generations' in state['paused'] and (not maxg or g + 1 < maxg):
                    eng.resume(f'max_generations is now {maxg or "unlimited"} in evo/config.toml')
                    log.append(f'resumed: max_generations is now {maxg or "unlimited"}')
                    continue
                if 'approve' in state['paused']:
                    _, terms = eng.approval_terms(g)
                    draw(f'APPROVAL NEEDED (uses your Claude subscription):\n  {terms}\n'
                         f'  Tasks: evo/tasks/e{g:02d}-*.md')
                    try:
                        answer = input(f'Approve generation {g:02d}? Type y to launch, anything else to stop: ')
                    except EOFError:  # no keyboard (e.g. started by another program): never approve
                        answer = ''
                    answer = answer.strip().lower()
                    if answer == 'y':
                        eng.approve(g)
                        log.append(f'approved generation {g:02d}')
                        continue
                    draw('Not approved. Nothing was launched; run it again when you want to continue.')
                    return 0
                draw(f"Stopped: {state['paused']}")
                return 0
            interval = cfg['run']['step_interval_seconds']
            deadline = time.time() + interval
            while time.time() < deadline:
                draw(f'next check in {int(deadline - time.time())} s  |  Ctrl+C stops (the run resumes when started again)')
                time.sleep(min(redraw_seconds, max(1, deadline - time.time())))
    except KeyboardInterrupt:
        print('\nStopped by you. Start it again to continue; nothing is lost.')
        return 0


def emergency_stop(eng, args):
    """`python -m evo stop`: see Engine.stop. The browser is the only immediate way to stop a cloud session."""
    import webbrowser
    if eng.state is None:
        print('No run.')
        return 1
    stopped = eng.stop(args.reason, message=not args.no_message)
    print('The run is STOPPED: no new sessions, no nudges. `python -m evo resume --reason "..."` continues it.')
    if not stopped:
        print('No cloud session was running.')
        return 0
    print(f'{len(stopped)} running session(s) retired (their work will not be admitted):')
    for aid, sid, url, sent in stopped:
        print(f"  {aid}  {url}  stop message {'queued' if sent else 'NOT queued'}")
    print('A queued message is read when the agent finishes its current step, so it can take a few minutes.')
    print('To stop a session at once, use the stop control on its page at claude.ai/code (opening them now).')
    if not args.no_open:
        for _, _, url, _ in stopped:
            webbrowser.open(url)
    return 0


def best(eng, target_root=None):
    """The #1 of the latest selected generation (final ranking, §11), copied to best-bot/<id>/."""
    import shutil
    from evo.orchestrator.config import ROOT
    s = eng.state
    gens = sorted((int(g), gen) for g, gen in (s or {}).get('generations', {}).items() if gen.get('survivors'))
    if not gens:
        print('No generation has been selected yet.')
        return 1
    g, gen = gens[-1]
    winner = gen['ranking'][0]
    print(f'Best bot: {winner}  (generation {g:02d}, final ranking #1; hall of fame {", ".join(s["hall_of_fame"])})')
    for i, b in enumerate(gen['ranking'][:5], 1):
        r = gen['ratings'][b]
        strength = 'DQ' if r['strength'] is None else f"{r['strength']:+.2f}"
        print(f"  {i}. {b:<8} {strength:>6}  90% [{r['interval'][0]:+.2f}, {r['interval'][1]:+.2f}]")
    src = ROOT / 'bots' / winner
    out = Path(target_root or ROOT / 'best-bot') / winner
    if out.exists():
        shutil.rmtree(out)
    shutil.copytree(src / 'src', out / 'src')
    if (src / 'NOTES.md').is_file():
        shutil.copy(src / 'NOTES.md', out / 'NOTES.md')
    print(f'Copied to {out} (src/ and NOTES.md). The same code is on main in bots/{winner}/.')
    print(f'To put it on the ladder, submit bots/{winner} yourself with cambc (see `cambc submit --help`); '
          'evo never touches the platform account.')
    return 0


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
