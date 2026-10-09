#ifndef NTVDMEX_WOWSCHED_H
#define NTVDMEX_WOWSCHED_H
#include <windows.h>
/*
 * wowsched.h -- a cooperative scheduler for Win16 tasks. GH #128, session 38.
 *
 * ── WHY THE HOST HAS TO DO THIS AT ALL ───────────────────────────────────────
 * krnl386 has no scheduler. Every Win16 scheduling primitive -- Yield (0x1d),
 * OldYield (0x75), DirectedYield (0x96), WaitEvent (0x1e), PostEvent (0x1f),
 * SetPriority (0x20), LockCurrentTask (0x21) -- arrives at WOW32 as a thunk;
 * none of them does any work on the 16-bit side.
 * krnl386 keeps the STATE (the task list, and each parked task's SS:SP at
 * TDB+0x02/+0x04) and hands every DECISION to the 32-bit side. On real WOW that
 * side runs each 16-bit task on its own Win32 thread and blocks it. We have one
 * CPU, so we interleave them ourselves.
 *
 * ── WHAT A CONTEXT IS, AND WHY IT IS SO SMALL ────────────────────────────────
 * The whole guest register file is one contiguous block in the VDM TIB, from
 * VTIB_GS (0x364) to VTIB_SS (0x3A0) inclusive. Saving a task is a 0x40-byte
 * copy. It is that cheap because we never invent a frame: every context we save
 * is one krnl386 built and is standing on a stack krnl386 owns, and every
 * context we restore is resumed at an instruction krnl386 chose.
 *
 * ── THE HANDSHAKE, AS OBSERVED ───────────────────────────────────────────────
 * When one task launches another, WOW32 0x74 arrives with the creator's SS:SP
 * already set aside, the guest already on the NEW task's stack and the new TDB
 * already current. Returning from the thunk enters the new task.
 *
 * Every thunk frame carries an EPILOGUE MODE word (bp-24, WOW32_OFF_MODE) that
 * the 16-bit side always pushes as 0; the non-zero modes are for the 32-bit side
 * to choose. Returning mode 25 from 0x74 brings the CREATOR back instead -- on
 * its own stack, its BP restored, current again (session 38, on the rig). So
 * "end this task's turn and put its creator back" is one word on the stack.
 *
 * ── THE THREE MOMENTS ────────────────────────────────────────────────────────
 * A. At WOW32 0x74 we SAVE the launch frame and change nothing, so the new task
 *    runs first, exactly as it does today.
 * B. At the new task's first WaitEvent -- the handshake between InitTask and
 *    InitApp in every Win16 startup -- we save IT, restore the launch frame, and
 *    return that frame through mode 25 so the creator carries on and LoadModule
 *    completes.
 * C. The creator eventually ENDS ITSELF: it leaves the task list, no task is
 *    current any more, and it moves to a private kernel stack. From then on the
 *    machine belongs to the scheduler, and the first kernel code to touch the
 *    current task loads a NULL SELECTOR -- #GP with err=0. That fault IS the
 *    cue: no task is current, and one is waiting. We resume it instead of
 *    reflecting.
 *
 * ⚠ WHAT THIS FIRST CUT DOES NOT DO. At (C) the creator's remaining teardown is
 *   abandoned -- it had already retired, but it was still freeing selectors, and
 *   those leak. That is a truncation, not a design, and it is logged as one.
 * ⚠ AND THE RETURN VALUE AT (B) IS READ, NOT GUESSED. LoadModule's result is the
 *   new task's instance handle, and by the time the task reaches WaitEvent
 *   krnl386 has already computed it: after InitTask it is in TDB+0x1c, the
 *   same field GetExePtr matches on. We read it back out of the guest rather
 *   than inventing a number -- an earlier probe hardcoded 0x03d6 and that is
 *   exactly the kind of thing that stops reproducing.
 */

#define WOWSCHED_CTX_LO   0x364      /* VTIB_GS  -- the low end of the block */
#define WOWSCHED_CTX_LEN  0x40       /* .. through VTIB_SS inclusive         */
#define WOWSCHED_VTIB_EIP 0x390      /* the saved EIP within the block       */

typedef struct _WOWSCHED_SLOT {
    INT   IsUsed;                    /* 1 = this slot holds a resumable task  */
    BYTE  Context[WOWSCHED_CTX_LEN];     /* the guest register file, verbatim     */
    DWORD ModeLinear;                /* linear address of that frame's mode   */
    WORD  Task;                      /* the TDB selector it belongs to        */
    INT   IsFresh;                   /* s92: parked at its LAUNCH, never run yet */
    INT   IsRunnable;                /* s92: parked MID-WORK (it yielded to a task it
                                        launched), not waiting for input            */
    INT   CallbackDepth;                /* s92: the callback depth it yielded at -- it is
                                        resumed only at that same depth (LIFO frames) */
    INT   IsWaitingForMessage;                /* s92: parked in an EMPTY GetMessage (E): runnable
                                        again once its own queue is not               */
    INT   BaseDepth;                   /* s92: the callback depth its top level ran at;
                                        CallbackDepth == BaseDepth = parked holding no host frame,
                                        so it may resume at ANY depth (re-based there)  */
} WOWSCHED_SLOT, *PWOWSCHED_SLOT;
typedef const WOWSCHED_SLOT *PCWOWSCHED_SLOT;

/* The running task's base depth -- see `BaseDepth`. */
extern INT g_WowSchedCurrentBase;

/* s92 (#306): THE RUN QUEUE. One slot was "the task that is not running" -- complete
   for two tasks and wrong for a third: Calc's WinHelp (WOWEXEC + Calc + WINHELP) was
   created and never scheduled, and the process could not end. Every task that is not
   running is parked in one of these; the running one is never in the table. */
#define WOWSCHED_MAX 8

/* Save the live guest context. `eipAdjust` is added to the saved EIP, which is how
   a context saved AT a BOP resumes AFTER it -- the guest must not re-execute the
   three BOP bytes, and a context that does is an infinite loop, not a task. */
VOID WowSchedSave(PWOWSCHED_SLOT slot, volatile BYTE *tib,
                          DWORD modeLinear, WORD task, INT eipAdjust);

VOID WowSchedRestore(PWOWSCHED_SLOT slot, volatile BYTE *tib);

/* ── ★★★ SWAP: PARK THE RUNNING TASK WHERE THE OTHER ONE WAS. (session 39) ────
     `g_ws_task` means "the one task that is not running". With two tasks that is
     a complete description, so a round-robin needs no extra slot -- take a copy
     of the parked one, overwrite the slot with the running one, then restore the
     copy. The alternative (a second named slot) has to answer "which slot is the
     other one" at every site, and that question has no stable answer.
   ⚠ THIS IS ONLY SOUND BECAUSE EACH TASK'S FRAME IS ON ITS OWN STACK. The frame
     we park stays exactly where it is in guest memory while the other task runs,
     because a Win16 task's SS is its own. That is the same property moment (A)
     relies on, and it is why the mode and return-value words can be written into
     a frame now and read by the guest's epilogue much later. */
VOID WowSchedSwap(PWOWSCHED_SLOT slot, volatile BYTE *tib,
                          DWORD modeLinear, WORD currentTask, INT eipAdjust);

/* The two words the host writes into a saved frame before resuming it: the
   epilogue mode (bp-24) and the return-value hole (bp-16), which sit 8 bytes
   apart, so one recorded address locates both. */
VOID WowSchedPoke(DWORD linear, WORD value);

#define WOWSCHED_RETLIN(modelin) ((modelin) + (DWORD)(WOW32_OFF_RET - WOW32_OFF_MODE))

#endif /* NTVDMEX_WOWSCHED_H */
