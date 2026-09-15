; p_bios.com -- the small BIOS services every DOS program leans on, and the one
; timer hook that games actually use: INT 11h, INT 12h, INT 1Ah and INT 1Ch.
;
; WHY INT 1Ch IS THE POINT OF THIS PROBE. The BIOS timer handler calls INT 1Ch once
; per tick, and that vector -- not INT 08h -- is where a well-behaved game puts its
; own timer, because hooking it needs no EOI and no chaining. If our INT 08h arm does
; not call it, a game's music, animation and input pacing all just stop, and nothing
; anywhere reports an error: the timer "works", the guest simply never hears from it.
; That is the same shape as the two bugs p_pic.asm exists for.
;
; ⚠ RTC TIME AND DATE ARE NOT COMPARABLE between two machines and are not compared.
;   What IS a contract is that they come back as BCD -- a guest that parses BCD reads
;   garbage from a binary answer -- so the probe emits a BCD-VALIDITY verdict rather
;   than the reading itself.
; ⚠ Every wait is bounded, and the INT 1Ch vector is always put back.
;
; nasm -f bin p_bios.asm -o p_bios.com

        org     100h
        jmp     start
%include "probe.inc"

; ---- our INT 1Ch hook: the lightest possible, since it runs inside INT 08h ----
hdlr1c:
        push    ax
        push    ds
        push    cs
        pop     ds
        inc     word [c1c_n]
        cmp     byte [in1c], 0
        je      .fresh
        inc     word [c1c_nest]
.fresh:
        inc     byte [in1c]
        pushf
        call    far [old1c]
        dec     byte [in1c]
        pop     ds
        pop     ax
        iret

; ---- helpers --------------------------------------------------------------

spin1:
        push    cx
        mov     cx, 0FFFFh
.l:     loop    .l
        pop     cx
        ret

bticks:                                 ; AX = 0040:006C low word
        push    es
        mov     ax, 40h
        mov     es, ax
        mov     ax, [es:6Ch]
        pop     es
        ret

; isbcd -- AL in, AL = 1 if both nibbles are 0-9
isbcd:
        push    bx
        mov     bl, al
        and     bl, 0Fh
        cmp     bl, 9
        ja      .no
        mov     bl, al
        shr     bl, 4
        cmp     bl, 9
        ja      .no
        mov     al, 1
        pop     bx
        ret
.no:    xor     al, al
        pop     bx
        ret

; ---- cases ----------------------------------------------------------------

start:
        PROBE_BEGIN "bios"

        ; ---- INT 11h equipment word. Not comparable between two machines -- it
        ; DESCRIBES the hardware -- but recorded on both so the description we hand a
        ; guest can be read next to a real one. Abstained in oracle-rules.json.
        POISON
        int     11h
        call    probe_capture
        EMIT    "int11.equip", "AX"

        ; ---- INT 12h conventional memory in KB. This one IS a contract: 640 is what
        ; every DOS program expects to find and what it sizes its own limits from.
        POISON
        int     12h
        call    probe_capture
        EMIT    "int12.memk", "AX"

        ; ---- INT 1Ah AH=00h: the tick counter, and AL = the midnight-rollover flag
        ; (which must be 0 unless a day has turned over since the last read -- a host
        ; that leaves it set makes a guest think it has, every single time).
        POISON
        xor     ah, ah
        int     1Ah
        call    probe_capture
        mov     ax, [__ax]
        and     ax, 0FFh
        mov     [__ax], ax
        EMIT    "int1a.00.midnight", "AX"

        ; ...and that it ADVANCES. CX:DX is the count; spin and compare the low word.
        xor     ah, ah
        int     1Ah
        mov     [t0], dx
        ; ⚠ 400 units was too impatient and reported "does not advance" on the RIG
        ; purely because one tick takes longer than that there -- a probe artifact
        ; that looked exactly like a dead clock. Same bound as everywhere else.
        mov     si, 20000
.tw:    call    spin1
        dec     si
        jz      .tdone
        xor     ah, ah
        int     1Ah
        cmp     dx, [t0]
        je      .tw
.tdone:
        xor     ah, ah
        int     1Ah
        mov     ax, 1
        cmp     dx, [t0]
        jne     .adv
        xor     ax, ax
.adv:   mov     [__ax], ax
        EMIT    "int1a.00.advances", "AX"

        ; ---- INT 1Ah AH=02h RTC time -> CH:CL:DH, BCD. The READING differs between
        ; machines; being BCD at all does not.
        POISON
        mov     ah, 02h
        int     1Ah
        call    probe_capture
        mov     al, byte [__cx + 1]     ; CH hours
        call    isbcd
        mov     bl, al
        mov     al, byte [__cx]         ; CL minutes
        call    isbcd
        and     bl, al
        mov     al, byte [__dx + 1]     ; DH seconds
        call    isbcd
        and     bl, al
        xor     ah, ah
        mov     al, bl
        mov     [__ax], ax
        EMIT    "int1a.02.isbcd", "AX,CF"

        ; ---- INT 1Ah AH=04h date -> CH century, CL year, DH month, DL day, BCD.
        POISON
        mov     ah, 04h
        int     1Ah
        call    probe_capture
        mov     al, byte [__cx + 1]     ; century
        call    isbcd
        mov     bl, al
        mov     al, byte [__cx]         ; year
        call    isbcd
        and     bl, al
        mov     al, byte [__dx + 1]     ; month
        call    isbcd
        and     bl, al
        mov     al, byte [__dx]         ; day
        call    isbcd
        and     bl, al
        xor     ah, ah
        mov     al, bl
        mov     [__ax], ax
        ; the century must be 19 or 20 BCD -- a zero here is the tell for "we did not
        ; implement this and the register is whatever the caller left".
        mov     al, byte [__cx + 1]
        xor     ah, ah
        mov     [__bx], ax
        EMIT    "int1a.04.isbcd", "AX"

        ; ---- ★ INT 1Ch: IS THE GUEST'S TIMER HOOK ACTUALLY CALLED?
        mov     ax, 351Ch
        int     21h
        mov     [old1c], bx
        mov     [old1c + 2], es
        push    ds
        push    cs
        pop     ds
        mov     dx, hdlr1c
        mov     ax, 251Ch
        int     21h
        pop     ds

        call    bticks                  ; measure against the BIOS's own tick count
        mov     [t0], ax
        xor     si, si
.w1c:   call    bticks
        sub     ax, [t0]
        cmp     ax, 5
        jae     .d1c
        call    spin1
        inc     si
        cmp     si, 20000
        jb      .w1c
.d1c:
        call    bticks
        sub     ax, [t0]
        mov     [dticks], ax

        push    ds                      ; ALWAYS put the vector back
        lds     dx, [old1c]
        mov     ax, 251Ch
        int     21h
        pop     ds

        mov     ax, 1                   ; was it called at all?
        cmp     word [c1c_n], 0
        ja      .was
        xor     ax, ax
.was:   mov     [__ax], ax
        mov     ax, [c1c_n]
        mov     [__bx], ax              ; informational: how many
        mov     ax, [dticks]
        mov     [__cx], ax              ; informational: BIOS ticks over the same span
        EMIT    "int1c.called", "AX"

        ; ---- ...ONCE PER TICK. A hook called at the wrong RATE is a game running at
        ; the wrong speed, which is the failure this project has spent most time on.
        ; Allow one either way for the races at each end of the window.
        mov     ax, [c1c_n]
        sub     ax, [dticks]
        cmp     ax, 8000h               ; |difference| <= 1 ?
        jb      .pos
        neg     ax
.pos:   cmp     ax, 1
        mov     ax, 1
        jbe     .rate
        xor     ax, ax
.rate:  mov     [__ax], ax
        EMIT    "int1c.perTick", "AX"

        ; ---- and never re-entered, for the same reason INT 08h must not be.
        mov     ax, [c1c_nest]
        mov     [__ax], ax
        EMIT    "int1c.nested", "AX"

        PROBE_END

; ---- data -----------------------------------------------------------------
old1c    dd 0
c1c_n    dw 0
c1c_nest dw 0
dticks   dw 0
t0       dw 0
in1c     db 0
