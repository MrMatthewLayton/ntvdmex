; mixbench.asm -- apparent CPU speed for FIVE SHAPES OF WORK, not one. (GH #56)
;
; ── WHY THIS EXISTS, AND WHY cpubench.com COULD NOT ANSWER IT ───────────────────
; The CPU-speed dropdown is a duty cycle against ONE constant, CPUSPEED_REF_MHZ,
; and that constant is calibrated by cpubench.com -- a loop of eight register-to-
; register ADDs that touches no memory, no port and no video. The header of
; cpubench.asm says so out loud: "NO REGISTER IN THE LOOP TOUCHES MEMORY, and that
; is deliberate".
;
; ⚠⚠ SO THE ACCEPTANCE TEST AND THE CALIBRATION ARE THE SAME WORKLOAD, and an
;    instrument that is the same shape as the thing it calibrates can only ever
;    confirm itself. `cpuswp.bat` re-runs cpubench at each index and checks the
;    reported MHz tracks the label; it will PASS however wrong the constant is for
;    every other kind of code, because the ratio it measures is the ratio it was
;    built from. That is the gap this program exists to fill.
;
; ► THE CLAIM UNDER TEST: an unthrottled NTVDMEX does NOT present one speed. It
;   runs register ALU code at near-native rates because that IS native -- the guest
;   is on the real CPU -- while every port access is an IOPL-0 #GP reflected out to
;   a user-mode servicer, which costs orders of magnitude more than the ISA cycle it
;   stands in for. If those two shapes report speeds that differ by more than the
;   spacing of the menu itself, then NO single constant can be right, the duty cycle
;   derived from the ALU number over-throttles everything else by that ratio, and
;   "66 MHz" means 66 only for code that looks like cpubench.
;
; ── THE FIVE CASES, AND WHY EACH IS HERE ───────────────────────────────────────
;   1 ALU    8 register ADDs. THE CONTROL: byte-for-byte cpubench's inner loop, so
;            a disagreement between this program and that one is a bug in one of
;            them and not a finding.
;   2 MEM    the same 12 cycles as loads and stores against a resident buffer.
;            Isolates "touching memory at all" from the ALU control.
;   3 VID13  the same 12 cycles of stores into the mode-13h framebuffer at A000:0.
;            Isolates our VIDEO MAPPING from ordinary guest memory.
;   4 VID12  the same again in mode 12h, where A0000 is the per-plane remapped
;            backing. Isolates the PLANAR path -- Doom's -- from the linear one.
;   5 PORT   8 OUTs to the VGA DAC index. THE ONE THAT IS EXPECTED TO BE SLOW: this
;            is the trap round trip, and Doom's mode-Y drawing does ~43,000 port
;            writes a second through exactly it.
;
; ⚠⚠ CASE 5 IS KNOWN WRONG AND ITS NUMBER MUST NOT BE QUOTED. First run on the rig
;    (2026-09-09, index 0) reported `32 iters / 32 ticks -> 0 MHz`, i.e. ~6.9 ms per
;    OUT. iobench.com case 3 is the SAME INSTRUCTION AT THE SAME PORT and reports
;    117,920 accesses in 5 ticks = 429,400/s = 2.33 us each, which is 6.9 MHz
;    apparent against a 486's 16-cycle OUT -- a factor of ~3000 apart. iobench is
;    the established instrument and it wins; the bug is in here and is NOT yet
;    found. PBATCH has been cut from 16 to 2 so this polls at iobench's rate (16
;    accesses, not 128), which is the first thing to re-test.
;    ► USE iobench.com FOR THE PORT NUMBER UNTIL THIS AGREES WITH IT.
;    ★ It is left in rather than deleted because an instrument that disagrees with
;      a trusted one is evidence about something, and because "0 MHz apparent" is
;      exactly the plausible-looking wrong answer this project has been bitten by
;      before -- it belongs in the record, labelled, not quietly removed.
;
; ★ CASES 1-4 ARE ALL 12 CYCLES ON A 486 BY CONSTRUCTION, so their reported MHz are
;   directly comparable to each other and to cpubench with no arithmetic in between.
;   Case 5 is 132 and carries its own constant. Making the cycle counts equal is
;   worth more than making the loops pretty: it means a reader can compare the four
;   numbers without trusting my cycle table.
;
; ── 486 CYCLE COSTS USED (Intel i486 published timings) ────────────────────────
;   add r,r 1   dec r 1   jnz taken 3   mov r8,[mem] 1   mov [mem],r8 1
;   out dx,al 16
;   cases 1-4: 8x1 + 1 + 3 = 12      case 5: 8x16 + 1 + 3 = 132
;
; ⚠ THE CYCLE TABLE IS AN ASSUMPTION AND THE RAW COUNTS ARE PRINTED BESIDE EVERY
;   DERIVED FIGURE, for the reason cpubench states: a derived number with no inputs
;   next to it is one nobody can check. If you distrust the MHz, use the iters.
;
; ── TIMING DISCIPLINE (this shape wedged the rig once -- see iobench.asm) ───────
; Do NOT poll 0040:006C directly: the host injects INT 08h from its exec loop, and
; that loop only runs when the guest faults or BOPs, so a pure memory spin on the
; tick never advances it and cannot be stopped by the headless deadline. Poll with
; INT 1Ah -- a BOP -- once per BATCH units of work.
;
; ⚠ THE VIDEO CASES RESTORE MODE 3 BEFORE REPORTING. Printing through INT 21h while
;   in a graphics mode renders as glyphs into the framebuffer and the result is
;   unreadable in a log. Mode set is outside the timed region either way.
; ⚠ CASES RUN CHEAPEST-RISK FIRST. A mode set that goes wrong must not be able to
;   cost us the ALU/MEM/PORT numbers, so the two video cases go last and each is
;   reported the moment it finishes.
;
; Assemble: nasm -f bin mixbench.asm -o mixbench.com
bits 16
org 0x100

; ★ EACH CASE IS BOUNDED BY WALL CLOCK, NOT BY WORK, AND THAT IS WHAT MAKES THIS
;   SAFE TO RUN THROTTLED. The PIT advances by real elapsed time whatever the speed
;   setting does (cpuspeed.h is explicit: a throttled guest still gets 18.2 ticks a
;   second rather than a compressed clock), so a case takes TICKS/18.2 seconds at
;   EVERY index and only the iteration count falls. Without that, index 13 would run
;   ~100x longer than index 0 and die on the headless deadline instead of reporting.
; ⚠ 18 ticks ~ 1 s, not 9: at a 1.8% duty the guest advances in ~9 bursts a second,
;   so half a second samples 4 of them and quantises the answer by ~25%.
TICKS   equ 18                      ; ~1.0 s per case; 5 cases ~ 5 s total
BATCH   equ 4096                    ; iterations between tick polls (cases 1-4)
PBATCH  equ 2                       ; iterations between tick polls (case 5: 16 OUTs,
                                    ; matching iobench's poll rate -- see the case-5
                                    ; warning in the header)

start:
    cld
    mov dx, s_banner
    mov ah, 0x09
    int 0x21

    ; ---- 1 ALU (the control) -------------------------------------------------
    mov dx, s_c1
    mov ah, 0x09
    int 0x21
    mov dword [cyc], 12
    call bench_alu
    call report

    ; ---- 2 MEM ---------------------------------------------------------------
    mov dx, s_c2
    mov ah, 0x09
    int 0x21
    mov dword [cyc], 12
    call bench_mem
    call report

    ; ---- 5 PORT (before the video cases: no mode change can cost us this) -----
    mov dx, s_c5
    mov ah, 0x09
    int 0x21
    mov dword [cyc], 132
    call bench_port
    call report

    ; ---- 3 VID13 -------------------------------------------------------------
    mov dx, s_c3
    mov ah, 0x09
    int 0x21
    mov ax, 0x0013                  ; mode 13h, 320x200x256 linear
    int 0x10
    mov dword [cyc], 12
    call bench_vid
    mov ax, 0x0003                  ; back to text BEFORE printing anything
    int 0x10
    call report

    ; ---- 4 VID12 -------------------------------------------------------------
    mov dx, s_c4
    mov ah, 0x09
    int 0x21
    mov ax, 0x0012                  ; mode 12h, 640x480 planar (Doom's shape)
    int 0x10
    mov dword [cyc], 12
    call bench_vid
    mov ax, 0x0003
    int 0x10
    call report

    mov dx, s_done
    mov ah, 0x09
    int 0x21
    mov ax, 0x4C00
    int 0x21

; ── case 1: register ALU. Byte-for-byte cpubench's loop. ───────────────────────
bench_alu:
    call bench_begin
    mov ax, 1
    mov bx, 1
.outer:
    mov cx, BATCH
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
    jnz .inner
    add dword [count], BATCH
    call tick_done
    jz .outer
    ret

; ── case 2: memory. Four loads + four stores against a resident buffer. ────────
bench_mem:
    call bench_begin
    mov si, buf
    mov di, buf+8
.outer:
    mov cx, BATCH
.inner:
    mov al, [si]
    mov [di], al
    mov al, [si+2]
    mov [di+2], al
    mov al, [si+4]
    mov [di+4], al
    mov al, [si+6]
    mov [di+6], al
    dec cx
    jnz .inner
    add dword [count], BATCH
    call tick_done
    jz .outer
    ret

; ── cases 3+4: video stores. Same 12 cycles, into A000:0000. ──────────────────
; The offsets are fixed rather than walking, deliberately: this measures the cost
; of REACHING our framebuffer mapping, not the guest's memory bandwidth, and a
; walking pointer would mix the two.
bench_vid:
    call bench_begin
    push es
    mov ax, 0xA000
    mov es, ax
    xor di, di
    mov al, 0x1F
.outer:
    mov cx, BATCH
.inner:
    mov [es:di], al
    mov [es:di+2], al
    mov [es:di+4], al
    mov [es:di+6], al
    mov [es:di+8], al
    mov [es:di+10], al
    mov [es:di+12], al
    mov [es:di+14], al
    dec cx
    jnz .inner
    add dword [count], BATCH
    call tick_done
    jz .outer
    pop es
    ret

; ── case 5: port OUT. The trap round trip. ────────────────────────────────────
; 0x3C8 is the VGA DAC write index -- a write-only register whose value we then
; never use, so this is inert: it selects a palette entry nobody writes.
bench_port:
    call bench_begin
    mov dx, 0x3C8
    xor al, al
.outer:
    mov cx, PBATCH
.inner:
    out dx, al
    out dx, al
    out dx, al
    out dx, al
    out dx, al
    out dx, al
    out dx, al
    out dx, al
    dec cx
    jnz .inner
    add dword [count], PBATCH
    call tick_done
    jz .outer
    ret

; ── the harness ───────────────────────────────────────────────────────────────
bench_begin:
    mov dword [count], 0
    push eax
    call gettick
    mov [tbase], eax
    pop eax
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
    int 0x1A
    movzx eax, cx
    shl eax, 16
    movzx ecx, dx
    or eax, ecx
    pop edx
    pop ecx
    ret

; ── the report ────────────────────────────────────────────────────────────────
;   MHz = iters * cyc * 18.2065 / ticks / 1e6
; Computed as (iters/ticks) * cyc * 182065 / 1e10, the division by ticks FIRST so
; a long run cannot overflow, and the 182065 multiply taken through the full
; EDX:EAX product because iters-per-tick is already ~16 million on a modern box.
; ⚠ cpubench folded its cycle count into a single magic 2185; this keeps `cyc`
;   separate because five cases do not share one.
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
    mul dword [cyc]                 ; EDX:EAX = per-tick * cycles  (fits in EAX)
    mov ebx, 182065
    mul ebx                         ; EDX:EAX = the full product
    mov ebx, 1000000000
    div ebx                         ; /1e9 ... then /10 below, to keep the
    xor edx, edx                    ; quotient inside EAX at every step
    mov ebx, 10
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
s_banner  db 13,10,'== mixbench: apparent speed by SHAPE of work (GH #56) ==',13,10
          db 'cases 1-4 are 12 cycles/iter on a 486; case 5 is 132',13,10,'$'
s_c1      db 13,10,'[1 ALU  ] 8 reg add       $'
s_c2      db 13,10,'[2 MEM  ] 4 ld + 4 st     $'
s_c5      db 13,10,'[5 PORT ] 8 out 3C8h      $'
s_c3      db 13,10,'[3 VID13] 8 st A000 m13h  $'
s_c4      db 13,10,'[4 VID12] 8 st A000 m12h  $'
s_iters   db ' iters / $'
s_ticks   db ' ticks -> $'
s_mhz     db ' MHz apparent',13,10,'$'
s_noticks db '(no ticks elapsed -- the BIOS tick is not advancing)',13,10,'$'
s_done    db 13,10,'mixbench done',13,10,'$'

cyc       dd 12
tbase     dd 0
count     dd 0
elapsed   dd 0
digits    dw 0
buf       times 32 db 0
