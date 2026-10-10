#!/usr/bin/env python3
"""docs/STYLE.md 5a, the order of a file's items, as text -> text transformations.

Used by style.py; not run on its own.

A header is read as ITEMS between its include guard and its final #endif: preprocessor lines
(an #if ... #endif block is one item), declarations, definitions. Each carries the comment
lines directly above it and the comment block before that when only blank lines separate them
(a section's introduction travels with the section's first item). Items are then stably sorted:
includes (system before project), defines, types (with compile-time checks after them),
variables, prototypes, inline functions.

A source file keeps its first #include block (its own header first, then <system>, then
"project") and then has defines, types, forward declarations, variables and functions, each
static helper above its first caller.

A file with something the parser cannot place (a #pragma, an #if block holding code, an
#include after code, an item it cannot classify) raises ValueError, and is left as written.
"""
import re, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from restyle import lex, TOKEN

RANK = {'include_sys': 1.0, 'include': 1.1, 'define': 2, 'type': 3, 'check': 3.5,
        'var': 4, 'proto': 5, 'inline': 6}

def code_mask(text):
    mask = [False] * len(text)
    for kind, a, b in lex(text):
        if kind == 'code':                              # strings are blanked: a '{' in one is not a brace
            for k in range(a, b): mask[k] = True
    return mask

def parse(text):
    """(prefix, items, suffix) or raises ValueError. Items: dict(kind, text, name)."""
    lines = text.split('\n')
    offs = [0]
    for l in lines: offs.append(offs[-1] + len(l) + 1)
    mask = code_mask(text)
    def code_of(i):
        a, b = offs[i], offs[i] + len(lines[i])
        return ''.join(ch if mask[k] else ' ' for k, ch in zip(range(a, b), text[a:b]))
    codes = [code_of(i) for i in range(len(lines))]
    from ccomment import Comments
    inside = Comments(lines).starts_inside           # does line k begin inside a comment?
    # the guard: the first #ifndef/#define pair; the end: the last #endif
    gi = next(i for i, c in enumerate(codes) if c.strip().startswith('#ifndef'))
    gd = gi + 1
    while not codes[gd].strip(): gd += 1
    if not re.match(r'#\s*define\s+\w+\s*$', codes[gd].strip()): raise ValueError('no include guard')
    ge = max(i for i, c in enumerate(codes) if c.strip().startswith('#endif'))
    body = list(range(gd + 1, ge))
    items = []; pending = []          # comment/blank lines waiting for their item
    i = gd + 1
    while i < ge:
        c = codes[i].strip()
        if not c:                                         # a comment line or a blank line
            pending.append(i); i += 1; continue
        start = i
        if c.startswith('#'):
            word = re.match(r'#\s*(\w+)', c).group(1)
            if word in ('if', 'ifdef', 'ifndef'):
                depth = 0; j = i
                while True:
                    cj = codes[j].strip()
                    if re.match(r'#\s*(if|ifdef|ifndef)\b', cj): depth += 1
                    elif re.match(r'#\s*endif\b', cj):
                        depth -= 1
                        if depth == 0: break
                    j += 1
                end = j; kind = 'cond'
            else:
                end = i
                while lines[end].rstrip().endswith('\\'): end += 1
                kind = {'include': 'include', 'define': 'define', 'undef': 'define'}.get(word, 'other')
                if word == 'pragma': raise ValueError('#pragma')
        else:
            # a C item: up to the `;` at depth 0, or the closing brace of a function body
            depth = 0; j = i; end = None; saw_body = False; so_far = ''
            while j < ge:
                for ch in codes[j]:
                    if ch == '{' and depth == 0 and re.search(r'\)\s*$', so_far):
                        saw_body = True
                    if ch in '({[': depth += 1
                    elif ch in ')}]': depth -= 1
                    so_far += ch
                so_far += ' '
                stripped = codes[j].rstrip()
                if depth == 0 and (stripped.endswith(';') or (saw_body and stripped.endswith('}'))):
                    end = j; break
                j += 1
            if end is None: raise ValueError(f'unterminated item at line {i + 1}')
            stmt = ' '.join(codes[i:end + 1])
            toks = TOKEN.findall(stmt)
            if saw_body: kind = 'inline'
            elif toks[0] in ('typedef', 'struct', 'enum', 'union'): kind = 'type'
            elif toks[0] in ('C_ASSERT', '_Static_assert', 'STATIC_ASSERT'): kind = 'check'
            elif '(' in toks and re.search(r'\b[A-Za-z_]\w*\s*\([^()]*(\([^()]*\)[^()]*)*\)\s*;\s*$', stmt) and '=' not in stmt.split('(')[0]:
                kind = 'proto'
            elif toks[0] in ('extern', 'static', 'const') or '=' not in stmt:
                kind = 'var'
            else:
                kind = 'other'
        # a comment that runs on after the item's last line belongs to it
        while end + 1 < ge and inside[end + 1]:
            end += 1
        items.append({'kind': kind, 'lead': pending, 'lines': list(range(start, end + 1))})
        pending = []
        i = end + 1
    if [k for k in items if k['kind'] == 'other']:
        raise ValueError('unclassified item: ' + lines[[k for k in items if k['kind'] == 'other'][0]['lines'][0]].strip()[:60])
    return lines, gd, ge, items, pending

def item_text(lines, item, trim_leading_blank=True):
    lead = item['lead']
    while lead and not lines[lead[0]].strip(): lead = lead[1:]   # blank lines are re-made
    return [lines[k] for k in lead] + [lines[k] for k in item['lines']]

def cond_kind(lines, item):
    inner = [lines[k] for k in item['lines'][1:-1]]
    kinds = set()
    for l in inner:
        s = l.strip()
        if not s or s.startswith(('/*', '*', '//')): continue
        if s.startswith('#define') or s.startswith('#undef'): kinds.add('define')
        elif s.startswith('#include'): kinds.add('include')
        else: kinds.add('x')
    if kinds == {'define'}: return 'define'
    if kinds == {'include'}: return 'include'
    raise ValueError('#if block with code in it')

def rank(lines, item):
    k = item['kind']
    if k == 'cond': k = cond_kind(lines, item)
    if k == 'include':
        return RANK['include_sys'] if '<' in lines[item['lines'][0]] else RANK['include']
    return RANK[k]

def render(lines, gd, ge, items, tail, ge_end=None):
    """The header with its items in STYLE order, one blank line between groups and wherever an
    item had blank lines before its comment (a section start)."""
    if ge_end is None: ge_end = len(lines)
    order = sorted(range(len(items)), key=lambda n: (rank(lines, items[n]), n))
    out = lines[:gd + 1] + ['']
    prev_rank = None
    for n in order:
        it = items[n]; r = rank(lines, it)
        had_gap = any(not lines[k].strip() for k in it['lead']) or n == 0
        if out[-1] != '' and (prev_rank is None or r != prev_rank or had_gap):
            out.append('')
        out.extend(item_text(lines, it))
        prev_rank = r
    tail_lines = [lines[k] for k in tail]
    while tail_lines and not tail_lines[0].strip(): tail_lines = tail_lines[1:]
    if tail_lines: out += [''] + tail_lines
    out += [''] + lines[ge:ge_end]
    text = '\n'.join(out)
    return re.sub(r'\n{3,}', '\n\n', text)

def reorder_header(text):
    lines, gd, ge, items, tail = parse(text)
    return render(lines, gd, ge, items, tail)

# --- a source file's include block ---------------------------------------------------------------
def reorder_includes(text, path):
    """The first block of #includes in a .c (blank lines and comment lines may sit between
    them): its own header first, then <system>, then "project"; otherwise as written."""
    lines = text.split('\n')
    idx = [i for i, l in enumerate(lines) if l.startswith('#include')]
    if not idx: return text
    first = idx[0]; last = first
    for i in idx[1:]:
        between = lines[last + 1:i]
        if all(not b.strip() or b.lstrip().startswith(('/*', '*', '//')) for b in between): last = i
        else: break
    block = [l for l in lines[first:last + 1] if l.startswith('#include')]
    others = [l for l in lines[first:last + 1] if not l.startswith('#include')]
    if any(o.strip() for o in others): return text            # comments between includes: leave
    own = os.path.basename(path)[:-2] + '.h'
    def group(l):
        m = re.match(r'#include\s*([<"])([^>"]+)[>"]', l)
        if m.group(1) == '"' and os.path.basename(m.group(2)) == own: return 0
        return 1 if m.group(1) == '<' else 2
    ordered = sorted(block, key=group)
    return '\n'.join(lines[:first] + ordered + lines[last + 1:])

# --- a source file's body: STYLE.md 5a -----------------------------------------------------------
C_RANK = {'define': 1, 'type': 2, 'var': 3, 'proto': 4, 'func': 5}

def parse_c(text):
    """(lines, head_end, items): the items after the file's first #include block."""
    lines = text.split('\n')
    offs = [0]
    for l in lines: offs.append(offs[-1] + len(l) + 1)
    mask = code_mask(text)
    codes = [''.join(ch if mask[k] else ' ' for k, ch in zip(range(offs[i], offs[i] + len(lines[i])), lines[i]))
             for i in range(len(lines))]
    from ccomment import Comments
    inside = Comments(lines).starts_inside
    inc = [i for i, c in enumerate(codes) if c.strip().startswith('#include')]
    if not inc: raise ValueError('no includes')
    head_end = inc[0]
    while head_end + 1 < len(lines) and (codes[head_end + 1].strip().startswith('#include') or not codes[head_end + 1].strip()):
        head_end += 1
    # back up over trailing blank/comment lines: they belong to the first item
    while head_end > inc[0] and not codes[head_end].strip(): head_end -= 1
    items = []; pending = []; i = head_end + 1; n = len(lines)
    while i < n:
        c = codes[i].strip()
        if not c: pending.append(i); i += 1; continue
        start = i
        if c.startswith('#'):
            word = re.match(r'#\s*(\w+)', c).group(1)
            if word in ('if', 'ifdef', 'ifndef'):
                depth = 0; j = i
                while True:
                    cj = codes[j].strip()
                    if re.match(r'#\s*(if|ifdef|ifndef)\b', cj): depth += 1
                    elif re.match(r'#\s*endif\b', cj):
                        depth -= 1
                        if depth == 0: break
                    j += 1
                end = j
                inner = [codes[k].strip() for k in range(i + 1, j) if codes[k].strip()]
                if all(x.startswith(('#define', '#undef', '#else', '#elif')) for x in inner): kind = 'define'
                else: raise ValueError('#if block with code in it')
            elif word in ('define', 'undef'):
                end = i
                while lines[end].rstrip().endswith('\\'): end += 1
                kind = 'define'
            elif word == 'include': raise ValueError('#include after code')
            else: raise ValueError('#' + word)
        else:
            depth = 0; j = i; end = None; body = False; so_far = ''
            while j < n:
                for ch in codes[j]:
                    if ch == '{' and depth == 0 and re.search(r'\)\s*$', so_far): body = True
                    if ch in '({[': depth += 1
                    elif ch in ')}]': depth -= 1
                    so_far += ch
                so_far += ' '
                st = codes[j].rstrip()
                if depth == 0 and (st.endswith(';') or (body and st.endswith('}'))):
                    end = j; break
                j += 1
            if end is None: raise ValueError(f'unterminated item at line {i + 1}')
            stmt = ' '.join(codes[i:end + 1]); toks = TOKEN.findall(stmt)
            if body: kind = 'func'
            elif toks[0] == 'typedef': kind = 'type'
            elif toks[0] in ('struct', 'enum', 'union') and '(' not in stmt.split('{')[0].split('=')[0]: kind = 'type'
            elif toks[0] in ('C_ASSERT', '_Static_assert'): kind = 'type'
            elif re.search(r'\)\s*;\s*$', stmt) and '=' not in stmt.split('(')[0] and not re.search(r'\(\s*\*', stmt.split(')')[0]): kind = 'proto'
            else: kind = 'var'
        while end + 1 < n and inside[end + 1]: end += 1
        name = None
        if kind in ('func', 'proto'):
            m = re.search(r'([A-Za-z_]\w*)\s*\(', ' '.join(codes[start:end + 1]).split('{')[0])
            name = m.group(1) if m else None
        items.append({'kind': kind, 'lead': pending, 'lines': list(range(start, end + 1)), 'name': name,
                      'static': codes[start].lstrip().startswith('static')})
        pending = []; i = end + 1
    return lines, codes, head_end, items, pending

def reorder_c(text):
    lines, codes, head_end, items, tail = parse_c(text)
    defined = [re.match(r'\s*#\s*(?:define|undef)\s+(\w+)', codes[it['lines'][0]]) for it in items if it['kind'] == 'define']
    names = [m.group(1) for m in defined if m]
    if len(names) != len(set(names)): raise ValueError('a macro defined twice or undefined')
    funcs = [it for it in items if it['kind'] == 'func']
    static_funcs = {it['name']: it for it in funcs if it['static'] and it['name']}
    def refs(it):
        body = ' '.join(codes[k] for k in it['lines'])
        return [t for t in TOKEN.findall(body) if t in static_funcs and t != it['name']]
    # functions: each static helper above its first caller (depth first from the original order)
    placed = []; done = set(); active = set(); cyclic = set()
    def emit(it):
        key = id(it)
        if key in done: return
        if key in active: cyclic.add(it['name']); return
        active.add(key)
        for name in dict.fromkeys(refs(it)):
            emit(static_funcs[name])
        active.discard(key); done.add(key); placed.append(it)
    for it in funcs: emit(it)
    # forward declarations: only for a static function referenced before its definition
    order_pos = {id(it): k for k, it in enumerate(placed)}
    var_refs = set()
    for it in items:
        if it['kind'] == 'var': var_refs.update(t for t in refs(it))
    needed = set(cyclic) | var_refs
    for it in placed:
        for name in refs(it):
            if order_pos[id(static_funcs[name])] > order_pos[id(it)]: needed.add(name)
    protos = []; dropped = []
    for it in items:
        if it['kind'] != 'proto': continue
        nm = it['name']
        if nm in static_funcs and nm not in needed and not [k for k in it['lead'] if lines[k].strip()]:
            dropped.append(' '.join(codes[k] for k in it['lines'])); continue   # a bare forward declaration nobody needs now
        protos.append(it)
    have = {p['name'] for p in protos}
    missing = [n for n in needed if n not in have]
    added = []
    unexplained = [n for n in missing if n not in var_refs]
    if unexplained: raise ValueError('would need new forward declarations: ' + ', '.join(sorted(unexplained))[:80])
    for nm in sorted(missing, key=lambda n: order_pos[id(static_funcs[n])]):
        # a function a variable's initialiser names needs declaring above the variables: its
        # signature, as written, ended with `;`
        it = static_funcs[nm]
        sig = '\n'.join(lines[k] for k in it['lines'])
        sig = sig[:sig.index('{')].rstrip() if '{' in sig else sig
        protos.append({'kind': 'proto', 'lead': [], 'lines': [], 'name': nm, 'static': True, 'text': sig + ';'})
        added.append(sig + ';')
    groups = [[it for it in items if it['kind'] == 'define'], [it for it in items if it['kind'] == 'type'],
              protos,                                  # declarations before definitions (a table may name a function)
              [it for it in items if it['kind'] == 'var' and not it['static']] + [it for it in items if it['kind'] == 'var' and it['static']],
              placed]
    out = lines[:head_end + 1]
    for g in groups:
        for it in g:
            lead = it['lead']
            had_gap = any(not lines[k].strip() for k in lead)
            while lead and not lines[lead[0]].strip(): lead = lead[1:]
            block = [lines[k] for k in lead] + ([it['text']] if 'text' in it else [lines[k] for k in it['lines']])
            if out and out[-1] != '' and (had_gap or it['kind'] in ('func', 'type') or out[-1].rstrip().endswith('}')):
                out.append('')
            out.extend(block)
        if g and out[-1] != '': out.append('')
    out += [lines[k] for k in tail]
    new = re.sub(r'\n{3,}', '\n\n', '\n'.join(out)).rstrip('\n') + '\n'
    return new, dropped, added
