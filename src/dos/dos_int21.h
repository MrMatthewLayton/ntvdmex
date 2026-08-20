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
    HANDLE   fh[24];           /* DOS handle -> Win32 (0..4 console; files in 5+)  */
    uint16_t first_mcb;        /* MCB chain root (AH=48/49/4A)                     */
    uint16_t dta_seg, dta_off; /* Disk Transfer Area (AH=1A/2F)                    */
    uint8_t  ver_major, ver_minor;  /* reported DOS version -- GH #28, default 6.22 */
    uint8_t  alloc_strat;      /* AH=58h allocation strategy (0 = first fit)       */
    uint8_t  umb_link;         /* AH=58h UMB link state (0 = not linked)           */
    uint16_t sysvars_seg, sysvars_off;  /* AH=52h list of lists, planted by the host */
    HANDLE   find_h[8];        /* AH=4Eh/4Fh live searches; slot stashed in the DTA */
    uint16_t last_err;         /* AH=59h extended error -- last failing call's AX   */
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
int dos_int21(dos_machine_t *m);

#endif /* DOS_INT21_H */
