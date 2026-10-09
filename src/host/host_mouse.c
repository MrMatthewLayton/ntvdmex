/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The mouse: INT 33h, its coordinate model, event queue and callbacks, and the
 *   graphics cursor.
 *
 * Its own translation unit (#335): declared in host_mouse.h.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include "host_state.h"
#include "log.h"
#include "host_mouse.h"
#include "main.h"
#include "host_window.h"
#include "host_dpmi.h"
#include "host_irq.h"

#define I33_SITE_CONTEXT_BEFORE     4   /* g_MouseI33Site: bytes kept from before the INT */

/* Mouse state shared UI thread -> V86 thread (INT 33h). Position is in guest
 * pixels (mapped from the window client); buttons: bit0 L, bit1 R, bit2 M.
 */
volatile LONG g_MouseX = I33_FALLBACK_X, g_MouseY = I33_FALLBACK_Y, g_MouseButtons = 0;
/* RELATIVE MOTION, WHICH IS WHAT A GAME ACTUALLY ASKS FOR:
 * INT 33h function 0Bh reports MICKEYS MOVED SINCE THE LAST CALL, and we derived that
 * from the absolute pointer: (x - last_x) * 8. Fine for a menu, useless for Doom -- the
 * absolute position is CLAMPED to the window, so push the mouse left past the edge and
 * the deltas simply stop: a turn ends at the frame boundary and you cannot spin. It is
 * also wrong in kind. The guest wants how far the DEVICE moved, not where the Windows
 * pointer ended up.
 * - So take the device's own counts. WM_INPUT (raw input, XP+) gives true relative
 *   movement, unclamped, and it keeps working while the pointer is hidden and clipped by
 *   capture -- which is exactly the state you play in. Accumulated here, drained by 0Bh.
 *
 * [CAUTION]: A FALLBACK, not a replacement: if RegisterRawInputDevices fails we go back to the
 * absolute-derived delta rather than reporting no motion at all.
 */
volatile LONG g_MouseDx, g_MouseDy;      /* raw mickeys since the last 0Bh drain */
/* Doom reports "no mouse look" while capture is demonstrably on. Three things can be
 * false and they need opposite fixes: raw input never REGISTERED, WM_INPUT never
 * ARRIVING, or the guest never ASKING (INT 33h 0Bh). Count all three -- and note that
 * the raw path DISABLES the absolute-derived fallback, so a silent raw failure reports
 * no motion at all where the old code at least reported clamped motion.
 */
DWORD g_MouseWmInput, g_MouseRawAbsolute, g_MouseI33[16], g_MouseI33Other;
DWORD g_MouseI33AxOverflow, g_MouseI33SiteCount, g_MouseI33SiteOverflow;
/* An offset register from the caller: full-width from a 32-bit PM caller, a word from
 * V86, 16-bit PM, or a 0300 excursion (s74c, the 0Ch handler ZAR installs at
 * 0x347:0x0044xxxx).
 */
static DWORD MouseI33Offset(volatile BYTE *tib, INT source, DWORD offset)
{
    if (source == I33_SRC_PM && DpmiSelectorIs32((WORD)VDM_REG16(tib, VTIB_CS))) return offset;
    return offset & WORD_MASK;
}

INT g_MouseAbsent = 0;          /* nomouse.flag: INT 33h 0000h answers "none" */
volatile LONG g_MouseHidden = 1;       /* INT 33h cursor hide-count; 0 => visible */

volatile LONG g_MousePressCount[MS_BTNS], g_MouseReleaseCount[MS_BTNS];
static volatile LONG g_MousePressX[MS_BTNS], g_MousePressY[MS_BTNS];
static volatile LONG g_MouseReleaseX[MS_BTNS],   g_MouseReleaseY[MS_BTNS];
DWORD         g_MouseEdges;            /* transitions seen -- STAGE2 evidence */

/* THE DRIVER'S SCREEN IS NOT THE FRAME:
 * Every INT 33h coordinate is in the driver's VIRTUAL screen, and in a 320-wide mode
 * that screen is 640 across: the mouse driver's cell is 8 pixels and mode 13h's is
 * 16, so the driver counts X in half-pixels. Guests know this and halve it. We were
 * handing back physical pixels, so every hit-test in a 320-wide mode landed at half
 * scale -- the pointer and the thing it was supposed to be over were never in the
 * same place. Y is 1:1 in all the standard modes (200/350/480).
 */
/* THE DRIVER'S SCREEN IS THE VIDEO MODE'S, NOT THE PRESENT SURFACE'S (Importance = 5):
 * Every helper here used g_Video.frame.w/h -- the dimensions of the SNAPSHOT the UI
 * thread presents. That surface does not exist until something has been drawn, so
 * in a headless run (and in the window between a mode set and the first present)
 * frame.h is ZERO, and then:
 *    I33Text()  -> 0 > 200 is false, so the text-mode scaling never engages;
 *    I33VirtualMaximumY() -> falls back to 479, in a twenty-five-row text screen;
 * and a guest that asks 26h or reads a position is answered for a 640x480 screen
 * that is not on the machine. Measured with p_mouse.asm against the real MOUSE.COM
 * on 6.22: reset put our pointer at y=240 where the driver says 96, and the
 * MOUSEI33 line said `text=0 mkind=0 fh=0 vmaxy=1df` -- mode 3, and none of it
 * reaching the arithmetic.
 *
 * g_Video.gw/gh are the MODE's extent, set by the mode set itself and always
 * populated (VddVideoReset seeds them), which is also what the real driver keys
 * off: it hooks INT 10h and rebuilds its screen when the mode changes.
 */
UINT I33Width(VOID)
{
    return g_Video.GraphicsWidth ? g_Video.GraphicsWidth : I33_DEFAULT_WIDTH;
}

UINT I33Height(VOID)
{
    return g_Video.GraphicsHeight ? g_Video.GraphicsHeight : I33_DEFAULT_HEIGHT;
}

INT I33XShift(VOID)
{
    UINT width = I33Width();
    return (width <= I33_NARROW_MODE_WIDTH) ? 1 : 0;
}

LONG I33VirtualX(LONG pixelX)
{
    return pixelX << I33XShift();
}

static LONG I33PixelX(LONG virtualX)
{
    return virtualX >> I33XShift();
}

static LONG I33VirtualMaximumX(VOID)
{
    return (LONG)(I33Width() << I33XShift()) - 1;
}

/* ...AND IN A TEXT MODE THE VIRTUAL SCREEN IS 640x200, WHATEVER THE FONT:
 * The driver's text cell is 8x8 virtual pixels (8x4 in 50-line mode), so a text
 * application does `row = DX / 8` -- 0..199 over 25 rows. Our text frame is 400
 * lines tall and we handed that back raw, so every row came out DOUBLED: a click on
 * QBasic's menu bar landed two rows below it and no menu ever opened by mouse.
 */
INT  I33Text(VOID)
{
    return g_Video.ModeKind == VIDEO_KIND_TEXT && !g_Video.IsVesa && I33Height() > I33_TEXT_VIRTUAL_HEIGHT;
}

LONG I33VirtualY(LONG pixelY)
{
    return I33Text() ? pixelY * I33_TEXT_VIRTUAL_HEIGHT / (LONG)I33Height() : pixelY;
}

static LONG I33PixelY(LONG virtualY)
{
    return I33Text() ? virtualY * (LONG)I33Height() / I33_TEXT_VIRTUAL_HEIGHT : virtualY;
}

LONG I33VirtualMaximumY(VOID)
{
    return I33Text() ? I33_TEXT_VIRTUAL_MAX_Y : (LONG)I33Height() - 1;
}

/* ...AND A TEXT POSITION IS A CELL, NOT A PIXEL:
 * The real driver quantises to its 8x8 virtual cell in a text mode: measured on
 * 6.22, set-position 100,50 reads back as 96,48 and 101,51 reads back as 96,48
 * too. A host that returns the exact value it was handed disagrees with the
 * driver about which cell the pointer is in, which is the whole question a text
 * UI asks. Snap BEFORE clamping, so snapping can never push the pointer outside
 * a range the guest set with 07h/08h.
 */
static LONG I33Snapshot(LONG value)
{
    return I33Text() ? (value & ~(LONG)I33_TEXT_CELL_MASK) : value;
}

/* 07h/08h cursor ranges, in VIRTUAL coordinates. -1 = the guest never set one, so the
 * mode's own extent applies; a guest that sets a range while in one mode and then
 * changes mode keeps its range, which is what the real driver does.
 */
static volatile LONG g_MouseMinimumX = -1, g_MouseMaximumX = -1, g_MouseMinimumY = -1, g_MouseMaximumY = -1;
/* 0Fh / 1Ah / 1Bh. The defaults are the real driver's: 8 mickeys per 8 pixels
 * horizontally, 16 vertically (a mouse moves further across a screen than down it),
 * and a 64 mickey/second double-speed threshold.
 */
static volatile LONG g_MouseMickeyX = I33_DEFAULT_MICKEYS_X, g_MouseMickeyY = I33_DEFAULT_MICKEYS_Y, g_MouseDoubleThreshold = I33_DEFAULT_DOUBLE_SPEED;
/* 1Ah/1Bh SPEED IS NOT 0Fh's RATIO. (#249):
 * 1Ah sets the horizontal/vertical SPEED (0-100, default 50) and the double-speed
 * threshold ON THE SAME 0-100 SCALE (default 50); 0Fh sets the mickey-to-pixel
 * RATIO and 13h the threshold in MICKEYS/SECOND (default 64). Every arm used to
 * write the one set of variables, so a guest that set a speed and then a ratio read
 * its ratio back from 1Bh. Measured (p_mouse2): MOUSE.COM 6.24 and DOSBox-X agree
 * -- after a reset 1Bh says 50/50/50, after 1Ah 40/60/32 + 0Fh 8/16 it still says
 * 40/60/32, and 13h DX=100 leaves 1Bh's DX at 32.
 *
 * [CAUTION]: None of them is APPLIED -- the pointer follows the host cursor and 0Bh reports
 * device counts scaled by msens; see docs/inventory/mouse.md (N/A).
 */
static volatile LONG g_MouseSpeedX = I33_DEFAULT_SPEED, g_MouseSpeedY = I33_DEFAULT_SPEED, g_MouseSpeedDouble = I33_DEFAULT_SPEED;
/* THE v7/v8 STATE THAT 25h-34h REPORT (#249):
 * 09h's hot spot (2Ah reads it back), whether 0Ah asked for the HARDWARE text cursor
 * and its scan lines (25h bits 13-12, 27h AX/BX), 1Dh's display page and 1Ch's
 * report rate (25h bits 11-8). Each is reset by 00h/21h like the rest.
 */
static volatile LONG g_MouseHotX = 0, g_MouseHotY = 0;
static volatile LONG g_MouseTcHardware = 0, g_MouseTcHardwareLow = 0, g_MouseTcHardwareHigh = 0;
static volatile LONG g_MousePage = 0;
static volatile LONG g_MouseRate = I33_DEFAULT_RATE;     /* 1Ch code: 3 = 100 reports/s, the PS/2 aux default */
DWORD         g_MouseEventInstalls;
/* [CAUTION]: 2 s WAS WRONG. "In flight" lasts until the handler's RETF reaches our stub, and the
 * exec loop only notices that at its next pass -- for a guest that runs natively for
 * seconds between traps that is seconds, not milliseconds. Timing out early marks the
 * callback lost, so when the RETF does arrive the stub is "stray", steps over its BOP
 * and IRETs on a stack that holds no frame: a jump into garbage.
 */
#define MS_CB_TIMEOUT_MS    30000u
volatile LONG g_MouseEventPend;     /* event bits raised since the last callback (any-pending flag) */
/* ONE CALLBACK PER EVENT, IN ORDER, WITH THE BUTTON STATE OF THAT MOMENT:
 * The first cut OR-ed the event bits into one word and delivered them in a single
 * call. A click is a press and a release; both bits arrived together with BX = the
 * CURRENT state (released), so the handler saw "pressed" with no button down -- a
 * click that never happened. The driver on real hardware calls the handler once
 * per mouse packet, and that is what a program's own event queue expects. Ring of
 * 32; motion is coalesced into a pending motion-only entry, buttons never are.
 */
#define MS_EVQ  32
static MOUSE_EVENT_ENTRY g_MouseEventQueue[MS_EVQ];
static volatile LONG g_MouseEventQueueHead, g_MouseEventQueueTail;   /* UI pushes at head, exec pops at tail */
static DWORD g_MouseEventQueueDropped;
static DWORD  g_MouseCallbackSince;            /* GetTickCount()|1 when it went in flight */
static struct
{
    DWORD Eax, Ebx, Ecx, Edx, Esi, Edi, Ebp, Esp, Eip, Eflags, Cs, Ds, Es, Ss;
} g_MouseCallbackSaved;
VOID MouseEventRaise(LONG bits)
{
    LONG head = g_MouseEventQueueHead, tail = g_MouseEventQueueTail;
    ++g_MouseEventRaised;
    /* Motion after motion, not yet delivered: update the queued entry in place. */
    if (bits == 1 && head != tail)
    {
        LONG last = (head + MS_EVQ - 1) % MS_EVQ;
        if (g_MouseEventQueue[last].Bits == 1)
        {
            g_MouseEventQueue[last].X = g_MouseX; g_MouseEventQueue[last].Y = g_MouseY; g_MouseEventQueue[last].Buttons = g_MouseButtons;
            (VOID)__sync_fetch_and_or((LONG *)&g_MouseEventPend, bits);
            return;
        }
    }
    if ((head + 1) % MS_EVQ == tail) /* full: drop the newest */
    {
        ++g_MouseEventQueueDropped;
        return;
    }
    g_MouseEventQueue[head].Bits = bits; g_MouseEventQueue[head].Buttons = g_MouseButtons;
    g_MouseEventQueue[head].X = g_MouseX;  g_MouseEventQueue[head].Y = g_MouseY;
    g_MouseEventQueueHead = (head + 1) % MS_EVQ;
    (VOID)__sync_fetch_and_or((LONG *)&g_MouseEventPend, bits);
}

/* Record a button transition. Normally UI-thread only, and the position is taken
 * from the live driver position rather than the message's client coordinates because
 * while captured it is WM_INPUT, not WM_MOUSEMOVE, that owns it.
 *
 * [CAUTION]: HostMouseButton below is a SECOND writer, from the scripted-input thread. It
 * is why g_MouseButtons is updated there with a compare-exchange loop rather than the plain
 * exchange the window procedure can afford.
 */
VOID MouseButtonEdges(LONG prev, LONG now)
{
    INT index;
    for (index = 0; index < MS_BTNS; ++index)
    {
        LONG bit = 1L << index;
        if ((prev ^ now) & bit)
        {
            ++g_MouseEdges;
            if (now & bit) { InterlockedIncrement(&g_MousePressCount[index]);
                             g_MousePressX[index] = g_MouseX; g_MousePressY[index] = g_MouseY;
                             MouseEventRaise(1L << (2 * index + 1)); }       /* press event */
            else           { InterlockedIncrement(&g_MouseReleaseCount[index]);
                             g_MouseReleaseX[index]   = g_MouseX; g_MouseReleaseY[index]   = g_MouseY;
                             MouseEventRaise(1L << (2 * index + 2)); }       /* release event */
        }
    }
}

/* Press or release a button from the keys.txt script -- the `m0` token. Takes the
 * same two steps the window procedure takes, so the guest cannot tell the difference:
 * the level goes into g_MouseButtons and the EDGE goes into the press/release counters that
 * INT 33h 05h/06h report. A compare-exchange loop, not an exchange, because the UI
 * thread is writing the same word from a real mouse.
 */
VOID HostMouseButton(INT button, INT down)
{
    LONG bit;
    if (button < 0 || button >= MS_BTNS) return;
    bit = 1L << button;
    for (;;)
    {
        LONG prev = g_MouseButtons;
        LONG now  = down ? (prev | bit) : (prev & ~bit);
        if (InterlockedCompareExchange(&g_MouseButtons, now, prev) != prev) continue;
        if (prev != now) MouseButtonEdges(prev, now);
        return;
    }
}

VOID MouseChildExited(VOID)
{
    InterlockedExchange(&g_MouseWantCapture, 0);
    InterlockedExchange(&g_MouseAutoCaptureDone, 0);
    InterlockedExchange(&g_MouseWantRelease, 1);
}

INT CaptureAllowed(VOID)
{
    return g_MouseWantCapture != 0 && !g_MouseSeamless;
}

/* RULE 6 (see the capture rules above InputCaptureSet): does host mouse input reach
 * the guest right now? Captured: yes. Never used the mouse: yes (ordinary window).
 * Uses the mouse but released: no. Read on the UI thread only.
 */
INT MouseGoesToGuest(VOID)
{
    return g_Captured || !CaptureAllowed();
}

DWORD g_MouseShapeSets;      /* 09h (and 0Ah BX=1): cursor shapes defined */
/* 09h's BITMAP IS DRAWN NOW (#264) (Importance = 1):
 * It used to be "accepted and discarded" and the host arrow drawn regardless, so a
 * game's crosshair or hand was never seen. Now a defined shape replaces the arrow,
 * applied with the driver's own arithmetic (i33_driver.h: AND the screen mask, XOR the
 * cursor mask, hot spot on the pointer); a reset (00h/21h) puts the host arrow back,
 * as a reset puts the real driver's default arrow back.
 *
 * [CAUTION]: DOUBLE-BUFFERED, because the exec thread writes it (09h) and the UI thread reads it
 * (the present): 09h fills the buffer NOT being shown and then flips g_MouseGraphicsCursorBuffer,
 * so a present never sees half of one shape and half of another.
 */
static WORD      g_MouseGraphicsCursorScreen[2][I33_GC_ROWS], g_MouseGraphicsCursorCurrent[2][I33_GC_ROWS];
static volatile LONG g_MouseGraphicsCursorBuffer;          /* which of the two the present reads */
volatile LONG g_MouseGraphicsCursorDefined;      /* 0 = the host arrow; 1 = 09h's bitmap */
DWORD         g_MouseGraphicsCursorBadPointer;       /* 09h ES:DX we refused to read */
/* 2Bh-2Eh / 33h: THE ACCELERATION PROFILES, STORED, NOT APPLIED (#265):
 * See i33_driver.h for the layout and for why the curves are never applied. Exec
 * thread only.
 */
static BYTE       g_MouseAcceleration[I33_ACC_LEN];
static LONG          g_MouseAccelerationCurrent = I33_ACC_DEFAULT;
static INT           g_MouseAccelerationOk;          /* g_MouseAcceleration holds the defaults or a 2Bh load */
DWORD         g_MouseAccelerationCalls;       /* 2Bh-2Eh/33h/34h answered -- STAGE2 evidence */
/* 18h/19h: THE SHIFT-QUALIFIED HANDLERS, AND NOW THEY ARE CALLED (#265):
 * They were refused (AX=FFFFh) because nothing delivered them. MouseEventQueueTake() now
 * picks, per event, between these and 0Ch's handler by the BDA shift state -- the
 * one picker both delivery paths (V86 MouseCallbackTry, PM DpmiInjectPmMouseCallback) use.
 */
static I33_ALTERNATE       g_MouseAlt[I33_ALT_N];
/* Fill the hidden buffer, then show it (see g_MouseGraphicsCursorBuffer). Does not set g_MouseGraphicsCursorDefined:
 * the caller decides whether the shape is the guest's (09h) or a restored one (17h).
 */
static VOID I33GraphicsCursorDefine(const WORD *screen, const WORD *current)
{
    INT row, buffer = (INT)((g_MouseGraphicsCursorBuffer + 1) & 1);
    for (row = 0; row < I33_GC_ROWS; ++row)
    {
        g_MouseGraphicsCursorScreen[buffer][row] = screen[row];
        g_MouseGraphicsCursorCurrent[buffer][row] = current[row];
    }
    InterlockedExchange(&g_MouseGraphicsCursorBuffer, buffer);
}

/* 0Ah BX=0: the text cursor's screen (AND) and cursor (XOR) masks over the cell's
 * (char, attr) word. The driver's defaults invert the colours and leave the character.
 */
volatile LONG g_MouseTextCursorAnd = I33_DEFAULT_TEXT_AND, g_MouseTextCursorXor = I33_DEFAULT_TEXT_XOR;
DWORD g_MouseStateBadPointer;    /* 16h/17h: ES:DX we refused to dereference */
DWORD g_MouseI33Unimplemented;      /* calls that reached `default:` -- see there */

/* Range accessors. A guest range wins; otherwise the current mode's own extent, so a
 * mode change moves the limits with it rather than pinning the pointer to whatever
 * was on screen when the driver was reset.
 */
static LONG I33RangeXMinimum(VOID)
{
    return g_MouseMinimumX >= 0 ? g_MouseMinimumX : 0;
}

static LONG I33RangeYMinimum(VOID)
{
    return g_MouseMinimumY >= 0 ? g_MouseMinimumY : 0;
}

static LONG I33RangeXMaximum(VOID)
{
    return g_MouseMaximumX >= 0 ? g_MouseMaximumX : I33VirtualMaximumX();
}

static LONG I33RangeYMaximum(VOID)
{
    return g_MouseMaximumY >= 0 ? g_MouseMaximumY : I33VirtualMaximumY();
}

LONG I33ClampX(LONG virtualX)
{ LONG low = I33RangeXMinimum(), high = I33RangeXMaximum();
  return virtualX < low ? low : (virtualX > high ? high : virtualX); }
LONG I33ClampY(LONG virtualY)
{ LONG low = I33RangeYMinimum(), high = I33RangeYMaximum();
  return virtualY < low ? low : (virtualY > high ? high : virtualY); }
/* 07h/08h hand over CX and DX and the driver takes them EITHER WAY ROUND -- passing
 * max first is common enough that a driver which honoured the order literally would
 * pin the pointer to a single coordinate. Swap rather than reject.
 */
static VOID I33SetRange(volatile LONG *low, volatile LONG *high, LONG first, LONG second)
{
    if (first > second)
    {
        LONG swap = first;
        first = second;
        second = swap;
    }
    if (first < 0) first = 0;
    if (second < first) second = first;
    InterlockedExchange(low, first); InterlockedExchange(high, second);
}

/* 15h/16h/17h state block. The layout is OURS -- the guest is told the size by 15h
 * and only ever hands the same buffer back to 17h, so nothing outside this file
 * reads it. Versioned so a restore cannot be fed a block from an older build.
 */
#define I33_STATE_MAGIC 0x4133564EuL       /* 'NV3A' (#264/#265 added the cursor bitmap,
                                              the alternate handlers and the profile) */
typedef struct
{
    DWORD Magic;
    LONG  X, Y, Hidden;
    LONG  MinimumX, MaximumX, MinimumY, MaximumY;
    LONG  MickeyX, MickeyY, Double;
    LONG  EventMask, EventSegment, EventOffset;
    LONG  SpeedX, SpeedY, SpeedDouble, HotX, HotY, Page, Rate;
    LONG  GraphicsCursorDefined, AccelerationCurrent;
    WORD  GraphicsCursorScreen[I33_GC_ROWS], GraphicsCursorCurrent[I33_GC_ROWS];
    I33_ALTERNATE Alt[I33_ALT_N];
} I33_STATE;

static VOID I33StateSave(volatile BYTE *destination)
{
    I33_STATE state;
    state.Magic = I33_STATE_MAGIC;
    state.X = g_MouseX; state.Y = g_MouseY; state.Hidden = g_MouseHidden;
    state.MinimumX = g_MouseMinimumX; state.MaximumX = g_MouseMaximumX;
    state.MinimumY = g_MouseMinimumY; state.MaximumY = g_MouseMaximumY;
    state.MickeyX = g_MouseMickeyX; state.MickeyY = g_MouseMickeyY; state.Double = g_MouseDoubleThreshold;
    state.EventMask = g_MouseEventMask; state.EventSegment = g_MouseEventSegment; state.EventOffset = g_MouseEventOffset;
    state.SpeedX = g_MouseSpeedX; state.SpeedY = g_MouseSpeedY; state.SpeedDouble = g_MouseSpeedDouble; state.HotX = g_MouseHotX; state.HotY = g_MouseHotY;
    state.Page = g_MousePage; state.Rate = g_MouseRate;
    state.GraphicsCursorDefined = g_MouseGraphicsCursorDefined; state.AccelerationCurrent = g_MouseAccelerationCurrent;
    { INT row, buffer = (INT)(g_MouseGraphicsCursorBuffer & 1);
      for (row = 0; row < I33_GC_ROWS; ++row)
      {
          state.GraphicsCursorScreen[row] = g_MouseGraphicsCursorScreen[buffer][row];
          state.GraphicsCursorCurrent[row] = g_MouseGraphicsCursorCurrent[buffer][row];
      }
      }
    memcpy(state.Alt, g_MouseAlt, sizeof state.Alt);
    { UINT index; const BYTE *bytes = (const BYTE *)&state;
      for (index = 0; index < sizeof state; ++index) destination[index] = bytes[index]; }
}

static VOID I33StateLoad(volatile BYTE *source)
{
    I33_STATE state;
    { UINT index; BYTE *bytes = (BYTE *)&state;
      for (index = 0; index < sizeof state; ++index) bytes[index] = source[index]; }
    if (state.Magic != I33_STATE_MAGIC)
    {
        ++g_MouseStateBadPointer;
        return;
    }
    InterlockedExchange(&g_MouseX, state.X);       InterlockedExchange(&g_MouseY, state.Y);
    InterlockedExchange(&g_MouseHidden, state.Hidden);
    InterlockedExchange(&g_MouseMinimumX, state.MinimumX); InterlockedExchange(&g_MouseMaximumX, state.MaximumX);
    InterlockedExchange(&g_MouseMinimumY, state.MinimumY); InterlockedExchange(&g_MouseMaximumY, state.MaximumY);
    InterlockedExchange(&g_MouseMickeyX, state.MickeyX);
    InterlockedExchange(&g_MouseMickeyY, state.MickeyY);
    InterlockedExchange(&g_MouseDoubleThreshold, state.Double);
    InterlockedExchange(&g_MouseEventMask, state.EventMask);
    InterlockedExchange(&g_MouseEventSegment, state.EventSegment);
    InterlockedExchange(&g_MouseEventOffset, state.EventOffset);
    InterlockedExchange(&g_MouseSpeedX, state.SpeedX); InterlockedExchange(&g_MouseSpeedY, state.SpeedY); InterlockedExchange(&g_MouseSpeedDouble, state.SpeedDouble);
    InterlockedExchange(&g_MouseHotX, state.HotX); InterlockedExchange(&g_MouseHotY, state.HotY);
    InterlockedExchange(&g_MousePage, state.Page);   InterlockedExchange(&g_MouseRate, state.Rate);
    I33GraphicsCursorDefine(state.GraphicsCursorScreen, state.GraphicsCursorCurrent);
    InterlockedExchange(&g_MouseGraphicsCursorDefined, state.GraphicsCursorDefined ? 1 : 0);
    if (state.AccelerationCurrent >= 1 && state.AccelerationCurrent <= I33_ACC_N) g_MouseAccelerationCurrent = state.AccelerationCurrent;
    memcpy(g_MouseAlt, state.Alt, sizeof g_MouseAlt);
}

/* Resolve a guest ES:DX to something we may touch. The segment means different things
 * on the three entry paths -- a real-mode paragraph in V86 and through 0300, an LDT
 * selector in PM -- and NEITHER may be dereferenced on trust: this address comes from
 * a guest register, and 16h/17h are exactly where a wrong one would fault the host
 * rather than the guest. MemoryReadable is the same guard the call-site capture uses.
 */
static volatile BYTE *I33GuestPointer(volatile BYTE *tib, INT source,
                                    WORD segment, DWORD offset, SIZE_T length, INT forWrite)
{
    ULONG_PTR linear;
    (VOID)tib;
    linear = (source == I33_SRC_PM) ? (ULONG_PTR)(DpmiSelectorBase(segment) + offset)
                              : (ULONG_PTR)(((DWORD)segment << PARAGRAPH_SHIFT) + offset);
    if (!linear || !MemoryReadable(linear, length)) return NULL;
    /* 16h WRITES, and MemoryReadable says nothing about that -- a read-only page passes
     * it and then faults the HOST on the first store. Ask separately rather than
     * assume a guest buffer is writable because it usually is.
     */
    if (forWrite)
    {
        MEMORY_BASIC_INFORMATION memoryInfo;
        if (VirtualQuery((LPCVOID)linear, &memoryInfo, sizeof memoryInfo) != sizeof memoryInfo) return NULL;
        if (!(memoryInfo.Protect & (PAGE_READWRITE | PAGE_WRITECOPY
                          | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
            return NULL;
    }
    return (volatile BYTE *)linear;
}

/* 00h and 21h both reset. Everything the driver owns goes back to its power-on value
 * -- and that includes the things the old 00h arm left standing: the ranges, the
 * sensitivity, the event handler and the button counts. A guest that resets and then
 * asks "how many clicks" must be told none, not however many it ignored earlier.
 */
VOID I33ResetState(VOID)
{
    INT index;
    InterlockedExchange(&g_MouseHidden, 1);               /* hidden until Show */
    InterlockedExchange(&g_MouseMinimumX, -1); InterlockedExchange(&g_MouseMaximumX, -1);
    InterlockedExchange(&g_MouseMinimumY, -1); InterlockedExchange(&g_MouseMaximumY, -1);
    InterlockedExchange(&g_MouseMickeyX, I33_DEFAULT_MICKEYS_X); InterlockedExchange(&g_MouseMickeyY, I33_DEFAULT_MICKEYS_Y);
    InterlockedExchange(&g_MouseDoubleThreshold, I33_DEFAULT_DOUBLE_SPEED);
    InterlockedExchange(&g_MouseEventMask, 0);
    InterlockedExchange(&g_MouseEventSegment, 0);
    InterlockedExchange(&g_MouseEventOffset, 0);
    InterlockedExchange(&g_MouseTextCursorAnd, I33_DEFAULT_TEXT_AND); InterlockedExchange(&g_MouseTextCursorXor, I33_DEFAULT_TEXT_XOR);
    InterlockedExchange(&g_MouseSpeedX, I33_DEFAULT_SPEED); InterlockedExchange(&g_MouseSpeedY, I33_DEFAULT_SPEED); InterlockedExchange(&g_MouseSpeedDouble, I33_DEFAULT_SPEED);
    InterlockedExchange(&g_MouseHotX, 0);  InterlockedExchange(&g_MouseHotY, 0);
    InterlockedExchange(&g_MouseTcHardware, 0);
    InterlockedExchange(&g_MousePage, 0);   InterlockedExchange(&g_MouseRate, I33_DEFAULT_RATE);
    /* #264/#265: the default cursor (the host arrow), no alternate handlers, the default
     * profiles. [CAUTION] That a reset clears 18h's handlers and the loaded profiles is the
     * reading "everything the driver owns goes back to power-on" -- UNMEASURED.
     */
    InterlockedExchange(&g_MouseGraphicsCursorDefined, 0);
    ZeroMemory(g_MouseAlt, sizeof g_MouseAlt);
    I33AccelerationDefaults(g_MouseAcceleration); g_MouseAccelerationCurrent = I33_ACC_DEFAULT; g_MouseAccelerationOk = 1;
    for (index = 0; index < MS_BTNS; ++index)
    {
        InterlockedExchange(&g_MousePressCount[index], 0);
        InterlockedExchange(&g_MouseReleaseCount[index], 0);
    }
    InterlockedExchange(&g_MouseDx, 0); InterlockedExchange(&g_MouseDy, 0);
    /* A RESET PUTS THE POINTER IN THE MIDDLE OF THE SCREEN:
     * We left it wherever it happened to be -- which, before any mouse has moved,
     * is the startup value 320,240. In a twenty-five-row text screen that is row
     * 30: off the bottom, and off the 640x200 virtual screen entirely. Measured on
     * 6.22: after AX=0000 in mode 3 the real driver reports 320,96 -- the centre,
     * snapped to its cell grid. The ranges are cleared just above, so the extent
     * used here is the mode's own.
     */
    {   LONG centreX = I33ClampX(I33Snapshot((I33VirtualMaximumX() + 1) / 2));
        LONG centreY = I33ClampY(I33Snapshot((I33VirtualMaximumY() + 1) / 2));
        InterlockedExchange(&g_MouseX, I33PixelX(centreX));
        InterlockedExchange(&g_MouseY, I33PixelY(centreY)); }
}

/* The mickeys moved since the last read, DRAINED -- 0Bh's arithmetic, lifted out
 * unchanged so 27h (#249) reads the same counters the same way: the driver has ONE
 * pair of motion accumulators and both calls reset it. Clamped to signed 16 bits
 * (a guest reads them as such; a big sweep must not wrap round and turn the player
 * the wrong way). x/y = the pointer now, for the no-raw-input fallback.
 */
static VOID I33TakeMotion(LONG positionX, LONG positionY, LONG *outDeltaX, LONG *outDeltaY)
{
    static LONG lastX = I33_FALLBACK_X, lastY = I33_FALLBACK_Y;             /* fallback only (exec thread) */
    LONG deltaX, deltaY;
    if (g_MouseRawOk)                                    /* the device's own counts */
    {
        deltaX = InterlockedExchange(&g_MouseDx, 0);
        deltaY = InterlockedExchange(&g_MouseDy, 0);
        deltaX = deltaX * g_MouseSensitivity / PERCENT; deltaY = deltaY * g_MouseSensitivity / PERCENT;
    }
    else                                              /* fallback: absolute-derived */
    {
        deltaX = (positionX - lastX) * I33_ABSOLUTE_DELTA_SCALE; deltaY = (positionY - lastY) * I33_ABSOLUTE_DELTA_SCALE;
        lastX = positionX; lastY = positionY;
    }
    if (deltaX >  INT16_MAX_VALUE) deltaX =  INT16_MAX_VALUE; else if (deltaX < INT16_MIN_VALUE) deltaX = INT16_MIN_VALUE;
    if (deltaY >  INT16_MAX_VALUE) deltaY =  INT16_MAX_VALUE; else if (deltaY < INT16_MIN_VALUE) deltaY = INT16_MIN_VALUE;
    *outDeltaX = deltaX; *outDeltaY = deltaY;
}

/* 32h's ANSWER: WHICH OF 25h-34h THIS DRIVER REALLY SERVICES. (#249):
 * Bit 15 = 25h ... bit 0 = 34h (RBIL INT 33h AX=0032h). We report 8.00 in 24h, so a
 * guest may call any of them -- and 32h is the call a careful one makes FIRST. It
 * used to fall into `default:` and hand the guest its own AX (0032h) back as the
 * mask: "26h and 2Fh exist", from a driver that had neither. A bit is set ONLY where
 * the arm below gives the documented answer:
 * 25h 26h 27h 2Ah 2Fh 30h 31h 32h  -> set
 * 28h/29h  answered ("cannot set" / "no modes to list"), but no mode list exists,
 *          so not claimed;
 * 2Bh-2Eh acceleration profiles, 33h switch settings, 34h MOUSE.INI -> set since
 *          #265: answered with the documented shapes (the profiles are stored and
 *          handed back, NOT applied -- see i33_driver.h). E43Ch -> E7FFh.
 */
#define I33_ACTIVE_FNS  (0x8000u | 0x4000u | 0x2000u | 0x0400u    /* 25h 26h 27h 2Ah */ \
                       | 0x0200u | 0x0100u | 0x0080u | 0x0040u    /* 2Bh 2Ch 2Dh 2Eh */ \
                       | 0x0020u | 0x0010u | 0x0008u | 0x0004u    /* 2Fh 30h 31h 32h */ \
                       | 0x0002u | 0x0001u)                       /* 33h 34h */

/* -- WHERE A POINTER INTO THE DRIVER GOES (#265). 2Ch/2Dh/34h hand back ES:SI / ES:DX
 * at the driver's own bytes (VDD_MOUSE_SEG). A real-mode caller (V86, or a DPMI 0300h
 * excursion) gets the paragraph; a protected-mode caller cannot use a paragraph in ES
 * -- loading it would fault -- so it gets a data selector over the same 64 KB
 * (DpmiSegmentToDescriptor, cached: one LDT slot for the life of the VDM), and the offset
 * goes into the full 32-bit register so a flat client reading ES:ESI sees no junk in
 * the top half. [CAUTION] A real driver under DOS/4GW never sees this question -- the
 * extender translates the calls it knows and passes the rest down; ours is the shape
 * a client that issues the INT in PM would most usefully get. UNMEASURED.
 */
static volatile BYTE *I33DriverData(VOID)
{
    return (volatile BYTE *)VddMapFlat(&g_Bus, VDD_MOUSE_SEG, 0);
}

static VOID I33ResultPointer(volatile BYTE *tib, INT source, INT offsetRegister, WORD offset)
{
    if (source == I33_SRC_PM)
    {
        VDM_SET16(tib, VTIB_ES, DpmiSegmentToDescriptor(VDD_MOUSE_SEG));
        VDM_REG(tib, offsetRegister) = offset;
    }
    else
    {
        VDM_SET16(tib, VTIB_ES, VDD_MOUSE_SEG);
        VDM_SET16(tib, offsetRegister, offset);
    }
}

static VOID I33AccelerationReady(VOID)
{
    if (!g_MouseAccelerationOk)
    {
        I33AccelerationDefaults(g_MouseAcceleration);
        g_MouseAccelerationCurrent = I33_ACC_DEFAULT;
        g_MouseAccelerationOk = 1;
    }
}

/* INT 33h mouse driver (functions DOS apps actually use). The host draws the
 * cursor (overlay in the present path) when the hide-count is 0, so apps that
 * rely on the driver cursor (the common case) get a visible pointer.
 */
VOID MouseInt33(volatile BYTE *tib, INT source)
{
    DWORD ax = VDM_REG16(tib, VTIB_EAX);
    LONG positionX = g_MouseX, positionY = g_MouseY, buttons = g_MouseButtons;
    if (ax < ARRAYSIZE(g_MouseI33)) g_MouseI33[ax]++; else g_MouseI33Other++;
    /* The real histogram, and the caller. See the commentary on g_MouseI33Ax. */
    {   DWORD cs  = VDM_REG16(tib, VTIB_CS);
        DWORD eip = VDM_REG(tib, VTIB_EIP);
        /* An EIP is only 16 bits wide when its code selector is -- so mask it in V86
         * and NEVER in PM, where a flat selector makes EIP the address itself.
         */
        DWORD linear = (source == I33_SRC_V86) ? ((cs << PARAGRAPH_SHIFT) + (eip & WORD_MASK))
                                         : (DpmiSelectorBase((WORD)cs) + eip);
        UINT index;
        for (index = 0; index < I33_AXN && g_MouseI33Ax[index].Count; ++index)
            if (g_MouseI33Ax[index].Ax == (WORD)ax) break;
        if (index < I33_AXN)
        {
            g_MouseI33Ax[index].Ax = (WORD)ax;
            ++g_MouseI33Ax[index].Count;
        }
        else ++g_MouseI33AxOverflow;
        for (index = 0; index < g_MouseI33SiteCount; ++index) if (g_MouseI33Site[index].Linear == linear) break;
        if (index < I33_SITEN)
        {
            if (index == g_MouseI33SiteCount)                    /* first call from this site */
            {
                g_MouseI33Site[index].Linear = linear; g_MouseI33Site[index].Cs = (WORD)cs;
                g_MouseI33Site[index].Eip = eip; g_MouseI33Site[index].Ax = (WORD)ax;
                g_MouseI33Site[index].Source = (BYTE)source;
                /* The bytes AROUND the site, so the diff against DOOM.EXE needs no
                 * second run. Guarded: an address derived from a guest register is
                 * not an address we may dereference on trust.
                 */
                if (MemoryReadable((ULONG_PTR)(linear - I33_SITE_CONTEXT_BEFORE), sizeof g_MouseI33Site[index].Context))
                {
                    UINT byteIndex;
                    for (byteIndex = 0; byteIndex < sizeof g_MouseI33Site[index].Context; ++byteIndex)
                        g_MouseI33Site[index].Context[byteIndex] = ((volatile BYTE *)(ULONG_PTR)(linear - I33_SITE_CONTEXT_BEFORE))[byteIndex];
                }
                ++g_MouseI33SiteCount;
            }
            ++g_MouseI33Site[index].Count;
        }
        else ++g_MouseI33SiteOverflow;
    }
    /* The guest is USING the mouse, not merely asking whether one exists -- so take
     * it into the window. See the note on g_MouseWantCapture for why 0x00 is not in
     * this set and why the UI thread is the one that acts.
     */
    /* - 0Ch/14h JOIN THE SET (s64). Installing an event handler is the single most
     * explicit way a guest can say "the mouse is mine" -- more so than polling --
     * and a menu-driven program that only ever waits on its callback would
     * otherwise never trigger the grab at all. 07h/08h (fencing the pointer into a
     * region) is the same declaration made differently. 00h stays out: detection is
     * not use, and it is the AX a mis-patched site is most likely to arrive with.
     */
    if (ax == I33_FN_SHOW_CURSOR || ax == I33_FN_GET_POSITION || ax == I33_FN_GET_PRESS_DATA || ax == I33_FN_GET_RELEASE_DATA
        || ax == I33_FN_SET_X_RANGE || ax == I33_FN_SET_Y_RANGE || ax == I33_FN_READ_MOTION
        || ax == I33_FN_SET_EVENT_HANDLER || ax == I33_FN_EXCHANGE_EVENT_HANDLER || ax == I33_FN_SET_ALTERNATE_HANDLER)
        InterlockedExchange(&g_MouseWantCapture, 1);
    switch (ax)
    {
    case I33_FN_RESET:                                        /* reset + get status */
        if (g_MouseAbsent) /* no driver */
        {
            VDM_SET16(tib, VTIB_EAX, I33_NOT_INSTALLED);
            break;
        }
        VDM_SET16(tib, VTIB_EAX, I33_INSTALLED);               /* driver installed */
        VDM_SET16(tib, VTIB_EBX, I33_BUTTON_COUNT);               /* 2 buttons */
        I33ResetState();
        break;
    case I33_FN_SHOW_CURSOR:                                        /* show cursor (dec count) */
        if (g_MouseHidden > 0) InterlockedDecrement(&g_MouseHidden);
        break;
    case I33_FN_HIDE_CURSOR:                                        /* hide cursor (inc count) */
        InterlockedIncrement(&g_MouseHidden);
        break;
    case I33_FN_GET_POSITION:                                        /* get position + buttons */
        VDM_SET16(tib, VTIB_ECX, (WORD)I33ClampX(I33Snapshot(I33VirtualX(positionX))));
        VDM_SET16(tib, VTIB_EDX, (WORD)I33ClampY(I33Snapshot(I33VirtualY(positionY))));
        VDM_SET16(tib, VTIB_EBX, (WORD)buttons);
        break;
    case I33_FN_SET_POSITION:                                        /* set cursor position */
    {
        LONG virtualX = I33ClampX(I33Snapshot((LONG)(INT16)VDM_REG16(tib, VTIB_ECX)));
        LONG virtualY = I33ClampY(I33Snapshot((LONG)(INT16)VDM_REG16(tib, VTIB_EDX)));
        InterlockedExchange(&g_MouseX, I33PixelX(virtualX));
        InterlockedExchange(&g_MouseY, I33PixelY(virtualY));
        break; }
    /* 05h / 06h: THE COUNTS, AND THE POSITION AT THE TRANSITION:
     * BX on entry selects the button (0 L, 1 R, 2 M) and it is an INPUT we used to
     * ignore entirely. AX returns the CURRENT button mask, BX the number of
     * transitions SINCE THE LAST CALL for that button, CX/DX where the last one
     * happened. The count is drained by the read -- that is what "since" means,
     * and a guest that polls in a loop must not see the same click twice.
     */
    case I33_FN_GET_PRESS_DATA: case I33_FN_GET_RELEASE_DATA:
    {
        INT      isRelease = (ax == I33_FN_GET_RELEASE_DATA);
        DWORD    button  = VDM_REG16(tib, VTIB_EBX);
        volatile LONG *counter, *pressX, *pressY;
        LONG count;
        if (button >= MS_BTNS) button = 0;                      /* driver clamps, not faults */
        counter = isRelease ? &g_MouseReleaseCount[button]   : &g_MousePressCount[button];
        pressX  = isRelease ? &g_MouseReleaseX[button]   : &g_MousePressX[button];
        pressY  = isRelease ? &g_MouseReleaseY[button]   : &g_MousePressY[button];
        count   = InterlockedExchange(counter, 0);
        if (count > MAXSHORT) count = MAXSHORT;
        VDM_SET16(tib, VTIB_EAX, (WORD)buttons);
        VDM_SET16(tib, VTIB_EBX, (WORD)count);
        /* No transition yet: the documented answer is the CURRENT position, not a
         * stale zero -- a guest that plots at CX/DX would jump to the top-left.
         */
        VDM_SET16(tib, VTIB_ECX, (WORD)I33ClampX(I33VirtualX(count ? *pressX : positionX)));
        VDM_SET16(tib, VTIB_EDX, (WORD)I33ClampY(I33VirtualY(count ? *pressY : positionY)));
        break; }
    case I33_FN_SET_X_RANGE:                                        /* set X range (virtual) */
        I33SetRange(&g_MouseMinimumX, &g_MouseMaximumX,
                      (LONG)(INT16)VDM_REG16(tib, VTIB_ECX),
                      (LONG)(INT16)VDM_REG16(tib, VTIB_EDX));
        InterlockedExchange(&g_MouseX, I33PixelX(I33ClampX(I33VirtualX(positionX))));
        break;
    case I33_FN_SET_Y_RANGE:                                        /* set Y range (virtual) */
        I33SetRange(&g_MouseMinimumY, &g_MouseMaximumY,
                      (LONG)(INT16)VDM_REG16(tib, VTIB_ECX),
                      (LONG)(INT16)VDM_REG16(tib, VTIB_EDX));
        InterlockedExchange(&g_MouseY, I33PixelY(I33ClampY(I33VirtualY(positionY))));
        break;
    case I33_FN_DEFINE_GRAPHICS_CURSOR:                                        /* define graphics cursor */
    {
        /* [INFO]: #264: THE BITMAP IS READ AND DRAWN. It was "accepted and ignored on
         * purpose" because the host arrow had nowhere to put it. ES:DX -> 16 screen-mask
         * words then 16 cursor-mask words (i33_driver.h). The hot spot (BX, CX; -16..16)
         * is kept as before (#249) -- 2Ah reports it back, and the overlay places the
         * bitmap by it. A pointer we may not read is refused and COUNTED, and the shape
         * before it stays: a cursor made of whatever an unreadable address held is worse
         * than the previous one.
         */
        volatile BYTE *guest = I33GuestPointer(tib, source, (WORD)VDM_REG16(tib, VTIB_ES),
                                         MouseI33Offset(tib, source, VDM_REG(tib, VTIB_EDX)),
                                         I33_GC_DEFINITION_SIZE, X86_ACCESS_READ);
        InterlockedExchange(&g_MouseHotX, (LONG)(SHORT)VDM_REG16(tib, VTIB_EBX));
        InterlockedExchange(&g_MouseHotY, (LONG)(SHORT)VDM_REG16(tib, VTIB_ECX));
        ++g_MouseShapeSets;
        if (!guest)
        {
            ++g_MouseGraphicsCursorBadPointer;
            break;
        }
        {   WORD screen[I33_GC_ROWS], current[I33_GC_ROWS]; INT row;
            for (row = 0; row < I33_GC_ROWS; ++row)
            {
                screen[row] = (WORD)(guest[X86_WORD_SIZE * row] | (guest[X86_WORD_SIZE * row + 1] << BYTE_SHIFT));
                current[row] = (WORD)(guest[I33_GC_CURSOR_MASK_OFFSET + X86_WORD_SIZE * row] | (guest[I33_GC_CURSOR_MASK_OFFSET + X86_WORD_SIZE * row + 1] << BYTE_SHIFT));
            }
            I33GraphicsCursorDefine(screen, current);
            InterlockedExchange(&g_MouseGraphicsCursorDefined, 1); }
        break; }
    case I33_FN_DEFINE_TEXT_CURSOR:                                        /* define text cursor */
        /* BX=0: a SOFTWARE cursor -- CX is the screen mask (AND), DX the cursor mask
         * (XOR), applied to the (char, attr) word of the cell under the pointer. That
         * is the whole text-mode pointer, and the present path now draws it that way
         * (VddVideoTextCursor). BX=1 asks for the HARDWARE cursor to be moved
         * instead; counted with the shapes and drawn as the software default, which
         * is at least a pointer in the right cell.
         */
        if (VDM_REG16(tib, VTIB_EBX) == 0)
        {
            InterlockedExchange(&g_MouseTextCursorAnd, (LONG)VDM_REG16(tib, VTIB_ECX));
            InterlockedExchange(&g_MouseTextCursorXor, (LONG)VDM_REG16(tib, VTIB_EDX));
            InterlockedExchange(&g_MouseTcHardware, 0);
        }
        else                                          /* 25h/27h report it (#249) */
        {
            InterlockedExchange(&g_MouseTcHardwareLow, (LONG)VDM_REG16(tib, VTIB_ECX));
            InterlockedExchange(&g_MouseTcHardwareHigh, (LONG)VDM_REG16(tib, VTIB_EDX));
            InterlockedExchange(&g_MouseTcHardware, 1);
            ++g_MouseShapeSets;
        }
        break;
    case I33_FN_READ_MOTION:                                        /* read relative motion */
    {
        LONG deltaX, deltaY;
        I33TakeMotion(positionX, positionY, &deltaX, &deltaY);                /* SIGNED 16-bit, drained */
        VDM_SET16(tib, VTIB_ECX, (WORD)(SHORT)deltaX);
        VDM_SET16(tib, VTIB_EDX, (WORD)(SHORT)deltaY);
        break; }
    /* -- 0Ch SET / 14h EXCHANGE the event handler. See the note on g_MouseEventMask:
     * stored and reported, NOT yet invoked. 14h must return the PREVIOUS pair or a
     * guest that chains handlers jumps to whatever we failed to tell it.
     */
    /* ES:(E)DX -- a flat PM client's handler offset is a full 32-bit linear (ZAR:
     * 0x347:0x0044xxxx, s74c); a 16-bit or V86 caller's is a word.
     */
    case I33_FN_SET_EVENT_HANDLER:
        InterlockedExchange(&g_MouseEventMask, (LONG)VDM_REG16(tib, VTIB_ECX));
        InterlockedExchange(&g_MouseEventSegment,  (LONG)VDM_REG16(tib, VTIB_ES));
        InterlockedExchange(&g_MouseEventOffset,  (LONG)MouseI33Offset(tib, source, VDM_REG(tib, VTIB_EDX)));
        ++g_MouseEventInstalls;
        break;
    case I33_FN_EXCHANGE_EVENT_HANDLER:                                        /* exchange event handler */
    {
        LONG oldMask = g_MouseEventMask, oldSegment = g_MouseEventSegment, oldOffset = g_MouseEventOffset;
        InterlockedExchange(&g_MouseEventMask, (LONG)VDM_REG16(tib, VTIB_ECX));
        InterlockedExchange(&g_MouseEventSegment,  (LONG)VDM_REG16(tib, VTIB_ES));
        InterlockedExchange(&g_MouseEventOffset,  (LONG)MouseI33Offset(tib, source, VDM_REG(tib, VTIB_EDX)));
        ++g_MouseEventInstalls;
        VDM_SET16(tib, VTIB_ECX, (WORD)oldMask);
        VDM_SET16(tib, VTIB_EDX, (WORD)oldOffset);
        VDM_SET16(tib, VTIB_ES,  (WORD)oldSegment);
        break; }
    case I33_FN_SET_MICKEY_RATIO:                                        /* mickeys per 8 pixels */
        {   LONG mickeysX = (LONG)VDM_REG16(tib, VTIB_ECX);
            LONG mickeysY = (LONG)VDM_REG16(tib, VTIB_EDX);
            if (mickeysX > 0) InterlockedExchange(&g_MouseMickeyX, mickeysX);
            if (mickeysY > 0) InterlockedExchange(&g_MouseMickeyY, mickeysY); }
        break;
    case I33_FN_CONDITIONAL_OFF:                                        /* conditional-off region */
        /* The region in which the driver hides its own cursor while the guest
         * redraws under it. We re-render the frame from VRAM every present and draw
         * the overlay on top, so there is never a cursor to erase -- honouring this
         * would change nothing visible. Accepted deliberately.
         */
        break;
    case I33_FN_SET_DOUBLE_SPEED:                                        /* double-speed threshold */
        InterlockedExchange(&g_MouseDoubleThreshold, (LONG)VDM_REG16(tib, VTIB_EDX));
        break;
    case I33_FN_GET_STATE_SIZE:                                        /* get state buffer size */
        VDM_SET16(tib, VTIB_EBX, (WORD)sizeof(I33_STATE));
        break;
    case I33_FN_SAVE_STATE:                                        /* save state -> ES:DX */
    case I33_FN_RESTORE_STATE:                                        /* restore state <- ES:DX */
    {
        volatile BYTE *guest = I33GuestPointer(tib, source,
                                         (WORD)VDM_REG16(tib, VTIB_ES),
                                         (WORD)VDM_REG16(tib, VTIB_EDX),
                                         sizeof(I33_STATE), ax == I33_FN_SAVE_STATE);
        if (!guest) /* refuse, do not fault */
        {
            ++g_MouseStateBadPointer;
            break;
        }
        if (ax == I33_FN_SAVE_STATE) I33StateSave(guest); else I33StateLoad(guest);
        break; }
    case I33_FN_SET_SENSITIVITY:                                        /* set sensitivity (SPEED) */
        /* The speeds, NOT 0Fh's mickey ratio -- see g_MouseSpeedX. */
        InterlockedExchange(&g_MouseSpeedX, (LONG)VDM_REG16(tib, VTIB_EBX));
        InterlockedExchange(&g_MouseSpeedY, (LONG)VDM_REG16(tib, VTIB_ECX));
        InterlockedExchange(&g_MouseSpeedDouble, (LONG)VDM_REG16(tib, VTIB_EDX));
        break;
    case I33_FN_GET_SENSITIVITY:                                        /* get sensitivity */
        VDM_SET16(tib, VTIB_EBX, (WORD)g_MouseSpeedX);
        VDM_SET16(tib, VTIB_ECX, (WORD)g_MouseSpeedY);
        VDM_SET16(tib, VTIB_EDX, (WORD)g_MouseSpeedDouble);
        break;
    /* 1Ch, 1Dh/1Eh, 22h/23h: THE SMALL SETTINGS, ANSWERED. (#249):
     * All four used to reach `default:` -- so 1Eh "get page" and 23h "get language"
     * handed the caller's own BX back as the answer. 1Ch's rate is reported by 25h;
     * 1Dh's page is stored and read back (the pointer is drawn by the host on
     * whatever is displayed, so there is no per-page cursor to move). 22h is
     * accepted and 23h says 0, English: this is the US driver, whose messages are
     * not translated -- the same answer the US MOUSE.COM gives whatever it was told.
     */
    case I33_FN_SET_INTERRUPT_RATE:                                        /* set interrupt rate */
        {   LONG rate = (LONG)VDM_REG16(tib, VTIB_EBX);
            if (rate <= I33_RATE_MAX) InterlockedExchange(&g_MouseRate, rate); }
        break;
    case I33_FN_SET_DISPLAY_PAGE:                                        /* set display page */
        InterlockedExchange(&g_MousePage, (LONG)VDM_REG16(tib, VTIB_EBX));
        break;
    case I33_FN_GET_DISPLAY_PAGE:                                        /* get display page */
        VDM_SET16(tib, VTIB_EBX, (WORD)g_MousePage);
        break;
    case I33_FN_SET_LANGUAGE:                                        /* set language */
        break;
    case I33_FN_GET_LANGUAGE:                                        /* get language: English */
        VDM_SET16(tib, VTIB_EBX, I33_LANGUAGE_ENGLISH);
        break;
    /* 0Dh/0Eh light-pen emulation on/off: no outputs, and there is no light pen for
     * INT 10h AH=04h to report -- nothing to switch. Named so it is not counted as
     * an unknown call.
     */
    case I33_FN_LIGHT_PEN_ON: case I33_FN_LIGHT_PEN_OFF:
        break;
    /* 18h/19h ALTERNATE (SHIFT-QUALIFIED) HANDLERS. (#265):
     * 18h answers AX=0018h on success, FFFFh on error. Through `default:` the
     * caller's own 0018h came back -- "installed" -- for a handler nothing called;
     * #249 made that an honest FFFFh; now MouseEventQueueTake() delivers them, so they
     * install. 18h refuses a mask with no Shift/Ctrl/Alt bit and a fourth
     * combination. 19h: BX:DX = the handler for CX's shift combination, CX = its whole
     * mask; CX=0 = none (BX/DX left alone). The rules, and which of them are our
     * reading rather than a measurement, are in i33_driver.h.
     */
    case I33_FN_SET_ALTERNATE_HANDLER:
        if (I33AlternateSet(g_MouseAlt, (WORD)VDM_REG16(tib, VTIB_ECX),
                        (WORD)VDM_REG16(tib, VTIB_ES),
                        (UINT32)MouseI33Offset(tib, source, VDM_REG(tib, VTIB_EDX))))
        {
            VDM_SET16(tib, VTIB_EAX, I33_FN_SET_ALTERNATE_HANDLER);
            ++g_MouseEventInstalls;
        }
        else VDM_SET16(tib, VTIB_EAX, I33_FAILED);
        break;
    case I33_FN_GET_ALTERNATE_HANDLER:
    {
        INT byteIndex = I33AlternateFind(g_MouseAlt, (WORD)VDM_REG16(tib, VTIB_ECX));
        if (byteIndex < 0)
        {
            VDM_SET16(tib, VTIB_ECX, I33_NO_ALTERNATE_HANDLER);
            break;
        }
        VDM_SET16(tib, VTIB_ECX, g_MouseAlt[byteIndex].Mask);
        VDM_SET16(tib, VTIB_EBX, g_MouseAlt[byteIndex].Segment);
        if (source == I33_SRC_PM) VDM_REG(tib, VTIB_EDX) = g_MouseAlt[byteIndex].Offset;
        else VDM_SET16(tib, VTIB_EDX, (WORD)g_MouseAlt[byteIndex].Offset);
        break; }
    /* -- 20h ENABLE IS NOT A RESET. (#249) It shared 21h's arm, so enabling the driver
     * wiped the ranges, the handler and the counts, and answered AX=FFFFh. Measured
     * (p_mouse2 i33.20.*): MOUSE.COM 6.24 and DOSBox-X both return AX untouched
     * (0020h) and keep a 07h fence across it. We are never disabled (1Fh refuses), so
     * there is nothing to re-enable.
     */
    case I33_FN_ENABLE_DRIVER:                                        /* enable driver */
        break;
    case I33_FN_SOFTWARE_RESET:                                        /* software reset */
        /* 21h differs from 00h: it does NOT re-probe the hardware, and it answers in
         * AX/BX the same way. We have no hardware to re-probe, so the two are the
         * same action here -- but say so, rather than letting 21h fall through to a
         * `default:` that would leave AX holding 0x21.
         */
        VDM_SET16(tib, VTIB_EAX, I33_INSTALLED);
        VDM_SET16(tib, VTIB_EBX, I33_BUTTON_COUNT);
        I33ResetState();
        break;
    /* 1Fh DISABLE DRIVER: WE REFUSE, AND THAT IS THE SAFE ANSWER:
     * Success means "AX=001Fh, and ES:BX is the INT 33h vector that was there
     * BEFORE the driver hooked it" -- so the guest can restore it. There is no such
     * vector here: we ARE the driver and the BOP stub in IVT[33h] is all there has
     * ever been. Answering success with ES:BX = 0000:0000 hands a guest a null far
     * pointer and invites it to install it, which turns "disable the mouse" into a
     * jump to the interrupt table. AX=FFFFh (failure) is both true and harmless --
     * a guest that cannot disable the driver simply carries on using it.
     */
    case I33_FN_DISABLE_DRIVER:
        VDM_SET16(tib, VTIB_EAX, I33_FAILED);
        break;
    case I33_FN_GET_DRIVER_VERSION:                                        /* driver version / type */
        VDM_SET16(tib, VTIB_EBX, I33_DRIVER_VERSION);               /* report 8.00 */
        /* CH=04 PS/2. CL is the IRQ, and the real driver answers FF for a PS/2
         * mouse rather than 0 -- measured, p_mouse.asm i33.24.version.
         */
        VDM_SET16(tib, VTIB_ECX, (I33_MOUSE_TYPE_PS2 << BYTE_SHIFT) | I33_PS2_IRQ);
        break;
    case I33_FN_GET_MAXIMUM_VIRTUAL:                                        /* max virtual coordinates */
        VDM_SET16(tib, VTIB_EBX, I33_DRIVER_ENABLED);               /* driver not disabled */
        VDM_SET16(tib, VTIB_ECX, (WORD)I33RangeXMaximum());
        VDM_SET16(tib, VTIB_EDX, (WORD)I33RangeYMaximum());
        break;
    /* 25h-34h: WHAT 24h's "8.00" PROMISES. (#249):
     * Every one of these reached `default:` and returned the caller's registers, so
     * a version-gated guest was told "success, and here is what you passed". The
     * ones below answer as RBIL's MS Mouse 7.x/8.x entries document; 32h says which.
     */
    case I33_FN_GET_DRIVER_INFO:                                        /* general driver info */
    {
        /* AX: bit 15 = loaded as a device driver (no: we are the TSR shape), 14 = the
         * newer integrated driver (yes -- every 7.x/8.x driver is; DOSBox-X's 8.05 sets
         * it too), 13-12 = cursor type (00 software text, 01 hardware text, 1x
         * graphics), 11-8 = 1Ch's interrupt rate, 7-0 = Mouse Display Drivers loaded
         * (none). BX/CX/DX = cursor lock / in mouse code / mouse busy: all 0 -- the
         * driver is host code and is never "busy" from the guest's side.
         */
        WORD cursorType = (WORD)(g_Video.ModeKind != VIDEO_KIND_TEXT || g_Video.IsVesa ? I33_CURSOR_GRAPHICS
                            : g_MouseTcHardware ? I33_CURSOR_HARDWARE_TEXT : I33_CURSOR_SOFTWARE_TEXT);
        VDM_SET16(tib, VTIB_EAX, (WORD)(I33_INFO_INTEGRATED_DRIVER | (cursorType << I33_INFO_CURSOR_TYPE_SHIFT) | ((g_MouseRate & I33_INFO_RATE_MASK) << BYTE_SHIFT)));
        VDM_SET16(tib, VTIB_EBX, I33_NOT_BUSY);
        VDM_SET16(tib, VTIB_ECX, I33_NOT_BUSY);
        VDM_SET16(tib, VTIB_EDX, I33_NOT_BUSY);
        break; }
    case I33_FN_GET_MASKS_AND_MICKEYS:                                        /* masks + mickey counts */
    {
        /* AX/BX = the text cursor's screen/cursor masks, or the hardware cursor's scan
         * lines when 0Ah BX=1 chose it; CX/DX = mickeys since the last read -- the SAME
         * counters 0Bh drains (I33TakeMotion), signed.
         */
        LONG deltaX, deltaY;
        I33TakeMotion(positionX, positionY, &deltaX, &deltaY);
        VDM_SET16(tib, VTIB_EAX, (WORD)(g_MouseTcHardware ? g_MouseTcHardwareLow : g_MouseTextCursorAnd));
        VDM_SET16(tib, VTIB_EBX, (WORD)(g_MouseTcHardware ? g_MouseTcHardwareHigh : g_MouseTextCursorXor));
        VDM_SET16(tib, VTIB_ECX, (WORD)(SHORT)deltaX);
        VDM_SET16(tib, VTIB_EDX, (WORD)(SHORT)deltaY);
        break; }
    case I33_FN_SET_VIDEO_MODE:                                        /* set video mode */
        /* The driver sets modes only from its own list (29h), and it has none: the
         * mode set belongs to INT 10h. CL != 0 = failed.
         */
        VDM_SET16(tib, VTIB_ECX, (WORD)((VDM_REG(tib, VTIB_ECX) & HIGH_BYTE_MASK) | I33_VIDEO_MODE_FAILED));
        break;
    case I33_FN_ENUMERATE_VIDEO_MODES:                                        /* enumerate video modes */
        /* CX = 0: the end of the list, at once. (DS:DX would name the mode; DX = 0 and
         * DS is left alone -- writing DS from here would be loaded into a PM caller's
         * selector too.)
         */
        VDM_SET16(tib, VTIB_ECX, I33_MODE_LIST_END);
        VDM_SET16(tib, VTIB_EDX, I33_MODE_LIST_END);
        break;
    case I33_FN_GET_HOT_SPOT:                                        /* cursor hot spot */
        /* AX = the visibility counter as the MS driver keeps it: 0 shown, negative
         * hidden (we count hides up from 0, so it is our count negated); BX/CX = 09h's
         * hot spot; DX = mouse type, 4 = PS/2 (as 24h's CH).
         */
        VDM_SET16(tib, VTIB_EAX, (WORD)(SHORT)(-g_MouseHidden));
        VDM_SET16(tib, VTIB_EBX, (WORD)(SHORT)g_MouseHotX);
        VDM_SET16(tib, VTIB_ECX, (WORD)(SHORT)g_MouseHotY);
        VDM_SET16(tib, VTIB_EDX, I33_MOUSE_TYPE_PS2);
        break;
    /* 2Bh-2Eh, 33h, 34h: THE PROFILES, THE SETTINGS BLOCK, THE .INI NAME. (#265):
     * All reached `default:` (and 32h said so). The register contracts are RBIL's; the
     * block layouts and every UNMEASURED choice are in i33_driver.h. The profiles are
     * STORED AND HANDED BACK, never applied. Failure is AX=FFFEh throughout -- the
     * value RBIL gives 2Dh/2Eh -- and 2Bh, whose RBIL entry says only "success flag",
     * is given the same 0000h/FFFEh pair (UNMEASURED).
     */
    case I33_FN_LOAD_ACCELERATION_PROFILES:                                        /* load acceleration profiles */
    {
        /* BX = the profile to make active (1-4), or FFFFh = restore the default curves;
         * ES:SI -> a 144h-byte block (not read for FFFFh).
         */
        WORD bx = (WORD)VDM_REG16(tib, VTIB_EBX);
        ++g_MouseAccelerationCalls; I33AccelerationReady();
        if (bx == I33_ACCELERATION_RESTORE_DEFAULTS)
        {
            I33AccelerationDefaults(g_MouseAcceleration);
            g_MouseAccelerationCurrent = I33_ACC_DEFAULT;
        }
        else if (bx >= 1 && bx <= I33_ACC_N)
        {
            volatile BYTE *guest = I33GuestPointer(tib, source, (WORD)VDM_REG16(tib, VTIB_ES),
                                             MouseI33Offset(tib, source, VDM_REG(tib, VTIB_ESI)),
                                             I33_ACC_LEN, X86_ACCESS_READ);
            UINT index;
            if (!guest)
            {
                ++g_MouseStateBadPointer;
                VDM_SET16(tib, VTIB_EAX, I33_ACCELERATION_ERROR);
                break;
            }
            for (index = 0; index < I33_ACC_LEN; ++index) g_MouseAcceleration[index] = guest[index];
            g_MouseAccelerationCurrent = bx;
        }
        else
        {
            VDM_SET16(tib, VTIB_EAX, I33_ACCELERATION_ERROR);
            break;
        }
        VDM_SET16(tib, VTIB_EAX, I33_SUCCESS);
        break; }
    case I33_FN_GET_ACCELERATION_PROFILES:                                        /* get acceleration profiles */
    {
        /* AX=0, BX = the active profile, ES:SI -> the block -- written out fresh on every
         * call, so a guest that scribbled on the last copy reads a good one.
         */
        volatile BYTE *driverData = I33DriverData(); UINT index;
        ++g_MouseAccelerationCalls; I33AccelerationReady();
        for (index = 0; index < I33_ACC_LEN; ++index) driverData[VDD_MOUSE_ACC + index] = g_MouseAcceleration[index];
        VDM_SET16(tib, VTIB_EAX, I33_SUCCESS);
        VDM_SET16(tib, VTIB_EBX, (WORD)g_MouseAccelerationCurrent);
        I33ResultPointer(tib, source, VTIB_ESI, VDD_MOUSE_ACC);
        break; }
    case I33_FN_SELECT_ACCELERATION_PROFILE:                                        /* select acceleration profile */
    {
        /* BX = 1-4 selects, FFFFh only asks. AX=0 with BX = the active profile and ES:SI
         * -> its 16-byte name; an invalid BX is AX=FFFEh with BX = the (unchanged)
         * active profile, and ES:SI -- "destroyed" per RBIL -- left as it was.
         */
        WORD bx = (WORD)VDM_REG16(tib, VTIB_EBX);
        volatile BYTE *driverData = I33DriverData(); UINT index;
        ++g_MouseAccelerationCalls; I33AccelerationReady();
        if (bx != I33_ACCELERATION_QUERY && (bx < 1 || bx > I33_ACC_N))
        {
            VDM_SET16(tib, VTIB_EAX, I33_ACCELERATION_ERROR);
            VDM_SET16(tib, VTIB_EBX, (WORD)g_MouseAccelerationCurrent);
            break;
        }
        if (bx != I33_ACCELERATION_QUERY) g_MouseAccelerationCurrent = bx;
        for (index = 0; index < I33_ACC_LEN; ++index) driverData[VDD_MOUSE_ACC + index] = g_MouseAcceleration[index];
        VDM_SET16(tib, VTIB_EAX, I33_SUCCESS);
        VDM_SET16(tib, VTIB_EBX, (WORD)g_MouseAccelerationCurrent);
        I33ResultPointer(tib, source, VTIB_ESI,
                    (WORD)(VDD_MOUSE_ACC + I33_ACC_NAMES + (g_MouseAccelerationCurrent - 1) * I33_ACC_NAMELEN));
        break; }
    case I33_FN_SET_ACCELERATION_PROFILE_NAMES:                                        /* set acceleration profile names */
    {
        /* ES:SI -> 64 bytes, four 16-byte names. BL = 0: they become the names. BL != 0:
         * "fill ES:SI buffer with default names on return" -- read here as RESTORE the
         * default names and hand them back. [CAUTION] UNMEASURED (RBIL is the only voice; only an
         * 8.10+ driver has 2Eh at all).
         */
        INT fill = (VDM_REG(tib, VTIB_EBX) & BYTE_MASK) != 0;
        volatile BYTE *guest = I33GuestPointer(tib, source, (WORD)VDM_REG16(tib, VTIB_ES),
                                         MouseI33Offset(tib, source, VDM_REG(tib, VTIB_ESI)),
                                         I33_ACC_N * I33_ACC_NAMELEN, fill);
        UINT index;
        ++g_MouseAccelerationCalls; I33AccelerationReady();
        if (!guest)
        {
            ++g_MouseStateBadPointer;
            VDM_SET16(tib, VTIB_EAX, I33_ACCELERATION_ERROR);
            break;
        }
        if (fill)
        {
            I33AccelerationDefaultNames(g_MouseAcceleration + I33_ACC_NAMES);
            for (index = 0; index < I33_ACC_N * I33_ACC_NAMELEN; ++index) guest[index] = g_MouseAcceleration[I33_ACC_NAMES + index];
        }
        else
            for (index = 0; index < I33_ACC_N * I33_ACC_NAMELEN; ++index) g_MouseAcceleration[I33_ACC_NAMES + index] = guest[index];
        VDM_SET16(tib, VTIB_EAX, I33_SUCCESS);
        break; }
    case I33_FN_SWITCH_SETTINGS:                                        /* switch settings + profiles */
    {
        /* CX = the buffer's size, ES:DX -> it. AX=0, CX = bytes written (at most 154h);
         * a short buffer gets the head of the block (I33SettingsBlock). A buffer we
         * may not write gets CX=0 -- "nothing returned" -- not a fault.
         */
        UINT cap = (UINT)VDM_REG16(tib, VTIB_ECX), count, index;
        BYTE block[I33_SET_LEN];
        I33_SETTINGS settings;
        volatile BYTE *guest = NULL;
        ++g_MouseAccelerationCalls; I33AccelerationReady();
        if (cap > I33_SET_LEN) cap = I33_SET_LEN;
        if (cap) guest = I33GuestPointer(tib, source, (WORD)VDM_REG16(tib, VTIB_ES),
                                   MouseI33Offset(tib, source, VDM_REG(tib, VTIB_EDX)), cap, X86_ACCESS_WRITE);
        if (!guest)
        {
            if (cap) ++g_MouseStateBadPointer;
            cap = 0;
        }
        settings.Type = I33_MOUSE_TYPE_PS2; settings.Language = I33_LANGUAGE_ENGLISH;
        settings.HorizontalSpeed = (BYTE)g_MouseSpeedX; settings.VerticalSpeed = (BYTE)g_MouseSpeedY;
        settings.DoubleSpeed = (BYTE)g_MouseSpeedDouble; settings.Curve = (BYTE)g_MouseAccelerationCurrent;
        settings.Rate = (BYTE)g_MouseRate;
        count = I33SettingsBlock(block, cap, &settings, g_MouseAcceleration);
        for (index = 0; index < count; ++index) guest[index] = block[index];
        VDM_SET16(tib, VTIB_EAX, I33_SUCCESS);
        VDM_SET16(tib, VTIB_ECX, (WORD)count);
        break; }
    case I33_FN_GET_INI_FILE_NAME:                                        /* initialization file name */
    {
        /* AX=0, ES:DX -> "MOUSE.INI". There is no such file: a real driver names the one
         * it read; with none present the name a guest gets is the one it would look for,
         * and opening it fails exactly as on a machine without one. The bare name (no
         * path) is ours -- UNMEASURED.
         */
        static const CHAR iniName[] = I33_INI_FILE_NAME;
        volatile BYTE *driverData = I33DriverData(); UINT index;
        ++g_MouseAccelerationCalls;
        for (index = 0; index < sizeof iniName; ++index) driverData[VDD_MOUSE_INI + index] = (BYTE)iniName[index];
        VDM_SET16(tib, VTIB_EAX, I33_SUCCESS);
        I33ResultPointer(tib, source, VTIB_EDX, VDD_MOUSE_INI);
        break; }
    case I33_FN_HARDWARE_RESET:                                        /* mouse hardware reset */
        /* FFFFh = done. There is no device under us to re-initialise; the driver's
         * own state is untouched, as the call documents (00h/21h reset that).
         */
        VDM_SET16(tib, VTIB_EAX, I33_HARDWARE_RESET_DONE);
        break;
    case I33_FN_BALLPOINT_INFO:                                        /* BallPoint information */
        VDM_SET16(tib, VTIB_EAX, I33_NO_BALLPOINT);               /* FFFFh = no BallPoint */
        break;
    case I33_FN_GET_CURRENT_VIRTUAL:                                        /* current min/max virtual */
        VDM_SET16(tib, VTIB_EAX, (WORD)I33RangeXMinimum());
        VDM_SET16(tib, VTIB_EBX, (WORD)I33RangeYMinimum());
        VDM_SET16(tib, VTIB_ECX, (WORD)I33RangeXMaximum());
        VDM_SET16(tib, VTIB_EDX, (WORD)I33RangeYMaximum());
        break;
    case I33_FN_GET_ACTIVE_ADVANCED:                                        /* active advanced fns */
        VDM_SET16(tib, VTIB_EAX, (WORD)I33_ACTIVE_FNS);
        VDM_SET16(tib, VTIB_EBX, I33_RESERVED);               /* BX/CX/DX reserved = 0 */
        VDM_SET16(tib, VTIB_ECX, I33_RESERVED);
        VDM_SET16(tib, VTIB_EDX, I33_RESERVED);
        break;
    /* Logitech CyberMan / SWIFT probe. Doom makes it once and PRINTS the answer, so
     * this is the one unimplemented call whose behaviour was already measured: the
     * documented "no SWIFT support" reply is AX = 0, and leaving AX holding 0x53C1
     * only happened to read as a refusal. Say no on purpose.
     */
    case I33_FN_SWIFT_SUPPORT:
        VDM_SET16(tib, VTIB_EAX, I33_NO_SWIFT);
        break;
    /* [CAUTION]: `default:` IS THE BUG SHAPE THIS FILE HAS PAID FOR SIX TIMES (Importance = 1):
     * It returns with the guest's registers untouched, which is not "unsupported",
     * it is "success, and here is whatever was already in AX". Every function the
     * driver actually defines is now handled above, so anything reaching here is
     * either a genuinely exotic call or a MIS-PATCHED `CD 33` site -- and those need
     * opposite fixes, so COUNT it and leave the registers alone rather than invent
     * an answer. g_MouseI33Ax already records which AX values and from which sites.
     */
    default: ++g_MouseI33Unimplemented; break;
    }
}

/* Is there anyone to deliver an event TO -- 0Ch's handler (a mask and an address) or
 * any 18h handler with event bits?
 */
INT MouseAnyHandler(VOID)
{
    return (g_MouseEventMask && (g_MouseEventSegment | g_MouseEventOffset) != 0) || I33AlternateAny(g_MouseAlt);
}

/* THE ONE PICKER BOTH DELIVERY PATHS USE (#265):
 * Pop the oldest queued event SOMEONE asked for and say who: 0Ch's handler, or the
 * 18h handler whose Shift/Ctrl/Alt combination is the one held (I33PickHandler). Unwanted
 * events are skipped, as they always were. The shift state is read HERE, at delivery,
 * from BDA 0040:0017 -- not at the event: the UI thread that queues events must not
 * touch guest memory, and a key held for a click is still held a loop pass later.
 *
 * [CAUTION]: A Shift released inside that window would route the click to 0Ch's handler.
 */
INT MouseEventQueueTake(MOUSE_EVENT_ENTRY *event, LONG *outAx, WORD *segment, DWORD *offset)
{
    LONG tail; INT guard = 0;
    UINT mainMask = (g_MouseEventSegment | g_MouseEventOffset) ? (UINT)g_MouseEventMask : 0u;
    BYTE shiftFlags = *(volatile BYTE *)(ULONG_PTR)(BIOS_BDA_BASE + BIOS_BDA_SHIFT_FLAGS);
    while (g_MouseEventQueueTail != g_MouseEventQueueHead && guard++ < MS_EVQ)
    {
        UINT handlerAx; INT handlerChoice;
        tail = g_MouseEventQueueTail;
        *event = g_MouseEventQueue[tail];
        g_MouseEventQueueTail = (tail + 1) % MS_EVQ;
        handlerChoice = I33PickHandler(g_MouseAlt, (UINT)event->Bits, shiftFlags, mainMask, &handlerAx);
        if (handlerChoice == I33_PICK_NOBODY) continue;
        if (g_MouseEventQueueTail == g_MouseEventQueueHead) InterlockedExchange(&g_MouseEventPend, 0);
        *outAx = (LONG)handlerAx;
        if (handlerChoice >= 0)
        {
            *segment = g_MouseAlt[handlerChoice].Segment;
            *offset = g_MouseAlt[handlerChoice].Offset;
            ++g_MouseAltCalls;
        }
        else
        {
            *segment = (WORD)g_MouseEventSegment;
            *offset = (DWORD)g_MouseEventOffset;
        }
        return 1;
    }
    if (g_MouseEventQueueTail == g_MouseEventQueueHead) InterlockedExchange(&g_MouseEventPend, 0);
    return 0;
}

/* Deliver pending mouse events to the guest's INT 33h handler -- see g_MouseEventPend.
 * Called at the exec-loop boundary, right after the IRQ gates, V86 thread only.
 */
VOID MouseCallbackTry(volatile BYTE *tib)
{
    LONG pend;
    WORD handlerSegment; DWORD handlerOffset;
    DWORD cs, ip, flags, ss, sp;
    MOUSE_EVENT_ENTRY event = { 0, 0, 0, 0 };
    if (g_MouseCallbackActive)                            /* a handler that never came back */
    {
        if ((DWORD)(GetTickCount() - (g_MouseCallbackSince & ~1u)) > MS_CB_TIMEOUT_MS)
        {
            g_MouseCallbackActive = 0; ++g_MouseCallbackLost;
        }
        ++g_MouseCallbackWhy[MOUSE_CB_WHY_IN_FLIGHT]; return;
    }
    /* 18h's handlers count as handlers (#265): a guest with only those still gets calls. */
    if ((!g_MouseEventMask && !I33AlternateAny(g_MouseAlt)) || g_MouseEventQueueHead == g_MouseEventQueueTail)
    {
        ++g_MouseCallbackWhy[MOUSE_CB_WHY_NO_EVENTS];
        return;
    }
    if (!MouseAnyHandler())
    {
        ++g_MouseCallbackWhy[MOUSE_CB_WHY_NO_HANDLER];
        return;
    }
    if (g_DpmiPm) {                               /* PM client: DpmiInjectPmMouseCallback()
                                                      delivers from the PM loop; leave the
                                                      queue for it (s74c -- it used to be
                                                      DROPPED here, ZAR never saw a click) */
        ++g_MouseCallbackPm; return;
    }
    cs = VDM_REG16(tib, VTIB_CS); ip = VDM_REG16(tib, VTIB_EIP);
    ss = VDM_REG16(tib, VTIB_SS); sp = VDM_REG16(tib, VTIB_ESP);
    if (cs == DOS_HDLR_SEG)
    {
        /* THE STUBS ARE WHERE THE EXEC LOOP ACTUALLY RUNS FOR THIS GUEST:
         * Measured (QB, 13.6 s): 926 exec-loop passes in all, every IRQ delivered by
         * the async path, and every pass with mouse events pending was AT our INT 08h
         * or INT 09h stub -- QB chains its timer/keyboard hooks to the BIOS and
         * otherwise runs without trapping. Refusing there, as the IRQ gates do to avoid
         * re-entering the handler, meant the callback never ran once (cb_why stub=41,
         * inj=0). Calling from the stub is safe for a CALLBACK: the stub is about to
         * IRET, the live IF is clear so nothing nests inside the handler, and the
         * driver on real hardware calls it from an interrupt context too. Only the
         * INT 09h stub's own BOP instruction (0x4C..0x4E, not yet executed) is kept
         * out, so the byte it consumes is not disturbed.
         */
        if (ip >= DOS_HDLR_INT09_STUB_OFF && ip < DOS_HDLR_INT09_STUB_OFF + VDM_BOP_LENGTH)
        {
            ++g_MouseCallbackWhy[MOUSE_CB_WHY_IN_STUB];
            return;
        }
        if ((ip >= DOS_HDLR_INT08_STUB_OFF && ip < DOS_HDLR_INT08_STUB_END) || ip == DOS_HDLR_INT09_STUB_OFF + VDM_BOP_LENGTH) flags = EFLAGS_IF; /* stub about to IRET: deliver */
        else flags = PeekWord((ss << PARAGRAPH_SHIFT) + ((sp + X86_FRAME16_FLAGS) & WORD_MASK));    /* the FLAGS the stub IRETs to */
    }
    else flags = VDM_REG(tib, VTIB_EFLAGS);
    if (!IfOrVif(flags)) /* interrupts off: like an IRQ, wait */
    {
        ++g_MouseCallbackWhy[MOUSE_CB_WHY_IF_OFF];
        return;
    }
    /* -- THE RETURN STUB, RE-VERIFIED EVERY TIME (see MS_CB_RET_OFF). A guest that has
     * written over it would be sent into data by its own RETF; refusing is the
     * lesser harm, and the refusal is counted and named.
     */
    {   const volatile BYTE *returnStub = (const volatile BYTE *)(ULONG_PTR)((DOS_HDLR_SEG << PARAGRAPH_SHIFT) + MS_CB_RET_OFF);
        if (returnStub[0] != VDM_BOP0 || returnStub[1] != VDM_BOP1 || returnStub[VDM_BOP_NUMBER_OFFSET] != MS_CB_BOP)
        {
            if (g_MouseCallbackWhy[MOUSE_CB_WHY_STUB_CLOBBERED] < 4)
            {
                CHAR lineBuffer[160], *lineCursor = lineBuffer;
                lineCursor = LogPut(lineCursor, "MOUSECB REFUSED: return stub at 0050:");
                lineCursor = LogHex(lineCursor, MS_CB_RET_OFF); lineCursor = LogPut(lineCursor, " overwritten by the guest: ");
                lineCursor = LogDump(lineCursor, (const VOID *)returnStub, 4); lineCursor = LogPut(lineCursor, "\r\n");
                LogAppend(LOG_PATH, lineBuffer, lineCursor);
            }
            ++g_MouseCallbackWhy[MOUSE_CB_WHY_STUB_CLOBBERED];
            g_MouseEventQueueTail = g_MouseEventQueueHead; InterlockedExchange(&g_MouseEventPend, 0);
            return;
        }
    }
    /* The oldest queued event a handler asked for, and which handler (MouseEventQueueTake). */
    if (!MouseEventQueueTake(&event, &pend, &handlerSegment, &handlerOffset) || !pend) return;
    /* Save the whole interrupted context host-side. */
    g_MouseCallbackSaved.Eax = VDM_REG(tib, VTIB_EAX); g_MouseCallbackSaved.Ebx = VDM_REG(tib, VTIB_EBX);
    g_MouseCallbackSaved.Ecx = VDM_REG(tib, VTIB_ECX); g_MouseCallbackSaved.Edx = VDM_REG(tib, VTIB_EDX);
    g_MouseCallbackSaved.Esi = VDM_REG(tib, VTIB_ESI); g_MouseCallbackSaved.Edi = VDM_REG(tib, VTIB_EDI);
    g_MouseCallbackSaved.Ebp = VDM_REG(tib, VTIB_EBP); g_MouseCallbackSaved.Esp = VDM_REG(tib, VTIB_ESP);
    g_MouseCallbackSaved.Eip = VDM_REG(tib, VTIB_EIP); g_MouseCallbackSaved.Eflags = VDM_REG(tib, VTIB_EFLAGS);
    g_MouseCallbackSaved.Cs  = VDM_REG(tib, VTIB_CS);  g_MouseCallbackSaved.Ds  = VDM_REG(tib, VTIB_DS);
    g_MouseCallbackSaved.Es  = VDM_REG(tib, VTIB_ES);  g_MouseCallbackSaved.Ss  = VDM_REG(tib, VTIB_SS);
    /* The far return the handler's RETF will take, then the call itself. */
    sp = (sp - X86_WORD_SIZE) & WORD_MASK; PokeWord((ss << PARAGRAPH_SHIFT) + sp, DOS_HDLR_SEG);
    sp = (sp - X86_WORD_SIZE) & WORD_MASK; PokeWord((ss << PARAGRAPH_SHIFT) + sp, MS_CB_RET_OFF);
    VDM_SET16(tib, VTIB_ESP, (WORD)sp);
    VDM_SET16(tib, VTIB_EAX, (WORD)pend);
    VDM_SET16(tib, VTIB_EBX, (WORD)event.Buttons);          /* the state AT the event */
    VDM_SET16(tib, VTIB_ECX, (WORD)I33ClampX(I33VirtualX(event.X)));
    VDM_SET16(tib, VTIB_EDX, (WORD)I33ClampY(I33VirtualY(event.Y)));
    VDM_SET16(tib, VTIB_ESI, 0);
    VDM_SET16(tib, VTIB_EDI, 0);
    VDM_SET16(tib, VTIB_DS,  DOS_HDLR_SEG);        /* "the driver's DS" */
    VDM_SET16(tib, VTIB_CS,  handlerSegment);                /* 0Ch's handler, or 18h's (#265) */
    VDM_SET16(tib, VTIB_EIP, (WORD)handlerOffset);
    g_MouseCallbackActive = 1; g_MouseCallbackSince = GetTickCount() | 1;
    ++g_MouseCallbackInjected;
    if (g_MouseCallbackInjected <= 3) g_MouseCallbackTrace = 10;     /* see g_MouseCallbackTrace: the next VM events */
    if (g_MouseCallbackInjected <= 16)                         /* the first few, with where from */
    {
        CHAR lineBuffer[384], *lineCursor = lineBuffer;
        lineCursor = LogPut(lineCursor, "MOUSECB inject #"); lineCursor = LogHex(lineCursor, g_MouseCallbackInjected);
        lineCursor = LogPut(lineCursor, " ev=0x");   lineCursor = LogHex(lineCursor, (DWORD)pend);
        lineCursor = LogPut(lineCursor, " from=0x"); lineCursor = LogHex(lineCursor, g_MouseCallbackSaved.Cs);
        lineCursor = LogPut(lineCursor, ":0x");      lineCursor = LogHex(lineCursor, g_MouseCallbackSaved.Eip);
        lineCursor = LogPut(lineCursor, " efl=0x");  lineCursor = LogHex(lineCursor, g_MouseCallbackSaved.Eflags);
        lineCursor = LogPut(lineCursor, " ss:sp=0x"); lineCursor = LogHex(lineCursor, g_MouseCallbackSaved.Ss);
        lineCursor = LogPut(lineCursor, ":0x");      lineCursor = LogHex(lineCursor, g_MouseCallbackSaved.Esp);
        lineCursor = LogPut(lineCursor, " -> 0x");   lineCursor = LogHex(lineCursor, (DWORD)handlerSegment);
        lineCursor = LogPut(lineCursor, ":0x");      lineCursor = LogHex(lineCursor, handlerOffset & WORD_MASK);
        /* The three things the handler's return depends on, read back from guest memory:
         * the code at the handler, the return BOP at DOS_HDLR_SEG:MS_CB_RET_OFF, and the
         * far-return frame just pushed. If any is not what was intended, the trace that
         * follows is explained before it is read.
         */
        lineCursor = LogPut(lineCursor, " code@hdl=");
        lineCursor = LogDump(lineCursor, (const VOID *)(ULONG_PTR)(((DWORD)handlerSegment << PARAGRAPH_SHIFT) + (handlerOffset & WORD_MASK)), 8);
        lineCursor = LogPut(lineCursor, " ret@50:12=");
        lineCursor = LogDump(lineCursor, (const VOID *)(ULONG_PTR)((DOS_HDLR_SEG << PARAGRAPH_SHIFT) + MS_CB_RET_OFF), 4);
        lineCursor = LogPut(lineCursor, " frame@sp=");
        lineCursor = LogDump(lineCursor, (const VOID *)(ULONG_PTR)((ss << PARAGRAPH_SHIFT) + sp), 4);
        lineCursor = LogPut(lineCursor, "\r\n");
        LogAppend(LOG_PATH, lineBuffer, lineCursor);
        /* Once: the DOS communication area as the guest has left it. This is how the
         * BASIC slots were found (0050:0012 = the saved INT 1Ch vector) and it says
         * whether anything else of ours below 0x40 -- the INT 10h stub at 0x20 sits
         * right after BASIC's INT 24h slot at 0x1A..0x1D -- has been written over.
         */
        if (g_MouseCallbackInjected == 1)
        {
            CHAR alternateBuffer[256], *alternateCursor = alternateBuffer;
            alternateCursor = LogPut(alternateCursor, "MOUSECB area 0050:0000..003F=");
            alternateCursor = LogDump(alternateCursor, (const VOID *)(ULONG_PTR)(DOS_HDLR_SEG << PARAGRAPH_SHIFT), 0x40);
            alternateCursor = LogPut(alternateCursor, "\r\n");
            LogAppend(LOG_PATH, alternateBuffer, alternateCursor);
        }
    }
}

/* The BOP at DOS_HDLR_SEG:MS_CB_RET_OFF: the handler RETF'd here, put everything back. */
VOID MouseCallbackReturn(volatile BYTE *tib)
{
    if (!g_MouseCallbackActive)                           /* not ours to unwind: step over */
    {
        CHAR lineBuffer[160], *lineCursor = lineBuffer;
        ++g_MouseCallbackStray; VDM_REG(tib, VTIB_EIP) += VDM_BOP_LENGTH;
        lineCursor = LogPut(lineCursor, "MOUSECB STRAY return, nothing in flight: ss:sp=0x");
        lineCursor = LogHex(lineCursor, VDM_REG16(tib, VTIB_SS)); lineCursor = LogPut(lineCursor, ":0x");
        lineCursor = LogHex(lineCursor, VDM_REG16(tib, VTIB_ESP)); lineCursor = LogPut(lineCursor, "\r\n");
        LogAppend(LOG_PATH, lineBuffer, lineCursor);
        return;
    }
    if (g_MouseCallbackDone < 16)
    {
        CHAR lineBuffer[96], *lineCursor = lineBuffer;
        lineCursor = LogPut(lineCursor, "MOUSECB return #"); lineCursor = LogHex(lineCursor, g_MouseCallbackDone + 1);
        lineCursor = LogPut(lineCursor, " ok\r\n"); LogAppend(LOG_PATH, lineBuffer, lineCursor);
    }
    VDM_REG(tib, VTIB_EAX) = g_MouseCallbackSaved.Eax; VDM_REG(tib, VTIB_EBX) = g_MouseCallbackSaved.Ebx;
    VDM_REG(tib, VTIB_ECX) = g_MouseCallbackSaved.Ecx; VDM_REG(tib, VTIB_EDX) = g_MouseCallbackSaved.Edx;
    VDM_REG(tib, VTIB_ESI) = g_MouseCallbackSaved.Esi; VDM_REG(tib, VTIB_EDI) = g_MouseCallbackSaved.Edi;
    VDM_REG(tib, VTIB_EBP) = g_MouseCallbackSaved.Ebp; VDM_REG(tib, VTIB_ESP) = g_MouseCallbackSaved.Esp;
    VDM_REG(tib, VTIB_EIP) = g_MouseCallbackSaved.Eip; VDM_REG(tib, VTIB_EFLAGS) = g_MouseCallbackSaved.Eflags;
    VDM_REG(tib, VTIB_CS)  = g_MouseCallbackSaved.Cs;  VDM_REG(tib, VTIB_DS)  = g_MouseCallbackSaved.Ds;
    VDM_REG(tib, VTIB_ES)  = g_MouseCallbackSaved.Es;  VDM_REG(tib, VTIB_SS)  = g_MouseCallbackSaved.Ss;
    g_MouseCallbackActive = 0; ++g_MouseCallbackDone;
}

enum
{
    OVERLAY_CURSOR_ROWS = 16, OVERLAY_CURSOR_OUTLINE = 0, OVERLAY_CURSOR_FILL = 15
};   /* the host-drawn pointer: black outline, white fill */
/* Classic arrow cursor: 'o' = black outline (index 0), 'X' = white fill (15),
 * ' ' = transparent; hotspot at the top-left tip. Drawn into the presenter's 8-bpp
 * SNAPSHOT each present (#264) -- [WARNING] not the frame: in mode 13h the frame IS guest VRAM,
 * so "re-rendered every tick, leaves no trail" was false there; see the present path.
 */
/* The INT 33h driver cursor. This was hand-drawn ASCII art until the demo sweep
 * turned up its one cosmetic defect -- "the mouse cursor is not quite the right
 * shape" -- so it is now DECODED FROM REAL ARTWORK and regenerated rather than
 * remembered. 'o' = outline (palette index 0), 'X' = fill (index 15), ' ' =
 * transparent; only the data changed, OverlayCursor() is untouched.
 * Regenerate with:  python3 tools/gen/mkcursor.py cursors/cursor-pointer.cur
 */
/* Generated by tools/gen/mkcursor.py from cursors/cursor-pointer.cur -- 16x16, hotspot (0,0).
 * Do not hand-edit: regenerate from the artwork instead.
 */
static PCSTR const g_MouseCursorShape[OVERLAY_CURSOR_ROWS] = {
    "oo",
    "oXo",
    "oXXo",
    "oXXXo",
    "oXXXXo",
    "oXXXXXo",
    "oXXXXXXo",
    "oXXXXXXXo",
    "oXXXXXXXXo",
    "oXXXXXooooo",
    "oXXoXXo",
    "oXo oXXo",
    "oo  oXXo",
    "o    oXXo",
    "     oXXo",
    "      oo",
};
static VOID OverlayCursor(BYTE *pixels, INT width, INT height, INT stride, INT cursorX, INT cursorY)
{
    INT row;
    for (row = 0; row < OVERLAY_CURSOR_ROWS; ++row)
    {
        PCSTR shapeRow = g_MouseCursorShape[row]; INT column, screenRow = cursorY + row;
        if (screenRow < 0 || screenRow >= height) continue;
        for (column = 0; shapeRow[column]; ++column)
        {
            INT screenColumn = cursorX + column; CHAR shape = shapeRow[column];
            if (shape == ' ' || screenColumn < 0 || screenColumn >= width) continue;
            pixels[screenRow * stride + screenColumn] = (shape == 'o') ? OVERLAY_CURSOR_OUTLINE : OVERLAY_CURSOR_FILL;     /* black outline / white fill */
        }
    }
}

/* THE GRAPHICS-MODE POINTER, onto the presenter's 8-bpp snapshot (#264):
 * Until the guest defines a shape (09h) it is the host arrow above; after, it is the
 * guest's bitmap with the driver's AND/XOR arithmetic and hot spot (i33_driver.h).
 *
 * [CAUTION]: The frame holds what each renderer made of video memory: the 4-bit plane value
 * in a 16-colour mode, the byte in 13h/mode Y/8-bpp VESA, 0/15 in CGA mode 06h -- all
 * of which an XOR of 0Fh treats as the driver would -- and in CGA 4-colour the
 * PALETTE-MAPPED colour, which I33GraphicsCursorRow maps back through the renderer's own table
 * first. [CAUTION] Mode 11h (and 0Fh) render all four planes while the attribute controller
 * shows fewer; an XOR of 0Fh there sets planes the display would ignore. Cosmetic,
 * unmeasured, and the renderer's question rather than the cursor's.
 */
VOID MouseDrawGraphicsCursor(BYTE *pixels, INT width, INT height, INT stride)
{
    INT buffer;
    const BYTE *cga4Map = NULL;
    /* #325: the pointer lives in the mode's extent (gw x gh); the snapshot may be the
     * CRTC's real geometry (Mode X is 320x240 in a mode 13h extent of 320x200).
     */
    INT cursorX = (INT)((LONG)g_MouseX * width / (LONG)I33Width()), cursorY = (INT)((LONG)g_MouseY * height / (LONG)I33Height());
    if (!g_MouseGraphicsCursorDefined)
    {
        OverlayCursor(pixels, width, height, stride, cursorX, cursorY);
        return;
    }
    buffer = (INT)(g_MouseGraphicsCursorBuffer & 1);
    if (g_Video.ModeKind == VIDEO_KIND_CGA && !g_Video.IsVesa && g_Video.CgaBpp != 1)
        cga4Map = VddVideoCga4Map(&g_Video);
    I33GraphicsCursorDraw(pixels, width, height, stride, cursorX, cursorY, (INT)g_MouseHotX, (INT)g_MouseHotY,
                g_MouseGraphicsCursorScreen[buffer], g_MouseGraphicsCursorCurrent[buffer], I33_GC_ONES_COLOUR, cga4Map);
}
