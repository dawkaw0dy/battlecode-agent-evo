"""Generation state machine (PLAN §5). `step()` advances as far as it can and returns; it is idempotent.

Every phase change is saved to the state file, committed and pushed to main. Pushing a job file
is its own commit and push (D24), so the eval workflow sees exactly one new job per push.
"""
import json
import math
import re
from pathlib import Path

from evo import manifest
from evo.orchestrator import admit, collect, gitops, jobs, launch, maptags, mutate, rating, select, tasks, verify
from evo.orchestrator import state as st
from evo.orchestrator.config import ROOT, agent_id, job_id

TERMINAL = ('done', 'dead', 'rejected')


class Engine:
    def __init__(self, cfg, *, root=ROOT, launcher=None, replay=None, push=True, out=print):
        self.cfg, self.root, self.push_enabled, self.out = cfg, Path(root), push, out
        self.launcher = launcher or launch.make(cfg)
        self.replay = replay or verify.replay_locally
        self.state_path = self.root / cfg['run']['state']
        self.state = st.load(self.state_path)
        self.blocked = None  # why the current phase cannot proceed right now (not saved; re-checked each step)

    # ---------------------------------------------------------------- persistence
    def rel(self, p):
        return Path(p).relative_to(self.root).as_posix()

    def save(self, message, paths=()):
        st.save(self.state, self.state_path)
        sha = gitops.commit([*paths, self.rel(self.state_path)], message, self.root)
        if sha and self.push_enabled:
            gitops.push_main(self.root)
        return sha

    # ---------------------------------------------------------------- init / approve
    def init(self):
        if self.state is not None:
            self.out(f'state exists: {self.rel(self.state_path)} (generation {self.state["generation"]}, '
                     f'{self.state["phase"]})')
            return self.state
        self.state = st.empty()
        archetypes = sorted(p.stem for p in (self.root / self.cfg['run']['archetypes_dir']).glob('*.md'))
        n = self.cfg['population']
        if len(archetypes) < n + 2:
            raise SystemExit(f'need at least n + 2 = {n + 2} archetypes, found {len(archetypes)} (D4)')
        taken = []
        for slot in range(n):
            rng = mutate.slot_rng(self.cfg['seed'], 0, slot)
            arche = mutate.choose_archetype(rng, archetypes, recent=[], taken=taken)
            taken.append(arche)
            self.add_agent(0, slot, kind='immigrant', parents=['starter'], archetype=arche,
                           deadline=math.ceil(1.5 * self.cfg['agents']['deadline_minutes']))
        st.log(self.state, f'initialised generation 0 with {n} immigrants')
        self.save('evo: init generation 00')
        return self.state

    def add_agent(self, g, slot, *, kind, parents, deadline, archetype=None, operator=None, focus=None,
                  evidence=None, failed_ideas=None):
        aid = agent_id(self.cfg, g, slot)
        self.state['agents'][aid] = {
            'id': aid, 'generation': g, 'slot': slot, 'kind': kind, 'parents': parents, 'operator': operator,
            'focus': focus, 'archetype': archetype, 'evidence': evidence or [], 'failed_ideas': failed_ideas or [],
            'rng_key': f"{self.cfg['seed']}:{g}:{slot}", 'deadline_minutes': deadline, 'task_sha256': None,
            'sessions': [], 'nudged': [], 'status': 'planned', 'branch': None, 'commit': None,
            'src_sha256': None, 'reason': None}
        st.gen(self.state, g)['slots'].append(aid)
        return aid

    def new_agents(self, g=None):
        gen = st.gen(self.state, g)
        return [a for a in gen['slots'] if a not in gen.get('elites', [])]

    def approval_terms(self, g):
        """(approval dict, one-line text) for generation g, without recording anything."""
        agents = [self.state['agents'][a] for a in self.new_agents(g)]
        r = self.cfg['agents']['relaunch_dead']
        deadline = max((a['deadline_minutes'] for a in agents), default=0)
        approval = {'sessions': len(agents), 'relaunches_per_slot': r, 'nudges_per_session': 1,
                    'deadline_minutes': deadline, 'max_sessions_total': self.cfg['agents']['max_sessions_total'],
                    'approved_at': st.now()}
        text = (f'generation {g:02d}: at most {len(agents)} sessions, up to {r} relaunch(es) per slot '
                f'(max {len(agents) * r} more), 1 nudge per session, deadline {deadline} min each. '
                f'Run total so far {self.state["sessions_launched"]} of {approval["max_sessions_total"]}.')
        return approval, text

    def approve(self, g):
        if self.state is None:
            raise SystemExit('no state; run `python -m evo init` first')
        gen = st.gen(self.state, g)
        approval, text = self.approval_terms(g)
        self.out('Approving ' + text)
        gen['approval'] = approval
        if self.state['paused'] and 'approve' in self.state['paused']:
            self.state['paused'] = None
        st.log(self.state, f'approved generation {g:02d}: {approval}')
        self.save(f'evo: approve generation {g:02d}')

    def unhalt(self, reason):
        """Clear a HALT after a human looked at it (§10.4); the next step redoes the halted phase's checks."""
        if not self.state or not self.state['halted']:
            self.out('not halted')
            return
        st.log(self.state, f"halt cleared ({reason}); was: {self.state['halted'][:300]}")
        self.state['halted'] = None
        self.save(f'evo: halt cleared: {reason[:60]}')

    # ---------------------------------------------------------------- step
    def step(self, max_transitions=20):
        if self.state is None:
            raise SystemExit('no state; run `python -m evo init` first')
        for _ in range(max_transitions):
            if self.state['halted']:
                self.out(f"HALTED: {self.state['halted']}")
                return 'halted'
            if self.state['paused']:
                self.out(f"paused: {self.state['paused']}")
                return 'paused'
            before = (self.state['generation'], self.state['phase'])
            getattr(self, 'phase_' + self.state['phase'].lower())()
            if self.state['halted'] or self.state['paused']:
                continue
            if (self.state['generation'], self.state['phase']) == before:
                return 'waiting'
        return 'waiting'

    # ---------------------------------------------------------------- phases
    def phase_planned(self):
        g = self.state['generation']
        paths = tasks.write_all(self.cfg, self.state, self.new_agents(), self.root)
        st.set_phase(self.state, 'TASKS_PUSHED', f'{len(paths)} task files')
        self.save(f'evo: tasks generation {g:02d}', paths)

    def launch_preconditions(self):
        problems = []
        if gitops.dirty(self.root):
            problems.append('working tree is not clean')
        if self.push_enabled and not gitops.in_sync_with_origin(self.root):
            problems.append('local main is not equal to origin/main')
        expected = json.loads((self.root / manifest.MANIFEST).read_text(encoding='utf-8'))
        diffs = manifest.differences(expected, manifest.compute(self.root, expected['runner_version']))
        if diffs:
            problems.append('manifest out of date: ' + '; '.join(diffs[:5]))
        return problems

    def phase_tasks_pushed(self):
        g = self.state['generation']
        problems = self.launch_preconditions()
        if problems:
            # Not a pause: an uncommitted file or an unpushed commit is fixed by a human and then simply
            # re-checked on the next step (the app shows the reason meanwhile).
            self.blocked = 'cannot launch yet: ' + '; '.join(problems)
            return None
        self.blocked = None
        if self.cfg['agents']['require_approval'] and not st.gen(self.state)['approval']:
            st.pause(self.state, f'waiting for approval: python -m evo approve {g:02d}')
            return self.save(f'evo: generation {g:02d} waits for approval')
        st.set_phase(self.state, 'RUNNING')
        self.save(f'evo: generation {g:02d} running')

    def budget(self, agent):
        """Can this agent's next session start within the approval and the run cap?"""
        gen = st.gen(self.state)
        approval = gen['approval'] or {'sessions': len(self.new_agents()),
                                       'relaunches_per_slot': self.cfg['agents']['relaunch_dead'],
                                       'nudges_per_session': 1}
        if self.state['sessions_launched'] >= self.cfg['agents']['max_sessions_total']:
            return False, 'max_sessions_total reached'
        attempts = len(agent['sessions'])
        if attempts == 0:
            first = sum(1 for a in self.new_agents() if self.state['agents'][a]['sessions'])
            return (first < approval['sessions'], 'approved sessions used up')
        return (attempts - 1 < approval['relaunches_per_slot'], 'no relaunch left in the approval')

    def phase_running(self):
        g = self.state['generation']
        gen = st.gen(self.state)
        if self.push_enabled:
            gitops.fetch(self.root)
        changed = False
        now = st.now()
        for aid in self.new_agents():
            a = self.state['agents'][aid]
            if a['status'] != 'running':
                continue
            branch, commit, marker = collect.find_marker(a, self.root)
            session = a['sessions'][-1]
            if branch:
                if marker.get('env_failed'):
                    gen['env_failed'] += 1
                    st.log(self.state, f'{aid}: env_failed in {session["id"]}')
                    if gen['env_failed'] >= 2:
                        st.pause(self.state, 'two env_failed markers this generation: the environment is broken')
                    self.retire(a, 'env_failed')
                else:
                    a.update(status='done', branch=branch, commit=commit, done_at=st.now())
                    st.log(self.state, f'{aid}: done on {branch} at {commit[:10]}')
                changed = True
            elif now > session['launched_at'] + a['deadline_minutes'] * 60:
                self.retire(a, 'deadline')
                changed = True
            elif now > session['launched_at'] + (a['deadline_minutes'] - 20) * 60 and session['id'] not in a['nudged']:
                if self.launcher.nudge(session['id'], '20 minutes left: finish, write NOTES.md and DONE.json, push.'):
                    a['nudged'].append(session['id'])
                    changed = True
        running = sum(1 for x in self.new_agents() if self.state['agents'][x]['status'] == 'running')
        for aid in self.new_agents():
            a = self.state['agents'][aid]
            if a['status'] != 'planned' or running >= self.cfg['agents']['max_parallel'] or self.state['paused']:
                continue
            ok, why = self.budget(a)
            if not ok:
                a.update(status='dead', reason=why)
                changed = True
                continue
            if self.push_enabled and not gitops.in_sync_with_origin(self.root):
                st.pause(self.state, 'local main differs from origin/main; not launching')
                break
            try:
                info = self.launcher.launch(aid, len(a['sessions']) + 1, tasks.prompt(aid))
            except launch.LaunchError as exc:
                self.state['launch_errors_in_a_row'] += 1
                st.log(self.state, f'{aid}: launch failed: {exc}')
                if self.state['launch_errors_in_a_row'] >= 3:
                    st.pause(self.state, f'3 launch errors in a row (last: {exc})')
                changed = True
                break
            self.state['launch_errors_in_a_row'] = 0
            self.state['sessions_launched'] += 1
            a['sessions'].append({'id': info['id'], 'url': info.get('url'), 'launched_at': st.now(),
                                  'attempt': len(a['sessions']) + 1, 'base_commit': gitops.head(self.root),
                                  'launch_output': info.get('output')})
            a['status'] = 'running'
            running += 1
            changed = True
            st.log(self.state, f"{aid}: launched {info['id']}")
        if all(self.state['agents'][x]['status'] in TERMINAL for x in self.new_agents()):
            st.set_phase(self.state, 'ADMITTED')
            changed = True
        if changed:
            self.save(f'evo: generation {g:02d} sessions')

    def retire(self, a, why):
        ok, _ = self.budget(a)
        if ok and not self.state['paused']:
            a.update(status='planned', reason=f'relaunch after {why}')
        else:
            a.update(status='dead', reason=why)
        st.log(self.state, f"{a['id']}: {a['status']} ({why})")

    def phase_admitted(self):
        g = self.state['generation']
        paths = []
        for aid in self.new_agents():
            a = self.state['agents'][aid]
            if a['status'] != 'done':
                continue
            if self.push_enabled:
                gitops.fetch(self.root)
            problems, extracted = admit.check(aid, a['commit'], self.root)
            if problems:
                a.update(status='rejected', reason='; '.join(problems)[:2000])
                st.log(self.state, f'{aid}: rejected: {a["reason"][:200]}')
                continue
            copied, src = admit.copy_in(aid, extracted, a['commit'], self.root)
            paths += copied
            a.update(status='admitted', src_sha256=src)
            st.log(self.state, f'{aid}: admitted ({src[:12]})')
        manifest.write(self.root, jobs_runner(self.root))
        paths.append(self.rel(self.root / manifest.MANIFEST))
        members = [x for x in st.gen(self.state)['slots'] if self.state['agents'][x]['status'] == 'admitted']
        st.gen(self.state)['members'] = members
        s = select.survivor_count(self.cfg['population'], self.cfg['keep_fraction'])
        self.save(f'evo: admit generation {g:02d}', paths)
        if len(members) < s + 1:
            st.pause(self.state, f'only {len(members)} admitted bots (need {s + 1}); not evaluating')
            return self.save(f'evo: generation {g:02d} too small')
        st.set_phase(self.state, 'EVAL_A_PENDING')
        self.save(f'evo: generation {g:02d} stage A')

    # ---------------------------------------------------------------- evaluation
    def stage_plan(self, stage):
        gen = st.gen(self.state)
        key = f'plan_{stage}'
        if key in gen:
            return gen[key]
        g = self.state['generation']
        members = gen['members']
        panel = jobs.panel(self.cfg, self.state['hall_of_fame'])
        round_robin = len(self.state['hall_of_fame']) < 2
        maps_a = jobs.stage_a_maps(self.cfg, g, self.root)
        heldout_b, heldout_c = jobs.heldout_split(self.cfg, g, self.root)
        if stage == 'A':
            pairs = jobs.stage_a_pairs(members, panel, maps_a, round_robin)
        elif stage == 'B':
            order = [b for b in gen['ranking_A'] if b in members and not gen['ratings_A'][b]['disqualified']]
            top = order[:self.cfg['eval']['stage_b_top']]
            gen['top'] = top
            pairs = jobs.stage_b_pairs(top, panel, jobs.all_maps(self.root) + heldout_b, maps_a)
        else:  # C: the two bots at the survivor boundary, on held-out maps not used yet (§11)
            x, y = gen['stage_c_bots']
            pairs = jobs.dedupe_pairs([((x, y), heldout_c)] + [((b, p), heldout_c) for b in (x, y)
                                                               for p in panel if p not in (x, y)])
        gen[key] = [[list(p), m] for p, m in pairs]
        return gen[key]

    def pairs(self, stage):
        return [((p[0], p[1]), m) for p, m in self.stage_plan(stage)]

    def bots_of(self, pairs):
        return sorted({b for (a, b2), _ in pairs for b in (a, b2)})

    def phase_eval_a_pending(self):
        self.eval_pending('A')

    def phase_eval_b_pending(self):
        self.eval_pending('B')

    def eval_pending(self, stage):
        g = self.state['generation']
        gen = st.gen(self.state)
        pending = [j for j in gen['jobs'].setdefault(stage, []) if self.state['jobs'][j]['status'] == 'pending']
        if not pending:
            pairs = self.pairs(stage)
            srcs = jobs.srcs_for(self.bots_of(pairs), self.root)
            maps = sorted({m for _, ms in pairs for m in ms})
            shas = jobs.map_shas(maps, self.root)
            rv = jobs_runner(self.root)
            discarded = {tuple(d) for d in gen.get(f'discarded_{stage}', [])}
            units = [(a, b, [m for m in ms if (a, b, m) not in discarded])
                     for a, b, ms in jobs.units_needed(pairs, srcs, shas, self.cache(), rv)]
            units = [u for u in units if u[2]]
            if not units:
                st.set_phase(self.state, f'EVAL_{stage}_DONE', 'all games cached')
                return self.save(f'evo: generation {g:02d} stage {stage} complete')
            jid = job_id(self.cfg, g, stage, gen.get(f'retries_{stage}', 0))
            self.push_job(jid, units, stage)
            return
        jid = pending[-1]
        rec = self.state['jobs'][jid]
        if self.push_enabled:
            gitops.fetch(self.root)
        commit, results = jobs.fetch_results(jid, self.root)
        if results and results.get('trigger_sha') == rec['pushed_commit']:
            rec.update(status='arrived', results_commit=commit, arrived_at=st.now())
            st.set_phase(self.state, f'EVAL_{stage}_DONE', f'results for {jid}')
            return self.save(f'evo: results {jid} arrived')
        if st.now() - rec['pushed_at'] > self.cfg['run']['results_timeout_hours'] * 3600:
            if rec.get('repushed'):
                st.pause(self.state, f'no results for {jid} after two pushes')
                return self.save(f'evo: {jid} timed out')
            rec['repushed'] = True
            rec['status'] = 'abandoned'
            gen[f'retries_{stage}'] = gen.get(f'retries_{stage}', 0) + 1
            st.log(self.state, f'{jid}: no results after the timeout; re-queued')
            self.save(f'evo: {jid} timed out, re-queue')

    def push_job(self, jid, units, stage):
        g = self.state['generation']
        rel, job = jobs.write_job(self.cfg, jid, units, g, stage, self.root)
        sha = gitops.commit([rel], f'evo: job {jid} ({sum(2 * len(s["maps"]) for s in job["shards"])} games)',
                            self.root)
        if self.push_enabled:
            gitops.push_main(self.root)
        self.state['jobs'][jid] = {'generation': g, 'stage': stage, 'pushed_commit': sha, 'pushed_at': st.now(),
                                   'status': 'pending', 'retries': st.gen(self.state).get(f'retries_{stage}', 0)}
        st.gen(self.state)['jobs'].setdefault(stage, []).append(jid)
        st.log(self.state, f'{jid}: pushed {sha[:10]} with {len(job["shards"])} shards')
        self.save(f'evo: job {jid} pushed')

    def phase_eval_a_done(self):
        self.eval_done('A')

    def phase_eval_b_done(self):
        self.eval_done('B')

    def phase_eval_c_pending(self):
        self.eval_pending('C')

    def phase_eval_c_done(self):
        self.eval_done('C')

    def eval_done(self, stage):
        g = self.state['generation']
        gen = st.gen(self.state)
        for jid in gen['jobs'].setdefault(stage, []):
            rec = self.state['jobs'][jid]
            if rec['status'] != 'arrived':
                continue
            job = json.loads(gitops.show(rec['pushed_commit'], f'evo/jobs/{jid}.json', self.root))
            results = json.loads(gitops.show(rec['results_commit'], f'results/{jid}.json', self.root))
            problems, missing = verify.check_results(job, rec['pushed_commit'], results, self.root)
            if problems:
                st.halt(self.state, f'{jid}: results failed verification: ' + '; '.join(problems[:5]))
                return self.save(f'evo: HALTED on {jid}')
            sample = verify.spotcheck_sample(results['rows'], self.cfg, g, jid)
            if self.cfg['eval'].get('spotcheck_where', 'actions') == 'actions':
                mismatches = self.spotcheck_on_actions(jid, rec, job, sample)
                if mismatches is None:
                    return  # the spot-check job is pushed or still running
            else:
                self.out(f'{jid}: spot-checking {len(sample)} games locally…')
                mismatches = verify.spotcheck(sample, self.root, self.replay)
                rec['spotcheck'] = {'games': len(sample), 'mismatches': mismatches}
            if mismatches:
                st.halt(self.state, f'{jid}: spot-check mismatch: ' + '; '.join(mismatches[:3]))
                return self.save(f'evo: HALTED on {jid} spot-check')
            self.cache().add(results['rows'], job['runner_version'])
            rec['status'] = 'verified'
            if missing:
                retries = gen.get(f'retries_{stage}', 0)
                if retries < 2:
                    gen[f'retries_{stage}'] = retries + 1
                    st.log(self.state, f'{jid}: {len(missing)} games missing; re-queued')
                else:
                    gen.setdefault(f'discarded_{stage}', []).extend([[a, b, m] for a, b, m, _ in missing])
                    st.log(self.state, f'{jid}: {len(missing)} games discarded after 2 retries')
                st.set_phase(self.state, f'EVAL_{stage}_PENDING', 'missing games')
                return self.save(f'evo: {jid} verified, games missing', [self.rel(path_in(self.root, self.cfg, 'cache'))])
            st.log(self.state, f'{jid}: verified, {len(results["rows"])} rows, spot-check {len(sample)} OK')
        games = self.games({'A': ['A'], 'B': ['A', 'B'], 'C': ['A', 'B', 'C']}[stage])
        bots = gen['members'] + [p for p in jobs.panel(self.cfg, self.state['hall_of_fame']) if p not in gen['members']]
        ratings, order = rating.rate(games, bots, max_fault_rate=self.cfg['eval']['max_fault_rate'],
                                     seed=f"{self.cfg['seed']}:{g}:{stage}",
                                     replicates=self.cfg['run']['bootstrap_replicates'],
                                     exceptions_disqualify=self.cfg['eval'].get('exceptions_disqualify', False))
        gen[f'ratings_{stage}'] = ratings_json(ratings)
        gen[f'ranking_{stage}'] = order
        if stage == 'A':
            st.set_phase(self.state, 'EVAL_B_PENDING', 'stage A ranked: ' + ', '.join(order))
        else:
            gen['ratings'], gen['ranking'] = gen[f'ratings_{stage}'], order
            boundary = self.stage_c_boundary(ratings, order, games) if stage == 'B' else None
            if boundary:
                gen['stage_c_bots'] = boundary
                st.set_phase(self.state, 'EVAL_C_PENDING', f'stage C for {boundary[0]} vs {boundary[1]}')
            else:
                st.set_phase(self.state, 'SELECTED', 'final: ' + ', '.join(order))
        self.save(f'evo: generation {g:02d} stage {stage} rated', [self.rel(path_in(self.root, self.cfg, 'cache'))])

    def spotcheck_on_actions(self, jid, rec, job, sample):
        """D35: replay the seeded sample as job <jid>-spot on another runner image.

        Returns None while the spot job is being pushed or played, else the list of mismatches.
        """
        spot = rec.get('spotcheck') or {}
        sid = f'{jid}-spot'
        if 'job' not in spot:
            cells = {}
            for row in sample:
                cells.setdefault((row['a'], row['b']), set()).add(row['map'])
            units = [(a, b, sorted(ms)) for (a, b), ms in sorted(cells.items())]
            rel, sjob = jobs.write_job(self.cfg, sid, units, self.state['generation'], 'spot', self.root,
                                       runs_on=self.cfg['eval'].get('spotcheck_runs_on', 'ubuntu-22.04'))
            changed = [b for b, h in sjob['src_sha256'].items() if job['src_sha256'].get(b) != h]
            if changed:
                return [f'bot sources changed since {jid} was played: {changed}']
            sha = gitops.commit([rel], f'evo: spot-check job {sid} ({len(sample)} sampled games)', self.root)
            if self.push_enabled:
                gitops.push_main(self.root)
            rec['spotcheck'] = {'job': sid, 'games': len(sample), 'pushed_commit': sha, 'pushed_at': st.now(),
                                'status': 'pending'}
            st.log(self.state, f'{sid}: pushed {sha[:10]} ({len(sample)} sampled games, {len(sjob["shards"])} shards)')
            self.save(f'evo: spot-check {sid} pushed')
            return None
        if self.push_enabled:
            gitops.fetch(self.root)
        commit, results = jobs.fetch_results(sid, self.root)
        if not results or results.get('trigger_sha') != spot['pushed_commit']:
            if st.now() - spot['pushed_at'] > self.cfg['run']['results_timeout_hours'] * 3600:
                st.pause(self.state, f'no spot-check results for {sid}')
                self.save(f'evo: {sid} timed out')
            return None
        sjob = json.loads(gitops.show(spot['pushed_commit'], f'evo/jobs/{sid}.json', self.root))
        problems, missing = verify.check_results(sjob, spot['pushed_commit'], results, self.root)
        replayed = {(r['a'], r['b'], r['map'], r['side']): (r['outcome'], r.get('gems'), r.get('rounds'))
                    for r in results['rows']}
        mismatches = [f'{sid}: {p}' for p in problems]
        for row in sample:
            key = (row['a'], row['b'], row['map'], row['side'])
            want, got = (row['outcome'], row['gems'], row['rounds']), replayed.get(key)
            if got != want:
                mismatches.append(f'{key}: shard {want}, spot-check {got}')
        spot.update(status='done', results_commit=commit, mismatches=mismatches, arrived_at=st.now(),
                    runner=sjob.get('runs_on'), missing=len(missing))
        return mismatches

    def stage_c_boundary(self, ratings, order, games):
        """§11 Stage C: the last survivor and the first non-survivor, if their intervals overlap and
        unused held-out maps exist; else None."""
        g = self.state['generation']
        _, heldout_c = jobs.heldout_split(self.cfg, g, self.root)
        if not heldout_c:
            return None
        members = st.gen(self.state)['members']
        ranked = [b for b in order if b in members and not ratings[b]['disqualified']]
        s = select.survivor_count(self.cfg['population'], self.cfg['keep_fraction'])
        chosen, _ = select.survivors(ranked, ratings, games, s)
        rest = [b for b in ranked if b not in chosen]
        if len(chosen) < s or not rest:
            return None
        last, first_out = chosen[-1], rest[0]
        (lo1, hi1), (lo2, hi2) = ratings[last]['interval'], ratings[first_out]['interval']
        return [last, first_out] if lo1 <= hi2 and lo2 <= hi1 else None

    def cache(self):
        return jobs.Cache(path_in(self.root, self.cfg, 'cache'))

    def games(self, stages):
        pairs = [p for s in stages for p in self.pairs(s)]
        srcs = jobs.srcs_for(self.bots_of(pairs), self.root)
        shas = jobs.map_shas(sorted({m for _, ms in pairs for m in ms}), self.root)
        seen, out = set(), []
        for game in self.cache().games(pairs, srcs, shas, jobs_runner(self.root)):
            key = (game['A'], game['B'], game['map'])
            if key not in seen:
                seen.add(key)
                out.append(game)
        return out

    # ---------------------------------------------------------------- selection
    def phase_selected(self):
        g = self.state['generation']
        gen = st.gen(self.state)
        games = self.games(['A', 'B', 'C'] if 'plan_C' in gen else ['A', 'B'])
        members = gen['members']
        if gen.get('selection_done'):  # resumed after a max_generations pause: only plan the next generation
            if self.max_generations_reached(g):
                return self.save(f'evo: generation {g:02d} selected (run complete)')
            self.plan_next(gen['survivors'], games, members)
            return self.save(f'evo: generation {g + 1:02d} planned')
        ratings = {b: dict(r, interval=tuple(r['interval'])) for b, r in gen['ratings'].items()}
        for b in ratings:
            if ratings[b]['strength'] is None:
                ratings[b]['strength'] = -math.inf
        order = [b for b in gen['ranking'] if b in members]
        s = select.survivor_count(self.cfg['population'], self.cfg['keep_fraction'])
        survivors, skipped = select.survivors(order, ratings, games, s)
        gen['survivors'], gen['similar_skipped'] = survivors, skipped
        if survivors and survivors[0] not in self.state['hall_of_fame']:
            self.state['hall_of_fame'].append(survivors[0])
        lineage = []
        for b in members:
            a = self.state['agents'][b]
            r = ratings[b]
            a['status'] = 'disqualified' if r['disqualified'] else 'rated'
            lineage.append({'agent': b, 'generation': g, 'kind': a['kind'], 'parents': a['parents'],
                            'operator': a['operator'], 'focus': a['focus'], 'archetype': a['archetype'],
                            'rng_key': a['rng_key'], 'status': a['status'],
                            'strength': None if math.isinf(r['strength']) else r['strength'],
                            'interval': list(r['interval']), 'fault_rate': r['fault_rate'],
                            'elite': b in gen.get('elites', []), 'survived': b in survivors,
                            'rank': order.index(b) + 1})
        for aid in st.gen(self.state)['slots']:
            a = self.state['agents'][aid]
            if aid not in members:
                lineage.append({'agent': aid, 'generation': g, 'kind': a['kind'], 'parents': a['parents'],
                                'operator': a['operator'], 'focus': a['focus'], 'archetype': a['archetype'],
                                'rng_key': a['rng_key'], 'status': a['status'], 'reason': a['reason']})
        lin = path_in(self.root, self.cfg, 'lineage')
        lin.parent.mkdir(parents=True, exist_ok=True)
        with lin.open('a', encoding='utf-8', newline='\n') as f:
            for row in lineage:
                f.write(json.dumps(row, sort_keys=True) + '\n')
        st.log(self.state, f'survivors {survivors}, hall of fame {self.state["hall_of_fame"]}')
        gen['selection_done'] = True
        if self.max_generations_reached(g):
            return self.save(f'evo: generation {g:02d} selected (run complete)', [self.rel(lin)])
        self.plan_next(survivors, games, members)
        self.save(f'evo: generation {g:02d} selected, generation {g + 1:02d} planned', [self.rel(lin)])

    def max_generations_reached(self, g):
        maxg = self.cfg['run']['max_generations']
        if maxg and g + 1 >= maxg:
            st.pause(self.state, f'generation {g:02d} selected; max_generations={maxg} reached')
            return True
        return False

    def resume(self, reason):
        """Clear a pause (for example max_generations, after raising it). Approval pauses use `approve`."""
        if not self.state or not self.state['paused']:
            self.out('not paused')
            return
        if 'approve' in self.state['paused']:
            raise SystemExit(f"paused for approval: use `python -m evo approve` ({self.state['paused']})")
        gen = st.gen(self.state)
        if self.state['phase'] == 'SELECTED' and gen.get('survivors') is not None:
            gen['selection_done'] = True  # generations selected before this flag existed
        st.log(self.state, f"resumed ({reason}); was paused: {self.state['paused']}")
        self.state['paused'] = None
        self.save(f'evo: resumed: {reason[:60]}')

    def plan_next(self, survivors, games, members):
        g = self.state['generation'] + 1
        cfg = self.cfg
        elites = survivors[:cfg['elites']]
        children = cfg['population'] - len(elites) - cfg['immigrants']
        alloc = select.allocation(survivors, children)
        tags = maptags.load(cfg)
        failed = self.failed_ideas([m for m in members if m not in survivors])
        gen = st.gen(self.state, g)
        gen['elites'] = list(elites)
        for e in elites:
            gen['slots'].append(e)
            self.state['agents'][e]['status'] = 'admitted'
        slot = len(elites)
        weights = cfg['mutation']['operator_weights']
        for parent, count in zip(survivors, alloc):
            for _ in range(count):
                rng = mutate.slot_rng(cfg['seed'], g, slot)
                op = mutate.weighted_choice(rng, weights)
                parents = [parent]
                if op == 'crossover':
                    partner = mutate.crossover_partner(parent, survivors, games)
                    if partner is None:
                        op = 'subsystem'
                    else:
                        parents.append(partner)
                ev = mutate.evidence(parent, games, cfg['mutation']['evidence_losses'])
                focus = mutate.choose_focus(rng, cfg['mutation']['focus'], cfg['mutation']['focus_tags'], tags, ev,
                                            self.state['agents'][parent].get('focus'))
                self.add_agent(g, slot, kind='child', parents=parents, operator=op, focus=focus, evidence=ev,
                               failed_ideas=failed, deadline=cfg['agents']['deadline_minutes'])
                slot += 1
        archetypes = sorted(p.stem for p in (self.root / cfg['run']['archetypes_dir']).glob('*.md'))
        recent = [a['archetype'] for a in self.state['agents'].values()
                  if a['kind'] == 'immigrant' and g - 2 <= a['generation'] < g]
        taken = []
        for _ in range(cfg['immigrants']):
            rng = mutate.slot_rng(cfg['seed'], g, slot)
            arche = mutate.choose_archetype(rng, archetypes, recent, taken)
            taken.append(arche)
            base = ['starter']
            if cfg['mutation']['immigrant_base'] == 'hall_of_fame' and g >= 2 and self.state['hall_of_fame']:
                base = [self.state['hall_of_fame'][-1]]
            self.add_agent(g, slot, kind='immigrant', parents=base, archetype=arche, failed_ideas=failed,
                           deadline=cfg['agents']['deadline_minutes'])
            slot += 1
        self.state['generation'] = g
        st.set_phase(self.state, 'PLANNED', f'{len(gen["slots"])} slots')

    def failed_ideas(self, losers, limit=15):
        ideas = []
        for b in losers:
            notes = self.root / 'bots' / b / 'NOTES.md'
            if not notes.is_file():
                continue
            m = re.search(r'^##\s*Tried and failed.*?$(.*?)(?=^##\s|\Z)', notes.read_text(encoding='utf-8'),
                          re.M | re.S)
            if m:
                for line in m.group(1).splitlines():
                    line = line.strip().lstrip('-*').strip()
                    if line:
                        ideas.append(f'{b}: {line}'[:300])
        return ideas[:limit]


def ratings_json(ratings):
    return {b: {**r, 'strength': None if math.isinf(r['strength']) else r['strength'],
                'interval': list(r['interval'])} for b, r in ratings.items()}


def jobs_runner(root):
    return json.loads((Path(root) / manifest.MANIFEST).read_text(encoding='utf-8'))['runner_version']


def path_in(root, cfg, key):
    return Path(root) / cfg['run'][key]
