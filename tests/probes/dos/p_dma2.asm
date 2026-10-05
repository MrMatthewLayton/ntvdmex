; p_dma2.com -- the 8237A's own transfers: the REQUEST register, MEMORY-TO-MEMORY
;               through the TEMPORARY register, and the AT CASCADE. GH #246.
;               docs/ref/dma.md; docs/inventory/dma.md 5; tests/unit/dma_test.c T13-T15.
;
; ── WHAT THIS IS FOR ─────────────────────────────────────────────────────────
; p_dma.asm asks the register file. This asks the parts of the chip that MOVE
; something with no device on DACK at all -- the only transfers a DOS program can
; start and observe on its own:
;   * a SOFTWARE REQUEST (09h) on channel 1 in VERIFY mode: the controller walks
;     the address and count to terminal count without a memory cycle;
;   * MEMORY-TO-MEMORY (command bit 0): channel 0's request copies a block from
;     channel 0's address to channel 1's through the temporary register, which
;     afterwards reads the last byte moved (0Dh);
;   * THE CASCADE: with channel 4 masked, controller 1 is cut off the bus and the
;     same software request must WAIT until channel 4 is unmasked again.
;
; ── ⛔⛔⛔ THE SAFETY NOTE. THE FLOPPY IS CHANNEL 2 AND IT CARRIES THE ANSWER. ──
; * CHANNELS 0 AND 1 ONLY (the issue's rule). Nothing writes channel 2's address,
;   count, page (81h), mode or mask; no WRITE-ALL-MASK (0Fh), no CLEAR-MASK (0Eh).
; * NO MASTER CLEAR on controller 1 (0Dh) -- it would mask the floppy.
; * THE COMMAND REGISTER (08h) IS WRITTEN, and that is the one shared byte: it
;   holds controller-wide bits. It is written 01h for the copy and 00h straight
;   after, which is the power-on value every BIOS here leaves (nothing reads it
;   back -- the 8237A cannot -- so 00h is restored by assumption, and the floppy
;   writing OUT.TXT afterwards is the evidence it was right).
; * CHANNEL 4 IS MASKED FOR A FEW INSTRUCTIONS in case E and unmasked again before
;   anything else runs. While it is masked the floppy cannot transfer either --
;   that is the point of the case -- so no DOS call happens in between.
; * Every write that moves memory targets this program's own buffers, checked not
;   to cross a 64 KB DMA page. Channel 0's and 1's page registers (87h, 83h) are
;   saved and restored.
;
; ORACLE-ALSO: pcem   (a real AMI 486 BIOS -- see scripts/pcemoracle.py)
; nasm -f bin p_dma2.asm -o p_dma2.com

        org     100h
        jmp     start
%include "probe.inc"

; ---------------------------------------------------------------- state
pg0_was db      0
pg1_was db      0
src_off dw      0                       ; chosen source buffer offset
dst_off dw      0                       ; chosen destination buffer offset
r1      dw      0
r2      dw      0
        align   16
srcbuf  times 64 db 0
dstbuf  times 64 db 0

; ---- phys: SI = offset in DS -> DL = page, AX = address. CF set if a 16-byte
;      block at that address would cross a 64 KB page.
phys:
        push    bx
        mov     ax, ds
        mov     dx, ax
        shl     ax, 4
        shr     dx, 12                  ; DL = high nibble of the segment
        add     ax, si
        adc     dl, 0
        cmp     ax, 0FFE0h
        cmc                             ; CF = 1 when ax >= FFE0h
        pop     bx
        ret

; ---- pick an offset inside a 64-byte buffer (BX) whose block does not cross
;      a page: the start, or 32 bytes in. Returns SI.
pick:
        mov     si, bx
        call    phys
        jnc     .ok
        add     si, 32
.ok:    ret

; ---- program channel CL (0 or 1): SI = buffer offset, DI = count, BL = mode.
progch:
        push    ax
        push    dx
        mov     al, 4
        or      al, cl
        out     00Ah, al                ; mask the channel while programming it
        mov     al, 0
        out     00Ch, al                ; clear byte pointer
        call    phys                    ; DL = page, AX = address
        push    dx
        mov     dx, 0
        mov     dl, cl
        shl     dl, 1                   ; 00h (ch0) or 02h (ch1)
        out     dx, al
        mov     al, ah
        out     dx, al
        inc     dx                      ; 01h / 03h: count
        mov     ax, di
        out     dx, al
        mov     al, ah
        out     dx, al
        pop     ax                      ; AL = page
        cmp     cl, 0
        jne     .p1
        out     087h, al
        jmp     .pm
.p1:    out     083h, al
.pm:    mov     al, bl
        or      al, cl
        out     00Bh, al                ; mode
        pop     dx
        pop     ax
        ret

; ---- AX = channel CL's current count (via the flip-flop)
rdcount:
        push    dx
        mov     al, 0
        out     00Ch, al
        xor     dx, dx
        mov     dl, cl
        shl     dl, 1
        inc     dl
        in      al, dx
        mov     ah, al
        in      al, dx
        xchg    al, ah
        pop     dx
        ret

; ---- a short settle: nothing a real 8237 needs, but a few bus cycles cost nothing
settle:
        push    cx
        mov     cx, 64
.s:     in      al, 080h
        loop    .s
        pop     cx
        ret

start:
        PROBE_BEGIN "dma2"
        in      al, 087h
        mov     [pg0_was], al
        in      al, 083h
        mov     [pg1_was], al

        mov     bx, srcbuf
        call    pick
        mov     [src_off], si
        mov     bx, dstbuf
        call    pick
        mov     [dst_off], si
        ; source pattern A0h.., destination zeroed
        mov     si, [src_off]
        mov     cx, 16
        mov     al, 0A0h
.f:     mov     [si], al
        inc     si
        inc     al
        loop    .f
        mov     di, [dst_off]
        mov     cx, 16
.z:     mov     byte [di], 0
        inc     di
        loop    .z

; ════════════════════════════════════════════════════════════════════════════
; A. A SOFTWARE REQUEST, VERIFY MODE, CHANNEL 1 MASKED.   datasheet: request
;    register; "non-maskable"; cleared at TC.
;    16 verify cycles. AX = the current count afterwards: 000Fh = nothing ran;
;    FFFFh = ran to TC by the datasheet; 0000h = ran to TC, our model's resting
;    count (see dma_test.c T13).
; ════════════════════════════════════════════════════════════════════════════
        in      al, 008h                ; drop any latched TC first
        mov     cl, 1
        mov     si, [dst_off]
        mov     di, 000Fh
        mov     bl, 080h                ; block, verify
        call    progch                  ; (leaves channel 1 MASKED)
        mov     al, 005h                ; request: set, channel 1
        out     009h, al
        call    settle
        call    rdcount
        mov     [r1], ax
        mov     ax, [r1]
        POISON
        call    probe_capture
        EMIT    "dma2.req.count", "AX"

; B. ...its address (0010h past the start, as the low byte) and status.
;    AH = status & 22h (TC1, DRQ1) -- first read; AL = low byte of the address
;    moved, i.e. current minus base.
        in      al, 008h
        and     al, 022h
        mov     [r1], al
        mov     al, 0
        out     00Ch, al
        in      al, 002h
        mov     ah, al
        in      al, 002h
        xchg    al, ah                  ; AX = current address
        mov     si, [dst_off]
        push    ax
        call    phys
        mov     bx, ax                  ; base address
        pop     ax
        sub     ax, bx
        mov     ah, [r1]
        POISON
        call    probe_capture
        EMIT    "dma2.req.status.moved", "AX"

; ════════════════════════════════════════════════════════════════════════════
; C. MEMORY-TO-MEMORY: ten bytes, channel 0 -> channel 1.   datasheet
;    AX = first copied byte : tenth copied byte (A0A9h if it ran).
; ════════════════════════════════════════════════════════════════════════════
        mov     cl, 0
        mov     si, [src_off]
        mov     di, 000Fh
        mov     bl, 088h                ; block, read
        call    progch
        mov     cl, 1
        mov     si, [dst_off]
        mov     di, 0009h               ; ten bytes
        mov     bl, 084h                ; block, write
        call    progch
        mov     al, 001h                ; command: memory-to-memory
        out     008h, al
        mov     al, 004h                ; request: set, channel 0
        out     009h, al
        call    settle
        mov     al, 000h                ; command back to 00h AT ONCE
        out     008h, al
        mov     al, 000h                ; reset any request left on channel 0
        out     009h, al
        mov     si, [dst_off]
        mov     ah, [si]
        mov     al, [si+9]
        POISON
        call    probe_capture
        EMIT    "dma2.m2m.copy", "AX"

; D. ...the eleventh byte (not copied: 00h) and the TEMPORARY register.
;    AH = dst[10], AL = in 0Dh (A9h by the datasheet: the last byte moved).
        mov     si, [dst_off]
        mov     al, [si+10]
        mov     [r1], al
        in      al, 00Dh
        mov     ah, [r1]
        POISON
        call    probe_capture
        EMIT    "dma2.m2m.past.temp", "AX"

; ════════════════════════════════════════════════════════════════════════════
; E. THE CASCADE. Channel 4 masked: controller 1 has no path to the bus, so a
;    software request on channel 1 must WAIT. Unmask channel 4 and it runs.
;    AH = low byte of the count while channel 4 was masked (07h = held),
;    AL = low byte after unmasking (00h or FFh = served).
; ════════════════════════════════════════════════════════════════════════════
        mov     cl, 1
        mov     si, [dst_off]
        mov     di, 0007h
        mov     bl, 080h                ; block, verify
        call    progch
        cli
        mov     al, 004h                ; mask channel 4 (D4h: set, ch 0 of ctrl 2)
        out     0D4h, al
        mov     al, 005h
        out     009h, al                ; request channel 1
        call    settle
        call    rdcount
        mov     [r1], ax
        mov     al, 000h                ; unmask channel 4 -- the floppy's path back
        out     0D4h, al
        call    settle
        call    rdcount
        mov     [r2], ax
        sti
        mov     al, 001h                ; reset any request still on channel 1
        out     009h, al
        mov     ah, byte [r1]
        mov     al, byte [r2]
        POISON
        call    probe_capture
        EMIT    "dma2.cascade.held.served", "AX"

        ; leave channels 0 and 1 masked, their pages as found
        mov     al, 004h
        out     00Ah, al
        mov     al, 005h
        out     00Ah, al
        mov     al, [pg0_was]
        out     087h, al
        mov     al, [pg1_was]
        out     083h, al
        in      al, 008h                ; drop the TCs we latched

        PROBE_END
