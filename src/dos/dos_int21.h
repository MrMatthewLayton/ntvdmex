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
    char    *out; int out_cap; int out_len;  /* captured console output (02/09/40) */
    char    *tp;               /* current trace cursor (caller resets + flushes)   */
} dos_machine_t;

/* Zero the handle table, set the MCB root, default DTA = PSP:0x80. */
void dos_int21_init(dos_machine_t *m, uint16_t first_mcb);

/* Service one INT 21h BOP (function in AH). Returns 1 to continue the guest, 0 to
   terminate (AH=4Ch). Appends a trace via m->tp; writes console output to m->out. */
int dos_int21(dos_machine_t *m);

#endif /* DOS_INT21_H */
