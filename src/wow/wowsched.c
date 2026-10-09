/* wowsched.c -- the Win16 task scheduler's context switch: save, restore and swap a task's VDM context, and poke its mode word.
 *
 * The function definitions of wowsched.h, which keeps their declarations and doc comments (#335). */
#include "wowsched.h"
#include "../ntvdmex_bits.h"   /* defines only: BYTE_MASK, BYTE_SHIFT */

INT g_WowSchedCurrentBase = 0;   /* see wowsched.h */

VOID WowSchedSave(PWOWSCHED_SLOT slot, volatile BYTE *tib,
                          DWORD modeLinear, WORD task, INT eipAdjust)
{
    UINT index;
    volatile BYTE *source = (volatile BYTE *)tib + WOWSCHED_CTX_LO;
    for (index = 0; index < WOWSCHED_CTX_LEN; ++index) slot->Context[index] = source[index];
    *(DWORD *)(slot->Context + (WOWSCHED_VTIB_EIP - WOWSCHED_CTX_LO)) += (DWORD)eipAdjust;   /* VTIB_EIP */
    slot->ModeLinear = modeLinear;
    slot->Task    = task;
    slot->IsUsed    = 1;
    slot->IsFresh   = 0;                  /* the caller marks a launch-parked task fresh */
    slot->IsRunnable = 0;                 /* ...and a mid-work one runnable              */
    slot->CallbackDepth  = 0;
    slot->IsWaitingForMessage  = 0;
    slot->BaseDepth     = g_WowSchedCurrentBase;
}

VOID WowSchedRestore(PWOWSCHED_SLOT slot, volatile BYTE *tib)
{
    UINT index;
    volatile BYTE *destination = (volatile BYTE *)tib + WOWSCHED_CTX_LO;
    for (index = 0; index < WOWSCHED_CTX_LEN; ++index) destination[index] = slot->Context[index];
    slot->IsUsed = 0;
    g_WowSchedCurrentBase = slot->BaseDepth;          /* a top-level resume is re-based by the caller */
}

VOID WowSchedSwap(PWOWSCHED_SLOT slot, volatile BYTE *tib,
                          DWORD modeLinear, WORD currentTask, INT eipAdjust)
{
    WOWSCHED_SLOT resumeSlot = *slot;                 /* the one we are going back to */
    WowSchedSave(slot, tib, modeLinear, currentTask, eipAdjust); /* the running one takes its place */
    WowSchedRestore(&resumeSlot, tib);
}

VOID WowSchedPoke(DWORD linear, WORD value)
{
    volatile BYTE *bytes = (volatile BYTE *)(ULONG_PTR)linear;
    bytes[0] = (BYTE)(value & BYTE_MASK);
    bytes[1] = (BYTE)(value >> BYTE_SHIFT);
}
