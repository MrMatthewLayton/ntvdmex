/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Build a DOS environment block in conventional memory. Pure logic
 * over a `base` pointer (same convention as dos_mcb.h). M2.5.
 *
 * Layout DOS programs expect at the environment segment (PSP:0x2C):
 *   NAME=VALUE\0 NAME=VALUE\0 ... \0      (a trailing \0 ends the variable list)
 *   <WORD count = 1>                       (DOS 3.0+: number of strings following)
 *   <program full path>\0                  (argv[0] for the guest)
 * An empty/short env is a common cause of real tools mis-starting; a well-formed
 * block (COMSPEC etc. + the program path) is what they read.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_DOS_ENV_H
#define NTVDMEX_DOS_ENV_H

#include "../ntvdmex_types.h"
#include "dos_mcb.h"

static inline volatile BYTE *DosEnvPutString(_Out_ volatile BYTE *cursor, _In_ PCSTR text)
{
    while (*text) *cursor++ = (BYTE)*text++;
    return cursor;
}

/* Build a standard environment at environmentSegment and append the program path. Returns the
 * number of bytes written (fits in the 0x10-paragraph env block laid by DosMcbInitialize).
 */
/* PATH IS NOT DECORATION -- krnl386 SEARCHES IT. (GH #128, session 37) (Importance = 1):
 * `PATH=C:\` was fine while every guest was a DOS program launched by full path.
 * It is not fine for WOW: krnl386's file search reads the environment block's
 * `PATH=`, splits it on `;`, and tries each directory in turn -- and the module it looks for that way is **WOWEXEC.EXE**, the Win16
 * program itself, whose name comes from `[boot] WOWSHELL` in SYSTEM.INI as a bare
 * filename with no directory. With `C:\` the only entry, the search could never
 * succeed, and krnl386 reported it the way it reports any module it cannot find:
 * "Please re-install the following module to your system32 directory: WOWEXEC.EXE".
 *
 * [CAUTION]: Passed in rather than hardcoded here, and defaulted to the old value, so a DOS
 * guest takes a byte-identical path to before. The WOW caller passes the host's
 * REAL Windows and system directories -- which is also what a real WOW launch
 * gets, since ntvdm inherits the NT process environment.
 */
/* [CAUTION]: THE BLOCK IS 256 BYTES AND WHAT FOLLOWS IT IS A LANDMINE. DOS_ENV_SEG is
 * 0x0060 and the DOS-resident filler block starts at 0x0070, so this has exactly
 * 0x10 paragraphs -- and linear 0x714, a few bytes into that block, is the NT
 * kernel's VDM interrupt-state dword, which dos_layout.h records as breaking
 * EVERY guest when written from user mode. Nothing here was bounded before,
 * which was survivable while every string was a literal and stopped being so the
 * moment PATH and the program path became host-supplied. Both are truncated.
 *
 * [CAUTION]: #207: the block is now at 0x7F (DOS_ENV_SEG), still 0x10 paragraphs, and what
 * follows it is the DOS block's MCB header at linear 0x8F0 (DOS_RESBLK_MCB) -- an
 * overrun now breaks the MCB chain instead of [0x714]. Different landmine, same cap.
 */
#define DOS_ENV_CAP     0x100

static inline volatile BYTE *DosEnvPutBounded(_Out_ volatile BYTE *cursor,
                                              _In_ volatile BYTE *end, _In_ PCSTR text)
{
    while (*text && cursor < end) *cursor++ = (BYTE)*text++;
    return cursor;
}

/* THE CARD THE BLASTER VARIABLE DESCRIBES:
 * The Audio settings page can move the Sound Blaster's port, IRQ and DMA
 * channel, and the moment it can, a hard-coded BLASTER string becomes a LIE
 * told to every guest that reads it -- the exact failure the note below warns
 * about, just arriving from the dialog instead of from a typo. So the string is
 * built from the same numbers vdd_sb is configured with, and there is one
 * struct that carries them.
 *
 * [CAUTION]: `Dma16Channel` is advertised as H only when it is set. The default card has a 16-bit
 * channel (5) that this string has never mentioned, and Doom's audio was tuned
 * against the string as it stands; adding H unasked would change what DMX sees
 * on the one guest whose sound is user-confirmed.
 */
typedef struct _DOS_SB_CONFIG
{
    WORD IoBase;        /* I/O base, e.g. 0x220 -> "A220" */
    BYTE Irq;
    BYTE Dma8Channel;   /* 8-bit DMA channel -> "D" */
    BYTE Dma16Channel;  /* 16-bit channel -> "H"; 0 = do not advertise one */
    BYTE Type;          /* BLASTER "T" value */
    WORD MpuBase;       /* #231: MPU-401 base -> "P330"; 0 = do not advertise one */
    WORD Emu8kBase;     /* #233: EMU8000 base -> "E620"; 0 = no AWE wavetable fitted */
} DOS_SB_CONFIG, *PDOS_SB_CONFIG;

typedef const DOS_SB_CONFIG *PCDOS_SB_CONFIG;

/* T3 = an SB 2.0-class card. [CAUTION] THAT DISAGREES WITH THE DSP VERSION WE REPORT
 * (4.05, an SB16, which would be T6) and it has done since the string was
 * written. It is left alone deliberately: Doom's sound is user-confirmed against
 * this exact string, T is not what a driver uses to find the card, and changing
 * it is a measurement to make on the rig, not a tidy-up to slip into a change
 * that cannot be gated there. Recorded, not silently corrected.
 */
#define DOS_SB_DEFAULT_TYPE         3

/* The card this host has always claimed when no configuration is supplied:
 * "BLASTER=A220 I5 D1 T3".
 */
#define DOS_SB_DEFAULT_IO_BASE      0x220
#define DOS_SB_DEFAULT_IRQ          5
#define DOS_SB_DEFAULT_DMA8         1
#define DOS_SB_NOT_ADVERTISED       0       /* A 0 port or channel is left out of the string */

/* Writing numbers into the string. */
#define DOS_ENV_DECIMAL_DIGITS_MAX  12
#define DOS_ENV_SCAN_MAX            0x7FFE  /* An environment block is under 32 KB */
#define DOS_ENV_HEX_FIRST_SHIFT     8       /* Three hex digits: bits 11-8 first */

/* The block's fixed text. */
#define DOS_ENV_COMSPEC             "COMSPEC=C:\\COMMAND.COM"
#define DOS_ENV_PATH_PREFIX         "PATH="
#define DOS_ENV_DEFAULT_PATH        "C:\\"
#define DOS_ENV_PROMPT              "PROMPT=$p$g"
#define DOS_ENV_DEFAULT_PROGRAM     "C:\\PROGRAM.COM"
#define DOS_ENV_TAIL_RESERVE        8       /* Room kept for the tail: see DosEnvBuildWithCard */
#define DOS_ENV_STRING_COUNT_LOW    0x01    /* WORD: one string follows */
#define DOS_ENV_STRING_COUNT_HIGH   0x00

static inline volatile BYTE *DosEnvPutDecimal(_Out_ volatile BYTE *cursor, _In_ volatile BYTE *end,
                                               _In_ UINT value)
{
    CHAR digits[DOS_ENV_DECIMAL_DIGITS_MAX]; INT digitCount = 0;
    if (!value)
    {
        if (cursor < end) *cursor++ = '0';
        return cursor;
    }
    while (value && digitCount < (INT)sizeof digits)
    {
        digits[digitCount++] = (CHAR)('0' + value % DECIMAL_RADIX);
        value /= DECIMAL_RADIX;
    }
    while (digitCount-- > 0 && cursor < end) *cursor++ = (BYTE)digits[digitCount];
    return cursor;
}

/* Three hex digits, upper case: every base a Sound Blaster can sit at (0x220 to
 * 0x280) is three, and that is how every BLASTER string in the wild spells it.
 */
static inline volatile BYTE *DosEnvPutThreeHexDigits(_Out_ volatile BYTE *cursor, _In_ volatile BYTE *end,
                                                     _In_ UINT value)
{
    static const CHAR hexDigits[] = HEX_DIGITS_UPPER;
    INT shift;
    for (shift = DOS_ENV_HEX_FIRST_SHIFT; shift >= 0; shift -= NIBBLE_SHIFT) if (cursor < end) *cursor++ = (BYTE)hexDigits[(value >> shift) & NIBBLE_MASK];
    return cursor;
}

/* Emit "BLASTER=A220 I5 D1 T3" for `card`, or exactly that literal when card is NULL
 * -- so a caller that has no configuration to offer gets the card this host has
 * always claimed, byte for byte.
 */
static inline volatile BYTE *DosEnvPutBlaster(_Out_ volatile BYTE *cursor, _In_ volatile BYTE *end,
                                              _In_opt_ PCDOS_SB_CONFIG card)
{
    DOS_SB_CONFIG defaultCard;
    if (!card)
    {
        defaultCard.IoBase = DOS_SB_DEFAULT_IO_BASE; defaultCard.Irq = DOS_SB_DEFAULT_IRQ; defaultCard.Dma8Channel = DOS_SB_DEFAULT_DMA8; defaultCard.Dma16Channel = DOS_SB_NOT_ADVERTISED;
        defaultCard.Type = DOS_SB_DEFAULT_TYPE; defaultCard.MpuBase = DOS_SB_NOT_ADVERTISED; defaultCard.Emu8kBase = DOS_SB_NOT_ADVERTISED;
        card = &defaultCard;
    }
    cursor = DosEnvPutBounded(cursor, end, "BLASTER=A");
    cursor = DosEnvPutThreeHexDigits(cursor, end, card->IoBase);
    cursor = DosEnvPutBounded(cursor, end, " I");   cursor = DosEnvPutDecimal(cursor, end, card->Irq);
    cursor = DosEnvPutBounded(cursor, end, " D");   cursor = DosEnvPutDecimal(cursor, end, card->Dma8Channel);
    if (card->Dma16Channel)
    {
        cursor = DosEnvPutBounded(cursor, end, " H");
        cursor = DosEnvPutDecimal(cursor, end, card->Dma16Channel);
    }
    if (card->MpuBase)
    {
        cursor = DosEnvPutBounded(cursor, end, " P");
        cursor = DosEnvPutThreeHexDigits(cursor, end, card->MpuBase);
    }
    if (card->Emu8kBase)
    {
        cursor = DosEnvPutBounded(cursor, end, " E");
        cursor = DosEnvPutThreeHexDigits(cursor, end, card->Emu8kBase);
    }
    cursor = DosEnvPutBounded(cursor, end, " T");   cursor = DosEnvPutDecimal(cursor, end, card->Type);
    return cursor;
}

/* EXTRA VARIABLES THE GUEST NEEDS AND WE HAD NO WAY TO GIVE IT (Importance = 1):
 * Until now this block was a fixed four -- COMSPEC, PATH, PROMPT, BLASTER -- and a
 * DOS program that is configured through its environment simply could not be
 * configured. That is not a corner case: it is how a whole class of DOS software
 * takes its settings, and the one that found it is ZAR (GH #23), whose own
 * RUNZAR.BAT is nothing but
 *
 *     set DOS4GVM=@ZAR.VMC
 *     zar
 *
 * -- i.e. the game's supported way to start it selects DOS/4GW's VIRTUAL MEMORY
 * manager, and we were launching the .EXE directly and silently getting a
 * different memory strategy from the one the game ships with.
 *
 * `extra` is the raw text of the host's dosenv.txt knob: one NAME=VALUE per line.
 * Lines may also be separated by ';' so a caller with no file can pass a literal.
 * Blank lines and lines beginning with '#' are skipped, so the knob can be commented.
 *
 * [CAUTION]: NOTHING IS VALIDATED. A DOS environment is a list of NUL-terminated strings and
 * DOS itself does not care what is in them; a name with no '=' is legal and some
 * programs use exactly that. Rejecting shapes here would be inventing a rule the
 * thing we are emulating does not have.
 *
 * [CAUTION]: THE CAP IS REAL AND SILENT TRUNCATION WOULD BE THE WORST OUTCOME -- an env var
 * that is half present is a value, and a wrong one. Every write is bounded by
 * `variablesEnd`, which already reserves room for the terminator, the count WORD and the
 * program path, so the tail krnl386 reads can never be what is lost; an entry that
 * does not fit is dropped whole rather than clipped.
 */
static inline DWORD DosEnvBuildWithCard(_In_opt_ volatile BYTE *base, _In_ WORD environmentSegment,
                                        _In_opt_ PCSTR programPath, _In_opt_ PCSTR pathVariable,
                                        _In_opt_ PCDOS_SB_CONFIG card, _In_opt_ PCSTR extra)
{
    volatile BYTE *environment = DosMcbSegmentAddress(base, environmentSegment);
    volatile BYTE *cursor = environment;
    /* Leave room for the trailing NUL, the count WORD, the program path and its
     * NUL, so the tail krnl386 actually reads can never be the part that is lost.
     */
    volatile BYTE *variablesEnd = environment + DOS_ENV_CAP - DOS_ENV_TAIL_RESERVE;
    cursor = DosEnvPutBounded(cursor, variablesEnd, DOS_ENV_COMSPEC); *cursor++ = 0;
    cursor = DosEnvPutBounded(cursor, variablesEnd, DOS_ENV_PATH_PREFIX);
    cursor = DosEnvPutBounded(cursor, variablesEnd, (pathVariable && pathVariable[0]) ? pathVariable : DOS_ENV_DEFAULT_PATH);  *cursor++ = 0;
    cursor = DosEnvPutBounded(cursor, variablesEnd, DOS_ENV_PROMPT);             *cursor++ = 0;
    /* BLASTER IS HOW A DOS GAME FINDS THE SOUND CARD:
     * Every Sound Blaster install sets it, so every DOS sound driver reads it and
     * only falls back to probing when it is absent -- and a fallback probe is a
     * WORSE test of our card than being told where it is, because it also has to
     * guess the IRQ and DMA channel.
     *
     * [CAUTION]: THESE MUST TRACK vdd_sb.h. Telling the guest I7 while the VDD raises IRQ5
     * is worse than saying nothing: a driver that believes the string masks the
     * line it was told about and waits on an interrupt that arrives elsewhere.
     * That is why the numbers now come in as an argument rather than being
     * typed here twice -- see DOS_SB_CONFIG.
     */
    cursor = DosEnvPutBlaster(cursor, variablesEnd, card); *cursor++ = 0;
    if (extra)
    {
        PCSTR remaining = extra;
        while (*remaining)
        {
            PCSTR line = remaining;
            INT lineLength = 0, characterIndex;
            while (line[lineLength] && line[lineLength] != '\n' && line[lineLength] != '\r' && line[lineLength] != ';') ++lineLength;
            /* Drop the entry WHOLE if it cannot fit -- see the cap note above. */
            if (lineLength > 0 && line[0] != '#' && cursor + lineLength + 1 <= variablesEnd)
            {
                for (characterIndex = 0; characterIndex < lineLength; ++characterIndex) *cursor++ = (BYTE)line[characterIndex];
                *cursor++ = 0;
            }
            remaining = line + lineLength;
            while (*remaining == '\n' || *remaining == '\r' || *remaining == ';') ++remaining;
        }
    }
    *cursor++ = 0;                                       /* trailing \0 ends the var list */
    *cursor++ = DOS_ENV_STRING_COUNT_LOW; *cursor++ = DOS_ENV_STRING_COUNT_HIGH;                       /* WORD: one string follows */
    cursor = DosEnvPutBounded(cursor, environment + DOS_ENV_CAP - 1,
                     (programPath && programPath[0]) ? programPath : DOS_ENV_DEFAULT_PROGRAM);
    *cursor++ = 0;
    return (DWORD)(cursor - environment);
}

/* The historical shapes, kept so every existing caller reads the same: no card
 * supplied means the card this host has always claimed.
 */
static inline DWORD DosEnvBuildWithPath(_In_opt_ volatile BYTE *base, _In_ WORD environmentSegment,
                                        _In_opt_ PCSTR programPath, _In_opt_ PCSTR pathVariable)
{
    return DosEnvBuildWithCard(base, environmentSegment, programPath, pathVariable, NULL, NULL);
}

static inline DWORD DosEnvBuild(_In_opt_ volatile BYTE *base, _In_ WORD environmentSegment,
                                _In_opt_ PCSTR programPath)
{
    return DosEnvBuildWithCard(base, environmentSegment, programPath, DOS_ENV_DEFAULT_PATH, NULL, NULL);
}

#endif /* NTVDMEX_DOS_ENV_H */
