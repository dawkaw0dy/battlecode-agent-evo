"""Map tags for focus weighting (D26), derived once from the map files with tools/mapgen.mjs.

Rules (camp counts are monster tiles; types follow cambc::camp_type: wolf, elk, gem cluster,
gold cluster, monolith, golem):
  small (area ≤ 1600) | medium | large (area ≥ 4900); champs-3/4/5;
  elk (≥ 5 elk tiles), gem-clusters (≥ 15), gold-clusters (≥ 15), monolith, golem (any);
  many-camps (≥ 45 camp tiles).
"""
import json
import subprocess

from evo.orchestrator.config import ROOT, path

WOLF, ELK, GEM, GOLD, MONOLITH, GOLEM = range(6)


def tags_for(info):
    area = info['w'] * info['h']
    camps = [c[0] for c in info['camps']]
    tags = ['small' if area <= 1600 else 'large' if area >= 4900 else 'medium', f"champs-{info['nc']}"]
    if camps.count(ELK) >= 5:
        tags.append('elk')
    if camps.count(GEM) >= 15:
        tags.append('gem-clusters')
    if camps.count(GOLD) >= 15:
        tags.append('gold-clusters')
    if MONOLITH in camps:
        tags.append('monolith')
    if GOLEM in camps:
        tags.append('golem')
    if len(camps) >= 45:
        tags.append('many-camps')
    return tags


def map_info(map_file):
    out = subprocess.run(['node', '--no-warnings', str(ROOT / 'tools/mapgen.mjs'), 'info', str(map_file)],
                         capture_output=True, text=True, check=True, cwd=ROOT).stdout
    return json.loads(out)


def build(cfg):
    tags = {p.stem: tags_for(map_info(p)) for p in sorted((ROOT / 'maps').glob('*.map26a'))}
    target = path(cfg, 'maptags')
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(json.dumps(tags, indent=1, sort_keys=True) + '\n', encoding='utf-8', newline='\n')
    return target, tags


def load(cfg):
    p = path(cfg, 'maptags')
    return json.loads(p.read_text(encoding='utf-8')) if p.exists() else {}
