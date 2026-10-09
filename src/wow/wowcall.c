/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * CALLING 16-BIT CODE FROM THE HOST. GH #128, session 40.
 *
 * The code of wowcall.h (#335): its functions and state, in their original order;
 * its own translation unit, declared in wowcall.h.
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

/* Forward declarations, from when this file was part of main.c's unit (they were in wowcall.h). */
INT  WowEnumBusy(VOID);
INT  WowEnumBegin(
    INT kind,
    DWORD procedure,
    WORD dataSelector,
    DWORD lParam,
    DWORD returnLinear,
    WORD parent);
VOID WowEnumLine(INT startX, INT startY, INT endX, INT endY);

/* s89: a SECOND far pointer into the same stack block. EnumFontFamilies' callback
 * takes two structures (ENUMLOGFONT, NEWTEXTMETRIC); they travel as one blob and
 * this names the argument (HIGH word index) that points `off` bytes into it. Set
 * just before WowCallEnter, which consumes and clears it. -1 = none.
 */
INT  g_WowCallBlob2Argument = -1;
INT  g_WowCallBlob2Offset = 0;
/* #295: where the LAST blob went, as a host linear address (ssbase + SP), 0 if the
 * last call placed none. EnumMetaFile reads the guest's handle table back out of
 * it after the callback returns -- see wowgdi.h's g_WowGdiMetafile note.
 */
DWORD g_WowCallBlobLinear = 0;

WOWENUM_FONT g_WowEnumFonts[WOWENUM_MAXFONT];
INT g_WowEnumFontCount;

/* -- s92 (#306): A MESSAGE FOR ANOTHER TASK'S WINDOW RUNS AS THAT TASK. Win16's
 * SendMessage across tasks is a directed yield: the receiver's procedure runs on
 * the receiver's stack with the receiver current, and the sender waits. Run on
 * the sender's stack instead, WinHelp's WM_WINHELP handler asked GetCurrentTask,
 * got Notepad, enumerated Notepad's windows and sent Notepad a WM_COMMAND; and a
 * near pointer to a local reads garbage when SS is not the procedure's DS. The
 * host knows where the receiver's stack is free -- below where it is parked --
 * so main.c (ws_retarget) answers "which SS:SP, and switch the task word";
 * `g_WowCallUntarget` switches it back when the procedure returns.
 */
WORD (*g_WowCallCurrentTask)(VOID) = 0;
INT  (*g_WowCallRetarget)(WORD window, PWORD stackSelector, PWORD stackPointer, PDWORD stackBase, PWORD previousTask) = 0;
VOID (*g_WowCallUntarget)(WORD previousTask) = 0;

WOWCALL_FRAME g_WowCallFrames[WOWCALL_MAX_DEPTH];
INT             g_WowCallDepth  = 0;
static DWORD           g_WowCallCount  = 0;   /* how many 16-bit calls this run made */

/* Push one word onto the guest stack at ssbase:*sp, growing down. */
static VOID WowCallPush(DWORD stackBase, PWORD stackPointer, WORD value)
{
    volatile BYTE *bytes;

    *stackPointer = (WORD)(*stackPointer - WOW_WORD_BYTES);
    bytes = (volatile BYTE *)(ULONG_PTR)(stackBase + *stackPointer);
    bytes[0] = (BYTE)(value & BYTE_MASK);
    bytes[1] = (BYTE)(value >> BYTE_SHIFT);
}

/* Enter a 16-bit FAR PASCAL window procedure. The caller must ALREADY have
 * advanced EIP past its own BOP -- what we save here is where the guest goes
 * when the callback returns, and a context saved on the BOP would execute it
 * a second time.
 *
 * `returnLinear` is the linear address of the originating call's return-value hole,
 * so that a WM_CREATE answering -1 can still fail the CreateWindow that sent
 * it. Pass 0 when there is nothing to revise.
 *
 * Returns 1 if the guest is now standing at the procedure's first instruction.
 */
/* `isAbsent` = the target's code selector is NOT PRESENT, so we must reach it
 * through the RETF trampoline rather than by writing CS. See WOWCALL_RETF_OFF.
 */
INT WowCallEnter(
    volatile BYTE *tib,
    DWORD stackBase,
    WORD returnSelector,
    DWORD procedure,
    WORD dataSelector,
    PCWORD argumentWords,
    INT argumentWordCount,
    DWORD returnLinear,
    INT returnMode,
    PWORD sink,
    WORD window,
    WORD message,
    PCBYTE blob,
    INT blobLength,
    INT blobArgument,
    INT isAbsent)
{
    PWOWCALL_FRAME frame;
    WORD arguments[WOWCALL_MAX_ARGW];
    WORD stackPointer;
    INT index;

    if (g_WowCallDepth >= WOWCALL_MAX_DEPTH)
        return 0;
    if (!stackBase || !returnSelector || !(procedure >> WORD_SHIFT))
        return 0;
    if (argumentWordCount < 0 || argumentWordCount > WOWCALL_MAX_ARGW)
        return 0;
    if (blobLength < 0 || blobLength > WOWCALL_MAX_BLOB)
        return 0;
    for (index = 0; index < argumentWordCount; ++index)
        arguments[index] = argumentWords[index];

    frame = &g_WowCallFrames[g_WowCallDepth++];
    WowSchedSave(&frame->Saved, tib, 0, 0, 0);
    frame->ReturnLinear  = returnLinear;
    frame->Procedure    = procedure;
    frame->Window    = window;
    frame->Message     = message;
    frame->ReturnMode = returnMode;
    frame->Sink    = sink;
    frame->Action  = WOWCALL_ACT_NONE;   /* the caller sets it after we succeed */
    frame->ActionArgument  = 0;
    frame->EnteredTask   = g_WowCallCurrentTask ? g_WowCallCurrentTask() : 0;
    frame->PreviousTask = 0;
    if (window && g_WowCallRetarget)            /* s92 #306: see g_WowCallRetarget */
    {
        WORD newStackSelector = 0, newStackPointer = 0;
        DWORD newStackBase = 0;
        if (g_WowCallRetarget(window, &newStackSelector, &newStackPointer, &newStackBase, &frame->PreviousTask) && newStackBase)
        {
            VDM_SET16(tib, VTIB_SS,  newStackSelector);
            VDM_SET16(tib, VTIB_ESP, newStackPointer);
            stackBase = newStackBase;
        }
        else
            frame->PreviousTask = 0;
    }

    /* Pascal order: the FIRST declared argument is pushed FIRST, so it ends up
     * at the highest address -- which is what `[bp+0x0e] == hwnd` in a window
     * procedure means under the documented Pascal convention. A DWORD is two words, high first, for the same reason. The caller
     * hands them in declared order and this pushes them in that order.
     */
    stackPointer = (WORD)(VDM_REG(tib, VTIB_ESP) & WORD_MASK);

    /* THE STRUCTURE GOES DOWN FIRST, BELOW THE ARGUMENTS (Importance = 3):
     * A pointer argument has to point at memory the GUEST can address, and the
     * only such memory this host can hand out for the duration of one call is
     * the guest's own stack. So the bytes are placed below the current SP and
     * the far pointer that names them is written into the argument that was
     * reserved for it -- which cannot be done by the caller, because SS:SP is
     * only known here.
     *
     * [CAUTION]: IT MUST GO BELOW THE ARGUMENTS, NOT ABOVE. The procedure returns with
     * `retf 0x0a`, which discards exactly the argument bytes; anything placed
     * above them would still be on the stack afterwards and would silently
     * move SP for whoever we interrupted.
     *
     * [CAUTION]: SP IS KEPT EVEN. A 16-bit stack that goes odd costs an access penalty on
     * every push for the rest of the call and is a trap for the next reader.
     *
     * [CAUTION]: AND IT IS THE SELECTOR, NOT THE BASE, THAT THE GUEST NEEDS: `ssbase` is
     * a host linear address and means nothing to 16-bit code.
     */
    g_WowCallBlobLinear = 0;
    if (blob && blobLength > 0 && blobArgument >= 0 && blobArgument + 1 < argumentWordCount)
    {
        WORD stackSelector = (WORD)(VDM_REG(tib, VTIB_SS) & WORD_MASK);
        INT  blobBytes  = (blobLength + 1) & ~1;
        stackPointer = (WORD)(stackPointer - blobBytes);
        for (index = 0; index < blobLength; ++index)
            *(volatile BYTE *)(ULONG_PTR)(stackBase + (DWORD)(WORD)(stackPointer + index)) = blob[index];
        arguments[blobArgument]     = stackSelector;                       /* the far pointer's HIGH */
        arguments[blobArgument + 1] = stackPointer;                       /* ... and its offset */
        g_WowCallBlobLinear    = stackBase + (DWORD)stackPointer;
        if (g_WowCallBlob2Argument >= 0 && g_WowCallBlob2Argument + 1 < argumentWordCount
            && g_WowCallBlob2Offset > 0 && g_WowCallBlob2Offset < blobLength)
        {
            arguments[g_WowCallBlob2Argument]     = stackSelector;
            arguments[g_WowCallBlob2Argument + 1] = (WORD)(stackPointer + g_WowCallBlob2Offset);
        }
    }
    g_WowCallBlob2Argument = -1;
    g_WowCallBlob2Offset = 0;

    for (index = 0; index < argumentWordCount; ++index)
        WowCallPush(stackBase, &stackPointer, arguments[index]);
    WowCallPush(stackBase, &stackPointer, returnSelector);       /* the far return address: CS ... */
    WowCallPush(stackBase, &stackPointer, 0);            /* ... then IP, at offset 0 */
    /* [INFO]: AND, IF THE SEGMENT IS NOT LOADED, THE TARGET ITSELF -- so the RETF we
     * are about to enter on pops it and faults on OUR behalf. Same order as
     * the return address above: CS first, so IP ends up at [SP].
     */
    if (isAbsent)
    {
        WowCallPush(stackBase, &stackPointer, (WORD)(procedure >> WORD_SHIFT));
        WowCallPush(stackBase, &stackPointer, (WORD)(procedure & WORD_MASK));
    }
    VDM_SET16(tib, VTIB_ESP, stackPointer);

    /* DS is the contract (see the header note); AX carries the same value so
     * that a MakeProcInstance-style `mov ds,ax` prologue is satisfied too. One
     * assignment cannot be right for one form and wrong for the other, because
     * both forms read the same register.
     */
    VDM_SET16(tib, VTIB_EAX, dataSelector);
    VDM_SET16(tib, VTIB_DS,  dataSelector);
    if (isAbsent)
    {
        /* Enter on the RETF, which is in a segment that IS present. Its own #NP
         * on the popped selector is restartable, so krnl386 loads the segment
         * and the retry lands in the procedure with this identical stack.
         */
        VDM_SET16(tib, VTIB_CS,  returnSelector);
        VDM_REG(tib, VTIB_EIP) = (DWORD)WOWCALL_RETF_OFF;
    }
    else
    {
        VDM_SET16(tib, VTIB_CS,  (WORD)(procedure >> WORD_SHIFT));
        VDM_REG(tib, VTIB_EIP) = (DWORD)(procedure & WORD_MASK);
    }
    ++g_WowCallCount;
    return 1;
}

/* The procedure returned. `result` is DX:AX, read by the caller before this.
 * Puts the interrupted context back and hands the result to whoever asked.
 * Returns the frame that was in flight, or NULL if there was none -- and "none"
 * is not a curiosity, it means something executed our return stub that we did
 * not send there, which is a fact worth printing rather than swallowing.
 */
DWORD g_WowCallLastResult;   /* the last nested call's DX:AX (sink keeps only AX) */
PWOWCALL_FRAME WowCallLeave(volatile BYTE *tib, DWORD result)
{
    PWOWCALL_FRAME frame;

    if (g_WowCallDepth <= 0)
        return NULL;
    frame = &g_WowCallFrames[--g_WowCallDepth];
    WowSchedRestore(&frame->Saved, tib);
    if (frame->PreviousTask && g_WowCallUntarget)
        g_WowCallUntarget(frame->PreviousTask);
    if (frame->Sink)
        *frame->Sink = (WORD)result;
    g_WowCallLastResult = result;            /* s91 #309: DX:AX, for WOWCallback16Ex */
    frame->Written = 0;
    if (frame->ReturnLinear)
    {
        volatile BYTE *hole = (volatile BYTE *)(ULONG_PTR)frame->ReturnLinear;
        DWORD value = 0;
        INT isWrite = 0;
        /* [INFO]: WM_CREATE MAY REFUSE. Returning -1 from WM_CREATE is the documented
         * way for a window procedure to abort its own creation, and the host
         * must honour it: the call that made the window comes back 0. The
         * return hole is guest memory and outlives the context switch, so
         * revising it is a four-byte write, not a special case.
         */
        if (frame->ReturnMode == WOWCALL_RET_KEEP)
        {
            if (frame->Message == WM_CREATE16 && (WORD)result == WOWCALL_CREATE_REFUSED)
                isWrite = 1;
        }
        else
        {
            /* [CAUTION]: MASK A WORD RETURN. DX is not the high half of a WORD result --
             * see WOWCALL_RET_RESULTW above, and the LocalAlloc call that
             * proved it.
             */
            value = (frame->ReturnMode == WOWCALL_RET_RESULTW) ? (result & WORD_MASK) : result;
            isWrite = 1;                  /* SendMessage: the procedure's answer */
        }
        if (isWrite)
        {
            frame->Written = value;
            hole[0] = (BYTE)(value & BYTE_MASK);
            hole[1] = (BYTE)((value >> BYTE_SHIFT)  & BYTE_MASK);
            hole[2] = (BYTE)((value >> WORD_SHIFT) & BYTE_MASK);
            hole[3] = (BYTE)((value >> TOP_BYTE_SHIFT) & BYTE_MASK);
        }
    }
    return frame;
}
