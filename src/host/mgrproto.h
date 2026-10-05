#ifndef NTVDMEX_HOST_MGRPROTO_H
#define NTVDMEX_HOST_MGRPROTO_H
/*
 * mgrproto.h -- the protocol between a host (ntvdmhost.exe) and the NTVDMEX
 * manager (ntvdmex.exe). GH #281, session 88. Both sides include this file; there
 * is no other definition of any value below.
 *
 * ── THE SHAPE ────────────────────────────────────────────────────────────────
 * The manager owns ONE tray icon and the list of running programs. A host does
 * not ask to be added and then trust it: it RE-ANNOUNCES ITSELF every couple of
 * seconds (MGR_OP_HELLO over WM_COPYDATA), and the manager upserts by pid. So a
 * renamed program, a manager that was restarted, and the race where a manager is
 * exiting just as a host starts all repair themselves on the next announcement,
 * with no special case for any of them. A host that cannot find the manager
 * starts it (throttled). The manager drops a host when its PROCESS HANDLE
 * signals -- never on a message, because a crashed host sends none -- and exits a
 * few seconds after its list goes empty.
 *
 * ── THE OTHER DIRECTION ─────────────────────────────────────────────────────
 * The manager tells a host what to do with a REGISTERED window message whose
 * wParam is an MGR_COMMAND_*. It deliberately does not post the host's own IDM_*
 * numbers: those are an implementation detail of main.c's menu and may move.
 *
 * ⚠ ALL ANSI, ALL FIXED-SIZE. The two sides are separate executables built from
 *   the same tree, but a stale manager and a new host can meet on a user's
 *   machine; `Version` and `Size` let either refuse what it does not understand.
 */

#define MGR_CLASS        "NTVDMEX_Manager"          /* the manager's hidden window */
#define MGR_MUTEX        "NTVDMEX_Manager_Single"   /* one manager per session     */
#define MGR_CMD_MSGNAME  "NTVDMEX_ManagerCommand"   /* RegisterWindowMessage name  */
#define MGR_EXE          "ntvdmex.exe"              /* beside ntvdmhost.exe        */

#define MGR_MAGIC        0x4D47524Eu                /* 'NRGM' */
#define MGR_VERSION      1

#define MGR_OP_HELLO     1        /* "I am running; this is what to show for me" */
#define MGR_OP_BYE       2        /* "I am going" -- optional; the handle is the truth */

#define MGR_KIND_DOS     1
#define MGR_KIND_WIN16   2

/* wParam of the registered command message, manager -> host. */
#define MGR_COMMAND_SHOW          1   /* bring this program's window forward          */
#define MGR_COMMAND_SETTINGS      2   /* open the Settings dialog                      */
#define MGR_COMMAND_CLOSE_PROGRAM 3   /* the host's own "Close Program"                */
#define MGR_COMMAND_EXIT          4   /* the host's own "Exit"                         */

#define MGR_NAME_SIZE    64

typedef struct _MGR_MESSAGE {
    DWORD Magic;                  /* MGR_MAGIC                                     */
    DWORD Version;                /* MGR_VERSION                                   */
    DWORD Size;                   /* sizeof(MGR_MESSAGE) as the SENDER built it    */
    DWORD Operation;              /* MGR_OP_*                                      */
    DWORD ProcessId;              /* the host process                              */
    DWORD Kind;                   /* MGR_KIND_*                                    */
    DWORD CommandWindow;          /* the host window that takes MGR_COMMAND_*      */
    DWORD ShowTargetWindow;       /* the window "Show" brings forward (0 = ask)    */
    CHAR  Name[MGR_NAME_SIZE];    /* what the menu calls this program, NUL-ended   */
} MGR_MESSAGE, *PMGR_MESSAGE;

typedef const MGR_MESSAGE *PCMGR_MESSAGE;

#endif /* NTVDMEX_HOST_MGRPROTO_H */
