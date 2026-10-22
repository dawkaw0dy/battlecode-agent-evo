"""Find agents' DONE markers on remote branches (PLAN §7). Branch names do not matter."""
import json

from evo.orchestrator import gitops
from evo.orchestrator.launch import same_session


def marker_path(agent_id):
    return f'evo/agents/{agent_id}/DONE.json'


def find_marker(agent, root=gitops.ROOT):
    """(branch, commit, marker) of the newest valid marker for this agent, or (None, None, reason)."""
    agent_id = agent['id']
    launched_at = min(s['launched_at'] for s in agent['sessions']) if agent.get('sessions') else 0
    current = agent['sessions'][-1]['id'] if agent.get('sessions') else None
    candidates, reasons = [], []
    for name, sha, when in gitops.remote_branches(root):
        if when + 300 < launched_at:  # tips older than the launch cannot hold this agent's work
            continue
        data = gitops.show(sha, marker_path(agent_id), root)
        if data is None:
            continue
        try:
            marker = json.loads(data)
        except ValueError:
            reasons.append(f'{name}: DONE.json is not JSON')
            continue
        problem = validate(agent, marker, sha, current, root)
        if problem:
            reasons.append(f'{name}: {problem}')
            continue
        candidates.append((when, name, sha, marker))
    if not candidates:
        return None, None, '; '.join(reasons) or 'no marker yet'
    when, name, sha, marker = max(candidates)
    return name, sha, marker


def validate(agent, marker, sha, current_session, root):
    if marker.get('agent') != agent['id']:
        return f"agent {marker.get('agent')!r} != {agent['id']}"
    if marker.get('task_sha256') != agent['task_sha256']:
        return 'task_sha256 does not match the task'
    base = marker.get('base_commit')
    if not base or not gitops.rev(base, root) or not gitops.is_ancestor(base, sha, root):
        return 'base_commit is not an ancestor of the branch'
    if not gitops.is_ancestor(base, 'origin/main', root):
        return 'base_commit is not a commit of main'
    if not same_session(marker.get('session_id'), current_session):
        return f"session_id {marker.get('session_id')!r} is not the current session"
    return None
