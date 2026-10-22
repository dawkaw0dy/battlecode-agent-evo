#!/usr/bin/env python3
"""Print the environment fingerprint as JSON (PLAN §6.1). Argument: the smoke benchmark output dir."""
import hashlib
import json
import os
import platform
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def output(cmd):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8',
                              errors='replace', check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return None


def main():
    smoke = Path(sys.argv[1]) if len(sys.argv) > 1 else None
    summary = None
    if smoke and (smoke / 'summary.json').is_file():
        summary = json.loads((smoke / 'summary.json').read_text(encoding='utf-8'))
    print(json.dumps({
        'cli': output(['cambc', '--version']),
        'manifest_sha256': hashlib.sha256((ROOT / 'evo/env/manifest.json').read_bytes()).hexdigest(),
        'nproc': os.cpu_count(),
        'python': platform.python_version(),
        'node': output(['node', '--version']),
        'smoke_finished': bool(summary and summary['games'] == 2 and not summary['errors']),
        'smoke_seconds': summary and summary['elapsed_seconds'],
        'remote': os.environ.get('CLAUDE_CODE_REMOTE'),
        'session_id': os.environ.get('CLAUDE_CODE_REMOTE_SESSION_ID'),
    }, indent=1))


if __name__ == '__main__':
    main()
