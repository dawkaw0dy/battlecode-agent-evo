"""Source digest shared by tools/benchmark.py, admission and the Actions job check (PLAN §8.4)."""
import hashlib
from pathlib import Path


def digest(bot_dir):
    """sha256 over every file under bot_dir/src: POSIX relative path, then raw bytes.

    Paths are relative to bot_dir (e.g. ``src/main.cpp``) and always use ``/``,
    so Windows and Linux agree (DECISIONS D1).
    """
    bot_dir = Path(bot_dir)
    h = hashlib.sha256()
    for path in sorted((bot_dir / 'src').rglob('*')):
        if not path.is_file() or '__pycache__' in path.parts:
            continue
        h.update(path.relative_to(bot_dir).as_posix().encode())
        h.update(path.read_bytes())
    return h.hexdigest()
