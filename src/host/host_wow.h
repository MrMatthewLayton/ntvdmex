/* host_wow.h -- what host_wow.c offers the host's other files.
 *
 * Declarations only (#335): defined in host_wow.c. */
#ifndef NTVDMEX_HOST_WOW_H
#define NTVDMEX_HOST_WOW_H
#include "host_state.h"

PVOID ShimMapFlat(WORD segment, DWORD offset, INT isProtectedMode);
VOID WowShimsLoad(VOID);

/* Defined in host_wow.c (#335). */
INT WowDlgIsSelectorAbsent(WORD selector);
/* ── ★★★ THE NESTED RUN: CALL 16-BIT CODE AND WAIT FOR THE ANSWER. (s89, #162) ─────
     Every call into Win16 code so far was ARRANGED from a BOP and taken on the way out
     (wowcall.h): fine for anything the guest asked us to do, impossible for a question
     WINDOWS asks mid-way through its own work -- WM_CTLCOLOR comes from inside a
     control's paint and needs a brush before the paint can go on. This runs the call
     to completion right here: WowCallEnter parks the current context and enters the
     procedure exactly as a deferred callback would (unloaded segments included), and
     this loop drives the guest -- servicing every BOP, USER and GDI calls included,
     the way the PM IRQ injector does -- until the procedure's return stub pops that
     frame (WowCallLeave restores the parked context), then hands the result back.
   ⚠ Only on the guest thread, in protected mode, a Win16 session, below the callback
     depth limit. A run that stops without returning unwinds its frame and says so. */
INT WowCall16SyncEx(DWORD proc, WORD ds, const WORD *args, INT argumentCount, WORD hwnd, WORD message, WORD *result, BYTE *blob, INT blobLength, INT blobArgument, const INT *fix, INT fixupCount);
#endif
