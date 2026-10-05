; p_kbd.com -- the BIOS keyboard contract: INT 16h against the BDA ring buffer.
;
; WHY THIS SHAPE. Every keyboard defect this project has paid for was in a surface
; nobody had inventoried: the INT 09h hook that peeks port 60h and chains, the
; missing Alt column, IRQ1 left in service, the BDA display fields reading zero.
; The DOS services, which WERE measured against 6.22, caused none of them. So:
; measure this surface the same way, one case at a time, against the reference.
;
; THE TRICK THAT MAKES IT HEADLESS. A probe cannot type. But the BIOS keyboard
; buffer is a DOCUMENTED ring at 0040:001E with head/tail at 0040:001A/001C, and
; key-stuffing TSRs have written it directly since 1983 -- so the probe injects
; the entries itself and then asks INT 16h what it makes of them. That tests the
; whole read side (00/01/02/09/10/11/12, wrap, enhanced-key skipping) with nobody
; at the keyboard, deterministically, on every host.
;
; ⚠ EVERY BLOCKING READ IS GUARDED BY A PEEK. An unguarded AH=00h on a host whose
;   ring never fills BLOCKS FOREVER, and the probe dies as a harness timeout --
;   an absence that reads as a hang instead of as data. read16/read10 below peek
;   first and emit AX=DEAD for "the buffer was empty, no read attempted".
; ⚠ head/tail are reset to the buffer start before each case, so the emitted
;   values are comparable across hosts instead of depending on what the shell
;   happened to leave behind.
; ⚠ QEMU's SeaBIOS is a REWRITE, not IBM's BIOS -- these rows are provisional
;   until PCem (genuine AMI/IBM ROM) confirms them. See docs/PARITY.md.
;
; nasm -f bin p_kbd.asm -o p_kbd.com

        org     100h
        jmp     start
%include "probe.inc"

; ZF is the entire answer for AH=01h/11h, and the dump has no ZF column -- so
; hoist it into AX (40h = set = "no key waiting") and compare that.
%macro EMIT_ZF 1
        mov     ax, [__fl]
        and     ax, 40h
        mov     [__ax], ax
        EMIT    %1, "AX"
%endmacro

; AL is the answer for AH=05h/02h/09h; AH is left at the call value on some BIOSes
; and zeroed on others, which is a difference about nothing. Compare AL alone.
%macro EMIT_AL 1
        mov     ax, [__ax]
        and     ax, 0FFh
        mov     [__ax], ax
        EMIT    %1, "AX"
%endmacro

; ---- helpers -------------------------------------------------------------

bda:                                    ; ES = 0040h, the BIOS data area
        mov     ax, 40h
        mov     es, ax
        ret

reset:                                  ; head = tail = buffer start -> empty, and
        mov     ax, [es:80h]            ; at a KNOWN offset, so head/tail compare
        mov     [es:1Ah], ax
        mov     [es:1Ch], ax
        ret

inject:                                 ; AX -> ring at tail, tail advanced (wrapping)
        push    bx                      ; deliberately NOT AH=05h: writing the ring
        mov     bx, [es:1Ch]            ; ourselves is what lets us test AH=05h
        mov     [es:bx], ax
        add     bx, 2
        cmp     bx, [es:82h]
        jb      .ok
        mov     bx, [es:80h]
.ok:    mov     [es:1Ch], bx
        pop     bx
        ret

peek16:                                 ; AH=01h -- never blocks
        mov     ah, 01h
        int     16h
        call    probe_capture
        ret

read16:                                 ; AH=00h, guarded. AX=DEAD if nothing waiting.
        mov     ah, 01h
        int     16h
        jnz     .have
        call    probe_capture
        mov     word [__ax], 0DEADh
        ret
.have:  xor     ax, ax
        int     16h
        call    probe_capture
        ret

read10:                                 ; AH=10h, guarded the same way (via AH=11h)
        mov     ah, 11h
        int     16h
        jnz     .have
        call    probe_capture
        mov     word [__ax], 0DEADh
        ret
.have:  mov     ah, 10h
        int     16h
        call    probe_capture
        ret

; ---- cases ---------------------------------------------------------------

start:
        PROBE_BEGIN "kbd"
        call    bda

        ; ---- the buffer's own geometry. A program that relocates the ring writes
        ; these two; everything below assumes the BIOS honours them.
        mov     ax, [es:80h]
        mov     [__ax], ax
        mov     ax, [es:82h]
        mov     [__bx], ax
        EMIT    "bda.buffer", "AX,BX"

        ; ---- 0040:0096 bit 4 = "enhanced keyboard installed". A guest that reads
        ; 0 here uses the 83-key tables and never asks for F11/F12 or Ctrl+arrows.
        ; The raw bytes ride along informationally: 0017/0018 are live shift state
        ; and depend on the machine, so they are NOT in the signature.
        mov     al, [es:96h]
        and     ax, 10h
        mov     [__ax], ax
        mov     al, [es:96h]
        xor     ah, ah
        mov     [__bx], ax
        mov     al, [es:17h]
        xor     ah, ah
        mov     [__cx], ax
        mov     al, [es:18h]
        xor     ah, ah
        mov     [__dx], ax
        EMIT    "bda.enhanced", "AX"

        ; ---- EMPTY is head == tail, and both peeks must agree about it.
        call    reset
        call    peek16
        EMIT_ZF "16.01.empty"
        mov     ah, 11h
        int     16h
        call    probe_capture
        EMIT_ZF "16.11.empty"

        ; ---- ONE ORDINARY KEY. 'a' with its scancode, the shape of every
        ; printable key a DOS editor reads.
        call    reset
        mov     ax, 1E61h
        call    inject
        call    peek16
        EMIT    "16.01.peek", "AX"
        EMIT_ZF "16.01.peek.zf"
        ; A PEEK MUST NOT CONSUME. head must still be at the start, tail one on.
        mov     ax, [es:1Ah]
        mov     [__ax], ax
        mov     ax, [es:1Ch]
        mov     [__bx], ax
        EMIT    "16.01.nocons", "AX,BX"
        call    read16
        EMIT    "16.00.read", "AX"
        mov     ax, [es:1Ah]
        mov     [__ax], ax
        EMIT    "16.00.head", "AX"
        call    peek16
        EMIT_ZF "16.00.drained"

        ; ---- ★ THE ENHANCED-KEY RULE, which is the one most emulators get wrong.
        ; F11 is 8500h: a code only a 101-key keyboard can produce. The IBM rule is
        ; that AH=00h/01h SKIP such keystrokes (an old program must never see a code
        ; it has no table for) while AH=10h/11h return them. A host that hands 8500h
        ; to AH=01h breaks every pre-1986 program; one that never returns it at all
        ; breaks F11/F12 everywhere.
        call    reset
        mov     ax, 8500h
        call    inject
        mov     ah, 11h
        int     16h
        call    probe_capture
        EMIT    "16.11.enh", "AX"
        EMIT_ZF "16.11.enh.zf"
        call    peek16
        EMIT    "16.01.enh", "AX"
        EMIT_ZF "16.01.enh.zf"
        mov     ax, [es:1Ah]            ; a skipping BIOS has advanced head past it
        mov     [__ax], ax
        mov     ax, [es:1Ch]
        mov     [__bx], ax
        EMIT    "16.01.enh.headtail", "AX,BX"

        ; ---- and AH=10h must hand the same entry back intact.
        call    reset
        mov     ax, 8500h
        call    inject
        call    read10
        EMIT    "16.10.enh", "AX"

        ; ---- an EXTENDED key with a zero ASCII byte (Left arrow). Ordinary calls
        ; DO return these -- they are 83-key codes -- so this separates "skips
        ; enhanced" from "skips anything with AL=0", which is a different bug.
        call    reset
        mov     ax, 4B00h
        call    inject
        call    read16
        EMIT    "16.00.arrow", "AX"

        ; ---- AH=05h: the BIOS's own way INTO the ring, and the only write path a
        ; program is supposed to use (key-stuffing TSRs, DOSKEY, installers).
        call    reset
        mov     ax, 05B1h               ; AL poisoned: B1 back = "nobody answered"
        mov     cx, 1C0Dh
        int     16h
        call    probe_capture
        EMIT_AL "16.05.push"
        mov     ax, [es:1Ch]
        mov     [__ax], ax
        EMIT    "16.05.tail", "AX"
        call    read16
        EMIT    "16.05.readback", "AX"

        ; ---- WRAP. tail at the last slot must come back round to the start.
        call    reset
        mov     ax, [es:82h]
        sub     ax, 2                   ; the last slot
        mov     [es:1Ah], ax
        mov     [es:1Ch], ax
        mov     ax, 1111h
        call    inject
        mov     ax, [es:1Ch]            ; must have wrapped to the buffer start
        mov     [__ax], ax
        EMIT    "ring.wrap.tail", "AX"
        mov     ax, 2222h
        call    inject
        call    read16
        EMIT    "ring.wrap.first", "AX"
        call    read16
        EMIT    "ring.wrap.second", "AX"

        ; ---- FULL. A 16-slot ring holds fifteen entries; the sixteenth push must
        ; be refused with AL=1 rather than eating the head.
        call    reset
        mov     cx, 15
.fill:  push    cx
        mov     ah, 05h
        mov     cx, 3931h
        int     16h
        pop     cx
        loop    .fill
        mov     ax, 05B1h
        mov     cx, 3932h
        int     16h
        call    probe_capture
        mov     ax, [__ax]
        and     ax, 0FFh
        mov     [__ax], ax
        mov     ax, [es:1Ch]            ; recomputed AFTER probe_capture, which
        sub     ax, [es:1Ah]            ; overwrites __bx
        mov     [__bx], ax
        EMIT    "16.05.full", "AX,BX"

        ; ---- AH=02h/12h report the BDA shift bytes. Set them and see whether the
        ; call is reading the same memory the guest can.
        call    reset
        mov     byte [es:17h], 03h
        mov     byte [es:18h], 00h
        mov     ax, 02B1h
        int     16h
        call    probe_capture
        EMIT_AL "16.02.shift"
        mov     ax, 12B1h
        int     16h
        call    probe_capture
        EMIT    "16.12.ext", "AX"
        mov     byte [es:17h], 00h

        ; ---- AH=09h: which INT 16h functions this BIOS admits to having.
        mov     ax, 09B1h               ; ⚠ WITHOUT THIS POISON the row was a FALSE
        int     16h                     ; MATCH: a host that ignores AH=09h leaves AL
        call    probe_capture           ; alone and the caller's own byte reads as an
        EMIT_AL "16.09.support"

        ; ---- AH=03h typematic rate. Nothing to read back through the BIOS; this
        ; is here because a guest that sets it and gets CF=1 may give up entirely.
        mov     ax, 0305h
        xor     bx, bx
        int     16h
        call    probe_capture
        EMIT    "16.03.typematic", "CF"

        call    reset                   ; leave the shell a clean buffer
        PROBE_END
