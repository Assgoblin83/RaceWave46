#!/usr/bin/env python3
"""Check that libwr64game.so imports only symbols the host provides.

The host exports its globals with -rdynamic and links librecomp, ultramodern,
the C++ runtime and libc. The module is linked without those libraries, so a
missing hook only appears as a dlopen failure on the player's machine. This
check catches that at build time and keeps the committed import list honest.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
IMPORTS = ROOT / 'config/game-module-imports.txt'

# Runtime symbols supplied by libc, libm, libgcc_s and libstdc++, which the
# host process loads even though they are not in its own dynamic symbol table.
RUNTIME_SYMBOLS = {
    '_ITM_deregisterTMCloneTable',
    '_ITM_registerTMCloneTable',
    '_Unwind_Resume',
    '__cxa_finalize',
    '__cxa_guard_abort',
    '__cxa_guard_acquire',
    '__cxa_guard_release',
    '__gmon_start__',
    '__gxx_personality_v0',
    '__stack_chk_fail',
    'fegetround',
    'fesetround',
    'lrint',
    'lrintf',
    'memcpy',
    'memmove',
    'memset',
    'printf',
    'sqrt',
    'sqrtf',
    'strlen',
}


def symbols(path: Path, *arguments: str) -> set[str]:
    result = subprocess.run(
        ['nm', *arguments, '--format=posix', str(path)],
        capture_output=True, text=True, check=True)
    names = set()
    for line in result.stdout.splitlines():
        fields = line.split()
        if not fields:
            continue
        names.add(fields[0].split('@', 1)[0])
    return names


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', type=Path, required=True)
    parser.add_argument('--module', type=Path, required=True)
    parser.add_argument('--imports', type=Path, default=IMPORTS)
    args = parser.parse_args()

    exported = symbols(args.host, '-D', '--defined-only')
    undefined = symbols(args.module, '-u')
    expected = {line.strip() for line in args.imports.read_text().splitlines()
                if line.strip() and not line.startswith('#')}

    unresolved = undefined - exported - RUNTIME_SYMBOLS
    not_exported = expected - exported
    missing_from_list = (undefined & exported) - expected
    stale = expected - undefined

    failed = False
    if unresolved:
        print('The module imports symbols the host does not export:',
              *sorted(unresolved), sep='\n  ', file=sys.stderr)
        failed = True
    if not_exported:
        print('The host no longer exports committed module imports:',
              *sorted(not_exported), sep='\n  ', file=sys.stderr)
        failed = True
    if missing_from_list:
        print('New module imports are not in config/game-module-imports.txt:',
              *sorted(missing_from_list), sep='\n  ', file=sys.stderr)
        failed = True
    if stale:
        print('Committed module imports are no longer used:',
              *sorted(stale), sep='\n  ', file=sys.stderr)
        failed = True
    if failed:
        return 1
    print(f'Game module symbols verified: {len(expected)} host imports.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
