"""Configuration: evo/config.toml (PLAN §3) plus an optional [run] section for file locations.

The [run] section lets a test run (M2) use its own ids and files next to a real run:
  id_prefix  'e'   -> agent ids eGG-SS        job_prefix 'g' -> job ids gGG-A
  state, lineage, cache, maptags  file paths      archetypes_dir   immigrant strategies
  launcher   'cloud' | 'fake'      max_generations  0 = unlimited
"""
import copy
import os
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_PATH = ROOT / 'evo' / 'config.toml'

RUN_DEFAULTS = {
    'id_prefix': 'e', 'job_prefix': 'g',
    'state': 'evo/state.json', 'lineage': 'evo/lineage.jsonl', 'cache': 'evo/cache.jsonl',
    'maptags': 'evo/prompts/map_tags.json', 'archetypes_dir': 'evo/prompts/archetypes',
    'launcher': 'cloud', 'max_generations': 0, 'step_interval_seconds': 300,
    'bootstrap_replicates': 200, 'results_timeout_hours': 8,
}
FOCUS_TAGS = {  # D26: which map tags make a focus area more likely after losses there
    'camps': ['many-camps', 'gem-clusters', 'gold-clusters'], 'combat': ['small', 'champs-5'],
    'spells': ['elk', 'golem'], 'pathing': ['large'], 'opening': ['small'], 'endgame': ['monolith', 'large'],
    'defense': ['champs-3', 'small'], 'fuel': ['large', 'many-camps'], 'map-adaptive': [],
}


def load(path=None):
    path = Path(path or os.environ.get('EVO_CONFIG') or DEFAULT_PATH)
    cfg = tomllib.loads(path.read_text(encoding='utf-8'))
    run = copy.deepcopy(RUN_DEFAULTS)
    run.update(cfg.get('run', {}))
    cfg['run'] = run
    cfg.setdefault('mutation', {}).setdefault('focus_tags', FOCUS_TAGS)
    cfg['_path'] = str(path)
    return cfg


def path(cfg, key):
    return ROOT / cfg['run'][key]


def agent_id(cfg, generation, slot):
    return f"{cfg['run']['id_prefix']}{generation:02d}-{slot:02d}"


def job_id(cfg, generation, stage, retry=0):
    base = f"{cfg['run']['job_prefix']}{generation:02d}-{stage}"
    return f'{base}-retry{retry}' if retry else base
