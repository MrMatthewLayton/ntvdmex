#ifndef WOWSCHED_H
#define WOWSCHED_H
/*
 * wowsched.h -- a cooperative scheduler for Win16 tasks. GH #128, session 38.
 *
 * ── WHY THE HOST HAS TO DO THIS AT ALL ───────────────────────────────────────
 * krnl386 has no scheduler. Every Win16 scheduling primitive -- Yield (0x1d),
 * OldYield (0x75), DirectedYield (0x96), WaitEvent (0x1e), PostEvent (0x1f),
 * SetPriority (0x20), LockCurrentTask (0x21) -- is a bare exported pass-through
 * to WOW32 with no 16-bit body and no call or jump to it anywhere in the binary.
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
 * ── THE HANDSHAKE, READ OUT OF THE BINARY ────────────────────────────────────
 *   seg1:0x97be  push [0x228] / push bp     ; on the CREATOR's stack
 *          97c3  mov di,ss / mov cx,sp      ; its stack, kept in registers
 *          97e9  mov ss,[bp+8] / mov sp,si  ; switch to the NEW task's stack
 *          97ee  mov [0x228],es             ; current task = the new TDB
 *          9822  jmp 0xb1d0                 ; ★ WOW32 0x74, through the thunk
 *          985c  ...pop the Win16 entry frame...
 *          9879  iret                       ; ENTER THE TASK
 *
 * The thunk (seg1:0x2bb6) ends by popping an EPILOGUE MODE off the guest stack
 * (at bp-24, `push 0`ed at seg1:0x2bc7) and dispatching through a 38-entry table
 * at cs:0x2a36. krnl386 never sets that word; all 37 non-zero modes exist for
 * the 32-bit side. Mode 25 lands at seg1:0x2c4e, which pops the thunk frame --
 * so DI and CX come back -- and jumps to
 *
 *   seg1:0x9827  mov ss,di / mov sp,cx / pop bp / pop [0x228]
 *
 * i.e. the creator, back on its own stack, its BP restored, current again. So
 * "end this task's turn and put its creator back" is one word on the stack.
 *
 * ── THE THREE MOMENTS ────────────────────────────────────────────────────────
 * A. At WOW32 0x74 we SAVE the launch frame and change nothing, so the new task
 *    runs first, exactly as it does today.
 * B. At the new task's first WaitEvent -- the handshake between InitTask and
 *    InitApp in every Win16 startup -- we save IT, restore the launch frame, and
 *    return that frame through mode 25 so the creator carries on and LoadModule
 *    completes.
 * C. The creator eventually ENDS ITSELF: seg1:0xcd36 unlinks its record,
 *    seg1:0xcd3c unsigns it, seg1:0xcd41 sets [0x228] = 0 and seg1:0xcd66 moves
 *    to a private kernel stack. From that instruction the machine belongs to the
 *    scheduler, and the first code to touch the current task
 *    (seg1:0x321f `mov es,[0x228]` / `test es:[0x18],2`) dereferences a NULL
 *    SELECTOR -- #GP with err=0. That fault IS the cue: no task is current, and
 *    one is waiting. We resume it instead of reflecting.
 *
 * ⚠ WHAT THIS FIRST CUT DOES NOT DO. At (C) the creator's remaining teardown is
 *   abandoned -- it had already retired, but it was still freeing selectors, and
 *   those leak. That is a truncation, not a design, and it is logged as one.
 * ⚠ AND THE RETURN VALUE AT (B) IS READ, NOT GUESSED. LoadModule's result is the
 *   new task's instance handle, and by the time the task reaches WaitEvent
 *   krnl386 has already computed it: InitTask (seg2:0x2e02) writes it to
 *   TDB+0x1c, which is the same field GetExePtr matches on. We read it back out
 *   of the guest rather than inventing a number -- an earlier probe hardcoded
 *   0x03d6 and that is exactly the kind of thing that stops reproducing.
 */

#define WOWSCHED_CTX_LO   0x364      /* VTIB_GS  -- the low end of the block */
#define WOWSCHED_CTX_LEN  0x40       /* .. through VTIB_SS inclusive         */

typedef struct {
    int   used;                      /* 1 = this slot holds a resumable task  */
    BYTE  ctx[WOWSCHED_CTX_LEN];     /* the guest register file, verbatim     */
    DWORD modelin;                   /* linear address of that frame's mode   */
    WORD  task;                      /* the TDB selector it belongs to        */
} wowsched_slot_t;

/* Save the live guest context. `eipadj` is added to the saved EIP, which is how
   a context saved AT a BOP resumes AFTER it -- the guest must not re-execute the
   three BOP bytes, and a context that does is an infinite loop, not a task. */
static void wowsched_save(wowsched_slot_t *s, volatile BYTE *tib,
                          DWORD modelin, WORD task, int eipadj)
{
    unsigned k;
    volatile BYTE *src = (volatile BYTE *)tib + WOWSCHED_CTX_LO;
    for (k = 0; k < WOWSCHED_CTX_LEN; ++k) s->ctx[k] = src[k];
    *(DWORD *)(s->ctx + (0x390 - WOWSCHED_CTX_LO)) += (DWORD)eipadj;   /* VTIB_EIP */
    s->modelin = modelin;
    s->task    = task;
    s->used    = 1;
}

static void wowsched_restore(wowsched_slot_t *s, volatile BYTE *tib)
{
    unsigned k;
    volatile BYTE *dst = (volatile BYTE *)tib + WOWSCHED_CTX_LO;
    for (k = 0; k < WOWSCHED_CTX_LEN; ++k) dst[k] = s->ctx[k];
    s->used = 0;
}

/* The two words the host writes into a saved frame before resuming it: the
   epilogue mode (bp-24) and the return-value hole (bp-16), which sit 8 bytes
   apart, so one recorded address locates both. */
static void wowsched_poke(DWORD lin, WORD v)
{
    volatile BYTE *p = (volatile BYTE *)(ULONG_PTR)lin;
    p[0] = (BYTE)(v & 0xFF);
    p[1] = (BYTE)(v >> 8);
}

#define WOWSCHED_RETLIN(modelin) ((modelin) + (DWORD)(WOW32_OFF_RET - WOW32_OFF_MODE))

#endif /* WOWSCHED_H */
