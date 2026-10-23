"""Terminal dashboard for `python -m evo run`: what the run is doing and when it will need you next.

Estimates come from this run's own history (session and job durations recorded in the state), with
fallbacks for the first generation. Actions shard progress is read from GitHub's public API at most
every two minutes (unauthenticated: 60 requests per hour), and simply left out when unavailable.
"""
import json
import math
import statistics
import time
import urllib.request

from evo.orchestrator import state as st

REPO = 'dawkaw0dy/battlecode-agent-evo'
DEFAULT_SESSION = 15 * 60   # gens 0–2: agents finished in 7–17 minutes
DEFAULT_JOB = 4 * 60        # an evaluation job: shards plus aggregation
DEFAULT_SPOT = 3 * 60       # a spot-check job on ubuntu-22.04
BAR = 24


def fmt_duration(seconds):
    seconds = max(0, int(seconds))
    h, rem = divmod(seconds, 3600)
    m, s = divmod(rem, 60)
    return f'{h}h {m:02d}m' if h else f'{m}m {s:02d}s'


def clock(ts):
    return time.strftime('%H:%M', time.localtime(ts))


def bar(fraction, width=BAR):
    fraction = min(1.0, max(0.0, fraction))
    filled = int(round(fraction * width))
    return '[' + '#' * filled + '-' * (width - filled) + ']'


def history(state):
    """Median durations observed in this run (seconds)."""
    sessions = [a['done_at'] - a['sessions'][-1]['launched_at'] for a in state['agents'].values()
                if a.get('done_at') and a.get('sessions')]
    jobs = [j['arrived_at'] - j['pushed_at'] for j in state['jobs'].values() if j.get('arrived_at')]
    spots = [j['spotcheck']['arrived_at'] - j['spotcheck']['pushed_at'] for j in state['jobs'].values()
             if isinstance(j.get('spotcheck'), dict) and j['spotcheck'].get('arrived_at')]
    med = lambda xs, d: statistics.median(xs) if xs else d
    return med(sessions, DEFAULT_SESSION), med(jobs, DEFAULT_JOB), med(spots, DEFAULT_SPOT)


class Actions:
    """Shard progress of the evo-eval run triggered by a commit, cached for two minutes."""

    def __init__(self, repo=REPO, ttl=120):
        self.repo, self.ttl, self.cache = repo, ttl, {}

    def _get(self, url):
        req = urllib.request.Request(url, headers={'Accept': 'application/vnd.github+json', 'User-Agent': 'evo'})
        with urllib.request.urlopen(req, timeout=10) as r:
            return json.load(r)

    def progress(self, sha):
        hit = self.cache.get(sha)
        if hit and time.time() - hit[0] < self.ttl:
            return hit[1]
        value = None
        try:
            runs = self._get(f'https://api.github.com/repos/{self.repo}/actions/runs?head_sha={sha}')['workflow_runs']
            run = next((r for r in runs if r['name'] == 'evo-eval'), None)
            if run:
                jobs = self._get(run['jobs_url'] + '?per_page=100')['jobs']
                shards = [j for j in jobs if j['name'].startswith('shard')]
                value = {'status': run['status'], 'done': sum(j['status'] == 'completed' for j in shards),
                         'total': len(shards), 'failed': sum(j['conclusion'] == 'failure' for j in shards)}
        except Exception:
            value = hit[1] if hit else None
        self.cache[sha] = (time.time(), value)
        return value


def stage_jobs(state, gen, stage):
    return [state['jobs'][j] | {'id': j} for j in gen.get('jobs', {}).get(stage, [])]


def stages_after(phase):
    """Evaluation stages still to come after the current one (Stage C is not predicted)."""
    return {'A': 1, 'B': 0, 'C': 0}.get(phase[5], 0) if phase.startswith('EVAL_') else 2


def render(eng, actions=None, now=None):
    """Lines describing the run right now."""
    s, cfg = eng.state, eng.cfg
    now = now or st.now()
    if s is None:
        return ['No run yet: `python -m evo init` creates generation 00.']
    g = s['generation']
    gen = s['generations'].get(str(g), {})
    session_s, job_s, spot_s = history(s)
    lines = [f"evo run  |  generation {g:02d}  |  phase {s['phase']}  |  {time.strftime('%H:%M:%S')}",
             f"sessions used {s['sessions_launched']} of {cfg['agents']['max_sessions_total']}  |  "
             f"hall of fame {', '.join(s['hall_of_fame']) or '-'}", '']
    if s['halted']:
        lines += [f"HALTED: {s['halted']}", '', 'Look at the problem, then `python -m evo unhalt --reason "…"`.']
        return lines
    eta = None
    if s['phase'] in ('RUNNING', 'TASKS_PUSHED', 'PLANNED'):
        lines.append('Agents')
        ends = []
        waiting = 0
        for aid in eng.new_agents():
            a = s['agents'][aid]
            if a['status'] == 'running':
                start = a['sessions'][-1]['launched_at']
                deadline = start + a['deadline_minutes'] * 60
                guess = max(now + 60, min(deadline, start + session_s))
                ends.append(guess)
                lines.append(f"  {aid}  running  {bar((now - start) / (deadline - start))} "
                             f"{fmt_duration(now - start)} of {a['deadline_minutes']}m  (likely done ~{clock(guess)})")
            else:
                if a['status'] == 'planned':
                    waiting += 1
                seen = (f"  (effort {a.get('effort_level') or 'default'}, model {a.get('model') or 'default'})"
                        if 'effort_level' in a else '')
                lines.append(f"  {aid}  {a['status']}{seen}")
        if s['phase'] == 'RUNNING':
            k = max(1, cfg['agents']['max_parallel'])
            sessions_end = max(ends, default=now) + math.ceil(waiting / k) * session_s
            eta = sessions_end + 2 * (job_s + spot_s) + 120
    if s['phase'].startswith('EVAL_') or s['phase'] in ('ADMITTED', 'SELECTED'):
        lines.append('Evaluation')
        for stage in ('A', 'B', 'C'):
            for job in stage_jobs(s, gen, stage):
                if job['status'] in ('verified',):
                    note = 'verified' + (f", spot-check {job['spotcheck']['games']} OK" if job.get('spotcheck') else '')
                elif job['status'] == 'arrived':
                    spot = job.get('spotcheck') or {}
                    note = 'results in; ' + ('spot-check running on Actions' if spot.get('status') == 'pending'
                                             else 'checking')
                    if spot.get('pushed_commit') and actions:
                        p = actions.progress(spot['pushed_commit'])
                        if p:
                            note += f"  {bar(p['done'] / max(1, p['total']), 12)} {p['done']}/{p['total']} shards"
                else:
                    note = f"playing on Actions for {fmt_duration(now - job['pushed_at'])}"
                    p = actions.progress(job['pushed_commit']) if actions else None
                    if p:
                        note += f"  {bar(p['done'] / max(1, p['total']), 12)} {p['done']}/{p['total']} shards"
                        if p['failed']:
                            note += f" ({p['failed']} failed)"
                lines.append(f"  stage {stage}  {job['id']:<14} {note}")
        if s['phase'].startswith('EVAL_'):
            pending = [j for st_ in ('A', 'B', 'C') for j in stage_jobs(s, gen, st_) if j['status'] == 'pending']
            started = pending[-1]['pushed_at'] if pending else now
            this_stage = max(now + 60, started + job_s) + spot_s if pending else now + spot_s
            eta = this_stage + stages_after(s['phase']) * (job_s + spot_s)
    if gen.get('ranking') and s['phase'] == 'SELECTED':
        lines.append('Ranking')
        for i, b in enumerate(gen['ranking'][:6], 1):
            r = gen['ratings'][b]
            strength = 'DQ' if r['strength'] is None else f"{r['strength']:+.2f}"
            mark = '  survivor' if b in (gen.get('survivors') or []) else ''
            lines.append(f"  {i}. {b:<8} {strength:>6}  [{r['interval'][0]:+.2f}, {r['interval'][1]:+.2f}]{mark}")
    lines.append('')
    if getattr(eng, 'blocked', None):
        lines.append(f'Waiting: {eng.blocked}  (fix it; the app re-checks every step)')
    if s['paused']:
        lines.append(f"Paused: {s['paused']}")
    elif eta:
        lines.append(f"Generation {g:02d} should be selected around {clock(eta)} (in about {fmt_duration(eta - now)}); "
                     f"then the next approval or the end of the run.")
    for line in s['log'][-5:]:
        lines.append('  - ' + line[:150])
    return lines
