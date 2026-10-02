/* dos_int21.h -- the DOS INT 21h service surface for the host.
 *
 * One BOP per INT 21h surfaces here; dos_int21() reads the guest registers from
 * the VDM_TIB, services the call (console, Win32-backed file I/O, memory via
 * dos_mcb.h, misc), and returns CF on the FLAGS the INT pushed on the V86 stack.
 * Ported from tools/vdmhost/vdmhost.c; the memory calls now delegate to the shared
 * dos_mcb.h allocator instead of an inline copy.
 */
#ifndef DOS_INT21_H
#define DOS_INT21_H

#include <windows.h>
#include <stdint.h>
#include "ntvdm.h"
#include "dos_layout.h"   /* DOS_MAX_FILES -- the capacity fh[] must match */
#include "dos_clock.h"    /* the VDM's own clock -- GH #250 */


/* A trace is opt-in via cfg\dostrace.flag, which says who PAYS for it and nothing
   about how big it gets. XP's COMMAND.COM in a command loop wrote 2,166,824 lines
   and 268 MB before this existed. Opt-in is not the same as bounded. */
#define DOS_TRACE_MAX 4000

/* DOS-machine state the INT 21h surface owns. */
typedef struct {
    volatile BYTE *tib;        /* guest CONTEXT (registers via VDM_REG)            */
    HANDLE   fh[DOS_MAX_FILES]; /* DOS handle -> Win32 (0..4 console; files in 5+) */
    uint16_t first_mcb;        /* MCB chain root (AH=48/49/4A)                     */
    uint16_t dta_seg, dta_off; /* Disk Transfer Area (AH=1A/2F)                    */
    uint8_t  ver_major, ver_minor;  /* reported DOS version -- GH #28, default 6.22 */
    /* ── #208, THE USER'S CHOICE: SETVER, NOT A SESSION-WIDE 5.00. ─────────────────
         XP's COMMAND.COM refuses anything but 5.00, and every program started from
         Windows now runs UNDER it -- so a session-wide 5.00 would have changed the
         version every program sees. Instead, like DOS's own SETVER table, the version
         is answered PER PROCESS: a PSP listed here (an NTVDM-aware shell) is told
         shell_ver; everything else gets ver_major/ver_minor, the Settings version. */
#define DOS_V5_PSPS 4
    uint16_t v5_psp[DOS_V5_PSPS];   /* 0 = unused slot                                */
    uint8_t  shell_ver_major, shell_ver_minor;
    uint8_t  alloc_strat;      /* AH=58h allocation strategy (0 = first fit)       */
    uint8_t  umb_link;         /* AH=58h UMB link state (0 = not linked)           */
    uint8_t  break_on;         /* AH=33h extended Ctrl-Break checking (BREAK=)     */
    uint16_t sysvars_seg, sysvars_off;  /* AH=52h list of lists, planted by the host */
    HANDLE   find_h[8];        /* AH=4Eh/4Fh live searches; slot stashed in the DTA */
    uint16_t last_err;         /* AH=59h extended error -- last failing call's AX   */
    uint8_t  verify;           /* AH=2Eh/54h verify-after-write flag                */
    uint16_t child_rc;         /* AH=4Dh return code of the last child              */
    HANDLE   fcb_find;         /* AH=11h/12h FCB search in progress                  */
    uint8_t  switch_char;      /* AH=37h -- oracle says '/' on 6.22                  */
    uint16_t psp_seg;          /* AH=50h/51h/62h -- the CURRENT process's PSP        */
    /* AH=4Bh EXEC.  dos_int21 only RECORDS the request; the host performs the
       load and the control transfer, because the loader, the file I/O and the
       guest's register frame all live there.  See exec_begin() in main.c. */
    int      exec_pending;
    /* ── GH #49: TSR RESIDENCY. AH=31h and INT 27h terminate the program but
         must NOT free its memory or unwind its interrupt vectors -- that is the
         whole of "stay resident". `tsr_keep` is the paragraph count the program
         asked to keep; `tsr_pending` tells the host to take the resident exit
         path instead of the ordinary one. */
    int      tsr_pending;
    uint16_t tsr_keep;
    uint8_t  exec_mode;        /* AL: 00 load+go, 01 load only, 03 overlay          */
    /* AL=01 and AL=03 both ANSWER through the caller's parameter block, so the
       host needs to find it again after the load. AL=01 writes the entry SS:SP
       and CS:IP back into it; AL=03 reads the load segment and relocation
       factor out of it. (GH #50) */
    uint16_t exec_pb_seg, exec_pb_off;
    uint16_t exec_ovl_seg, exec_ovl_reloc;
    char     exec_path[128];
    /* The name EXACTLY as the caller passed it in DS:DX. DOS 6.22 appends this,
       verbatim -- not qualified, not upcased -- to the child's environment copy
       after the 00 01 00; measured by p_exec/p_child (child.env.namekind). */
    char     exec_name[128];
    uint16_t exec_env;         /* 0 = inherit the parent's environment (a COPY)      */
    uint16_t exec_tail_seg, exec_tail_off;
    uint16_t exec_fcb1_seg, exec_fcb1_off;
    uint16_t exec_fcb2_seg, exec_fcb2_off;
    char    *out; int out_cap; int out_len;  /* captured console output (02/09/40) */
    int      out_trunc;        /* set when output was dropped -- see OUTC()        */
    uint8_t  unimpl21[32];     /* GH #27: DOS-defined services we have not written  */
    uint8_t  noop21[32];       /* GH #27: services 6.22 does not define either       */
    char    *tp;               /* current trace cursor (caller resets + flushes)   */
    int      exit_code;        /* AH=4Ch AL -- DOS errorlevel (read after the loop) */
    void   (*conout)(void *ctx, uint8_t ch);  /* optional sink for console output  */
    void    *conctx;           /* passed to conout (e.g. the video VDD)            */
    int    (*conin)(void *ctx); /* optional source for console input (blocking)     */
    void    *cinctx;           /* passed to conin (e.g. the keyboard VDD)          */
    int    (*coninnb)(void *ctx); /* non-blocking console read: char, or -1 if none */
    /* RETRY: set by a blocking service that has nothing to return yet. The host must then
       leave the guest's EIP ON the BOP so it re-executes the INT -- turning a host-side
       block into a guest-side poll. This matters enormously: blocking the exec thread in C
       stops the GUEST dead, so its timer stops, its music stops and its screen freezes until
       a key arrives. A real BIOS spins in the guest with interrupts enabled and the machine
       stays alive; now so do we. */
    int    retry;
    int    (*conpeek)(void *ctx); /* non-blocking status: 1 if a key is ready       */
    /* AH=0Ah (buffered input) IS A BLOCKING SERVICE THAT SPANS MANY RETRIES, so the
       line it is collecting has to survive them. The characters live in the GUEST's
       buffer, which is untouched between retries; what we need to remember is how far
       in we are, and WHICH buffer it was -- a different DS:DX means a different call,
       not a continuation. See the handler. */
    uint16_t line_seg, line_off;
    int      line_n, line_active;
    /* #251: AH=3Fh FROM THE CONSOLE IS DOS's OWN LINE EDITOR, and unlike AH=0Ah its
       line lives on DOS's side: it is read whole (127 characters + CR LF) and handed
       out across as many reads as the caller makes. con_n counts what is typed while
       collecting; con_len/con_pos are what is left to hand out. */
    BYTE     con_line[130];
    int      con_n, con_len, con_pos, con_collecting;
    int      trace_all;        /* log EVERY INT 21h call -- see the trace at entry */
    DWORD    trace_n;          /* how many have been printed; capped at DOS_TRACE_MAX */
    /* THE CURRENT DRIVE WHEN WIN32 CANNOT STAND ON IT. -1 = the current drive is the
       process current directory's, as it always was. DOS selects a drive (AH=0Eh)
       from the CDS without touching the media -- oracle: `0Eh B:` on a one-floppy
       machine selects the phantom B: and 19h reads it back -- but Win32's
       SetCurrentDirectory("A:") on an empty floppy or CD-ROM drive says NOT READY.
       So a drive that exists (GetLogicalDrives) but cannot be entered is held here,
       19h/47h/36h answer for it, and every relative path is prefixed with it so the
       access fails on THAT drive the way DOS's would, instead of quietly landing on
       C:. QB.EXE's File dialog sizes its drive list by select-then-read-back, and
       listed one drive on a machine with four. (s72) */
    int      vdrive;
    /* WHICH OF THE FIVE STANDARD HANDLES ARE STILL OPEN (bits 0-4, set at startup).
       fh[0..4] are NULL because they are devices, not files, so "NULL" cannot also
       mean "free" for them -- and the difference is load-bearing. DOS hands out the
       LOWEST FREE handle, which is how `> file` works: the shell closes handle 1 and
       opens the target, and the target BECOMES handle 1. See AH=3Ch. */
    /* 32 bits, not 8: a device handle is not confined to slots 0-4. AH=45h can
       duplicate the console into any free slot, which is how a shell saves stdout
       before redirecting -- see DOS_DEV_SLOTS in dos_fh.h. */
    uint32_t std_open;
    /* ── ★★ EACH PROGRAM HAS ITS OWN HANDLE TABLE ON DOS. OURS IS ONE TABLE. (s81) ──
         fh[]/std_open are the machine's ONLY handle table, so a child that closed its
         handle 1 closed the SHELL's stdout: Doom's SETUP does exactly that, and XP's
         COMMAND.COM came back unable to print its prompt or the "Invalid handle" error
         about it -- a silent spin the user saw as a flashing cursor. On DOS the child
         works on a COPY (the PSP's JFT) and its closes are its own.
       ⇒ EXEC pushes the parent's table here; while a child runs, closing or dup2-ing
         over a Win32 handle the parent still holds only UNBINDS it; terminate closes
         whatever the child still has open (as DOS does) and pops the parent's table
         back. See dos_handles_push/pop. */
#define DOS_HSTACK 8
    struct { HANDLE fh[DOS_MAX_FILES]; uint32_t std_open; } hsave[DOS_HSTACK];
    int      hdepth;
    /* AH=11h/12h: the 11-byte template the live FCB search matches against (s81) --
       see dos_find_match in dos_int21.c. */
    uint8_t  fcb_tmpl[11];
    /* GH #250: AH=2Dh reloads the BIOS tick count (0040:006C) the way DOS's CLOCK$
       does. A hook rather than a store, because the pacer thread increments that
       dword under the PIT's own lock and a bare write could be lost between its read
       and its write. NULL (off-VM) = the ticks are left alone. */
    void   (*set_ticks)(void *ctx, uint32_t ticks);
    void    *ticks_ctx;
} dos_machine_t;

/* GH #250: the host's local time as fields, and the VDM's reading of a clock that is
   `off` centiseconds from it (g_dos_clock.dos_off / .rtc_off). Win32 lives here, the
   arithmetic in dos_clock.h. */
void dos_clock_host_now(dclk_t *t);
void dos_clock_read(int64_t off, dclk_t *out);

/* Zero the handle table, set the MCB root, default DTA = PSP:0x80. */
void dos_int21_init(dos_machine_t *m, uint16_t first_mcb);

/* Per-process handle tables, DOS-style (see dos_machine_t::hsave). push at EXEC,
   pop at the child's terminate; `tsr` = the child stays resident, so the files it
   still holds stay open (DOS does not close a TSR's handles). */
void dos_handles_push(dos_machine_t *m);
void dos_handles_pop(dos_machine_t *m, int tsr);
/* Close DOS handle `slot` in the current table -- for real only if no parent still
   holds the same Win32 handle. Use instead of CloseHandle(m->fh[slot]). */
void dos_handle_release(dos_machine_t *m, unsigned slot);

/* Service one INT 21h BOP (function in AH). Returns 1 to continue the guest, 0 to
   terminate (AH=4Ch). Appends a trace via m->tp; writes console output to m->out. */
extern int g_dos_int21_pm;      /* 1 = client is in protected mode (DPMI) */
void dos_int21_set_pm(int on);  /* CF/ZF -> live VTIB_EFLAGS, not a V86 FLAGS frame */
/* The reported DOS version, which is a LIE THE GUEST GETS TO CHOOSE -- real DOS has
   SETVER for exactly this. Default is 6.22 (the oracle), and that default is what
   XP's own COMMAND.COM refuses: it prints "Incorrect DOS version" and terminates,
   because NT's DOS has always reported 5.00 and its shell is built to match.
   Call before dos_int21_init's defaults are wanted, or any time after. */
void dos_int21_set_version(dos_machine_t *m, uint8_t major, uint8_t minor);
/* #208: the process at `psp` is an NTVDM-aware shell and is answered 5.00 by AH=30h
   and AX=3306h (on=1), or no longer is (on=0, e.g. at its terminate). */
void dos_int21_shell_psp(dos_machine_t *m, uint16_t psp, int on);
/* The current drive, 0 = A:. Published because the NTVDM `BOP 0x54 sub 01` answer has
   to carry it (COMMAND.COM reads it out of the reply block and immediately issues
   AH=0Eh with it) -- and the host must not re-derive the same policy separately, which
   is how `vdrive` would have been silently dropped. */
uint8_t dos_int21_cur_drive(const dos_machine_t *m);

/* ── INT 21h AH=53h, THE PRIVATE SUB-FUNCTIONS, AS A TABLE RATHER THAN A SWITCH. ──
     Documented AH=53h is BPB->DPB and has no AL selector; NT's NTDOS.SYS overloads it
     as a private query and XP's COMMAND.COM reads the answer out of AL. The defaults
     here are the values MEASURED against stock ntvdm (see the handler), but that
     measurement was taken by a probe whose output was REDIRECTED TO A FILE, and at
     least one of these sub-functions is suspected of depending on exactly that -- so
     the table is a knob (`cfg\int53.txt`) and not a constant. Index = AL, 0..7; AL>7
     keeps DOS's "invalid function" (AX=1, CF=1). */
typedef struct { uint16_t ax; uint8_t cf; } dos_int53_ans_t;
#define DOS_INT53_N 8
extern dos_int53_ans_t g_dos_int53[DOS_INT53_N];

int dos_int21(dos_machine_t *m);

#endif /* DOS_INT21_H */
