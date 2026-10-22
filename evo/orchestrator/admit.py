"""Admission (PLAN §8): scope, allowlist and digest on an agent commit, then copy into main.

The scope diff starts at the merge-base of the agent commit with origin/main, never at the
agent's own claimed base_commit, so commits on the branch cannot hide out-of-scope edits.
"""
import shutil
import tarfile
import tempfile
from pathlib import Path

from evo.eval import allowlist
from evo.eval.digest import digest
from evo.orchestrator import gitops
from evo.orchestrator.config import ROOT


def scope_problems(agent_id, changed):
    allowed = (f'bots/{agent_id}/', f'evo/agents/{agent_id}/')
    return [f'out of scope: {p}' for p in changed if not p.startswith(allowed)]


def extract_bot(commit_sha, agent_id, target, root=ROOT):
    """Write bots/<id> from the commit into target/<id>; regular files only."""
    tar, members = gitops.archive_members(commit_sha, f'bots/{agent_id}', root)
    problems = []
    dest = Path(target) / agent_id
    prefix = f'bots/{agent_id}/'
    for m in members:
        if not m.name.startswith(prefix):
            continue  # the leading 'bots' and 'bots/<id>' directory entries
        rel = Path(m.name[len(prefix):])
        if m.issym() or m.islnk():
            problems.append(f'{m.name}: symlink or hard link')
        elif m.isfile():
            out = dest / rel
            out.parent.mkdir(parents=True, exist_ok=True)
            out.write_bytes(tar.extractfile(m).read())
        elif not m.isdir():
            problems.append(f'{m.name}: not a regular file')
    return dest, problems


def check(agent_id, commit_sha, root=ROOT):
    """(problems, extracted_dir or None). Empty problems = admissible."""
    base = gitops.git('merge-base', 'origin/main', commit_sha, root=root)
    problems = scope_problems(agent_id, gitops.changed_files(base, commit_sha, root))
    tmp = Path(tempfile.mkdtemp(prefix='evo-admit-'))
    try:
        dest, link_problems = extract_bot(commit_sha, agent_id, tmp, root)
    except (gitops.GitError, tarfile.TarError) as exc:
        return problems + [f'cannot read bots/{agent_id}: {exc}'], None
    problems += link_problems
    if not problems:
        problems += allowlist.check_bot(dest)
    return problems, dest


def copy_in(agent_id, extracted, commit_sha, root=ROOT):
    """Copy src/, NOTES.md and DONE.json into the working tree; return the paths to commit and src_sha256."""
    bot = Path(root) / 'bots' / agent_id
    if (bot / 'src').exists():
        shutil.rmtree(bot / 'src')
    shutil.copytree(Path(extracted) / 'src', bot / 'src')
    paths = [f'bots/{agent_id}/src']
    notes = Path(extracted) / 'NOTES.md'
    if notes.is_file():
        shutil.copy(notes, bot / 'NOTES.md')
        paths.append(f'bots/{agent_id}/NOTES.md')
    marker = gitops.show(commit_sha, f'evo/agents/{agent_id}/DONE.json', root)
    gitops.write_file(f'evo/agents/{agent_id}/DONE.json', marker, root)
    paths.append(f'evo/agents/{agent_id}/DONE.json')
    shutil.rmtree(Path(extracted).parent, ignore_errors=True)
    return paths, digest(bot)
