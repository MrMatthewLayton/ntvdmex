/* host_bios.c -- the BIOS and the devices it fronts: serial and parallel ports, keyboard actions,
 *   INT 15h, print screen, and the BIOS BOP dispatcher.
 *
 * Its own translation unit (#335): declared in host_bios.h. */
#include "host_state.h"
#include "log.h"
#include "bios_kbdact.h"
#include "host_bios.h"
#include "main.h"
#include "host_video.h"
#include "host_dos.h"
#include "host_mouse.h"
#include "host_timing.h"


NETBIOS_STATE    g_Net;       NTVDD_DEVICE g_NetDevice;    /* GH #8 (s91) */
BYTE      g_GenericStubVector[DOS_GENSTUB_N];  /* #315: which vector each generic stub is */
/* Serial debug sink (DPMI harness): COM1 is captured by QEMU (-serial file:vm/serial.log)
   so the host reads the log directly -- no GUI screendump, no stale-host ambiguity. */
static HANDLE g_Serial = INVALID_HANDLE_VALUE;
/* GH #45: LPT1 is a spool file. Opened lazily on the first byte printed, so a
   run that never prints leaves no file behind to confuse the next one.
 ⚠ DO NOT CALL IT LPT1.PRN. `LPT1` is a RESERVED WIN32 DEVICE NAME -- reserved
   with any extension, in any directory -- so CreateFileA("C:\ntvdmex\LPT1.PRN")
   opens the actual parallel port rather than a file, and fails when nothing is
   attached. Measured on the rig: INT 17h reported ready, the probe passed every
   status check, and no file existed. Same list bites CON, PRN, AUX, NUL and
   COM1-9. The name below is deliberately not on it. */
static HANDLE g_Lpt = INVALID_HANDLE_VALUE;
static INT    g_LptFailed = 0;      /* opened once and could not: stop retrying */
#define LPT_SPOOL_PATH OUT_("PRINTOUT.TXT")
/* ── GH #9: WHAT THE GUEST'S COM PORTS TRANSMIT INTO. ────────────────────────
     The same spool shape as LPT1 above, and for the same reason: a file is a
     real device as far as a DOS program can tell, and it is inspectable after
     the run, which "discarded" is not.
   ⚠⚠ AND IT IS DELIBERATELY NOT g_Serial. That handle is the HOST'S OWN debug
     channel -- SerialOut() writes our log to it -- so pointing the guest's
     COM1 at it would interleave guest bytes with our diagnostics and corrupt
     both. The two things are both called "COM1" and are not the same port; the
     guest's is virtual hardware, ours is where this host talks to the outside.
   ⚠ The RESERVED-DEVICE-NAME trap in the LPT note above applies here word for
     word: COM1-9 are reserved with any extension, in any directory. These names
     are deliberately not on that list. */
HANDLE g_ComSpool[COMM_MAX_PORTS];
INT    g_ComFailed[COMM_MAX_PORTS];
/* Composed at open time (the root is runtime-derived now, see NTVDMEX_DIR). */
/* ⚠ One name per VDD slot, fitted or not (GH #181 grew the slots to four): a
     slot with no name would hand OUT_() a NULL the first time a later change
     fits COM3 and a guest transmits on it. */
static PCSTR const g_ComSpoolName[COMM_MAX_PORTS] =
    { "SERIAL1.TXT", "SERIAL2.TXT", "SERIAL3.TXT", "SERIAL4.TXT" };
#define COMM_SPOOL_PATH(i) OUT_(g_ComSpoolName[i])
/* Opened lazily on the first byte, so a run that never transmits leaves no file
   to confuse the next one -- and flushed per byte, because a VDM is far more
   often killed than exited and an unflushed buffer would read as "nothing was
   ever sent". Both rules are the LPT spool's, learned there. */
VOID ComTransmitSink(PVOID context, INT port, BYTE byteValue)
{
    (VOID)context;
    if (port < 0 || port >= COMM_MAX_PORTS || g_ComFailed[port]) return;
    if (g_ComSpool[port] == INVALID_HANDLE_VALUE) {
        g_ComSpool[port] = CreateFileA(COMM_SPOOL_PATH(port), GENERIC_WRITE,
                                        FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                                        FILE_ATTRIBUTE_NORMAL, NULL);
        if (g_ComSpool[port] == INVALID_HANDLE_VALUE) { g_ComFailed[port] = 1; return; }
    }
    { DWORD bytesWritten = 0; WriteFile(g_ComSpool[port], &byteValue, 1, &bytesWritten, NULL);
      FlushFileBuffers(g_ComSpool[port]); }
}
/* ── THE EQUIPMENT WORD IS A CLAIM ABOUT HARDWARE, SO COMPUTE IT FROM THE
     HARDWARE. (GH #9, session 56) ──────────────────────────────────────────
     Two arms answer INT 11h -- one in PM, one in V86 -- and both used the bare
     constant 0x4021 with a comment reading "one floppy, 80x25 colour, ONE
     SERIAL, one parallel". The value does not say that. Bits 9-11 are the
     serial count and they are 0 in 0x4021; the comment had been wrong at both
     sites, and the 0040:0000 note further down quotes the same wrong reading.
     That is exactly the disagreement between the equipment word and the port
     base table that cost COMM.DRV a session -- the wrong half was just being
     read out of a comment rather than out of memory.
     So derive the serial count from the VDD that actually claimed the ports.
     bit0 floppy present, bits4-5 video (10b = 80x25 colour), bits6-7 floppy
     count-1, bits9-11 serial ports, bits14-15 parallel ports, bit1 coprocessor
     (CLEAR -- krnl386 reads bit 1 at start-up and its coprocessor answer
     follows it, so the two modes must not disagree; sharing one function is now
     the mechanism rather than the intention). */
/* ★ Whether the guest is told a math coprocessor is installed -- the `Fpu` row
     of the settings dialog, kept as a plain flag because the equipment word is
     built long before the settings struct exists in this file. SettingsApply()
     is the only writer. */
static INT g_FpuPresent = 1;

WORD BiosEquipmentWord(VOID)
{
    WORD equipment = BIOS_EQUIPMENT_ONE_PARALLEL | BIOS_EQUIPMENT_VIDEO_80X25_COLOUR | BIOS_EQUIPMENT_FLOPPY;                      /* floppy, 80x25 colour, 1 parallel  */
    INT portCount = 0, index;
    for (index = 0; index < COMM_MAX_PORTS; ++index) if (VddCommIsFitted(&g_Comm, index)) ++portCount;
    equipment = (WORD)((equipment & ~BIOS_EQUIPMENT_SERIAL_MASK_U) | ((DWORD)(portCount & BIOS_EQUIPMENT_SERIAL_COUNT_MASK) << BIOS_EQUIPMENT_SERIAL_SHIFT));
    /* ── ★ BIT 1 IS "A MATH COPROCESSOR IS INSTALLED", AND IT WAS ALWAYS CLEAR.
         (session 57, GH #136) The guest runs 16-bit code on the REAL CPU, which
         has had an FPU since the 486DX -- so answering "no coprocessor" was not
         a conservative default, it was a wrong one, and a program that asks
         before using x87 was being sent down its emulator path for no reason.
       ⚠ AND IT IS THE `Fpu` SETTING'S FIRST EFFECT. That row has existed since
         the settings dialog did and consulted nothing; this is the line that
         makes it mean something. Unticking it now tells the guest what the host
         used to tell it unconditionally, which is a real configuration -- a
         program can be forced onto its emulator to compare the two. */
    if (g_FpuPresent) equipment |= BIOS_EQUIPMENT_FPU;
    /* Bit 12: game adapter installed. The CARD exists iff the JoystickType
       setting says so; whether a stick is plugged into it is the port's
       business (an empty gameport still answers), not the equipment word's. */
    if (g_Joystick.Type != JOYSTICK_TYPE_NONE) equipment |= BIOS_EQUIPMENT_GAMEPORT;
    return equipment;
}
enum { SERIAL_WRITE_TIMEOUT_MS = 250 };   /* SerialInitialize: per write */
/* ── ★ AND 0040:0010 SAYS THE SAME THING. (GH #253) ───────────────────────────────
     A real BIOS's INT 11h is a read of 0040:0010; ours computes, so the BDA copy has to
     be WRITTEN from this function or the two doors disagree (see bios_bda.h). Written
     once at start-up by BiosBdaInitialize(), and again here whenever a setting that feeds
     the word changes while the guest runs (the joystick type -> bit 12).
   ⚠ g_BdaReady gates it: SettingsApply() first runs at the top of WinMain, before
     VdmRegisterWithKernel() has committed the guest's low memory, and a write to linear 0x410 then
     would fault the host. It is set by the start-up block that calls BiosBdaInitialize(). */
INT g_BdaReady;
VOID BiosBdaRefreshEquipment(VOID)
{
    if (g_BdaReady) BiosBdaSetEquipment(NULL, BiosEquipmentWord());
}
VOID SerialInitialize(VOID)
{
    DCB deviceControlBlock;
    COMMTIMEOUTS timeouts;
    g_Serial = CreateFileA(HOST_DEVICE_COM1, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (g_Serial == INVALID_HANDLE_VALUE) return;
    { UINT index; PSTR cursor = (PSTR)&deviceControlBlock; for (index = 0; index < sizeof deviceControlBlock; ++index) cursor[index] = 0; }
    deviceControlBlock.DCBlength = sizeof deviceControlBlock;
    if (GetCommState(g_Serial, &deviceControlBlock)) {
        deviceControlBlock.BaudRate = CBR_115200; deviceControlBlock.ByteSize = BITS_PER_BYTE; deviceControlBlock.Parity = NOPARITY; deviceControlBlock.StopBits = ONESTOPBIT;
        /* ⚠ TURN FLOW CONTROL OFF EXPLICITLY. This used to keep whatever the driver's
             default DCB said, and on a REAL serial port with no cable a handshake line
             that never asserts makes WriteFile wait for a peer that does not exist.
             The dev VM has COM1 captured by QEMU and never showed it; the bare-metal
             rig has real hardware. A debug sink that can block the process it is
             instrumenting is worse than no sink. */
        deviceControlBlock.fOutxCtsFlow = FALSE; deviceControlBlock.fOutxDsrFlow = FALSE;
        deviceControlBlock.fDsrSensitivity = FALSE;
        deviceControlBlock.fOutX = FALSE; deviceControlBlock.fInX = FALSE;
        deviceControlBlock.fRtsControl = RTS_CONTROL_ENABLE;
        deviceControlBlock.fDtrControl = DTR_CONTROL_ENABLE;
        SetCommState(g_Serial, &deviceControlBlock);
    }
    /* And a hard write deadline, because the default comm timeouts are all zero,
       which means "wait forever". */
    { UINT index; PSTR cursor = (PSTR)&timeouts; for (index = 0; index < sizeof timeouts; ++index) cursor[index] = 0; }
    timeouts.WriteTotalTimeoutConstant = SERIAL_WRITE_TIMEOUT_MS;          /* ms, per write */
    SetCommTimeouts(g_Serial, &timeouts);
}
/* Write [buf..end) to COM1 (and it's already in the file log via log_*). */
VOID SerialOut(PCSTR buffer, PCSTR end)
{
    DWORD wrote;
    if (g_Serial != INVALID_HANDLE_VALUE && end > buffer) {
        WriteFile(g_Serial, buffer, (DWORD)(end - buffer), &wrote, NULL);
        FlushFileBuffers(g_Serial);
    }
}

/* One printer, reachable two ways. INT 17h and the 0x378 data/strobe registers
   are the SAME LPT1 -- a guest may use either, and some use both in one run --
   so they must share the spool rather than open it twice. Returns 0 if the byte
   went nowhere, which is what lets INT 17h report an I/O error instead of the
   ready status that once made the whole feature look like it worked. */
INT LptSpoolPut(BYTE character)
{
    DWORD bytesWritten = 0;
    if (g_LptFailed) return 0;
    if (g_Lpt == INVALID_HANDLE_VALUE) {
        g_Lpt = CreateFileA(LPT_SPOOL_PATH, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (g_Lpt == INVALID_HANDLE_VALUE) {
            CHAR lineBuffer[128], *lineCursor = lineBuffer;
            g_LptFailed = 1;
            lineCursor = LogPut(lineCursor, "  LPT1 spool OPEN FAILED err=0x");
            lineCursor = LogHex(lineCursor, GetLastError());
            lineCursor = LogPut(lineCursor, " -> reporting I/O error, not ready\r\n");
            LogAppend(LOG_PATH, lineBuffer, lineCursor); SerialOut(lineBuffer, lineCursor);
            return 0;
        }
    }
    WriteFile(g_Lpt, &character, 1, &bytesWritten, NULL);
    FlushFileBuffers(g_Lpt);          /* the run may be killed, not exited */
    return 1;
}
VOID LptTransmitSink(PVOID context, INT port, BYTE byteValue)
{ (VOID)context; (VOID)port; LptSpoolPut(byteValue); }
/* #254: which bios_kbdact.asm entry a KB_ACT_* runs, or -1 for none.
   ⛔ NEVER INTO A ROM BOP. A fresh VDM leaves some vectors on the VDM's own ROM at
     F000, and a handler there that is an NTVDM BOP (`C4 C4 xx`) is not ours: the exec
     loop refuses a guest-origin BOP and ENDS THE RUN. So INT 1Bh / INT 05h are called
     only when the vector has left the ROM (hooked by a guest, a TSR or a DOS) and
     does not land on such a BOP. INT 15h is always ours (DOS_CTAB_SEG). */
INT KeyboardActionEntry(INT keyboardAction)
{
    UINT vector;
    switch (keyboardAction) {
    case INPUT_ACTION_BREAK:  vector = VECTOR_CTRL_BREAK; break;
    case INPUT_ACTION_PRINT_SCREEN:  vector = VECTOR_PRINT_SCREEN; break;
    case INPUT_ACTION_SYSREQ_DOWN: return BIOS_KEYBOARD_ACTION_SYSREQ_DOWN;
    case INPUT_ACTION_SYSREQ_UP: return BIOS_KEYBOARD_ACTION_SYSREQ_UP;
    case INPUT_ACTION_PAUSE:  return BIOS_KEYBOARD_ACTION_PAUSE;
    default:            return -1;
    }
    {   WORD offset = *(volatile WORD *)(ULONG_PTR)(IVT_OFFSET_ADDRESS(vector));
        WORD segment = *(volatile WORD *)(ULONG_PTR)(IVT_SEGMENT_ADDRESS(vector));
        const volatile BYTE *target = (const volatile BYTE *)(ULONG_PTR)(((DWORD)segment << PARAGRAPH_SHIFT) + offset);
        if ((segment | offset) == 0) return -1;
        /* MEASURED (p_ivtkbd, rig): a fresh VDM has IVT[05h] = F000:FF54 (`E9 3D A4`, a
           jump deeper into the VDM's ROM) and IVT[1Bh] = F000:FF53 (`CF`). The jump's
           target is not ours to vouch for, so the VDM's ROM is never entered from
           here: the call is made only once a guest, a TSR or a DOS has hooked the
           vector -- which is the case Ctrl-Break / Print Screen handling exists for. */
        if (segment >= BIOS_ROM_SEGMENT) return -1;
        if (target[0] == VDM_BOP0 && target[1] == VDM_BOP1 && segment != DOS_HDLR_SEG && segment != DOS_CTAB_SEG) return -1;
    }
    return keyboardAction == INPUT_ACTION_BREAK ? BIOS_KEYBOARD_ACTION_BREAK : BIOS_KEYBOARD_ACTION_PRINT_SCREEN;
}

INT Int15Hooked(VOID)
{
    return *(volatile WORD *)(ULONG_PTR)(IVT_SEGMENT_ADDRESS(VECTOR_SYSTEM)) != DOS_CTAB_SEG
        || *(volatile WORD *)(ULONG_PTR)(IVT_OFFSET_ADDRESS(VECTOR_SYSTEM)) != g_Int15StubOffset;
}
enum { PRINT_SCREEN_SAVED_REGISTERS = 9 };   /* PrintScreenBop: what the INT 05h sequencer preserves */
/* ── #274: THE HOST HALF OF THE DEFAULT INT 05h (bios_kbdact.asm p5; bios_prtsc.h). ───
     begin: if a print is already running -> CF=1, nothing touched (the nested INT 05h
     the BIOS's busy byte exists to refuse). Otherwise save the registers the loop and
     the guest's INT 17h may disturb, read the mode and cursor through OUR INT 10h, and
     hand back the first byte. next: judge INT 17h's AH, hand back the next byte. At the
     end the cursor goes back, every saved register is restored, CF=1.
   ⛔⛔ THE STATUS BYTE IS NOT AT 0050:0000 HERE, AND CANNOT BE WITHOUT A LAYOUT CHANGE.
     0050:0000 is DOS_HDLR_SEG:0000 -- the first byte of OUR INT 21h stub (`C4 C4 20 CF`,
     IVT[21h] = 0050:0000). Writing the IBM status values there (01h busy, 00h done) would
     turn every INT 21h into garbage the moment someone pressed Print Screen. So the
     status lives in g_PrintScreenStatus (same values, same meaning) and the guest reads C4h
     at 0050:0000. ⚠ The converse hazard predates this: a program that DISABLES print
     screen the classic way, `mov byte [0050:0000],1`, overwrites our INT 21h stub. Moving
     the stub off 0050:0000 is its own change (IVT[21h], the PM INT 21h paths, WOW) --
     filed in the #274 report, not done in passing. */
static BIOS_PRINT_SCREEN_JOB g_PrintScreen;
static DWORD      g_PrintScreenSaved[PRINT_SCREEN_SAVED_REGISTERS];
static VOID PrintScreenInt10(NTVDD_REGISTERS *registers)
{
    HOST_LOCK();
    VddBusDeliverInterrupt(&g_Bus, VECTOR_VIDEO, registers);
    HOST_UNLOCK();
}
static BYTE PrintScreenReadCharacter(PVOID context, BYTE row, BYTE column)
{
    NTVDD_REGISTERS registers;
    (VOID)context;
    ZeroMemory(&registers, sizeof registers);
    registers.Eax = VIDEO_FUNCTION_SET_CURSOR << BYTE_SHIFT; registers.Ebx = (DWORD)g_PrintScreen.Page << BYTE_SHIFT; registers.Edx = ((DWORD)row << BYTE_SHIFT) | column;
    PrintScreenInt10(&registers);                                   /* set cursor  */
    registers.Eax = VIDEO_FUNCTION_READ_CHARACTER << BYTE_SHIFT; registers.Ebx = (DWORD)g_PrintScreen.Page << BYTE_SHIFT;
    PrintScreenInt10(&registers);                                   /* read cell   */
    return (BYTE)registers.Eax;
}
static VOID PrintScreenBop(volatile BYTE *tib, INT begin)
{
    static const INT savedRegisters[PRINT_SCREEN_SAVED_REGISTERS] = { VTIB_EAX, VTIB_EBX, VTIB_ECX, VTIB_EDX, VTIB_ESI,
                               VTIB_EDI, VTIB_EBP, VTIB_DS, VTIB_ES };
    BYTE character = 0;
    INT status, index;
    if (begin) {
        NTVDD_REGISTERS registers;
        if (g_PrintScreen.IsActive) { VDM_REG(tib, VTIB_EFLAGS) |= EFLAGS_CF_U; return; }
        for (index = 0; index < PRINT_SCREEN_SAVED_REGISTERS; ++index) g_PrintScreenSaved[index] = VDM_REG(tib, savedRegisters[index]);
        g_PrintScreenStatus = BIOS_PRINT_SCREEN_STATUS_BUSY;
        ZeroMemory(&registers, sizeof registers);
        registers.Eax = VIDEO_FUNCTION_GET_MODE << BYTE_SHIFT; PrintScreenInt10(&registers);               /* AH = columns, BH = page */
        {   BYTE cols = (BYTE)(registers.Eax >> BYTE_SHIFT), page = (BYTE)(registers.Ebx >> BYTE_SHIFT);
            ZeroMemory(&registers, sizeof registers);
            registers.Eax = VIDEO_FUNCTION_GET_CURSOR << BYTE_SHIFT; registers.Ebx = (DWORD)page << BYTE_SHIFT; PrintScreenInt10(&registers);
            BiosPrintScreenBegin(&g_PrintScreen, cols, *(volatile BYTE *)(ULONG_PTR)(BIOS_BDA_BASE + BIOS_BDA_VIDEO_ROWS), page,
                             (WORD)registers.Edx); }
        ++g_PrintScreenJobs;
        status = BiosPrintScreenStep(&g_PrintScreen, 0, PrintScreenReadCharacter, 0, &character);
    } else {
        if (!g_PrintScreen.IsActive) { VDM_REG(tib, VTIB_EFLAGS) |= EFLAGS_CF_U; return; }
        status = BiosPrintScreenStep(&g_PrintScreen, (BYTE)(VDM_REG(tib, VTIB_EAX) >> BYTE_SHIFT),
                             PrintScreenReadCharacter, 0, &character);
    }
    if (status == BIOS_PRINT_SCREEN_STEP_EMIT) {
        VDM_REG(tib, VTIB_EAX) = (VDM_REG(tib, VTIB_EAX) & HIGH_WORD_MASK_U) | character;  /* AH=00h */
        VDM_REG(tib, VTIB_EDX) &= HIGH_WORD_MASK_U;                               /* LPT1   */
        VDM_REG(tib, VTIB_EFLAGS) &= ~EFLAGS_CF_U;
        return;
    }
    {   NTVDD_REGISTERS registers;
        ZeroMemory(&registers, sizeof registers);
        registers.Eax = VIDEO_FUNCTION_SET_CURSOR << BYTE_SHIFT; registers.Ebx = (DWORD)g_PrintScreen.Page << BYTE_SHIFT; registers.Edx = g_PrintScreen.Cursor;
        PrintScreenInt10(&registers); }
    g_PrintScreenStatus = (status == BIOS_PRINT_SCREEN_STEP_ERROR) ? BIOS_PRINT_SCREEN_STATUS_ERROR : BIOS_PRINT_SCREEN_STATUS_OK;
    if (status == BIOS_PRINT_SCREEN_STEP_ERROR) ++g_PrintScreenErrors;
    for (index = 0; index < PRINT_SCREEN_SAVED_REGISTERS; ++index) VDM_REG(tib, savedRegisters[index]) = g_PrintScreenSaved[index];
    VDM_REG(tib, VTIB_EFLAGS) |= EFLAGS_CF_U;
}
static UINT      g_DiskCount = 1;   /* AH=08h's DL: how many floppy drives */
static UINT      g_DiskStatus;      /* AH=01h's last-status byte           */

/* ── #238: THE PACER WOKE 485 TIMES A SECOND, NOT 1000. (s85) ─────────────────────────
     Sleep(1) on XP, even under timeBeginPeriod(1), sleeps until the SECOND timer
     interrupt -- ~2 ms. Measured on the rig (3DBench, runs/s85/3db/pace): 483-494 wakes
     per second, every second. And the pacer is the only thread whose async attempt can
     land on a guest that runs without trapping: the exec thread's own attempts bail
     `not_in_exec` by construction (it cannot suspend itself), and the cooperative gate
     only sees the guest at a trap. One wake places at most one tick, so a 1 kHz timer
     got ~500/s and its program's clock ran at half speed.
   ► A periodic multimedia timer fires on the 1 ms period itself; TIME_CALLBACK_EVENT_SET
     makes its callback a SetEvent, so nothing of ours runs on winmm's thread. Bound by
     name like every other winmm call. If it cannot be had, the Sleep loop is unchanged. */
/* ── #206: INT 15h AH=83h/86h, THE AT BIOS'S TWO TIMED WAITS. ─────────────────────────
     AH=86h waits CX:DX microseconds and returns; AH=83h starts the same countdown and
     returns at once, and when it runs out the BIOS sets bit 7 of the caller's flag byte
     (ES:BX), which the caller polls. A real BIOS counts them on the RTC's periodic
     interrupt; one of them at a time, and a second request while one runs is refused
     (CF=1, AH=83h, "busy"). The BIOS data area mirrors it: 40:98 the flag's far
     pointer, 40:9C the microsecond count, 40:A0 the wait-active flag.
   ► AH=86h used to return at once ("the PIT already paces us"), so a program using it
     as a delay got none. It now re-executes its BOP until the deadline, like INT 16h's
     blocking read, so interrupts are still taken while it waits.
   ► AH=83h is posted from the pacer thread (1 kHz), because the caller polls MEMORY and
     need not trap at all while it does. */
volatile LONGLONG g_Int15WaitEnd;    /* QPC of the AH=86h deadline; 0 = none     */
volatile DWORD    g_Int15EventLinear;     /* linear address of its flag byte          */
/* ── FILE > CLOSE PROGRAM, THE EXEC-THREAD HALF. (GH #152) ─────────────────────────
     See g_ExecMachine. The UI only raises g_CloseRequest; the exec thread takes it at the
     top of its loop (V86) or of the PM loop, where the guest is stopped and no BOP is
     half-answered -- the same boundary every injected IRQ uses. */
/* ── INT 15h AH=87h: MOVE EXTENDED MEMORY BLOCK. (GH #54) ──────────────────────────
     ES:SI -> the caller's GDT, CX = words (at most 8000h). Every address goes through
     DosExtMemResolve -- see dos_extmem.h for why a guest's linear address above the HMA
     is NOT something we may simply write to. Returns the AH status: 00 done, 02 (the
     BIOS's "exception interrupt error") for anything unresolvable. memmove, because a
     caller may overlap source and destination and a real BIOS copies forwards through
     a descriptor pair that does not care. */
static BYTE *g_ExtendedMemoryRaw;                  /* AH=88h's 15 MB, allocated on first use */
static DWORD    g_Int15Function87Count, g_Int15Function87Refused;
/* `gdt_lin` = where the caller's 48-byte GDT is: ES:SI in V86; in PM the base of the
   selector in ES plus (E)SI (#244, the PM arm). */
UINT Int15MoveBlockAt(volatile BYTE *tib, DWORD gdtLinear)
{
    DWORD cx = VDM_REG16(tib, VTIB_ECX), length = cx * X86_WORD_SIZE;
    const volatile BYTE *gdt = (const volatile BYTE *)(ULONG_PTR)gdtLinear;
    UINT32 source, destination;
    BYTE *sourcePointer, *destinationPointer;
    UINT status = 0;
    if (cx == 0) return 0;                      /* nothing to move: done */
    if (cx > BIOS_MOVE_BLOCK_WORDS_MAX_U) { status = BIOS_MOVE_BLOCK_EXCEPTION; goto out; }  /* past the 64 KB a descriptor spans */
    if (!gdt) { status = BIOS_MOVE_BLOCK_EXCEPTION; goto out; }          /* PM: the GDT pointer did not resolve */
    source = DosExtMemDescriptorBase(gdt + BIOS_MOVE_BLOCK_SOURCE);
    destination = DosExtMemDescriptorBase(gdt + BIOS_MOVE_BLOCK_DESTINATION);
    if (!g_ExtendedMemoryRaw && (DosExtMemClassify(&g_Xms, source, length) == DOS_EXTMEM_REGION_RAW
                          || DosExtMemClassify(&g_Xms, destination, length) == DOS_EXTMEM_REGION_RAW))
        g_ExtendedMemoryRaw = (BYTE *)VirtualAlloc(NULL, DOS_EXTMEM_RAW_LENGTH,
                                               MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    sourcePointer = DosExtMemResolve(&g_Xms, 0, g_ExtendedMemoryRaw, source, length);
    destinationPointer = DosExtMemResolve(&g_Xms, 0, g_ExtendedMemoryRaw, destination, length);
    if (!sourcePointer || !destinationPointer) { status = BIOS_MOVE_BLOCK_EXCEPTION; goto out; }
    MoveMemory(destinationPointer, sourcePointer, length);
out:
    ++g_Int15Function87Count;
    if (status) ++g_Int15Function87Refused;
    if (g_Int15Function87Count <= 8 || status) {                /* the first few, and every refusal */
        static DWORD saidRefused = 0;
        if (!status || ++saidRefused <= 16) {
            CHAR buffer[160], *cursor = buffer;
            cursor = LogPut(cursor, "  INT15 AH=87h move 0x"); cursor = LogHex(cursor, length);
            cursor = LogPut(cursor, " bytes src=0x"); cursor = LogHex(cursor, gdt ? DosExtMemDescriptorBase(gdt + BIOS_MOVE_BLOCK_SOURCE) : 0);
            cursor = LogPut(cursor, " dst=0x");       cursor = LogHex(cursor, gdt ? DosExtMemDescriptorBase(gdt + 0x18) : 0);
            cursor = LogPut(cursor, status ? " -> REFUSED (unresolvable range), AH=02\r\n" : " -> done\r\n");
            LogAppend(LOG_PATH, buffer, cursor);
        }
    }
    return status;
}
static UINT Int15MoveBlock(volatile BYTE *tib)
{
    DWORD es = VDM_REG16(tib, VTIB_ES), si = VDM_REG16(tib, VTIB_ESI);
    return Int15MoveBlockAt(tib, (es << PARAGRAPH_SHIFT) + si);
}

WORD g_DosMemoryTop  = (WORD)DOS_MEM_TOP;
/* --- guest register view <-> VDM_TIB CONTEXT (for bus interrupt dispatch) --- */
VOID RegistersLoad(NTVDD_REGISTERS *registers, volatile BYTE *tib)
{
    registers->Eax = VDM_REG(tib, VTIB_EAX); registers->Ebx = VDM_REG(tib, VTIB_EBX);
    registers->Ecx = VDM_REG(tib, VTIB_ECX); registers->Edx = VDM_REG(tib, VTIB_EDX);
    registers->Esi = VDM_REG(tib, VTIB_ESI); registers->Edi = VDM_REG(tib, VTIB_EDI);
    registers->Ebp = VDM_REG(tib, VTIB_EBP);
    registers->Ds = (WORD)VDM_REG(tib, VTIB_DS); registers->Es = (WORD)VDM_REG(tib, VTIB_ES);
    registers->CarryFlag = 0;
}
/* STORE EVERYTHING LOAD READS. This wrote back only the four general registers, so any
   service whose ANSWER is a pointer silently threw that answer away: INT 10h AH=11h AL=30h
   returns the character generator in ES:BP, and the guest got back whatever ES:BP it
   happened to be holding -- so it drew its text out of an arbitrary chunk of memory. That
   is the "garbled text" in Skyroads, and it is why fixing the handler to SET ES:BP (and
   later replacing the font tables themselves) changed nothing: neither value ever reached
   the guest. Same silent loss applied to every ES:DI and DS:SI answer (VESA info blocks,
   INT 33h, INT 10h 1Bh). RegistersLoad already reads all seven, so writing all seven back is
   symmetric: a handler that does not touch one stores the value it was given. */
VOID RegistersStore(NTVDD_REGISTERS *registers, volatile BYTE *tib)
{
    VDM_REG(tib, VTIB_EAX) = registers->Eax; VDM_REG(tib, VTIB_EBX) = registers->Ebx;
    VDM_REG(tib, VTIB_ECX) = registers->Ecx; VDM_REG(tib, VTIB_EDX) = registers->Edx;
    VDM_REG(tib, VTIB_ESI) = registers->Esi; VDM_REG(tib, VTIB_EDI) = registers->Edi;
    VDM_REG(tib, VTIB_EBP) = registers->Ebp;
    VDM_REG(tib, VTIB_DS)  = registers->Ds;  VDM_REG(tib, VTIB_ES)  = registers->Es;
}

#define V86BOP_DONE  1      /* serviced; EIP is past the BOP, onto the stub's IRET/RETF  */
#define V86BOP_RERUN 4      /* serviced, still waiting; EIP left ON the BOP to re-execute */
#define V86BOP_2F_LOG_LINES_MAX 512     /* BOP 2Fh lines logged before the cap        */
/* ── ★★★ OUR BIOS AND DRIVER STUBS, SERVICED FROM ONE PLACE. (GH #247) ─────────────────
     Every stub we plant in the IVT is `BOP nn ; IRET` (or RETF), and until #247 the only
     code that knew what each `nn` MEANS was the body of WinMain's exec loop. So a stub
     reached from anywhere ELSE -- a DPMI 0301h/0302h real-mode procedure that calls
     INT 16h, or 0300h simulating INT 1Ah -- arrived in the nested V86 loop as an
     "unexpected RM event" and the call was abandoned half-way. 0300h dodged that by
     never running our stubs at all: it serviced 21h/33h/10h host-side and returned
     every other vector's registers unchanged with CF=0.
   ► Moved here VERBATIM (the arms, their comments, their order) so the exec loop and the
     nested loop run one copy of each answer, and cannot drift apart about what
     INT 15h AH=88h returns depending on who asked.
   ⚠ NOT HERE, ON PURPOSE: INT 08h/09h and the INT 33h callback return (their EOI and
     their re-entry rules belong to IRQ delivery, which the nested loop does not do for
     our stubs -- see g_NestedRm), and INT 20h/27h (they END the program, which only the
     exec loop can do). A nested loop that meets one of those still stops and says so.
   Returns V86BOP_NONE (not one of these -- the caller carries on down its own chain),
   V86BOP_DONE (serviced, EIP past the BOP, onto the stub's IRET/RETF) or V86BOP_RERUN
   (serviced, still WAITING -- EIP left on the BOP so it re-executes: INT 16h AH=00h
   with no key, INT 15h AH=86h mid-countdown). */
#define V86BOP_RET(v) do { *logCursor = cursor; return (v); } while (0)
INT V86BiosBop(volatile BYTE *tib, UINT bopNumber, PSTR *logCursor, PSTR base)
{
    PSTR cursor = *logCursor;
    /* ── GH #8 (s91): INT 2Ah / INT 5Ch, the network interface, to whichever device
         claimed them (vdd_net.c). A bus claim on a vector with no stub behind it was
         never delivered: INT 14h works because its BOP is wired here by number, and
         a NetBIOS program's INT 5Ch went to an IRET. ⚠ Only from OUR stub -- 2Ah/5Ch
         are numbers a guest's own BOP may also use (the s78 rule). */
    /* s91 (#315): a GENERIC stub (vdd_plant_generic_ints) -- the slot it sits in names
         the vector; the bus delivers it to whoever claimed it. Only from our stub. */
    if (bopNumber == DOS_GENSTUB_BOP && VDM_REG16(tib, VTIB_CS) == DOS_CTAB_SEG) {
        DWORD ip = VDM_REG16(tib, VTIB_EIP);
        if (ip >= DOS_GENSTUB_OFF && ip < DOS_GENSTUB_OFF + DOS_GENSTUB_N * DOS_GENSTUB_SIZE) {
            NTVDD_REGISTERS registers; RegistersLoad(&registers, tib);
            HOST_LOCK();
            VddBusDeliverInterrupt(&g_Bus, g_GenericStubVector[(ip - DOS_GENSTUB_OFF) / DOS_GENSTUB_SIZE], &registers);
            HOST_UNLOCK();
            RegistersStore(&registers, tib);
            {   /* CF into the FLAGS the stub's IRET restores (an INT pushed them) */
                WORD *flagsWord = (WORD *)(ULONG_PTR)((VDM_REG16(tib, VTIB_SS) << PARAGRAPH_SHIFT)
                                    + ((VDM_REG16(tib, VTIB_ESP) + X86_FRAME16_FLAGS) & WORD_MASK));
                if (registers.CarryFlag) *flagsWord |= EFLAGS_CF; else *flagsWord &= (WORD)~EFLAGS_CF;
            }
            VDM_REG(tib, VTIB_EIP) += VDM_BOP_LENGTH;
            V86BOP_RET(V86BOP_DONE);
        }
    }
    /* #274: the default INT 05h's two BOP sites (bios_kbdact.asm p5). Here rather than in
         the exec loop's INT 09h arm because INT 05h is a SOFTWARE interrupt a program may
         issue from anywhere, including a DPMI 0300h -- so the nested loop must serve it
         too. Same number as the INT 09h stub's BOP, told apart by address. */
    if (bopNumber == DOS_BOP_FOR_VECTOR(VECTOR_KEYBOARD) && VDM_REG16(tib, VTIB_CS) == DOS_CTAB_SEG) {
        DWORD ip = VDM_REG16(tib, VTIB_EIP);
        if (ip == DOS_KBDACT_OFF + BIOS_KEYBOARD_ACTION_DEFAULT_INT05_BEGIN || ip == DOS_KBDACT_OFF + BIOS_KEYBOARD_ACTION_DEFAULT_INT05_NEXT) {
            PrintScreenBop(tib, ip == DOS_KBDACT_OFF + BIOS_KEYBOARD_ACTION_DEFAULT_INT05_BEGIN);
            VDM_REG(tib, VTIB_EIP) += VDM_BOP_LENGTH;
            V86BOP_RET(V86BOP_DONE);
        }
    }
    if ((bopNumber == DOS_BOP_FOR_VECTOR(VECTOR_NETWORK) || bopNumber == DOS_BOP_FOR_VECTOR(VECTOR_NETBIOS)) && VDM_REG16(tib, VTIB_CS) == DOS_CTAB_SEG) {
        NTVDD_REGISTERS registers; RegistersLoad(&registers, tib);
        HOST_LOCK();
        VddBusDeliverInterrupt(&g_Bus, (BYTE)bopNumber, &registers);
        HOST_UNLOCK();
        RegistersStore(&registers, tib);
        VDM_REG(tib, VTIB_EIP) += VDM_BOP_LENGTH;
        /* A no-wait NCB's POST routine (vdd_net.h): put one more interrupt frame on
           the guest's stack, below the caller's, so the stub's IRET enters POST with
           ES:BX = the NCB and POST's own IRET returns to the caller. FLAGS = the
           caller's with IF clear, as a hardware interrupt would enter it. */
        if (g_Net.IsPostPending) {
            DWORD ss = VDM_REG16(tib, VTIB_SS), sp = VDM_REG16(tib, VTIB_ESP);
            volatile WORD *frame = (volatile WORD *)(ULONG_PTR)((ss << PARAGRAPH_SHIFT) + sp);
            WORD flags = frame[X86_FRAME16_FLAGS_WORD];
            WORD newSp = (WORD)(sp - X86_IRET16_SIZE);
            volatile WORD *newFrame = (volatile WORD *)(ULONG_PTR)((ss << PARAGRAPH_SHIFT) + newSp);
            g_Net.IsPostPending = 0;
            newFrame[X86_FRAME16_IP_WORD] = g_Net.PostOffset; newFrame[X86_FRAME16_CS_WORD] = g_Net.PostSegment; newFrame[X86_FRAME16_FLAGS_WORD] = (WORD)(flags & ~EFLAGS_IF);
            VDM_REG(tib, VTIB_ESP) = (VDM_REG(tib, VTIB_ESP) & HIGH_WORD_MASK_U) | newSp;
        }
        V86BOP_RET(V86BOP_DONE);
    }
    if (bopNumber == DOS_BOP_FOR_VECTOR(VECTOR_VIDEO)) {
        NTVDD_REGISTERS registers; RegistersLoad(&registers, tib);
        HOST_LOCK();
        VddBusDeliverInterrupt(&g_Bus, VECTOR_VIDEO, &registers);
        HOST_UNLOCK();
        Int10WaitAfter();                     /* #226: 4F07h BL=80h */
        RegistersStore(&registers, tib);
        VideoTrapSync();     /* mode 12h: interpret the guest (GH #55) */
        VDM_REG(tib, VTIB_EIP) += VDM_BOP_LENGTH;
        V86BOP_RET(V86BOP_DONE);
    }
    if (bopNumber == DOS_BOP_FOR_VECTOR(VECTOR_KEYBOARD_SERVICES)) {
        NTVDD_REGISTERS registers; BYTE int16Ah; RegistersLoad(&registers, tib); int16Ah = VddGetAh(&registers);
        HOST_LOCK();
        VddBusDeliverInterrupt(&g_Bus, VECTOR_KEYBOARD_SERVICES, &registers);
        HOST_UNLOCK();
        /* A blocking BIOS read with no key must NOT park the exec thread -- doing that
           stops the guest dead, so its timer, its music and its screen freeze until a key
           arrives. (Same fault as INT 21h AH=01/07/08, fixed the same way.) Leave EIP on
           the BOP instead: the guest re-executes INT 16h and keeps taking timer
           interrupts while it waits, which is what a real BIOS spin does. */
        if ((int16Ah == BIOS_KEYBOARD_READ || int16Ah == BIOS_KEYBOARD_READ_EXTENDED) && registers.ZeroFlag != 0 && g_Running) V86BOP_RET(V86BOP_RERUN);
        RegistersStore(&registers, tib);
        HostSetFlags(tib, registers.CarryFlag, registers.ZeroFlag);
        VDM_REG(tib, VTIB_EIP) += VDM_BOP_LENGTH;
        V86BOP_RET(V86BOP_DONE);
    }
    if (bopNumber == DOS_BOP_FOR_VECTOR(VECTOR_MOUSE)) {   /* INT 33h mouse  */
        MouseInt33(tib, I33_SRC_V86);
        VDM_REG(tib, VTIB_EIP) += VDM_BOP_LENGTH;
        V86BOP_RET(V86BOP_DONE);
    }
    {   /* ---- BIOS services: INT 11h/12h/13h/14h/15h/17h/25h/26h ---------
           GH #43/#44/#45. Every one of these was previously an IRET that
           returned whatever the caller already had in its registers.
           NOTE ON EVIDENCE: the 6.22 oracle is NOT truth here -- QEMU runs
           SeaBIOS, so a BIOS answer from it is another reimplementation's
           opinion (epic #24). The equipment word and memory size are
           statements about OUR virtual machine's configuration, which is
           ours to declare; the rest report "not present" honestly and log,
           rather than pretending hardware exists. */
        INT handled = 1;
        WORD *flagsPointer = (WORD *)((VDM_REG16(tib, VTIB_SS) << PARAGRAPH_SHIFT)
                              + ((VDM_REG16(tib, VTIB_ESP) + X86_FRAME16_FLAGS) & WORD_MASK));
        #define BCF_SET() (*flagsPointer |= EFLAGS_CF)
        #define BCF_CLR() (*flagsPointer &= (WORD)~EFLAGS_CF)
        #define BSETAX(v) (VDM_REG(tib, VTIB_EAX) = \
            (VDM_REG(tib, VTIB_EAX) & HIGH_WORD_MASK_U) | ((DWORD)(v) & WORD_MASK))
        if (bopNumber == DOS_BOP_FOR_VECTOR(VECTOR_EQUIPMENT)) {
            /* Equipment word -- see BiosEquipmentWord(). The serial count
               comes from the VDD that claimed the ports, so this and the
               0040:0000 port base table cannot drift apart. */
            BSETAX(BiosEquipmentWord());
            BCF_CLR();
        } else if (bopNumber == DOS_BOP_FOR_VECTOR(VECTOR_MEMORY_SIZE)) {
            /* KB of conventional memory. 640 CONTRADICTED OUR OWN MEMORY MAP
               once DOS_MEM_TOP moved to the real EBDA boundary: the MCB chain
               ends at 0x9FC0 and the PSP says 0x9FC0, which is 639K, while
               this still claimed 640. Real DOS reports 639 for exactly that
               reason -- the top 1KB is the Extended BIOS Data Area. Derived
               from the map rather than typed, so the two cannot drift. */
            BSETAX(BiosBaseKbOfTop(g_DosMemoryTop));   /* #253 EBDA; #136 the setting */
            BCF_CLR();
        } else if (bopNumber == DOS_BOP_FOR_VECTOR(VECTOR_SYSTEM)) {
            UINT int15Ah = (VDM_REG(tib, VTIB_EAX) >> BYTE_SHIFT) & BYTE_MASK;
            if (int15Ah == BIOS_SYSTEM_EXTENDED_MEMORY) {                /* extended memory, KB */
                /* ⚠ THIS ARM IS LOGGED BECAUSE ITS SILENCE COST A WRONG CONCLUSION.
                     A serviced call that leaves no trace is indistinguishable in a
                     log from one that never happened, and session 58 read exactly
                     that backwards twice while chasing ZAR (GH #23): the absence of
                     an AH=88h line was taken as "the guest never asks", then as
                     "the guest must ask". It does NOT ask -- DOS/16M installs its
                     OWN protected-mode INT 15h handler (that is what its 33 INT 31h
                     AX=0205 calls are for) and answers the extended-memory question
                     internally, so this arm is never reached by that guest at all.
                   ⚠ AND A REAL DEFECT IS RECORDED HERE RATHER THAN QUIETLY FIXED:
                     0x3C00 "matching the XMS pool" hands the SAME memory out twice
                     -- once here as raw extended memory a caller may take for
                     itself, and again through XMS. A real machine cannot do that,
                     because HIMEM.SYS hooks AH=88h and reports what is left after it
                     has claimed extended memory, which is ZERO. Changing it was
                     tried and REVERTED: it is inert for ZAR (never called) and is an
                     unvalidated behaviour change for every other guest. It is worth
                     doing deliberately, with Doom and the DOS batteries re-gated on
                     it -- on its own merits, not as a ZAR fix.
                   ★ #48: the number itself is now ONE number. It is CMOS_EXTENDED_KB, the same
                     figure CMOS 17h/30h and SysVars+0x45 report; the XMS pool is that
                     less the 64K HMA (XMS_POOL_KB). The double hand-out above is
                     unchanged -- this arm still does not answer 0 as HIMEM's hook would
                     (6.22's MEM: "Memory accessible using Int 15h 0"). */
                BSETAX((WORD)CMOS_EXTENDED_KB);     /* 15 MB -- the machine, not the XMS pool */
                BCF_CLR();
                { CHAR waitLine[128], *waitCursor = waitLine;
                  waitCursor = LogPut(waitCursor, "  INT15 AH=88h extended memory -> 0x3C00 KB\r\n");
                  LogAppend(LOG_PATH, waitLine, waitCursor); SerialOut(waitLine, waitCursor); }
            } else if (int15Ah == BIOS_SYSTEM_WAIT) {         /* wait CX:DX microseconds (#206) */
                DWORD microseconds = (VDM_REG16(tib, VTIB_ECX) << WORD_SHIFT) | VDM_REG16(tib, VTIB_EDX);
                LARGE_INTEGER now;
                QueryPerformanceCounter(&now);
                if (g_Int15EventEnd) {           /* an AH=83h event is counting: busy */
                    BSETAX((WORD)((VDM_REG(tib, VTIB_EAX) & BYTE_MASK) | (BIOS_SYSTEM_STATUS_BUSY << BYTE_SHIFT)));
                    BCF_SET(); ++g_Int15Busy;
                } else if (!g_Int15WaitEnd) {  /* first pass: start the countdown   */
                    if (microseconds == 0) BCF_CLR();
                    else { g_Int15WaitEnd = Int15QpcAfterMicroseconds(microseconds); ++g_Int15Waits; handled = V86BOP_RERUN; }
                } else if (now.QuadPart >= g_Int15WaitEnd) {
                    g_Int15WaitEnd = 0;        /* elapsed                           */
                    BCF_CLR();
                } else {
                    /* Still waiting: re-execute the BOP (handled = 4). Sleep when there
                       is time to, so a long wait does not burn the CPU the guest's own
                       interrupts and the host's threads need -- a 1 ms nap is far
                       below any wait a program asks this for. */
                    LARGE_INTEGER frequency; QueryPerformanceFrequency(&frequency);
                    if ((g_Int15WaitEnd - now.QuadPart) * MILLISECONDS_PER_SECOND > INT15_WAIT_SLEEP_MS * frequency.QuadPart) Sleep(1);
                    handled = V86BOP_RERUN;
                }
            } else if (int15Ah == BIOS_SYSTEM_EVENT_WAIT) {         /* event wait (#206) */
                UINT eventWaitAl = VDM_REG(tib, VTIB_EAX) & BYTE_MASK;
                if (eventWaitAl == BIOS_EVENT_WAIT_CANCEL) {            /* cancel                            */
                    g_Int15EventEnd = 0;
                    *(volatile BYTE *)(ULONG_PTR)(BIOS_BDA_BASE + BIOS_BDA_WAIT_ACTIVE) = BIOS_BDA_WAIT_NONE;
                    BCF_CLR();
                } else if (g_Int15EventEnd || g_Int15WaitEnd) {
                    BSETAX((WORD)((VDM_REG(tib, VTIB_EAX) & BYTE_MASK) | (BIOS_SYSTEM_STATUS_BUSY << BYTE_SHIFT)));
                    BCF_SET(); ++g_Int15Busy;   /* one countdown at a time           */
                } else {
                    DWORD microseconds = (VDM_REG16(tib, VTIB_ECX) << WORD_SHIFT) | VDM_REG16(tib, VTIB_EDX);
                    WORD es = (WORD)VDM_REG16(tib, VTIB_ES), bx = (WORD)VDM_REG16(tib, VTIB_EBX);
                    g_Int15EventLinear = ((DWORD)es << PARAGRAPH_SHIFT) + bx;
                    *(volatile WORD  *)(ULONG_PTR)(BIOS_BDA_BASE + BIOS_BDA_WAIT_FLAG_POINTER) = bx;    /* 40:98 flag pointer  */
                    *(volatile WORD  *)(ULONG_PTR)(BIOS_BDA_BASE + BIOS_BDA_WAIT_FLAG_SEGMENT) = es;
                    *(volatile DWORD *)(ULONG_PTR)(BIOS_BDA_BASE + BIOS_BDA_WAIT_COUNT) = microseconds;    /* 40:9C count, us     */
                    *(volatile BYTE  *)(ULONG_PTR)(BIOS_BDA_BASE + BIOS_BDA_WAIT_ACTIVE) = BIOS_BDA_WAIT_IN_PROGRESS;  /* 40:A0 wait active   */
                    ++g_Int15Events;
                    g_Int15EventEnd = Int15QpcAfterMicroseconds(microseconds ? microseconds : 1);
                    BCF_CLR();
                }
            } else if (int15Ah == BIOS_SYSTEM_SYSREQ) {         /* SysReq key (#254)              */
                /* The hook our INT 09h now calls on SysReq press (AL=0) / release
                   (AL=1). The BIOS's own default does nothing and returns AH=0,
                   CF=0 -- a multitasker hooks it. (p_kbd2 int15.85) */
                BSETAX((WORD)(VDM_REG(tib, VTIB_EAX) & BYTE_MASK));
                BCF_CLR();
            } else if (int15Ah == BIOS_SYSTEM_KEYBOARD_INTERCEPT) {         /* keyboard intercept (#206) */
                /* The default handler: CF=1 and AL untouched, "process this key".
                   A TSR that hooks INT 15h answers for itself. #244: our INT 09h CALLS
                   it once IVT[15h] is hooked (Int15Hooked, bios_kbdact.asm k4f); while
                   it is not, this answer is the one the call would get, so it is skipped. */
                BCF_SET();
            } else if (int15Ah == BIOS_SYSTEM_JOYSTICK) {
                /* ── BIOS joystick support (session 62). DX picks the half:
                     0 = switches (buttons, bits 4-7 of AL, ACTIVE LOW like
                     the port), 1 = the four resistive inputs. The values
                     come from the same host-fed sample the gameport VDD
                     answers with, so the two interfaces cannot disagree.
                     No stick (or JoystickType None) -> AH=86h CF=1, which
                     is what sends a well-behaved game to its keyboard
                     path. Logged once, not per call -- a game polls this
                     at frame rate and a trace the guest drives outruns
                     the guest. */
                UINT joystickDx = VDM_REG16(tib, VTIB_EDX);
                { static INT said = 0;
                  if (!said) { said = 1;
                    CHAR joystickLine[64], *joystickCursor = joystickLine;
                    joystickCursor = LogPut(joystickCursor, "  INT15 AH=84h joystick, dx=0x");
                    joystickCursor = LogHex(joystickCursor, joystickDx);
                    joystickCursor = LogPut(joystickCursor, VddJoystickIsLive(&g_Joystick) ? " (live)\r\n" : " (absent)\r\n");
                    LogAppend(LOG_PATH, joystickLine, joystickCursor); SerialOut(joystickLine, joystickCursor); } }
                if (!VddJoystickIsLive(&g_Joystick)) {
                    BSETAX((WORD)((VDM_REG(tib, VTIB_EAX) & BYTE_MASK) | (BIOS_SYSTEM_STATUS_UNSUPPORTED << BYTE_SHIFT)));
                    BCF_SET();
                } else if (joystickDx == BIOS_JOYSTICK_READ_BUTTONS) {
                    UINT mask = (1u << VddJoystickButtonsWired(&g_Joystick)) - 1u;
                    BSETAX((WORD)(((~g_Joystick.Buttons & mask) & JOYSTICK_BUTTON_MASK) << NIBBLE_SHIFT));
                    BCF_CLR();
                } else if (joystickDx == BIOS_JOYSTICK_READ_AXES) {
                    BSETAX((WORD)g_Joystick.Axis[0]);
                    VDM_REG(tib, VTIB_EBX) = (VDM_REG(tib, VTIB_EBX) & HIGH_WORD_MASK_U) | g_Joystick.Axis[1];
                    VDM_REG(tib, VTIB_ECX) = (VDM_REG(tib, VTIB_ECX) & HIGH_WORD_MASK_U)
                                           | (VddJoystickAxes(&g_Joystick) >= JOYSTICK_4AXIS_WIRED ? g_Joystick.Axis[2] : 0u);
                    VDM_REG(tib, VTIB_EDX) = (VDM_REG(tib, VTIB_EDX) & HIGH_WORD_MASK_U)
                                           | (VddJoystickAxes(&g_Joystick) >= JOYSTICK_4AXIS_WIRED ? g_Joystick.Axis[3] : 0u);
                    BCF_CLR();
                } else {
                    BSETAX((WORD)((VDM_REG(tib, VTIB_EAX) & BYTE_MASK) | (BIOS_SYSTEM_STATUS_UNSUPPORTED << BYTE_SHIFT)));
                    BCF_SET();
                }
            } else if (int15Ah == BIOS_SYSTEM_GET_EBDA) {         /* EBDA segment (#253)          */
                /* ES = the EBDA, CF=0, AX untouched -- as SeaBIOS answers it
                   (p_int15 int15.c1.status). Used to fall to the UNIMPL arm
                   below: CF=1, "no EBDA", while INT 12h withheld its kilobyte. */
                VDM_SET16(tib, VTIB_ES, g_DosMemoryTop);   /* #136: = BIOS_EBDA_SEG at 640 KB */
                BCF_CLR();
            } else if (int15Ah == BIOS_SYSTEM_GET_CONFIGURATION) {         /* get system config table (#54) */
                VDM_SET16(tib, VTIB_ES, DOS_CTAB_SEG);
                VDM_SET16(tib, VTIB_EBX, DOS_SYSCONF_OFF);
                BSETAX((WORD)(VDM_REG(tib, VTIB_EAX) & BYTE_MASK));   /* AH=0 */
                BCF_CLR();
            } else if (int15Ah == BIOS_SYSTEM_MOVE_BLOCK) {         /* move extended memory block (#54) */
                BSETAX((WORD)((Int15MoveBlock(tib) << BYTE_SHIFT)
                              | (VDM_REG(tib, VTIB_EAX) & BYTE_MASK)));
                /* AT BIOS: success is AH=0 with CF=0 AND ZF=1 */
                if ((VDM_REG(tib, VTIB_EAX) >> BYTE_SHIFT) & BYTE_MASK) { BCF_SET(); *flagsPointer &= (WORD)~EFLAGS_ZF; }
                else                                      { BCF_CLR(); *flagsPointer |= EFLAGS_ZF; }
            } else {
                /* ► LOG BEFORE BSETAX, NOT AFTER. The first cut printed AX *after*
                     this arm had already overwritten AH with 0x86, so the log said
                     "ax=0x86de" and only AL was the guest's -- the instrument
                     reporting its own write back as the guest's request. */
                { CHAR extendedLine[96], *extendedCursor = extendedLine;
                  extendedCursor = LogPut(extendedCursor, "  INT15 UNIMPL ax=0x");
                  extendedCursor = LogHex(extendedCursor, VDM_REG16(tib, VTIB_EAX));
                  extendedCursor = LogPut(extendedCursor, " bx=0x"); extendedCursor = LogHex(extendedCursor, VDM_REG16(tib, VTIB_EBX));
                  extendedCursor = LogPut(extendedCursor, " cx=0x"); extendedCursor = LogHex(extendedCursor, VDM_REG16(tib, VTIB_ECX));
                  extendedCursor = LogPut(extendedCursor, " dx=0x"); extendedCursor = LogHex(extendedCursor, VDM_REG16(tib, VTIB_EDX));
                  extendedCursor = LogPut(extendedCursor, " es=0x"); extendedCursor = LogHex(extendedCursor, VDM_REG16(tib, VTIB_ES));
                  extendedCursor = LogPut(extendedCursor, "\r\n"); LogAppend(LOG_PATH, extendedLine, extendedCursor); SerialOut(extendedLine, extendedCursor); }
                BSETAX((WORD)((VDM_REG(tib, VTIB_EAX) & BYTE_MASK) | (BIOS_SYSTEM_STATUS_UNSUPPORTED << BYTE_SHIFT)));
                BCF_SET();                     /* AH=86h: unsupported fn */
                g_BiosUnimplemented[VECTOR_SYSTEM] = 1;
                /* ► WHICH function, because "INT15" alone does not say. This arm is
                     the only difference between a Doom run that works and one given
                     a command-line argument: with an argument the trace is identical
                     until here, DOS/4GW takes this CF=1, and exits CLEANLY in 328 ms
                     (STAGE2: complete, run_ms=0x148) without printing a character. */
            }
        } else if (bopNumber == DOS_BOP_FOR_VECTOR(VECTOR_SERIAL)) {               /* SERIAL.  GH #45, #9     */
            /* ── ★★★ NOW ANSWERED BY THE PART, NOT BY THIS ARM. (GH #9) ──
                 What used to be here was a plausible set of status bits and
                 a transmit that wrote to g_Serial -- THE HOST'S OWN DEBUG
                 CHANNEL, which interleaved guest bytes with our log. Worse,
                 receive returned TIMEOUT unconditionally, because there was
                 nothing to receive FROM: ports 0x3F8.. were unclaimed and no
                 byte could enter the machine by any route. The port was
                 declared in the equipment word and could never be used.
                 vdd_comm.c is a real 8250/16550A, so INT 14h and the port
                 registers are now two views of ONE device -- a byte sent
                 through the BIOS in local loopback is readable from RBR, and
                 a byte the host pushes arrives whichever way the guest reads.
               ⚠ The 6.22 oracle is still NOT truth here -- QEMU runs SeaBIOS,
                 so its answer is another reimplementation's opinion
                 (epic #24). These bits are a statement about OUR machine, and
                 the thing that pins them is the loopback self-test, which is
                 the part's own documented behaviour rather than an opinion. */
            NTVDD_REGISTERS int14Registers; RegistersLoad(&int14Registers, tib);
            HOST_LOCK();
            VddBusDeliverInterrupt(&g_Bus, VECTOR_SERIAL, &int14Registers);
            HOST_UNLOCK();
            RegistersStore(&int14Registers, tib);
            BCF_CLR();
        } else if (bopNumber == DOS_BOP_FOR_VECTOR(VECTOR_PRINTER)) {               /* PRINTER, LPT1.  GH #45  */
            /* Printed output goes to a SPOOL FILE, which is a real printer
               as far as a DOS program can tell and is inspectable afterwards
               -- the alternative was reporting "selected, out of paper"
               forever, which is a port that exists and can never be used.
               Status bits: 7 not busy, 6 acknowledge, 4 selected, 3 I/O
               error, 0 timeout. 0x90 = not busy + selected = ready. */
            UINT int17Ah = (VDM_REG(tib, VTIB_EAX) >> BYTE_SHIFT) & BYTE_MASK;
            UINT int17Dx = VDM_REG16(tib, VTIB_EDX);
            /* ── #256: DX IS THE PRINTER NUMBER, AND THE BDA SAYS WHICH EXIST. ──────
                 Every DX was LPT1. A BIOS finds printer DX's port in 0040:0008+2*DX
                 and, if there is none (or DX > 2), returns AT ONCE with every register
                 as passed -- p_int17: 6.22/SeaBIOS, DOSBox-X and PCem's AMI agree on
                 LPT3 and DX=3. Only our one fitted port (378h) is backed, so a guest
                 that moved a base elsewhere in the table gets "absent" too.
               ► An unknown AH also leaves AX as passed (SeaBIOS, DOSBox-X). PCem's AMI
                 answers the status for it instead -- disputed, recorded in
                 oracle-rules.json; "ready" for a call that does nothing was neither. */
            WORD base17 = (int17Dx < BIOS_BDA_LPT_PORTS) ? *(volatile WORD *)(ULONG_PTR)(BIOS_BDA_BASE + BIOS_BDA_LPT_BASES + X86_WORD_SIZE * int17Dx) : 0;
            if (base17 != LPT_DEFAULT_BASE || !VddLptIsFitted(&g_Comm, 0)) {
                /* absent printer: nothing, registers as passed */
            } else if (int17Ah == BIOS_PRINTER_PRINT) {         /* print AL                 */
                BYTE character = (BYTE)(VDM_REG(tib, VTIB_EAX) & BYTE_MASK);
                /* Shared with the 0x378 port model -- see LptSpoolPut.
                   One printer, two ways in. */
                if (LptSpoolPut(character)) {
                    BSETAX((WORD)((BIOS_PRINTER_READY << BYTE_SHIFT) | (VDM_REG(tib, VTIB_EAX) & BYTE_MASK)));
                } else {
                    /* ── DO NOT REPORT READY WHEN THE BYTE WENT NOWHERE. ──
                         The first cut did exactly that: status 0x90 on every
                         call while no file was ever created, so the probe
                         passed every check and the feature did not work.
                         Bit 3 is I/O ERROR and bit 5 is OUT OF PAPER; a
                         program that checks either can now tell. */
                    BSETAX((WORD)((BIOS_PRINTER_FAILED << BYTE_SHIFT) | (VDM_REG(tib, VTIB_EAX) & BYTE_MASK)));
                    g_BiosUnimplemented[VECTOR_PRINTER] = 1;
                }
                BCF_CLR();
            } else if (int17Ah == BIOS_PRINTER_INITIALIZE || int17Ah == BIOS_PRINTER_GET_STATUS) {  /* init / status   */
                BSETAX((WORD)((g_LptFailed ? (BIOS_PRINTER_FAILED << BYTE_SHIFT) : (BIOS_PRINTER_READY << BYTE_SHIFT))
                              | (VDM_REG(tib, VTIB_EAX) & BYTE_MASK)));
                BCF_CLR();
            } else {
                g_BiosUnimplemented[VECTOR_PRINTER] = 1;       /* unknown AH: AX as passed */
            }
        } else if (bopNumber == DOS_BOP_FOR_VECTOR(VECTOR_DISK)) {               /* disk services  GH #44   */
            UINT int13Ah = (VDM_REG(tib, VTIB_EAX) >> BYTE_SHIFT) & BYTE_MASK;
            UINT int13Dl = VDM_REG(tib, VTIB_EDX) & BYTE_MASK;
            PDOS_DISK_GEOMETRY int13Geometry = DiskFor(int13Dl);
            if (int13Ah == BIOS_DISK_RESET) { BSETAX(0); g_DiskStatus = 0; BCF_CLR(); }
            else if (int13Ah == BIOS_DISK_GET_STATUS) { BSETAX((WORD)(g_DiskStatus << BYTE_SHIFT)); BCF_CLR(); }
            else if (!int13Geometry) {
                /* No image behind this drive letter. AH=80 is "drive not
                   ready", which is what a real machine says for a floppy
                   bay with nothing in it -- and is distinguishable from
                   AH=01 "bad command", which would mean the SERVICE does
                   not exist. Those are different answers to a guest. */
                BSETAX(BIOS_DISK_STATUS_NOT_READY << BYTE_SHIFT); g_DiskStatus = BIOS_DISK_STATUS_NOT_READY; BCF_SET();
                g_BiosUnimplemented[VECTOR_DISK] = 1;
            } else if (int13Ah == BIOS_DISK_GET_PARAMETERS) {          /* get drive parameters    */
                /* BH is ZEROED, not preserved: 6.22 answered BX=0004 to a
                   call made with BX poisoned to B1B1. Keeping the caller's
                   BH would hand back its own junk in half the register. */
                VDM_SET16(tib, VTIB_EBX, (WORD)int13Geometry->DriveType);
                VDM_SET16(tib, VTIB_ECX, DosDiskPackCx(int13Geometry));
                VDM_SET16(tib, VTIB_EDX,
                          (WORD)(((int13Geometry->Heads - 1) << BYTE_SHIFT) | g_DiskCount));
                BSETAX(0); g_DiskStatus = 0; BCF_CLR();
            } else if (int13Ah == BIOS_DISK_GET_TYPE) {          /* get disk type           */
                /* AH=01: floppy WITHOUT change-line support, which is what
                   6.22 answered (AX=0100) and is the truthful claim -- we
                   cannot detect a media swap under an image file. */
                BSETAX(BIOS_DISK_TYPE_FLOPPY_NO_CHANGE_LINE << BYTE_SHIFT); BCF_CLR();
            } else if (int13Ah == BIOS_DISK_READ || int13Ah == BIOS_DISK_WRITE || int13Ah == BIOS_DISK_VERIFY) {
                UINT sectorCount = VDM_REG(tib, VTIB_EAX) & BYTE_MASK;
                UINT int13Cx = VDM_REG16(tib, VTIB_ECX);
                UINT sector  = int13Cx & BIOS_CHS_SECTOR_MASK;
                UINT cylinder  = ((int13Cx >> BYTE_SHIFT) & BYTE_MASK) | ((int13Cx & BIOS_CHS_CYLINDER_HIGH_MASK) << BIOS_CHS_CYLINDER_HIGH_SHIFT);
                UINT head = (VDM_REG(tib, VTIB_EDX) >> BYTE_SHIFT) & BYTE_MASK;
                DWORD    lba = 0;
                if (!DosDiskChsToLba(int13Geometry, (WORD)cylinder, (WORD)head, (WORD)sector, &lba)
                    || lba + sectorCount > int13Geometry->TotalSectors) {
                    BSETAX(BIOS_DISK_STATUS_SECTOR_NOT_FOUND << BYTE_SHIFT); g_DiskStatus = BIOS_DISK_STATUS_SECTOR_NOT_FOUND;  /* sector not found */
                    BCF_SET();
                } else if (int13Ah == BIOS_DISK_VERIFY) {      /* verify: bounds only     */
                    BSETAX((WORD)sectorCount); g_DiskStatus = 0; BCF_CLR();
                } else {
                    DWORD linear = (VDM_REG16(tib, VTIB_ES) << PARAGRAPH_SHIFT)
                              + VDM_REG16(tib, VTIB_EBX);
                    INT isOk = DiskIo(int13Dl, lba, sectorCount, (BYTE *)(ULONG_PTR)linear,
                                     int13Ah == BIOS_DISK_WRITE);
                    if (isOk) { BSETAX((WORD)sectorCount); g_DiskStatus = 0; BCF_CLR(); }
                    else    { BSETAX(BIOS_DISK_STATUS_SECTOR_NOT_FOUND << BYTE_SHIFT); g_DiskStatus = BIOS_DISK_STATUS_SECTOR_NOT_FOUND; BCF_SET(); }
                }
            } else {
                BSETAX(BIOS_DISK_STATUS_BAD_COMMAND << BYTE_SHIFT); BCF_SET();      /* bad command             */
                g_BiosUnimplemented[VECTOR_DISK] = 1;
            }
        } else if (bopNumber == DOS_BOP_FOR_VECTOR(VECTOR_DOS_IDLE)) {               /* DOS idle                 */
            BCF_CLR();                         /* nothing to yield to      */
        } else if (bopNumber == DOS_BOP_FOR_VECTOR(VECTOR_FAST_CONSOLE_OUTPUT)) {               /* fast console output      */
            /* AL is the character. Programs that hook this expect it to
               PRINT; leaving it as an IRET swallowed the output silently. */
            VddVideoPutChar(&g_Video, (BYTE)(VDM_REG(tib, VTIB_EAX) & BYTE_MASK));
            BCF_CLR();
        } else if (bopNumber == DOS_BOP_FOR_VECTOR(VECTOR_ABSOLUTE_DISK_READ) || bopNumber == DOS_BOP_FOR_VECTOR(VECTOR_ABSOLUTE_DISK_WRITE)) { /* absolute disk read/write */
            /* AL = drive (0 = A:), CX = sector count, DX = first sector,
               DS:BX = buffer. LBA directly -- no CHS, which is the whole
               point of this pair. The stub RETFs, leaving the caller's
               pushed FLAGS for it to discard; see the stub planting. */
            UINT drive = VDM_REG(tib, VTIB_EAX) & BYTE_MASK;
            UINT count = VDM_REG16(tib, VTIB_ECX);
            UINT32 firstSector = VDM_REG16(tib, VTIB_EDX);
            PDOS_DISK_GEOMETRY int25Geometry = DiskFor(drive);
            DWORD linear = (VDM_REG16(tib, VTIB_DS) << PARAGRAPH_SHIFT)
                      + VDM_REG16(tib, VTIB_EBX);
            if (!int25Geometry) { BSETAX(DOS_ABSOLUTE_UNKNOWN_UNIT); BCF_SET(); g_BiosUnimplemented[bopNumber] = 1; }
            else if (firstSector + count > int25Geometry->TotalSectors) { BSETAX(DOS_ABSOLUTE_SECTOR_NOT_FOUND); BCF_SET(); }
            else if (DiskIo(drive, firstSector, count, (BYTE *)(ULONG_PTR)linear, bopNumber == DOS_BOP_FOR_VECTOR(VECTOR_ABSOLUTE_DISK_WRITE)))
                 { BSETAX(0); BCF_CLR(); }
            else { BSETAX(DOS_ABSOLUTE_SECTOR_NOT_FOUND); BCF_SET(); }  /* AL=08 sector not found */
        } else handled = 0;
        #undef BCF_SET
        #undef BCF_CLR
        #undef BSETAX
        if (handled == V86BOP_RERUN) V86BOP_RET(V86BOP_RERUN);   /* #206: still waiting -- re-run the BOP */
        if (handled) { VDM_REG(tib, VTIB_EIP) += VDM_BOP_LENGTH; V86BOP_RET(V86BOP_DONE); }
    }
    if (bopNumber == DOS_BOP_FOR_VECTOR(VECTOR_TIME)) {   /* INT 1Ah BIOS time */
        NTVDD_REGISTERS registers; RegistersLoad(&registers, tib);
        HOST_LOCK();
        VddBusDeliverInterrupt(&g_Bus, VECTOR_TIME, &registers);
        HOST_UNLOCK();
        RegistersStore(&registers, tib);
        HostSetFlags(tib, registers.CarryFlag, registers.ZeroFlag);
        VDM_REG(tib, VTIB_EIP) += VDM_BOP_LENGTH;
        V86BOP_RET(V86BOP_DONE);
    }
    if (bopNumber == DOS_BOP_FOR_VECTOR(VECTOR_MULTIPLEX)) {   /* INT 2Fh multiplex */
        DWORD ax = VDM_REG16(tib, VTIB_EAX);
        /* ── AX ALONE IS NOT THE CALL. ───────────────────────────────────────
             INT 2Fh is a multiplex: the function is AX, but the REQUEST is in
             the other registers and the ANSWER goes back through them too. We
             pass everything we do not recognise straight through, so the guest
             reads its own registers back as our reply -- the same "does nothing,
             reports success" shape as the DPMI 0300 bug -- and logging AX alone
             cannot show it. XP's COMMAND.COM asks 122Eh five times and 5501h
             once, then terminates without printing, so those registers are the
             evidence. Print them. */
        /* ── ⛔ CAPPED. The FOURTH instrument in one session to need this, and the
             last one standing after the BOP logger (268 MB, twice) and the INT 21h
             trace (2,166,824 lines). XP's COMMAND.COM loops through its whole
             init -- INT 2Fh included -- so an uncapped per-call line here wrote
             211 MB on its own. LOG_MAX_BYTES stops the DISK filling; it does not
             make a log readable, and a 211 MB file over SMB is its own outage.
           ⇒ 512 lines, then one line saying so. The first 512 are where any
             INT 2Fh answer worth reading is. */
        { static DWORD count2F = 0;
          if (++count2F == V86BOP_2F_LOG_LINES_MAX + 1) {
              cursor = LogPut(cursor, "STAGE2: BOP2F ... CAPPED at 512 lines (guest is looping)\r\n");
              LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
          }
          if (count2F > V86BOP_2F_LOG_LINES_MAX) goto bop2FServiced; }
        cursor = LogPut(cursor, "STAGE2: BOP2F ax=0x"); cursor = LogHex(cursor, ax);
        cursor = LogPut(cursor, " bx=0x");  cursor = LogHex(cursor, VDM_REG16(tib, VTIB_EBX));
        cursor = LogPut(cursor, " cx=0x");  cursor = LogHex(cursor, VDM_REG16(tib, VTIB_ECX));
        cursor = LogPut(cursor, " dx=0x");  cursor = LogHex(cursor, VDM_REG16(tib, VTIB_EDX));
        cursor = LogPut(cursor, " ds:si=0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_DS));
        cursor = LogPut(cursor, ":0x");     cursor = LogHex(cursor, VDM_REG16(tib, VTIB_ESI));
        cursor = LogPut(cursor, " es:di=0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_ES));
        cursor = LogPut(cursor, ":0x");     cursor = LogHex(cursor, VDM_REG16(tib, VTIB_EDI));
        cursor = LogPut(cursor, " from=0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_CS));
        cursor = LogPut(cursor, ":0x");     cursor = LogHex(cursor, VDM_REG16(tib, VTIB_EIP));
        cursor = LogPut(cursor, "\r\n");
        LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
    bop2FServiced:
        /* ── "NO XMS" MEANS NOT ANSWERING, NOT ANSWERING BADLY. ────────────
             A machine with no HIMEM.SYS does not reply to 4300 at all, so AL
             keeps whatever the caller put there and the caller reads "not
             80h" -- which is exactly what a real one sees. Returning an entry
             point that then refuses every call would be a driver that lies
             about being installed. */
        if (ax == MULTIPLEX_XMS_INSTALLATION_CHECK && g_XmsOn) {                     /* XMS installation check */
            VDM_SET16(tib, VTIB_EAX, (VDM_REG(tib, VTIB_EAX) & HIGH_BYTE_MASK) | MULTIPLEX_XMS_INSTALLED);  /* AL=80h installed */
        } else if (ax == MULTIPLEX_XMS_GET_ENTRY && g_XmsOn) {              /* get XMS entry -> ES:BX */
            VDM_SET16(tib, VTIB_ES, DOS_HDLR_SEG);
            VDM_SET16(tib, VTIB_EBX, XMS_ENTRY_OFF);
        } else if (ax == MULTIPLEX_DPMI_INSTALLATION_CHECK) {                           /* DPMI installation check (SPIKE) */
            /* AX=0 present; BX bit0=1 (32-bit programs supported, run 81); CL = the CPU
               class (see DPMI_CPU_CLASS); DX=0.90; SI=0 private paras; ES:DI = mode-switch
               entry to FAR-CALL. A 16-bit client ignores BX; a 32-bit client reads bit0 to
               decide to far-call with AX=1. */
            VDM_SET16(tib, VTIB_EAX, 0);
            VDM_SET16(tib, VTIB_EBX, MULTIPLEX_DPMI_32BIT_SUPPORTED);
            VDM_SET16(tib, VTIB_ECX, (VDM_REG(tib, VTIB_ECX) & HIGH_BYTE_MASK) | DPMI_CPU_CLASS);
            VDM_SET16(tib, VTIB_EDX, DPMI_VERSION_090);               /* DPMI 0.90        */
            VDM_SET16(tib, VTIB_ESI, 0);
            VDM_SET16(tib, VTIB_ES,  DOS_HDLR_SEG);
            VDM_SET16(tib, VTIB_EDI, DPMI_ENTRY_OFF);
            cursor = LogPut(cursor, "STAGE2: DPMI 1687 -> AX=0 ES:DI=0x"); cursor = LogHex(cursor, DOS_HDLR_SEG);
            cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, DPMI_ENTRY_OFF); cursor = LogPut(cursor, " (guest must far-call this)\r\n");
            LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
        } else if (ax == MULTIPLEX_DOS_TABLES) {
            /* ── THE TABLES XP's COMMAND.COM ASKS FOR BEFORE IT PRINTS. ────
                 DL selects; ES:DI comes back as a far pointer. It zeroes
                 ES:DI, calls five times (DL = 0,2,4,6,8) and STORES each
                 answer -- so passing the call through, which is what we did,
                 handed it back the 0000:0000 it supplied and left it holding
                 five null pointers.
               ★ MEASURED against two real Microsoft kernels before a line of
                 this was written (tests/probes/dos/p_int2f.asm):
                     DL=0 0001:0D8F   DL=2 0001:0B3B   DL=4 0001:0D8F
                     DL=6 0000:0000   DL=8 03E7:0188
                 DL=0 and DL=4 return the SAME pointer on both, so they share
                 one table here. DL=6 is legitimately NULL on both, so null
                 is the right answer and not a gap.
               ⚠ THE CONTENTS ARE BUILD-SPECIFIC (6.22 and PCem disagree on
                 DL=0/2/4), so there is nothing canonical to copy and these
                 are zero-filled -- grounded in PCem, where COMMAND.COM runs
                 perfectly, returning a DL=0 table whose first 32 bytes are
                 all zero. Thirty-two bytes is all that was measured.
               ⚠ AX IS LEFT ALONE. Real DOS does not report anything in it
                 here, and inventing a success code would be a claim we have
                 not measured. */
            DWORD dl2e = VDM_REG(tib, VTIB_EDX) & BYTE_MASK;
            WORD  toff = 0;
            if (dl2e == DOS_INT2F_TBL_A_DL || dl2e == DOS_INT2F_TBL_A_DL_ALIAS) toff = DOS_INT2F_TBL_A;
            else if (dl2e == DOS_INT2F_TBL_B_DL)            toff = DOS_INT2F_TBL_B;
            else if (dl2e == DOS_INT2F_TBL_C_DL)            toff = DOS_INT2F_TBL_C;
            if (toff) {
                VDM_SET16(tib, VTIB_ES,  DOS_CTAB_SEG);
                VDM_SET16(tib, VTIB_EDI, toff);
            } else {
                VDM_SET16(tib, VTIB_ES,  0);      /* DL=6, and anything else */
                VDM_SET16(tib, VTIB_EDI, 0);
            }
            cursor = LogPut(cursor, "STAGE2: 2F/122E dl="); cursor = LogHexByte(cursor, (UINT)dl2e);
            cursor = LogPut(cursor, " -> ES:DI=0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_ES));
            cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_EDI));
            cursor = LogPut(cursor, "\r\n");
            LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
        } else if (ax == MULTIPLEX_DEVICE_API_ENTRY) {                          /* get device API entry point */
            /* ES:DI = 0:0 means "no API for that device ID", and we have none.
               ⚠ Leaving the registers alone would be a POINTER-RETURNING call
                 that returns whatever was in ES:DI -- the caller then far-calls
                 into it. krnl386 happens to be safe (it asks for device 9 with
                 ES:DI already zero, so an untouched answer still reads as "none"
                 -- observed), but that is the CALLER being careful, and it is
                 not something to rely on from other callers. */
            VDM_SET16(tib, VTIB_ES, 0);
            VDM_SET16(tib, VTIB_EDI, 0);
            cursor = LogPut(cursor, "STAGE2: 2F/1684 device API -> none (ES:DI=0)\r\n");
            LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
        }
        /* ── The rest of what krnl386 asks INT 2Fh (as logged), and why leaving
             it alone is the RIGHT answer rather than merely the easy one:

             1600h  "is enhanced-mode Windows running?" AL unchanged = 0x00 =
                    no. Which is true, and krnl386 does not act on the answer.
             1689h  kernel idle call (documented). Fire-and-forget: there are
                    no return registers to set.
             168Ah  get vendor-specific API entry. AL unchanged = 0x8A means
                    "not supported" (documented). The vendor string it passes
                    in DS:SI is "MS-DOS" -- so the thing it is looking for is
                    NTVDM's private WOW API. See the vendor-API note at
                    g_WowVendorSelector for what happens when that is refused. */
        VDM_REG(tib, VTIB_EIP) += VDM_BOP_LENGTH;                        /* -> the IRET (CF) */
        V86BOP_RET(V86BOP_DONE);
    }
    if (bopNumber == DOS_BOP_XMS_ENTRY) {   /* XMS API far-call entry */
        /* ── LOG WHO CALLED, NOT JUST WHAT THEY ASKED. (GH #47) ───────────
             This printed AH and nothing else, so "MEM asks the version twice
             and stops" was all we could see -- not WHERE it stops, which is
             the actual question. The entry is reached by FAR CALL, so the
             return address is sitting on the guest stack at SS:SP: offset
             first, then segment.
           ★ IT ALSO CRACKS THE SEGMENT MAP. MEM.EXE is relocation-free
             (e_crlc=0) and computes its own segment bases at run time, so
             a file offset cannot place the code. One logged CS:IP per call
             pins the base. */
        {   DWORD callerStackLinear = (VDM_REG16(tib, VTIB_SS) << PARAGRAPH_SHIFT)
                       + VDM_REG16(tib, VTIB_ESP);
            const volatile BYTE *callerStack = (const volatile BYTE *)(ULONG_PTR)callerStackLinear;
            cursor = LogPut(cursor, " XMS AH=0x");
            cursor = LogHexByte(cursor, (UINT)((VDM_REG(tib, VTIB_EAX) >> BYTE_SHIFT) & BYTE_MASK));
            cursor = LogPut(cursor, " BL=0x"); cursor = LogHexByte(cursor, (UINT)(VDM_REG(tib, VTIB_EBX) & BYTE_MASK));
            cursor = LogPut(cursor, " DX=0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_EDX));
            cursor = LogPut(cursor, " <- caller ");
            cursor = LogHex(cursor, (DWORD)(callerStack[2] | (callerStack[3] << BYTE_SHIFT)));   /* return CS */
            cursor = LogPut(cursor, ":");
            cursor = LogHex(cursor, (DWORD)(callerStack[0] | (callerStack[1] << BYTE_SHIFT)));   /* return IP */
            cursor = LogPut(cursor, "\r\n"); }
        LogAppend(LOG_PATH, base, cursor); cursor = base;
        HostXms(tib);
        VDM_REG(tib, VTIB_EIP) += VDM_BOP_LENGTH;                        /* -> the RETF      */
        V86BOP_RET(V86BOP_DONE);
    }
    if (bopNumber == DOS_BOP_FOR_VECTOR(VECTOR_EMS)) {   /* INT 67h EMM (EMS) */
        cursor = LogPut(cursor, " EMS AH=0x"); cursor = LogHex(cursor, (VDM_REG(tib, VTIB_EAX) >> BYTE_SHIFT) & BYTE_MASK); cursor = LogPut(cursor, "\r\n");
        LogAppend(LOG_PATH, base, cursor); cursor = base;
        HOST_LOCK();
        HostEms(tib);
        HOST_UNLOCK();
        VDM_REG(tib, VTIB_EIP) += VDM_BOP_LENGTH;                        /* -> the IRET      */
        V86BOP_RET(V86BOP_DONE);
    }
    V86BOP_RET(V86BOP_NONE);
}
#undef V86BOP_RET
