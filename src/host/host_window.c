/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The window: menus, the tray, the status strip, the clipboard, mouse capture,
 *   fullscreen, scaling and the window procedure.
 *
 * Its own translation unit (#335): declared in host_window.h.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include "host_window.h"
#include <commctrl.h>
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
#include "host_bios.h"
#include "host_dos.h"
#include "host_input.h"
#include "host_install.h"
#include "host_irq.h"
#include "host_mouse.h"
#include "host_settings.h"
#include "host_timing.h"
#include "host_video.h"

/* dwExtraInfo tag on keystrokes WE synthesise with SendInput (the Start-menu
 * suppression Ctrl tap), so our own WM_KEYDOWN/UP handlers recognise and drop them
 * instead of forwarding them to the guest. Any non-zero sentinel a real device will
 * not use.
 */
#define HOST_INJECT_TAG     0x4E56444Du     /* 'NVDM' */

/* Create every Settings page off-screen at startup and log whether the templates
 * still build. See the call site: a bad DIALOGEX fails to CREATE, silently.
 */
#define DLGCHECK_FLAG       CFG_("dlgcheck.flag")
static DWORD  g_SpeakerRealHz;   /* sampled under the lock, applied outside it */
DWORD    g_PitDeliverSkipped;  /* attempts foregone: g_Lock busy when the crystal knocked */
DWORD    g_UiTickSkips;
/* who raised each present */
DWORD g_UiHookPresents;
DWORD g_UiTimerPresents;
static volatile LONG g_UiPresentPending;                /* one WM_APP_PRESENT in flight */
static int      g_UiForced;                              /* this body run was raised by the hook; stays int: INT here moves the compiled code */
static DWORD    g_UiInputFirst;                         /* input served ahead of a queued present */
/* Which of the three exits from the cooperative IRQ1 gate fires. See its call site. */
DWORD g_Irq1Checks;
DWORD g_Irq1NoIf;
DWORD g_Irq1In08;
DWORD g_Irq1In09;
DWORD    g_Irq1AsyncRetry;
INT            g_Headless      = 0;  /* AUTOEXIT marker present: SMB test harness -> bound infinite runs */
DWORD          g_Irq1Injected      = 0;  /* INT 09h injections (should track scancodes) */
UINT       g_CaptureMs    = CAPTURE_MS_DEFAULT; /* CAPTURE_FLAG contents: ms between shots */
/* #58: an optional SECOND number in capture.flag -- ms to wait before the first shot --
 * so the 40-shot budget can be spent on one moment (Doom's melt) instead of the start.
 */
DWORD g_CaptureDelayMs;
DWORD g_CaptureStart;
INT            g_Capture       = 0;  /* CAPTURE_FLAG present: opt-in self-screenshot for graphical tests */
/* The UI tick must be well ABOVE the guest refresh (60/70 Hz) or the phase window
 * is unreachable and every present falls back to the staleness path.
 */
#define VID_PRESENT_TICK_MS     5
#define VID_PRESENT_STALE_MS    25  /* Never let the screen go quiet longer than this */
volatile LONG g_CloseRequest;     /* UI -> exec thread: end the innermost program */
INT  g_TopIsShell;           /* depth 0 is a shell: nothing to close there */
/* The menu bar while FULLSCREEN has it detached. There used to be a second
 * detacher -- a "Show Menu Bar" toggle -- and it was a ONE-WAY DOOR: unticking it
 * removed the only control that could put it back. Removed (user report, s79);
 * fullscreen is now the only thing that takes the bar away, and Alt+Enter always
 * brings it back. Declared here rather than beside the fullscreen code because the
 * menu_* helpers above that point have to compensate for a detached bar.
 */
static HMENU        g_FsMenu;

static INT            g_PauseSuspended    = 0;   /* the CPU thread is suspended BY THE PAUSE */
DWORD g_PauseCount;
DWORD g_PauseCooperative;
DWORD g_PauseMs;
enum
{
    INSTALL_UNFORCED = 0, INSTALL_FORCED = 1
};   /* InstallPerform: /force replaces another program's Debugger value */
enum
{
    SCREENSHOT_NAME_ROOM = 24, SCREENSHOT_NAME_DIGITS = 12
};   /* "shot_manual_NN.bmp": path room kept for it, and where NN starts */
/* Async-preemption probe driver (session 11, QIMODE_PATH bit 2). Raises IRQ 5 from a
 * thread that is NOT the exec thread -- exactly how the audio thread raises the Sound
 * Blaster's completion IRQ -- while the guest (qirq.com) spins in pure V86 code that
 * never traps. After each raise it watches [0x714] for up to 50 ms: the kernel clears
 * VDM_INT_HARDWARE when its APC actually dispatches, so a transition here is proof the
 * APC ran even if the guest never sees a vector. Logs the first few raises with the
 * queue call's NTSTATUS, which is the whole experimental record.
 */
/* SYNTHETIC KEYPRESSES (qimode bit 5). The "press a key and it hangs" regression cannot be
 * reproduced from here -- the rig has no remote input -- so drive the exact same path the UI
 * thread uses for a real key: push a make code into the 0x60 FIFO, raise IRQ1, then the break
 * code, repeatedly. If the in-service interlock is wrong this will hang the guest just as a
 * human would, and if it is right the run completes with the key counts advancing.
 */
/* Capture > Take Screenshot (and Ctrl+F5). Puts the current frame on the clipboard as a
 * CF_DIB so it can be pasted straight into Paint, AND writes the same image as a .bmp next
 * to the test results on the share -- the second half means a screenshot can be looked at
 * from the build machine without anyone having to move a file around. 8bpp frames carry
 * their palette in the DIB colour table, which is what makes a mode-13h capture come out
 * with the right colours rather than a grey mush.
 */
static VOID HostScreenshot(VOID)
{
    static INT sequence = 0;
    BITMAPINFOHEADER *bitmapHeader;
    HGLOBAL memoryHandle;
    DWORD width;
    DWORD height;
    DWORD stride;
    DWORD palCount;
    DWORD imageSize;
    DWORD dibSize;
    DWORD bitsPerPixel;
    BYTE *dib;
    BYTE *bits;
    const BYTE *source;

    /* #155: 8-bit AND direct-colour (32bpp, VBE 2 LFB) frames. It returned silently for
     * anything but 8-bit -- the user pressed Ctrl+F5 in Heaven7's direct-colour part and
     * got nothing, with nothing said.
     */
    HOST_LOCK();
    width = g_Video.Frame.Width;
    height = g_Video.Frame.Height;
    bitsPerPixel = g_Video.Frame.BitsPerPixel;
    source = g_Video.Frame.Pixels;
    if (!source || !width || !height || (bitsPerPixel != BMP_PALETTED_BPP && bitsPerPixel != BMP_XRGB_BPP))
    {
        HOST_UNLOCK();
        return;
    }
    stride = (bitsPerPixel == BMP_PALETTED_BPP) ? ((width + BMP_ROW_PAD) & ~BMP_ROW_ALIGN_MASK) : width * BMP_XRGB_PIXEL_BYTES;    /* DIB rows are 4-byte aligned */
    palCount  = (bitsPerPixel == BMP_PALETTED_BPP) ? BMP_PALETTE_ENTRIES : 0;
    imageSize = stride * height;
    dibSize = sizeof(BITMAPINFOHEADER) + palCount * BMP_QUAD_BYTES + imageSize;
    memoryHandle = GlobalAlloc(GMEM_MOVEABLE, dibSize);
    if (!memoryHandle)
    {
        HOST_UNLOCK();
        return;
    }
    dib = (BYTE *)GlobalLock(memoryHandle);
    if (!dib)
    {
        GlobalFree(memoryHandle);
        HOST_UNLOCK();
        return;
    }
    {
        UINT index;
        for (index = 0; index < dibSize; ++index)
            dib[index] = 0;
    }
    bitmapHeader = (BITMAPINFOHEADER *)dib;
    bitmapHeader->biSize = sizeof(BITMAPINFOHEADER);
    bitmapHeader->biWidth = (LONG)width;
    bitmapHeader->biHeight = (LONG)height;   /* positive => bottom-up */
    bitmapHeader->biPlanes = 1;
    bitmapHeader->biBitCount = (WORD)bitsPerPixel;
    bitmapHeader->biCompression = 0;
    bitmapHeader->biSizeImage = imageSize;
    bitmapHeader->biClrUsed = palCount;
    bitmapHeader->biClrImportant = palCount;
    { DWORD index;
    BYTE *palette = dib + sizeof(BITMAPINFOHEADER);
      for (index = 0; index < palCount; ++index)
      {
          UINT32 colour = g_Video.Frame.Palette ? g_Video.Frame.Palette[index] : 0;
          palette[index*BMP_QUAD_BYTES+0] = (BYTE)(colour & BYTE_MASK);          /* B */
          palette[index*BMP_QUAD_BYTES+1] = (BYTE)((colour >> BYTE_SHIFT) & BYTE_MASK);   /* G */
          palette[index*BMP_QUAD_BYTES+2] = (BYTE)((colour >> WORD_SHIFT) & BYTE_MASK);  /* R */
          palette[index*BMP_QUAD_BYTES+3] = 0;
      } }
    bits = dib + sizeof(BITMAPINFOHEADER) + palCount * BMP_QUAD_BYTES;
    { DWORD row, column, rowBytes = (bitsPerPixel == BMP_PALETTED_BPP) ? width : width * BMP_XRGB_PIXEL_BYTES;
      for (row = 0; row < height; ++row)                      /* flip: DIB row 0 is the bottom */
      {
          const BYTE *sourceRow = source + (SIZE_T)(height - 1 - row) * g_Video.Frame.Stride;
          BYTE *destinationRow = bits + (SIZE_T)row * stride;
          for (column = 0; column < rowBytes; ++column)
              destinationRow[column] = sourceRow[column];
          if (bitsPerPixel == BMP_XRGB_BPP)
              for (column = BMP_XRGB_ALPHA_OFFSET; column < rowBytes; column += BMP_XRGB_PIXEL_BYTES)
                  destinationRow[column] = 0;                                                                                                                     /* XRGB: no alpha */
      } }
    HOST_UNLOCK();

    /* The file FIRST, while the block is still ours. It used to be written after the
     * block had been handed to the clipboard -- or FREED, when OpenClipboard failed:
     * a use-after-free on exactly the path nobody tests.
     */
    {
        CHAR path[MAX_PATH];
        PCSTR directory = NTVDMEX_OUT;   /* screenshots are output, not clutter in the root */
        INT index = 0;
        INT index2;
        while (directory[index] && index < MAX_PATH - SCREENSHOT_NAME_ROOM)
        {
            path[index] = directory[index];
            ++index;
        }
        { PCSTR name = HOST_MANUAL_SHOT_NAME;
          for (index2 = 0; name[index2]; ++index2)
              path[index + index2] = name[index2];
          path[index + SCREENSHOT_NAME_DIGITS] = (CHAR)('0' + (sequence / DECIMAL_RADIX) % DECIMAL_RADIX);
          path[index + SCREENSHOT_NAME_DIGITS + 1] = (CHAR)('0' + sequence % DECIMAL_RADIX);
          path[index + index2] = 0; }
        ++sequence;
        { HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
          if (file != INVALID_HANDLE_VALUE)
          {
              BYTE fileHeader[BMP_FILE_HEADER_BYTES];
              DWORD bytesWritten;
              DWORD offset = BMP_FILE_HEADER_BYTES + sizeof(BITMAPINFOHEADER) + palCount * BMP_QUAD_BYTES;
              DWORD fileSize = BMP_FILE_HEADER_BYTES + dibSize;
              fileHeader[0]='B';
              fileHeader[1]='M';
              fileHeader[BMP_FILE_SIZE_OFFSET]=(BYTE)fileSize;
              fileHeader[BMP_FILE_SIZE_OFFSET + 1]=(BYTE)(fileSize>>BYTE_SHIFT);
              fileHeader[BMP_FILE_SIZE_OFFSET + 2]=(BYTE)(fileSize>>WORD_SHIFT);
              fileHeader[BMP_FILE_SIZE_OFFSET + 3]=(BYTE)(fileSize>>TOP_BYTE_SHIFT);
              fileHeader[BMP_RESERVED1_OFFSET]=fileHeader[BMP_RESERVED1_OFFSET + 1]=fileHeader[BMP_RESERVED2_OFFSET]=fileHeader[BMP_RESERVED2_OFFSET + 1]=0;
              fileHeader[BMP_DATA_OFFSET_OFFSET]=(BYTE)offset;
              fileHeader[BMP_DATA_OFFSET_OFFSET + 1]=(BYTE)(offset>>BYTE_SHIFT);
              fileHeader[BMP_DATA_OFFSET_OFFSET + 2]=(BYTE)(offset>>WORD_SHIFT);
              fileHeader[BMP_DATA_OFFSET_OFFSET + 3]=(BYTE)(offset>>TOP_BYTE_SHIFT);
              WriteFile(file, fileHeader, BMP_FILE_HEADER_BYTES, &bytesWritten, NULL);
              WriteFile(file, dib, dibSize, &bytesWritten, NULL);
              CloseHandle(file);
          } }
    }
    GlobalUnlock(memoryHandle);
    if (OpenClipboard(g_Window))                     /* paste-into-Paint path */
    {
        EmptyClipboard();
        if (!SetClipboardData(CF_DIB, memoryHandle))
            GlobalFree(memoryHandle);                                            /* else clipboard owns it */
        CloseClipboard();
    }
    else
        GlobalFree(memoryHandle);
}

/* #155: Tools > Capture > Record Audio -- a toggle over the recorder cfg\wavrec.flag
 * already drives. Each recording is its own numbered file in the capture folder, so a
 * second one never overwrites the first; stopping patches the header (HostRecordFinish).
 */
static VOID HostRecordToggle(VOID)
{
    static INT sequence = 0;
    CHAR path[MAX_PATH];
    CHAR *cursor;

    if (AudioWaveIsRecording())
    {
        HostRecordFinish();
        return;
    }
    cursor = LogPut(path, NTVDMEX_OUT); cursor = LogPut(cursor, "capture_audio_");
    *cursor++ = (CHAR)('0' + (sequence / DECIMAL_RADIX) % DECIMAL_RADIX);
    *cursor++ = (CHAR)('0' + sequence % DECIMAL_RADIX);
    cursor = LogPut(cursor, ".wav");
    ++sequence;
    if (AudioWaveRecordStart(path, g_Wave.SampleHz) == 0)
    {
        CHAR lineBuffer[MAX_PATH + 64];
        CHAR *lineCursor = LogPut(lineBuffer, "STAGE2: audio recording started -> ");
        lineCursor = LogPut(lineCursor, path); lineCursor = LogPut(lineCursor, "\r\n"); LogAppend(LOG_PATH, lineBuffer, lineCursor);
    }
}

/* #155: the folder screenshots and recordings land in, in Explorer. CreateProcess on
 * explorer.exe rather than ShellExecute: no new import for one button.
 */
static VOID HostOpenCaptureFolder(VOID)
{
    CHAR command[MAX_PATH + 32];
    CHAR *cursor;
    STARTUPINFOA startupInfo;
    PROCESS_INFORMATION processInfo;
    INT index;

    cursor = LogPut(command, HOST_EXPLORER_COMMAND); cursor = LogPut(cursor, NTVDMEX_OUT); cursor = LogPut(cursor, "\"");
    for (index = 0; index < (INT)sizeof startupInfo; ++index)
        ((PSTR)&startupInfo)[index] = 0;
    startupInfo.cb = sizeof startupInfo;
    if (CreateProcessA(NULL, command, NULL, NULL, FALSE, 0, NULL, NULL, &startupInfo, &processInfo))
    {
        CloseHandle(processInfo.hThread);
        CloseHandle(processInfo.hProcess);
    }
}

/* Finish an audio recording (cfg\wavrec.flag or, later, the Capture menu): patch the
 * WAV sizes and say what was captured. Every exit path calls it -- a recording whose
 * header still says "0 bytes" is a file most players refuse. Idempotent.
 */
VOID HostRecordFinish(VOID)
{
    CHAR lineBuffer[160];
    CHAR *lineCursor = lineBuffer;
    UINT32 frames;
    UINT32 dropped;

    if (!AudioWaveIsRecording())
        return;
    dropped = AudioWaveRecordDropped();
    frames  = AudioWaveRecordStop();
    lineCursor = LogPut(lineCursor, "STAGE2: audio recording closed: frames=0x"); lineCursor = LogHex(lineCursor, frames);   /* stereo L/R pairs (#189) */
    lineCursor = LogPut(lineCursor, " dropped=0x"); lineCursor = LogHex(lineCursor, dropped);
    lineCursor = LogPut(lineCursor, dropped ? " (the ring filled -- the file has holes)\r\n" : "\r\n");
    LogAppend(LOG_PATH, lineBuffer, lineCursor); SerialOut(lineBuffer, lineCursor);
}

INT           g_MouseRawOk;           /* raw mouse registered with the window */
#define DDFLIP_DRIVER_FLAG  CFG_("ddflip_driver.flag")  /* s86: DirectDraw flip timed by the driver (old path) */
INT g_TextDump = 0;              /* textdump.flag: dump the text screen too */
/* g_SimIntBusy (declared above AsyncInjectIrq) is set across 0300h. */
static LONG g_MouseRawTotalX;
static LONG g_MouseRawTotalY;
/* THE HOST ARROW over the video area, which is a SEPARATE thing from the INT 33h
 * driver cursor above.
 * - IT IS NO LONGER A MENU ITEM OR A HOTKEY, and that is the point: the pointer's
 *   visibility is not an independent knob, it is what EXCLUSIVE MODE looks like.
 *   Captured, the guest owns the mouse and the desktop arrow must be gone; not
 *   captured, the mouse belongs to the Windows desktop and the arrow must be there.
 *   Two controls for one idea is how a UI starts disagreeing with itself -- the user
 *   put it plainly: the pointer "belongs to the Windows XP desktop, and NTVDMEX at
 *   the same time", which is exactly what a separate toggle produces.
 *
 * [WARNING]: SUPERSEDED BY #218 (below): the setting and its menu item are gone; what follows
 * is the history of how it got there.
 *
 * [CAUTION]: THE SETTING SURVIVES AND IS STILL LIVE. `ShowHostCursor` now means "show the
 * desktop arrow over the video WHEN NOT CAPTURED" (default 1). Capture overrides
 * it unconditionally; there is no state in which an exclusive-mode guest shows the
 * host pointer. So the row is still honoured -- it is narrower, not dead.
 *
 * [INFO]: s64: THE SETTING IS NOW PHRASED AS `HideHostCursor`, and the flag with it. The row
 * read "Show the host mouse cursor over the video area" and defaulted to ON, so the
 * only thing it could ever do was be switched OFF -- a checkbox whose default is
 * "yes, do the ordinary thing" is a checkbox that reads as a question nobody asked.
 * Phrased as HIDE, ticking it is the action and leaving it alone is the default.
 *
 * [CAUTION]: The REGISTRY KEY changed with it (`ShowHostCursor` -> `HideHostCursor`) rather than
 * keeping the old name with the opposite meaning, because a stored 1 that used to
 * mean "show" and now means "hide" is a value that silently flips on upgrade. A new
 * name simply defaults, and the default is the behaviour everyone already had.
 */
/* #218 (user, s83 sweep): SMART MOUSE, which REPLACES the setting above (Importance = 1):
 * A program that does NOT use the mouse keeps the Windows pointer; once it has sat
 * still over the video for CURSOR_IDLE_MS it hides, and any movement brings it back
 * -- "like Windows Media Player over a video" -- in a window and fullscreen alike.
 * A program that DOES use the mouse is governed by capture alone (rules 1-6 below).
 * The HideHostCursor setting and View > Show Host Cursor are gone: the toggle "feels
 * jaggy", and a pointer that hides itself needs no knob. UI thread only.
 */
#define CURSOR_IDLE_MS  5000u

/* s84 (user): Settings > Input > Show Host Mouse Cursor -- HOSTCUR_* in settings.h.
 * Smart is the rule above; Always never hides over the video, Never always does.
 */
INT   g_HostCursorMode = HOSTCUR_SMART;
static DWORD g_CursorMovedMs;         /* GetTickCount of the last real movement */
static POINT g_CursorLastPoint = { -1, -1 };
static INT   g_CursorIdle;             /* hidden for stillness, until it next moves */
volatile LONG g_MouseAutoCaptureDone = 0;   /* we have grabbed once; never again */
/* A PROGRAM THAT TOOK THE MOUSE GIVES IT BACK WHEN IT EXITS. (s81, user) (Importance = 1):
 * The grab above is latched per PROCESS, so after a game quit to the prompt the
 * pointer stayed captured over a shell that has no use for it -- and the next game
 * could never be auto-captured again. On every return to a parent the exec thread
 * clears the request and the latch and asks the UI thread (the owner of ClipCursor)
 * to let go.
 */
volatile LONG g_MouseWantRelease = 0;
/* Reported at STAGE2, because "the guest asked and we took it" and "the guest asked
 * and we did not" are different outcomes and a headless run must be able to tell
 * them apart. want=1 fired=0 means the request was raised and the window was not in
 * the foreground -- which is correct behaviour, not a failure, and only a counter
 * can say which of the two happened.
 */
static DWORD         g_MouseAutoCaptureFired = 0;

DWORD         g_MouseAltCalls;       /* events delivered to an alternate handler */

enum                                         /* wired command IDs */
{
    IDM_STUB = 1,                            /* every not-yet-wired item */
    IDM_FILE_EXIT, IDM_FILE_CLOSEPROG,
    IDM_FILE_OPEN,                           /* #153: Open Executable... */
    IDM_DISP_FULLSCREEN,
    IDM_INPUT_CAPTURE,          /* (IDM_INPUT_CURSOR retired -- see g_cursor_show) */
    IDM_FILE_SETTINGS,
    IDM_CAP_SHOT,
    IDM_HELP_ABOUT,
    IDM_TRAY_SHOW,                           /* bring the hidden host window back */
    IDM_FILE_INSTALL, IDM_FILE_UNINSTALL, IDM_FILE_STATUS,   /* GH #13 */
    /* THE EDIT ITEMS HAVE REAL IDS PURELY SO THEY CAN BE ADDRESSED:
     * Implemented since s82 (#154 -- see g_MarkMode); they needed their own
     * ids first because "grey these five in a graphics mode" names them one at a
     * time, and EnableMenuItem with MF_BYCOMMAND cannot distinguish five items
     * that all carry IDM_STUB.
     *
     * [CAUTION]: THIS IS NOT THE SCAFFOLD-STUB DECISION BEING RE-LITIGATED. That decision
     * says an UNIMPLEMENTED item stays enabled, and in text mode these still are.
     * Greying them in a graphics mode is a different claim -- mark, copy and
     * paste operate on a CHARACTER GRID, and in mode 13h there is no such thing
     * to select -- so it is about what the item MEANS here, not about whether it
     * is finished. See MenuSyncModal().
     */
    IDM_EDIT_MARK, IDM_EDIT_COPY, IDM_EDIT_COPYSCREEN, IDM_EDIT_PASTE,
    IDM_EDIT_SELECTALL,
    /* The View menu's two CHECKBOX settings (the combos are ranges, below). */
    IDM_VIEW_VSYNC, IDM_VIEW_BLINK,
    IDM_VIEW_HOSTCURSOR,                     /* RETIRED by #218; kept so later ids keep their numbers */
    /* #155. APPENDED, not beside IDM_CAP_SHOT: the rig scripts post these ids as
     * NUMBERS (textedit.bat: 14-18), and an insertion renumbers everything after it.
     */
    IDM_CAP_AUDIO, IDM_CAP_FOLDER,

    /* ONE CONTIGUOUS RANGE PER DROPDOWN SETTING (Importance = 1):
     * Every combo on the Display page, plus the CPU speed, appears in a menu as a
     * run of ids `base + index`, so one handler serves all of them: find which
     * range the id fell in, and the offset IS the setting's value. No per-item
     * cases, and nothing to keep in step when a list gains an entry.
     *
     * [CAUTION]: THE ITEM TEXT IS NOT DUPLICATED HERE. MenuCombo() below builds each
     * submenu by walking g_SetDefinitions' own `items` string -- the same string the
     * dialog fills its combo from -- so the menu and the dialog cannot disagree
     * about what the options are or which index each one means. Duplicating the
     * list is how a menu ends up setting Scale2x when it says Scanlines.
     *
     * [CAUTION]: SPAN IS THE MOST ITEMS ANY ONE LIST MAY HAVE. MenuCombo() stops at it, so
     * overflowing collides with nothing -- the extra items simply do not appear,
     * which is visible, rather than silently invoking the next setting along.
     */
#define IDM_COMBO_SPAN  32
    IDM_RECENT_0    = 180,                   /* #153: Open Recent, MRU_MAX items */
    IDM_COMBO_BASE  = 200,
    IDM_SPEED_0     = IDM_COMBO_BASE + 0 * IDM_COMBO_SPAN,   /* CPU speed (#56) */
    IDM_WINSIZE_0   = IDM_COMBO_BASE + 1 * IDM_COMBO_SPAN,
    IDM_RENDER_0    = IDM_COMBO_BASE + 2 * IDM_COMBO_SPAN,
    IDM_SCALER_0    = IDM_COMBO_BASE + 3 * IDM_COMBO_SPAN,
    IDM_FILTER_0    = IDM_COMBO_BASE + 4 * IDM_COMBO_SPAN,
    IDM_FSKIP_0     = IDM_COMBO_BASE + 5 * IDM_COMBO_SPAN,
    IDM_ASPECT_0    = IDM_COMBO_BASE + 6 * IDM_COMBO_SPAN,
    IDM_TINT_0      = IDM_COMBO_BASE + 7 * IDM_COMBO_SPAN,   /* #229 (user, s84) */
    IDM_FIT_0       = IDM_COMBO_BASE + 8 * IDM_COMBO_SPAN    /* #325 */
};

/* Which setting each range drives. The ONLY place the two are tied together. */
static const struct
{
    UINT Base;
    INT IsSet;
}

g_MenuCombos[] = {
    { IDM_SPEED_0,   SET_SPEEDMODE }, { IDM_WINSIZE_0, SET_WINSIZE   },
    { IDM_RENDER_0,  SET_RENDERER  }, { IDM_SCALER_0,  SET_SCALER    },
    { IDM_FILTER_0,  SET_FILTER    }, { IDM_FSKIP_0,   SET_FRAMESKIP },
    { IDM_ASPECT_0,  SET_ASPECT    }, { IDM_TINT_0,    SET_TINT      },
    { IDM_FIT_0,     SET_FIT       },
};
#define MENU_COMBO_N    ((INT)(sizeof g_MenuCombos / sizeof g_MenuCombos[0]))

/* ...and the checkbox ones, same idea. */
static const struct
{
    UINT Id;
    INT IsSet;
}

g_MenuChecks[] = {
    { IDM_VIEW_VSYNC, SET_VSYNC }, { IDM_VIEW_BLINK, SET_BLINKCURSOR },
};
#define MENU_CHECK_N    ((INT)(sizeof g_MenuChecks / sizeof g_MenuChecks[0]))

static VOID MenuItem (HMENU menu, PCSTR text, UINT thunkId)
{
    AppendMenuA(menu, MF_STRING, thunkId, text);
}

static VOID MenuSeparator(HMENU menu)
{
    AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
}

static VOID MenuSubmenu(HMENU parent, PCSTR text, HMENU child)
{
    AppendMenuA(parent, MF_POPUP, (UINT_PTR)child, text);
}

static HMENU MenuPopup(VOID)
{
    return CreatePopupMenu();
}

/* A submenu built FROM THE SETTING ITSELF: one item per entry of g_SetDefinitions[set]'s
 * own '|'-separated list, at ids base+0, base+1, ... See the note by IDM_COMBO_BASE
 * for why the text is taken from there rather than written out again here.
 */
static VOID MenuCombo(HMENU parent, PCSTR label, INT set, UINT base)
{
    HMENU submenu = MenuPopup();
    CHAR item[64];
    INT index;

    for (index = 0; index < IDM_COMBO_SPAN
                && SettingsItem(g_SetDefinitions[set].Items, index, item, (INT)sizeof item); ++index)
        MenuItem(submenu, item, base + (UINT)index);
    MenuSubmenu(parent, label, submenu);
}

/* THE MENU BAR AFTER THE SETTINGS MOVE:
 * CPU, Display, Audio, Input and Drive are GONE from the bar. Everything they held
 * that was configuration now lives on a tab of the Settings dialog, which is one
 * place to look instead of five menus deep in submenus, and one store instead of a
 * tick per item.
 * - What stayed behind is what was never a setting: Fullscreen, input
 *   capture, the mount commands. Those are ACTIONS -- things you do once, now, and
 *   usually by keystroke. A command you reach for mid-game does not belong behind an
 *   OK button, so View and Machine keep them.
 * The rest is still scaffold: items carrying IDM_STUB no-op until they are wired.
 */
static HMENU g_RecentMenu;                  /* File > Open Recent (#153) */
static HMENU BuildMenu(VOID)
{
    HMENU menuBar = CreateMenu();
    HMENU menu;
    HMENU submenu;
    HMENU tools;

    menu = MenuPopup();                                                   /* File */
    /* #153. No Ctrl+O: that chord belongs to the DOS program (WordStar's own menu). The
     * Open Recent list is filled when it opens -- see MenuRecentFill.
     */
    MenuItem(menu, MENU_TEXT_OPEN_EXECUTABLE, IDM_FILE_OPEN);
    g_RecentMenu = MenuPopup();
    MenuSubmenu(menu, MENU_TEXT_OPEN_RECENT, g_RecentMenu);
    /* Save State / Load State removed (s81, #145, user decision); what a real
     * implementation would need is #146.
     */
    MenuSeparator(menu);
    /* The old "Configuration" submenu (Edit Config File, Open Config Folder, ...)
     * described a config FILE that never existed; the store is HKCU and the dialog
     * is how you edit it. One entry, and it is this one.
     */
    MenuItem(menu, MENU_TEXT_CLOSE_PROGRAM, IDM_FILE_CLOSEPROG);
    MenuItem(menu, MENU_TEXT_EXIT_ACCELERATOR, IDM_FILE_EXIT);
    MenuSubmenu(menuBar, MENU_TEXT_FILE, menu);

    /* EDIT IS A TEXT-MODE MENU, AND MenuSyncModal() SAYS SO AT OPEN TIME:
     * Every item here works on the character grid the text renderer maintains.
     * In a graphics mode there is no grid to mark, copy or paste into, so they
     * are greyed rather than left to fail silently -- see the note by
     * IDM_EDIT_MARK for why that is not the scaffold-stub rule being bent.
     */
    menu = MenuPopup();                                                   /* Edit */
    /* #154: no Ctrl+C / Ctrl+V labels -- those keys belong to the DOS program (Ctrl+C
     * is Break), and a label naming a shortcut that does not exist is a small lie.
     */
    MenuItem(menu,MENU_TEXT_MARK,IDM_EDIT_MARK);
    MenuItem(menu,MENU_TEXT_COPY,IDM_EDIT_COPY);
    MenuItem(menu,MENU_TEXT_COPY_SCREEN,IDM_EDIT_COPYSCREEN);
    MenuItem(menu,MENU_TEXT_PASTE,IDM_EDIT_PASTE);
    MenuItem(menu,MENU_TEXT_SELECT_ALL,IDM_EDIT_SELECTALL);
    MenuSubmenu(menuBar, MENU_TEXT_EDIT, menu);

    /* VIEW IS THE DISPLAY PAGE, AND IT IS SESSION-ONLY (Importance = 1):
     * Every knob on the Settings dialog's Display tab is here too, because these
     * are the ones you reach for WHILE something is running -- a scaler you want
     * to compare, a window you want bigger -- and going through a modal dialog to
     * try one is the wrong shape for that.
     *
     * [CAUTION]: THE TWO ROUTES MEAN DIFFERENT THINGS, DELIBERATELY. The dialog edits the
     * SAVED configuration (g_SettingsDisk) and writing it is what OK does. The menu
     * changes only what is in force right now (g_Settings) and never touches the
     * registry, so anything tried here is gone at the next launch. That is the
     * whole point: experimenting must not silently reconfigure the machine.
     * - Renderer is GDI | DirectDraw and both are real (s81, #147): the window is
     *   always GDI, and DirectDraw makes FULLSCREEN the exclusive DirectDraw mode.
     *   The list comes from g_SetDefinitions so it says exactly what the dialog says.
     */
    menu = MenuPopup();                                                   /* View */
    MenuItem(menu,MENU_TEXT_FULLSCREEN,IDM_DISP_FULLSCREEN);
    MenuSeparator(menu);
    MenuCombo(menu, MENU_TEXT_WINDOW_SIZE, SET_WINSIZE,   IDM_WINSIZE_0);
    MenuCombo(menu, MENU_TEXT_RENDERER,    SET_RENDERER,  IDM_RENDER_0);
    MenuCombo(menu, MENU_TEXT_SCALER,      SET_SCALER,    IDM_SCALER_0);
    MenuCombo(menu, MENU_TEXT_FILTERING,   SET_FILTER,    IDM_FILTER_0);
    MenuCombo(menu, MENU_TEXT_FRAME_SKIP,  SET_FRAMESKIP, IDM_FSKIP_0);
    /* -- ASPECT RATIO IS A LOCK ON THE WINDOW, not just a letterbox. Picking a
     * ratio constrains the window's shape as you drag it, so the picture fills the
     * client and there are no bars at all -- letterboxing only reappears if the
     * window ends up off-aspect anyway (maximised). "None" is a free resize.
     */
    MenuCombo(menu, MENU_TEXT_ASPECT_RATIO, SET_ASPECT,    IDM_ASPECT_0);
    MenuCombo(menu, MENU_TEXT_FULL_SCREEN_FIT, SET_FIT,     IDM_FIT_0);       /* #325 */
    /* #229 (user, s84): "Nice, working! Can you add these to the View menu as well." */
    MenuCombo(menu, MENU_TEXT_COLOUR_FILTER, SET_TINT,     IDM_TINT_0);
    /* #230 (docs/EMULATION.md): "force vsync for programs that don't ask for it" is
     * what this always did -- every blit is timed to the monitor's blank whether or
     * not the guest waits for retrace -- so it is named for that. DirectDraw's
     * fullscreen flip waits for the blank regardless.
     */
    MenuItem(menu,MENU_TEXT_FORCE_VSYNC,IDM_VIEW_VSYNC);              /* user, s81: with the picture group */
    MenuSeparator(menu);
    MenuItem(menu,MENU_TEXT_BLINK_CURSOR,IDM_VIEW_BLINK);
    /* #218 (user, s83 sweep): "Show Host Cursor" (#157) is GONE AGAIN, for good -- it
     * "feels jaggy". A program that does not use the mouse now keeps the pointer and
     * it hides itself after 5 s still over the video; see CURSOR_IDLE_MS.
     */
    /* [CAUTION]: "Show Host Cursor" AND ITS Ctrl+F8 HOTKEY WERE REMOVED HERE, DELIBERATELY.
     * The desktop pointer's visibility is not a knob of its own -- it is what
     * exclusive mode looks like, and Win+F10 is the control for that. See the
     * note on g_cursor_show.
     */
    MenuSubmenu(menuBar, MENU_TEXT_VIEW, menu);

    /* TOOLS: ONE MENU FOR EVERYTHING THAT IS NOT THE PICTURE (Importance = 1):
     * The bar was File / Edit / View / Machine / Capture / Debug / Help -- seven
     * tops for a window whose whole job is to show one DOS program. Machine,
     * Capture and Debug are each a handful of items nobody opens mid-game, and
     * three top-level menus is how a menu bar stops being scannable.
     * - So they become SUBMENUS of one Tools menu, in place, with their contents
     *   untouched. Nothing is renamed and nothing is dropped: a user who knew where
     *   Swap Disk was finds it under Tools > Machine, one level deeper.
     * - And the four File items that were never file operations come here too.
     *   Settings, Install, Uninstall and Installation Status are all things you do
     *   TO the machine or TO this installation; File now holds only what opens,
     *   saves or closes something, which is what the word means everywhere else on
     *   this desktop.
     *
     * [CAUTION]: SETTINGS GOES LAST, under a separator. Tools > Options at the bottom is the
     * convention every Windows application of this era follows, and the install
     * verbs sit above it because they are the destructive ones -- they should not
     * be the thing your hand lands on.
     */
    tools = MenuPopup();                                               /* Tools */

    /* -- s81 (#148), user decision: every UNIMPLEMENTED item is removed -- Restart,
     * Pause, Ctrl+Alt+Del, Key Mapper, the mount/boot/swap/drive items, the whole
     * Debug submenu, Help's Quick Start / Keyboard Shortcuts. They are recorded for
     * review in #149, #150 and #151. This reverses the old scaffold-stubs-enabled rule
     * for these items: the user found dead menu items worse than absent ones.
     */
    /* THE APPROXIMATE-SPEED DROPDOWN. (GH #56) (Importance = 1):
     * The user asked for this in the MENU, and the menu is where it belongs: it
     * is a knob you reach for while watching something run too fast, not one you
     * set up before launching.
     *
     * [CAUTION]: AND IT IS SESSION-ONLY, like View. It used to write straight to the
     * registry, which made it the odd one out the moment the display knobs
     * arrived: a speed tried from the menu became the machine's permanent speed,
     * while a scaler tried from the menu did not. One rule -- the menu is for
     * trying things, the dialog is for keeping them.
     */
    /* user, s81: with only two items left, Machine is flattened into Tools itself. */
    MenuCombo(tools, MENU_TEXT_LIMIT_SPEED, SET_SPEEDMODE, IDM_SPEED_0);
    {   UINT speed;                                  /* #224: faster than this PC -> grey */
        for (speed = 1; speed < CPUSPEED_COUNT; ++speed)
            if (!CpuSpeedIsAvailable(speed, HostCpuMhz()))
                EnableMenuItem(tools, IDM_SPEED_0 + speed, MF_BYCOMMAND | MF_GRAYED); }
    /* The accelerator column names the RELEASE, because that is the one a captured
     * user needs and cannot look up -- the menu is unreachable while capture is held.
     */
    MenuItem(tools,MENU_TEXT_CAPTURE_MOUSE,IDM_INPUT_CAPTURE);
    MenuSeparator(tools);

    menu = MenuPopup();                                                   /* Tools>Capture */
    MenuItem(menu,MENU_TEXT_SCREENSHOT_ACCELERATOR,IDM_CAP_SHOT);
    MenuSubmenu(menu,MENU_TEXT_RECORD_VIDEO,(submenu=MenuPopup(),MenuItem(submenu,MENU_TEXT_START_STOP,IDM_STUB),submenu));
    MenuItem(menu,MENU_TEXT_RECORD_AUDIO,IDM_CAP_AUDIO);          /* #155: ticked while recording */
    MenuSubmenu(menu,MENU_TEXT_RECORD_MUSIC,(submenu=MenuPopup(),MenuItem(submenu,MENU_TEXT_START_STOP,IDM_STUB),submenu));
    MenuSeparator(menu);
    MenuItem(menu,MENU_TEXT_CAPTURE_FOLDER,IDM_CAP_FOLDER);
    MenuItem(menu,MENU_TEXT_CAPTURE_SETTINGS,IDM_STUB);
    MenuSubmenu(tools, MENU_TEXT_CAPTURE, menu);

    MenuSeparator(tools);
    /* -- INSTALLING IS AN ACTION, SO IT IS ON A MENU AND NOT A SETTINGS PAGE.
     * It changes a machine-wide registry value, needs Administrator, and is the
     * one thing here that outlives the process -- none of which belongs behind a
     * tab of checkboxes. The same three verbs exist on the command line
     * (/install, /uninstall, /status) for scripted use.
     */
    MenuItem(tools, MENU_TEXT_INSTALL, IDM_FILE_INSTALL);
    MenuItem(tools, MENU_TEXT_UNINSTALL, IDM_FILE_UNINSTALL);
    MenuItem(tools, MENU_TEXT_INSTALL_STATUS, IDM_FILE_STATUS);
    MenuSeparator(tools);
    /* The old "Configuration" submenu (Edit Config File, Open Config Folder, ...)
     * described a config FILE that never existed; the store is HKCU and the dialog
     * is how you edit it. One entry, and it is this one.
     */
    MenuItem(tools, MENU_TEXT_SETTINGS, IDM_FILE_SETTINGS);
    MenuSubmenu(menuBar, MENU_TEXT_TOOLS, tools);

    menu = MenuPopup();                                                   /* Help */
    MenuItem(menu,MENU_TEXT_ABOUT,IDM_HELP_ABOUT);
    MenuSubmenu(menuBar, MENU_TEXT_HELP, menu);
    return menuBar;
}

/* A WIN16 GUEST HAS NO VDM WINDOW, AND NOW NEITHER DO WE. (#128, s.42) (Importance = 2):
 * On real XP a 16-bit Windows program shows no VDM window at all: it puts its own
 * windows on the desktop and the machine hosting it is invisible. Ours used to
 * sit on top of the guest's own windows showing a BLACK TEXT SCREEN -- not merely
 * redundant but misleading, because there is nothing for a Win16 guest to draw
 * there. krnl386 never sets a video mode.
 *
 * For a `-w` launch the window is created and never shown. But the menu behind
 * it is not useless -- Settings, Close Program, About and the screenshot are all
 * still things you might want during a Win16 run -- so it goes to the SYSTEM
 * TRAY, which is where a running-but-invisible machine belongs.
 *
 * [CAUTION]: THE WINDOW IS CREATED EITHER WAY, and that is deliberate rather than lazy: it
 * owns the present surface, the raw-input registration, the frame timer and the
 * tray callbacks. Hidden is a state; absent would be a second code path through
 * everything the UI thread does.
 * - A DOS guest keeps its window and gets no icon. Whether it should have one too
 *   is an open question (the user's, and a fair one) -- the only thing that would
 *   change is the condition on this flag, which is why it is one flag.
 */
#define WM_TRAY         (WM_APP + 1)
#define WM_APP_PRESENT  (WM_APP + 2)    /* The guest finished a frame: present now (Auto) */

/* Runs on the GUEST thread inside status_in, under the device lock: post and leave.
 * One in flight at a time so a fast poller cannot flood the queue.
 */
VOID HostPresentHook(PVOID context)
{
    (VOID)context;
    if (g_UiTickMinimumMs != UITICK_AUTO || !g_Window)
        return;
    if (InterlockedCompareExchange(&g_UiPresentPending, 1, 0) == 0)
        PostMessageA(g_Window, WM_APP_PRESENT, 0, 0);
}

#define TRAY_ID     1
static INT  g_TrayOn    = 0;                /* the icon is currently installed */

static VOID TrayAdd(HINSTANCE instance, HWND window)
{
    NOTIFYICONDATAA notifyIconData;
    PCSTR tip = HOST_TRAY_WIN16_TIP;
    INT index;

    if (g_TrayOn)
        return;
    ZeroMemory(&notifyIconData, sizeof notifyIconData);
    notifyIconData.cbSize           = sizeof notifyIconData;
    notifyIconData.hWnd             = window;
    notifyIconData.uID              = TRAY_ID;
    notifyIconData.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    notifyIconData.uCallbackMessage = WM_TRAY;
    notifyIconData.hIcon            = LoadIconA(instance, MAKEINTRESOURCEA(IDI_MAINICON));
    if (!notifyIconData.hIcon)
        notifyIconData.hIcon = LoadIconA(NULL, IDI_APPLICATION);
    for (index = 0; tip[index] && index < (INT)sizeof notifyIconData.szTip - 1; ++index)
        notifyIconData.szTip[index] = tip[index];
    notifyIconData.szTip[index] = 0;
    g_TrayOn = Shell_NotifyIconA(NIM_ADD, &notifyIconData) ? 1 : 0;
}

VOID TrayRemove(HWND window)
{
    NOTIFYICONDATAA notifyIconData;

    if (!g_TrayOn)
        return;
    ZeroMemory(&notifyIconData, sizeof notifyIconData);
    notifyIconData.cbSize = sizeof notifyIconData;
    notifyIconData.hWnd = window;
    notifyIconData.uID = TRAY_ID;
    Shell_NotifyIconA(NIM_DELETE, &notifyIconData);
    g_TrayOn = 0;
}

/* THE NTVDMEX MANAGER: ONE TRAY ICON FOR EVERY PROGRAM. (GH #281, s88) (Importance = 2):
 * User design: the first host brings up `ntvdmex.exe`, every host (DOS and
 * Win16) appears in its one tray menu, and it exits when the last has gone.
 * Protocol and lifetime in src/host/mgrproto.h; the manager in src/manager/.
 * - THIS SIDE IS ONE LOW-PRIORITY THREAD that re-announces the host every two
 *   seconds. Not the UI thread: a SendMessageTimeout on a frame tick is a stall
 *   Skyroads would feel, and the announcement has no deadline worth that.
 *
 * [CAUTION]: IF ntvdmex.exe IS NOT BESIDE US, nothing changes: a Win16 host keeps its own
 * tray icon exactly as before (g_ManagerAvailable = 0), and a DOS host never had one.
 */
static INT   g_ManagerAvailable;                    /* ntvdmex.exe exists beside the host */
static UINT  g_ManagerCommandMessage;                   /* the registered manager->host message */
static CHAR  g_ManagerExe[MAX_PATH];
/* for the log */
static DWORD g_ManagerHellos;
static DWORD g_ManagerLaunches;
typedef INT (WINAPI *PFN_INTGETWT)(HWND, LPWSTR, INT);

/* What the menu calls this program. DOS: the running program's name ("Doom"), or
 * "MS-DOS Prompt" for a bare shell. Win16: its main window's caption up to " - "
 * ("Notepad - (Untitled)" -> "Notepad"), else the program file's name. The caption
 * is read with InternalGetWindowText, which takes it from the window WITHOUT a
 * message -- GetWindowText from this thread would wait on the exec thread, which is
 * usually inside the guest.
 */
static VOID ManagerName(PSTR out, HWND *show)
{
    CHAR raw[MGR_NAME_SIZE];
    INT index;
    INT length = 0;
    PCSTR source = NULL;

    raw[0] = 0;
    *show = g_Window;
    if (g_WowLaunch)
    {
        static PFN_INTGETWT internalGetWindowText;
        if (!internalGetWindowText) internalGetWindowText = (PFN_INTGETWT)(ULONG_PTR)GetProcAddress(GetModuleHandleA(HOST_MODULE_USER32),
                                                                  HOST_EXPORT_INTERNAL_GET_WINDOW_TEXT);
        *show = NULL;
        for (index = 0; index < WOWUSER_MAX_WIN; ++index)
        {
            const WOWUSER_WINDOW *wowWindow = &g_WowUserWindows[index];
            HWND window = wowWindow->Window32;
            if (!wowWindow->Window16 || wowWindow->Parent || wowWindow->IsDying || wowWindow->IsForeign || !window || !IsWindowVisible(window))
                continue;
            *show = window;
            if (internalGetWindowText)
            {
                WCHAR wideBuffer[MGR_NAME_SIZE];
                INT length16 = internalGetWindowText(window, wideBuffer, MGR_NAME_SIZE);
                if (length16 > 0)
                    WideCharToMultiByte(CP_ACP, 0, wideBuffer, length16 + 1, raw, sizeof raw, NULL, NULL);
            }
            break;
        }
        if (!raw[0])
            source = g_WowCommandProgram;
    }
    else
    {
        source = g_ProgramName;
    }
    if (source)
    {
        PCSTR baseName = source;
        PCSTR cursor;
        for (cursor = source; *cursor; ++cursor)
            if (*cursor == '\\' || *cursor == '/')
                baseName = cursor + 1;
        if (!*baseName || !lstrcmpiA(baseName, HOST_COMMAND_COM) || !lstrcmpiA(baseName, HOST_CMD_EXE)
            || !lstrcmpA(baseName, HOST_PROGRAM_NONE))
        {
            lstrcpynA(out, g_WowLaunch ? HOST_MANAGER_WIN16_NAME : HOST_MANAGER_DOS_NAME, MGR_NAME_SIZE);
            return;
        }
        /* "DOOM.EXE" -> "Doom": the base name, first letter up, the rest down. */
        for (index = 0; baseName[index] && baseName[index] != '.' && length < MGR_NAME_SIZE - 1; ++index)
        {
            CHAR character = baseName[index];
            if (length == 0)
            {
                if (character >= 'a' && character <= 'z')
                    character = (CHAR)(character - ASCII_CASE_BIT);
            }
            else
            {
                if (character >= 'A' && character <= 'Z')
                    character = (CHAR)(character + ASCII_CASE_BIT);
            }
            raw[length++] = character;
        }
        raw[length] = 0;
    }
    raw[MGR_NAME_SIZE - 1] = 0;
    for (index = 0; raw[index]; ++index)                         /* "Notepad - (Untitled)" -> "Notepad" */
        if (raw[index] == ' ' && raw[index + 1] == '-' && raw[index + 2] == ' ')
        {
            raw[index] = 0;
            break;
        }
    lstrcpynA(out, raw[0] ? raw : HOST_PRODUCT_NAME, MGR_NAME_SIZE);
}

enum
{
    MANAGER_RELAUNCH_MS = 5000, MANAGER_START_WAIT_MS = 500, MANAGER_SEND_TIMEOUT_MS = 500, MANAGER_POLL_MS = 2000
};   /* ManagerThread */
static DWORD WINAPI ManagerThread(LPVOID unused)
{
    DWORD lastLaunch = 0;

    (VOID)unused;
    while (g_Window && IsWindow(g_Window))
    {
        HWND manager = FindWindowA(MGR_CLASS, NULL);
        if (!manager)
        {
            DWORD now = GetTickCount();
            if (!lastLaunch || now - lastLaunch >= MANAGER_RELAUNCH_MS)
            {
                STARTUPINFOA startupInfo;
                PROCESS_INFORMATION processInfo;
                CHAR commandLine[MAX_PATH + 4];
                lastLaunch = now;
                ZeroMemory(&startupInfo, sizeof startupInfo);
                startupInfo.cb = sizeof startupInfo;
                commandLine[0] = '"';
                lstrcpynA(commandLine + 1, g_ManagerExe, MAX_PATH);
                lstrcatA(commandLine, "\"");
                if (CreateProcessA(g_ManagerExe, commandLine, NULL, NULL, FALSE, DETACHED_PROCESS,
                                   NULL, NULL, &startupInfo, &processInfo))
                {
                    ++g_ManagerLaunches;
                    CloseHandle(processInfo.hThread);
                    CloseHandle(processInfo.hProcess);
                }
            }
            Sleep(MANAGER_START_WAIT_MS);                              /* give it a moment to appear */
            continue;
        }
        {   MGR_MESSAGE message;
        COPYDATASTRUCT copyData;
        HWND show;
        DWORD_PTR result = 0;
            ZeroMemory(&message, sizeof message);
            message.Magic = MGR_MAGIC;
            message.Version = MGR_VERSION;
            message.Size = sizeof message;
            message.Operation = MGR_OP_HELLO;
            message.ProcessId = GetCurrentProcessId();
            message.Kind = g_WowLaunch ? MGR_KIND_WIN16 : MGR_KIND_DOS;
            ManagerName(message.Name, &show);
            message.CommandWindow = (DWORD)(ULONG_PTR)g_Window;
            message.ShowTargetWindow = (DWORD)(ULONG_PTR)show;
            copyData.dwData = MGR_MAGIC;
            copyData.cbData = sizeof message;
            copyData.lpData = &message;
            if (SendMessageTimeoutA(manager, WM_COPYDATA, (WPARAM)g_Window, (LPARAM)&copyData,
                                    SMTO_ABORTIFHUNG, MANAGER_SEND_TIMEOUT_MS, &result) && result)
                ++g_ManagerHellos;
        }
        Sleep(MANAGER_POLL_MS);
    }
    return 0;
}

static VOID ManagerStart(VOID)
{
    PSTR cursor;
    HANDLE thread;

    g_ManagerCommandMessage = RegisterWindowMessageA(MGR_CMD_MSGNAME);
    if (!GetModuleFileNameA(NULL, g_ManagerExe, sizeof g_ManagerExe))
        return;
    for (cursor = g_ManagerExe + lstrlenA(g_ManagerExe); cursor > g_ManagerExe && cursor[-1] != '\\'; --cursor) ;
    if (cursor - g_ManagerExe + lstrlenA(MGR_EXE) >= (INT)sizeof g_ManagerExe)
        return;
    lstrcpyA(cursor, MGR_EXE);
    if (GetFileAttributesA(g_ManagerExe) == INVALID_FILE_ATTRIBUTES)
        return;
    g_ManagerAvailable = 1;
    thread = CreateThread(NULL, 0, ManagerThread, NULL, 0, NULL);
    if (thread)
    {
        SetThreadPriority(thread, THREAD_PRIORITY_LOWEST);
        CloseHandle(thread);
    }
    else
        g_ManagerAvailable = 0;
}

/* The tray's context menu: the items from the menu bar that still MEAN something
 * when there is no window to look at. Deliberately short -- a tray menu that
 * mirrored the whole bar would offer Fullscreen and Capture Input for a machine
 * with no screen and no focus.
 */
static VOID TrayMenu(HWND window)
{
    HMENU menu = CreatePopupMenu();
    POINT point;

    if (!menu)
        return;
    /* s88 (user): a Win16 host's machine window is never shown, so there is no
     * "Show NTVDMEX Window" here any more (this menu only exists for Win16).
     */
    AppendMenuA(menu, MF_STRING, IDM_FILE_SETTINGS,  MENU_TEXT_SETTINGS);
    AppendMenuA(menu, MF_STRING, IDM_CAP_SHOT,       MENU_TEXT_SCREENSHOT);
    AppendMenuA(menu, MF_STRING, IDM_HELP_ABOUT,     MENU_TEXT_ABOUT);
    AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(menu, MF_STRING, IDM_FILE_CLOSEPROG, MENU_TEXT_CLOSE_PROGRAM);
    AppendMenuA(menu, MF_STRING, IDM_FILE_EXIT,      MENU_TEXT_EXIT);
    GetCursorPos(&point);
    /* [CAUTION]: SetForegroundWindow FIRST and a stray post AFTER: without them a tray
     * menu does not dismiss when you click away from it. This is the documented
     * dance and it is not optional -- a menu that will not close is worse than
     * no menu.
     */
    SetForegroundWindow(window);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, point.x, point.y, 0, window, NULL);
    PostMessageA(window, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

static HWND g_Status;                        /* the native comctl32 status bar */

/* THE CAPTION IS A CONSTANT NOW:
 * It used to carry the running program's name and, while captured, the release
 * chord. Both have moved to the status strip below, where they sit beside the
 * machine state they belong with -- and where a 63-character program name can no
 * longer push the release chord off the end of a caption the window manager is
 * free to truncate. The window is a DOS machine whatever happens to be running
 * inside it, so the title says exactly that and nothing else.
 */
/* User, s84: "Windows NT Virtual DOS Machine" (was "Microsoft Windows XP ..."). The rig
 * harness finds the window by this string -- scripts/bm/*.bat and rigshot.c follow it.
 */
#define VDM_WIN_TITLE   "Windows NT Virtual DOS Machine"

/* THE STATUS STRIP:
 * PROG.EXE | 16-bit Real mode | the input-capture state and the chord for it.
 *
 * [CAUTION]: THREE REAL PARTS, NOT ONE STRING WITH BARS IN IT. This used to pack the program
 * name, the bitness and the CPU mode into the left part separated by a literal
 * "   |   ", which is a drawn-by-hand imitation of the sunken divider comctl32
 * already puts between parts -- and it did not line up with the genuine divider
 * before the right-hand part, so the strip had two kinds of separator on it.
 *
 * [INFO]: AND THE BITNESS AND THE MODE ARE ONE FACT, so they are one field: "16-bit Real
 * mode", "32-bit Protected mode". They were never independent -- 32-bit only ever
 * means a DPMI client in protected mode -- so splitting them invited the reader to
 * look for a combination that cannot occur.
 * The mode is not decoration. A DPMI guest crossing into 32-bit protected mode is
 * the largest single change of behaviour this host has -- different interrupt
 * delivery, different pointer widths, a different service path for every INT -- and
 * before it was on the strip the only way to know it had happened was the log.
 * The mode pair is not decoration. A DPMI guest crossing into 32-bit protected mode
 * is the largest single change of behaviour this host has -- different interrupt
 * delivery, different pointer widths, a different service path for every INT -- and
 * until now the only way to know it had happened was to read the log afterwards.
 *
 * [CAUTION]: IN EXCLUSIVE FULLSCREEN NEITHER PART IS VISIBLE: the DirectDraw primary covers
 * the strip exactly as it covers the menu bar. Win+F10 still releases.
 */
/* s84 (user spec): EVERY PART LEFT-ALIGNED, EACH SIZED TO ITS TEXT (Importance = 1):
 * appname.exe | 16-bit Real Mode | 66 MHz
 * appname.exe | 32-bit Protected Mode | 133 MHz | Press WIN to release mouse
 * appname.exe | 32-bit Protected Mode | Unlimited | Click video to capture mouse
 * The capture message used to live in a fixed-width part against the RIGHT edge,
 * which cut it off; it now follows the speed. A program that never used the mouse
 * gets no fourth part at all -- there is nothing to say about capture. The speed is
 * the clock alone ("66 MHz", "Unlimited"), never the CPU's name.
 */
/* ON it now */
static CHAR g_StatusLeft[128];
static CHAR g_StatusMode[96];
static CHAR g_StatusSpeed[32];
static CHAR g_StatusRight[64];
/* #325: the window's whole scale (setting or drag), the frame size it was sized for, and
 * whether 1x did not fit the screen and was scaled down. See HostApplyScale.
 */
static INT g_ScaleFactor = 0;            /* the scale the window is AT (after fitting) */
static INT g_ScaleWant = 0;         /* the scale ASKED for (setting or drag) -- every mode
                                        change starts from this, so one mode too big for 2x
                                        does not leave the next one stuck at 1x            */
static INT g_WindowFrameWidth;
static INT g_WindowFrameHeight;
static INT g_FitDown;

/* How wide a string renders IN THE STATUS BAR'S OWN FONT. Asking the control for
 * its font matters: the strip is themed, so measuring with the stock system font
 * would size the part for text of a different width than the one drawn in it.
 */
static INT StatusTextWidth(PCSTR text)
{
    HDC deviceContext;
    HFONT font;
    HFONT old = NULL;
    SIZE extent;
    INT length = 0;
    INT width = 0;

    if (!g_Status || !text)
        return 0;
    while (text[length])
        ++length;
    deviceContext = GetDC(g_Status);
    if (!deviceContext)
        return 0;
    font = (HFONT)SendMessageA(g_Status, WM_GETFONT, 0, 0);
    if (font)
        old = (HFONT)SelectObject(deviceContext, font);
    if (GetTextExtentPoint32A(deviceContext, text, length, &extent))
        width = extent.cx;
    if (old)
        SelectObject(deviceContext, old);
    ReleaseDC(g_Status, deviceContext);
    return width;
}

enum
{
    STATUS_PART_PROGRAM = 0, STATUS_PART_MODE = 1, STATUS_PART_SPEED = 2, STATUS_PART_CAPTURE = 3, STATUS_PARTS = 4, STATUS_TEXT_INSET = 18, STATUS_PROGRAM_WIDTH_MAX = 260, STATUS_PART_WIDTH_MIN = 40, STATUS_PART_TO_EDGE = -1
};   /* the status strip, left to right */
/* EACH PART HUGS ITS TEXT (s84: extended from the name to every part) (Importance = 1):
 * Measure each string in the bar's own font, add the control's inset, and lay the
 * parts out left to right; the LAST part runs to the edge (-1) so the size grip has
 * somewhere to sit. Re-cut whenever any text changes: re-partitioning blanks every
 * part, so StatusUpdate re-pushes all of them after it.
 *
 * [CAUTION]: CLAMPED: a 63-character program name must not push everything else off the strip.
 */
static VOID StatusSetParts(PCSTR const *text, int count) /* stays int: INT here moves the compiled code */
{
    INT parts[STATUS_PARTS];
    INT index;
    INT right = 0;

    if (!g_Status || count < 1 || count > STATUS_PARTS)
        return;
    for (index = 0; index < count; ++index)
    {
        INT width = StatusTextWidth(text[index]) + STATUS_TEXT_INSET;       /* the control's own left inset */
        if (index == STATUS_PART_PROGRAM && width > STATUS_PROGRAM_WIDTH_MAX)
            width = STATUS_PROGRAM_WIDTH_MAX;
        if (width < STATUS_PART_WIDTH_MIN)
            width = STATUS_PART_WIDTH_MIN;
        right += width;
        parts[index] = (index == count - 1) ? STATUS_PART_TO_EDGE : right;
    }
    SendMessageA(g_Status, SB_SETPARTS, (WPARAM)count, (LPARAM)parts);
}

/* The CPU speed as the strip shows it: the clock only, no CPU name (user, s84). */
static VOID StatusSpeedText(PSTR out)
{
    UINT mhz = ((UINT)g_CpuSpeedIndex < CPUSPEED_COUNT) ? g_CpuSpeedMhz[g_CpuSpeedIndex] : 0u;
    PSTR cursor = out;

    if (!mhz)
    {
        LogPut(out, HOST_SPEED_UNLIMITED);
        return;
    }
    if (mhz >= MEGAHERTZ_PER_GIGAHERTZ_U && mhz % MEGAHERTZ_PER_GIGAHERTZ_U == 0u)
    {
        cursor = LogDecimal(cursor, mhz / MEGAHERTZ_PER_GIGAHERTZ_U);
        LogPut(cursor, " GHz");
    }
    else
    {
        cursor = LogDecimal(cursor, mhz);
        LogPut(cursor, " MHz");
    }
}

/* Compose the parts, and push them only when something CHANGES.
 *
 * [CAUTION]: This is POLLED from the UI tick rather than driven by a dirty flag, deliberately.
 * The mode facts -- g_DpmiPm and g_DpmiIsClient32 -- are set on the V86 thread deep
 * inside the DPMI mode switch, and a flag there would be one more thing every future
 * site that changes mode has to remember. Comparing short strings once per frame
 * costs nothing and cannot be forgotten.
 */
static VOID StatusUpdate(VOID)
{
    PCSTR text[STATUS_PARTS];
    CHAR speed[32];
    INT parts;

    if (!g_Status)
        return;
    text[STATUS_PART_PROGRAM] = g_ProgramName;
    /* One field, because they are one fact: 32-bit only ever means a DPMI client in
     * protected mode. Title case (user, s84).
     */
    {   /* #325: the picture's own resolution and how it is shown -- "at 2x" when the
             drawn rectangle is an exact whole multiple, "scaled" otherwise -- read
             from what PresentGdi actually drew, so it is true in a window, maximised
             and fullscreen alike. */
        static CHAR modeText[96];
        PCSTR mode = (g_DpmiPm && g_DpmiIsClient32) ? STATUS_TEXT_PM32
                      : g_DpmiPm                      ? STATUS_TEXT_PM16
                                                       : STATUS_TEXT_REAL_MODE;
        PSTR cursor = LogPut(modeText, mode);
        if (g_PresentDdraw.IsSnapshotValid && g_PresentDdraw.SnapshotWidth > 0 && g_PresentDdraw.SnapshotHeight > 0 && g_PresentDdraw.LastDestinationWidth > 0)
        {
            INT snapshotWidth = g_PresentDdraw.SnapshotWidth;
            INT snapshotHeight = g_PresentDdraw.SnapshotHeight;
            INT destinationWidth = g_PresentDdraw.LastDestinationWidth;
            INT destinationHeight = g_PresentDdraw.LastDestinationHeight;
            cursor = LogPut(cursor, ", ");  cursor = LogDecimal(cursor, (UINT)snapshotWidth); cursor = LogPut(cursor, "x"); cursor = LogDecimal(cursor, (UINT)snapshotHeight);
            if (destinationWidth % snapshotWidth == 0 && destinationHeight % snapshotHeight == 0 && destinationWidth / snapshotWidth == destinationHeight / snapshotHeight)
            {
                cursor = LogPut(cursor, " at "); cursor = LogDecimal(cursor, (UINT)(destinationWidth / snapshotWidth)); cursor = LogPut(cursor, "x");
            }
            else
                cursor = LogPut(cursor, g_FitDown ? " (scaled to fit)" : " (scaled)");
        }
        text[STATUS_PART_MODE] = modeText;
    }
    StatusSpeedText(speed);
    text[STATUS_PART_SPEED] = speed;
    /* Only a program that asked for the mouse has anything to say about capture
     * (rule 1); exactly the user's wording.
     */
    text[STATUS_PART_CAPTURE] = g_Captured        ? STATUS_TEXT_RELEASE_MOUSE
           : CaptureAllowed() ? STATUS_TEXT_CAPTURE_MOUSE
                               : STATUS_TEXT_NONE;
    parts = text[STATUS_PART_CAPTURE][0] ? STATUS_PARTS : STATUS_PARTS - 1;
    if (StringsEqual(text[STATUS_PART_PROGRAM], g_StatusLeft) && StringsEqual(text[STATUS_PART_MODE], g_StatusMode) &&
        StringsEqual(text[STATUS_PART_SPEED], g_StatusSpeed) && StringsEqual(text[STATUS_PART_CAPTURE], g_StatusRight))
        return;
    StatusSetParts(text, parts);
    LogPut(g_StatusLeft, text[STATUS_PART_PROGRAM]); LogPut(g_StatusMode, text[STATUS_PART_MODE]);
    LogPut(g_StatusSpeed, text[STATUS_PART_SPEED]); LogPut(g_StatusRight, text[STATUS_PART_CAPTURE]);
    SendMessageA(g_Status, SB_SETTEXTA, STATUS_PART_PROGRAM, (LPARAM)text[STATUS_PART_PROGRAM]);
    SendMessageA(g_Status, SB_SETTEXTA, STATUS_PART_MODE, (LPARAM)text[STATUS_PART_MODE]);
    SendMessageA(g_Status, SB_SETTEXTA, STATUS_PART_SPEED, (LPARAM)text[STATUS_PART_SPEED]);
    if (parts == STATUS_PARTS)
        SendMessageA(g_Status, SB_SETTEXTA, STATUS_PART_CAPTURE, (LPARAM)text[STATUS_PART_CAPTURE]);
}

/* Create the native status bar child; record its height so the video blit reserves
 * that strip.
 */
static VOID MakeStatus(HWND parent, HINSTANCE instance)
{
    RECT statusRect;

    g_Status = CreateWindowExA(0, STATUSCLASSNAME, NULL,
                               WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                               0, 0, 0, 0, parent, NULL, instance, NULL);
    if (!g_Status)
        return;
    /* Dock it to the parent's width BEFORE cutting the parts: a status bar created
     * at 0x0 has a zero client rect, so the split would be computed against nothing
     * and would stay wrong until the first user resize.
     */
    SendMessageA(g_Status, WM_SIZE, 0, 0);
    g_StatusLeft[0] = 0;                       /* force the first cut + push */
    StatusUpdate();
    GetWindowRect(g_Status, &statusRect);
    if (statusRect.bottom > statusRect.top)
        g_PresentDdraw.StatusHeight = statusRect.bottom - statusRect.top;
}

/* Tick/untick a menu item BY COMMAND, through whichever menu is live: FULLSCREEN
 * detaches the bar into g_FsMenu, and a toggle pressed by hotkey while it is
 * detached must still be recorded there or the tick is stale when it returns.
 */
static VOID MenuCheck(HWND window, UINT commandId, INT isOn)
{
    HMENU menu = GetMenu(window);

    if (!menu)
        menu = g_FsMenu;
    if (menu)
        CheckMenuItem(menu, commandId, MF_BYCOMMAND | (UINT)(isOn ? MF_CHECKED : MF_UNCHECKED));
}

/* ITEMS THAT ONLY MEAN SOMETHING IN A TEXT MODE (Importance = 1):
 * Mark, Copy, Copy Whole Screen, Paste and Select All all operate on the CHARACTER
 * GRID. In mode 13h or a planar mode there is no grid -- there are pixels -- so
 * there is nothing to mark and nothing to paste into, and an enabled item that
 * cannot do its job is the "runs but lies" shape this project treats as the most
 * expensive kind of defect.
 * - Driven from WM_INITMENUPOPUP rather than from the mode set, because the mode is
 *   the GUEST's to change and it can change at any instant: syncing when the menu is
 *   about to be drawn is the only moment the answer is guaranteed current, and it
 *   costs five EnableMenuItem calls on a user action.
 *
 * [CAUTION]: NOT the scaffold-stub rule (unimplemented items stay enabled) -- see the note by
 * IDM_EDIT_MARK. In text mode these are enabled exactly as before.
 */
/* #152: is there a program to close? Greyed at the shell's own prompt and once the
 * run is over. In a Win16 VDM it is always there -- the user's rule (s81): Close
 * Program on a Win16 program ends it AND NTVDMEX, since there is no shell under it.
 */
static INT CloseProgramAvailable(VOID)
{
    if (!g_Running || g_WoundDown)
        return 0;
    if (g_WowLaunch)
        return 1;
    if (g_DpmiDone)
        return 0;
    return g_ExecDepth > 0 || !g_TopIsShell;
}

/* #154: THE EDIT MENU, ON THE CHARACTER GRID:
 * The five items were greyed outside text mode and did nothing inside it. The grid
 * is page 0 at VIDEO_TEXT_OFFSET, 8x16 cells -- exactly what the text renderer draws, so
 * what is copied is what is seen.
 *   Mark        the next left drag selects a rectangle of cells (Esc cancels)
 *   Select All  the whole grid
 *   Copy        the selection, as CF_OEMTEXT (the grid is code page 437; Windows
 *               converts), lines right-trimmed, CRLF between them
 *   Copy Whole Screen   the same, for every cell
 *   Paste       the clipboard, TYPED: real scancodes through the same path as a
 *               keypress, so a prompt, EDIT and a program reading port 60h all get it
 * The selection is shown by the presenter inverting it after each frame.
 */
static INT g_MarkMode;
static INT g_MarkDrag;
static INT g_SelectionOn;
static INT g_SelectionColumn0;
static INT g_SelectionRow0;
static INT g_SelectionColumn1;
static INT g_SelectionRow1;
static volatile LONG g_PasteBusy;

static VOID SelectionPublish(VOID)
{
    INT cols = g_Video.Columns;
    INT rows = g_Video.Rows;
    INT column0 = g_SelectionColumn0 < g_SelectionColumn1 ? g_SelectionColumn0 : g_SelectionColumn1;
    INT column1 = g_SelectionColumn0 < g_SelectionColumn1 ? g_SelectionColumn1 : g_SelectionColumn0;
    INT row0 = g_SelectionRow0 < g_SelectionRow1 ? g_SelectionRow0 : g_SelectionRow1;
    INT row1 = g_SelectionRow0 < g_SelectionRow1 ? g_SelectionRow1 : g_SelectionRow0;

    if (cols < 1)
        cols = 1;
    if (rows < 1)
        rows = 1;
    g_PresentDdraw.IsSelection = g_SelectionOn;
    {   /* in FRAME pixels: the live cell -- 9 dots wide (#324), and cell_h tall, which
           is 8 in a 50-line screen (this used VIDEO_CELL_HEIGHT, so a 50-line selection was
           drawn at twice its height). */
        INT cellWidth = VddVideoTextCellWidth(&g_Video);
        INT cellHeight = g_Video.CellHeight ? g_Video.CellHeight : VIDEO_CELL_HEIGHT;
        g_PresentDdraw.SelectionX0 = column0 * cellWidth;
        g_PresentDdraw.SelectionX1 = (column1 + 1) * cellWidth;
        g_PresentDdraw.SelectionY0 = row0 * cellHeight;
        g_PresentDdraw.SelectionY1 = (row1 + 1) * cellHeight;
    }
    HOST_LOCK();
    g_Video.IsDirty = 1;
    HOST_UNLOCK();
    if (g_PresentDdraw.Window)
        InvalidateRect(g_PresentDdraw.Window, NULL, FALSE);
}

static VOID SelectionClear(VOID)
{
    g_MarkMode = g_MarkDrag = 0;
    g_SelectionOn = 0;
    SelectionPublish();
}

/* Client pixel -> cell, through the rectangle the last frame was drawn into. */
static INT ClientToCell(INT clientX, INT clientY, INT *column, INT *row)
{
    INT sourceX;
    INT sourceY;

    if (g_PresentDdraw.LastDestinationWidth <= 0 || g_PresentDdraw.LastDestinationHeight <= 0 || g_Video.Columns < 1 || g_Video.Rows < 1)
        return 0;
    sourceX = (clientX - g_PresentDdraw.LastDestinationX) * g_PresentDdraw.LastSourceWidth / g_PresentDdraw.LastDestinationWidth;
    sourceY = (clientY - g_PresentDdraw.LastDestinationY) * g_PresentDdraw.LastSourceHeight / g_PresentDdraw.LastDestinationHeight;
    *column = sourceX / VddVideoTextCellWidth(&g_Video);
    *row = sourceY / (g_Video.CellHeight ? g_Video.CellHeight : VIDEO_CELL_HEIGHT);
    if (*column < 0)
        *column = 0;
    if (*row < 0)
        *row = 0;
    if (*column >= g_Video.Columns)
        *column = g_Video.Columns - 1;
    if (*row >= g_Video.Rows)
        *row = g_Video.Rows - 1;
    return 1;
}

enum
{
    TEXT_COPY_SELECTION = 0, TEXT_COPY_SCREEN = 1
};   /* TextCopy: the marked region, or the whole screen */
static VOID TextCopy(HWND window, INT all)
{
    INT column0;
    INT column1;
    INT row0;
    INT row1;
    INT row;
    INT column;
    INT length = 0;
    static CHAR text[132 * 60 * 2 + 256];
    HGLOBAL memory;
    PSTR destination;

    if (g_Video.ModeKind != VIDEO_KIND_TEXT || !g_Video.VideoMemory)
        return;
    if (all || !g_SelectionOn)
    {
        column0 = 0;
        row0 = 0;
        column1 = g_Video.Columns - 1;
        row1 = g_Video.Rows - 1;
    }
    else
    {
        column0 = g_SelectionColumn0 < g_SelectionColumn1 ? g_SelectionColumn0 : g_SelectionColumn1;
        column1 = g_SelectionColumn0 < g_SelectionColumn1 ? g_SelectionColumn1 : g_SelectionColumn0;
        row0 = g_SelectionRow0 < g_SelectionRow1 ? g_SelectionRow0 : g_SelectionRow1;
        row1 = g_SelectionRow0 < g_SelectionRow1 ? g_SelectionRow1 : g_SelectionRow0;
    }
    HOST_LOCK();
    for (row = row0; row <= row1 && length < (INT)sizeof text - 140; ++row)
    {
        INT start = length;
        for (column = column0; column <= column1; ++column)
        {
            BYTE ch = g_Video.VideoMemory[VIDEO_TEXT_OFFSET + ((g_Video.CrtcStartLive * VIDEO_TEXT_CELL_BYTES_U + (UINT)(row * g_Video.Columns + column) * VIDEO_TEXT_CELL_BYTES_U) & VIDEO_TEXT_WINDOW_MASK_U)];   /* the DISPLAYED page (#252) */
            text[length++] = (CHAR)(ch ? ch : ' ');
        }
        while (length > start && text[length - 1] == ' ') --length;          /* right-trim the line */
        if (row < row1)
        {
            text[length++] = '\r';
            text[length++] = '\n';
        }
    }
    HOST_UNLOCK();
    while (length >= 2 && text[length - 2] == '\r' && text[length - 1] == '\n')
        length -= 2;                                                                           /* the empty rows below */
    text[length] = 0;
    memory = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)length + 1);
    destination = memory ? (PSTR)GlobalLock(memory) : NULL;
    if (!destination)
    {
        if (memory)
            GlobalFree(memory);
        return;
    }
    for (column = 0; column <= length; ++column)
        destination[column] = text[column];
    GlobalUnlock(memory);
    if (OpenClipboard(window))
    {
        EmptyClipboard();
        if (!SetClipboardData(CF_OEMTEXT, memory))
            GlobalFree(memory);
        CloseClipboard();
    }
    else
        GlobalFree(memory);
}

enum
{
    PASTE_KEY_HOLD_MS = 12, PASTE_KEY_GAP_MS = 20
};   /* PasteThread: per typed key */
/* Typed, not injected: ~30 characters a second, so a guest that reads slowly (or a
 * BIOS ring of 15 keys) is not overrun. One paste at a time; a new one while typing
 * is refused rather than interleaved.
 */
static DWORD WINAPI PasteThread(LPVOID parameter)
{
    PSTR text = (PSTR)parameter;
    INT index;

    for (index = 0; text[index] && g_Running; ++index)
    {
        BYTE ch = (BYTE)text[index];
        BYTE scan = 0;
        BYTE isShifted = 0;
        if (ch == '\r')
        {
            scan = INPUT_SCAN_ENTER;
            if (text[index + 1] == '\n')
                ++index;
        }
        else if (ch == '\n')
            scan = INPUT_SCAN_ENTER;
        else if (ch == '\t')
            scan = INPUT_SCAN_TAB;
        else { BYTE scanCode;
        INT shift;                           /* #136: on the active layout */
               if (VddInputCharToKey(&g_Input, ch, &scanCode, &shift))
               {
                   scan = scanCode;
                   isShifted = (BYTE)shift;
               }
               }
        if (!scan)
            continue;                                         /* not typeable: skipped */
        if (isShifted)
            HostKeyScancode(INPUT_SCAN_LEFT_SHIFT, INPUT_KEY_NORMAL, INPUT_KEY_MAKE);
        HostKeyScancode(scan, INPUT_KEY_NORMAL, INPUT_KEY_MAKE);
        Sleep(PASTE_KEY_HOLD_MS);
        HostKeyScancode(scan, INPUT_KEY_NORMAL, INPUT_KEY_BREAK);
        if (isShifted)
            HostKeyScancode(INPUT_SCAN_LEFT_SHIFT, INPUT_KEY_NORMAL, INPUT_KEY_BREAK);
        Sleep(PASTE_KEY_GAP_MS);
    }
    HeapFree(GetProcessHeap(), 0, text);
    InterlockedExchange(&g_PasteBusy, 0);
    return 0;
}

enum
{
    PASTE_TEXT_MAX = 4096
};   /* TextPaste: the most it types in one go */
static VOID TextPaste(HWND window)
{
    HANDLE clipboard;
    PCSTR source;
    PSTR copy;
    INT length = 0;

    if (InterlockedExchange(&g_PasteBusy, 1))
        return;
    if (!OpenClipboard(window))
    {
        InterlockedExchange(&g_PasteBusy, 0);
        return;
    }
    clipboard = GetClipboardData(CF_TEXT);
    source = clipboard ? (PCSTR)GlobalLock(clipboard) : NULL;
    copy = source ? (PSTR)HeapAlloc(GetProcessHeap(), 0, PASTE_TEXT_MAX + 1) : NULL;
    if (copy)
    {
        while (length < PASTE_TEXT_MAX && source[length])
        {
            copy[length] = source[length];
            ++length;
        }
        copy[length] = 0;
    }
    if (source)
        GlobalUnlock(clipboard);
    CloseClipboard();
    if (!copy || !length)
    {
        if (copy)
            HeapFree(GetProcessHeap(), 0, copy);
        InterlockedExchange(&g_PasteBusy, 0);
        return;
    }
    { HANDLE thread = CreateThread(NULL, 0, PasteThread, copy, 0, NULL);
      if (thread)
          CloseHandle(thread);
      else
      {
          HeapFree(GetProcessHeap(), 0, copy);
          InterlockedExchange(&g_PasteBusy, 0);
      }
      }
}

static VOID MenuSyncModal(HWND window, HMENU popup)
{
    static const UINT textOnly[] = { IDM_EDIT_MARK, IDM_EDIT_COPY, IDM_EDIT_COPYSCREEN,
                                      IDM_EDIT_PASTE, IDM_EDIT_SELECTALL };
    HMENU menu = GetMenu(window);
    UINT  flag;
    UINT index;
    /* The tray menu is its own popup, not a child of the menu bar: grey it directly. */
    if (popup) EnableMenuItem(popup, IDM_FILE_CLOSEPROG, MF_BYCOMMAND
                              | (CloseProgramAvailable() ? MF_ENABLED : MF_GRAYED));
    if (!menu)
        menu = g_FsMenu;
    if (!menu)
        return;
    EnableMenuItem(menu, IDM_FILE_CLOSEPROG, MF_BYCOMMAND
                   | (CloseProgramAvailable() ? MF_ENABLED : MF_GRAYED));
    flag = (g_Video.ModeKind == VIDEO_KIND_TEXT) ? MF_ENABLED : (MF_GRAYED | MF_DISABLED);
    for (index = 0; index < sizeof textOnly / sizeof textOnly[0]; ++index)
        EnableMenuItem(menu, textOnly[index], MF_BYCOMMAND | flag);
    CheckMenuItem(menu, IDM_CAP_AUDIO, MF_BYCOMMAND | (AudioWaveIsRecording() ? MF_CHECKED : MF_UNCHECKED));
    /* #154: Copy needs a selection; Paste needs text, and not a paste already typing. */
    if (flag == MF_ENABLED)
    {
        if (!g_SelectionOn)
            EnableMenuItem(menu, IDM_EDIT_COPY, MF_BYCOMMAND | MF_GRAYED);
        if (g_PasteBusy || !IsClipboardFormatAvailable(CF_TEXT))
            EnableMenuItem(menu, IDM_EDIT_PASTE, MF_BYCOMMAND | MF_GRAYED);
    }
}

/* INPUT CAPTURE ("exclusivity"):
 * Two different things are stealing the guest's keys, and they need different fixes.
 * 1. WINDOWS' OWN MENU KEYS. F10 and Alt do not arrive as WM_KEYDOWN at all -- they are
 *  SYSTEM keys (WM_SYSKEYDOWN) and DefWindowProc turns them into menu activation. So a
 *  DOS program that wants F10 -- Doom's SETUP.EXE is the reported case -- never sees
 *  it, and the menu bar lights up instead. That is fixed unconditionally below: system
 *  keys are routed to the guest like any other, because in a DOS box they ARE the
 *  guest's. Alt+F4 is the deliberate exception while uncaptured, so the window can
 *  always be closed.
 * 2. THE SHELL'S CHORDS -- Alt+Tab, Ctrl+Esc, the Windows keys. Those never reach the
 *  window at all; only a low-level hook sees them, and only while we are foreground.
 *  Swallowing them is what "exclusive" means, and it is a MODE, not a default: a
 *  window that eats Alt+Tab whenever it has focus is hostile. WIN+F10 toggles it
 *  (Scroll Lock too, where the keyboard still has one),
 *  swallowed by us so the guest never sees it -- there is no chord a DOS guest cannot
 *  generate, so the release must be one WE reserve. It is deliberately NOT Ctrl+F10
 *  (DOSBox's binding): Doom fires with Ctrl and uses every F-key, so that chord fought
 *  the guest for keys it needs constantly -- and F10 is a SYSTEM key, so the check
 *  never even ran. See the WM_KEYDOWN handler.
 *
 * [CAUTION]: Ctrl+Alt+Del is the Secure Attention Sequence and CANNOT be hooked. That is by
 * design in Windows and not a defect here.
 *
 * [CAUTION]: Capture is dropped on WM_KILLFOCUS -- otherwise a clipped cursor and a swallowed
 * Alt+Tab would strand the user in a window they cannot leave.
 */
HHOOK         g_LowLevelKeyboard;
INT           g_LowLevelKeyboardOn = 0;     /* OFF by default; llkbd.txt = 1. See InputCaptureSet. */
static volatile LONG g_UiBeat;          /* ++ per WM_TIMER: the UI thread is pumping */
static DWORD         g_CaptureWatchdogReleased;   /* times the watchdog had to hand the box back */
/* [CAUTION]: host_key_held() WAS HERE AND IS GONE (s64). It answered "is a Windows key held", which
 * only ever mattered to the Win+F10 and Win+Click CHORDS. The Windows key is now a release key in
 * its own right (capture rule 4), so nothing needs to ask whether it is held alongside something
 * else. g_WindowsKeyDown stays: LowLevelKeyboardProcedure swallows the Win DOWN when the low-level
 * hook is enabled, and must still track it so it cannot leave the system believing the key is
 * stuck.
 */

/* THE CAPTURE RULES, WRITTEN DOWN ONCE. (s64, user spec) (Importance = 3):
 * Capture was a toggle with four ways to flip it (Win+F10, Scroll Lock, Win+Click, a
 * menu item), it applied to a machine that might have no interest in the mouse at
 * all, and it was reachable in states where it did nothing but strand the pointer.
 * The user's words: "capture is a bit buggy, so here's absolutely how I want it to
 * work". So it is now a POLICY, not a toggle, and this is the whole of it:
 *
 *   1. A GUEST THAT NEVER HOOKS THE MOUSE NEVER CAPTURES IT. No chord, no menu item,
 *      no click takes the pointer -- it belongs to the Windows desktop and stays
 *      there. Skyroads is this case, and there is nothing capture could give it.
 *   2. A GUEST THAT HOOKS THE MOUSE CAPTURES IMMEDIATELY, at the moment it hooks.
 *      That is g_MouseWantCapture, raised by MouseInt33 the first time the guest
 *      USES the driver, and performed on the UI tick.
 *   3. CAPTURED, THE POINTER DOES NOT EXIST. Not over the video, not over the status
 *      bar, not over the menu -- ClipCursor already confines it to our client, and
 *      nothing about a captured machine wants a desktop arrow drawn on top of it.
 *   4. THE WINDOWS KEY ALONE RELEASES. One key, no chord. It is the only key a DOS
 *      guest cannot generate, which is the entire requirement for a release key, and
 *      a chord is one thing too many to remember while a game has your mouse.
 *   5. A CLICK IN THE VIDEO AREA RE-CAPTURES -- and ONLY the video area. Not focus,
 *      not the title bar, not the menu, not the status strip. That is what keeps the
 *      window draggable and the menus reachable after a release: getting your mouse
 *      back is a deliberate click into the picture, and everything else on the window
 *      still belongs to Windows.
 *   6. RELEASED MEANS RELEASED (s70, user spec). A guest that uses the mouse and is
 *      NOT captured receives NOTHING from it: no raw deltas (0Bh), no position (03h),
 *      no buttons (05h/06h). Before this, raw input followed FOCUS not capture, so
 *      after the Windows key gave the pointer back, dragging across the desktop still
 *      mouse-looked in the game as long as our window was foreground, and a right
 *      click over the picture still reached it. Release also reports any held button
 *      as let go, so the guest is never left believing a button is down. A guest that
 *      never used the mouse is untouched -- it was never captured and the plain
 *      window behaviour is right for it. One decision point: MouseGoesToGuest().
 *
 * [CAUTION]: CaptureAllowed() gates rule 1 and it is deliberately the SAME latch the automatic
 * grab keys on. "Has this guest ever used the mouse" has one answer and one variable;
 * two would drift, and a UI that disagrees with itself about who owns the pointer is
 * the bug this replaces. It is defined up beside g_MouseAutoCaptureFired because the
 * status strip needs it and is built before this point.
 */

/* Is a point (client coords) over the VIDEO, as opposed to the status strip? The menu
 * bar and the caption are not in client space at all, so they cannot reach here.
 */
static INT IsPointOverVideo(HWND window, INT clientX, INT clientY)
{
    RECT clientRect;
    INT videoHeight;

    if (!GetClientRect(window, &clientRect))
        return 0;
    videoHeight = clientRect.bottom - (g_PresentDdraw.StatusHeight ? g_PresentDdraw.StatusHeight : PRESENT_STATUS_HEIGHT);
    return clientX >= 0 && clientX < clientRect.right && clientY >= 0 && clientY < videoHeight;
}

/* ONE DECISION, ONE PLACE: IS THE DESKTOP ARROW VISIBLE RIGHT NOW?:
 * There were four copies of this expression -- in InputCaptureSet, in
 * host_cursor_set, in HostFullscreenToggle and in WM_SETCURSOR -- and they had
 * already drifted: three of them tested `g_cursor_show` without asking whether the
 * pointer was over the VIDEO or over the status bar, which is the only part of the
 * window the setting was ever about.
 * - THE ORDER MATTERS AND IT IS NOT SYMMETRIC:
 *   captured   -> hidden EVERYWHERE on the window (rule 3). The status strip and
 *                 the menu are ours too, and a captured machine has no pointer.
 *   fullscreen -> hidden everywhere. Exclusive mode has no chrome to point at.
 *   otherwise  -> the setting, and ONLY over the video. The status bar keeps its
 *                 arrow and its size grip whatever the checkbox says.
 */
static INT HostCursorVisibleAt(INT overVideo)
{
    if (g_Captured)
        return 0;
    if (!overVideo || g_HostCursorMode == HOSTCUR_ALWAYS)
        return 1;                                                     /* s84 */
    if (g_HostCursorMode == HOSTCUR_NEVER)
        return 0;
    /* #218: fullscreen no longer hides it outright -- a program that does not use the
     * mouse keeps a usable pointer there too, and the idle rule applies to both.
     */
    return !(g_CursorIdle && !CaptureAllowed());
}

/* Apply it NOW rather than waiting for WM_SETCURSOR, which only fires when the mouse
 * next MOVES or re-enters -- without this the state changes and the pointer does not
 * until you jiggle it. Guarded on the pointer actually being over us: SetCursor
 * changes the shape there and then, and we have no business touching it while it is
 * over someone else's window. The status bar is a CHILD, so accept it as ours.
 */
static VOID HostCursorRefresh(HWND window)
{
    POINT point;
    POINT client;
    HWND under;

    if (!window || !GetCursorPos(&point))
        return;
    under = WindowFromPoint(point);
    if (under != window && GetParent(under) != window)
        return;
    client = point;
    ScreenToClient(window, &client);
    SetCursor(HostCursorVisibleAt(IsPointOverVideo(window, client.x, client.y))
              ? LoadCursorA(NULL, IDC_ARROW) : NULL);
}

/* Confine the pointer to our client area. Split out of InputCaptureSet because the
 * rect is only true for the geometry it was computed from: go fullscreen (or resize)
 * while captured and the clip is still the OLD window, so the mouse is fenced into a
 * corner of the screen the picture no longer occupies. Anything that changes the
 * window's shape must re-apply it.
 */
static VOID CaptureClipRect(HWND window, RECT *clip)
{
    POINT topLeft;

    GetClientRect(window, clip);
    topLeft.x = clip->left;
    topLeft.y = clip->top;
    ClientToScreen(window, &topLeft);
    clip->left = topLeft.x;
    clip->top = topLeft.y;
    clip->right += topLeft.x;
    clip->bottom += topLeft.y;
}

static VOID CaptureClipApply(HWND window)
{
    RECT clip;

    if (!window || !g_Captured)
        return;
    CaptureClipRect(window, &clip);
    ClipCursor(&clip);
}

/* CAPTURED MEANS THE POINTER NEVER REACHES THE DESKTOP. (user, s84) (Importance = 1):
 * Focusing the window from its TITLE BAR captured (WM_ACTIVATE, rule 5) -- and then
 * the press on the caption entered Windows' own move loop, which leaves the cursor
 * UNCLIPPED when it ends. So we believed we were captured, the pointer was hidden over
 * the video, and it walked straight out onto the desktop. ClipCursor is global state
 * anyone can change, so re-applying it at WM_EXITSIZEMOVE is not enough on its own:
 * from the UI tick, while captured and in front (and not mid-drag, where Windows owns
 * the clip), check the clip is still OURS and restore it if not. Counted.
 */
static INT   g_InSizeMove;
static DWORD g_ClipRepairs;
static VOID CaptureClipGuard(HWND window)
{
    RECT want;
    RECT current;

    if (!window || !g_Captured || g_InSizeMove || GetForegroundWindow() != window)
        return;
    CaptureClipRect(window, &want);
    if (!GetClipCursor(&current))
        return;
    if (current.left != want.left || current.top != want.top ||
        current.right != want.right || current.bottom != want.bottom)
    {
        ClipCursor(&want);
        ++g_ClipRepairs;
    }
}

enum
{
    FULLSCREEN_RELEASE_HINT_MS = 4000
};   /* FullscreenReleaseHint: how long its hint shows */
/* #138: in fullscreen there is no status strip, so say how to get the mouse back. */
static VOID FullscreenReleaseHint(VOID)
{
    g_PresentDdraw.HintText  = HOST_CAPTURE_HINT_TEXT;
    g_PresentDdraw.HintUntil = GetTickCount() + FULLSCREEN_RELEASE_HINT_MS;
}

VOID InputCaptureSet(HWND window, INT isOn)
{
    /* RULE 1. Refuse rather than assert: this is reached from the menu, the click
     * path and the UI tick, and "the guest never asked for the mouse" is a normal
     * state, not an error.
     */
    if (isOn && !CaptureAllowed())
        return;
    if (isOn == g_Captured)
        return;
    InterlockedExchange(&g_Captured, isOn ? 1 : 0);
    if (isOn && g_PresentDdraw.IsFullscreen)
        FullscreenReleaseHint();
    if (isOn)
    {
        /* [WARNING]: THIS HOOK CAN JAM THE WHOLE MACHINE, SO IT IS OFF BY DEFAULT (Importance = 2):
         * WH_KEYBOARD_LL is SYSTEM-WIDE: every keystroke on the box is routed through
         * THIS process's UI thread. If that thread stalls -- and a VDM host has many
         * ways to stall, including being blocked behind the guest -- then the entire
         * machine's keyboard stalls with it. Add ClipCursor below, which confines the
         * mouse system-wide, and a host that wedges while captured leaves a computer
         * that is running, pingable, and completely unusable.
         * Reported by the user, 2026-09-09, twice in one session: "that basically
         * crashed Windows, and I had to restart the rig", then "NTVDMEX jams the rig".
         * - AND WHAT IT BUYS IS SMALL: swallowing Win, Alt+Tab and Ctrl/Alt+Esc so the
         *   guest keeps focus. Losing that means Alt+Tab works again -- which is an
         *   ESCAPE ROUTE from a misbehaving guest, not a regression. The trade is not
         *   close: a stuck Alt+Tab costs a keystroke, a stuck hook costs the session.
         *
         * [CAUTION]: ClipCursor stays, because it is released on capture exit, on WM_KILLFOCUS,
         * on WM_DESTROY (see HostPanicRelease) and by Windows on process death. The
         * hook is the one that outlives a wedge.
         */
        if (!g_LowLevelKeyboard && g_LowLevelKeyboardOn)
            g_LowLevelKeyboard = SetWindowsHookExA(WH_KEYBOARD_LL, LowLevelKeyboardProcedure,
                                        GetModuleHandleA(NULL), 0);
        CaptureClipApply(window);
    }
    else
    {
        ClipCursor(NULL);
        if (g_LowLevelKeyboard)
        {
            UnhookWindowsHookEx(g_LowLevelKeyboard);
            g_LowLevelKeyboard = NULL;
        }
        /* RULE 6: the UP that follows will not reach the guest, so report any held
         * button as released NOW -- edges and all, so 06h sees it.
         */
        {   LONG prev = InterlockedExchange(&g_MouseButtons, 0);
            if (prev)
                MouseButtonEdges(prev, 0); }
    }
    MenuCheck(window, IDM_INPUT_CAPTURE, isOn);
    HostCursorRefresh(window);              /* apply the pointer change now, not on next move */
    /* The status strip says how to get back out. It is repainted from the UI tick,
     * which is where SendMessage to the control is safe -- InputCaptureSet is also
     * reached from the WM_KEYDOWN path, but the tick is the single writer.
     */
}

INT OtherHostsRunning(VOID)
{
    INT instance;
    CHAR name[48];

    for (instance = 1; instance <= HOST_INSTANCES_MAX; ++instance)
    {
        HANDLE mutex;
        PSTR cursor = LogPut(name, HOST_INSTANCE_MUTEX);
        if (instance == g_Instance)
            continue;
        if (instance > 1)
        {
            *cursor++ = '_';
            cursor = LogDecimal(cursor, (UINT)instance);
        }
        mutex = OpenMutexA(SYNCHRONIZE, FALSE, name);
        if (mutex)
        {
            CloseHandle(mutex);
            return 1;
        }
    }
    return 0;
}

static INT OpenAtPrompt(VOID)
{
    return g_Running && !g_WoundDown && !g_WowLaunch && !g_DpmiDone
        && g_ExecDepth == 0 && g_TopIsShell && g_Machine && g_Machine->IsLineActive;
}

/* A DOS image: .COM or .BAT by name, or an MZ .EXE without an NE/PE header. */
static INT OpenIsDosImage(PCSTR path)
{
    INT length = lstrlenA(path);
    BYTE header[DOS_MZ_NEW_HEADER_MIN];
    DWORD got = 0;
    DWORD newHeaderOffset;
    DWORD signatureBytesRead = 0;
    BYTE signature[DOS_EXE_SIGNATURE_SIZE];
    HANDLE file;

    if (length >= DOS_DOT_EXTENSION_LENGTH && (!lstrcmpiA(path + length - DOS_DOT_EXTENSION_LENGTH, HOST_EXTENSION_COM) || !lstrcmpiA(path + length - DOS_DOT_EXTENSION_LENGTH, HOST_EXTENSION_BAT)))
        return 1;
    file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return 0;
    if (!ReadFile(file, header, sizeof header, &got, NULL) || got < sizeof header || header[0] != 'M' || header[1] != 'Z')
    {
        CloseHandle(file);
        return 0; }
    newHeaderOffset = *(const DWORD *)(header + DOS_MZ_NEW_HEADER);
    if (newHeaderOffset >= DOS_MZ_NEW_HEADER_MIN && SetFilePointer(file, (LONG)newHeaderOffset, NULL, FILE_BEGIN) == newHeaderOffset
        && ReadFile(file, signature, DOS_EXE_SIGNATURE_SIZE, &signatureBytesRead, NULL) && signatureBytesRead == DOS_EXE_SIGNATURE_SIZE
        && ((signature[0] == 'N' && signature[1] == 'E') || (signature[0] == 'P' && signature[1] == 'E')))
    {
        CloseHandle(file);
        return 0; }
    CloseHandle(file);
    return 1;
}

/* Build "<backspaces>X:\r CD \dir\r NAME.EXT\r" from an 8.3 path; 0 if it cannot. */
static INT OpenPromptLine(PCSTR shortPath, INT backspaceCount, PSTR out, INT cap)
{
    INT length = lstrlenA(shortPath);
    INT slash = -1;
    INT index;
    INT directoryLength;
    PSTR cursor = out;
    PSTR end = out + cap - 1;

    for (index = 0; index < length; ++index)
        if (shortPath[index] == '\\')
            slash = index;
    if (length < 4 || shortPath[1] != ':' || shortPath[2] != '\\' || slash < 2)
        return 0;
    directoryLength = (slash == 2) ? 1 : slash - 2;                     /* "\" or "\DIR\SUB" */
    if (directoryLength > DOS_DIRECTORY_MAX || length - slash - 1 > DOS_SHORT_NAME_SIZE - 1 || backspaceCount + length + 16 > cap)
        return 0;
    for (index = 0; index < backspaceCount; ++index)
        *cursor++ = ASCII_BACKSPACE;
    *cursor++ = shortPath[0];
    *cursor++ = ':';
    *cursor++ = '\r';
    cursor = LogPut(cursor, "CD ");
    for (index = 2; index < (slash == 2 ? 3 : slash); ++index)
        *cursor++ = shortPath[index];
    *cursor++ = '\r';
    for (index = slash + 1; index < length; ++index)
        *cursor++ = shortPath[index];
    *cursor++ = '\r';
    if (cursor > end)
        return 0;
    *cursor = 0;
    return 1;
}

static VOID OpenProgram(HWND window, PCSTR path)
{
    CHAR shortPath[MAX_PATH];
    CHAR directory[MAX_PATH];
    CHAR command[MAX_PATH + 4];
    CHAR line[512];
    DWORD attributes = GetFileAttributesA(path);
    DWORD shortLength;
    INT index;
    INT cut = -1;
    STARTUPINFOA startupInfo;
    PROCESS_INFORMATION processInfo;

    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY))
    {
        CHAR message[MAX_PATH + 64];
        CHAR *cursor = LogPut(message, HOST_PROGRAM_NOT_FOUND_TEXT);
        LogPut(cursor, path);
        MessageBoxA(window, message, HOST_PRODUCT_NAME, MB_OK | MB_ICONEXCLAMATION);
        return;
    }
    MruAdd(path);
    if (g_Captured)
        InputCaptureSet(window, FALSE);
    shortLength = GetShortPathNameA(path, shortPath, sizeof shortPath);
    if (OpenAtPrompt() && shortLength && shortLength < sizeof shortPath && OpenIsDosImage(path)
        && OpenPromptLine(shortPath, g_Machine->LineLength, line, (INT)sizeof line)
        && TypeInPush(line))
    {
        CHAR lineBuffer[MAX_PATH + 64];
        CHAR *lineCursor = LogPut(lineBuffer, "OPEN: typed at the prompt [");
        lineCursor = LogPut(lineCursor, shortPath); lineCursor = LogPut(lineCursor, "]\r\n");
        LogAppend(LOG_PATH, lineBuffer, lineCursor);
        return;
    }
    lstrcpynA(directory, path, sizeof directory);
    for (index = 0; directory[index]; ++index)
        if (directory[index] == '\\')
            cut = index;
    if (cut >= 0)
        directory[cut == 2 ? 3 : cut] = 0;
    command[0] = '"';
    lstrcpynA(command + 1, path, MAX_PATH);
    LogPut(command + lstrlenA(command), "\"");
    for (index = 0; index < (INT)sizeof startupInfo; ++index)
        ((PSTR)&startupInfo)[index] = 0;
    startupInfo.cb = sizeof startupInfo;
    if (CreateProcessA(NULL, command, NULL, NULL, FALSE, CREATE_NEW_CONSOLE, NULL,
                       cut >= 0 ? directory : NULL, &startupInfo, &processInfo))
    {
        CHAR lineBuffer[MAX_PATH + 64];
        CHAR *lineCursor = LogPut(lineBuffer, "OPEN: started in a new window [");
        lineCursor = LogPut(lineCursor, path); lineCursor = LogPut(lineCursor, "]\r\n");
        LogAppend(LOG_PATH, lineBuffer, lineCursor);
        CloseHandle(processInfo.hThread);
        CloseHandle(processInfo.hProcess);
    }
    else
    {
        CHAR message[MAX_PATH + 96];
        CHAR *cursor = LogPut(message, HOST_PROGRAM_START_FAILED_TEXT);
        cursor = LogPut(cursor, path); cursor = LogPut(cursor, HOST_WINDOWS_ERROR_TEXT); cursor = LogHex(cursor, GetLastError()); *cursor = 0;
        MessageBoxA(window, message, HOST_PRODUCT_NAME, MB_OK | MB_ICONERROR);
    }
}

static VOID OpenProgramDialog(HWND window)
{
    CHAR file[MAX_PATH];
    OPENFILENAMEA openFile;
    INT index;

    file[0] = 0;
    for (index = 0; index < (INT)sizeof openFile; ++index)
        ((PSTR)&openFile)[index] = 0;
    openFile.lStructSize = sizeof openFile;
    openFile.hwndOwner   = window;
    openFile.lpstrFilter = HOST_OPEN_FILTER;
    openFile.lpstrFile   = file;
    openFile.nMaxFile    = sizeof file;
    openFile.lpstrTitle  = HOST_OPEN_TITLE;
    openFile.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (g_Captured)
        InputCaptureSet(window, FALSE);               /* the dialog needs the pointer */
    if (GetOpenFileNameA(&openFile))
        OpenProgram(window, file);
}

/* Rebuilt every time the submenu opens, so it always shows the registry's list --
 * including programs other NTVDMEX windows have run since this one started.
 */
static VOID MenuRecentFill(VOID)
{
    CHAR list[MRU_MAX][MAX_PATH];
    INT count;
    INT index;

    if (!g_RecentMenu)
        return;
    while (GetMenuItemCount(g_RecentMenu) > 0)
        DeleteMenu(g_RecentMenu, 0, MF_BYPOSITION);
    count = MruLoad(list);
    if (!count)
    {
        AppendMenuA(g_RecentMenu, MF_STRING | MF_GRAYED, IDM_RECENT_0, MENU_TEXT_RECENT_EMPTY);
        return;
    }
    for (index = 0; index < count; ++index)
    {
        CHAR text[2 * MAX_PATH + 8];
        CHAR *cursor = text;
        PCSTR source;
        *cursor++ = '&';
        *cursor++ = (CHAR)('1' + index);
        *cursor++ = ' ';
        for (source = list[index]; *source; ++source) /* a literal & */
        {
            if (*source == '&')
                *cursor++ = '&';
            *cursor++ = *source;
        }
        *cursor = 0;
        AppendMenuA(g_RecentMenu, MF_STRING, IDM_RECENT_0 + (UINT)index, text);
    }
}

enum
{
    HOST_PANIC_RESUME_MAX = 64
};   /* HostPanicRelease: undo at most this many suspends */
/* [WARNING]: GIVE THE MACHINE BACK. CALL THIS BEFORE ANY TEARDOWN PATH (Importance = 3):
 * Two things this host does are SYSTEM-WIDE and outlive our window, and a third
 * can stop the process dying at all:
 *   1. WH_KEYBOARD_LL routes every keystroke on the box through our UI thread.
 *   2. ClipCursor confines the mouse system-wide to our client rect.
 *   3. WE SUSPEND OUR OWN GUEST THREAD -- AsyncInjectIrq and the CPU throttle
 *      both do, dozens of times a second. A process cannot finish exiting while
 *      one of its threads is suspended, so a teardown that races a suspend leaves
 *      a ZOMBIE holding (1) and (2). The machine then runs, pings, and cannot be
 *      typed at: exactly the "whole machine jammed" the user hit, and exactly why
 *      "I closed it and reran it" made a second instance on top of a live hook.
 * WM_DESTROY used to stop the audio and clear g_Running and do none of this.
 *
 * [CAUTION]: ORDER: release the SYSTEM-WIDE things first, because they are what strands a
 * human. The thread resume is a loop -- suspend counts NEST, and the throttle and
 * the injector can each hold one.
 */
VOID HostPanicRelease(VOID)
{
    ClipCursor(NULL);
    if (g_LowLevelKeyboard)
    {
        UnhookWindowsHookEx(g_LowLevelKeyboard);
        g_LowLevelKeyboard = NULL;
    }
    InterlockedExchange(&g_Captured, 0);
    if (g_HostCpu)
    {
        INT guard = 0;
        /* ResumeThread returns the PREVIOUS count; >1 means it is still suspended.
         * Bounded so a bad handle cannot spin here forever.
         */
        while (guard++ < HOST_PANIC_RESUME_MAX) { DWORD prev = ResumeThread(g_HostCpu);
                               if (prev == (DWORD)-1 || prev <= 1)
                                   break; }
    }
}

enum
{
    CAPTURE_WATCH_MS = 250
};   /* CaptureWatchdogThread */
/* [WARNING]: THE LAST LINE OF DEFENCE: HAND THE MACHINE BACK WITHOUT BEING ASKED (Importance = 2):
 * HostPanicRelease covers the paths where we KNOW we are going away. This covers
 * the one where we do not: the UI thread stops pumping while capture is held. That
 * is the state that cost the user two hard resets -- the box runs, answers ping, and
 * cannot be typed at or clicked out of, because the things capture holds are
 * system-wide and only that same stalled thread ever releases them.
 * A separate thread owes the user nothing but this: if capture is held and the UI
 * thread has not reached its timer for CAPWD_STALL_MS, take the machine back. It
 * does NOT try to diagnose, kill or recover the guest -- a wedged emulator is a bug
 * report, a wedged computer is a lost afternoon, and only the second is urgent.
 *
 * [CAUTION]: ClipCursor and UnhookWindowsHookEx are safe from another thread; SetCursor and
 * the menu check are not, so they are deliberately not touched here.
 *
 * [CAUTION]: It must not fire while merely SLOW. 3 s is far longer than any frame this host
 * has ever taken (worst measured UI gap: 35 ms) and far shorter than a human's
 * patience with a dead keyboard.
 */
#define CAPWD_STALL_MS  3000u
DWORD WINAPI CaptureWatchdogThread(LPVOID parameter)
{
    LONG  last = -1;
    DWORD lastMs = GetTickCount();

    (VOID)parameter;
    while (g_Running)
    {
        Sleep(CAPTURE_WATCH_MS);
        if (!g_Captured)
        {
            last = g_UiBeat;
            lastMs = GetTickCount();
            continue;
        }
        if (g_UiBeat != last)
        {
            last = g_UiBeat;
            lastMs = GetTickCount();
            continue;
        }
        if (GetTickCount() - lastMs < CAPWD_STALL_MS)
            continue;
        {   CHAR lineBuffer[192], *lineCursor = lineBuffer;
            ClipCursor(NULL);
            if (g_LowLevelKeyboard)
            {
                UnhookWindowsHookEx(g_LowLevelKeyboard);
                g_LowLevelKeyboard = NULL;
            }
            InterlockedExchange(&g_Captured, 0);
            ++g_CaptureWatchdogReleased;
            lineCursor = LogPut(lineCursor, "CAPTURE-WATCHDOG: UI thread silent for ");
            lineCursor = LogHex(lineCursor, GetTickCount() - lastMs);
            lineCursor = LogPut(lineCursor, " ms while captured -- cursor clip and keyboard hook RELEASED\r\n");
            LogAppend(LOG_PATH, lineBuffer, lineCursor);
            SerialOut(lineBuffer, lineCursor); }
        lastMs = GetTickCount();
    }
    return 0;
}

/* #218: the idle clock. A MOVE is a real change of screen position -- Windows also
 * sends WM_MOUSEMOVE when nothing moved (a window appearing under a still pointer, a
 * SetCursor), and counting those would keep a still pointer visible for ever.
 */
static VOID CursorIdleNoteMove(HWND window)
{
    POINT point;

    if (!GetCursorPos(&point))
        return;
    if (point.x == g_CursorLastPoint.x && point.y == g_CursorLastPoint.y)
        return;
    g_CursorLastPoint = point;
    g_CursorMovedMs = GetTickCount();
    if (g_CursorIdle)
    {
        g_CursorIdle = 0;
        HostCursorRefresh(window);
    }
}

/* From the UI tick: still for CURSOR_IDLE_MS over OUR video -> hide. Only for a program
 * that does not use the mouse (one that does is governed by capture), and never while
 * the pointer is over someone else's window or our status strip.
 */
static VOID CursorIdleTick(HWND window)
{
    POINT point;
    POINT client;
    HWND under;

    if (g_CursorIdle || g_Captured || CaptureAllowed())
        return;
    if (g_HostCursorMode != HOSTCUR_SMART)
        return;                                             /* s84: the idle rule is Smart's */
    if (!g_CursorMovedMs)
    {
        g_CursorMovedMs = GetTickCount();
        return;
    }
    if (GetTickCount() - g_CursorMovedMs < CURSOR_IDLE_MS)
        return;
    if (!GetCursorPos(&point))
        return;
    if (point.x != g_CursorLastPoint.x || point.y != g_CursorLastPoint.y)     /* moved elsewhere */
    {
        g_CursorLastPoint = point;
        g_CursorMovedMs = GetTickCount();
        return; }
    under = WindowFromPoint(point);
    if (under != window)
        return;
    client = point;
    ScreenToClient(window, &client);
    if (!IsPointOverVideo(window, client.x, client.y))
        return;
    g_CursorIdle = 1;
    HostCursorRefresh(window);
}

/* FULLSCREEN IS A WINDOW STYLE, NOT JUST A DIRECTDRAW MODE. (s64) (Importance = 2):
 * We asked DirectDraw for an exclusive fullscreen mode and left the WINDOW exactly
 * as it was: WS_OVERLAPPEDWINDOW, caption, thick resizing frame, menu bar. The
 * DirectDraw primary covers the pixels, so it LOOKS right -- until the pointer nears
 * an edge and Windows hit-tests the frame that is still there and hands back a
 * RESIZE cursor over a fullscreen game. The user saw exactly that and drew the right
 * conclusion: "fullscreen mode might not be using the correct window style. It
 * should essentially have no style."
 * - So take the style off going in and put it back coming out: WS_POPUP over the whole
 *   virtual screen, no caption, no frame, no menu. There is then no non-client area to
 *   hit-test, which is what actually removes the resize cursors -- SetCursor could
 *   never have done it, because the frame cursors are decided in WM_NCHITTEST long
 *   before WM_SETCURSOR is asked.
 *
 * [CAUTION]: AND RESTORE THE GEOMETRY, WHICH IS THE OTHER HALF OF THE SAME BUG. Going exclusive
 * leaves the window sized to the screen; coming back out we used to keep that size,
 * so Alt+Enter twice turned a 640x480 window into a full-screen-sized one that was
 * no longer fullscreen. Alt+Enter is a TOGGLE and a toggle must land back where it
 * started. GetWindowPlacement/SetWindowPlacement rather than a bare rect: it carries
 * the maximised/minimised state too, so a window that was maximised before returns
 * maximised rather than to some remembered restored size.
 *
 * [CAUTION]: ORDER, both ways. Going in: style first, then DirectDraw -- ddraw wants the window
 * it is about to own to already be the shape it will be. Coming out: DirectDraw
 * first, then style, then placement, so the mode is back before we ask Windows to
 * lay a window out on it.
 */
static WINDOWPLACEMENT g_FullscreenPlace;      /* geometry to come back to */
static LONG            g_FullscreenStyle;      /* the style we took off */
static LONG            g_FullscreenExStyle;
static INT             g_FullscreenSaved;

/* Undo everything the fullscreen entry changed. Separate because BOTH the normal exit
 * and the "DirectDraw refused" path need it, and a chromeless window that is not
 * fullscreen is a worse state than either end of the toggle.
 */
static VOID HostFullscreenToggleRestore(HWND window)
{
    if (g_Status) { ShowWindow(g_Status, SW_SHOW);
                    SendMessageA(g_Status, WM_SIZE, 0, 0); }   /* re-dock at the bottom */
    if (g_FsMenu)
    {
        SetMenu(window, g_FsMenu);
        g_FsMenu = NULL;
    }
    if (g_FullscreenStyle)
        SetWindowLongA(window, GWL_STYLE, g_FullscreenStyle);
    if (g_FullscreenExStyle)
        SetWindowLongA(window, GWL_EXSTYLE, g_FullscreenExStyle);
    /* SWP_FRAMECHANGED before the placement: the frame has to exist again for
     * SetWindowPlacement's rect to mean the same thing it did when we saved it.
     */
    SetWindowPos(window, NULL, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE
                 | SWP_FRAMECHANGED);
    if (g_FullscreenSaved) { g_FullscreenPlace.length = sizeof g_FullscreenPlace;
                      SetWindowPlacement(window, &g_FullscreenPlace);
                      g_FullscreenSaved = 0; }
    g_FullscreenStyle = g_FullscreenExStyle = 0;
    DrawMenuBar(window);
    InvalidateRect(window, NULL, TRUE);
}

static VOID HostFullscreenToggle(HWND window)
{
    if (g_Safe.Fullscreen && !g_PresentDdraw.IsFullscreen)
        return;                                                      /* s90 #132: SAFE MODE stays windowed */
    INT want = !g_PresentDdraw.IsFullscreen;
    if (want)
    {
        g_FullscreenPlace.length = sizeof g_FullscreenPlace;
        g_FullscreenSaved   = GetWindowPlacement(window, &g_FullscreenPlace) ? 1 : 0;
        g_FullscreenStyle   = GetWindowLongA(window, GWL_STYLE);
        g_FullscreenExStyle = GetWindowLongA(window, GWL_EXSTYLE);
        g_FsMenu    = GetMenu(window);
        SetMenu(window, NULL);
        /* WS_VISIBLE stays -- everything else that draws or hit-tests chrome goes.
         * WS_EX_ prefixes that put a border on (WINDOWEDGE / CLIENTEDGE / DLGMODALFRAME
         * / STATICEDGE) go with it.
         */
        SetWindowLongA(window, GWL_STYLE, (g_FullscreenStyle & ~(LONG)WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowLongA(window, GWL_EXSTYLE, g_FullscreenExStyle
                       & ~(LONG)(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE
                                 | WS_EX_DLGMODALFRAME | WS_EX_STATICEDGE));
        /* The status strip is a CHILD WINDOW, so taking the menu and frame off does
         * not remove it -- it would sit on top of the picture at whatever size it
         * last docked to. PresentGdi already stops reserving room for it in
         * fullscreen; this stops it being drawn.
         */
        if (g_Status)
            ShowWindow(g_Status, SW_HIDE);
        SetWindowPos(window, HWND_TOP, 0, 0,
                     GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
                     SWP_FRAMECHANGED | SWP_NOACTIVATE);
        if (PresentDdrawSetFullscreen(&g_PresentDdraw, TRUE) != 0)
        {
            /* No DirectDraw, or it refused. Do not leave the user in a chromeless
             * window that is not fullscreen either -- put it all back.
             */
            HostFullscreenToggleRestore(window);
            return;
        }
        if (g_Captured)
            FullscreenReleaseHint();                      /* #138 */
    }
    else
    {
        PresentDdrawSetFullscreen(&g_PresentDdraw, FALSE);
        HostFullscreenToggleRestore(window);
    }
    /* The window just changed shape. If the guest holds the mouse, the ClipCursor
     * rect is now describing the window we USED to be -- re-fence it. Alt+Enter is
     * reachable while captured (see WM_SYSKEYDOWN), so this is a live path, not a
     * theoretical one.
     */
    CaptureClipApply(window);
    HostCursorRefresh(window);
    /* SAY WHAT ACTUALLY HAPPENED, IN DECIMAL, EVERY TOGGLE (Importance = 2):
     * "None of them seem to actually achieve sharp pixels" (user, s64) could not be
     * diagnosed from outside: a requested mode that the display REFUSES falls back
     * silently, and a fitted rectangle that is a fractional multiple of the frame
     * looks identical in a log to one that is whole. Both are now on one line.
     * READ IT LIKE THIS:
     * req != got      -> the display refused the mode; the fallback is what you
     *                    are looking at, and no scaling choice can fix softness
     *                    introduced by the monitor rescaling a non-native signal.
     * scale=NxM       -> WHOLE NUMBERS MEAN SHARP. Anything else is a forced ratio
     *                    with no exact whole pair, Fill, or a frame larger than
     *                    the screen -- drawn with the Filtering setting (#325).
     * got == native   -> the panel is getting its own resolution, which is the
     *                    other half of sharpness and the half we do not control.
     */
    if (g_PresentDdraw.IsFullscreen)
    {
        CHAR lineBuffer[256];
        CHAR *lineCursor = lineBuffer;
        RECT clientRect;
        INT clientWidth = 0;
        INT clientHeight = 0;
        if (GetClientRect(window, &clientRect))
        {
            clientWidth = clientRect.right;
            clientHeight = clientRect.bottom;
        }
        lineCursor = LogPut(lineCursor, "FULLSCREEN: path=");
        lineCursor = LogPut(lineCursor, (g_PresentDdraw.DirectDraw && g_PresentDdraw.Back) ? "ddraw-exclusive" : "gdi-borderless");
        lineCursor = LogPut(lineCursor, " client=");           lineCursor = LogDecimal(lineCursor, (UINT)clientWidth);
        lineCursor = LogPut(lineCursor, "x");                  lineCursor = LogDecimal(lineCursor, (UINT)clientHeight);
        lineCursor = LogPut(lineCursor, " frame=");            lineCursor = LogDecimal(lineCursor, (UINT)g_Video.Frame.Width);
        lineCursor = LogPut(lineCursor, "x");                  lineCursor = LogDecimal(lineCursor, (UINT)g_Video.Frame.Height);
        if (g_Video.Frame.Width && g_Video.Frame.Height)
        {
            INT frameX;
            INT frameY;
            INT frameWidth;
            INT frameHeight;
            PresentLayout(g_PresentDdraw.Aspect, g_PresentDdraw.Fit, PRESENT_LAYOUT_SCREEN, clientWidth, clientHeight, (INT)g_Video.Frame.Width,
                           (INT)g_Video.Frame.Height, &frameX, &frameY, &frameWidth, &frameHeight);   /* #325: what is drawn */
            lineCursor = LogPut(lineCursor, " dest=");  lineCursor = LogDecimal(lineCursor, (UINT)frameWidth);
            lineCursor = LogPut(lineCursor, "x");       lineCursor = LogDecimal(lineCursor, (UINT)frameHeight);
            lineCursor = LogPut(lineCursor, " at ");    lineCursor = LogDecimal(lineCursor, (UINT)frameX);
            lineCursor = LogPut(lineCursor, ",");       lineCursor = LogDecimal(lineCursor, (UINT)frameY);
            lineCursor = LogPut(lineCursor, " scale="); lineCursor = LogDecimal(lineCursor, (UINT)(frameWidth / (INT)g_Video.Frame.Width));
            lineCursor = LogPut(lineCursor, "x");       lineCursor = LogDecimal(lineCursor, (UINT)(frameHeight / (INT)g_Video.Frame.Height));
            lineCursor = LogPut(lineCursor, " rem=");   lineCursor = LogDecimal(lineCursor, (UINT)(frameWidth % (INT)g_Video.Frame.Width));
            lineCursor = LogPut(lineCursor, ",");       lineCursor = LogDecimal(lineCursor, (UINT)(frameHeight % (INT)g_Video.Frame.Height));
        }
        lineCursor = LogPut(lineCursor, " aspect=");  lineCursor = LogDecimal(lineCursor, (UINT)g_PresentDdraw.Aspect);
        lineCursor = LogPut(lineCursor, "\r\n");
        LogAppend(LOG_PATH, lineBuffer, lineCursor); SerialOut(lineBuffer, lineCursor);
    }
}

/* -- START FULLSCREEN (s68). One decision per process, on the UI thread, and it only
 * ever turns fullscreen ON: Alt+Enter owns everything after that. `graphics` says
 * whether the guest is in a graphics mode right now -- Always fires regardless,
 * Graphics only waits for it, Never does nothing.
 */
static INT g_AutoFullscreenDone = 0;
static VOID HostAutoFullscreenConsider(HWND window, INT graphics)
{
    DWORD mode = g_Settings.Values[SET_AUTOFS];

    if (g_AutoFullscreenDone || g_WowLaunch || g_Headless)
        return;
    if (mode == AUTOFS_NEVER)
    {
        g_AutoFullscreenDone = 1;
        return;
    }
    if (mode == AUTOFS_GRAPHICS && !graphics)
        return;
    g_AutoFullscreenDone = 1;
    if (!g_PresentDdraw.IsFullscreen)
        HostFullscreenToggle(window);
}

/* Frames the presenter drops between the ones it shows. 0 = every frame, which is
 * what this host has always done. Read on the UI thread's timer tick.
 */
INT g_FrameSkip;

/* #325: THE WINDOW IS THE PICTURE, AT A WHOLE SCALE:
 * 1x is one desktop pixel per frame pixel and Nx an N x N block, and the picture is
 * the FRAME's own size -- 720x400 text, 320x200, Mode X 320x240, a 1280x1024 VESA
 * mode -- with the window's frame (borders, caption, menu, status strip) added
 * outside it. There is no minimum any more: it was 640x480 on-aspect, so 320x200 at
 * "1x" was really 2x by 2.4x and nothing was pixel-exact. A forced ratio keeps the
 * width and shapes the height (PresentWindowPicture).
 */

static VOID HostFrameSize(INT *frameWidth, INT *frameHeight)
{
    if (g_PresentDdraw.IsSnapshotValid && g_PresentDdraw.SnapshotWidth > 0 && g_PresentDdraw.SnapshotHeight > 0)
    {
        *frameWidth = g_PresentDdraw.SnapshotWidth;
        *frameHeight = g_PresentDdraw.SnapshotHeight;
        return;
    }
    if (g_Video.Frame.Width && g_Video.Frame.Height)
    {
        *frameWidth = (INT)g_Video.Frame.Width;
        *frameHeight = (INT)g_Video.Frame.Height;
        return;
    }
    *frameWidth = VIDEO_TEXT_FRAME_WIDTH;
    *frameHeight = VIDEO_TEXT_FRAME_HEIGHT;                          /* before the first frame: VGA text */
}

static VOID HostPicture(INT scale, INT *pictureWidth, INT *pictureHeight)
{
    INT frameWidth;
    INT frameHeight;

    HostFrameSize(&frameWidth, &frameHeight);
    PresentWindowPicture((INT)g_Settings.Values[SET_ASPECT], frameWidth, frameHeight, scale, pictureWidth, pictureHeight);
}

/* What the frame adds around the video: borders, caption, menu bar, status strip.
 *
 * [CAUTION]: Asked of the window rather than assumed, because fullscreen detaches the
 * menu and AdjustWindowRect's answer changes when it does.
 */
static VOID HostFrameExtra(HWND window, INT *extraWidth, INT *extraHeight)
{
    RECT frame;

    frame.left = 0;
    frame.top = 0;
    frame.right = 0;
    frame.bottom = 0;
    AdjustWindowRect(&frame, (DWORD)GetWindowLongA(window, GWL_STYLE), GetMenu(window) != NULL);
    *extraWidth = frame.right - frame.left;
    *extraHeight = (frame.bottom - frame.top) + (g_PresentDdraw.StatusHeight ? g_PresentDdraw.StatusHeight : PRESENT_STATUS_HEIGHT);
}

/* The client room the desktop's WORK AREA (the screen less the taskbar) leaves for a
 * picture once the window's frame is added.
 */
static VOID HostWorkArea(RECT *workArea)
{
    if (!SystemParametersInfoA(SPI_GETWORKAREA, 0, workArea, 0))
    {
        workArea->left = 0;
        workArea->top = 0;
        workArea->right = GetSystemMetrics(SM_CXSCREEN);
        workArea->bottom = GetSystemMetrics(SM_CYSCREEN);
    }
}

static VOID HostWorkRoom(INT *roomWidth, INT *roomHeight)
{
    RECT workArea;
    INT extraWidth;
    INT extraHeight;

    HostWorkArea(&workArea);
    if (g_PresentDdraw.Window)
        HostFrameExtra(g_PresentDdraw.Window, &extraWidth, &extraHeight);
    else
    {
        RECT frame;
        frame.left = frame.top = frame.right = frame.bottom = 0;
        AdjustWindowRect(&frame, WS_OVERLAPPEDWINDOW, TRUE);
        extraWidth = frame.right - frame.left;
        extraHeight = (frame.bottom - frame.top) + PRESENT_STATUS_HEIGHT;
    }
    *roomWidth = (workArea.right - workArea.left) - extraWidth;
    *roomHeight = (workArea.bottom - workArea.top) - extraHeight;
}

/* Does the picture at this scale fit on the work area? */
static INT WindowScaleFits(INT scale)
{
    INT pictureWidth;
    INT pictureHeight;
    INT roomWidth;
    INT roomHeight;

    HostPicture(scale, &pictureWidth, &pictureHeight);
    HostWorkRoom(&roomWidth, &roomHeight);
    return pictureWidth <= roomWidth && pictureHeight <= roomHeight;
}

enum
{
    HOST_SCALE_MAX = 4
};   /* the window's integer scales: 1x to 4x */
/* -- EVERY MENU-BACKED SETTING'S TICK, FROM THE ONE PLACE THAT KNOWS THE VALUES. -
 * CheckMenuRadioItem for the dropdowns rather than a tick, because they are
 * exclusive and a bullet is what Windows uses to say so -- and because it clears
 * the siblings in one call. A menu showing two scalers at once is worse than one
 * showing none: it looks authoritative.
 *
 * [CAUTION]: DRIVEN FROM g_Settings, NOT FROM WHATEVER THE HANDLER JUST DID. The dialog can
 * change these too, and so can the startup load; re-reading the live settings is
 * the only version that is right for all three callers.
 */
VOID MenuViewSync(HWND window)
{
    HMENU menu = GetMenu(window);
    INT index;

    if (!menu)
        menu = g_FsMenu;
    if (!menu)
        return;
    for (index = 0; index < MENU_COMBO_N; ++index)
    {
        UINT base = g_MenuCombos[index].Base;
        const SET_DEF *definition = &g_SetDefinitions[g_MenuCombos[index].IsSet];
        DWORD value = g_Settings.Values[g_MenuCombos[index].IsSet];
        if (value > definition->High)
            value = definition->Low;
        CheckMenuRadioItem(menu, base, base + (UINT)definition->High, base + (UINT)value, MF_BYCOMMAND);
    }
    for (index = 0; index < MENU_CHECK_N; ++index)
        CheckMenuItem(menu, g_MenuChecks[index].Id, MF_BYCOMMAND
                      | (g_Settings.Values[g_MenuChecks[index].IsSet] ? MF_CHECKED : MF_UNCHECKED));
    /* -- A SCALE THAT CANNOT FIT THE DISPLAY IS GREYED, NOT SILENTLY SUBSTITUTED.
     * HostApplyScale() steps down until the window fits, which is the right
     * thing to DO and the wrong thing to say nothing about: picking 3x on a
     * 1680x1050 desktop quietly gave 2x, so the menu reported a size the window
     * did not have and the setting looked broken rather than impossible.
     *
     * [CAUTION]: This is NOT the scaffold-stub case, so the standing "enabled, not greyed"
     * decision does not apply: those items are unimplemented, and this one is
     * implemented and physically impossible on THIS display. Greying says which.
     *
     * [CAUTION]: Re-evaluated on every sync rather than once, because the work area moves --
     * a taskbar that auto-hides, a second monitor, a resolution change.
     */
    {   INT scale;
        for (scale = 2; scale <= HOST_SCALE_MAX; ++scale)          /* #325: 1x is always offered (scaled to fit if need be) */
            EnableMenuItem(menu, IDM_WINSIZE_0 + (UINT)(scale - 1), MF_BYCOMMAND
                           | (WindowScaleFits(scale) ? MF_ENABLED : MF_GRAYED)); }
    /* -- RULE 1, SAID IN THE MENU. Same argument as the scale items above and NOT the
     * scaffold-stub case: Capture Mouse is implemented, and it is impossible for a
     * guest that has never called INT 33h -- there is nothing to capture the mouse
     * INTO. An enabled item that silently does nothing is the worse answer.
     */
    EnableMenuItem(menu, IDM_INPUT_CAPTURE, MF_BYCOMMAND
                   | (CaptureAllowed() ? MF_ENABLED : MF_GRAYED));
    CheckMenuItem(menu, IDM_INPUT_CAPTURE, MF_BYCOMMAND
                  | (g_Captured ? MF_CHECKED : MF_UNCHECKED));
}

/* #325: SIZE THE WINDOW TO THE PICTURE AT WHOLE SCALE k:
 * The largest scale up to k that fits the work area; if even 1x does not (1280x1024
 * on a 1050-line screen), the picture is scaled DOWN on-ratio to fit and the status
 * strip says so -- a window running off the screen is worse than a smaller picture.
 *
 * [CAUTION]: THE TOP-LEFT STAYS PUT, so a program that changes mode does not throw the window
 * around -- but a window that would now run off the work area is pulled back on.
 *
 * [CAUTION]: THE MENU BAR WRAPS when the window is narrow (320 wide at 1x), and AdjustWindowRect
 * does not know: the client is MEASURED after the move and the height corrected, or
 * the bottom of the picture would sit under the status strip.
 *
 * [CAUTION]: A MAXIMISED WINDOW IS RESTORED FIRST, or Windows still believes it is maximised and
 * the next restore snaps it back.
 */
static VOID HostApplyScale(HWND window, INT scale)
{
    INT pictureWidth;
    INT pictureHeight;
    INT roomWidth;
    INT roomHeight;
    INT extraWidth;
    INT extraHeight;
    INT windowWidth;
    INT windowHeight;
    INT left;
    INT top;
    INT index;
    RECT windowRect;
    RECT workArea;
    RECT clientRect;

    if (!window || g_PresentDdraw.IsFullscreen)
        return;                                           /* fullscreen owns the size */
    if (IsZoomed(window))
        ShowWindow(window, SW_RESTORE);
    if (scale < 1)
        scale = 1;
    g_ScaleWant = scale;
    while (scale > 1 && !WindowScaleFits(scale))
        --scale;
    g_ScaleFactor = scale;
    HostPicture(scale, &pictureWidth, &pictureHeight);
    HostWorkRoom(&roomWidth, &roomHeight);
    g_FitDown = 0;
    if (pictureWidth > roomWidth || pictureHeight > roomHeight)
    {
        INT fitX;
        INT fitY;
        PresentFitRatio(roomWidth, roomHeight, pictureWidth, pictureHeight, &fitX, &fitY, &pictureWidth, &pictureHeight);
        g_FitDown = 1;
    }
    HostFrameExtra(window, &extraWidth, &extraHeight);
    windowWidth = pictureWidth + extraWidth;
    windowHeight = pictureHeight + extraHeight;
    GetWindowRect(window, &windowRect);
    HostWorkArea(&workArea);
    left = windowRect.left;
    top = windowRect.top;
    if (left + windowWidth > workArea.right)
        left = workArea.right - windowWidth;
    if (top + windowHeight > workArea.bottom)
        top = workArea.bottom - windowHeight;
    if (left < workArea.left)
        left = workArea.left;
    if (top < workArea.top)
        top = workArea.top;
    SetWindowPos(window, NULL, left, top, windowWidth, windowHeight, SWP_NOZORDER | SWP_NOACTIVATE);
    for (index = 0; index < 2 && GetClientRect(window, &clientRect); ++index)
    {
        INT deltaWidth = pictureWidth - clientRect.right;
        INT deltaHeight = (pictureHeight + (g_PresentDdraw.StatusHeight ? g_PresentDdraw.StatusHeight : PRESENT_STATUS_HEIGHT)) - clientRect.bottom;
        if (!deltaWidth && !deltaHeight)
            break;
        windowWidth += deltaWidth;
        windowHeight += deltaHeight;
        SetWindowPos(window, NULL, 0, 0, windowWidth, windowHeight, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    HostFrameSize(&g_WindowFrameWidth, &g_WindowFrameHeight);
}

VOID HostApplyWindowSize(HWND window, DWORD index)
{
    HostApplyScale(window, (INT)index + 1 <= HOST_SCALE_MAX ? (INT)index + 1 : 1);
}

/* PUT g_Settings INTO EFFECT, WITHOUT TOUCHING THE REGISTRY:
 * One function so the View menu, the CPU Speed submenu and the dialog's OK all
 * reach the machine by the same path -- three call sites that each remembered a
 * different subset is how a knob ends up working from one route and not another.
 *
 * [CAUTION]: SettingsApplyDevices is NOT called here. Nothing on the View menu is an audio
 * setting, and re-pushing the mixer on every scaler change is reach this has no
 * business having. The dialog calls it separately, where it belongs.
 */
/* [CAUTION]: THE RESIZE FIRES ONLY WHEN THE SIZE SETTING ACTUALLY CHANGED, and that is not
 * an optimisation. Every live apply passes through here, so resizing
 * unconditionally would mean picking a different SCALER snapped a window the user
 * had dragged to their own size back to 1x -- the setting reaching past its own
 * business, which is the thing that makes people stop touching the menu.
 */
DWORD g_WindowSizeLive = WINDOW_SETTING_UNSET_U;   /* what the window is currently AT */
DWORD g_AspectLive  = WINDOW_SETTING_UNSET_U;   /* ...and the shape it is that size IN */
enum
{
    WINDOW_RESIZE_SETTLE_MS = 150
};   /* a frame-size change must hold this long before it is applied */
/* -- #325: THE WINDOW FOLLOWS THE MODE. When the frame's size changes -- text to a
 * 320x200 game, a VESA mode, Mode X -- a normal (not maximised, not fullscreen)
 * window is re-sized to the new picture at the scale it is at. Debounced: the new
 * size must hold for 150 ms, so a program that passes through a mode on its way to
 * another does not make the window jump twice. Maximised and fullscreen are fitted
 * per frame by PresentLayout and are left alone.
 */
static VOID HostFollowFrame(HWND window)
{
    static INT pendW;
    static INT pendH;
    static DWORD pendT;
    INT frameWidth;
    INT frameHeight;

    if (!window || g_PresentDdraw.IsFullscreen || IsZoomed(window) || IsIconic(window) || !g_PresentDdraw.IsSnapshotValid)
        return;
    HostFrameSize(&frameWidth, &frameHeight);
    if (frameWidth == g_WindowFrameWidth && frameHeight == g_WindowFrameHeight)
    {
        pendW = pendH = 0;
        return;
    }
    if (frameWidth != pendW || frameHeight != pendH)
    {
        pendW = frameWidth;
        pendH = frameHeight;
        pendT = GetTickCount();
        return;
    }
    if (GetTickCount() - pendT < WINDOW_RESIZE_SETTLE_MS)
        return;
    HostApplyScale(window, g_ScaleWant ? g_ScaleWant : (INT)g_Settings.Values[SET_WINSIZE] + 1);
}

/* THE TABBED SETTINGS DIALOG:
 * IDD_SETTINGS is only a frame: a tab control, OK, Cancel, Restore Defaults. Each
 * tab is its OWN child dialog (IDD_PAGE_*), created here and parked in the tab's
 * display rectangle. Nothing below is written per-control -- every fill and every
 * read is a loop over g_SetDefinitions in settings.h, so adding a knob is one table row and
 * one line of .rc layout, not four edits in four functions that can disagree.
 */
/* s84, the user's redesign: six tabs in a 640 x 480 dialog. Processor, Memory and
 * Advanced became "Machine"; General is "MS-DOS"; Display is "Video".
 */
const INT g_SettingsPages[NTVDMEX_PAGE_COUNT] = {
    IDD_PAGE_GENERAL, IDD_PAGE_CPU, IDD_PAGE_DISPLAY,
    IDD_PAGE_AUDIO,   IDD_PAGE_INPUT, IDD_PAGE_DRIVES
};
#define PLANEDUMP_FLAG  CFG_("planedump.flag")
enum
{
    PAUSE_SUSPEND_TRIES = 200
};   /* HostPauseSet: attempts to catch the CPU thread */
enum
{
    UI_TICK_FRAME_NUMERATOR = 9, UI_TICK_FRAME_DENOMINATOR = 10, UI_PRESENT_STALE_FRAMES = 2
};   /* the UI tick: 90% of a frame; quiet for two */
/* --- the UI thread: window + present + frame timer ------------------------- */
/* -- #219: ONLY THE FOCUSED NTVDMEX WINDOW RUNS. (user, s83 sweep; decision 2026-09-28:
 * always, no setting) Skyroads and Doom side by side were "very jittery": two guests
 * each burning a core. So a window that loses focus is paused COMPLETELY -- CPU,
 * timers, sound, input -- and carries on exactly where it was when it gets focus back.
 * - THE CPU: the thread running the guest is suspended while it is INSIDE GUEST CODE,
 *   by the same handshake the async injectors use (own g_AsyncContextWrite, suspend, read the
 *   context so the suspend has landed, and only keep it if g_InExec is still 1 -- a
 *   thread in host code might hold g_Lock). Holding g_AsyncContextWrite for the whole pause
 *   also makes every injector decline. If the thread is in host code at that moment it
 *   parks itself at the top of its exec loop instead (the two `while (g_PauseWant)`s).
 * - TIME: HostPitGenerate drops the paused interval instead of owing it, so there is
 *   no burst of ticks on resume; HostAudioFill returns silence without rendering any
 *   device, so a DMA block or an envelope resumes from the same sample.
 * - SOUND THAT IS NOT OURS TO FREEZE: notes already in XP's MIDI synth are silenced
 *   (they cannot be held), and the real speaker is switched off.
 *
 * [CAUTION]: NOT WIN16 (-w): its windows are real desktop windows and one WOW VDM hosts several
 * tasks, so "the window lost focus" does not mean "this program is in the background".
 *
 * [CAUTION]: NOT HEADLESS: the rig harness runs unattended with nothing focused.
 */
static VOID HostPauseSet(INT isOn)
{
    if (isOn)
    {
        DWORD index;
        if (g_PauseWant || g_Headless || g_WowLaunch || !g_HostCpu)
            return;
        InterlockedExchange(&g_PauseWant, 1);
        g_PauseMs = GetTickCount();
        ++g_PauseCount;
        AudioWaveMidiSilence(&g_Wave);
        if (g_SpeakerReal)
            PcSpeakerSet(&g_PcSpeaker, 0);
        for (index = 0; index < PAUSE_SUSPEND_TRIES && g_PauseWant; ++index)
        {
            CONTEXT context;
            if (InterlockedCompareExchange(&g_AsyncContextWrite, 1, 0) != 0)
            {
                Sleep(1);
                continue;
            }
            if (SuspendThread(g_HostCpu) == (DWORD)-1)
            {
                ASYNC_CTX_RELEASE();
                break;
            }
            context.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(g_HostCpu, &context) && g_InExec == 1)
            {
                g_PauseSuspended = 1;
                break;
            }
            ResumeThread(g_HostCpu);
            ASYNC_CTX_RELEASE();
            Sleep(1);
        }
        if (g_Window)
            InvalidateRect(g_Window, NULL, FALSE);
    }
    else
    {
        if (!g_PauseWant)
            return;
        if (g_PauseSuspended)
        {
            g_PauseSuspended = 0;
            ResumeThread(g_HostCpu);
            ASYNC_CTX_RELEASE();
        }
        InterlockedExchange(&g_PauseWant, 0);
        {   CHAR buffer[160], *cursor = buffer;
            cursor = LogPut(cursor, "PAUSE: resumed after "); cursor = LogDecimal(cursor, GetTickCount() - g_PauseMs);
            cursor = LogPut(cursor, " ms (pauses="); cursor = LogDecimal(cursor, g_PauseCount);
            cursor = LogPut(cursor, " parked-in-loop="); cursor = LogDecimal(cursor, g_PauseCooperative); cursor = LogPut(cursor, ")\r\n");
            LogAppend(LOG_PATH, buffer, cursor); }
    }
}

enum
{
    HOST_DUMP_INTERVAL_MS = 5000, FULLSCREEN_HINT_MS = 5000, CAPTURE_SHOTS_MAX = 40, CAPTURE_NAME_DIGITS = 4, HOST_MINIMUM_PICTURE_WIDTH = 160, HOST_MINIMUM_PICTURE_HEIGHT = 100
};   /* the census interval, the leave-fullscreen hint, capture.flag's shots and "shotNN.bmp", the smallest picture */
static LRESULT CALLBACK HostWindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    /* GH #281: a command from the manager's tray menu, mapped onto this host's own
     * menu so each does exactly what the window's menu does. Show for a Win16 host
     * is the manager's job (it brings the program's own window forward); this host's
     * window is the hidden machine, which is not what anyone wants to see.
     */
    if (g_ManagerCommandMessage && message == g_ManagerCommandMessage)
    {
        switch (wParam)
        {
        case MGR_COMMAND_SHOW:
            if (!g_WowLaunch) { if (IsIconic(window))
                ShowWindow(window, SW_RESTORE);
                                 SetForegroundWindow(window); }
            return 0;

        case MGR_COMMAND_SETTINGS:
            PostMessageA(window, WM_COMMAND, IDM_FILE_SETTINGS, 0);
        return 0;

        case MGR_COMMAND_CLOSE_PROGRAM:
            PostMessageA(window, WM_COMMAND, IDM_FILE_CLOSEPROG, 0);
        return 0;

        case MGR_COMMAND_EXIT:
            PostMessageA(window, WM_COMMAND, IDM_FILE_EXIT, 0);
        return 0;
        }
        return 0;
    }
    switch (message)
    {
    case WM_APP_PRESENT:
        InterlockedExchange(&g_UiPresentPending, 0);
        g_UiForced = 1;                 /* raised by the guest's frame: no floor, no phase gate */

        /* fall through */
    case WM_TIMER:
        /* Liveness beat for the capture watchdog: proof the UI thread is still pumping
         * messages. Taken FIRST, before any of the frame work below, so the beat means
         * "this thread reached its timer", not "this thread finished a frame".
         */
        InterlockedIncrement(&g_UiBeat);
        CursorIdleTick(window);                    /* #218: 5 s still over the video -> hide */
        CaptureClipGuard(window);                  /* captured -> the clip is still ours */
        BackgroundPriorityTick(window);                        /* #211 */
        /* -- THE 5 ms FRAME TIMER WAS NEVER ACTUALLY HONOURED, AND THE PACER REVEALED IT.
         * SetTimer asks for VID_PRESENT_TICK_MS = 5, but XP's default timer granularity
         * is 15.6 ms, so this body has ALWAYS run at ~64 Hz -- which is exactly the
         * "HostPitSync ran 65 times a second" that e220033 measured and set out to
         * fix. That commit raises the system-wide resolution with timeBeginPeriod(1) so
         * the pacer's 1 ms Sleep works (audio_wave.c does NOT do this -- the claim that
         * it already did is about loading winmm dynamically, not about resolution).
         * Raising it system-wide also makes THIS timer start firing at the 5 ms it
         * always asked for: **3x the frame work**, and VddBusFrame renders the whole
         * frame under g_Lock every single tick, at a site whose measured holds are
         * 13-22 ms. A body that cannot finish inside its own period saturates the UI
         * thread, and the UI thread is the one that turns WM_KEYUP into a break code.
         * - That is the shape the user measured by hand: pitpace=1 and pitpace=4 feel
         *   IDENTICAL (both call timeBeginPeriod), pitpace=0 is clean (it does not), and
         *   disabling the pacer's injection did not help (resolution still raised). The
         *   pacer's PERIOD was never the variable.
         * - So bound the expensive body to the rate it has actually always run at, and
         *   leave the tick itself fast -- the present is phase-gated and WANTS to sample
         *   often. uitick.txt sets the floor in ms; 0 restores the unbounded behaviour.
         */
        {   static LARGE_INTEGER body;
            LARGE_INTEGER bodyNow;
            /* Auto: the timer is the FALLBACK for a guest that never polls the retrace,
             * at 90% of the mode's own frame period so it cannot fall to every other
             * tick; a hook-raised run resets the interval so the two never double up.
             */
            UINT32 floorMicroseconds = g_UiTickMinimumMs != UITICK_AUTO
                                ? (UINT32)g_UiTickMinimumMs * MICROSECONDS_PER_MILLISECOND_U
                                : VddVideoFrameUs(&g_Video) * UI_TICK_FRAME_NUMERATOR / UI_TICK_FRAME_DENOMINATOR;
            QueryPerformanceCounter(&bodyNow);
            if (!g_UiForced && body.QuadPart &&
                QpcMicroseconds(bodyNow.QuadPart - body.QuadPart) < floorMicroseconds)
            {
                ++g_UiTickSkips;
                return 0;
            }
            body = bodyNow;
        }
        /* DO NOT MAKE THE MEASUREMENT DEPEND ON A CLEAN EXIT:
         * The STAGE2 summary is only written when the guest terminates, and two runs
         * in a row failed to produce one -- the first because the guest was still
         * running when the log was read, the second because the box was rebooted. An
         * instrument that only reports if the run ends politely is an instrument that
         * does not report. Dump it every 5 s instead, so whatever state the run ends
         * in, the last five seconds of it are on disk.
         */
        {   static DWORD lastKeyDump;
            DWORD nowTicks = GetTickCount();
            /* g_KeyMessageCount counts WINDOWS key messages, so gating on it alone silently
             * disabled this whole dump for SCRIPTED runs (keys.txt drives
             * HostKeyScancode directly, never WM_KEYDOWN) -- a headless repro
             * attempt produced one sample and no IRQ1GATE at all. Gate on either half.
             */
            if ((g_KeyMessageCount || g_KeyDeliveryCount) && (DWORD)(nowTicks - lastKeyDump) >= HOST_DUMP_INTERVAL_MS)
            {
                /* [CAUTION]: 768, and the margin is the point. At 384 this line already emitted
                 * 379 bytes; adding two more fields took it past 418 and SMASHED THE UI
                 * THREAD'S STACK. Two experiments "died" on that and both verdicts were
                 * mine, not the code's -- the tell was a build whose new code path had
                 * provably never run (its counter read zero) dying identically to one
                 * whose had. Leave room.
                 */
                CHAR keyLine[1024];
                CHAR *keyCursor = keyLine;
                UINT index;
                lastKeyDump = nowTicks;
                keyCursor = LogPut(keyCursor, "KEYLAT msgq_ms[0,1,2,4,8,16,32,64+]=");
                for (index = 0; index < ARRAYSIZE(g_KeyMessageHistogram); ++index)
                {
                    keyCursor = LogPut(keyCursor, index ? "," : "");
                    keyCursor = LogHex(keyCursor, g_KeyMessageHistogram[index]);
                }
                keyCursor = LogPut(keyCursor, " n=");      keyCursor = LogHex(keyCursor, g_KeyMessageCount);
                keyCursor = LogPut(keyCursor, " max_ms="); keyCursor = LogHex(keyCursor, g_KeyMessageMaximumMs);
                keyCursor = LogPut(keyCursor, " || deliver_ms=");
                for (index = 0; index < ARRAYSIZE(g_KeyDeliveryHistogram); ++index)
                {
                    keyCursor = LogPut(keyCursor, index ? "," : "");
                    keyCursor = LogHex(keyCursor, g_KeyDeliveryHistogram[index]);
                }
                keyCursor = LogPut(keyCursor, " n=");      keyCursor = LogHex(keyCursor, g_KeyDeliveryCount);
                keyCursor = LogPut(keyCursor, " max_ms="); keyCursor = LogHex(keyCursor, g_KeyDeliveryMaximumMs);
                keyCursor = LogPut(keyCursor, " || MOUSE raw_ok="); keyCursor = LogHex(keyCursor, (DWORD)g_MouseRawOk);
                keyCursor = LogPut(keyCursor, " wm_input=");  keyCursor = LogHex(keyCursor, g_MouseWmInput);
                keyCursor = LogPut(keyCursor, " abs_pkts=");  keyCursor = LogHex(keyCursor, g_MouseRawAbsolute);
                keyCursor = LogPut(keyCursor, " totx=");      keyCursor = LogHex(keyCursor, (DWORD)g_MouseRawTotalX);
                keyCursor = LogPut(keyCursor, " toty=");      keyCursor = LogHex(keyCursor, (DWORD)g_MouseRawTotalY);
                keyCursor = LogPut(keyCursor, " i33[0]=");    keyCursor = LogHex(keyCursor, g_MouseI33[0]);
                keyCursor = LogPut(keyCursor, " i33[3]=");    keyCursor = LogHex(keyCursor, g_MouseI33[I33_FN_GET_POSITION]);
                keyCursor = LogPut(keyCursor, " i33[B]=");    keyCursor = LogHex(keyCursor, g_MouseI33[I33_FN_READ_MOTION]);
                keyCursor = LogPut(keyCursor, " i33[1]=");    keyCursor = LogHex(keyCursor, g_MouseI33[1]);
                keyCursor = LogPut(keyCursor, " i33oth=");    keyCursor = LogHex(keyCursor, g_MouseI33Other);
                keyCursor = LogPut(keyCursor, " captured=");  keyCursor = LogHex(keyCursor, (DWORD)g_Captured);
                keyCursor = LogPut(keyCursor, " simint_unh="); keyCursor = LogHex(keyCursor, g_SimIntUnhandled);
                { UINT vector;
                for (vector = 0; vector < ARRAYSIZE(g_SimIntVector); ++vector) if (g_SimIntVector[vector])
                {
                      keyCursor = LogPut(keyCursor, " v"); keyCursor = LogHexByte(keyCursor, (BYTE)vector);
                      keyCursor = LogPut(keyCursor, "x");
                      keyCursor = LogHex(keyCursor, g_SimIntVector[vector]); } }
                keyCursor = LogPut(keyCursor, " || ui_gap_us="); keyCursor = LogHex(keyCursor, g_UiGapMicroseconds);
                keyCursor = LogPut(keyCursor, " lk_wait_us="); keyCursor = LogHex(keyCursor, g_LockWaitMicroseconds);
                keyCursor = LogPut(keyCursor, " in_first="); keyCursor = LogHex(keyCursor, g_UiInputFirst);
                keyCursor = LogPut(keyCursor, " || IRQ1GATE checks="); keyCursor = LogHex(keyCursor, g_Irq1Checks);
                keyCursor = LogPut(keyCursor, " no_if=");   keyCursor = LogHex(keyCursor, g_Irq1NoIf);
                keyCursor = LogPut(keyCursor, " in_08h=");  keyCursor = LogHex(keyCursor, g_Irq1In08);
                keyCursor = LogPut(keyCursor, " in_09h=");  keyCursor = LogHex(keyCursor, g_Irq1In09);
                keyCursor = LogPut(keyCursor, " injected="); keyCursor = LogHex(keyCursor, g_Irq1Injected);
                keyCursor = LogPut(keyCursor, " async_inj="); keyCursor = LogHex(keyCursor, g_Irq1AsyncInjected);
                keyCursor = LogPut(keyCursor, " retries=");   keyCursor = LogHex(keyCursor, g_Irq1AsyncRetry);
                keyCursor = LogPut(keyCursor, "\r\n");
                LogAppend(LOG_PATH, keyLine, keyCursor);
            } }
        /* THE INT 33h DETAIL, ON ITS OWN GATE AND ITS OWN LINES:
         * Two rules learned the hard way, both of them here on purpose:
         * 1. GATE IT ON WHAT IT MEASURES. The block above only runs once a KEY has
         *  been seen, which silently disables it for a run where nobody types --
         *  and a headless Doom attract run is exactly that. This one reports as
         *  soon as an INT 33h call has arrived, which is its own subject.
         * 2. ONE LINE PER SITE, not one line for everything. The line above smashed
         *  the UI thread's stack at 379 bytes in a 384-byte buffer; a dozen sites
         *  with a 12-byte context dump each is several times that. Bounded work
         *  per line, and the buffer is sized for the worst case with room over.
         */
        {   static DWORD lastMouseDump;
            DWORD nowTicks = GetTickCount();
            if (g_MouseI33SiteCount && (DWORD)(nowTicks - lastMouseDump) >= HOST_DUMP_INTERVAL_MS)
            {
                CHAR mouseLine[768];
                CHAR *mouseCursor = mouseLine;
                UINT index;
                lastMouseDump = nowTicks;
                mouseCursor = LogPut(mouseCursor, "MOUSEI33 ax:");
                for (index = 0; index < I33_AXN && g_MouseI33Ax[index].Count; ++index)
                {
                    mouseCursor = LogPut(mouseCursor, " "); mouseCursor = LogHexByte(mouseCursor, (g_MouseI33Ax[index].Ax >> BYTE_SHIFT) & BYTE_MASK);
                    mouseCursor = LogHexByte(mouseCursor, g_MouseI33Ax[index].Ax & BYTE_MASK);
                    mouseCursor = LogPut(mouseCursor, "x"); mouseCursor = LogHex(mouseCursor, g_MouseI33Ax[index].Count);
                }
                mouseCursor = LogPut(mouseCursor, " ax_ovf="); mouseCursor = LogHex(mouseCursor, g_MouseI33AxOverflow);
                mouseCursor = LogPut(mouseCursor, " sites="); mouseCursor = LogHex(mouseCursor, g_MouseI33SiteCount);
                mouseCursor = LogPut(mouseCursor, " site_ovf="); mouseCursor = LogHex(mouseCursor, g_MouseI33SiteOverflow);
                /* Auto-capture, so a headless run can show it. want=1 fired=0 is the
                 * window not being in the foreground, which is deliberate.
                 */
                mouseCursor = LogPut(mouseCursor, " autocap_want="); mouseCursor = LogHex(mouseCursor, (DWORD)g_MouseWantCapture);
                mouseCursor = LogPut(mouseCursor, " fired="); mouseCursor = LogHex(mouseCursor, g_MouseAutoCaptureFired);
                mouseCursor = LogPut(mouseCursor, " captured="); mouseCursor = LogHex(mouseCursor, (DWORD)g_Captured);
                /* THE s64 EVIDENCE, AND EACH FIELD ANSWERS ONE QUESTION (Importance = 1):
                 * edges  -- did the HOST see any button transitions at all? 0 with
                 *         a user who was clicking means the fault is above INT
                 *         33h (focus, capture, the window), not in the driver.
                 * p0/r0  -- left-button presses/releases WAITING to be collected.
                 *         These drain on read, so a guest that polls 05h/06h
                 *         keeps them near 0; a guest that never asks lets them
                 *         pile up, and THAT is the discriminator between "we
                 *         never saw the click" and "the guest never asked".
                 * evt    -- 0Ch/14h handler installs. NON-ZERO IS A KNOWN GAP: we
                 *         store the handler and never call it, so a guest that
                 *         waits on its callback will sit there. Named, not
                 *         silent -- see the note on g_MouseEventMask.
                 * unimpl -- calls that reached `default:`. Should be 0 now that
                 *         every documented function is handled; anything else
                 *         is either exotic or a mis-patched CD 33 site.
                 * xsh    -- the virtual-coordinate shift (1 in a 320-wide mode).
                 */
                mouseCursor = LogPut(mouseCursor, " edges="); mouseCursor = LogHex(mouseCursor, g_MouseEdges);
                mouseCursor = LogPut(mouseCursor, " p0=");    mouseCursor = LogHex(mouseCursor, (DWORD)g_MousePressCount[0]);
                mouseCursor = LogPut(mouseCursor, " r0=");    mouseCursor = LogHex(mouseCursor, (DWORD)g_MouseReleaseCount[0]);
                mouseCursor = LogPut(mouseCursor, " evt=");   mouseCursor = LogHex(mouseCursor, g_MouseEventInstalls);
                mouseCursor = LogPut(mouseCursor, " cb_inj=");  mouseCursor = LogHex(mouseCursor, g_MouseCallbackInjected);
                mouseCursor = LogPut(mouseCursor, " cb_done="); mouseCursor = LogHex(mouseCursor, g_MouseCallbackDone);
                mouseCursor = LogPut(mouseCursor, " cb_lost="); mouseCursor = LogHex(mouseCursor, g_MouseCallbackLost);
                mouseCursor = LogPut(mouseCursor, " cb_pm=");   mouseCursor = LogHex(mouseCursor, g_MouseCallbackPm);
                mouseCursor = LogPut(mouseCursor, " cb_stray="); mouseCursor = LogHex(mouseCursor, g_MouseCallbackStray);
                mouseCursor = LogPut(mouseCursor, " cb_why[fly,none,nohdl,stub,if,clob]=");
                {
                    INT reason;
                    for (reason = 0; reason < ARRAYSIZE(g_MouseCallbackWhy); ++reason)
                    {
                        mouseCursor = LogHex(mouseCursor, g_MouseCallbackWhy[reason]);
                        mouseCursor = LogPut(mouseCursor, reason < ARRAYSIZE(g_MouseCallbackWhy) - 1 ? "," : "");
                    }
                }
                mouseCursor = LogPut(mouseCursor, " raised="); mouseCursor = LogHex(mouseCursor, g_MouseEventRaised);
                mouseCursor = LogPut(mouseCursor, " mask=0x");  mouseCursor = LogHex(mouseCursor, (DWORD)g_MouseEventMask);
                mouseCursor = LogPut(mouseCursor, " hdl=0x");   mouseCursor = LogHex(mouseCursor, (DWORD)g_MouseEventSegment);
                mouseCursor = LogPut(mouseCursor, ":0x");       mouseCursor = LogHex(mouseCursor, (DWORD)g_MouseEventOffset);
                mouseCursor = LogPut(mouseCursor, " shape="); mouseCursor = LogHex(mouseCursor, g_MouseShapeSets);
                /* #264/#265: is a guest bitmap the pointer (gc=1), how many 09h pointers
                 * were unreadable, events routed to an 18h handler, 2Bh-34h calls.
                 */
                mouseCursor = LogPut(mouseCursor, " gc=");    mouseCursor = LogHex(mouseCursor, (DWORD)g_MouseGraphicsCursorDefined);
                mouseCursor = LogPut(mouseCursor, " gcbad="); mouseCursor = LogHex(mouseCursor, g_MouseGraphicsCursorBadPointer);
                mouseCursor = LogPut(mouseCursor, " alt=");   mouseCursor = LogHex(mouseCursor, g_MouseAltCalls);
                mouseCursor = LogPut(mouseCursor, " acc=");   mouseCursor = LogHex(mouseCursor, g_MouseAccelerationCalls);
                mouseCursor = LogPut(mouseCursor, " badptr=");mouseCursor = LogHex(mouseCursor, g_MouseStateBadPointer);
                mouseCursor = LogPut(mouseCursor, " unimpl=");mouseCursor = LogHex(mouseCursor, g_MouseI33Unimplemented);
                mouseCursor = LogPut(mouseCursor, " xsh=");   mouseCursor = LogHex(mouseCursor, (DWORD)I33XShift());
                /* THE VIRTUAL SCREEN THIS DRIVER IS ACTUALLY USING. Without these,
                 * a coordinate that comes back wrong is indistinguishable from a
                 * text-mode test that quietly evaluated false, and the difference
                 * is which file the bug is in. text= is I33Text()'s verdict, not
                 * mkind, because that verdict is what every coordinate call uses.
                 */
                mouseCursor = LogPut(mouseCursor, " text=");  mouseCursor = LogHex(mouseCursor, (DWORD)I33Text());
                mouseCursor = LogPut(mouseCursor, " mkind="); mouseCursor = LogHex(mouseCursor, (DWORD)g_Video.ModeKind);
                mouseCursor = LogPut(mouseCursor, " vesa=");  mouseCursor = LogHex(mouseCursor, (DWORD)g_Video.IsVesa);
                mouseCursor = LogPut(mouseCursor, " fh=");    mouseCursor = LogHex(mouseCursor, (DWORD)g_Video.Frame.Height);
                mouseCursor = LogPut(mouseCursor, " vmaxy="); mouseCursor = LogHex(mouseCursor, (DWORD)I33VirtualMaximumY());
                mouseCursor = LogPut(mouseCursor, " msy=");   mouseCursor = LogHex(mouseCursor, (DWORD)g_MouseY);
                mouseCursor = LogPut(mouseCursor, "\r\n");
                LogAppend(LOG_PATH, mouseLine, mouseCursor);
                for (index = 0; index < g_MouseI33SiteCount && index < I33_SITEN; ++index)
                {
                    CHAR siteLine[256];
                    CHAR *siteCursor = siteLine;
                    siteCursor = LogPut(siteCursor, "MOUSEI33 site src=");  siteCursor = LogHexByte(siteCursor, g_MouseI33Site[index].Source);
                    siteCursor = LogPut(siteCursor, " lin=0x");   siteCursor = LogHex(siteCursor, g_MouseI33Site[index].Linear);
                    siteCursor = LogPut(siteCursor, " cs=0x");    siteCursor = LogHex(siteCursor, g_MouseI33Site[index].Cs);
                    siteCursor = LogPut(siteCursor, " eip=0x");   siteCursor = LogHex(siteCursor, g_MouseI33Site[index].Eip);
                    siteCursor = LogPut(siteCursor, " ax0=0x");   siteCursor = LogHex(siteCursor, g_MouseI33Site[index].Ax);
                    siteCursor = LogPut(siteCursor, " n=");       siteCursor = LogHex(siteCursor, g_MouseI33Site[index].Count);
                    siteCursor = LogPut(siteCursor, " ctx[-4]="); siteCursor = LogDump(siteCursor, g_MouseI33Site[index].Context, sizeof g_MouseI33Site[index].Context);
                    siteCursor = LogPut(siteCursor, "\r\n");
                    LogAppend(LOG_PATH, siteLine, siteCursor);
                }
            } }
        /* THE GUEST ASKED FOR THE MOUSE: TAKE IT (Importance = 1):
         * Raised on the exec thread by MouseInt33 and performed here, because
         * ClipCursor / SetWindowsHookEx / SetCursor belong to the window's own
         * thread. Latched, so this fires once per program and Win+F10 stays the
         * last word on it -- see the note on g_MouseWantCapture.
         *
         * [CAUTION]: NOT while the window is in the background: grabbing the pointer out of
         * whatever the user is actually doing, because a DOS program in another
         * window polled its mouse, is exactly the behaviour that makes capture
         * feel like something being done TO you.
         */
        if (InterlockedExchange(&g_MouseWantRelease, 0) && g_Captured)
            InputCaptureSet(window, FALSE);      /* the program that owned it has exited */
        if (CaptureAllowed() && !g_MouseAutoCaptureDone && !g_Captured   /* #136: not seamless */
            && GetForegroundWindow() == window)
        {
            InterlockedExchange(&g_MouseAutoCaptureDone, 1);
            ++g_MouseAutoCaptureFired;
            InputCaptureSet(window, TRUE);
        }
        StatusUpdate();          /* program name / width / mode / capture, on the UI thread */
        /* Drive the PIT from REAL elapsed time so the BIOS tick (0040:006C) and
         * INT 1Ah track wall-clock regardless of WM_TIMER jitter; clamp after a
         * stall so we don't flood a catch-up burst of ticks.
         */
        /* The frame loop no longer OWNS the PIT (that is HostPitSync's shared clock), but
         * it must still drive it: if only the exec thread advanced time, a guest spinning in
         * its own code would wait forever for a clock that only ticks when it traps -- which
         * is exactly the deadlock the first cut of this produced (async_bail=497,
         * async_inj=0, guest frozen 25 s). Ticking here is also what lets the async injector
         * preempt, since this thread is not the one stuck inside VdmStartExecution.
         */
        /* BEFORE ANY LOCK. If this gap is large while lk_wait stays small, the UI
         * thread is not running at all and no amount of lock-splitting will help.
         */
        {   static LARGE_INTEGER previous;
            LARGE_INTEGER numerator;
            QueryPerformanceCounter(&numerator);
            if (previous.QuadPart)
            {
                UINT32 gap = QpcMicroseconds(numerator.QuadPart - previous.QuadPart);
                if (gap > g_UiGapMicroseconds)
                    g_UiGapMicroseconds = gap;
            }
            previous = numerator; }
        g_Pit.FrameMicroseconds = 0;
        HostPitSync();
        HostKeyTypematic();       /* pumped from BOTH threads, like the PIT */
        HostKeyPresent();
        HOST_LOCK();
        VddBusFrame(&g_Bus);          /* tick PIT + render into g_Video.frame */
        /* THE REAL PC SPEAKER, SAMPLED HERE AND DRIVEN BELOW:
         * The gate and the tone are read UNDER the lock, because the exec thread
         * writes both; the driver call happens OUTSIDE it, because a
         * DeviceIoControl held across the bus lock would stall the guest and the
         * audio pump behind a kernel transition. This tick is 5 ms, which is
         * finer than any tone a human hears as a separate note, and PcSpeakerSet()
         * sends nothing unless the tone actually changed -- so a guest that is
         * not beeping costs one comparison per tick.
         */
        if (g_SpeakerReal)
            g_SpeakerRealHz = VddSpeakerIsActive(&g_Speaker) ? VddSpeakerHz(&g_Speaker) : 0;
        /* PRESENT IN PHASE WITH THE GUEST'S FRAME, not on our own timer.
         * This tick used to run at 30 Hz and snapshot whenever it happened to fire.
         * Once the guest was correctly paced to 60/70 Hz that meant sampling once
         * per TWO of its frames at an arbitrary phase -- so a program that erases an
         * object and redraws it one pixel along (BOUNCEBX) got caught between the
         * two about half the time, and the object was simply missing from that
         * frame. That is the residual tearing left after the 0x3DA fix, and it was
         * ours, not the guest's: its frame rate measured 59.9 Hz.
         * Now the timer runs fast and we snapshot only when the emulated CRT is in
         * the tail of its active period, i.e. the guest has finished drawing and is
         * parked in its retrace wait. The staleness fallback guarantees we still
         * present if the phase window keeps being missed, so a guest that never
         * touches 0x3DA is unaffected.
         */
        {   static DWORD lastPresent = 0;
            DWORD nowTicks = GetTickCount();
            INT stale = (DWORD)(nowTicks - lastPresent) >= VID_PRESENT_STALE_MS;
            /* Auto: the hook owns the cadence; the timer presents only when it has
             * gone quiet (a guest that never polls the retrace), never on top of it --
             * the first cut presented 112 frames twice in 30 s that way.
             */
            int timerOk = g_UiTickMinimumMs == UITICK_AUTO /* stays int: INT here moves the compiled code */
                           ? ((DWORD)(nowTicks - lastPresent) >= UI_PRESENT_STALE_FRAMES * (VddVideoFrameUs(&g_Video) / MICROSECONDS_PER_MILLISECOND_U))
                           : (VddVideoIsPresentReady(&g_Video) || stale);
            if (g_UiForced || timerOk)
            {
                lastPresent = nowTicks;
                if (g_UiForced)
                    ++g_UiHookPresents;
                else
                    ++g_UiTimerPresents;
                /* THE DRIVER CURSOR. In a text mode it is not a sprite at all: the real
                 * driver inverts the character cell under the pointer (0Ah masks), and
                 * stamping a 16x16 arrow into a text frame is what "a graphical mouse
                 * cursor over a text interface" was. st->fb is re-rendered from the text
                 * buffer every present, so this one may be drawn into the frame.
                 */
                INT msText = (g_Video.ModeKind == VIDEO_KIND_TEXT && !g_Video.IsVesa);
                if (g_MouseHidden == 0 && msText && g_Video.Frame.BitsPerPixel == VIDEO_BPP_INDEXED && g_Video.Frame.Pixels)
                {
                    INT clientHeight = g_Video.CellHeight ? g_Video.CellHeight : VIDEO_CELL_HEIGHT;
                    VddVideoTextCursor(&g_Video, (INT)(g_MouseX / VIDEO_CELL_WIDTH), (INT)(g_MouseY / clientHeight),
                                          (WORD)g_MouseTextCursorAnd, (WORD)g_MouseTextCursorXor);
                }
                VddVideoFrameTouch(&g_Video);               /* raster-split state + frame no. */
                g_PresentDdraw.IsModeVesa = g_Video.IsVesa;             /* #228: Auto aspect needs it */
                PresentDdrawSnapshot(&g_PresentDdraw, &g_Video.Frame); /* consistent copy UNDER lock */
                /* -- THE GRAPHICS CURSOR GOES ON THE SNAPSHOT, NEVER ON THE FRAME. (#264)
                 * It used to be stamped into g_Video.frame.pixels before the snapshot --
                 * and in mode 13h that pointer IS st->vmem, the guest's own A0000
                 * aperture (vid_frame: "vmem is the FB"); in an 8-bpp VESA mode it is
                 * the guest's banked/LFB copy. So every present with the pointer
                 * shown WROTE THE ARROW INTO GUEST VIDEO MEMORY and never restored it:
                 * droppings wherever a 13h guest did not repaint, and a guest reading
                 * its screen back (a paint program's save-under, a GET) read our arrow.
                 * The planar/CGA/mode-Y frames are host buffers (st->fb), which is why
                 * only the linear modes ever showed it -- the comment above
                 * OverlayCursor said "the frame is re-rendered from VRAM every tick",
                 * true for those and not for 13h.
                 * - OVERLAY, NOT VRAM -- A DECISION, AND WHY. A real driver draws into
                 *   video memory (saving and restoring what was under it at every move),
                 *   and a guest that reads VRAM back while the pointer is shown sees it.
                 *   Doing the same from the host would race a guest that runs natively
                 *   and writes A0000 directly: a save-under taken between two of its
                 *   writes restores stale pixels. Real programs hide the pointer (02h /
                 *   10h) around their drawing precisely because of this; a host overlay
                 *   needs none of it, so it is drawn here, on the presenter's private
                 *   copy, which the guest can never see. p_mouse3 (`i33.09.13h.vram.*`)
                 *   measures whether the oracles' drivers write VRAM, so the gap is
                 *   recorded rather than assumed (docs/inventory/mouse.md, 09h).
                 */
                if (g_MouseHidden == 0 && !msText && g_PresentDdraw.IsSnapshotValid && g_PresentDdraw.SnapshotBpp == VIDEO_BPP_INDEXED)
                    MouseDrawGraphicsCursor(g_PresentDdraw.Snapshot, g_PresentDdraw.SnapshotWidth, g_PresentDdraw.SnapshotHeight, g_PresentDdraw.SnapshotWidth);
                HOST_UNLOCK();
                HostFollowFrame(window);                        /* #325: the window follows the mode */
                /* FRAME SKIP DROPS THE BLIT, NOT THE SNAPSHOT:
                 * The snapshot is what keeps our copy of the frame current, and
                 * WM_PAINT blits that copy on every expose -- so skipping the
                 * snapshot too would leave a stale picture on screen after a
                 * window move, which is a bug, not a setting. Skipping only the
                 * blit gives back exactly what the blit costs.
                 */
                {   static UINT frameSkipCounter;
                    if (g_FrameSkip <= 0 || (frameSkipCounter++ % (UINT)(g_FrameSkip + 1)) == 0)
                        PresentDdrawPresent(&g_PresentDdraw);  /* vsync'd blit OUTSIDE the lock */
                }
            }
            else
            {
                HOST_UNLOCK();  /* not our phase: keep the last frame up */
            }
            g_UiForced = 0;
        }
        if (!g_AutoFullscreenDone)                   /* "Graphics only": the first graphics mode */
            HostAutoFullscreenConsider(g_Window, g_Video.ModeKind != VIDEO_KIND_TEXT || g_Video.IsVesa);
        if (g_SpeakerReal && !g_PauseWant)
            PcSpeakerSet(&g_PcSpeaker, g_SpeakerRealHz);                                  /* outside the lock */
        /* Headless remote visual capture (session-9): the host screenshots ITSELF to
         * C:\ntvdmex\shotNN.bmp every ~2s so a graphical run (Skyroads, the PM demos)
         * is verifiable off the SMB share -- VNC capture is dead on the real box. The
         * snapshot is owned by this UI thread, so no extra lock is needed; capped at
         * 40 frames so a long run never fills the disk. rt.bat copies shot*.bmp off.
         */
        if (g_Capture)
        {
            static UINT capTick = 0;
            static UINT captureSequence = 0;
            static INT capFailed = 0;
            /* - 2 s IS FAR TOO SLOW TO CATCH A MODE SWITCH. Doom runs about ten
             * seconds and sets mode 13h in the last fraction of it, so a 2 s cadence
             * caught exactly ONE frame -- blank text mode, two distinct colours. At
             * ~300 ms the 40-frame budget spans a whole run and straddles the switch,
             * which is the only way to SEE what the guest drew: the rig has no VNC and
             * `screendump` is QEMU-only, so these BMPs are the only eyes we have.
             */
            /* - THE CADENCE IS A KNOB, BECAUSE 40 FRAMES x 300 ms ONLY SEES THE FIRST
             * TWELVE SECONDS. A scripted keypress run presses its first key at 14 s
             * -- deliberately, so the game has reached its demo -- and every shot had
             * already been taken by then. The run looked like "the keys did nothing"
             * when what actually happened is that nobody was looking. capture.flag's
             * CONTENTS are the period in milliseconds now; empty keeps the 300 ms
             * default, and 1100 spans a whole 45 s headless run.
             */
            if (GetTickCount() - g_CaptureStart >= g_CaptureDelayMs        /* #58 */
                && (capTick++ % (g_CaptureMs / VID_PRESENT_TICK_MS + 1)) == 0 && captureSequence < CAPTURE_SHOTS_MAX)
            {
                /* -- [WARNING] THESE INDICES WERE HARDCODED, AND THE PATH MOVED UNDER THEM.
                 * They were 15 and 16, which addressed the two digits back when this
                 * was `C:\ntvdmex\shot00.bmp`. The s61 one-folder move made it
                 * `C:\Documents and Settings\...\out\shot00.bmp`, where 15 and 16
                 * land in "Documents and" -- so every shot was written to
                 * `C:\Documents an07 Settings\...`, a directory that does not exist.
                 *
                 * [CAUTION]: AND IT FAILED IN SILENCE FOR THREE SESSIONS: save_bmp returns <0,
                 * cap_seq simply does not advance, and nothing is logged. The rig has
                 * no VNC, so these BMPs are the only eyes on a graphical run --
                 * "the guest drew nothing" and "we could not write the file" looked
                 * identical, which is the exact failure this codebase keeps paying
                 * for. Derive the offset from the string so the next path change
                 * cannot do it again, and SAY SO when a write fails.
                 */
                CHAR name[] = HOST_SHOT_NAME;
                PCSTR path;
                name[CAPTURE_NAME_DIGITS] = (CHAR)('0' + (captureSequence / DECIMAL_RADIX) % DECIMAL_RADIX);
                name[CAPTURE_NAME_DIGITS + 1] = (CHAR)('0' + captureSequence % DECIMAL_RADIX);
                /* -- ...AND THE SAME FRAME AS TEXT, when asked. A picture cannot say
                 * whether a missing line was never written or merely never drawn,
                 * and that distinction is where three wrong guesses went on
                 * QBasic's empty file list. Gated: no run pays for it unaltered.
                 */
                if (g_TextDump && g_Video.ModeKind == VIDEO_KIND_TEXT && !g_Video.IsVesa)
                {
                    static CHAR textSnapshot[8192];
                    CHAR shotName[] = HOST_SHOT_TEXT_NAME;
                    INT textLength;
                    shotName[CAPTURE_NAME_DIGITS] = name[CAPTURE_NAME_DIGITS];
                    shotName[CAPTURE_NAME_DIGITS + 1] = name[CAPTURE_NAME_DIGITS + 1];
                    textLength = VddVideoTextSnapshot(&g_Video, textSnapshot, sizeof textSnapshot);
                    if (textLength > 0)
                    {
                        HANDLE textFile = CreateFileA(OUT_(shotName), GENERIC_WRITE, 0, NULL,
                                                CREATE_ALWAYS, 0, NULL);
                        if (textFile != INVALID_HANDLE_VALUE)
                        {
                            DWORD bytesWritten = 0;
                            WriteFile(textFile, textSnapshot, (DWORD)textLength, &bytesWritten, NULL);
                            CloseHandle(textFile);
                        }
                    }
                }
                path = OUT_(name);
                if (PresentDdrawSaveBmp(&g_PresentDdraw, path) == 0)
                {
                    ++captureSequence;
                    if (GetFileAttributesA(PLANEDUMP_FLAG) != INVALID_FILE_ATTRIBUTES)
                        PlanesDumpBeside(path);
                }
                else if (!capFailed)
                {
                    CHAR errorLine[320];
                    CHAR *errorCursor = errorLine;
                    capFailed = 1;
                    errorCursor = LogPut(errorCursor, "CAPTURE: save_bmp FAILED for ");
                    errorCursor = LogPut(errorCursor, path);
                    errorCursor = LogPut(errorCursor, " -- snap_valid=");
                    errorCursor = LogDecimal(errorCursor, (UINT)g_PresentDdraw.IsSnapshotValid);
                    errorCursor = LogPut(errorCursor, " w=");  errorCursor = LogDecimal(errorCursor, (UINT)g_PresentDdraw.SnapshotWidth);
                    errorCursor = LogPut(errorCursor, " h=");  errorCursor = LogDecimal(errorCursor, (UINT)g_PresentDdraw.SnapshotHeight);
                    errorCursor = LogPut(errorCursor, " (reported once)\r\n");
                    LogAppend(LOG_PATH, errorLine, errorCursor);
                }
            }
        }
        return 0;

    case WM_ERASEBKGND:
        return 1;                       /* we own the whole client -> no white erase */

    case WM_SETCURSOR:
        /* RULE 3: CAPTURED, THERE IS NO POINTER ANYWHERE ON THIS WINDOW:
         * The old test was `(HWND)wp == h && HTCLIENT`, so the status bar -- a
         * CHILD, which forwards its own hit-test here with wp set to ITSELF --
         * kept its arrow while the guest owned the mouse. ClipCursor confines the
         * pointer to our client rect, so that arrow had nowhere to go but sit on
         * top of a captured game. Captured or fullscreen: hide it for every
         * hit-test on every part of this window, ours or a child's.
         * Otherwise fall back to the per-area decision, which is the only place
         * the Hide-over-video setting applies.
         */
        if (g_Captured)
        {
            SetCursor(NULL);
            return TRUE;
        }
        /* #218: otherwise only the idle rule hides it, and only over the video. */
        if ((HWND)wParam == window && LOWORD(lParam) == HTCLIENT
            && (g_CursorIdle || g_HostCursorMode == HOSTCUR_NEVER))
        {
            POINT client;
            if (GetCursorPos(&client))
            {
                ScreenToClient(window, &client);
                if (!HostCursorVisibleAt(IsPointOverVideo(window, client.x, client.y)))
                {
                    SetCursor(NULL);
                    return TRUE;
                }
            }
        }
        /* Fullscreen has no frame, so the default would be whatever class cursor is
         * there; give it the plain arrow a usable pointer means.
         */
        if (g_PresentDdraw.IsFullscreen && LOWORD(lParam) == HTCLIENT)
        {
            SetCursor(LoadCursorA(NULL, IDC_ARROW));
            return TRUE;
        }
        break;

    case WM_PAINT:                       /* re-blit the last snapshot on expose/move */
    {
        PAINTSTRUCT paint;
        BeginPaint(window, &paint);
        PresentDdrawPresent(&g_PresentDdraw);
        EndPaint(window, &paint);
        return 0; }

    /* THE ASPECT LOCK, WHILE THE MOUSE IS STILL DOWN (Importance = 1):
     * WM_SIZING hands us the rectangle Windows is ABOUT to use, so correcting it
     * here makes the window snap to the ratio as it is dragged rather than jumping
     * to it on release. Which side to correct depends on which handle is held: a
     * side handle drives the other dimension, a corner drives height from width.
     *
     * [CAUTION]: MOVE THE EDGE THE USER IS NOT HOLDING. Adjusting `right` while they drag the
     * left handle makes the window walk across the desktop -- correct in size and
     * visibly wrong to use.
     */
    case WM_SIZING:
        if (!g_PresentDdraw.IsFullscreen)
        {
            RECT *sizingRect = (RECT *)lParam;
            INT numerator;
            INT denominator;
            INT extraWidth;
            INT extraHeight;
            INT viewWidth;
            INT viewHeight;
            INT sourceWidth;
            INT sourceHeight;
            HostFrameExtra(window, &extraWidth, &extraHeight);
            HostFrameSize(&sourceWidth, &sourceHeight);
            viewWidth = (sizingRect->right - sizingRect->left) - extraWidth;
            viewHeight = (sizingRect->bottom - sizingRect->top) - extraHeight;
            if (viewWidth < 1)
                viewWidth = 1;
            if (viewHeight < 1)
                viewHeight = 1;
            if (PresentIsNative((INT)g_Settings.Values[SET_ASPECT]))
            {
                /* -- #325: NATIVE SNAPS TO WHOLE MULTIPLES while the frame is dragged --
                 * the nearest k to where the held edge is, never below 1x -- and that
                 * k becomes the window's scale, so the next mode change keeps it.
                 */
                INT scale = (wParam == WMSZ_TOP || wParam == WMSZ_BOTTOM) ? (viewHeight + sourceHeight / 2) / sourceHeight
                                                              : (viewWidth + sourceWidth / 2) / sourceWidth;
                if (scale < 1)
                    scale = 1;
                PresentWindowPicture((INT)g_Settings.Values[SET_ASPECT], sourceWidth, sourceHeight, scale, &viewWidth, &viewHeight);
                g_ScaleFactor = g_ScaleWant = scale;
                g_FitDown = 0;
            }
            else
            {
                PresentTargetRatio((INT)g_Settings.Values[SET_ASPECT], sourceWidth, sourceHeight, &numerator, &denominator);
                switch (wParam)
                {
                case WMSZ_LEFT:
                case WMSZ_RIGHT:
                    viewHeight = (INT)((INT32)viewWidth * denominator / numerator);
                    break;   /* width drives height */

                case WMSZ_TOP:
                case WMSZ_BOTTOM:
                    viewWidth = (INT)((INT32)viewHeight * numerator / denominator);
                    break;   /* height drives width */

                default:
                    viewHeight = (INT)((INT32)viewWidth * denominator / numerator);
                    break;   /* a corner: width wins */
                }
            }
            if (wParam == WMSZ_LEFT || wParam == WMSZ_TOPLEFT || wParam == WMSZ_BOTTOMLEFT)
                sizingRect->left  = sizingRect->right - (viewWidth + extraWidth);
            else
                sizingRect->right = sizingRect->left  + (viewWidth + extraWidth);
            if (wParam == WMSZ_TOP || wParam == WMSZ_TOPLEFT || wParam == WMSZ_TOPRIGHT)
                sizingRect->top    = sizingRect->bottom - (viewHeight + extraHeight);
            else
                sizingRect->bottom = sizingRect->top    + (viewHeight + extraHeight);
            return TRUE;
        }
        break;

    /* #325: the floor is the picture at 1x (it was 640x480 on-aspect, which made 1x of a
     * 320x200 mode impossible) -- or, when 1x does not fit the screen, small enough that
     * the scaled-down window is reachable.
     */
    case WM_GETMINMAXINFO:
        if (g_Window && !g_PresentDdraw.IsFullscreen)
        {
            MINMAXINFO *minMax = (MINMAXINFO *)lParam;
            INT pictureWidth;
            INT pictureHeight;
            INT extraWidth;
            INT extraHeight;
            INT roomWidth;
            INT roomHeight;
            HostPicture(1, &pictureWidth, &pictureHeight);
            HostWorkRoom(&roomWidth, &roomHeight);
            if (pictureWidth > roomWidth || pictureHeight > roomHeight)
            {
                pictureWidth = HOST_MINIMUM_PICTURE_WIDTH;
                pictureHeight = HOST_MINIMUM_PICTURE_HEIGHT;
            }
            HostFrameExtra(window, &extraWidth, &extraHeight);
            minMax->ptMinTrackSize.x = pictureWidth + extraWidth;
            minMax->ptMinTrackSize.y = pictureHeight + extraHeight;
            return 0;
        }
        break;

    case WM_SIZE:
        if (g_Status)
        {
            SendMessageA(g_Status, WM_SIZE, 0, 0);            /* let it re-dock */
            g_StatusLeft[0] = 0;       /* force a re-cut; the last part follows the width */
            StatusUpdate();         /* re-partitioning blanks them; fill them again */
        }
        return 0;

    /* The one moment a menu's enable state is guaranteed to be current: the guest
     * can change video mode whenever it likes, so this is synced at open time
     * rather than at mode-set time.
     */
    case WM_INITMENUPOPUP:
        if ((HMENU)wParam == g_RecentMenu)
            MenuRecentFill();
        MenuSyncModal(window, (HMENU)wParam);
        return 0;

    case WM_COMMAND:
        /* EVERY MENU-BACKED SETTING, IN ONE PLACE (Importance = 1):
         * The dropdowns are contiguous RANGES, so they are handled before the
         * switch: find which range the id fell in and the offset IS the value.
         * One block for the CPU speed and all five display dropdowns, rather than
         * thirty near-identical cases that would each have to remember to apply
         * and re-tick.
         *
         * [CAUTION]: NOTHING HERE SAVES. A menu is for trying something; the Settings dialog
         * is for keeping it. See the note by g_SettingsDisk.
         *
         * [CAUTION]: AND THE OFFSET IS RANGE-CHECKED AGAINST THE SETTING'S OWN `hi`. The id
         * space reserves IDM_COMBO_SPAN per dropdown, which is more entries than
         * any list has -- so an id inside the span but past the end of the list
         * is not a value, and writing it would put the setting somewhere the
         * dialog cannot even display.
         */
        {   UINT commandId = (UINT)LOWORD(wParam);
            INT itemIndex;
            for (itemIndex = 0; itemIndex < MENU_COMBO_N; ++itemIndex)
            {
                UINT base = g_MenuCombos[itemIndex].Base;
                if (commandId >= base && commandId < base + IDM_COMBO_SPAN)
                {
                    DWORD value = (DWORD)(commandId - base);
                    if (value <= g_SetDefinitions[g_MenuCombos[itemIndex].IsSet].High)
                    {
                        g_Settings.Values[g_MenuCombos[itemIndex].IsSet] = value;
                        SettingsApplyLive(window);
                    }
                    return 0;
                }
            }
            for (itemIndex = 0; itemIndex < MENU_CHECK_N; ++itemIndex)
            {
                if (commandId == g_MenuChecks[itemIndex].Id)
                {
                    g_Settings.Values[g_MenuChecks[itemIndex].IsSet] = g_Settings.Values[g_MenuChecks[itemIndex].IsSet] ? 0u : 1u;
                    SettingsApplyLive(window);
                    return 0;
                }
            }
        }
        /* #153: Open Recent's items are a range, like the combos above. */
        if (LOWORD(wParam) >= IDM_RECENT_0 && LOWORD(wParam) < IDM_RECENT_0 + MRU_MAX)
        {
            CHAR list[MRU_MAX][MAX_PATH];
            INT numerator = MruLoad(list);
            INT itemIndex = LOWORD(wParam) - IDM_RECENT_0;
            if (itemIndex < numerator)
                OpenProgram(window, list[itemIndex]);
            return 0;
        }
        switch (LOWORD(wParam))
        {
        case IDM_FILE_EXIT:
            DestroyWindow(window);
        return 0;

        case IDM_FILE_OPEN:
            OpenProgramDialog(window);
        return 0;   /* #153 */

        /* #154: the text-mode Edit menu -- see g_MarkMode. */
        case IDM_EDIT_MARK:
            if (g_Video.ModeKind == VIDEO_KIND_TEXT)
            {
                if (g_Captured)
                    InputCaptureSet(window, FALSE);               /* the drag needs the pointer */
                g_MarkMode = 1;
                g_MarkDrag = 0;
                g_SelectionOn = 0;
                SelectionPublish();
                g_PresentDdraw.HintText  = HOST_MARK_HINT_TEXT;
                g_PresentDdraw.HintUntil = GetTickCount() + FULLSCREEN_HINT_MS;
            }
            return 0;

        case IDM_EDIT_SELECTALL:
            if (g_Video.ModeKind == VIDEO_KIND_TEXT)
            {
                g_SelectionColumn0 = 0;
                g_SelectionRow0 = 0;
                g_SelectionColumn1 = g_Video.Columns - 1;
                g_SelectionRow1 = g_Video.Rows - 1;
                g_SelectionOn = 1;
                g_MarkMode = 1;
                g_MarkDrag = 0;
                SelectionPublish();
            }
            return 0;

        case IDM_EDIT_COPY:
            TextCopy(window, TEXT_COPY_SELECTION);
        SelectionClear();
        return 0;

        case IDM_EDIT_COPYSCREEN:
            TextCopy(window, TEXT_COPY_SCREEN);
        return 0;

        case IDM_EDIT_PASTE:
            TextPaste(window);
        return 0;

        case IDM_FILE_CLOSEPROG:                       /* #152 -- see CloseProgramNow */
            if (!CloseProgramAvailable())
                return 0;                               /* greyed; belt and braces */
            if (g_WowLaunch) /* Win16: the same as Exit */
            {
                DestroyWindow(window);
                return 0;
            }
            if (g_Captured)
                InputCaptureSet(window, FALSE);               /* the shell does not own the mouse */
            InterlockedExchange(&g_CloseRequest, 1);
            return 0;

        case IDM_DISP_FULLSCREEN:
            HostFullscreenToggle(window);
        return 0;

        /* [CAUTION]: CONFIRM FIRST. Both of these change how EVERY DOS and Win16 program on
         * the machine starts, and both outlive this process -- an accidental
         * click on a menu is not consent for that.
         */
        case IDM_FILE_INSTALL:
        case IDM_FILE_UNINSTALL:
        {
            CHAR message[2048];
            INT want = (LOWORD(wParam) == IDM_FILE_INSTALL);
            INT isOk;
#define HOST_INSTALL_CONFIRM_TEXT "Make NTVDMEX this machine's virtual DOS machine?\n\n" \
                      "Every MS-DOS and 16-bit Windows program will then start " \
                      "through NTVDMEX instead of Microsoft's ntvdm.exe.\n\n" \
                      "This changes a machine-wide setting and needs Administrator. " \
                      "It is reversible from this menu."
#define HOST_UNINSTALL_CONFIRM_TEXT "Remove NTVDMEX from the launch path?\n\n" \
                      "This machine will go back to using its own ntvdm.exe for " \
                      "MS-DOS and 16-bit Windows programs."
            if (MessageBoxA(window, want ? HOST_INSTALL_CONFIRM_TEXT : HOST_UNINSTALL_CONFIRM_TEXT,
                            HOST_PRODUCT_NAME, MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
                return 0;
            message[0] = 0;
            isOk = InstallPerform(want, INSTALL_UNFORCED, message, sizeof message);
            MessageBoxA(window, message, HOST_PRODUCT_NAME,
                        MB_OK | (isOk ? MB_ICONINFORMATION : MB_ICONERROR));
            return 0; }

        case IDM_FILE_STATUS:
        {
            CHAR message[2048];
            message[0] = 0;
            InstallStatusText(message, sizeof message);
            MessageBoxA(window, message, HOST_PRODUCT_NAME, MB_OK | MB_ICONINFORMATION);
            return 0; }

        case IDM_FILE_SETTINGS:
            /* Modal, on the UI thread. The guest keeps running throughout -- it lives
             * on the exec thread, and the PIT is paced by its own thread -- so this
             * freezes the picture, not the machine.
             */
            DialogBoxParamA(GetModuleHandleA(NULL), MAKEINTRESOURCEA(IDD_SETTINGS),
                            window, SettingsDialogProcedure, 0);
            return 0;

        case IDM_INPUT_CAPTURE:
            InputCaptureSet(window, !g_Captured);
        return 0;

        case IDM_CAP_SHOT:
            HostScreenshot();
        return 0;

        case IDM_CAP_AUDIO:
            HostRecordToggle();
        return 0;              /* #155 */

        case IDM_CAP_FOLDER:
            HostOpenCaptureFolder();
        return 0;    /* #155 */

        /* THE SYSTEM ABOUT BOX, WHICH IS WHAT A PROGRAM OF THIS ERA USES (Importance = 1):
         * ShellAbout is the shared About dialog every Win16 and early Win32
         * application on this desktop puts behind Help > About -- Program
         * Manager's is this dialog -- so ours being a plain MessageBox was the
         * one place the host stopped looking like the thing it replaces. It also
         * gives us the pieces a MessageBox cannot: the Windows version and the
         * physical-memory and resource figures, filled in by the shell.
         *
         * [CAUTION]: szApp IS TWO FIELDS SEPARATED BY '#'. Before the hash goes in the TITLE
         * BAR (the shell prefixes "About "), after it on the first line of the
         * body. Passing a string with no hash puts the whole thing in both.
         *
         * [CAUTION]: AND THE SHELL PUTS ITS OWN BRANDING ON THE FIRST LINE, which is why the
         * product name is repeated in the body text rather than left to the
         * caption alone -- the caption is the only part we fully own.
         * - NOT YET SEEN ON HARDWARE: what XP renders here needs one look before
         *   this is called done.
         */
        case IDM_HELP_ABOUT:
#define HOST_ABOUT_TEXT "A from-scratch ntvdm.exe for Windows XP.\r\n" \
                        "MS-DOS on the real CPU in V86, and 16-bit Windows through " \
                        "the system's own krnl386."
            ShellAboutA(window, HOST_ABOUT_TITLE, HOST_ABOUT_TEXT,
                        LoadIconA(GetModuleHandleA(NULL), MAKEINTRESOURCEA(IDI_MAINICON)));
            return 0;

        /* [INFO]: The way back to a window that was never shown. Not a toggle: "hide it
         * again" is what the close button already means for a machine whose
         * icon is in the tray, and two ways to do one thing is how a UI starts
         * disagreeing with itself.
         */
        case IDM_TRAY_SHOW:
            ShowWindow(window, SW_SHOW);
            SetForegroundWindow(window);
            return 0;

        default:
            return 0;               /* IDM_STUB / not-yet-wired items: no-op */
        }

    /* THE TRAY ICON'S OWN MESSAGES (Importance = 1):
     * Right-click (or the keyboard's context key) opens the menu; double-click
     * shows the window, which is what a tray icon is expected to do everywhere
     * else on this desktop.
     */
    case WM_TRAY:
        if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU)
        {
            TrayMenu(window);
            return 0;
        }
        /* s88: double-click no longer shows the hidden Win16 machine window. */
        return 0;

    case WM_SYSKEYDOWN:                  /* F10 / Alt / Alt+key -- see InputCaptureSet */
        /* WIN+F10 IS THE HOST KEY:
         * It has to be handled HERE and not in WM_KEYDOWN, because F10 is a SYSTEM key
         * and only ever arrives as WM_SYSKEYDOWN -- binding it in WM_KEYDOWN is exactly
         * why the first attempt at capture silently never fired.
         * Why the Windows key: DOS predates it, so no guest asks for it, and it is not
         * a reserved XP shortcut. Scroll Lock is kept as an alternative below but must
         * not be the only one -- plenty of current keyboards no longer have the key.
         */
        /* [CAUTION]: Win+F10 IS GONE (s64). The release key is the WINDOWS KEY ALONE -- see
         * the capture rules above InputCaptureSet. A chord and a bare key for the
         * same job means the chord's second half arrives at the guest after the bare
         * key has already released, and the user asked for exactly one rule.
         */
        /* ALT+ENTER IS A HOST KEY EVEN WHILE CAPTURED. (s64, user report) (Importance = 1):
         * Reported as "Alt+Enter worked a few times in DOOM, and then stopped -- I
         * guess the game hooks the enter key". The game does not: the guard here was
         * `!g_Captured`, and Doom takes the mouse the moment it first polls INT 33h.
         * So Alt+Enter worked right up until the auto-grab fired and never again --
         * "a few times, then stopped" exactly.
         * - Alt+Enter is the one hotkey that must survive capture. It is the universal
         *   Windows binding for fullscreen, no DOS program asks for it (DOS predates
         *   the convention), and fullscreen is a HOST property of the window rather
         *   than anything the guest has an opinion about. F11 deliberately does NOT
         *   get the same treatment below -- that one is Doom's gamma key.
         */
        if (wParam == VK_RETURN)
        {
            HostFullscreenToggle(window);
            return 0;
        }
        /* Alt+F4 stays Windows' while uncaptured: there must always be a way to close
         * the window that does not require knowing a chord. Captured, it is the guest's.
         */
        if (wParam == VK_F4 && !g_Captured)
            break;
        KeyMessageNote();
        KeyPushMake(lParam);
        return 0;                        /* never let DefWindowProc open the menu bar */

    case WM_SYSKEYUP:
        if (wParam == VK_F4 && !g_Captured)
            break;
        KeyMessageNote();
        KeyPushBreak(lParam);
        return 0;

    case WM_SYSCHAR:
        return 0;                        /* swallow the menu-mnemonic beep */

    case WM_KILLFOCUS:
        InputCaptureSet(window, FALSE);         /* never strand the user in a captured window */
        HostReleaseModifiers();        /* ...nor the guest with Alt held forever */
        return 0;

    case WM_ENTERSIZEMOVE:
        g_InSizeMove = 1;
    break;

    case WM_EXITSIZEMOVE:                /* Windows' move/size loop leaves the cursor unclipped */
        g_InSizeMove = 0;
        CaptureClipApply(window);
        break;

    case WM_ACTIVATE:
        /* -- RULE 5 (s69, user spec): GAINING FOCUS RE-CAPTURES, for a guest that asked
         * for the mouse. The click-in-the-video path (below) already covers "click the
         * window"; this adds "focus the window" -- alt-tab back, or the click that
         * activated an unfocused window (WA_CLICKACTIVE), grabs the mouse straight
         * away. InputCaptureSet gates it on rule 1 (CaptureAllowed), so a guest
         * that never touched INT 33h is unaffected. WM_KILLFOCUS is the matching
         * release, so alt-tab away frees the pointer and alt-tab back takes it.
         *
         * [CAUTION]: This composes with the Windows-key release ONLY because that release now
         * suppresses the Start menu (see the WM_KEYDOWN handler): without suppression
         * the menu would steal focus and returning to the window would re-capture
         * instantly, making the release feel dead.
         */
        if (LOWORD(wParam) != WA_INACTIVE)
            InputCaptureSet(window, TRUE);
        HostPauseSet(LOWORD(wParam) == WA_INACTIVE);   /* #219 */
        break;                           /* let DefWindowProc do the focus bookkeeping */

    case WM_KEYDOWN:
        /* Our own Start-menu-suppression Ctrl tap (see the VK_LWIN handler) comes back
         * to us as a normal keystroke; drop it so it never reaches the guest.
         */
        if (GetMessageExtraInfo() == (LPARAM)HOST_INJECT_TAG)
            return 0;
        /* #154: while marking, Esc cancels and Enter copies -- the Windows console's own
         * keys -- and nothing typed reaches the guest until the mark is over.
         */
        if (g_MarkMode)
        {
            if (wParam == VK_ESCAPE)
                SelectionClear();
            else if (wParam == VK_RETURN)
            {
                if (g_SelectionOn)
                    TextCopy(window, TEXT_COPY_SELECTION);
                SelectionClear();
            }
            return 0;
        }
        KeyMessageNote();
        /* RULE 4: THE WINDOWS KEY ALONE RELEASES THE CAPTURE (Importance = 1):
         * The lineage, because each step was a real fix: Ctrl+F10 (broken twice over
         * -- F10 is a SYSTEM key and only ever arrives as WM_SYSKEYDOWN, so the chord
         * could never fire; and DOOM USES CTRL TO FIRE and every F-key besides), then
         * Win+F10 with Scroll Lock as an alternative, then Win+Click, and now this.
         * One key, no chord, and it only ever RELEASES -- it is not a toggle, because
         * the way back in is a click in the video (rule 5) and a key that did both
         * would fight that click. Win is the right key for the same reason Win+F10
         * was: DOS predates it, so no guest can ask for it, and a captured guest owns
         * every key that a guest CAN ask for.
         * Swallowed either way, so the guest never sees it.
         *
         * [CAUTION]: It is handled on the DOWN. Windows opens the Start menu on the UP, and by
         * then capture is already released and ClipCursor already cleared -- so even
         * in the case we cannot suppress (the low-level hook is off by default, see
         * InputCaptureSet) the user lands on a desktop with their mouse back,
         * which is what they were asking for by pressing it.
         */
        if (wParam == VK_LWIN || wParam == VK_RWIN)
        {
            if (g_Captured)
                InputCaptureSet(window, FALSE);
            /* SUPPRESS THE START MENU WITHOUT THE SYSTEM-WIDE HOOK. (s69, user ask) (Importance = 1):
             * The Windows equivalent of e.preventDefault() for a keystroke is a
             * WH_KEYBOARD_LL hook returning nonzero -- and we HAVE that (LowLevelKeyboardProcedure,
             * llkbd.txt) -- but it is off by default because that hook is system-wide
             * and jammed the rig twice (see InputCaptureSet). This is the safe
             * equivalent: Explorer opens the Start menu on the WIN key-UP only if no
             * other key was pressed while WIN was held, so -- WIN still down here --
             * inject ONE benign keystroke. Explorer then sees WIN+Ctrl, not a lone
             * WIN, and never opens the menu. A one-shot SendInput cannot stall the
             * keyboard the way a persistent hook can. Ctrl is inert on the desktop,
             * and we are UNCAPTURED by now so it never reaches the guest.
             *
             * [CAUTION]: DESKTOP BEHAVIOUR -- verify by hand; it cannot be tested headless.
             */
            { INPUT inputs[2];
              inputs[0].type = INPUT_KEYBOARD;
              inputs[0].ki.wVk = VK_CONTROL;
              inputs[0].ki.wScan = 0;
              inputs[0].ki.dwFlags = 0;
              inputs[0].ki.time = 0;
              inputs[0].ki.dwExtraInfo = HOST_INJECT_TAG;   /* so our own key handlers skip it */
              inputs[1] = inputs[0];
              inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
              SendInput(ARRAYSIZE(inputs), inputs, sizeof(INPUT)); }
            return 0;
        }
        /* [CAUTION]: Scroll Lock was the alternative capture toggle. Retired with Win+F10 for
         * the same reason: rule 4 says the release is the Windows key, and rule 1
         * says a guest that never hooked the mouse cannot capture at all -- so a
         * second toggle could only ever disagree with one of them.
         */
        /* - CAPTURED MEANS CAPTURED. Every other host hotkey stands down and the key goes
         * to the guest -- F11 is Doom's gamma, Ctrl+F5/F8 collide with its fire key.
         * Exclusivity that still eats keys is not exclusivity.
         */
        if (g_Captured)
        {
            KeyPushMake(lParam);
            break;
        }
        if (wParam == VK_F11)
        {
            HostFullscreenToggle(window);
            return 0;
        }
        if (wParam == VK_F5 && (GetKeyState(VK_CONTROL) & HOST_KEY_DOWN_BIT))
        {
            HostScreenshot();
            return 0;
        }
        /* [CAUTION]: Ctrl+F8 (host cursor on/off) WAS REMOVED WITH ITS MENU ITEM. Its
         * argument was "fullscreen is when you most want it and there is no menu
         * bar to reach" -- which is true of EXCLUSIVE MODE, and that is a mode, not
         * a cursor knob. One idea, one control; see CURSOR_IDLE_MS (#218).
         * Removing it also gives the F-key back to the guest.
         */
        /* Raw AT keyboard: push the MAKE scancode (lParam bits 16-23 = the OEM scan
         * code) into the 0x60/0x64 FIFO and raise IRQ1, so action games that hook
         * INT 09h or poll port 0x60 for real-time held-key state get input. This runs
         * for every key, alongside the INT 16h ring below (which other games poll).
         */
        KeyPushMake(lParam);
        /* NOTHING ELSE TO DO. The scancode above is the whole keystroke: INT 09h translates
         * it and fills the BIOS ring in guest memory, which is the single buffer INT 16h and
         * a BDA-reading program both look at. This used to ALSO push a keycode straight into
         * a host-side ring -- a second, invisible buffer that made INT 16h appear to work
         * while 0040:001E stayed empty forever, which is exactly why the Skyroads menu
         * ignored every arrow. One key, one path.
         */
        break;

    case WM_KEYUP:                       /* raw AT keyboard BREAK code + IRQ1 */
        /* Skip our injected Start-menu-suppression Ctrl -- otherwise the guest gets a
         * break code with no matching make.
         */
        if (GetMessageExtraInfo() == (LPARAM)HOST_INJECT_TAG)
            return 0;
        /* The Windows key is the HOST's (rule 4) and its DOWN is never forwarded -- so
         * its UP must not be either. It was: the guest got a lone E0 DB break code for
         * a key that did not exist when it was written (QB's scancode tables stop at
         * 0x58), which is the "Win key almost always crashes QBasic" report.
         */
        if (wParam == VK_LWIN || wParam == VK_RWIN)
            return 0;
        KeyMessageNote();
        KeyPushBreak(lParam);
        break;

    case WM_CHAR:                        /* Windows' translation: not ours to use */
        /* WM_KEYDOWN already delivered this key as a scancode, and INT 09h turns that into
         * the BIOS keycode. Pushing the WM_CHAR ascii as well would enter every printable
         * key TWICE. (The scancode path is also the only one that can produce the AL=0
         * extended codes an arrow needs, which WM_CHAR never generates at all.)
         */
        return 0;

    case WM_INPUT:                       /* raw relative motion -- see g_MouseDx */
    {
        RAWINPUT rawInput;
        UINT size = sizeof rawInput;
        if (g_PfnGetRawInput
                && g_PfnGetRawInput((HRAWINPUT)lParam, RID_INPUT, &rawInput, &size, sizeof(RAWINPUTHEADER))
                != (UINT)-1
            && rawInput.header.dwType == RIM_TYPEMOUSE
            && !(rawInput.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE))
        {
            /* Counted separately: an ABSOLUTE packet (RDP, some tablets/VMs) carries a
             * screen coordinate, not a delta, and adding it as one would fling the view.
             */
            ++g_MouseWmInput;
            InterlockedExchangeAdd(&g_MouseRawTotalX, (LONG)rawInput.data.mouse.lLastX);
            InterlockedExchangeAdd(&g_MouseRawTotalY, (LONG)rawInput.data.mouse.lLastY);
            /* RULE 6: raw input follows FOCUS, not capture -- a released guest would
             * otherwise keep mouse-looking while the user drags on the desktop.
             */
            if (!MouseGoesToGuest())
                break;
            /* #136: seamless -- only while Windows' pointer is over our picture. */
            if (g_MouseSeamless && !g_Captured)
            {
                POINT cursorPoint;
                if (!GetCursorPos(&cursorPoint) || WindowFromPoint(cursorPoint) != window)
                    break;
                ScreenToClient(window, &cursorPoint);
                if (!IsPointOverVideo(window, cursorPoint.x, cursorPoint.y))
                    break;
            }
            InterlockedExchangeAdd(&g_MouseDx, (LONG)rawInput.data.mouse.lLastX);
            InterlockedExchangeAdd(&g_MouseDy, (LONG)rawInput.data.mouse.lLastY);
            /* While captured the pointer is clipped, so WM_MOUSEMOVE stops telling the
             * truth about position -- drive the driver cursor from the deltas instead,
             * so INT 33h 03h still reports somewhere sensible.
             */
            if (g_Captured && I33Width() && I33Height())
            {
                /* -- #291 (user, s89): SENSITIVITY DRIVES THE CAPTURED POINTER. A game
                 * that reads the POSITION (Lemmings: INT 33h 03h only, never 0Bh)
                 * was never touched by the setting, and a mickey moved it one GAME
                 * pixel -- across a 320-wide picture scaled up on screen, much too
                 * fast. Scaled by Sensitivity here, with the remainder carried in
                 * hundredths so a slow hand still moves the pointer.
                 */
                static LONG remainderX;
                static LONG remainderY;
                LONG scaledX = (LONG)rawInput.data.mouse.lLastX * g_MouseSensitivity + remainderX;
                LONG scaledY = (LONG)rawInput.data.mouse.lLastY * g_MouseSensitivity + remainderY;
                LONG newX = g_MouseX + scaledX / PERCENT;
                LONG newY = g_MouseY + scaledY / PERCENT;
                remainderX = scaledX % PERCENT;
                remainderY = scaledY % PERCENT;
                if (newX < 0)
                    newX = 0;
                else if (newX >= (LONG)I33Width())
                    newX = (LONG)I33Width() - 1;
                if (newY < 0)
                    newY = 0;
                else if (newY >= (LONG)I33Height())
                    newY = (LONG)I33Height() - 1;
                if (newX != g_MouseX || newY != g_MouseY)
                    MouseEventRaise(1);                                         /* motion event */
                InterlockedExchange(&g_MouseX, newX);
                InterlockedExchange(&g_MouseY, newY);
            }
        }
        else if (rawInput.header.dwType == RIM_TYPEMOUSE)
            ++g_MouseRawAbsolute;
        break; }                         /* DefWindowProc must run: WM_INPUT cleanup */

    case WM_MOUSEMOVE:                   /* map client -> guest pixels, + buttons */

    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    {
        RECT clientRect;
        INT clientWidth;
        INT clientHeight;
        INT frameWidth;
        INT frameHeight;
        LONG buttons = 0;
        if (message == WM_MOUSEMOVE)
            CursorIdleNoteMove(window);                                   /* #218 */
        /* #154: Mark owns the mouse until the selection is copied or cancelled. The
         * guest sees none of it -- a drag that also clicked in the program would do two
         * things at once. A right click cancels, as in the console.
         */
        if (g_MarkMode)
        {
            INT cellColumn;
            INT cellRow;
            INT clientX = (INT16)LOWORD(lParam);
            INT clientY = (INT16)HIWORD(lParam);
            if (message == WM_RBUTTONDOWN)
            {
                SelectionClear();
                return 0;
            }
            if (message == WM_LBUTTONDOWN && ClientToCell(clientX, clientY, &cellColumn, &cellRow))
            {
                SetCapture(window);
                g_MarkDrag = 1;
                g_SelectionOn = 1;
                g_SelectionColumn0 = g_SelectionColumn1 = cellColumn;
                g_SelectionRow0 = g_SelectionRow1 = cellRow;
                SelectionPublish();
            }
            else if (message == WM_MOUSEMOVE && g_MarkDrag && ClientToCell(clientX, clientY, &cellColumn, &cellRow))
            {
                if (cellColumn != g_SelectionColumn1 || cellRow != g_SelectionRow1)
                {
                    g_SelectionColumn1 = cellColumn;
                    g_SelectionRow1 = cellRow;
                    SelectionPublish();
                }
            }
            else if (message == WM_LBUTTONUP && g_MarkDrag)
            {
                g_MarkDrag = 0;
                ReleaseCapture();
            }
            return 0;
        }
        /* RULE 5: A CLICK IN THE VIDEO RE-CAPTURES. ONLY THE VIDEO (Importance = 1):
         * This replaces Win+Click (s63), which was a toggle and needed a modifier
         * precisely BECAUSE it was one -- a plain click that could also release
         * would take the mouse away from a guest mid-game. Release is the Windows
         * key now (rule 4), so the click only ever goes one way and needs nothing
         * held.
         * - WHAT "ONLY THE VIDEO" BUYS. The caption, the frame and the menu bar are
         *   not client area at all, so they never reach here: the window stays
         *   draggable and the menus stay usable while a guest is waiting to have its
         *   mouse handed back. The status strip IS client area and a child besides,
         *   so it is excluded explicitly -- clicking the size grip should resize the
         *   window, not disappear the pointer.
         *
         * [CAUTION]: CONSUME THE DOWN. The click that takes the mouse back must not ALSO
         * arrive at the guest as a button press, or every re-entry fires the
         * weapon / picks the menu item under the pointer. The matching UP is
         * harmless and is deliberately left alone: swallowing an UP without its
         * DOWN is how a guest ends up believing a button is held forever.
         *
         * [CAUTION]: And gated on rule 1 via InputCaptureSet: for a guest that never touched
         * INT 33h this is an ordinary click on an ordinary window.
         */
        if (message == WM_LBUTTONDOWN && !g_Captured && CaptureAllowed()
            && IsPointOverVideo(window, (INT16)LOWORD(lParam), (INT16)HIWORD(lParam)))
        {
            InputCaptureSet(window, TRUE);
            return 0;
        }
        /* RULE 6: a mouse-using guest that is released sees no position and no
         * buttons. Not consumed -- DefWindowProc still gets its ordinary click.
         */
        if (!MouseGoesToGuest())
            break;
        /* -- #325: g_MouseX/y are in the MODE's extent (gw x gh -- what the driver reports,
         * 640 wide in text whatever the cell width), mapped through the rectangle the
         * picture was actually drawn into. It used the whole client, which put the
         * guest pointer in the wrong place whenever the picture was letterboxed --
         * and with whole-number scaling, maximised and fullscreen always are.
         */
        frameWidth = (INT)I33Width();
        frameHeight = (INT)I33Height();
        GetClientRect(window, &clientRect);
        { INT originX = 0, originY = 0, frameX, frameY;
          clientWidth = clientRect.right;
          clientHeight = clientRect.bottom - (g_PresentDdraw.StatusHeight ? g_PresentDdraw.StatusHeight : PRESENT_STATUS_HEIGHT);
          if (g_PresentDdraw.LastDestinationWidth > 0 && g_PresentDdraw.LastDestinationHeight > 0)
          {
              originX = g_PresentDdraw.LastDestinationX;
              originY = g_PresentDdraw.LastDestinationY;
              clientWidth = g_PresentDdraw.LastDestinationWidth;
              clientHeight = g_PresentDdraw.LastDestinationHeight;
          }
          if (clientWidth < 1)
              clientWidth = 1;
          if (clientHeight < 1)
              clientHeight = 1;
          frameX = ((INT16)LOWORD(lParam) - originX) * frameWidth / clientWidth;
          frameY = ((INT16)HIWORD(lParam) - originY) * frameHeight / clientHeight;
          if (frameX < 0)
              frameX = 0;
          else if (frameX >= frameWidth)
              frameX = frameWidth - 1;
          if (frameY < 0)
              frameY = 0;
          else if (frameY >= frameHeight)
              frameY = frameHeight - 1;
          if (!g_Captured) { if (frameX != g_MouseX || frameY != g_MouseY)
              MouseEventRaise(1);                                                               /* motion */
                             InterlockedExchange(&g_MouseX, frameX);   /* captured: WM_INPUT owns it */
                             InterlockedExchange(&g_MouseY, frameY); } }
        if (wParam & MK_LBUTTON)
            buttons |= I33_BUTTON_LEFT_BIT;
        if (wParam & MK_RBUTTON)
            buttons |= I33_BUTTON_RIGHT_BIT;
        if (wParam & MK_MBUTTON)
            buttons |= I33_BUTTON_MIDDLE_BIT;
        /* THE EDGE, NOT JUST THE LEVEL -- see MouseButtonEdges. The exchange must
         * happen first: the transition is recorded against the position we have
         * just written above, which is what 05h/06h are required to report.
         */
        {   LONG prev = InterlockedExchange(&g_MouseButtons, buttons);
            if (prev != buttons)
                MouseButtonEdges(prev, buttons); }
        return 0; }

    case WM_DESTROY:
        HostPauseSet(FALSE);               /* #219: a paused CPU thread must be let go */
        /* GIVE THE MACHINE BACK FIRST -- before g_Running, before the audio unwind,
         * before anything that can block. Everything below this line is about our
         * process; this line is about the user's computer. See HostPanicRelease.
         */
        HostPanicRelease();
        InterlockedExchange(&g_Running, 0);
        /* THE NUMBERS FOR A RUN A HUMAN ACTUALLY PLAYED (Importance = 2):
         * The full STAGE2 report is written by the EXEC thread when the guest
         * terminates. Closing the window does not terminate the guest -- it tears
         * the process down from the UI thread -- so an interactive session, which
         * is the only kind that can report FEEL, produced no numbers at all. Every
         * figure this project has on the keyboard-yield fault came from synthetic
         * runs for exactly that reason.
         * So dump the handful that matter from HERE, where a close always lands.
         * One line, no allocation, safe from this thread (all file-scope counters).
         *
         * [CAUTION]: Read `yld` against `raise-att`: they are equal by construction when the
         * keyboard is stealing the timer's slot, which is the whole mechanism.
         */
        {   CHAR closeLine[512], *closeCursor = closeLine;
            closeCursor = LogPut(closeCursor, "CLOSE: keyirq="); closeCursor = LogHex(closeCursor, (DWORD)g_KeyIrqRetry);
            closeCursor = LogPut(closeCursor, " courier=");      closeCursor = LogHex(closeCursor, (DWORD)g_CourierOn);
            closeCursor = LogPut(closeCursor, " irq0_inj=");     closeCursor = LogHex(closeCursor, g_Irq0Injected);
            closeCursor = LogPut(closeCursor, " anom_n=");       closeCursor = LogHex(closeCursor, g_Irq0AnomalyCount);
            closeCursor = LogPut(closeCursor, " gen=");          closeCursor = LogHex(closeCursor, g_Irq0AnomalyGeneration);
            closeCursor = LogPut(closeCursor, " del=");          closeCursor = LogHex(closeCursor, g_Irq0AnomalyDelete);
            closeCursor = LogPut(closeCursor, " anom[raise,att,nie,yld]=");
            closeCursor = LogHex(closeCursor, g_Irq0AnomalyRaise); closeCursor = LogPut(closeCursor, ",");
            closeCursor = LogHex(closeCursor, g_Irq0AnomalyAttempts);   closeCursor = LogPut(closeCursor, ",");
            closeCursor = LogHex(closeCursor, g_Irq0AnomalyNie);   closeCursor = LogPut(closeCursor, ",");
            closeCursor = LogHex(closeCursor, g_Irq0AnomalyYield);
            closeCursor = LogPut(closeCursor, " gap_ms[<1,1,2,4,8,16,32,64+]=");
            { UINT bucket;
            for (bucket = 0; bucket < ARRAYSIZE(g_Irq0GapHistogram); ++bucket)
            {
                closeCursor = LogPut(closeCursor, bucket ? "," : "");
                closeCursor = LogHex(closeCursor, g_Irq0GapHistogram[bucket]); } }
            closeCursor = LogPut(closeCursor, " gap_max=");  closeCursor = LogHex(closeCursor, g_Irq0GapMaximumMs);
            closeCursor = LogPut(closeCursor, " keydel_ms[0,1,2,4,8,16,32,64+]=");
            { UINT histogramIndex;
            for (histogramIndex = 0; histogramIndex < ARRAYSIZE(g_KeyDeliveryHistogram); ++histogramIndex)
            {
                closeCursor = LogPut(closeCursor, histogramIndex ? "," : "");
                closeCursor = LogHex(closeCursor, g_KeyDeliveryHistogram[histogramIndex]); } }
            closeCursor = LogPut(closeCursor, " keydel_max="); closeCursor = LogHex(closeCursor, g_KeyDeliveryMaximumMs);
            closeCursor = LogPut(closeCursor, " cour_inj=");   closeCursor = LogHex(closeCursor, g_CourierInjected);
            closeCursor = LogPut(closeCursor, " capwd=");      closeCursor = LogHex(closeCursor, g_CaptureWatchdogReleased);
            closeCursor = LogPut(closeCursor, " pit_skip=");   closeCursor = LogHex(closeCursor, g_PitDeliverSkipped);
            /* NAME THE LOCK HOLDER. (s61, and it is the whole question now.) (Importance = 3):
             * The arm-4 close said 86% of the stalls are GENERATION -- the 8254 never
             * made the tick, because HostPitSync could not run. Every caller of it
             * takes g_Lock, so the culprit is whoever was holding g_Lock, and the
             * lock instrument has known that number since session 21 without ever
             * being printed anywhere an INTERACTIVE run could reach.
             * - hold@line is the answer: it is the __LINE__ of the outermost acquire
             *   that held longest. Read it with wait@line (who was stuck behind it)
             *   and ui_gap (whether the UI thread was running at all).
             */
            closeCursor = LogPut(closeCursor, " lk_hold_us="); closeCursor = LogHex(closeCursor, g_LockHoldMicroseconds);
            closeCursor = LogPut(closeCursor, "@line ");       closeCursor = LogHex(closeCursor, (DWORD)g_LockHoldSite);
            closeCursor = LogPut(closeCursor, " lk_wait_us="); closeCursor = LogHex(closeCursor, g_LockWaitMicroseconds);
            closeCursor = LogPut(closeCursor, "@line ");       closeCursor = LogHex(closeCursor, (DWORD)g_LockWaitSite);
            closeCursor = LogPut(closeCursor, " ui_gap_us=");  closeCursor = LogHex(closeCursor, g_UiGapMicroseconds);
            closeCursor = LogPut(closeCursor, "\r\n");
            LogAppend(LOG_PATH, closeLine, closeCursor);
            SerialOut(closeLine, closeCursor); }
        /* #225: ...AND THE SPEED LIMIT'S, for the same reason. The user plays with the
         * throttle on and closes the window, so "slow in-game" came back with no
         * duty, no holds and no per-second clock. Second line, same rules.
         */
        {   CHAR closeLine[2048], *closeCursor = closeLine;
        UINT second;
        DWORD last = 0;   /* 90 x 9 + ~350 */
            closeCursor = LogPut(closeCursor, "CLOSE2: cpuspd idx="); closeCursor = LogHex(closeCursor, (DWORD)g_CpuSpeedIndex);
            closeCursor = LogPut(closeCursor, " duty_bp=");     closeCursor = LogHex(closeCursor, (DWORD)g_CpuSpeedDuty);
            closeCursor = LogPut(closeCursor, " duty_rm_bp=");  closeCursor = LogHex(closeCursor, (DWORD)g_CpuSpeedDutyRm);
            closeCursor = LogPut(closeCursor, " pm=");          closeCursor = LogHex(closeCursor, (DWORD)g_DpmiPm);
            closeCursor = LogPut(closeCursor, " delivered_bp="); closeCursor = LogHex(closeCursor, CpuSpeedDeliveredBp(g_CpuSpeedRunMs, GetTickCount() - g_StartMs));
            closeCursor = LogPut(closeCursor, " exec_ms=");     closeCursor = LogHex(closeCursor, g_CpuSpeedRunMs);
            closeCursor = LogPut(closeCursor, " held_ms=");     closeCursor = LogHex(closeCursor, g_CpuSpeedHeldMs);
            closeCursor = LogPut(closeCursor, " periods=");     closeCursor = LogHex(closeCursor, g_CpuSpeedPeriods);
            closeCursor = LogPut(closeCursor, " coop=");        closeCursor = LogHex(closeCursor, g_CpuSpeedCooperativeCatches);
            closeCursor = LogPut(closeCursor, " coop_to=");     closeCursor = LogHex(closeCursor, g_CpuSpeedCooperativeTimeouts);
            closeCursor = LogPut(closeCursor, " missed=");      closeCursor = LogHex(closeCursor, g_CpuSpeedMissed);
            closeCursor = LogPut(closeCursor, " hold_max_us="); closeCursor = LogHex(closeCursor, g_CpuSpeedHoldMaximumMicroseconds);
            closeCursor = LogPut(closeCursor, " vbl_edges=");   closeCursor = LogHex(closeCursor, g_Video.VblEdges);
            closeCursor = LogPut(closeCursor, " vbl_owed=");    closeCursor = LogHex(closeCursor, g_Video.Port3DaVblOwed);
            closeCursor = LogPut(closeCursor, " p3da=");        closeCursor = LogHex(closeCursor, g_Video.Port3DaReads);
            closeCursor = LogPut(closeCursor, " clip_repairs=");closeCursor = LogHex(closeCursor, g_ClipRepairs);
            closeCursor = LogPut(closeCursor, " irq0tl=");
            for (second = 0; second < IRQ0TL_SECS; ++second)
                if (g_Irq0TimeLast[second])
                    last = second;
            for (second = 0; second <= last && second < IRQ0TL_SECS; ++second)
            {
                closeCursor = LogPut(closeCursor, second ? "," : "");
                closeCursor = LogHex(closeCursor, g_Irq0TimeLast[second]);
            }
            closeCursor = LogPut(closeCursor, "\r\n");
            LogAppend(LOG_PATH, closeLine, closeCursor);
            if (g_CpuSpeedPeriods)
                CpuSpeedTimelineDump("CLOSE2:"); }
        /* PANIC-STOP THE SOUND, HERE, BEFORE ANYTHING ELSE UNWINDS.
         * Closing the window used to leave notes sounding until the host was
         * restarted (user, 2026-08-21). Nothing ever called AudioWaveStop -- it
         * existed and had no caller -- so waveOut kept playing, and the mixer kept
         * being asked for samples from an OPL whose voices were still keyed on. A
         * sustaining voice (EGT=1) holds its level forever by design, so "forever"
         * is exactly what you get: the chord under the cursor at the moment you
         * clicked the X, indefinitely.
         * ORDER MATTERS. Stop the device FIRST: that resets waveOut and ends the
         * fill callbacks, so no audio thread is inside VddAudioMix when the OPL
         * is torn down underneath it. Only then silence the chip. Doing it the
         * other way round races the callback for the state it is reading.
         */
        HostRecordFinish();       /* before the device stops feeding it */
        AudioWaveStop(&g_Wave);
        /* [CAUTION]: AND THE OTHER SPEAKER, WHICH IS NOT OURS TO LEAVE RUNNING. Beep.sys
         * keeps sounding after the process that started it exits -- the note
         * under the cursor would outlive the host and only a reboot would clear
         * it. Same failure the OPL had, on a device we do not even own.
         */
        PcSpeakerClose(&g_PcSpeaker);
        HOST_LOCK();
        VddOplReset(&g_Opl);                    /* all voices off, registers clear */
        HOST_UNLOCK();
        if (g_KeyEvent)
            SetEvent(g_KeyEvent);               /* unblock the V86 thread */
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(window, message, wParam, lParam);
}

enum
{
    HID_USAGE_PAGE_GENERIC_DESKTOP = 0x01, HID_USAGE_GENERIC_MOUSE = 0x02
};   /* hidusage.h: raw input's mouse */
DWORD WINAPI UiThread(LPVOID argument)
{
    WNDCLASSA windowClass;
    MSG message;
    RECT rect;
    HINSTANCE instance = GetModuleHandleA(NULL);
    INITCOMMONCONTROLSEX commonControls;

    (VOID)argument;
    commonControls.dwSize = sizeof commonControls;
    commonControls.dwICC = ICC_WIN95_CLASSES;
    InitCommonControlsEx(&commonControls);                  /* activate the Luna (v6) context */
    ZeroMemory(&windowClass, sizeof windowClass);
    windowClass.lpfnWndProc = HostWindowProcedure;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorA(NULL, IDC_ARROW);
    windowClass.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    windowClass.hIcon = LoadIconA(instance, MAKEINTRESOURCEA(IDI_MAINICON));    /* IDI_MAINICON: title bar + taskbar */
    windowClass.lpszClassName = HOST_WINDOW_CLASS;
    if (!RegisterClassA(&windowClass))
        return 1;
    /* The initial size. Same helper the View menu and the dialog resize through, so
     * "what 2x means" has one definition rather than one per call site.
     */
    {   INT scale = (INT)g_Settings.Values[SET_WINSIZE] + 1, pictureWidth, pictureHeight;
        g_WindowSizeLive = g_Settings.Values[SET_WINSIZE];   /* the window is now AT this size */
        g_AspectLive  = g_Settings.Values[SET_ASPECT];    /* ...and in this shape */
        while (scale > 1 && !WindowScaleFits(scale))
            --scale;
        g_ScaleFactor = scale;
        g_ScaleWant = (INT)g_Settings.Values[SET_WINSIZE] + 1;
        HostPicture(scale, &pictureWidth, &pictureHeight);           /* #325: 720x400 text until a frame */
        rect.left = 0;
        rect.top = 0;
        rect.right = pictureWidth;
        rect.bottom = pictureHeight + PRESENT_STATUS_HEIGHT; }
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, TRUE);   /* TRUE: window has a menu */
    g_Window = CreateWindowA(windowClass.lpszClassName, VDM_WIN_TITLE, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                           CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top,
                           NULL, NULL, instance, NULL);
    if (!g_Window)
        return 1;
    {   RAWINPUTDEVICE rawInputDevice;              /* generic desktop / mouse */
        rawInputDevice.usUsagePage = HID_USAGE_PAGE_GENERIC_DESKTOP;
        rawInputDevice.usUsage = HID_USAGE_GENERIC_MOUSE;
        rawInputDevice.dwFlags = 0;                 /* follow focus: no INPUTSINK, foreground only */
        rawInputDevice.hwndTarget = g_Window;
        g_MouseRawOk = (g_PfnRegisterRawInput && g_PfnRegisterRawInput(&rawInputDevice, 1, sizeof rawInputDevice)) ? 1 : 0;
    }
    SetMenu(g_Window, BuildMenu());
    MenuViewSync(g_Window);                  /* View + CPU Speed reflect the state */
    /* DOES THE SETTINGS DIALOG STILL BUILD? (Importance = 1):
     * A malformed DIALOGEX template does not draw badly -- it FAILS TO CREATE, and
     * the menu item then silently does nothing. Adding a control whose window class
     * is not registered, or a style constant the resource compiler did not know, is
     * exactly how that happens, and a successful BUILD says nothing about it.
     * - So create every page off-screen, check the controls that matter are really
     *   there, log it, and destroy them. Deterministic, needs no clicking, and it
     *   runs in a headless test -- which matters because the alternative (driving the
     *   menu with synthetic clicks) lands on the desktop of whoever is using the box.
     *
     * [CAUTION]: Behind a flag: six dialogs at every startup is a cost no shipped run needs
     * to pay, and this answers a question that only changes when the .rc does.
     */
    if (GetFileAttributesA(DLGCHECK_FLAG) != INVALID_FILE_ATTRIBUTES)
    {
        CHAR createLine[512];
        CHAR *createCursor = createLine;
        INT pageIndex;
        HINSTANCE moduleInstance = GetModuleHandleA(NULL);
        for (pageIndex = 0; pageIndex < NTVDMEX_PAGE_COUNT; ++pageIndex)
        {
            HWND page = CreateDialogParamA(moduleInstance, MAKEINTRESOURCEA(g_SettingsPages[pageIndex]),
                                         g_Window, SettingsPageProcedure, 0);
            createCursor = LogPut(createCursor, "DLGCHECK page="); createCursor = LogHex(createCursor, (DWORD)g_SettingsPages[pageIndex]);
            createCursor = LogPut(createCursor, page ? " CREATED" : " **FAILED** err=");
            if (!page)
                createCursor = LogHex(createCursor, GetLastError());
            if (page)
            {
                /* A control that must exist on THIS page, by id, so "the page was
                 * created" cannot be mistaken for "the controls I moved are on it".
                 * Each is 0 on every page but the one that owns it -- which is the
                 * point: it proves the pages hold what they should (s84: all four are on
                 * Machine, and 0 on every other page).
                 */
                createCursor = LogPut(createCursor, " speed=");  createCursor = LogHex(createCursor, GetDlgItem(page, IDC_S_SPEEDMODE) ? 1u : 0u);
                createCursor = LogPut(createCursor, " cpuinfo=");createCursor = LogHex(createCursor, GetDlgItem(page, IDC_S_CPUINFO) ? 1u : 0u);
                createCursor = LogPut(createCursor, " convkb="); createCursor = LogHex(createCursor, GetDlgItem(page, IDC_S_CONVKB) ? 1u : 0u);
                createCursor = LogPut(createCursor, " pitpace=");createCursor = LogHex(createCursor, GetDlgItem(page, IDC_S_PITPACE) ? 1u : 0u);
                DestroyWindow(page);
            }
            createCursor = LogPut(createCursor, "\r\n");
            LogAppend(LOG_PATH, createLine, createCursor); createCursor = createLine;
        }
    }
    if (GetFileAttributesA(SETSHOT_PATH) != INVALID_FILE_ATTRIBUTES)   /* s84, test-only */
        PostMessageA(g_Window, WM_COMMAND, IDM_FILE_SETTINGS, 0);
    PresentDdrawInitialize(&g_PresentDdraw, g_Window);          /* GDI windowed; DDraw for fullscreen */
    SettingsApplyPresent(&g_PresentDdraw, &g_Settings);      /* ...which zeroes its own struct */
    g_PresentDdraw.IsFlipDriverTimed = GetFileAttributesA(DDFLIP_DRIVER_FLAG) != INVALID_FILE_ATTRIBUTES;
    MakeStatus(g_Window, instance);                     /* native themed status bar */
    /* AND NOW RE-SIZE TO THE STATUS BAR'S REAL HEIGHT (Importance = 1):
     * The window was created against PRESENT_STATUS_HEIGHT, a compile-time GUESS at
     * how tall a comctl32 status bar is. The real one measures 23 on the rig, not
     * 22 -- so the client area was a pixel short of the framebuffer and the guest
     * picture lost a row at EVERY scale. Invisible until the View menu made the
     * window resize live and 1x came back one pixel taller than it started.
     *
     * [CAUTION]: The theme decides that height, so it is not a constant to correct; ask the
     * control after it exists and size the window to the answer.
     */
    HostApplyWindowSize(g_Window, g_Settings.Values[SET_WINSIZE]);
    /* [INFO]: A WIN16 GUEST GETS NO VDM WINDOW -- see the note by TrayAdd. The window
     * is built either way (it owns the present surface, the raw input, the frame
     * timer and the tray callbacks); only whether anyone sees it changes.
     */
    /* GH #281: with the manager present, IT owns the tray icon -- for this host and
     * every other. Without it (ntvdmex.exe missing) a Win16 host keeps its own.
     */
    ManagerStart();
    /* [WARNING]: A Win16 host's window is NEVER shown (user, s88: "Win16 apps should not
     * show an NTVDMEX host window ... at all"). 9b69fe1 folded the manager test
     * into this condition and a Win16 launch WITH a manager fell into the else:
     * every Win16 program came up with a black machine window beside it.
     */
    if (g_WowLaunch)
    {
        if (!g_ManagerAvailable)
            TrayAdd(instance, g_Window);
    }
    else
    {
        ShowWindow(g_Window, SW_SHOW);
        UpdateWindow(g_Window);
    }
    /* [INFO]: THE START HAS SUCCEEDED -- say so NOW, not at exit. (GH #132, 2026-09-12)
     * From here the user can close us, so the "no working VDM on the box" failure
     * the three-strikes counter defends against can no longer happen. Clearing it
     * at exit instead counted every X-button quit and every guest crash as a
     * failed start, and three play-tests uninstalled the host.
     */
    RecoveryOk();
    {   CHAR stageLine[96], *stageCursor = stageLine;
        stageCursor = LogPut(stageCursor, "STAGE0: window up -> start counted as SUCCEEDED (GH #132)\r\n");
        LogAppend(LOG_PATH, stageLine, stageCursor); }
    HostAutoFullscreenConsider(g_Window, g_Video.ModeKind != VIDEO_KIND_TEXT || g_Video.IsVesa);
    SetTimer(g_Window, 1, VID_PRESENT_TICK_MS, NULL);  /* fast tick; present is PHASE-gated */
    /* [WARNING]: A POSTED PRESENT MUST NOT STARVE INPUT. (user, s86: Duke3D fullscreen) (Importance
     * = 2): GetMessage hands out POSTED messages before keyboard and mouse input. The guest posts
     * WM_APP_PRESENT from its own retrace poll, one in flight, cleared at the START of the frame
     * body -- so when the body takes about a frame period (Duke3D, fullscreen, a busy scene) the
     * next present is already queued every time we come back here, and input is never reached.
     * Measured: 40 keystrokes held 11 s in our queue (KEYLAT max_ms=0x2ae9), raw mouse frozen,
     * Alt+Enter dead, while the guest and this thread's frame body both ran on. The machine's input
     * was never stuck; ours was.
     * - So before a present is dispatched, everything input-class already queued goes
     *   first. Here, at the TOP-LEVEL pump only: a modal loop (menu, size/move) never
     *   runs this, so nothing re-enters the frame body. Spelled out rather than
     *   PM_QS_INPUT because the header's QS_INPUT only includes QS_RAWINPUT (WM_INPUT,
     *   the captured mouse) for _WIN32_WINNT >= 0x0501.
     */
    while (GetMessageA(&message, NULL, 0, 0) > 0)
    {
        if (message.message == WM_APP_PRESENT)
        {
            MSG pending;
            while (PeekMessageA(&pending, NULL, 0, 0, PM_REMOVE |
                                (((UINT)QS_KEY | (UINT)QS_MOUSEMOVE | (UINT)QS_MOUSEBUTTON | (UINT)QS_RAWINPUT) << WORD_SHIFT)))    /* KEY MOUSEMOVE MOUSEBUTTON RAWINPUT */
            {
                if (pending.message == WM_QUIT)
                {
                    PostQuitMessage((INT)pending.wParam);
                    break;
                }
                ++g_UiInputFirst;
                TranslateMessage(&pending);
                DispatchMessageA(&pending);
            }
        }
        TranslateMessage(&message);
        DispatchMessageA(&message);
    }
    TrayRemove(g_Window);            /* or the icon outlives the process */
    PresentDdrawShutdown(&g_PresentDdraw);
    /* [WARNING]: THE WINDOW CLOSING MUST KILL THE PROCESS, NOT JUST THIS THREAD. (s63) (Importance
     * = 2): Reported by the user: launch a game, close the window, launch something else -- and the
     * host "flashes open and immediately exits", so nothing can be tested. The cause is the
     * single-instance mutex (WinMain) held for the PROCESS's life, colliding with a ZOMBIE left by
     * this very close:
     *   WinMain runs on the MAIN thread, which is the GUEST exec thread. Closing
     *   the window runs WM_DESTROY *on this UI thread* -- it sets g_Running=0 and
     *   PostQuitMessage, so this message loop returns and all the machine-release
     *   cleanup (HostPanicRelease, audio, OPL, tray) has already run. But the
     *   main thread only notices g_Running=0 when it RETURNS from VdmRunGuest; a guest
     *   spinning in a tight loop that traps nothing sits inside VdmStartExecution
     *   forever, so WinMain never returns, ExitProcess is never reached, and the
     *   process lingers -- STILL OWNING THE MUTEX. The next launch reads
     *   ERROR_ALREADY_EXISTS and bails in milliseconds: the flash-and-vanish.
     *
     * By here the user has closed the window and the cleanup is done, so we WANT
     * to be gone regardless of where the guest thread is stuck. ExitProcess is not
     * safe -- it tries to unwind the wedged PM engine and can hang (see the
     * watchdog's note) -- so TERMINATE. This frees the mutex and there is no zombie
     * for the next launch to trip over. Belt-and-suspenders with the mutex guard,
     * which now also opens the door when it finds the holder was abandoned.
     */
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;                                            /* not reached */
}
