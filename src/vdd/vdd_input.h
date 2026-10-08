/*
 * vdd_input.h -- the keyboard input VDD.  (M3 slice-6, ADR-0008)
 *
 * A pure key ring buffer + the INT 16h (BIOS keyboard) servicer. The host's UI
 * thread pushes keystrokes (scancode<<8 | ascii) as they arrive; the V86/DOS
 * side drains them via INT 16h and the INT 21h input calls. Blocking (INT 16h
 * AH=00 waiting for a key) is the host's job -- this VDD stays pure and reports
 * "no key" via the zero flag, exactly like the BIOS.
 */
#ifndef NTVDMEX_VDD_INPUT_H
#define NTVDMEX_VDD_INPUT_H

/* A key event's flags, as the host passes them on. */
#define INPUT_KEY_MAKE      0
#define INPUT_KEY_BREAK     1
#define INPUT_KEY_NORMAL    0
#define INPUT_KEY_EXTENDED  1       /* E0-prefixed */
#define INPUT_RELEASED      0
#define INPUT_PRESSED       1

#include "vdd_bus.h"
#include "../dos/bios_bda_fields.h"   /* the BDA's fields */

#ifndef INPUT_SCANCODE_QUEUE_SIZE
#define INPUT_SCANCODE_QUEUE_SIZE 32        /* ring capacity (power of two not required)      */
#endif

/* THE BIOS KEYBOARD BUFFER LIVES IN THE GUEST, NOT IN US.
   `bda` points at guest segment 0x40 (linear 0x400). Every keystroke is stored in the real
   BIOS ring at 0040:001E with its head/tail at 0040:001A/001C, and INT 16h reads that same
   ring -- because a DOS program is entitled to read it directly, and a great many do. We
   used to keep the ring host-side instead, so INT 16h worked while the buffer in guest
   memory stayed empty forever (head==tail==0x1E in every capture). The Skyroads MENU polls
   exactly that buffer, which is why no arrow or Enter ever registered there while the same
   keys worked in-game (where the game installs its own INT 09h handler and reads port 60h).
   One buffer, in the place the hardware documentation says it is. */
/* ── #274: THE RING'S BOUNDS ARE TWO BDA WORDS, NOT TWO CONSTANTS. ─────────────────────
     0040:0080 / 0040:0082 hold the ring's start and one-past-end OFFSETS within segment
     0040h (AT BIOS and later; IBM AT TR, RBIL MEMORY.LST "0040:0080"). POST sets them to
     001Eh / 003Eh -- the constants above -- and every AT-class INT 09h / INT 16h wraps at
     what they say. That is how a TSR enlarges or relocates the keyboard buffer: it points
     both words (and head = tail = start) at a bigger area inside segment 0040h. Our ring
     wrapped at the constants and nothing wrote the words at all. VddInputReset() now
     writes POST's values, and push/pop/peek read the words on every call. A pair that is
     not usable (odd, start >= end, or room for fewer than two entries) falls back to the
     POST bounds rather than sending writes anywhere in guest memory. */
#define INPUT_INT16_FUNCTION_GROUPS 4   /* read, status, shift status, other     */
#define INPUT_PORT_LOG_ENTRIES 16
#define INPUT_PORT_LOG_FIELDS  2         /* (port low byte, value)                */
#define INPUT_ACTION_KINDS     6
#define INPUT_SCANCODE_DOWN    0x50     /* the cursor-down key's make code           */
#define INPUT_SCAN_TAB              0x0F
#define INPUT_SCAN_BREAK_BIT        0x80    /* make code | this = the key's release     */
#define INPUT_SCAN_PREFIX_E0        0xE0    /* an extended key's prefix                 */
#define INPUT_SCAN_ENTER            0x1C    /* keypad Enter when E0-prefixed             */
#define INPUT_SCAN_LEFT_SHIFT       0x2A
#define INPUT_SCAN_CTRL             0x1D
#define INPUT_SCAN_RIGHT_SHIFT      0x36
#define INPUT_SCAN_ALT              0x38
#define INPUT_LAYOUT_LAST           3       /* Layout: 0 US, 1 UK, 2 German, 3 French    */
#define INPUT_DEVICE_NAME      "input"
#define INPUT_HOST_KEY_BYTES_MAX 6         /* VddInputHostKeyBytes: the Pause sequence    */

typedef struct _INPUT_STATE {
    PVDD_BUS Bus;
    BYTE *BiosData;              /* guest 0040:0000; NULL only in unit tests before setup */
    BYTE  IsExtendedPending;      /* an E0 prefix has been seen; next code is an extended key */
    /* Raw AT keyboard controller (ports 0x60/0x64): the byte stream an INT 09h
       ISR / a port-polling game reads. UI pushes make (sc) + break (sc|0x80)
       codes; the guest drains one per IN 0x60. Separate from the INT 16h ring. */
    BYTE  ScanCodeQueue[INPUT_SCANCODE_QUEUE_SIZE];
    INT      ScanCodeHead, ScanCodeTail; /* scancode FIFO: empty when ScanCodeHead==ScanCodeTail      */
    /* HOW THE GUEST ASKS FOR KEYS. Arrow keys work in a Skyroads level but not in its
       menus, which means the two read the keyboard by different routes -- so count them:
       [0]=INT 16h AH=00/10 (blocking read), [1]=AH=01/11 (peek), [2]=AH=02 (shift flags),
       [3]=other, plus raw port 0x60 reads. Whichever the menu uses is where to look. */
    UINT32 Int16Calls[INPUT_INT16_FUNCTION_GROUPS];
    UINT32 Port60Reads;
    /* Scancode accounting. The FIFO discards silently when it fills, so a guest that
       will not drain it (Skyroads runs with IF clear for long stretches) loses bytes
       with nothing anywhere to say so -- and a lost E0 prefix turns an arrow into a
       different key entirely. Counted so "dropped" is a measurement, not a deduction
       from pushes-minus-reads. */
    UINT32 ScanCodesPushed;
    UINT32 ScanCodesDropped;
    UINT32 ScanCodeHighWater;       /* deepest the FIFO ever got (of INPUT_SCANCODE_QUEUE_SIZE)     */

    /* WHAT THE GUEST ASKS OF THE 8042. Every write to 0x60/0x64 used to be dropped
       on the floor with a comment saying so -- a silent failure, which is the one
       thing this project's standing principle rules out. It matters here because
       the TYPEMATIC RATE is not a constant: DOS action games commonly set their own
       via command 0xF3 (often the fastest, 30 cps / 250 ms delay) precisely so held
       keys feel responsive. Ignoring that leaves us repeating at whatever rate we
       chose for ourselves, which is a guess dressed as hardware.
       Logged as a SEQUENCE rather than OR-ed together: an OR over a run cannot tell
       "asked for once at init" from "asked for repeatedly", and that exact weakness
       has already produced one confident wrong answer on this project. */
    UINT32 KeyboardPortWrites;   /* writes to 0x60/0x64 seen                        */
    BYTE  KeyboardPortLog[INPUT_PORT_LOG_ENTRIES][INPUT_PORT_LOG_FIELDS];   /* first 16 as (port low byte, value)          */
    BYTE  KeyboardPortLogCount;
    BYTE  IsKeyboardRateExpected;  /* 0xF3 seen; the next byte is the rate/delay      */
    BYTE  IsTypematicSet;    /* the guest set a rate at least once          */
    BYTE  TypematicByte;   /* the last rate/delay byte it asked for       */
    BYTE  LastScanCode;          /* last byte handed out on IN 0x60 (re-read)       */
    /* ── ★ THE BYTE THE GUEST TOOK FIRST IS STILL THE BIOS'S TO TRANSLATE. ────────
         The commonest INT 09h hook of the era reads port 0x60 ITSELF, looks at the
         scancode, and then chains to the BIOS handler it displaced (QB.EXE 4.5 does
         exactly this: `in al,60h` at 1DDC8h, then `int 0EFh` = the saved vector).
         On an 8042 that is fine: the output buffer keeps presenting the same byte
         until the next one is loaded, so the BIOS's own `in al,60h` sees it again.
         Our FIFO POPPED on the guest's read, so the BIOS arm found nothing, stored
         nothing, and every key typed into QBasic vanished -- "I couldn't type".
         This flag is that byte's pending translation: set by a port read, cleared
         when the BIOS arm consumes it or a newer byte arrives. */
    BYTE  BiosScanCodesOwed;
    /* ── ★★★ THE 8042 IS A CONTROLLER, NOT A SCANCODE FIFO. (docs/ref/kbc.md) ──────
         Everything above models the keyboard's byte stream. These model the chip
         that stream passes through -- and two of the things it controls have
         nothing to do with typing: THE A20 GATE and THE CPU RESET LINE.
       ★ MEASURED (p_kbc.asm, 2026-09-23): PCem, with a real AMI 486 BIOS, answers
         the self test with 0x55, returns a real output port (0xCF) for command
         D0h, and reads a status of 0x1C when idle. 6.22-under-QEMU and dosbox-x
         answer NONE of the commands -- so here it is the period-correct machine
         that has the feature and the software emulators that cut the corner, which
         is the reverse of the usual split and about as strong as evidence gets.
       ⚠ `InputControllerReply` IS NOT `sc_last`. A controller reply and a scancode share one
         output buffer on the real part, but keeping them apart here is what stops
         a discarded command handing the guest a KEYSTROKE where it expected the
         output port -- which is what we did, and what a driver reads bit 1 of as
         the state of A20. */
    BYTE  ControllerCommand;          /* an 8042 command awaiting its parameter byte     */
    BYTE  ControllerReply;        /* a controller reply presented at port 60h        */
    BYTE  IsControllerReplyReady;    /* ...and whether one is presented                 */
    BYTE  ControllerCommandByte;      /* the command byte (20h reads, 60h writes)        */
    BYTE  ControllerOutputPort;      /* the output port: bit 0 reset, bit 1 A20         */
    BYTE  IsLastWriteCommand; /* status bit 3 (A2): the last write went to 64h   */
    UINT32 ControllerResetsAsked;  /* FEh, or an output-port write with bit 0 clear   */
    UINT32 OwedScanCodesServed;   /* BIOS arm keys served from the guest-read byte   */
    /* ── ★ THE KEYBOARD TAKES ~1 ms TO SEND THE NEXT BYTE, AND CODE RELIES ON IT. ──────
         An AT keyboard cannot send while the 8042's output buffer is full; once the
         guest reads port 60h the next byte still needs ~11 bit-times on the keyboard's
         clock to arrive. So two reads of port 60h inside the same interrupt handler
         return the SAME byte on real hardware -- which is exactly what layered INT 09h
         hooks depend on: QB.EXE 4.5 has two (the IDE's at 3BE9Bh and the runtime's at
         1DDB1h), each does `in al,60h`, and then the BIOS is chained and reads it a
         third time. Our FIFO popped on EVERY read, so with bytes already queued (fast
         typing, or a held Alt repeating) the three readers each got a DIFFERENT byte: the
         sequence scrambled, Alt never released, and QB fell over "intermittently, mostly
         with Alt". DOSBox models the same delay (its KEYDELAY). With no clock injected
         (the battery's default) the hold is zero and behaviour is as before. */
    UINT64 (*TimeMicroseconds)(VOID);  /* host clock; NULL = no transfer delay          */
    UINT64 ScanCodeHoldUntil;     /* the next byte is not presented before this     */
    BYTE  IsScanCodeIrqUp;         /* IRQ1 raised for the byte at the head, not yet popped */
    UINT32 ScanCodeHeldReads;     /* port 60h reads answered with the same byte (held) */
    BYTE  Layout;            /* #136: 0 US, 1 UK, 2 German, 3 French (SET_KBLAYOUT) */
    BYTE  E1Pending;        /* #254: bytes left of an E1 (Pause) sequence      */
    UINT32 BiosActions[INPUT_ACTION_KINDS];   /* #254: INPUT_ACTION_* raised, by kind (STAGE2 counter)  */
} INPUT_STATE, *PINPUT_STATE;

typedef const INPUT_STATE *PCINPUT_STATE;

/* ── #254: WHAT THE BIOS INT 09h DOES BESIDES STORE A KEY. ───────────────────────────
     VddInputBiosConsume() returns one of these; the caller that can run guest code
     (the V86 INT 09h arm) resumes the guest in the matching BIOS routine -- INT 1Bh for
     Ctrl-Break, INT 05h for Print Screen, INT 15h AH=85h for SysReq, the pause loop.
     The BDA side (ring flush, 0040:0071, 0040:0018) is already done by then. */
#define INPUT_ACTION_NONE    0
#define INPUT_ACTION_BREAK   1        /* Ctrl-Break: INT 1Bh (0000h stored, 0071h bit 7)  */
#define INPUT_ACTION_PRINT_SCREEN   2        /* Print Screen: INT 05h                            */
#define INPUT_ACTION_SYSREQ_DOWN 3        /* SysReq pressed:  INT 15h AX=8500h                */
#define INPUT_ACTION_SYSREQ_UP 4        /* SysReq released: INT 15h AX=8501h                */
#define INPUT_ACTION_PAUSE   5        /* Pause: spin until 0040:0018 bit 3 clears         */
/* The pause flag must not outlive a caller that cannot run the loop (PM, nested). */
VOID VddInputPauseCancel(PINPUT_STATE state);
/* #254: a ring entry as DOS's CON reads it -- the grey-key E0 forms folded to the
   83-key ones (AL E0h -> 00h, scan E0h -> the keypad Enter / slash scan), so a
   DOS line editor never sees 0E0h as a character. */
WORD VddInputDosKey(WORD key);
#define INPUT_KEYBOARD_TRANSFER_US 900u        /* ~11 bits at the keyboard's ~12 kHz clock       */
/* Present the next queued byte once the transfer delay has passed: raises IRQ1 if one
   is not already up. Cheap when nothing is queued; the host calls it every exec-loop
   pass and from its UI timer. */
VOID VddInputPoll(PINPUT_STATE state);
INT  VddInputScanCodesQueued(PCINPUT_STATE state);     /* bytes in the FIFO, held or not */

/* BIOS ring ops, all operating on the guest's buffer at 0040:001E.
   (push = UI thread; pop/peek = V86 thread; caller serialises.) */
INT  VddInputPush(PINPUT_STATE state, WORD key);   /* full -> discard (0), as the BIOS does */
INT  VddInputPop (PINPUT_STATE st, WORD *key);  /* 1 if a key was returned */
INT  VddInputPeek(PINPUT_STATE state, WORD *key);  /* 1 if a key is available */

/* raw scancode FIFO ops (ports 0x60/0x64) -- UI thread pushes, V86 drains. */
VOID VddInputPushScanCode(PINPUT_STATE state, BYTE scanCode);
/* #136: which key (and Shift) types character `ch` on the active layout -- the
   reverse of the BIOS translation, for typing text in (Edit > Paste). 0 = none. */
INT VddInputCharToKey(PCINPUT_STATE state, BYTE character, BYTE *scanCode, INT *isShift);
INT  VddInputScanCodePending(PCINPUT_STATE state);     /* 1 if a scancode waits   */

/* THE BIOS INT 09h HANDLER. Takes the byte out of the controller, re-asserts the line if
   more are queued, tracks the E0 prefix and the shift/ctrl/alt/lock state into 0040:0017,
   translates the make code to a BIOS keycode (AH=scancode, AL=ascii; AL=0 for the extended
   keys, which is what makes an arrow an arrow) and stores it in the ring at 0040:001E.
   It used to consume the byte and DISCARD it -- the FIFO drained, so keystrokes kept
   interrupting, but a guest that had not hooked INT 09h could never see a key at all. */
INT  VddInputBiosConsume(PINPUT_STATE state);         /* -> KB_ACT_* (#254)     */
/* ── #244: THE SAME HANDLER IN TWO HALVES, FOR THE INT 15h AH=4Fh INTERCEPT. ─────────
     An AT/PS/2 BIOS reads the byte, calls INT 15h AH=4Fh with it in AL and CF=1, and
     translates whatever AL comes back -- or nothing, if the hook returned CF=0. The
     call is guest code (bios_kbdact.asm), so the host splits consume() around it:
     fetch() takes the byte out of the controller exactly as consume() would (-1 if
     none is presented), translate() is the BIOS's view of a byte. consume() is
     fetch() + translate(), unchanged for every caller that does not need the hook. */
INT  VddInputBiosFetch(PINPUT_STATE state);           /* byte, or -1 for none   */
INT  VddInputBiosTranslate(PINPUT_STATE state, BYTE scanCode);   /* -> KB_ACT_*    */
/* ── #274: WHAT THE KEYBOARD SENDS FOR ONE HOST KEY EVENT. ────────────────────────────
     Most keys are `[E0] code` on the press and `[E0] code|80h` on the release. Two are
     not, and Win32 hides both behind ordinary-looking key messages:
       Pause       Win32: scan 45h, NOT extended (NumLock is 45h WITH the bit).
                   The keyboard sends E1 1D 45 E1 9D C5 on the PRESS and nothing at all
                   on the release, and never repeats it.
       Ctrl+Break  Win32: VK_CANCEL, scan 46h extended. The keyboard sends E0 46 E0 C6
                   on the PRESS and nothing on the release, and never repeats it.
     (IBM PS/2 Keyboard TR, "Scan code set 1"; RBIL PORTS.LST 60h.) Writes the bytes
     to out[] (room for 6) and returns how many; *no_repeat is set for the two keys
     whose make must not be auto-repeated. ⚠ The Win32 side of the mapping (45h not
     extended = Pause) is from the documentation and this file's own NumLock note,
     not measured on the rig. */
INT  VddInputHostKeyBytes(BYTE rawScanCode, INT isExtended, INT isBreak,
                              BYTE bytes[INPUT_HOST_KEY_BYTES_MAX], INT *isNoRepeat);

INT  VddInputInitialize(PVDD_BUS bus, PVOID context);          /* claims INT 16h          */
VOID VddInputReset(PVOID context);

/* ── A20, AND WHY IT IS EXPOSED. (docs/ref/kbc.md 4) ─────────────────────────
     The 8042's output port bit 1, System Control Port A bit 1 (port 92h) and the
     XMS driver's AH=03h..07h are three doors onto ONE wire. The controller owns
     the bit; the host's XMS arm reads and writes it through these so the three
     answers cannot drift apart -- a guest that opened the gate the hardware way
     and then asked XMS used to be told it was shut.
   ⚠ THIS IS THE FLAG, NOT THE ADDRESS WRAP. Not modelling the wrap is a separate
     decision, recorded in dos_xms.h and main.c, and it still stands. */
VOID VddInputSetA20(PINPUT_STATE state, INT isOn);
INT  VddInputGetA20(PCINPUT_STATE state);
static inline NTVDD_DEVICE VddInputDevice(PINPUT_STATE state)
{ NTVDD_DEVICE device; device.Name = INPUT_DEVICE_NAME; device.Initialize = VddInputInitialize; device.Reset = VddInputReset;
  device.Shutdown = 0; device.Context = state; return device; }

#endif /* NTVDMEX_VDD_INPUT_H */
