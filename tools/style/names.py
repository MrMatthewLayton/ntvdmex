#!/usr/bin/env python3
"""The names and types of docs/STYLE.md sections 1 and 2, checked. Report only: it never
changes a file.

    names.py [--summary] [FILE...]    every finding as FILE:LINE: RULE message;
                                      --summary prints only the count per rule
    names.py --strict [FILE...]       the same, and exit 1 if there is a finding

With no FILE: every C source and header git tracks under src/ and tests/ (sdk/ is the
third-party interface and keeps its <stdint.h> types).

  N1  function      a function's name is PascalCase
  N2  global        a file-scope variable is g_ + PascalCase
  N3  local         a local variable or parameter is camelCase, and longer than one letter
  N4  member        a structure member is PascalCase
  N5  type          Windows types: never uint8_t / int / unsigned / char / long / LPDWORD ...
  N6  width         a line is at most 100 columns
  N7  true          a BOOL is never compared with TRUE

It reads tokens, not a parse tree, so a finding is a strong hint rather than a proof; the
exceptions it knows are listed in ALLOWED below, each with its reason.
"""
import os
import re
import subprocess
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import restyle                                                   # noqa: E402
from restyle import Braces                                        # noqa: E402

ROOTS = ('src', 'tests')

EXPORTED = set()                                    # filled from the .def files by main()

GENERATED = {
    'src/vdd/opl_tables.h',
    'src/vdd/vbe_pm.h',
    'src/vdd/vga_defaults.h',
    'src/vdd/vga_modedefs.h',
}

# Names fixed by something outside the project: the C runtime, the linker, Windows.
ALLOWED = {
    'main',                                 # a test program's entry point
    'WinMain', 'DllMain',                   # Windows' entry points
    'WinMainCRTStartup', 'DllMainCRTStartup', 'mainCRTStartup',
    'memcpy', 'memset', 'memmove', 'memcmp', 'strlen',   # what GCC emits calls to (runtime.c)
    '__chkstk', '___chkstk_ms', '_alloca',
}

# Structures that copy a layout defined outside the project keep its member names (STYLE.md 7).
MIRRORS = {
    'src/vdm/ntvdm.h': "NTVDM's own interface structures",
    'src/vdd/audio_wave.c': "the waveIn/waveOut structures of mmsystem.h, declared by hand",
    'tests/probes/dos/isvtest/isvtest.c': "the VDD API's structures, as a third-party VDD sees them",
}

# Files that must use the C types: they define the Windows ones, or implement the C runtime's.
TYPE_DEFINERS = {
    'src/ntvdmex_types.h': 'defines the Windows types from the C types, for the off-machine build',
    'src/runtime.c': "implements memcpy and the rest with the C runtime's own signatures",
}

PASCAL = re.compile(r'^[A-Z][A-Za-z0-9]*$')
CAMEL = re.compile(r'^[a-z][A-Za-z0-9]*$')
GLOBAL = re.compile(r'^g_[A-Z][A-Za-z0-9]*$')

FORBIDDEN_TYPES = {
    'uint8_t': 'BYTE', 'uint16_t': 'WORD', 'uint32_t': 'DWORD', 'uint64_t': 'UINT64',
    'int8_t': 'INT8', 'int16_t': 'INT16', 'int32_t': 'INT32', 'int64_t': 'INT64',
    'size_t': 'SIZE_T', 'uintptr_t': 'ULONG_PTR', 'intptr_t': 'LONG_PTR',
    'int': 'INT', 'unsigned': 'UINT', 'void': 'VOID', 'char': 'CHAR', 'long': 'INT32',
    'short': 'INT16', 'float': 'FLOAT', 'bool': 'BOOL',
    'LPBYTE': 'PBYTE', 'LPWORD': 'PWORD', 'LPDWORD': 'PDWORD', 'LPVOID': 'PVOID',
    'LPCVOID': 'PCVOID', 'LPSTR': 'PSTR', 'LPCSTR': 'PCSTR', 'LPWSTR': 'PWSTR',
    'LPCWSTR': 'PCWSTR', 'LPBOOL': 'PBOOL', 'LPINT': 'PINT', 'LPLONG': 'PLONG',
}

TYPE_WORDS = restyle.TYPE_WORDS | set(FORBIDDEN_TYPES)
NOT_A_NAME = {'sizeof', 'return', 'if', 'for', 'while', 'switch', 'case', 'do', 'else', 'goto',
              'break', 'continue', 'default', 'typedef', 'struct', 'union', 'enum'}


def is_type_word(token):
    """A word that can start a declaration: a C type word, or a Windows-style type name."""
    if token in TYPE_WORDS:
        return True
    return bool(re.match(r'^(P?C?)[A-Z][A-Z0-9_]*$', token)) and token not in ('TRUE', 'FALSE', 'NULL')


class Findings:
    def __init__(self):
        self.items = []

    def add(self, path, line, rule, message):
        self.items.append((path, line, rule, message))


def line_of(text, position):
    return text.count('\n', 0, position) + 1


def in_directive_mask(text):
    """For each character: is it inside a preprocessor directive (with its continuations)?"""
    mask = [False] * len(text)
    position = 0
    for line in text.split('\n'):
        end = position + len(line)
        if line.lstrip().startswith('#'):
            for k in range(position, end):
                mask[k] = True
        position = end + 1
    # continuation lines
    for match in re.finditer(r'\\\n[^\n]*', text):
        if mask[match.start()]:
            for k in range(match.start(), match.end()):
                mask[k] = True
    return mask


def declarator_names(sig, start, end):
    """The names a declaration `start..end` (tokens, `;` excluded) declares, with positions:
    for each top-level declarator, the last identifier before its `=`, `[`, `:` or the end --
    or, for a function pointer, the name in `(*name)`. Initialisers are skipped."""
    names = []
    k = start
    while k < end:
        # one declarator: up to the next top-level comma
        depth = 0
        candidate = None
        while k < end:
            token, position = sig[k]
            if depth == 0 and token == ',':
                break
            if depth == 0 and token == '=':           # the initialiser: skip it
                level = 0
                while k + 1 < end:
                    t = sig[k + 1][0]
                    if t in '([{':
                        level += 1
                    elif t in ')]}':
                        level -= 1
                    elif t == ',' and level == 0:
                        break
                    k += 1
                k += 1
                continue
            if token == '(' and depth == 0 and k + 2 < end and sig[k + 1][0] == '*' \
                    and re.match(r'^[A-Za-z_]\w*$', sig[k + 2][0]):
                candidate = sig[k + 2]                  # (*name)(...)
            if token in '([{':
                if token in '[' and depth == 0 and candidate is None:
                    pass
                depth += 1
            elif token in ')]}':
                depth -= 1
            elif depth == 0 and token == ':':           # a bit field's width
                pass
            elif depth == 0 and re.match(r'^[A-Za-z_]\w*$', token) and not is_type_word(token) \
                    and token not in NOT_A_NAME and token not in ('const', 'volatile', 'static'):
                if candidate is None or not (sig[k - 1][0] == '*' and k >= 2 and sig[k - 2][0] == '('):
                    candidate = (token, position)
            k += 1
        if candidate:
            names.append(candidate)
        k += 1                                          # past the comma
    return names


def dedupe(names):
    seen = set()
    out = []
    for name in names:
        if name[1] not in seen:
            seen.add(name[1])
            out.append(name)
    return out


def check_file(path, text, findings):
    lines = text.split('\n')
    for number, line in enumerate(lines, 1):                              # N6
        if len(line) > 100:
            findings.add(path, number, 'N6', f'{len(line)} columns')

    directive = in_directive_mask(text)
    braces = Braces(text)
    sig = [(t, p) for t, p in braces.sig if not directive[p]]
    # bracket matching on the tokens outside directives
    stack = []
    match = {}
    for index, (token, _) in enumerate(sig):
        if token in '({[':
            stack.append(index)
        elif token in ')}]' and stack:
            opener = stack.pop()
            match[opener] = index
            match[index] = opener

    # what each `{` opens: a function body, a struct/union/enum, an initialiser, a block
    kind = {}
    for index, (token, _) in enumerate(sig):
        if token != '{':
            continue
        previous = sig[index - 1][0] if index else ''
        if previous == ')':
            opener = match.get(index - 1)
            before = sig[opener - 1][0] if opener else ''
            named = re.match(r'^[A-Za-z_]\w*$', before) and before not in NOT_A_NAME
            built = before == ')'                   # a name a macro builds: INT FN(Step)(...)
            kind[index] = 'function' if (opener is not None and (named or built)
                                         and depth_at(sig, opener) == 0) else 'block'
        elif previous in ('=', ',', '(', 'return') or (previous == '{' and kind.get(index - 1) == 'init'):
            kind[index] = 'init'
        elif (index >= 1 and sig[index - 1][0] in ('struct', 'union', 'enum')) or \
                (index >= 2 and sig[index - 2][0] in ('struct', 'union', 'enum')):
            kind[index] = 'enum' if 'enum' in (sig[index - 1][0], sig[index - 2][0]) else 'record'
        else:
            kind[index] = 'block'

    # the innermost enclosing `{` of each token
    enclosing_brace = []
    stack = []
    for index, (token, _) in enumerate(sig):
        enclosing_brace.append(stack[-1] if stack else None)
        if token == '{':
            stack.append(index)
        elif token == '}' and stack:
            stack.pop()

    def scope(index):
        """'file', 'record', 'enum', 'init', 'local' (inside a function)."""
        brace = enclosing_brace[index]
        while brace is not None:
            k = kind.get(brace)
            if k in ('record', 'enum', 'init'):
                return k
            if k == 'function':
                return 'local'
            brace = enclosing_brace[brace]
        return 'file'

    # N5 and N7 on every token
    for index, (token, position) in enumerate(sig):
        if token in FORBIDDEN_TYPES and path not in TYPE_DEFINERS:
            findings.add(path, line_of(text, position), 'N5', f'`{token}` -- use {FORBIDDEN_TYPES[token]}')
        if token == 'TRUE' and index and sig[index - 1][0] in ('==', '!='):
            findings.add(path, line_of(text, position), 'N7', 'compared with TRUE -- test the BOOL itself')
        if token == 'TRUE' and index + 1 < len(sig) and sig[index + 1][0] in ('==', '!='):
            findings.add(path, line_of(text, position), 'N7', 'compared with TRUE -- test the BOOL itself')

    # statements: walk them, at every brace level
    index = 0
    count = len(sig)
    while index < count:
        token, position = sig[index]
        starts_statement = index == 0 or sig[index - 1][0] in (';', '{', '}')
        if not starts_statement or token in ('}', ';'):
            index += 1
            continue
        where = scope(index)
        if where in ('init', 'enum'):
            index += 1
            continue
        # the statement's extent: to `;` at depth 0, or a `{` that opens a body
        k = index
        depth = 0
        body = None
        while k < count:
            t = sig[k][0]
            if t == '{' and depth == 0:
                body = k
                break
            if t in '([':
                depth += 1
            elif t in ')]':
                depth -= 1
            elif t == ';' and depth == 0:
                break
            k += 1
        tokens = [t for t, _ in sig[index:k]]
        if not tokens:
            index += 1
            continue
        first = tokens[0]
        if body is not None and kind.get(body) == 'function':
            check_function(path, text, sig, index, body, match, findings)
            index = body + 1
            continue
        if body is not None:
            # struct/union/enum definition, or a control statement's block: look inside
            if kind.get(body) == 'init' and first != 'typedef' and where == 'file':
                check_declaration(path, text, sig, index, body, where, findings)
            index = body + 1
            continue
        if first == 'typedef' or first in NOT_A_NAME or first in ('extern',) and len(tokens) > 1 \
                and tokens[1] == '"C"':
            if first == 'typedef' and where == 'record':
                pass
            index = k + 1
            continue
        if where in ('file', 'record', 'local') and looks_like_declaration(tokens):
            check_declaration(path, text, sig, index, k, where, findings)
        index = k + 1

    return findings


def depth_at(sig, index):
    depth = 0
    for token, _ in sig[:index]:
        if token == '{':
            depth += 1
        elif token == '}':
            depth -= 1
    return depth


def looks_like_declaration(tokens):
    words = [t for t in tokens if t not in ('static', 'const', 'volatile', 'extern', 'register',
                                             'inline', '__inline', 'struct', 'union', 'enum')]
    if len(words) < 2:
        return False
    if not is_type_word(words[0]) and not words[0].endswith('_t'):
        return False
    rest = words[1]
    if rest == '(':                                 # only a function pointer: TYPE (*name)(...)
        return len(words) > 5 and words[2] == '*' and re.match(r'^[A-Za-z_]\w*$', words[3]) \
            and words[4] == ')' and words[5] == '('
    return bool(re.match(r'^[A-Za-z_]\w*$', rest)) or rest == '*'


def check_declaration(path, text, sig, start, end, where, findings):
    tokens = [t for t, _ in sig[start:end]]
    if tokens[0] == 'extern':
        where = 'file'                              # a global or a function, declared where used
    if '(' in tokens:
        # a prototype `TYPE Name(...)`: its name is a function's; a function pointer is a variable
        paren = tokens.index('(')
        if paren + 1 < len(tokens) and tokens[paren + 1] != '*' and paren >= 1 \
                and re.match(r'^[A-Za-z_]\w*$', tokens[paren - 1]) and where == 'file':
            name, position = sig[start + paren - 1]
            if 'dllexport' not in tokens:
                check_function_name(path, text, name, position, findings)
            return
    if where == 'record' and path in MIRRORS:
        return
    for name, position in declarator_names(sig, start, end):
        check_variable(path, text, name, position, where, findings)


def check_function_name(path, text, name, position, findings):
    if name in ALLOWED or name in EXPORTED or PASCAL.match(name):
        return
    if re.match(r'^_*[A-Z][A-Z0-9_]*$', name) or name.startswith('__') or not re.match(r'^\w+$', name):
        return                                      # a macro invocation, or a compiler keyword
    findings.add(path, line_of(text, position), 'N1', f'function `{name}` -- PascalCase with its module prefix')


def check_variable(path, text, name, position, where, findings):
    line = line_of(text, position)
    if where == 'file':
        if not GLOBAL.match(name):
            findings.add(path, line, 'N2', f'global `{name}` -- g_ + PascalCase')
    elif where == 'record':
        if not PASCAL.match(name):
            findings.add(path, line, 'N4', f'member `{name}` -- PascalCase')
    elif where == 'local':
        if len(name) == 1:
            findings.add(path, line, 'N3', f'local `{name}` -- a single letter; name what it holds')
        elif not CAMEL.match(name):
            findings.add(path, line, 'N3', f'local `{name}` -- camelCase')


def check_function(path, text, sig, start, body, match, findings):
    # the name: the identifier before the parameter list's `(`
    close = body - 1
    opener = match.get(close)
    if opener is None or opener < 1:
        return
    name, position = sig[opener - 1]
    if 'dllexport' not in [t for t, _ in sig[start:opener]]:   # an export keeps its name (STYLE.md 7)
        check_function_name(path, text, name, position, findings)
    # parameters: each top-level comma-separated piece's last identifier
    pieces = []
    piece_start = opener + 1
    depth = 0
    for k in range(opener + 1, close):
        t = sig[k][0]
        if t in '([':
            depth += 1
        elif t in ')]':
            depth -= 1
        elif t == ',' and depth == 0:
            pieces.append((piece_start, k))
            piece_start = k + 1
    pieces.append((piece_start, close))
    for a, b in pieces:
        words = [sig[k] for k in range(a, b)]
        if not words or [w for w, _ in words] in (['VOID'], ['void'], ['...']):
            continue
        names = declarator_names(sig, a, b)
        if names:
            check_variable(path, text, names[-1][0], names[-1][1], 'local', findings)


def exported_names():
    """Every name a tracked .def file exports or imports: fixed by the program that links to it."""
    names = set()
    out = subprocess.check_output(['git', 'ls-files', '*.def'], text=True)
    for path in out.split():
        in_exports = False
        with open(path, encoding='utf-8', errors='replace') as handle:
            for line in handle:
                line = line.split(';')[0].strip()
                if re.match(r'^(EXPORTS|IMPORTS)\b', line):
                    in_exports = True
                    continue
                if re.match(r'^(LIBRARY|NAME|DESCRIPTION|SECTIONS|STACKSIZE|HEAPSIZE)\b', line):
                    in_exports = False
                    continue
                match = re.match(r'^([A-Za-z_]\w*)', line)
                if in_exports and match:
                    names.add(match.group(1))
    return names


def tracked_files():
    out = subprocess.check_output(['git', 'ls-files', '--', *ROOTS], text=True)
    return [p for p in out.split('\n') if p.endswith(('.c', '.h')) and p not in GENERATED]


def main(argv):
    summary = '--summary' in argv
    strict = '--strict' in argv
    paths = [a for a in argv if not a.startswith('--')] or tracked_files()
    EXPORTED.update(exported_names())
    findings = Findings()
    for path in paths:
        with open(path, encoding='utf-8', errors='replace') as handle:
            check_file(path, handle.read(), findings)
    counts = Counter(rule for _, _, rule, _ in findings.items)
    if not summary:
        for path, line, rule, message in findings.items:
            print(f'{path}:{line}: {rule} {message}')
    for rule in sorted(counts):
        print(f'names: {rule} {counts[rule]}', file=sys.stderr)
    print(f'names: {len(findings.items)} findings in {len(paths)} files', file=sys.stderr)
    return 1 if strict and findings.items else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
