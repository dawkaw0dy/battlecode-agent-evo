"""git, through subprocess argument lists only (no shell). Pushes use the clone's own push URL (D20)."""
import io
import subprocess
import tarfile
from pathlib import Path

from evo.orchestrator.config import ROOT


class GitError(RuntimeError):
    pass


def git(*args, root=ROOT, check=True, binary=False):
    proc = subprocess.run(['git', *args], cwd=root, capture_output=True, stdin=subprocess.DEVNULL)
    if check and proc.returncode != 0:
        raise GitError(f"git {' '.join(args)}: {proc.stderr.decode('utf-8', 'replace').strip()}")
    return proc.stdout if binary else proc.stdout.decode('utf-8', 'replace').strip()


def head(root=ROOT):
    return git('rev-parse', 'HEAD', root=root)


def branch(root=ROOT):
    return git('rev-parse', '--abbrev-ref', 'HEAD', root=root)


def dirty(root=ROOT):
    return git('status', '--porcelain', root=root)


def fetch(root=ROOT):
    git('fetch', '-q', 'origin', '--prune', root=root)


def rev(ref, root=ROOT):
    out = git('rev-parse', '-q', '--verify', f'{ref}^{{commit}}', root=root, check=False)
    return out or None


def commit(paths, message, root=ROOT):
    """Stage exactly `paths` (additions, changes and deletions) and commit; None if nothing changed."""
    git('add', '-A', '--', *[str(p) for p in paths], root=root)
    if not git('diff', '--cached', '--name-only', root=root):
        return None
    git('commit', '-q', '-m', message, root=root)
    return head(root)


def push_main(root=ROOT):
    git('push', '-q', 'origin', 'HEAD:main', root=root)


def in_sync_with_origin(root=ROOT):
    fetch(root)
    return branch(root) == 'main' and head(root) == rev('origin/main', root)


def remote_branches(root=ROOT, exclude=('main', 'evo-results', 'HEAD')):
    out = git('for-each-ref', '--format=%(refname:short) %(objectname) %(committerdate:unix)',
              'refs/remotes/origin', root=root)
    rows = []
    for line in out.splitlines():
        name, sha, when = line.split()
        short = name.split('/', 1)[1] if '/' in name else name
        if short not in exclude and name != 'origin':
            rows.append((name, sha, int(when)))
    return rows


def show(ref, path, root=ROOT):
    """Bytes of `path` at `ref`, or None."""
    proc = subprocess.run(['git', 'show', f'{ref}:{path}'], cwd=root, capture_output=True, stdin=subprocess.DEVNULL)
    return proc.stdout if proc.returncode == 0 else None


def is_ancestor(a, b, root=ROOT):
    return subprocess.run(['git', 'merge-base', '--is-ancestor', a, b], cwd=root,
                          capture_output=True).returncode == 0


def changed_files(base, commit_sha, root=ROOT):
    out = git('diff', '--name-only', '--no-renames', base, commit_sha, root=root)
    return [p for p in out.splitlines() if p]


def archive_members(commit_sha, path, root=ROOT):
    """tarfile members and data under `path` at `commit_sha` (git archive, no checkout)."""
    data = git('archive', '--format=tar', commit_sha, '--', path, root=root, binary=True)
    tar = tarfile.open(fileobj=io.BytesIO(data))
    return tar, tar.getmembers()


def write_file(rel, data, root=ROOT):
    target = Path(root) / rel
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)
