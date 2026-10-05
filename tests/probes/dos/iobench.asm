; iobench.asm -- port-I/O trap THROUGHPUT benchmark (sound epic, slice 1).
;
; Why: every guest IN/OUT is an IOPL-0 #GP reflected out to our user-mode monitor
; and serviced by host_try_io*(). That round-trip costs far more than the ISA access
; it stands in for. Skyroads' AdLib register-write helper spends 43 port accesses per
; OPL register (6 delay reads after the address write, 35 after the data write), so
; OPL music is throughput-bound before a single FM sample is synthesised. This probe
; MEASURES the cost, so the burst fast path can be judged against a number.
;
; Four shapes, each run for TICKS BIOS ticks, printing the access count and the tick
; delta actually observed (the host-side rate arithmetic is done off-box, which keeps
; this probe free of 32-bit division and of assumptions about the tick rate):
;   1. `IN AL,DX` unrolled     (0x388, OPL status)  -- one trap per access
;   2. `IN AL,DX` + `LOOP`     (0x388)              -- the EXACT Skyroads OPL idiom,
;                                                      the shape host_io_loop_burst
;                                                      collapses into a single trap
;   3. `OUT DX,AL` unrolled    (0x3C8, VGA DAC idx) -- one trap per access
;   4. `REP OUTSB` x64         (0x3C9)              -- the VGA palette-set shape
;
; Case 4 doubles as a CORRECTNESS probe: a REP string op is restartable (EIP stays on
; the instruction until CX drains), so it exercises whichever EIP shape the hardware
; reflect reports for a repeated I/O -- if it is unserviced the case simply stalls
; until the host's headless deadline, leaving cases 1-3 in the log.
;
; TIMING DISCIPLINE (learned the hard way -- this wedged the rig once): do NOT poll
; the BIOS tick by reading 0040:006C directly. The host injects INT 08h from its own
; exec loop, and that loop only regains control when the guest faults or BOPs; a pure
; memory spin on the tick therefore never advances it, never returns to the host, and
; cannot even be stopped by the headless deadline. We poll with INT 1Ah instead --
; a BOP, so the host gets a turn every poll. Polls are amortised one per BATCH units
; of work so they do not distort the measurement.
;
; Assemble: nasm -f bin iobench.asm -o iobench.com
bits 16
org 0x100

TICKS       equ 5                       ; ~0.27 s per case (4 cases << the 30 s cap)
BATCH       equ 16                      ; accesses between tick polls (case 1 + 3)
BLOCKS      equ 8                       ; 35-access blocks between polls (case 2)
REPS        equ 4                       ; 64-byte REP runs between polls (case 4)

start:
    cld
    mov dx, s_banner
    mov ah, 0x09
    int 0x21

    mov dx, s_c1
    mov ah, 0x09
    int 0x21
    call bench_in1
    call report

    mov dx, s_c2
    mov ah, 0x09
    int 0x21
    call bench_inloop
    call report

    mov dx, s_c3
    mov ah, 0x09
    int 0x21
    call bench_out1
    call report

    mov dx, s_c4
    mov ah, 0x09
    int 0x21
    call bench_repouts
    call report

    mov dx, s_done
    mov ah, 0x09
    int 0x21
    mov ax, 0x4C00
    int 0x21

; ===========================================================================
; benchmarks -- each leaves [count] = accesses done, [elapsed] = ticks taken
; ===========================================================================

; --- case 1: single IN AL,DX, unrolled so no LOOP idiom is present ---------
bench_in1:
    call bench_start
    mov dx, 0x388
.outer:
    times BATCH in al, dx
    add dword [count], BATCH
    call tick_done
    jz .outer
    ret

; --- case 2: `mov cx,35 / in al,dx / loop` -- Skyroads' OPL write delay ----
bench_inloop:
    call bench_start
    mov dx, 0x388
.outer:
    mov bp, BLOCKS
.blk:
    mov cx, 35
.inner:
    in al, dx
    loop .inner
    add dword [count], 35
    dec bp
    jnz .blk
    call tick_done
    jz .outer
    ret

; --- case 3: single OUT DX,AL, unrolled -----------------------------------
bench_out1:
    call bench_start
    mov dx, 0x3C8
    xor al, al
.outer:
    times BATCH out dx, al
    add dword [count], BATCH
    call tick_done
    jz .outer
    ret

; --- case 4: REP OUTSB, 64 bytes a go -------------------------------------
bench_repouts:
    call bench_start
.outer:
    mov bp, REPS
.blk:
    mov si, palbuf
    mov cx, 64
    mov dx, 0x3C9
    rep outsb
    add dword [count], 64
    dec bp
    jnz .blk
    call tick_done
    jz .outer
    ret

; ===========================================================================
; helpers
; ===========================================================================

; Start a measurement: zero the counter and latch the current tick.
bench_start:
    mov dword [count], 0
    call gettick
    mov [tbase], eax
    ret

; Poll the tick; set ZF if the measurement should continue (elapsed < TICKS),
; clear ZF when it is done, leaving [elapsed] = ticks actually observed.
tick_done:
    push eax
    push ebx
    call gettick
    sub eax, [tbase]
    mov [elapsed], eax
    cmp eax, TICKS
    jb .go                              ; below -> keep going (ZF set below)
    xor ebx, ebx                        ; done: clear ZF via cmp of unequals
    cmp ebx, 1
    jmp .out
.go:
    xor ebx, ebx
    cmp ebx, ebx                        ; ZF = 1 -> caller loops
.out:
    pop ebx
    pop eax
    ret

; Read the BIOS tick as a 32-bit value in EAX via INT 1Ah (a BOP, so the host's
; exec loop gets a turn -- see the TIMING DISCIPLINE note at the top).
gettick:
    push ecx
    push edx
    xor ah, ah
    int 0x1A                            ; CX:DX = tick count
    movzx eax, cx
    shl eax, 16
    movzx ecx, dx
    or eax, ecx
    pop edx
    pop ecx
    ret

; Print "<count> acc / <elapsed> ticks".
report:
    mov eax, [count]
    call printdec32
    mov dx, s_acc
    mov ah, 0x09
    int 0x21
    mov eax, [elapsed]
    call printdec32
    mov dx, s_ticks
    mov ah, 0x09
    int 0x21
    ret

; Print EAX as unsigned decimal. Digits are pushed then popped so output is
; most-significant first; the digit count lives in memory because we cannot
; assume INT 21h preserves CX across the print loop.
printdec32:
    push eax
    push ebx
    push edx
    mov word [digits], 0
    mov ebx, 10
.split:
    xor edx, edx
    div ebx                             ; EDX:EAX / 10 -> EAX quot, EDX rem
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
s_banner db 13,10,'== iobench: port-I/O trap throughput ==',13,10,'$'
s_c1     db '1. IN AL,DX  unrolled (0x388) : $'
s_c2     db '2. IN AL,DX + LOOP    (0x388) : $'
s_c3     db '3. OUT DX,AL unrolled (0x3C8) : $'
s_c4     db '4. REP OUTSB x64      (0x3C9) : $'
s_acc    db ' acc / $'
s_ticks  db ' ticks',13,10,'$'
s_done   db 'iobench done',13,10,'$'

tbase    dd 0
count    dd 0
elapsed  dd 0
digits   dw 0
palbuf   times 64 db 0x2A
