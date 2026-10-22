import hashlib

from evo.eval.digest import digest


def test_digest_uses_posix_paths_and_raw_bytes(tmp_path):
    src = tmp_path / 'bot' / 'src'
    src.mkdir(parents=True)
    (src / 'main.cpp').write_bytes(b'int main() {}\r\n')
    (src / 'a.h').write_bytes(b'#pragma once\n')
    expected = hashlib.sha256()
    for rel, data in [('src/a.h', b'#pragma once\n'), ('src/main.cpp', b'int main() {}\r\n')]:
        expected.update(rel.encode())
        expected.update(data)
    assert digest(tmp_path / 'bot') == expected.hexdigest()


def test_digest_skips_pycache_and_directories(tmp_path):
    src = tmp_path / 'bot' / 'src'
    (src / '__pycache__').mkdir(parents=True)
    (src / 'main.cpp').write_bytes(b'x')
    before = digest(tmp_path / 'bot')
    (src / '__pycache__' / 'junk.pyc').write_bytes(b'junk')
    (src / 'empty').mkdir()
    assert digest(tmp_path / 'bot') == before


def test_digest_changes_with_content(tmp_path):
    src = tmp_path / 'bot' / 'src'
    src.mkdir(parents=True)
    (src / 'main.cpp').write_bytes(b'x')
    before = digest(tmp_path / 'bot')
    (src / 'main.cpp').write_bytes(b'y')
    assert digest(tmp_path / 'bot') != before
