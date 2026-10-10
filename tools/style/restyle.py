#!/usr/bin/env python3
"""The layout and comment rules of docs/STYLE.md, as text -> text transformations.

Used by style.py; not run on its own. restyle_text() applies, in order:
  1. header   -- the first comment becomes the standard file header: the project line, the
                 description (the old `name.h -- description`, filename dropped), the rest of
                 the old header, three blank comment lines, the SPDX licence and copyright.
  2. comments -- ASCII only: markers become [INFO]: / [CAUTION]: / [WARNING]:, box-drawing
                 banners become `TITLE (Importance = n):` (n = the stars), rule lines go,
                 every multi-line comment gets the ` * ` gutter with `*/` on its own line,
                 and the old hanging indents are flattened. Strings are never touched.
  3. braces   -- Allman: every block brace on its own line (initialisers excepted); a block
                 that opens and closes on one line is expanded onto lines of its own.
  3b. flow    -- one statement a line (log lines excepted), bodies on their own line, blank
                 lines between cases, around every block and after declarations, signatures.
  4. defines  -- #define values, and their trailing comments, aligned to a 4-column stop.
  5. spacing  -- a blank line after every function and before a comment that follows a
                 #define; at most one blank line in a row; no trailing whitespace.

Proof: the comment-free code, whitespace-collapsed, must be identical before and after
(string literals included) -- code_fingerprint(). Only split_declarators() changes tokens
(`INT a, b;` -> `INT a;` `INT b;`), and it runs first so that the proof covers the rest.
"""
import re, sys, os

PROJECT_LINE = 'NTVDMEX -- An NTVDM replacement for Microsoft Windows'
LICENCE = ['SPDX-License-Identifier: MIT', 'Copyright (c) 2026 Matthew Layton']

CHAR_MAP = {
    '—': '--', '–': '-', '×': 'x', '…': '...', '±': '+/-', '≈': '~',
    'µ': 'u', '→': '->', '←': '<-', '⇒': '=>', '·': '-', '•': '-',
    '►': '-', '▸': '-', '▶': '-', '✓': 'OK', '✅': 'OK', '©': '(c)',
    '∝': '~', 'Á': 'A', 'Â': 'A', 'À': 'A', '│': '|', '┃': '|',
    '╡': '|', '╢': '|', '╖': '+', '─': '-', '═': '=',
    '§': 'section ', '‘': "'", '’': "'", '“': '"', '”': '"', ' ': ' ',
}
STAR, CAUTION, WARNING, IMPLIES = '★', '⚠', '⛔', '⇒'
RULE_CHARS = '─═'
BULLETS = '►▸▶•'

# --- lexing --------------------------------------------------------------------------------------
def lex(text):
    """[(kind, start, end)] with kind in code/comment/linecomment/string; covers the text."""
    out = []; i = 0; n = len(text); start = 0
    def flush(k):
        if k > start: out.append(('code', start, k))
    while i < n:
        c = text[i]
        if text.startswith('/*', i):
            flush(i); k = text.find('*/', i + 2); k = n if k < 0 else k + 2
            out.append(('comment', i, k)); i = start = k
        elif text.startswith('//', i):
            flush(i); k = text.find('\n', i); k = n if k < 0 else k
            out.append(('linecomment', i, k)); i = start = k
        elif c in '"\'':
            flush(i); j = i + 1
            while j < n and text[j] != c and text[j] != '\n': j += 2 if text[j] == '\\' else 1
            out.append(('string', i, j + 1)); i = start = j + 1
        else:
            i += 1
    flush(n)
    return out

TOKEN = re.compile(r'[A-Za-z_]\w*|\d[\w.]*|->|\+\+|--|<<=|>>=|[-+*/%&|^!=<>]=|&&|\|\||<<|>>|##|\S')

def code_fingerprint(text):
    """The code as tokens: comments gone, strings whole, whitespace ignored -- except that a
    newline still ends a preprocessor directive, so directives keep their line structure."""
    out = []; at_line_start = True; in_directive = False
    for kind, a, b in lex(text):
        piece = text[a:b]
        if kind in ('comment', 'linecomment'):
            continue
        if kind == 'string':
            out.append(piece); at_line_start = False; continue
        for line_no, line in enumerate(re.split(r'(?<!\\)\n', piece)):
            if line_no > 0:
                if in_directive: out.append('<EOL>')
                in_directive = False; at_line_start = True
            stripped = line.replace('\\\n', ' ')
            if at_line_start and stripped.lstrip().startswith('#'): in_directive = True
            toks = TOKEN.findall(stripped)
            if toks: at_line_start = False
            out.extend(toks)
    return ' '.join(out)

def map_chars(s):
    return ''.join(CHAR_MAP.get(ch, ch) for ch in s)

# --- 2. comment bodies ---------------------------------------------------------------------------
BANNER = re.compile(r'^[' + RULE_CHARS + r'\s]*[' + RULE_CHARS + r']{2,}\s*([' + STAR + CAUTION + WARNING + r']*)\s*(.*?)\s*[' + RULE_CHARS + r']{2,}[' + RULE_CHARS + r'\s]*$')
RULE_ONLY = re.compile(r'^[\s' + RULE_CHARS + r'=\-]*[' + RULE_CHARS + r']{3,}[\s' + RULE_CHARS + r'=\-]*$')

def title_line(symbols, title):
    """A banner's title. The symbols in front of it are its importance (one per symbol); a
    warning-sign banner is also a caution, a no-entry banner a warning."""
    inside = ''.join(ch for ch in title if ch in STAR + CAUTION + WARNING)   # "(5) ★★★ TITLE"
    if inside:
        symbols += inside
        title = re.sub(r'[' + STAR + CAUTION + WARNING + r']+\s*', '', title)
    title = title.rstrip().rstrip('.:').rstrip()
    tag ='[WARNING]: ' if WARNING in symbols else '[CAUTION]: ' if CAUTION in symbols else ''
    title = tag + map_chars(title)
    return f'{title} (Importance = {len(symbols)}):' if symbols else f'{title}:'

def convert_line(text):
    """One line of comment text (gutter and indent already removed): markers and banners."""
    m = BANNER.match(text)
    if m and m.group(2):
        return title_line(m.group(1), m.group(2))

    if RULE_ONLY.match(text) and not re.search(r'[A-Za-z0-9]', text):
        return None                                     # a pure rule line goes
    lead = ''
    m = re.match(r'^(' + STAR + r'+|' + CAUTION + r'+|' + WARNING + r'+)\s*', text)
    if m:
        sym = m.group(1)[0]
        lead = {STAR: '[INFO]: ', CAUTION: '[CAUTION]: ', WARNING: '[WARNING]: '}[sym]
        text = text[m.end():]
        # a starred heading inside a body: "★★ TITLE. text..." stays a marker line
    elif text[:1] in BULLETS:
        text = '- ' + text[1:].lstrip()
    # symbols in the middle of a line
    text = re.sub(STAR + r'+\s*', '', text)
    text = re.sub(CAUTION + r'+\s*', '[CAUTION] ', text)
    text = re.sub(WARNING + r'+\s*', '[WARNING] ', text)
    return map_chars(lead + text).rstrip()

def body_lines(raw, first_col):
    """The comment's text lines without the /* */, the gutter or the old indentation."""
    inner = raw[2:-2] if raw.endswith('*/') else raw[2:]
    lines = inner.split('\n')
    # an old ASCII-art box (`*  text   *` on most lines): its right border and its rules go
    bordered = [l for l in lines if re.search(r'\S\s{2,}\*\s*$', l) or re.match(r'^\s*\*?\s*[=*-]{5,}\s*\*?\s*$', l)]
    if len(lines) > 2 and len(bordered) >= max(2, (len(lines) - 1) * 0.6):
        lines = [re.sub(r'\s+\*\s*$', '', l) if re.search(r'\S\s{2,}\*\s*$', l) else l for l in lines]
        lines = [l for l in lines if not re.match(r'^\s*\*?\s*[=*-]{5,}\s*\*?\s*$', l)]
        lines = [re.sub(r'^\s*\*\s*$', ' *', l) for l in lines]
        if not lines: lines = ['']
        lines[0] = re.sub(r'^\s*\*\s*', '', lines[0])     # the first text line came from inside the box
    gutter = len(lines) > 1 and all(re.match(r'^\s*\*( |$)', l) or not l.strip() for l in lines[1:] if l.strip() != '')
    if gutter and len(lines) > 1 and all((re.match(r'^\s*\*', l) or not l.strip()) for l in lines[1:]):
        rest = [re.sub(r'^\s*\* ?', '', l) if l.strip() else '' for l in lines[1:]]
    else:
        rest = [l if l.strip() else '' for l in lines[1:]]
    first = lines[0]
    if first.startswith(' '): first = first[1:]
    # Indentation. A comment in the OLD style (it still has a banner or the old symbols) hung
    # its text two columns under its title and under each marker: that hang is flattened. Any
    # other comment keeps its indentation as written, relative to its least-indented line --
    # which is also what makes a second run change nothing.
    indents = [len(l) - len(l.lstrip()) for l in rest if l.strip()]
    base = min(indents) if indents else 0
    old_style = any(ch in raw for ch in STAR + CAUTION + WARNING + IMPLIES + RULE_CHARS + BULLETS)
    level = lambda d: 0 if d <= base + 2 else d - base - 2
    first = first.strip()
    m = re.match(r'^-{2,}\s+(.*)$', first)            # an ASCII banner opening the comment: --- title ---
    if m:
        banner = m.group(1)
        t = re.match(r'^(.*?[^\s-])\s+-{1,}\s*\*?$', banner)
        first = title_line('', t.group(1)) if t else re.sub(r'\s*\*$', '', banner).rstrip()
    out = [first]
    marker = None                                   # (indent, new indent, extra for continuations)
    for l in rest:
        if not l.strip(): out.append(''); marker = None; continue
        d = len(l) - len(l.lstrip()); t = l.strip()
        if not old_style:
            new = d - base
        elif t[:1] in STAR + CAUTION + WARNING + IMPLIES:
            new = level(d); marker = (d, new, 0)      # its continuation lines join it
        elif t[:1] in BULLETS:
            new = level(d); marker = (d, new, 2)      # a list item's continuation stays under its text
        elif marker is not None and d == marker[0] + 2:
            new = marker[1] + marker[2]
        else:
            new = level(d)
            if marker is not None and d <= marker[0]: marker = None
        out.append(' ' * new + t)
    while out and out[-1] == '': out.pop()
    while len(out) > 1 and out[0] == '': out.pop(0)
    return out

MARKERS = ('[INFO]:', '[CAUTION]:', '[WARNING]:')

def wrap(words, width, first_prefix, next_prefix):
    out = []; line = first_prefix
    for w in words:
        if line.strip() and len(line) + 1 + len(w) > width:
            out.append(line.rstrip()); line = next_prefix + w
        else:
            line = (line + ' ' + w) if line.strip() else line + w
    if line.strip(): out.append(line.rstrip())
    return out

def convert_body(lines, width):
    """Comment text lines -> converted lines: markers, banners, `=>` paragraphs, rule lines
    dropped, a blank line before every marker paragraph, no doubled blank lines, and a marker
    paragraph that no longer fits `width` rewrapped (its line breaks are kept otherwise)."""
    converted = []
    for l in lines:
        if not l.strip(): converted.append(''); continue
        lead = len(l) - len(l.lstrip())
        t = l.strip()
        if t.startswith(IMPLIES):                         # "=> conclusion": its own paragraph
            if converted and converted[-1] != '': converted.append('')
            t = t[1:].lstrip()
        c = convert_line(t)
        if c is None: continue
        if lead == 0 and c.startswith(MARKERS) and converted and converted[-1] != '':
            converted.append('')                          # a marker starts a paragraph
        converted.append(' ' * lead + c)
    while converted and converted[-1] == '': converted.pop()
    while converted and converted[0] == '': converted.pop(0)
    collapsed = []
    for l in converted:
        if l == '' and collapsed and collapsed[-1] == '': continue
        collapsed.append(l)
    # rewrap a marker paragraph that overflows
    out = []; k = 0
    while k < len(collapsed):
        l = collapsed[k]
        if l.startswith(MARKERS):
            j = k + 1
            while j < len(collapsed) and collapsed[j] and not collapsed[j][:1].isspace() and not collapsed[j].startswith(MARKERS) and not collapsed[j].startswith('- '):
                j += 1
            para = collapsed[k:j]
            if any(len(x) > width for x in para):
                para = wrap(' '.join(para).split(), width, '', '')
            out.extend(para); k = j
        else:
            out.append(l); k += 1
    return out

def render_comment(lines, indent):
    """Rebuild a standalone comment: one line if it fits one line, else the gutter form."""
    collapsed = convert_body(lines, 100 - len(indent) - 3)
    if not collapsed: return None
    if len(collapsed) == 1:
        return f'{indent}/* {collapsed[0]} */'
    return '\n'.join([f'{indent}/* {collapsed[0]}'] + [f'{indent} * {l}'.rstrip() for l in collapsed[1:]] + [f'{indent} */'])

def restyle_comments(text, path):
    toks = lex(text); out = []; pos = 0; trailing_multiline = 0
    for kind, a, b in toks:
        if kind == 'linecomment':
            out.append(text[pos:a]); out.append(map_chars(text[a:b].replace(STAR, '').replace(CAUTION, '[CAUTION]').replace(WARNING, '[WARNING]'))); pos = b; continue
        if kind != 'comment': continue
        line_start = text.rfind('\n', 0, a) + 1
        line_end = text.find('\n', b); line_end = len(text) if line_end < 0 else line_end
        before = text[line_start:a]; after = text[b:line_end]
        raw = text[a:b]
        if 'SPDX-License-Identifier' in raw:              # the file header is already in shape
            continue
        if before.strip() == '' and after.strip() == '':
            new = render_comment(body_lines(raw, len(before)), before)
            out.append(text[pos:line_start])
            if new is None:                               # nothing left (a pure rule): drop the line
                pos = line_end + 1 if line_end < len(text) else line_end
                continue
            out.append(new); pos = b
        else:                                             # a trailing or inline comment: characters only
            if '\n' in raw: trailing_multiline += 1
            out.append(text[pos:a])
            inner = raw[2:-2]
            inner = re.sub(r'(' + STAR + r')+\s*', '', inner)
            inner = re.sub(CAUTION + r'+\s*', '[CAUTION] ', inner)
            inner = re.sub(WARNING + r'+\s*', '[WARNING] ', inner)
            if '\n' not in inner: inner = inner.rstrip() + ' '
            out.append('/*' + map_chars(inner) + '*/'); pos = b
    out.append(text[pos:])
    return ''.join(out), trailing_multiline

# --- 1. the file header --------------------------------------------------------------------------
def restyle_header(text, path=''):
    """Returns (text, note). The header comment is the one whose first line names this file
    (`name.h -- ...`), or else the file's first comment if nothing but an include guard and
    #includes stand before it. It moves to the top; whatever stood above it follows it."""
    toks = lex(text)
    comments = [t for t in toks if t[0] == 'comment']
    if not comments: return text, 'no header comment'
    base = os.path.basename(path)
    named = [t for t in comments[:12]
             if base and re.match(r'^/\*[\s*]*' + re.escape(base) + r'\s+(--|\u2014|-)\s', text[t[1]:t[2]])]
    if named:
        a, b = named[0][1], named[0][2]
    else:
        a, b = comments[0][1], comments[0][2]
        if text[:a].strip() and not re.match(r'^\s*(#ifndef\s+\w+\s*\n\s*#define\s+\w+[^\n]*\n)(\s*#include[^\n]*\n)*\s*$', text[:a]):
            return text, 'header comment is not first'
    # take the header comment out (with its line); what was above it follows it
    line_start = text.rfind('\n', 0, a) + 1
    line_end = text.find('\n', b); line_end = len(text) if line_end < 0 else line_end + 1
    prefix = text[:line_start]; after_header = text[line_end:]
    guard = prefix.strip('\n') + '\n' if prefix.strip() else ''
    lines = body_lines(text[a:b], 0)
    if any('SPDX-License-Identifier' in l for l in lines): return text, None   # already done
    first = lines[0]
    m = re.match(r'^(?:\*\s*)?[\w./-]+\.(?:h|c|S|asm|inc)\s+(?:--|—|-)\s+(.*)$', first)
    note = None
    if m: first = m.group(1)
    elif first.startswith(PROJECT_LINE.split(' ')[0]) or first.startswith('NTDVMEX'):
        lines = lines[1:]
        while lines and lines[0] == '': lines = lines[1:]
        first = lines[0] if lines else ''
    else:
        note = 'no "name -- description" first line (kept as written)'
    first = re.sub('^[' + STAR + CAUTION + WARNING + r']+\s*', '', first)
    first = first[:1].upper() + first[1:]
    rest = lines[1:]
    # a blank line after the description sentence, unless the sentence runs on
    if rest and rest[0] != '' and first.rstrip().endswith('.') and rest[0][:1].isupper():
        rest = [''] + rest
    elif rest and rest[0] != '' and not (first.rstrip().endswith('.') or rest[0][:1] in '(' or rest[0][:1].islower()):
        pass
    if 'MIT License' in rest:                         # a full licence text gives way to SPDX
        k = rest.index('MIT License')
        end = next((j for j in range(k, len(rest)) if rest[j].rstrip().endswith('SOFTWARE.')), len(rest) - 1)
        rest = rest[:k] + rest[end + 1:]
    body = [first] + rest
    converted = convert_body(body, 97)
    rendered = '\n'.join(['/* ' + PROJECT_LINE, ' *'] + [(' * ' + l).rstrip() for l in converted]
                          + [' *', ' *', ' *'] + [' * ' + l for l in LICENCE] + [' */'])
    new = rendered + '\n\n' + guard + after_header if guard else rendered + '\n\n' + after_header.lstrip('\n')
    return new, note

# --- 3. Allman braces ----------------------------------------------------------------------------
KEYWORDS_BLOCK = {'if', 'for', 'while', 'switch'}

class Braces:
    """The code's significant tokens, their bracket matching, and which braces are blocks."""
    def __init__(self, text):
        self.text = text
        is_code = [False] * len(text)
        for kind, a, b in lex(text):
            if kind == 'code':
                for k in range(a, b): is_code[k] = True
        in_pp = [False] * len(text); k = 0
        while k < len(text):                      # preprocessor lines (and continuations)
            ls = k; le = text.find('\n', k); le = len(text) if le < 0 else le
            line = text[ls:le]
            if line.strip() and line.lstrip().startswith('#') and is_code[ls + len(line) - len(line.lstrip())]:
                e = le
                while e < len(text) and e > 0 and text[e - 1] == '\\':
                    ne = text.find('\n', e + 1); e = len(text) if ne < 0 else ne
                for q in range(ls, e): in_pp[q] = True
                k = e + 1
            else:
                k = le + 1
        self.sig = [(m.group(), m.start()) for m in TOKEN.finditer(text) if is_code[m.start()] and not in_pp[m.start()]]
        stack = []; self.match = {}
        for idx, (t, p) in enumerate(self.sig):
            if t in '({[': stack.append(idx)
            elif t in ')}]' and stack:
                o = stack.pop(); self.match[o] = idx; self.match[idx] = o
        self.kind = {}
        for idx, (t, p) in enumerate(self.sig):
            if t != '{': continue
            prev = self.sig[idx - 1][0] if idx else ';'
            if prev in ('=', ',', 'return', '(', '['):
                self.kind[idx] = 'init'
            elif prev == '{':
                self.kind[idx] = 'init' if self.kind.get(idx - 1) == 'init' else 'block'
            elif prev == ')':
                o = self.match.get(idx - 1)
                before = self.sig[o - 1][0] if o is not None and o > 0 else ''
                self.kind[idx] = 'init' if (before in ('=', ',', 'return', '(') or before in '+-*/%&|^!<>?:') else 'block'
            else:
                self.kind[idx] = 'block'
    def indent_at(self, p):
        ls = self.text.rfind('\n', 0, p) + 1
        line = self.text[ls:]
        return line[:len(line) - len(line.lstrip(' \t'))]
    def statement_indent(self, idx):
        if idx and self.sig[idx - 1][0] == ')':
            o = self.match.get(idx - 1)
            if o is not None: return self.indent_at(self.sig[o][1])
        return self.indent_at(self.sig[idx][1])
    def blocks(self):
        return [(idx, self.match[idx]) for idx, (t, p) in enumerate(self.sig)
                if t == '{' and self.kind.get(idx) == 'block' and idx in self.match]

def apply_edits(text, edits):
    edits.sort(key=lambda e: e[0], reverse=True)
    last = len(text) + 1
    for a, b, ins in edits:
        if b > last: continue
        text = text[:a] + ins + text[b:]; last = a
    return text

DECLARATORS = re.compile(r'^[\w\s,*\[\]]*;')

def expand_one_line_blocks(text):
    """One pass: every outermost block that opens and closes on one line goes onto lines of
    its own -- statement, `{`, the body one level in, `}`, and what followed on its own line."""
    br = Braces(text); sig = br.sig; edits = []; covered_until = -1; count = 0
    for idx, close in br.blocks():
        p, q = sig[idx][1], sig[close][1]
        if p < covered_until or '\n' in text[p:q]: continue
        ls = text.rfind('\n', 0, p) + 1
        le = text.find('\n', q); le = len(text) if le < 0 else le
        before = text[ls:p]; body = text[p + 1:q].strip(); after = text[q + 1:le].strip()
        ind = br.statement_indent(idx) if before.strip() else before
        head = before.rstrip()
        comment = ''
        if after.startswith('/*') and after.endswith('*/') and '*/' not in after[2:-2]:
            comment, after = after, ''
        if before.strip():
            out = head + (' ' + comment if comment else '') + '\n' + ind + '{\n'
        else:
            out = before + '{' + (' ' + comment if comment else '') + '\n'
        # the body's statements, one per line: on one line, `if (x) a; return b;` would read
        # as if the `if` guarded both (GCC says so: -Wmisleading-indentation)
        cuts = []; depth = 0
        for j in range(idx + 1, close):
            t, tp = sig[j]
            if t in '([{': depth += 1
            elif t in ')]}':
                depth -= 1
                if t == '}' and depth == 0 and sig[j + 1][0] not in (';', 'else', 'while') and j + 1 < close:
                    cuts.append(tp + 1)
            elif t == ';' and depth == 0 and j + 1 < close:
                cuts.append(tp + 1)
        pieces = []; start = p + 1
        for c in cuts + [q]:
            piece = text[start:c].strip()
            if piece: pieces.append(piece)
            start = c
        for piece in pieces:
            out += ind + '    ' + piece + '\n'
        out += ind + '}'
        if after:
            if after.startswith(';') or after.startswith('while') or DECLARATORS.match(after):
                out += ('' if after.startswith(';') else ' ') + after
            else:
                out += '\n' + ind + after              # `else ...` or the next statement
        edits.append((ls, le, out)); covered_until = q; count += 1
    return apply_edits(text, edits), count

def restyle_braces(text, expand_one_line=True):
    """Allman. Returns (text, one-line blocks left)."""
    if expand_one_line:
        for _ in range(40):
            text, count = expand_one_line_blocks(text)
            if not count: break
    br = Braces(text); sig = br.sig; edits = []; left = 0
    for idx, (t, p) in enumerate(sig):
        if t == '{' and br.kind.get(idx) == 'block':
            close = br.match.get(idx)
            ls = text.rfind('\n', 0, p) + 1
            if close is not None and '\n' not in text[p:sig[close][1]]:
                left += 1; continue                    # a one-line block that could not be expanded
            if text[ls:p].strip():                     # code before the brace on its line
                ws = p
                while ws > ls and text[ws - 1] in ' \t': ws -= 1
                ind = br.statement_indent(idx)
                le = text.find('\n', p); le = len(text) if le < 0 else le
                rest = text[p + 1:le]
                if rest.strip().startswith('/*') and rest.strip().endswith('*/') and '*/' not in rest.strip()[2:-2]:
                    # the comment keeps its column: the brace and its space become padding
                    edits.append((ws, le, ' ' * (p - ws + 1) + rest.rstrip() + '\n' + ind + '{'))
                elif rest.strip() == '':
                    edits.append((ws, p + 1, '\n' + ind + '{'))
        if t == 'else' and idx and sig[idx - 1][0] == '}':
            q = sig[idx - 1][1]
            if text.rfind('\n', 0, p) < q:              # `} else` on one line
                edits.append((q + 1, p, '\n' + br.indent_at(q)))
    return apply_edits(text, edits), left

def same_line(text, a, b):
    return '\n' not in text[a:b]

# --- 3b. statements, bodies, signatures ----------------------------------------------------------
def line_start_of(text, p): return text.rfind('\n', 0, p) + 1
def line_end_of(text, p):
    k = text.find('\n', p); return len(text) if k < 0 else k
def indent_of(text, p):
    ls = line_start_of(text, p); line = text[ls:line_end_of(text, p)]
    return line[:len(line) - len(line.lstrip(' \t'))]
def same_line_code_after(text, p):
    """The rest of p's line after p, if it holds code (not just a comment)."""
    rest = text[p:line_end_of(text, p)].strip()
    if not rest: return False
    if rest.startswith('/*') and rest.endswith('*/') and '*/' not in rest[2:-2]: return False
    if rest.startswith('//'): return False
    return True

def enclosing(br):
    """For every token index: the index of the innermost enclosing `{`, and the paren depth."""
    sig = br.sig; encl = []; parens = []; bstack = []; depth = 0
    for idx, (t, p) in enumerate(sig):
        encl.append(bstack[-1] if bstack else None); parens.append(depth)
        if t == '{': bstack.append(idx)
        elif t == '}' and bstack: bstack.pop()
        elif t in '([': depth += 1
        elif t in ')]': depth -= 1
    return encl, parens

def in_block(br, encl, idx):
    e = encl[idx]
    return e is not None and br.kind.get(e) == 'block'

LOG_CALL = re.compile(r'\b(Log[A-Z]\w*|SerialOut)\s*\(')

def is_log_line(text, p):
    """Every statement on p's line writes the log (a Log* or SerialOut call), or only resets a
    cursor (`cursor = base;`): the house idiom of one log field per line, kept together."""
    ls = line_start_of(text, p); line = text[ls:line_end_of(text, p)]
    code = re.sub(r'/\*.*?\*/|//.*$', '', line)
    parts = [x.strip() for x in code.split(';') if x.strip()]
    return len(parts) > 1 and any(LOG_CALL.search(x) for x in parts) and all(
        LOG_CALL.search(x) or re.match(r'^\**\w+\s*=\s*\w+$', x) for x in parts)

def split_statements(text):
    """One statement per line (log lines excepted); a case label alone on its line."""
    br = Braces(text); sig = br.sig; encl, parens = enclosing(br); edits = []
    label_lines = set()                     # lines whose case label is split off in this pass
    for idx, (t, p) in enumerate(sig):
        if not in_block(br, encl, idx) or parens[idx] != 0: continue
        if t == ';' and idx + 1 < len(sig):
            nt, np_ = sig[idx + 1]
            if nt == '}' or '\n' in text[p:np_]: continue
            if not same_line_code_after(text, p + 1): continue
            if is_log_line(text, p): continue
            # after a label split in this pass the statement moves one level in, so its
            # indent is not yet known: the next pass splits it
            if line_start_of(text, p) in label_lines: continue
            edits.append((p + 1, np_, '\n' + indent_of(text, p)))
        elif t in ('case', 'default') and (idx == 0 or sig[idx - 1][0] in (';', '{', '}', ':')):
            j = idx + 1
            while j < len(sig) and not (sig[j][0] == ':' and parens[j] == parens[idx]): j += 1
            if j + 1 >= len(sig): continue
            colon = sig[j][1]; nt, np_ = sig[j + 1]
            if '\n' in text[colon:np_] or nt == '}': continue
            if not same_line_code_after(text, colon + 1): continue
            extra = '' if nt in ('case', 'default') else '    '
            edits.append((colon + 1, np_, '\n' + indent_of(text, p) + extra))
            label_lines.add(line_start_of(text, p))
    return apply_edits(text, edits), len(edits)

def move_bodies(text):
    """A single-statement body of if/for/while/else, on the keyword's line, goes to the next."""
    br = Braces(text); sig = br.sig; edits = []; multi = 0; lines_done = set()
    for idx, (t, p) in enumerate(sig):
        body = None
        if t in ('if', 'for', 'while') and idx + 1 < len(sig) and sig[idx + 1][0] == '(':
            close = br.match.get(idx + 1)
            if close is None or close + 1 >= len(sig): continue
            if t == 'while' and idx and sig[idx - 1][0] == '}': continue      # do ... while
            body = close + 1
        elif t == 'else' and idx + 1 < len(sig) and sig[idx + 1][0] not in ('{', 'if'):
            body = idx + 1
        if body is None: continue
        bt, bp = sig[body]
        if bt in ('{', ';'): continue
        anchor = sig[body - 1][1] + len(sig[body - 1][0]) - 1   # the last character before the body
        if '\n' in text[anchor:bp]: continue                    # already on its own line
        ls = line_start_of(text, p)
        if ls in lines_done: continue                            # one level a pass: nested bodies next time

        # the statement's end: the `;` at its own depth (a nested control keeps its own body)
        depth = 0; end = None
        for j in range(body, len(sig)):
            tj = sig[j][0]
            if tj in '([{': depth += 1
            elif tj in ')]}': depth -= 1
            elif tj == ';' and depth == 0: end = j; break
            if depth < 0: break
        if end is None: continue
        if '\n' in text[bp:sig[end][1]]:
            multi += 1; continue                                 # a body over several lines: leave it
        new_indent = indent_of(text, p) + '    '
        edits.append((anchor + 1, bp, '\n' + new_indent)); lines_done.add(ls)
        # a comment after the statement keeps its column if it can
        se = sig[end][1] + 1; le = line_end_of(text, se); rest = text[se:le]
        if rest.strip().startswith('/*') and rest.strip().endswith('*/'):
            column = se - line_start_of(text, se) + len(rest) - len(rest.lstrip())
            new_end = len(new_indent) + (se - bp)
            pad = column - new_end if column - new_end >= 1 else 1
            edits.append((se, se + len(rest) - len(rest.lstrip()), ' ' * pad))
    return apply_edits(text, edits), len(edits), multi

def blank_between_cases(text):
    lines = text.split('\n')
    mask = [False] * len(text)
    for kind, a, b in lex(text):
        if kind == 'code':
            for k in range(a, b): mask[k] = True
    starts = [0]
    for l in lines[:-1]: starts.append(starts[-1] + len(l) + 1)
    out_insert = set()
    for i, l in enumerate(lines):
        s = l.lstrip()
        if not (s.startswith('case ') or s.startswith('case(') or s.startswith('default:') or s.startswith('default :')): continue
        if not mask[starts[i] + len(l) - len(s)]: continue
        j = i - 1
        while j >= 0 and (lines[j].lstrip().startswith(('/*', '*', '//')) and lines[j].strip()):
            j -= 1                                              # its comment goes with it
        if j < 0: continue
        prev = lines[j].strip()
        if prev == '' or prev.endswith('{') or prev == '{': continue
        if (prev.startswith('case ') or prev.startswith('default')) and prev.endswith(':'): continue
        out_insert.add(j + 1)
    if not out_insert: return text, 0
    out = []
    for i, l in enumerate(lines):
        if i in out_insert: out.append('')
        out.append(l)
    return '\n'.join(out), len(out_insert)

TYPE_WORDS = {'const', 'volatile', 'static', 'unsigned', 'signed', 'struct', 'enum', 'union', 'register',
              'char', 'int', 'long', 'short', 'float', 'double', 'void', 'extern', 'auto', '_Bool', 'inline'}

def is_declaration(tokens):
    if len(tokens) < 2: return False
    t0, t1 = tokens[0], tokens[1]
    if t0 in TYPE_WORDS: return True
    if re.match(r'^[A-Z][A-Z0-9_]*$', t0) or t0.endswith('_t'):
        if re.match(r'^[A-Za-z_]\w*$', t1) or t1 == '*': return True
        if t1 == '(' and len(tokens) > 2 and tokens[2] == '*': return True
    return False

def blank_after_declarations(text):
    """A function's local declarations are followed by a blank line."""
    br = Braces(text); sig = br.sig; edits = []
    for idx, close in br.blocks():
        p = sig[idx][1]
        if line_start_of(text, p) != p or text[p:line_end_of(text, p)].strip() != '{': continue
        if indent_of(text, p) != '': continue                   # function bodies only (column 0)
        stmts = []; j = idx + 1; cur = []; depth = 0; first = None
        while j < close:
            tj, pj = sig[j]
            if first is None: first = j
            cur.append(tj)
            if tj in '([{': depth += 1
            elif tj in ')]}': depth -= 1
            if depth == 0 and (tj == ';' or (tj == '}' and cur[0] not in ('=',))):
                stmts.append((first, j, list(cur))); cur = []; first = None
            j += 1
        k = 0
        while k < len(stmts) and is_declaration(stmts[k][2]): k += 1
        if k == 0 or k == len(stmts): continue
        last_decl_end = sig[stmts[k - 1][1]][1]
        next_start = sig[stmts[k][0]][1]
        if line_end_of(text, last_decl_end) >= next_start: continue   # same line: leave it
        between = text[line_end_of(text, last_decl_end) + 1:line_start_of(text, next_start)]
        if any(not l.strip() for l in between.split('\n')[:-1]) : continue   # a blank line is there
        e = line_end_of(text, last_decl_end)
        edits.append((e, e, '\n'))
    return apply_edits(text, edits), len(edits)

def restyle_signatures(text):
    """A function's signature on one line if it fits in 100 columns, else one parameter a line."""
    br = Braces(text); sig = br.sig; encl, parens = enclosing(br); edits = []
    for idx, (t, p) in enumerate(sig):
        if t != '(' or encl[idx] is not None or parens[idx] != 0 or idx < 2: continue
        name_t, name_p = sig[idx - 1]
        if not re.match(r'^[A-Za-z_]\w*$', name_t) or name_t in ('if', 'for', 'while', 'switch', 'return', 'sizeof'): continue
        close = br.match.get(idx)
        if close is None or close + 1 >= len(sig): continue
        after = sig[close + 1][0]
        if after not in ('{', ';'): continue
        # the statement this is in starts after the previous `;` / `}` / directive at file scope
        k = idx - 1
        while k > 0 and sig[k - 1][0] not in (';', '}', '{') and encl[k - 1] is None: k -= 1
        head_tokens = [x[0] for x in sig[k:idx]]
        if head_tokens[0] == 'typedef' or '=' in head_tokens or len(head_tokens) < 2: continue
        inside = text[p + 1:sig[close][1]]
        if '/*' in inside or '//' in inside or '#' in inside: continue
        # split the parameters at the top-level commas
        params = []; depth = 0; start = p + 1
        for j in range(idx + 1, close):
            tj, pj = sig[j]
            if tj in '([{': depth += 1
            elif tj in ')]}': depth -= 1
            elif tj == ',' and depth == 0:
                params.append(text[start:pj]); start = pj + 1
        params.append(text[start:sig[close][1]])
        params = [re.sub(r'\s+', ' ', x).strip() for x in params]
        if params == ['']: continue
        ls = line_start_of(text, p)
        head = text[ls:p + 1]
        if '\n' in head: continue
        one = head + ', '.join(params) + ')'
        tail = ';' if after == ';' else ''
        if len(one + tail) <= 100 and len(params) and True:
            target = one
        else:
            target = head + '\n' + ',\n'.join(indent_of(text, p) + '    ' + x for x in params) + ')'
        current = text[ls:sig[close][1] + 1]
        if current != target:
            edits.append((ls, sig[close][1] + 1, target))
    return apply_edits(text, edits), len(edits)

def split_declarators(text):
    """One declaration per line: `CHAR *path, letter;` -> `CHAR *path;` / `CHAR letter;`.
    Struct/union members, locals and file-scope variables. Left alone: typedefs (the house
    `typedef ... X, *PX;`), function pointers, inline struct bodies, a declaration with a
    comment inside it, one that does not start and end its own line(s)."""
    br = Braces(text); sig = br.sig; encl, parens = enclosing(br); edits = []
    n = len(sig); idx = 0
    while idx < n:
        t, p = sig[idx]
        # a statement starts after ; { } at depth 0 of its own braces, outside parens
        if parens[idx] != 0 or (encl[idx] is not None and br.kind.get(encl[idx]) != 'block') \
                or not (idx == 0 or sig[idx - 1][0] in (';', '{', '}')):
            idx += 1; continue
        # the statement's tokens up to its `;`
        j = idx; depth = 0; body = False
        while j < n:
            tj = sig[j][0]
            if tj == '{' and depth == 0 and sig[j - 1][0] != '=':
                body = True; break                      # a function or type body: look inside it
            if tj in '([{': depth += 1
            elif tj in ')]}':
                depth -= 1
                if depth < 0: break
            elif tj == ';' and depth == 0: break
            j += 1
        if body or j >= n or sig[j][0] != ';': idx += 1; continue
        toks = [x[0] for x in sig[idx:j]]
        if not toks: idx += 1; continue
        start, end = p, sig[j][1]
        idx_next = j + 1
        if toks[0] == 'typedef' or not is_declaration(toks + [';']) or '{' in toks:
            idx = idx_next; continue
        # the top-level commas
        commas = []; depth = 0
        for k in range(idx, j):
            tk = sig[k][0]
            if tk in '([{': depth += 1
            elif tk in ')]}': depth -= 1
            elif tk == ',' and depth == 0: commas.append(k)
        if not commas: idx = idx_next; continue
        stmt_text = text[start:end]
        if '/*' in stmt_text or '//' in stmt_text: idx = idx_next; continue
        if text[line_start_of(text, start):start].strip() or same_line_code_after(text, end + 1):
            idx = idx_next; continue
        trailing = text[end + 1:line_end_of(text, end)].strip()     # a comment for the whole list
        # the first declarator: its name is the identifier before the first , ; = [ :
        first_end = commas[0]
        k = idx
        while k < first_end and sig[k][0] not in ('=', '[', ':'): k += 1
        name_k = k - 1
        if not re.match(r'^[A-Za-z_]\w*$', sig[name_k][0]) or '(' in toks[:name_k - idx + 1]:
            idx = idx_next; continue
        base_end = name_k
        while base_end - 1 >= idx and sig[base_end - 1][0] == '*': base_end -= 1
        if base_end <= idx: idx = idx_next; continue
        base = re.sub(r'\s+', ' ', text[start:sig[base_end - 1][1] + len(sig[base_end - 1][0])]).strip()
        bounds = [sig[base_end][1]] + [sig[c][1] + 1 for c in commas]
        ends = [sig[c][1] for c in commas] + [end]
        decls = [text[a:b].strip() for a, b in zip(bounds, ends)]
        if any('\n' in d or not d or d.startswith('(') for d in decls): idx = idx_next; continue
        ind = indent_of(text, start)
        out = '\n'.join(ind + base + ' ' + d + ';' for d in decls)
        if trailing:                                     # it described them all: it goes above them
            out = ind + trailing + '\n' + out
            edits.append((line_start_of(text, start), line_end_of(text, end), out))
        else:
            edits.append((line_start_of(text, start), end + 1, out))
        idx = idx_next
    return apply_edits(text, edits), len(edits)

def statement_end(br, i):
    """The index of the last token of the statement that starts at token i."""
    sig = br.sig; n = len(sig); t = sig[i][0]
    if t in ('if', 'for', 'while', 'switch') and i + 1 < n and sig[i + 1][0] == '(':
        close = br.match.get(i + 1)
        if close is None or close + 1 >= n: return None
        e = statement_end(br, close + 1)
        if e is not None and t == 'if' and e + 1 < n and sig[e + 1][0] == 'else':
            return statement_end(br, e + 2)
        return e
    if t == 'else': return statement_end(br, i + 1)
    if t == 'do':
        e = statement_end(br, i + 1)
        if e is None or e + 2 >= n or sig[e + 1][0] != 'while': return None
        close = br.match.get(e + 2)
        return close + 1 if close is not None and close + 1 < n and sig[close + 1][0] == ';' else None
    if t == '{': return br.match.get(i)
    depth = 0
    for j in range(i, n):
        tj = sig[j][0]
        if tj in '([{': depth += 1
        elif tj in ')]}':
            depth -= 1
            if depth < 0: return None
        elif tj == ';' and depth == 0: return j
    return None

def blocks_stand_apart(text):
    """A blank line after every if/for/while/do/switch statement, and one before it."""
    br = Braces(text); sig = br.sig; lines = text.split('\n')
    line_of = lambda pos: text.count('\n', 0, pos)
    starts = [0]
    for l in lines[:-1]: starts.append(starts[-1] + len(l) + 1)
    mask = [False] * len(text)
    for kind, a, b in lex(text):
        if kind in ('code', 'string'):
            for q in range(a, b): mask[q] = True
    def code_line(k):
        if k < 0 or k >= len(lines): return ''
        a = starts[k]
        return ''.join(ch if mask[a + c] else ' ' for c, ch in enumerate(lines[k])).strip()
    before = set(); after = set()
    for i, (t, p) in enumerate(sig):
        if t not in ('if', 'for', 'while', 'switch', 'do'): continue
        prev = sig[i - 1][0] if i else ';'
        if prev == 'else': continue                              # part of an if/else chain
        if t == 'while' and prev == '}':
            o = br.match.get(i - 1)
            if o is not None and o > 0 and sig[o - 1][0] == 'do': continue   # a do-loop's tail
        L = line_of(p)
        if code_line(L).split('(')[0].split()[0:1] != [t] and not code_line(L).startswith(t): continue
        if code_line(L)[:len(t)] != t: continue                  # not the first thing on its line
        e = statement_end(br, i)
        if e is None: continue
        E = line_of(sig[e][1])
        # before it
        P = L - 1
        pc = code_line(P); praw = lines[P].strip() if P >= 0 else ''
        if P >= 0 and praw and not (pc.endswith('{') or pc == '' or pc.endswith(':') or pc.startswith('#')
                                     or pc.endswith(')') or pc.endswith('else') or pc == 'do'):
            before.add(L)
        # after it
        N = E + 1
        if N < len(lines):
            nc = code_line(N); nraw = lines[N].strip()
            if nraw and not (nc.startswith('}') or nc.startswith('else') or nc.startswith('#')
                             or (nc.startswith('while') and nc.endswith(';'))):
                before.add(N)
    if not before: return text, 0
    out = []
    for k, l in enumerate(lines):
        if k in before and out and out[-1].strip() != '': out.append('')
        out.append(l)
    return '\n'.join(out), len(before)

def indent_case_bodies(text):
    """A case label's statements are one level in. A statement at the label's own indent is
    moved in, with the comment lines directly above it -- unless it follows a braced block at
    the label's indent (`case X:` / `{ ... }` / `break;`), which is left as written."""
    br = Braces(text); sig = br.sig; lines_to_move = set()
    lines = text.split('\n')
    starts = [0]
    for l in lines[:-1]: starts.append(starts[-1] + len(l) + 1)
    line_of = lambda pos: text.count('\n', 0, pos)
    for open_idx, close_idx in br.blocks():
        if open_idx < 1 or sig[open_idx - 1][0] != ')': continue
        paren = br.match.get(open_idx - 1)
        if paren is None or paren < 1 or sig[paren - 1][0] != 'switch': continue
        i = open_idx + 1; label_indent = None; previous = None
        while i < close_idx:
            t, p = sig[i]
            if t in ('case', 'default'):
                j = i + 1; depth = 0
                while j < close_idx and not (sig[j][0] == ':' and depth == 0):
                    if sig[j][0] in '([': depth += 1
                    elif sig[j][0] in ')]': depth -= 1
                    j += 1
                label_indent = indent_of(text, p); previous = 'label'; i = j + 1; continue
            end = statement_end(br, i)
            if end is None or end >= close_idx: break
            if label_indent is not None and t != '{':
                first_line = line_of(p)
                at_line_start = text[starts[first_line]:p].strip() == ''
                if at_line_start and indent_of(text, p) == label_indent and previous != 'block':
                    last_line = line_of(sig[end][1])
                    k = first_line - 1                  # the comment lines directly above it
                    while k >= 0 and lines[k].strip().startswith(('/*', '*', '//')) \
                            and indent_of(text, starts[k]) == label_indent:
                        k -= 1
                    lines_to_move.update(range(k + 1, last_line + 1))
            previous = 'block' if t == '{' else 'statement'
            i = end + 1
    if not lines_to_move: return text, 0
    for k in lines_to_move:
        if lines[k].strip(): lines[k] = '    ' + lines[k]
    return '\n'.join(lines), len(lines_to_move)

def restyle_flow(text):
    counts = {}
    for _ in range(20):
        text, n1 = split_statements(text)
        text, n2, multi = move_bodies(text)
        counts['multi_line_bodies_left'] = multi
        if not n1 and not n2: break
    text, counts['case_bodies'] = indent_case_bodies(text)
    text, counts['case_gaps'] = blank_between_cases(text)
    text, counts['block_gaps'] = blocks_stand_apart(text)
    text, counts['declaration_gaps'] = blank_after_declarations(text)
    text, counts['signatures'] = restyle_signatures(text)
    return text, counts

# --- 4. #define alignment ------------------------------------------------------------------------
DEFINE = re.compile(r'^#define\s+(\w+(?:\([^)]*\))?)[ \t]+(.*?)\s*$')

def split_value_comment(rest):
    toks = lex(rest)
    for kind, a, b in toks:
        if kind == 'comment' and rest[b:].strip() == '':
            return rest[:a].rstrip(), rest[a:b]
    return rest.rstrip(), ''

def tidy_define_comment(comment, identifiers):
    """`/*  the x   value  */` -> `/* The x value */`: single spaces, and a capital first
    letter unless the first word is a name the code uses."""
    if '\n' in comment: return comment
    inner = re.sub(r'\s+', ' ', comment[2:-2]).strip()
    m = re.match(r'([a-z][a-z]*)\b', inner)
    if m and m.group(1) not in identifiers:
        inner = inner[0].upper() + inner[1:]
    return '/* ' + inner + ' */'

def restyle_defines(text):
    identifiers = set(re.findall(r'[A-Za-z_]\w*', ''.join(text[a:b] for k, a, b in lex(text) if k == 'code')))
    lines = text.split('\n')
    # runs: maximal stretches of #define / comment-only / blank lines (no other code)
    i = 0; n = len(lines)
    while i < n:
        if not lines[i].startswith('#define'): i += 1; continue
        j = i; members = []
        while j < n:
            l = lines[j]
            if l.startswith('#define'):
                if l.rstrip().endswith('\\'): break
                m = DEFINE.match(l)
                if m: members.append(j)
            elif l.strip() == '' or l.lstrip().startswith(('/*', '*')) :
                pass
            else:
                break
            j += 1
        parsed = {}
        for k in members:
            m = DEFINE.match(lines[k]); value, comment = split_value_comment(m.group(2))
            if '/*' in value or (comment and not comment.endswith('*/')):
                continue                                   # a comment that runs on: leave the line
            if comment: comment = tidy_define_comment(comment, identifiers)
            parsed[k] = (m.group(1), value, comment)
        if parsed:
            names = sorted(len('#define ' + nm) for nm, _, _ in parsed.values())
            cap = names[len(names) * 3 // 4] + 12 if len(names) >= 4 else names[-1]
            fit = [x for x in names if x <= max(cap, 0)] or names
            col = -(-(max(fit) + 2) // 4) * 4
            ends = []
            for nm, value, comment in parsed.values():
                head = '#define ' + nm
                start = col if len(head) < col - 1 else len(head) + 1
                if comment: ends.append(start + len(value))
            ccol = -(-(max([e for e in ends if e + 2 <= 80] or ends or [0]) + 2) // 4) * 4
            for k, (nm, value, comment) in parsed.items():
                head = '#define ' + nm
                line = head + ' ' * (col - len(head)) if len(head) < col - 1 else head + ' '
                line += value
                if comment:
                    if len(line) < ccol - 1 and ccol + len(comment) <= 120: line += ' ' * (ccol - len(line))
                    else: line += ' '
                    line += comment
                lines[k] = line.rstrip()
        i = max(j, i + 1)
    # consecutive #include lines: their trailing comments share one 4-column stop
    i = 0
    while i < n:
        if not lines[i].startswith('#include'): i += 1; continue
        j = i
        while j < n and lines[j].startswith('#include'): j += 1
        parts = {}
        for k in range(i, j):
            m = re.match(r'^(#include\s+["<][^">]+[">])\s*(/\*.*\*/)\s*$', lines[k])
            if m: parts[k] = (m.group(1), re.sub(r'\s+\*/$', ' */', m.group(2)))
        if parts:
            col = -(-(max(len(h) for h, _ in parts.values()) + 2) // 4) * 4
            for k, (h, c) in parts.items():
                lines[k] = h + ' ' * (col - len(h)) + c
        i = j
    return '\n'.join(lines)

# --- 5. spacing ----------------------------------------------------------------------------------
def restyle_spacing(text):
    lines = text.split('\n'); out = []
    toks = lex(text)
    # which lines start inside a comment or string (never edited)
    for idx, l in enumerate(lines):
        out.append(l.rstrip())
    res = []
    for idx, l in enumerate(out):
        prev = res[-1] if res else None
        # a comment straight after a #define line gets a blank line before it
        if l.lstrip().startswith('/*') and prev is not None and prev.startswith('#define') and not prev.rstrip().endswith('\\'):
            res.append('')
        res.append(l)
        # a blank line after a function's closing brace
        if l == '}' and idx + 1 < len(out) and out[idx + 1].strip() != '':
            res.append('')
    final = []
    for l in res:
        if l == '' and final and final[-1] == '': continue
        final.append(l)
    while final and final[-1] == '': final.pop()
    return '\n'.join(final) + '\n'

# --- driver ------------------------------------------------------------------------------
def restyle_text(text, path):
    """(new text, proven, notes). `proven` is False when the code changed beyond the declarator
    split: the caller must then leave the file alone."""
    text, declarators = split_declarators(text)
    original = text
    text, note = restyle_header(text, path)
    text, multiline = restyle_comments(text, path)
    text, skipped = restyle_braces(text)
    text, flow = restyle_flow(text)
    text = restyle_defines(text)
    text = restyle_spacing(text)
    proven = code_fingerprint(original) == code_fingerprint(text)
    leftover = sorted({ch for kind, a, b in lex(text) if kind in ('comment', 'linecomment')
                       for ch in text[a:b] if ord(ch) > 127})
    notes = {'header': note, 'one_line_blocks_left': skipped, 'non_ascii_left': ''.join(leftover),
             'multi_line_bodies_left': flow.get('multi_line_bodies_left', 0)}
    return text, proven, {k: v for k, v in notes.items() if v}
