#!/usr/bin/env python3
"""Run deterministic, side-balanced release matches and retain raw evidence."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import queue
import re
import shutil
import signal
import subprocess
import sys
import threading
import time
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def duration(seconds):
    seconds = max(0, int(seconds))
    minutes, seconds = divmod(seconds, 60)
    hours, minutes = divmod(minutes, 60)
    return f"{hours}h {minutes:02}m" if hours else f"{minutes}m {seconds:02}s"


class Progress:
    """Render one live terminal line, or readable milestones when redirected."""

    def __init__(self, total, enabled=True, color='auto', stream=None):
        self.stream = stream if stream is not None else sys.stdout
        self.enabled = enabled
        self.tty = self.stream.isatty()
        self.color = color == 'always' or (color == 'auto' and self.tty and 'NO_COLOR' not in os.environ)
        self.total = total
        self.started = time.monotonic()
        self.active = set()
        self.finished = set()
        self.failed = set()
        self.phase = 'Preparing'
        self.event = None
        self.last_draw = 0
        self.last_text = None

    def paint(self, text, code):
        return f"\033[{code}m{text}\033[0m" if self.color else text

    def say(self, text=''):
        if self.enabled:
            if self.tty:
                print('\r\033[2K', end='', file=self.stream)
                self.last_text = None
            print(text, file=self.stream, flush=True)

    def consume(self, line):
        # --color never makes the CLI's event prefixes stable in pipes.
        line = line.strip()
        if line.startswith('Building '):
            self.phase = 'Building ' + Path(line.removeprefix('Building ')).name
            self.say('  ' + self.phase)
        elif line.startswith('[|] STARTING'):
            self.event = 'start'
        elif line.startswith('[+]'):
            self.event = 'done'
        elif line.startswith('[-]') or line.startswith('[!]'):
            self.event = 'failed'
        elif line.startswith('Replay:') and self.event:
            tag = Path(line.removeprefix('Replay:').strip()).stem
            if self.event == 'start':
                self.active.add(tag)
                self.phase = 'Playing'
            else:
                self.active.discard(tag)
                if tag not in self.finished:
                    self.finished.add(tag)
                    if self.event == 'failed':
                        self.failed.add(tag)
                    if not self.tty:
                        label = 'FAILED' if tag in self.failed else 'finished'
                        self.say(f"  [{len(self.finished):>{len(str(self.total))}}/{self.total}] {tag:<24} {label}  |  {duration(time.monotonic() - self.started)} elapsed")
            self.event = None
        elif line.startswith('error:'):
            self.say(self.paint('  ' + line, '31'))

    def draw(self, force=False):
        now = time.monotonic()
        if not self.enabled or not self.tty or (not force and now - self.last_draw < 0.1):
            return
        self.last_draw = now
        done = len(self.finished)
        elapsed = now - self.started
        remaining = f"  ETA {duration(elapsed / done * (self.total - done))}" if done else ''
        width = 20
        filled = width * done // max(1, self.total)
        bar = self.paint('#' * filled + '-' * (width - filled), '36')
        activity = ', '.join(sorted(self.active)) or self.phase
        text = f"  [{bar}] {done}/{self.total}  {duration(elapsed)}{remaining}  |  {activity}"
        # Account for ANSI codes when limiting the visible terminal width.
        columns = shutil.get_terminal_size((100, 24)).columns
        if len(re.sub(r'\x1b\[[0-9;]*m', '', text)) > columns - 1:
            text = f"  [{bar}] {done}/{self.total}  {duration(elapsed)}{remaining}  |  {len(self.active)} running"
        if text == self.last_text and not force:
            return
        self.last_text = text
        print('\r\033[2K' + text, end='', file=self.stream, flush=True)

    def close(self):
        if self.enabled and self.tty:
            print('\r\033[2K', end='', file=self.stream, flush=True)


def run_batch(cmd, output, progress):
    """Drain CLI output continuously, keeping the full log and a responsive UI."""
    events = queue.Queue()
    interrupted = False
    start = time.monotonic()
    with (output / 'runner.log').open('w') as log:
        process = subprocess.Popen(cmd, cwd=ROOT, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, text=True,
                                   encoding='utf-8', errors='replace',
                                   start_new_session=True)

        def read_output():
            try:
                for line in process.stdout:
                    log.write(line)
                    log.flush()
                    events.put(line)
            finally:
                events.put(None)

        reader = threading.Thread(target=read_output, daemon=True)
        reader.start()
        try:
            while True:
                try:
                    line = events.get(timeout=0.1)
                except queue.Empty:
                    progress.draw()
                    continue
                if line is None:
                    break
                progress.consume(line)
                progress.draw()
            returncode = process.wait()
        except KeyboardInterrupt:
            interrupted = True
            progress.say('  Stopping matches; saving completed results…')
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
            returncode = 130
        finally:
            reader.join(timeout=5)
            process.stdout.close()
            progress.close()
    return returncode, interrupted, time.monotonic() - start


def read_result(output, name, side):
    tag = f'{name}-{side}'
    try:
        data = json.loads((output / f'{tag}.json').read_text())
        team = 'teamA' if side == 'A' else 'teamB'
        scores = {t['team']: int(t['submittedGems']) for t in data['teams']}
        with zipfile.ZipFile(output / f'{tag}.repl26a') as replay:
            bot_errors = {t: replay.read(f'team-{s}.stdout').count(b'bot error:')
                          for t, s in [('teamA', 'a'), ('teamB', 'b')]}
        row = {'map': name, 'side': side, 'win': data['match']['outcome'] == team,
               'scores': scores, 'bot_errors': bot_errors,
               'margin': scores[team] - scores['teamB' if side == 'A' else 'teamA'],
               'rounds': data['match']['roundsStarted'],
               'faults': data['diagnostics']['botFaults']}
        if any(row['faults'].values()):
            row['error'] = 'bot faults: match is invalid for strength comparisons'
        if any(bot_errors.values()):
            row['error'] = 'caught bot exceptions: inspect replay stdout'
        if data['match']['outcome'] not in ('teamA', 'teamB'):
            row['error'] = 'unfinished match'
    except Exception as exc:
        row = {'map': name, 'side': side, 'error': str(exc)}
    return row


def summarize(rows, runner_exit, elapsed, interrupted=False):
    valid = [r for r in rows if 'error' not in r]
    wins = sum(r['win'] for r in valid)
    return {'games': len(valid), 'errors': len(rows) - len(valid),
            'wins': wins, 'losses': len(valid) - wins,
            'win_rate': wins / len(valid) if valid else None,
            'mean_margin': sum(r['margin'] for r in valid) / len(valid) if valid else 0.0,
            'bot_faults': sum(sum(r.get('faults', {}).values()) for r in rows),
            'bot_errors': sum(sum(r.get('bot_errors', {}).values()) for r in rows),
            'runner_exit': runner_exit, 'elapsed_seconds': round(elapsed, 2),
            'interrupted': interrupted}


def render_summary(candidate, opponent, maps, rows, summary, output):
    """Return a plain-text report suitable for terminals and summary.txt."""
    valid = summary['games']
    rate = f"{summary['win_rate']:.1%}" if valid else 'n/a'
    status = 'INTERRUPTED' if summary['interrupted'] else ('FAILED' if summary['errors'] or summary['runner_exit'] else 'COMPLETE')
    lines = ['', f"  {status}  {candidate} vs {opponent}", '  ' + '─' * 66,
             f"  {summary['wins']}W / {summary['losses']}L   Win rate {rate}   Mean gems {summary['mean_margin']:+.1f}",
             f"  {valid}/{len(rows)} valid games   {summary['errors']} invalid   Elapsed {duration(summary['elapsed_seconds'])}", '']
    width = max(12, max(map(len, maps), default=0))
    lines += [f"  {'Map':<{width}}  {'Side A':>12}  {'Side B':>12}  {'Wins':>6}  {'Avg gems':>9}",
              '  ' + '─' * (width + 49)]
    indexed = {(r['map'], r['side']): r for r in rows}
    for name in maps:
        pair = [indexed[(name, side)] for side in ('A', 'B')]
        cells = ['ERROR' if 'error' in r else f"{'W' if r['win'] else 'L'} {r['margin']:+d}" for r in pair]
        good = [r for r in pair if 'error' not in r]
        wins = f"{sum(r['win'] for r in good)}/{len(good)}" if good else '—'
        margin = f"{sum(r['margin'] for r in good) / len(good):+.1f}" if good else '—'
        lines.append(f"  {name:<{width}}  {cells[0]:>12}  {cells[1]:>12}  {wins:>6}  {margin:>9}")
    lines += ['', '  W/L and gem margins are from the candidate’s perspective.',
              '  Gem ties can be decided by gold, kills, or the Team B tiebreaker.',
              f"  Engine faults: {summary['bot_faults']}   Caught exceptions: {summary['bot_errors']}   Runner exit: {summary['runner_exit']}"]
    errors = [r for r in rows if 'error' in r]
    if errors:
        lines += ['', '  Invalid games:']
        for row in errors:
            lines.append(f"    {row['map']} ({row['side']}): {row['error']}")
    lines += ['', f"  Results: {output / 'results.json'}",
              f"  Summary: {output / 'summary.json'}",
              f"  Log:     {output / 'runner.log'}", '']
    return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('candidate')
    parser.add_argument('opponent')
    parser.add_argument('--maps', nargs='*')
    parser.add_argument('--map-dir', type=Path, default=ROOT / 'maps',
                        help='Directory containing .map26a files (default: repository maps/)')
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--runner', default=str(ROOT / '.venv/bin/cambc'))
    parser.add_argument('--timeout', type=int, default=300)
    parser.add_argument('--json', action='store_true', help='Print JSON rows and summary instead of the terminal report')
    parser.add_argument('--no-progress', action='store_true', help='Hide live progress; still print the final report')
    parser.add_argument('--color', choices=['auto', 'always', 'never'], default='auto', help='Terminal colors (auto respects NO_COLOR)')
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    if args.timeout < 1:
        parser.error('--timeout must be positive')
    map_dir = args.map_dir.resolve()
    if not map_dir.is_dir():
        parser.error(f'map directory not found: {map_dir}')
    maps = args.maps or [p.stem for p in sorted(map_dir.glob('*.map26a'))]
    if not maps or len(maps) != len(set(maps)):
        parser.error('choose at least one map, without duplicates')
    for bot in (args.candidate, args.opponent):
        if Path(bot).name != bot or not (ROOT / 'bots' / bot / 'src').is_dir():
            parser.error(f'bot source directory not found: bots/{bot}/src')
    for name in maps:
        if Path(name).name != name or not (map_dir / (name + '.map26a')).is_file():
            parser.error(f'map not found: {name}')
    try:
        runner_version = subprocess.check_output(
            [args.runner, '--version'], text=True, stderr=subprocess.STDOUT).strip()
    except (OSError, subprocess.CalledProcessError) as exc:
        parser.error(f'unable to run {args.runner}: {exc}')
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        parser.error('output must be empty; choose a fresh directory')
    progress = Progress(len(maps) * 2, enabled=not args.json, color=args.color)
    progress.say('')
    progress.say(progress.paint(f'  BENCHMARK  {args.candidate} vs {args.opponent}', '1;36'))
    progress.say(f"  {len(maps)} {'map' if len(maps) == 1 else 'maps'} · {len(maps) * 2} games · {args.jobs} workers · release mode")
    progress.say(f'  Output: {args.output.resolve()}')
    if args.no_progress:
        progress.enabled = False
    def digest(bot):
        h = hashlib.sha256()
        for path in sorted((ROOT / 'bots' / bot / 'src').rglob('*')):
            if not path.is_file() or '__pycache__' in path.parts:
                continue
            h.update(str(path.relative_to(ROOT / 'bots' / bot)).encode())
            h.update(path.read_bytes())
        return h.hexdigest()
    metadata = {'candidate': args.candidate, 'opponent': args.opponent,
                'source_sha256': {b: digest(b) for b in (args.candidate, args.opponent)},
                'runner': runner_version,
                'maps': maps, 'map_dir': str(map_dir), 'release': True,
                'map_sha256': {m: hashlib.sha256((map_dir / (m + '.map26a')).read_bytes()).hexdigest() for m in maps}}
    (output / 'metadata.json').write_text(json.dumps(metadata, indent=2) + '\n')
    # Each suite owns its source/build directories. This prevents rebuild races
    # between suites and makes edits during a running match harmless.
    inputs = {}
    for bot in dict.fromkeys((args.candidate, args.opponent)):
        inputs[bot] = output / 'inputs' / bot
        shutil.copytree(ROOT / 'bots' / bot / 'src', inputs[bot] / 'src',
                        ignore=shutil.ignore_patterns('__pycache__'))
    tasks = [(m, side) for m in maps for side in ('A', 'B')]
    manifest = []
    for name, side in tasks:
        a, b = (args.candidate, args.opponent) if side == 'A' else (args.opponent, args.candidate)
        tag = f'{name}-{side}'
        manifest.append(f"{inputs[a]} {inputs[b]} {map_dir / (name + '.map26a')} {output / (tag + '.repl26a')} --statistics {output / (tag + '.json')}")
    manifest_path = output / 'manifest.txt'
    manifest_path.write_text('\n'.join(manifest) + '\n')
    cmd = [args.runner, 'run-batch', str(manifest_path), '--release', '--json',
           '--jobs', str(args.jobs), '--timeout', str(args.timeout), '--color', 'never']
    try:
        runner_exit, interrupted, elapsed = run_batch(cmd, output, progress)
    except OSError as exc:
        progress.close()
        print(f"Unable to start runner: {exc}", file=sys.stderr)
        return 1
    rows = [read_result(output, name, side) for name, side in tasks]
    (output / 'results.json').write_text(json.dumps(rows, indent=2) + '\n')
    summary = summarize(rows, runner_exit, elapsed, interrupted)
    (output / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    report = render_summary(args.candidate, args.opponent, maps, rows, summary, output)
    (output / 'summary.txt').write_text(report)
    if args.json:
        for row in rows:
            print(json.dumps(row))
        print(json.dumps(summary))
    else:
        label = 'COMPLETE' if not summary['errors'] and not runner_exit else 'FAILED'
        if interrupted:
            label = 'INTERRUPTED'
        report = report.replace(label, progress.paint(label, '32' if label == 'COMPLETE' else '31'), 1)
        print(report, flush=True)
    return 130 if interrupted else int(bool(summary['errors'] or runner_exit))


if __name__ == '__main__':
    raise SystemExit(main())
