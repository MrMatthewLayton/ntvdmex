/* dos_layout.h -- the host's conventional-memory layout for a DOS process.
 * Shared by the host (which places the PSP/IVT/handler/env) and the INT 21h
 * surface (which reports these segments back to the guest). Matches the spike.
 */
#ifndef NTVDMEX_DOS_LAYOUT_H
#define NTVDMEX_DOS_LAYOUT_H

#include "dos_mcb.h"          /* DOS_PSP_SEG (0x0100), DOS_MEM_TOP (0xA000) */

#define DOS_HDLR_SEG  0x0050  /* INT 21h BOP handler segment (linear 0x0500) */
/* The host's stubs in DOS_HDLR_SEG -- where WinMain plants each and points its vector. */
#define DOS_HDLR_INT21_STUB_OFF 0x0000
#define DOS_HDLR_INT10_STUB_OFF 0x0020
#define DOS_HDLR_INT16_STUB_OFF 0x0028
#define DOS_HDLR_INT33_STUB_OFF 0x0030
#define DOS_HDLR_INT08_STUB_OFF 0x0034  /* BOP 08h ; INT 1Ch ; IRET                     */
#define DOS_HDLR_INT08_STUB_END 0x003A
#define DOS_HDLR_INT1C_STUB_OFF 0x003A
#define DOS_HDLR_INT1A_STUB_OFF 0x003C
#define DOS_HDLR_INT2F_STUB_OFF 0x0040
#define DOS_HDLR_INT67_STUB_OFF 0x0048
#define DOS_HDLR_INT09_STUB_OFF 0x004C  /* BOP 09h ; IRET                               */
#define DOS_HDLR_INT09_STUB_END 0x0050
#define DOS_HDLR_TRAMPOLINE_OFF  0x0060  /* qimode VIF: sti ; jmp far entry  */
#define DOS_HDLR_TRAMPOLINE_SIZE 6
/* ── THE CURRENT DRIVE, IN ONE PLACE. (session 56) ───────────────────────────
     Three routes ask this machine what drive it is on -- INT 21h AH=19h, the
     WOW32 select-drive thunk (id 0xc8), and krnl386's own cached copy, which
     is filled from whichever answered last. They must not
     disagree; two of them disagreeing is exactly the shape of the equipment
     word vs the 0040:0000 port table, and of the BIOS vs the UART registers.
   ⚠ IT IS A CONSTANT BECAUSE CHANGING DRIVES IS NOT SUPPORTED. INT 21h AH=0Eh
     accepts a select and ignores it, so anything that reported a DIFFERENT
     drive back would be claiming a switch that did not happen. When a real
     per-process current drive exists, this becomes a variable and all three
     routes read it. */
#define DOS_CURRENT_DRIVE 0x02      /* C: -- 0 = A: */
/* ── #207: THE ENVIRONMENT MOVED FROM 0x60 TO 0x7F, ABOVE SysVars. ─────────────────
     The first MCB has to lie above the SysVars segment (MEM /D's "MSDOS System Data"
     row is SysVars seg .. first MCB, and was negative) -- see DosMcbInitialize. The env is
     the first block of the chain, so it is the thing that had to move; it lands in
     linear 0x7F0..0x8EF, the gap between the SDA (ends 0x7E0) and the DOS block's MCB
     at 0x8F0, which nothing used (checked: no define, literal or plant in 0x7E0..0x8FF).
     Same 256 bytes (DOS_ENV_CAP), same owner, same PSP:2C mechanism, BELOW the program
     -- not the s73 top-of-memory move that broke DOS/4GW (that one moved PSP+2).
   ⚠ What follows the block is now the DOS block's MCB at 0x8F0, not [0x714]: an env
     write past DOS_ENV_CAP would break the chain instead of the kernel. Still capped. */
#define DOS_ENV_SEG   0x007F  /* environment segment (linear 0x07F0) = DOS_FIRST_MCB+1 */
/* ── #48: THE CHARACTER/BLOCK DEVICE HEADERS, at linear 0x600 (the env's old home). ──
     Kernel data below the first MCB, as IO.SYS's headers are on 6.22 (0070:0023 CON ..
     0070:006B the block driver -- measured, sysvars_test.c). Built by DosDeviceChainBuild;
     0x60:0x00..0xEF. Segment 0x50 is NOT used for this: its offsets 0x100+ alias this
     same memory, and the handler segment's own slots stop at 0xFF (main.c MS_CB_RET_OFF). */
#define DOS_DEV_SEG   0x0060
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
/* ⚠⚠ 0x00D4 WAS DOS_SYSVARS_OFF + 0x44, SO THE SDA SAT ON TOP OF SYSVARS.
     That made DOS_INDOS_OFF (0xD5) literally SysVars+0x45 -- and MEM.EXE
     decides from the word at SysVars+0x45 whether to report extended memory at
     all. InDOS is zero while a program runs, so MEM read zero and skipped the
     whole report. That is GH
     #47's "Extended (XMS) 0K", and it was a LAYOUT COLLISION, not a driver bug.
   0x6C..0x8B is clear: past the IRET stub (0x58), the case-map (0x59) and the
   opt-in VIF trampoline (0x60..0x65), and below the MCB-head word at 0x8E. */
/* ⛔ s81 (#208): 0x6C..0x8B IN DOS_HDLR_SEG WAS NOT CLEAR. The DPMI callback slots
     (0x60-0x6F), the PM-return catcher (0x70), the raw-switch entry (0x74) and the fault
     BOP (0x80) are all planted in it, so the crit-error/InDOS pair read as stub bytes
     (measured "10 CF" -- CF is an IRET) whenever start-up planted them last. p_err caught
     it the first time the launch order changed. The SDA now lives where 6.22 keeps it: in
     DOS's data segment beside SysVars -- DOS_SDA_SEG:DOS_SDA_OFF, below. */
#define DOS_SDA_OFF     0x00A0      /* in DOS_SDA_SEG: [0]=crit-err flag, [1]=InDOS, zeros */
#define DOS_SDA_LEN     0x20
#define DOS_INDOS_OFF   (DOS_SDA_OFF + 1)

/* ── ⚠⚠ THE SECOND ABSOLUTE-OFFSET READ MEM.EXE MAKES, AND IT IS NOT A SYSVARS
     FIELD EITHER. (GH #47) ───────────────────────────────────────────────────
     MEM asks AH=52h for SysVars, keeps the SEGMENT, THROWS THE OFFSET AWAY, and
     uses the word at <SysVars segment>:0x008C as the conventional/upper LINE:
     every block in the MCB walk at or above it is reported as upper memory.
     We left 0x008C at ZERO, so EVERY block compared >= 0 and the ENTIRE chain
     was filed as UPPER MEMORY -- which is the phantom `Upper 1,663K` with our real free block sitting in it as `Largest
     free upper memory block 548K`, and `Conventional Free 0K` underneath.

     MS-DOS 6.22 has **0xFFFF** here (`docs/research/evidence/lolprobe-msdos622.txt`,
     dump offset 0x8C = FF FF), i.e. "no block is upper", which is the truth on a
     machine with no UMB provider. It is the truth here too: we refuse AH=5803 for
     exactly that reason.

   ★ AND THE ADJACENT WORD EXPLAINS THE PAIR. 6.22 has the FIRST MCB SEGMENT at
     0x008E as well as at SysVars-2 -- the same 0x0253 in both places. So 0x8C/0x8E
     are "first UMB" / "first MCB", and our MCB head already lands on 0x8E because
     DOS_SYSVARS_OFF is 0x90. That was luck, not design; this makes the pair
     deliberate.
   ⚠ (s81: SUPERSEDED -- SysVars DID move, to 6.22's own offset in its own segment,
     which is what makes 0x8C a SysVars field rather than a neighbour. See below.)
   ⚠ SO DOS_SYSVARS_OFF IS NOT FREE TO MOVE. Two absolute offsets in this segment
     are load-bearing for MEM (0x8C here, and the SDA collision at SysVars+0x45
     that #47 already cost a session to find). Moving SysVars moves neither. */
/* ── ★★ s81 (#47, MEM /C): THE PAIR IS NOT "LUCK", IT IS 6.22's OWN LAYOUT. ──────
     6.22 puts SysVars at OFFSET 0x26 of its segment (0116:0026). So its absolute
     0x8C is SysVars+0x66 and 0x8E is SysVars+0x68: MEM's "absolute" read and MEM /C's
     RELATIVE read of SysVars+0x66 are THE SAME FIELD there -- "first MCB in upper
     memory", FFFFh when there is none (measured: p_sysvar, 6.22 and PCem).
   ⛔ OURS WAS AT 0050:0090, and SysVars is ~0x70 bytes long, so SysVars+0x60 onward
     WAS THE FIRST MCB HEADER at linear 0x5F0. MEM /C read SysVars+0x66 -- that MCB's
     reserved bytes, 0000 -- as "the upper-memory chain starts at segment 0", walked the
     IVT as an MCB, and booked ~1 MB to MSDOS. And the krnl386 table pointer at
     SysVars+0x6A was sitting in that MCB's name field, commented as "free".
   ⇒ SysVars now lives at DOS_SYSVARS_SEG:0x0026, exactly 6.22's offset, in the DOS-
     resident filler block (linear 0x720-0x8FF is otherwise unused; it is past the
     kernel's [0x714] dword and below DOS_CTAB_SEG). Both MEM reads now hit one field
     for the same reason they do on real DOS.
   ★ #207: and that space is no longer inside ANY MCB block. The filler block that held
     it began at 0x70, below SysVars, which made the chain start below SysVars and MEM
     /D's MSDOS row negative. The chain now starts at DOS_FIRST_MCB (0x7E), so
     0x720..0x7DF (SysVars, its -2 word, the SDA) is kernel data BELOW the first MCB --
     6.22's shape: its SysVars is at 0116:0026 and its first MCB at 0253. */
#define DOS_SYSVARS_SEG 0x0072     /* SysVars' segment: AH=52h returns ES=this    */
#define DOS_SYSVARS_OFF 0x0026      /* 6.22's offset; MCB head word at -2 (GH #35) */
#define DOS_UMBHEAD_OFF 0x008C      /* = SysVars+0x66: first UMB MCB; FFFF = none  */
#define DOS_UMBHEAD_NONE 0xFFFF
#define DOS_SYSVARS_LEN 0x0070      /* bytes from SysVars+0 that we own and zero   */
#define DOS_SDA_SEG     DOS_SYSVARS_SEG   /* the SDA shares DOS's data segment, as on 6.22 */
/* #48: NUL's strategy/interrupt entries (DosNulStubBuild, 8 bytes). A header's entries
   are offsets in ITS OWN segment, and NUL's header is inline in SysVars, so they must be
   in this segment: 0072:0010 = linear 0x730..0x737 -- past [0x714] (0x714..0x717), below
   SysVars' -2 word at 0x744, and written by nothing else (sysvars_test pins all three). */
#define DOS_NULSTUB_OFF 0x0010

/* AH=65h character tables (GH #38) live inside the DOS-resident filler block the
   MCB chain reserves at paragraph 0x0070 (0x8E paragraphs, owner 8 = "DOS").
   ⚠ #207: that block's MCB is now at 0x8F (DOS_RESBLK_MCB, 0x6F paragraphs), so its
   data STARTS at 0x90 = this segment. Nothing in it moved; only the part below 0x900
   (which held SysVars, the SDA and [0x714]) left the block for the kernel area below
   the first MCB. Read "the block" below as 0x900..0xFEF.
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
#define DOS_FLTSITE_SIZE  4
#define DOS_FLTSITE_N     32
#define DOS_FLTRET_OFF    0x02E0   /* the client handler's far-return catcher   */
#define DOS_BIOS_STUBS    0x0300
#define DOS_BIOS_STUB_SIZE 4       /* BOP nn ; IRET or RETF                    */
#define DOS_DPB_OFF       0x0340   /* AH=1Fh/32h drive parameter block, 33 bytes */
#define DOS_MEDIA_OFF     0x0364   /* AH=1Bh/1Ch media descriptor byte           */

/* ── WOW: THE TABLE krnl386 READS AT SysVars+0x6A.  GH #128 ─────────────────
   Before it does anything else, krnl386 issues INT 21h AH=52h, takes the WORD
   at SysVars+0x6A as an offset in the SysVars segment, and keeps six pointers
   from the table there (entries +0x00, +0x0c, +0x10, +0x18, +0x24, +0x28),
   pairing each with a selector for the SysVars segment (DPMI 0002).

 ★ THE +0x6A WORD IS AN OFFSET, NOT A FAR POINTER, and the table it names holds
   FAR pointers -- but only the OFFSET half of each is used; the selector is
   krnl386's own.  So every target must live in the SysVars segment.
   Measured off stock ntvdm, not guessed: `lolprobe` recorded [ES:BX+6A]=0x1482
   with a table of 4-byte entries whose segment half is the SysVars segment
   every time (docs/research/evidence/lolprobe-stock-ntvdm.txt).

 ⚠ WHY THIS IS NOT OPTIONAL AND WHY ITS ABSENCE WAS DANGEROUS.  The whole
   SysVars block used to be zeroed except the MCB head, so [BX+0x6A] read 0 and
   the six "pointers" became offsets 0x00, 0x0c, 0x10... into DOS_HDLR_SEG --
   which is the INT 21h BOP stub and the DPMI entry points.  krnl386 does not
   only read through them, it WRITES (a word, through the +0x24 one), so the
   previous state had the guest scribbling on our own handler code.  It is scored as part of "Unable to initialize heap" because it
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
/* ── LASTDRIVE IS 5, WHICH IS WHAT REAL DOS REPORTS. (GH #48) ──────────────────
   It was 26, chosen as a generous ceiling. MS-DOS 6.22 says 5 -- measured,
   SysVars+0x21 in tests/probes/dos/p_sysvar.asm -- and 5 is not an arbitrary
   smaller number: the CDS array is INDEXED BY DRIVE LETTER and must be exactly
   LASTDRIVE entries of 88 bytes, so the ceiling decides whether the array can
   exist at all. At 26 it needs 2288 bytes and the resident block has ~500 free;
   at 5 it needs 440 and fits, so the choice was between a truthful ceiling with
   a real CDS array and an inflated one with none.
 ⚠ A: through E: covers every drive this host exposes (C: is index 2), and
   krnl386 only needs the byte to be non-zero -- it reads it through the
   SysVars+0x6A table to decide there are drives at all. */
/* ── s71: LASTDRIVE IS 26 AGAIN, AND THE CDS ARRAY LIVES IN ITS OWN BLOCK. ─────
   5 was 6.22's DEFAULT, and it was the right number for a host that only ever
   exposed C:. This host exposes what the machine has -- the user's has A:, C:,
   D: (CD-ROM) and Z: (network) -- and a DOS program sizes its drive list from
   INT 21h AH=0Eh's answer, which IS LASTDRIVE: QB.EXE's file dialog showed only
   C: for exactly that reason. A LASTDRIVE that cannot name Z: is a lie about the
   machine, so it is 26, as a CONFIG.SYS with LASTDRIVE=Z makes 6.22 report.
   The 26 x 88 = 2288-byte CDS array does not fit the resident filler, so it
   takes a block of its own reserved at the top of conventional memory
   (DosMcbReserveTop), owned by DOS like the rest of the resident data. The
   program block loses 0x90 paragraphs, which is what LASTDRIVE=Z costs on a
   real PC too. */
#define DOS_LASTDRIVE     26
#define DOS_DRIVE_LETTERS 26        /* A: to Z:                                 */
#define DOS_DRIVE_C       2         /* drive numbers count from A: = 0          */
#define DOS_CDS_PARAS     0x8F      /* 26 * 88 = 2288 bytes = 143 paragraphs        */
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
   them in passes that check by accident. tests/probes/dos/p_psp.asm therefore
   asserts the live INT 24h separately, and it must point at real code.
   Oracle, MS-DOS 6.22: all three match (SI=1) and INT 24h lives at 03E7:0155,
   inside COMMAND.COM. */
#define DOS_CRIT_STUBS    0x04D0   /* 3 stubs x 4 bytes: INT 22h, 23h, 24h */
#define DOS_CRIT_STUB_INT22  0      /* BOP 30h ; IRET                     */
#define DOS_CRIT_STUB_INT23  4      /* IRET                               */
#define DOS_CRIT_STUB_INT24  8      /* MOV AL,3 ; IRET                    */
/* ── GH #34: WHERE DOS CALLS THE GUEST'S INT 24h FROM. `CD 24 / C4 C4 20` -- INT 24h,
     then BOP 20h at +2, which the exec loop recognises by its ADDRESS (not its
     number: 20h is the INT 21h BOP) and takes as "the handler answered in AL".
     Five bytes, 0x4DB..0x4DF: the gap between the INT 24h default stub (0x4D8..0x4DA)
     and the INT 2Fh tables at 0x4E0. */
#define DOS_CRIT_RAISE    0x04DB
#define DOS_CRIT_RETURN   (DOS_CRIT_RAISE + 2)
/* ── ⛔ DOS_CDS_OFF IS DEAD, AND THE SPACE IT DESCRIBED IS RECLAIMED BELOW. ────
   It read: "the CDS array, 88 bytes each, LASTDRIVE of them ... 5 x 88 = 440
   bytes, ending at 0x697, inside the block."  That stopped being true in s71,
   when LASTDRIVE went back to 26 and THE CDS ARRAY MOVED INTO ITS OWN RESERVED
   BLOCK (`g_cds_seg`, main.c: DosMcbReserveTop).  The define was left behind
   and is referenced by nothing -- checked, zero call sites -- while its comment
   went on claiming 0x4E0..0x697 for a table that is no longer there.
 ⚠ A STALE RESERVATION IS WORSE THAN NO RESERVATION: it reads as "occupied" to
   anyone looking for room, so the space stays unusable for ever, and it reads as
   "this is the CDS" to anyone debugging a pointer into it.  Deleted rather than
   left as a comment, because a comment is what it already was.

   ── INT 2Fh AX=122Eh: the tables XP's COMMAND.COM asks for at startup. ───────
   Five selectors (DL = 0,2,4,6,8); it zeroes ES:DI, calls, and stores whatever
   comes back.  MEASURED on two real Microsoft kernels (tests/probes/dos/p_int2f.asm,
   docs/research/xp-command-com.md):

       DL=0  0001:0D8F      DL=2  0001:0B3B      DL=4  0001:0D8F   (== DL=0)
       DL=6  0000:0000      DL=8  03E7:0188

 ★ DL=0 AND DL=4 RETURN THE SAME POINTER on both machines, so they share a table
   here rather than getting two that happen to hold the same thing.
 ★ DL=6 IS LEGITIMATELY NULL on both, so we answer null and claim no space.
 ⚠ THE CONTENTS ARE BUILD-SPECIFIC -- 6.22 and PCem disagree on DL=0/2/4 -- so
   there is nothing canonical to copy, and these are zero-filled.  That is not a
   guess for its own sake: PCem, a machine where COMMAND.COM runs perfectly,
   returns a DL=0 table whose first 32 bytes are all zero.  ⚠ Thirty-two bytes is
   all that was measured; it is evidence, not proof. */
#define DOS_INT2F_TBL_A   0x04E0   /* 64 bytes: DL=0 and DL=4 share this     */
#define DOS_INT2F_TBL_B   0x0520   /* 64 bytes: DL=2                         */
#define DOS_INT2F_TBL_C   0x0560   /* 64 bytes: DL=8                         */
#define DOS_INT2F_TBLS_LEN 192     /* A, B and C together                    */
/* ...ending at 0x5A0, well inside the block, which runs to 0x6F0 (linear 0xFF0,
   the next MCB header). */
/* ── GH #54: INT 15h AH=C0h's SYSTEM CONFIGURATION TABLE, 10 bytes. ──────────────
   Size word 8, model FC / submodel 01 / revision 00 (an AT-class machine, which is
   what PCem's real AMI 486 BIOS reports), then five feature bytes that DESCRIBE THIS
   MACHINE -- see dos_sysconf_table() in main.c for what each bit claims and why. */
#define DOS_SYSCONF_OFF   0x05A0
/* GH #251: the AUX/PRN driver code (dos_auxprn.h, 169 bytes -> 0x659), below the
   block's end at 0x6F0. */
#define DOS_AUXPRN_OFF    0x05B0
#define DOS_AUXPRN_LEN    0x00B0
/* GH #254: the BIOS INT 09h side-calls (bios_kbdact.h). #244/#274 added the INT 15h
   AH=4Fh call and the default INT 05h: 69 bytes, so the slot grew 40h -> 50h and the
   generic stubs below moved up by 10h into the block's last free 16 bytes. */
#define DOS_KBDACT_OFF    0x0660
#define DOS_KBDACT_LEN    0x0050
/* s91 (#315): GENERIC VDD INTERRUPT STUBS -- one `BOP 5Bh ; IRET` per vector a device
   claimed that has no stub of its own (a third-party driver's INT 61h, say). 16 slots
   x 4 bytes, 0x6B0..0x6EF -- the block's last byte is 0x6EF. See vdd_plant_generic_ints.
   (Were 0x6A0..0x6DF until #244/#274 needed the room; nothing hard-codes the offset.) */
#define DOS_GENSTUB_OFF   0x06B0
#define DOS_GENSTUB_N     16
#define DOS_GENSTUB_SIZE  4      /* BOP 5Bh ; IRET                              */
#define DOS_GENSTUB_BOP   0x5B
/* The host's own stubs issue BOP n for INT n -- except INT 21h's stub, which has BOP 20h, and
   so INT 20h's, which has 30h; and the XMS far-call entry, which is BOP 43h. */
#define DOS_BOP_FOR_VECTOR(vector)  (vector)
#define DOS_BOP_INT21     0x20
#define DOS_BOP_INT20     0x30
#define DOS_BOP_XMS_ENTRY 0x43
/* Which entries of the table krnl386 actually reads, and what each becomes.
   Only these six are consulted; the rest are present so the table has stock's
   shape rather than a shorter one that happens to be enough today. */
#define DOS_WOW_E_LASTDRV 0x00     /* -> SysVars+0x21, the LASTDRIVE byte        */
#define DOS_WOW_E_CURDRV  0x0C     /* -> current-drive byte (a Win16 task's     *
                                    *    INT 21h AH=19h is answered from it)     */
#define DOS_WOW_E_C       0x10
#define DOS_WOW_E_E       0x18
#define DOS_WOW_E_D       0x24     /* krnl386 WRITES a word through this one     */
#define DOS_WOW_E_F       0x28

/* ── THE SYSTEM FILE TABLE.  krnl386 COUNTS FILE HANDLES BEFORE IT WILL START. ──
     At start-up krnl386 calls INT 21h AH=52h and walks the SFT chain from
     SysVars+4 -- each block a far "next" pointer (offset FFFFh ends the chain)
     and a word entry count -- adding up the counts, and refuses to start if the
     total is too small (see DOS_MAX_FILES).

   ⚠ SysVars+4 WAS ZERO, AND A ZERO CHAIN HEAD IS NOT AN EMPTY CHAIN.  It mapped
     a selector on 0000:0000 and read the IVT as an SFT header: word 0 there is
     not FFFFh, so it followed the "next" pointer into the ROM and round a
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
     at least 0x7f (127) of them -- 0x64 (100) in one configuration. With 64
     advertised it exited via ExitKernelThunk carrying 0x40, i.e. quoting our own
     count back at us.
   ⚠ THIS IS THE REAL TABLE, NOT A NUMBER TO SATISFY A CHECK. The SFT block
     advertises exactly DOS_SFT_ENTRIES == this, so raising what we claim also
     raises what we can actually open -- claiming 128 while keeping 64 slots is
     the "runs but lies" failure this project has paid for before. 128 clears both
     thresholds and stays inside the byte krnl386 sums the counts into (so a
     single block may not exceed 255). */
#define DOS_MAX_FILES 128
#define DOS_STD_HANDLES   5      /* stdin, stdout, stderr, AUX, PRN             */
#define DOS_HANDLE_AUX    3
#define DOS_HANDLE_PRN    4

#define DOS_SFT_ENTRIES   DOS_MAX_FILES   /* == the size of dos_machine_t::fh[]  */
#define DOS_SFT_ENTSZ     0x3B      /* DOS 4.0+ SFT entry: 59 bytes              */
#define DOS_SFT_NEXT_OFFSET  0        /* the block header: far pointer to the next block, */
#define DOS_SFT_NEXT_SEGMENT 2        /* FFFF:FFFF = the last block,                       */
#define DOS_SFT_COUNT        4        /* then the entry count                              */
#define DOS_SFT_LAST         0xFFFF
#define DOS_SFT_HEADER    6         /* the block's far "next" pointer and word entry count */
#define DOS_SFT_BYTES     (DOS_SFT_HEADER + DOS_SFT_ENTRIES * DOS_SFT_ENTSZ)
#define DOS_SFT_PARAS     ((DOS_SFT_BYTES + PARAGRAPH_LAST_BYTE) / PARAGRAPH_SIZE)

#endif /* NTVDMEX_DOS_LAYOUT_H */
