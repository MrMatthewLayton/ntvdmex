; cpubench.asm -- how fast does an unthrottled NTVDMEX look to a DOS program? (GH #56)
;
; ── WHY THIS EXISTS ─────────────────────────────────────────────────────────────
; The CPU-speed dropdown is a duty cycle, and every speed on it is a fraction of ONE
; number: CPUSPEED_REF_MHZ, "the MHz an unthrottled host presents to a guest". The
; issue's own instruction was `calibrate against a period benchmark, not a guessed
; instructions-per-second number`, and this is that benchmark. It is also the
; ACCEPTANCE instrument: re-run it at each setting and the reported MHz should track
; the label on the menu.
;
; ── HOW THE NUMBER IS EARNED ────────────────────────────────────────────────────
; The inner loop is eight register-to-register ALU ops, a DEC and a taken JNZ. On a
; 486 every one of those is documented at one cycle except the taken branch at three:
;
;       8 x `add`  =  8
;       1 x `dec`  =  1
;       1 x `jnz`  =  3   (taken)
;                   ---
;                    12 cycles per iteration, on a 486
;
; So MHz = iterations-per-second x 12 / 1,000,000. That is a REAL calibration against
; a period CPU's published timings rather than a guess, and it is stated here so the
; assumption can be argued with rather than inherited.
;
; ⚠ It is an APPROXIMATION and the header says so too: a 486's actual throughput
;   depends on the prefetch queue and on where the code sits, and a modern CPU
;   executing the same bytes has none of a 486's constraints. What this measures
;   honestly is a RATIO -- how much faster than a 486 at 1 MHz this box runs this
;   loop -- which is exactly what the duty cycle needs.
;
; ⚠ NO REGISTER IN THE LOOP TOUCHES MEMORY, and that is deliberate: memory-bound code
;   would measure the host's cache rather than its execution rate, and the throttle
;   is a lever on execution.
;
; ── TIMING DISCIPLINE (this shape wedged the rig once -- see iobench.asm) ────────
; Do NOT poll 0040:006C directly. The host injects INT 08h from its exec loop, and
; that loop only gets a turn when the guest faults or BOPs; a pure memory spin on the
; tick therefore never advances it and cannot even be stopped by the headless
; deadline. Poll with INT 1Ah -- a BOP, so the host gets a turn -- once per BATCH
; iterations, which is often enough to be interruptible and rare enough not to be
; what is being measured.
;
; ⚠ AND THAT IS ALSO WHY THIS PROBE CANNOT TEST THE `no control points at all` CASE.
;   It traps once a batch. The suspend-based throttle does not depend on control
;   points -- that is the entire reason it suspends rather than waiting at one -- but
;   this probe is not the evidence for that claim and must not be quoted as if it is.
;
; Assemble: nasm -f bin cpubench.asm -o cpubench.com
bits 16
org 0x100

TICKS  equ 37                       ; ~2.0 s at 18.2 Hz -- long enough that one tick
                                    ; of quantisation is under 3% of the answer
BATCH  equ 4096                     ; iterations between tick polls: ~49k 486-cycles,
                                    ; so the INT 1Ah round trip is noise in the total

start:
    cld
    mov dx, s_banner
    mov ah, 0x09
    int 0x21

    call bench
    call report

    mov dx, s_done
    mov ah, 0x09
    int 0x21
    mov ax, 0x4C00
    int 0x21

; ── the measurement ────────────────────────────────────────────────────────────
; [count] = iterations completed, [elapsed] = BIOS ticks they took.
bench:
    mov dword [count], 0
    call gettick
    mov [tbase], eax
.outer:
    mov cx, BATCH
    mov ax, 1
    mov bx, 1
.inner:
    add ax, bx
    add bx, ax
    add ax, bx
    add bx, ax
    add ax, bx
    add bx, ax
    add ax, bx
    add bx, ax
    dec cx
    jnz .inner                      ; 12 cycles on a 486, per the header
    add dword [count], BATCH
    call tick_done
    jz .outer
    ret

; Poll the tick; ZF set = keep going, ZF clear = done, [elapsed] = ticks observed.
tick_done:
    push eax
    push ebx
    call gettick
    sub eax, [tbase]
    mov [elapsed], eax
    cmp eax, TICKS
    jb .go
    xor ebx, ebx
    cmp ebx, 1                      ; unequal -> ZF clear -> stop
    jmp .out
.go:
    xor ebx, ebx
    cmp ebx, ebx                    ; equal -> ZF set -> loop
.out:
    pop ebx
    pop eax
    ret

; BIOS tick as 32 bits in EAX, via INT 1Ah (a BOP -- see the discipline note).
gettick:
    push ecx
    push edx
    xor ah, ah
    int 0x1A                        ; CX:DX = tick count
    movzx eax, cx
    shl eax, 16
    movzx ecx, dx
    or eax, ecx
    pop edx
    pop ecx
    ret

; ── the report ─────────────────────────────────────────────────────────────────
; Prints the two RAW numbers and then the MHz it computes from them. Both, not just
; the MHz: a derived figure with no inputs beside it is a number nobody can check,
; and this project has already paid for one instrument that inferred its own frame.
;
;   MHz = iters * 12 / (ticks * 54.925 ms) / 1e6
;       = iters * 12 * 18.2065 / ticks / 1e6
; Done in integer arithmetic as (iters/ticks) * 2185 / 10,000,000, where 2185 =
; 12 * 18.2065 * 10. The iters/ticks division comes FIRST so a two-second run
; cannot overflow, and the multiply uses the full EDX:EAX product because
; iters-per-tick is already ~16 million on a modern box.
; ⚠ THE DIVISOR IS 1e7 AND WAS 1e4 IN THE FIRST CUT, which reported 3,638,198 MHz
;   -- a number so wrong it was obviously wrong, which is the only reason it cost
;   nothing. A factor-of-1000 error that had landed inside a plausible range would
;   have been adopted as the calibration constant and quietly made every speed on
;   the menu mean something else.
report:
    mov eax, [count]
    call printdec32
    mov dx, s_iters
    mov ah, 0x09
    int 0x21
    mov eax, [elapsed]
    call printdec32
    mov dx, s_ticks
    mov ah, 0x09
    int 0x21

    mov eax, [count]
    xor edx, edx
    mov ebx, [elapsed]
    test ebx, ebx
    jz .noticks
    div ebx                         ; EAX = iterations per tick
    mov ebx, 2185
    mul ebx                         ; EDX:EAX = per-tick * 2185
    mov ebx, 10000000
    div ebx                         ; EAX = MHz
    call printdec32
    mov dx, s_mhz
    mov ah, 0x09
    int 0x21
    ret
.noticks:
    mov dx, s_noticks
    mov ah, 0x09
    int 0x21
    ret

; Print EAX as unsigned decimal, most significant digit first.
printdec32:
    push eax
    push ebx
    push edx
    mov word [digits], 0
    mov ebx, 10
.split:
    xor edx, edx
    div ebx
    push dx
    inc word [digits]
    test eax, eax
    jnz .split
.emit:
    pop dx
    add dl, '0'
    mov ah, 0x02
    int 0x21
    dec word [digits]
    jnz .emit
    pop edx
    pop ebx
    pop eax
    ret

; ===========================================================================
s_banner  db 13,10,'== cpubench: apparent CPU speed (GH #56) ==',13,10
          db 'loop = 8 add + dec + jnz = 12 cycles on a 486',13,10,'$'
s_iters   db ' iters / $'
s_ticks   db ' ticks -> $'
s_mhz     db ' MHz apparent',13,10,'$'
s_noticks db '(no ticks elapsed -- the BIOS tick is not advancing)',13,10,'$'
s_done    db 'cpubench done',13,10,'$'

tbase     dd 0
count     dd 0
elapsed   dd 0
digits    dw 0
