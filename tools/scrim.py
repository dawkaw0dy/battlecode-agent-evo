#!/usr/bin/env python3
"""Upload a bot, play an unranked five-match group against a team, and save the replays.

Drives the platform through the `cambc` CLI, so it reuses the CLI's login
(`cambc login`, valid for seven days) and never handles credentials itself.

    python tools/scrim.py v16 "Zarzorki"
    python tools/scrim.py v16 1890540d-1ffc-4847-adbf-d0d08772f268 --maps 00rows,00lanes,00swirl,00split,00eight
    python tools/scrim.py --no-submit - "Team M^2FC"        # play with the current live bot
    python tools/scrim.py v16 "Zarzorki" --restore v14      # make v14 live again afterwards

Warning: the platform keeps one live bot per team. An upload becomes the live
bot for every following match, ranked ladder matches included, until another
version is uploaded or made live. Use --restore to re-upload your ladder bot.

Output (default replays/scrim/<timestamp>-<bot>-vs-<team>/):
    group.zip                 the five match bundles as downloaded
    NN-<map>.repl26a          each replay (open with `cambc view <file>`)
    NN-<map>/                 each bundle extracted: replay, stdout, fuel trace
    result.json, result.txt   per-match result from our side and the total
"""
import argparse
import io
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time
import zipfile

ROOT = Path(__file__).resolve().parents[1]
UUID = re.compile(r'^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$', re.I)
DONE = {'finished', 'failed', 'error', 'cancelled', 'canceled', 'aborted'}


def default_runner():
    exe = ROOT / '.venv' / ('Scripts/cambc.exe' if os.name == 'nt' else 'bin/cambc')
    return str(exe) if exe.exists() else 'cambc'


class Cli:
    def __init__(self, runner):
        self.runner = runner

    def run(self, *args, capture=True):
        result = subprocess.run([self.runner, *args], cwd=ROOT, text=True, encoding='utf-8', errors='replace',
                                stdout=subprocess.PIPE if capture else None,
                                stderr=subprocess.PIPE if capture else None)
        if result.returncode:
            detail = (result.stderr or result.stdout or '').strip() if capture else ''
            if 'cambc login' in detail or 'session' in detail.lower():
                detail += '\nRun `cambc login` (from the repository root) and try again.'
            sys.exit(f"error: cambc {' '.join(args)} failed ({result.returncode})\n{detail}")
        return result.stdout

    def json(self, *args):
        out = self.run(*args, '--json')
        try:
            return json.loads(out)
        except json.JSONDecodeError:
            sys.exit(f"error: cambc {' '.join(args)} returned non-JSON output:\n{out}")


def find_team(cli, query):
    """Resolve a team UUID or a ladder name (exact, then unique substring) to (id, name)."""
    if UUID.match(query):
        return query, query
    teams, page = [], 1
    while True:
        data = cli.json('ladder', '--page', str(page), '--limit', '100')
        teams += data.get('teams', [])
        if not data.get('teams') or len(teams) >= data.get('total', 0):
            break
        page += 1
    want = query.casefold()
    exact = [t for t in teams if t['name'].casefold() == want]
    partial = [t for t in teams if want in t['name'].casefold()]
    matches = exact or partial
    if len(matches) != 1:
        names = ', '.join(f"{t['name']!r} (#{t['rank']})" for t in matches[:10]) or 'none'
        sys.exit(f"error: team {query!r} matched {len(matches)} ladder teams: {names}. "
                 "Use a more exact name or the team ID.")
    return matches[0]['teamId'], matches[0]['name']


def find_group_id(data):
    """Pull the match-group ID out of `match-groups create --json`, whatever its nesting."""
    if isinstance(data, dict):
        for key in ('groupId', 'id'):
            if isinstance(data.get(key), str) and UUID.match(data[key]):
                return data[key]
        for key in ('group', 'matchGroup'):
            if key in data and (found := find_group_id(data[key])):
                return found
        for value in data.values():
            if isinstance(value, (dict, list)) and (found := find_group_id(value)):
                return found
    elif isinstance(data, list):
        for value in data:
            if found := find_group_id(value):
                return found
    return None


def wait_for_group(cli, group_id, timeout, interval):
    started = time.monotonic()
    last = None
    while True:
        group = cli.json('match-groups', 'show', group_id)
        matches = group.get('matches', [])
        states = [m.get('status') for m in matches]
        line = ', '.join(f"{s}: {states.count(s)}" for s in sorted(set(states), key=str))
        elapsed = int(time.monotonic() - started)
        if line != last:
            print(f"  [{elapsed // 60}m{elapsed % 60:02}s] {line or 'no matches yet'}", flush=True)
            last = line
        if len(matches) == 5 and all(s in DONE for s in states):
            return group
        if time.monotonic() - started > timeout:
            sys.exit(f"error: group {group_id} not finished after {timeout}s; "
                     f"rerun later with --group {group_id}")
        time.sleep(interval)


def summarise(group, team_id):
    rows = []
    for match in sorted(group['matches'], key=lambda m: m.get('position', 0)):
        ours = next((t for t in match['teams'] if t.get('teamId') == team_id), None)
        theirs = next((t for t in match['teams'] if t is not ours), {})
        rows.append({
            'position': match.get('position'),
            'matchId': match['matchId'],
            'map': match.get('map'),
            'status': match.get('status'),
            'side': ours and ours.get('side'),
            'result': ours and ours.get('result'),
            'endReason': match.get('endReason'),
            'opponent': theirs.get('name'),
            'ourPicks': ours and ours.get('draftPicks'),
            'theirPicks': theirs.get('draftPicks'),
            'engineVersion': ours and ours.get('engineVersion'),
        })
    return rows


def save_replays(cli, group_id, out):
    bundle = out / 'group.zip'
    cli.run('match-groups', 'download', group_id, '-o', str(bundle))
    saved = []
    with zipfile.ZipFile(bundle) as outer:
        for name in sorted(outer.namelist()):
            if not name.endswith('.zip'):
                continue
            saved.append((name.removesuffix('.zip'), outer.read(name)))
    return saved


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('bot', help="bot folder under bots/ to upload (ignored with --no-submit or --group)")
    parser.add_argument('opponent', help='opponent team name (as on the ladder) or team ID')
    parser.add_argument('--maps', help='five map names or IDs, comma-separated (default: platform picks)')
    parser.add_argument('--no-submit', action='store_true', help='play with the current live bot, upload nothing')
    parser.add_argument('--group', help='skip upload and queueing; wait for and download this existing group ID')
    parser.add_argument('--restore', metavar='BOT', help='after downloading, upload this bot folder to make it live again')
    parser.add_argument('--settle', type=int, default=20,
                        help='seconds to wait after the upload before queueing (default 20)')
    parser.add_argument('--timeout', type=int, default=3600, help='seconds to wait for the matches (default 3600)')
    parser.add_argument('--interval', type=int, default=15, help='seconds between status polls (default 15)')
    parser.add_argument('--output', type=Path, help='output directory (default replays/scrim/<timestamp>-...)')
    parser.add_argument('--runner', default=default_runner(), help='cambc executable (default .venv cambc)')
    args = parser.parse_args()

    cli = Cli(args.runner)
    quota = cli.json('match-groups', 'quota')
    me = quota['team']
    used, limit = quota['quota'].get('used'), quota['quota'].get('limit')
    print(f"Team {me['name']} (rank {quota.get('requesterRank')}), unranked quota {used}/{limit}")
    opp_id, opp_name = find_team(cli, args.opponent)
    if opp_id == me['id']:
        sys.exit('error: the opponent is your own team')
    print(f"Opponent: {opp_name} ({opp_id})")

    submitted = not (args.no_submit or args.group)
    if submitted:
        if not (ROOT / 'bots' / args.bot).is_dir():
            sys.exit(f"error: bots/{args.bot} does not exist")
        print(f"Uploading bots/{args.bot} (it becomes your live bot, ranked matches included)...")
        print(cli.run('submit', args.bot).strip())
        if args.settle:
            print(f"Waiting {args.settle}s for the platform to register the upload...")
            time.sleep(args.settle)

    group_id = args.group
    if not group_id:
        if quota.get('ready') is False:
            sys.exit('error: the platform says your team cannot queue unranked matches right now')
        create = ['match-groups', 'create', opp_id] + (['--maps', args.maps] if args.maps else [])
        created = cli.json(*create)
        group_id = find_group_id(created)
        if not group_id:
            sys.exit(f"error: could not find the group ID in:\n{json.dumps(created, indent=1)}")
    print(f"Match group {group_id}; waiting for five matches...")
    group = wait_for_group(cli, group_id, args.timeout, args.interval)

    label = 'live' if not submitted else args.bot
    safe_opp = re.sub(r'[^A-Za-z0-9_.-]+', '_', opp_name).strip('_') or 'opponent'
    out = args.output or ROOT / 'replays' / 'scrim' / f"{time.strftime('%Y%m%d-%H%M%S')}-{label}-vs-{safe_opp}"
    out.mkdir(parents=True, exist_ok=True)
    rows = summarise(group, me['id'])
    by_id = {r['matchId']: r for r in rows}
    for name, data in save_replays(cli, group_id, out):
        number, _, match_id = name.partition('-')
        row = by_id.get(match_id, {})
        stem = f"{number}-{row.get('map') or match_id}"
        with zipfile.ZipFile(io.BytesIO(data)) as inner:
            inner.extractall(out / stem)
            if 'replay.repl26a' in inner.namelist():
                (out / f"{stem}.repl26a").write_bytes(inner.read('replay.repl26a'))
        row['replay'] = f"{stem}.repl26a"

    wins = sum(r['result'] == 'win' for r in rows)
    losses = sum(r['result'] == 'loss' for r in rows)
    result = {'groupId': group_id, 'team': me, 'opponent': {'id': opp_id, 'name': opp_name},
              'bot': None if not submitted else args.bot, 'maps': args.maps,
              'wins': wins, 'losses': losses, 'matches': rows}
    (out / 'result.json').write_text(json.dumps(result, indent=1), encoding='utf-8')
    lines = [f"{label} vs {opp_name}: {wins}-{losses}  (group {group_id})"]
    for r in rows:
        lines.append(f"  {r['position']}. {str(r['map']):<22} side {r['side']}  {str(r['result']):<5} "
                     f"{str(r['endReason']):<14} picks {r['ourPicks']} vs {r['theirPicks']}")
    text = '\n'.join(lines)
    (out / 'result.txt').write_text(text + '\n', encoding='utf-8')
    print(text)
    print(f"Saved to {out}")

    if args.restore:
        if not (ROOT / 'bots' / args.restore).is_dir():
            sys.exit(f"error: bots/{args.restore} does not exist; nothing restored")
        print(f"Restoring: uploading bots/{args.restore} as the live bot...")
        print(cli.run('submit', args.restore).strip())


if __name__ == '__main__':
    main()
