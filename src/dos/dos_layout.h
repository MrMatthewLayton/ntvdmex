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
/* ── GH #128: the PM-fault reflect's PER-CLASS BOP sites. ───────────────────
   The kernel picks the reflect's CS:EIP out of a table indexed by FAULT CLASS
   (see dpmi_install_fault_trampoline).  Session 32 filled all eight entries so
   that no fault could kill the VDM silently -- but it filled them with the SAME
   {selector, offset}, which throws away the class, and the class is the only
   channel that carries WHICH exception fired.  The kernel's frame does not say:
   it carries the error code, CS:IP, FLAGS and SS:SP of the faulting instruction
   and nothing else (measured, session 34).
   ⚠ AND THE OBVIOUS GUESS IS A TRAP: the RE'd class for a #GP is 6, and #UD --
     the exception krnl386 actually raises -- is x86 vector 6 as well.  Any
     reading that conflates them is unfalsifiable.  So give each class its own
     4-byte site and let the reflected EIP name it.
   Lives in the MCB-reserved resident block, in the gap between the AH=65h
   character tables (which end at 0x254) and the BIOS entry stubs (0x300).
   ★ THIRTY-TWO, not eight.  Eight was the size of the table we happened to
     declare, not a fact about the kernel.  Session 34 measured krnl386's
     deliberate `0f ff` (#UD, x86 vector 6) arriving at index 6 -- which is
     equally consistent with "index == vector" and with "index == NT class, and
     class 6 covers #UD as well as #GP".  Those two readings diverge the first
     time a NON-6 exception fires, and only if the table is wide enough to have
     an entry for it.  Eight entries can never tell them apart; thirty-two can,
     and cost 384 bytes of a static array.
   The DPMI dispatch reads the index AS the exception number.  That is the only
   reading under which delivery is defined at all, it is consistent with the one
   measurement there is, and it is guarded: we deliver only if the client has
   actually registered a handler for that exception, so a wrong reading stops
   the run and says so rather than calling the wrong handler. */
#define DOS_FLTSITE_OFF   0x0260   /* 32 sites x 4 bytes = 0x260..0x2DF */
#define DOS_FLTSITE_N     32
#define DOS_FLTRET_OFF    0x02E0   /* the client handler's far-return catcher   */
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
/* ── GH #48: the DPB CHAIN. ────────────────────────────────────────────────────
   One 33-byte drive parameter block per drive that exists, linked and
   terminated, in the same MCB-reserved resident block as everything else here.
   The block runs to linear 0xFEF, i.e. offset 0x6EF within DOS_CTAB_SEG, so
   0x3C0 upwards is free and eight drives (264 bytes) fit with room over.
 ⚠ THE CDS ARRAY IS NOT HERE, AND THAT IS DELIBERATE. It is indexed by drive
   letter and must therefore be LASTDRIVE (26) entries of 88 bytes = 2288 --
   more than this block has left. Building a shorter one would be worse than
   having none: a walker reads LASTDRIVE entries whatever we allocate, so it
   would run off the end into whatever follows, which is precisely the silent
   wander the AH=52h stub was written to prevent. It needs a memory-map change
   (resident DOS grows, DOS_PSP_SEG moves up) and gets its own pass. */
#define DOS_DPBCHAIN_OFF  0x03C0
#define DOS_DPBCHAIN_MAX  8
/* ── GH #34: INT 22h / 23h / 24h, the three vectors a PSP SAVES. ───────────────
   DOS stores the live copies of these into every PSP it builds (at +0x0A, +0x0E
   and +0x12) and restores them when the program ends -- which is why a child
   cannot leave a parent's handlers broken, and what a program installing its own
   INT 24h relies on. They were left at whatever the IVT already held, and the
   PSP fields were ZERO.
 ⚠ ZERO IS THE DANGEROUS VALUE HERE, because "the PSP copy matches the live
   vector" is trivially true when BOTH are 0000:0000 -- a host that never fills
   them in passes that check by accident. tools/dostest/p_psp.asm therefore
   asserts the live INT 24h separately, and it must point at real code.
   Oracle, MS-DOS 6.22: all three match (SI=1) and INT 24h lives at 03E7:0155,
   inside COMMAND.COM. */
#define DOS_CRIT_STUBS    0x04D0   /* 3 stubs x 4 bytes: INT 22h, 23h, 24h */
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

/* ── THE SYSTEM FILE TABLE.  krnl386 COUNTS FILE HANDLES BEFORE IT WILL START. ──
     At seg1:0xbf97 krnl386 calls INT 21h AH=52h, steps to SysVars+4, and walks the
     SFT chain adding up each block's entry count:

         bfaf  xor bx,bx
         bfb1  mov cx, es:[bx+4]        ; entries in this block
         bfb5  add ah, cl               ; running total
         bfb7  cmp word ptr es:[bx], -1 ; offset FFFFh == end of chain
         bfbb  je  0xbfcb
         bfbd  mov cx, es:[bx+2]        ; next segment
         bfc1  mov dx, es:[bx]          ; next offset
         bfc6  call 0xbfde              ; re-base its scratch selector on it
         bfc9  jmp  0xbfaf
         bfcb  cmp ah, [bp-5] / jb -> the error exit at 0x987a

   ⚠ SysVars+4 WAS ZERO, AND A ZERO CHAIN HEAD IS NOT AN EMPTY CHAIN.  It re-based
     the scratch selector on 0000:0000 and read the IVT as an SFT header: word 0
     there is not FFFFh, so it followed the "next" pointer into the ROM and round a
     three-address cycle -- 0x00000000 -> 0x000fa357 -> 0x000bc370 -> 0x00000000 --
     forever.  Measured: 117 MB of INT 31h 0007/0008 in one run.  The terminator is
     the point of this structure at least as much as the count is.
   The shape is MS-DOS's and is confirmed against stock ntvdm rather than recalled:
     lolprobe-stock-ntvdm.txt has SysVars+4 = A7:00CE and the block at 00CE reads
     `00 00 | 2A 03 | 05 00` -- next 032A:0000, five entries.  Ours is one block,
     terminated, and its entry count is what our INT 21h layer can ACTUALLY open
     (dos_machine_t::fh[]), not a number chosen to pass the check. */
/* ── HOW MANY FILES DOS CAN HAVE OPEN AT ONCE. ─────────────────────────────────
     Was 64, and 64 is a number krnl386 measurably refuses to start on: it walks
     the SFT chain (see DOS_SFT_* in dos_layout.h), totals the entries and demands
     at least `[bp-5]` of them -- 0x7f (127), or 0x64 (100) on one branch, both
     read straight out of seg1 at 0xbf7a/0xbf8b. With 64 advertised it exited via
     ExitKernelThunk carrying 0x40, i.e. quoting our own count back at us.
   ⚠ THIS IS THE REAL TABLE, NOT A NUMBER TO SATISFY A CHECK. The SFT block
     advertises exactly DOS_SFT_ENTRIES == this, so raising what we claim also
     raises what we can actually open -- claiming 128 while keeping 64 slots is
     the "runs but lies" failure this project has paid for before. 128 clears both
     thresholds and stays inside the byte accumulator krnl386 sums into
     (`add ah,cl`, so a single block may not exceed 255). */
#define DOS_MAX_FILES 128

#define DOS_SFT_ENTRIES   DOS_MAX_FILES   /* == the size of dos_machine_t::fh[]  */
#define DOS_SFT_ENTSZ     0x3B      /* DOS 4.0+ SFT entry: 59 bytes              */
#define DOS_SFT_BYTES     (6 + DOS_SFT_ENTRIES * DOS_SFT_ENTSZ)
#define DOS_SFT_PARAS     ((DOS_SFT_BYTES + 15) / 16)

#endif /* DOS_LAYOUT_H */
