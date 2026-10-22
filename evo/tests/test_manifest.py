import hashlib
import json
import subprocess

from evo import manifest


def make_repo(root):
    subprocess.run(['git', 'init', '-q', str(root)], check=True)
    files = {
        'GameRules.md': b'rules\n', 'BotAPICpp.md': b'api\n', 'patch-notes/patch100.txt': b'p\n',
        'cambc.toml': b'sdk_version = "9.9.9"\n', 'cmake/CMakeLists.txt': b'cmake\n',
        'sdk/cpp/cambc.h': b'#pragma once\n', 'maps/m.map26a': b'\x00\x01',
        'tools/benchmark.py': b'print()\n', 'bots/starter/src/main.cpp': b'int main(){}\n',
        'bots/starter/.clangd': b'x\n',
    }
    for rel, data in files.items():
        (root / rel).parent.mkdir(parents=True, exist_ok=True)
        (root / rel).write_bytes(data)
    (root / '.gitignore').write_bytes(b'ignored/\n')
    return files


def test_compute_hashes_groups_and_bots(tmp_path):
    files = make_repo(tmp_path)
    m = manifest.compute(tmp_path, 'cambc==9.9.9')
    assert m['sdk_version'] == '9.9.9'
    assert m['files']['sdk']['cmake/CMakeLists.txt'] == hashlib.sha256(files['cmake/CMakeLists.txt']).hexdigest()
    assert set(m['files']['maps']) == {'maps/m.map26a'}
    assert list(m['bots']) == ['starter']
    assert m['files']['env'] == {}


def test_write_is_canonical_and_detects_changes(tmp_path):
    make_repo(tmp_path)
    digest = manifest.write(tmp_path, 'cambc==9.9.9')
    data = (tmp_path / manifest.MANIFEST).read_bytes()
    assert hashlib.sha256(data).hexdigest() == digest
    assert manifest.write(tmp_path, 'cambc==9.9.9') == digest
    expected = json.loads(data)
    (tmp_path / 'tools' / 'stray.py').write_bytes(b'x')
    (tmp_path / 'maps' / 'm.map26a').write_bytes(b'\x00\x02')
    (tmp_path / 'ignored').mkdir()
    (tmp_path / 'ignored' / 'junk').write_bytes(b'x')
    diffs = manifest.differences(expected, manifest.compute(tmp_path, 'cambc==9.9.9'))
    assert diffs == ['maps: maps/m.map26a changed', 'tools: tools/stray.py unexpected']
