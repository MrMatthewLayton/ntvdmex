#ifndef NTVDMEX_WOWCONV_H
#define NTVDMEX_WOWCONV_H
#include "../ntvdmex_types.h"
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
 *   Windows types and no host state, and `tests/unit/wow_test.c` pins every
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
     Win16 caller compares it with 2 -- Solitaire for equality, Minesweeper as a
     SIGNED number, so -1 reads as "fewer than 2". Win32's -1 for direct-colour
     devices is a sentinel that predates neither program.
   ★ <= 8bpp gives the exact count. Deeper has no colour table at all, so it
     gives 256: the largest a Windows 3.1 driver ever reported, and the largest a
     program of this era was built to read. */
static INT WowConvNumColors(INT bitsPerPixel)
{
    if (bitsPerPixel <= 0)  return 2;          /* nonsense in, the safe floor out */
    if (bitsPerPixel <= 8)  return 1 << bitsPerPixel;
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
static INT WowConvBackgroundBrushKind(UINT value)
{
    if (!value) return WOWCONV_HBR_NONE;
    if (value <= (UINT)WOWCONV_COLOR_MAX + 1) return WOWCONV_HBR_SYSCOLOR;
    return WOWCONV_HBR_HANDLE;
}

/* ── A Win16 RECT IS 8 BYTES ─────────────────────────────────────────────────
     Four 16-bit SIGNED ints, in the order left, top, right, bottom. Kept here
     with the sign extension explicit because a rectangle read unsigned lays a
     window out at 65488 instead of -48. */
#define WOWCONV_RECT16_SIZE 8
static INT WowConvRect16Get(PCBYTE rect, INT index)
{
    INT value = (INT)((UINT)rect[index * 2] | ((UINT)rect[index * 2 + 1] << 8));
    return (value & 0x8000) ? value - 0x10000 : value;
}
static VOID WowConvRect16Put(PBYTE rect, INT index, INT value)
{
    rect[index * 2]     = (BYTE)(value & 0xff);
    rect[index * 2 + 1] = (BYTE)((value >> 8) & 0xff);
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

     Writes a 40-byte BITMAPINFOHEADER plus the widened palette into `output`.
     Returns the offset OF THE PIXELS within the source, or 0 if it does not add
     up (which is a refusal, not a guess). */
static UINT WowConvDibHeaderSize(PCBYTE header)
{
    return (UINT)header[0] | ((UINT)header[1] << 8)
         | ((UINT)header[2] << 16) | ((UINT)header[3] << 24);
}

static UINT WowConvDibCoreToInfo(PCBYTE core, UINT length,
                                         PBYTE output, UINT capacity,
                                         PUINT paletteCount)
{
    UINT width, height, bitCount, paletteEntries, pixelOffset, index;
    if (!core || !output || length < 12) return 0;
    if (WowConvDibHeaderSize(core) != 12) return 0;
    width  = (UINT)core[4]  | ((UINT)core[5]  << 8);
    height  = (UINT)core[6]  | ((UINT)core[7]  << 8);
    bitCount = (UINT)core[10] | ((UINT)core[11] << 8);
    if (bitCount != 1 && bitCount != 4 && bitCount != 8 && bitCount != 24) return 0;
    paletteEntries    = (bitCount <= 8) ? (1u << bitCount) : 0u;
    pixelOffset = 12 + paletteEntries * 3;
    if (pixelOffset >= length) return 0;               /* no room for any pixels */
    if (capacity < 40 + paletteEntries * 4) return 0;
    for (index = 0; index < 40; ++index) output[index] = 0;
    output[0] = 40;                                            /* biSize     */
    output[4] = (BYTE)(width & 0xff);
    output[5] = (BYTE)((width >> 8) & 0xff);            /* biWidth    */
    output[8] = (BYTE)(height & 0xff);
    output[9] = (BYTE)((height >> 8) & 0xff);            /* biHeight   */
    output[12] = 1;                                            /* biPlanes   */
    output[14] = (BYTE)(bitCount & 0xff);
    output[15] = (BYTE)((bitCount >> 8) & 0xff);          /* biBitCount */
    /* RGBTRIPLE -> RGBQUAD. Both are B,G,R order, so only the fourth
       (reserved) byte is new -- but the STRIDE is the whole point. */
    for (index = 0; index < paletteEntries; ++index) {
        output[40 + index * 4 + 0] = core[12 + index * 3 + 0];
        output[40 + index * 4 + 1] = core[12 + index * 3 + 1];
        output[40 + index * 4 + 2] = core[12 + index * 3 + 2];
        output[40 + index * 4 + 3] = 0;
    }
    if (paletteCount) *paletteCount = paletteEntries;
    return pixelOffset;
}

/* ── ★★ WHICH PROCEDURE DRIVES A WINDOW. (session 57) ────────────────────────
     A Win16 window is driven by its CLASS's window procedure -- except a dialog
     built from a template that names no class, which is a `#32770` window: the
     class is the system's, there is no 16-bit window procedure, and the thing
     that drives it is the DLGPROC the guest passed to DialogBox. Both can be
     present (a dialog made from the application's own class, like CALC's
     `SciCalc`), and then the class procedure is the one Windows calls.
   ⚠ THE ORDER IS THE WHOLE FUNCTION, and it is here rather than at the three
     call sites -- SendMessage, DispatchMessage and the modal loop -- because
     three sites deciding this for themselves is how one of them comes to
     disagree. 0 means nothing can be told about the window at all, which is a
     fact its callers must handle rather than paper over. */
static UINT WowConvWindowProcedure(UINT windowProcedure, UINT dialogProcedure)
{
    return windowProcedure ? windowProcedure : dialogProcedure;
}

/* ── ★★★★★ WHEN A MODAL LOOP MUST STOP. (session 57) ─────────────────────────
     `DialogBox` is defined as not returning until `EndDialog`, so the host runs
     a message loop on the guest's behalf -- and the one way to make that worse
     than doing nothing is for the loop to have a state in which it neither runs
     nor exits. Session 56 said so when it declined to write the loop: "a
     half-built modal loop that never returns is worse than an honest immediate
     return, because it hangs the guest instead of ending it."
   ⇒ So the decision is a pure function of four facts, it is total (every
     combination returns something), and the battery pins it. The loop itself
     cannot hang unless this returns RUN forever, and this returns RUN only when
     the dialog is alive, drivable and not yet finished.
   ⚠ THE ORDER IS LOAD-BEARING, in one place especially: `isEnded` OUTRANKS a
     destroyed window. A dialog procedure that calls EndDialog and whose window
     the OS then tears down has ANSWERED, and the caller is entitled to that
     answer -- checking liveness first would throw away the result of the dialog
     the user just clicked OK on and return 0 instead. */
#define WOWCONV_MODAL_RUN     0   /* keep pumping                              */
#define WOWCONV_MODAL_END     1   /* EndDialog: the caller gets nResult         */
#define WOWCONV_MODAL_GONE    2   /* the window is destroyed: 0                */
#define WOWCONV_MODAL_NOPROC  3   /* nothing to dispatch to: 0, immediately    */
#define WOWCONV_MODAL_EXPIRED 4   /* the bounded input wait ran out: 0         */
static INT WowConvModalExit(INT isEnded, INT isWindowAlive, INT hasProcedure,
                              INT isWaitExpired)
{
    if (isEnded)         return WOWCONV_MODAL_END;
    if (!isWindowAlive) return WOWCONV_MODAL_GONE;
    if (!hasProcedure)     return WOWCONV_MODAL_NOPROC;
    if (isWaitExpired)  return WOWCONV_MODAL_EXPIRED;
    return WOWCONV_MODAL_RUN;
}

/* ── ⚠⚠ THE `ABC` STRUCTURE IS SIX BYTES IN Win16 AND TWELVE IN Win32 ─────────
     The same shape of trap as RECT above, and worse in one way: RECT is wrong by
     a factor of two on ONE structure, this is wrong by a factor of two on an
     ARRAY -- one entry per character in the range, so measuring a 224-glyph font
     into a guest's buffer would write 2,688 bytes where 1,344 were reserved.
       Win16  short abcA; unsigned short abcB; short abcC;   =  6
       Win32  LONG  abcA; UINT           abcB; LONG  abcC;   = 12
     The values themselves are the same numbers: A and C are signed and may be
     negative (an italic glyph overhangs), B is a width and cannot be. So this is
     purely a narrowing, and the only judgement in it is what to do when a value
     does not fit -- which is to CLAMP, because a 16-bit field that wraps turns a
     small overhang into a huge one and lays the text out catastrophically rather
     than slightly wrongly. */
#define WOWCONV_ABC16_SIZE 6
#define WOWCONV_ABC32_SIZE 12
static INT WowConvClamp16(long value)
{
    if (value >  32767L) return  32767;
    if (value < -32768L) return -32768;
    return (INT)value;
}
static VOID WowConvAbc32To16(const long *abc32, PBYTE abc16)
{
    INT widthA = WowConvClamp16(abc32[0]);
    long widthB = abc32[1] < 0 ? 0 : abc32[1];      /* a width is never negative */
    INT widthC = WowConvClamp16(abc32[2]);
    if (widthB > 65535L) widthB = 65535L;
    abc16[0] = (BYTE)(widthA & 0xff);
    abc16[1] = (BYTE)((widthA >> 8) & 0xff);
    abc16[2] = (BYTE)(widthB & 0xff);
    abc16[3] = (BYTE)((widthB >> 8) & 0xff);
    abc16[4] = (BYTE)(widthC & 0xff);
    abc16[5] = (BYTE)((widthC >> 8) & 0xff);
}

/* ── WINDOWS METAFILE BYTES: THE HEADER AND ONE RECORD. (#295) ───────────────
     EnumMetaFile hands the guest one METARECORD at a time, and a Win32 WMF is the
     SAME BYTES a Win16 metafile is -- the format was frozen in 3.0 and Win32 only
     wraps it -- so no field is converted here. What is checked is the walk, which
     is the part that can go wrong silently:
       METAHEADER (18 bytes)              METARECORD
         +0  WORD  mtType  (1 mem, 2 disk)  +0 DWORD rdSize  -- in WORDS, header incl.
         +2  WORD  mtHeaderSize (= 9 words) +4 WORD  rdFunction (0 = META_EOF)
         +4  WORD  mtVersion                +6 WORD  rdParm[rdSize - 3]
         +6  DWORD mtSize  (WORDS, header incl.)
         +10 WORD  mtNoObjects  -- the HANDLETABLE's length, the callback's nObj
         +12 DWORD mtMaxRecord
         +16 WORD  mtNoParameters
   ⚠ SIZES ARE IN WORDS. A walk that treats rdSize as bytes steps into the middle
     of the second record and reads parameters as a function number -- a wrong
     sequence that still looks like a sequence.
   ⚠ A RECORD SHORTER THAN ITS OWN HEADER (rdSize < 3) WOULD NEVER ADVANCE, and a
     record that claims more than the buffer holds would read past it; both are
     REFUSED (0), never clamped. The version word is not checked: 0x0100 and 0x0300
     both exist and nothing here depends on it.
   The bound is the smaller of the buffer and mtSize, so trailing bytes after the
   metafile proper are not walked as records. */
#define WOWCONV_MF_HDR    18
#define WOWCONV_MF_RECHDR 6
static unsigned long WowConvRead32(PCBYTE bytes)
{
    return (unsigned long)bytes[0] | ((unsigned long)bytes[1] << 8)
         | ((unsigned long)bytes[2] << 16) | ((unsigned long)bytes[3] << 24);
}
/* Returns the byte offset of the first record (18), or 0 if this is not a WMF.
   *objectCount = mtNoObjects; *recordsEnd = the byte length the records may occupy. */
static unsigned long WowConvMetafileHeader(PCBYTE bytes, unsigned long length,
                                       PUINT objectCount, unsigned long *recordsEnd)
{
    UINT type, headerWords;
    unsigned long metafileBytes;
    if (!bytes || length < WOWCONV_MF_HDR) return 0;
    type = (UINT)(bytes[0] | (bytes[1] << 8));
    headerWords  = (UINT)(bytes[2] | (bytes[3] << 8));
    if ((type != 1 && type != 2) || headerWords != 9) return 0;
    metafileBytes = WowConvRead32(bytes + 6);
    if (metafileBytes > 0x7fffffffUL / 2) return 0;
    metafileBytes *= 2;
    if (metafileBytes < WOWCONV_MF_HDR) return 0;           /* says it has no room for itself */
    if (objectCount) *objectCount = (UINT)(bytes[10] | (bytes[11] << 8));
    if (recordsEnd)  *recordsEnd  = (metafileBytes < length) ? metafileBytes : length;
    return WOWCONV_MF_HDR;
}
/* The record at `offset`: 1 and its byte length + function, or 0 if there is no
   whole record there (offset at or past `recordsEnd`, rdSize < 3, or running past `recordsEnd`). */
static INT WowConvMetafileRecord(PCBYTE metafile, unsigned long recordsEnd,
                             unsigned long offset, unsigned long *recordBytes, PUINT function)
{
    unsigned long recordWords;
    if (!metafile || offset >= recordsEnd || recordsEnd - offset < WOWCONV_MF_RECHDR) return 0;
    recordWords = WowConvRead32(metafile + offset);
    if (recordWords < 3 || recordWords > (recordsEnd - offset) / 2) return 0;
    if (recordBytes) *recordBytes = recordWords * 2;
    if (function)  *function  = (UINT)(metafile[offset + 4] | (metafile[offset + 5] << 8));
    return 1;
}

#endif /* NTVDMEX_WOWCONV_H */
