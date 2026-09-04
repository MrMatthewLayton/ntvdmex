#ifndef WOWCONV_H
#define WOWCONV_H
/*
 * wowconv.h -- ★★★★★ THE Win16/Win32 SEMANTIC DELTAS, IN ONE PLACE, TESTABLE.
 * GH #128, session 51.
 *
 * ── WHY THIS FILE EXISTS ────────────────────────────────────────────────────
 * This host does not reimplement the Win16 API; it TRANSLATES it. XP's own
 * krnl386/USER/GDI run natively on the real CPU, XP's Win32 API does the work,
 * and what we write is the adapter between them -- the piece Microsoft ships as
 * `wow32.dll` and never documented.
 *
 * Almost every defect in that adapter has had the same shape, and it is NOT
 * "the call is missing". It is that Win32 inherited Win16's names, constants and
 * structures, so passing a call straight through is right MOST of the time --
 * which trains you to trust it -- and then silently wrong in a narrow place:
 *
 *   GetDeviceCaps(NUMCOLORS)  Win32 answers -1 for any device deeper than 8bpp.
 *                             No program written in 1992 has ever seen -1, and
 *                             WINMINE.EXE's `cmp ax,2 / jle` reads it as
 *                             MONOCHROME. (session 51, and the game was B/W)
 *   RECT                      Win16's is four `int`s = 8 BYTES. Win32's is four
 *                             LONGs = 16. Handing one to the other reads this
 *                             rectangle plus eight bytes of whatever follows.
 *   packed DIB                Windows 3.0's BITMAPCOREHEADER is 12 bytes with an
 *                             RGBTRIPLE table; the modern one is 40 with RGBQUAD.
 *                             Every card face in SOL.EXE is the old form.
 *   WNDCLASS.hbrBackground    EITHER a real brush handle OR a COLOR_* index
 *                             BIASED BY ONE. Guessing wrong paints the window
 *                             the wrong colour and nothing reports an error.
 *
 * None of those FAIL. Win16 signals errors in-band -- 0, or -1 -- and the CALL
 * SITE decides what the value means, so a wrong answer produces a program that
 * runs and looks plausible while doing the wrong thing. There is no exception to
 * catch and nothing in a log to grep for.
 *
 * ⇒ So they are collected HERE, as pure functions of their inputs, with no
 *   Windows types and no host state, and `tools/dostest/wow_test.c` pins every
 *   one of them off-VM. Before this file the WOW translation layer had 0 of the
 *   project's 842 checks, and every defect in it was found by the user's eye.
 *
 * ⚠ NOTHING IN HERE MAY TOUCH THE HOST OR THE GUEST. The moment a function here
 *   needs a handle table or guest memory it stops being testable without a rig,
 *   which is the entire point of the file. Fetch the bytes in the caller, pass
 *   them in.
 */

/* ── NUMCOLORS ───────────────────────────────────────────────────────────────
     Win16's meaning is "how many entries in this device's colour table", and a
     Win16 caller tests it against 2 -- for equality (Solitaire, seg1:0x016a) or
     with a SIGNED `jle` (Minesweeper, seg1:0x182a). Win32's -1 for direct-colour
     devices is a sentinel that predates neither program.
   ★ <= 8bpp gives the exact count. Deeper has no colour table at all, so it
     gives 256: the largest a Windows 3.1 driver ever reported, and the largest a
     program of this era was built to read. */
static int wowconv_numcolors(int bpp)
{
    if (bpp <= 0)  return 2;          /* nonsense in, the safe floor out */
    if (bpp <= 8)  return 1 << bpp;
    return 256;
}

/* ── WNDCLASS.hbrBackground ──────────────────────────────────────────────────
     Three cases, and the third is why this is not a boolean:
       0                     NO background erase. Must STAY 0 -- a class that
                             says it paints its own background must not be
                             painted over.
       1 .. COLORMAX+1       a COLOR_* system index, biased by one so that 0 can
                             mean "none". Win 3.1's last was COLOR_BTNHIGHLIGHT
                             = 20.
       anything else         a real brush handle the program made.
   ⚠ The bias is the trap: COLOR_WINDOW is 5 and a class naming it stores 6. */
#define WOWCONV_COLOR_MAX 20
#define WOWCONV_HBR_NONE     0
#define WOWCONV_HBR_SYSCOLOR 1
#define WOWCONV_HBR_HANDLE   2
static int wowconv_hbrback_kind(unsigned v)
{
    if (!v) return WOWCONV_HBR_NONE;
    if (v <= (unsigned)WOWCONV_COLOR_MAX + 1) return WOWCONV_HBR_SYSCOLOR;
    return WOWCONV_HBR_HANDLE;
}

/* ── A Win16 RECT IS 8 BYTES ─────────────────────────────────────────────────
     Four 16-bit SIGNED ints, in the order left, top, right, bottom. Kept here
     with the sign extension explicit because a rectangle read unsigned lays a
     window out at 65488 instead of -48. */
#define WOWCONV_RECT16_SIZE 8
static int wowconv_rect16_get(const unsigned char *p, int i)
{
    int v = (int)((unsigned)p[i * 2] | ((unsigned)p[i * 2 + 1] << 8));
    return (v & 0x8000) ? v - 0x10000 : v;
}
static void wowconv_rect16_put(unsigned char *p, int i, int v)
{
    p[i * 2]     = (unsigned char)(v & 0xff);
    p[i * 2 + 1] = (unsigned char)((v >> 8) & 0xff);
}

/* ── PACKED DIB: THE 12-BYTE CORE HEADER ─────────────────────────────────────
     BITMAPCOREHEADER                    BITMAPINFOHEADER
       +0  DWORD bcSize   = 12             +0  DWORD biSize = 40
       +4  WORD  bcWidth   (UNSIGNED)      +4  LONG  biWidth
       +6  WORD  bcHeight  (UNSIGNED)      +8  LONG  biHeight
       +8  WORD  bcPlanes                  +12 WORD  biPlanes
       +10 WORD  bcBitCount                +14 WORD  biBitCount
       then RGBTRIPLE[] -- 3 bytes each    then RGBQUAD[] -- 4 bytes each

     The two differ in more than length, which is why this converts field by
     field rather than casting: a cast would read the width as a 32-bit value
     spanning bcWidth AND bcHeight, and walk the palette at the wrong stride.
   ⚠ There is no biClrUsed in the core header -- the table is always the full
     2^bcBitCount entries at <= 8bpp, and absent above it.

     Writes a 40-byte BITMAPINFOHEADER plus the widened palette into `out`.
     Returns the offset OF THE PIXELS within the source, or 0 if it does not add
     up (which is a refusal, not a guess). */
static unsigned wowconv_dib_size(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8)
         | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

static unsigned wowconv_dib_core_to_info(const unsigned char *core, unsigned len,
                                         unsigned char *out, unsigned cap,
                                         unsigned *pal_out)
{
    unsigned wid, hgt, bits, pal, srcoff, i;
    if (!core || !out || len < 12) return 0;
    if (wowconv_dib_size(core) != 12) return 0;
    wid  = (unsigned)core[4]  | ((unsigned)core[5]  << 8);
    hgt  = (unsigned)core[6]  | ((unsigned)core[7]  << 8);
    bits = (unsigned)core[10] | ((unsigned)core[11] << 8);
    if (bits != 1 && bits != 4 && bits != 8 && bits != 24) return 0;
    pal    = (bits <= 8) ? (1u << bits) : 0u;
    srcoff = 12 + pal * 3;
    if (srcoff >= len) return 0;               /* no room for any pixels */
    if (cap < 40 + pal * 4) return 0;
    for (i = 0; i < 40; ++i) out[i] = 0;
    out[0] = 40;                                            /* biSize     */
    out[4] = (unsigned char)(wid & 0xff);
    out[5] = (unsigned char)((wid >> 8) & 0xff);            /* biWidth    */
    out[8] = (unsigned char)(hgt & 0xff);
    out[9] = (unsigned char)((hgt >> 8) & 0xff);            /* biHeight   */
    out[12] = 1;                                            /* biPlanes   */
    out[14] = (unsigned char)(bits & 0xff);
    out[15] = (unsigned char)((bits >> 8) & 0xff);          /* biBitCount */
    /* RGBTRIPLE -> RGBQUAD. Both are B,G,R order, so only the fourth
       (reserved) byte is new -- but the STRIDE is the whole point. */
    for (i = 0; i < pal; ++i) {
        out[40 + i * 4 + 0] = core[12 + i * 3 + 0];
        out[40 + i * 4 + 1] = core[12 + i * 3 + 1];
        out[40 + i * 4 + 2] = core[12 + i * 3 + 2];
        out[40 + i * 4 + 3] = 0;
    }
    if (pal_out) *pal_out = pal;
    return srcoff;
}

#endif /* WOWCONV_H */
