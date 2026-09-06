/* install.h -- making NTVDMEX the machine's VDM, and taking it back. (GH #13, #130)
 *
 * ── WHY THIS IS A FILE AND NOT A LINE IN A README ───────────────────────────────
 * Installing has been `reg add` typed by hand for the whole life of this project.
 * That is fine for a bench and it is not a product: it needs the exact key path,
 * the exact value name, an absolute path to the exe, and it silently overwrites
 * whatever was there before with no way back. Every one of those is a way to end up
 * with a machine whose DOS support is broken and no obvious reason why.
 *
 * ── WHAT INSTALLING ACTUALLY IS ─────────────────────────────────────────────────
 * One REG_SZ under Image File Execution Options for ntvdm.exe, named `Debugger`.
 * Windows then launches OUR exe in place of ntvdm.exe, passing ntvdm's own command
 * line along. That is the whole mechanism -- see ADR-0007 -- and it is why
 * uninstalling is a single value deletion and needs no repair of anything else.
 *
 * ⚠ AND WHY IT MUST BE REVERSIBLE RATHER THAN JUST REMOVABLE. `Debugger` is not
 *   ours; it is a general Windows facility, and something else may already be using
 *   it on this machine. Overwriting that and then DELETING on uninstall would leave
 *   the box worse than we found it, with no record of what used to be there. So the
 *   previous value is saved before we overwrite, and put back on the way out.
 *
 * ── THE PART THAT IS TESTABLE OFF THE VM ────────────────────────────────────────
 * The registry calls need a machine. Deciding WHAT STATE WE ARE IN -- absent, ours,
 * or somebody else's -- is string comparison, and it is where the traps live: the
 * stored path may be quoted or not, the case will not match (`C:\NTVDMEX` vs
 * `c:\ntvdmex`), and a value written by hand often has trailing whitespace. Get any
 * of those wrong and `install` reports "already installed" on a machine that is not,
 * or `uninstall` refuses to remove our own value. So that decision lives here, in
 * plain C with no Windows in it, and tools/dostest/install_test.c exercises it.
 */
#ifndef NTVDMEX_INSTALL_H
#define NTVDMEX_INSTALL_H

/* The IFEO value we own. One definition, because a typo in either half of this is
   an install that appears to succeed and routes nothing. */
#define INSTALL_KEY  "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\" \
                     "Image File Execution Options\\ntvdm.exe"
#define INSTALL_VAL  "Debugger"
/* Where the displaced value is kept so uninstall can put it back. Under HKCU
   alongside the settings, not under the IFEO key: writing our own bookkeeping into
   somebody else's key is how you leave litter that outlives the uninstall. */
#define INSTALL_PREV_VAL "PreviousDebugger"

typedef enum {
    INSTALL_ABSENT = 0,   /* no Debugger value: the machine uses its own ntvdm  */
    INSTALL_OURS,         /* it points at this exe -- we are the VDM            */
    INSTALL_OTHER         /* it points at something else -- NOT ours to delete  */
} install_state;

/* ── PATH COMPARISON, THE WAY THE REGISTRY ACTUALLY HOLDS THEM. ──────────────────
     Windows paths are case-insensitive, the value may or may not be quoted, and a
     hand-written one usually has a stray space. Compare on those terms or the
     answer is wrong for the most common way this value gets set: by a person. */
static int install_path_eq(const char *a, const char *b)
{
    int ai = 0, bi = 0, ae, be;
    if (!a || !b) return 0;
    while (a[ai] == ' ' || a[ai] == '\t' || a[ai] == '"') ++ai;
    while (b[bi] == ' ' || b[bi] == '\t' || b[bi] == '"') ++bi;
    ae = ai; while (a[ae]) ++ae;
    be = bi; while (b[be]) ++be;
    while (ae > ai && (a[ae-1] == ' ' || a[ae-1] == '\t' || a[ae-1] == '"')) --ae;
    while (be > bi && (b[be-1] == ' ' || b[be-1] == '\t' || b[be-1] == '"')) --be;
    if (ae - ai != be - bi) return 0;
    while (ai < ae) {
        char ca = a[ai], cb = b[bi];
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        /* A forward slash is a legal separator in a Win32 path and a person who
           types one has still named the same file. */
        if (ca == '/') ca = '\\';
        if (cb == '/') cb = '\\';
        if (ca != cb) return 0;
        ++ai; ++bi;
    }
    return 1;
}

/* `cur` is the Debugger value as read (NULL or "" when there is none); `self` is
   this executable's full path. */
static install_state install_classify(const char *cur, const char *self)
{
    int i = 0;
    if (!cur) return INSTALL_ABSENT;
    while (cur[i] == ' ' || cur[i] == '\t' || cur[i] == '"') ++i;
    if (!cur[i]) return INSTALL_ABSENT;      /* present but empty is not installed */
    return install_path_eq(cur, self) ? INSTALL_OURS : INSTALL_OTHER;
}

/* What an install would DO from here -- so the caller reports the same thing it is
   about to perform, rather than the two being decided in different places. */
typedef enum {
    INSTALL_ACT_NOTHING = 0,  /* already in the requested state             */
    INSTALL_ACT_WRITE,        /* write our path (saving anything displaced) */
    INSTALL_ACT_RESTORE,      /* put the previous value back                */
    INSTALL_ACT_DELETE,       /* remove the value entirely                  */
    INSTALL_ACT_REFUSE        /* somebody else's value -- not ours to touch */
} install_action;

static install_action install_plan(install_state st, int want_installed, int have_prev)
{
    if (want_installed)
        return (st == INSTALL_OURS) ? INSTALL_ACT_NOTHING : INSTALL_ACT_WRITE;
    /* ⚠ UNINSTALLING SOMEBODY ELSE'S VALUE IS REFUSED, NOT SILENTLY DONE. If the
         Debugger points at another program we never installed, deleting it would
         break whatever that is and we would have no way to tell the user what we
         removed. Absent is already the goal, so that is NOTHING, not an error. */
    if (st == INSTALL_ABSENT) return INSTALL_ACT_NOTHING;
    if (st == INSTALL_OTHER)  return INSTALL_ACT_REFUSE;
    return have_prev ? INSTALL_ACT_RESTORE : INSTALL_ACT_DELETE;
}

#endif /* NTVDMEX_INSTALL_H */
