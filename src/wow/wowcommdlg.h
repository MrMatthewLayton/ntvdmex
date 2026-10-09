/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * COMMDLG.DLL's OWN ID SPACE -- File > Open.  GH #128, s44.
 *
 * WHY THIS IS SMALL, AND WHY THAT WAS A SURPRISE:
 * The plan was to implement `DialogBox`: turn a Win16 DIALOG template into a real
 * window and run the guest's dialog procedure. That turned out to be the wrong
 * plan, and the run said so before a line of it was written:
 *
 * * Notepad's File > Open does not go through USER's DialogBox at all. Its
 *   import table (`tools/ne/neimports.py`) names the call outright:
 *   `COMMDLG.1 GETOPENFILENAME`.
 *
 * File > Open is ONE call, and the run confirms it: driving Alt-F-O on the live
 * guest produced exactly two unimplemented BOPs, both from a module this host had
 * never seen -- `id 0x01, 4 args, retstub 0x0012` and `id 0x1a, 0 args, retstub
 * 0x0090` -- with the stub segment COMMDLG's.
 *
 * THE IDS ARE THE EXPORT ORDINALS, CONFIRMED SEVEN TIMES (Importance = 2):
 * COMMDLG's non-resident name table against the ids its calls arrive with:
 *    1 GETOPENFILENAME -> 0x01     15 CHOOSEFONT   -> 0x0f
 *    2 GETSAVEFILENAME -> 0x02     20 PRINTDLG     -> 0x14
 *    5 CHOOSECOLOR     -> 0x05     26 COMMDLGEXTENDEDERROR -> 0x1a
 *   11 FINDTEXT        -> 0x0b     12 REPLACETEXT  -> 0x0c
 * Seven independent agreements is a reading, not a coincidence -- and it is the
 * same shape SHELL.DLL turned out to have. [CAUTION] It is NOT a rule: krnl386's ids are
 * nothing like its ordinals. Each module is checked on its own.
 *
 * THE Win16 OPENFILENAME, 0x48 BYTES, AS NOTEPAD PASSES IT (Importance = 3):
 * Not from a header -- the guest declares its own size and fills its own fields,
 * and the structure that arrives (dumped through the far-pointer argument) has
 * every filled field on a field boundary of the layout below:
 *
 * +0x00 0x0048                     lStructSize, from the guest
 * +0x08 / +0x0c / +0x28 / +0x2c / +0x38   far pointers into Notepad's DS
 * +0x30 0x00001004
 *
 * Five far pointers at +0x08/+0x0c/+0x28/+0x2c/+0x38 and a DWORD 0x00001004 at
 * +0x30 (OFN_FILEMUSTEXIST | OFN_HIDEREADONLY, exactly what File > Open wants).
 * A wrong layout does not put five far pointers on five pointer fields and a
 * plausible flag word on the flag field.
 *
 * [CAUTION]: AND IT IS NOT THE Win32 LAYOUT. `hwndOwner` and `hInstance` are **2 bytes
 * each** here and 4 each in Win32, so every field after +0x08 is at a different
 * offset in the two structures. They are converted field by field below; there
 * is no memcpy that could ever be right.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_WOWCOMMDLG_H
#define NTVDMEX_WOWCOMMDLG_H

#define WOWCDLG_GETOPENFILENAME         0x0001
#define WOWCDLG_GETSAVEFILENAME         0x0002
#define WOWCDLG_EXTENDEDERROR           0x001a

/* One far pointer, 4 argument bytes -- what the stub declares. */
#define WOWCDLG_OPENFILENAME_ARG_LPOFN  0

/* ...and the one far pointer each of the others takes. */
#define WOWCDLG_ARG_LPSTRUCT            0

#define WOWCDLG_OFN16_SIZE              0x48
#define WOWCDLG_OFN16_STRUCTSIZE        0x00
#define WOWCDLG_OFN16_HWNDOWNER         0x04
#define WOWCDLG_OFN16_HINSTANCE         0x06
#define WOWCDLG_OFN16_FILTER            0x08
#define WOWCDLG_OFN16_CUSTFILTER        0x0c
#define WOWCDLG_OFN16_MAXCUSTFILTER     0x10
#define WOWCDLG_OFN16_FILTERINDEX       0x14
#define WOWCDLG_OFN16_FILE              0x18
#define WOWCDLG_OFN16_MAXFILE           0x1c
#define WOWCDLG_OFN16_FILETITLE         0x20
#define WOWCDLG_OFN16_MAXFILETITLE      0x24
#define WOWCDLG_OFN16_INITIALDIR        0x28
#define WOWCDLG_OFN16_TITLE             0x2c
#define WOWCDLG_OFN16_FLAGS             0x30
#define WOWCDLG_OFN16_FILEOFFSET        0x34
#define WOWCDLG_OFN16_FILEEXTENSION     0x36
#define WOWCDLG_OFN16_DEFEXT            0x38
#define WOWCDLG_OFN16_CUSTDATA          0x3c
#define WOWCDLG_OFN16_HOOK              0x40
#define WOWCDLG_OFN16_TEMPLATENAME      0x44

/* [CAUTION]: THE HOOK AND TEMPLATE BITS ARE REFUSED, NOT HONOURED. Either one asks the
 * 32-bit side to call back into 16-bit code, or to build a dialog from the
 * application's own template, and neither is built. Passing them to Win32
 * unchanged would hand comdlg32 a 16:16 function pointer it would call as a
 * flat one. Notepad sets neither (its Flags are 0x1004), so this strips
 * something nothing has asked for -- and says so on the line if it ever does.
 */
#define WOWCDLG_OFN16_HOOKBITS          (0x00000020UL | 0x00000040UL | 0x00002000UL)

/* ENABLEHOOK | ENABLETEMPLATE | ENABLETEMPLATEHANDLE */

/* An 8.3 leaf: the most the base name and extension may hold. */
#define WOWCDLG_DOS_NAME_MAX            8
#define WOWCDLG_DOS_EXTENSION_MAX       3

/* -- #294: the rest of COMMDLG's table. Ids = export ordinals (see the top). */
#define WOWCDLG_CHOOSECOLOR             0x0005
#define WOWCDLG_FINDTEXT                0x000b
#define WOWCDLG_REPLACETEXT             0x000c
#define WOWCDLG_CHOOSEFONT              0x000f

/* Win16 FINDREPLACE, 0x24 bytes (3.1 SDK; Wine's FINDREPLACE16 agrees). */
#define WOWCDLG_FR16_SIZE               0x24
#define WOWCDLG_FR16_STRUCTSIZE         0x00
#define WOWCDLG_FR16_HWNDOWNER          0x04
#define WOWCDLG_FR16_FLAGS              0x08
#define WOWCDLG_FR16_FINDWHAT           0x0c
#define WOWCDLG_FR16_REPLACEWITH        0x10
#define WOWCDLG_FR16_FINDWHATLEN        0x14
#define WOWCDLG_FR16_REPLACEWITHLEN     0x16
#define WOWCDLG_FR16_CUSTDATA           0x18
#define WOWCDLG_FR16_HOOKBITS           (0x00000100UL | 0x00000200UL | 0x00002000UL)

/* FR_ENABLEHOOK | FR_ENABLETEMPLATE | FR_ENABLETEMPLATEHANDLE */

/* Win16 CHOOSECOLOR, 0x20 bytes. */
#define WOWCDLG_CC16_SIZE               0x20
#define WOWCDLG_CC16_STRUCTSIZE         0x00
#define WOWCDLG_CC16_HWNDOWNER          0x04
#define WOWCDLG_CC16_RGBRESULT          0x08
#define WOWCDLG_CC16_CUSTCOLORS         0x0c
#define WOWCDLG_CC16_FLAGS              0x10
#define WOWCDLG_CC16_CUSTDATA           0x14
#define WOWCDLG_CC16_HOOKBITS           (0x00000010UL | 0x00000020UL | 0x00000040UL)

/* CC_ENABLEHOOK | CC_ENABLETEMPLATE | CC_ENABLETEMPLATEHANDLE */

/* Win16 CHOOSEFONT, 0x2e bytes. */
#define WOWCDLG_CF16_SIZE               0x2e
#define WOWCDLG_CF16_STRUCTSIZE         0x00
#define WOWCDLG_CF16_HWNDOWNER          0x04
#define WOWCDLG_CF16_HDC                0x06
#define WOWCDLG_CF16_LOGFONT            0x08
#define WOWCDLG_CF16_POINTSIZE          0x0c
#define WOWCDLG_CF16_FLAGS              0x0e
#define WOWCDLG_CF16_RGBCOLORS          0x12
#define WOWCDLG_CF16_CUSTDATA           0x16
#define WOWCDLG_CF16_HOOK               0x1a
#define WOWCDLG_CF16_TEMPLATENAME       0x1e
#define WOWCDLG_CF16_HINSTANCE          0x22
#define WOWCDLG_CF16_STYLE              0x24
#define WOWCDLG_CF16_FONTTYPE           0x28
#define WOWCDLG_CF16_SIZEMIN            0x2a
#define WOWCDLG_CF16_SIZEMAX            0x2c
#define WOWCDLG_CF16_HOOKBITS           (0x00000008UL | 0x00000010UL | 0x00000020UL)

/* CF_ENABLEHOOK | CF_ENABLETEMPLATE | CF_ENABLETEMPLATEHANDLE */

/* Win16 PRINTDLG, 0x34 bytes (3.1 SDK; Wine's PRINTDLG16 agrees). */
#define WOWCDLG_PRINTDLG                0x0014
#define WOWCDLG_PD16_SIZE               0x34
#define WOWCDLG_PD16_STRUCTSIZE         0x00
#define WOWCDLG_PD16_HWNDOWNER          0x04
#define WOWCDLG_PD16_HDEVMODE           0x06
#define WOWCDLG_PD16_HDEVNAMES          0x08
#define WOWCDLG_PD16_HDC                0x0a
#define WOWCDLG_PD16_FLAGS              0x0c
#define WOWCDLG_PD16_FROMPAGE           0x10
#define WOWCDLG_PD16_TOPAGE             0x12
#define WOWCDLG_PD16_MINPAGE            0x14
#define WOWCDLG_PD16_MAXPAGE            0x16
#define WOWCDLG_PD16_COPIES             0x18
#define WOWCDLG_PD16_CUSTDATA           0x1c
#define WOWCDLG_PD16_HOOKBITS  (0x00001000UL | 0x00002000UL | 0x00004000UL | 0x00008000UL \
                        | 0x00010000UL | 0x00020000UL)
/* PD_ENABLEPRINTHOOK | PD_ENABLESETUPHOOK | PD_ENABLE{PRINT,SETUP}TEMPLATE[HANDLE] */

/* Win16 LOGFONT: five INT16s, eight BYTEs, a 32-byte face -- 50 bytes. */
#define WOWCDLG_LF16_HEIGHT             0
#define WOWCDLG_LF16_WIDTH              2
#define WOWCDLG_LF16_ESCAPEMENT         4
#define WOWCDLG_LF16_ORIENTATION        6
#define WOWCDLG_LF16_WEIGHT             8
#define WOWCDLG_LF16_ITALIC             10
#define WOWCDLG_LF16_UNDERLINE          11
#define WOWCDLG_LF16_STRIKEOUT          12
#define WOWCDLG_LF16_CHARSET            13
#define WOWCDLG_LF16_OUTPRECISION       14
#define WOWCDLG_LF16_CLIPPRECISION      15
#define WOWCDLG_LF16_QUALITY            16
#define WOWCDLG_LF16_PITCHANDFAMILY     17
#define WOWCDLG_LF16_FACENAME           18
#define WOWCDLG_LF16_FACESIZE           32

/* One open Find/Replace dialog: the Win32 FINDREPLACE comdlg32 keeps a pointer
 * to for the dialog's whole life, and how to reach the guest's copy.
 */
#define WOWCDLG_MAX_FIND                4
typedef struct _WOWCDLG_FIND
{
    HWND           Dialog;       /* NULL = free */
    FINDREPLACEA   FindReplace;
    volatile BYTE *Guest;        /* the guest's FINDREPLACE, host linear */
    DWORD          Guest16;      /* ...and as the guest's own 16:16 pointer */
    WORD           Owner16, Window16;
} WOWCDLG_FIND, *PWOWCDLG_FIND;

/* Defined in wowcommdlg.c (#335). */
INT WowCdlgRelay(UINT message, LPARAM lParam);
INT WowCdlgIsDialogMessage(PMSG message);
INT WowCommdlgCall(PWOW32_FRAME frame, PSTR note, INT noteCapacity);
#endif /* NTVDMEX_WOWCOMMDLG_H */
