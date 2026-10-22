"""Evaluation jobs and the result cache (PLAN §4, §9, §11 stages).

A game is identified by (src_sha256 of team A, src_sha256 of team B, map_sha256, runner_version).
Games are deterministic (M0 item 8), so a cached game is never played again.
"""
import hashlib
import json
import random
from pathlib import Path

from evo.eval import jobfile
from evo.eval.digest import digest
from evo.orchestrator import gitops
from evo.orchestrator.config import ROOT, path


def all_maps(root=ROOT):
    return sorted(p.stem for p in (Path(root) / 'maps').glob('*.map26a'))


def stage_a_maps(cfg, generation, root=ROOT):
    """Rotating subset chosen by the generation seed (§3)."""
    maps = all_maps(root)
    rng = random.Random(int(hashlib.sha256(f"{cfg['seed']}:{generation}:stage-a".encode()).hexdigest(), 16))
    k = min(cfg['eval']['stage_a_maps'], len(maps))
    return sorted(rng.sample(maps, k))


def game_key(row, runner_version):
    a_team = row['a'] if row['side'] == 'A' else row['b']
    b_team = row['b'] if row['side'] == 'A' else row['a']
    return (row['src_sha256'][a_team], row['src_sha256'][b_team], row['map_sha256'], runner_version)


class Cache:
    """evo/cache.jsonl: one verified result row per line, plus the runner version it was played with."""

    def __init__(self, file):
        self.file = Path(file)
        self.rows = {}
        if self.file.exists():
            for line in self.file.read_text(encoding='utf-8').splitlines():
                if line.strip():
                    entry = json.loads(line)
                    self.rows[tuple(entry['key'])] = entry['row']

    def add(self, rows, runner_version):
        new = []
        for row in rows:
            key = game_key(row, runner_version)
            if key not in self.rows:
                self.rows[key] = row
                new.append({'key': list(key), 'row': row})
        if new:
            self.file.parent.mkdir(parents=True, exist_ok=True)
            with self.file.open('a', encoding='utf-8', newline='\n') as f:
                for entry in new:
                    f.write(json.dumps(entry, sort_keys=True) + '\n')

    def has(self, src_a, src_b, map_sha, runner_version):
        return (src_a, src_b, map_sha, runner_version) in self.rows

    def game(self, team_a, team_b, map_name, srcs, map_shas, runner_version):
        """The cached game with `team_a` on team A, named with current bot names, or None."""
        row = self.rows.get((srcs[team_a], srcs[team_b], map_shas[map_name], runner_version))
        if row is None:
            return None
        failed_srcs = {row['src_sha256'][b] for b in row.get('build_failed', []) if b in row['src_sha256']}
        return {'A': team_a, 'B': team_b, 'map': map_name, 'outcome': row['outcome'],
                'faults': row.get('faults', {'teamA': 0, 'teamB': 0}),
                'bot_errors': row.get('bot_errors', {'teamA': 0, 'teamB': 0}),
                'build_failed': [b for b in (team_a, team_b) if srcs[b] in failed_srcs],
                'gems': row.get('gems', {'teamA': 0, 'teamB': 0}), 'rounds': row.get('rounds')}

    def games(self, pairs_maps, srcs, map_shas, runner_version):
        """Both sides of every requested (pair, map) cell that is cached."""
        out = []
        for (a, b), maps in pairs_maps:
            for m in maps:
                for x, y in ((a, b), (b, a)):
                    g = self.game(x, y, m, srcs, map_shas, runner_version)
                    if g is not None:
                        out.append(g)
        return out


def units_needed(pairs_maps, srcs, map_shas, cache, runner_version):
    """(a, b, maps) units for the (pair, map) cells whose two games are not both cached."""
    units = []
    for (a, b), maps in pairs_maps:
        need = [m for m in maps
                if not (cache.has(srcs[a], srcs[b], map_shas[m], runner_version)
                        and cache.has(srcs[b], srcs[a], map_shas[m], runner_version))]
        if need:
            units.append((a, b, need))
    return units


def panel(cfg, hall_of_fame):
    """§11: `starter` (the configured panel) plus the last hall_of_fame_in_panel champions."""
    k = cfg['eval']['hall_of_fame_in_panel']
    return list(dict.fromkeys(list(cfg['eval']['panel']) + (hall_of_fame[-k:] if k else [])))


def stage_a_pairs(bots, panel_bots, maps, round_robin):
    """§11 Stage A: round robin of the generation plus the panel (while the hall of fame has < 2 bots),
    otherwise every generation bot against every panel bot."""
    pairs = []
    panel = panel_bots
    if round_robin:
        pool = sorted(set(bots))
        for i, a in enumerate(pool):
            for b in pool[i + 1:]:
                pairs.append(((a, b), maps))
        for a in pool:
            for p in panel:
                if p not in pool:
                    pairs.append(((a, p), maps))
    else:
        for a in sorted(set(bots)):
            for p in panel:
                if p != a:
                    pairs.append(((a, p), maps))
    return pairs


def stage_b_pairs(top, panel, maps_all, maps_a):
    """§11 Stage B: round robin of the top bots on all maps, plus their panel games on the other maps."""
    pairs = []
    for i, a in enumerate(top):
        for b in top[i + 1:]:
            pairs.append(((a, b), maps_all))
    rest = [m for m in maps_all if m not in set(maps_a)]
    for a in top:
        for p in panel:
            if p != a and rest:
                pairs.append(((a, p), rest))
    return pairs


def write_job(cfg, job_id, units, generation, stage, root=ROOT):
    job = jobfile.build_units(job_id, units, generation=generation, stage=stage,
                              shard_games=cfg['eval']['shard_games'], root=root)
    if len(job['shards']) > 256:
        raise ValueError(f'{job_id}: {len(job["shards"])} shards exceed the 256-entry matrix limit')
    rel = f'evo/jobs/{job_id}.json'
    gitops.write_file(rel, jobfile.dumps(job), root)
    return rel, job


def fetch_results(job_id, root=ROOT):
    """results/<job_id>.json from origin/evo-results as (commit, dict), or (None, None)."""
    tip = gitops.rev('origin/evo-results', root)
    if not tip:
        return None, None
    data = gitops.show(tip, f'results/{job_id}.json', root)
    if data is None:
        return None, None
    return tip, json.loads(data)


def srcs_for(bots, root=ROOT):
    return {b: digest(Path(root) / 'bots' / b) for b in bots}


def map_shas(maps, root=ROOT):
    return {m: jobfile.map_sha256(root, m) for m in maps}


def cache_for(cfg):
    return Cache(path(cfg, 'cache'))
