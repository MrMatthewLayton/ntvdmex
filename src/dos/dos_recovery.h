/* dos_recovery.h -- what to do when the host will not start.  GH #132.
 *
 * NTVDMEX installs itself as the machine's VDM through an IFEO Debugger value on
 * ntvdm.exe. That is one registry value away from being reversible -- and one
 * registry value away from being CATASTROPHIC, because if the host wedges on
 * startup then every DOS and every Win16 launch on the machine is broken, and
 * the fix requires editing the registry with no working VDM to do it from.
 *
 * ⚠ THIS IS NOT HYPOTHETICAL. Session 52 wedged the bare-metal rig TWICE in one
 *   day, both times on a blocking Win32 call before the host had a window:
 *   GetDiskFreeSpaceA on an empty floppy drive, and a modal "cannot find the
 *   file" box under stock ntvdm. Neither could be recovered remotely -- kill,
 *   reboot and shutdown all failed -- and one of them ALSO left the IFEO key
 *   removed, so every later run silently measured stock ntvdm.
 *
 * The policy: count CONSECUTIVE failed starts. A start counts as failed until
 * the run ends cleanly, so a hang or a crash leaves the count raised.
 *
 *   0 .. SAFE-1        run normally
 *   SAFE .. GONE-1     run in SAFE MODE -- skip everything optional
 *   GONE and above     SELF-UNINSTALL: drop the IFEO value so the machine's own
 *                      ntvdm takes over again, and say so loudly
 *
 * ★ SELF-UNINSTALL IS THE POINT. Safe mode is a nicety; the thing that matters
 *   is that a machine cannot be left with no working VDM. Three strikes and we
 *   take ourselves out of the path rather than break every 16-bit program on
 *   the box until someone edits the registry by hand.
 *
 * Pure -- no Windows types -- so tools/dostest/recovery_test.c can pin it.
 */
#ifndef DOS_RECOVERY_H
#define DOS_RECOVERY_H

#define DOS_RECOVER_SAFE  2      /* this many consecutive failures -> safe mode */
#define DOS_RECOVER_GONE  3      /* ...and this many -> take ourselves out       */

typedef enum {
    DOS_START_NORMAL = 0,
    DOS_START_SAFE   = 1,
    DOS_START_UNINSTALL = 2
} dos_start_mode;

/* `fails` is the number of consecutive starts that did NOT end cleanly, read
   before this one is counted. */
static dos_start_mode dos_recovery_decide(unsigned fails)
{
    if (fails >= DOS_RECOVER_GONE) return DOS_START_UNINSTALL;
    if (fails >= DOS_RECOVER_SAFE) return DOS_START_SAFE;
    return DOS_START_NORMAL;
}

/* Parse the counter file's contents. Anything unreadable counts as ZERO, not as
   a failure: a corrupt counter must not be able to uninstall us on its own, and
   "the file is missing" is the normal state on a healthy machine. */
static unsigned dos_recovery_parse(const char *buf, unsigned len)
{
    unsigned v = 0, i, digits = 0;
    if (!buf) return 0;
    for (i = 0; i < len; ++i) {
        if (buf[i] >= '0' && buf[i] <= '9') {
            v = v * 10 + (unsigned)(buf[i] - '0');
            if (++digits > 4) return 0;          /* absurd -> treat as zero */
        } else if (digits) break;                /* stop at the first non-digit */
        else if (buf[i] != ' ' && buf[i] != '\r' && buf[i] != '\n' && buf[i] != '\t')
            return 0;                            /* leading junk -> zero        */
    }
    return v;
}

#endif /* DOS_RECOVERY_H */
