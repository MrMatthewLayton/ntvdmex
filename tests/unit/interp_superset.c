/* interp_superset.c -- "the new interpreter does everything the old one did, the same
 * way, and only ADDS". GH #194.
 *
 * WHY A SECOND MODE. interpfuzz.sh's digest compares HEAD and the working tree over
 * everything, which is exactly right for a change that must not alter behaviour -- and
 * useless for #194, whose whole point is that instructions which used to BAIL now run:
 * the digest differs on the first `66 9C`, and says nothing about whether anything ELSE
 * moved. This runs both interpreters in lockstep on the same random programs and state:
 *   - wherever the OLD one executed a step, the NEW one must execute it too, with an
 *     identical register file, IP, flags, memory writes and port traffic;
 *   - wherever the old one declined, the new one may run -- that step is counted by
 *     opcode (the coverage the new forms got) and rolled back, and the program ends.
 * A mismatch is printed with the bytes and both states. ALLOW="66:8C 66:E5" (opcode
 * keys, `66:` = under the operand-size prefix) names INTENDED changes, which are counted
 * but do not fail the run.
 *
 *   interp_superset [programs] [steps] [seed] [pm]       (pm=1: the fake PM table)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "interp_xcpu.h"

VOID ref_Initialize(PCBYTE image, INT isProtectedMode);  VOID new_Initialize(PCBYTE image, INT isProtectedMode);
VOID ref_Poke(UINT32 linear, BYTE value);     VOID new_Poke(UINT32 linear, BYTE value);
PCBYTE ref_Memory(VOID);               VOID new_Sync(PCBYTE image);
INT  ref_Step(PINTERP_XCPU state, PUINT64 effects);       INT  new_Step(PINTERP_XCPU state, PUINT64 effects);
VOID new_UndoBegin(VOID);  INT new_UndoRollback(VOID);  VOID new_UndoEnd(VOID);

static BYTE g_Image[XMEM_SIZE];
static UINT64 g_Random;
static UINT32 InterpSupersetRandom(VOID) { g_Random ^= g_Random << 13; g_Random ^= g_Random >> 7; g_Random ^= g_Random << 17; return (UINT32)(g_Random >> 11); }

/* Opcode key: 0x10000 if 0x66 was among the prefixes, 0x0F00|op2 for the 0F map. */
static UINT InterpSupersetKeyOf(PCBYTE memory, UINT32 linear)
{
    UINT hasOperandSize = 0, index;
    for (index = 0; index < 5; ++index) {
        BYTE byteValue = memory[(linear + index) % XMEM_SIZE];
        if (byteValue == 0x66) { hasOperandSize = 1; continue; }
        if (byteValue == 0x26 || byteValue == 0x2E || byteValue == 0x36 || byteValue == 0x3E || byteValue == 0x64 || byteValue == 0x65 ||
            byteValue == 0xF2 || byteValue == 0xF3 || byteValue == 0x67 || byteValue == 0xF0) continue;
        if (byteValue == 0x0F) return (hasOperandSize << 16) | 0x0F00 | memory[(linear + index + 1) % XMEM_SIZE];
        return (hasOperandSize << 16) | byteValue;
    }
    return 0xFFFFF;
}
static VOID InterpSupersetKeyString(UINT key, PSTR output)
{
    if ((key & 0xFF00) == 0x0F00) sprintf(output, "%s0F%02X", (key >> 16) ? "66:" : "", key & 0xFF);
    else sprintf(output, "%s%02X", (key >> 16) ? "66:" : "", key & 0xFF);
}

static UINT g_ExtraByKey[0x20000], g_MismatchByKey[0x20000];

/* Whole-token match in a space-separated list: "8C" must not match "66:8C". */
static INT InterpSupersetIsAllowedKey(PCSTR allowList, PCSTR keyText)
{
    size_t length = strlen(keyText);
    PCSTR cursor = allowList;
    if (!allowList) return 0;
    while ((cursor = strstr(cursor, keyText)) != NULL) {
        if ((cursor == allowList || cursor[-1] == ' ') && (cursor[length] == 0 || cursor[length] == ' ')) return 1;
        cursor += length;
    }
    return 0;
}

static UINT32 InterpSupersetLinearOf(PCINTERP_XCPU state, INT isProtectedMode)
{
    /* Only for printing the bytes: real-mode CS:IP, or the fake PM base. */
    UINT32 base = isProtectedMode ? (((UINT32)(state->Segments[1] >> 3) * 0x1230u) & 0xFFFF0u) : ((UINT32)state->Segments[1] << 4);
    return (base + state->Ip) % XMEM_SIZE;
}

static VOID InterpSupersetDump(PCSTR tag, PCINTERP_XCPU state)
{
    INT index;
    printf("    %s ip=%04X cs=%04X fl=%08X", tag, state->Ip, state->Segments[1], state->Flags);
    for (index = 0; index < 8; ++index) printf(" r%d=%08X", index, state->Registers[index]);
    printf(" seg=%04X/%04X/%04X/%04X/%04X/%04X\n", state->Segments[0], state->Segments[1], state->Segments[2], state->Segments[3], state->Segments[4], state->Segments[5]);
}

INT main(INT argc, PSTR *argv)
{
    long programCount = argc > 1 ? atol(argv[1]) : 200000;
    INT  stepLimit = argc > 2 ? atoi(argv[2]) : 64;
    UINT64 seed = argc > 3 ? strtoull(argv[3], 0, 0) : 0x5EEDF00DULL;
    INT isProtectedMode = argc > 4 ? atoi(argv[4]) : 0;
    PCSTR allowList = getenv("ALLOW");
    long program, sameCount = 0, extraCount = 0, badCount = 0, allowedCount = 0, printedCount = 0;
    UINT keyIndex;
    UINT32 index;

    g_Random = seed ? seed : 1;
    for (index = 0; index < XMEM_SIZE; ++index) g_Image[index] = (BYTE)InterpSupersetRandom();
    ref_Initialize(g_Image, isProtectedMode); new_Initialize(g_Image, isProtectedMode);
    for (program = 0; program < programCount; ++program) {
        INTERP_XCPU state; INT position;
        memset(&state, 0, sizeof state);
        for (position = 0; position < 8; ++position) state.Registers[position] = (InterpSupersetRandom() & 3) ? (InterpSupersetRandom() & 0xFFFF) : InterpSupersetRandom();
        for (position = 0; position < 6; ++position) state.Segments[position] = (WORD)(InterpSupersetRandom() & 0xFFFF);
        state.Registers[1] &= 0xFFFF;
        state.Segments[1] = (WORD)(InterpSupersetRandom() % 0xF000);
        if (isProtectedMode) state.Segments[1] = (WORD)((state.Segments[1] & ~7u) | 7u);      /* an LDT selector, RPL 3 */
        state.Ip = (WORD)InterpSupersetRandom();
        state.Flags = 0x0002 | (InterpSupersetRandom() & 0x0ED5u);
        { UINT32 linear = InterpSupersetLinearOf(&state, isProtectedMode), offset;
          for (offset = 0; offset < 64; ++offset) { BYTE byteValue = (BYTE)InterpSupersetRandom(); ref_Poke((linear + offset) % XMEM_SIZE, byteValue); new_Poke((linear + offset) % XMEM_SIZE, byteValue); } }
        for (position = 0; position < stepLimit; ++position) {
            INTERP_XCPU referenceState = state, newState = state; UINT64 referenceEffects = 0, newEffects = 0; INT isReferenceOk, isNewOk;
            UINT key = InterpSupersetKeyOf(ref_Memory(), InterpSupersetLinearOf(&state, isProtectedMode));
            new_UndoBegin();
            isReferenceOk = ref_Step(&referenceState, &referenceEffects);
            isNewOk = new_Step(&newState, &newEffects);
            if (isReferenceOk) {
                new_UndoEnd();
                if (!isNewOk || memcmp(&referenceState, &newState, sizeof referenceState) || referenceEffects != newEffects) {
                    CHAR keyText[16]; InterpSupersetKeyString(key, keyText);
                    g_MismatchByKey[key & 0x1FFFF]++;
                    if (InterpSupersetIsAllowedKey(allowList, keyText)) ++allowedCount;
                    else {
                        ++badCount;
                        if (printedCount++ < 12) {
                            PCBYTE memory = ref_Memory(); UINT32 linear = InterpSupersetLinearOf(&state, isProtectedMode), offset;
                            printf("MISMATCH %s new_ok=%d fx %s  bytes:", keyText, isNewOk, referenceEffects == newEffects ? "same" : "DIFFER");
                            for (offset = 0; offset < 8; ++offset) printf(" %02X", memory[(linear + offset) % XMEM_SIZE]);
                            printf("\n"); InterpSupersetDump("in ", &state); InterpSupersetDump("old", &referenceState); InterpSupersetDump("new", &newState);
                        }
                    }
                    new_Sync(ref_Memory());          /* realign memory, end this program */
                    break;
                }
                ++sameCount; state = referenceState;
                continue;
            }
            /* The old interpreter declined. */
            if (isNewOk) {
                ++extraCount; g_ExtraByKey[key & 0x1FFFF]++;
                if (!new_UndoRollback()) new_Sync(ref_Memory());
            } else new_UndoEnd();
            break;
        }
    }
    printf("superset: %ld programs, %ld steps identical, %ld steps newly executed, "
           "%ld mismatches (%ld allowed)\n", programCount, sameCount, extraCount, badCount + allowedCount, allowedCount);
    printf("newly executed, by opcode (top 40):\n");
    { INT shownCount;
      for (shownCount = 0; shownCount < 40; ++shownCount) {
          UINT best = 0, bestKey = 0; CHAR keyText[16];
          for (keyIndex = 0; keyIndex < 0x20000; ++keyIndex) if (g_ExtraByKey[keyIndex] > best) { best = g_ExtraByKey[keyIndex]; bestKey = keyIndex; }
          if (!best) break;
          InterpSupersetKeyString(bestKey, keyText); printf("  %-8s %u\n", keyText, best); g_ExtraByKey[bestKey] = 0;
      } }
    if (allowedCount) {
        printf("allowed (intended) changes, by opcode:\n");
        for (keyIndex = 0; keyIndex < 0x20000; ++keyIndex) if (g_MismatchByKey[keyIndex]) { CHAR keyText[16]; InterpSupersetKeyString(keyIndex, keyText); if (InterpSupersetIsAllowedKey(allowList, keyText)) printf("  %-8s %u\n", keyText, g_MismatchByKey[keyIndex]); }
    }
    if (badCount) { printf("⛔ NOT A SUPERSET\n"); return 1; }
    printf("SUPERSET OK\n");
    return 0;
}
