"""Render evo/tasks/<id>.md from evo/prompts (PLAN §6). The rendered file is the whole prompt."""
import hashlib

from evo.orchestrator.config import ROOT

PROMPTS = ROOT / 'evo' / 'prompts'
OPERATOR_TITLES = {'tweak': 'parameter tweak', 'subsystem': 'subsystem rewrite', 'crossover': 'crossover',
                   'radical': 'radical new strategy'}


def fill(template, values):
    out = template
    for key, value in values.items():
        out = out.replace('{{' + key + '}}', str(value))
    return out


def fragment(kind, name):
    return (PROMPTS / kind / f'{name}.md').read_text(encoding='utf-8').strip()


def evidence_table(rows, hall_of_fame):
    if not rows:
        return ''
    lines = ["## Evidence: the parent's worst losses last generation",
             '| Opponent | Map | Side | Gems (parent – opp) | Rounds |', '|---|---|---|---|---|']
    for r in rows:
        opp = r['opponent'] + (' (hall of fame)' if r['opponent'] in hall_of_fame else '')
        lines.append(f"| {opp} | {r['map']} | {r['side']} | {r['gems_parent']} – {r['gems_opponent']} | {r['rounds']} |")
    return '\n'.join(lines)


def render(cfg, state, agent_id, root=ROOT):
    a = state['agents'][agent_id]
    g = a['generation']
    manifest_sha = hashlib.sha256((root / 'evo/env/manifest.json').read_bytes()).hexdigest()
    deadline = a['deadline_minutes']
    if a['kind'] == 'immigrant':
        arche = (root / cfg['run']['archetypes_dir'] / f"{a['archetype']}.md").read_text(encoding='utf-8').strip()
        base = a['parents'][0] if a['parents'] else 'starter'
        start = (f'Copy bots/{base}/src into bots/{agent_id}/src. You are an immigrant: build a whole strategy '
                 f'from this archetype (keep only the infrastructure of the starting bot).\n\n### Archetype: '
                 f"{a['archetype']}\n{arche}")
        mutation = focus = ''
    else:
        p = a['parents'][0]
        start = f'Copy bots/{p}/src into bots/{agent_id}/src. Read bots/{p}/NOTES.md first, especially "Tried and failed".'
        if a['operator'] == 'crossover':
            start += (f'\nParent B is {a["parents"][1]}: read bots/{a["parents"][1]}/NOTES.md and its sources '
                      f'in bots/{a["parents"][1]}/src.')
        mutation = f"## Mutation: {OPERATOR_TITLES[a['operator']]}\n{fragment('operators', a['operator'])}"
        focus = f"## Focus: {a['focus']}\n{fragment('focus', a['focus'])}"
    ideas = a.get('failed_ideas') or []
    failed = ''
    if ideas:
        failed = ('## Ideas other agents tried this generation that did not survive\n'
                  + '\n'.join(f'- {i}' for i in ideas))
    text = fill((PROMPTS / 'agent.md').read_text(encoding='utf-8'), {
        'id': agent_id, 'generation': g, 'slot': a['slot'], 'env_manifest_sha256': manifest_sha,
        'game_budget': cfg['agents']['local_game_budget'], 'deadline_minutes': deadline,
        'starting_point': start, 'mutation': mutation, 'focus': focus,
        'evidence': evidence_table(a.get('evidence') or [], state['hall_of_fame']), 'failed_ideas': failed,
    })
    while '\n\n\n' in text:
        text = text.replace('\n\n\n', '\n\n')
    return text.encode('utf-8')


def prompt(agent_id):
    """The one-line prompt sent to the cloud session (§6)."""
    return f'You are evolution agent {agent_id}. Read evo/tasks/{agent_id}.md on main and follow it exactly.'


def write_all(cfg, state, agent_ids, root=ROOT):
    paths = []
    for agent_id in agent_ids:
        data = render(cfg, state, agent_id, root)
        rel = f'evo/tasks/{agent_id}.md'
        (root / rel).parent.mkdir(parents=True, exist_ok=True)
        (root / rel).write_bytes(data)
        state['agents'][agent_id]['task_sha256'] = hashlib.sha256(data).hexdigest()
        paths.append(rel)
    return paths

