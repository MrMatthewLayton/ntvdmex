"""Comment structure of a C file, for order.py: which line opened the comment a
line starts in, and the first line of the comment(s) directly above a statement."""
import re

def blank(t):
    out = list(t); i = 0; n = len(t)
    while i < n:
        if t.startswith('/*', i):
            k = t.find('*/', i + 2); k = n - 2 if k < 0 else k
            for j in range(i, min(k + 2, n)):
                if out[j] != '\n': out[j] = ' '
            i = k + 2
        elif t.startswith('//', i):
            k = t.find('\n', i); k = n if k < 0 else k
            for j in range(i, k): out[j] = ' '
            i = k
        elif t[i] in '"\'':
            q = t[i]; j = i + 1
            while j < n and t[j] != q and t[j] != '\n': j += 2 if t[j] == '\\' else 1
            for x in range(i + 1, min(j, n)): out[x] = ' '
            i = j + 1
        else: i += 1
    return ''.join(out)

class Comments:
    def __init__(self, lines):
        self.L = lines
        self.C = blank('\n'.join(lines)).split('\n')
        self.opener = [None] * len(lines); self.starts_inside = [False] * len(lines)
        state = False; frm = None
        for k, line in enumerate(lines):
            self.starts_inside[k] = state; self.opener[k] = frm if state else None
            i = 0
            while i < len(line):
                if state:
                    j = line.find('*/', i)
                    if j < 0: i = len(line); break
                    state = False; i = j + 2
                elif line.startswith('/*', i): state = True; frm = k; i += 2
                elif line.startswith('//', i): break
                elif line[i] in '"\'':
                    q = line[i]; i += 1
                    while i < len(line) and line[i] != q: i += 2 if line[i] == '\\' else 1
                    i += 1
                else: i += 1
    def ends_inside(self, k):
        return k + 1 < len(self.L) and self.starts_inside[k + 1]
    def above(self, i):
        """First line of the comment(s) directly above line i -- over comment lines and blank
        lines inside a comment, never into the tail of a comment that opened on a code line,
        and stopping at a blank line between items."""
        a = i
        while a > 0 and self.C[a - 1].strip() == '':
            if not self.L[a - 1].strip() and not self.starts_inside[a - 1]: break
            o = self.opener[a - 1]
            if o is not None and self.C[o].strip(): break
            a -= 1
        while a < i and not self.L[a].strip(): a += 1
        return a
    def statement_end(self, k):
        """Extend a statement ending on line k over the rest of a comment it opens."""
        while self.ends_inside(k): k += 1
        return k
