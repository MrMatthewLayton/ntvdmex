/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * COMMDLG.DLL's OWN ID SPACE -- File > Open.  GH #128, s44.
 *
 * The code of wowcommdlg.h (#335): its functions and state, in their original order;
 * its own translation unit, declared in wowcommdlg.h.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include "host_state.h"
#include "log.h"
#include "ne.h"
#include "wow32.h"
#include "wowanchors.h"
#include "wowsched.h"
#include "wowcall.h"
#include "wowmsg.h"
#include "wowres.h"
#include "wowwin.h"
#include "wowgdi.h"
#include "wowuser.h"
#include "wowdlg.h"
#include "wowenum.h"
#include "wowshell.h"
#include "wowcommdlg.h"

static DWORD WowCdlgPeekDword(const volatile BYTE *bytes, INT offset)
{
    return (DWORD)Wow32PeekWord((volatile BYTE *)bytes + offset)
         | ((DWORD)Wow32PeekWord((volatile BYTE *)bytes + offset + WOW_WORD_BYTES) << WORD_SHIFT);
}

static VOID WowCdlgPokeDword(volatile BYTE *bytes, INT offset, DWORD value)
{
    Wow32PokeWord(bytes + offset,     (WORD)(value & WORD_MASK));
    Wow32PokeWord(bytes + offset + WOW_WORD_BYTES, (WORD)(value >> WORD_SHIFT));
}

/* An answer CommDlgExtendedError owes for a call THIS HOST refused before comdlg32
 * saw it (a wrong lStructSize): comdlg32's own per-thread value would say 0, and
 * stock says CDERR_STRUCTSIZE (w_cdlg). Cleared by every call that reaches comdlg32.
 */
static DWORD g_WowCdlgError = 0;

static VOID WowCdlgLogFont16To32(const volatile BYTE *logFont16, PLOGFONTA logFont)
{
    INT index;

    logFont->lfHeight      = (LONG)(SHORT)Wow32PeekWord((volatile BYTE *)logFont16 + WOWCDLG_LF16_HEIGHT);
    logFont->lfWidth       = (LONG)(SHORT)Wow32PeekWord((volatile BYTE *)logFont16 + WOWCDLG_LF16_WIDTH);
    logFont->lfEscapement  = (LONG)(SHORT)Wow32PeekWord((volatile BYTE *)logFont16 + WOWCDLG_LF16_ESCAPEMENT);
    logFont->lfOrientation = (LONG)(SHORT)Wow32PeekWord((volatile BYTE *)logFont16 + WOWCDLG_LF16_ORIENTATION);
    logFont->lfWeight      = (LONG)(SHORT)Wow32PeekWord((volatile BYTE *)logFont16 + WOWCDLG_LF16_WEIGHT);
    logFont->lfItalic = logFont16[WOWCDLG_LF16_ITALIC];
    logFont->lfUnderline = logFont16[WOWCDLG_LF16_UNDERLINE];
    logFont->lfStrikeOut = logFont16[WOWCDLG_LF16_STRIKEOUT];
    logFont->lfCharSet = logFont16[WOWCDLG_LF16_CHARSET];
    logFont->lfOutPrecision = logFont16[WOWCDLG_LF16_OUTPRECISION];
    logFont->lfClipPrecision = logFont16[WOWCDLG_LF16_CLIPPRECISION];
    logFont->lfQuality = logFont16[WOWCDLG_LF16_QUALITY];
    logFont->lfPitchAndFamily = logFont16[WOWCDLG_LF16_PITCHANDFAMILY];
    for (index = 0; index < LF_FACESIZE - 1 && logFont16[WOWCDLG_LF16_FACENAME + index]; ++index)
        logFont->lfFaceName[index] = (CHAR)logFont16[WOWCDLG_LF16_FACENAME + index];
    logFont->lfFaceName[index] = 0;
}

static VOID WowCdlgLogFont32To16(const LOGFONTA *logFont, volatile BYTE *logFont16)
{
    INT index;

    Wow32PokeWord(logFont16 + WOWCDLG_LF16_HEIGHT, (WORD)(SHORT)logFont->lfHeight);
    Wow32PokeWord(logFont16 + WOWCDLG_LF16_WIDTH, (WORD)(SHORT)logFont->lfWidth);
    Wow32PokeWord(logFont16 + WOWCDLG_LF16_ESCAPEMENT, (WORD)(SHORT)logFont->lfEscapement);
    Wow32PokeWord(logFont16 + WOWCDLG_LF16_ORIENTATION, (WORD)(SHORT)logFont->lfOrientation);
    Wow32PokeWord(logFont16 + WOWCDLG_LF16_WEIGHT, (WORD)(SHORT)logFont->lfWeight);
    logFont16[WOWCDLG_LF16_ITALIC] = logFont->lfItalic;
    logFont16[WOWCDLG_LF16_UNDERLINE] = logFont->lfUnderline;
    logFont16[WOWCDLG_LF16_STRIKEOUT] = logFont->lfStrikeOut;
    logFont16[WOWCDLG_LF16_CHARSET] = logFont->lfCharSet;
    logFont16[WOWCDLG_LF16_OUTPRECISION] = logFont->lfOutPrecision;
    logFont16[WOWCDLG_LF16_CLIPPRECISION] = logFont->lfClipPrecision;
    logFont16[WOWCDLG_LF16_QUALITY] = logFont->lfQuality;
    logFont16[WOWCDLG_LF16_PITCHANDFAMILY] = logFont->lfPitchAndFamily;
    for (index = 0; index < WOWCDLG_LF16_FACESIZE; ++index)
        logFont16[WOWCDLG_LF16_FACENAME + index] = (index < LF_FACESIZE && logFont->lfFaceName[index]) ? (BYTE)logFont->lfFaceName[index] : 0;
    logFont16[WOWCDLG_LF16_FACENAME + WOWCDLG_LF16_FACESIZE - 1] = 0;
}

static WOWCDLG_FIND g_WowCdlgFinds[WOWCDLG_MAX_FIND];
static UINT         g_WowCdlgFindMessage = 0;     /* "commdlg_FindReplace" */

/* Called by WowWinProc for every message to a guest window it would otherwise
 * not relay. Returns 1 if it was a Find/Replace notification and was posted to
 * the guest. The dialog SENDS it (on this thread, from the pump); posting is
 * enough because the program reads everything from its own FINDREPLACE, which
 * is complete before this returns.
 */
INT WowCdlgRelay(UINT message, LPARAM lParam)
{
    INT index;

    if (!g_WowCdlgFindMessage || message != g_WowCdlgFindMessage)
        return 0;
    for (index = 0; index < WOWCDLG_MAX_FIND; ++index)
    {
        PWOWCDLG_FIND slot = &g_WowCdlgFinds[index];
        DWORD flags;
        if (!slot->Dialog || (LPARAM)&slot->FindReplace != lParam)
            continue;
        flags = slot->FindReplace.Flags;
        WowCdlgPokeDword(slot->Guest, WOWCDLG_FR16_FLAGS, flags & ~WOWCDLG_FR16_HOOKBITS);
        WowMsgPost(slot->Owner16, (WORD)message, 0, slot->Guest16, GetTickCount(), 0, 0);
        if (flags & FR_DIALOGTERM)
        {
            PWOWUSER_WINDOW window = WowUserFindWindow(slot->Window16);
            if (window && window->Window32 == slot->Dialog)
            {
                window->Window16 = 0;
                window->Window32 = NULL;
            }
            slot->Dialog = NULL;
        }
        return 1;
    }
    return 0;
}

/* Called by WowWinPump for every Win32 message it drains: an open Find/Replace
 * dialog gets its keyboard (Tab, Enter, Esc) the way any modeless dialog does.
 */
INT WowCdlgIsDialogMessage(PMSG message)
{
    INT index;

    for (index = 0; index < WOWCDLG_MAX_FIND; ++index)
        if (g_WowCdlgFinds[index].Dialog && IsWindow(g_WowCdlgFinds[index].Dialog)
            && IsDialogMessageA(g_WowCdlgFinds[index].Dialog, message))
            return 1;
    return 0;
}

/* [CAUTION]: CALLED ONLY WHEN THE STUB IS COMMDLG'S. The caller checks, as it does for
 * USER and SHELL. `0x01` is MessageBox in USER's table and GetOpenFileName here.
 */
INT WowCommdlgCall(PWOW32_FRAME frame, PSTR note, INT noteCapacity)
{
    if (noteCapacity)
        note[0] = 0;
    if (frame->Id != WOWCDLG_EXTENDEDERROR)
        g_WowCdlgError = 0;                                       /* a new call, a new answer */
    switch (frame->Id)
    {

    /* 0x01 GetOpenFileName / 0x02 GetSaveFileName(lpOFN) (Importance = 5):
     *
     * [INFO]: THE REAL Win32 DIALOG IS THE RIGHT ANSWER, and again it is not a
     * shortcut -- it is what WOW does. COMMDLG's exported entry points come
     * straight out to the 32-bit side (each arrives here as an id), so on a
     * real XP box this call lands in comdlg32 and the user gets the OS's file dialog. Building a
     * Windows 3.1 file dialog here would be inventing chrome, which is the
     * answer session 42 threw away.
     *
     * [CAUTION]: MODAL, ON THE EXEC THREAD -- the whole VDM stops until the dialog is
     * dismissed, same as ShellAbout. Right for the calling task, wrong for any
     * other. The caller announces it before blocking.
     *
     * [CAUTION]: THE POINTERS INSIDE ARE THE GUEST'S. They are 16:16 far pointers resolved
     * to host linear addresses, which is safe because the guest's memory IS our
     * memory -- and it is what makes the write-back work: comdlg32 puts the
     * chosen path straight into the application's own buffer, at the size the
     * application declared. Nothing is copied back by hand except the three
     * scalars Win32 keeps in ITS structure rather than the guest's.
     *
     * [CAUTION]: lStructSize IS CHECKED, NOT ASSUMED. Notepad's structure arrives
     * declaring 0x48; anything else means this layout is wrong for this
     * caller, and the honest answer is to refuse rather than read 72 bytes of
     * something else. A refusal reads as "user cancelled", which is a state
     * every caller already handles.
     */
    case WOWCDLG_GETOPENFILENAME:
    case WOWCDLG_GETSAVEFILENAME:
    {
        volatile BYTE *guest = Wow32ArgPointer(frame, WOWCDLG_OPENFILENAME_ARG_LPOFN);
        INT isSave = (frame->Id == WOWCDLG_GETSAVEFILENAME);
        OPENFILENAMEA openFileName;
        DWORD structSize, flags;
        WORD  owner16;
        PWOWUSER_WINDOW window;
        INT noteLength = 0, isOk = 0;
        UINT byteIndex;

        WowNotePut(note, noteCapacity, &noteLength, isSave ? "GetSaveFileName" : "GetOpenFileName");
        if (!guest)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- NULL lpOFN; answered 0 (cancelled)");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        structSize = WowCdlgPeekDword(guest, WOWCDLG_OFN16_STRUCTSIZE);
        WowNotePut(note, noteCapacity, &noteLength, " lStructSize=0x");
        WowNoteHex(note, noteCapacity, &noteLength, structSize, WOW_HEX_WORD_DIGITS);
        if (structSize != WOWCDLG_OFN16_SIZE)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NOT 0x48; this host's OPENFILENAME"
                                       " layout does not describe that structure."
                                       " REFUSED (reads as cancelled) rather than"
                                       " read 72 bytes of something else");
            Wow32SetReturn(frame, 0);
            return 1;
        }

        for (byteIndex = 0; byteIndex < sizeof openFileName; ++byteIndex)
            ((PBYTE)&openFileName)[byteIndex] = 0;
        openFileName.lStructSize = sizeof openFileName;
        owner16 = Wow32PeekWord(guest + WOWCDLG_OFN16_HWNDOWNER);
        window = owner16 ? WowUserFindWindow(owner16) : NULL;
        openFileName.hwndOwner = window ? window->Window32 : NULL;
        openFileName.hInstance = NULL;              /* only meaningful with a template */

        openFileName.lpstrFilter       = (LPCSTR)Wow32FarAt(frame, guest, WOWCDLG_OFN16_FILTER);
        openFileName.lpstrCustomFilter = (LPSTR) Wow32FarAt(frame, guest, WOWCDLG_OFN16_CUSTFILTER);
        openFileName.nMaxCustFilter    = WowCdlgPeekDword(guest, WOWCDLG_OFN16_MAXCUSTFILTER);
        openFileName.nFilterIndex      = WowCdlgPeekDword(guest, WOWCDLG_OFN16_FILTERINDEX);
        openFileName.lpstrFile         = (LPSTR) Wow32FarAt(frame, guest, WOWCDLG_OFN16_FILE);
        openFileName.nMaxFile          = WowCdlgPeekDword(guest, WOWCDLG_OFN16_MAXFILE);
        openFileName.lpstrFileTitle    = (LPSTR) Wow32FarAt(frame, guest, WOWCDLG_OFN16_FILETITLE);
        openFileName.nMaxFileTitle     = WowCdlgPeekDword(guest, WOWCDLG_OFN16_MAXFILETITLE);
        openFileName.lpstrInitialDir   = (LPCSTR)Wow32FarAt(frame, guest, WOWCDLG_OFN16_INITIALDIR);
        /* s90: NULL means "the current directory" in Win16's COMMDLG -- that is where
         * Windows 3.1 always opened. XP's comdlg32 instead prefers the folder last used
         * by this EXECUTABLE, and every Win16 program here is ntvdmhost.exe, so Sound
         * Recorder's Open dialog came up in Doom's folder (runs/s90/srp0.png). The
         * guest's DOS current directory IS this process's (INT 21h AH=47 reads it).
         */
        /* s91: an EMPTY string means the same -- Media Player passes one, and
         * comdlg32 opened at C:\ for it, so its own TONE.WAV was "not found".
         */
        if (!openFileName.lpstrInitialDir || !openFileName.lpstrInitialDir[0])
        {
            static CHAR currentDirectory[MAX_PATH];
            if (GetCurrentDirectoryA(sizeof currentDirectory, currentDirectory))
                openFileName.lpstrInitialDir = currentDirectory;
        }
        openFileName.lpstrTitle        = (LPCSTR)Wow32FarAt(frame, guest, WOWCDLG_OFN16_TITLE);
        openFileName.lpstrDefExt       = (LPCSTR)Wow32FarAt(frame, guest, WOWCDLG_OFN16_DEFEXT);

        flags = WowCdlgPeekDword(guest, WOWCDLG_OFN16_FLAGS);
        /* [CAUTION]: OFN_NOCHANGEDIR IS FORCED ON, AND IT IS NOT A PREFERENCE (Importance = 2):
         * comdlg32 changes the PROCESS current directory to wherever the user
         * browsed. That directory is Win32 state; a Win16 guest's current
         * directory is DOS-side state this host keeps for it, and the two are
         * not the same object. Letting the dialog move one and not the other
         * desynchronises them silently -- measured: after one File > Open the
         * run shows `WOW32 0xc9 GetCurrentDirectory drive=3 ->
         * "Documents and Settings\Matthew\My Documents"`, which the guest never
         * asked for and cannot have caused. Every later relative path the guest
         * resolves is then resolved against a directory it does not believe it
         * is in.
         *
         * [INFO]: The chosen file is unaffected: lpstrFile comes back FULLY QUALIFIED,
         * so nothing the caller does with the result depends on the CWD. This
         * suppresses a side effect, not an answer.
         */
        openFileName.Flags = (flags & ~WOWCDLG_OFN16_HOOKBITS) | OFN_NOCHANGEDIR;

        WowNotePut(note, noteCapacity, &noteLength, " owner=0x");
        WowNoteHex(note, noteCapacity, &noteLength, owner16, WOW_HEX_WORD_DIGITS);
        if (owner16 && !window)
            WowNotePut(note, noteCapacity, &noteLength, "(NO SUCH WINDOW -- unowned)");
        WowNotePut(note, noteCapacity, &noteLength, " flags=0x");
        WowNoteHex(note, noteCapacity, &noteLength, flags, WOW_HEX_DWORD_DIGITS);
        if (flags & WOWCDLG_OFN16_HOOKBITS)
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ HOOK/TEMPLATE BITS STRIPPED (a 16-bit"
                                       " hook procedure is not callable from"
                                       " comdlg32)");
        WowNotePut(note, noteCapacity, &noteLength, " dir=");
        WowNoteQuoted(note, noteCapacity, &noteLength, openFileName.lpstrInitialDir ? openFileName.lpstrInitialDir : "(null)");
        WowNotePut(note, noteCapacity, &noteLength, " nMaxFile=0x");
        WowNoteHex(note, noteCapacity, &noteLength, openFileName.nMaxFile, WOW_HEX_WORD_DIGITS);
        if (openFileName.lpstrFile)
        {
            WowNotePut(note, noteCapacity, &noteLength, " file=");
            WowNoteQuoted(note, noteCapacity, &noteLength, openFileName.lpstrFile);
        }

        /* [CAUTION]: nMaxFile is the GUEST'S claim about its own buffer and the only bound
         * there is -- comdlg32 writes the chosen path into it. A caller that
         * declared 0 gets a refusal rather than a dialog whose result has
         * nowhere to go.
         */
        if (!openFileName.lpstrFile || !openFileName.nMaxFile)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- ★ NO RESULT BUFFER; refused");
            Wow32SetReturn(frame, 0);
            return 1;
        }

        isOk = isSave ? GetSaveFileNameA(&openFileName) : GetOpenFileNameA(&openFileName);

        if (isOk)
        {
            /* THE ANSWER MUST BE A **SHORT (8.3)** PATH (Importance = 4):
             * The dialog hands back `C:\Documents and Settings\Matthew\My
             * Documents\test.txt`, and the caller is a 1993 program: it passes
             * that straight to `KERNEL.74 OpenFile`, whose Win16 path parser
             * cannot take components like "Documents and Settings". The open
             * fails inside krnl386, before our DOS layer -- which would have
             * opened the long path perfectly well -- is ever asked.
             *
             * [INFO]: AND THE GUEST SAID SO ITSELF, once it could speak. With
             * MessageBox implemented, Notepad puts up "Cannot open the
             * C:\Documents and Settings\Matthew\My Documents\test.txt file."
             * That sentence is the measurement; before it, two sessions of
             * this looked like "nothing happens".
             *
             * [CAUTION]: THIS IS A CONVERSION, NOT A DIFFERENT ANSWER. The short form
             * names the same file, and it is what a 16-bit program on a real
             * XP box gets, for exactly this reason.
             *
             * [CAUTION]: IF THE VOLUME HAS NO 8.3 NAMES the conversion fails, and the
             * long path is left alone rather than replaced by something
             * shorter and wrong -- the caller then fails the way it does
             * today, and the log says which case it was.
             */
            /* AND FOR A **SAVE** THE FILE DOES NOT EXIST YET (Importance = 5):
             * `GetShortPathNameA` resolves a path by looking it up, so it
             * fails outright on a name that is not on disk -- which is every
             * `GetSaveFileName`. That is not a rare corner: it is the normal
             * case for File > Save As, and it left MS Paint holding
             * `C:\Documents and Settings\Matthew\Desktop\test.BMP` -- 46
             * characters into krnl386's DOS path code. This host's own log
             * said so at the time (*"NO 8.3 NAME ... a Win16 OpenFile will
             * probably refuse it"*) and the sentence was read as a note rather
             * than as the defect it was.
             *
             * Shorten the part that DOES exist -- the directory -- and put the
             * leaf back on. The directory is what carries the long names
             * (`Documents and Settings`, `Matthew`); the leaf came out of an
             * 8.3-shaped filter in the first place.
             *
             * [CAUTION]: ONLY IF THE LEAF ITSELF FITS 8.3. A guest handed
             * `C:\DOCUME~1\MATTHE~1\Desktop\my long name.bmp` is no better
             * off, so that case falls through to the honest "left LONG" arm
             * rather than producing a path that is short in the middle and
             * impossible at the end.
             */
            CHAR shortPath[MAX_PATH];
            DWORD shortLength = GetShortPathNameA(openFileName.lpstrFile, shortPath, sizeof shortPath);
            if (!shortLength)
            {
                CHAR directory[MAX_PATH];
                INT  lastSlash = -1, position, length = 0, baseLength = 0, extensionLength = 0, isLeafShort = 1;
                while (length < (INT)sizeof directory - 1 && openFileName.lpstrFile[length])
                {
                    directory[length] = openFileName.lpstrFile[length];
                    if (directory[length] == '\\')
                        lastSlash = length;
                    ++length;
                }
                directory[length] = 0;
                if (lastSlash > 0)
                {
                    DWORD directoryLength;
                    directory[lastSlash] = 0;
                    /* the leaf must be 8.3 for this to be worth doing */
                    for (position = lastSlash + 1; openFileName.lpstrFile[position]; ++position)
                    {
                        if (openFileName.lpstrFile[position] == '.')
                        {
                            extensionLength = 0;
                            baseLength = -1;
                        }
                        else if (baseLength < 0)
                            ++extensionLength;
                        else
                            ++baseLength;
                    }
                    if (baseLength < 0)
                        baseLength = 0;
                    { INT stemLength = 0;
                    for (position = lastSlash + 1; openFileName.lpstrFile[position]
                                       && openFileName.lpstrFile[position] != '.'; ++position)
                        ++stemLength;
                      if (stemLength > WOWCDLG_DOS_NAME_MAX || extensionLength > WOWCDLG_DOS_EXTENSION_MAX)
                          isLeafShort = 0; }
                    directoryLength = isLeafShort ? GetShortPathNameA(directory, shortPath, sizeof shortPath) : 0;
                    if (directoryLength && directoryLength + 1 + (DWORD)(length - lastSlash) < sizeof shortPath)
                    {
                        DWORD cursor = directoryLength;
                        for (position = lastSlash; openFileName.lpstrFile[position]; ++position)
                            shortPath[cursor++] = openFileName.lpstrFile[position];
                        shortPath[cursor] = 0;
                        shortLength = cursor;
                        WowNotePut(note, noteCapacity, &noteLength, " [directory shortened, leaf kept"
                                                   " -- the file does not exist yet]");
                    }
                }
            }
            if (shortLength && shortLength < sizeof shortPath && shortLength + 1 <= openFileName.nMaxFile)
            {
                DWORD index, fileOffset = 0, extensionOffset = 0;
                for (index = 0; index <= shortLength; ++index)
                    openFileName.lpstrFile[index] = shortPath[index];
                for (index = 0; shortPath[index]; ++index)
                {
                    if (shortPath[index] == '\\' || shortPath[index] == ':')
                        fileOffset = index + 1;
                    if (shortPath[index] == '.')
                        extensionOffset = index + 1;
                }
                openFileName.nFileOffset    = (WORD)fileOffset;
                openFileName.nFileExtension = (WORD)(extensionOffset > fileOffset ? extensionOffset : 0);
                WowNotePut(note, noteCapacity, &noteLength, " -> SHORTENED for a Win16 caller: ");
                WowNoteQuoted(note, noteCapacity, &noteLength, shortPath);
            }
            else
            {
                WowNotePut(note, noteCapacity, &noteLength, " -- ★ NO 8.3 NAME for this path"
                                           " (or it does not fit the caller's"
                                           " buffer); left LONG, and a Win16"
                                           " OpenFile will probably refuse it");
            }
            /* Only the scalars Win32 keeps in its OWN structure need carrying
             * back; the strings were written straight into the guest's buffers.
             */
            Wow32PokeWord(guest + WOWCDLG_OFN16_FILEOFFSET,    openFileName.nFileOffset);
            Wow32PokeWord(guest + WOWCDLG_OFN16_FILEEXTENSION, openFileName.nFileExtension);
            WowCdlgPokeDword(guest, WOWCDLG_OFN16_FILTERINDEX, openFileName.nFilterIndex);
            WowCdlgPokeDword(guest, WOWCDLG_OFN16_FLAGS,
                      (openFileName.Flags & ~WOWCDLG_OFN16_HOOKBITS) | (flags & WOWCDLG_OFN16_HOOKBITS));
            WowNotePut(note, noteCapacity, &noteLength, " -> CHOSE ");
            WowNoteQuoted(note, noteCapacity, &noteLength, openFileName.lpstrFile);
        }
        else
        {
            WowNotePut(note, noteCapacity, &noteLength, " -> cancelled (or failed); the guest asks"
                                       " CommDlgExtendedError next");
        }
        Wow32SetReturn(frame, (DWORD)(isOk ? 1 : 0));
        return 1;
    }

    /* 0x1a CommDlgExtendedError() (Importance = 1):
     * Notepad calls it the instant GetOpenFileName returns 0 (it is the next
     * BOP in the run), to tell "the user pressed Cancel" from "the dialog failed".
     *
     * [INFO]: THE REAL ONE IS THE RIGHT ANSWER, and it is genuinely informative here
     * rather than a pass-through for its own sake: comdlg32 keeps this per
     * THREAD, and the thread that just ran the dialog is this one. So it
     * reports on the call we actually made. Zero means "cancelled", which is
     * what a returning-0-because-we-refused case should also say.
     */
    /* #294: 0x0b FindText / 0x0c ReplaceText(lpFR) -- THE MODELESS ONES:
     * Notepad's Search > Find (#285) and Cardfile's. Same principle as the file
     * dialog: on a real XP box this lands in comdlg32, so the OS's dialog is
     * the answer. What differs is that it is MODELESS: the call returns the
     * dialog's HWND at once, the dialog stays up, and every Find Next / Replace
     * / close is SENT to the owner as the registered message
     * "commdlg_FindReplace" with lParam -> the FINDREPLACE. So three things:
     *   1. the Win32 FINDREPLACE outlives the call -- it lives in a slot here,
     *      and its two string pointers point straight INTO the guest's own
     *      buffers (which the API requires the program to keep alive), so the
     *      text the user types is already in the program's buffer;
     *   2. the dialog gets a Win16 handle (a window record with no 16-bit
     *      procedure), because the program keeps it and passes it to
     *      IsDialogMessage in its message loop, and tests it for 0;
     *   3. the notification is relayed by WowWinProc (WowCdlgRelay) to the
     *      guest's owner window with lParam = the GUEST's own 16:16 pointer,
     *      after copying the flags Win32 set back into the guest's struct.
     * The message number needs no translation: the guest's
     * RegisterWindowMessage is answered by Win32's, so both sides hold the
     * same atom for "commdlg_FindReplace".
     *
     * [CAUTION]: The Win16 FINDREPLACE is 0x24 bytes and NOT the Win32 layout (2-byte
     * hwndOwner/hInstance) -- converted field by field, like OPENFILENAME.
     *
     * [CAUTION]: Hooks and templates are stripped, as for the file dialog: a 16-bit hook
     * is not callable from comdlg32.
     */
    case WOWCDLG_FINDTEXT:
    case WOWCDLG_REPLACETEXT:
    {
        volatile BYTE *guest = Wow32ArgPointer(frame, WOWCDLG_ARG_LPSTRUCT);
        DWORD guest16 = (DWORD)Wow32ArgWord(frame, WOWCDLG_ARG_LPSTRUCT) | ((DWORD)Wow32ArgWord(frame, WOWCDLG_ARG_LPSTRUCT + WOW_WORD_BYTES) << WORD_SHIFT);
        INT isReplace = (frame->Id == WOWCDLG_REPLACETEXT);
        PWOWCDLG_FIND slot = NULL;
        PWOWUSER_WINDOW ownerWindow, window;
        PWOWUSER_CLASS dialogClass;
        WORD owner16;
        DWORD flags;
        HWND dialog;
        INT noteLength = 0, index;
        WowNotePut(note, noteCapacity, &noteLength, isReplace ? "ReplaceText" : "FindText");
        if (!guest || WowCdlgPeekDword(guest, WOWCDLG_FR16_STRUCTSIZE) != WOWCDLG_FR16_SIZE)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- NULL or lStructSize != 0x24; refused");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        for (index = 0; index < WOWCDLG_MAX_FIND; ++index) if (!g_WowCdlgFinds[index].Dialog)
        {
            slot = &g_WowCdlgFinds[index];
            break;
        }
        owner16 = Wow32PeekWord(guest + WOWCDLG_FR16_HWNDOWNER);
        ownerWindow = owner16 ? WowUserFindWindow(owner16) : NULL;
        dialogClass  = WowUserFindClass("#32770");
        if (!slot || !ownerWindow || !ownerWindow->Window32 || !dialogClass)
        {
            WowNotePut(note, noteCapacity, &noteLength, !slot ? " -- all find slots in use"
                                          : " -- no owner window (FindText requires one)");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        if (!g_WowCdlgFindMessage)
            g_WowCdlgFindMessage = RegisterWindowMessageA(FINDMSGSTRINGA);
        for (index = 0; index < (INT)sizeof slot->FindReplace; ++index)
            ((PBYTE)&slot->FindReplace)[index] = 0;
        flags = WowCdlgPeekDword(guest, WOWCDLG_FR16_FLAGS);
        slot->FindReplace.lStructSize      = sizeof slot->FindReplace;
        slot->FindReplace.hwndOwner        = ownerWindow->Window32;
        slot->FindReplace.Flags            = flags & ~WOWCDLG_FR16_HOOKBITS;
        slot->FindReplace.lpstrFindWhat    = (LPSTR)Wow32FarAt(frame, guest, WOWCDLG_FR16_FINDWHAT);
        slot->FindReplace.lpstrReplaceWith = (LPSTR)Wow32FarAt(frame, guest, WOWCDLG_FR16_REPLACEWITH);
        slot->FindReplace.wFindWhatLen     = Wow32PeekWord(guest + WOWCDLG_FR16_FINDWHATLEN);
        slot->FindReplace.wReplaceWithLen  = Wow32PeekWord(guest + WOWCDLG_FR16_REPLACEWITHLEN);
        slot->FindReplace.lCustData        = (LPARAM)WowCdlgPeekDword(guest, WOWCDLG_FR16_CUSTDATA);
        if (!slot->FindReplace.lpstrFindWhat || !slot->FindReplace.wFindWhatLen
            || (isReplace && (!slot->FindReplace.lpstrReplaceWith || !slot->FindReplace.wReplaceWithLen)))
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- no string buffer; refused");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        window = WowUserNewWindow();
        if (!window) { WowNotePut(note, noteCapacity, &noteLength, " -- OUT OF WINDOW SLOTS");
                  Wow32SetReturn(frame, 0);
                  return 1; }
        dialog = isReplace ? ReplaceTextA(&slot->FindReplace) : FindTextA(&slot->FindReplace);
        if (!dialog)
        {
            window->Window16 = 0;                           /* give the slot back */
            WowNotePut(note, noteCapacity, &noteLength, " -- comdlg32 refused (err 0x");
            WowNoteHex(note, noteCapacity, &noteLength, CommDlgExtendedError(), WOW_HEX_DWORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, ")");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        window->Class = (WORD)(dialogClass - g_WowUserClasses);
        window->Style = (DWORD)GetWindowLongA(dialog, GWL_STYLE);
        window->WindowProcedure = 0;
        window->Parent = owner16;
        window->Menu = 0;
        window->Instance = 0;
        window->Text[0] = 0;
        window->Memory16 = 0;
        window->MenuItems = 0;
        window->IsDying = 0;
        for (index = 0; index < WOWUSER_MAX_EXTRA; ++index)
            window->Extra[index] = 0;
        window->Window32 = dialog;
        slot->Dialog = dialog;
        slot->Guest = guest;
        slot->Guest16 = guest16;
        slot->Owner16 = owner16;
        slot->Window16 = window->Window16;
        WowNotePut(note, noteCapacity, &noteLength, " flags=0x");
        WowNoteHex(note, noteCapacity, &noteLength, flags, WOW_HEX_DWORD_DIGITS);
        if (flags & WOWCDLG_FR16_HOOKBITS)
            WowNotePut(note, noteCapacity, &noteLength, " (hook/template bits STRIPPED)");
        WowNotePut(note, noteCapacity, &noteLength, " what=");
        WowNoteQuoted(note, noteCapacity, &noteLength, slot->FindReplace.lpstrFindWhat);
        WowNotePut(note, noteCapacity, &noteLength, " -> MODELESS dialog hwnd16=0x");
        WowNoteHex(note, noteCapacity, &noteLength, window->Window16, WOW_HEX_WORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, ", notifications as msg 0x");
        WowNoteHex(note, noteCapacity, &noteLength, g_WowCdlgFindMessage, WOW_HEX_WORD_DIGITS);
        Wow32SetReturn(frame, window->Window16);
        return 1;
    }

    /* -- #294: 0x05 ChooseColor(lpCC) -- modal, the OS's dialog. Win16
     * CHOOSECOLOR is 0x20 bytes; lpCustColors points at the guest's own 16
     * COLORREFs, which are the same bytes in both worlds, so comdlg32 reads
     * and updates them in place. rgbResult and Flags are carried back.
     */
    case WOWCDLG_CHOOSECOLOR:
    {
        volatile BYTE *guest = Wow32ArgPointer(frame, WOWCDLG_ARG_LPSTRUCT);
        CHOOSECOLORA chooseColor;
        PWOWUSER_WINDOW ownerWindow;
        DWORD flags;
        INT noteLength = 0, isOk, index;
        WowNotePut(note, noteCapacity, &noteLength, "ChooseColor");
        if (!guest || WowCdlgPeekDword(guest, WOWCDLG_CC16_STRUCTSIZE) != WOWCDLG_CC16_SIZE)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- NULL or lStructSize != 0x20; refused");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        for (index = 0; index < (INT)sizeof chooseColor; ++index)
            ((PBYTE)&chooseColor)[index] = 0;
        ownerWindow = WowUserFindWindow(Wow32PeekWord(guest + WOWCDLG_CC16_HWNDOWNER));
        flags = WowCdlgPeekDword(guest, WOWCDLG_CC16_FLAGS);
        chooseColor.lStructSize  = sizeof chooseColor;
        chooseColor.hwndOwner    = ownerWindow ? ownerWindow->Window32 : NULL;
        chooseColor.rgbResult    = WowCdlgPeekDword(guest, WOWCDLG_CC16_RGBRESULT);
        chooseColor.lpCustColors = (COLORREF *)Wow32FarAt(frame, guest, WOWCDLG_CC16_CUSTCOLORS);
        chooseColor.Flags        = flags & ~WOWCDLG_CC16_HOOKBITS;
        chooseColor.lCustData    = (LPARAM)WowCdlgPeekDword(guest, WOWCDLG_CC16_CUSTDATA);
        if (!chooseColor.lpCustColors)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- no lpCustColors (required); refused");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        isOk = ChooseColorA(&chooseColor);
        if (isOk)
        {
            WowCdlgPokeDword(guest, WOWCDLG_CC16_RGBRESULT, chooseColor.rgbResult);
            WowCdlgPokeDword(guest, WOWCDLG_CC16_FLAGS, (chooseColor.Flags & ~WOWCDLG_CC16_HOOKBITS) | (flags & WOWCDLG_CC16_HOOKBITS));
        }
        WowNotePut(note, noteCapacity, &noteLength, isOk ? " -> chose 0x" : " -> cancelled; rgb 0x");
        WowNoteHex(note, noteCapacity, &noteLength, chooseColor.rgbResult, WOW_HEX_DWORD_DIGITS);
        Wow32SetReturn(frame, (DWORD)(isOk ? 1 : 0));
        return 1;
    }

    /* -- #294: 0x0f ChooseFont(lpCF) -- modal, the OS's dialog. Win16
     * CHOOSEFONT is 0x2e bytes and its LOGFONT is the 16-bit one (five INT16s,
     * eight BYTEs, a 32-byte face: 50 bytes), so the font is converted both
     * ways rather than pointed at.
     *
     * [CAUTION]: PRINTER FONTS NEED A PRINTER DC, and hDC here is a Win16 GDI token for a
     * DC this host may not have; the flag is narrowed to screen fonts and the
     * line says so. On a machine with no printer (the rig) stock's comdlg32
     * would show screen fonts only anyway.
     *
     * [CAUTION]: CF_USESTYLE's lpszStyle is a guest buffer comdlg32 writes into directly.
     */
    case WOWCDLG_CHOOSEFONT:
    {
        volatile BYTE *guest = Wow32ArgPointer(frame, WOWCDLG_ARG_LPSTRUCT);
        volatile BYTE *logFont16;
        CHOOSEFONTA chooseFont;
        LOGFONTA logFont;
        PWOWUSER_WINDOW ownerWindow;
        DWORD flags;
        INT noteLength = 0, isOk, index;
        WowNotePut(note, noteCapacity, &noteLength, "ChooseFont");
        if (!guest || WowCdlgPeekDword(guest, WOWCDLG_CF16_STRUCTSIZE) != WOWCDLG_CF16_SIZE)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- NULL or lStructSize != 0x2e; refused");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        logFont16 = Wow32FarAt(frame, guest, WOWCDLG_CF16_LOGFONT);
        if (!logFont16)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- no lpLogFont (required); refused");
            Wow32SetReturn(frame, 0);
            return 1;
        }
        for (index = 0; index < (INT)sizeof chooseFont; ++index)
            ((PBYTE)&chooseFont)[index] = 0;
        for (index = 0; index < (INT)sizeof logFont; ++index)
            ((PBYTE)&logFont)[index] = 0;
        WowCdlgLogFont16To32(logFont16, &logFont);
        ownerWindow = WowUserFindWindow(Wow32PeekWord(guest + WOWCDLG_CF16_HWNDOWNER));
        flags = WowCdlgPeekDword(guest, WOWCDLG_CF16_FLAGS);
        chooseFont.lStructSize = sizeof chooseFont;
        chooseFont.hwndOwner   = ownerWindow ? ownerWindow->Window32 : NULL;
        chooseFont.lpLogFont   = &logFont;
        chooseFont.iPointSize  = (INT)(SHORT)Wow32PeekWord(guest + WOWCDLG_CF16_POINTSIZE);
        chooseFont.Flags       = flags & ~WOWCDLG_CF16_HOOKBITS;
        if (chooseFont.Flags & CF_PRINTERFONTS)
        {
            chooseFont.Flags = (chooseFont.Flags & ~CF_PRINTERFONTS) | CF_SCREENFONTS;
            WowNotePut(note, noteCapacity, &noteLength, " (printer fonts -> screen fonts: no printer DC)");
        }
        chooseFont.rgbColors   = WowCdlgPeekDword(guest, WOWCDLG_CF16_RGBCOLORS);
        chooseFont.lCustData   = (LPARAM)WowCdlgPeekDword(guest, WOWCDLG_CF16_CUSTDATA);
        chooseFont.lpszStyle   = (LPSTR)Wow32FarAt(frame, guest, WOWCDLG_CF16_STYLE);
        if (!chooseFont.lpszStyle)
            chooseFont.Flags &= ~CF_USESTYLE;
        chooseFont.nSizeMin    = (INT)(SHORT)Wow32PeekWord(guest + WOWCDLG_CF16_SIZEMIN);
        chooseFont.nSizeMax    = (INT)(SHORT)Wow32PeekWord(guest + WOWCDLG_CF16_SIZEMAX);
        WowNotePut(note, noteCapacity, &noteLength, " flags=0x");
        WowNoteHex(note, noteCapacity, &noteLength, flags, WOW_HEX_DWORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, " face=");
        WowNoteQuoted(note, noteCapacity, &noteLength, logFont.lfFaceName);
        isOk = ChooseFontA(&chooseFont);
        if (isOk)
        {
            WowCdlgLogFont32To16(&logFont, logFont16);
            Wow32PokeWord(guest + WOWCDLG_CF16_POINTSIZE, (WORD)chooseFont.iPointSize);
            WowCdlgPokeDword(guest, WOWCDLG_CF16_RGBCOLORS, chooseFont.rgbColors);
            Wow32PokeWord(guest + WOWCDLG_CF16_FONTTYPE, (WORD)chooseFont.nFontType);
            WowCdlgPokeDword(guest, WOWCDLG_CF16_FLAGS, (chooseFont.Flags & ~WOWCDLG_CF16_HOOKBITS) | (flags & WOWCDLG_CF16_HOOKBITS));
            WowNotePut(note, noteCapacity, &noteLength, " -> chose ");
            WowNoteQuoted(note, noteCapacity, &noteLength, logFont.lfFaceName);
            WowNotePut(note, noteCapacity, &noteLength, " pt10=0x");
            WowNoteHex(note, noteCapacity, &noteLength, (DWORD)chooseFont.iPointSize, WOW_HEX_WORD_DIGITS);
        }
        else
        {
            WowNotePut(note, noteCapacity, &noteLength, " -> cancelled (or failed)");
        }
        Wow32SetReturn(frame, (DWORD)(isOk ? 1 : 0));
        return 1;
    }

    /* -- #294 (s91): 0x14 PrintDlg(lpPD) -- modal, the OS's dialog, as WOW does.
     * Converted field by field (2-byte handles in the 16-bit struct). What comes
     * back: the page range, nCopies, Flags, and -- for PD_RETURNDC/PD_RETURNIC --
     * the printer DC as one of our GDI tokens (Win32 draws on it).
     *
     * [CAUTION]: hDevMode/hDevNames ARE NOT CARRIED, either way. They are Win16 GLOBAL
     * handles: krnl386 owns that heap, so reading the guest's needs a GlobalLock
     * call into it and returning new ones a GlobalAlloc chain (the clipboard's
     * shape, wowcall.h). Not built: comdlg32 is given NULL (the default printer)
     * and the guest's two words are left as it set them. Win32's are freed. The
     * line says so whenever the guest offered one.
     *
     * [INFO]: w_cdlg vs stock: a wrong size -> 0 + CDERR_STRUCTSIZE; PD_RETURNDEFAULT on
     * a box with no printer -> 0 + PDERR_NODEFAULTPRN, with and without RETURNIC.
     */
    case WOWCDLG_PRINTDLG:
    {
        volatile BYTE *guest = Wow32ArgPointer(frame, WOWCDLG_ARG_LPSTRUCT);
        PRINTDLGA printDialog;
        PWOWUSER_WINDOW ownerWindow;
        DWORD flags;
        WORD devMode16, devNames16;
        INT noteLength = 0, isOk, index;
        WowNotePut(note, noteCapacity, &noteLength, "PrintDlg");
        if (!guest || WowCdlgPeekDword(guest, WOWCDLG_PD16_STRUCTSIZE) != WOWCDLG_PD16_SIZE)
        {
            WowNotePut(note, noteCapacity, &noteLength, " -- NULL or lStructSize != 0x34; refused,"
                                       " CDERR_STRUCTSIZE");
            g_WowCdlgError = CDERR_STRUCTSIZE;
            Wow32SetReturn(frame, 0);
            return 1;
        }
        for (index = 0; index < (INT)sizeof printDialog; ++index)
            ((PBYTE)&printDialog)[index] = 0;
        ownerWindow = WowUserFindWindow(Wow32PeekWord(guest + WOWCDLG_PD16_HWNDOWNER));
        flags = WowCdlgPeekDword(guest, WOWCDLG_PD16_FLAGS);
        devMode16 = Wow32PeekWord(guest + WOWCDLG_PD16_HDEVMODE);
        devNames16 = Wow32PeekWord(guest + WOWCDLG_PD16_HDEVNAMES);
        printDialog.lStructSize = sizeof printDialog;
        printDialog.hwndOwner   = ownerWindow ? ownerWindow->Window32 : NULL;
        printDialog.Flags       = flags & ~WOWCDLG_PD16_HOOKBITS;
        printDialog.nFromPage   = Wow32PeekWord(guest + WOWCDLG_PD16_FROMPAGE);
        printDialog.nToPage     = Wow32PeekWord(guest + WOWCDLG_PD16_TOPAGE);
        printDialog.nMinPage    = Wow32PeekWord(guest + WOWCDLG_PD16_MINPAGE);
        printDialog.nMaxPage    = Wow32PeekWord(guest + WOWCDLG_PD16_MAXPAGE);
        printDialog.nCopies     = Wow32PeekWord(guest + WOWCDLG_PD16_COPIES);
        printDialog.lCustData   = (LPARAM)WowCdlgPeekDword(guest, WOWCDLG_PD16_CUSTDATA);
        WowNotePut(note, noteCapacity, &noteLength, " flags=0x");
        WowNoteHex(note, noteCapacity, &noteLength, flags, WOW_HEX_DWORD_DIGITS);
        if (flags & WOWCDLG_PD16_HOOKBITS)
            WowNotePut(note, noteCapacity, &noteLength, " (hook/template bits STRIPPED)");
        if (devMode16 | devNames16)
            WowNotePut(note, noteCapacity, &noteLength, " -- ⚠ the guest's hDevMode/hDevNames are NOT"
                                       " read (Win16 global handles); default printer");
        isOk = PrintDlgA(&printDialog);
        if (printDialog.hDevMode)
            GlobalFree(printDialog.hDevMode);
        if (printDialog.hDevNames)
            GlobalFree(printDialog.hDevNames);
        if (isOk)
        {
            WORD token = 0;
            if (printDialog.hDC)
            {
                token = WowGdiH16((HGDIOBJ)printDialog.hDC, WOWGDI_KIND_DC);
                if (!token)
                    DeleteDC(printDialog.hDC);
            }
            Wow32PokeWord(guest + WOWCDLG_PD16_HDC,      token);
            Wow32PokeWord(guest + WOWCDLG_PD16_FROMPAGE, printDialog.nFromPage);
            Wow32PokeWord(guest + WOWCDLG_PD16_TOPAGE,   printDialog.nToPage);
            Wow32PokeWord(guest + WOWCDLG_PD16_COPIES,   printDialog.nCopies);
            WowCdlgPokeDword(guest, WOWCDLG_PD16_FLAGS, (printDialog.Flags & ~WOWCDLG_PD16_HOOKBITS) | (flags & WOWCDLG_PD16_HOOKBITS));
            WowNotePut(note, noteCapacity, &noteLength, " -> OK hDC token 0x");
            WowNoteHex(note, noteCapacity, &noteLength, token, WOW_HEX_WORD_DIGITS);
            WowNotePut(note, noteCapacity, &noteLength, " copies=");
            WowNoteHex(note, noteCapacity, &noteLength, printDialog.nCopies, WOW_HEX_WORD_DIGITS);
        }
        else
        {
            WowNotePut(note, noteCapacity, &noteLength, " -> 0, CommDlgExtendedError 0x");
            WowNoteHex(note, noteCapacity, &noteLength, CommDlgExtendedError(), WOW_HEX_DWORD_DIGITS);
        }
        Wow32SetReturn(frame, (DWORD)(isOk ? 1 : 0));
        return 1;
    }

    case WOWCDLG_EXTENDEDERROR:
    {
        DWORD error = g_WowCdlgError ? g_WowCdlgError : CommDlgExtendedError();
        INT noteLength = 0;
        WowNotePut(note, noteCapacity, &noteLength, "CommDlgExtendedError -> 0x");
        WowNoteHex(note, noteCapacity, &noteLength, error, WOW_HEX_DWORD_DIGITS);
        WowNotePut(note, noteCapacity, &noteLength, error ? " (the dialog FAILED)"
                                     : " (0 = the user cancelled)");
        Wow32SetReturn(frame, error);
        return 1;
    }

    default:
        return 0;
    }
}
