/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * X86 instruction LENGTH decoding, and the instruction-boundary test the
 * INT-site patcher needs.  Header-only, no CRT, no <windows.h> (so the off-VM battery
 * can compile it with the native cc).
 *
 * WHY THIS EXISTS -- IT IS THE BUG THAT KILLED DOOM FOR FIVE SESSIONS.
 * Protected-mode `INT nn` is the one fault XP will not reflect, so dpmi_patch_code_region()
 * rewrites every `CD nn` in the client's code to a BOP (`C4 C4`) and services it in the
 * host.  It did that by scanning for the BYTE PAIR, with no idea where instructions start.
 *
 * In Doom's code object that is wrong in exactly three places, and one of them is fatal:
 *
 *    obj1+0x3593f   39 fa  7e cd  31 c9        cmp edx,edi / jle -51 / xor ecx,ecx
 *                          ^^ ^^^^^^^
 *    The `cd` is the DISPLACEMENT of `jle`, and the `31` is the opcode of the `xor`.
 *    Patching turned that into
 *                          7e c4  c4 c9        jle -60 / (garbage)
 *    i.e. R_InitTextureMapping's loop-2 back edge now jumps to obj1+0x35905, which is
 *    the middle of a `jl` -- and the guest executes `cmp ecx,[ebx+0x034fe02d]` off a
 *    register holding an angle.  Wild read, #PF at CPL 3, and XP tears the VDM down
 *    silently: no VEH, no watchdog line, no last log entry.  Sessions 16-20 chased that
 *    as a mystery in Doom.  It was ours.
 *    (The other two: obj1+0x0ae0f is the displacement of a `call rel32`, obj1+0x0512d
 *    is a word in a data table.  Both were being corrupted too.)
 *
 * - THE RULE: PATCH ONLY WHAT IS AN INSTRUCTION.  x86 is self-synchronising -- decode
 *   forward from a few dozen earlier offsets and the streams converge on the real
 *   boundaries within a handful of instructions.  So for each candidate site, decode
 *   from each of the preceding `span` bytes and count how many land exactly on it.
 *   Measured against objdump over Doom's 32-bit code object and DOS/4GW's two 16-bit
 *   modules (242 real INT sites, 7 false byte pairs):
 *
 *      real sites   19..48 votes out of 48        -> all 242 kept
 *      false pairs   0.. 3 votes out of 48        -> all   7 rejected
 *
 *  A 25% threshold sits in the middle of that gap with room on both sides.  It is
 *  deliberately biased toward KEEPING: a missed real site is an unpatched `CD nn` that
 *  kills the guest, while a rejected false one only costs a service we never needed.
 *
 * - SCOPE.  This decodes LENGTHS, not semantics.  It has no notion of what an
 *   instruction does and never needs one.  Undecodable opcodes return 0, which the
 *   boundary test reads as "this stream is not code" -- a vote against, which is the
 *   conservative direction for a stream that started mid-instruction.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef HOST_X86LEN_H
#define HOST_X86LEN_H

#include "../ntvdmex_types.h"

/* imm kinds. `z` = 2 bytes with a 16-bit operand size, 4 with a 32-bit one. */
#define XL_NONE                     0
#define XL_IB                       1       /* imm8 */
#define XL_IZ                       2       /* imm16/imm32 by operand size */
#define XL_IW                       3       /* imm16 always (ret imm16) */
#define XL_MOFF                     4       /* Moffs: 2/4 by ADDRESS size */
#define XL_ENTER                    5       /* imm16 + imm8 */
#define XL_FAR                      6       /* ptr16:16/32 -> z + 2 */
#define XL_G3B                      7       /* Group 3 /0,/1 take imm8 */
#define XL_G3Z                      8       /* Group 3 /0,/1 take immz */

#define XL_MR                       0x10    /* Has a modrm byte */
#define XL_KIND_MASK                0x0F    /* The immediate kind, below XL_MR */

/* The instruction set's own numbers. */
#define X86_MODRM_MODE_SHIFT        6
#define X86_MODRM_REG_SHIFT         3
#define X86_MODRM_FIELD_MASK        7
#define X86_MODE_DISP8              1
#define X86_MODE_DISP_FULL          2
#define X86_MODE_REGISTER           3
#define X86_RM_SIB                  4       /* 32-bit addressing: a SIB byte follows */
#define X86_RM_DISP32               5       /* ...mode 0: disp32, no base */
#define X86_RM16_DISP16             6       /* 16-bit addressing, mode 0: disp16 */
#define X86_DISP16_SIZE             2
#define X86_DISP32_SIZE             4
#define X86_IMM16_SIZE              2u
#define X86_IMM32_SIZE              4u
#define X86_SELECTOR_SIZE           2
#define X86_ENTER_IMMEDIATE_SIZE    3       /* imm16 + imm8 */
#define X86_GROUP3_IMMEDIATE_FORMS  2       /* /0 and /1 (TEST) take an immediate */
#define X86_MAX_PREFIXES            8       /* More is prefix soup, not code */
#define X86_MAX_INSTRUCTION         16u     /* Longer than any real instruction */
#define X86_VOTE_FRACTION           4       /* A quarter of the streams must agree */

/* Does an instruction START at b[off]?  Decodes forward from each of the preceding
 * `span` bytes and counts how many streams land exactly on `off`.  See the header
 * commentary for the measured separation and why the threshold is a quarter.
 */
#define X86_BOUNDARY_SPAN           48u

/* Length in bytes of the instruction at b[i], or 0 if it cannot be decoded / runs off
 * the end.  `d32` is the code segment's D/B bit (1 = 32-bit default operand+address).
 */
UINT X86InstructionLength(const BYTE *bytes, UINT offset, UINT length, INT isDefault32);

INT X86IsInstructionStart(const BYTE *bytes, UINT offset, UINT length, INT isDefault32);

/* May the `CD nn` at b[off] be rewritten to a BOP?
 *
 * - THE TWO ERRORS USED TO BE SYMMETRIC. THEY ARE NOT ANY MORE, AND THAT IS THE
 *   WHOLE OF THIS RULE.  Both failures were measured on the test machine, one after the other:
 *     accepted a false one  -> Doom died in R_InitTextureMapping (five sessions lost)
 *     refused a real one    -> the run died inside DOS/4GW's own startup, at its
 *                              `mov ah,30h / int 21h` DOS-version check, 54,000 log
 *                              lines earlier
 *   Because refusing was ALSO fatal, this rule was deliberately narrow: it rejected a
 *   site only when it could name the owning instruction AND that instruction was a
 *   RELATIVE BRANCH -- the class that provably rewrites a jump target.  Everything
 *   else was kept.  "Reject anything a confirmed instruction covers" was tried and
 *   reverted, because DOS/4GW's version check is preceded by the string
 *   "requires DOS/16M\n\r$": every backward anchor decodes ASCII, the real site scores
 *   1 vote in 48, and the ASCII stream's `30 cd` (xor ch,cl) "covers" it with 47.  By
 *   coverage alone that was indistinguishable from Doom's `jle`.
 *
 * - SESSION 39: THE SECOND HALF OF THAT PREMISE IS NO LONGER TRUE.  A raw `CD nn`
 *   in protected mode is not fatal any more.  Since session 34 a #GP whose error code
 *   has the IDT bit set IS that interrupt: the host takes the vector out of the error
 *   code, CONFIRMS it against the two bytes at the faulting address, services it, and
 *   patches the site on the way past (see the `#GP(IDT) is a RAW INT` arm in main.c).
 *   That patch needs no heuristic at all -- the CPU has just executed those two bytes
 *   AS an interrupt, which is the strongest possible evidence, and it is exactly what
 *   this vote has only ever been trying to approximate.
 *
 * So the costs have INVERTED:
 *     false accept -> silent code corruption, fatal, and hard to find
 *     false reject -> one extra #GP, serviced, then patched correctly and for good
 * and the rule must follow the premise: WHEN IN DOUBT, REJECT.
 *
 * - WHAT FORCED IT (session 39, GH #128).  A `cmp cl,ch` (3a cd) followed by a
 *   `jne` (75 xx) in krnl386's code: the `cd 75` spanning the two is not an
 *   instruction; the vote for starting at the `cd` failed, the owner was correctly
 *   named as the `cmp` -- and the `cmp` is not a relative branch, so the old rule
 *   KEPT it.  `cd 75` became `c4 c4`, the `jne` became an `les`, and WOWEXEC died with
 *   "General Protection Fault in module KRNL386.EXE at 0001:2053" the moment it tried
 *   to launch an application.  Found by the method that found Doom's: when a guest
 *   dies at an address, diff the bytes there against the file on disk -- one byte
 *   differed, and the scan's own log line already named the offset it had patched.
 *
 * - THE DOS/4GW SITE IS NOW REJECTED TOO, AND THAT IS THE INTENDED OUTCOME rather
 *   than a regression this rule tolerates: it is a real `int 21h`, it is left raw, the
 *   first execution faults, and the #GP(IDT) arm services and patches it.  One fault,
 *   once.
 *
 * [CAUTION]: THE ONE CASE THAT STILL NEEDS THE SCAN is a code selector whose base is 0: the
 * #GP(IDT) arm guards on `gcb &&`, so it declines to service there.  A flat base-0
 * code region is not scanned as a range either, so those sites were already raw
 * before this change -- it makes nothing worse -- but that is the gap to close if a
 * guest is ever seen dying on a raw INT after this.
 */
INT X86IsIntSiteReal(const BYTE *bytes, UINT offset, UINT length, INT isDefault32);

#endif /* HOST_X86LEN_H */
