#ifndef NTVDMEX_WOWCOMMDLG_H
#define NTVDMEX_WOWCOMMDLG_H
/*
 * wowcommdlg.h -- ★★★ COMMDLG.DLL's OWN ID SPACE -- File > Open.  GH #128, s44.
 *
 * ── WHY THIS IS SMALL, AND WHY THAT WAS A SURPRISE ──────────────────────────
 * The plan was to implement `DialogBox`: turn a Win16 DIALOG template into a real
 * window and run the guest's dialog procedure. That turned out to be the wrong
 * plan, and the run said so before a line of it was written:
 *
 *   * Notepad's File > Open does not go through USER's DialogBox at all. Its
 *     import table (`tools/ne/neimports.py`) names the call outright:
 *     `COMMDLG.1 GETOPENFILENAME`.
 *
 * ⇒ File > Open is ONE call, and the run confirms it: driving Alt-F-O on the live
 *   guest produced exactly two unimplemented BOPs, both from a module this host had
 *   never seen -- `id 0x01, 4 args, retstub 0x0012` and `id 0x1a, 0 args, retstub
 *   0x0090` -- with the stub segment COMMDLG's.
 *
 * ── ★★ THE IDS ARE THE EXPORT ORDINALS, CONFIRMED SEVEN TIMES ───────────────
 * COMMDLG's non-resident name table against the ids its calls arrive with:
 *      1 GETOPENFILENAME -> 0x01     15 CHOOSEFONT   -> 0x0f
 *      2 GETSAVEFILENAME -> 0x02     20 PRINTDLG     -> 0x14
 *      5 CHOOSECOLOR     -> 0x05     26 COMMDLGEXTENDEDERROR -> 0x1a
 *     11 FINDTEXT        -> 0x0b     12 REPLACETEXT  -> 0x0c
 * Seven independent agreements is a reading, not a coincidence -- and it is the
 * same shape SHELL.DLL turned out to have. ⚠ It is NOT a rule: krnl386's ids are
 * nothing like its ordinals. Each module is checked on its own.
 *
 * ── ★★★ THE Win16 OPENFILENAME, 0x48 BYTES, AS NOTEPAD PASSES IT ────────────
 * Not from a header -- the guest declares its own size and fills its own fields,
 * and the structure that arrives (dumped through the far-pointer argument) has
 * every filled field on a field boundary of the layout below:
 *
 *   +0x00 0x0048                     ★ lStructSize, from the guest
 *   +0x08 / +0x0c / +0x28 / +0x2c / +0x38   far pointers into Notepad's DS
 *   +0x30 0x00001004
 *
 * Five far pointers at +0x08/+0x0c/+0x28/+0x2c/+0x38 and a DWORD 0x00001004 at
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
#define WOWCDLG_OPENFILENAME_ARG_LPOFN            0
/* ...and the one far pointer each of the others takes. */
#define WOWCDLG_ARG_LPSTRUCT 0

#define WOWCDLG_OFN16_SIZE          0x48
#define WOWCDLG_OFN16_STRUCTSIZE    0x00
#define WOWCDLG_OFN16_HWNDOWNER     0x04
#define WOWCDLG_OFN16_HINSTANCE     0x06
#define WOWCDLG_OFN16_FILTER        0x08
#define WOWCDLG_OFN16_CUSTFILTER    0x0c
#define WOWCDLG_OFN16_MAXCUSTFILTER 0x10
#define WOWCDLG_OFN16_FILTERINDEX   0x14
#define WOWCDLG_OFN16_FILE          0x18
#define WOWCDLG_OFN16_MAXFILE       0x1c
#define WOWCDLG_OFN16_FILETITLE     0x20
#define WOWCDLG_OFN16_MAXFILETITLE  0x24
#define WOWCDLG_OFN16_INITIALDIR    0x28
#define WOWCDLG_OFN16_TITLE         0x2c
#define WOWCDLG_OFN16_FLAGS         0x30
#define WOWCDLG_OFN16_FILEOFFSET    0x34
#define WOWCDLG_OFN16_FILEEXTENSION 0x36
#define WOWCDLG_OFN16_DEFEXT        0x38
#define WOWCDLG_OFN16_CUSTDATA      0x3c
#define WOWCDLG_OFN16_HOOK          0x40
#define WOWCDLG_OFN16_TEMPLATENAME  0x44

/* ⚠ THE HOOK AND TEMPLATE BITS ARE REFUSED, NOT HONOURED. Either one asks the
     32-bit side to call back into 16-bit code, or to build a dialog from the
     application's own template, and neither is built. Passing them to Win32
     unchanged would hand comdlg32 a 16:16 function pointer it would call as a
     flat one. Notepad sets neither (its Flags are 0x1004), so this strips
     something nothing has asked for -- and says so on the line if it ever does. */
#define WOWCDLG_OFN16_HOOKBITS  (0x00000020UL | 0x00000040UL | 0x00002000UL)
/* ENABLEHOOK | ENABLETEMPLATE | ENABLETEMPLATEHANDLE */

/* An 8.3 leaf: the most the base name and extension may hold. */
#define WOWCDLG_DOS_NAME_MAX      8
#define WOWCDLG_DOS_EXTENSION_MAX 3

static DWORD WowCdlgPeekDword(const volatile BYTE *bytes, INT offset)
{
    return (DWORD)wow32_peekw((volatile BYTE *)bytes + offset)
         | ((DWORD)wow32_peekw((volatile BYTE *)bytes + offset + WOW_WORD_BYTES) << WOW_WORD_SHIFT);
}

static VOID WowCdlgPokeDword(volatile BYTE *bytes, INT offset, DWORD value)
{
    wow32_pokew(bytes + offset,     (WORD)(value & WOW_WORD_MASK));
    wow32_pokew(bytes + offset + WOW_WORD_BYTES, (WORD)(value >> WOW_WORD_SHIFT));
}

/* ── #294: the rest of COMMDLG's table. Ids = export ordinals (see the top). */
#define WOWCDLG_CHOOSECOLOR   0x0005
#define WOWCDLG_FINDTEXT      0x000b
#define WOWCDLG_REPLACETEXT   0x000c
#define WOWCDLG_CHOOSEFONT    0x000f

/* Win16 FINDREPLACE, 0x24 bytes (3.1 SDK; Wine's FINDREPLACE16 agrees). */
#define WOWCDLG_FR16_SIZE           0x24
#define WOWCDLG_FR16_STRUCTSIZE     0x00
#define WOWCDLG_FR16_HWNDOWNER      0x04
#define WOWCDLG_FR16_FLAGS          0x08
#define WOWCDLG_FR16_FINDWHAT       0x0c
#define WOWCDLG_FR16_REPLACEWITH    0x10
#define WOWCDLG_FR16_FINDWHATLEN    0x14
#define WOWCDLG_FR16_REPLACEWITHLEN 0x16
#define WOWCDLG_FR16_CUSTDATA       0x18
#define WOWCDLG_FR16_HOOKBITS  (0x00000100UL | 0x00000200UL | 0x00002000UL)
/* FR_ENABLEHOOK | FR_ENABLETEMPLATE | FR_ENABLETEMPLATEHANDLE */

/* Win16 CHOOSECOLOR, 0x20 bytes. */
#define WOWCDLG_CC16_SIZE       0x20
#define WOWCDLG_CC16_STRUCTSIZE 0x00
#define WOWCDLG_CC16_HWNDOWNER  0x04
#define WOWCDLG_CC16_RGBRESULT  0x08
#define WOWCDLG_CC16_CUSTCOLORS 0x0c
#define WOWCDLG_CC16_FLAGS      0x10
#define WOWCDLG_CC16_CUSTDATA   0x14
#define WOWCDLG_CC16_HOOKBITS  (0x00000010UL | 0x00000020UL | 0x00000040UL)
/* CC_ENABLEHOOK | CC_ENABLETEMPLATE | CC_ENABLETEMPLATEHANDLE */

/* Win16 CHOOSEFONT, 0x2e bytes. */
#define WOWCDLG_CF16_SIZE         0x2e
#define WOWCDLG_CF16_STRUCTSIZE   0x00
#define WOWCDLG_CF16_HWNDOWNER    0x04
#define WOWCDLG_CF16_HDC          0x06
#define WOWCDLG_CF16_LOGFONT      0x08
#define WOWCDLG_CF16_POINTSIZE    0x0c
#define WOWCDLG_CF16_FLAGS        0x0e
#define WOWCDLG_CF16_RGBCOLORS    0x12
#define WOWCDLG_CF16_CUSTDATA     0x16
#define WOWCDLG_CF16_HOOK         0x1a
#define WOWCDLG_CF16_TEMPLATENAME 0x1e
#define WOWCDLG_CF16_HINSTANCE    0x22
#define WOWCDLG_CF16_STYLE        0x24
#define WOWCDLG_CF16_FONTTYPE     0x28
#define WOWCDLG_CF16_SIZEMIN      0x2a
#define WOWCDLG_CF16_SIZEMAX      0x2c
#define WOWCDLG_CF16_HOOKBITS  (0x00000008UL | 0x00000010UL | 0x00000020UL)
/* CF_ENABLEHOOK | CF_ENABLETEMPLATE | CF_ENABLETEMPLATEHANDLE */

/* Win16 PRINTDLG, 0x34 bytes (3.1 SDK; Wine's PRINTDLG16 agrees). */
#define WOWCDLG_PRINTDLG     0x0014
#define WOWCDLG_PD16_SIZE       0x34
#define WOWCDLG_PD16_STRUCTSIZE 0x00
#define WOWCDLG_PD16_HWNDOWNER  0x04
#define WOWCDLG_PD16_HDEVMODE   0x06
#define WOWCDLG_PD16_HDEVNAMES  0x08
#define WOWCDLG_PD16_HDC        0x0a
#define WOWCDLG_PD16_FLAGS      0x0c
#define WOWCDLG_PD16_FROMPAGE   0x10
#define WOWCDLG_PD16_TOPAGE     0x12
#define WOWCDLG_PD16_MINPAGE    0x14
#define WOWCDLG_PD16_MAXPAGE    0x16
#define WOWCDLG_PD16_COPIES     0x18
#define WOWCDLG_PD16_CUSTDATA   0x1c
#define WOWCDLG_PD16_HOOKBITS  (0x00001000UL | 0x00002000UL | 0x00004000UL | 0x00008000UL \
                        | 0x00010000UL | 0x00020000UL)
/* PD_ENABLEPRINTHOOK | PD_ENABLESETUPHOOK | PD_ENABLE{PRINT,SETUP}TEMPLATE[HANDLE] */

/* An answer CommDlgExtendedError owes for a call THIS HOST refused before comdlg32
   saw it (a wrong lStructSize): comdlg32's own per-thread value would say 0, and
   stock says CDERR_STRUCTSIZE (w_cdlg). Cleared by every call that reaches comdlg32. */
static DWORD g_WowCdlgError = 0;

/* Win16 LOGFONT: five INT16s, eight BYTEs, a 32-byte face -- 50 bytes. */
#define WOWCDLG_LF16_HEIGHT         0
#define WOWCDLG_LF16_WIDTH          2
#define WOWCDLG_LF16_ESCAPEMENT     4
#define WOWCDLG_LF16_ORIENTATION    6
#define WOWCDLG_LF16_WEIGHT         8
#define WOWCDLG_LF16_ITALIC         10
#define WOWCDLG_LF16_UNDERLINE      11
#define WOWCDLG_LF16_STRIKEOUT      12
#define WOWCDLG_LF16_CHARSET        13
#define WOWCDLG_LF16_OUTPRECISION   14
#define WOWCDLG_LF16_CLIPPRECISION  15
#define WOWCDLG_LF16_QUALITY        16
#define WOWCDLG_LF16_PITCHANDFAMILY 17
#define WOWCDLG_LF16_FACENAME       18
#define WOWCDLG_LF16_FACESIZE       32
static VOID WowCdlgLogFont16To32(const volatile BYTE *logFont16, PLOGFONTA logFont)
{
    INT index;
    logFont->lfHeight      = (LONG)(SHORT)wow32_peekw((volatile BYTE *)logFont16 + WOWCDLG_LF16_HEIGHT);
    logFont->lfWidth       = (LONG)(SHORT)wow32_peekw((volatile BYTE *)logFont16 + WOWCDLG_LF16_WIDTH);
    logFont->lfEscapement  = (LONG)(SHORT)wow32_peekw((volatile BYTE *)logFont16 + WOWCDLG_LF16_ESCAPEMENT);
    logFont->lfOrientation = (LONG)(SHORT)wow32_peekw((volatile BYTE *)logFont16 + WOWCDLG_LF16_ORIENTATION);
    logFont->lfWeight      = (LONG)(SHORT)wow32_peekw((volatile BYTE *)logFont16 + WOWCDLG_LF16_WEIGHT);
    logFont->lfItalic = logFont16[WOWCDLG_LF16_ITALIC]; logFont->lfUnderline = logFont16[WOWCDLG_LF16_UNDERLINE]; logFont->lfStrikeOut = logFont16[WOWCDLG_LF16_STRIKEOUT];
    logFont->lfCharSet = logFont16[WOWCDLG_LF16_CHARSET]; logFont->lfOutPrecision = logFont16[WOWCDLG_LF16_OUTPRECISION]; logFont->lfClipPrecision = logFont16[WOWCDLG_LF16_CLIPPRECISION];
    logFont->lfQuality = logFont16[WOWCDLG_LF16_QUALITY]; logFont->lfPitchAndFamily = logFont16[WOWCDLG_LF16_PITCHANDFAMILY];
    for (index = 0; index < LF_FACESIZE - 1 && logFont16[WOWCDLG_LF16_FACENAME + index]; ++index) logFont->lfFaceName[index] = (CHAR)logFont16[WOWCDLG_LF16_FACENAME + index];
    logFont->lfFaceName[index] = 0;
}

static VOID WowCdlgLogFont32To16(const LOGFONTA *logFont, volatile BYTE *logFont16)
{
    INT index;
    wow32_pokew(logFont16 + WOWCDLG_LF16_HEIGHT, (WORD)(SHORT)logFont->lfHeight);
    wow32_pokew(logFont16 + WOWCDLG_LF16_WIDTH, (WORD)(SHORT)logFont->lfWidth);
    wow32_pokew(logFont16 + WOWCDLG_LF16_ESCAPEMENT, (WORD)(SHORT)logFont->lfEscapement);
    wow32_pokew(logFont16 + WOWCDLG_LF16_ORIENTATION, (WORD)(SHORT)logFont->lfOrientation);
    wow32_pokew(logFont16 + WOWCDLG_LF16_WEIGHT, (WORD)(SHORT)logFont->lfWeight);
    logFont16[WOWCDLG_LF16_ITALIC] = logFont->lfItalic; logFont16[WOWCDLG_LF16_UNDERLINE] = logFont->lfUnderline; logFont16[WOWCDLG_LF16_STRIKEOUT] = logFont->lfStrikeOut;
    logFont16[WOWCDLG_LF16_CHARSET] = logFont->lfCharSet; logFont16[WOWCDLG_LF16_OUTPRECISION] = logFont->lfOutPrecision; logFont16[WOWCDLG_LF16_CLIPPRECISION] = logFont->lfClipPrecision;
    logFont16[WOWCDLG_LF16_QUALITY] = logFont->lfQuality; logFont16[WOWCDLG_LF16_PITCHANDFAMILY] = logFont->lfPitchAndFamily;
    for (index = 0; index < WOWCDLG_LF16_FACESIZE; ++index) logFont16[WOWCDLG_LF16_FACENAME + index] = (index < LF_FACESIZE && logFont->lfFaceName[index]) ? (BYTE)logFont->lfFaceName[index] : 0;
    logFont16[WOWCDLG_LF16_FACENAME + WOWCDLG_LF16_FACESIZE - 1] = 0;
}

/* One open Find/Replace dialog: the Win32 FINDREPLACE comdlg32 keeps a pointer
   to for the dialog's whole life, and how to reach the guest's copy. */
#define WOWCDLG_MAX_FIND 4
typedef struct _WOWCDLG_FIND {
    HWND           Dialog;       /* NULL = free */
    FINDREPLACEA   FindReplace;
    volatile BYTE *Guest;        /* the guest's FINDREPLACE, host linear */
    DWORD          Guest16;      /* ...and as the guest's own 16:16 pointer */
    WORD           Owner16, Window16;
} WOWCDLG_FIND, *PWOWCDLG_FIND;
static WOWCDLG_FIND g_WowCdlgFinds[WOWCDLG_MAX_FIND];
static UINT         g_WowCdlgFindMessage = 0;     /* "commdlg_FindReplace" */

/* Called by wowwin_proc for every message to a guest window it would otherwise
   not relay. Returns 1 if it was a Find/Replace notification and was posted to
   the guest. The dialog SENDS it (on this thread, from the pump); posting is
   enough because the program reads everything from its own FINDREPLACE, which
   is complete before this returns. */
static INT WowCdlgRelay(UINT message, LPARAM lParam)
{
    INT index;
    if (!g_WowCdlgFindMessage || message != g_WowCdlgFindMessage) return 0;
    for (index = 0; index < WOWCDLG_MAX_FIND; ++index) {
        PWOWCDLG_FIND slot = &g_WowCdlgFinds[index];
        DWORD flags;
        if (!slot->Dialog || (LPARAM)&slot->FindReplace != lParam) continue;
        flags = slot->FindReplace.Flags;
        WowCdlgPokeDword(slot->Guest, WOWCDLG_FR16_FLAGS, flags & ~WOWCDLG_FR16_HOOKBITS);
        WowMsgPost(slot->Owner16, (WORD)message, 0, slot->Guest16, GetTickCount(), 0, 0);
        if (flags & FR_DIALOGTERM) {
            wowuser_win_t *window = wowuser_findwin(slot->Window16);
            if (window && window->hwnd32 == slot->Dialog) { window->hwnd = 0; window->hwnd32 = NULL; }
            slot->Dialog = NULL;
        }
        return 1;
    }
    return 0;
}

/* Called by wowwin_pump for every Win32 message it drains: an open Find/Replace
   dialog gets its keyboard (Tab, Enter, Esc) the way any modeless dialog does. */
static INT WowCdlgIsDialogMessage(PMSG message)
{
    INT index;
    for (index = 0; index < WOWCDLG_MAX_FIND; ++index)
        if (g_WowCdlgFinds[index].Dialog && IsWindow(g_WowCdlgFinds[index].Dialog)
            && IsDialogMessageA(g_WowCdlgFinds[index].Dialog, message)) return 1;
    return 0;
}

/*
 * ⚠ CALLED ONLY WHEN THE STUB IS COMMDLG'S. The caller checks, as it does for
 *   USER and SHELL. `0x01` is MessageBox in USER's table and GetOpenFileName here.
 */
static INT WowCommdlgCall(wow32_frame_t *frame, PSTR note, INT noteCapacity)
{
    if (noteCapacity) note[0] = 0;
    if (frame->id != WOWCDLG_EXTENDEDERROR) g_WowCdlgError = 0;   /* a new call, a new answer */
    switch (frame->id) {

    /* ── ★★★★★ 0x01 GetOpenFileName / 0x02 GetSaveFileName(lpOFN) ────────────
         ★ THE REAL Win32 DIALOG IS THE RIGHT ANSWER, and again it is not a
           shortcut -- it is what WOW does. COMMDLG's exported entry points come
           straight out to the 32-bit side (each arrives here as an id), so on a
           real XP box this call lands in comdlg32 and the user gets the OS's file dialog. Building a
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
       ⚠ lStructSize IS CHECKED, NOT ASSUMED. Notepad's structure arrives
         declaring 0x48; anything else means this layout is wrong for this
         caller, and the honest answer is to refuse rather than read 72 bytes of
         something else. A refusal reads as "user cancelled", which is a state
         every caller already handles. */
    case WOWCDLG_GETOPENFILENAME:
    case WOWCDLG_GETSAVEFILENAME: {
        volatile BYTE *guest = wow32_argptr(frame, WOWCDLG_OPENFILENAME_ARG_LPOFN);
        INT isSave = (frame->id == WOWCDLG_GETSAVEFILENAME);
        OPENFILENAMEA openFileName;
        DWORD structSize, flags;
        WORD  owner16;
        wowuser_win_t *window;
        INT noteLength = 0, isOk = 0;
        UINT byteIndex;

        wu_puts(note, noteCapacity, &noteLength, isSave ? "GetSaveFileName" : "GetOpenFileName");
        if (!guest) {
            wu_puts(note, noteCapacity, &noteLength, " -- NULL lpOFN; answered 0 (cancelled)");
            wow32_setret(frame, 0);
            return 1;
        }
        structSize = WowCdlgPeekDword(guest, WOWCDLG_OFN16_STRUCTSIZE);
        wu_puts(note, noteCapacity, &noteLength, " lStructSize=0x");
        wu_puthex(note, noteCapacity, &noteLength, structSize, WOW_HEX_WORD_DIGITS);
        if (structSize != WOWCDLG_OFN16_SIZE) {
            wu_puts(note, noteCapacity, &noteLength, " -- ★ NOT 0x48; this host's OPENFILENAME"
                                       " layout does not describe that structure."
                                       " REFUSED (reads as cancelled) rather than"
                                       " read 72 bytes of something else");
            wow32_setret(frame, 0);
            return 1;
        }

        for (byteIndex = 0; byteIndex < sizeof openFileName; ++byteIndex) ((PBYTE)&openFileName)[byteIndex] = 0;
        openFileName.lStructSize = sizeof openFileName;
        owner16 = wow32_peekw(guest + WOWCDLG_OFN16_HWNDOWNER);
        window = owner16 ? wowuser_findwin(owner16) : NULL;
        openFileName.hwndOwner = window ? window->hwnd32 : NULL;
        openFileName.hInstance = NULL;              /* only meaningful with a template */

        openFileName.lpstrFilter       = (LPCSTR)wow32_farat(frame, guest, WOWCDLG_OFN16_FILTER);
        openFileName.lpstrCustomFilter = (LPSTR) wow32_farat(frame, guest, WOWCDLG_OFN16_CUSTFILTER);
        openFileName.nMaxCustFilter    = WowCdlgPeekDword(guest, WOWCDLG_OFN16_MAXCUSTFILTER);
        openFileName.nFilterIndex      = WowCdlgPeekDword(guest, WOWCDLG_OFN16_FILTERINDEX);
        openFileName.lpstrFile         = (LPSTR) wow32_farat(frame, guest, WOWCDLG_OFN16_FILE);
        openFileName.nMaxFile          = WowCdlgPeekDword(guest, WOWCDLG_OFN16_MAXFILE);
        openFileName.lpstrFileTitle    = (LPSTR) wow32_farat(frame, guest, WOWCDLG_OFN16_FILETITLE);
        openFileName.nMaxFileTitle     = WowCdlgPeekDword(guest, WOWCDLG_OFN16_MAXFILETITLE);
        openFileName.lpstrInitialDir   = (LPCSTR)wow32_farat(frame, guest, WOWCDLG_OFN16_INITIALDIR);
        /* s90: NULL means "the current directory" in Win16's COMMDLG -- that is where
           Windows 3.1 always opened. XP's comdlg32 instead prefers the folder last used
           by this EXECUTABLE, and every Win16 program here is ntvdmhost.exe, so Sound
           Recorder's Open dialog came up in Doom's folder (runs/s90/srp0.png). The
           guest's DOS current directory IS this process's (INT 21h AH=47 reads it). */
        /* s91: an EMPTY string means the same -- Media Player passes one, and
           comdlg32 opened at C:\ for it, so its own TONE.WAV was "not found". */
        if (!openFileName.lpstrInitialDir || !openFileName.lpstrInitialDir[0]) {
            static CHAR currentDirectory[MAX_PATH];
            if (GetCurrentDirectoryA(sizeof currentDirectory, currentDirectory)) openFileName.lpstrInitialDir = currentDirectory;
        }
        openFileName.lpstrTitle        = (LPCSTR)wow32_farat(frame, guest, WOWCDLG_OFN16_TITLE);
        openFileName.lpstrDefExt       = (LPCSTR)wow32_farat(frame, guest, WOWCDLG_OFN16_DEFEXT);

        flags = WowCdlgPeekDword(guest, WOWCDLG_OFN16_FLAGS);
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
        openFileName.Flags = (flags & ~WOWCDLG_OFN16_HOOKBITS) | OFN_NOCHANGEDIR;

        wu_puts(note, noteCapacity, &noteLength, " owner=0x");
        wu_puthex(note, noteCapacity, &noteLength, owner16, WOW_HEX_WORD_DIGITS);
        if (owner16 && !window) wu_puts(note, noteCapacity, &noteLength, "(NO SUCH WINDOW -- unowned)");
        wu_puts(note, noteCapacity, &noteLength, " flags=0x");
        wu_puthex(note, noteCapacity, &noteLength, flags, WOW_HEX_DWORD_DIGITS);
        if (flags & WOWCDLG_OFN16_HOOKBITS)
            wu_puts(note, noteCapacity, &noteLength, " -- ★ HOOK/TEMPLATE BITS STRIPPED (a 16-bit"
                                       " hook procedure is not callable from"
                                       " comdlg32)");
        wu_puts(note, noteCapacity, &noteLength, " dir=");
        wu_putq(note, noteCapacity, &noteLength, openFileName.lpstrInitialDir ? openFileName.lpstrInitialDir : "(null)");
        wu_puts(note, noteCapacity, &noteLength, " nMaxFile=0x");
        wu_puthex(note, noteCapacity, &noteLength, openFileName.nMaxFile, WOW_HEX_WORD_DIGITS);
        if (openFileName.lpstrFile) {
            wu_puts(note, noteCapacity, &noteLength, " file=");
            wu_putq(note, noteCapacity, &noteLength, openFileName.lpstrFile);
        }

        /* ⚠ nMaxFile is the GUEST'S claim about its own buffer and the only bound
             there is -- comdlg32 writes the chosen path into it. A caller that
             declared 0 gets a refusal rather than a dialog whose result has
             nowhere to go. */
        if (!openFileName.lpstrFile || !openFileName.nMaxFile) {
            wu_puts(note, noteCapacity, &noteLength, " -- ★ NO RESULT BUFFER; refused");
            wow32_setret(frame, 0);
            return 1;
        }

        isOk = isSave ? GetSaveFileNameA(&openFileName) : GetOpenFileNameA(&openFileName);

        if (isOk) {
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
            CHAR shortPath[MAX_PATH];
            DWORD shortLength = GetShortPathNameA(openFileName.lpstrFile, shortPath, sizeof shortPath);
            if (!shortLength) {
                CHAR directory[MAX_PATH];
                INT  lastSlash = -1, position, length = 0, baseLength = 0, extensionLength = 0, isLeafShort = 1;
                while (length < (INT)sizeof directory - 1 && openFileName.lpstrFile[length]) {
                    directory[length] = openFileName.lpstrFile[length];
                    if (directory[length] == '\\') lastSlash = length;
                    ++length;
                }
                directory[length] = 0;
                if (lastSlash > 0) {
                    DWORD directoryLength;
                    directory[lastSlash] = 0;
                    /* the leaf must be 8.3 for this to be worth doing */
                    for (position = lastSlash + 1; openFileName.lpstrFile[position]; ++position) {
                        if (openFileName.lpstrFile[position] == '.') { extensionLength = 0; baseLength = -1; }
                        else if (baseLength < 0) ++extensionLength; else ++baseLength;
                    }
                    if (baseLength < 0) baseLength = 0;
                    { INT stemLength = 0; for (position = lastSlash + 1; openFileName.lpstrFile[position]
                                       && openFileName.lpstrFile[position] != '.'; ++position) ++stemLength;
                      if (stemLength > WOWCDLG_DOS_NAME_MAX || extensionLength > WOWCDLG_DOS_EXTENSION_MAX) isLeafShort = 0; }
                    directoryLength = isLeafShort ? GetShortPathNameA(directory, shortPath, sizeof shortPath) : 0;
                    if (directoryLength && directoryLength + 1 + (DWORD)(length - lastSlash) < sizeof shortPath) {
                        DWORD cursor = directoryLength;
                        for (position = lastSlash; openFileName.lpstrFile[position]; ++position) shortPath[cursor++] = openFileName.lpstrFile[position];
                        shortPath[cursor] = 0;
                        shortLength = cursor;
                        wu_puts(note, noteCapacity, &noteLength, " [directory shortened, leaf kept"
                                                   " -- the file does not exist yet]");
                    }
                }
            }
            if (shortLength && shortLength < sizeof shortPath && shortLength + 1 <= openFileName.nMaxFile) {
                DWORD index, fileOffset = 0, extensionOffset = 0;
                for (index = 0; index <= shortLength; ++index) openFileName.lpstrFile[index] = shortPath[index];
                for (index = 0; shortPath[index]; ++index) {
                    if (shortPath[index] == '\\' || shortPath[index] == ':') fileOffset = index + 1;
                    if (shortPath[index] == '.') extensionOffset = index + 1;
                }
                openFileName.nFileOffset    = (WORD)fileOffset;
                openFileName.nFileExtension = (WORD)(extensionOffset > fileOffset ? extensionOffset : 0);
                wu_puts(note, noteCapacity, &noteLength, " -> SHORTENED for a Win16 caller: ");
                wu_putq(note, noteCapacity, &noteLength, shortPath);
            } else {
                wu_puts(note, noteCapacity, &noteLength, " -- ★ NO 8.3 NAME for this path"
                                           " (or it does not fit the caller's"
                                           " buffer); left LONG, and a Win16"
                                           " OpenFile will probably refuse it");
            }
            /* Only the scalars Win32 keeps in its OWN structure need carrying
               back; the strings were written straight into the guest's buffers. */
            wow32_pokew(guest + WOWCDLG_OFN16_FILEOFFSET,    openFileName.nFileOffset);
            wow32_pokew(guest + WOWCDLG_OFN16_FILEEXTENSION, openFileName.nFileExtension);
            WowCdlgPokeDword(guest, WOWCDLG_OFN16_FILTERINDEX, openFileName.nFilterIndex);
            WowCdlgPokeDword(guest, WOWCDLG_OFN16_FLAGS,
                      (openFileName.Flags & ~WOWCDLG_OFN16_HOOKBITS) | (flags & WOWCDLG_OFN16_HOOKBITS));
            wu_puts(note, noteCapacity, &noteLength, " -> CHOSE ");
            wu_putq(note, noteCapacity, &noteLength, openFileName.lpstrFile);
        } else {
            wu_puts(note, noteCapacity, &noteLength, " -> cancelled (or failed); the guest asks"
                                       " CommDlgExtendedError next");
        }
        wow32_setret(frame, (DWORD)(isOk ? 1 : 0));
        return 1;
    }

    /* ── ★ 0x1a CommDlgExtendedError() ───────────────────────────────────────
         Notepad calls it the instant GetOpenFileName returns 0 (it is the next
         BOP in the run), to tell "the user pressed Cancel" from "the dialog failed".
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
           3. the notification is relayed by wowwin_proc (WowCdlgRelay) to the
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
        volatile BYTE *guest = wow32_argptr(frame, WOWCDLG_ARG_LPSTRUCT);
        DWORD guest16 = (DWORD)wow32_argw(frame, WOWCDLG_ARG_LPSTRUCT) | ((DWORD)wow32_argw(frame, WOWCDLG_ARG_LPSTRUCT + WOW_WORD_BYTES) << WOW_WORD_SHIFT);
        INT isReplace = (frame->id == WOWCDLG_REPLACETEXT);
        PWOWCDLG_FIND slot = NULL;
        wowuser_win_t *ownerWindow, *window;
        wowuser_class_t *dialogClass;
        WORD owner16;
        DWORD flags;
        HWND dialog;
        INT noteLength = 0, index;
        wu_puts(note, noteCapacity, &noteLength, isReplace ? "ReplaceText" : "FindText");
        if (!guest || WowCdlgPeekDword(guest, WOWCDLG_FR16_STRUCTSIZE) != WOWCDLG_FR16_SIZE) {
            wu_puts(note, noteCapacity, &noteLength, " -- NULL or lStructSize != 0x24; refused");
            wow32_setret(frame, 0);
            return 1;
        }
        for (index = 0; index < WOWCDLG_MAX_FIND; ++index) if (!g_WowCdlgFinds[index].Dialog) { slot = &g_WowCdlgFinds[index]; break; }
        owner16 = wow32_peekw(guest + WOWCDLG_FR16_HWNDOWNER);
        ownerWindow = owner16 ? wowuser_findwin(owner16) : NULL;
        dialogClass  = wowuser_find("#32770");
        if (!slot || !ownerWindow || !ownerWindow->hwnd32 || !dialogClass) {
            wu_puts(note, noteCapacity, &noteLength, !slot ? " -- all find slots in use"
                                          : " -- no owner window (FindText requires one)");
            wow32_setret(frame, 0);
            return 1;
        }
        if (!g_WowCdlgFindMessage) g_WowCdlgFindMessage = RegisterWindowMessageA(FINDMSGSTRINGA);
        for (index = 0; index < (INT)sizeof slot->FindReplace; ++index) ((PBYTE)&slot->FindReplace)[index] = 0;
        flags = WowCdlgPeekDword(guest, WOWCDLG_FR16_FLAGS);
        slot->FindReplace.lStructSize      = sizeof slot->FindReplace;
        slot->FindReplace.hwndOwner        = ownerWindow->hwnd32;
        slot->FindReplace.Flags            = flags & ~WOWCDLG_FR16_HOOKBITS;
        slot->FindReplace.lpstrFindWhat    = (LPSTR)wow32_farat(frame, guest, WOWCDLG_FR16_FINDWHAT);
        slot->FindReplace.lpstrReplaceWith = (LPSTR)wow32_farat(frame, guest, WOWCDLG_FR16_REPLACEWITH);
        slot->FindReplace.wFindWhatLen     = wow32_peekw(guest + WOWCDLG_FR16_FINDWHATLEN);
        slot->FindReplace.wReplaceWithLen  = wow32_peekw(guest + WOWCDLG_FR16_REPLACEWITHLEN);
        slot->FindReplace.lCustData        = (LPARAM)WowCdlgPeekDword(guest, WOWCDLG_FR16_CUSTDATA);
        if (!slot->FindReplace.lpstrFindWhat || !slot->FindReplace.wFindWhatLen
            || (isReplace && (!slot->FindReplace.lpstrReplaceWith || !slot->FindReplace.wReplaceWithLen))) {
            wu_puts(note, noteCapacity, &noteLength, " -- no string buffer; refused");
            wow32_setret(frame, 0);
            return 1;
        }
        window = wowuser_newwin();
        if (!window) { wu_puts(note, noteCapacity, &noteLength, " -- OUT OF WINDOW SLOTS");
                  wow32_setret(frame, 0); return 1; }
        dialog = isReplace ? ReplaceTextA(&slot->FindReplace) : FindTextA(&slot->FindReplace);
        if (!dialog) {
            window->hwnd = 0;                               /* give the slot back */
            wu_puts(note, noteCapacity, &noteLength, " -- comdlg32 refused (err 0x");
            wu_puthex(note, noteCapacity, &noteLength, CommDlgExtendedError(), WOW_HEX_DWORD_DIGITS);
            wu_puts(note, noteCapacity, &noteLength, ")");
            wow32_setret(frame, 0);
            return 1;
        }
        window->cls = (WORD)(dialogClass - g_wu_class);
        window->style = (DWORD)GetWindowLongA(dialog, GWL_STYLE);
        window->wndproc = 0;
        window->parent = owner16; window->menu = 0; window->hinst = 0;
        window->text[0] = 0; window->hmem = 0; window->menuitems = 0; window->dying = 0;
        for (index = 0; index < WOWUSER_MAX_EXTRA; ++index) window->extra[index] = 0;
        window->hwnd32 = dialog;
        slot->Dialog = dialog; slot->Guest = guest; slot->Guest16 = guest16;
        slot->Owner16 = owner16; slot->Window16 = window->hwnd;
        wu_puts(note, noteCapacity, &noteLength, " flags=0x"); wu_puthex(note, noteCapacity, &noteLength, flags, WOW_HEX_DWORD_DIGITS);
        if (flags & WOWCDLG_FR16_HOOKBITS)
            wu_puts(note, noteCapacity, &noteLength, " (hook/template bits STRIPPED)");
        wu_puts(note, noteCapacity, &noteLength, " what=");
        wu_putq(note, noteCapacity, &noteLength, slot->FindReplace.lpstrFindWhat);
        wu_puts(note, noteCapacity, &noteLength, " -> MODELESS dialog hwnd16=0x");
        wu_puthex(note, noteCapacity, &noteLength, window->hwnd, WOW_HEX_WORD_DIGITS);
        wu_puts(note, noteCapacity, &noteLength, ", notifications as msg 0x");
        wu_puthex(note, noteCapacity, &noteLength, g_WowCdlgFindMessage, WOW_HEX_WORD_DIGITS);
        wow32_setret(frame, window->hwnd);
        return 1;
    }

    /* ── #294: 0x05 ChooseColor(lpCC) -- modal, the OS's dialog. Win16
         CHOOSECOLOR is 0x20 bytes; lpCustColors points at the guest's own 16
         COLORREFs, which are the same bytes in both worlds, so comdlg32 reads
         and updates them in place. rgbResult and Flags are carried back. */
    case WOWCDLG_CHOOSECOLOR: {
        volatile BYTE *guest = wow32_argptr(frame, WOWCDLG_ARG_LPSTRUCT);
        CHOOSECOLORA chooseColor;
        wowuser_win_t *ownerWindow;
        DWORD flags;
        INT noteLength = 0, isOk, index;
        wu_puts(note, noteCapacity, &noteLength, "ChooseColor");
        if (!guest || WowCdlgPeekDword(guest, WOWCDLG_CC16_STRUCTSIZE) != WOWCDLG_CC16_SIZE) {
            wu_puts(note, noteCapacity, &noteLength, " -- NULL or lStructSize != 0x20; refused");
            wow32_setret(frame, 0);
            return 1;
        }
        for (index = 0; index < (INT)sizeof chooseColor; ++index) ((PBYTE)&chooseColor)[index] = 0;
        ownerWindow = wowuser_findwin(wow32_peekw(guest + WOWCDLG_CC16_HWNDOWNER));
        flags = WowCdlgPeekDword(guest, WOWCDLG_CC16_FLAGS);
        chooseColor.lStructSize  = sizeof chooseColor;
        chooseColor.hwndOwner    = ownerWindow ? ownerWindow->hwnd32 : NULL;
        chooseColor.rgbResult    = WowCdlgPeekDword(guest, WOWCDLG_CC16_RGBRESULT);
        chooseColor.lpCustColors = (COLORREF *)wow32_farat(frame, guest, WOWCDLG_CC16_CUSTCOLORS);
        chooseColor.Flags        = flags & ~WOWCDLG_CC16_HOOKBITS;
        chooseColor.lCustData    = (LPARAM)WowCdlgPeekDword(guest, WOWCDLG_CC16_CUSTDATA);
        if (!chooseColor.lpCustColors) {
            wu_puts(note, noteCapacity, &noteLength, " -- no lpCustColors (required); refused");
            wow32_setret(frame, 0);
            return 1;
        }
        isOk = ChooseColorA(&chooseColor);
        if (isOk) {
            WowCdlgPokeDword(guest, WOWCDLG_CC16_RGBRESULT, chooseColor.rgbResult);
            WowCdlgPokeDword(guest, WOWCDLG_CC16_FLAGS, (chooseColor.Flags & ~WOWCDLG_CC16_HOOKBITS) | (flags & WOWCDLG_CC16_HOOKBITS));
        }
        wu_puts(note, noteCapacity, &noteLength, isOk ? " -> chose 0x" : " -> cancelled; rgb 0x");
        wu_puthex(note, noteCapacity, &noteLength, chooseColor.rgbResult, WOW_HEX_DWORD_DIGITS);
        wow32_setret(frame, (DWORD)(isOk ? 1 : 0));
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
        volatile BYTE *guest = wow32_argptr(frame, WOWCDLG_ARG_LPSTRUCT);
        volatile BYTE *logFont16;
        CHOOSEFONTA chooseFont;
        LOGFONTA logFont;
        wowuser_win_t *ownerWindow;
        DWORD flags;
        INT noteLength = 0, isOk, index;
        wu_puts(note, noteCapacity, &noteLength, "ChooseFont");
        if (!guest || WowCdlgPeekDword(guest, WOWCDLG_CF16_STRUCTSIZE) != WOWCDLG_CF16_SIZE) {
            wu_puts(note, noteCapacity, &noteLength, " -- NULL or lStructSize != 0x2e; refused");
            wow32_setret(frame, 0);
            return 1;
        }
        logFont16 = wow32_farat(frame, guest, WOWCDLG_CF16_LOGFONT);
        if (!logFont16) {
            wu_puts(note, noteCapacity, &noteLength, " -- no lpLogFont (required); refused");
            wow32_setret(frame, 0);
            return 1;
        }
        for (index = 0; index < (INT)sizeof chooseFont; ++index) ((PBYTE)&chooseFont)[index] = 0;
        for (index = 0; index < (INT)sizeof logFont; ++index) ((PBYTE)&logFont)[index] = 0;
        WowCdlgLogFont16To32(logFont16, &logFont);
        ownerWindow = wowuser_findwin(wow32_peekw(guest + WOWCDLG_CF16_HWNDOWNER));
        flags = WowCdlgPeekDword(guest, WOWCDLG_CF16_FLAGS);
        chooseFont.lStructSize = sizeof chooseFont;
        chooseFont.hwndOwner   = ownerWindow ? ownerWindow->hwnd32 : NULL;
        chooseFont.lpLogFont   = &logFont;
        chooseFont.iPointSize  = (INT)(SHORT)wow32_peekw(guest + WOWCDLG_CF16_POINTSIZE);
        chooseFont.Flags       = flags & ~WOWCDLG_CF16_HOOKBITS;
        if (chooseFont.Flags & CF_PRINTERFONTS) {
            chooseFont.Flags = (chooseFont.Flags & ~CF_PRINTERFONTS) | CF_SCREENFONTS;
            wu_puts(note, noteCapacity, &noteLength, " (printer fonts -> screen fonts: no printer DC)");
        }
        chooseFont.rgbColors   = WowCdlgPeekDword(guest, WOWCDLG_CF16_RGBCOLORS);
        chooseFont.lCustData   = (LPARAM)WowCdlgPeekDword(guest, WOWCDLG_CF16_CUSTDATA);
        chooseFont.lpszStyle   = (LPSTR)wow32_farat(frame, guest, WOWCDLG_CF16_STYLE);
        if (!chooseFont.lpszStyle) chooseFont.Flags &= ~CF_USESTYLE;
        chooseFont.nSizeMin    = (INT)(SHORT)wow32_peekw(guest + WOWCDLG_CF16_SIZEMIN);
        chooseFont.nSizeMax    = (INT)(SHORT)wow32_peekw(guest + WOWCDLG_CF16_SIZEMAX);
        wu_puts(note, noteCapacity, &noteLength, " flags=0x"); wu_puthex(note, noteCapacity, &noteLength, flags, WOW_HEX_DWORD_DIGITS);
        wu_puts(note, noteCapacity, &noteLength, " face="); wu_putq(note, noteCapacity, &noteLength, logFont.lfFaceName);
        isOk = ChooseFontA(&chooseFont);
        if (isOk) {
            WowCdlgLogFont32To16(&logFont, logFont16);
            wow32_pokew(guest + WOWCDLG_CF16_POINTSIZE, (WORD)chooseFont.iPointSize);
            WowCdlgPokeDword(guest, WOWCDLG_CF16_RGBCOLORS, chooseFont.rgbColors);
            wow32_pokew(guest + WOWCDLG_CF16_FONTTYPE, (WORD)chooseFont.nFontType);
            WowCdlgPokeDword(guest, WOWCDLG_CF16_FLAGS, (chooseFont.Flags & ~WOWCDLG_CF16_HOOKBITS) | (flags & WOWCDLG_CF16_HOOKBITS));
            wu_puts(note, noteCapacity, &noteLength, " -> chose ");
            wu_putq(note, noteCapacity, &noteLength, logFont.lfFaceName);
            wu_puts(note, noteCapacity, &noteLength, " pt10=0x");
            wu_puthex(note, noteCapacity, &noteLength, (DWORD)chooseFont.iPointSize, WOW_HEX_WORD_DIGITS);
        } else {
            wu_puts(note, noteCapacity, &noteLength, " -> cancelled (or failed)");
        }
        wow32_setret(frame, (DWORD)(isOk ? 1 : 0));
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
        volatile BYTE *guest = wow32_argptr(frame, WOWCDLG_ARG_LPSTRUCT);
        PRINTDLGA printDialog;
        wowuser_win_t *ownerWindow;
        DWORD flags;
        WORD devMode16, devNames16;
        INT noteLength = 0, isOk, index;
        wu_puts(note, noteCapacity, &noteLength, "PrintDlg");
        if (!guest || WowCdlgPeekDword(guest, WOWCDLG_PD16_STRUCTSIZE) != WOWCDLG_PD16_SIZE) {
            wu_puts(note, noteCapacity, &noteLength, " -- NULL or lStructSize != 0x34; refused,"
                                       " CDERR_STRUCTSIZE");
            g_WowCdlgError = CDERR_STRUCTSIZE;
            wow32_setret(frame, 0);
            return 1;
        }
        for (index = 0; index < (INT)sizeof printDialog; ++index) ((PBYTE)&printDialog)[index] = 0;
        ownerWindow = wowuser_findwin(wow32_peekw(guest + WOWCDLG_PD16_HWNDOWNER));
        flags = WowCdlgPeekDword(guest, WOWCDLG_PD16_FLAGS);
        devMode16 = wow32_peekw(guest + WOWCDLG_PD16_HDEVMODE);
        devNames16 = wow32_peekw(guest + WOWCDLG_PD16_HDEVNAMES);
        printDialog.lStructSize = sizeof printDialog;
        printDialog.hwndOwner   = ownerWindow ? ownerWindow->hwnd32 : NULL;
        printDialog.Flags       = flags & ~WOWCDLG_PD16_HOOKBITS;
        printDialog.nFromPage   = wow32_peekw(guest + WOWCDLG_PD16_FROMPAGE);
        printDialog.nToPage     = wow32_peekw(guest + WOWCDLG_PD16_TOPAGE);
        printDialog.nMinPage    = wow32_peekw(guest + WOWCDLG_PD16_MINPAGE);
        printDialog.nMaxPage    = wow32_peekw(guest + WOWCDLG_PD16_MAXPAGE);
        printDialog.nCopies     = wow32_peekw(guest + WOWCDLG_PD16_COPIES);
        printDialog.lCustData   = (LPARAM)WowCdlgPeekDword(guest, WOWCDLG_PD16_CUSTDATA);
        wu_puts(note, noteCapacity, &noteLength, " flags=0x"); wu_puthex(note, noteCapacity, &noteLength, flags, WOW_HEX_DWORD_DIGITS);
        if (flags & WOWCDLG_PD16_HOOKBITS)
            wu_puts(note, noteCapacity, &noteLength, " (hook/template bits STRIPPED)");
        if (devMode16 | devNames16)
            wu_puts(note, noteCapacity, &noteLength, " -- ⚠ the guest's hDevMode/hDevNames are NOT"
                                       " read (Win16 global handles); default printer");
        isOk = PrintDlgA(&printDialog);
        if (printDialog.hDevMode)  GlobalFree(printDialog.hDevMode);
        if (printDialog.hDevNames) GlobalFree(printDialog.hDevNames);
        if (isOk) {
            WORD token = 0;
            if (printDialog.hDC) {
                token = wowgdi_h16((HGDIOBJ)printDialog.hDC, WOWGDI_KIND_DC);
                if (!token) DeleteDC(printDialog.hDC);
            }
            wow32_pokew(guest + WOWCDLG_PD16_HDC,      token);
            wow32_pokew(guest + WOWCDLG_PD16_FROMPAGE, printDialog.nFromPage);
            wow32_pokew(guest + WOWCDLG_PD16_TOPAGE,   printDialog.nToPage);
            wow32_pokew(guest + WOWCDLG_PD16_COPIES,   printDialog.nCopies);
            WowCdlgPokeDword(guest, WOWCDLG_PD16_FLAGS, (printDialog.Flags & ~WOWCDLG_PD16_HOOKBITS) | (flags & WOWCDLG_PD16_HOOKBITS));
            wu_puts(note, noteCapacity, &noteLength, " -> OK hDC token 0x");
            wu_puthex(note, noteCapacity, &noteLength, token, WOW_HEX_WORD_DIGITS);
            wu_puts(note, noteCapacity, &noteLength, " copies=");
            wu_puthex(note, noteCapacity, &noteLength, printDialog.nCopies, WOW_HEX_WORD_DIGITS);
        } else {
            wu_puts(note, noteCapacity, &noteLength, " -> 0, CommDlgExtendedError 0x");
            wu_puthex(note, noteCapacity, &noteLength, CommDlgExtendedError(), WOW_HEX_DWORD_DIGITS);
        }
        wow32_setret(frame, (DWORD)(isOk ? 1 : 0));
        return 1;
    }

    case WOWCDLG_EXTENDEDERROR: {
        DWORD error = g_WowCdlgError ? g_WowCdlgError : CommDlgExtendedError();
        INT noteLength = 0;
        wu_puts(note, noteCapacity, &noteLength, "CommDlgExtendedError -> 0x");
        wu_puthex(note, noteCapacity, &noteLength, error, WOW_HEX_DWORD_DIGITS);
        wu_puts(note, noteCapacity, &noteLength, error ? " (the dialog FAILED)"
                                     : " (0 = the user cancelled)");
        wow32_setret(frame, error);
        return 1;
    }

    default:
        return 0;
    }
}

#endif /* NTVDMEX_WOWCOMMDLG_H */
