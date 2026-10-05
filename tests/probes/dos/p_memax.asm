; p_memax.com -- what AX holds after a SUCCESSFUL INT 21h 48h/4Ah/49h (GH #258).
;
; The documented returns cover only the failure case (CF=1, AX=error, BX=max for 48h/4Ah).
; QuickBASIC 4.5's Quick Library loader uses AX after a successful AH=4Ah shrink as the
; block's segment, and we returned AH=4Ah with the caller's AL (AX=4Axx) -- so it loaded
; the library at a wrong segment and later freed a block that was never there. This
; measures what the reference kernels leave in AX, poisoned so "unchanged" is visible.
; Each row: AX = 1 the block's segment, 2 unchanged (the poison), 0 something else.
;
; nasm -f bin p_memax.asm -o p_memax.com

        org     100h
        jmp     start
%include "probe.inc"

start:
        PROBE_BEGIN "memax"

        mov     ah, 4Ah                         ; shrink ourselves so 48h has room
        mov     bx, 1000h
        int     21h

        ; ---- 48h: allocate 10h paragraphs (AX = segment, documented)
        mov     bx, 10h
        mov     ax, 4855h
        int     21h
        mov     [blk], ax
        call    probe_capture
        mov     word [__ax], 1                  ; 48h's AX IS the segment by definition
        EMIT    "mem.48", "CF"

%macro CLASSIFY 2                               ; %1 = poison AX, %2 = case name
        call    probe_capture
        mov     ax, [__ax]
        mov     bx, 1
        cmp     ax, [blk]
        je      %%k
        mov     bx, 2
        cmp     ax, %1
        je      %%k
        xor     bx, bx
%%k:    mov     [__ax], bx
        EMIT    %2, "AX,CF"
%endmacro

        mov     es, [blk]                       ; ---- 4Ah shrink 10h -> 8
        mov     bx, 8
        mov     ax, 4A55h
        int     21h
        CLASSIFY 4A55h, "mem.4a.shrink.ax"

        mov     es, [blk]                       ; ---- 4Ah grow 8 -> 10h
        mov     bx, 10h
        mov     ax, 4A55h
        int     21h
        CLASSIFY 4A55h, "mem.4a.grow.ax"

        mov     es, [blk]                       ; ---- 4Ah same size
        mov     bx, 10h
        mov     ax, 4A55h
        int     21h
        CLASSIFY 4A55h, "mem.4a.same.ax"

        mov     es, [blk]                       ; ---- 49h free
        mov     ax, 4955h
        int     21h
        pushf
        push    ax
        CLASSIFY 4955h, "mem.49.ax"
        pop     ax                              ; ...and AX relative to the block
        popf
        sub     ax, [blk]
        mov     [__ax], ax
        mov     word [__fl], 0
        EMIT    "mem.49.ax_minus_seg", "AX"

        PROBE_END

blk     dw      0
