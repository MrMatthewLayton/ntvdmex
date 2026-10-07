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
 * plain C with no Windows in it, and tests/unit/install_test.c exercises it.
 */
#ifndef NTVDMEX_INSTALL_H
#define NTVDMEX_INSTALL_H
#include "../ntvdmex_types.h"

/* The IFEO value we own. One definition, because a typo in either half of this is
   an install that appears to succeed and routes nothing. */
#define INSTALL_KEY  "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\" \
                     "Image File Execution Options\\ntvdm.exe"
#define INSTALL_VAL  "Debugger"
/* Where the displaced value is kept so uninstall can put it back. Under HKCU
   alongside the settings, not under the IFEO key: writing our own bookkeeping into
   somebody else's key is how you leave litter that outlives the uninstall. */
#define INSTALL_PREV_VAL "PreviousDebugger"

typedef enum _INSTALL_STATE {
    INSTALL_ABSENT = 0,   /* no Debugger value: the machine uses its own ntvdm  */
    INSTALL_OURS,         /* it points at this exe -- we are the VDM            */
    INSTALL_OTHER         /* it points at something else -- NOT ours to delete  */
} INSTALL_STATE;

/* The name an ntvdmex host goes by, in any folder (InstallNamesNtvdmex). */
#define INSTALL_HOST_NAME        "ntvdmhost.exe"
#define INSTALL_HOST_NAME_LENGTH 13

/* ── PATH COMPARISON, THE WAY THE REGISTRY ACTUALLY HOLDS THEM. ──────────────────
     Windows paths are case-insensitive, the value may or may not be quoted, and a
     hand-written one usually has a stray space. Compare on those terms or the
     answer is wrong for the most common way this value gets set: by a person. */
static INT InstallIsSamePath(PCSTR first, PCSTR second)
{
    INT firstIndex = 0, secondIndex = 0, firstEnd, secondEnd;
    if (!first || !second) return 0;
    while (first[firstIndex] == ' ' || first[firstIndex] == '\t' || first[firstIndex] == '"') ++firstIndex;
    while (second[secondIndex] == ' ' || second[secondIndex] == '\t' || second[secondIndex] == '"') ++secondIndex;
    firstEnd = firstIndex; while (first[firstEnd]) ++firstEnd;
    secondEnd = secondIndex; while (second[secondEnd]) ++secondEnd;
    while (firstEnd > firstIndex && (first[firstEnd-1] == ' ' || first[firstEnd-1] == '\t' || first[firstEnd-1] == '"')) --firstEnd;
    while (secondEnd > secondIndex && (second[secondEnd-1] == ' ' || second[secondEnd-1] == '\t' || second[secondEnd-1] == '"')) --secondEnd;
    if (firstEnd - firstIndex != secondEnd - secondIndex) return 0;
    while (firstIndex < firstEnd) {
        CHAR firstChar = first[firstIndex], secondChar = second[secondIndex];
        if (firstChar >= 'A' && firstChar <= 'Z') firstChar = (CHAR)(firstChar - 'A' + 'a');
        if (secondChar >= 'A' && secondChar <= 'Z') secondChar = (CHAR)(secondChar - 'A' + 'a');
        /* A forward slash is a legal separator in a Win32 path and a person who
           types one has still named the same file. */
        if (firstChar == '/') firstChar = '\\';
        if (secondChar == '/') secondChar = '\\';
        if (firstChar != secondChar) return 0;
        ++firstIndex; ++secondIndex;
    }
    return 1;
}

/* `current` is the Debugger value as read (NULL or "" when there is none); `self` is
   this executable's full path. */
static INSTALL_STATE InstallClassify(PCSTR current, PCSTR self)
{
    INT index = 0;
    if (!current) return INSTALL_ABSENT;
    while (current[index] == ' ' || current[index] == '\t' || current[index] == '"') ++index;
    if (!current[index]) return INSTALL_ABSENT;      /* present but empty is not installed */
    return InstallIsSamePath(current, self) ? INSTALL_OURS : INSTALL_OTHER;
}

/* What an install would DO from here -- so the caller reports the same thing it is
   about to perform, rather than the two being decided in different places. */
typedef enum _INSTALL_ACTION {
    INSTALL_ACT_NOTHING = 0,  /* already in the requested state             */
    INSTALL_ACT_WRITE,        /* write our path (saving anything displaced) */
    INSTALL_ACT_RESTORE,      /* put the previous value back                */
    INSTALL_ACT_DELETE,       /* remove the value entirely                  */
    INSTALL_ACT_REFUSE        /* somebody else's value -- not ours to touch */
} INSTALL_ACTION;

static INSTALL_ACTION InstallPlan(INSTALL_STATE state, INT isWantInstalled, INT hasPrevious);

/* ── IS THE VALUE ANOTHER COPY OF US? (s81, #195) A Debugger value naming some OTHER
     ntvdmhost.exe -- installed from an extracted zip, then uninstalling from bin\ -- was
     classified as a stranger's and refused, which locked the user out of uninstalling
     with the copy they had. The file name is the test: `ntvdmhost.exe`, any folder. */
static INT InstallNamesNtvdmex(PCSTR current)
{
    static const CHAR wanted[] = INSTALL_HOST_NAME;
    PCSTR scan, nameStart; INT index, length = 0;
    if (!current) return 0;
    while (*current == ' ' || *current == '\t' || *current == '"') ++current;
    for (scan = current; *scan && *scan != '"'; ++scan) {           /* up to a closing quote */
        length = (INT)(scan - current) + 1;
        if (length >= INSTALL_HOST_NAME_LENGTH) {                    /* ends in "ntvdmhost.exe"? */
            for (index = 0, nameStart = scan - (INSTALL_HOST_NAME_LENGTH - 1); index < INSTALL_HOST_NAME_LENGTH; ++index) {
                CHAR character = nameStart[index]; if (character >= 'A' && character <= 'Z') character = (CHAR)(character - 'A' + 'a');
                if (character != wanted[index]) break;
            }
            if (index == INSTALL_HOST_NAME_LENGTH && (nameStart == current || nameStart[-1] == '\\' || nameStart[-1] == '/')
                && (scan[1] == 0 || scan[1] == '"' || scan[1] == ' ' || scan[1] == '\t')) return 1;
        }
    }
    return 0;
}

/* The full decision. `isOtherUs`: an INSTALL_OTHER value names an ntvdmhost.exe.
   `isForce`: /uninstall /force -- remove whatever is there (the message names it). */
static INSTALL_ACTION InstallPlanEx(INSTALL_STATE state, INT isWantInstalled, INT hasPrevious,
                                    INT isOtherUs, INT isForce)
{
    if (!isWantInstalled && state == INSTALL_OTHER && (isOtherUs || isForce))
        return hasPrevious ? INSTALL_ACT_RESTORE : INSTALL_ACT_DELETE;
    return InstallPlan(state, isWantInstalled, hasPrevious);
}

static INSTALL_ACTION InstallPlan(INSTALL_STATE state, INT isWantInstalled, INT hasPrevious)
{
    if (isWantInstalled)
        return (state == INSTALL_OURS) ? INSTALL_ACT_NOTHING : INSTALL_ACT_WRITE;
    /* ⚠ UNINSTALLING SOMEBODY ELSE'S VALUE IS REFUSED, NOT SILENTLY DONE. If the
         Debugger points at another program we never installed, deleting it would
         break whatever that is and we would have no way to tell the user what we
         removed. Absent is already the goal, so that is NOTHING, not an error. */
    if (state == INSTALL_ABSENT) return INSTALL_ACT_NOTHING;
    if (state == INSTALL_OTHER)  return INSTALL_ACT_REFUSE;
    return hasPrevious ? INSTALL_ACT_RESTORE : INSTALL_ACT_DELETE;
}

#endif /* NTVDMEX_INSTALL_H */
