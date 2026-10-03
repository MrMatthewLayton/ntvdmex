#ifndef WOWCOMMDLG_H
#define WOWCOMMDLG_H
/*
 * wowcommdlg.h -- ★★★ COMMDLG.DLL's OWN ID SPACE -- File > Open.  GH #128, s44.
 *
 * ── WHY THIS IS SMALL, AND WHY THAT WAS A SURPRISE ──────────────────────────
 * The plan was to implement `DialogBox`: turn a Win16 DIALOG template into a real
 * window and run the guest's dialog procedure. That turned out to be the wrong
 * plan, and the binaries said so before a line of it was written:
 *
 *   * `USER.87 DIALOGBOX` resolves to entry-table segment 1, offset 0x208e, and
 *     the bytes there are `55 8b ec 68 b1 20 8b 46 10 ...` -- ordinary 16-bit
 *     code, not a `6a XX 68 00 00 68` WOW32 stub. Same for `CREATEDIALOG` (0x1ff0),
 *     `ENDDIALOG` (0x2120) and `DIALOGBOXPARAM` (0x20fd). ⇒ **USER owns the dialog
 *     engine and its modal loop.** We are not asked for one.
 *   * And Notepad does not reach it anyway. `tools/ne/neimports.py` names its call
 *     site outright: `notepad seg1:0x0192  COMMDLG.1 GETOPENFILENAME`.
 *
 * ⇒ File > Open is ONE call, and the run confirms it: driving Alt-F-O on the live
 *   guest produced exactly two unimplemented BOPs, both from a table this host had
 *   never seen -- `id 0x01, 4 args, retstub 0x0012` and `id 0x1a, 0 args, retstub
 *   0x0090`. COMMDLG's own stub table has `id 0x01` at `seg1:0x0005` and `id 0x1a`
 *   at `seg1:0x0083`, and a stub is 13 bytes: 0x0005+13 = 0x0012, 0x0083+13 =
 *   0x0090. Both match to the byte.
 *
 * ── ★★ THE IDS ARE THE EXPORT ORDINALS, CONFIRMED SEVEN TIMES ───────────────
 * COMMDLG's non-resident name table against its stub ids:
 *      1 GETOPENFILENAME -> 0x01     15 CHOOSEFONT   -> 0x0f
 *      2 GETSAVEFILENAME -> 0x02     20 PRINTDLG     -> 0x14
 *      5 CHOOSECOLOR     -> 0x05     26 COMMDLGEXTENDEDERROR -> 0x1a
 *     11 FINDTEXT        -> 0x0b     12 REPLACETEXT  -> 0x0c
 * Seven independent agreements is a reading, not a coincidence -- and it is the
 * same shape SHELL.DLL turned out to have. ⚠ It is NOT a rule: krnl386's ids are
 * nothing like its ordinals. Each module is checked on its own.
 *
 * ── ★★★ THE Win16 OPENFILENAME, 0x48 BYTES, READ OUT OF NOTEPAD ────────────
 * Not from a header -- the guest declares its own size and fills its own fields,
 * and every store lands on a field boundary of the layout below:
 *
 *   notepad seg2:0x055d  mov word [0x0b16], 0x0048   ★ lStructSize, from the guest
 *   notepad seg1:0x015a  mov word [0x0b1e], 0x0ad4 / mov [0x0b20], ds   -> +0x08
 *   notepad seg1:0x0164  mov word [0x0b22], 0x0872 / mov [0x0b24], ds   -> +0x0c
 *   notepad seg1:0x0146  mov word [0x0b3e], 0x09f4 / mov [0x0b40], ds   -> +0x28
 *   notepad seg1:0x0150  mov ax,[0x74] / [0x0b42] / mov [0x0b44], ds    -> +0x2c
 *   notepad seg1:0x017b  mov word [0x0b46], 0x1004 / [0x0b48], 0        -> +0x30
 *   notepad seg1:0x016e  mov ax,[0x68]+3 / [0x0b4e] / [0x0b50], ds      -> +0x38
 *
 * The structure base is `ds:0x0b16` -- the very pointer pushed at `seg1:0x018f`.
 * Four far pointers at +0x08/+0x0c/+0x28/+0x2c/+0x38 and a DWORD 0x00001004 at
 * +0x30 (OFN_FILEMUSTEXIST | OFN_HIDEREADONLY, exactly what File > Open wants).
 * A wrong layout does not put five far pointers on five pointer fields and a
 * plausible flag word on the flag field.
 *
 * ⚠⚠ AND IT IS NOT THE Win32 LAYOUT. `hwndOwner` and `hInstance` are **2 bytes
 *   each** here and 4 each in Win32, so every field after +0x08 is at a different
 *   offset in the two structures. They are converted field by field below; there
 *   is no memcpy that could ever be right.
 */

#define WOWCDLG_GETOPENFILENAME  0x0001
#define WOWCDLG_GETSAVEFILENAME  0x0002
#define WOWCDLG_EXTENDEDERROR    0x001a

/* One far pointer, 4 argument bytes -- what the stub declares. */
#define OFN_ARG_LPOFN            0

#define WOW_OFN16_SIZE      0x48
#define OFN16_STRUCTSIZE    0x00
#define OFN16_HWNDOWNER     0x04
#define OFN16_HINSTANCE     0x06
#define OFN16_FILTER        0x08
#define OFN16_CUSTFILTER    0x0c
#define OFN16_MAXCUSTFILTER 0x10
#define OFN16_FILTERINDEX   0x14
#define OFN16_FILE          0x18
#define OFN16_MAXFILE       0x1c
#define OFN16_FILETITLE     0x20
#define OFN16_MAXFILETITLE  0x24
#define OFN16_INITIALDIR    0x28
#define OFN16_TITLE         0x2c
#define OFN16_FLAGS         0x30
#define OFN16_FILEOFFSET    0x34
#define OFN16_FILEEXTENSION 0x36
#define OFN16_DEFEXT        0x38
#define OFN16_CUSTDATA      0x3c
#define OFN16_HOOK          0x40
#define OFN16_TEMPLATENAME  0x44

/* ⚠ THE HOOK AND TEMPLATE BITS ARE REFUSED, NOT HONOURED. Either one asks the
     32-bit side to call back into 16-bit code, or to build a dialog from the
     application's own template, and neither is built. Passing them to Win32
     unchanged would hand comdlg32 a 16:16 function pointer it would call as a
     flat one. Notepad sets neither (its Flags are 0x1004), so this strips
     something nothing has asked for -- and says so on the line if it ever does. */
#define OFN16_HOOKBITS  (0x00000020UL | 0x00000040UL | 0x00002000UL)
/* ENABLEHOOK | ENABLETEMPLATE | ENABLETEMPLATEHANDLE */

static DWORD wcd_peekd(const volatile BYTE *p, int off)
{
    return (DWORD)wow32_peekw((volatile BYTE *)p + off)
         | ((DWORD)wow32_peekw((volatile BYTE *)p + off + 2) << 16);
}

static void wcd_poked(volatile BYTE *p, int off, DWORD v)
{
    wow32_pokew(p + off,     (WORD)(v & 0xFFFF));
    wow32_pokew(p + off + 2, (WORD)(v >> 16));
}

/* ── #294: the rest of COMMDLG's table. Ids = export ordinals (see the top). */
#define WOWCDLG_CHOOSECOLOR   0x0005
#define WOWCDLG_FINDTEXT      0x000b
#define WOWCDLG_REPLACETEXT   0x000c
#define WOWCDLG_CHOOSEFONT    0x000f

/* Win16 FINDREPLACE, 0x24 bytes (3.1 SDK; Wine's FINDREPLACE16 agrees). */
#define WOW_FR16_SIZE        0x24
#define FR16_STRUCTSIZE      0x00
#define FR16_HWNDOWNER       0x04
#define FR16_FLAGS           0x08
#define FR16_FINDWHAT        0x0c
#define FR16_REPLACEWITH     0x10
#define FR16_FINDWHATLEN     0x14
#define FR16_REPLACEWITHLEN  0x16
#define FR16_CUSTDATA        0x18
#define FR16_HOOKBITS  (0x00000100UL | 0x00000200UL | 0x00002000UL)
/* FR_ENABLEHOOK | FR_ENABLETEMPLATE | FR_ENABLETEMPLATEHANDLE */

/* Win16 CHOOSECOLOR, 0x20 bytes. */
#define WOW_CC16_SIZE        0x20
#define CC16_STRUCTSIZE      0x00
#define CC16_HWNDOWNER       0x04
#define CC16_RGBRESULT       0x08
#define CC16_CUSTCOLORS      0x0c
#define CC16_FLAGS           0x10
#define CC16_CUSTDATA        0x14
#define CC16_HOOKBITS  (0x00000010UL | 0x00000020UL | 0x00000040UL)
/* CC_ENABLEHOOK | CC_ENABLETEMPLATE | CC_ENABLETEMPLATEHANDLE */

/* Win16 CHOOSEFONT, 0x2e bytes. */
#define WOW_CF16_SIZE        0x2e
#define CF16_STRUCTSIZE      0x00
#define CF16_HWNDOWNER       0x04
#define CF16_HDC             0x06
#define CF16_LOGFONT         0x08
#define CF16_POINTSIZE       0x0c
#define CF16_FLAGS           0x0e
#define CF16_RGBCOLORS       0x12
#define CF16_CUSTDATA        0x16
#define CF16_HOOK            0x1a
#define CF16_TEMPLATENAME    0x1e
#define CF16_HINSTANCE       0x22
#define CF16_STYLE           0x24
#define CF16_FONTTYPE        0x28
#define CF16_SIZEMIN         0x2a
#define CF16_SIZEMAX         0x2c
#define CF16_HOOKBITS  (0x00000008UL | 0x00000010UL | 0x00000020UL)
/* CF_ENABLEHOOK | CF_ENABLETEMPLATE | CF_ENABLETEMPLATEHANDLE */

/* Win16 PRINTDLG, 0x34 bytes (3.1 SDK; Wine's PRINTDLG16 agrees). */
#define WOWCDLG_PRINTDLG     0x0014
#define WOW_PD16_SIZE        0x34
#define PD16_STRUCTSIZE      0x00
#define PD16_HWNDOWNER       0x04
#define PD16_HDEVMODE        0x06
#define PD16_HDEVNAMES       0x08
#define PD16_HDC             0x0a
#define PD16_FLAGS           0x0c
#define PD16_FROMPAGE        0x10
#define PD16_TOPAGE          0x12
#define PD16_MINPAGE         0x14
#define PD16_MAXPAGE         0x16
#define PD16_COPIES          0x18
#define PD16_CUSTDATA        0x1c
#define PD16_HOOKBITS  (0x00001000UL | 0x00002000UL | 0x00004000UL | 0x00008000UL \
                        | 0x00010000UL | 0x00020000UL)
/* PD_ENABLEPRINTHOOK | PD_ENABLESETUPHOOK | PD_ENABLE{PRINT,SETUP}TEMPLATE[HANDLE] */

/* An answer CommDlgExtendedError owes for a call THIS HOST refused before comdlg32
   saw it (a wrong lStructSize): comdlg32's own per-thread value would say 0, and
   stock says CDERR_STRUCTSIZE (w_cdlg). Cleared by every call that reaches comdlg32. */
static DWORD g_wcd_err = 0;

/* Win16 LOGFONT: five INT16s, eight BYTEs, a 32-byte face -- 50 bytes. */
static void wcd_lf16_to32(const volatile BYTE *p, LOGFONTA *lf)
{
    int i;
    lf->lfHeight      = (LONG)(short)wow32_peekw((volatile BYTE *)p + 0);
    lf->lfWidth       = (LONG)(short)wow32_peekw((volatile BYTE *)p + 2);
    lf->lfEscapement  = (LONG)(short)wow32_peekw((volatile BYTE *)p + 4);
    lf->lfOrientation = (LONG)(short)wow32_peekw((volatile BYTE *)p + 6);
    lf->lfWeight      = (LONG)(short)wow32_peekw((volatile BYTE *)p + 8);
    lf->lfItalic = p[10]; lf->lfUnderline = p[11]; lf->lfStrikeOut = p[12];
    lf->lfCharSet = p[13]; lf->lfOutPrecision = p[14]; lf->lfClipPrecision = p[15];
    lf->lfQuality = p[16]; lf->lfPitchAndFamily = p[17];
    for (i = 0; i < LF_FACESIZE - 1 && p[18 + i]; ++i) lf->lfFaceName[i] = (char)p[18 + i];
    lf->lfFaceName[i] = 0;
}

static void wcd_lf32_to16(const LOGFONTA *lf, volatile BYTE *p)
{
    int i;
    wow32_pokew(p + 0, (WORD)(short)lf->lfHeight);
    wow32_pokew(p + 2, (WORD)(short)lf->lfWidth);
    wow32_pokew(p + 4, (WORD)(short)lf->lfEscapement);
    wow32_pokew(p + 6, (WORD)(short)lf->lfOrientation);
    wow32_pokew(p + 8, (WORD)(short)lf->lfWeight);
    p[10] = lf->lfItalic; p[11] = lf->lfUnderline; p[12] = lf->lfStrikeOut;
    p[13] = lf->lfCharSet; p[14] = lf->lfOutPrecision; p[15] = lf->lfClipPrecision;
    p[16] = lf->lfQuality; p[17] = lf->lfPitchAndFamily;
    for (i = 0; i < 32; ++i) p[18 + i] = (i < LF_FACESIZE && lf->lfFaceName[i]) ? (BYTE)lf->lfFaceName[i] : 0;
    p[18 + 31] = 0;
}

/* One open Find/Replace dialog: the Win32 FINDREPLACE comdlg32 keeps a pointer
   to for the dialog's whole life, and how to reach the guest's copy. */
#define WCD_MAX_FIND 4
typedef struct {
    HWND           dlg;          /* NULL = free */
    FINDREPLACEA   fr;
    volatile BYTE *o;            /* the guest's FINDREPLACE, host linear */
    DWORD          seg16;        /* ...and as the guest's own 16:16 pointer */
    WORD           owner16, hwnd16;
} wcd_find_t;
static wcd_find_t g_wcd_find[WCD_MAX_FIND];
static UINT       g_wcd_frmsg = 0;     /* "commdlg_FindReplace" */

/* Called by wowwin_proc for every message to a guest window it would otherwise
   not relay. Returns 1 if it was a Find/Replace notification and was posted to
   the guest. The dialog SENDS it (on this thread, from the pump); posting is
   enough because the program reads everything from its own FINDREPLACE, which
   is complete before this returns. */
static int wowcdlg_relay(UINT msg, LPARAM lp)
{
    int i;
    if (!g_wcd_frmsg || msg != g_wcd_frmsg) return 0;
    for (i = 0; i < WCD_MAX_FIND; ++i) {
        wcd_find_t *s = &g_wcd_find[i];
        DWORD fl;
        if (!s->dlg || (LPARAM)&s->fr != lp) continue;
        fl = s->fr.Flags;
        wcd_poked(s->o, FR16_FLAGS, fl & ~FR16_HOOKBITS);
        wowmsg_post(s->owner16, (WORD)msg, 0, s->seg16, GetTickCount(), 0, 0);
        if (fl & FR_DIALOGTERM) {
            wowuser_win_t *w = wowuser_findwin(s->hwnd16);
            if (w && w->hwnd32 == s->dlg) { w->hwnd = 0; w->hwnd32 = NULL; }
            s->dlg = NULL;
        }
        return 1;
    }
    return 0;
}

/* Called by wowwin_pump for every Win32 message it drains: an open Find/Replace
   dialog gets its keyboard (Tab, Enter, Esc) the way any modeless dialog does. */
static int wowcdlg_isdlgmsg(MSG *m)
{
    int i;
    for (i = 0; i < WCD_MAX_FIND; ++i)
        if (g_wcd_find[i].dlg && IsWindow(g_wcd_find[i].dlg)
            && IsDialogMessageA(g_wcd_find[i].dlg, m)) return 1;
    return 0;
}

/*
 * ⚠ CALLED ONLY WHEN THE STUB IS COMMDLG'S. The caller checks, as it does for
 *   USER and SHELL. `0x01` is MessageBox in USER's table and GetOpenFileName here.
 */
static int wowcommdlg_call(wow32_frame_t *f, char *note, int notecap)
{
    if (notecap) note[0] = 0;
    if (f->id != WOWCDLG_EXTENDEDERROR) g_wcd_err = 0;   /* a new call, a new answer */
    switch (f->id) {

    /* ── ★★★★★ 0x01 GetOpenFileName / 0x02 GetSaveFileName(lpOFN) ────────────
         ★ THE REAL Win32 DIALOG IS THE RIGHT ANSWER, and again it is not a
           shortcut -- it is what WOW does. COMMDLG's exported entry points are
           thunks (10 stubs in a 33 KB module), so on a real XP box this call
           lands in comdlg32 and the user gets the OS's file dialog. Building a
           Windows 3.1 file dialog here would be inventing chrome, which is the
           answer session 42 threw away.
       ⚠ MODAL, ON THE EXEC THREAD -- the whole VDM stops until the dialog is
         dismissed, same as ShellAbout. Right for the calling task, wrong for any
         other. The caller announces it before blocking.
       ⚠ THE POINTERS INSIDE ARE THE GUEST'S. They are 16:16 far pointers resolved
         to host linear addresses, which is safe because the guest's memory IS our
         memory -- and it is what makes the write-back work: comdlg32 puts the
         chosen path straight into the application's own buffer, at the size the
         application declared. Nothing is copied back by hand except the three
         scalars Win32 keeps in ITS structure rather than the guest's.
       ⚠ lStructSize IS CHECKED, NOT ASSUMED. The guest declares 0x48 at
         `notepad seg2:0x055d`; anything else means this layout is wrong for this
         caller, and the honest answer is to refuse rather than read 72 bytes of
         something else. A refusal reads as "user cancelled", which is a state
         every caller already handles. */
    case WOWCDLG_GETOPENFILENAME:
    case WOWCDLG_GETSAVEFILENAME: {
        volatile BYTE *o = wow32_argptr(f, OFN_ARG_LPOFN);
        int save = (f->id == WOWCDLG_GETSAVEFILENAME);
        OPENFILENAMEA w32;
        DWORD sz, flags;
        WORD  hwnd16;
        wowuser_win_t *w;
        int k = 0, ok = 0;
        unsigned ci;

        wu_puts(note, notecap, &k, save ? "GetSaveFileName" : "GetOpenFileName");
        if (!o) {
            wu_puts(note, notecap, &k, " -- NULL lpOFN; answered 0 (cancelled)");
            wow32_setret(f, 0);
            return 1;
        }
        sz = wcd_peekd(o, OFN16_STRUCTSIZE);
        wu_puts(note, notecap, &k, " lStructSize=0x");
        wu_puthex(note, notecap, &k, sz, 4);
        if (sz != WOW_OFN16_SIZE) {
            wu_puts(note, notecap, &k, " -- ★ NOT 0x48; this host's OPENFILENAME"
                                       " layout does not describe that structure."
                                       " REFUSED (reads as cancelled) rather than"
                                       " read 72 bytes of something else");
            wow32_setret(f, 0);
            return 1;
        }

        for (ci = 0; ci < sizeof w32; ++ci) ((BYTE *)&w32)[ci] = 0;
        w32.lStructSize = sizeof w32;
        hwnd16 = wow32_peekw(o + OFN16_HWNDOWNER);
        w = hwnd16 ? wowuser_findwin(hwnd16) : NULL;
        w32.hwndOwner = w ? w->hwnd32 : NULL;
        w32.hInstance = NULL;              /* only meaningful with a template */

        w32.lpstrFilter       = (LPCSTR)wow32_farat(f, o, OFN16_FILTER);
        w32.lpstrCustomFilter = (LPSTR) wow32_farat(f, o, OFN16_CUSTFILTER);
        w32.nMaxCustFilter    = wcd_peekd(o, OFN16_MAXCUSTFILTER);
        w32.nFilterIndex      = wcd_peekd(o, OFN16_FILTERINDEX);
        w32.lpstrFile         = (LPSTR) wow32_farat(f, o, OFN16_FILE);
        w32.nMaxFile          = wcd_peekd(o, OFN16_MAXFILE);
        w32.lpstrFileTitle    = (LPSTR) wow32_farat(f, o, OFN16_FILETITLE);
        w32.nMaxFileTitle     = wcd_peekd(o, OFN16_MAXFILETITLE);
        w32.lpstrInitialDir   = (LPCSTR)wow32_farat(f, o, OFN16_INITIALDIR);
        /* s90: NULL means "the current directory" in Win16's COMMDLG -- that is where
           Windows 3.1 always opened. XP's comdlg32 instead prefers the folder last used
           by this EXECUTABLE, and every Win16 program here is ntvdmhost.exe, so Sound
           Recorder's Open dialog came up in Doom's folder (runs/s90/srp0.png). The
           guest's DOS current directory IS this process's (INT 21h AH=47 reads it). */
        if (!w32.lpstrInitialDir) {
            static char cwd16[MAX_PATH];
            if (GetCurrentDirectoryA(sizeof cwd16, cwd16)) w32.lpstrInitialDir = cwd16;
        }
        w32.lpstrTitle        = (LPCSTR)wow32_farat(f, o, OFN16_TITLE);
        w32.lpstrDefExt       = (LPCSTR)wow32_farat(f, o, OFN16_DEFEXT);

        flags = wcd_peekd(o, OFN16_FLAGS);
        /* ── ⚠⚠ OFN_NOCHANGEDIR IS FORCED ON, AND IT IS NOT A PREFERENCE. ─────
             comdlg32 changes the PROCESS current directory to wherever the user
             browsed. That directory is Win32 state; a Win16 guest's current
             directory is DOS-side state this host keeps for it, and the two are
             not the same object. Letting the dialog move one and not the other
             desynchronises them silently -- measured: after one File > Open the
             run shows `WOW32 0xc9 GetCurrentDirectory drive=3 ->
             "Documents and Settings\Matthew\My Documents"`, which the guest never
             asked for and cannot have caused. Every later relative path the guest
             resolves is then resolved against a directory it does not believe it
             is in.
           ★ The chosen file is unaffected: lpstrFile comes back FULLY QUALIFIED,
             so nothing the caller does with the result depends on the CWD. This
             suppresses a side effect, not an answer. */
        w32.Flags = (flags & ~OFN16_HOOKBITS) | OFN_NOCHANGEDIR;

        wu_puts(note, notecap, &k, " owner=0x");
        wu_puthex(note, notecap, &k, hwnd16, 4);
        if (hwnd16 && !w) wu_puts(note, notecap, &k, "(NO SUCH WINDOW -- unowned)");
        wu_puts(note, notecap, &k, " flags=0x");
        wu_puthex(note, notecap, &k, flags, 8);
        if (flags & OFN16_HOOKBITS)
            wu_puts(note, notecap, &k, " -- ★ HOOK/TEMPLATE BITS STRIPPED (a 16-bit"
                                       " hook procedure is not callable from"
                                       " comdlg32)");
        wu_puts(note, notecap, &k, " nMaxFile=0x");
        wu_puthex(note, notecap, &k, w32.nMaxFile, 4);
        if (w32.lpstrFile) {
            wu_puts(note, notecap, &k, " file=");
            wu_putq(note, notecap, &k, w32.lpstrFile);
        }

        /* ⚠ nMaxFile is the GUEST'S claim about its own buffer and the only bound
             there is -- comdlg32 writes the chosen path into it. A caller that
             declared 0 gets a refusal rather than a dialog whose result has
             nowhere to go. */
        if (!w32.lpstrFile || !w32.nMaxFile) {
            wu_puts(note, notecap, &k, " -- ★ NO RESULT BUFFER; refused");
            wow32_setret(f, 0);
            return 1;
        }

        ok = save ? GetSaveFileNameA(&w32) : GetOpenFileNameA(&w32);

        if (ok) {
            /* ── ★★★★ THE ANSWER MUST BE A **SHORT (8.3)** PATH. ─────────────
                 The dialog hands back `C:\Documents and Settings\Matthew\My
                 Documents\test.txt`, and the caller is a 1993 program: it passes
                 that straight to `KERNEL.74 OpenFile`, whose Win16 path parser
                 cannot take components like "Documents and Settings". The open
                 fails inside krnl386, before our DOS layer -- which would have
                 opened the long path perfectly well -- is ever asked.
               ★ AND THE GUEST SAID SO ITSELF, once it could speak. With
                 MessageBox implemented, Notepad puts up "Cannot open the
                 C:\Documents and Settings\Matthew\My Documents\test.txt file."
                 That sentence is the measurement; before it, two sessions of
                 this looked like "nothing happens".
               ⚠ THIS IS A CONVERSION, NOT A DIFFERENT ANSWER. The short form
                 names the same file, and it is what a 16-bit program on a real
                 XP box gets, for exactly this reason.
               ⚠ IF THE VOLUME HAS NO 8.3 NAMES the conversion fails, and the
                 long path is left alone rather than replaced by something
                 shorter and wrong -- the caller then fails the way it does
                 today, and the log says which case it was. */
            /* ── ★★★★★ AND FOR A **SAVE** THE FILE DOES NOT EXIST YET. ───────
                 `GetShortPathNameA` resolves a path by looking it up, so it
                 fails outright on a name that is not on disk -- which is every
                 `GetSaveFileName`. That is not a rare corner: it is the normal
                 case for File > Save As, and it left MS Paint holding
                 `C:\Documents and Settings\Matthew\Desktop\test.BMP` -- 46
                 characters into krnl386's DOS path code. This host's own log
                 said so at the time (*"NO 8.3 NAME … a Win16 OpenFile will
                 probably refuse it"*) and the sentence was read as a note rather
                 than as the defect it was.
               ⇒ Shorten the part that DOES exist -- the directory -- and put the
                 leaf back on. The directory is what carries the long names
                 (`Documents and Settings`, `Matthew`); the leaf came out of an
                 8.3-shaped filter in the first place.
               ⚠ ONLY IF THE LEAF ITSELF FITS 8.3. A guest handed
                 `C:\DOCUME~1\MATTHE~1\Desktop\my long name.bmp` is no better
                 off, so that case falls through to the honest "left LONG" arm
                 rather than producing a path that is short in the middle and
                 impossible at the end. */
            char shortp[MAX_PATH];
            DWORD sn = GetShortPathNameA(w32.lpstrFile, shortp, sizeof shortp);
            if (!sn) {
                char dir[MAX_PATH];
                int  cut = -1, i2, n2 = 0, base = 0, ext = 0, okleaf = 1;
                while (n2 < (int)sizeof dir - 1 && w32.lpstrFile[n2]) {
                    dir[n2] = w32.lpstrFile[n2];
                    if (dir[n2] == '\\') cut = n2;
                    ++n2;
                }
                dir[n2] = 0;
                if (cut > 0) {
                    DWORD dn;
                    dir[cut] = 0;
                    /* the leaf must be 8.3 for this to be worth doing */
                    for (i2 = cut + 1; w32.lpstrFile[i2]; ++i2) {
                        if (w32.lpstrFile[i2] == '.') { ext = 0; base = -1; }
                        else if (base < 0) ++ext; else ++base;
                    }
                    if (base < 0) base = 0;
                    { int b = 0; for (i2 = cut + 1; w32.lpstrFile[i2]
                                       && w32.lpstrFile[i2] != '.'; ++i2) ++b;
                      if (b > 8 || ext > 3) okleaf = 0; }
                    dn = okleaf ? GetShortPathNameA(dir, shortp, sizeof shortp) : 0;
                    if (dn && dn + 1 + (DWORD)(n2 - cut) < sizeof shortp) {
                        DWORD j = dn;
                        for (i2 = cut; w32.lpstrFile[i2]; ++i2) shortp[j++] = w32.lpstrFile[i2];
                        shortp[j] = 0;
                        sn = j;
                        wu_puts(note, notecap, &k, " [directory shortened, leaf kept"
                                                   " -- the file does not exist yet]");
                    }
                }
            }
            if (sn && sn < sizeof shortp && sn + 1 <= w32.nMaxFile) {
                DWORD i, slash = 0, dot = 0;
                for (i = 0; i <= sn; ++i) w32.lpstrFile[i] = shortp[i];
                for (i = 0; shortp[i]; ++i) {
                    if (shortp[i] == '\\' || shortp[i] == ':') slash = i + 1;
                    if (shortp[i] == '.') dot = i + 1;
                }
                w32.nFileOffset    = (WORD)slash;
                w32.nFileExtension = (WORD)(dot > slash ? dot : 0);
                wu_puts(note, notecap, &k, " -> SHORTENED for a Win16 caller: ");
                wu_putq(note, notecap, &k, shortp);
            } else {
                wu_puts(note, notecap, &k, " -- ★ NO 8.3 NAME for this path"
                                           " (or it does not fit the caller's"
                                           " buffer); left LONG, and a Win16"
                                           " OpenFile will probably refuse it");
            }
            /* Only the scalars Win32 keeps in its OWN structure need carrying
               back; the strings were written straight into the guest's buffers. */
            wow32_pokew(o + OFN16_FILEOFFSET,    w32.nFileOffset);
            wow32_pokew(o + OFN16_FILEEXTENSION, w32.nFileExtension);
            wcd_poked(o, OFN16_FILTERINDEX, w32.nFilterIndex);
            wcd_poked(o, OFN16_FLAGS,
                      (w32.Flags & ~OFN16_HOOKBITS) | (flags & OFN16_HOOKBITS));
            wu_puts(note, notecap, &k, " -> CHOSE ");
            wu_putq(note, notecap, &k, w32.lpstrFile);
        } else {
            wu_puts(note, notecap, &k, " -> cancelled (or failed); the guest asks"
                                       " CommDlgExtendedError next");
        }
        wow32_setret(f, (DWORD)(ok ? 1 : 0));
        return 1;
    }

    /* ── ★ 0x1a CommDlgExtendedError() ───────────────────────────────────────
         Notepad calls it the instant GetOpenFileName returns 0 (`seg1:0x0197
         or ax,ax`), to tell "the user pressed Cancel" from "the dialog failed".
       ★ THE REAL ONE IS THE RIGHT ANSWER, and it is genuinely informative here
         rather than a pass-through for its own sake: comdlg32 keeps this per
         THREAD, and the thread that just ran the dialog is this one. So it
         reports on the call we actually made. Zero means "cancelled", which is
         what a returning-0-because-we-refused case should also say. */
    /* ── #294: 0x0b FindText / 0x0c ReplaceText(lpFR) -- THE MODELESS ONES. ──
         Notepad's Search > Find (#285) and Cardfile's. Same principle as the file
         dialog: on a real XP box this lands in comdlg32, so the OS's dialog is
         the answer. What differs is that it is MODELESS: the call returns the
         dialog's HWND at once, the dialog stays up, and every Find Next / Replace
         / close is SENT to the owner as the registered message
         "commdlg_FindReplace" with lParam -> the FINDREPLACE. So three things:
           1. the Win32 FINDREPLACE outlives the call -- it lives in a slot here,
              and its two string pointers point straight INTO the guest's own
              buffers (which the API requires the program to keep alive), so the
              text the user types is already in the program's buffer;
           2. the dialog gets a Win16 handle (a window record with no 16-bit
              procedure), because the program keeps it and passes it to
              IsDialogMessage in its message loop, and tests it for 0;
           3. the notification is relayed by wowwin_proc (wowcdlg_relay) to the
              guest's owner window with lParam = the GUEST's own 16:16 pointer,
              after copying the flags Win32 set back into the guest's struct.
         The message number needs no translation: the guest's
         RegisterWindowMessage is answered by Win32's, so both sides hold the
         same atom for "commdlg_FindReplace".
       ⚠ The Win16 FINDREPLACE is 0x24 bytes and NOT the Win32 layout (2-byte
         hwndOwner/hInstance) -- converted field by field, like OPENFILENAME.
       ⚠ Hooks and templates are stripped, as for the file dialog: a 16-bit hook
         is not callable from comdlg32. */
    case WOWCDLG_FINDTEXT:
    case WOWCDLG_REPLACETEXT: {
        volatile BYTE *o = wow32_argptr(f, 0);
        DWORD seg16 = (DWORD)wow32_argw(f, 0) | ((DWORD)wow32_argw(f, 2) << 16);
        int repl = (f->id == WOWCDLG_REPLACETEXT);
        wcd_find_t *s = NULL;
        wowuser_win_t *ow, *w;
        wowuser_class_t *c;
        WORD hwnd16;
        DWORD flags;
        HWND dlg;
        int k = 0, i;
        wu_puts(note, notecap, &k, repl ? "ReplaceText" : "FindText");
        if (!o || wcd_peekd(o, FR16_STRUCTSIZE) != WOW_FR16_SIZE) {
            wu_puts(note, notecap, &k, " -- NULL or lStructSize != 0x24; refused");
            wow32_setret(f, 0);
            return 1;
        }
        for (i = 0; i < WCD_MAX_FIND; ++i) if (!g_wcd_find[i].dlg) { s = &g_wcd_find[i]; break; }
        hwnd16 = wow32_peekw(o + FR16_HWNDOWNER);
        ow = hwnd16 ? wowuser_findwin(hwnd16) : NULL;
        c  = wowuser_find("#32770");
        if (!s || !ow || !ow->hwnd32 || !c) {
            wu_puts(note, notecap, &k, !s ? " -- all find slots in use"
                                          : " -- no owner window (FindText requires one)");
            wow32_setret(f, 0);
            return 1;
        }
        if (!g_wcd_frmsg) g_wcd_frmsg = RegisterWindowMessageA(FINDMSGSTRINGA);
        for (i = 0; i < (int)sizeof s->fr; ++i) ((BYTE *)&s->fr)[i] = 0;
        flags = wcd_peekd(o, FR16_FLAGS);
        s->fr.lStructSize      = sizeof s->fr;
        s->fr.hwndOwner        = ow->hwnd32;
        s->fr.Flags            = flags & ~FR16_HOOKBITS;
        s->fr.lpstrFindWhat    = (LPSTR)wow32_farat(f, o, FR16_FINDWHAT);
        s->fr.lpstrReplaceWith = (LPSTR)wow32_farat(f, o, FR16_REPLACEWITH);
        s->fr.wFindWhatLen     = wow32_peekw(o + FR16_FINDWHATLEN);
        s->fr.wReplaceWithLen  = wow32_peekw(o + FR16_REPLACEWITHLEN);
        s->fr.lCustData        = (LPARAM)wcd_peekd(o, FR16_CUSTDATA);
        if (!s->fr.lpstrFindWhat || !s->fr.wFindWhatLen
            || (repl && (!s->fr.lpstrReplaceWith || !s->fr.wReplaceWithLen))) {
            wu_puts(note, notecap, &k, " -- no string buffer; refused");
            wow32_setret(f, 0);
            return 1;
        }
        w = wowuser_newwin();
        if (!w) { wu_puts(note, notecap, &k, " -- OUT OF WINDOW SLOTS");
                  wow32_setret(f, 0); return 1; }
        dlg = repl ? ReplaceTextA(&s->fr) : FindTextA(&s->fr);
        if (!dlg) {
            w->hwnd = 0;                               /* give the slot back */
            wu_puts(note, notecap, &k, " -- comdlg32 refused (err 0x");
            wu_puthex(note, notecap, &k, CommDlgExtendedError(), 8);
            wu_puts(note, notecap, &k, ")");
            wow32_setret(f, 0);
            return 1;
        }
        w->cls = (WORD)(c - g_wu_class);
        w->style = (DWORD)GetWindowLongA(dlg, GWL_STYLE);
        w->wndproc = 0;
        w->parent = hwnd16; w->menu = 0; w->hinst = 0;
        w->text[0] = 0; w->hmem = 0; w->menuitems = 0; w->dying = 0;
        for (i = 0; i < WOWUSER_MAX_EXTRA; ++i) w->extra[i] = 0;
        w->hwnd32 = dlg;
        s->dlg = dlg; s->o = o; s->seg16 = seg16;
        s->owner16 = hwnd16; s->hwnd16 = w->hwnd;
        wu_puts(note, notecap, &k, " flags=0x"); wu_puthex(note, notecap, &k, flags, 8);
        if (flags & FR16_HOOKBITS)
            wu_puts(note, notecap, &k, " (hook/template bits STRIPPED)");
        wu_puts(note, notecap, &k, " what=");
        wu_putq(note, notecap, &k, s->fr.lpstrFindWhat);
        wu_puts(note, notecap, &k, " -> MODELESS dialog hwnd16=0x");
        wu_puthex(note, notecap, &k, w->hwnd, 4);
        wu_puts(note, notecap, &k, ", notifications as msg 0x");
        wu_puthex(note, notecap, &k, g_wcd_frmsg, 4);
        wow32_setret(f, w->hwnd);
        return 1;
    }

    /* ── #294: 0x05 ChooseColor(lpCC) -- modal, the OS's dialog. Win16
         CHOOSECOLOR is 0x20 bytes; lpCustColors points at the guest's own 16
         COLORREFs, which are the same bytes in both worlds, so comdlg32 reads
         and updates them in place. rgbResult and Flags are carried back. */
    case WOWCDLG_CHOOSECOLOR: {
        volatile BYTE *o = wow32_argptr(f, 0);
        CHOOSECOLORA cc;
        wowuser_win_t *ow;
        DWORD flags;
        int k = 0, ok, i;
        wu_puts(note, notecap, &k, "ChooseColor");
        if (!o || wcd_peekd(o, CC16_STRUCTSIZE) != WOW_CC16_SIZE) {
            wu_puts(note, notecap, &k, " -- NULL or lStructSize != 0x20; refused");
            wow32_setret(f, 0);
            return 1;
        }
        for (i = 0; i < (int)sizeof cc; ++i) ((BYTE *)&cc)[i] = 0;
        ow = wowuser_findwin(wow32_peekw(o + CC16_HWNDOWNER));
        flags = wcd_peekd(o, CC16_FLAGS);
        cc.lStructSize  = sizeof cc;
        cc.hwndOwner    = ow ? ow->hwnd32 : NULL;
        cc.rgbResult    = wcd_peekd(o, CC16_RGBRESULT);
        cc.lpCustColors = (COLORREF *)wow32_farat(f, o, CC16_CUSTCOLORS);
        cc.Flags        = flags & ~CC16_HOOKBITS;
        cc.lCustData    = (LPARAM)wcd_peekd(o, CC16_CUSTDATA);
        if (!cc.lpCustColors) {
            wu_puts(note, notecap, &k, " -- no lpCustColors (required); refused");
            wow32_setret(f, 0);
            return 1;
        }
        ok = ChooseColorA(&cc);
        if (ok) {
            wcd_poked(o, CC16_RGBRESULT, cc.rgbResult);
            wcd_poked(o, CC16_FLAGS, (cc.Flags & ~CC16_HOOKBITS) | (flags & CC16_HOOKBITS));
        }
        wu_puts(note, notecap, &k, ok ? " -> chose 0x" : " -> cancelled; rgb 0x");
        wu_puthex(note, notecap, &k, cc.rgbResult, 8);
        wow32_setret(f, (DWORD)(ok ? 1 : 0));
        return 1;
    }

    /* ── #294: 0x0f ChooseFont(lpCF) -- modal, the OS's dialog. Win16
         CHOOSEFONT is 0x2e bytes and its LOGFONT is the 16-bit one (five INT16s,
         eight BYTEs, a 32-byte face: 50 bytes), so the font is converted both
         ways rather than pointed at.
       ⚠ PRINTER FONTS NEED A PRINTER DC, and hDC here is a Win16 GDI token for a
         DC this host may not have; the flag is narrowed to screen fonts and the
         line says so. On a machine with no printer (the rig) stock's comdlg32
         would show screen fonts only anyway.
       ⚠ CF_USESTYLE's lpszStyle is a guest buffer comdlg32 writes into directly. */
    case WOWCDLG_CHOOSEFONT: {
        volatile BYTE *o = wow32_argptr(f, 0);
        volatile BYTE *lf16;
        CHOOSEFONTA cf;
        LOGFONTA lf;
        wowuser_win_t *ow;
        DWORD flags;
        int k = 0, ok, i;
        wu_puts(note, notecap, &k, "ChooseFont");
        if (!o || wcd_peekd(o, CF16_STRUCTSIZE) != WOW_CF16_SIZE) {
            wu_puts(note, notecap, &k, " -- NULL or lStructSize != 0x2e; refused");
            wow32_setret(f, 0);
            return 1;
        }
        lf16 = wow32_farat(f, o, CF16_LOGFONT);
        if (!lf16) {
            wu_puts(note, notecap, &k, " -- no lpLogFont (required); refused");
            wow32_setret(f, 0);
            return 1;
        }
        for (i = 0; i < (int)sizeof cf; ++i) ((BYTE *)&cf)[i] = 0;
        for (i = 0; i < (int)sizeof lf; ++i) ((BYTE *)&lf)[i] = 0;
        wcd_lf16_to32(lf16, &lf);
        ow = wowuser_findwin(wow32_peekw(o + CF16_HWNDOWNER));
        flags = wcd_peekd(o, CF16_FLAGS);
        cf.lStructSize = sizeof cf;
        cf.hwndOwner   = ow ? ow->hwnd32 : NULL;
        cf.lpLogFont   = &lf;
        cf.iPointSize  = (INT)(short)wow32_peekw(o + CF16_POINTSIZE);
        cf.Flags       = flags & ~CF16_HOOKBITS;
        if (cf.Flags & CF_PRINTERFONTS) {
            cf.Flags = (cf.Flags & ~CF_PRINTERFONTS) | CF_SCREENFONTS;
            wu_puts(note, notecap, &k, " (printer fonts -> screen fonts: no printer DC)");
        }
        cf.rgbColors   = wcd_peekd(o, CF16_RGBCOLORS);
        cf.lCustData   = (LPARAM)wcd_peekd(o, CF16_CUSTDATA);
        cf.lpszStyle   = (LPSTR)wow32_farat(f, o, CF16_STYLE);
        if (!cf.lpszStyle) cf.Flags &= ~CF_USESTYLE;
        cf.nSizeMin    = (INT)(short)wow32_peekw(o + CF16_SIZEMIN);
        cf.nSizeMax    = (INT)(short)wow32_peekw(o + CF16_SIZEMAX);
        wu_puts(note, notecap, &k, " flags=0x"); wu_puthex(note, notecap, &k, flags, 8);
        wu_puts(note, notecap, &k, " face="); wu_putq(note, notecap, &k, lf.lfFaceName);
        ok = ChooseFontA(&cf);
        if (ok) {
            wcd_lf32_to16(&lf, lf16);
            wow32_pokew(o + CF16_POINTSIZE, (WORD)cf.iPointSize);
            wcd_poked(o, CF16_RGBCOLORS, cf.rgbColors);
            wow32_pokew(o + CF16_FONTTYPE, (WORD)cf.nFontType);
            wcd_poked(o, CF16_FLAGS, (cf.Flags & ~CF16_HOOKBITS) | (flags & CF16_HOOKBITS));
            wu_puts(note, notecap, &k, " -> chose ");
            wu_putq(note, notecap, &k, lf.lfFaceName);
            wu_puts(note, notecap, &k, " pt10=0x");
            wu_puthex(note, notecap, &k, (DWORD)cf.iPointSize, 4);
        } else {
            wu_puts(note, notecap, &k, " -> cancelled (or failed)");
        }
        wow32_setret(f, (DWORD)(ok ? 1 : 0));
        return 1;
    }

    /* ── #294 (s91): 0x14 PrintDlg(lpPD) -- modal, the OS's dialog, as WOW does.
         Converted field by field (2-byte handles in the 16-bit struct). What comes
         back: the page range, nCopies, Flags, and -- for PD_RETURNDC/PD_RETURNIC --
         the printer DC as one of our GDI tokens (Win32 draws on it).
       ⚠ hDevMode/hDevNames ARE NOT CARRIED, either way. They are Win16 GLOBAL
         handles: krnl386 owns that heap, so reading the guest's needs a GlobalLock
         call into it and returning new ones a GlobalAlloc chain (the clipboard's
         shape, wowcall.h). Not built: comdlg32 is given NULL (the default printer)
         and the guest's two words are left as it set them. Win32's are freed. The
         line says so whenever the guest offered one.
       ★ w_cdlg vs stock: a wrong size -> 0 + CDERR_STRUCTSIZE; PD_RETURNDEFAULT on
         a box with no printer -> 0 + PDERR_NODEFAULTPRN, with and without RETURNIC. */
    case WOWCDLG_PRINTDLG: {
        volatile BYTE *o = wow32_argptr(f, 0);
        PRINTDLGA pd;
        wowuser_win_t *ow;
        DWORD flags;
        WORD dm16, dn16;
        int k = 0, ok, i;
        wu_puts(note, notecap, &k, "PrintDlg");
        if (!o || wcd_peekd(o, PD16_STRUCTSIZE) != WOW_PD16_SIZE) {
            wu_puts(note, notecap, &k, " -- NULL or lStructSize != 0x34; refused,"
                                       " CDERR_STRUCTSIZE");
            g_wcd_err = CDERR_STRUCTSIZE;
            wow32_setret(f, 0);
            return 1;
        }
        for (i = 0; i < (int)sizeof pd; ++i) ((BYTE *)&pd)[i] = 0;
        ow = wowuser_findwin(wow32_peekw(o + PD16_HWNDOWNER));
        flags = wcd_peekd(o, PD16_FLAGS);
        dm16 = wow32_peekw(o + PD16_HDEVMODE);
        dn16 = wow32_peekw(o + PD16_HDEVNAMES);
        pd.lStructSize = sizeof pd;
        pd.hwndOwner   = ow ? ow->hwnd32 : NULL;
        pd.Flags       = flags & ~PD16_HOOKBITS;
        pd.nFromPage   = wow32_peekw(o + PD16_FROMPAGE);
        pd.nToPage     = wow32_peekw(o + PD16_TOPAGE);
        pd.nMinPage    = wow32_peekw(o + PD16_MINPAGE);
        pd.nMaxPage    = wow32_peekw(o + PD16_MAXPAGE);
        pd.nCopies     = wow32_peekw(o + PD16_COPIES);
        pd.lCustData   = (LPARAM)wcd_peekd(o, PD16_CUSTDATA);
        wu_puts(note, notecap, &k, " flags=0x"); wu_puthex(note, notecap, &k, flags, 8);
        if (flags & PD16_HOOKBITS)
            wu_puts(note, notecap, &k, " (hook/template bits STRIPPED)");
        if (dm16 | dn16)
            wu_puts(note, notecap, &k, " -- ⚠ the guest's hDevMode/hDevNames are NOT"
                                       " read (Win16 global handles); default printer");
        ok = PrintDlgA(&pd);
        if (pd.hDevMode)  GlobalFree(pd.hDevMode);
        if (pd.hDevNames) GlobalFree(pd.hDevNames);
        if (ok) {
            WORD tok = 0;
            if (pd.hDC) {
                tok = wowgdi_h16((HGDIOBJ)pd.hDC, WOWGDI_KIND_DC);
                if (!tok) DeleteDC(pd.hDC);
            }
            wow32_pokew(o + PD16_HDC,      tok);
            wow32_pokew(o + PD16_FROMPAGE, pd.nFromPage);
            wow32_pokew(o + PD16_TOPAGE,   pd.nToPage);
            wow32_pokew(o + PD16_COPIES,   pd.nCopies);
            wcd_poked(o, PD16_FLAGS, (pd.Flags & ~PD16_HOOKBITS) | (flags & PD16_HOOKBITS));
            wu_puts(note, notecap, &k, " -> OK hDC token 0x");
            wu_puthex(note, notecap, &k, tok, 4);
            wu_puts(note, notecap, &k, " copies=");
            wu_puthex(note, notecap, &k, pd.nCopies, 4);
        } else {
            wu_puts(note, notecap, &k, " -> 0, CommDlgExtendedError 0x");
            wu_puthex(note, notecap, &k, CommDlgExtendedError(), 8);
        }
        wow32_setret(f, (DWORD)(ok ? 1 : 0));
        return 1;
    }

    case WOWCDLG_EXTENDEDERROR: {
        DWORD e = g_wcd_err ? g_wcd_err : CommDlgExtendedError();
        int k = 0;
        wu_puts(note, notecap, &k, "CommDlgExtendedError -> 0x");
        wu_puthex(note, notecap, &k, e, 8);
        wu_puts(note, notecap, &k, e ? " (the dialog FAILED)"
                                     : " (0 = the user cancelled)");
        wow32_setret(f, e);
        return 1;
    }

    default:
        return 0;
    }
}

#endif /* WOWCOMMDLG_H */
