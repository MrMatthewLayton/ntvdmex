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

/*
 * ⚠ CALLED ONLY WHEN THE STUB IS COMMDLG'S. The caller checks, as it does for
 *   USER and SHELL. `0x01` is MessageBox in USER's table and GetOpenFileName here.
 */
static int wowcommdlg_call(wow32_frame_t *f, char *note, int notecap)
{
    if (notecap) note[0] = 0;
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
            char shortp[MAX_PATH];
            DWORD sn = GetShortPathNameA(w32.lpstrFile, shortp, sizeof shortp);
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
    case WOWCDLG_EXTENDEDERROR: {
        DWORD e = CommDlgExtendedError();
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
