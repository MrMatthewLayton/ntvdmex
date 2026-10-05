; p_env.com -- the environment block a program is handed, as text.  (s73)
;
; The DOS environment is how a whole class of programs is configured (LIB and
; INCLUDE for the Microsoft tool chain, DOS4GVM for DOS/4GW, BLASTER for every
; sound driver) and stock ntvdm passes the launcher's NT environment through to
; the guest.  NTVDMEX built a fixed four-variable block instead, which is why
; `LINK` under it could not find BCOM45.LIB with LIB set in the very cmd window
; that launched it (QuickBASIC's Make EXE from a batch file).
;
; The strings are MACHINE-SPECIFIC (they are the launcher's environment), so this
; is not a paritysweep row: it prints the block as text for a human to read side
; by side across hosts -- the same launcher, the same window, two VDMs.  What is
; comparable is the SHAPE: the block's MCB owner and size, the count word, and
; the program path after it; those go through EMIT.
;
; nasm -f bin p_env.asm -o p_env.com

        org     100h
        jmp     start
%include "probe.inc"

; putfar -- print the ASCIIZ at ES:DI through probe_putc, advance DI past the NUL.
putfar:
.l:     mov     al, [es:di]
        inc     di
        or      al, al
        jz      .d
        call    probe_putc
        jmp     .l
.d:     ret

start:
        PROBE_BEGIN "env"

        ; ---- the block itself: PSP:2C is its segment; its MCB is one paragraph
        ; below.  Owner should be OUR PSP (it is the program's environment, and
        ; MEM /C lists it against the program); size is what the diff compares.
        mov     ax, [2Ch]
        mov     [envseg], ax
        mov     es, ax
        mov     [__ax], ax              ; the segment itself (host-specific, informational)
        mov     bx, ax
        dec     bx
        mov     ds, bx                  ; DS = the MCB
        mov     al, [0]
        xor     ah, ah
        mov     bx, [1]                 ; owner
        mov     cx, [3]                 ; paragraphs
        push    cs
        pop     ds
        mov     [__bx], bx
        mov     [__cx], cx
        mov     [__dx], ax              ; the signature byte, 'M' or 'Z'
        mov     ax, cs                  ; the PSP segment, so owner==psp can be read off
        mov     [__si], ax
        xor     ax, ax
        mov     [__di], ax
        mov     [__fl], ax
        mov     ax, [envseg]
        mov     [__ax], ax
        ; owner == our PSP -> DI=1, else 0 (the host-independent claim)
        mov     ax, cs
        cmp     ax, [__bx]
        jne     .own
        mov     word [__di], 1
.own:   EMIT    "env.mcb", "DI"

        ; ---- walk the strings: one text line each, ENV=<string>
        xor     di, di
        xor     cx, cx                  ; string count
.walk:  cmp     byte [es:di], 0
        je      .end
        push    cx
        mov     si, s_env
        call    probe_puts
        call    putfar
        call    probe_crlf
        pop     cx
        inc     cx
        jmp     .walk
.end:   inc     di                      ; past the terminating NUL
        mov     [__ax], cx              ; number of strings
        mov     ax, [es:di]             ; the count word (DOS 3+: 1)
        mov     [__bx], ax
        add     di, 2
        mov     [envoff], di
        ; total bytes to the end of the program path
        push    di
        call    strlen_far
        pop     di
        mov     [__cx], ax              ; program path length
        mov     word [__dx], 0
        mov     word [__si], 0
        mov     word [__di], 0
        mov     word [__fl], 0
        EMIT    "env.tail", "BX"
        mov     si, s_path
        call    probe_puts
        mov     di, [envoff]
        call    putfar
        call    probe_crlf

        PROBE_END

; strlen_far -- AX = length of the ASCIIZ at ES:DI (DI preserved by the caller)
strlen_far:
        xor     ax, ax
.l:     cmp     byte [es:di], 0
        je      .d
        inc     di
        inc     ax
        jmp     .l
.d:     ret

s_env:  db      "ENV=", 0
s_path: db      "ENVPATH=", 0
envseg: dw      0
envoff: dw      0
