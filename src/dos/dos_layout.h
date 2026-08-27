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
#define DOS_DPB_OFF       0x0340   /* AH=1Fh/32h drive parameter block, 33 bytes */
#define DOS_MEDIA_OFF     0x0364   /* AH=1Bh/1Ch media descriptor byte           */

/* ── WOW: THE TABLE krnl386 READS AT SysVars+0x6A.  GH #128 ─────────────────
   Before it does anything else, krnl386's init entry (seg1:0xc041) issues
   INT 21h AH=52h, and then:

       mov di, es:[bx+0x6a]        ; a WORD offset, in the SysVars segment
       mov [0x26d], di / mov [0x26f], es
       mov ax, es:[di+0x00] ...    ; and +0x0c, +0x10, +0x18, +0x24, +0x28

   caching six pointers into DOS's data area, then converting the SysVars
   segment to a selector with DPMI 0002 and pairing it with each offset.

 ★ THE +0x6A WORD IS AN OFFSET, NOT A FAR POINTER, and the table it names holds
   FAR pointers -- but krnl386 reads only the OFFSET half of each and supplies
   the selector itself.  So every target must live in the SysVars segment.
   Measured off stock ntvdm, not guessed: `lolprobe` recorded [ES:BX+6A]=0x1482
   with a table of 4-byte entries whose segment half is the SysVars segment
   every time (docs/research/evidence/lolprobe-stock-ntvdm.txt).

 ⚠ WHY THIS IS NOT OPTIONAL AND WHY ITS ABSENCE WAS DANGEROUS.  The whole
   SysVars block used to be zeroed except the MCB head, so [BX+0x6A] read 0 and
   the six "pointers" became offsets 0x00, 0x0c, 0x10... into DOS_HDLR_SEG --
   which is the INT 21h BOP stub and the DPMI entry points.  krnl386 does not
   only read through them, it WRITES (seg1:0x52b5 stores a word through the
   +0x24 one), so the previous state had the guest scribbling on our own
   handler code.  It is scored as part of "Unable to initialize heap" because it
   happens before the heap is built, but it would have corrupted the host
   whatever came next.

   Offsets below are within DOS_CTAB_SEG (linear 0x900), which is inside the
   same MCB-reserved resident block, well clear of linear 0x714 (see above) and
   above every other user of that block.  DOS_WOW_VARS is deliberately a small
   private scratch area: krnl386 reads and writes these bytes and nothing in our
   DOS consults them, so it stays self-consistent.  Two of the six ARE known and
   are seeded for real -- see dos_wow_publish(). */
/* How many drive letters DOS admits to. Reported through SysVars+0x21, which
   krnl386 reads via the table below; the guest's own drive set comes from the
   INT 21h surface, so this is a ceiling, not a claim that all of them exist. */
#define DOS_LASTDRIVE     26
#define DOS_WOW_TBL_OFF   0x0370   /* 11 far pointers = 44 bytes                */
#define DOS_WOW_TBL_N     11
#define DOS_WOW_VARS_OFF  0x03A0   /* the storage those pointers point AT        */
#define DOS_WOW_VARS_LEN  0x20
/* Which entries of the table krnl386 actually reads, and what each becomes.
   Only these six are consulted; the rest are present so the table has stock's
   shape rather than a shorter one that happens to be enough today. */
#define DOS_WOW_E_LASTDRV 0x00     /* -> SysVars+0x21, the LASTDRIVE byte        */
#define DOS_WOW_E_CURDRV  0x0C     /* -> current-drive byte (seg1:0x5343 returns *
                                    *    it as INT 21h AH=19h's answer)          */
#define DOS_WOW_E_C       0x10
#define DOS_WOW_E_E       0x18
#define DOS_WOW_E_D       0x24     /* krnl386 WRITES a word through this one     */
#define DOS_WOW_E_F       0x28

#endif /* DOS_LAYOUT_H */
