/* exec_test.c -- EXEC's two header questions, pinned off-VM.  GH #255.
 *
 *   1. dos_exe_kind: is this a DOS program, or a Windows one EXEC must hand to
 *      Windows? Built from synthetic headers -- including the three that must NOT be
 *      taken for Windows (an LE-bound DOS/4GW game, a BIND'ed OS/2 family program,
 *      an old .EXE with junk at 3Ch).
 *   2. dos_exec_size: how much memory the header buys. The expectations are the
 *      rows p_exmem.asm measured on MS-DOS 6.22, PCem and DOSBox-X, quoted by name;
 *      the children are p_exmemc.asm's header, rebuilt here (one 512-byte page, a
 *      32-byte header, 130h bytes of image -- which DOS counts as 1Eh paragraphs).
 *
 *   cc -std=c99 -I src/dos -o exec_test tests/unit/exec_test.c && ./exec_test
 */
#include <stdio.h>
#include <string.h>
#include "dos_loader.h"

static int checks, fails;

static void eq(const char *what, long got, long want)
{
    ++checks;
    if (got == want) return;
    ++fails;
    printf("  FAIL %-58s got 0x%lX, want 0x%lX\n", what, got, want);
}

static uint8_t f[0x400];

static void w16(unsigned off, unsigned v) { f[off] = (uint8_t)v; f[off + 1] = (uint8_t)(v >> 8); }

/* an MZ file with e_lfanew = lf and `sig` there */
static void mk(unsigned lf, const char *sig, unsigned extra_off, unsigned extra)
{
    memset(f, 0, sizeof f);
    f[0] = 'M'; f[1] = 'Z';
    w16(8, 4);                                  /* 64-byte header */
    w16(0x18, 0x40);                            /* e_lfarlc */
    w16(0x3C, lf);
    if (sig) memcpy(f + lf, sig, strlen(sig));
    if (extra_off) w16(lf + extra_off, extra);
}

/* p_exmemc.asm's child: 32-byte header, 130h-byte image, min/max as given */
static uint32_t child(unsigned mina, unsigned maxa)
{
    uint32_t len = 0x20 + 0x130;
    memset(f, 0, sizeof f);
    f[0] = 'M'; f[1] = 'Z';
    w16(2, len % 512); w16(4, (len + 511) / 512);
    w16(8, 2); w16(10, mina); w16(12, maxa);
    w16(16, 0x130); w16(0x18, 0x1C);
    return len;
}

int main(void)
{
    unsigned sub;
    uint16_t alloc; int high, rc;
    uint32_t n;

    /* ── 1. what kind of program ── */
    mk(0x80, "PE\0\0", 0x5C, 2);
    eq("PE, GUI subsystem -> PE",        dos_exe_kind(f, sizeof f, &sub), DOS_EXE_PE);
    eq("PE, GUI subsystem -> subsys 2",  sub, 2);
    mk(0x80, "PE\0\0", 0x5C, 3);
    dos_exe_kind(f, sizeof f, &sub);
    eq("PE, console subsystem -> subsys 3", sub, 3);
    mk(0x80, "NE", 0x36, 2);
    eq("NE, target OS 2 (Windows) -> WOW", dos_exe_kind(f, sizeof f, &sub), DOS_EXE_NE);
    mk(0x80, "NE", 0x36, 0);
    eq("NE, target OS 0 (Windows 1/2) -> WOW", dos_exe_kind(f, sizeof f, &sub), DOS_EXE_NE);
    mk(0x80, "NE", 0x36, 1);
    eq("NE, target OS 1 (OS/2, BIND'ed) -> DOS: the stub IS the program",
       dos_exe_kind(f, sizeof f, &sub), DOS_EXE_DOS);
    mk(0x80, "LE", 0, 0);
    eq("LE (DOS/4GW-bound game) -> DOS", dos_exe_kind(f, sizeof f, &sub), DOS_EXE_DOS);
    mk(0x80, "LX", 0, 0);
    eq("LX -> DOS", dos_exe_kind(f, sizeof f, &sub), DOS_EXE_DOS);
    mk(0x20, "PE\0\0", 0, 0);
    eq("e_lfanew below 40h -> DOS (old .EXE, junk at 3Ch)",
       dos_exe_kind(f, sizeof f, &sub), DOS_EXE_DOS);
    mk(0x80, "PE\0\0", 0, 0);
    eq("e_lfanew past what was read -> DOS", dos_exe_kind(f, 0x81, &sub), DOS_EXE_DOS);
    mk(0x80, "PE\0\0", 0, 0); f[0x83] = 'X';
    eq("\"PE\" without its two NULs -> DOS", dos_exe_kind(f, sizeof f, &sub), DOS_EXE_DOS);
    mk(0x80, "PE\0\0", 0, 0); f[0] = 'Z'; f[1] = 'M';
    eq("not MZ -> DOS (a .COM)", dos_exe_kind(f, sizeof f, &sub), DOS_EXE_DOS);

    /* ── 2. how much memory (largest free block 8000h paragraphs) ── */
    n = child(0x100, 0x200);
    eq("image paras of p_exmemc's child: one page less the header", dos_image_paras(f, n), 0x1E);
    rc = dos_exec_size(f, n, 0x8000, &alloc, &high);
    eq("exmem.a.exec min 100h max 200h -> ok", rc, 0);
    eq("exmem.a.child BX=022E: 10h + 1Eh + 200h", alloc, 0x22E);
    eq("exmem.a not loaded high", high, 0);

    n = child(0xF000, 0xFFFF);
    rc = dos_exec_size(f, n, 0x8000, &alloc, &high);
    eq("exmem.b.exec min F000h > largest -> 8", rc, 8);

    n = child(0, 0);
    rc = dos_exec_size(f, n, 0x8000, &alloc, &high);
    eq("exmem.c.exec min = max = 0 -> ok", rc, 0);
    eq("exmem.c load HIGH", high, 1);
    eq("exmem.c takes the whole block", alloc, 0x8000);

    n = child(0, 0xFFFF);
    rc = dos_exec_size(f, n, 0x8000, &alloc, &high);
    eq("exmem.d max FFFFh -> the whole block", alloc, 0x8000);
    eq("exmem.d not high", high, 0);

    n = child(0, 0x10);
    rc = dos_exec_size(f, n, 0x8000, &alloc, &high);
    eq("exmem.e.child BX=003E: 10h + 1Eh + 10h", alloc, 0x3E);

    n = child(0x40, 0x10);
    dos_exec_size(f, n, 0x8000, &alloc, &high);
    eq("max below min -> min wins (10h + 1Eh + 40h)", alloc, 0x6E);

    n = child(0, 0x200);
    rc = dos_exec_size(f, n, 0x100, &alloc, &high);
    eq("max beyond the block -> capped at the block", alloc, 0x100);
    eq("...and that is not an error", rc, 0);

    n = child(0x100, 0x200);
    rc = dos_exec_size(f, n, 0x12E, &alloc, &high);
    eq("min exactly fits (10h + 1Eh + 100h = 12Eh) -> ok", rc, 0);
    rc = dos_exec_size(f, n, 0x12D, &alloc, &high);
    eq("one paragraph short -> 8", rc, 8);

    f[0] = 0xB4;                                   /* a .COM */
    rc = dos_exec_size(f, 0x100, 0x8000, &alloc, &high);
    eq(".COM takes the largest block", alloc, 0x8000);

    printf("== %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
