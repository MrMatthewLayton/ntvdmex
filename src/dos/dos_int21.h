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

/* DOS-machine state the INT 21h surface owns. */
typedef struct {
    volatile BYTE *tib;        /* guest CONTEXT (registers via VDM_REG)            */
    HANDLE   fh[64];           /* DOS handle -> Win32 (0..4 console; files in 5+)  */
    uint16_t first_mcb;        /* MCB chain root (AH=48/49/4A)                     */
    uint16_t dta_seg, dta_off; /* Disk Transfer Area (AH=1A/2F)                    */
    uint8_t  ver_major, ver_minor;  /* reported DOS version -- GH #28, default 6.22 */
    uint8_t  alloc_strat;      /* AH=58h allocation strategy (0 = first fit)       */
    uint8_t  umb_link;         /* AH=58h UMB link state (0 = not linked)           */
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
    uint8_t  exec_mode;        /* AL: 00 load+go, 01 load only, 03 overlay          */
    char     exec_path[128];
    uint16_t exec_env;         /* 0 = inherit the parent's environment               */
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
} dos_machine_t;

/* Zero the handle table, set the MCB root, default DTA = PSP:0x80. */
void dos_int21_init(dos_machine_t *m, uint16_t first_mcb);

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
int dos_int21(dos_machine_t *m);

#endif /* DOS_INT21_H */
