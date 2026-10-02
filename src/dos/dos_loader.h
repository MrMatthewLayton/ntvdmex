/* dos_loader.h -- load a DOS program image (MZ .EXE or flat .COM) into
 * conventional memory. Pure logic over a `base` pointer (same convention as
 * dos_mcb.h): base=NULL for the host's absolute V86 addressing, a byte buffer
 * for off-VM tests. Ported from the M2.3 loader in tools/vdmhost/vdmhost.c.
 * Verified off-VM by tools/dostest/mcb_test.c.
 */
#ifndef DOS_LOADER_H
#define DOS_LOADER_H

#include <stdint.h>
#include "dos_mcb.h"        /* mcb_at / mcb_rd16 / mcb_wr16 paragraph addressing */

typedef struct {
    uint16_t cs, ip;        /* entry CS:IP                                   */
    uint16_t ss, sp;        /* entry SS:SP                                   */
    int      is_exe;        /* 1 = MZ .EXE, 0 = flat .COM                    */
    uint32_t img_size;      /* bytes placed in conventional memory           */
} dos_image_t;

static inline uint16_t dos_ld_rd16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* Place `file` (nread bytes) into conventional memory at `base`:
 *  - MZ .EXE: load module copied to (psp_seg+0x10):0, relocations applied,
 *    CS:IP/SS:SP from the header (segment fields biased by the load segment).
 *  - flat .COM: copied to psp_seg:0x100; CS=SS=psp_seg, IP=0x100, SP=0xFFFE.
 * Returns the entry context. Mirrors vdmhost.c's M2.3 loader exactly. */
/* ── load_seg: WHERE THE IMAGE GOES, which is not always just past the PSP. ──
     An MZ image with e_minalloc = e_maxalloc = 0 is LOADED HIGH -- at the top of
     the block EXEC gave it, with the PSP still at the bottom (GH #255; see
     dos_exec_size). 0 = the ordinary place, psp_seg + 10h. Ignored for a .COM. */
static inline dos_image_t dos_load_at(volatile uint8_t *base, const uint8_t *file,
                                      uint32_t nread, uint16_t psp_seg, uint16_t at) {
    dos_image_t e;
    uint32_t i;
    if (nread >= 0x1C && file[0] == 'M' && file[1] == 'Z') {
        uint32_t hdrsize  = (uint32_t)dos_ld_rd16(file + 8) * 16;  /* e_cparhdr */
        uint32_t e_crlc   = dos_ld_rd16(file + 6);                 /* reloc count   */
        uint32_t e_lfarlc = dos_ld_rd16(file + 24);               /* reloc tbl off */
        uint16_t load_seg = at ? at : (uint16_t)(psp_seg + 0x10);
        volatile uint8_t *img = mcb_at(base, load_seg);
        uint16_t e_cblp = dos_ld_rd16(file + 2);                  /* bytes, last pg */
        uint16_t e_cp   = dos_ld_rd16(file + 4);                  /* page count    */
        uint32_t totalused = e_cp ? ((uint32_t)(e_cp - 1) * 512 + (e_cblp ? e_cblp : 512))
                                  : nread;
        uint32_t imgsize;
        if (totalused > nread) totalused = nread;
        imgsize = totalused > hdrsize ? totalused - hdrsize : 0;
        for (i = 0; i < imgsize; ++i) img[i] = file[hdrsize + i];
        for (i = 0; i < e_crlc; ++i) {                            /* apply relocations */
            uint32_t ro = dos_ld_rd16(file + e_lfarlc + i * 4);
            uint32_t rs = dos_ld_rd16(file + e_lfarlc + i * 4 + 2);
            volatile uint8_t *loc = (volatile uint8_t *)((uintptr_t)base
                                    + (((uint32_t)(load_seg + rs)) << 4) + ro);
            mcb_wr16(loc, (uint16_t)(mcb_rd16(loc) + load_seg));
        }
        e.cs = (uint16_t)(load_seg + dos_ld_rd16(file + 22));     /* e_cs */
        e.ip = dos_ld_rd16(file + 20);                            /* e_ip */
        e.ss = (uint16_t)(load_seg + dos_ld_rd16(file + 14));     /* e_ss */
        e.sp = dos_ld_rd16(file + 16);                            /* e_sp */
        e.is_exe = 1;
        e.img_size = imgsize;
    } else {                                                      /* flat .COM */
        uint32_t n = nread > 0xFE00 ? 0xFE00 : nread;
        volatile uint8_t *code = mcb_at(base, psp_seg) + 0x100;   /* psp_seg:0x100 */
        for (i = 0; i < n; ++i) code[i] = file[i];
        e.cs = psp_seg; e.ip = 0x100; e.ss = psp_seg; e.sp = 0xFFFE;
        e.is_exe = 0;
        e.img_size = n;
    }
    return e;
}

static inline dos_image_t dos_load(volatile uint8_t *base, const uint8_t *file,
                                   uint32_t nread, uint16_t psp_seg) {
    return dos_load_at(base, file, nread, psp_seg, 0);
}

/* ── THE IMAGE'S SIZE IN PARAGRAPHS, AS DOS COUNTS IT FOR MEMORY: WHOLE PAGES. ──
     e_cp * 32 - e_cparhdr -- the 512-byte page count, NOT trimmed by e_cblp (the
     bytes used in the last page). MEASURED (p_exmem.asm, 6.22 under QEMU and
     DOSBox-X agree): the child's file is 150h bytes, a 20h-byte header and 130h of
     image -- 13h paragraphs of actual bytes -- and DOS sizes its block as if the
     image were 1Eh (one page, 20h paragraphs, less the 2-paragraph header). The
     loader still COPIES only the e_cblp-trimmed bytes (dos_load_at); this is the
     size DOS RESERVES for it, and the distance below the block's end a load-high
     image is put at. */
static inline uint16_t dos_image_paras(const uint8_t *file, uint32_t nread) {
    uint32_t pages, hdr;
    if (nread < 0x1C || file[0] != 'M' || file[1] != 'Z') return 0;
    pages = (uint32_t)dos_ld_rd16(file + 4) * 32u;
    hdr   = dos_ld_rd16(file + 8);
    return (uint16_t)(pages > hdr ? pages - hdr : 0);
}

/* ── ★ HOW MUCH MEMORY EXEC GIVES AN MZ PROGRAM. (GH #255) ──────────────────────
     EXEC used to hand every child ALL of the largest free block, whatever the
     header said. The header says two things about memory beyond the image:
       e_minalloc  paragraphs it CANNOT run without -- not there => error 8, and the
                   program is never loaded;
       e_maxalloc  paragraphs it would LIKE -- the block is cut down to that, which is
                   how a program linked /CPARMAXALLOC leaves memory for its children.
     And one combination is special: minalloc = maxalloc = 0 means LOAD HIGH -- the
     whole block, with the image at its TOP.
     ★ MEASURED, NOT DERIVED (tools/dostest/p_exmem.asm; 6.22 under QEMU and
       DOSBox-X agree): a child whose image DOS counts as 1Eh paragraphs (see
       dos_image_paras -- whole pages) with min/max = 100h/200h gets a block of
       10h (PSP) + 1Eh + 200h = 22Eh; max 10h gives 3Eh; min F000h is refused with
       AX=0008 and never runs; min/max 0/0 puts CS 1Eh paragraphs below the block's
       end; max FFFFh takes the whole largest block. Every child used to get 8AE2h
       paragraphs -- all of it -- on the rig.
     Inputs: the file, and `largest` = the largest free block in paragraphs. Out:
     *alloc (paragraphs to allocate, PSP included) and *high (1 = load high).
     Returns 0, or 8 (DOS's "insufficient memory") when even minalloc will not fit.
     A .COM (or anything not MZ) is not sized here: it takes the largest block. */
static inline int dos_exec_size(const uint8_t *file, uint32_t nread, uint16_t largest,
                                uint16_t *alloc, int *high) {
    uint32_t imgpara, need, want;
    uint16_t mina, maxa;
    *high = 0;
    if (nread < 0x1C || file[0] != 'M' || file[1] != 'Z') { *alloc = largest; return 0; }
    imgpara = dos_image_paras(file, nread);
    mina = dos_ld_rd16(file + 10);
    maxa = dos_ld_rd16(file + 12);
    need = 0x10 + imgpara + mina;
    if (need > largest) return 8;
    if (mina == 0 && maxa == 0) { *high = 1; *alloc = largest; return 0; }
    want = 0x10 + imgpara + maxa;
    if (want < need) want = need;              /* a maxalloc below minalloc */
    *alloc = (uint16_t)(want < largest ? want : largest);
    return 0;
}

/* ── ★ IS THIS A DOS PROGRAM AT ALL? (GH #255) ──────────────────────────────────
     A Windows program is an MZ file too: its MZ part is a STUB ("This program
     cannot be run in DOS mode" / "requires Microsoft Windows"), and e_lfanew at 3Ch
     points at the real header. MS-DOS runs the stub, because to DOS that is the
     program. NTVDM does not: an NE goes to WOW and a PE to Win32, so on stock XP
     typing NOTEPAD at COMMAND.COM starts Notepad. We are the NTVDM replacement, so
     EXEC asks this before loading anything.
   ⚠ ONLY WINDOWS'S OWN TWO SIGNATURES, AND ONLY A WINDOWS NE. The 3Ch slot is read
     whatever the file is, so anything else must stay a DOS program:
       "LE"/"LX"  -- a DOS/4GW-bound game (Doom, Duke3D): the stub IS the extender;
       NE with target OS 1 (OS/2) -- a BIND'ed "family API" program, whose stub IS
                  the DOS version of the program (XP has no OS/2 subsystem either);
       an e_lfanew below 40h or past the end of what was read -- an old DOS .EXE
                  with whatever happens to sit at 3Ch.
     NE target OS (ne_exetyp, NE+36h): 2 = Windows; 0 = unspecified, which is what
     Windows 1.x/2.x programs carry (the field came later) -- both are WOW's.
     Out (PE only): *subsys = the PE optional header's Subsystem (2 GUI, 3 console),
     which decides whether EXEC waits for it. */
#define DOS_EXE_DOS 0       /* load it ourselves: .COM, plain MZ, LE/LX, OS/2 NE   */
#define DOS_EXE_NE  1       /* a Windows NE: WOW's                                 */
#define DOS_EXE_PE  2       /* a PE: Win32's                                       */
static inline int dos_exe_kind(const uint8_t *file, uint32_t nread, unsigned *subsys) {
    uint32_t lf;
    *subsys = 0;
    if (nread < 0x40 || file[0] != 'M' || file[1] != 'Z') return DOS_EXE_DOS;
    lf = (uint32_t)dos_ld_rd16(file + 0x3C) | ((uint32_t)dos_ld_rd16(file + 0x3E) << 16);
    if (lf < 0x40 || lf > nread - 4) return DOS_EXE_DOS;
    if (file[lf] == 'P' && file[lf + 1] == 'E' && file[lf + 2] == 0 && file[lf + 3] == 0) {
        if (lf + 0x5E <= nread) *subsys = dos_ld_rd16(file + lf + 0x5C);
        return DOS_EXE_PE;
    }
    if (file[lf] == 'N' && file[lf + 1] == 'E' && lf + 0x37 <= nread) {
        uint8_t os = file[lf + 0x36];
        if (os == 2 || os == 0) return DOS_EXE_NE;
    }
    return DOS_EXE_DOS;
}

/* ── AH=4Bh AL=03: LOAD AN OVERLAY. (GH #50) ──────────────────────────────────
   Not a process: no PSP, no memory allocated, no transfer of control. The caller
   says WHERE to put it (load_seg) and, separately, WHAT TO RELOCATE BY
   (reloc_factor) -- and those two are not the same number. A large program swaps
   overlays into one buffer it already owns, so the relocation factor is the
   buffer's segment while the load segment may differ.
 ★ THE FACTOR, NOT THE LOAD SEGMENT. tools/dostest/p_ovl.asm passes a factor of
   0x1234 that is deliberately NOT the load segment, and MS-DOS 6.22 relocates by
   the factor:
       CASE=ovl.relocated.word SIG=AX AX=1234
   which separates the three ways to be wrong -- 0 (never relocated), the load
   segment (relocated by the wrong value), or their sum (relocated twice). */
static inline uint32_t dos_load_overlay(volatile uint8_t *base, const uint8_t *file,
                                        uint32_t nread, uint16_t load_seg,
                                        uint16_t reloc_factor) {
    uint32_t i, hdrsize, e_crlc, e_lfarlc, totalused, imgsize;
    uint16_t e_cblp, e_cp;
    volatile uint8_t *img;
    if (nread < 0x1C || file[0] != 'M' || file[1] != 'Z') {
        /* A .COM overlay is the file, verbatim, with nothing to relocate. */
        img = mcb_at(base, load_seg);
        for (i = 0; i < nread; ++i) img[i] = file[i];
        return nread;
    }
    hdrsize  = (uint32_t)dos_ld_rd16(file + 8) * 16;
    e_crlc   = dos_ld_rd16(file + 6);
    e_lfarlc = dos_ld_rd16(file + 24);
    e_cblp   = dos_ld_rd16(file + 2);
    e_cp     = dos_ld_rd16(file + 4);
    totalused = e_cp ? ((uint32_t)(e_cp - 1) * 512 + (e_cblp ? e_cblp : 512)) : nread;
    if (totalused > nread) totalused = nread;
    imgsize = totalused > hdrsize ? totalused - hdrsize : 0;
    img = mcb_at(base, load_seg);
    for (i = 0; i < imgsize; ++i) img[i] = file[hdrsize + i];
    for (i = 0; i < e_crlc; ++i) {
        uint32_t ro = dos_ld_rd16(file + e_lfarlc + i * 4);
        uint32_t rs = dos_ld_rd16(file + e_lfarlc + i * 4 + 2);
        volatile uint8_t *loc = (volatile uint8_t *)((uintptr_t)base
                                + (((uint32_t)(load_seg + rs)) << 4) + ro);
        mcb_wr16(loc, (uint16_t)(mcb_rd16(loc) + reloc_factor));
    }
    return imgsize;
}

#endif /* DOS_LOADER_H */
