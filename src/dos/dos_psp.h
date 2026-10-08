/* dos_psp.h -- build a DOS Program Segment Prefix (+ a minimal environment) in
 * conventional memory. Pure logic over a `base` pointer (same convention as
 * dos_mcb.h). Ported from the M2.1 PSP setup in tools/vdmhost/vdmhost.c.
 * Verified off-VM by tests/unit/mcb_test.c.
 */
#ifndef NTVDMEX_DOS_PSP_H
#define NTVDMEX_DOS_PSP_H

#include "../ntvdmex_types.h"
#include "dos_mcb.h"        /* DosMcbSegmentAddress / DosMcbWriteWord paragraph addressing */

/* The PSP's fields, as offsets from psp_seg:0. */
#define DOS_PSP_SIZE                 0x100
#define DOS_PSP_INT20                0x00   /* INT 20h (legacy exit): opcode, then vector  */
#define DOS_PSP_MEMORY_TOP           0x02   /* WORD: segment of top-of-memory              */
#define DOS_PSP_INT22_COPY           0x0A   /* the saved INT 22h/23h/24h vectors           */
#define DOS_PSP_INT23_COPY           0x0E
#define DOS_PSP_INT24_COPY           0x12
#define DOS_PSP_PARENT               0x16   /* WORD: the parent's PSP segment              */
#define DOS_PSP_JFT                  0x18   /* the job file table, one byte per handle     */
#define DOS_PSP_JFT_FIRST_CLOSED     0x1D   /* the first entry past the std handles        */
#define DOS_PSP_JFT_END              0x2C
#define DOS_PSP_ENVIRONMENT          0x2C   /* WORD: environment segment                   */
#define DOS_PSP_JFT_SIZE             0x32   /* WORD: JFT size                              */
#define DOS_PSP_JFT_POINTER          0x34   /* far pointer to the JFT: offset, segment     */
#define DOS_PSP_PREVIOUS             0x38   /* previous PSP                                */
#define DOS_PSP_DISPATCH             0x50   /* INT 21h ; RETF                              */
#define DOS_PSP_COMMAND_TAIL_LENGTH  0x80
#define DOS_PSP_COMMAND_TAIL         0x81

/* What DosPspBuild writes into them. */
#define DOS_PSP_OPCODE_INT           0xCD
#define DOS_PSP_OPCODE_RETF          0xCB
#define DOS_PSP_JFT_STDIN_ENTRY      1      /* JFT: std handles open                       */
#define DOS_PSP_JFT_STDOUT_ENTRY     1
#define DOS_PSP_JFT_STDERR_ENTRY     1
#define DOS_PSP_JFT_AUX_ENTRY        0
#define DOS_PSP_JFT_PRN_ENTRY        2
#define DOS_PSP_JFT_CLOSED           0xFF   /* remaining JFT = closed                      */
#define DOS_PSP_JFT_HANDLES          0x14   /* 20 handles                                  */
#define DOS_PSP_NO_PREVIOUS          0xFF   /* previous PSP = 0xFFFFFFFF                   */
#define DOS_PSP_COMMAND_TAIL_END     0x0D   /* the byte the command-tail parser scans for  */
#define DOS_PSP_COMMAND_TAIL_MAX     126

/* The vectors a PSP saves, and the IVT they come from. */
#define DOS_PSP_SAVED_VECTORS        3

/* Build a PSP at pspSegment:0 that points at the environment at environmentSegment:0.
 * topSegment is the top-of-conventional-memory segment (0xA000 = 640KB). The command tail
 * is empty (length 0) until M2.5 wires real args; psp[0x81] holds the 0x0D the command-tail
 * parser scans for. Mirrors vdmhost.c's PSP setup.
 *
 * ⚠ IT DOES NOT TOUCH THE ENVIRONMENT BLOCK. (s74) It used to write three NULs at
 *   environmentSegment:0 "to make an empty environment" -- harmless for the first program,
 *   whose env is built straight afterwards, but EXEC hands a child the PARENT'S
 *   block when the parameter block says "inherit", and three NULs over `COM` of
 *   `COMSPEC=` turned the block into: strings end at +1, count word at +2, program
 *   name at +4 = "PEC=C:\COMMAND.COM". That is the file Heaven7's DOS4GW.EXE
 *   reported it could not open. The WOW launch had already found the same wipe
 *   and rebuilt its env after this call; the EXEC path had not. Whoever wants an
 *   empty environment writes the one NUL themselves. */
static inline VOID DosPspBuild(_In_opt_ volatile BYTE *base, _In_ WORD pspSegment,
                               _In_ WORD environmentSegment, _In_ WORD topSegment) {
    volatile BYTE *psp = DosMcbSegmentAddress(base, pspSegment);
    DWORD byteIndex;

    for (byteIndex = 0; byteIndex < DOS_PSP_SIZE; ++byteIndex) psp[byteIndex] = 0;
    psp[DOS_PSP_INT20] = DOS_PSP_OPCODE_INT; psp[DOS_PSP_INT20 + 1] = VECTOR_TERMINATE;                /* INT 20h (legacy exit)      */
    DosMcbWriteWord(psp + DOS_PSP_MEMORY_TOP, topSegment);                     /* segment of top-of-memory   */
    psp[DOS_PSP_JFT] = DOS_PSP_JFT_STDIN_ENTRY; psp[DOS_PSP_JFT + 1] = DOS_PSP_JFT_STDOUT_ENTRY; psp[DOS_PSP_JFT + 2] = DOS_PSP_JFT_STDERR_ENTRY;       /* JFT: std handles open      */
    psp[DOS_PSP_JFT + 3] = DOS_PSP_JFT_AUX_ENTRY; psp[DOS_PSP_JFT + 4] = DOS_PSP_JFT_PRN_ENTRY;
    for (byteIndex = DOS_PSP_JFT_FIRST_CLOSED; byteIndex < DOS_PSP_JFT_END; ++byteIndex) psp[byteIndex] = DOS_PSP_JFT_CLOSED;       /* remaining JFT = closed     */
    DosMcbWriteWord(psp + DOS_PSP_ENVIRONMENT, environmentSegment);                     /* environment segment        */
    DosMcbWriteWord(psp + DOS_PSP_JFT_SIZE, DOS_PSP_JFT_HANDLES);                        /* JFT size (20 handles)      */
    DosMcbWriteWord(psp + DOS_PSP_JFT_POINTER, DOS_PSP_JFT);                        /* JFT pointer: offset        */
    DosMcbWriteWord(psp + DOS_PSP_JFT_POINTER + 2, pspSegment);                     /* JFT pointer: segment       */
    psp[DOS_PSP_PREVIOUS] = DOS_PSP_NO_PREVIOUS; psp[DOS_PSP_PREVIOUS + 1] = DOS_PSP_NO_PREVIOUS;                /* previous PSP = 0xFFFFFFFF  */
    psp[DOS_PSP_PREVIOUS + 2] = DOS_PSP_NO_PREVIOUS; psp[DOS_PSP_PREVIOUS + 3] = DOS_PSP_NO_PREVIOUS;
    psp[DOS_PSP_DISPATCH] = DOS_PSP_OPCODE_INT; psp[DOS_PSP_DISPATCH + 1] = VECTOR_DOS; psp[DOS_PSP_DISPATCH + 2] = DOS_PSP_OPCODE_RETF; /* INT 21h ; RETF          */
    psp[DOS_PSP_COMMAND_TAIL_LENGTH] = 0; psp[DOS_PSP_COMMAND_TAIL] = DOS_PSP_COMMAND_TAIL_END;                   /* empty command tail + 0x0D  */
}

/* Set the PSP command tail at pspSegment:0x80 from an arguments string (no leading space):
   [0x80] = length, [0x81..] = " <arguments>", terminated by 0x0D (the parser scans for it).
   A leading space is the DOS convention. Empty/NULL arguments -> length 0, 0x0D at 0x81. */
static inline VOID DosPspBuildCommandTail(_In_opt_ volatile BYTE *base, _In_ WORD pspSegment,
                                          _In_opt_ PCSTR arguments) {
    volatile BYTE *psp = DosMcbSegmentAddress(base, pspSegment);
    INT length = 0, argumentIndex;
    if (arguments && arguments[0]) {
        psp[DOS_PSP_COMMAND_TAIL + length++] = ' ';                         /* conventional leading space */
        for (argumentIndex = 0; arguments[argumentIndex] && length < DOS_PSP_COMMAND_TAIL_MAX; ++argumentIndex) psp[DOS_PSP_COMMAND_TAIL + length++] = (BYTE)arguments[argumentIndex];
    }
    psp[DOS_PSP_COMMAND_TAIL_LENGTH] = (BYTE)length;
    psp[DOS_PSP_COMMAND_TAIL + length] = DOS_PSP_COMMAND_TAIL_END;
}

/* ── SAVE THE LIVE INT 22h/23h/24h VECTORS INTO THE PSP. (GH #34) ─────────────
   DOS does this as part of building a PSP, and restores them on termination.
   Reads the IVT directly (segment 0), so it must run AFTER the host has planted
   its handlers -- saving a vector that is still 0000:0000 stores a null that the
   program will happily restore later.
   Measured on MS-DOS 6.22 (tests/probes/dos/p_psp.asm): the PSP's copy of all three
   EQUALS the live vector at program entry, which is the host-independent
   invariant the probe asserts. */
static inline VOID DosPspSaveVectors(_In_opt_ volatile BYTE *base, _In_ WORD pspSegment,
                                     _In_ WORD parentPsp) {
    static const BYTE vectors[DOS_PSP_SAVED_VECTORS] = { VECTOR_TERMINATE_ADDRESS, VECTOR_CTRL_C, VECTOR_CRITICAL_ERROR };
    static const BYTE copyOffsets[DOS_PSP_SAVED_VECTORS]  = { DOS_PSP_INT22_COPY, DOS_PSP_INT23_COPY, DOS_PSP_INT24_COPY };
    volatile BYTE *psp = DosMcbSegmentAddress(base, pspSegment);
    volatile BYTE *ivt = DosMcbSegmentAddress(base, IVT_BASE_SEGMENT);
    UINT vectorIndex, byteIndex;
    for (vectorIndex = 0; vectorIndex < DOS_PSP_SAVED_VECTORS; ++vectorIndex)
        for (byteIndex = 0; byteIndex < IVT_ENTRY_SIZE; ++byteIndex)
            psp[copyOffsets[vectorIndex] + byteIndex] = ivt[vectors[vectorIndex] * IVT_ENTRY_SIZE + byteIndex];
    DosMcbWriteWord(psp + DOS_PSP_PARENT, parentPsp);          /* the parent's PSP segment */
}

/* The other half of the contract: put them back. A program that installed its
   own INT 24h must not leave it installed after it exits -- that is how one
   guest's critical-error handler ends up servicing the next one's failure. */
static inline VOID DosPspRestoreVectors(_In_opt_ volatile BYTE *base, _In_ WORD pspSegment) {
    static const BYTE vectors[DOS_PSP_SAVED_VECTORS] = { VECTOR_TERMINATE_ADDRESS, VECTOR_CTRL_C, VECTOR_CRITICAL_ERROR };
    static const BYTE copyOffsets[DOS_PSP_SAVED_VECTORS]  = { DOS_PSP_INT22_COPY, DOS_PSP_INT23_COPY, DOS_PSP_INT24_COPY };
    volatile BYTE *psp = DosMcbSegmentAddress(base, pspSegment);
    volatile BYTE *ivt = DosMcbSegmentAddress(base, IVT_BASE_SEGMENT);
    UINT vectorIndex, byteIndex;
    for (vectorIndex = 0; vectorIndex < DOS_PSP_SAVED_VECTORS; ++vectorIndex)
        for (byteIndex = 0; byteIndex < IVT_ENTRY_SIZE; ++byteIndex)
            ivt[vectors[vectorIndex] * IVT_ENTRY_SIZE + byteIndex] = psp[copyOffsets[vectorIndex] + byteIndex];
}

#endif /* NTVDMEX_DOS_PSP_H */
