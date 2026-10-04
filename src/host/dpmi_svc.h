/* dpmi_svc.h -- the INT 31h answers that are decided by the DPMI specification, not by
 * the host's plumbing. GH #248.
 *
 * Header-only and dependency-free (stdint, no windows.h), the same shape as dpmi_rmcs.h,
 * so the off-VM battery (tools/dostest/dpmisvc_test.c) compiles exactly the code the host
 * runs. What lives here is the part of each service with a RIGHT ANSWER: which selector
 * is invalid, which callback address names which slot, whether a resize can stay put.
 * The part that touches the LDT, VirtualAlloc or the guest's memory stays in main.c.
 *
 * ⚠ THE SPECIFICATION IS NOT IN THE REPO (docs/ref/SOURCES.md). Everything below that
 *   says "the spec" is the DPMI 0.9/1.0 text as published by the DPMI Committee; the
 *   error numbers are 1.0's, which 0.9 hosts return too (0.9 only promises CF).
 * ⚠ NO ORACLE HERE ANSWERS INT 31h. MS-DOS 6.22, DOSBox-X and PCem all run without a
 *   DPMI host (tools/dostest/p_dpmins.com: AX comes back 1687h untouched on all three),
 *   so the spec is the only authority for this file, and stock ntvdm -- the one machine
 *   that could answer -- needs the IFEO bracket.
 */
#ifndef NTVDMEX_DPMI_SVC_H
#define NTVDMEX_DPMI_SVC_H

#include <stdint.h>

/* ── ERROR CODES (DPMI 1.0 numbering; returned in AX with CF=1). ─────────────────────── */
#define DPMI_E_DESC_UNAVAIL   0x8011   /* descriptor unavailable                        */
#define DPMI_E_LIN_UNAVAIL    0x8012   /* linear memory unavailable                     */
#define DPMI_E_PHYS_UNAVAIL   0x8013   /* physical memory unavailable                   */
#define DPMI_E_CB_UNAVAIL     0x8015   /* callback unavailable                          */
#define DPMI_E_HANDLE_UNAVAIL 0x8016   /* handle unavailable                            */
#define DPMI_E_INVALID_VALUE  0x8021   /* invalid value                                 */
#define DPMI_E_INVALID_SEL    0x8022   /* invalid selector                              */
#define DPMI_E_INVALID_HANDLE 0x8023   /* invalid handle                                */
#define DPMI_E_INVALID_CB     0x8024   /* invalid callback                              */
#define DPMI_E_INVALID_LINEAR 0x8025   /* invalid linear address (#268)                 */

/* ── ONE MACHINE, DESCRIBED ONCE: 1687h AND 0400h. ──────────────────────────────────────
     Both report the processor class in CL, and they disagreed: 1687h said 4 (s79, the
     GetWinFlags fix -- see the long note at DPMI_CPU_CLASS's old home in main.c and
     getwinflags-is-a-dpmi-answer) while 0400h still said 3, the value that fix replaced.
     A client that asks both was told two different machines. Every site now reads these.
   ► CL: the spec's processor type (02h 286, 03h 386, 04h 486, 05h Pentium). The value is
     the s79 measurement's -- stock answers CL > 3 to krnl386 -- not a claim about the
     host CPU, which is past a 486 several times over.
   ► 0400h BX: bit 0 = a 32-bit host (true: 32-bit clients run); bit 1 = interrupts are
     reflected to VIRTUAL 8086 mode (false: we report what 0400h always reported, and a
     client that cares reads it only as a hint); bit 2 = virtual memory (false: 0501h
     memory is committed at allocation).
   ► 0400h DX: DH = master PIC base 08h, DL = slave PIC base 70h -- where the IRQs land. */
#define DPMI_CPU_CLASS   0x04
#define DPMI_VER_AX      0x005A        /* AH = 0 major, AL = 90 (5Ah) minor -> "0.90"   */
#define DPMI_VER_BX      0x0001
#define DPMI_VER_DX      0x0870

/* ── IS THIS A SELECTOR THE CLIENT MAY NAME? (0001h, 0007h-000Ah, 0101h) ───────────────
     These all reported a bad selector as SUCCESS -- 0001h kept it and said nothing,
     0007h-0009h ignored the call, 000Ah aliased the code segment instead. A client that
     checks CF (most C runtimes do, through their int386x wrapper) then went on believing
     a descriptor had been set that never was; the fault arrives later, somewhere else.
   The spec's 8022h is "the selector is not a valid LDT selector, or is not allocated".
     Three things make one invalid here:
       - TI = 0: a GDT selector is never the client's;
       - index 0 (the null selector) or at/after the end of our table;
       - an index the host never allocated -- UNLESS THE GUEST OWNS THE TABLE.
   ★ THAT LAST EXCEPTION IS krnl386. Under WOW the client manages the LDT itself through
     NTVDM's vendor window (04F1h/04F2h, the descriptor shadow): it picks free indices
     out of its OWN free list, writes them, and calls 0007h/0008h/000Ch on them -- often
     before (or without) the 04F2h commit that would teach our table about them. Measured
     in s84 logs: 0007h on 0x2ef, 0xc97, 0xcaf ... none of which we handed out. There the
     allocation record is the guest's, not ours, and refusing would kill Win16 -- so in
     that mode only the range is checked. (`guest_owns_table` = the shadow exists.) */
static int dpmi_sel_valid(uint16_t sel, int idx_max, int allocated, int guest_owns_table)
{
    int idx = sel >> 3;
    if (!(sel & 4)) return 0;                        /* TI = 0: GDT          */
    if (idx < 1 || idx >= idx_max) return 0;         /* null, or off the end */
    if (!allocated && !guest_owns_table) return 0;   /* never handed out     */
    return 1;
}

/* ── REAL-MODE CALLBACKS: SIXTEEN, AND THEY CAN BE GIVEN BACK. (0303h/0304h) ───────────
     The spec has a host provide AT LEAST 16 callbacks per client; we had 4, and no 0304h,
     so a client that allocates and frees -- a mouse driver hook per mode set, a
     Ctrl-Break handler installed and removed -- ran out on its fifth and could never
     recover one. Each slot is a 4-byte real-mode entry `C4 C4 55 xx` (a BOP the host
     traps) in DOS_HDLR_SEG; the slot is named by WHERE the BOP executes, so the address
     IS the slot and 0304h decodes it back with no table lookup.
   ⚠ THEY MOVED, from 0x60..0x6F to DOS_HDLR_SEG:0x90..0xCF. Sixteen do not fit where four
     did (0x70 is the PM-return catcher, 0x74 the raw-switch entry), and 0x84..0xDF was the
     only 64-byte run the segment map had left. The old place was also borrowed by the
     opt-in entry trampoline (0x60..0x65) -- that collision is gone with the move.
   ► `ip` is the BOP's own address at the trap (the nested loops step past a BOP with
     `EIP += 3` themselves); an address inside the 4-byte stub maps to the same slot by
     the integer division, so a caller that has already stepped is not misread. */
#define DPMI_CB_SLOTS      16
#define DPMI_CB_STRIDE     4

static uint16_t dpmi_cb_entry(uint16_t base, int slot)
{
    return (uint16_t)(base + slot * DPMI_CB_STRIDE);
}
/* The slot whose BOP is executing at CS:IP, or -1. For the trap path. */
static int dpmi_cb_slot_at(uint16_t base, uint16_t cs, uint16_t want_cs, uint16_t ip)
{
    int s;
    if (cs != want_cs || ip < base) return -1;
    s = (ip - base) / DPMI_CB_STRIDE;
    return (s < DPMI_CB_SLOTS) ? s : -1;
}
/* 0304h: the slot whose ADDRESS is exactly CX:DX, or -1. Stricter than the trap path:
   the client must hand back the address it was given, not something inside the stub. */
static int dpmi_cb_slot_of(uint16_t base, uint16_t cs, uint16_t want_cs, uint16_t off)
{
    int s = dpmi_cb_slot_at(base, cs, want_cs, off);
    return (s >= 0 && dpmi_cb_entry(base, s) == off) ? s : -1;
}

/* ── 0503h RESIZE MEMORY BLOCK: STAY PUT IF IT FITS, OTHERWISE MOVE AND COPY. ──────────
     The spec lets the host move the block (it returns a new linear address AND a new
     handle) and leaves fixing up descriptors to the client. A 0501h block here is one
     VirtualAlloc, committed to the page; it can grow in place only into pages it already
     has, because a reservation cannot be extended. So:
       new size 0                      -> 8021h (the spec's invalid value)
       new size <= committed bytes     -> in place, same address, same handle
       otherwise                       -> a new block, copy min(old, new), free the old.
     `committed` is the allocation's page-rounded size as the OS reports it. */
#define DPMI_RESIZE_BAD    0
#define DPMI_RESIZE_INPLACE 1
#define DPMI_RESIZE_MOVE   2
static int dpmi_resize_plan(uint32_t new_size, uint32_t committed, uint32_t *copy)
{
    if (copy) *copy = 0;
    if (new_size == 0) return DPMI_RESIZE_BAD;
    if (new_size <= committed) return DPMI_RESIZE_INPLACE;
    if (copy) *copy = committed;                     /* new > committed: all of the old */
    return DPMI_RESIZE_MOVE;
}

/* ══ #268: THE #248 REMAINDERS -- VALUES, NOT JUST SELECTORS. ══════════════════════════
     #248 made a bad SELECTOR an error. These are the calls whose selector is fine but
     whose VALUE is not -- the spec's 8021h (invalid value) and 8025h (invalid linear
     address) -- plus the two services whose answer was a shape rather than a check:
     0100h above 64 KB, and 0500h's 48-byte block.
   ⚠ UNDER WOW NONE OF THE VALUE CHECKS APPLY (`guest_owns_table`), for the same reason
     dpmi_sel_valid() checks only the range there: krnl386 owns the table, it was never
     measured against these refusals, and a refused descriptor call is a dead Win16
     session rather than a visible error. The DOS shelf is where the spec is enforced. */

/* ── 0008h: A LIMIT ABOVE 1 MB MUST BE PAGE-GRANULAR. ──────────────────────────────────
     The spec, 0008h: "Limits greater than 1 MB must have the low 12 bits set" -- above
     1 MB the descriptor holds the limit in 4 KB units (G = 1), so 0x123456 is a number no
     descriptor can carry, and the host must not silently round it. We used to: the
     install path shifted it right by 12, and the client got a segment 0x1000-ish bytes
     different from the one it asked for without being told. 8021h is the answer.
   ► 0xFFFFFFFF (DOS/4GW's flat 4 GB) has its low 12 bits set and PASSES; it is then
     clamped to XP's LDT cap by dpmi_install() -- a separate, logged deviation (an NT LDT
     cannot hold a 4 GB descriptor at all; stock ntvdm is under the same cap).
   ► 16-bit hosts must also refuse CX != 0; we are a 32-bit host (0400h BX bit 0). */
static int dpmi_limit_ok(uint32_t limit)
{
    if (limit <= 0xFFFFFu) return 1;
    return (limit & 0xFFFu) == 0xFFFu;
}

/* ── 0009h: THE ACCESS-RIGHTS WORD, CHECKED AS THE SPEC DESCRIBES IT. ──────────────────
     CL is the descriptor's access byte, CH (32-bit hosts) its byte 6:
       CL bit 7   P     present                    -- either value is legal
       CL 6-5     DPL   "must equal caller's CPL"  -- ⚠ NOT ENFORCED, see below
       CL bit 4   S     must be 1 (code or data; a system descriptor is not the client's)
       CL bit 3         1 = code, 0 = data
       CL bit 2         data: expand-down;  code: must be 0 (no conforming code)
       CL bit 1         data: writable;     code: readable
       CL bit 0         accessed
       CH bit 7   G     CH bit 6  B/D        CH bit 5  must be 0       CH bit 4  available
       CH 3-0           ignored (the limit's top nibble; 0008h sets the limit)
   ⚠ THE DPL RULE IS DELIBERATELY NOT ENFORCED. ZAR (DOS/16M) builds its VESA LFB
     selector with 0009h CX=8092h -- DPL 0 -- and ZAR running is the project's acceptance
     test. dpmi_install() forces every present descriptor to DPL 3 instead (s74c, the
     ZAR VESA fix); enforcing the spec here would refuse the call that fix exists for.
     A spec-strict host (HDPMI, CWSDPMI) is expected to answer 8021h there; that is a
     measured-on-the-oracles question p_dpmi2 asks (`int31.0009.dpl0`). */
static int dpmi_access_ok(uint16_t cx)
{
    uint8_t cl = (uint8_t)cx, ch = (uint8_t)(cx >> 8);
    if (!(cl & 0x10)) return 0;                      /* S = 0: a system descriptor     */
    if ((cl & 0x08) && (cl & 0x04)) return 0;        /* conforming code                */
    if (ch & 0x20) return 0;                         /* CH bit 5 must be 0             */
    return 1;
}

/* ── 0007h: A BASE NO DESCRIPTOR CAN CARRY IS 8025h. ───────────────────────────────────
     The spec (1.0) has 0007h answer 8025h, invalid linear address, when the new base
     would put the segment outside the client's linear space. Here that space is the
     process's user half: XP's LDT validator refuses any descriptor whose base lies past
     MmHighestUserAddress (`cap`, XP_LDT_MAX_LINEAR in main.c), whatever its limit -- and
     what used to happen was a "DPMI-LDT: install REJECTED ... clamp ALSO refused" line
     in our log, CF=0 to the client, and the OLD descriptor still live in the real LDT
     while our table said otherwise.
   ► Only the BASE is judged. base + limit past the cap is the flat-selector case
     (DOS/4GW: base 0, then 0008h 4 GB) that dpmi_install() clamps and installs; refusing
     it would break every DOS/4GW game. */
static int dpmi_base_ok(uint32_t base, uint32_t cap)
{
    return base <= cap;
}

/* ── 0100h ABOVE 64 KB: A CHAIN OF DESCRIPTORS, ONE PER 64 KB. ─────────────────────────
     The spec, 0100h: "If the size of the block requested is greater than 64K bytes
     (BX > 1000h) then contiguous descriptors will be allocated", the base selector is
     returned, and each next descriptor (reached through 0003h's increment) is based
     64 KB above the previous one with a 64 KB limit -- the last one holding what is
     left. "If more than one descriptor is allocated under 32-bit DPMI hosts, the limit
     of the first descriptor will be set to the size of the entire block" (16-bit hosts
     give it 64 KB). We are a 32-bit host, so descriptor 0 spans the whole block.
     We used to allocate ONE descriptor whose limit was the whole block: right for a
     32-bit client that only uses the first selector, and a #GP for the 16-bit client
     the chain exists for -- it adds 8 to reach the second 64 KB and finds whatever
     descriptor happened to be next. ⚠ No shelf guest has been seen to ask for more than
     64 KB of DOS memory; the evidence is the spec text and `p_dpmi2`'s chain rows.
   ► `paras` 0 keeps today's single descriptor with limit 0. */
static int dpmi_dosmem_count(uint16_t paras)
{
    return paras ? (int)(((uint32_t)paras + 0xFFFu) >> 12) : 1;
}
/* Descriptor `i` of a block of `paras` paragraphs: offset of its base from the block's
   start, and its byte limit. */
static void dpmi_dosmem_desc(uint16_t paras, int i, uint32_t *base_off, uint32_t *limit)
{
    uint32_t bytes = (uint32_t)paras << 4, off = (uint32_t)i << 16, left;
    *base_off = off;
    if (!paras) { *limit = 0; return; }
    if (i == 0) { *limit = bytes - 1; return; }      /* 32-bit host: the whole block   */
    left = bytes - off;
    *limit = (left > 0x10000u ? 0x10000u : left) - 1;
}

/* ── 0500h: THE WHOLE 30h-BYTE BLOCK, AND THE FREE COUNTS MOVE WITH ALLOCATION. ────────
     The 0.9 layout:
       00 largest available free block, BYTES      04 maximum unlocked page allocation
       08 maximum locked page allocation           0C linear address space size, pages
       10 total number of unlocked pages           14 total number of free pages
       18 total number of physical pages           1C free linear address space, pages
       20 size of paging file/partition, pages     24..2F reserved, all FFh
     "fields the host cannot supply are -1". Ours are all knowable: 0501h is a
     VirtualAlloc in our own process against a nominal pool of `pool_pages`, and
     `used_pages` is what the client holds right now (the live 0501h blocks, page-
     rounded). Before #268 every field said the full pool for ever -- a client that
     allocates and then sizes the next request from free pages was told nothing had
     changed. No pages are ever locked away from the client (0600h is a no-op: nothing
     here pages out), so the unlocked/locked maxima are both the free count; there is no
     paging file (0400h BX bit 2 = 0, no virtual memory), so +20 is 0, not -1.
   ⚠ THE POOL IS A PROMISE, NOT A LIMIT. 0501h does not refuse past it; the number exists
     so a client that sizes its heap from +00 gets a sane answer. */
#define DPMI_MEMINFO_DWORDS 12
static void dpmi_meminfo(uint32_t out[DPMI_MEMINFO_DWORDS], uint32_t pool_pages, uint32_t used_pages)
{
    uint32_t freep = used_pages < pool_pages ? pool_pages - used_pages : 0;
    int i;
    for (i = 0; i < DPMI_MEMINFO_DWORDS; ++i) out[i] = 0xFFFFFFFFu;
    out[0] = freep << 12;                            /* largest free block, bytes      */
    out[1] = freep;                                  /* max unlocked page allocation   */
    out[2] = freep;                                  /* max locked page allocation     */
    out[3] = pool_pages;                             /* linear address space, pages    */
    out[4] = pool_pages;                             /* total unlocked pages           */
    out[5] = freep;                                  /* free pages                     */
    out[6] = pool_pages;                             /* total physical pages           */
    out[7] = freep;                                  /* free linear address space      */
    out[8] = 0;                                      /* no paging file                 */
}

#endif /* NTVDMEX_DPMI_SVC_H */
