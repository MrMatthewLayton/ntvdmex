/* bios_prtsc.h -- the BIOS's default INT 05h (Print Screen), as a byte sequencer. #274.
 *
 * The guest half is bios_kbdact.asm's `p5`: a loop that asks the host (BOP 09h) for the
 * next byte and prints it through INT 17h, so a hooked INT 17h sees every byte. This is
 * the host half: WHICH byte comes next, and whether the printer's answer ends the job.
 * Pure, so tests/unit/prtsc_test.c can run it; the host supplies the screen reader.
 *
 * ── THE CONTRACT, AND WHERE IT COMES FROM. ─────────────────────────────────────────────
 *   IBM PC/AT Technical Reference, BIOS listing PRINT_SCREEN (and RBIL INT 05h,
 *   MEMORY.LST 0050:0000):
 *     • 0050:0000 = 01h while printing; a second INT 05h meanwhile returns at once.
 *       00h when the screen has gone out, FFh if the printer reported an error.
 *     • Columns and the active page from INT 10h AH=0Fh; rows = 25 on the AT -- the
 *       EGA/VGA-era routine takes 0040:0084 + 1, which is what we use (0 -> 25).
 *     • First the "initial" line feed / carriage return, then every cell of every row,
 *       read with INT 10h AH=08h at that position (a NUL cell prints as a space), each
 *       row followed by line feed / carriage return. The cursor is saved first (AH=03h)
 *       and put back at the end, error or not.
 *     • After each SCREEN byte, INT 17h's AH is tested against 29h (bit 0 time-out,
 *       bit 3 I/O error, bit 5 out of paper); any of them ends the job with FFh. The
 *       line-end bytes are not tested.
 *   ⚠ UNMEASURED, FROM THE LISTING AS RECALLED: the line-end order is LF then CR
 *     ("WILL NOW SEND INITIAL LF,CR TO PRINTER"), and only screen bytes are tested.
 *     p_kbd3 records the first bytes INT 17h receives so PCem's AMI BIOS settles it.
 *   ⛔ OURS KEEPS THAT STATUS HOST-SIDE (main.c g_prtsc_status), NOT AT 0050:0000:
 *     that byte is DOS_HDLR_SEG:0000, the first byte of our INT 21h stub. See prtsc_bop.
 *   ⚠ OURS READS THE SCREEN THROUGH OUR OWN INT 10h, HOST-SIDE, not through the guest's
 *     INT 10h vector: a program that hooked INT 10h would see the IBM routine's 2000
 *     AH=02h/08h calls and does not see ours. INT 17h -- the half a program actually
 *     hooks to capture a print-out -- is the guest's.
 */
#ifndef NTVDMEX_DOS_BIOS_PRTSC_H
#define NTVDMEX_DOS_BIOS_PRTSC_H

#include "../ntvdmex_types.h"

/* The status byte at 0050:0000 and its values (kept host-side here: see above). */
#define BIOS_PRINT_SCREEN_STATUS_LINEAR  0x500u    /* 0050:0000 */
#define BIOS_PRINT_SCREEN_STATUS_BUSY    0x01
#define BIOS_PRINT_SCREEN_STATUS_OK      0x00
#define BIOS_PRINT_SCREEN_STATUS_ERROR   0xFF
/* INT 17h AH: time-out | I/O error | out of paper. */
#define BIOS_PRINT_SCREEN_PRINTER_ERROR_MASK 0x29

/* Rows when 0040:0084 is 0 (never set). */
#define BIOS_PRINT_SCREEN_DEFAULT_ROWS   25

/* The bytes the job sends besides the screen cells. */
#define BIOS_PRINT_SCREEN_BLANK_CELL      ' '      /* what a NUL cell prints as */

/* What BiosPrintScreenStep returns. */
enum {
    BIOS_PRINT_SCREEN_STEP_EMIT = 0,
    BIOS_PRINT_SCREEN_STEP_DONE = 1,
    BIOS_PRINT_SCREEN_STEP_ERROR = 2
};

/* Where the job is: the initial line end, the cells, each row's line end, the end. */
enum {
    BIOS_PRINT_SCREEN_PHASE_INITIAL_LF,
    BIOS_PRINT_SCREEN_PHASE_INITIAL_CR,
    BIOS_PRINT_SCREEN_PHASE_CELL,
    BIOS_PRINT_SCREEN_PHASE_LF,
    BIOS_PRINT_SCREEN_PHASE_CR,
    BIOS_PRINT_SCREEN_PHASE_END
};

typedef struct _BIOS_PRINT_SCREEN_JOB {
    BYTE  IsActive;
    BYTE  Columns, Rows, Page;
    BYTE  Row, Column, Phase;
    BYTE  ShouldTestStatus;       /* the last byte out was a screen cell: test its AH */
    WORD  Cursor;                 /* AH=03h's DX, put back at the end                 */
} BIOS_PRINT_SCREEN_JOB, *PBIOS_PRINT_SCREEN_JOB;

typedef const BIOS_PRINT_SCREEN_JOB *PCBIOS_PRINT_SCREEN_JOB;

/* Returns the character at (row, column) of the active page. */
typedef BYTE (*PBIOS_PRINT_SCREEN_READ_CELL)(PVOID context, BYTE row, BYTE column);

/* `bdaRowsMinusOne` is 0040:0084 (rows - 1; 0 on a BIOS that never set it). */
static inline VOID BiosPrintScreenBegin(_Out_ PBIOS_PRINT_SCREEN_JOB job, _In_ BYTE columns,
                                        _In_ BYTE bdaRowsMinusOne, _In_ BYTE page,
                                        _In_ WORD cursor)
{
    job->IsActive = TRUE;
    job->Columns = columns;
    job->Rows = (BYTE)(bdaRowsMinusOne ? bdaRowsMinusOne + 1 : BIOS_PRINT_SCREEN_DEFAULT_ROWS);
    job->Page = page;
    job->Row = job->Column = 0; job->Phase = BIOS_PRINT_SCREEN_PHASE_INITIAL_LF;
    job->ShouldTestStatus = FALSE; job->Cursor = cursor;
}

/* The next byte for INT 17h. `printerStatus` is the AH INT 17h answered for the PREVIOUS
   byte (ignored for the first one and after a line-end byte). BIOS_PRINT_SCREEN_STEP_EMIT
   -> *nextByte is the byte; BIOS_PRINT_SCREEN_STEP_DONE / BIOS_PRINT_SCREEN_STEP_ERROR ->
   the job is over and `IsActive` is clear. `readCell` returns the character at
   (row, column) of the active page. */
static inline INT BiosPrintScreenStep(_Inout_ PBIOS_PRINT_SCREEN_JOB job,
                                      _In_ BYTE printerStatus,
                                      _In_ PBIOS_PRINT_SCREEN_READ_CELL readCell,
                                      _In_opt_ PVOID context, _Out_ PBYTE nextByte)
{
    BYTE character;
    if (!job->IsActive) return BIOS_PRINT_SCREEN_STEP_DONE;
    if (job->ShouldTestStatus && (printerStatus & BIOS_PRINT_SCREEN_PRINTER_ERROR_MASK)) {
        job->IsActive = FALSE; return BIOS_PRINT_SCREEN_STEP_ERROR;
    }
    job->ShouldTestStatus = FALSE;
    switch (job->Phase) {
    case BIOS_PRINT_SCREEN_PHASE_INITIAL_LF:
        *nextByte = ASCII_LF; job->Phase = BIOS_PRINT_SCREEN_PHASE_INITIAL_CR;
        return BIOS_PRINT_SCREEN_STEP_EMIT;
    case BIOS_PRINT_SCREEN_PHASE_INITIAL_CR:
        *nextByte = ASCII_CR;
        job->Phase = (job->Rows && job->Columns) ? BIOS_PRINT_SCREEN_PHASE_CELL
                                                 : BIOS_PRINT_SCREEN_PHASE_END;
        return BIOS_PRINT_SCREEN_STEP_EMIT;
    case BIOS_PRINT_SCREEN_PHASE_CELL:
        character = readCell(context, job->Row, job->Column);
        *nextByte = character ? character : (BYTE)BIOS_PRINT_SCREEN_BLANK_CELL;
        job->ShouldTestStatus = TRUE;
        if (++job->Column >= job->Columns) job->Phase = BIOS_PRINT_SCREEN_PHASE_LF;
        return BIOS_PRINT_SCREEN_STEP_EMIT;
    case BIOS_PRINT_SCREEN_PHASE_LF:
        *nextByte = ASCII_LF; job->Phase = BIOS_PRINT_SCREEN_PHASE_CR;
        return BIOS_PRINT_SCREEN_STEP_EMIT;
    case BIOS_PRINT_SCREEN_PHASE_CR:
        *nextByte = ASCII_CR;
        job->Column = 0;
        job->Phase = (++job->Row >= job->Rows) ? BIOS_PRINT_SCREEN_PHASE_END
                                               : BIOS_PRINT_SCREEN_PHASE_CELL;
        return BIOS_PRINT_SCREEN_STEP_EMIT;
    default:
        job->IsActive = FALSE;
        return BIOS_PRINT_SCREEN_STEP_DONE;
    }
}

#endif /* NTVDMEX_DOS_BIOS_PRTSC_H */
