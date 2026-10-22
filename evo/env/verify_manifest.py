#!/usr/bin/env python3
"""Check the checkout against evo/env/manifest.json (PLAN §6.1). Exit 1 on any difference."""
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from evo import manifest  # noqa: E402


def main():
    expected = json.loads((ROOT / manifest.MANIFEST).read_text(encoding='utf-8'))
    actual = manifest.compute(ROOT, expected['runner_version'])
    diffs = manifest.differences(expected, actual)
    for line in diffs:
        print('manifest mismatch:', line, file=sys.stderr)
    if diffs:
        return 1
    print(f"manifest OK ({sum(len(g) for g in expected['files'].values())} files, {len(expected['bots'])} bots)")
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
