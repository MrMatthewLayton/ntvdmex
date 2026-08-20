/* dos_layout.h -- the host's conventional-memory layout for a DOS process.
 * Shared by the host (which places the PSP/IVT/handler/env) and the INT 21h
 * surface (which reports these segments back to the guest). Matches the spike.
 */
#ifndef DOS_LAYOUT_H
#define DOS_LAYOUT_H

#include "dos_mcb.h"          /* DOS_PSP_SEG (0x0100), DOS_MEM_TOP (0xA000) */

#define DOS_HDLR_SEG  0x0050  /* INT 21h BOP handler segment (linear 0x0500) */
#define DOS_ENV_SEG   0x0060  /* environment segment (linear 0x0600)         */
#define DOS_LOAD_OFF  0x0010  /* .EXE load module = DOS_PSP_SEG + this       */
#define DOS_DBCS_OFF  0x0018  /* empty DBCS table parked at DOS_HDLR_SEG:this */
#define DOS_EMM_NAME_OFF 0x000A /* "EMMXXXX0" device-header name (M4 EMS detect, *
                                 * INT 67h vector segment : offset 0Ah)        */

/* Bare RETF, planted by the host. INT 21h AH=38h hands the caller a FAR pointer
   to DOS's case-mapping routine; pointing it at nothing would send any program
   that actually calls it into the weeds, so it points here and returns
   immediately -- an identity case map, which is a correct no-op for ASCII. */
#define DOS_CASEMAP_OFF 0x0059

/* AH=34h hands the guest a FAR pointer to the InDOS flag, and AH=5D06h a pointer
   to the swappable data area whose first bytes are the critical-error flag and
   that same InDOS byte. Both point here. */
#define DOS_SDA_OFF     0x00D4      /* [0]=crit-err flag, [1]=InDOS, then zeros */
#define DOS_SDA_LEN     0x20
#define DOS_INDOS_OFF   (DOS_SDA_OFF + 1)

/* AH=65h character tables (GH #38) live inside the DOS-resident filler block the
   MCB chain reserves at paragraph 0x0070 (0x8E paragraphs, owner 8 = "DOS").
   That block exists to stand in for resident DOS, so no guest allocates over it,
   and these tables ARE resident DOS data.  The handler segment cannot host them:
   DOS_HDLR_SEG (0x50) runs into DOS_ENV_SEG (0x60) after only 256 bytes and the
   tables need ~600.

   !! DO NOT MOVE THIS DOWN TO THE START OF THE BLOCK (0x0071 / linear 0x710) !!
   Linear 0x714 is the KERNEL's VDM interrupt-state dword -- the [0x714] that
   session 10 found wedges guests when written from user mode.  The block's own
   data area begins at 0x710, so the obvious placement lands directly on it and
   breaks EVERY guest, selftest included.  Starting at 0x0090 (linear 0x900)
   clears it with room to spare and still ends well below the next MCB header at
   0xFF0. */
#define DOS_CTAB_SEG      0x0090
#define DOS_CTAB_UPPER    0x0000   /* 130 bytes */
#define DOS_CTAB_FNUPPER  0x0090   /* 130 bytes */
#define DOS_CTAB_FNTERM   0x0120   /*  24 bytes */
#define DOS_CTAB_COLLATE  0x0140   /* 258 bytes */
#define DOS_CTAB_DBCS     0x0250   /*   4 bytes */
/* BIOS entry stubs (INT 11h/12h/13h/14h/15h/17h/25h/26h), 4 bytes each:
   BOP <int> ; IRET.  They live here rather than in DOS_HDLR_SEG because that
   segment is down to scattered free bytes and these want 32 contiguous. */
#define DOS_BIOS_STUBS    0x0300

#endif /* DOS_LAYOUT_H */
