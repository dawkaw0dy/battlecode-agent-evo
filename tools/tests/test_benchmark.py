"""Verify reporting and runner lifecycle without expensive game simulations."""
import importlib.util
import hashlib
import io
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest

import shutil

REPO = Path(__file__).resolve().parents[2]
# benchmark.py writes with the locale encoding; UTF-8 mode keeps Windows runs identical to Linux.
os.environ['PYTHONUTF8'] = '1'
SCRIPT = REPO / 'tools' / 'benchmark.py'
spec = importlib.util.spec_from_file_location('benchmark', SCRIPT)
benchmark = importlib.util.module_from_spec(spec)
spec.loader.exec_module(benchmark)

FAKE_RUNNER = r'''#!/usr/bin/env python3
import json, os, pathlib, sys, time, zipfile
if '--version' in sys.argv:
    print('cambc test-runner')
    raise SystemExit(0)
manifest = pathlib.Path(sys.argv[2])
print('Building /tmp/v10', flush=True)
if os.environ.get('BENCHMARK_TEST_WAIT'):
    (manifest.parent / 'ready').touch()
    time.sleep(30)
for i, line in enumerate(manifest.read_text().splitlines()):
    fields = line.split()
    replay, stats = pathlib.Path(fields[3]), pathlib.Path(fields[5])
    print('[|] STARTING match', flush=True)
    print('    Replay: ' + str(replay), flush=True)
    if os.environ.get('BENCHMARK_TEST_FAIL') and i == 1:
        print('[-] FAILED match', flush=True)
        print('    Replay: ' + str(replay), flush=True)
        continue
    stats.write_text(json.dumps({
        'teams': [{'team': 'teamA', 'submittedGems': '1000'},
                  {'team': 'teamB', 'submittedGems': '900'}],
        'match': {'outcome': 'teamA', 'roundsStarted': 500},
        'diagnostics': {'botFaults': {'teamA': 0, 'teamB': 0}}}))
    with zipfile.ZipFile(replay, 'w') as archive:
        archive.writestr('team-a.stdout', '')
        archive.writestr('team-b.stdout', '')
    print('[+] TEAM A WIN match', flush=True)
    print('    Replay: ' + str(replay), flush=True)
raise SystemExit(1 if os.environ.get('BENCHMARK_TEST_FAIL') else 0)
'''


class BenchmarkTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='benchmark-test-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        # benchmark.py resolves bots/ and maps/ next to its own tools/ directory,
        # so run a copy inside a throwaway repository tree.
        self.repo = self.root / 'repo'
        for rel in ('tools/benchmark.py', 'evo/__init__.py', 'evo/eval/__init__.py', 'evo/eval/digest.py'):
            (self.repo / rel).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy(REPO / rel, self.repo / rel)
        for bot in ('v10', 'v06'):
            (self.repo / 'bots' / bot / 'src').mkdir(parents=True)
            (self.repo / 'bots' / bot / 'src' / 'main.cpp').write_text(f'// {bot}\n')
        (self.repo / 'maps').mkdir()
        (self.repo / 'maps' / 'rows.map26a').write_bytes(b'fake map')
        self.script = self.repo / 'tools' / 'benchmark.py'
        (self.root / 'runner.py').write_text(FAKE_RUNNER)
        if os.name == 'nt':
            self.runner = self.root / 'runner.cmd'
            self.runner.write_text(f'@"{sys.executable}" "%~dp0runner.py" %*\n')
        else:
            self.runner = self.root / 'runner'
            self.runner.write_text(f'#!{sys.executable}\n' + FAKE_RUNNER)
            self.runner.chmod(0o755)
        self.output = self.root / 'output'

    def command(self, *extra):
        return [sys.executable, str(self.script), 'v10', 'v06', '--maps', 'rows',
                '--output', str(self.output), '--runner', str(self.runner), *extra]

    def test_plain_report_and_progress(self):
        result = subprocess.run(self.command('--color', 'never'), capture_output=True, text=True, encoding='utf-8')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('[2/2]', result.stdout)
        self.assertIn('50.0%', result.stdout)
        self.assertIn('W +100', result.stdout)
        self.assertIn('L -100', result.stdout)
        self.assertNotIn('\x1b', result.stdout)
        self.assertIn('COMPLETE', (self.output / 'summary.txt').read_text())
        self.assertIn('[+] TEAM A WIN', (self.output / 'runner.log').read_text())
        summary = json.loads((self.output / 'summary.json').read_text())
        self.assertEqual((summary['wins'], summary['losses'], summary['games']), (1, 1, 2))
        metadata = json.loads((self.output / 'metadata.json').read_text())
        # C++ candidates used to receive the hash of an empty Python-only list.
        self.assertNotEqual(metadata['source_sha256']['v10'], hashlib.sha256(b'').hexdigest())

    def test_json_stdout_stays_machine_readable(self):
        result = subprocess.run(self.command('--json'), capture_output=True, text=True, encoding='utf-8')
        self.assertEqual(result.returncode, 0, result.stderr)
        objects = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertEqual(len(objects), 3)
        self.assertEqual(objects[-1]['win_rate'], 0.5)

    def test_custom_pool_discovery_and_provenance(self):
        pool = self.root / 'new-maps'
        pool.mkdir()
        map_file = pool / 'unseen-layout.map26a'
        map_file.write_bytes(b'custom map bytes for fake runner')
        command = self.command('--map-dir', str(pool), '--no-progress')
        del command[4:6]  # Discover the custom pool instead of requesting rows.
        result = subprocess.run(command, capture_output=True, text=True, encoding='utf-8')
        self.assertEqual(result.returncode, 0, result.stderr)
        metadata = json.loads((self.output / 'metadata.json').read_text())
        self.assertEqual(metadata['maps'], ['unseen-layout'])
        self.assertEqual(metadata['map_sha256']['unseen-layout'],
                         hashlib.sha256(map_file.read_bytes()).hexdigest())
        manifest = (self.output / 'manifest.txt').read_text().splitlines()
        self.assertEqual(len(manifest), 2)
        self.assertTrue(all(line.split()[2] == str(map_file.resolve()) for line in manifest))

    def test_failed_match_is_excluded_and_reported(self):
        env = dict(os.environ, BENCHMARK_TEST_FAIL='1')
        result = subprocess.run(self.command('--no-progress'), capture_output=True, text=True, encoding='utf-8', env=env)
        self.assertEqual(result.returncode, 1)
        self.assertIn('FAILED', result.stdout)
        self.assertIn('ERROR', result.stdout)
        self.assertNotIn('[1/2]', result.stdout)
        summary = json.loads((self.output / 'summary.json').read_text())
        self.assertEqual((summary['games'], summary['errors'], summary['wins']), (1, 1, 1))
        self.assertEqual(summary['win_rate'], 1.0)

    @unittest.skipIf(os.name == 'nt', 'SIGINT cannot be sent to a child process on Windows')
    def test_interrupt_saves_partial_report(self):
        env = dict(os.environ, BENCHMARK_TEST_WAIT='1')
        process = subprocess.Popen(self.command('--no-progress'), stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True, encoding='utf-8', env=env)
        try:
            deadline = time.monotonic() + 10
            while not (self.output / 'ready').exists() and time.monotonic() < deadline:
                if process.poll() is not None:
                    self.fail(process.communicate())
                time.sleep(0.02)
            self.assertTrue((self.output / 'ready').exists())
            process.send_signal(signal.SIGINT)
            stdout, stderr = process.communicate(timeout=10)
            self.assertEqual(process.returncode, 130, stderr)
            self.assertIn('INTERRUPTED', stdout)
            self.assertIn('Win rate n/a', stdout)
            summary = json.loads((self.output / 'summary.json').read_text())
            self.assertTrue(summary['interrupted'])
            self.assertEqual(summary['games'], 0)
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()

    def test_live_progress_events_and_terminal_display(self):
        class Terminal(io.StringIO):
            def isatty(self):
                return True
        terminal = Terminal()
        progress = benchmark.Progress(2, color='never', stream=terminal)
        for line in ['[|] STARTING game', 'Replay: /tmp/rows-A.repl26a',
                     '[+] TEAM A WIN', 'Replay: /tmp/rows-A.repl26a',
                     '[|] STARTING game', 'Replay: /tmp/rows-B.repl26a']:
            progress.consume(line)
        self.assertEqual(progress.finished, {'rows-A'})
        self.assertEqual(progress.active, {'rows-B'})
        progress.draw(force=True)
        self.assertIn('1/2', terminal.getvalue())
        self.assertIn('ETA', terminal.getvalue())
        progress.consume('[-] FAILED')
        progress.consume('Replay: /tmp/rows-B.repl26a')
        self.assertEqual(progress.failed, {'rows-B'})


if __name__ == '__main__':
    unittest.main()
