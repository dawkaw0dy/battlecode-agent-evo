"""Print stdin as one GitHub Actions annotation (job logs need a token; annotations are public).

usage: some command | python3 evo/m0/annotate.py "title" [error|warning]
"""
import sys

title = sys.argv[1]
level = sys.argv[2] if len(sys.argv) > 2 else 'warning'
text = sys.stdin.read()[-3800:].replace('%', '%25').replace('\r', '').replace('\n', '%0A')
print(f'::{level} title={title}::{text}')
