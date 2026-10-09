/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Off-VM battery for the default INT 05h's byte sequencer (#274).
 *
 * src/dos/bios_prtsc.h decides which byte the guest's p5 loop (bios_kbdact.asm) prints
 * next through INT 17h, and whether INT 17h's answer ends the job. Driven here with a
 * fake screen and a fake printer status.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include <stdio.h>
#include <string.h>
#include "bios_prtsc.h"

/* The fake screen: the largest mode the checks print, filled with letters. */
#define PRTSC_TEST_SCREEN_ROWS          50
#define PRTSC_TEST_SCREEN_COLUMNS       132
#define PRTSC_TEST_FIRST_LETTER         'A'
#define PRTSC_TEST_LETTER_COUNT         26
#define PRTSC_TEST_NUL_CELL             0

/* The job's parameters: columns, 0040:0084 (rows - 1), the page and the saved cursor. */
#define PRTSC_TEST_COLUMNS_80           80
#define PRTSC_TEST_COLUMNS_132          132
#define PRTSC_TEST_NO_COLUMNS           0
#define PRTSC_TEST_BDA_ROWS_25          24
#define PRTSC_TEST_BDA_ROWS_50          49
#define PRTSC_TEST_BDA_ROWS_UNSET       0
#define PRTSC_TEST_PAGE                 0
#define PRTSC_TEST_CURSOR               0x1234

/* The printer's answers (INT 17h AH). */
#define PRTSC_TEST_PRINTER_READY        0x90    /* 90h: selected, not busy */
#define PRTSC_TEST_FIRST_STATUS         0
#define PRTSC_TEST_NEVER_FAIL           0       /* Fail at no byte */
#define PRTSC_TEST_NO_STATUS            0
#define PRTSC_TEST_TIME_OUT             0x01    /* AH bit 0 */
#define PRTSC_TEST_IO_ERROR             0x08    /* AH bit 3 */
#define PRTSC_TEST_OUT_OF_PAPER         0x20    /* AH bit 5 */
#define PRTSC_TEST_NOT_ERROR_BITS       0xD6    /* Every bit but 29h */
#define PRTSC_TEST_ALL_ERROR_BITS       0x29

/* The bytes a job sends, and where the checks look in them. */
#define PRTSC_TEST_OUTPUT_SIZE          8000
#define PRTSC_TEST_LINE_END_BYTES       2       /* LF, CR */
#define PRTSC_TEST_ROWS_25              25
#define PRTSC_TEST_ROWS_50              50
#define PRTSC_TEST_BYTES_80X25          2052
#define PRTSC_TEST_LAST_COLUMN_80       79
#define PRTSC_TEST_ROW_0_END            82      /* 2 + 80: the first byte after row 0's cells */
#define PRTSC_TEST_INDEX_CELL_0_0       2       /* Where row 0, columns 0, 1 and 79 land */
#define PRTSC_TEST_INDEX_CELL_0_1       3
#define PRTSC_TEST_INDEX_CELL_0_79      81
#define PRTSC_TEST_LF                   0x0A
#define PRTSC_TEST_CR                   0x0D
#define PRTSC_TEST_SPACE                ' '
#define PRTSC_TEST_FIRST_CELL_BYTE      3       /* Byte number (1-based) of the 1st cell */
#define PRTSC_TEST_MIDDLE_BYTE          50
#define PRTSC_TEST_LATER_BYTE           100
#define PRTSC_TEST_INITIAL_LF_BYTE      1
#define PRTSC_TEST_LAST_CELL_OF_ROW_0   82
#define PRTSC_TEST_NO_CONTEXT           0

static INT g_Checks = 0, g_Failures = 0;

static VOID PrintScreenTestCheck(BOOL passed, PCSTR message)
{
    g_Checks++;
    if (passed)
    {
        printf("  PASS  %s\n", (message));
    }
    else
    {
        printf("  FAIL  %s\n", (message));
        g_Failures++;
    }
}

static BYTE g_Screen[PRTSC_TEST_SCREEN_ROWS][PRTSC_TEST_SCREEN_COLUMNS];
static INT g_CellReads;
static BYTE PrintScreenTestReadCell(PVOID context, BYTE row, BYTE column)
{
    (VOID)context;
    ++g_CellReads;
    return g_Screen[row][column];
}

/* Run a whole job; the printer answers PRTSC_TEST_PRINTER_READY for every byte except byte
 * number `failAt` (1-based, 0 = never), which gets `failStatus`. Returns the final result.
 */
static BYTE g_Output[PRTSC_TEST_OUTPUT_SIZE];
static INT g_OutputCount;
static INT PrintScreenTestRunJob(PBIOS_PRINT_SCREEN_JOB job, BYTE columns, BYTE bdaRowsMinusOne,
                                 INT failAt, BYTE failStatus)
{
    BYTE nextByte, printerStatus = PRTSC_TEST_FIRST_STATUS;
    INT result;
    g_OutputCount = 0; g_CellReads = 0;
    BiosPrintScreenBegin(job, columns, bdaRowsMinusOne, PRTSC_TEST_PAGE, PRTSC_TEST_CURSOR);
    for (;;)
    {
        result = BiosPrintScreenStep(job, printerStatus, PrintScreenTestReadCell,
                                     PRTSC_TEST_NO_CONTEXT, &nextByte);
        if (result != BIOS_PRINT_SCREEN_STEP_EMIT) return result;
        if (g_OutputCount < (INT)sizeof g_Output) g_Output[g_OutputCount] = nextByte;
        ++g_OutputCount;
        printerStatus = (failAt && g_OutputCount == failAt) ? failStatus : PRTSC_TEST_PRINTER_READY;
    }
}

INT main(VOID)
{
    BIOS_PRINT_SCREEN_JOB job;
    INT result, row, column;
    BOOL passed;
    printf("== prtsc_test: the default INT 05h (#274) ==\n");
    for (row = 0; row < PRTSC_TEST_SCREEN_ROWS; ++row)
        for (column = 0; column < PRTSC_TEST_SCREEN_COLUMNS; ++column)
            g_Screen[row][column] =
                (BYTE)(PRTSC_TEST_FIRST_LETTER + (row + column) % PRTSC_TEST_LETTER_COUNT);
    g_Screen[0][0] = PRTSC_TEST_NUL_CELL;                 /* a NUL cell */

    result = PrintScreenTestRunJob(&job, PRTSC_TEST_COLUMNS_80, PRTSC_TEST_BDA_ROWS_25,
                                   PRTSC_TEST_NEVER_FAIL, PRTSC_TEST_NO_STATUS);
    PrintScreenTestCheck(result == BIOS_PRINT_SCREEN_STEP_DONE && !job.IsActive,
                         "80x25: the job ends DONE and inactive");
    PrintScreenTestCheck(g_OutputCount == PRTSC_TEST_LINE_END_BYTES + PRTSC_TEST_ROWS_25
                             * (PRTSC_TEST_COLUMNS_80 + PRTSC_TEST_LINE_END_BYTES),
                         "80x25: 2 + 25 x (80 + 2) = 2052 bytes through INT 17h");
    PrintScreenTestCheck(g_Output[0] == PRTSC_TEST_LF && g_Output[1] == PRTSC_TEST_CR,
                         "the initial line end is LF, CR (IBM listing, as recalled)");
    PrintScreenTestCheck(g_Output[PRTSC_TEST_INDEX_CELL_0_0] == PRTSC_TEST_SPACE,
                         "a NUL cell prints as a space");
    PrintScreenTestCheck(g_Output[PRTSC_TEST_INDEX_CELL_0_1] == g_Screen[0][1]
                             && g_Output[PRTSC_TEST_INDEX_CELL_0_79]
                                == g_Screen[0][PRTSC_TEST_LAST_COLUMN_80],
                         "row 0, columns 1 and 79 in order");
    PrintScreenTestCheck(g_Output[PRTSC_TEST_ROW_0_END] == PRTSC_TEST_LF
                             && g_Output[PRTSC_TEST_ROW_0_END + 1] == PRTSC_TEST_CR
                             && g_Output[PRTSC_TEST_ROW_0_END + PRTSC_TEST_LINE_END_BYTES]
                                == g_Screen[1][0],
                         "each row ends LF, CR; row 1 follows");
    PrintScreenTestCheck(g_CellReads == PRTSC_TEST_COLUMNS_80 * PRTSC_TEST_ROWS_25,
                         "one screen read per cell");
    PrintScreenTestCheck(job.Cursor == PRTSC_TEST_CURSOR,
                         "the saved cursor is kept for the host to put back");

    result = PrintScreenTestRunJob(&job, PRTSC_TEST_COLUMNS_80, PRTSC_TEST_BDA_ROWS_UNSET,
                                   PRTSC_TEST_NEVER_FAIL, PRTSC_TEST_NO_STATUS);
    PrintScreenTestCheck(result == BIOS_PRINT_SCREEN_STEP_DONE
                             && g_OutputCount == PRTSC_TEST_LINE_END_BYTES + PRTSC_TEST_ROWS_25
                                * (PRTSC_TEST_COLUMNS_80 + PRTSC_TEST_LINE_END_BYTES),
                         "0040:0084 = 0 (never set) -> 25 rows");
    result = PrintScreenTestRunJob(&job, PRTSC_TEST_COLUMNS_132, PRTSC_TEST_BDA_ROWS_50,
                                   PRTSC_TEST_NEVER_FAIL, PRTSC_TEST_NO_STATUS);
    PrintScreenTestCheck(result == BIOS_PRINT_SCREEN_STEP_DONE
                             && g_OutputCount == PRTSC_TEST_LINE_END_BYTES + PRTSC_TEST_ROWS_50
                                * (PRTSC_TEST_COLUMNS_132 + PRTSC_TEST_LINE_END_BYTES),
                         "132x50: columns from AH=0Fh, rows from 0040:0084 + 1");

    result = PrintScreenTestRunJob(&job, PRTSC_TEST_COLUMNS_80, PRTSC_TEST_BDA_ROWS_25,
                                   PRTSC_TEST_FIRST_CELL_BYTE, PRTSC_TEST_TIME_OUT);
    PrintScreenTestCheck(result == BIOS_PRINT_SCREEN_STEP_ERROR && !job.IsActive
                             && g_OutputCount == PRTSC_TEST_FIRST_CELL_BYTE,
                         "a time-out (AH bit 0) on a cell ends the job: ERROR, no more bytes");
    result = PrintScreenTestRunJob(&job, PRTSC_TEST_COLUMNS_80, PRTSC_TEST_BDA_ROWS_25,
                                   PRTSC_TEST_MIDDLE_BYTE, PRTSC_TEST_IO_ERROR);
    PrintScreenTestCheck(result == BIOS_PRINT_SCREEN_STEP_ERROR
                             && g_OutputCount == PRTSC_TEST_MIDDLE_BYTE,
                         "an I/O error (AH bit 3) ends it too");
    result = PrintScreenTestRunJob(&job, PRTSC_TEST_COLUMNS_80, PRTSC_TEST_BDA_ROWS_25,
                                   PRTSC_TEST_LATER_BYTE, PRTSC_TEST_OUT_OF_PAPER);
    PrintScreenTestCheck(result == BIOS_PRINT_SCREEN_STEP_ERROR
                             && g_OutputCount == PRTSC_TEST_LATER_BYTE,
                         "out of paper (AH bit 5) ends it too");
    result = PrintScreenTestRunJob(&job, PRTSC_TEST_COLUMNS_80, PRTSC_TEST_BDA_ROWS_25,
                                   PRTSC_TEST_LATER_BYTE, PRTSC_TEST_NOT_ERROR_BITS);
    PrintScreenTestCheck(result == BIOS_PRINT_SCREEN_STEP_DONE
                             && g_OutputCount == PRTSC_TEST_BYTES_80X25,
                         "AH bits outside 29h (busy, ack, selected...) are not errors");
    result = PrintScreenTestRunJob(&job, PRTSC_TEST_COLUMNS_80, PRTSC_TEST_BDA_ROWS_25,
                                   PRTSC_TEST_INITIAL_LF_BYTE,
                                   PRTSC_TEST_ALL_ERROR_BITS);   /* the initial LF fails */
    PrintScreenTestCheck(result == BIOS_PRINT_SCREEN_STEP_DONE
                             && g_OutputCount == PRTSC_TEST_BYTES_80X25,
                         "a line-end byte's status is not tested (IBM tests screen bytes only)");
    result = PrintScreenTestRunJob(&job, PRTSC_TEST_COLUMNS_80, PRTSC_TEST_BDA_ROWS_25,
                                   PRTSC_TEST_LAST_CELL_OF_ROW_0,
                                   PRTSC_TEST_ALL_ERROR_BITS);   /* the LAST cell of row 0 */
    PrintScreenTestCheck(result == BIOS_PRINT_SCREEN_STEP_ERROR
                             && g_OutputCount == PRTSC_TEST_LAST_CELL_OF_ROW_0,
                         "...but the last cell before a line end is");

    passed = TRUE;
    memset(&job, 0, sizeof job);
    {   BYTE nextByte;
        passed = BiosPrintScreenStep(&job, PRTSC_TEST_FIRST_STATUS, PrintScreenTestReadCell,
                                     PRTSC_TEST_NO_CONTEXT, &nextByte)
                 == BIOS_PRINT_SCREEN_STEP_DONE; }
    PrintScreenTestCheck(passed, "a step with no job running says DONE");
    result = PrintScreenTestRunJob(&job, PRTSC_TEST_NO_COLUMNS, PRTSC_TEST_BDA_ROWS_25,
                                   PRTSC_TEST_NEVER_FAIL, PRTSC_TEST_NO_STATUS);
    PrintScreenTestCheck(result == BIOS_PRINT_SCREEN_STEP_DONE
                             && g_OutputCount == PRTSC_TEST_LINE_END_BYTES,
                         "0 columns: just the initial line end");

    printf("\n%d checks, %d failed\n", g_Checks, g_Failures);
    return g_Failures ? 1 : 0;
}
