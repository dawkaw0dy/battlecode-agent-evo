"""Bot source allowlist (PLAN §8 steps 2 and 3, D13). Shared by admission and the Actions job check."""
import re
from pathlib import Path

ALLOWED_SUFFIXES = {'.cpp', '.h', '.hpp'}
MAX_FILES = 64
MAX_FILE_BYTES = 300 * 1024
MAX_TOTAL_BYTES = 1024 * 1024
SDK_HEADERS = {'cambc.h', 'imports.h', 'wasm_simd128.h'}
STD_HEADERS = frozenset(
    line.strip() for line in (Path(__file__).with_name('std_headers.txt')).read_text(encoding='utf-8').splitlines()
    if line.strip() and not line.startswith('#'))

# Preprocessor directive at the start of a logical line: '#', optional spaces, a name.
DIRECTIVE = re.compile(r'^\s*(?:#|%:)\s*([A-Za-z_]\w*)(.*)$')
INCLUDE_ARG = re.compile(r'^\s*(<([^<>]*)>|"([^"]*)")\s*(?://.*|/\*.*)?$')
FORBIDDEN_TOKENS = re.compile(r'\b(?:asm|__asm|__asm__|__has_include|__has_include_next|_Pragma|__pragma)\b')


def logical_lines(text):
    """Join backslash-continued lines, keeping the first physical line number."""
    out, buf, start = [], '', None
    for number, line in enumerate(text.split('\n'), 1):
        if start is None:
            start = number
        if line.endswith('\\'):
            buf += line[:-1]
            continue
        out.append((start, buf + line))
        buf, start = '', None
    if buf:
        out.append((start, buf))
    return out


def check_source(name, text, local_headers):
    """Problems in one source file's preprocessor use and inline assembly."""
    problems = []
    for number, line in logical_lines(text.replace('\r\n', '\n')):
        where = f'{name}:{number}'
        if '%:' in line and DIRECTIVE.match(line):
            problems.append(f'{where}: digraph directive')
        if FORBIDDEN_TOKENS.search(line):
            problems.append(f'{where}: forbidden token {FORBIDDEN_TOKENS.search(line).group(0)}')
        m = DIRECTIVE.match(line)
        if not m:
            continue
        directive, rest = m.group(1), m.group(2)
        if directive in ('embed', 'import', 'include_next'):
            problems.append(f'{where}: #{directive} is not allowed')
        elif directive == 'pragma':
            if rest.strip() != 'once':
                problems.append(f'{where}: #pragma other than once')
        elif directive == 'include':
            arg = INCLUDE_ARG.match(rest)
            if not arg:
                problems.append(f'{where}: #include must be <name> or "name"')
                continue
            header = arg.group(2) if arg.group(2) is not None else arg.group(3)
            if any(c in header for c in '/\\') or '..' in header or not header:
                problems.append(f'{where}: include path {header!r} is not a plain name')
            elif arg.group(2) is not None:
                if header not in STD_HEADERS and header not in SDK_HEADERS:
                    problems.append(f'{where}: <{header}> is not a standard or SDK header')
            elif header not in SDK_HEADERS and header not in local_headers:
                problems.append(f'{where}: "{header}" is neither an SDK header nor a file in src/')
    return problems


def check_bot(bot_dir):
    """All allowlist problems for bots/<id> (empty list = acceptable)."""
    src = Path(bot_dir) / 'src'
    if not src.is_dir() or src.is_symlink():
        return [f'{src}: missing or not a directory']
    problems, total = [], 0
    entries = sorted(src.iterdir())
    files = [p for p in entries if p.is_file() and not p.is_symlink()]
    for p in entries:
        if p.is_symlink():
            problems.append(f'{p.name}: symlink')
        elif p.is_dir():
            problems.append(f'{p.name}/: subdirectories are not allowed')
        elif not p.is_file():
            problems.append(f'{p.name}: not a regular file')
        elif p.suffix not in ALLOWED_SUFFIXES:
            problems.append(f'{p.name}: only .cpp, .h, .hpp are allowed')
    if len(files) > MAX_FILES:
        problems.append(f'{len(files)} files (max {MAX_FILES})')
    local = {p.name for p in files if p.suffix in ('.h', '.hpp')}
    for p in files:
        size = p.stat().st_size
        total += size
        if size > MAX_FILE_BYTES:
            problems.append(f'{p.name}: {size} bytes (max {MAX_FILE_BYTES})')
        if p.suffix in ALLOWED_SUFFIXES:
            try:
                text = p.read_bytes().decode('utf-8')
            except UnicodeDecodeError:
                problems.append(f'{p.name}: not UTF-8')
                continue
            problems += check_source(p.name, text, local)
    if total > MAX_TOTAL_BYTES:
        problems.append(f'{total} bytes in total (max {MAX_TOTAL_BYTES})')
    return problems
