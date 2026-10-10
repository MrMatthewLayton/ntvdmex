#!/usr/bin/env python3
"""The house style of docs/STYLE.md, checked or applied.

    style.py --check [FILE...]    report every file not in the house style, with the diff;
                                  exit 1 if there is one
    style.py --fix [FILE...]      rewrite those files in place

With no FILE, every C source and header git tracks under src/, sdk/, tests/ and tools/.

A file goes through two stages, each a text -> text transformation:

  layout  restyle.py -- the file header, comments, braces, statements, blank lines, #defines
  order   order.py   -- the order of the file's items (STYLE.md 5a)

Every change is proven before it is kept. The layout stage must leave the comment-free code
token for token the same, except for one declaration a line (`INT a, b;` -> `INT a;` and
`INT b;`). The order stage must keep the same tokens, give or take whole forward declarations
it adds or drops. A file whose change cannot be proven is reported and never written: under
--check it fails, under --fix it is left as it was.

A file the order stage cannot parse (see order.py) keeps its order as written; that is not a
failure. Generated files are skipped.
"""
import difflib
import os
from concurrent.futures import ProcessPoolExecutor
import subprocess
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import order                                                     # noqa: E402
import restyle                                                   # noqa: E402

ROOTS = ('src', 'sdk', 'tests', 'tools')

GENERATED = {
    'src/vdd/opl_tables.h',
    'src/vdd/vbe_pm.h',
    'src/vdd/vga_defaults.h',
    'src/vdd/vga_modedefs.h',
}


class Unproven(Exception):
    """A stage changed the code in a way its proof does not allow."""


def tokens(text):
    return Counter(restyle.code_fingerprint(text).split())


def layout(text, path):
    new, proven, _notes = restyle.restyle_text(text, path)
    if not proven:
        raise Unproven('the layout stage changed the code')
    return new


def reorder(text, path):
    """The order stage, or the text as it was when the parser cannot place everything."""
    try:
        if path.endswith('.h'):
            new = order.reorder_header(text)
            dropped, added = [], []
        else:
            new, dropped, added = order.reorder_c(order.reorder_includes(text, path))
    except (ValueError, StopIteration):
        return text
    expected = tokens(text)
    for declaration in dropped:
        expected -= tokens(declaration)
    for declaration in added:
        expected += tokens(declaration)
    if tokens(new) != expected:
        raise Unproven('the order stage changed the code')
    return new


PASSES = 4


def style_text(text, path):
    """The file in the house style. Raises Unproven.

    The stages feed each other (items brought together by the order stage are aligned by the
    layout stage), so both run until a pass changes nothing."""
    for _ in range(PASSES):
        new = reorder(layout(text, path), path)
        if new == text:
            return text
        text = new
    raise Unproven(f'still changing after {PASSES} passes')


def tracked_files():
    out = subprocess.check_output(['git', 'ls-files', '--', *ROOTS], text=True)
    return [p for p in out.split('\n') if p.endswith(('.c', '.h')) and p not in GENERATED]


def examine(path):
    """(path, old text, new text, reason refused or None)."""
    with open(path, encoding='utf-8', newline='') as handle:
        old = handle.read()
    try:
        return path, old, style_text(old, path), None
    except Unproven as reason:
        return path, old, old, str(reason)


def main(argv):
    modes = [a for a in argv if a in ('--check', '--fix')]
    paths = [a for a in argv if not a.startswith('--')]
    unknown = [a for a in argv if a.startswith('--') and a not in ('--check', '--fix')]
    if len(modes) != 1 or unknown:
        print(__doc__.strip().split('\n\n')[0], file=sys.stderr)
        return 2
    fix = modes[0] == '--fix'
    if not paths:
        paths = tracked_files()
    paths = [p for p in paths if p not in GENERATED]
    results = None
    if len(paths) > 8:
        try:
            with ProcessPoolExecutor() as pool:
                results = list(pool.map(examine, paths, chunksize=4))
        except (OSError, NotImplementedError):
            results = None                          # no process pool here: one at a time
    if results is None:
        results = [examine(path) for path in paths]
    changed = 0
    refused = 0
    for path, old, new, reason in results:
        if reason:
            refused += 1
            print(f'{path}: REFUSED -- {reason}; fix this file by hand', file=sys.stderr)
            continue
        if new == old:
            continue
        changed += 1
        if fix:
            with open(path, 'w', encoding='utf-8', newline='') as handle:
                handle.write(new)
            print(f'{path}: restyled')
        else:
            sys.stdout.writelines(difflib.unified_diff(
                old.splitlines(keepends=True), new.splitlines(keepends=True),
                f'a/{path}', f'b/{path}'))
    total = len(paths)
    if fix:
        print(f'style: {changed} of {total} files restyled, {refused} refused', file=sys.stderr)
        return 1 if refused else 0
    if changed or refused:
        print(f'style: {changed} of {total} files are not in the house style, {refused} refused. '
              f'Run scripts/style.sh --fix', file=sys.stderr)
        return 1
    print(f'style: {total} files in the house style', file=sys.stderr)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
