; p_lpt.com -- INT 17h (printer) and INT 14h (serial).  GH #45.
;
; ⚠ THE 6.22 ORACLE IS NOT TRUTH HERE. QEMU runs SeaBIOS, so its answers are
;   another reimplementation's opinion, not MS-DOS's (epic #24). These bits are a
;   statement about OUR virtual machine's configuration, which is ours to declare
;   -- so the bar this probe enforces is INTERNAL CONSISTENCY, which is a real
;   bar and the one we were failing:
;
;     INT 11h reports one serial port and one parallel port fitted (0x4021),
;     and INT 14h then returned status 0x0000 -- every bit clear, i.e. a port
;     that exists and will never be ready. A guest that believes the equipment
;     word and polls the port waits forever.
;
; So: the equipment word and the port status must agree, a printed byte must
; actually arrive somewhere, and a read from a port with nothing attached must
; report TIMEOUT rather than block or lie about having data.
;
; nasm -f bin p_lpt.asm -o p_lpt.com

        org     100h
        jmp     start
%include "probe.inc"

start:
        PROBE_BEGIN "lpt"

        ; ---- INT 11h: what the machine SAYS it has. Bits 9-11 = serial ports,
        ; bits 14-15 = parallel ports. Everything below is measured against this.
        POISON
        int     11h
        call    probe_capture
        EMIT    "int11.equipment", "AX"

        ; ---- INT 17h AH=02: printer status. Bit 7 not busy, bit 4 selected;
        ; 0x90 = ready. 0x30 (selected + OUT OF PAPER) is what we used to say,
        ; which is a printer that can never accept a byte.
        mov     ax, 0200h
        mov     dx, 0                           ; LPT1
        int     17h
        call    probe_capture
        EMIT    "int17.02.status", "AX"

        ; ---- INT 17h AH=00: print two bytes. The acceptance test is not this
        ; status word -- it is whether the bytes turn up on the other side, which
        ; is checked off-probe by reading the spool file.
        mov     ax, 0048h                       ; 'H'
        mov     dx, 0
        int     17h
        call    probe_capture
        EMIT    "int17.00.print.H", "AX"

        mov     ax, 0069h                       ; 'i'
        mov     dx, 0
        int     17h
        call    probe_capture
        EMIT    "int17.00.print.i", "AX"

        ; ---- INT 14h AH=03: serial line status. AH bits 5/6 = transmit
        ; registers empty, so a guest may send; bit 0 clear = no data waiting.
        mov     ax, 0300h
        mov     dx, 0                           ; COM1
        int     14h
        call    probe_capture
        EMIT    "int14.03.status", "AX"

        ; ---- INT 14h AH=01: send a byte. AH bit 7 clear = no error.
        mov     ax, 0158h                       ; 'X'
        mov     dx, 0
        int     14h
        call    probe_capture
        EMIT    "int14.01.send", "AX"

        ; ---- INT 14h AH=02: receive with nothing attached. AH bit 7 = TIMEOUT,
        ; which is what a real UART reports and what a polling guest is written
        ; to expect. Reporting "data ready" here would hand the guest a garbage
        ; byte; reporting a permanently-not-ready port hangs it.
        mov     ax, 0200h
        mov     dx, 0
        int     14h
        call    probe_capture
        EMIT    "int14.02.recv.timeout", "AX"

        PROBE_END
